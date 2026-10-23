// The runner's bench mode, --bench: it measures a release-local build instead of testing it. Every jsc-hosted script
// plain mode runs under --milestone goes through --bench-processes fresh copies of each sequence, one process at a time
// under the machine lock, which the runner holds, with the bench report on. Each run's line gives its time spans'
// minimum and median over the copies beside every copy's value, and each Consumer run that may install also runs as
// native copies, whose artifact holds no body, for the installation bound per body. --bench-breakdowns adds
// jitCacheBenchBreakdowns to every process, whose parts and steps then explain the totals they also raise.

import { cpSync, mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { cpus, loadavg, release, tmpdir, totalmem } from "node:os";
import { basename, dirname, join, resolve, sep } from "node:path";
import {
  type BodyStatus,
  type BodyTally,
  type SpanSummary,
  addImportSide,
  addNativeSide,
  boundBody,
  boundCoverage,
  isMinimumSpan,
  loadText,
  percentText,
  processSpans,
  ratioText,
  readBenchReport,
  smallestBodyCandidates,
  spansByCopy,
  summarize,
} from "./bench-report.ts";
import { type Build, Launcher, type ProcessIO, benchEnvironment, readText, tail } from "./launcher.ts";
import { ResultsLog } from "./results.ts";
import type { RunnerOptions } from "./runs.ts";
import {
  type CommandContext,
  type Script,
  type SequencePaths,
  addresses,
  configuresJITCache,
  endsBySignal,
  expectedExit,
  exitProblem,
  flagName,
  jscArguments,
  selectScripts,
  selectSequences,
} from "./scripts.ts";
import { checkStatuses } from "./statuses.ts";

class BenchError extends Error {}

// The Consumer runs whose bodies enter the coverage: those whose options leave both sides' work as the bench measures it,
// so none beyond JITCache's own flags, --useConcurrentJIT and --diskCachePath. --verboseOSR, for one, logs inside the
// native finalize span, and --collectContinuously adds collections to both sides.
export const coversRun = (options: string[]) =>
  options.every(option => option.startsWith("--jitcache") || ["--useConcurrentJIT", "--diskCachePath"].includes(flagName(option)));

// Whether the build's times mean anything: a release build without ASan or the twins checks, as release-local is.
function benchProfile(build: Build): { name: string; buildType: string; warning: string | null } {
  const { buildType } = build;
  const name = basename(resolve(build.directory, "../.."));
  const reasons = [
    buildType === "Debug" || buildType === "unknown" ? `of build type ${buildType}` : null,
    build.sanitized ? "built with ASan" : null,
    build.twins ? "a twins build" : null,
  ].filter(Boolean);
  const warning = reasons.length ? `${build.directory} is ${reasons.join(", ")}, so its bench numbers mean nothing; the bench measures a release-local build` : null;
  return { name, buildType, warning };
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
// variable, and with jitCacheBenchBreakdowns under --bench-breakdowns, which stays out of each run's own options and so
// out of coversRun. It gathers every run's measures and every Consumer run's bound by copy.
class Bench implements CommandContext {
  readonly twins = false;
  readonly extraOptions: string[];
  readonly mallocForceEnabled = false;
  readonly launcher: Launcher;
  readonly base: string; // the bench's temporary directory
  readonly results: ResultsLog;
  readonly nativeArtifact: string;
  readonly runs = new Map<string, RunMeasures>();
  readonly bounds = new Map<string, RunBound>();
  readonly listed: string[] = [];
  readonly malformed: string[] = [];
  skipped = 0; // the scripts the bench does not run
  processes = 0;
  kept = false;

  constructor(readonly options: RunnerOptions, readonly build: Build) {
    this.extraOptions = options.benchBreakdowns ? ["--jitCacheBenchBreakdowns=true"] : [];
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

// The scripts plain mode runs, jsc-hosted, each with the sequences of it that run on this build: the bench runs at
// milestones, so it runs the milestone sequences too. Every other script and sequence is listed, and a malformed script
// counts against the bench.
function benchScripts(bench: Bench): { script: Script; sequences: number[] }[] {
  const { options } = bench;
  const scripts: { script: Script; sequences: number[] }[] = [];
  for (const { display, script } of selectScripts(options.testPaths, options.filter)) {
    if (typeof script === "string") {
      bench.malformed.push(`${display}: ${script}`);
      bench.results.write({ kind: "bench-script", script: display, outcome: "malformed", reason: script });
      continue;
    }
    const cadence = selectSequences(script, { milestone: true, build: bench.build });
    const skip = script.requiresTwins ? "requires twins"
      : script.host === "bun" ? "Bun-hosted, and the bench measures jsc-hosted scripts only; the macro benches measure Bun"
      : script.qemuCpu.length ? "declares jitcache-qemu-cpu, so it runs only under QEMU"
      : cadence.reason;
    if (skip) {
      ++bench.skipped;
      bench.listed.push(`${display}: ${skip}`);
      bench.results.write({ kind: "bench-script", script: display, outcome: "skip", reason: skip });
      continue;
    }
    for (const note of cadence.skipped) bench.listed.push(`${display}: ${note}`);
    scripts.push({ script, sequences: cadence.selected });
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
  // The plain-mode command line, with the bench report right after --jitcache-log. A native copy also tells the script
  // so, since it imports nothing and a check that assumes an import would fail there.
  const args = run.role === "Maintenance"
    ? ["--jitcache-maintenance", "--", ...run.options.map(word => word.replaceAll("$ARTIFACT", paths.artifact))]
    : jscArguments(bench, script, k, run.role, run.options, paths, name, { benchReport: report, scriptArguments: side === "native" ? ["native"] : [] });
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
      checkStatuses(checked, k, i, run, readText(io.stderr) ?? "", (_, check, detail) => problems.push(`${check}: ${detail}`));
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

// A run's line: its time spans with their minimum and median beside every copy's value, and every other field, the
// process's memory and its wall time by copy.
function runRecord(measures: RunMeasures) {
  const spans: Record<string, SpanSummary> = {};
  const fields: Record<string, (number | null)[]> = {};
  const memory: Record<string, (number | null)[]> = {};
  for (const [name, values] of spansByCopy(measures.spans)) {
    if (name.startsWith("process.")) memory[name.slice("process.".length)] = values;
    else if (isMinimumSpan(name)) spans[name] = summarize(values)!;
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

const breakdownsNote = "breakdowns: every process times the counted parts of its requests and the steps of its installs, whose clock reads raise each import's cost, so this run explains a bound and sets none";

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
  if (options.benchBreakdowns) console.log(breakdownsNote);
  bench.results.write({
    kind: "bench",
    date: new Date().toISOString(),
    build: { directory: build.directory, profile: profile.name, buildType: profile.buildType, architecture: build.architecture, twins: build.twins, sanitized: build.sanitized },
    machine: { cpus: cpus().length, model: cpus()[0]?.model ?? null, memoryBytes: totalmem(), kernel: release() },
    copies,
    breakdowns: options.benchBreakdowns,
    warning: profile.warning,
  });
  const scripts = benchScripts(bench);
  const loadAverage: { before: number[] | null; copies: number[][]; after: number[] | null } = { before: null, copies: [], after: null };
  let error: string | null = null;
  try {
    if (!scripts.length) throw new BenchError("no script of the test paths runs in plain mode in jsc");
    loadAverage.before = loadavg();
    await createNativeArtifact(bench);
    const sequenceCount = scripts.reduce((sum, { sequences }) => sum + sequences.length, 0);
    console.log(`${scripts.length} scripts, ${sequenceCount} sequences, under the machine lock; load average ${loadText(loadAverage.before)}`);
    // Copy n of every sequence runs before copy n + 1 of any, so a slow spell of the machine reaches few copies of each
    // case and the minimum holds.
    for (let copy = 0; copy < copies; ++copy) {
      const copyStarted = performance.now();
      for (const { script, sequences } of scripts) {
        for (const k of sequences) await runBenchCopy(bench, script, k, copy);
      }
      loadAverage.copies.push(loadavg());
      console.log(`copy ${copy + 1} of ${copies}: ${((performance.now() - copyStarted) / 1000).toFixed(0)} s, load average ${loadText(loadAverage.copies[copy])}`);
    }
    loadAverage.after = loadavg();
  } catch (caught) {
    if (!(caught instanceof BenchError)) throw caught;
    error = caught.message;
  }

  // A script counts as measured once a process of it ran, so a bench that stopped before its copies measured none.
  const measured = new Map<Script, number>();
  const sequencesOf = new Map(scripts.map(({ script, sequences }) => [script, sequences.length]));
  for (const measures of bench.runs.values()) measured.set(measures.script, (measured.get(measures.script) ?? 0) + measures.problems.length);
  for (const [script, problems] of measured) bench.results.write({ kind: "bench-script", script: script.display, outcome: "measured", sequences: sequencesOf.get(script), problems });
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
  // Each line says whether its run timed breakdowns, whose ratios set no bound.
  for (const row of coverage) bench.results.write({ kind: "bound-coverage", breakdowns: options.benchBreakdowns, ...row });
  const statuses = Object.fromEntries((["joined", "set aside", "native only", "import only"] as BodyStatus[]).map(status => [status, bodies.filter(({ body }) => body.status === status).length]));
  const problemRuns = records.filter(record => record.problems.length).length;
  const exit = error || bench.malformed.length || problemRuns ? 1 : 0;
  const kept = bench.kept || bench.results.path.startsWith(`${bench.base}${sep}`);
  bench.results.write({
    kind: "bench-summary",
    copies,
    scripts: { measured: measured.size, skipped: bench.skipped, malformed: bench.malformed.length },
    runs: records.length,
    processes: bench.processes,
    problemRuns,
    droppedCopies: records.reduce((sum, record) => sum + record.dropped, 0),
    bodies: statuses,
    keylessCompiles: [...bench.bounds.values()].reduce((sum, bound) => sum + bound.keylessCompiles, 0),
    listed: bench.listed,
    malformed: bench.malformed,
    loadAverage,
    seconds: Math.round((performance.now() - started) / 1000),
    kept: kept ? bench.base : null,
    error,
    exit,
  });

  for (const [script, problems] of measured) {
    if (!problems) {
      const sequences = sequencesOf.get(script) ?? 0;
      console.log(`MEASURED ${script.display}${sequences > 1 ? ` (${sequences} sequences)` : ""}`);
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
  if (options.benchBreakdowns) console.log(breakdownsNote);
  if (kept) console.log(`kept: ${bench.base}`);
  else rmSync(bench.base, { recursive: true, force: true });
  console.log(`results: ${bench.results.path}`);
  return exit;
}
