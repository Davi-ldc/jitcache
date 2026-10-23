#!/usr/bin/env bun
// JITCache's macro benches (docs/JitCache/HARNESS.md, Benches): what each request costs a fresh Bun process serving a
// case of this directory with JITCache off and as a consumer, and how long the process takes to finish. Every
// Bun process runs pinned to one CPU with Bun's shipped configuration, so concurrent JIT stays on and its worker competes
// with execution, as on a small serverless instance.
//
// Each case runs in variants, the ways a function deploys: from source through run.ts, as a bundle Bun runs, and as a
// standalone executable without and with Bun's bytecode cache, the last with JITCache off only, since JITCache ignores
// code built with --bytecode and a consumer there would import none of the app. Before the first repetition the driver
// builds each case's bundle and executables with the Bun it measures, from an entry it writes that imports the case's
// app, its requests and calls.ts, the module that makes the calls in every variant. A repetition starts with fresh
// producers for each case, branch and variant JITCache runs in: one calls delta at exit and one never does, and a
// second delta capture (--delta-capture) adds one that calls delta under it. Then, for each scenario, processes with
// JITCache off and consumers of the first producer's artifact run interleaved across the variants, beside the modes the
// run asks for: consumers of the other producers, consumers that arm imported LLInt counters from zero, and cold
// processes, which start after the files their variant reads, and a consumer's artifact, were dropped from the page
// cache. After the last repetition, untimed runs write JITCache's bench report and log the compiles of each call. Bun
// names each process's app by its entry, run.ts, the bundle or the executable's embedded entry, so the producers and
// consumers of one case, branch and variant name one app. Every process must compute the same checksum at each
// call as every other process of its case and branch, whatever its variant. The records go to results.jsonl, and
// summary.md and request-curves.tsv are built from that file alone, so --summarize can rebuild them and pool several runs.

import { dlopen, FFIType } from "bun:ffi";
import { accessSync, appendFileSync, closeSync, constants, existsSync, fstatSync, mkdirSync, openSync, readdirSync, readFileSync, readSync, realpathSync, rmSync, statSync, writeFileSync } from "node:fs";
import { cpus, hostname, loadavg, release } from "node:os";
import { dirname, join, resolve } from "node:path";
import { holdMachineLock } from "../../Tools/Scripts/jitcache/lock.ts";
import { cases } from "./run.ts";

// The cases, run.ts, calls.ts and node_modules sit beside this file.
const benchDirectory = import.meta.dir;

const caseNames = ["hono", "elysia", "react-ssr", "preact-ssr", "webhook", "express", "postgres", "pdf"] as const;
type CaseName = (typeof caseNames)[number];
// pdf and postgres run only when --cases names them; postgres queries the server postgres.sh starts.
const defaultCases: CaseName[] = ["hono", "elysia", "react-ssr", "preact-ssr", "webhook", "express"];
const branchNames = ["fixed", "varied"] as const;
type Branch = (typeof branchNames)[number];
// The branches a case runs: postgres sends the one request its upstream bench sends, so it runs the fixed branch alone.
const branchesOf = (options: Options, name: CaseName): Branch[] => (name === "postgres" ? options.branches.filter(branch => branch === "fixed") : options.branches);
// How a process runs its case: `source` runs run.ts, which imports the app through Bun's runtime transpiler cache;
// `bundle` runs the app.js of `bun build --target=bun`; `compile` runs the standalone executable of `bun build --compile`;
// `compile-bytecode` runs the same built with --bytecode, which adds Bun's bytecode cache.
const variantNames = ["source", "bundle", "compile", "compile-bytecode"] as const;
type VariantName = (typeof variantNames)[number];
// JITCache ignores code built with --bytecode, so compile-bytecode runs with JITCache off only: it is the reference Bun's
// own bytecode cache gives, and it has no producer, consumer or bench report run.
const runsJITCache = (variant: VariantName) => variant !== "compile-bytecode";
// The variants a build makes, and among them the standalone executables, which take JITCache's flags from BUN_OPTIONS.
type BuiltVariant = Exclude<VariantName, "source">;
const isBuilt = (variant: VariantName): variant is BuiltVariant => variant !== "source";
const isCompiled = (variant: VariantName) => variant === "compile" || variant === "compile-bytecode";
// The variants a case runs in: pdf reads inputs/pdf relative to its own source file, so it runs only from source.
const variantsOf = (options: Options, name: CaseName): VariantName[] => (name === "pdf" ? options.variants.filter(variant => !isBuilt(variant)) : options.variants);

// The values of Bun's --jitcache-delta-capture: with `all`, delta also captures the bodies that ran without compiling.
const deltaCaptures = ["all", "compiled"] as const;
type DeltaCapture = (typeof deltaCaptures)[number];
type DeltaCaptureName = "All" | "Compiled";
const deltaCaptureName = (deltaCapture: DeltaCapture): DeltaCaptureName => (deltaCapture === "all" ? "All" : "Compiled");

// `producer` calls delta at exit, under the run's first delta capture when it names one, and `producerNoDelta` never
// does, so its artifact keeps only the captures made at each baseline compile; `producerAll` or `producerCompiled` calls
// delta under the run's second delta capture. `consumer` reads the first one's artifact, `consumerNoLLIntProgress` the
// same with the LLInt counters of imported bodies armed from zero, `consumerNoDelta` the second one's, and `consumerAll` or
// `consumerCompiled` the third one's. `offCold` and `consumerCold` start after the files their variant reads, and the
// consumer's artifact, were dropped from the page cache.
type Mode = "off" | "offCold" | "consumer" | "consumerCold" | "consumerNoDelta" | "consumerNoLLIntProgress" | `consumer${DeltaCaptureName}` | "producer" | "producerNoDelta" | `producer${DeltaCaptureName}`;
const comparedModes: Mode[] = ["off", "consumer", "consumerAll", "consumerCompiled", "consumerNoLLIntProgress", "consumerNoDelta", "offCold", "consumerCold"];
// What each compared mode is measured against: a cold consumer against a cold JITCache-off process, which read the same
// files from storage besides the artifact, and every other mode against off.
const referenceOf = (mode: Mode): Mode | null => (mode === "off" ? null : mode === "consumerCold" ? "offCold" : "off");
// Producers and timed processes are measured. Report runs write JITCache's bench report, whose clock reads only
// consumers would pay, and census runs log every compile they request, so neither is timed.
type Purpose = "producer" | "timed" | "report" | "census";

// HARNESS.md's target: a case at least 20% faster to finish than with JITCache off.
const targetRatio = 0.8;
// The headline metric (HARNESS.md, Benches): the median and p90 of request k for k = 1 to requestCurveCalls, of the sum
// of requests 2 to requestSumLast in each process, and of startup plus request 1. The summary shows the k of
// requestTableCalls and request-curves.tsv every k.
const requestCurveCalls = 50;
const requestSumLast = 35;
const requestTableCalls = [1, 2, 3, 5, 10, 20, 35, 50];
// The variants' comparison also sums requests 2 to longRequestSumLast, the whole of a 500-call process but its first call.
const longRequestSumLast = 500;
// The calls after which the summary gives the time to finish, the k of its Ck/W, and the last calls of its census
// windows.
const finishCalls = [1, 10, 50, 100, 500];
const curveCalls = [1, 2, 5, 10, 50, 100, 500];
const censusBounds = [1, 2, 5, 10, 50, 100, 500];
// W is the median of the producers' last ten calls, 91 to 100 when they serve 100.
const warmedCalls = 10;
// A process pinned to one CPU spends at most its wall time in CPU time; more means it also ran elsewhere.
const cpuTimeSlack = 1.05;
// A repetition waits up to idleWaitSeconds for an idle machine: a 1-minute load average below idleLoad and no other
// user's process above idleProcessPercent of a CPU.
const idleLoad = 0.5;
const idleProcessPercent = 10;
const idleWaitSeconds = 300;

const usage = `Usage: bun bench.ts --bun=<Bun executable> [options]
       bun bench.ts --summarize <results.jsonl>... [--out=<directory>]

  --cases=${defaultCases.join(",")}
                              pdf runs only when named, and in the source variant alone; postgres runs only when
                              named, and needs the server ./postgres.sh start runs
  --branches=fixed,varied     postgres runs the fixed branch alone
  --variants=${variantNames.join(",")}
                              how each process runs its case: source runs run.ts through Bun's transpiler cache,
                              bundle the app.js of bun build --target=bun, compile the executable of bun build
                              --compile, and compile-bytecode the same built with --bytecode, with JITCache off only;
                              each case's builds run once, before the first repetition
  --scenarios=50,500          the calls of each measured process; the longest scenario also runs consumers of the
                              producer that never calls delta
  --processes=10              processes per mode, scenario, case, branch and variant in each repetition; with 0, only
                              the producers and the untimed runs
  --repetitions=3             each with fresh producers
  --producer-calls=100
  --delta-capture=all|compiled
                              the delta capture every producer passes to Bun's --jitcache-delta-capture, by default
                              none, so Bun's own default; a second value after a comma adds producers that call delta
                              under it and their consumers, consumerAll or consumerCompiled
  --compare-llint-progress    add consumers whose imported bodies arm their LLInt counters from zero
                              (BUN_JSC_jitCacheImportLLIntProgress=0), consumerNoLLIntProgress
  --cold-processes=3          JITCache-off processes and consumers each, per scenario, case, branch and variant in
                              each repetition, that start after the files their variant reads and a consumer's
                              artifact are dropped from the page cache: for source the Bun executable, this directory
                              with its node_modules and the transpiler cache, for bundle the Bun executable and
                              app.js, and for the compiled variants the executable
  --cpu=<n>                   the CPU every Bun process is pinned to, by default the highest this process may use
  --census, --no-census       count the baseline compiles each call starts and the DFG and FTL compiles it requests,
                              in an untimed off process and each warm consumer mode, in each variant
  --reports, --no-reports     run untimed consumers per scenario with JITCache's bench report: warm, cold and the
                              second delta capture's
  --out=<directory>           jitcache-macro/<time> beside the Bun executable by default; with a compiled variant it
                              may hold no space, since BUN_OPTIONS, which splits at spaces, carries the paths of its
                              artifacts and reports
  --timeout=<seconds>         1800 by default; a process that runs longer is killed and fails
  --check                     correctness only: 1 repetition, 1 process per mode and variant, scenarios of 5 and 35
                              calls, producers of 20 calls and no wait for an idle machine; its times measure nothing
  --summarize                 rebuild summary.md and request-curves.tsv from results files, pooling them; they go
                              beside the only file, or to --out, which several files require`;

class UsageError extends Error {}

// ---------------------------------------------------------------------------------------------------------------------
// Command line

interface Options {
  bun: string;
  cases: CaseName[];
  branches: Branch[];
  variants: VariantName[];
  scenarios: number[]; // the calls of a measured process, ascending
  processes: number; // per mode, scenario, case, branch and variant in a repetition
  repetitions: number;
  producerCalls: number;
  deltaCapture: DeltaCapture[]; // the producers' delta captures, first the main one; empty leaves Bun's default
  compareLLIntProgress: boolean; // whether consumerNoLLIntProgress runs
  coldProcesses: number; // of each cold mode, per scenario, case, branch and variant in a repetition
  cpu: number; // the CPU every Bun process is pinned to
  census: boolean;
  reports: boolean;
  out: string;
  timeoutSeconds: number;
  check: boolean; // correctness only: times that measure nothing
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
  const variants = list("--variants", variantNames, oneOf("--variants", variantNames));
  // A check's longer scenario reaches request 35, so its summary computes the sum of requests 2 to 35.
  const scenarios = list("--scenarios", check ? [5, requestSumLast] : [50, 500], text => integer(text, "--scenarios", 0)).sort((a, b) => a - b);
  const processes = number("--processes", check ? 1 : 10, 0);
  const repetitions = number("--repetitions", check ? 1 : 3, 1);
  const producerCalls = number("--producer-calls", check ? 20 : 100, 1);
  const deltaCapture = list("--delta-capture", [], oneOf("--delta-capture", deltaCaptures));
  const compareLLIntProgress = takeFlag("--compare-llint-progress");
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
  // A compiled variant's JITCache flags travel in BUN_OPTIONS, which Bun splits at whitespace, and each of their paths
  // lies under the run's directory.
  if (variants.some(isCompiled) && /\s/.test(out)) throw new UsageError(`the run's directory ${out} holds whitespace, which BUN_OPTIONS would split the compiled variants' JITCache paths at; choose another with --out`);
  return { kind: "run", options: { bun, cases, branches, variants, scenarios, processes, repetitions, producerCalls, deltaCapture, compareLLIntProgress, coldProcesses, cpu, census, reports, out, timeoutSeconds, check } };
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

// Every Bun process and every build gets these variables and no other, so no JSC_ or BUN_JSC_ option and no Bun setting
// reaches it from the driver's environment. NODE_ENV is production, as in a deployment, and calls.ts requires it.
// BUN_DEBUG_QUIET_LOGS=1 keeps a debug Bun's scoped logs out of standard output, which carries the events calls.ts
// writes; a release Bun ignores it. The run's own transpiler cache, which Bun fills with the transpiled sources of 4 KiB
// or more, holds only what the bench's processes read, so a cold process can drop all of it and nothing another run
// left behind serves it.
function processEnvironment(transpilerCache: string): Record<string, string> {
  const environment: Record<string, string> = {};
  for (const name of ["PATH", "HOME", "TMPDIR", "LANG"]) {
    const value = process.env[name];
    if (value !== undefined) environment[name] = value;
  }
  environment.NODE_ENV = "production";
  environment.BUN_DEBUG_QUIET_LOGS = "1";
  environment.BUN_RUNTIME_TRANSPILER_CACHE_PATH = transpilerCache;
  return environment;
}

// ---------------------------------------------------------------------------------------------------------------------
// The bench report

// What the summary reads from one process's report. Each field is read only when it is a number, and unknown events and
// fields are only counted, so a field the report gains or loses changes no other number.
interface ReportCounts {
  events: Record<string, number>; // every event name, with its count
  malformedLines: number;
  start: { outcome: unknown; role: unknown; nanoseconds: number | null; indexedBodies: number | null } | null;
  installs: number;
  installNanoseconds: number; // the install events' totals: wall time capped at the thread's CPU time
  requests: Record<string, number>; // by action: import, attach
  requestNanoseconds: number; // the request events' counted and apart parts: thread CPU time
  // The module-analysis events by outcome (hit, miss, absent, off, invalid): their count, their nanoseconds, the thread
  // CPU time from the key's lookup to the import's return, and their bytes, the analysis sections read. Results files
  // written before the event existed have none.
  moduleAnalyses?: Record<string, { count: number; nanoseconds: number; bytes: number }>;
  // The last lookup event's counts by result; bodyVersion.absent counts the requests for bodies the artifact lacks, the
  // misses.
  lookups: Record<string, number>;
  compiles: number; // native baseline compiles
  compileNanoseconds: number; // their compilation and finalization: wall time capped at the thread's CPU time
  captures: Record<string, number>; // by "<trigger> <outcome>"
  committedCaptures: number;
  capturedBytes: number; // the sections the committed captures wrote
  deltas: number;
  deltaCommittedBodies: number;
  deltaCommittedBytes: number;
  deltaNanoseconds: number; // its three phases: thread CPU time
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
    moduleAnalyses: {},
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
      case "module-analysis": {
        const outcome = (counts.moduleAnalyses![String(event.outcome)] ??= { count: 0, nanoseconds: 0, bytes: 0 });
        outcome.count++;
        outcome.nanoseconds += numberOr(event.nanoseconds, 0);
        outcome.bytes += numberOr(event.bytes, 0);
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
        counts.deltaNanoseconds += numberOr(event.phase1Nanoseconds, 0) + numberOr(event.phase2Nanoseconds, 0) + numberOr(event.phase3Nanoseconds, 0);
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
  variant: VariantName;
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

// What a cold process's drop did to one group of the files it reads.
interface PageCacheGroup {
  name: string;
  files: number;
  bytes: number;
  residentBytes: number | null; // what mincore found cached right after the drop, or null when the drop failed
}

interface ProcessRecord extends Job {
  kind: "process";
  run?: number; // the results file a pooled summary read it from
  label: string;
  command: string[];
  environment: Record<string, string>; // what the process got beyond the run's environment
  exitCode: number | null;
  signal: string | null;
  timedOut: boolean;
  wallMilliseconds: number | null; // from the spawn to the exit
  startupMilliseconds: number | null; // the ready line's Bun.nanoseconds()
  endMilliseconds: number | null; // the end line's Bun.nanoseconds(); wall time minus it is the exit time
  callMilliseconds: number[];
  variants: number[]; // the request variant each call sent
  checksums: string[]; // the first 16 hex digits of each response's SHA-256, plenty to tell responses apart
  cpuMicroseconds: { user: number; system: number } | null; // every thread's
  contextSwitches: { voluntary: number; involuntary: number } | null;
  inputBlocks: number | null; // getrusage's ru_inblock: the 512-byte blocks read from storage, where the kernel counts them
  end: EndLine | null;
  pinnedCpu: { busyMilliseconds: number; stealMilliseconds: number; idleMilliseconds: number } | null; // from /proc/stat
  pageCache: PageCacheGroup[] | null; // a cold process's drop
  committedBodies: number | null; // the keys a producer's pack lists
  packBytes: number | null; // the producer's pack's valid end, or null without a pack
  report: ReportCounts | null; // a report run's
  // A census run's compiles by tier, as readCensus counts them, at 0 before call 1 and at k during call k.
  compiles: { baseline: number[]; dfg: number[]; ftl: number[] } | null;
  strayLines: number; // standard output lines that are no event of calls.ts
  problems: string[];
}

interface Context {
  options: Options;
  environment: Record<string, string>;
  transpilerCache: string; // every Bun process's BUN_RUNTIME_TRANSPILER_CACHE_PATH
  pageCache: PageCache | null; // when the run has cold processes
  driverRunsBun: boolean; // whether the Bun is the executable that runs this driver, whose pages cannot be dropped
  clockTicks: number; // per second, /proc/stat's unit
  processDirectory: string;
}

function labelOf(job: Job): string {
  const where = `${job.case} ${job.branch} ${job.variant}`;
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

const isCold = (mode: Mode) => mode === "offCold" || mode === "consumerCold";
// The second delta capture's producer and consumer, when the run names one.
const secondDeltaCaptureOf = (options: Options): DeltaCapture | null => options.deltaCapture[1] ?? null;

// The artifact a producer writes and its consumers read, one per producer, repetition, case, branch and variant, so the
// bodies of different builds never share one, and the delta capture that producer passes, or null for Bun's default.
function artifactOf(options: Options, job: Job): { path: string; deltaCapture: DeltaCapture | null } | null {
  let producer: string;
  let deltaCapture = options.deltaCapture[0] ?? null;
  switch (job.mode) {
    case "off":
    case "offCold":
      return null;
    case "producerNoDelta":
    case "consumerNoDelta":
      producer = "no-delta";
      break;
    case "producerAll":
    case "consumerAll":
    case "producerCompiled":
    case "consumerCompiled":
      deltaCapture = secondDeltaCaptureOf(options)!;
      producer = `delta-${deltaCapture}`;
      break;
    default:
      producer = "delta";
  }
  return { path: join(options.out, "artifacts", `r${job.repetition}-${job.case}-${job.branch}-${job.variant}-${producer}`), deltaCapture };
}

// The JSC options a mode sets, beyond the run's environment.
function modeEnvironment(job: Job): Record<string, string> {
  const environment: Record<string, string> = {};
  if (job.mode === "consumerNoLLIntProgress") environment.BUN_JSC_jitCacheImportLLIntProgress = "0";
  // JSC then logs the compiles readCensus counts.
  if (job.purpose === "census") environment.BUN_JSC_logCompilationChanges = "true";
  return environment;
}

// A process's command line and what it gets beyond the run's environment. JITCache's flags go where its variant's Bun
// reads its own: before the script for source and bundle, and in BUN_OPTIONS, space-separated, for a compiled
// executable, whose arguments all belong to its app. A consumer takes the default mode.
function invocationOf(context: Context, job: Job, artifact: { path: string; deltaCapture: DeltaCapture | null } | null, reportPath: string | null): { command: string[]; environment: Record<string, string> } {
  const { options } = context;
  const flags = artifact === null ? [] : [`--jitcache=${artifact.path}`];
  if (job.purpose === "producer") {
    flags.push("--jitcache-mode=p");
    if (artifact!.deltaCapture !== null) flags.push(`--jitcache-delta-capture=${artifact!.deltaCapture}`);
    // Bun calls delta at a producer's exit by default; every producer names the flag anyway, and the no-delta producer
    // turns it off.
    flags.push(job.mode === "producerNoDelta" ? "--jitcache-delta-at-exit=0" : "--jitcache-delta-at-exit");
  }
  if (reportPath !== null) flags.push(`--jitcache-bench-report=${reportPath}`);
  const plan = [`--calls=${job.calls}`, `--branch=${job.branch}`];
  if (job.purpose === "census") plan.push("--mark-calls");
  const environment = modeEnvironment(job);
  const pinned = ["taskset", "--cpu-list", String(options.cpu)];
  if (!isBuilt(job.variant)) return { command: [...pinned, options.bun, ...flags, "run.ts", job.case, ...plan], environment };
  const app = builtAppOf(options, job.case, job.variant);
  if (!isCompiled(job.variant)) return { command: [...pinned, options.bun, ...flags, app, ...plan], environment };
  if (flags.length) environment.BUN_OPTIONS = flags.join(" ");
  return { command: [...pinned, app, ...plan], environment };
}

async function runJob(context: Context, job: Job): Promise<ProcessRecord> {
  const { options } = context;
  const label = labelOf(job);
  const base = join(context.processDirectory, label.replace(/ #?/g, "-"));
  const files = { stdout: `${base}.stdout`, stderr: `${base}.stderr` };
  const reportPath = job.purpose === "report" ? `${base}.bench.jsonl` : null;
  const artifact = artifactOf(options, job);
  const { command, environment } = invocationOf(context, job, artifact, reportPath);
  const record: ProcessRecord = {
    kind: "process",
    ...job,
    label,
    command,
    environment,
    exitCode: null,
    signal: null,
    timedOut: false,
    wallMilliseconds: null,
    startupMilliseconds: null,
    endMilliseconds: null,
    callMilliseconds: [],
    variants: [],
    checksums: [],
    cpuMicroseconds: null,
    contextSwitches: null,
    inputBlocks: null,
    end: null,
    pinnedCpu: null,
    pageCache: null,
    committedBodies: null,
    packBytes: null,
    report: null,
    compiles: null,
    strayLines: 0,
    problems: [],
  };
  const dropped = isCold(job.mode) ? await dropPageCache(context, job, artifact?.path ?? null, record) : [];

  const stdout = openSync(files.stdout, "w");
  const stderr = openSync(files.stderr, "w");
  const before = cpuCounters(options.cpu);
  try {
    const started = performance.now();
    const child = Bun.spawn(command, { cwd: benchDirectory, env: { ...context.environment, ...environment }, stdin: "ignore", stdout, stderr });
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
      record.inputBlocks = Number(usage.ops.in);
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
  // The files a cold process dropped go back into the page cache, so the processes after it start as warm as before: a
  // cold hono process would otherwise leave React's node_modules on storage for the next react-ssr process.
  for (const file of dropped) context.pageCache!.warm(file);

  const stderrText = readText(files.stderr) ?? "";
  readEvents(readText(files.stdout) ?? "", job, record);
  if (record.wallMilliseconds !== null) checkExit(record, options.timeoutSeconds, stderrText);
  checkPinning(record, options.cpu);
  // Bun warns once on standard error when JITCache turns itself off or stops writing, and the process then measured
  // native runs.
  if (artifact !== null) {
    for (const line of stderrText.split("\n")) if (line.startsWith("warn: JITCache")) record.problems.push(`warned: ${line}`);
  }
  if (job.purpose === "producer") {
    // sync writes the bodies back, so no write-back runs during a measured process.
    Bun.spawnSync(["sync"]);
    countBodies(artifact!.path, record);
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

// The lines calls.ts writes: a ready line, the lines of calls 1 to N in order, then an end line. Other lines are counted
// and left alone, since a debug Bun can write its own.
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
      const end = numberOr(event.endNanoseconds, null);
      if (end === null) record.problems.push("printed an end line without endNanoseconds");
      else record.endMilliseconds = Math.round(end / 100) / 1e4;
    }
  }
  if (!ready) record.problems.push("printed no ready line");
  if (!broken && record.callMilliseconds.length !== job.calls) record.problems.push(`printed ${record.callMilliseconds.length} call lines of ${job.calls}`);
  if (!ended) record.problems.push("printed no end line");
}

function checkExit(record: ProcessRecord, timeoutSeconds: number, stderrText: string) {
  const ending = record.signal ? `ended by ${record.signal}` : `exited with ${record.exitCode}`;
  let failed = true;
  if (record.timedOut) record.problems.push(`was killed after ${timeoutSeconds} seconds`);
  else if (record.signal !== null || record.exitCode !== 0) record.problems.push(ending);
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

// libc through Bun's FFI, since fincore and vmtouch are missing on some machines. posix_fadvise with POSIX_FADV_DONTNEED
// evicts a file's clean pages, and mincore over a read-only mapping of the file tells which of its pages are cached
// without reading any. The kernel keeps every page some process maps.
class PageCache {
  private readonly libc = dlopen("libc.so.6", {
    posix_fadvise: { args: [FFIType.i32, FFIType.i64, FFIType.i64, FFIType.i32], returns: FFIType.i32 },
    mmap: { args: [FFIType.ptr, FFIType.u64, FFIType.i32, FFIType.i32, FFIType.i32, FFIType.i64], returns: FFIType.ptr },
    munmap: { args: [FFIType.ptr, FFIType.u64], returns: FFIType.i32 },
    mincore: { args: [FFIType.ptr, FFIType.u64, FFIType.ptr], returns: FFIType.i32 },
    getpagesize: { args: [], returns: FFIType.i32 },
  }).symbols;
  readonly pageSize = this.libc.getpagesize();
  private readonly buffer = new Uint8Array(1 << 20);

  drop(path: string) {
    const fd = openSync(path, "r");
    try {
      const error = this.libc.posix_fadvise(fd, 0, 0, 4); // POSIX_FADV_DONTNEED, over the whole file
      if (error) throw new Error(`posix_fadvise failed on ${path} with error ${error}`);
    } finally {
      closeSync(fd);
    }
  }

  // The file's size, and the bytes of it the page cache holds.
  probe(path: string): { bytes: number; residentBytes: number } {
    const fd = openSync(path, "r");
    try {
      const bytes = fstatSync(fd).size;
      if (!bytes) return { bytes, residentBytes: 0 };
      const address = this.libc.mmap(null, bytes, 1, 1, fd, 0); // PROT_READ, MAP_SHARED
      // MAP_FAILED is (void*)-1, far above any user-space address.
      if (address === null || address >= 2 ** 53) throw new Error(`mmap failed on ${path}`);
      try {
        const pages = new Uint8Array(Math.ceil(bytes / this.pageSize));
        if (this.libc.mincore(address, bytes, pages)) throw new Error(`mincore failed on ${path}`);
        let residentBytes = 0;
        pages.forEach((page, index) => {
          if (page & 1) residentBytes += Math.min(this.pageSize, bytes - index * this.pageSize);
        });
        return { bytes, residentBytes };
      } finally {
        this.libc.munmap(address, bytes);
      }
    } finally {
      closeSync(fd);
    }
  }

  // Reads the whole file, which brings its pages back into the page cache.
  warm(path: string) {
    const fd = openSync(path, "r");
    try {
      while (readSync(fd, this.buffer) > 0) {}
    } finally {
      closeSync(fd);
    }
  }
}

// HARNESS.md Benches: a cold process starts after the files its variant reads from storage left the page cache, and a
// consumer's artifact as well, so cold consumers and cold JITCache-off processes pay the same reads but the artifact's.
// source reads the Bun executable, this directory with its node_modules and the transpiler cache; bundle the Bun
// executable and app.js; a compiled variant its executable alone. sync first writes back what a process left dirty, such
// as the transpiler cache's new entries, since only clean pages can leave, and mincore then checks that every page left.
// A process the driver just reaped can still map its files for a few milliseconds after its exit was reported (a
// JITCache consumer up to about 20 ms on WSL's kernel), so a group that keeps pages is dropped again until none stays,
// for up to dropPatienceMilliseconds. The Bun that runs this driver stays mapped for good, so a run that measures that
// Bun drops everything else. Returns the dropped files.
const dropPatienceMilliseconds = 1000;

async function dropPageCache(context: Context, job: Job, artifact: string | null, record: ProcessRecord): Promise<string[]> {
  const pageCache = context.pageCache!;
  const bunExecutable = { name: "Bun executable", files: () => (context.driverRunsBun ? [] : [realpathSync(context.options.bun)]) };
  const variant = job.variant;
  const groups: { name: string; files: () => string[] }[] = [];
  if (!isBuilt(variant)) {
    groups.push(
      bunExecutable,
      { name: "bench directory", files: () => listFiles(benchDirectory) },
      { name: "transpiler cache", files: () => (existsSync(context.transpilerCache) ? listFiles(context.transpilerCache) : []) },
    );
  } else {
    if (!isCompiled(variant)) groups.push(bunExecutable);
    groups.push({ name: "built app", files: () => [builtAppOf(context.options, job.case, variant)] });
  }
  if (artifact !== null) groups.push({ name: "artifact", files: () => listFiles(artifact) });
  Bun.spawnSync(["sync"]);
  // Drops the files and returns the bytes the page cache still holds.
  const drop = (files: string[]) => {
    for (const file of files) pageCache.drop(file);
    return sum(files.map(file => pageCache.probe(file).residentBytes));
  };
  const deadline = performance.now() + dropPatienceMilliseconds;
  const dropped: string[] = [];
  record.pageCache = [];
  for (const group of groups) {
    const result: PageCacheGroup = { name: group.name, files: 0, bytes: 0, residentBytes: null };
    record.pageCache.push(result);
    try {
      const files = group.files();
      result.files = files.length;
      result.bytes = sum(files.map(file => statSync(file).size));
      dropped.push(...files);
      let residentBytes = drop(files);
      while (residentBytes && performance.now() < deadline) {
        await Bun.sleep(5);
        residentBytes = drop(files);
      }
      result.residentBytes = residentBytes;
      if (residentBytes) record.problems.push(`kept ${residentBytes} of the ${result.bytes} bytes of the ${group.name} in the page cache ${dropPatienceMilliseconds} ms after the drop`);
    } catch (error) {
      record.problems.push(`could not drop the ${group.name} from the page cache: ${(error as Error).message}`);
    }
  }
  return dropped;
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

// A producer's bodies: the keys cache/pack lists, and the pack's valid end.
function countBodies(artifact: string, record: ProcessRecord) {
  const pack = readPackKeys(join(artifact, "cache", "pack"), record);
  record.packBytes = pack?.validEnd ?? null;
  record.committedBodies = pack?.keys.size ?? 0;
}

// The keys a pack's index lists and its valid end; null without a pack. The pack starts with JITCPACK and its layout
// version, 2, as 2 bytes at 8; its root at 56 is the valid end, 8 bytes little-endian, and the 8 bytes before the valid
// end, the latest index block's trailer, give that block's offset. An index block (JITCINDX) holds its previous block's
// offset at 8, its kind at 16 (0 for the full index the chain ends at, 1 for a delta), its entry count at 20, and 80-byte
// entries from 32, each starting with a 40-byte key. The engine checks the rest, the CRCs among it.
function readPackKeys(path: string, record: ProcessRecord): { keys: Set<string>; validEnd: number } | null {
  let bytes: Buffer;
  try {
    bytes = readFileSync(path);
  } catch {
    return null;
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.byteLength < 64 || bytes.toString("latin1", 0, 8) !== "JITCPACK" || view.getUint16(8, true) !== 2) {
    record.problems.push(`left a pack, ${path}, whose prefix is not one of layout 2`);
    return null;
  }
  const u64 = (offset: number) => Number(view.getBigUint64(offset, true));
  const validEnd = u64(56);
  if (validEnd < 64 + 40 || validEnd > bytes.byteLength) {
    record.problems.push(`left a pack, ${path}, whose valid end ${validEnd} lies outside its ${bytes.byteLength} bytes`);
    return null;
  }
  const chain: number[] = [];
  for (let at = u64(validEnd - 8); ; at = u64(at + 8)) {
    if (at + 32 > bytes.byteLength || bytes.toString("latin1", at, at + 8) !== "JITCINDX") {
      record.problems.push(`left a pack, ${path}, without an index block at ${at}`);
      return null;
    }
    chain.push(at);
    if (view.getUint32(at + 16, true) === 0) break;
  }
  const keys = new Set<string>();
  for (const at of chain.reverse()) {
    if (view.getUint32(at + 16, true) === 0) keys.clear();
    for (let index = 0; index < view.getUint32(at + 20, true); ++index) {
      const entry = at + 32 + 80 * index;
      keys.add(bytes.toString("hex", entry, entry + 40));
    }
  }
  return { keys, validEnd };
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

// With logCompilationChanges, JSC logs `DFG(Driver) compiling <code block> with <mode>, instructions size = <n>` when it
// requests an optimizing compile, on the thread that runs the call, so the line falls between that call's markers. It
// logs no baseline request, so baseline compiles count where a JIT worker starts each one: `DFG(Plan) compiling <code
// block> with Baseline, instructions size = <n>`. A window thus holds the baseline compiles that ran in it, when the
// worker took the one CPU from the calls, and a plan still queued when the process exits never counts.
// (verboseCompilationQueue would log every request, but it can deadlock: JITWorklist::enqueue prints the queue under the
// worklist's lock, and a worker prints the worklist, which takes that lock, under the log's.) The log's lock keeps each JSC line whole against
// other JSC lines, but a marker can land inside a worker's line and end it; the worker's next bytes continue that line,
// since the call's thread cannot log until the worker releases the lock. The census therefore joins the text a marker
// cut with the line after it, and counts a joined line in the window where it started.
function readCensus(text: string, calls: number, record: ProcessRecord) {
  const compiles = { baseline: new Array<number>(calls + 1).fill(0), dfg: new Array<number>(calls + 1).fill(0), ftl: new Array<number>(calls + 1).fill(0) };
  let call = 0;
  let pending = ""; // a worker's line that markers cut, so far
  let pendingCall = 0; // the window where it started
  for (const line of text.split("\n")) {
    if (call < calls && line.endsWith(`call ${call + 1}`)) {
      const cut = line.slice(0, -`call ${call + 1}`.length);
      if (!pending && cut) pendingCall = call;
      pending += cut;
      ++call;
      continue;
    }
    const whole = pending + line;
    const window = pending ? pendingCall : call;
    pending = "";
    const request = /^DFG\(Driver\) compiling .* with (\w+), instructions size = \d+$/.exec(whole);
    if (request) {
      const mode = request[1];
      if (mode === "DFG" || mode === "UnlinkedDFG") compiles.dfg[window]++;
      else if (mode === "FTL" || mode === "FTLForOSREntry") compiles.ftl[window]++;
      else record.problems.push(`logged an optimizing compile in mode ${mode}, which the census does not count`);
    } else if (/^DFG\(Plan\) compiling .* with Baseline, instructions size = \d+$/.test(whole)) {
      compiles.baseline[window]++;
    }
  }
  if (call !== calls) record.problems.push(`marked ${call} of its ${calls} calls on standard error`);
  record.compiles = compiles;
}

// ---------------------------------------------------------------------------------------------------------------------
// Builds

// A case's builds live in <out>/build/<case>: the entry the driver writes, and a directory for each built variant that
// holds the built app.
const buildDirectoryOf = (options: Options, name: CaseName) => join(options.out, "build", name);
const builtAppOf = (options: Options, name: CaseName, variant: BuiltVariant) => join(buildDirectoryOf(options, name), variant, variant === "bundle" ? "app.js" : "app");

interface BuildRecord {
  kind: "build";
  run?: number; // the results file a pooled summary read it from
  case: CaseName;
  variant: BuiltVariant;
  command: string[];
  exitCode: number | null;
  signal: string | null;
  wallMilliseconds: number;
  outfile: string;
  outfileBytes: number | null; // the size of app.js or of the executable
  problems: string[];
}

// The entry imports the case's app and request variants statically, so the build holds them, and hands them to
// calls.ts.
function entrySource(name: CaseName): string {
  const { app, requests } = cases[name];
  const from = (path: string) => JSON.stringify(join(benchDirectory, path));
  return [
    `// ${name} as bench.ts builds it.`,
    `import app from ${from(app)};`,
    `import requests from ${from(requests)};`,
    `import { serveBuilt } from ${from("calls.ts")};`,
    "",
    `await serveBuilt(${JSON.stringify(name)}, app, requests);`,
    "",
  ].join("\n");
}

// What follows `bun build <entry>`. The bundler replaces process.env.NODE_ENV with the value in its own environment, so
// a build gets the processes' environment, whose NODE_ENV=production selects production code as it does at run time.
function buildArguments(variant: BuiltVariant, outfile: string): string[] {
  switch (variant) {
    case "bundle":
      return ["--target=bun", "--format=esm", `--outfile=${outfile}`];
    case "compile":
      return ["--compile", "--format=esm", `--outfile=${outfile}`];
    case "compile-bytecode":
      return ["--compile", "--format=esm", "--bytecode", `--outfile=${outfile}`];
  }
}

// Builds each case's variants with the --bun executable, untimed, before the first repetition, in this directory as a
// source process runs. Every build is recorded, and the first that fails ends the builds after its output is printed.
// Returns whether every build succeeded.
function buildApps(context: Context, results: ResultsFile): boolean {
  const { options } = context;
  for (const name of options.cases) {
    const variants = variantsOf(options, name).filter(isBuilt);
    if (!variants.length) continue;
    const entry = join(buildDirectoryOf(options, name), "entry.ts");
    mkdirSync(dirname(entry), { recursive: true });
    writeFileSync(entry, entrySource(name));
    for (const variant of variants) {
      const outfile = builtAppOf(options, name, variant);
      mkdirSync(dirname(outfile), { recursive: true });
      const command = [options.bun, "build", entry, ...buildArguments(variant, outfile)];
      const started = performance.now();
      const built = Bun.spawnSync(command, { cwd: benchDirectory, env: context.environment, stdin: "ignore", stdout: "pipe", stderr: "pipe" });
      const record: BuildRecord = {
        kind: "build",
        case: name,
        variant,
        command,
        exitCode: built.exitCode,
        signal: built.signalCode ?? null,
        wallMilliseconds: Math.round((performance.now() - started) * 1e4) / 1e4,
        outfile,
        outfileBytes: statSync(outfile, { throwIfNoEntry: false })?.size ?? null,
        problems: [],
      };
      const output = `${built.stdout.toString()}${built.stderr.toString()}`.trimEnd();
      if (record.signal !== null || record.exitCode !== 0) record.problems.push(record.signal !== null ? `ended by ${record.signal}` : `exited with ${record.exitCode}`);
      else if (record.outfileBytes === null) record.problems.push(`wrote no ${outfile}`);
      if (record.problems.length && output) record.problems.push(`its output ends with: ${tail(output)}`);
      results.write(record);
      const label = `build ${name} ${variant}`;
      if (record.problems.length) {
        console.log(`${label}: FAILED: ${record.problems[0]}${output ? `; its output:\n${output}` : ""}`);
        return false;
      }
      console.log(`${label}: ${mebibytes(record.outfileBytes!)} MiB in ${milliseconds(record.wallMilliseconds)}`);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The plan

// The warm consumer modes the run compares besides `consumer`: the second delta capture's and the one whose imported
// LLInt counters start from zero.
function consumerVariants(options: Options): Mode[] {
  const second = secondDeltaCaptureOf(options);
  const modes: Mode[] = [];
  if (second !== null) modes.push(`consumer${deltaCaptureName(second)}`);
  if (options.compareLLIntProgress) modes.push("consumerNoLLIntProgress");
  return modes;
}

// One repetition's jobs in the order they run: the producers, then, for each process index, the modes of every case,
// branch and scenario, each variant's beside the others', in an order that rotates with the index, so no mode always
// runs right after the same one. Cold pair c, an off process and a consumer counted from 0, runs at index
// floor(c * processes / coldProcesses) + 1, which spreads the pairs over the indexes. A variant JITCache does not run in
// has no producer, and only its off processes, warm and cold, run.
function repetitionJobs(options: Options, repetition: number): Job[] {
  const jobs: Job[] = [];
  const longest = options.scenarios.at(-1)!;
  const second = secondDeltaCaptureOf(options);
  const producers: Mode[] = ["producer", "producerNoDelta", ...(second === null ? [] : [`producer${deltaCaptureName(second)}` as const])];
  for (const name of options.cases) {
    for (const branch of branchesOf(options, name)) {
      for (const variant of variantsOf(options, name).filter(runsJITCache)) {
        for (const mode of producers) jobs.push({ purpose: "producer", case: name, branch, variant, scenario: null, calls: options.producerCalls, mode, repetition, index: 1 });
      }
    }
  }
  for (let index = 1; index <= options.processes; ++index) {
    const colds = [...Array(options.coldProcesses).keys()].filter(cold => Math.floor((cold * options.processes) / options.coldProcesses) + 1 === index);
    for (const name of options.cases) {
      for (const branch of branchesOf(options, name)) {
        for (const scenario of options.scenarios) {
          const group: Job[] = [];
          for (const variant of variantsOf(options, name)) {
            const job = (mode: Mode, modeIndex: number): Job => ({ purpose: "timed", case: name, branch, variant, scenario, calls: scenario, mode, repetition, index: modeIndex });
            if (!runsJITCache(variant)) {
              group.push(job("off", index), ...colds.map(cold => job("offCold", cold + 1)));
              continue;
            }
            group.push(job("off", index), job("consumer", index), ...consumerVariants(options).map(mode => job(mode, index)));
            if (scenario === longest) group.push(job("consumerNoDelta", index));
            for (const cold of colds) group.push(job("offCold", cold + 1), job("consumerCold", cold + 1));
          }
          const shift = (index - 1) % group.length;
          jobs.push(...group.slice(shift), ...group.slice(0, shift));
        }
      }
    }
  }
  return jobs;
}

// After the last repetition, on its artifacts: for each case, branch and variant, the consumers of every scenario that
// write the bench report, warm, cold and of the second delta capture, and the census's off process and warm consumers,
// which make the longest scenario's calls; a variant JITCache does not run in gets only the census's off process.
function untimedJobs(options: Options): Job[] {
  const jobs: Job[] = [];
  const repetition = options.repetitions;
  const longest = options.scenarios.at(-1)!;
  const second = secondDeltaCaptureOf(options);
  for (const name of options.cases) {
    for (const branch of branchesOf(options, name)) {
      for (const variant of variantsOf(options, name)) {
        if (options.reports && runsJITCache(variant)) {
          const modes: Mode[] = ["consumer", "consumerCold", ...(second === null ? [] : [`consumer${deltaCaptureName(second)}` as const])];
          for (const scenario of options.scenarios) {
            for (const mode of modes) jobs.push({ purpose: "report", case: name, branch, variant, scenario, calls: scenario, mode, repetition, index: 1 });
          }
        }
        if (options.census) {
          const modes: Mode[] = runsJITCache(variant) ? ["off", "consumer", ...consumerVariants(options)] : ["off"];
          for (const mode of modes) jobs.push({ purpose: "census", case: name, branch, variant, scenario: null, calls: longest, mode, repetition, index: 1 });
        }
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
  parts.push(`wall ${milliseconds(record.wallMilliseconds)}`, `exit ${milliseconds(exitMillisecondsOf(record))}`, `CPU ${milliseconds(cpuMillisecondsOf(record))}`);
  if (record.committedBodies !== null) parts.push(`${plural(record.committedBodies, "body", "bodies")} committed${record.packBytes === null ? "" : `, a pack of ${record.packBytes} bytes`}`);
  if (record.report) parts.push(`${plural(record.report.installs, "install", "installs")}`);
  if (record.compiles) parts.push(`${sum(record.compiles.baseline)} baseline compiles started, ${sum(record.compiles.dfg)} DFG and ${sum(record.compiles.ftl)} FTL requested`);
  if (record.pageCache) parts.push(`${plural(sum(record.pageCache.map(group => group.files)), "file", "files")} dropped from the page cache`);
  if (isCold(record.mode) && record.inputBlocks !== null) parts.push(`${mebibytes(record.inputBlocks * 512)} MiB read from storage`);
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
  pinnedCpuSiblings: number[]; // the CPUs that share a core with options.cpu, itself included
  driverCpu: number | null;
  driverNote: string | null;
  driverRunsBun: boolean; // whether the Bun is the executable that runs the driver, whose pages no drop evicts
  pageSize: number | null; // the page cache's, when the run has cold processes
  taskIoAccounting: boolean; // whether the kernel counts each task's storage reads, which ru_inblock gives
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
// pooled runs apart. Files from before the variants have no variant in their records, and their processes ran run.ts,
// so they read as source.
interface Pool {
  runs: RunRecord[];
  repetitions: RepetitionRecord[];
  builds: BuildRecord[];
  processes: ProcessRecord[];
  ends: EndRecord[];
}

function readResults(paths: string[]): Pool {
  const pool: Pool = { runs: [], repetitions: [], builds: [], processes: [], ends: [] };
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
        if (record.kind === "run") {
          const runRecord = record as RunRecord;
          runRecord.options.variants ??= ["source"];
          pool.runs.push(runRecord);
        } else if (record.kind === "repetition") pool.repetitions.push(record as RepetitionRecord);
        else if (record.kind === "build") pool.builds.push(record as BuildRecord);
        else if (record.kind === "process") pool.processes.push({ variant: "source", ...record } as ProcessRecord);
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
// Wall time minus Bun's clock at the end line: the exit after that line, with the exec before Bun's clock starts, which
// every mode pays alike.
const exitMillisecondsOf: Measure = record => (record.wallMilliseconds !== null && typeof record.endMilliseconds === "number" ? record.wallMilliseconds - record.endMilliseconds : null);
const storageMebibytes: Measure = record => (typeof record.inputBlocks === "number" ? (record.inputBlocks * 512) / 1048576 : null);
// Startup plus the first k calls, as calls.ts times them.
const finishedAfter =
  (calls: number): Measure =>
  record =>
    record.startupMilliseconds === null || record.callMilliseconds.length < calls ? null : record.startupMilliseconds + sum(record.callMilliseconds.slice(0, calls));
// The headline metric's measures, as tmp/ops/next-round/scratch/request-curves.ts computed them for the 10-14 digest:
// request k is call k; the requests 2 to 35 of one process add up in call order; startup plus request 1 adds Bun's clock
// at the ready line to call 1, leaving out only the exec before that clock and the microseconds between the ready line
// and call 1.
const requestAt =
  (k: number): Measure =>
  record =>
    record.callMilliseconds[k - 1] ?? null;
const requestSum =
  (last: number): Measure =>
  record =>
    record.callMilliseconds.length < last ? null : sum(record.callMilliseconds.slice(1, last));
const laterRequests = requestSum(requestSumLast);
const firstRequestWithStartup: Measure = record => (record.startupMilliseconds === null || !record.callMilliseconds.length ? null : record.startupMilliseconds + record.callMilliseconds[0]);
// Orders repetitions as they ran, across pooled runs too.
const repetitionOrder = (record: { run?: number; repetition: number }) => (record.run ?? 0) * 1_000_000 + record.repetition;
const labelIn = (pool: Pool, record: ProcessRecord) => (pool.runs.length > 1 ? `run ${(record.run ?? 0) + 1} ${record.label}` : record.label);

const requestGuide = [
  `The headline metric (HARNESS.md, Benches). A cell gives the median/p90 in ms over a mode's processes, pooled over the repetitions, where p90 is the nearest rank, the ceil(0.9 n)-th value in ascending order. Column k is request k, call k of each process; sum 2 to ${requestSumLast} adds requests 2 to ${requestSumLast} in each process; startup + 1 adds Bun's clock at the ready line to request 1.`,
  `Against its reference, a mode's cell gives its median minus the reference's in ms and, in parentheses, its median over the reference's. consumerCold's reference is offCold, which reads the same files from storage but the artifact; every other mode's is off. Each variant has tables of its own, and a mode's reference ran the same variant. request-curves.tsv beside this file holds every request k up to ${requestCurveCalls}.`,
].join(" ");

const variantGuide = [
  "Each case and branch with JITCache off and as a consumer in every variant: source runs run.ts through Bun's transpiler cache, bundle the app.js of bun build --target=bun, compile the executable of bun build --compile, and compile-bytecode the same built with --bytecode, which runs with JITCache off only, since JITCache ignores code built with --bytecode.",
  `A cell gives the median in ms over a mode's processes in the shortest scenario that reaches its column, pooled over the repetitions, and in parentheses its ratio to compile-bytecode with JITCache off, which is Bun's bytecode cache alone. startup + 1 adds Bun's clock at the ready line to request 1, 3 is request 3, and 2 to k adds requests 2 to k in each process.`,
  "app is the size of app.js or of the executable, and artifact the median size of the packs the consumers read. When reports run, the untimed consumers of the longest scenario give the bodies whose baseline code each installed.",
].join(" ");

const readingGuide = [
  "The sections by case, branch and variant give the time to finish, secondary to the per-request latency, beside the census and JITCache's bench report. A cell over several processes gives their median, and wall and CPU time add the range in parentheses. CPU time adds the user and system time of all of a process's threads; peak memory is its VmHWM at its end line; exit is wall time minus Bun's clock at the end line, so it holds the exit after that line and the exec before Bun's clock starts; read from storage is getrusage's block input.",
  `A ratio divides a mode's median by the median of its reference's processes in the same table, pooled over the repetitions, and each repetition's own ratio follows in parentheses; consumerCold's reference is offCold, every other mode's off. Bold marks a ratio at or below ${targetRatio.toFixed(2)}, the target in HARNESS.md.`,
  "After k calls means startup plus the first k calls, as calls.ts times them. W is the producers' warmed call, the median of their calls 91 to 100 that sent the same variant. Ck is a mode's median time for call k (Nk with JITCache off), so Ck/W says how close call k comes to the warmed call.",
].join(" ");

function summarize(pool: Pool): { text: string; passed: boolean } {
  const problems = collectProblems(pool);
  const lines = ["# JITCache macro benches", ""];
  for (const run of pool.runs) lines.push(describeRun(run, pool), "");
  const revisions = [...new Set(pool.runs.map(run => run.revision))];
  if (revisions.length > 1) lines.push(`The pooled runs used different Bun builds (${revisions.join(", ")}), so their numbers do not compare.`, "");
  if (pool.runs.some(run => run.options.check)) lines.push("A check (--check) runs one short process per mode and variant without waiting for an idle machine, so its times measure nothing; only its checks count.", "");
  if (pool.repetitions.length) lines.push(...pool.repetitions.map(repetition => `- ${describeRepetition(repetition, pool)}`), "");
  lines.push("## Checks", "", ...describeProblems(problems), "", ...variantLines(pool), ...requestLines(pool), "## Reading the tables", "", readingGuide, "");
  for (const name of caseNames) {
    for (const branch of branchNames) {
      for (const variant of variantNames) {
        const records = pool.processes.filter(record => record.case === name && record.branch === branch && record.variant === variant);
        if (records.length) lines.push(...caseLines(pool, records, name, branch, variant));
      }
    }
  }
  return { text: lines.join("\n"), passed: !problems.length };
}

interface TimedGroup {
  case: CaseName;
  branch: Branch;
  variant: VariantName;
  scenario: number;
  mode: Mode;
  records: ProcessRecord[];
}

// The timed processes of each case, branch, variant, scenario and mode, in the order the tables give them.
function timedGroups(pool: Pool): TimedGroup[] {
  const timed = pool.processes.filter(record => record.purpose === "timed" && record.scenario !== null);
  const scenarios = [...new Set(timed.map(record => record.scenario!))].sort((a, b) => a - b);
  const groups: TimedGroup[] = [];
  for (const name of caseNames) {
    for (const branch of branchNames) {
      for (const variant of variantNames) {
        for (const scenario of scenarios) {
          for (const mode of comparedModes) {
            const records = timed.filter(record => record.case === name && record.branch === branch && record.variant === variant && record.scenario === scenario && record.mode === mode);
            if (records.length) groups.push({ case: name, branch, variant, scenario, mode, records });
          }
        }
      }
    }
  }
  return groups;
}

// The variants' comparison, for each case and branch: every variant with JITCache off and as a consumer at each measure
// below, read in the shortest scenario whose processes reach it, against compile-bytecode with JITCache off, beside
// the size of what the variant runs and, when reports run, what its consumers installed.
const variantColumns: { label: string; calls: number; measure: Measure }[] = [
  { label: "startup + 1", calls: 1, measure: firstRequestWithStartup },
  { label: "3", calls: 3, measure: requestAt(3) },
  { label: `2 to ${requestSumLast}`, calls: requestSumLast, measure: laterRequests },
  { label: `2 to ${longRequestSumLast}`, calls: longRequestSumLast, measure: requestSum(longRequestSumLast) },
];

function variantLines(pool: Pool): string[] {
  const groups = timedGroups(pool).filter(group => group.mode === "off" || group.mode === "consumer");
  if (!groups.length) return [];
  const lines = ["## Variants", "", variantGuide, ""];
  const reportCounts: [string, (counts: ReportCounts) => number][] = [["installed", counts => counts.installs]];
  for (const name of caseNames) {
    for (const branch of branchNames) {
      const mine = groups.filter(group => group.case === name && group.branch === branch);
      if (!mine.length) continue;
      const scenarios = [...new Set(mine.map(group => group.scenario))].sort((a, b) => a - b);
      const columns = variantColumns.flatMap(column => {
        const scenario = scenarios.find(candidate => candidate >= column.calls);
        return scenario === undefined ? [] : [{ ...column, scenario }];
      });
      const recordsOf = (variant: VariantName, mode: Mode, scenario: number) => mine.find(group => group.variant === variant && group.mode === mode && group.scenario === scenario)?.records ?? [];
      const reports = pool.processes.filter(record => record.purpose === "report" && record.mode === "consumer" && record.report && record.case === name && record.branch === branch);
      const reportScenario = Math.max(...reports.map(record => record.scenario ?? 0));
      const counts = reports.length ? reportCounts : [];
      const header = [...columns.map(column => column.label), "app, MiB", "artifact, MiB", ...counts.map(([label]) => label)];
      lines.push(`| ${name}, ${branch} | ${header.join(" | ")} |`, `|---|${header.map(() => "---|").join("")}`);
      for (const variant of variantNames) {
        for (const mode of ["off", "consumer"] as const) {
          if (!mine.some(group => group.variant === variant && group.mode === mode)) continue;
          const times = columns.map(({ measure, scenario }) => {
            const value = median(recordsOf(variant, mode, scenario).map(measure));
            const reference = variant === "compile-bytecode" && mode === "off" ? null : median(recordsOf("compile-bytecode", "off", scenario).map(measure));
            if (value === null) return "n/a";
            return reference === null || reference === 0 ? latency(value) : `${latency(value)} (${ratio(value / reference)})`;
          });
          const app = median(pool.builds.filter(build => build.case === name && build.variant === variant).map(build => build.outfileBytes));
          const packs = pool.processes.filter(record => record.purpose === "producer" && record.mode === "producer" && record.case === name && record.branch === branch && record.variant === variant);
          const artifact = median(packs.map(record => record.packBytes));
          const mineReported = reports.filter(record => record.variant === variant && record.scenario === reportScenario);
          const cells = [
            ...times,
            app === null ? "" : mebibytes(app),
            mode === "off" ? "" : artifact === null ? "n/a" : mebibytes(artifact),
            ...counts.map(([, read]) => (mode === "off" ? "" : spread(mineReported.map(record => read(record.report!))))),
          ];
          lines.push(`| ${variant} ${mode} | ${cells.join(" | ")} |`);
        }
      }
      lines.push("");
    }
  }
  return lines;
}

// The per-request measures of a scenario's processes, those its calls reach: request k for k up to `last`, the sum of
// requests 2 to 35, and startup plus request 1.
function requestMeasures(scenario: number, ks: number[]): { label: string; name: string; measure: Measure }[] {
  if (!scenario) return [];
  return [
    ...ks.filter(k => k <= scenario).map(k => ({ label: String(k), name: `call${k}`, measure: requestAt(k) })),
    ...(scenario >= requestSumLast ? [{ label: `sum 2 to ${requestSumLast}`, name: `sum_calls_2_${requestSumLast}`, measure: laterRequests }] : []),
    { label: "startup + 1", name: "startup_plus_call1", measure: firstRequestWithStartup },
  ];
}

// The headline section, variant by variant: each mode against its reference, then each case's per-request table by
// scenario, as the 10-14 digest gave them.
function requestLines(pool: Pool): string[] {
  const groups = timedGroups(pool);
  if (!groups.length) return [];
  const lines = ["## Per-request latency", "", requestGuide, ""];
  const find = (name: CaseName, branch: Branch, variant: VariantName, scenario: number, mode: Mode) =>
    groups.find(group => group.case === name && group.branch === branch && group.variant === variant && group.scenario === scenario && group.mode === mode)?.records ?? [];
  for (const mode of comparedModes) {
    const reference = referenceOf(mode);
    if (reference === null) continue;
    for (const variant of variantNames) {
      const mine = groups.filter(group => group.mode === mode && group.variant === variant);
      if (!mine.length) continue;
      const scenarios = [...new Set(mine.map(group => group.scenario))].sort((a, b) => a - b);
      const columns = scenarios.flatMap(scenario => requestMeasures(scenario, []).map(column => ({ ...column, scenario })));
      if (!columns.length) continue;
      lines.push(`| ${mode} against ${reference}, ${variant} | ${columns.map(column => `${column.label}, ${plural(column.scenario, "call", "calls")}`).join(" | ")} |`, `|---|${columns.map(() => "---|").join("")}`);
      for (const name of caseNames) {
        for (const branch of branchNames) {
          if (!mine.some(group => group.case === name && group.branch === branch)) continue;
          const cells = columns.map(({ scenario, measure }) => {
            const a = median(find(name, branch, variant, scenario, mode).map(measure));
            const b = median(find(name, branch, variant, scenario, reference).map(measure));
            return a === null || b === null || b === 0 ? "n/a" : `${a >= b ? "+" : "-"}${Math.abs(a - b).toFixed(2)} (${(a / b).toFixed(2)})`;
          });
          lines.push(`| ${name} ${branch} | ${cells.join(" | ")} |`);
        }
      }
      lines.push("");
    }
  }
  for (const name of caseNames) {
    for (const variant of variantNames) {
      const scenarios = [...new Set(groups.filter(group => group.case === name && group.variant === variant).map(group => group.scenario))].sort((a, b) => a - b);
      for (const scenario of scenarios) {
        const columns = requestMeasures(scenario, requestTableCalls);
        if (!columns.length) continue;
        lines.push(`| ${name}, ${variant}, ${plural(scenario, "call", "calls")} | ${columns.map(column => column.label).join(" | ")} |`, `|---|${columns.map(() => "---|").join("")}`);
        for (const group of groups.filter(group => group.case === name && group.variant === variant && group.scenario === scenario)) {
          const cells = columns.map(({ measure }) => {
            const values = group.records.map(measure);
            const middle = median(values);
            return middle === null ? "n/a" : `${latency(middle)}/${latency(p90(values)!)}`;
          });
          lines.push(`| ${group.branch} ${group.mode} | ${cells.join(" | ")} |`);
        }
        lines.push("");
      }
    }
  }
  return lines;
}

// request-curves.tsv: for each case, branch, variant, scenario and mode, each per-request measure up to request 50 and
// the time to finish's main measures, with the processes, the median and p90, and the median against the mode's
// reference. An empty cell has no value.
function requestCurves(pool: Pool): string {
  const groups = timedGroups(pool);
  const rows = [["case", "branch", "variant", "scenario", "mode", "measure", "n", "median_ms", "p90_ms", "reference", "reference_median_ms", "minus_reference_ms", "over_reference"]];
  const format = (value: number | null, digits: number) => (value === null ? "" : value.toFixed(digits));
  const ks = [...Array(requestCurveCalls).keys()].map(k => k + 1);
  for (const group of groups) {
    const reference = referenceOf(group.mode);
    const referenceRecords =
      reference === null ? [] : (groups.find(other => other.case === group.case && other.branch === group.branch && other.variant === group.variant && other.scenario === group.scenario && other.mode === reference)?.records ?? []);
    const measures = [
      ...requestMeasures(group.scenario, ks),
      { name: "startup", measure: (record: ProcessRecord) => record.startupMilliseconds },
      { name: "exit", measure: exitMillisecondsOf },
      { name: "wall", measure: (record: ProcessRecord) => record.wallMilliseconds },
      { name: "cpu", measure: cpuMillisecondsOf },
    ];
    for (const { name, measure } of measures) {
      const values = present(group.records.map(measure));
      const middle = median(values);
      const theirs = reference === null ? null : median(referenceRecords.map(measure));
      rows.push([
        group.case,
        group.branch,
        group.variant,
        String(group.scenario),
        group.mode,
        name,
        String(values.length),
        format(middle, 4),
        format(p90(values), 4),
        reference ?? "",
        format(theirs, 4),
        format(middle === null || theirs === null ? null : middle - theirs, 4),
        format(divide(middle, theirs), 3),
      ]);
    }
  }
  return `${rows.map(row => row.join("\t")).join("\n")}\n`;
}

function describeRun(run: RunRecord, pool: Pool): string {
  const { options, memory } = run;
  const gibibytes = (bytes: number | null) => (bytes === null ? "n/a" : `${(bytes / 2 ** 30).toFixed(1)} GiB`);
  const swapUsed = memory.swapTotalBytes === null || memory.swapFreeBytes === null ? null : memory.swapTotalBytes - memory.swapFreeBytes;
  const load = (average: number[]) => average.map(value => value.toFixed(2)).join(", ");
  const end = pool.ends.find(record => record.run === run.run);
  const siblings = run.pinnedCpuSiblings.filter(cpu => cpu !== options.cpu);
  const users = run.who ? run.who.split("\n").map(line => line.trim().split(/\s+/).slice(0, 2).join(" on ")).join("; ") : "nobody";
  const deltaCapture = options.deltaCapture ?? [];
  const fixedOnly = options.branches.length > 1 ? options.cases.filter(name => branchesOf(options, name).length === 1) : [];
  const unrun = options.cases.filter(name => branchesOf(options, name).length === 0);
  return [
    `${pool.runs.length > 1 ? `Run ${(run.run ?? 0) + 1}: ` : ""}Bun ${run.bun}, revision ${run.revision}.`,
    `Host ${run.host}, kernel ${run.kernel}, ${run.cpus} CPUs (${run.cpuModel ?? "model unknown"}), transparent huge pages ${run.transparentHugePages ?? "n/a"}, ${gibibytes(memory.totalBytes)} of memory (${gibibytes(memory.availableBytes)} available) and ${gibibytes(memory.swapTotalBytes)} of swap (${gibibytes(swapUsed)} in use); logged in: ${users}.`,
    `Every Bun process ran on CPU ${options.cpu}${siblings.length ? `, which shares its core with CPU ${joinWords(siblings.map(String))}` : ""}, and the driver ${run.driverCpu === null ? "had no CPU of its own" : `ran on CPU ${run.driverCpu}`}${run.driverNote ? ` (${run.driverNote})` : ""}.`,
    `${plural(options.repetitions, "repetition", "repetitions")} of ${plural(options.processes, "process", "processes")} per mode in the variants ${joinWords([...options.variants])}, scenarios of ${joinWords(options.scenarios.map(String))} calls, producers of ${options.producerCalls} calls and ${plural(options.coldProcesses, "cold pair", "cold pairs")}, a JITCache-off process and a consumer, per scenario, variant and repetition.`,
    options.cases.includes("pdf") && options.variants.some(isBuilt) ? "pdf reads inputs/pdf relative to its own source file, so it ran in the source variant alone." : "",
    fixedOnly.length ? `${joinWords(fixedOnly)} ran in the fixed branch alone, since ${fixedOnly.length > 1 ? "they send the one request their upstream benches send" : "it sends the one request its upstream bench sends"}.` : "",
    unrun.length ? `${joinWords(unrun)} did not run, since ${unrun.length > 1 ? "they run" : "it runs"} the fixed branch alone and the run had none.` : "",
    options.variants.some(variant => !runsJITCache(variant)) ? "compile-bytecode ran with JITCache off only, since JITCache ignores code built with --bytecode: its cold pairs were JITCache-off processes alone." : "",
    deltaCapture.length ? `The producers passed --jitcache-delta-capture=${deltaCapture[0]}${deltaCapture[1] ? `, and producer${deltaCaptureName(deltaCapture[1])} --jitcache-delta-capture=${deltaCapture[1]}` : ""}.` : "",
    options.compareLLIntProgress ? "consumerNoLLIntProgress ran with BUN_JSC_jitCacheImportLLIntProgress=0." : "",
    run.driverRunsBun ? "The driver runs this Bun itself, so its executable stays in the page cache when cold processes start." : "",
    run.taskIoAccounting === false ? "This kernel counts no task's storage reads, so read from storage stays at 0." : "",
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

// Each build's and each process's own problems, and each process whose response at some call differs from the first
// response any process of its case and branch got at that call, whatever its variant, since a consumer must compute
// what a JITCache-off process does and a built app what its source does.
function collectProblems(pool: Pool): { label: string; text: string }[] {
  const problems: { label: string; text: string }[] = [];
  for (const build of pool.builds) {
    const label = `${pool.runs.length > 1 ? `run ${(build.run ?? 0) + 1} ` : ""}build ${build.case} ${build.variant}`;
    for (const text of build.problems) problems.push({ label, text });
  }
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

function caseLines(pool: Pool, records: ProcessRecord[], name: CaseName, branch: Branch, variant: VariantName): string[] {
  const producers = records.filter(record => record.purpose === "producer");
  const warmed = warmedByVariant(producers);
  const producersLine = !runsJITCache(variant) && !producers.length ? "JITCache ignores code built with --bytecode, so this variant ran with JITCache off only." : describeProducers(producers);
  const lines = [`## ${name}, ${branch}, ${variant}`, "", [producersLine, describeWarmed(warmed, producers)].filter(Boolean).join(" "), ""];
  const census = records.filter(record => record.purpose === "census" && record.compiles);
  if (census.length) lines.push(...censusLines(pool, census));
  const scenarios = [...new Set(records.flatMap(record => (record.scenario === null ? [] : [record.scenario])))].sort((a, b) => a - b);
  for (const scenario of scenarios) lines.push(...scenarioLines(records.filter(record => record.scenario === scenario), scenario, warmed));
  return lines;
}

function describeProducers(producers: ProcessRecord[]): string {
  if (!producers.length) return "No producer ran.";
  const calls = joinWords([...new Set(producers.map(record => String(record.calls)))], "or");
  const kinds: [Mode, string][] = [
    ["producer", "when they called delta at exit"],
    ["producerAll", "when they called delta under the delta capture all"],
    ["producerCompiled", "when they called delta under the delta capture compiled"],
    ["producerNoDelta", "when they did not call delta"],
  ];
  const committed = kinds.flatMap(([mode, when]) => {
    const counts = producers.filter(record => record.mode === mode).map(record => (record.committedBodies === null ? "n/a" : `${record.committedBodies}${typeof record.packBytes === "number" ? ` (${record.packBytes} bytes)` : ""}`));
    return counts.length ? [`${joinWords(counts)} bodies ${when}`] : [];
  });
  return `Producers of ${calls} calls, one of each kind per repetition, committed ${committed.join(", ")}.`;
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

// The compiles of each window of calls, by tier, as readCensus counts them. Native baseline compiles show how soon a
// mode's bodies leave the LLInt, imported or not. A producer that stops after P calls takes warm baseline state only from
// bodies still in baseline then, so most DFG compiles should come after call P.
function censusLines(pool: Pool, census: ProcessRecord[]): string[] {
  const calls = Math.max(...census.map(record => record.calls));
  const bounds = calls ? [...censusBounds.filter(bound => bound < calls), calls] : [];
  const windows = [
    { label: "startup", from: 0, to: 0 },
    ...bounds.map((to, i) => {
      const from = i ? bounds[i - 1] + 1 : 1;
      return { label: from === to ? String(to) : `${from}-${to}`, from, to };
    }),
    { label: "all", from: 0, to: calls },
  ];
  const modes = comparedModes.filter(mode => census.some(record => record.mode === mode));
  const tiers = [
    ["baseline", "baseline"],
    ["dfg", "DFG"],
    ["ftl", "FTL"],
  ] as const;
  const count = (mode: Mode, tier: "baseline" | "dfg" | "ftl", from: number, to: number) => {
    const value = median(census.filter(record => record.mode === mode && record.compiles![tier]).map(record => sum(record.compiles![tier].slice(from, to + 1))));
    return value === null ? "n/a" : String(value);
  };
  // The DFG compiles after the call where the producer stopped, over all of a mode's census runs.
  const producerCalls = (record: ProcessRecord) => pool.runs.find(run => run.run === record.run)?.options.producerCalls ?? 0;
  const shares = modes.map(mode => {
    const runs = census.filter(record => record.mode === mode);
    const late = sum(runs.map(record => sum(record.compiles!.dfg.slice(producerCalls(record) + 1))));
    const all = sum(runs.map(record => sum(record.compiles!.dfg)));
    return `${late} of ${all}${all ? ` (${Math.round((late / all) * 100)}%)` : ""} ${mode === "off" ? "with JITCache off" : `in ${mode}`}`;
  });
  const stops = [...new Set(census.map(producerCalls))];
  const where = `call ${joinWords(stops.map(String), "or")}, where the producers stopped`;
  return [
    `Census: one untimed process per mode made ${plural(calls, "call", "calls")}. In each window of calls, the JIT worker started these baseline compiles and the calls requested these DFG and FTL compiles; a baseline plan still queued at exit counts nowhere.`,
    "",
    `| | ${windows.map(window => window.label).join(" | ")} |`,
    `|---|${windows.map(() => "---|").join("")}`,
    ...modes.flatMap(mode => tiers.map(([tier, name]) => `| ${mode}, ${name} | ${windows.map(({ from, to }) => count(mode, tier, from, to)).join(" | ")} |`)),
    "",
    calls > Math.min(...stops) ? `DFG compiles after ${where}: ${joinWords(shares)}.` : `The census ends before or at ${where}, so it shows nothing after it.`,
    "",
  ];
}

function scenarioLines(records: ProcessRecord[], scenario: number, warmed: Map<number, number>): string[] {
  const timed = records.filter(record => record.purpose === "timed");
  const of = (mode: Mode) => timed.filter(record => record.mode === mode);
  const modes = comparedModes.filter(mode => of(mode).length);
  // The pinned CPU idles only while the process waits on something other than its own threads' work, such as the
  // kernel's teardown of an inotify watch at exit or a read from storage; JITCache off idles about none warm, so that
  // row has no ratio, nor has the storage row, which warm processes keep at 0.
  const rows: { label: string; ratioLabel?: string; measure: Measure; range?: boolean; unit: string; noRatio?: true }[] = [
    { label: "wall time", measure: record => record.wallMilliseconds, range: true, unit: "ms" },
    { label: "CPU time", measure: cpuMillisecondsOf, range: true, unit: "ms" },
    { label: "pinned CPU idle", measure: record => record.pinnedCpu?.idleMilliseconds ?? null, unit: "ms", noRatio: true },
    { label: "startup", measure: record => record.startupMilliseconds, unit: "ms" },
    { label: "first call", ratioLabel: "first call, C1/N1", measure: record => record.callMilliseconds[0] ?? null, unit: "ms" },
    ...[...finishCalls.filter(calls => calls < scenario), scenario]
      .filter(calls => calls > 0)
      .map(calls => ({ label: `after ${plural(calls, "call", "calls")}`, measure: finishedAfter(calls), unit: "ms" })),
    { label: "exit", measure: exitMillisecondsOf, unit: "ms" },
    { label: "peak memory", measure: peakMebibytes, unit: "MiB" },
    { label: "read from storage", measure: storageMebibytes, unit: "MiB", noRatio: true },
  ];
  const lines = [`### ${plural(scenario, "call", "calls")}`, ""];
  if (modes.length) {
    lines.push(`| | ${modes.join(" | ")} |`, `|---|${modes.map(() => "---|").join("")}`, `| processes | ${modes.map(mode => of(mode).length).join(" | ")} |`);
    for (const { label, measure, range, unit } of rows) lines.push(`| ${label}, ${unit} | ${modes.map(mode => cell(of(mode), measure, range ?? false)).join(" | ")} |`);
    lines.push("");
  }
  const compared = modes.filter(mode => referenceOf(mode) !== null && of(referenceOf(mode)!).length);
  if (compared.length) {
    lines.push(`| ratio to reference | ${compared.join(" | ")} |`, `|---|${compared.map(() => "---|").join("")}`);
    for (const { label, ratioLabel, measure, noRatio } of rows) {
      if (!noRatio) lines.push(`| ${ratioLabel ?? label} | ${compared.map(mode => ratioCell(of(mode), of(referenceOf(mode)!), measure)).join(" | ")} |`);
    }
    lines.push("");
  }
  // What the cold processes' drops did, group by group, and whether mincore found any page left.
  const drops = timed.flatMap(record => record.pageCache ?? []);
  if (drops.length) {
    const names = [...new Set(drops.map(group => group.name))];
    const described = names.map(name => {
      const groups = drops.filter(group => group.name === name);
      return `the ${name} (${plural(median(groups.map(group => group.files))!, "file", "files")}, ${mebibytes(median(groups.map(group => group.bytes))!)} MiB)`;
    });
    const resident = present(drops.map(group => group.residentBytes));
    const failed = drops.length - resident.length;
    const checked = `${resident.some(Boolean) ? `mincore found up to ${Math.max(...resident)} bytes of a group still cached` : "mincore found none of their pages still cached"}${failed ? `, and ${plural(failed, "drop", "drops")} failed` : ""}`;
    lines.push(`Each cold process started after the driver dropped from the page cache ${joinWords(described)}; ${checked}. Afterwards the driver read every dropped file back.`, "");
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
    const columns = comparedModes.filter(mode => reports.some(record => record.mode === mode));
    const counts: [string, (counts: ReportCounts) => number][] = [
      ["installs", counts => counts.installs],
      ["imports", counts => counts.requests.import ?? 0],
      ["misses, bodies the artifact lacks", counts => counts.lookups["bodyVersion.absent"] ?? 0],
      ["native baseline compiles", counts => counts.compiles],
    ];
    // Bun imports a module's analysis in place of its analysis parse; each outcome gives its count, its thread CPU time
    // and the analysis bytes it read.
    const outcomes = [...new Set(reports.flatMap(record => Object.keys(record.report!.moduleAnalyses ?? {})))].sort();
    for (const outcome of outcomes) {
      const analysis = (counts: ReportCounts) => counts.moduleAnalyses?.[outcome] ?? { count: 0, nanoseconds: 0, bytes: 0 };
      counts.push(
        [`module analyses, ${outcome}`, counts => analysis(counts).count],
        [`module analyses, ${outcome}, µs`, counts => Math.round(analysis(counts).nanoseconds / 1000)],
        [`module analyses, ${outcome}, bytes`, counts => analysis(counts).bytes],
      );
    }
    lines.push(
      "Untimed consumers with JITCache's bench report:",
      "",
      `| | ${columns.join(" | ")} |`,
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

// The nearest rank: the ceil(0.9 n)-th value in ascending order.
function p90(values: (number | null | undefined)[]): number | null {
  const sorted = present(values).sort((a, b) => a - b);
  return sorted.length ? sorted[Math.ceil(0.9 * sorted.length) - 1] : null;
}

// Milliseconds or mebibytes, with the precision their size calls for.
const fixed = (value: number) => value.toFixed(Math.abs(value) < 10 ? 2 : Math.abs(value) < 1000 ? 1 : 0);
// A request's milliseconds, as the 10-14 digest wrote them: a warm request takes tens of microseconds.
const latency = (value: number) => value.toFixed(value < 1 ? 3 : value < 10 ? 2 : 1);
const mebibytes = (bytes: number) => fixed(bytes / 1048576);
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
  for (const tool of ["taskset", "sync", "flock", "getconf", "who"]) if (!Bun.which(tool)) return `${tool} is not on PATH`;
  return null;
}

// What keeps the run from using this Bun: a flag or JSC option the run needs that the Bun lacks. Bun lists the JITCache
// flags it takes in its help, and exits at startup on a BUN_JSC_ option its JSC does not have.
function bunRefusal(options: Options, help: string, environment: Record<string, string>): string | null {
  if (options.deltaCapture.length && !help.includes("--jitcache-delta-capture")) return `--delta-capture needs a Bun that takes --jitcache-delta-capture, which ${options.bun} does not list in its help`;
  if (options.compareLLIntProgress) {
    const result = Bun.spawnSync([options.bun, "-e", "0"], { env: { ...environment, ...modeEnvironment({ mode: "consumerNoLLIntProgress", purpose: "timed" } as Job) }, stdout: "pipe", stderr: "pipe" });
    if (!result.success) return `--compare-llint-progress needs a Bun whose JSC has the option jitCacheImportLLIntProgress, and ${options.bun} failed with it: ${result.stderr.toString().trim().split("\n")[0]}`;
  }
  return null;
}

// Cold processes, timed or the cold consumer the reports run, need the page cache's FFI.
const runsCold = (options: Options) => (options.processes > 0 && options.coldProcesses > 0) || options.reports;

const sameFile = (a: string, b: string) => {
  const [first, second] = [statSync(a), statSync(b)];
  return first.dev === second.dev && first.ino === second.ino;
};

// summary.md and request-curves.tsv, both built from the pooled records alone.
function writeSummary(pool: Pool, directory: string): { text: string; passed: boolean; summaryPath: string; curvesPath: string } {
  const { text, passed } = summarize(pool);
  mkdirSync(directory, { recursive: true });
  const summaryPath = join(directory, "summary.md");
  const curvesPath = join(directory, "request-curves.tsv");
  writeFileSync(summaryPath, text);
  writeFileSync(curvesPath, requestCurves(pool));
  return { text, passed, summaryPath, curvesPath };
}

async function main(argv: string[]): Promise<number> {
  const allowed = allowedCpus();
  let command: Command;
  try {
    command = parseCommandLine(argv, allowed.list);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    console.error(`bench.ts: ${error.message}\n${usage}`);
    return 2;
  }
  if (command.kind === "summarize") {
    let pool: Pool;
    try {
      pool = readResults(command.files);
    } catch (error) {
      console.error(`bench.ts: ${(error as Error).message}`);
      return 1;
    }
    const { text, passed, summaryPath, curvesPath } = writeSummary(pool, command.out ?? dirname(command.files[0]));
    console.log(`${text}\nsummary: ${summaryPath}\nrequest curves: ${curvesPath}`);
    return passed ? 0 : 1;
  }

  const { options } = command;
  const refused = refusal();
  if (refused) {
    console.error(`bench.ts: ${refused}`);
    return 1;
  }
  const transpilerCache = join(options.out, "transpiler-cache");
  const environment = processEnvironment(transpilerCache);
  const clockTicks = Number(Bun.spawnSync(["getconf", "CLK_TCK"]).stdout.toString().trim()) || 100;
  let pageCache: PageCache | null = null;
  if (runsCold(options)) {
    try {
      pageCache = new PageCache();
    } catch (error) {
      console.error(`bench.ts: cold processes need libc through bun:ffi, which failed: ${(error as Error).message}; --cold-processes=0 --no-reports runs none`);
      return 1;
    }
  }
  const driver = pinDriver(options.cpu, allowed.list);
  // Every build, test and other bench waits until this run exits, --check's included.
  holdMachineLock();
  const probe = (flag: string) => Bun.spawnSync([options.bun, flag], { env: environment, stdout: "pipe", stderr: "pipe" });
  const revisionProbe = probe("--revision");
  const revision = revisionProbe.stdout.toString().trim() || `--revision failed: ${revisionProbe.stderr.toString().trim()}`;
  const help = probe("--help").stdout.toString();
  const bunRefused = bunRefusal(options, help, environment);
  if (bunRefused) {
    console.error(`bench.ts: ${bunRefused}`);
    return 1;
  }
  const processDirectory = join(options.out, "processes");
  mkdirSync(processDirectory, { recursive: true });
  mkdirSync(join(options.out, "artifacts"));
  const results = new ResultsFile(join(options.out, "results.jsonl"));
  const driverRunsBun = sameFile(options.bun, process.execPath);
  const context: Context = { options, environment, transpilerCache, pageCache, driverRunsBun, clockTicks, processDirectory };
  const runRecord: RunRecord = {
    kind: "run",
    started: new Date().toISOString(),
    argv,
    options,
    bun: options.bun,
    revision,
    ...machineFacts(),
    pinnedCpuSiblings: smtSiblings(options.cpu),
    driverCpu: driver.cpu,
    driverNote: driver.note,
    driverRunsBun,
    pageSize: pageCache?.pageSize ?? null,
    taskIoAccounting: existsSync("/proc/self/io"),
    clockTicks,
    environment,
  };
  results.write(runRecord);
  console.log(
    `${options.bun} (${revision}): ${options.cases.join(", ")}; ${joinWords([...options.branches])} branches; variants ${joinWords([...options.variants])}; scenarios of ${joinWords(options.scenarios.map(String))} calls; ${plural(options.processes, "process", "processes")} per mode in ${plural(options.repetitions, "repetition", "repetitions")}. Bun runs on CPU ${options.cpu}, and the driver ${driver.cpu === null ? `has no CPU of its own: ${driver.note}` : `on CPU ${driver.cpu}`}.${driverRunsBun && runsCold(options) ? " The driver runs this Bun itself, so cold processes start with its executable cached." : ""}`,
  );
  const run = async (job: Job) => {
    const record = await runJob(context, job);
    results.write(record);
    console.log(progressLine(record));
  };
  // A failed build leaves its variant nothing to measure, so the run ends there and its summary lists the failure.
  if (buildApps(context, results)) {
    for (let repetition = 1; repetition <= options.repetitions; ++repetition) {
      const started = new Date().toISOString();
      const idle = await waitForIdle(clockTicks, !options.check);
      const record: RepetitionRecord = { kind: "repetition", repetition, started, ...idle };
      results.write(record);
      console.log(`repetition ${repetition}: ${idle.idle ? "idle" : "noisy"}, load average ${idle.loadAverage[0].toFixed(2)}${idle.waitedSeconds ? ` after a wait of ${idle.waitedSeconds} s` : ""}`);
      for (const job of repetitionJobs(options, repetition)) await run(job);
    }
    for (const job of untimedJobs(options)) await run(job);
  }
  const end: EndRecord = { kind: "end", finished: new Date().toISOString(), loadAverage: loadavg() };
  results.write(end);
  const { text, passed, summaryPath, curvesPath } = writeSummary(readResults([results.path]), options.out);
  console.log(`\n${text}\nresults: ${results.path}\nsummary: ${summaryPath}\nrequest curves: ${curvesPath}`);
  return passed ? 0 : 1;
}

if (import.meta.main) process.exit(await main(process.argv.slice(2)));
