// The runner's bench mode, --bench: it measures a release-local build instead of testing it. Every jsc-hosted script
// plain mode runs goes through --bench-processes fresh copies of each sequence, one process at a time under the build
// lock, with the bench report on and strict at its default. Each run's line gives its CPU-time spans' minimum and median
// over the copies beside every copy's value, and each Consumer run that may install also runs as native copies, whose
// artifact holds no body, for the installation bound per body.

import { spawn } from "node:child_process";
import { cpSync, mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { cpus, homedir, loadavg, release, tmpdir, totalmem } from "node:os";
import { basename, dirname, join, resolve, sep } from "node:path";
import {
  type BodyStatus,
  type BodyTally,
  type SpanSummary,
  addImportSide,
  addNativeSide,
  boundBody,
  boundCoverage,
  isCPUTime,
  loadText,
  percentText,
  processSpans,
  ratioText,
  readBenchReport,
  smallestBodyCandidates,
  spansByCopy,
  summarize,
} from "./bench-report.ts";
import { type Build, Launcher, type ProcessIO, benchEnvironment, readText, tail } from "./build.ts";
import { type Attempt, ResultsLog, type RunnerOptions, checkStatuses } from "./runs.ts";
import {
  type CommandContext,
  type Role,
  type Script,
  type SequencePaths,
  MalformedScript,
  addresses,
  collectScripts,
  configuresJITCache,
  corpusRoot,
  displayPath,
  endsBySignal,
  expectedExit,
  exitProblem,
  flagName,
  jscArguments,
  lanes,
  parseScript,
} from "./scripts.ts";

class BenchError extends Error {}

// The Consumer runs whose bodies enter the coverage: those whose options leave both sides' work as the bench measures it,
// so none beyond JITCache's own flags, --useConcurrentJIT and --diskCachePath. --verboseOSR, for one, logs inside the
// native finalize span, and --collectContinuously adds collections to both sides.
export const coversRun = (options: string[]) =>
  options.every(option => option.startsWith("--jitcache") || ["--useConcurrentJIT", "--diskCachePath"].includes(flagName(option)));

// build.ts's lock, which every build holds while it runs.
const buildLockPath = () => resolve(process.env.XDG_CACHE_HOME || join(homedir(), ".cache"), "jitcache", "build.lock");

interface HeldLock {
  readonly lost: boolean; // the holder exited before the release, and the lock with it
  release(): Promise<void>;
}

// The bench holds the build lock for its whole duration, so no build or other bench overlaps it. A flock process holds
// it, as build.ts takes it, until the runner closes that process's standard input or exits.
async function holdBuildLock(path: string): Promise<HeldLock> {
  mkdirSync(dirname(path), { recursive: true });
  for (const blocking of [false, true]) {
    if (blocking) console.log(`waiting for the build lock ${path}, which a build or another bench holds`);
    const holder = spawn("flock", ["--exclusive", ...(blocking ? [] : ["--nonblock"]), path, "sh", "-c", "echo held && exec cat >/dev/null"], { stdio: ["pipe", "pipe", "inherit"] });
    let releasing = false;
    let lost = false;
    const closed = new Promise<void>(done => holder.on("close", () => {
      if (!releasing) lost = true;
      done();
    }));
    const held = await new Promise<boolean>(done => {
      let output = "";
      holder.stdout!.on("data", chunk => {
        output += chunk;
        if (output.includes("held\n")) done(true);
      });
      holder.on("error", () => done(false));
      holder.on("close", () => done(false));
    });
    if (!held) continue;
    return {
      get lost() {
        return lost;
      },
      async release() {
        releasing = true;
        holder.stdin!.end();
        await closed;
      },
    };
  }
  throw new BenchError(`flock could not take the build lock ${path}`);
}

// Whether the build's times mean anything: a release build without ASan or the twins checks, as release-local is.
function benchProfile(build: Build): { name: string; buildType: string; warning: string | null } {
  const cache = readText(join(build.directory, "CMakeCache.txt")) ?? "";
  const buildType = /^CMAKE_BUILD_TYPE:[A-Z]+=(.*)$/m.exec(cache)?.[1]?.trim() || "unknown";
  const name = basename(resolve(build.directory, "../.."));
  const reasons = [
    buildType === "Debug" || buildType === "unknown" ? `of build type ${buildType}` : null,
    build.sanitized ? "built with ASan" : null,
    build.twins ? "a twins build" : null,
  ].filter(Boolean);
  const warning = reasons.length ? `${build.directory} is ${reasons.join(", ")}, so its bench numbers mean nothing; the bench measures a release-local build` : null;
  return { name, buildType, warning };
}

// The plain-mode command line with strict at its default, off, so the runner's --jitcache-strict=1 goes, and with the
// bench report right after --jitcache-log. The run's own options still come last.
export function benchArguments(context: CommandContext, script: Script, sequence: number, role: Role, options: string[], paths: SequencePaths, name: string, report: string): string[] {
  const args = jscArguments(context, script, sequence, role, options, paths, name);
  if (!configuresJITCache(role)) return args;
  const strict = args.indexOf("--jitcache-strict=1");
  if (strict < 0 || args[strict + 1] !== "--jitcache-log") throw new Error("jscArguments no longer writes --jitcache-strict=1 --jitcache-log");
  args.splice(strict, 2, "--jitcache-log", `--jitcache-bench-report=${report}`);
  return args;
}

interface RunProblem {
  copy: number;
  problem: string;
}

type Side = "import" | "native";

// One run of a sequence on one side over the bench's copies. The import side is the run as its sequence gives it; a
// Consumer run that jitcache-expect-no-install does not name also has a native side, its native copies.
interface RunMeasures {
  script: Script;
  sequence: number;
  run: number;
  side: Side;
  spans: (Map<string, number> | null)[]; // by copy, null for a copy that wrote no bench report or was dropped
  wallMilliseconds: (number | null)[];
  problems: RunProblem[];
  dropped: number; // the copies a problem in this run, or earlier in its copy, kept out
}

interface RunBound {
  script: Script;
  sequence: number;
  run: number;
  options: string[];
  covered: boolean; // coversRun: the run's bodies enter the coverage
  tallies: Map<string, BodyTally>;
  keylessCompiles: number;
  nativeNanoseconds: (number | null)[]; // by copy, every compile's time in the native copy
}

// A Consumer run that may install has native copies; one jitcache-expect-no-install names has nothing to join.
const hasNativeCopies = (script: Script, k: number, i: number) =>
  script.sequences[k][i].role === "Consumer" && !script.expectNoInstall.some(at => addresses(at, k, i));

// One invocation of the bench mode. Its command context is plain mode's, without --extra-options or the calibration's
// variable, and it gathers every run's measures and every Consumer run's bound by copy.
class Bench implements CommandContext {
  readonly twins = false;
  readonly extraOptions: string[] = [];
  readonly mallocForceEnabled = false;
  readonly launcher: Launcher;
  readonly base: string; // the bench's temporary directory
  readonly results: ResultsLog;
  readonly nativeArtifact: string;
  readonly runs = new Map<string, RunMeasures>();
  readonly bounds = new Map<string, RunBound>();
  readonly listed: string[] = [];
  readonly malformed: string[] = [];
  processes = 0;
  kept = false;

  constructor(readonly options: RunnerOptions, readonly build: Build) {
    this.launcher = new Launcher(build, options.timeoutSeconds, "max");
    this.base = mkdtempSync(join(tmpdir(), "jitcache-bench-"));
    this.results = new ResultsLog(build.resultsDirectory, this.base, "bench-");
    this.nativeArtifact = join(this.base, "native", "artifact");
  }

  measures(script: Script, sequence: number, run: number, side: Side): RunMeasures {
    const key = `${script.path}\0${sequence}\0${run}\0${side}`;
    let measures = this.runs.get(key);
    if (!measures) {
      const copies = this.options.benchProcesses;
      measures = { script, sequence, run, side, spans: Array(copies).fill(null), wallMilliseconds: Array(copies).fill(null), problems: [], dropped: 0 };
      this.runs.set(key, measures);
    }
    return measures;
  }

  bound(script: Script, sequence: number, run: number): RunBound {
    const key = `${script.path}\0${sequence}\0${run}`;
    let bound = this.bounds.get(key);
    if (!bound) {
      const options = script.sequences[sequence][run].options;
      bound = { script, sequence, run, options, covered: coversRun(options), tallies: new Map(), keylessCompiles: 0, nativeNanoseconds: Array(this.options.benchProcesses).fill(null) };
      this.bounds.set(key, bound);
    }
    return bound;
  }

  // Every run's sides of a script, in the order a copy runs them, each with its measures, so every run has a line even
  // when no copy reached it.
  sides(script: Script, sequence: number): { run: number; side: Side; measures: RunMeasures }[] {
    return script.sequences[sequence].flatMap((_, run) => (hasNativeCopies(script, sequence, run) ? (["import", "native"] as Side[]) : (["import"] as Side[]))
      .map(side => ({ run, side, measures: this.measures(script, sequence, run, side) })));
  }
}

// The scripts plain mode runs, jsc-hosted. Every other script is listed, and a malformed one counts against the bench.
function benchScripts(bench: Bench): Script[] {
  const { options } = bench;
  const paths = options.testPaths.length ? options.testPaths : lanes.map(lane => join(corpusRoot, lane));
  const scripts: Script[] = [];
  for (const path of collectScripts(paths)) {
    const display = displayPath(path);
    if (options.filter && !options.filter.test(display)) continue;
    let script: Script;
    try {
      script = parseScript(path);
    } catch (error) {
      if (!(error instanceof MalformedScript)) throw error;
      bench.malformed.push(`${display}: ${error.message}`);
      bench.results.write({ kind: "bench-script", script: display, outcome: "malformed", reason: error.message });
      continue;
    }
    const skip = script.requiresTwins ? "requires twins"
      : script.host === "bun" ? "Bun-hosted, and the bench measures jsc-hosted scripts only; the macro benches measure Bun"
      : script.qemuCpu.length ? "declares jitcache-qemu-cpu, so it runs only under QEMU"
      : null;
    if (skip) {
      bench.listed.push(`${display}: ${skip}`);
      bench.results.write({ kind: "bench-script", script: display, outcome: "skip", reason: skip });
      continue;
    }
    scripts.push(script);
  }
  return scripts;
}

// The native copies' artifact: a Producer run of an empty script, which compiles nothing and so commits no body. Every
// native copy opens it as a Consumer and checks that it still indexes no body.
async function createNativeArtifact(bench: Bench) {
  const directory = dirname(bench.nativeArtifact);
  mkdirSync(bench.nativeArtifact, { recursive: true });
  const report = join(directory, "producer.bench");
  const io: ProcessIO = { env: benchEnvironment(true), cwd: directory, stdout: join(directory, "producer.stdout"), stderr: join(directory, "producer.stderr") };
  const args = ["--destroy-vm", `--jitcache=${bench.nativeArtifact}`, "--jitcache-mode=p", "--jitcache-log", `--jitcache-bench-report=${report}`, "-e", ""];
  const end = await bench.launcher.engine(bench.build.jsc, args, io, null);
  ++bench.processes;
  const start = readBenchReport(report).events.find(event => event.event === "start");
  const exit = exitProblem(end, 0);
  if (exit || start?.outcome !== "started" || start.role !== "producer") {
    bench.kept = true;
    throw new BenchError(`the Producer that creates the native copies' artifact ${exit ?? `started ${start ? `${start.outcome} as ${start.role}` : "without a start event"}`}: ${tail(readText(io.stderr))}`);
  }
}

// One copy of a sequence, in a fresh directory: each run in order, and right after a Consumer run with native copies
// its native copy, which starts from a copy of the scratch directory the run started from, so it reads what the run
// read, such as the shell's bytecode cache, and writes nothing the sequence's later runs read. The copy stops at its
// first problem: what follows builds on a run that failed, so the copy contributes nothing from that run on.
async function runBenchCopy(bench: Bench, script: Script, k: number, copy: number) {
  const directory = mkdtempSync(join(bench.base, `${script.display.replace(/[^\w.-]+/g, "_")}.s${k}.c${copy}-`));
  const paths: SequencePaths = { artifact: join(directory, "artifact"), scratch: join(directory, "scratch"), layouts: [] };
  mkdirSync(paths.artifact);
  mkdirSync(paths.scratch);
  let stopped = false;
  for (const { run, side, measures } of bench.sides(script, k)) {
    if (stopped) {
      ++measures.dropped;
      continue;
    }
    const native: SequencePaths = { artifact: bench.nativeArtifact, scratch: join(directory, `native${run}`), layouts: [] };
    if (side === "import" && hasNativeCopies(script, k, run)) cpSync(paths.scratch, native.scratch, { recursive: true });
    if (!(await benchRun(bench, measures, copy, side === "import" ? paths : native))) {
      ++measures.dropped;
      stopped = true;
    }
  }
  if (stopped) {
    bench.kept = true;
    console.log(`  kept ${directory}`);
  } else {
    rmSync(directory, { recursive: true, force: true });
  }
}

// One process of a copy; returns whether it passed its checks, and only then records its spans and bound samples. Its
// checks are the test runs' exit and final-status checks, without the install check for a native copy, whose artifact
// holds no body, plus the bench report's own: a start event in every run with JITCache and, in a native copy, a Consumer
// start on an artifact that indexes no body.
async function benchRun(bench: Bench, measures: RunMeasures, copy: number, paths: SequencePaths): Promise<boolean> {
  const { script, sequence: k, run: i, side } = measures;
  const run = script.sequences[k][i];
  const name = `run${i}`;
  const report = join(paths.scratch, `${name}.bench`);
  const expected = expectedExit(script, k, i);
  const io: ProcessIO = { env: benchEnvironment(!endsBySignal(expected)), cwd: paths.scratch, stdout: join(paths.scratch, `${name}.stdout`), stderr: join(paths.scratch, `${name}.stderr`) };
  const args = run.role === "Maintenance"
    ? ["--jitcache-maintenance", "--", ...run.options.map(word => word.replaceAll("$ARTIFACT", paths.artifact))]
    : benchArguments(bench, script, k, run.role, run.options, paths, name, report);
  const started = performance.now();
  const end = await bench.launcher.engine(bench.build.jsc, args, io, null);
  const wallMilliseconds = performance.now() - started;
  ++bench.processes;

  const problems: string[] = [];
  const exit = exitProblem(end, expected);
  if (exit) problems.push(exit);
  const { events, unreadable } = configuresJITCache(run.role) ? readBenchReport(report) : { events: [], unreadable: 0 };
  if (configuresJITCache(run.role)) {
    if (unreadable) problems.push(`${unreadable} lines of its bench report are no event`);
    const start = events.find(event => event.event === "start");
    if (!start && !endsBySignal(expected)) problems.push("its bench report holds no start event");
    if (start && side === "native" && (start.outcome !== "started" || start.role !== "consumer" || start.indexedBodies !== 0))
      problems.push(`the native copy started ${start.outcome} as ${start.role} on an artifact of ${start.indexedBodies} bodies, where it must start as a consumer of an empty one`);
    if (!endsBySignal(expected)) {
      const checked = side === "native" ? { ...script, expectNoInstall: [...script.expectNoInstall, { sequence: k, run: i }] } : script;
      const attempt: Attempt = { sequence: k, attempt: 1, directory: paths.scratch, failures: [], marks: [], notes: [], faults: new Map() };
      checkStatuses(checked, k, i, run, readText(io.stderr) ?? "", attempt, (_, check, detail) => problems.push(`${check}: ${detail}`));
    }
  }
  if (problems.length) {
    measures.problems.push(...problems.map(problem => ({ copy, problem })));
    return false;
  }
  measures.wallMilliseconds[copy] = wallMilliseconds;
  if (events.length) measures.spans[copy] = processSpans(events);
  if (hasNativeCopies(script, k, i)) {
    const bound = bench.bound(script, k, i);
    if (side === "import") {
      addImportSide(bound.tallies, events);
    } else {
      const { keyless, nanoseconds } = addNativeSide(bound.tallies, events);
      bound.keylessCompiles += keyless;
      bound.nativeNanoseconds[copy] = nanoseconds;
    }
  }
  return true;
}

// A run's line: its CPU-time spans with their minimum and median beside every copy's value, and every other field, the
// process's memory and its wall time by copy.
function runRecord(measures: RunMeasures) {
  const spans: Record<string, SpanSummary> = {};
  const fields: Record<string, (number | null)[]> = {};
  const memory: Record<string, (number | null)[]> = {};
  for (const [name, values] of spansByCopy(measures.spans)) {
    if (name.startsWith("process.")) memory[name.slice("process.".length)] = values;
    else if (isCPUTime(name)) spans[name] = summarize(values)!;
    else fields[name] = values;
  }
  const run = measures.script.sequences[measures.sequence][measures.run];
  return {
    kind: "bench-run",
    script: measures.script.display,
    sequence: measures.sequence,
    run: measures.run,
    role: run.role,
    side: measures.side,
    options: run.options,
    copies: measures.spans.length,
    dropped: measures.dropped,
    spans,
    fields,
    memory,
    wallMilliseconds: measures.wallMilliseconds,
    problems: measures.problems,
  };
}

// The whole native cost of a run, the least over its native copies of every compile's time; 0 without a native copy.
const wholeNativeCost = (bound: RunBound) => {
  const totals = bound.nativeNanoseconds.filter((value): value is number => value !== null);
  return totals.length ? Math.min(...totals) : 0;
};

// The bench mode: every copy of every sequence, then the results file's lines and the summary. Returns the exit code.
export async function runBench(options: RunnerOptions, build: Build): Promise<number> {
  const bench = new Bench(options, build);
  const profile = benchProfile(build);
  const copies = options.benchProcesses;
  const started = performance.now();
  console.log(`bench of ${build.directory} (${profile.name}, ${profile.buildType}): ${copies} copies of each sequence`);
  if (profile.warning) console.log(`warning: ${profile.warning}`);
  bench.results.write({
    kind: "bench",
    date: new Date().toISOString(),
    build: { directory: build.directory, profile: profile.name, buildType: profile.buildType, architecture: build.architecture, twins: build.twins, sanitized: build.sanitized },
    machine: { cpus: cpus().length, model: cpus()[0]?.model ?? null, memoryBytes: totalmem(), kernel: release() },
    copies,
    warning: profile.warning,
  });
  const scripts = benchScripts(bench);
  const loadAverage: { before: number[] | null; copies: number[][]; after: number[] | null } = { before: null, copies: [], after: null };
  let error: string | null = null;
  let lock: HeldLock | null = null;
  let lockLost = false;
  try {
    if (!scripts.length) throw new BenchError("no script of the test paths runs in plain mode in jsc");
    lock = await holdBuildLock(buildLockPath());
    loadAverage.before = loadavg();
    await createNativeArtifact(bench);
    const sequences = scripts.reduce((sum, script) => sum + script.sequences.length, 0);
    console.log(`${scripts.length} scripts, ${sequences} sequences, under the build lock; load average ${loadText(loadAverage.before)}`);
    // Copy n of every sequence runs before copy n + 1 of any, so a slow spell of the machine reaches few copies of each
    // case and the minimum holds.
    for (let copy = 0; copy < copies; ++copy) {
      const copyStarted = performance.now();
      for (const script of scripts) {
        for (let k = 0; k < script.sequences.length; ++k) await runBenchCopy(bench, script, k, copy);
      }
      loadAverage.copies.push(loadavg());
      console.log(`copy ${copy + 1} of ${copies}: ${((performance.now() - copyStarted) / 1000).toFixed(0)} s, load average ${loadText(loadAverage.copies[copy])}`);
    }
    loadAverage.after = loadavg();
  } catch (caught) {
    if (!(caught instanceof BenchError)) throw caught;
    error = caught.message;
  } finally {
    // A holder that exited before the release took the lock with it, so a build may have overlapped the bench.
    if (lock) {
      lockLost = lock.lost;
      await lock.release();
    }
  }

  // A script counts as measured once a process of it ran, so a bench that stopped before its copies measured none.
  const measured = new Map<Script, number>();
  for (const measures of bench.runs.values()) measured.set(measures.script, (measured.get(measures.script) ?? 0) + measures.problems.length);
  for (const [script, problems] of measured) bench.results.write({ kind: "bench-script", script: script.display, outcome: "measured", sequences: script.sequences.length, problems });
  const records = [...bench.runs.values()].map(runRecord);
  for (const record of records) bench.results.write(record);
  const bodies = [...bench.bounds.values()].flatMap(bound => [...bound.tallies.values()].map(tally => ({ bound, body: boundBody(tally) })));
  for (const { bound, body } of bodies) {
    bench.results.write({ kind: "bound-body", script: bound.script.display, sequence: bound.sequence, run: bound.run, options: bound.options, inCoverage: bound.covered, ...body });
  }
  // Coverage over every script and over each, from the runs coversRun admits.
  const covered = [...bench.bounds.values()].filter(bound => bound.covered);
  const scopes: [string | null, RunBound[]][] = [[null, covered], ...[...new Set(covered.map(bound => bound.script))].map(script => [script.display, covered.filter(bound => bound.script === script)] as [string, RunBound[]])];
  const coverage = scopes.flatMap(([script, bounds]) => {
    const scopeBodies = bodies.filter(({ bound }) => bounds.includes(bound)).map(({ body }) => body);
    const whole = bounds.reduce((sum, bound) => sum + wholeNativeCost(bound), 0);
    return smallestBodyCandidates.map(smallestBody => boundCoverage(scopeBodies, whole, smallestBody, script));
  });
  for (const row of coverage) bench.results.write({ kind: "bound-coverage", ...row });
  const statuses = Object.fromEntries((["joined", "set aside", "native only", "import only"] as BodyStatus[]).map(status => [status, bodies.filter(({ body }) => body.status === status).length]));
  const problemRuns = records.filter(record => record.problems.length).length;
  const exit = error || lockLost || bench.malformed.length || problemRuns ? 1 : 0;
  const kept = bench.kept || bench.results.path.startsWith(`${bench.base}${sep}`);
  bench.results.write({
    kind: "bench-summary",
    copies,
    scripts: { measured: measured.size, skipped: bench.listed.length, malformed: bench.malformed.length },
    runs: records.length,
    processes: bench.processes,
    problemRuns,
    droppedCopies: records.reduce((sum, record) => sum + record.dropped, 0),
    bodies: statuses,
    keylessCompiles: [...bench.bounds.values()].reduce((sum, bound) => sum + bound.keylessCompiles, 0),
    listed: bench.listed,
    malformed: bench.malformed,
    loadAverage,
    lockLost,
    seconds: Math.round((performance.now() - started) / 1000),
    kept: kept ? bench.base : null,
    error,
    exit,
  });

  for (const [script, problems] of measured) {
    if (!problems) {
      console.log(`MEASURED ${script.display}${script.sequences.length > 1 ? ` (${script.sequences.length} sequences)` : ""}`);
      continue;
    }
    console.log(`PROBLEMS ${script.display}`);
    for (const record of records.filter(record => record.script === script.display && record.problems.length)) {
      const copiesOf = new Map<string, number[]>();
      for (const { copy, problem } of record.problems) copiesOf.set(problem, [...(copiesOf.get(problem) ?? []), copy]);
      for (const [problem, where] of copiesOf) {
        console.log(`  sequence ${record.sequence}, run ${record.run} (${record.role}${record.side === "native" ? ", native copy" : ""}), copies ${where.join(", ")}: ${problem}`);
      }
    }
  }
  for (const line of bench.malformed) console.log(`MALFORMED ${line}`);
  for (const line of bench.listed) console.log(`listed: ${line}`);
  if (error) console.log(`ERROR ${error}`);
  if (lockLost) console.log("ERROR the build lock's holder exited before the bench released it, so a build may have overlapped the bench");
  if (bodies.length) {
    console.log(`installation bound: ${Object.entries(statuses).map(([status, count]) => `${count} ${status}`).join(", ")}; coverage over every script, per script in the results:`);
    for (const row of coverage.filter(row => row.script === null)) {
      console.log(`  S ${String(row.smallestBody).padStart(4)}: ${String(row.covered).padStart(5)} bodies, ${percentText(row.nativeShare).padStart(6)} of the native cost, ${percentText(row.excludedShare).padStart(6)} below S, ratio p50 ${ratioText(row.p50)}, p99 ${ratioText(row.p99)}, max ${ratioText(row.max)}`);
    }
  }
  const dropped = records.reduce((sum, record) => sum + record.dropped, 0);
  console.log(`${measured.size} scripts measured in ${copies} copies, ${problemRuns} runs with problems, ${dropped} run copies dropped, ${bench.listed.length} listed, ${bench.malformed.length} malformed`);
  console.log(`load average ${loadText(loadAverage.before)} before, ${loadText(loadAverage.after)} after`);
  if (profile.warning) console.log(`warning: ${profile.warning}`);
  if (kept) console.log(`kept: ${bench.base}`);
  else rmSync(bench.base, { recursive: true, force: true });
  console.log(`results: ${bench.results.path}`);
  return exit;
}
