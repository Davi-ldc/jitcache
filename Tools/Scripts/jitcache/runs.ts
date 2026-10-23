// One invocation of the runner over the corpus and testjitcache. The Session holds what every sequence shares: the
// mode, the launchers, the twins-mode calibration, the results file, the job pool and the failures counted so far. Each
// sequence of a script that its cadence lets run runs as an attempt in a fresh directory: its runs in order, each
// checked after it ends (exit, checkers, final statuses, twin reports), then its oracle and, with --pin, the pin
// comparison. A sequence whose only failure is a heap coincidence runs once more. cpp.ts runs testjitcache's tests in
// the same session.

import { existsSync, mkdirSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, extname, join, sep } from "node:path";
import { repositoryPath } from "./env.ts";
import {
  type Build,
  Launcher,
  type ProcessEnd,
  type ProcessIO,
  StackSizes,
  UsageError,
  describeEnd,
  guestStackSizes,
  keepingExitStatus,
  loadBuild,
  readBytes,
  readText,
  tail,
} from "./launcher.ts";
import { describeLeaks, firstDifference, leakedObjects, leaksBeyond, oracleOptionSets } from "./oracle.ts";
import { type PinSetup, comparePinOptionSet } from "./pin.ts";
import { ResultsLog } from "./results.ts";
import {
  type CommandContext,
  type Role,
  type RunSpec,
  type Script,
  type SequencePaths,
  type TwinKind,
  addresses,
  bunInvocation,
  bunOptionProblem,
  concurrentJITOff,
  configuresJITCache,
  displayPath,
  endsBySignal,
  engineEnvironment,
  expectedExit,
  exitProblem,
  jscArguments,
  produces,
  selectScripts,
  selectSequences,
} from "./scripts.ts";
import { type Fail, checkStatuses } from "./statuses.ts";

// The runner's command line, which run-jitcache-tests parses; the bench mode and the ladder read it too.
export interface RunnerOptions {
  build: string;
  bun: string | null;
  mode: "twins" | "plain" | null; // null: twins exactly when the build has twins
  pin: string | null; // the pin's WebKit build directory
  pinOptions: string[]; // options this build's side of the pin comparison alone takes
  extraOptions: string[];
  filter: RegExp | null;
  jobs: number;
  timeoutSeconds: number;
  jsOnly: boolean;
  cppOnly: boolean;
  qemuCpu: string | null;
  testPaths: string[];
  milestone: boolean; // --milestone: the scripts and sequences a milestone cadence marks run too
  bench: boolean; // --bench: measure instead of testing
  benchProcesses: number; // its copies of each sequence
  benchBreakdowns: boolean; // --bench-breakdowns: every bench process runs with jitCacheBenchBreakdowns
  ladder: boolean; // --ladder: the whole verification of a twins build
  plainBuild: string | null; // the plain WebKit build directory of the ladder's plain-mode rung
}

export class CalibrationError extends Error {}

// ---------------------------------------------------------------------------------------------------------------------
// Results

export interface Failure {
  sequence: number | null; // null for the script as a whole
  attempt: number | null;
  run: number | null;
  role: Role | null;
  check: string;
  detail: string;
}

// A failure the session counted, as the ladder lists it: the file it names, from the repository's root or absolute
// outside it, or null when it names none, and what failed.
export interface ReportedFailure {
  file: string | null;
  detail: string;
}

class Pool {
  private running = 0;
  private readonly waiting: (() => void)[] = [];

  constructor(private readonly size: number) {}

  async run<T>(work: () => Promise<T>): Promise<T> {
    if (this.running >= this.size) await new Promise<void>(resume => this.waiting.push(resume));
    ++this.running;
    try {
      return await work();
    } finally {
      --this.running;
      this.waiting.shift()?.();
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// The session: one invocation of the runner

export class Session implements CommandContext {
  readonly mode: "twins" | "plain";
  readonly launcher: Launcher;
  readonly pinSetup: PinSetup | null;
  readonly results: ResultsLog;
  readonly base: string; // the invocation's temporary directory
  readonly pool: Pool;
  mallocForceEnabled = false;
  private calibrated = false;
  readonly listed: string[] = [];
  readonly failures: ReportedFailure[] = []; // each failure behind the failed count, with the file it names
  passed = 0;
  failed = 0;
  skipped = 0;
  awaitingMilestone = 0; // the sequences that did not run for want of --milestone alone
  private keepBase = false;

  constructor(readonly options: RunnerOptions, readonly build: Build) {
    if (options.mode === "twins" && !build.twins) throw new UsageError(`--mode=twins needs a twins build, and ${build.directory} is none`);
    if (options.qemuCpu !== null && !build.emulated) throw new UsageError("--qemu-cpu applies only to an aarch64 build on an x86_64 host");
    this.mode = options.mode ?? (build.twins ? "twins" : "plain");
    this.launcher = new Launcher(build, options.timeoutSeconds, options.qemuCpu ?? "max");
    this.pinSetup = null;
    if (options.pin) {
      // The pin runs on the same machine as this build, so both see one CPU feature vector, and under the same
      // emulation when this build's architecture needs it.
      const pin = loadBuild(options.pin);
      if (pin.architecture !== build.architecture) throw new UsageError(`--pin names a ${pin.architecture} build, and ${build.directory} is ${build.architecture}`);
      const pinLauncher = new Launcher(pin, options.timeoutSeconds, options.qemuCpu ?? "max");
      this.pinSetup = { context: this, build, launcher: this.launcher, pin, pinLauncher, pinOptions: options.pinOptions };
    }
    this.base = mkdtempSync(join(tmpdir(), "jitcache-tests-"));
    this.results = new ResultsLog(build.resultsDirectory, this.base);
    this.pool = new Pool(options.jobs);
  }

  get twins() {
    return this.mode === "twins";
  }

  get extraOptions() {
    return this.options.extraOptions;
  }

  // Before the first twins-mode sequence, checks that the heap moves between processes, and where it stays put, hands
  // the JSC heap to libpas, whose pages the kernel places.
  async calibrate() {
    if (!this.twins || this.calibrated) return;
    this.calibrated = true;
    const probeNames = ["vm", "cell", "atom"];
    for (const forced of [false, true]) {
      const directory = mkdtempSync(join(this.base, "calibration-"));
      const stacks = new StackSizes();
      const env = forced ? { ...process.env, WebKitMallocForceEnabled: "1" } : { ...process.env };
      const probes: string[][] = [];
      for (let i = 0; i < 2; ++i) {
        const layout = join(directory, `calibration${i}.layout`);
        const io = { env, cwd: directory, stdout: join(directory, `calibration${i}.stdout`), stderr: join(directory, `calibration${i}.stderr`) };
        const args = ["--useDollarVM=true", `--jitcache-twins-record-layout=${layout}`, ...this.options.extraOptions, "-e", ""];
        const end = await this.launcher.engine(this.build.jsc, args, io, stacks);
        // Only the layout matters: a process without --destroy-vm may still end on a leak report.
        const heap = /^heap (\S+) (\S+) (\S+)$/m.exec(readText(layout) ?? "");
        if (!heap) throw new CalibrationError(`calibration process ${i} wrote no heap line (${describeEnd(end)}): ${tail(readText(io.stderr))}`);
        probes.push(heap.slice(1));
      }
      const repeated = probeNames.filter((_, p) => probes[0][p] === probes[1][p]);
      this.results.write({ kind: "calibration", webKitMallocForceEnabled: forced, probes, repeated });
      if (!repeated.length) {
        this.mallocForceEnabled = forced;
        console.log(forced
          ? "calibration: the build's allocator keeps the heap at one base; every twins-mode process gets WebKitMallocForceEnabled=1"
          : "calibration: the heap moves between processes");
        rmSync(directory, { recursive: true, force: true });
        return;
      }
      console.log(`calibration${forced ? " with WebKitMallocForceEnabled=1" : ""}: the ${repeated.join(", ")} probe repeats between two processes`);
    }
    throw new CalibrationError("the heap domain does not move between processes, even with WebKitMallocForceEnabled=1, so no twin check could tell a heap target the image recorded from one it resolves");
  }

  keep() {
    this.keepBase = true;
  }

  // Prints the summary, writes it to the results and returns the exit code.
  finish(error: string | null): number {
    if (error) {
      console.log(`ERROR ${error}`);
      this.failed++;
    }
    for (const line of this.listed) console.log(`listed: ${line}`);
    const exit = this.failed ? 1 : 0;
    this.results.write({ kind: "summary", passed: this.passed, failed: this.failed, skipped: this.skipped, listed: this.listed, error, exit });
    console.log(`${this.passed} passed, ${this.failed} failed, ${this.skipped} skipped`);
    // The results fall back to the temporary directory when the build root takes no file, and then outlive the run too.
    if (this.keepBase || this.results.path.startsWith(`${this.base}${sep}`)) console.log(`kept: ${this.base}`);
    else rmSync(this.base, { recursive: true, force: true });
    console.log(`results: ${this.results.path}`);
    return exit;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// One attempt at a sequence

interface Attempt {
  sequence: number;
  attempt: number;
  directory: string;
  failures: Failure[];
  marks: Failure[]; // heap coincidences, which repeat the sequence when they are its only failures
  notes: string[]; // what the runner lists and goes on
  faults: Map<number, Set<string>>; // the fault steps each run's final statuses report
}

interface TwinEntry {
  kind: string;
  part?: string;
  domain?: string;
  check?: string;
  detail?: string;
  reason?: string;
}

const twinMatches = (entry: TwinEntry, declared: { kind: TwinKind; subject: string }) =>
  entry.kind === declared.kind && (declared.kind === "coincidence" ? entry.domain : entry.part) === declared.subject;

function cpuOf(script: Script, sequence: number, run: number): string | null {
  return script.qemuCpu.findLast(q => addresses(q.at, sequence, run))?.model ?? null;
}

async function runAttempt(session: Session, script: Script, k: number, attempt: number): Promise<Attempt> {
  const directory = mkdtempSync(join(session.base, `${script.display.replace(/[^\w.-]+/g, "_")}.s${k}.a${attempt}-`));
  const paths: SequencePaths = { artifact: join(directory, "artifact"), scratch: join(directory, "scratch"), layouts: [] };
  mkdirSync(paths.artifact);
  mkdirSync(paths.scratch);
  const { scratch } = paths;
  const runs = script.sequences[k];
  const stacks = new StackSizes();
  const result: Attempt = { sequence: k, attempt, directory, failures: [], marks: [], notes: [], faults: new Map() };
  const fail = (run: number | null, check: string, detail: string) =>
    result.failures.push({ sequence: k, attempt, run, role: run === null ? null : runs[run].role, check, detail });

  // Under QEMU every process of the sequence, its oracle runs and pin comparisons included, needs a guest stack size of
  // its own.
  const processes = runs.length + oracleOptionSets(runs).length * (comparesWithPin(session, script) ? 3 : 1);
  if (session.build.emulated && processes > guestStackSizes) {
    fail(null, "qemu-stack", `the sequence starts ${processes} processes, and only ${guestStackSizes} guest stack sizes differ under QEMU`);
    return result;
  }

  for (let i = 0; i < runs.length; ++i) {
    const run = runs[i];
    const name = `run${i}`;
    const expected = expectedExit(script, k, i);
    // The runner judges the leaks of every run that reaches LeakSanitizer's check at exit. A run expected to end by a
    // signal never does, so it keeps ASan's defaults, where abort_on_error=1 would pass any sanitizer error as the abort
    // it expects.
    const judged = !endsBySignal(expected);
    const io = (env: ProcessIO["env"]): ProcessIO => ({
      env: judged ? keepingExitStatus(env) : env,
      cwd: scratch,
      stdout: join(scratch, `${name}.stdout`),
      stderr: join(scratch, `${name}.stderr`),
    });
    let end: ProcessEnd;
    if (run.role === "Maintenance") {
      // <jsc> --jitcache-maintenance -- <command>: maintenance runs before JSC::initialize and runs no JavaScript, so
      // neither the directory's defaults nor --extra-options, which vary how JSC runs, apply.
      const command = run.options.map(word => word.replaceAll("$ARTIFACT", paths.artifact));
      end = await session.launcher.engine(session.build.jsc, ["--jitcache-maintenance", "--", ...command], io(engineEnvironment(session)), stacks, cpuOf(script, k, i));
    } else if (script.host === "jsc") {
      end = await session.launcher.engine(session.build.jsc, jscArguments(session, script, k, run.role, run.options, paths, name), io(engineEnvironment(session)), stacks, cpuOf(script, k, i));
    } else {
      const { args, env } = bunInvocation(session, script, k, run.role, run.options, paths, name);
      end = await session.launcher.engine(session.options.bun!, args, io(env), stacks, cpuOf(script, k, i));
    }

    // The checks after each run.
    const exit = exitProblem(end, expected);
    if (exit) fail(i, end.timedOut ? "timeout" : "exit", exit);
    // An Off run's leaks are the engine's, and runOracle judges those of a run that configures JITCache. A Maintenance
    // run has no oracle, so every leak it reports fails it.
    if (run.role === "Maintenance" && judged) {
      const leaked = leakedObjects(readText(join(scratch, `${name}.stderr`)) ?? "");
      if (leaked.length) fail(i, "leak", `the run has no oracle, and it leaked ${describeLeaks(leaked)}`);
    }
    // A run's checks each write <run>.<checker>.stdout and .stderr, with a check's index after the checker's name when
    // the run has several.
    const checks = script.checks.filter(c => addresses(c.at, k, i));
    for (const [j, check] of checks.entries()) {
      const label = `${name}.${basename(check.checker, extname(check.checker))}${checks.length > 1 ? `.${j}` : ""}`;
      const checkerIO = { env: { ...process.env }, cwd: scratch, stdout: join(scratch, `${label}.stdout`), stderr: join(scratch, `${label}.stderr`) };
      const checked = await session.launcher.host(process.execPath, [check.checker, join(scratch, `${name}.stdout`), join(scratch, `${name}.stderr`), scratch, ...check.arguments], checkerIO);
      if (checked.error || checked.timedOut || checked.code !== 0) fail(i, "checker", `${[displayPath(check.checker), ...check.arguments].join(" ")} ${describeEnd(checked)}: ${tail(readText(checkerIO.stderr) || readText(checkerIO.stdout))}`);
    }
    if (!configuresJITCache(run.role)) continue;
    const layout = join(scratch, `${name}.layout`);
    if (session.twins && existsSync(layout)) paths.layouts.push(layout);
    // Bun writes final statuses only in twins mode, and a run that ends by a signal writes none.
    if (!endsBySignal(expected) && (script.host === "jsc" || session.twins)) result.faults.set(i, checkStatuses(script, k, i, run, readText(join(scratch, `${name}.stderr`)) ?? "", fail));
    if (session.twins) checkTwinReports(session, script, k, i, runs, scratch, attempt, result, fail);
  }

  await runOracle(session, script, k, runs, paths, stacks, result, fail);
  if (comparesWithPin(session, script)) await runPinComparison(session, script, k, attempt, runs, paths, stacks, result, fail);
  return result;
}

// A run's twin reports, the main VM's and, for Bun, each worker VM's.
function checkTwinReports(session: Session, script: Script, k: number, i: number, runs: RunSpec[], scratch: string, attempt: number, result: Attempt, fail: Fail) {
  const name = `run${i}.twins`;
  if (!existsSync(join(scratch, name))) {
    fail(i, "twin-report", "the run wrote no twin report");
    return;
  }
  const files = [name, ...readdirSync(scratch).filter(file => file.startsWith(`${name}.`)).sort()];
  const entries: TwinEntry[] = [];
  for (const file of files) {
    (readText(join(scratch, file)) ?? "").split("\n").forEach((line, index) => {
      if (!line.trim()) return;
      try {
        entries.push(JSON.parse(line));
      } catch {
        fail(i, "twin-report", `${file}:${index + 1} is no JSON line: ${line}`);
      }
    });
  }
  const declared = script.expectTwin.filter(t => addresses(t.at, k, i));
  for (const twin of declared) {
    if (!entries.some(entry => twinMatches(entry, twin))) fail(i, "twin-expected", `no ${twin.kind} report of ${twin.subject} appeared, which jitcache-expect-twin requires`);
  }
  // A skip fails the run exactly when concurrent JIT is off in it and in every earlier run that produced.
  const offIn = (j: number) => concurrentJITOff(session, script, runs[j].options);
  const skipFails = offIn(i) && runs.every((run, j) => j >= i || !produces(run.role) || offIn(j));
  for (const entry of entries) {
    if (declared.some(twin => twinMatches(entry, twin))) continue;
    if (entry.kind === "difference") fail(i, "twin-difference", `${entry.part} ${entry.check}: ${entry.detail}`);
    else if (entry.kind === "skip" && skipFails) fail(i, "twin-skip", `${entry.part} ${entry.check} skipped with concurrent JIT off in this run and every earlier producing run: ${entry.reason}`);
    else if (entry.kind === "skip") result.notes.push(`run ${i} skipped ${entry.part} ${entry.check}: ${entry.reason}`);
    else if (entry.kind === "coincidence" && entry.domain === "heap" && attempt === 1) {
      result.marks.push({ sequence: k, attempt, run: i, role: runs[i].role, check: "twin-coincidence", detail: `heap: ${entry.detail}` });
    } else if (entry.kind === "coincidence") fail(i, "twin-coincidence", `${entry.domain}: ${entry.detail}`);
    else fail(i, "twin-report", `a report of unknown kind ${entry.kind}`);
  }
}

// The oracle: every run that configures JITCache against an Off run of its own options without the JITCache ones, one
// oracle run per distinct option set.
async function runOracle(session: Session, script: Script, k: number, runs: RunSpec[], paths: SequencePaths, stacks: StackSizes, result: Attempt, fail: Fail) {
  const { scratch } = paths;
  let j = 0;
  for (const { options, runs: compared } of oracleOptionSets(runs)) {
    const name = `oracle${j}`;
    // No exit check reads an oracle run's status, so every oracle run takes the environment that prints its leaks.
    const io: ProcessIO = { env: keepingExitStatus(engineEnvironment(session)), cwd: scratch, stdout: join(scratch, `${name}.stdout`), stderr: join(scratch, `${name}.stderr`) };
    // An oracle run is no run a jitcache-qemu-cpu index names, so under QEMU it takes --qemu-cpu's model.
    let end: ProcessEnd;
    if (script.host === "jsc") {
      end = await session.launcher.engine(session.build.jsc, jscArguments(session, script, k, "Off", options, paths, name), io, stacks);
    } else {
      const { args, env } = bunInvocation(session, script, k, "Off", options, paths, name);
      end = await session.launcher.engine(session.options.bun!, args, { ...io, env: keepingExitStatus(env) }, stacks);
    }
    const oracleOutput = readBytes(io.stdout);
    const oracleLeaks = leakedObjects(readText(io.stderr) ?? "");
    const heapCompared = session.twins && script.host === "jsc" && !script.heapOff;
    const oracleHeap = heapCompared ? readBytes(join(scratch, `${name}.heap`)) : null;
    for (const i of compared) {
      const output = readBytes(join(scratch, `run${i}.stdout`));
      if (!output.equals(oracleOutput)) fail(i, "oracle-output", `its output differs from oracle ${j}'s (${describeEnd(end)}) ${firstDifference(output, oracleOutput)}`);
      // A run that ends by a signal never reaches the end of the run, where LeakSanitizer checks for leaks and the shell
      // writes its heap description.
      if (endsBySignal(expectedExit(script, k, i))) continue;
      // A leak passes when the oracle reports it too, the engine's own.
      const leaked = leaksBeyond(leakedObjects(readText(join(scratch, `run${i}.stderr`)) ?? ""), oracleLeaks);
      if (leaked.length) fail(i, "leak", `of the objects it leaked, oracle ${j} (${describeEnd(end)}) reports no match for ${describeLeaks(leaked)}`);
      if (!heapCompared) continue;
      const heapFile = join(scratch, `run${i}.heap`);
      if (!existsSync(heapFile)) fail(i, "oracle-heap", "the run wrote no heap description");
      else if (!existsSync(join(scratch, `${name}.heap`))) fail(i, "oracle-heap", `oracle ${j} wrote no heap description (${describeEnd(end)})`);
      else {
        const heap = readBytes(heapFile);
        if (!heap.equals(oracleHeap!)) fail(i, "oracle-heap", `its heap description differs from oracle ${j}'s ${firstDifference(heap, oracleHeap!)}`);
      }
    }
    ++j;
  }
}

const comparesWithPin = (session: Session, script: Script) => session.pinSetup !== null && script.host === "jsc" && script.pinOff === null;

// The pin comparison, after the oracle: each option set of its Off runs; the first difference fails the sequence.
async function runPinComparison(session: Session, script: Script, k: number, attempt: number, runs: RunSpec[], paths: SequencePaths, stacks: StackSizes, result: Attempt, fail: Fail) {
  let j = 0;
  for (const { options } of oracleOptionSets(runs)) {
    const comparison = await comparePinOptionSet(session.pinSetup!, script, k, options, paths, j, stacks);
    const { difference } = comparison;
    session.results.write({
      kind: "pin",
      script: script.display,
      sequence: k,
      attempt,
      optionSet: j,
      options,
      outcome: difference ? "different" : "same",
      check: difference?.check ?? null,
      detail: difference?.detail ?? null,
      allocations: comparison.allocations,
      bodies: comparison.bodies,
    });
    if (difference) {
      fail(null, difference.check, `option set ${j} (${options.join(" ") || "no options"}): ${difference.detail}`);
      return;
    }
    ++j;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// Scripts

async function runSequence(session: Session, script: Script, k: number): Promise<Attempt> {
  const first = await runAttempt(session, script, k, 1);
  const repeats = !first.failures.length && first.marks.length > 0;
  session.results.write(attemptRecord(script, first, repeats ? "repeat" : first.failures.length ? "fail" : "pass"));
  if (!repeats) return first;
  // A heap coincidence can happen once by chance; one that recurs in fresh processes is a defect.
  const second = await runAttempt(session, script, k, 2);
  session.results.write(attemptRecord(script, second, second.failures.length ? "fail" : "pass"));
  second.notes.unshift(`repeated after a heap coincidence: ${first.marks.map(mark => `run ${mark.run} ${mark.detail}`).join("; ")}`);
  if (!second.failures.length) rmSync(first.directory, { recursive: true, force: true });
  return second;
}

function attemptRecord(script: Script, attempt: Attempt, outcome: string) {
  return {
    kind: "sequence",
    script: script.display,
    sequence: attempt.sequence,
    attempt: attempt.attempt,
    outcome,
    failures: attempt.failures,
    marks: attempt.marks,
    notes: attempt.notes,
    directory: attempt.directory,
  };
}

// The sequences given are those the script's cadence lets run in this session.
async function runScript(session: Session, script: Script, sequences: number[]) {
  const attempts = await Promise.all(sequences.map(k => session.pool.run(() => runSequence(session, script, k))));
  const failures = attempts.flatMap(attempt => attempt.failures);
  // After the last sequence, every required fault was reported by some run its directive names, among the sequences
  // that ran; a fault whose runs all sit in sequences that did not run is not checked.
  for (const fault of script.faults.filter(f => f.required)) {
    if (!sequences.some(k => script.sequences[k].some((_, i) => addresses(fault.at, k, i)))) continue;
    const reported = attempts.some(attempt => [...attempt.faults].some(([i, steps]) => addresses(fault.at, attempt.sequence, i) && steps.has(fault.step)));
    if (!reported) {
      const at = `${fault.at.sequence === null ? "" : `${fault.at.sequence}:`}${fault.at.run}`;
      failures.push({ sequence: null, attempt: null, run: null, role: null, check: "required-fault", detail: `no run ${at} reported ${fault.step}` });
    }
  }
  for (const attempt of attempts) {
    if (!attempt.failures.length) rmSync(attempt.directory, { recursive: true, force: true });
    else session.keep();
  }
  session.results.write({ kind: "script", script: script.display, outcome: failures.length ? "fail" : "pass", failures });
  if (failures.length) {
    session.failed++;
    for (const failure of failures) session.failures.push({ file: repositoryPath(script.path), detail: describeFailure(failure) });
    console.log(`FAIL ${script.display}`);
    for (const failure of failures) console.log(`  ${describeFailure(failure)}`);
    for (const attempt of attempts.filter(a => a.failures.length)) console.log(`  sequence ${attempt.sequence}: ${attempt.directory}`);
  } else {
    session.passed++;
    console.log(`PASS ${script.display}${sequences.length > 1 ? ` (${sequences.length} sequences)` : ""}`);
  }
  for (const attempt of attempts) for (const note of attempt.notes) console.log(`  listed: sequence ${attempt.sequence}: ${note}`);
}

function describeFailure(failure: Failure): string {
  const where = [
    failure.sequence === null ? null : `sequence ${failure.sequence}${failure.attempt && failure.attempt > 1 ? ` (attempt ${failure.attempt})` : ""}`,
    failure.run === null ? null : `run ${failure.run} (${failure.role})`,
  ].filter(Boolean);
  return `${where.length ? `${where.join(", ")}: ` : ""}${failure.check}: ${failure.detail}`;
}

export async function runScripts(session: Session) {
  const { options } = session;
  const runnable: { script: Script; sequences: number[] }[] = [];
  let pinScripts = 0;
  const malformed = (path: string, display: string, detail: string) => {
    session.failed++;
    const failure: Failure = { sequence: null, attempt: null, run: null, role: null, check: "malformed", detail };
    session.results.write({ kind: "script", script: display, outcome: "fail", failures: [failure] });
    session.failures.push({ file: repositoryPath(path), detail: `malformed: ${detail}` });
    console.log(`FAIL ${display}\n  malformed: ${detail}`);
  };
  for (const { path, display, script } of selectScripts(options.testPaths, options.filter)) {
    if (typeof script === "string") {
      malformed(path, display, script);
      continue;
    }
    const problem = bunOptionProblem(script, options.extraOptions);
    if (problem) {
      malformed(path, display, problem);
      continue;
    }
    const cadence = selectSequences(script, { milestone: options.milestone, build: session.build });
    const skip = script.requiresTwins && !session.twins ? "requires twins"
      : script.host === "bun" && !options.bun ? "Bun-hosted, and no --bun names a Bun executable"
      : script.qemuCpu.length && !session.build.emulated ? "declares jitcache-qemu-cpu, so it runs only under QEMU"
      : cadence.reason;
    if (skip) {
      session.skipped++;
      session.listed.push(`${display}: ${skip}`);
      session.results.write({ kind: "script", script: display, outcome: "skip", reason: skip });
      if (skip === cadence.reason) session.awaitingMilestone += cadence.awaitingMilestone;
      continue;
    }
    for (const note of cadence.skipped) session.listed.push(`${display}: ${note}`);
    session.awaitingMilestone += cadence.awaitingMilestone;
    if (script.host === "jsc") {
      // A script whose Off run cannot run on the pin says why in jitcache-pin, and the comparison lists it.
      if (session.pinSetup && script.pinOff !== null) session.listed.push(`${display}: the pin comparison skips it: ${script.pinOff}`);
      else pinScripts++;
    }
    runnable.push({ script, sequences: cadence.selected });
  }
  if (!runnable.length) return;
  await session.calibrate();
  await Promise.all(runnable.map(({ script, sequences }) => runScript(session, script, sequences)));
  // The comparison of the off path with the pin's runs only with --pin. In the ladder, the twins pass runs it or lists
  // its absence, and the plain pass, which never compares, would only repeat that or contradict it.
  if (pinScripts && !session.pinSetup && (session.twins || !options.ladder)) session.listed.push(`the pin comparison of ${pinScripts} jsc-hosted scripts: no --pin names the pin's build`);
}
