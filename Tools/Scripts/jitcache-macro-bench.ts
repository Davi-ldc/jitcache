#!/usr/bin/env bun
// JITCache's macro benches (docs/JitCache/HARNESS.md, Benches): how long a fresh Bun process takes to finish serving a
// case of PerformanceTests/JITCache with JITCache off and as a consumer. Every Bun process runs pinned to one CPU with
// Bun's shipped configuration, so concurrent JIT stays on and its worker competes with execution, as on a small
// serverless instance.
//
// A repetition starts with fresh producers for each case and branch: one calls delta at exit and one never does. Then,
// for each scenario, processes with JITCache off and consumers of the first producer's artifact run interleaved; the
// longest scenario adds consumers of the second producer's artifact, and a few consumers run after the artifact's pages
// were dropped from the page cache. After the last repetition, untimed runs write JITCache's bench report and log every
// optimizing compile by call. Every process must compute the same checksum at each call as every other process of its
// case and branch. The records go to results.jsonl, and summary.md is built from that file alone, so --summarize can
// rebuild it and pool several runs.

import { accessSync, appendFileSync, closeSync, constants, existsSync, mkdirSync, openSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { cpus, homedir, hostname, loadavg, release } from "node:os";
import { dirname, join, resolve } from "node:path";

const repositoryRoot = resolve(import.meta.dirname, "../..");
const benchDirectory = join(repositoryRoot, "PerformanceTests/JITCache");

const caseNames = ["hono", "elysia", "react-ssr", "preact-ssr", "webhook", "pdf"] as const;
type CaseName = (typeof caseNames)[number];
// pdf runs only when --cases names it.
const defaultCases: CaseName[] = ["hono", "elysia", "react-ssr", "preact-ssr", "webhook"];
const branchNames = ["fixed", "varied"] as const;
type Branch = (typeof branchNames)[number];

// `producer` calls delta at exit and `producerNoDelta` never does, so its artifact keeps only the captures made at each
// baseline compile. `consumer` reads the first one's artifact, `consumerCold` the same artifact after its pages left the
// page cache, and `consumerNoDelta` the second one's.
type Mode = "off" | "consumer" | "consumerNoDelta" | "consumerCold" | "producer" | "producerNoDelta";
const comparedModes: Mode[] = ["off", "consumer", "consumerNoDelta", "consumerCold"];
// Producers and timed processes are measured. Report runs write JITCache's bench report, whose clock reads only
// consumers would pay, and census runs log every optimizing compile, so neither is timed.
type Purpose = "producer" | "timed" | "report" | "census";

// HARNESS.md's target: a case at least 20% faster to finish than with JITCache off.
const targetRatio = 0.8;
// The calls after which the summary gives the time to finish, the k of its Ck/W, and the last calls of its census
// windows.
const finishCalls = [1, 10, 50, 100, 500];
const curveCalls = [1, 2, 5, 10, 50, 100, 500];
const censusBounds = [10, 50, 100, 500];
// W is the median of the producers' last ten calls, 91 to 100 when they serve 100.
const warmedCalls = 10;
// A process pinned to one CPU spends at most its wall time in CPU time; more means it also ran elsewhere.
const cpuTimeSlack = 1.05;
// A repetition waits up to idleWaitSeconds for an idle machine: a 1-minute load average below idleLoad and no other
// user's process above idleProcessPercent of a CPU.
const idleLoad = 0.5;
const idleProcessPercent = 10;
const idleWaitSeconds = 300;

const usage = `Usage: bun Tools/Scripts/jitcache-macro-bench.ts --bun=<Bun executable> [options]
       bun Tools/Scripts/jitcache-macro-bench.ts --summarize <results.jsonl>... [--out=<directory>]

  --cases=${defaultCases.join(",")}   pdf runs only when named
  --branches=fixed,varied
  --scenarios=50,500          the calls of each measured process; the longest scenario also runs consumers of the
                              producer that never calls delta
  --processes=10              processes per mode, scenario, case and branch in each repetition; with 0, only the
                              producers and the untimed runs
  --repetitions=3             each with fresh producers
  --producer-calls=100
  --cold-processes=3          consumers per scenario, case and branch in each repetition that start after the
                              artifact's pages are dropped from the page cache
  --cpu=<n>                   the CPU every Bun process is pinned to, by default the highest this process may use
  --census, --no-census       count the optimizing compiles of each call in an untimed off process and consumer
  --reports, --no-reports     run an untimed warm and cold consumer per scenario with JITCache's bench report
  --out=<directory>           jitcache-macro/<time> beside the Bun executable by default
  --timeout=<seconds>         1800 by default; a process that runs longer is killed and fails
  --check                     correctness only: 1 repetition, 1 process per mode, scenarios of 5 and 20 calls,
                              producers of 20 calls and no build lock, so it can run beside a build; its times
                              measure nothing
  --summarize                 rebuild summary.md from results files, pooling them; it goes beside the only file, or
                              to --out, which several files require`;

class UsageError extends Error {}

// ---------------------------------------------------------------------------------------------------------------------
// Command line

interface Options {
  bun: string;
  cases: CaseName[];
  branches: Branch[];
  scenarios: number[]; // the calls of a measured process, ascending
  processes: number; // per mode, scenario, case and branch in a repetition
  repetitions: number;
  producerCalls: number;
  coldProcesses: number; // per scenario, case and branch in a repetition
  cpu: number; // the CPU every Bun process is pinned to
  census: boolean;
  reports: boolean;
  out: string;
  timeoutSeconds: number;
  check: boolean; // correctness only: no build lock, and times that measure nothing
}

type Command = { kind: "run"; options: Options } | { kind: "summarize"; files: string[]; out: string | null };

function parseCommandLine(argv: string[], allowed: number[]): Command {
  const given = new Map<string, string | null>();
  const files: string[] = [];
  for (const argument of argv) {
    if (!argument.startsWith("--")) {
      files.push(argument);
      continue;
    }
    const equals = argument.indexOf("=");
    const name = equals < 0 ? argument : argument.slice(0, equals);
    if (given.has(name)) throw new UsageError(`${name} is given twice`);
    given.set(name, equals < 0 ? null : argument.slice(equals + 1));
  }
  const take = (name: string): string | null => {
    if (!given.has(name)) return null;
    const value = given.get(name);
    given.delete(name);
    if (!value) throw new UsageError(`${name} needs a value`);
    return value;
  };
  const takeFlag = (name: string): boolean => {
    if (!given.has(name)) return false;
    const value = given.get(name);
    given.delete(name);
    if (value !== null) throw new UsageError(`${name} takes no value`);
    return true;
  };
  // A setting that is on unless --no-<name> turns it off.
  const takeSwitch = (name: string): boolean => {
    const on = takeFlag(`--${name}`);
    const off = takeFlag(`--no-${name}`);
    if (on && off) throw new UsageError(`--${name} and --no-${name} are both given`);
    return !off;
  };
  const rejectRest = () => {
    const [name] = given.keys();
    if (name !== undefined) throw new UsageError(`unknown argument ${name}`);
  };

  if (takeFlag("--summarize")) {
    const out = take("--out");
    rejectRest();
    if (!files.length) throw new UsageError("--summarize needs a results file");
    if (files.length > 1 && out === null) throw new UsageError("--summarize with several results files needs --out, so it replaces no run's own summary");
    return { kind: "summarize", files: files.map(file => resolve(file)), out: out === null ? null : resolve(out) };
  }
  if (files.length) throw new UsageError(`unknown argument ${files[0]}`);

  const check = takeFlag("--check");
  const bunPath = take("--bun");
  if (bunPath === null) throw new UsageError("--bun is required");
  const bun = resolve(bunPath);
  if (!statSync(bun, { throwIfNoEntry: false })?.isFile()) throw new UsageError(`--bun names no file: ${bun}`);
  try {
    accessSync(bun, constants.X_OK);
  } catch {
    throw new UsageError(`--bun names a file that is not executable: ${bun}`);
  }
  const list = <T>(name: string, fallback: readonly T[], parse: (text: string) => T): T[] => {
    const text = take(name);
    if (text === null) return [...fallback];
    const values: T[] = [];
    for (const part of text.split(",")) {
      const value = parse(part);
      if (values.includes(value)) throw new UsageError(`${name} names ${part} twice`);
      values.push(value);
    }
    return values;
  };
  const oneOf =
    <T extends string>(name: string, names: readonly T[]) =>
    (text: string): T => {
      if (!(names as readonly string[]).includes(text)) throw new UsageError(`${name} names ${JSON.stringify(text)}, which is none of ${names.join(", ")}`);
      return text as T;
    };
  const number = (name: string, fallback: number, minimum: number) => integer(take(name) ?? String(fallback), name, minimum);

  const cases = list("--cases", defaultCases, oneOf("--cases", caseNames));
  const branches = list("--branches", branchNames, oneOf("--branches", branchNames));
  const scenarios = list("--scenarios", check ? [5, 20] : [50, 500], text => integer(text, "--scenarios", 0)).sort((a, b) => a - b);
  const processes = number("--processes", check ? 1 : 10, 0);
  const repetitions = number("--repetitions", check ? 1 : 3, 1);
  const producerCalls = number("--producer-calls", check ? 20 : 100, 1);
  const coldProcesses = number("--cold-processes", check ? 1 : 3, 0);
  const cpu = number("--cpu", Math.max(...allowed), 0);
  if (!allowed.includes(cpu)) throw new UsageError(`--cpu names CPU ${cpu}, which this process may not use`);
  const timeoutSeconds = number("--timeout", 1800, 1);
  const census = takeSwitch("census");
  const reports = takeSwitch("reports");
  const outPath = take("--out");
  rejectRest();
  // A producer refuses a path that already holds an artifact, so every run starts in a directory of its own.
  let out: string;
  if (outPath === null) {
    out = join(dirname(bun), "jitcache-macro", new Date().toISOString().replace(/[:.]/g, "-"));
  } else {
    out = resolve(outPath);
    const existing = statSync(out, { throwIfNoEntry: false });
    if (existing && (!existing.isDirectory() || readdirSync(out).length)) throw new UsageError(`--out names a file or a directory that is not empty: ${out}`);
  }
  return { kind: "run", options: { bun, cases, branches, scenarios, processes, repetitions, producerCalls, coldProcesses, cpu, census, reports, out, timeoutSeconds, check } };
}

function integer(text: string, name: string, minimum: number): number {
  if (!/^\d+$/.test(text) || Number(text) < minimum) throw new UsageError(`${name} takes ${minimum ? "positive" : "non-negative"} integers, not ${text}`);
  return Number(text);
}

// ---------------------------------------------------------------------------------------------------------------------
// The machine

// A CPU list as the kernel writes it, such as "0-3,8".
function parseCpuList(text: string): number[] {
  const list: number[] = [];
  for (const part of text.trim().split(",")) {
    const [first, last = first] = part.split("-").map(Number);
    for (let cpu = first; cpu <= last; ++cpu) list.push(cpu);
  }
  return list;
}

function allowedCpus(): { text: string; list: number[] } {
  const text = /^Cpus_allowed_list:\s*(\S+)$/m.exec(readFileSync("/proc/self/status", "utf8"))?.[1];
  if (!text) throw new Error("/proc/self/status gives no Cpus_allowed_list");
  return { text, list: parseCpuList(text) };
}

const smtSiblings = (cpu: number) => parseCpuList(readText(`/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list`) ?? String(cpu));

// The driver moves itself, every thread included, to a CPU of its own, off the measured CPU's core when it can; the
// tools it spawns inherit that CPU. taskset --pid pins the running process, which a re-exec would only repeat.
function pinDriver(cpu: number, allowed: number[]): { cpu: number | null; note: string | null } {
  const siblings = smtSiblings(cpu);
  const others = allowed.filter(other => other !== cpu);
  const choice = others.filter(other => !siblings.includes(other)).at(-1) ?? others.at(-1);
  if (choice === undefined) return { cpu: null, note: `it may use no CPU but ${cpu}, so it shares that one` };
  const result = Bun.spawnSync(["taskset", "--all-tasks", "--cpu-list", "--pid", String(choice), String(process.pid)], { stdout: "ignore", stderr: "pipe" });
  const now = allowedCpus().text;
  if (!result.success || now !== String(choice)) return { cpu: null, note: `it could not move to CPU ${choice} and may run on ${now}: ${result.stderr.toString().trim()}` };
  return { cpu: choice, note: siblings.includes(choice) ? `CPU ${choice} shares a core with CPU ${cpu}` : null };
}

// One CPU's counters from /proc/stat, in clock ticks. busy is user, nice, system, irq and softirq time.
interface CpuCounters {
  busy: number;
  steal: number;
  idle: number;
}

function cpuCounters(cpu: number): CpuCounters | null {
  const prefix = `cpu${cpu} `;
  const line = (readText("/proc/stat") ?? "").split("\n").find(candidate => candidate.startsWith(prefix));
  if (!line) return null;
  const [user, nice, system, idle, iowait, irq, softirq, steal] = line.slice(prefix.length).trim().split(/\s+/).map(Number);
  return { busy: user + nice + system + irq + softirq, steal, idle: idle + iowait };
}

interface BusyProcess {
  pid: number;
  uid: number;
  command: string;
  percent: number; // of one CPU
}

// Each process's share of a CPU over one second, from the counters ps reads in /proc/<pid>/stat. ps's own %CPU divides by
// the process's whole life, so it misses a long-lived process that just turned busy.
async function busyProcesses(clockTicks: number): Promise<BusyProcess[]> {
  const sample = () => {
    const samples = new Map<number, { uid: number; command: string; ticks: number }>();
    for (const name of readdirSync("/proc")) {
      if (!/^\d+$/.test(name)) continue;
      try {
        const stat = readFileSync(`/proc/${name}/stat`, "utf8");
        const close = stat.lastIndexOf(")");
        // The fields after the command start at the state, field 3; utime and stime are fields 14 and 15.
        const fields = stat.slice(close + 2).split(" ");
        samples.set(Number(name), { uid: statSync(`/proc/${name}`).uid, command: stat.slice(stat.indexOf("(") + 1, close), ticks: Number(fields[11]) + Number(fields[12]) });
      } catch {
        // The process ended during the listing.
      }
    }
    return samples;
  };
  const before = sample();
  const started = performance.now();
  await Bun.sleep(1000);
  const after = sample();
  const seconds = (performance.now() - started) / 1000;
  const busy: BusyProcess[] = [];
  for (const [pid, { uid, command, ticks }] of after) {
    const earlier = before.get(pid);
    if (!earlier) continue;
    const percent = Math.round(((ticks - earlier.ticks) / clockTicks / seconds) * 100);
    if (percent >= 1) busy.push({ pid, uid, command, percent });
  }
  return busy.sort((a, b) => b.percent - a.percent);
}

// HARNESS.md: a bench runs on a quiet machine. A repetition that starts without one after the wait is marked noisy.
async function waitForIdle(clockTicks: number, wait: boolean): Promise<{ idle: boolean; waitedSeconds: number; loadAverage: number[]; busiest: BusyProcess[] }> {
  const uid = process.getuid!();
  let waitedSeconds = 0;
  for (;;) {
    const busy = await busyProcesses(clockTicks);
    const loadAverage = loadavg();
    const idle = loadAverage[0] < idleLoad && !busy.some(entry => entry.uid !== uid && entry.percent > idleProcessPercent);
    if (idle || !wait || waitedSeconds >= idleWaitSeconds) return { idle, waitedSeconds, loadAverage, busiest: busy.slice(0, 5) };
    console.log(`waiting for an idle machine: load average ${loadAverage[0].toFixed(2)} after ${waitedSeconds} s`);
    await Bun.sleep(15_000);
    waitedSeconds += 15;
  }
}

interface MachineFacts {
  host: string;
  kernel: string;
  cpus: number;
  cpuModel: string | null;
  transparentHugePages: string | null; // the mode /sys/kernel/mm/transparent_hugepage/enabled selects
  memory: { totalBytes: number | null; availableBytes: number | null; swapTotalBytes: number | null; swapFreeBytes: number | null };
  loadAverage: number[];
  who: string;
}

function machineFacts(): MachineFacts {
  const meminfo = readText("/proc/meminfo") ?? "";
  const bytes = (field: string) => {
    const kibibytes = new RegExp(`^${field}:\\s+(\\d+) kB$`, "m").exec(meminfo)?.[1];
    return kibibytes === undefined ? null : Number(kibibytes) * 1024;
  };
  const hugePages = readText("/sys/kernel/mm/transparent_hugepage/enabled");
  return {
    host: hostname(),
    kernel: release(),
    cpus: cpus().length,
    cpuModel: cpus()[0]?.model ?? null,
    transparentHugePages: hugePages === null ? null : (/\[(\w+)\]/.exec(hugePages)?.[1] ?? hugePages.trim()),
    memory: { totalBytes: bytes("MemTotal"), availableBytes: bytes("MemAvailable"), swapTotalBytes: bytes("SwapTotal"), swapFreeBytes: bytes("SwapFree") },
    loadAverage: loadavg(),
    who: Bun.spawnSync(["who"], { stdout: "pipe", stderr: "ignore" }).stdout.toString().trim(),
  };
}

// Every Bun process gets these variables and no other, so no JSC_ or BUN_JSC_ option and no Bun setting reaches it from
// the driver's environment. NODE_ENV is production, as in a deployment, and run.ts requires it. BUN_DEBUG_QUIET_LOGS=1
// keeps a debug Bun's scoped logs out of standard output, which carries run.ts's events; a release Bun ignores it.
function processEnvironment(): Record<string, string> {
  const environment: Record<string, string> = {};
  for (const name of ["PATH", "HOME", "TMPDIR", "LANG"]) {
    const value = process.env[name];
    if (value !== undefined) environment[name] = value;
  }
  environment.NODE_ENV = "production";
  environment.BUN_DEBUG_QUIET_LOGS = "1";
  return environment;
}

// ---------------------------------------------------------------------------------------------------------------------
// The build lock

// HARNESS.md Benches: the bench holds the build lock while it measures, so no build or other run overlaps it; build.ts
// takes the same lock around every build. A child flock holds it until its standard input closes, which also happens
// when this process dies, so a crash cannot leave the lock taken.
async function takeBuildLock(): Promise<() => Promise<void>> {
  const directory = join(process.env.XDG_CACHE_HOME || join(homedir(), ".cache"), "jitcache");
  mkdirSync(directory, { recursive: true });
  const path = join(directory, "build.lock");
  const hold = (wait: boolean) =>
    Bun.spawn(["flock", "--exclusive", ...(wait ? [] : ["--nonblock"]), path, "sh", "-c", "echo held && exec cat >/dev/null"], {
      stdin: "pipe",
      stdout: "pipe",
      stderr: "inherit",
    });
  let holder = hold(false);
  if ((await firstLine(holder.stdout)) !== "held") {
    await holder.exited;
    console.log(`waiting for the build lock ${path}, which a build or another bench holds`);
    holder = hold(true);
    if ((await firstLine(holder.stdout)) !== "held") throw new Error(`flock could not take ${path}: it exited with ${await holder.exited}`);
  }
  return async () => {
    holder.stdin.end();
    await holder.exited;
  };
}

async function firstLine(stream: ReadableStream<Uint8Array>): Promise<string> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let text = "";
  while (!text.includes("\n")) {
    const { value, done } = await reader.read();
    if (done) break;
    text += decoder.decode(value, { stream: true });
  }
  reader.releaseLock();
  return text.split("\n")[0];
}

// ---------------------------------------------------------------------------------------------------------------------
// The bench report (SPEC-integrator.harness.md section 9.2)

// What the summary reads from one process's report. Each field is read only when it is a number, and unknown events and
// fields are only counted, so a field the report gains or loses changes no other number.
interface ReportCounts {
  events: Record<string, number>; // every event name, with its count
  malformedLines: number;
  start: { outcome: unknown; role: unknown; nanoseconds: number | null; indexedBodies: number | null } | null;
  installs: number;
  installNanoseconds: number; // the install events' totals: thread CPU time
  requests: Record<string, number>; // by action: import, seed, attach
  requestNanoseconds: number; // the request events' counted and apart parts: thread CPU time
  // The last lookup event's counts by result; bodyVersion.absent counts the requests for bodies the artifact lacks, the
  // misses.
  lookups: Record<string, number>;
  compiles: number; // native baseline compiles
  compileNanoseconds: number; // their compilation and finalization: thread CPU time
  captures: Record<string, number>; // by "<trigger> <outcome>"
  committedCaptures: number;
  capturedBytes: number; // the sections the committed captures wrote
  deltas: number;
  deltaCommittedBodies: number;
  deltaCommittedBytes: number;
  deltaNanoseconds: number; // both phases: thread CPU time
  budgetPeakBytes: number | null; // the last budget event's
  maxResidentBytes: number | null; // the last process event's
  executablePoolCommittedBytes: number | null; // the last process event's
}

const numberOr = <T>(value: unknown, fallback: T): number | T => (typeof value === "number" && Number.isFinite(value) ? value : fallback);

function readReport(text: string): ReportCounts {
  const counts: ReportCounts = {
    events: {},
    malformedLines: 0,
    start: null,
    installs: 0,
    installNanoseconds: 0,
    requests: {},
    requestNanoseconds: 0,
    lookups: {},
    compiles: 0,
    compileNanoseconds: 0,
    captures: {},
    committedCaptures: 0,
    capturedBytes: 0,
    deltas: 0,
    deltaCommittedBodies: 0,
    deltaCommittedBytes: 0,
    deltaNanoseconds: 0,
    budgetPeakBytes: null,
    maxResidentBytes: null,
    executablePoolCommittedBytes: null,
  };
  for (const line of text.split("\n")) {
    if (!line) continue;
    let event: Record<string, unknown>;
    try {
      event = JSON.parse(line);
    } catch {
      counts.malformedLines++;
      continue;
    }
    if (typeof event !== "object" || event === null || typeof event.event !== "string") {
      counts.malformedLines++;
      continue;
    }
    counts.events[event.event] = (counts.events[event.event] ?? 0) + 1;
    switch (event.event) {
      case "start":
        // A process's report holds its main VM's start first.
        counts.start ??= { outcome: event.outcome, role: event.role, nanoseconds: numberOr(event.nanoseconds, null), indexedBodies: numberOr(event.indexedBodies, null) };
        break;
      case "install":
        counts.installs++;
        counts.installNanoseconds += numberOr(event.total, 0);
        break;
      case "request": {
        const action = String(event.action);
        counts.requests[action] = (counts.requests[action] ?? 0) + 1;
        counts.requestNanoseconds += numberOr(event.counted, 0) + numberOr(event.apart, 0);
        break;
      }
      case "lookup":
        // Each flush writes the totals so far, so the last line holds the whole run's.
        counts.lookups = {};
        for (const [name, value] of Object.entries(event)) {
          if (name.endsWith(".count")) counts.lookups[name.slice(0, -".count".length)] = numberOr(value, 0);
        }
        break;
      case "compile":
        counts.compiles++;
        counts.compileNanoseconds += numberOr(event.compileNanoseconds, 0) + numberOr(event.finalizeNanoseconds, 0);
        break;
      case "capture": {
        const kind = `${String(event.trigger)} ${String(event.outcome)}`;
        counts.captures[kind] = (counts.captures[kind] ?? 0) + 1;
        if (event.outcome !== "committed") break;
        counts.committedCaptures++;
        // The writer's fields give each section's size as writer.<section>.bytes.
        for (const [name, value] of Object.entries(event)) {
          if (name.startsWith("writer.") && name.endsWith(".bytes")) counts.capturedBytes += numberOr(value, 0);
        }
        break;
      }
      case "delta":
        counts.deltas++;
        counts.deltaCommittedBodies += numberOr(event.committedBodies, 0);
        counts.deltaCommittedBytes += numberOr(event.committedBytes, 0);
        counts.deltaNanoseconds += numberOr(event.phase1Nanoseconds, 0) + numberOr(event.phase2Nanoseconds, 0);
        break;
      case "budget":
        counts.budgetPeakBytes = numberOr(event.peak, counts.budgetPeakBytes);
        break;
      case "process":
        counts.maxResidentBytes = numberOr(event.maxResidentBytes, counts.maxResidentBytes);
        counts.executablePoolCommittedBytes = numberOr(event.executablePoolCommittedBytes, counts.executablePoolCommittedBytes);
        break;
    }
  }
  return counts;
}

// ---------------------------------------------------------------------------------------------------------------------
// One process

interface Job {
  purpose: Purpose;
  case: CaseName;
  branch: Branch;
  scenario: number | null; // a timed or report run's
  calls: number;
  mode: Mode;
  repetition: number; // report and census runs read the last repetition's artifacts
  index: number; // from 1 among the timed processes of its mode, scenario, case and branch in its repetition
}

interface EndLine {
  rssBytes: number | null;
  peakRssBytes: number | null; // VmHWM, the process's own peak: getrusage's also holds its spawner's
  cpusAllowed: string | null;
  hardwareConcurrency: number | null;
  threads: Record<string, number>; // by name
}

interface ProcessRecord extends Job {
  kind: "process";
  run?: number; // the results file a pooled summary read it from
  label: string;
  command: string[];
  exitCode: number | null;
  signal: string | null;
  timedOut: boolean;
  wallMilliseconds: number | null; // from the spawn to the exit
  startupMilliseconds: number | null; // the ready line's Bun.nanoseconds()
  callMilliseconds: number[];
  variants: number[];
  checksums: string[]; // the first 16 hex digits of each response's SHA-256, plenty to tell responses apart
  cpuMicroseconds: { user: number; system: number } | null; // every thread's
  contextSwitches: { voluntary: number; involuntary: number } | null;
  end: EndLine | null;
  pinnedCpu: { busyMilliseconds: number; stealMilliseconds: number; idleMilliseconds: number } | null; // from /proc/stat
  pageCache: { files: number; residentBytes: number | null } | null; // a cold consumer's artifact after the drop, by fincore
  committedBodies: number | null; // a producer's
  report: ReportCounts | null; // a report run's
  compiles: { dfg: number[]; ftl: number[] } | null; // a census run's, at 0 before call 1 and at k during call k
  strayLines: number; // standard output lines that are no event of run.ts
  problems: string[];
}

interface Context {
  options: Options;
  environment: Record<string, string>;
  deltaAtExitFlag: boolean; // whether the Bun takes --jitcache-delta-at-exit
  fincore: boolean;
  clockTicks: number; // per second, /proc/stat's unit
  processDirectory: string;
}

function labelOf(job: Job): string {
  const where = `${job.case} ${job.branch}`;
  switch (job.purpose) {
    case "producer":
      return `r${job.repetition} ${where} ${job.mode}`;
    case "timed":
      return `r${job.repetition} ${where} ${job.scenario} ${job.mode} #${job.index}`;
    case "report":
      return `${where} ${job.scenario} ${job.mode} report`;
    case "census":
      return `${where} ${job.mode} census`;
  }
}

// The artifact a producer writes and its consumers read, one per producer, repetition, case and branch.
function artifactOf(options: Options, job: Job): string | null {
  if (job.mode === "off") return null;
  const producer = job.mode === "producerNoDelta" || job.mode === "consumerNoDelta" ? "no-delta" : "delta";
  return join(options.out, "artifacts", `r${job.repetition}-${job.case}-${job.branch}-${producer}`);
}

function commandOf(context: Context, job: Job, artifact: string | null, reportPath: string | null): { command: string[]; killAtEnd: boolean } {
  // JITCache's flags go before the script, where Bun reads its own. A consumer takes the default mode.
  const flags = artifact === null ? [] : [`--jitcache=${artifact}`];
  let killAtEnd = false;
  if (job.purpose === "producer") {
    flags.push("--jitcache-mode=p");
    // A Bun that takes --jitcache-delta-at-exit calls delta at exit only when given it. An older one always does, so its
    // no-delta producer kills itself right after its end line, before the exit hook can run.
    if (job.mode === "producer" && context.deltaAtExitFlag) flags.push("--jitcache-delta-at-exit");
    killAtEnd = job.mode === "producerNoDelta" && !context.deltaAtExitFlag;
  }
  if (reportPath !== null) flags.push(`--jitcache-bench-report=${reportPath}`);
  const script = ["run.ts", job.case, `--calls=${job.calls}`, `--branch=${job.branch}`];
  if (killAtEnd) script.push("--kill-at-end");
  if (job.purpose === "census") script.push("--mark-calls");
  return { command: ["taskset", "--cpu-list", String(context.options.cpu), context.options.bun, ...flags, ...script], killAtEnd };
}

async function runJob(context: Context, job: Job): Promise<ProcessRecord> {
  const { options } = context;
  const label = labelOf(job);
  const base = join(context.processDirectory, label.replace(/ #?/g, "-"));
  const files = { stdout: `${base}.stdout`, stderr: `${base}.stderr` };
  const reportPath = job.purpose === "report" ? `${base}.bench.jsonl` : null;
  const artifact = artifactOf(options, job);
  const { command, killAtEnd } = commandOf(context, job, artifact, reportPath);
  const record: ProcessRecord = {
    kind: "process",
    ...job,
    label,
    command,
    exitCode: null,
    signal: null,
    timedOut: false,
    wallMilliseconds: null,
    startupMilliseconds: null,
    callMilliseconds: [],
    variants: [],
    checksums: [],
    cpuMicroseconds: null,
    contextSwitches: null,
    end: null,
    pinnedCpu: null,
    pageCache: null,
    committedBodies: null,
    report: null,
    compiles: null,
    strayLines: 0,
    problems: [],
  };
  if (job.mode === "consumerCold") dropPageCache(artifact!, context.fincore, record);

  // A census run has JSC log every optimizing compile it requests, on standard error between run.ts's call markers.
  const environment = job.purpose === "census" ? { ...context.environment, BUN_JSC_logCompilationChanges: "true" } : context.environment;
  const stdout = openSync(files.stdout, "w");
  const stderr = openSync(files.stderr, "w");
  const before = cpuCounters(options.cpu);
  try {
    const started = performance.now();
    const child = Bun.spawn(command, { cwd: benchDirectory, env: environment, stdin: "ignore", stdout, stderr });
    const timer = setTimeout(() => {
      record.timedOut = true;
      child.kill("SIGKILL");
    }, options.timeoutSeconds * 1000);
    await child.exited;
    record.wallMilliseconds = Math.round((performance.now() - started) * 1e4) / 1e4;
    clearTimeout(timer);
    record.exitCode = child.exitCode;
    record.signal = child.signalCode;
    const usage = child.resourceUsage();
    if (usage) {
      record.cpuMicroseconds = { user: Number(usage.cpuTime.user), system: Number(usage.cpuTime.system) };
      record.contextSwitches = { voluntary: Number(usage.contextSwitches.voluntary), involuntary: Number(usage.contextSwitches.involuntary) };
    }
  } catch (error) {
    record.problems.push(`could not start: ${(error as Error).message}`);
  } finally {
    closeSync(stdout);
    closeSync(stderr);
  }
  const after = cpuCounters(options.cpu);
  if (before && after) {
    const milliseconds = (ticks: number) => (ticks * 1000) / context.clockTicks;
    record.pinnedCpu = { busyMilliseconds: milliseconds(after.busy - before.busy), stealMilliseconds: milliseconds(after.steal - before.steal), idleMilliseconds: milliseconds(after.idle - before.idle) };
  }

  const stderrText = readText(files.stderr) ?? "";
  readEvents(readText(files.stdout) ?? "", job, record);
  if (record.wallMilliseconds !== null) checkExit(record, killAtEnd, options.timeoutSeconds, stderrText);
  checkPinning(record, options.cpu);
  // Bun warns once on standard error when JITCache turns itself off or stops writing (SPEC-integrator.md section 11.2),
  // and the process then measured native runs.
  if (artifact !== null) {
    for (const line of stderrText.split("\n")) if (line.startsWith("warn: JITCache")) record.problems.push(`warned: ${line}`);
  }
  if (job.purpose === "producer") {
    // sync writes the bodies back, so no write-back runs during a measured process and a cold consumer's drop finds
    // clean pages it can evict.
    Bun.spawnSync(["sync"]);
    record.committedBodies = countBodies(artifact!);
    if (!record.committedBodies) record.problems.push("committed no body");
  }
  if (reportPath !== null) readReportRun(reportPath, record);
  if (job.purpose === "census") readCensus(stderrText, job.calls, record);
  // The record holds everything a passing process printed. A failed process keeps its output for whoever looks into it,
  // a census run its compile log, a report run its report, and any process standard error that is not empty.
  if (!record.problems.length && job.purpose !== "census") {
    rmSync(files.stdout);
    if (!stderrText) rmSync(files.stderr);
  }
  return record;
}

// run.ts's lines: a ready line, the lines of calls 1 to N in order, then an end line. Other lines are counted and left
// alone, since a debug Bun can write its own.
function readEvents(text: string, job: Job, record: ProcessRecord) {
  let ready = false;
  let ended = false;
  let broken = false;
  for (const line of text.split("\n")) {
    let event: Record<string, unknown> | null = null;
    try {
      event = line.startsWith("{") ? JSON.parse(line) : null;
    } catch {}
    if (!event || typeof event !== "object" || !["ready", "call", "end"].includes(event.event as string)) {
      if (line.trim()) record.strayLines++;
      continue;
    }
    if (ended) {
      record.problems.push(`printed a ${event.event} line after its end line`);
      continue;
    }
    if (event.event === "ready") {
      if (ready) record.problems.push("printed a second ready line");
      ready = true;
      const startup = numberOr(event.startupNanoseconds, null);
      if (startup === null) record.problems.push(`printed a ready line without startupNanoseconds: ${line}`);
      else record.startupMilliseconds = Math.round(startup / 100) / 1e4;
    } else if (event.event === "call") {
      if (broken) continue;
      const call = record.callMilliseconds.length + 1;
      const milliseconds = numberOr(event.milliseconds, null);
      const variant = numberOr(event.variant, null);
      const checksum = typeof event.checksum === "string" ? event.checksum : "";
      // The fixed branch sends variant 1 at every call.
      if (!ready || event.call !== call || milliseconds === null || variant === null || (job.branch === "fixed" && variant !== 1) || !checksum) {
        record.problems.push(`printed ${line} where the line of call ${call} was due`);
        broken = true;
        continue;
      }
      record.callMilliseconds.push(Math.round(milliseconds * 1e4) / 1e4); // performance.now() resolves microseconds
      record.variants.push(variant);
      record.checksums.push(checksum.slice(0, 16));
    } else {
      ended = true;
      const threads = event.threads !== null && typeof event.threads === "object" ? (event.threads as Record<string, number>) : {};
      const cpusAllowed = typeof event.cpusAllowed === "string" ? event.cpusAllowed : null;
      record.end = { rssBytes: numberOr(event.rssBytes, null), peakRssBytes: numberOr(event.peakRssBytes, null), cpusAllowed, hardwareConcurrency: numberOr(event.hardwareConcurrency, null), threads };
    }
  }
  if (!ready) record.problems.push("printed no ready line");
  if (!broken && record.callMilliseconds.length !== job.calls) record.problems.push(`printed ${record.callMilliseconds.length} call lines of ${job.calls}`);
  if (!ended) record.problems.push("printed no end line");
}

function checkExit(record: ProcessRecord, killAtEnd: boolean, timeoutSeconds: number, stderrText: string) {
  const ending = record.signal ? `ended by ${record.signal}` : `exited with ${record.exitCode}`;
  let failed = true;
  if (record.timedOut) record.problems.push(`was killed after ${timeoutSeconds} seconds`);
  else if (killAtEnd && record.signal !== "SIGKILL") record.problems.push(`${ending} where it should have killed itself after its end line`);
  else if (!killAtEnd && (record.signal !== null || record.exitCode !== 0)) record.problems.push(ending);
  else failed = false;
  if (failed && stderrText.trim()) record.problems.push(`standard error ends with: ${tail(stderrText)}`);
}

// HARNESS.md: each process sees one CPU and keeps its JIT worker threads, so the worker competes with execution.
function checkPinning(record: ProcessRecord, cpu: number) {
  const end = record.end;
  if (end) {
    if (end.cpusAllowed !== String(cpu)) record.problems.push(`may run on CPUs ${end.cpusAllowed}, not on CPU ${cpu} alone`);
    if (end.hardwareConcurrency !== 1) record.problems.push(`saw a hardwareConcurrency of ${end.hardwareConcurrency}, not 1`);
    if (!(end.threads.JITWorker >= 1)) record.problems.push("had no thread named JITWorker at its end line, so no JIT worker ran beside it");
  }
  const cpuMilliseconds = cpuMillisecondsOf(record);
  if (cpuMilliseconds !== null && record.wallMilliseconds !== null && cpuMilliseconds > record.wallMilliseconds * cpuTimeSlack) {
    record.problems.push(`spent ${fixed(cpuMilliseconds)} ms of CPU time in ${fixed(record.wallMilliseconds)} ms, so it also ran on another CPU`);
  }
}

// dd's nocache flag with count=0 asks the kernel to drop a file's pages from the page cache (posix_fadvise with
// POSIX_FADV_DONTNEED), which evicts the clean pages the producer's sync left. fincore, where it exists, checks that they
// left.
function dropPageCache(artifact: string, fincore: boolean, record: ProcessRecord) {
  let files: string[];
  try {
    files = listFiles(artifact);
  } catch (error) {
    record.problems.push(`found no artifact to drop from the page cache: ${(error as Error).message}`);
    return;
  }
  for (const file of files) {
    const result = Bun.spawnSync(["dd", `if=${file}`, "iflag=nocache", "count=0", "status=none"], { stdout: "ignore", stderr: "pipe" });
    if (!result.success) {
      record.problems.push(`could not drop ${file} from the page cache: ${result.stderr.toString().trim()}`);
      break;
    }
  }
  let residentBytes: number | null = null;
  if (fincore && files.length) {
    const result = Bun.spawnSync(["fincore", "--bytes", "--noheadings", "--output", "RES", ...files], { stdout: "pipe", stderr: "pipe" });
    if (result.success) residentBytes = sum(result.stdout.toString().split("\n").filter(Boolean).map(Number));
    else record.problems.push(`could not check the page cache with fincore: ${result.stderr.toString().trim()}`);
    if (residentBytes) record.problems.push(`kept ${residentBytes} bytes of its artifact in the page cache after the drop`);
  }
  record.pageCache = { files: files.length, residentBytes };
}

function listFiles(directory: string): string[] {
  const files: string[] = [];
  for (const entry of readdirSync(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) files.push(...listFiles(path));
    else if (entry.isFile()) files.push(path);
  }
  return files;
}

// The bodies an artifact holds, by their names (SPEC-integrator.container.md section 1.1).
function countBodies(artifact: string): number {
  try {
    return readdirSync(join(artifact, "cache", "bodies")).filter(name => /^[0-9a-f]{80}\.bin$/.test(name)).length;
  } catch {
    return 0;
  }
}

// A report run must have started JITCache as a consumer and installed bodies, which shows the case happened.
function readReportRun(path: string, record: ProcessRecord) {
  const text = readText(path);
  if (text === null) {
    record.problems.push(`wrote no bench report at ${path}`);
    return;
  }
  const counts = (record.report = readReport(text));
  if (counts.start?.outcome !== "started" || counts.start?.role !== "consumer") record.problems.push(`did not start JITCache as a consumer: its start event is ${JSON.stringify(counts.start)}`);
  if (!counts.installs) record.problems.push("installed no body");
  if (counts.malformedLines) record.problems.push(`wrote ${plural(counts.malformedLines, "bench report line that is", "bench report lines that are")} no event`);
}

// JSC logs `DFG(Driver) compiling <code block> with <mode>, instructions size = <n>` when it requests an optimizing
// compile, on the thread that runs the call, so the line falls between that call's markers, and the log's lock keeps it
// whole. A JIT worker writes its own lines in pieces, so a marker can land inside one of them and end that line.
function readCensus(text: string, calls: number, record: ProcessRecord) {
  const dfg = new Array<number>(calls + 1).fill(0);
  const ftl = new Array<number>(calls + 1).fill(0);
  let call = 0;
  for (const line of text.split("\n")) {
    const compile = /^DFG\(Driver\) compiling .* with (\w+), instructions size = \d+$/.exec(line);
    if (compile) {
      const mode = compile[1];
      if (mode === "DFG" || mode === "UnlinkedDFG") dfg[call]++;
      else if (mode === "FTL" || mode === "FTLForOSREntry") ftl[call]++;
      else record.problems.push(`logged an optimizing compile in mode ${mode}, which the census does not count`);
      continue;
    }
    if (call < calls && line.endsWith(`call ${call + 1}`)) ++call;
  }
  if (call !== calls) record.problems.push(`marked ${call} of its ${calls} calls on standard error`);
  record.compiles = { dfg, ftl };
}

// ---------------------------------------------------------------------------------------------------------------------
// The plan

// One repetition's jobs in the order they run: the producers, then, for each process index, the modes of every case,
// branch and scenario in an order that rotates with the index, so no mode always runs right after the same one. Cold
// consumer c, counted from 0, runs at index floor(c * processes / coldProcesses) + 1, which spreads them over the indexes.
function repetitionJobs(options: Options, repetition: number): Job[] {
  const jobs: Job[] = [];
  const longest = options.scenarios.at(-1)!;
  for (const name of options.cases) {
    for (const branch of options.branches) {
      for (const mode of ["producer", "producerNoDelta"] as const) jobs.push({ purpose: "producer", case: name, branch, scenario: null, calls: options.producerCalls, mode, repetition, index: 1 });
    }
  }
  for (let index = 1; index <= options.processes; ++index) {
    const colds = [...Array(options.coldProcesses).keys()].filter(cold => Math.floor((cold * options.processes) / options.coldProcesses) + 1 === index);
    for (const name of options.cases) {
      for (const branch of options.branches) {
        for (const scenario of options.scenarios) {
          const job = (mode: Mode, modeIndex: number): Job => ({ purpose: "timed", case: name, branch, scenario, calls: scenario, mode, repetition, index: modeIndex });
          const group = [job("off", index), job("consumer", index)];
          if (scenario === longest) group.push(job("consumerNoDelta", index));
          for (const cold of colds) group.push(job("consumerCold", cold + 1));
          const shift = (index - 1) % group.length;
          jobs.push(...group.slice(shift), ...group.slice(0, shift));
        }
      }
    }
  }
  return jobs;
}

// After the last repetition, on its artifacts: for each case and branch, a warm and a cold consumer of every scenario
// that write the bench report, and the census's off process and consumer, which make the longest scenario's calls.
function untimedJobs(options: Options): Job[] {
  const jobs: Job[] = [];
  const repetition = options.repetitions;
  const longest = options.scenarios.at(-1)!;
  for (const name of options.cases) {
    for (const branch of options.branches) {
      if (options.reports) {
        for (const scenario of options.scenarios) {
          for (const mode of ["consumer", "consumerCold"] as const) jobs.push({ purpose: "report", case: name, branch, scenario, calls: scenario, mode, repetition, index: 1 });
        }
      }
      if (options.census) {
        for (const mode of ["off", "consumer"] as const) jobs.push({ purpose: "census", case: name, branch, scenario: null, calls: longest, mode, repetition, index: 1 });
      }
    }
  }
  return jobs;
}

function progressLine(record: ProcessRecord): string {
  const calls = record.callMilliseconds;
  const parts = [`startup ${milliseconds(record.startupMilliseconds)}`];
  if (calls.length) parts.push(`call 1 ${milliseconds(calls[0])}`);
  if (calls.length > 1) parts.push(`call ${calls.length} ${milliseconds(calls.at(-1))}`);
  parts.push(`wall ${milliseconds(record.wallMilliseconds)}`, `CPU ${milliseconds(cpuMillisecondsOf(record))}`);
  if (record.committedBodies !== null) parts.push(`${plural(record.committedBodies, "body", "bodies")} committed`);
  if (record.report) parts.push(`${plural(record.report.installs, "install", "installs")}`);
  if (record.compiles) parts.push(`${sum(record.compiles.dfg)} DFG and ${sum(record.compiles.ftl)} FTL compiles`);
  if (record.pageCache) parts.push(`${plural(record.pageCache.files, "artifact file", "artifact files")} dropped from the page cache`);
  const line = `${record.label}: ${parts.join(", ")}`;
  return record.problems.length ? `${line}\n  FAILED: ${record.problems.join("\n  FAILED: ")}` : line;
}

// ---------------------------------------------------------------------------------------------------------------------
// Results

class ResultsFile {
  constructor(readonly path: string) {
    writeFileSync(path, "");
  }

  write(record: object) {
    appendFileSync(this.path, `${JSON.stringify(record)}\n`);
  }
}

interface RunRecord extends MachineFacts {
  kind: "run";
  run?: number;
  started: string;
  argv: string[];
  options: Options;
  bun: string;
  revision: string;
  deltaAtExitFlag: boolean;
  pinnedCpuSiblings: number[]; // the CPUs that share a core with options.cpu, itself included
  driverCpu: number | null;
  driverNote: string | null;
  fincore: boolean;
  clockTicks: number;
  environment: Record<string, string>; // every Bun process's
}

interface RepetitionRecord {
  kind: "repetition";
  run?: number;
  repetition: number;
  started: string;
  idle: boolean; // false marks the repetition noisy
  waitedSeconds: number;
  loadAverage: number[];
  busiest: BusyProcess[];
}

interface EndRecord {
  kind: "end";
  run?: number;
  finished: string;
  loadAverage: number[];
}

// The records of one or more results files. Each carries its file's ordinal as `run`, which keeps the repetitions of
// pooled runs apart.
interface Pool {
  runs: RunRecord[];
  repetitions: RepetitionRecord[];
  processes: ProcessRecord[];
  ends: EndRecord[];
}

function readResults(paths: string[]): Pool {
  const pool: Pool = { runs: [], repetitions: [], processes: [], ends: [] };
  paths.forEach((path, run) => {
    readFileSync(path, "utf8")
      .split("\n")
      .forEach((line, number) => {
        if (!line) return;
        let record: { kind?: unknown };
        try {
          record = { ...JSON.parse(line), run };
        } catch {
          throw new Error(`${path}:${number + 1} holds no JSON record`);
        }
        if (record.kind === "run") pool.runs.push(record as RunRecord);
        else if (record.kind === "repetition") pool.repetitions.push(record as RepetitionRecord);
        else if (record.kind === "process") pool.processes.push(record as ProcessRecord);
        else if (record.kind === "end") pool.ends.push(record as EndRecord);
      });
  });
  return pool;
}

// ---------------------------------------------------------------------------------------------------------------------
// The summary

type Measure = (record: ProcessRecord) => number | null;

const cpuMillisecondsOf: Measure = record => (record.cpuMicroseconds ? (record.cpuMicroseconds.user + record.cpuMicroseconds.system) / 1000 : null);
const peakMebibytes: Measure = record => (typeof record.end?.peakRssBytes === "number" ? record.end.peakRssBytes / 1048576 : null);
// Startup plus the first k calls, as run.ts times them.
const finishedAfter =
  (calls: number): Measure =>
  record =>
    record.startupMilliseconds === null || record.callMilliseconds.length < calls ? null : record.startupMilliseconds + sum(record.callMilliseconds.slice(0, calls));
// Orders repetitions as they ran, across pooled runs too.
const repetitionOrder = (record: { run?: number; repetition: number }) => (record.run ?? 0) * 1_000_000 + record.repetition;
const labelIn = (pool: Pool, record: ProcessRecord) => (pool.runs.length > 1 ? `run ${(record.run ?? 0) + 1} ${record.label}` : record.label);

const readingGuide = [
  "A cell over several processes gives their median, and wall and CPU time add the range in parentheses. CPU time adds the user and system time of all of a process's threads; peak memory is its VmHWM at its end line.",
  `A ratio divides a mode's median by the median of the off processes in the same table, pooled over the repetitions, and each repetition's own ratio follows in parentheses. Bold marks a ratio at or below ${targetRatio.toFixed(2)}, the target in HARNESS.md.`,
  "After k calls means startup plus the first k calls, as run.ts times them. W is the producers' warmed call, the median of their calls 91 to 100 that sent the same variant. Ck is a mode's median time for call k (Nk with JITCache off), so Ck/W says how close call k comes to the warmed call.",
].join(" ");

function summarize(pool: Pool): { text: string; passed: boolean } {
  const problems = collectProblems(pool);
  const lines = ["# JITCache macro benches", ""];
  for (const run of pool.runs) lines.push(describeRun(run, pool), "");
  const revisions = [...new Set(pool.runs.map(run => run.revision))];
  if (revisions.length > 1) lines.push(`The pooled runs used different Bun builds (${revisions.join(", ")}), so their numbers do not compare.`, "");
  if (pool.runs.some(run => run.options.check)) lines.push("A check (--check) takes no build lock and may run beside a build, so its times measure nothing; only its checks count.", "");
  if (pool.repetitions.length) lines.push(...pool.repetitions.map(repetition => `- ${describeRepetition(repetition, pool)}`), "");
  lines.push("## Checks", "", ...describeProblems(problems), "", "## Reading the tables", "", readingGuide, "");
  for (const name of caseNames) {
    for (const branch of branchNames) {
      const records = pool.processes.filter(record => record.case === name && record.branch === branch);
      if (records.length) lines.push(...caseLines(pool, records, name, branch));
    }
  }
  return { text: lines.join("\n"), passed: !problems.length };
}

function describeRun(run: RunRecord, pool: Pool): string {
  const { options, memory } = run;
  const gibibytes = (bytes: number | null) => (bytes === null ? "n/a" : `${(bytes / 2 ** 30).toFixed(1)} GiB`);
  const swapUsed = memory.swapTotalBytes === null || memory.swapFreeBytes === null ? null : memory.swapTotalBytes - memory.swapFreeBytes;
  const load = (average: number[]) => average.map(value => value.toFixed(2)).join(", ");
  const end = pool.ends.find(record => record.run === run.run);
  const siblings = run.pinnedCpuSiblings.filter(cpu => cpu !== options.cpu);
  const users = run.who ? run.who.split("\n").map(line => line.trim().split(/\s+/).slice(0, 2).join(" on ")).join("; ") : "nobody";
  return [
    `${pool.runs.length > 1 ? `Run ${(run.run ?? 0) + 1}: ` : ""}Bun ${run.bun}, revision ${run.revision}.`,
    run.deltaAtExitFlag ? "" : "This Bun has no --jitcache-delta-at-exit and calls delta at every producer's exit, so the producers without delta kill themselves right after their end line.",
    `Host ${run.host}, kernel ${run.kernel}, ${run.cpus} CPUs (${run.cpuModel ?? "model unknown"}), transparent huge pages ${run.transparentHugePages ?? "n/a"}, ${gibibytes(memory.totalBytes)} of memory (${gibibytes(memory.availableBytes)} available) and ${gibibytes(memory.swapTotalBytes)} of swap (${gibibytes(swapUsed)} in use); logged in: ${users}.`,
    `Every Bun process ran on CPU ${options.cpu}${siblings.length ? `, which shares its core with CPU ${joinWords(siblings.map(String))}` : ""}, and the driver ${run.driverCpu === null ? "had no CPU of its own" : `ran on CPU ${run.driverCpu}`}${run.driverNote ? ` (${run.driverNote})` : ""}.`,
    `${plural(options.repetitions, "repetition", "repetitions")} of ${plural(options.processes, "process", "processes")} per mode, scenarios of ${joinWords(options.scenarios.map(String))} calls, producers of ${options.producerCalls} calls and ${plural(options.coldProcesses, "cold consumer", "cold consumers")} per scenario and repetition.`,
    `Started ${run.started} with a load average of ${load(run.loadAverage)}${end ? ` and finished ${end.finished} with ${load(end.loadAverage)}` : "; the run has no end record"}. Command line: ${run.argv.join(" ")}.`,
  ]
    .filter(Boolean)
    .join(" ");
}

function describeRepetition(repetition: RepetitionRecord, pool: Pool): string {
  const name = pool.runs.length > 1 ? `Run ${(repetition.run ?? 0) + 1}, repetition ${repetition.repetition}` : `Repetition ${repetition.repetition}`;
  const waited = repetition.waitedSeconds ? ` after a wait of ${repetition.waitedSeconds} s` : "";
  const load = repetition.loadAverage[0].toFixed(2);
  if (repetition.idle) return `${name} started on an idle machine${waited}, with a load average of ${load}.`;
  const busiest = repetition.busiest.map(entry => `${entry.command} (pid ${entry.pid}, uid ${entry.uid}, ${entry.percent}%)`);
  return `${name} is noisy: it started${waited} with a load average of ${load}${busiest.length ? `, and the busiest processes were ${joinWords(busiest)}` : ""}.`;
}

// Each process's own problems, and each process whose response at some call differs from the first response any process
// of its case and branch got at that call: THREAD Verification has a consumer compute what a JITCache-off process does.
function collectProblems(pool: Pool): { label: string; text: string }[] {
  const problems: { label: string; text: string }[] = [];
  const first = new Map<string, { checksum: string; variant: number; label: string }>();
  for (const record of pool.processes) {
    const label = labelIn(pool, record);
    for (const text of record.problems) problems.push({ label, text });
    for (let index = 0; index < record.checksums.length; ++index) {
      const key = `${record.case} ${record.branch} ${index}`;
      const checksum = record.checksums[index];
      const variant = record.variants[index];
      const reference = first.get(key);
      if (!reference) {
        first.set(key, { checksum, variant, label });
      } else if (reference.checksum !== checksum || reference.variant !== variant) {
        problems.push({ label, text: `call ${index + 1} sent variant ${variant} and got checksum ${checksum}, where ${reference.label} sent variant ${reference.variant} and got ${reference.checksum}` });
        break;
      }
    }
  }
  return problems;
}

// Problems grouped by what went wrong, each with the processes it happened in.
function describeProblems(problems: { label: string; text: string }[]): string[] {
  if (!problems.length) return ["Every check passed."];
  const groups = new Map<string, string[]>();
  for (const { label, text } of problems) groups.set(text, [...(groups.get(text) ?? []), label]);
  const lines = [`${plural(problems.length, "problem", "problems")}:`, ""];
  for (const [text, labels] of groups) lines.push(`- ${labels.length > 3 ? `${labels.slice(0, 3).join(", ")} and ${labels.length - 3} more` : labels.join(", ")}: ${text}`);
  return lines;
}

function caseLines(pool: Pool, records: ProcessRecord[], name: CaseName, branch: Branch): string[] {
  const producers = records.filter(record => record.purpose === "producer");
  const warmed = warmedByVariant(producers);
  const lines = [`## ${name}, ${branch}`, "", [describeProducers(producers), describeWarmed(warmed, producers)].filter(Boolean).join(" "), ""];
  const census = records.filter(record => record.purpose === "census" && record.compiles);
  if (census.length) lines.push(...censusLines(pool, census));
  const scenarios = [...new Set(records.flatMap(record => (record.scenario === null ? [] : [record.scenario])))].sort((a, b) => a - b);
  for (const scenario of scenarios) lines.push(...scenarioLines(records.filter(record => record.scenario === scenario), scenario, warmed));
  return lines;
}

function describeProducers(producers: ProcessRecord[]): string {
  if (!producers.length) return "No producer ran.";
  const calls = joinWords([...new Set(producers.map(record => String(record.calls)))], "or");
  const committed = (["producer", "producerNoDelta"] as const).flatMap(mode => {
    const counts = producers.filter(record => record.mode === mode).map(record => (record.committedBodies === null ? "n/a" : String(record.committedBodies)));
    return counts.length ? [`${joinWords(counts)} bodies ${mode === "producer" ? "when they called delta at exit" : "when they did not"}`] : [];
  });
  return `Producers of ${calls} calls, one of each kind per repetition, committed ${committed.join(", and ")}.`;
}

// W by variant: the median of the producers' last ten calls that sent it. In the fixed branch that is the median of
// those ten calls; in the varied branch each call k compares with the warmed time of the request it sent.
function warmedByVariant(producers: ProcessRecord[]): Map<number, number> {
  const samples = new Map<number, number[]>();
  for (const record of producers) {
    for (let index = Math.max(0, record.callMilliseconds.length - warmedCalls); index < record.callMilliseconds.length; ++index) {
      samples.set(record.variants[index], [...(samples.get(record.variants[index]) ?? []), record.callMilliseconds[index]]);
    }
  }
  return new Map([...samples].sort(([a], [b]) => a - b).map(([variant, values]) => [variant, median(values)!]));
}

function describeWarmed(warmed: Map<number, number>, producers: ProcessRecord[]): string {
  if (!warmed.size) return "";
  const lengths = [...new Set(producers.map(record => record.callMilliseconds.length))];
  const span = lengths.length === 1 && lengths[0] >= warmedCalls ? `calls ${lengths[0] - warmedCalls + 1} to ${lengths[0]}` : `last ${warmedCalls} calls`;
  if (warmed.size === 1) return `W, the median of their ${span}, is ${fixed([...warmed.values()][0])} ms.`;
  return `W by variant, the median of their ${span} that sent it, in ms: ${[...warmed].map(([variant, value]) => `${variant}: ${fixed(value)}`).join(", ")}.`;
}

// The optimizing compiles requested in each window of calls. A producer that stops after P calls takes warm baseline
// state only from bodies still in baseline then, so most DFG compiles should come after call P.
function censusLines(pool: Pool, census: ProcessRecord[]): string[] {
  const calls = Math.max(...census.map(record => record.calls));
  const bounds = calls ? [...censusBounds.filter(bound => bound < calls), calls] : [];
  const windows = [{ label: "startup", from: 0, to: 0 }, ...bounds.map((to, i) => ({ label: `${i ? bounds[i - 1] + 1 : 1}-${to}`, from: i ? bounds[i - 1] + 1 : 1, to })), { label: "all", from: 0, to: calls }];
  const modes = (["off", "consumer"] as const).filter(mode => census.some(record => record.mode === mode));
  const count = (mode: Mode, kind: "dfg" | "ftl", from: number, to: number) => {
    const value = median(census.filter(record => record.mode === mode).map(record => sum(record.compiles![kind].slice(from, to + 1))));
    return value === null ? "n/a" : String(value);
  };
  // The DFG compiles after the call where the producer stopped, over all of a mode's census runs.
  const producerCalls = (record: ProcessRecord) => pool.runs.find(run => run.run === record.run)?.options.producerCalls ?? 0;
  const shares = modes.map(mode => {
    const runs = census.filter(record => record.mode === mode);
    const late = sum(runs.map(record => sum(record.compiles!.dfg.slice(producerCalls(record) + 1))));
    const all = sum(runs.map(record => sum(record.compiles!.dfg)));
    return `${late} of ${all}${all ? ` (${Math.round((late / all) * 100)}%)` : ""} ${mode === "off" ? "with JITCache off" : "in the consumer"}`;
  });
  const stops = [...new Set(census.map(producerCalls))];
  const where = `call ${joinWords(stops.map(String), "or")}, where the producers stopped`;
  return [
    `Census: one untimed process per mode made ${plural(calls, "call", "calls")}, and JSC requested these DFG and FTL compiles in each window of calls.`,
    "",
    `| calls | ${modes.flatMap(mode => [`${mode}: DFG`, `${mode}: FTL`]).join(" | ")} |`,
    `|---|${modes.map(() => "---|---|").join("")}`,
    ...windows.map(({ label, from, to }) => `| ${label} | ${modes.flatMap(mode => [count(mode, "dfg", from, to), count(mode, "ftl", from, to)]).join(" | ")} |`),
    "",
    calls > Math.min(...stops) ? `DFG compiles after ${where}: ${joinWords(shares)}.` : `The census ends before or at ${where}, so it shows nothing after it.`,
    "",
  ];
}

function scenarioLines(records: ProcessRecord[], scenario: number, warmed: Map<number, number>): string[] {
  const timed = records.filter(record => record.purpose === "timed");
  const of = (mode: Mode) => timed.filter(record => record.mode === mode);
  const modes = comparedModes.filter(mode => of(mode).length);
  const off = of("off");
  // The pinned CPU idles only while the process waits on something other than its own threads' work, such as the
  // kernel's teardown of an inotify watch at exit; JITCache off idles about none, so that row has no ratio.
  const rows: { label: string; ratioLabel?: string; measure: Measure; range?: boolean; unit: string; noRatio?: true }[] = [
    { label: "wall time", measure: record => record.wallMilliseconds, range: true, unit: "ms" },
    { label: "CPU time", measure: cpuMillisecondsOf, range: true, unit: "ms" },
    { label: "pinned CPU idle", measure: record => record.pinnedCpu?.idleMilliseconds ?? null, unit: "ms", noRatio: true },
    { label: "startup", measure: record => record.startupMilliseconds, unit: "ms" },
    { label: "first call", ratioLabel: "first call, C1/N1", measure: record => record.callMilliseconds[0] ?? null, unit: "ms" },
    ...[...finishCalls.filter(calls => calls < scenario), scenario]
      .filter(calls => calls > 0)
      .map(calls => ({ label: `after ${plural(calls, "call", "calls")}`, measure: finishedAfter(calls), unit: "ms" })),
    { label: "peak memory", measure: peakMebibytes, unit: "MiB" },
  ];
  const lines = [`### ${plural(scenario, "call", "calls")}`, ""];
  if (modes.length) {
    lines.push(`| | ${modes.join(" | ")} |`, `|---|${modes.map(() => "---|").join("")}`, `| processes | ${modes.map(mode => of(mode).length).join(" | ")} |`);
    for (const { label, measure, range, unit } of rows) lines.push(`| ${label}, ${unit} | ${modes.map(mode => cell(of(mode), measure, range ?? false)).join(" | ")} |`);
    lines.push("");
  }
  const compared = modes.filter(mode => mode !== "off");
  if (off.length && compared.length) {
    lines.push(`| ratio to off | ${compared.join(" | ")} |`, `|---|${compared.map(() => "---|").join("")}`);
    for (const { label, ratioLabel, measure, noRatio } of rows) {
      if (!noRatio) lines.push(`| ${ratioLabel ?? label} | ${compared.map(mode => ratioCell(of(mode), off, measure)).join(" | ")} |`);
    }
    lines.push("");
  }
  const cold = of("consumerCold");
  const warm = of("consumer");
  if (cold.length && warm.length) {
    const versus = (measure: Measure) => ratio(divide(median(cold.map(measure)), median(warm.map(measure))));
    const resident = present(cold.map(record => record.pageCache?.residentBytes));
    const checked = resident.length ? `fincore found at most ${Math.max(...resident)} bytes of the artifact still cached after a drop` : "fincore was missing, so nothing checked that the pages left";
    lines.push(`Cold consumers, which start after their artifact's pages are dropped from the page cache, took ${versus(record => record.wallMilliseconds)} times the warm consumers' wall time and ${versus(record => record.startupMilliseconds)} times their startup; ${checked}.`, "");
  }
  const points = curveCalls.filter(calls => calls <= scenario);
  if (warmed.size && modes.length && points.length) {
    lines.push(`| k | ${points.join(" | ")} |`, `|---|${points.map(() => "---|").join("")}`);
    for (const mode of modes) {
      const cells = points.map(k => {
        const reached = of(mode).filter(record => record.callMilliseconds.length >= k);
        const w = reached.length ? warmed.get(reached[0].variants[k - 1]) : undefined;
        return w === undefined ? "n/a" : ratio(divide(median(reached.map(record => record.callMilliseconds[k - 1])), w));
      });
      lines.push(`| ${mode === "off" ? "off, Nk/W" : `${mode}, Ck/W`} | ${cells.join(" | ")} |`);
    }
    lines.push("");
  }
  const reports = records.filter(record => record.purpose === "report" && record.report);
  if (reports.length) {
    const columns = (["consumer", "consumerCold"] as const).filter(mode => reports.some(record => record.mode === mode));
    const counts: [string, (counts: ReportCounts) => number][] = [
      ["installs", counts => counts.installs],
      ["imports", counts => counts.requests.import ?? 0],
      ["misses, bodies the artifact lacks", counts => counts.lookups["bodyVersion.absent"] ?? 0],
      ["native baseline compiles", counts => counts.compiles],
    ];
    lines.push(
      "Untimed consumers with JITCache's bench report:",
      "",
      `| | ${columns.map(mode => (mode === "consumer" ? "warm consumer" : "cold consumer")).join(" | ")} |`,
      `|---|${columns.map(() => "---|").join("")}`,
      ...counts.map(([label, read]) => `| ${label} | ${columns.map(mode => spread(reports.filter(record => record.mode === mode).map(record => read(record.report!)))).join(" | ")} |`),
      "",
    );
  }
  const steal = present(timed.map(record => record.pinnedCpu?.stealMilliseconds));
  const other = present(
    timed.map(record => {
      const own = cpuMillisecondsOf(record);
      return record.pinnedCpu && own !== null ? Math.max(0, record.pinnedCpu.busyMilliseconds - own) : null;
    }),
  );
  if (steal.length && other.length) {
    lines.push(`While each of these processes ran, /proc/stat counted at most ${fixed(Math.max(...steal))} ms stolen from its CPU by the hypervisor, and at most ${fixed(Math.max(...other))} ms (median ${fixed(median(other)!)} ms) of work on it other than the process's own.`, "");
  }
  return lines;
}

function cell(records: ProcessRecord[], measure: Measure, range: boolean): string {
  const values = present(records.map(measure)).sort((a, b) => a - b);
  if (!values.length) return "n/a";
  const middle = fixed(median(values)!);
  return range && values.length > 1 ? `${middle} (${fixed(values[0])}-${fixed(values.at(-1)!)})` : middle;
}

// The ratio of the medians over every repetition, then each repetition's own in the order they ran.
function ratioCell(records: ProcessRecord[], off: ProcessRecord[], measure: Measure): string {
  const ratioOf = (mine: ProcessRecord[], theirs: ProcessRecord[]) => divide(median(mine.map(measure)), median(theirs.map(measure)));
  const pooled = markedRatio(ratioOf(records, off));
  const repetitions = [...new Set([...records, ...off].map(repetitionOrder))].sort((a, b) => a - b);
  if (repetitions.length < 2) return pooled;
  const each = repetitions.map(order => markedRatio(ratioOf(records.filter(record => repetitionOrder(record) === order), off.filter(record => repetitionOrder(record) === order))));
  return `${pooled} (${each.join(", ")})`;
}

// A count over several processes: the median, with the range when they differ.
function spread(values: number[]): string {
  if (!values.length) return "n/a";
  const sorted = [...values].sort((a, b) => a - b);
  const middle = String(median(sorted));
  return sorted[0] === sorted.at(-1) ? middle : `${middle} (${sorted[0]}-${sorted.at(-1)})`;
}

const plural = (count: number, noun: string, nouns: string) => `${count} ${count === 1 ? noun : nouns}`;
const sum = (values: number[]) => values.reduce((total, value) => total + value, 0);
const present = (values: (number | null | undefined)[]) => values.filter((value): value is number => typeof value === "number" && Number.isFinite(value));
const divide = (a: number | null, b: number | null) => (a === null || b === null || b === 0 ? null : a / b);

function median(values: (number | null | undefined)[]): number | null {
  const sorted = present(values).sort((a, b) => a - b);
  if (!sorted.length) return null;
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

// Milliseconds or mebibytes, with the precision their size calls for.
const fixed = (value: number) => value.toFixed(Math.abs(value) < 10 ? 2 : Math.abs(value) < 1000 ? 1 : 0);
const ratio = (value: number | null) => (value === null ? "n/a" : value.toFixed(2));
const markedRatio = (value: number | null) => (value !== null && value <= targetRatio ? `**${ratio(value)}**` : ratio(value));

function milliseconds(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "n/a";
  if (value >= 10_000) return `${(value / 1000).toFixed(1)} s`;
  return `${value.toFixed(value < 10 ? 2 : 1)} ms`;
}

function joinWords(words: string[], conjunction = "and"): string {
  return words.length < 2 ? (words[0] ?? "") : `${words.slice(0, -1).join(", ")} ${conjunction} ${words.at(-1)}`;
}

function readText(path: string): string | null {
  try {
    return readFileSync(path, "utf8");
  } catch {
    return null;
  }
}

function tail(text: string, lines = 8): string {
  return text.trimEnd().split("\n").slice(-lines).join("\n");
}

// ---------------------------------------------------------------------------------------------------------------------

// What keeps the bench from running at all.
function refusal(): string | null {
  if (!statSync(join(benchDirectory, "node_modules"), { throwIfNoEntry: false })?.isDirectory()) return `${benchDirectory} has no node_modules; run bun install --frozen-lockfile there`;
  // Bun reads bunfig.toml from the directory it runs in, so a file there would configure every process.
  if (existsSync(join(benchDirectory, "bunfig.toml"))) return `${benchDirectory} holds a bunfig.toml, which would configure every Bun process the bench runs`;
  for (const tool of ["taskset", "dd", "sync", "flock", "getconf", "who"]) if (!Bun.which(tool)) return `${tool} is not on PATH`;
  return null;
}

async function main(argv: string[]): Promise<number> {
  const allowed = allowedCpus();
  let command: Command;
  try {
    command = parseCommandLine(argv, allowed.list);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    console.error(`jitcache-macro-bench: ${error.message}\n${usage}`);
    return 2;
  }
  if (command.kind === "summarize") {
    let pool: Pool;
    try {
      pool = readResults(command.files);
    } catch (error) {
      console.error(`jitcache-macro-bench: ${(error as Error).message}`);
      return 1;
    }
    const { text, passed } = summarize(pool);
    const directory = command.out ?? dirname(command.files[0]);
    mkdirSync(directory, { recursive: true });
    const summaryPath = join(directory, "summary.md");
    writeFileSync(summaryPath, text);
    console.log(`${text}\nsummary: ${summaryPath}`);
    return passed ? 0 : 1;
  }

  const { options } = command;
  const refused = refusal();
  if (refused) {
    console.error(`jitcache-macro-bench: ${refused}`);
    return 1;
  }
  const processDirectory = join(options.out, "processes");
  mkdirSync(processDirectory, { recursive: true });
  mkdirSync(join(options.out, "artifacts"));
  const results = new ResultsFile(join(options.out, "results.jsonl"));
  const environment = processEnvironment();
  const clockTicks = Number(Bun.spawnSync(["getconf", "CLK_TCK"]).stdout.toString().trim()) || 100;
  const driver = pinDriver(options.cpu, allowed.list);
  // A check measures nothing, so it leaves the lock to the builds it may run beside.
  const releaseLock = options.check ? async () => {} : await takeBuildLock();
  try {
    const probe = (flag: string) => Bun.spawnSync([options.bun, flag], { env: environment, stdout: "pipe", stderr: "pipe" });
    const revisionProbe = probe("--revision");
    const revision = revisionProbe.stdout.toString().trim() || `--revision failed: ${revisionProbe.stderr.toString().trim()}`;
    // A Bun that takes --jitcache-delta-at-exit lists it in its help.
    const deltaAtExitFlag = probe("--help").stdout.toString().includes("--jitcache-delta-at-exit");
    const context: Context = { options, environment, deltaAtExitFlag, fincore: Bun.which("fincore") !== null, clockTicks, processDirectory };
    const runRecord: RunRecord = {
      kind: "run",
      started: new Date().toISOString(),
      argv,
      options,
      bun: options.bun,
      revision,
      deltaAtExitFlag,
      ...machineFacts(),
      pinnedCpuSiblings: smtSiblings(options.cpu),
      driverCpu: driver.cpu,
      driverNote: driver.note,
      fincore: context.fincore,
      clockTicks,
      environment,
    };
    results.write(runRecord);
    console.log(
      `${options.bun} (${revision}): ${options.cases.join(", ")}; ${joinWords([...options.branches])} branches; scenarios of ${joinWords(options.scenarios.map(String))} calls; ${plural(options.processes, "process", "processes")} per mode in ${plural(options.repetitions, "repetition", "repetitions")}. Bun runs on CPU ${options.cpu}, and the driver ${driver.cpu === null ? `has no CPU of its own: ${driver.note}` : `on CPU ${driver.cpu}`}.${deltaAtExitFlag ? "" : " This Bun has no --jitcache-delta-at-exit, so the producers without delta kill themselves after their end line."}`,
    );
    const run = async (job: Job) => {
      const record = await runJob(context, job);
      results.write(record);
      console.log(progressLine(record));
    };
    for (let repetition = 1; repetition <= options.repetitions; ++repetition) {
      const started = new Date().toISOString();
      const idle = await waitForIdle(clockTicks, !options.check);
      const record: RepetitionRecord = { kind: "repetition", repetition, started, ...idle };
      results.write(record);
      console.log(`repetition ${repetition}: ${idle.idle ? "idle" : "noisy"}, load average ${idle.loadAverage[0].toFixed(2)}${idle.waitedSeconds ? ` after a wait of ${idle.waitedSeconds} s` : ""}`);
      for (const job of repetitionJobs(options, repetition)) await run(job);
    }
    for (const job of untimedJobs(options)) await run(job);
  } finally {
    await releaseLock();
  }
  const end: EndRecord = { kind: "end", finished: new Date().toISOString(), loadAverage: loadavg() };
  results.write(end);
  const { text, passed } = summarize(readResults([results.path]));
  const summaryPath = join(options.out, "summary.md");
  writeFileSync(summaryPath, text);
  console.log(`\n${text}\nresults: ${results.path}\nsummary: ${summaryPath}`);
  return passed ? 0 : 1;
}

if (import.meta.main) process.exit(await main(process.argv.slice(2)));
