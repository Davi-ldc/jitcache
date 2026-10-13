#!/usr/bin/env bun
// JITCache's macro benches (docs/JitCache/drafts/macro-benches.md, HARNESS.md Benches). Each case of
// PerformanceTests/JITCache runs in fresh Bun processes: one producer, which commits its artifact at exit, then
// processes with JITCache off alternating with consumers of that artifact. Every process must compute the same checksum
// for each pass. The summary sets startup, the first pass and the catch-up curve beside each process's wall time, peak
// memory and the counts of JITCache's bench report (SPEC-integrator.harness.md section 9).

import { accessSync, appendFileSync, closeSync, constants, mkdirSync, openSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { cpus, homedir, hostname, loadavg, totalmem } from "node:os";
import { dirname, join, resolve } from "node:path";

const repositoryRoot = resolve(import.meta.dirname, "../..");
const benchDirectory = join(repositoryRoot, "PerformanceTests/JITCache");

const caseNames = ["api", "elysia", "ingest", "ssr"] as const;
type CaseName = (typeof caseNames)[number];
// A JITCache mode is spelled as the bench report's start event spells its role.
type Mode = "off" | "producer" | "consumer";

// W is the median of the producer's last five passes (HARNESS.md Benches).
const warmedPasses = 5;
// HARNESS.md leaves catchUpMargin unset; the summary names the first pass within this candidate.
const candidateMargin = 1.1;

const usage = `Usage: bun Tools/Scripts/jitcache-macro-bench.ts --bun=<Bun executable> [--cases=api,elysia,ingest,ssr]
           [--processes=M] [--passes=P] [--out=<directory>] [--timeout=<seconds>]
Runs each case as one producer and, alternating, M processes with JITCache off and M consumers, each for P passes;
M is 5 and P is 34 by default. --out defaults to jitcache-macro/<time> in the directory that holds the Bun executable.
A process that runs longer than --timeout, 1800 seconds by default, is killed and fails its case.`;

class UsageError extends Error {}

// ---------------------------------------------------------------------------------------------------------------------
// Command line

interface Options {
  bun: string;
  cases: CaseName[];
  processes: number;
  passes: number;
  out: string;
  timeoutSeconds: number;
}

function parseCommandLine(argv: string[]): Options {
  let bun: string | null = null;
  let out: string | null = null;
  let cases: CaseName[] = [...caseNames];
  let processes = 5;
  let passes = 34;
  let timeoutSeconds = 1800;
  for (const argument of argv) {
    const equals = argument.indexOf("=");
    const name = equals < 0 ? argument : argument.slice(0, equals);
    const value = equals < 0 ? null : argument.slice(equals + 1);
    const required = () => {
      if (!value) throw new UsageError(`${name} needs a value`);
      return value;
    };
    switch (name) {
      case "--bun":
        bun = resolve(required());
        break;
      case "--cases":
        cases = parseCases(required());
        break;
      case "--processes":
        processes = positiveInteger(required(), name);
        break;
      case "--passes":
        passes = positiveInteger(required(), name);
        break;
      case "--out":
        out = resolve(required());
        break;
      case "--timeout":
        timeoutSeconds = positiveInteger(required(), name);
        break;
      default:
        throw new UsageError(`unknown argument ${argument}`);
    }
  }
  if (!bun) throw new UsageError("--bun is required");
  if (!statSync(bun, { throwIfNoEntry: false })?.isFile()) throw new UsageError(`--bun names no file: ${bun}`);
  try {
    accessSync(bun, constants.X_OK);
  } catch {
    throw new UsageError(`--bun names a file that is not executable: ${bun}`);
  }
  // A producer is rejected when its path already holds an artifact, so every run starts in a directory of its own.
  const existing = out ? statSync(out, { throwIfNoEntry: false }) : undefined;
  if (out && existing && (!existing.isDirectory() || readdirSync(out).length)) throw new UsageError(`--out names a file or a directory that is not empty: ${out}`);
  out ??= join(dirname(bun), "jitcache-macro", new Date().toISOString().replace(/[:.]/g, "-"));
  return { bun, cases, processes, passes, out, timeoutSeconds };
}

function parseCases(text: string): CaseName[] {
  const cases: CaseName[] = [];
  for (const name of text.split(",")) {
    if (!(caseNames as readonly string[]).includes(name)) throw new UsageError(`--cases names ${JSON.stringify(name)}, which is none of ${caseNames.join(", ")}`);
    if (cases.includes(name as CaseName)) throw new UsageError(`--cases names ${name} twice`);
    cases.push(name as CaseName);
  }
  return cases;
}

function positiveInteger(text: string, name: string): number {
  const number = Number(text);
  if (!/^\d+$/.test(text) || number < 1) throw new UsageError(`${name} takes a positive integer, not ${text}`);
  return number;
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

interface ProcessRecord {
  kind: "process";
  case: CaseName;
  mode: Mode;
  index: number; // from 1 within its mode
  command: string[];
  exitCode: number | null;
  signal: string | null;
  timedOut: boolean;
  wallMilliseconds: number; // from the spawn to the exit
  startupMilliseconds: number | null; // the ready line's Bun.nanoseconds()
  passMilliseconds: number[];
  checksums: string[];
  rssBytes: number | null; // the end line's
  maxRSSBytes: number | null; // the kernel's peak resident set for the process, the same measure in every mode
  cpuMicroseconds: { user: number; system: number } | null;
  strayLines: number; // standard output lines that are no event of run.ts
  jitcache: ReportCounts | null;
  problems: string[];
}

interface CaseRun {
  readonly options: Options;
  readonly name: CaseName;
  readonly directory: string;
  readonly artifact: string;
  readonly environment: Record<string, string>;
}

const labelOf = (record: { mode: Mode; index: number }) => `${record.mode} ${record.index}`;

// No JSC_ or BUN_JSC_ variable reaches a process, so every process runs the JSC options Bun ships with. NODE_ENV is
// production, as in a deployment, and run.ts requires it. BUN_DEBUG_QUIET_LOGS=1 keeps a debug Bun's scoped logs out of
// standard output, which carries run.ts's events; a release Bun ignores it.
function caseEnvironment(): Record<string, string> {
  const environment: Record<string, string> = {};
  for (const [name, value] of Object.entries(process.env)) {
    if (value === undefined || name.startsWith("JSC_") || name.startsWith("BUN_JSC_")) continue;
    environment[name] = value;
  }
  environment.NODE_ENV = "production";
  environment.BUN_DEBUG_QUIET_LOGS = "1";
  return environment;
}

async function runProcess(run: CaseRun, mode: Mode, index: number): Promise<ProcessRecord> {
  const { options, name, directory, artifact } = run;
  const files = { stdout: join(directory, `${mode}-${index}.stdout`), stderr: join(directory, `${mode}-${index}.stderr`), report: join(directory, `${mode}-${index}.bench.jsonl`) };
  // JITCache's flags go before the script, where Bun reads its own. A consumer takes the default mode.
  const flags = mode === "off" ? [] : [`--jitcache=${artifact}`, ...(mode === "producer" ? ["--jitcache-mode=p"] : []), `--jitcache-bench-report=${files.report}`];
  const command = [options.bun, ...flags, "run.ts", name, `--passes=${options.passes}`];
  const record: ProcessRecord = {
    kind: "process",
    case: name,
    mode,
    index,
    command,
    exitCode: null,
    signal: null,
    timedOut: false,
    wallMilliseconds: 0,
    startupMilliseconds: null,
    passMilliseconds: [],
    checksums: [],
    rssBytes: null,
    maxRSSBytes: null,
    cpuMicroseconds: null,
    strayLines: 0,
    jitcache: null,
    problems: [],
  };

  const stdout = openSync(files.stdout, "w");
  const stderr = openSync(files.stderr, "w");
  try {
    const started = performance.now();
    const child = Bun.spawn(command, { cwd: benchDirectory, env: run.environment, stdin: "ignore", stdout, stderr });
    const timer = setTimeout(() => {
      record.timedOut = true;
      child.kill("SIGKILL");
    }, options.timeoutSeconds * 1000);
    await child.exited;
    record.wallMilliseconds = performance.now() - started;
    clearTimeout(timer);
    record.exitCode = child.exitCode;
    record.signal = child.signalCode;
    const usage = child.resourceUsage();
    if (usage) {
      record.maxRSSBytes = usage.maxRSS;
      record.cpuMicroseconds = { user: Number(usage.cpuTime.user), system: Number(usage.cpuTime.system) };
    }
  } catch (error) {
    record.problems.push(`could not start: ${(error as Error).message}`);
    return record;
  } finally {
    closeSync(stdout);
    closeSync(stderr);
  }

  if (record.timedOut) record.problems.push(`killed after ${plural(options.timeoutSeconds, "second", "seconds")}`);
  else if (record.signal) record.problems.push(`ended by ${record.signal}`);
  else if (record.exitCode !== 0) record.problems.push(`exited with ${record.exitCode}`);
  readEvents(readText(files.stdout) ?? "", options.passes, record);
  const stderrText = readText(files.stderr) ?? "";
  if (record.problems.length && stderrText.trim()) record.problems.push(`standard error ends with: ${tail(stderrText)}`);

  if (mode !== "off") {
    const reportText = readText(files.report);
    if (reportText === null) {
      record.problems.push(`wrote no bench report at ${files.report}`);
    } else {
      const counts = (record.jitcache = readReport(reportText));
      const start = counts.start;
      if (start?.outcome !== "started" || start?.role !== mode) record.problems.push(`did not start JITCache as a ${mode}: its start event is ${JSON.stringify(start)}`);
      // The checks above pass in a run that measures nothing; these show that the case happened.
      if (mode === "producer" && !counts.committedCaptures) record.problems.push("committed no body");
      if (mode === "consumer" && !counts.installs) record.problems.push("installed no body");
    }
    // Bun warns once on standard error when JITCache turns itself off or stops writing (SPEC-integrator.md section
    // 11.2), and then the process measured native runs.
    for (const line of stderrText.split("\n")) {
      if (line.startsWith("warn: JITCache")) record.problems.push(`warned: ${line}`);
    }
  }
  return record;
}

// run.ts's events: a ready line, then pass lines 1 to P in order, then an end line. Other lines are counted and left
// alone, since a debug Bun can write its own.
function readEvents(text: string, passes: number, record: ProcessRecord) {
  let ready = false;
  let ended = false;
  for (const line of text.split("\n")) {
    let event: Record<string, unknown> | null = null;
    try {
      event = line.startsWith("{") ? JSON.parse(line) : null;
    } catch {}
    if (!event || typeof event !== "object" || !["ready", "pass", "end"].includes(event.event as string)) {
      if (line.trim()) record.strayLines++;
      continue;
    }
    if (ended) {
      record.problems.push(`printed ${JSON.stringify(event.event)} after its end line`);
      continue;
    }
    if (event.event === "ready") {
      if (ready) record.problems.push("printed a second ready line");
      ready = true;
      const startup = numberOr(event.startupNanoseconds, null);
      if (startup === null) record.problems.push(`printed a ready line without startupNanoseconds: ${line}`);
      else record.startupMilliseconds = startup / 1e6;
    } else if (event.event === "pass") {
      const expected = record.passMilliseconds.length + 1;
      const milliseconds = numberOr(event.milliseconds, null);
      if (!ready) {
        record.problems.push("printed a pass line before its ready line");
        ready = true;
      }
      if (event.pass !== expected || milliseconds === null || typeof event.checksum !== "string" || !event.checksum) {
        record.problems.push(`printed ${line} where the line of pass ${expected} was due`);
        continue;
      }
      record.passMilliseconds.push(milliseconds);
      record.checksums.push(event.checksum);
    } else {
      ended = true;
      record.rssBytes = numberOr(event.rssBytes, null);
    }
  }
  if (!ready) record.problems.push("printed no ready line");
  if (record.passMilliseconds.length !== passes) record.problems.push(`printed ${record.passMilliseconds.length} pass lines of ${passes}`);
  if (!ended) record.problems.push("printed no end line");
}

// ---------------------------------------------------------------------------------------------------------------------
// One case

interface Stat {
  min: number | null;
  median: number | null;
}

interface CaseSummary {
  kind: "case";
  case: CaseName;
  passed: boolean;
  problems: string[]; // each prefixed by its process
  kept: string | null; // a failed case's directory
  startupMilliseconds: { off: Stat; consumer: Stat; producer: number | null };
  firstPassMilliseconds: { off: Stat; consumer: Stat; producer: number | null }; // N1 and C1
  firstPassRatio: Stat; // C1/N1, from the minimums and from the medians
  warmedMilliseconds: number | null; // W
  warmedPasses: [number, number] | null; // the producer passes W is the median of
  // Ck/W and Nk/W for k = 2 to P, from the minimum over the processes of each pass.
  curve: { pass: number; consumer: number | null; off: number | null }[];
  // The first pass whose ratio is within the candidate margin, and the pass from which every ratio is.
  caughtUp: { consumer: { first: number | null; from: number | null }; off: { first: number | null; from: number | null } };
  wallMilliseconds: { off: Stat; consumer: Stat; producer: number | null };
  maxRSSBytes: { off: Stat; consumer: Stat; producer: number | null };
}

function statOf(values: (number | null | undefined)[]): Stat {
  const present = values.filter((value): value is number => typeof value === "number" && Number.isFinite(value)).sort((a, b) => a - b);
  if (!present.length) return { min: null, median: null };
  const middle = present.length >> 1;
  return { min: present[0], median: present.length % 2 ? present[middle] : (present[middle - 1] + present[middle]) / 2 };
}

const divide = (a: number | null, b: number | null) => (a === null || b === null || b === 0 ? null : a / b);

function caughtUp(ratios: (number | null)[], passes: number[]) {
  const within = ratios.map(ratio => ratio !== null && ratio <= candidateMargin);
  const firstIndex = within.indexOf(true);
  let fromIndex = within.length;
  while (fromIndex > 0 && within[fromIndex - 1]) --fromIndex;
  return { first: firstIndex < 0 ? null : passes[firstIndex], from: fromIndex === within.length ? null : passes[fromIndex] };
}

function summarize(name: CaseName, records: ProcessRecord[], passes: number): CaseSummary {
  const ofMode = (mode: Mode) => records.filter(record => record.mode === mode);
  const off = ofMode("off");
  const consumers = ofMode("consumer");
  const producer = ofMode("producer")[0] ?? null;
  const firstPass = { off: statOf(off.map(r => r.passMilliseconds[0])), consumer: statOf(consumers.map(r => r.passMilliseconds[0])), producer: producer?.passMilliseconds[0] ?? null };

  const producerPasses = producer?.passMilliseconds ?? [];
  const warmed = producerPasses.slice(-warmedPasses);
  const warmedMilliseconds = statOf(warmed).median;
  const curve: CaseSummary["curve"] = [];
  for (let pass = 2; pass <= passes; ++pass) {
    curve.push({
      pass,
      consumer: divide(statOf(consumers.map(r => r.passMilliseconds[pass - 1])).min, warmedMilliseconds),
      off: divide(statOf(off.map(r => r.passMilliseconds[pass - 1])).min, warmedMilliseconds),
    });
  }
  const curvePasses = curve.map(point => point.pass);
  const problems = records.flatMap(record => record.problems.map(problem => `${labelOf(record)}: ${problem}`));
  return {
    kind: "case",
    case: name,
    passed: problems.length === 0,
    problems,
    kept: null,
    startupMilliseconds: { off: statOf(off.map(r => r.startupMilliseconds)), consumer: statOf(consumers.map(r => r.startupMilliseconds)), producer: producer?.startupMilliseconds ?? null },
    firstPassMilliseconds: firstPass,
    firstPassRatio: { min: divide(firstPass.consumer.min, firstPass.off.min), median: divide(firstPass.consumer.median, firstPass.off.median) },
    warmedMilliseconds,
    warmedPasses: warmed.length ? [producerPasses.length - warmed.length + 1, producerPasses.length] : null,
    curve,
    caughtUp: { consumer: caughtUp(curve.map(point => point.consumer), curvePasses), off: caughtUp(curve.map(point => point.off), curvePasses) },
    wallMilliseconds: { off: statOf(off.map(r => r.wallMilliseconds)), consumer: statOf(consumers.map(r => r.wallMilliseconds)), producer: producer?.wallMilliseconds ?? null },
    maxRSSBytes: { off: statOf(off.map(r => r.maxRSSBytes)), consumer: statOf(consumers.map(r => r.maxRSSBytes)), producer: producer?.maxRSSBytes ?? null },
  };
}

async function runCase(options: Options, name: CaseName, results: ResultsFile, environment: Record<string, string>): Promise<{ summary: CaseSummary; records: ProcessRecord[] }> {
  const directory = join(options.out, name);
  mkdirSync(directory, { recursive: true });
  const run: CaseRun = { options, name, directory, artifact: join(directory, "artifact"), environment };
  // The producer goes first, since the consumers read what it commits. Off and consumer processes then alternate, so
  // drift on the machine spreads over both.
  const schedule: { mode: Mode; index: number }[] = [{ mode: "producer", index: 1 }];
  for (let index = 1; index <= options.processes; ++index) schedule.push({ mode: "off", index }, { mode: "consumer", index });

  const records: ProcessRecord[] = [];
  const reference = new Map<number, { checksum: string; label: string }>();
  for (const { mode, index } of schedule) {
    const record = await runProcess(run, mode, index);
    // A JITCache process must compute what a JITCache-off process computes (THREAD Verification), pass by pass.
    record.checksums.forEach((checksum, i) => {
      const first = reference.get(i + 1);
      if (!first) reference.set(i + 1, { checksum, label: labelOf(record) });
      else if (first.checksum !== checksum) record.problems.push(`computed checksum ${checksum.slice(0, 16)} for pass ${i + 1}, where ${first.label} computed ${first.checksum.slice(0, 16)}`);
    });
    results.write(record);
    console.log(progressLine(record));
    records.push(record);
  }

  const summary = summarize(name, records, options.passes);
  // A passing case keeps its numbers in the results; a failing one keeps every process's output, its bench reports and
  // the artifact, for whoever looks into it.
  if (summary.passed) rmSync(directory, { recursive: true, force: true });
  else summary.kept = directory;
  results.write(summary);
  return { summary, records };
}

function progressLine(record: ProcessRecord): string {
  const label = `${record.case} ${labelOf(record)}`;
  const parts = [`ready ${milliseconds(record.startupMilliseconds)}`];
  if (record.passMilliseconds.length) parts.push(`pass 1 ${milliseconds(record.passMilliseconds[0])}`);
  if (record.passMilliseconds.length > 1) parts.push(`pass ${record.passMilliseconds.length} ${milliseconds(record.passMilliseconds.at(-1)!)}`);
  parts.push(`wall ${milliseconds(record.wallMilliseconds)}`, `peak ${mebibytes(record.maxRSSBytes)}`);
  if (record.jitcache && record.mode === "consumer") parts.push(`${record.jitcache.installs} installs`);
  if (record.jitcache && record.mode === "producer") parts.push(`${record.jitcache.committedCaptures} commits`);
  const line = `${label}: ${parts.join(", ")}`;
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

function milliseconds(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return "n/a";
  if (value >= 10_000) return `${(value / 1000).toFixed(1)} s`;
  return `${value.toFixed(value < 10 ? 2 : 1)} ms`;
}

function mebibytes(value: number | null | undefined): string {
  return value === null || value === undefined ? "n/a" : `${(value / 1048576).toFixed(1)} MiB`;
}

const ratio = (value: number | null) => (value === null ? "n/a" : value.toFixed(2));
const minAndMedian = (stat: Stat, format: (value: number | null) => string) =>
  stat.min === null ? "n/a" : stat.min === stat.median ? format(stat.min) : `${format(stat.min)} (${format(stat.median)})`;
const plural = (count: number, noun: string, nouns: string) => `${count} ${count === 1 ? noun : nouns}`;

// A count over several processes: the median, with the range when the processes differ.
function spread(values: number[], format: (value: number) => string = String): string {
  if (!values.length) return "n/a";
  const sorted = [...values].sort((a, b) => a - b);
  const median = statOf(sorted).median!;
  return sorted[0] === sorted.at(-1) ? format(median) : `${format(median)} (${format(sorted[0])} to ${format(sorted.at(-1)!)})`;
}

const cpuMilliseconds = (nanoseconds: number) => (nanoseconds / 1e6).toFixed(1);
const bytesInMebibytes = (bytes: number) => (bytes / 1048576).toFixed(1);

// One keyed count, such as requests by action: a value per key that some process counted, spread over the processes.
function keyedCounts(maps: Record<string, number>[]): string {
  const keys = [...new Set(maps.flatMap(map => Object.keys(map).filter(key => map[key])))].sort();
  return keys.length ? keys.map(key => `${key} ${spread(maps.map(map => map[key] ?? 0))}`).join(", ") : "none";
}

function jitcacheRows(consumers: ReportCounts[], producer: ReportCounts | null): string[] {
  const row = (label: string, read: (counts: ReportCounts) => number | null, format: (value: number) => string = String) => {
    const consumerValues = consumers.map(read).filter((value): value is number => value !== null);
    const producerValue = producer ? read(producer) : null;
    return `| ${label} | ${spread(consumerValues, format)} | ${producerValue === null ? "n/a" : format(producerValue)} |`;
  };
  return [
    "| JITCache | consumer | producer |",
    "|---|---|---|",
    row("start, CPU ms", counts => counts.start?.nanoseconds ?? null, cpuMilliseconds),
    row("bodies indexed at start", counts => counts.start?.indexedBodies ?? null),
    row("installs", counts => counts.installs),
    row("install, CPU ms", counts => counts.installNanoseconds, cpuMilliseconds),
    `| requests | ${keyedCounts(consumers.map(counts => counts.requests))} | ${producer ? keyedCounts([producer.requests]) : "n/a"} |`,
    row("request points, CPU ms", counts => counts.requestNanoseconds, cpuMilliseconds),
    `| lookups, misses being bodyVersion.absent | ${keyedCounts(consumers.map(counts => counts.lookups))} | ${producer ? keyedCounts([producer.lookups]) : "n/a"} |`,
    row("native baseline compiles", counts => counts.compiles),
    row("native compile and finalize, CPU ms", counts => counts.compileNanoseconds, cpuMilliseconds),
    `| captures | ${keyedCounts(consumers.map(counts => counts.captures))} | ${producer ? keyedCounts([producer.captures]) : "n/a"} |`,
    row("committed captures, MiB of sections", counts => counts.capturedBytes, bytesInMebibytes),
    row("delta: committed bodies", counts => counts.deltaCommittedBodies),
    row("delta: committed MiB", counts => counts.deltaCommittedBytes, bytesInMebibytes),
    row("delta, CPU ms", counts => counts.deltaNanoseconds, cpuMilliseconds),
    row("producer budget peak, MiB", counts => counts.budgetPeakBytes, bytesInMebibytes),
    row("peak resident set at exit, MiB", counts => counts.maxResidentBytes, bytesInMebibytes),
    row("executable memory committed, MiB", counts => counts.executablePoolCommittedBytes, bytesInMebibytes),
  ];
}

function caseSection(summary: CaseSummary, records: ProcessRecord[], options: Options): string {
  const lines = [`## ${summary.case}: ${summary.passed ? "passed" : "FAILED"}`, ""];
  if (!summary.passed) {
    lines.push(...summary.problems.map(problem => `- ${problem}`), "", `Kept: ${summary.kept}`, "");
  }
  const w = summary.warmedPasses ? `${milliseconds(summary.warmedMilliseconds)}, the median of passes ${summary.warmedPasses[0]} to ${summary.warmedPasses[1]}` : "n/a";
  // The first pass within the margin and, when later passes leave it, the pass from which every pass stays within.
  const caught = (point: { first: number | null; from: number | null }) =>
    point.first === null ? "none" : point.from === point.first ? `${point.first}` : `${point.first} (stays from ${point.from ?? "no pass"})`;
  lines.push(
    "| | off | consumer | producer |",
    "|---|---|---|---|",
    `| startup | ${minAndMedian(summary.startupMilliseconds.off, milliseconds)} | ${minAndMedian(summary.startupMilliseconds.consumer, milliseconds)} | ${milliseconds(summary.startupMilliseconds.producer)} |`,
    `| first pass: N1, C1 | ${minAndMedian(summary.firstPassMilliseconds.off, milliseconds)} | ${minAndMedian(summary.firstPassMilliseconds.consumer, milliseconds)} | ${milliseconds(summary.firstPassMilliseconds.producer)} |`,
    `| C1/N1 | | ${minAndMedian(summary.firstPassRatio, ratio)} | |`,
    `| W | | | ${w} |`,
    `| first pass within ${candidateMargin} of W | ${caught(summary.caughtUp.off)} | ${caught(summary.caughtUp.consumer)} | |`,
    `| process wall time | ${minAndMedian(summary.wallMilliseconds.off, milliseconds)} | ${minAndMedian(summary.wallMilliseconds.consumer, milliseconds)} | ${milliseconds(summary.wallMilliseconds.producer)} |`,
    `| peak memory | ${minAndMedian(summary.maxRSSBytes.off, mebibytes)} | ${minAndMedian(summary.maxRSSBytes.consumer, mebibytes)} | ${mebibytes(summary.maxRSSBytes.producer)} |`,
    "",
  );
  if (summary.curve.length) {
    lines.push("| k | Ck/W | Nk/W |", "|---|---|---|", ...summary.curve.map(point => `| ${point.pass} | ${ratio(point.consumer)} | ${ratio(point.off)} |`), "");
  }
  const consumers = records.filter(record => record.mode === "consumer" && record.jitcache).map(record => record.jitcache!);
  const producer = records.find(record => record.mode === "producer")?.jitcache ?? null;
  if (consumers.length || producer) lines.push(...jitcacheRows(consumers, producer), "");
  if (options.passes <= warmedPasses) lines.push(`With ${options.passes} passes, W includes the producer's first passes.`, "");
  return lines.join("\n");
}

function summaryText(options: Options, revision: string, before: number[], after: number[], sections: string[]): string {
  const load = (average: number[]) => average.map(value => value.toFixed(2)).join(", ");
  return [
    "# JITCache macro benches",
    "",
    `Bun ${options.bun}, ${revision}. ${plural(options.processes, "process", "processes")} per mode, ${plural(options.passes, "pass", "passes")} each, on ${hostname()} (${cpus().length} CPUs, ${cpus()[0]?.model ?? "unknown CPU"}, ${mebibytes(totalmem())}). Load average ${load(before)} before and ${load(after)} after.`,
    "",
    "Times are wall time. A cell over several processes gives the minimum and, in parentheses, the median; Ck and Nk are the minimum over the consumers and over the off processes of pass k. The JITCache counts come from each process's bench report, and a consumer cell gives the median over the consumers, with the range when they differ. Peak memory is the kernel's peak resident set for each process.",
    "",
    ...sections,
  ].join("\n");
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

async function main(argv: string[]): Promise<number> {
  let options: Options;
  try {
    options = parseCommandLine(argv);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    console.error(`jitcache-macro-bench: ${error.message}\n${usage}`);
    return 2;
  }
  if (!statSync(join(benchDirectory, "node_modules"), { throwIfNoEntry: false })?.isDirectory()) {
    console.error(`jitcache-macro-bench: ${benchDirectory} has no node_modules; run bun install there`);
    return 1;
  }
  mkdirSync(options.out, { recursive: true });
  const results = new ResultsFile(join(options.out, "results.jsonl"));
  const environment = caseEnvironment();

  const release = await takeBuildLock();
  const sections: string[] = [];
  let passed = true;
  let revision = "";
  const before = loadavg();
  try {
    const probe = Bun.spawnSync([options.bun, "--revision"], { env: environment, stdout: "pipe", stderr: "pipe" });
    revision = probe.stdout.toString().trim() || `--revision failed: ${probe.stderr.toString().trim()}`;
    results.write({ kind: "run", bun: options.bun, revision, cases: options.cases, processes: options.processes, passes: options.passes, out: options.out, started: new Date().toISOString(), host: hostname(), cpus: cpus().length, cpuModel: cpus()[0]?.model ?? null, memoryBytes: totalmem(), loadAverage: before });
    console.log(`bun ${options.bun} (${revision}): ${options.cases.join(", ")}, ${plural(options.processes, "process", "processes")} per mode, ${plural(options.passes, "pass", "passes")}`);
    for (const name of options.cases) {
      const { summary, records } = await runCase(options, name, results, environment);
      passed &&= summary.passed;
      sections.push(caseSection(summary, records, options));
    }
  } finally {
    await release();
  }
  const after = loadavg();
  results.write({ kind: "end", finished: new Date().toISOString(), loadAverage: after, passed });

  const summary = summaryText(options, revision, before, after, sections);
  const summaryPath = join(options.out, "summary.md");
  writeFileSync(summaryPath, summary);
  console.log(`\n${summary}`);
  console.log(`results: ${results.path}\nsummary: ${summaryPath}`);
  return passed ? 0 : 1;
}

if (import.meta.main) process.exit(await main(process.argv.slice(2)));
