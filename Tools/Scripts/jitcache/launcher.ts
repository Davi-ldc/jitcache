// The build a run uses and the processes it starts: a WebKit build directory read into a Build, a process whose output
// goes to files and whose time is bounded, with every process it started killed when its time runs out, the
// environments of the processes whose leaks or times the harness reads, and the Launcher, which starts the build's
// engines natively or under QEMU user mode. The runner's other modules, its self-tests and the benches build on it.

import { spawn } from "node:child_process";
import { closeSync, openSync, readFileSync, readdirSync, statSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { aarch64AsanRoot, aarch64Sysroot } from "./env.ts";

// A command line the runner or a bench refuses: its main prints the message with the usage and exits with 2.
export class UsageError extends Error {}

const hostArchitecture = process.arch === "x64" ? "x86_64" : process.arch === "arm64" ? "aarch64" : process.arch;

// ---------------------------------------------------------------------------------------------------------------------
// The build

export interface Build {
  directory: string;
  jsc: string;
  testjitcache: string;
  architecture: string;
  buildType: string; // CMakeCache.txt's CMAKE_BUILD_TYPE, such as Debug or RelWithDebInfo, or unknown
  twins: boolean; // cmakeconfig.h defines ENABLE_JITCACHE_TWINS as 1
  sanitized: boolean; // CMakeCache.txt sets ENABLE_SANITIZERS to address
  emulated: boolean; // an aarch64 build on an x86_64 host, whose processes run under QEMU user mode
  emulationRoot: string; // qemu-aarch64 -L
  resultsDirectory: string;
}

export function loadBuild(directory: string): Build {
  const build = resolve(directory);
  if (!statSync(build, { throwIfNoEntry: false })?.isDirectory()) throw new UsageError(`--build names no directory: ${build}`);
  const jsc = join(build, "bin/jsc");
  if (!statSync(jsc, { throwIfNoEntry: false })?.isFile()) throw new UsageError(`${build} holds no bin/jsc`);
  const config = readText(join(build, "cmakeconfig.h")) ?? "";
  const cache = readText(join(build, "CMakeCache.txt")) ?? "";
  const twins = /^#define ENABLE_JITCACHE_TWINS 1\s*$/m.test(config);
  const sanitizers = /^ENABLE_SANITIZERS:[A-Z]+=(.*)$/m.exec(cache)?.[1] ?? "";
  const sanitized = sanitizers.split(/[;,\s]+/).includes("address");
  // <build-root>/linux-<arch>-<profile>/deps/WebKit names its architecture; another layout falls back on the cache.
  const profileDirectory = resolve(build, "../..");
  const laidOut = basename(resolve(build, "..")) === "deps" && /^linux-/.test(basename(profileDirectory));
  const architecture = /^linux-(x86_64|aarch64)-/.exec(basename(profileDirectory))?.[1]
    ?? /^CMAKE_SYSTEM_PROCESSOR:[A-Z]+=(.*)$/m.exec(cache)?.[1]?.trim()
    ?? hostArchitecture;
  const emulated = architecture === "aarch64" && hostArchitecture === "x86_64";
  return {
    directory: build,
    jsc,
    testjitcache: join(build, "bin/testjitcache"),
    architecture,
    buildType: /^CMAKE_BUILD_TYPE:[A-Z]+=(.*)$/m.exec(cache)?.[1]?.trim() || "unknown",
    twins,
    sanitized,
    emulated,
    emulationRoot: sanitized ? aarch64AsanRoot : aarch64Sysroot,
    resultsDirectory: laidOut ? join(dirname(profileDirectory), "jitcache-results") : join(build, "jitcache-results"),
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// Processes

export interface ProcessIO {
  env: Record<string, string | undefined>;
  cwd: string;
  stdout: string; // file paths
  stderr: string;
}

export interface ProcessEnd {
  code: number | null;
  signal: string | null;
  timedOut: boolean;
  error: string | null; // the process could not start
}

export function runProcess(executable: string, args: string[], io: ProcessIO, timeoutSeconds: number): Promise<ProcessEnd> {
  const stdout = openSync(io.stdout, "w");
  const stderr = openSync(io.stderr, "w");
  return new Promise(done => {
    let settled = false;
    let timedOut = false;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const finish = (end: ProcessEnd) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      closeSync(stdout);
      closeSync(stderr);
      done(end);
    };
    let child: ReturnType<typeof spawn>;
    try {
      child = spawn(executable, args, { cwd: io.cwd, env: io.env, stdio: ["ignore", stdout, stderr] });
    } catch (error) {
      finish({ code: null, signal: null, timedOut: false, error: String((error as Error).message ?? error) });
      return;
    }
    timer = setTimeout(() => {
      timedOut = true;
      if (child.pid) killTree(child.pid);
      else child.kill("SIGKILL");
    }, timeoutSeconds * 1000);
    child.on("error", error => finish({ code: null, signal: null, timedOut, error: error.message }));
    child.on("exit", (code, signal) => finish({ code, signal, timedOut, error: null }));
  });
}

// Every process that descends from pid, read from /proc, or none when /proc cannot be read. In a process's stat line the
// field after its state is its parent; the command name before them sits in parentheses and may hold spaces and
// parentheses itself, so the fields start after the last ')'.
function descendants(pid: number): number[] {
  const children = new Map<number, number[]>();
  try {
    for (const entry of readdirSync("/proc")) {
      if (!/^\d+$/.test(entry)) continue;
      const stat = readText(`/proc/${entry}/stat`);
      if (!stat) continue;
      const parent = Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[1]);
      children.set(parent, [...(children.get(parent) ?? []), Number(entry)]);
    }
  } catch {
    return [];
  }
  const found: number[] = [];
  for (let next = 0, queue = [pid]; next < queue.length; ++next) {
    for (const child of children.get(queue[next]) ?? []) {
      found.push(child);
      queue.push(child);
    }
  }
  return found;
}

// Kills a process whose time ran out and every process it started, such as the Bun processes a test of Bun's started,
// so none outlives the run and the machine lock. Each one is stopped before any is killed, scan after scan until a scan
// finds none left to stop: a stopped process starts no child, and a process killed while its child runs would leave
// that child to init, out of reach of the next scan. The processes keep their process group, so a signal the caller
// sends the runner's group still reaches them.
function killTree(pid: number) {
  const stopped = new Set<number>();
  let found = [pid];
  // The rounds are bounded in case a descendant the runner may not stop keeps starting children.
  for (let round = 0; found.length && round < 100; ++round) {
    for (const p of found) {
      stopped.add(p);
      try {
        process.kill(p, "SIGSTOP");
      } catch {
        // It has already exited.
      }
    }
    found = descendants(pid).filter(p => !stopped.has(p));
  }
  for (const p of stopped) {
    try {
      process.kill(p, "SIGKILL");
    } catch {
      // It has already exited.
    }
  }
}

// The guest stack sizes under QEMU: 8 + 64·k MiB with k drawn from 1 to 64, distinct among the processes of one
// sequence, or of one calibration, since under QEMU the stack is the one input that moves the heap. A process that
// belongs to neither, such as testjitcache's, whose heap no twin check compares, draws its k alone.
export const guestStackSizes = 64;
const guestStackMiB = (k: number) => 8 + 64 * k;
const drawGuestStack = () => 1 + Math.floor(Math.random() * guestStackSizes);

export class StackSizes {
  private readonly drawn = new Set<number>();

  // runAttempt refuses a sequence that would start more processes than there are sizes, so only a caller that skipped
  // that check can exhaust them.
  next(): number {
    if (this.drawn.size === guestStackSizes) throw new Error(`more than ${guestStackSizes} processes drew from one set of guest stack sizes`);
    let k: number;
    do k = drawGuestStack();
    while (this.drawn.has(k));
    this.drawn.add(k);
    return guestStackMiB(k);
  }
}

export class Launcher {
  constructor(readonly build: Build, readonly timeoutSeconds: number, readonly defaultCpu: string) {}

  // An executable of the build, of the pin or the Bun executable: under QEMU user mode when the build is emulated, as
  // qemu-aarch64 -L <root> -cpu <model> -s <n>M <executable> <arguments>. stacks holds the sizes the process's sequence
  // or calibration drew, and is null for a process outside both.
  engine(executable: string, args: string[], io: ProcessIO, stacks: StackSizes | null, cpu: string | null = null): Promise<ProcessEnd> {
    if (!this.build.emulated) return runProcess(executable, args, io, this.timeoutSeconds);
    const env = { ...io.env };
    // LeakSanitizer cannot start its tracer under QEMU.
    if (this.build.sanitized) env.ASAN_OPTIONS = env.ASAN_OPTIONS ? `${env.ASAN_OPTIONS}:detect_leaks=0` : "detect_leaks=0";
    // qemu-aarch64 8.2.2 reads -s 1G as a 128 KiB stack, so the size is written in M.
    const stack = stacks ? stacks.next() : guestStackMiB(drawGuestStack());
    const qemuArgs = ["-L", this.build.emulationRoot, "-cpu", cpu ?? this.defaultCpu, "-s", `${stack}M`, executable, ...args];
    return runProcess("qemu-aarch64", qemuArgs, { ...io, env }, this.timeoutSeconds);
  }

  // A process of the host, such as a checker, which the Bun that runs this runner runs.
  host(executable: string, args: string[], io: ProcessIO): Promise<ProcessEnd> {
    return runProcess(executable, args, io, this.timeoutSeconds);
  }
}

export function describeEnd(end: ProcessEnd): string {
  if (end.error) return `could not start: ${end.error}`;
  if (end.timedOut) return "timed out";
  if (end.signal) return `ended by ${end.signal}`;
  return `exited with ${end.code}`;
}

// ---------------------------------------------------------------------------------------------------------------------
// Environments

// The environment of a run whose leaks the runner judges itself, from the one engineEnvironment or bunInvocation gives.
// When a process leaks, LeakSanitizer ends it with ASan's exitcode in place of its own status, so a script that throws
// and leaks exits 1 where it would exit 3. exitcode=0 keeps the run's status and still prints the report leakedObjects
// reads, and abort_on_error=1 keeps every other sanitizer error fatal: it ends the process by SIGABRT, where exitcode=0
// would let it exit 0. report_objects=1 lists each leaked object's size, which LeakSanitizer reads from LSAN_OPTIONS
// alone. A process without ASan ignores both variables, and under QEMU the launcher appends detect_leaks=0 after these
// options.
export function keepingExitStatus(env: ProcessIO["env"]): ProcessIO["env"] {
  return {
    ...env,
    ASAN_OPTIONS: `${env.ASAN_OPTIONS ? `${env.ASAN_OPTIONS}:` : ""}exitcode=0:abort_on_error=1`,
    LSAN_OPTIONS: `${env.LSAN_OPTIONS ? `${env.LSAN_OPTIONS}:` : ""}report_objects=1`,
  };
}

// The environment of a bench's process. The benches measure the default configuration, so no JSC_ variable reaches
// their processes, which would add options nobody sees. A run whose exit status a bench reads keeps it under ASan, as
// the test runs do; a build without ASan ignores those variables.
export function benchEnvironment(judged: boolean): ProcessIO["env"] {
  const env = Object.fromEntries(Object.entries(process.env).filter(([name]) => !name.startsWith("JSC_")));
  return judged ? keepingExitStatus(env) : env;
}

// ---------------------------------------------------------------------------------------------------------------------
// Files

export function readText(path: string): string | null {
  try {
    return readFileSync(path, "utf8");
  } catch {
    return null;
  }
}

export function readBytes(path: string): Buffer {
  try {
    return readFileSync(path);
  } catch {
    return Buffer.alloc(0);
  }
}

// The last lines of a process's output, for a message about it.
export function tail(text: string | null, lines = 12): string {
  if (!text) return "(no output)";
  return text.trimEnd().split("\n").slice(-lines).join("\n");
}
