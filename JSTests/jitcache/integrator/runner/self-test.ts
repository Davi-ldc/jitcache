#!/usr/bin/env bun
// The runner's self-tests (SPEC-integrator.harness.md section 13: H1 to H8). A fixture case runs
// Tools/Scripts/run-jitcache-tests on one fixture of this directory and compares the outcome the runner records in its
// results with the fixture's claim: a fixture that must pass passes, and one that must fail fails with exactly the
// failures listed, by sequence, attempt, run and check, and with no other. Other cases drive processes of their own: the
// runner's calibration with heap probes that never move (H1), testjitcache's command line (H3, and H4's C++ half), the
// determinism of the end-of-run description (H5), and the pin comparison of a build with itself (H7), with the crafted
// output pairs of pin-compare-tests.ts. Every fixture of this directory has a case.
//
//   bun JSTests/jitcache/integrator/runner/self-test.ts --build=<twins WebKit build directory>
//       [--plain-build=<plain WebKit build directory>] [--jobs=<n>] [--timeout=<seconds>] [--qemu-cpu=<model>]
//       [--filter=<regex over case names>]
//
// H2 and H7 run on each architecture by naming that architecture's builds, such as those bun build.ts twins
// --arch=aarch64 and bun build.ts --arch=aarch64 made, whose processes run under QEMU. H7's plain half needs
// --plain-build and is skipped without it.

import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  type Build,
  type CommandContext,
  Launcher,
  MalformedScript,
  type PinSetup,
  type ProcessEnd,
  type Script,
  StackSizes,
  comparePinOptionSet,
  corpusRoot,
  describeEnd,
  jscArguments,
  loadBuild,
  oracleOptionSets,
  parseScript,
  readText,
  repositoryRoot,
  runProcess,
} from "../../../../Tools/Scripts/run-jitcache-tests";
import { runCraftedPinTests } from "./pin-compare-tests.ts";

const fixtures = import.meta.dirname;
const runner = join(repositoryRoot, "Tools/Scripts/run-jitcache-tests");

interface Expected {
  sequence: number | null; // null for a failure of the script as a whole
  run: number | null;
  check: string;
  attempt?: number; // 2 for a sequence the runner repeated after a heap coincidence
}

interface FixtureCase {
  fixture: string;
  claim: string;
  expect: "pass" | Expected[];
  listed?: RegExp; // a note the runner must list, such as a skip it let pass
  wide?: boolean; // runs its many sequences with --jobs
  args?: (context: Context) => string[]; // the runner's options beyond the common ones
  detail?: RegExp; // what some expected failure's detail says
}

const fail = (sequence: number, run: number, check: string, attempt = 1): Expected => ({ sequence, run, check, attempt });
// A failure of a sequence as a whole, which the pin comparison reports.
const sequenceFail = (sequence: number, check: string): Expected => ({ sequence, run: null, check, attempt: 1 });
// H7: this build stands for the pin, and the options given reach this build's side of the comparison alone.
const againstItself = (...pinOptions: string[]) => (context: Context) => [`--pin=${context.build.directory}`, ...(pinOptions.length ? [`--pin-options=${pinOptions.join(" ")}`] : [])];

const fixtureCases: FixtureCase[] = [
  { fixture: "oracle-consumer-output.js", claim: "H1: a Consumer that prints differently from its Off run fails", expect: [fail(0, 1, "oracle-output")] },
  { fixture: "oracle-producer-output.js", claim: "H1: a Producer that prints differently fails", expect: [fail(0, 0, "oracle-output")] },
  { fixture: "oracle-consumer-producer-output.js", claim: "H1: a ConsumerProducer, followed by a Consumer, that prints differently fails", expect: [fail(0, 1, "oracle-output")] },
  { fixture: "oracle-heap.js", claim: "H1: a Consumer that leaves a different value reachable from a global binding fails, against oracle0.heap", expect: [fail(0, 1, "oracle-heap")] },
  { fixture: "oracle-heap-off.js", claim: "H1: the same passes under jitcache-heap: off", expect: "pass" },
  { fixture: "twin-difference.js", claim: "H1: a twin difference fails the run", expect: [fail(0, 1, "twin-difference")] },
  { fixture: "twin-skip.js", claim: "H1: a skip fails when concurrent JIT is off in its run and every earlier producing run", expect: [fail(0, 1, "twin-skip")] },
  { fixture: "twin-skip-concurrent-run.js", claim: "H1: a skip passes when its run has concurrent JIT on", expect: "pass", listed: /skipped integrator test-entry/ },
  { fixture: "twin-skip-concurrent-producer.js", claim: "H1: a skip passes when an earlier producing run has concurrent JIT on", expect: "pass", listed: /skipped integrator test-entry/ },
  { fixture: "twin-heap-coincidence.js", claim: "H1: a heap coincidence repeats the sequence once, and fails when it recurs", expect: [fail(0, 1, "twin-coincidence", 2)] },
  { fixture: "twin-pool-coincidence.js", claim: "H1: an executable-pool coincidence fails at once", expect: [fail(0, 1, "twin-coincidence")] },
  { fixture: "twin-engine-coincidence.js", claim: "H1: an engine-image coincidence fails at once", expect: [fail(0, 1, "twin-coincidence")] },
  { fixture: "twin-expected-coincidence.js", claim: "H1: a coincidence jitcache-expect-twin declares passes", expect: "pass" },
  { fixture: "twin-expected-missing.js", claim: "H1: a declared report that never appears fails the run", expect: [fail(0, 1, "twin-expected")] },
  { fixture: "sequence-directive.js", claim: "H1: a directive written 1:0 applies to run 0 of the second sequence only, and each run receives its sequence index", expect: "pass" },
  { fixture: "abort.js", claim: "H1: jitcache-expect-exit accepts an abort; H4: a twin-report line written before an abort() is in the file", expect: "pass" },
  { fixture: "budget-limit.js", claim: "H1: a Producer whose one-page limit ends production fails without jitcache-expect-fault", expect: [fail(0, 0, "fault")] },
  { fixture: "budget-limit-declared.js", claim: "H1: it passes with jitcache-expect-fault: 0 budget.limit", expect: "pass" },
  { fixture: "budget-limit-other-step.js", claim: "H1: it fails when the directive names another step", expect: [fail(0, 0, "fault")] },
  { fixture: "no-install.js", claim: "H1: a Consumer with no body to install fails", expect: [fail(0, 1, "install")] },
  { fixture: "no-install-declared.js", claim: "H1: it passes with jitcache-expect-no-install: 1", expect: "pass" },
  { fixture: "placement.js", claim: "H2: across 50 sequences, placement moves the executable pool, the structure reservation and the heap", expect: "pass", wide: true },
  {
    fixture: "describe-differences.js",
    claim: "H5: each of nine differences JavaScript can reach changes the end-of-run description",
    expect: Array.from({ length: 9 }, (_, sequence) => fail(sequence, 0, "oracle-heap")),
  },
  { fixture: "describe-module-binding.mjs", claim: "H5: one module binding changes the end-of-run description", expect: [fail(0, 0, "oracle-heap")] },
  { fixture: "describe-alike.js", claim: "H5: pairs alike to JavaScript describe alike", expect: "pass" },
  { fixture: "describe-explicit-roots.js", claim: "H5: descriptions of explicit roots", expect: "pass" },
  { fixture: "events-llint.js", claim: "H6: LLInt instruction counts with the baseline JIT off", expect: "pass" },
  { fixture: "events-baseline.js", claim: "H6: a compiled function adds no LLInt instruction", expect: "pass" },
  { fixture: "events-optimizing.js", claim: "H6: optimizing compiles, exits, jettisons and reoptimizations match the engine's lines", expect: "pass" },
  { fixture: "events-dead-body.js", claim: "H6: a dead body's counts stay in the dump's total", expect: "pass" },
  { fixture: "events-live-bodies.js", claim: "H6: a Producer's dump has a line for each keyed body still alive", expect: "pass" },
  { fixture: "kill-points.js", claim: "H8: each kill point ends by SIGKILL and leaves its temporaries", expect: "pass" },
  { fixture: "kill-as-abort.js", claim: "H8: jitcache-expect-exit's abort rejects a kill", expect: [fail(0, 0, "exit")] },
  { fixture: "kill-not-reached.js", claim: "H8: a kill point the run never reaches leaves exit 0, which kill rejects", expect: [fail(0, 0, "exit")] },
  {
    fixture: "pin-allocation.js",
    claim: "H7: with --pin-options=--forceGCSlowPaths=true the comparison fails at the first block whose inline allocation became a jump to its slow path",
    expect: [sequenceFail(0, "pin-code")],
    args: againstItself("--forceGCSlowPaths=true"),
    detail: /block main:\d+ \((new_object|create_this)\)/,
  },
  { fixture: "pin-output.js", claim: "H7: a script whose pin run prints something else fails the sequence", expect: [sequenceFail(0, "pin-output")], args: againstItself("--useDollarVM=false") },
  { fixture: "pin-output-declared.js", claim: "H7: it passes once it declares jitcache-pin: off", expect: "pass", args: againstItself("--useDollarVM=false") },
  { fixture: "pin-status.js", claim: "H7: a script whose pin run ends with another status fails the sequence", expect: [sequenceFail(0, "pin-status")], args: againstItself("--useDollarVM=false") },
  { fixture: "pin-status-declared.js", claim: "H7: it passes once it declares jitcache-pin: off", expect: "pass", args: againstItself("--useDollarVM=false") },
];

interface Context {
  build: Build;
  plainBuild: Build | null;
  base: string;
  jobs: number;
  timeoutSeconds: number;
  qemuCpu: string | null;
  launcher: Launcher;
}

interface Case {
  name: string;
  claim: string;
  skip?: (context: Context) => string | null; // why the case cannot run with the builds it was given
  run: (context: Context) => Promise<string[]>; // the problems, none when the case passes
}

// ---------------------------------------------------------------------------------------------------------------------
// The runner as a black box

interface RunnerOutcome {
  end: ProcessEnd;
  directory: string; // the case's own directory, which holds the runner's output
  records: any[]; // the runner's results
  resultsPath: string | null;
  kept: string | null; // the temporary directory the runner kept for its failures
  output: string;
}

async function runRunner(context: Context, label: string, args: string[]): Promise<RunnerOutcome> {
  const directory = mkdtempSync(join(context.base, `${label}-`));
  const io = { env: { ...process.env }, cwd: repositoryRoot, stdout: join(directory, "runner.stdout"), stderr: join(directory, "runner.stderr") };
  const common = [`--build=${context.build.directory}`, "--js-only", `--timeout=${context.timeoutSeconds}`];
  if (context.qemuCpu) common.push(`--qemu-cpu=${context.qemuCpu}`);
  // The runner bounds each process it starts, so the runner itself only needs a bound that never cuts a sound run short.
  const end = await runProcess(process.execPath, [runner, ...common, ...args], io, 7 * 24 * 3600);
  const output = readText(io.stdout) ?? "";
  const resultsPath = /^results: (.*)$/m.exec(output)?.[1] ?? null;
  const kept = /^kept: (.*)$/m.exec(output)?.[1] ?? null;
  const records = resultsPath && existsSync(resultsPath)
    ? readFileSync(resultsPath, "utf8").split("\n").filter(Boolean).map(line => JSON.parse(line))
    : [];
  return { end, directory, records, resultsPath, kept, output: `${output}${readText(io.stderr) ?? ""}` };
}

// Removes what a case's runner left once the case passed; a failed case keeps everything for whoever looks into it.
function cleanUp(outcome: RunnerOutcome, problems: string[]) {
  if (problems.length) {
    problems.push(`the runner's output is in ${outcome.directory}${outcome.kept ? `, its kept runs in ${outcome.kept}` : ""}`);
    return;
  }
  rmSync(outcome.directory, { recursive: true, force: true });
  if (outcome.kept) rmSync(outcome.kept, { recursive: true, force: true });
  if (outcome.resultsPath) rmSync(outcome.resultsPath, { force: true });
}

const failureKey = (failure: { sequence: number | null; attempt?: number | null; run: number | null; check: string }) =>
  `${failure.sequence === null ? "script" : `sequence ${failure.sequence}, attempt ${failure.attempt ?? 1}`}${failure.run === null ? "" : `, run ${failure.run}`}: ${failure.check}`;

function fixtureCase(spec: FixtureCase): Case {
  return {
    name: spec.fixture.replace(/\.m?js$/, ""),
    claim: spec.claim,
    async run(context) {
      const args = [`--jobs=${spec.wide ? context.jobs : 1}`, ...(spec.args?.(context) ?? []), join(fixtures, spec.fixture)];
      const outcome = await runRunner(context, spec.fixture, args);
      const problems: string[] = [];
      const script = outcome.records.find(record => record.kind === "script");
      if (!outcome.resultsPath) problems.push(`the runner wrote no results (${describeEnd(outcome.end)}):\n${outcome.output}`);
      else if (!script) problems.push(`the runner recorded no outcome for the fixture:\n${outcome.output}`);
      else if (spec.expect === "pass") {
        if (script.outcome !== "pass") problems.push(`the fixture must pass, and the runner reports ${script.outcome}: ${(script.failures ?? []).map(failureKey).join("; ") || script.reason}`);
        if (outcome.end.code !== 0) problems.push(`the runner ${describeEnd(outcome.end)}, expected 0`);
      } else {
        const actual = new Set<string>((script.failures ?? []).map(failureKey));
        const expected = new Set<string>(spec.expect.map(failureKey));
        for (const key of expected) {
          if (!actual.has(key)) problems.push(`missing the failure ${key}`);
        }
        for (const failure of script.failures ?? []) {
          if (!expected.has(failureKey(failure))) problems.push(`an unexpected failure ${failureKey(failure)}: ${failure.detail}`);
        }
        if (spec.detail && !(script.failures ?? []).some((failure: { detail: string }) => spec.detail!.test(failure.detail)))
          problems.push(`no failure's detail matches ${spec.detail}: ${(script.failures ?? []).map((failure: { detail: string }) => failure.detail).join("; ")}`);
        if (outcome.end.code !== 1) problems.push(`the runner ${describeEnd(outcome.end)}, expected 1`);
      }
      if (spec.listed && !outcome.records.some(record => record.kind === "sequence" && record.notes?.some((note: string) => spec.listed!.test(note)))) {
        problems.push(`the runner listed no note matching ${spec.listed}`);
      }
      cleanUp(outcome, problems);
      return problems;
    },
  };
}

// H1: with heap probes that never move, the runner calibrates twice, the second time with WebKitMallocForceEnabled=1,
// and stops before any sequence with an error naming the heap domain.
const calibrationCase: Case = {
  name: "calibration-fixed-heap-probes",
  claim: "H1: heap probes that never move stop the runner before any sequence, after a second calibration with WebKitMallocForceEnabled=1",
  async run(context) {
    const outcome = await runRunner(context, "calibration", ["--jobs=1", "--extra-options=--jitcache-test-fixed-heap-probes", join(fixtures, "no-install-declared.js")]);
    const problems: string[] = [];
    const calibrations = outcome.records.filter(record => record.kind === "calibration");
    const forced = calibrations.map(record => record.webKitMallocForceEnabled);
    if (forced.length !== 2 || forced[0] !== false || forced[1] !== true) problems.push(`the runner calibrated with WebKitMallocForceEnabled ${JSON.stringify(forced)}, expected [false, true]`);
    if (calibrations.some(record => !record.repeated?.length)) problems.push("a calibration found heap probes that moved, which fixed probes cannot");
    if (outcome.records.some(record => record.kind === "sequence")) problems.push("a sequence ran after the calibration failed");
    const summary = outcome.records.find(record => record.kind === "summary");
    if (!summary?.error?.includes("heap domain")) problems.push(`the runner's error does not name the heap domain: ${summary?.error}`);
    if (outcome.end.code !== 1) problems.push(`the runner ${describeEnd(outcome.end)}, expected 1`);
    cleanUp(outcome, problems);
    return problems;
  },
};

// ---------------------------------------------------------------------------------------------------------------------
// H3 and H4's C++ half: testjitcache's command line

const testjitcacheCase: Case = {
  name: "testjitcache",
  claim: "H3: --list, group options, a failing check and an unknown option; H4: twin-report lines parse as JSON",
  async run(context) {
    const problems: string[] = [];
    const directory = mkdtempSync(join(context.base, "testjitcache-"));
    let index = 0;
    // These processes belong to no sequence, so under QEMU each draws its guest stack alone, as the runner's do.
    const testjitcache = async (args: string[], env: Record<string, string | undefined> = { ...process.env }) => {
      const name = `call${index++}`;
      const io = { env, cwd: directory, stdout: join(directory, `${name}.stdout`), stderr: join(directory, `${name}.stderr`) };
      const end = await context.launcher.engine(context.build.testjitcache, args, io, null);
      return { end, stdout: readText(io.stdout) ?? "", stderr: readText(io.stderr) ?? "" };
    };

    // --list prints one name<TAB>options line per test, with names unique and options a list of --name=value options.
    const listing = await testjitcache(["--list"]);
    if (listing.end.code !== 0) problems.push(`testjitcache --list ${describeEnd(listing.end)}`);
    const listed = new Map<string, string>();
    for (const line of listing.stdout.split("\n").filter(Boolean)) {
      const match = /^([A-Za-z_]\w*)\t(.*)$/.exec(line);
      if (!match) {
        problems.push(`--list printed a line that is no name<TAB>options: ${JSON.stringify(line)}`);
        continue;
      }
      if (listed.has(match[1])) problems.push(`--list names ${match[1]} twice`);
      if (match[2] && !match[2].split(" ").every(option => /^--\w+=\S*$/.test(option))) problems.push(`${match[1]}'s options are no list of --name=value options: ${match[2]}`);
      listed.set(match[1], match[2]);
    }
    const registered: [string, string][] = [
      ["integratorHarnessReadsGroupOptions", "--useConcurrentJIT=false --numberOfGCMarkers=1"],
      ["integratorHarnessDerivesDependentOptions", "--useJIT=false"],
      ["integratorHarnessReportsAFailingCheck", ""],
      ["integratorTwinReportLinesParseAsJSON", ""],
    ];
    for (const [name, options] of registered) {
      if (listed.get(name) !== options) problems.push(`--list gives ${name} ${JSON.stringify(listed.get(name))}, expected ${JSON.stringify(options)}`);
    }

    // A test registered with options reads them back, and so does the group that turns the JIT off.
    for (const [name, options] of registered.filter(([name]) => name !== "integratorHarnessReportsAFailingCheck")) {
      const run = await testjitcache([`--group=${options}`, `--filter=${name}`]);
      if (run.end.code !== 0 || !run.stdout.includes(`PASS ${name}\n`)) problems.push(`${name} did not pass in its group (${describeEnd(run.end)}):\n${run.stdout}${run.stderr}`);
    }

    // A failing check prints FAIL, then its file, line and message, and the process exits with 1.
    const failing = await testjitcache(["--filter=integratorHarnessReportsAFailingCheck"], { ...process.env, JITCACHE_TEST_FAIL_ON_PURPOSE: "integratorHarnessReportsAFailingCheck" });
    if (failing.end.code !== 1) problems.push(`a failing check left testjitcache ${describeEnd(failing.end)}, expected 1`);
    if (!/^FAIL integratorHarnessReportsAFailingCheck\n {4}\S*IntegratorTests\.cpp:\d+: check failed: !failsOnPurpose$/m.test(failing.stdout)) {
      problems.push(`a failing check did not print its file, line and message:\n${failing.stdout}`);
    }

    // An option JSC does not know makes the group's process exit with 2.
    const unknown = await testjitcache(["--group=--jitcacheNoSuchOption=1"]);
    if (unknown.end.code !== 2) problems.push(`a group with an unknown option left testjitcache ${describeEnd(unknown.end)}, expected 2`);

    if (problems.length) problems.push(`testjitcache's output is in ${directory}`);
    else rmSync(directory, { recursive: true, force: true });
    return problems;
  },
};

// ---------------------------------------------------------------------------------------------------------------------
// H5: two Off runs of each integrator JS test, and of each description fixture here, write identical end-of-run
// descriptions. Each sequence's Off runs are the ones its oracle starts: one per distinct option set of its runs,
// without the JITCache options.

const determinismCase: Case = {
  name: "describe-determinism",
  claim: "H5: two Off runs of each integrator JS test write identical end-of-run descriptions",
  async run(context) {
    const problems: string[] = [];
    const scriptFile = (name: string) => /\.m?js$/.test(name);
    const paths = [
      ...readdirSync(join(corpusRoot, "integrator")).filter(scriptFile).map(name => join(corpusRoot, "integrator", name)),
      ...readdirSync(fixtures).filter(name => scriptFile(name) && name.startsWith("describe-")).map(name => join(fixtures, name)),
    ];
    // The command lines of a twins-mode Off run, without --extra-options and the calibration's environment.
    const commands: CommandContext = { twins: true, extraOptions: [], mallocForceEnabled: false };
    const work: (() => Promise<void>)[] = [];
    for (const path of paths) {
      let script: Script;
      try {
        script = parseScript(path);
      } catch (error) {
        if (!(error instanceof MalformedScript)) throw error;
        problems.push(`${path} is malformed: ${error.message}`);
        continue;
      }
      // A Bun-hosted script's oracle compares its output alone, and it describes roots of its own (section 7.7).
      if (script.host !== "jsc") continue;
      script.sequences.forEach((runs, k) => {
        const optionSets = new Map<string, string[]>();
        for (const run of runs) {
          if (run.role === "Maintenance") continue;
          const options = run.options.filter(option => !option.startsWith("--jitcache"));
          optionSets.set(JSON.stringify(options), options);
        }
        [...optionSets.values()].forEach((options, j) => work.push(async () => {
          const directory = mkdtempSync(join(context.base, "describe-"));
          const sequencePaths = { artifact: join(directory, "artifact"), scratch: join(directory, "scratch"), layouts: [] };
          mkdirSync(sequencePaths.artifact);
          mkdirSync(sequencePaths.scratch);
          const stacks = new StackSizes();
          const descriptions: (string | null)[] = [];
          for (const name of ["first", "second"]) {
            const io = { env: { ...process.env }, cwd: sequencePaths.scratch, stdout: join(sequencePaths.scratch, `${name}.stdout`), stderr: join(sequencePaths.scratch, `${name}.stderr`) };
            const end = await context.launcher.engine(context.build.jsc, jscArguments(commands, script, k, "Off", options, sequencePaths, name), io, stacks);
            const description = readText(join(sequencePaths.scratch, `${name}.heap`));
            if (description === null) problems.push(`${script.display}, sequence ${k}, option set ${j}: the ${name} Off run wrote no description (${describeEnd(end)})`);
            descriptions.push(description);
          }
          if (descriptions[0] !== null && descriptions[1] !== null && descriptions[0] !== descriptions[1]) {
            problems.push(`${script.display}, sequence ${k}, option set ${j}: two Off runs described their heaps differently, in ${sequencePaths.scratch}`);
            return;
          }
          if (descriptions.every(description => description !== null)) rmSync(directory, { recursive: true, force: true });
        }));
      });
    }
    await inParallel(work, context.jobs);
    return problems;
  },
};

// ---------------------------------------------------------------------------------------------------------------------
// H7: the pin comparison of a build with itself finds no difference on the runner's fixtures and the integrator corpus.

// The scripts H7 compares: this directory's fixtures and the integrator corpus's scripts.
function comparedScripts(): string[] {
  const scriptFile = (name: string) => /\.m?js$/.test(name);
  const integrator = join(corpusRoot, "integrator");
  return [
    ...readdirSync(fixtures).filter(scriptFile).map(name => join(fixtures, name)),
    ...(existsSync(integrator) ? readdirSync(integrator).filter(scriptFile).map(name => join(integrator, name)) : []),
  ];
}

// Through the runner, in the twins build: --pin names the build itself, and pinOptions reach this build's side alone. The
// fixtures fail on purpose in other ways; only the comparison's records count here.
function pinSelfCase(name: string, claim: string, pinOptions: string[]): Case {
  return {
    name,
    claim,
    async run(context) {
      const integrator = join(corpusRoot, "integrator");
      const paths = [fixtures, ...(existsSync(integrator) ? [integrator] : [])];
      const outcome = await runRunner(context, name, [`--jobs=${context.jobs}`, ...againstItself(...pinOptions)(context), ...paths]);
      const problems: string[] = [];
      const comparisons = outcome.records.filter(record => record.kind === "pin");
      if (!comparisons.length) problems.push(`the runner compared no option set with the pin (${describeEnd(outcome.end)}):\n${outcome.output}`);
      for (const record of comparisons.filter(record => record.outcome !== "same"))
        problems.push(`${record.script}, sequence ${record.sequence}, option set ${record.optionSet}: ${record.check}: ${record.detail}`);
      cleanUp(outcome, problems);
      return problems;
    },
  };
}

// In a plain build the runner skips every script of integrator/, which requires twins (harness sub-SPEC section 7.4), so
// this half drives the runner's own comparison of each Off option set directly (harness sub-SPEC section 13, H7).
const pinSelfPlainCase: Case = {
  name: "pin-self-plain",
  claim: "H7: a plain build compared with itself on the runner's fixtures and the integrator corpus finds no difference",
  skip: context => (context.plainBuild ? null : "no --plain-build names a plain build"),
  async run(context) {
    const build = context.plainBuild!;
    const launcher = new Launcher(build, context.timeoutSeconds, context.qemuCpu ?? "max");
    const commands: CommandContext = { twins: false, extraOptions: [], mallocForceEnabled: false };
    const setup: PinSetup = { context: commands, build, launcher, pin: build, pinLauncher: launcher, pinOptions: [] };
    const problems: string[] = [];
    let comparisons = 0;
    const work: (() => Promise<void>)[] = [];
    for (const path of comparedScripts()) {
      let script: Script;
      try {
        script = parseScript(path);
      } catch (error) {
        if (!(error instanceof MalformedScript)) throw error;
        problems.push(`${path} is malformed: ${error.message}`);
        continue;
      }
      if (script.host !== "jsc" || script.pinOff !== null) continue;
      script.sequences.forEach((runs, k) => {
        oracleOptionSets(runs).forEach(({ options }, j) => work.push(async () => {
          const directory = mkdtempSync(join(context.base, "pin-plain-"));
          const paths = { artifact: join(directory, "artifact"), scratch: join(directory, "scratch"), layouts: [] };
          mkdirSync(paths.artifact);
          mkdirSync(paths.scratch);
          const { difference } = await comparePinOptionSet(setup, script, k, options, paths, j, new StackSizes());
          ++comparisons;
          if (difference) problems.push(`${script.display}, sequence ${k}, option set ${j}: ${difference.check}: ${difference.detail}; its runs are in ${paths.scratch}`);
          else rmSync(directory, { recursive: true, force: true });
        }));
      });
    }
    await inParallel(work, context.jobs);
    if (!comparisons) problems.push("no script had an option set to compare");
    return problems;
  },
};

const pinCraftedCase: Case = {
  name: "pin-compare-crafted",
  claim: "H7: jitcache-pin-compare.ts reports the crafted pairs that differ and finds the same code in those that differ only in what canonical code removes",
  run: async () => runCraftedPinTests(),
};

// ---------------------------------------------------------------------------------------------------------------------

async function inParallel(work: (() => Promise<void>)[], jobs: number) {
  let next = 0;
  await Promise.all(Array.from({ length: Math.min(jobs, work.length) }, async () => {
    while (next < work.length) await work[next++]();
  }));
}

async function main(argv: string[]): Promise<number> {
  let build: Build | null = null;
  let plainBuild: Build | null = null;
  let jobs = 5;
  let timeoutSeconds = 600;
  let qemuCpu: string | null = null;
  let filter: RegExp | null = null;
  for (const argument of argv) {
    const equals = argument.indexOf("=");
    const [name, value] = equals < 0 ? [argument, ""] : [argument.slice(0, equals), argument.slice(equals + 1)];
    if (name === "--build" && value) build = loadBuild(value);
    else if (name === "--plain-build" && value) plainBuild = loadBuild(value);
    else if (name === "--jobs" && /^[1-9]\d*$/.test(value)) jobs = Number(value);
    else if (name === "--timeout" && /^[1-9]\d*$/.test(value)) timeoutSeconds = Number(value);
    else if (name === "--qemu-cpu" && value) qemuCpu = value;
    else if (name === "--filter" && value) filter = new RegExp(value);
    else {
      console.error(`self-test.ts: unknown or malformed argument ${argument}`);
      return 2;
    }
  }
  if (!build) {
    console.error("self-test.ts: --build names the twins build whose runner and harness to test");
    return 2;
  }
  if (!build.twins) {
    console.error(`self-test.ts: ${build.directory} is no twins build, and every self-test needs one`);
    return 2;
  }
  if (plainBuild && (plainBuild.twins || plainBuild.architecture !== build.architecture)) {
    console.error(`self-test.ts: --plain-build names ${plainBuild.directory}, which is no plain ${build.architecture} build`);
    return 2;
  }
  const context: Context = {
    build,
    plainBuild,
    base: mkdtempSync(join(tmpdir(), "jitcache-runner-self-test-")),
    jobs,
    timeoutSeconds,
    qemuCpu,
    launcher: new Launcher(build, timeoutSeconds, qemuCpu ?? "max"),
  };

  const cases = [
    ...fixtureCases.map(fixtureCase),
    calibrationCase,
    testjitcacheCase,
    determinismCase,
    pinSelfCase("pin-self", "H7: the twins build compared with itself on the runner's fixtures and the integrator corpus finds no difference", []),
    pinSelfCase("pin-self-forced-blinding", "H7: it finds none either when this build's side blinds every immediate it considers", ["--jitcache-test-force-blinding"]),
    pinSelfPlainCase,
    pinCraftedCase,
  ];
  // Every fixture of this directory belongs to a case, so none sits here untested.
  const covered = new Set(fixtureCases.map(spec => spec.fixture));
  const uncovered = readdirSync(fixtures).filter(name => /\.m?js$/.test(name) && !covered.has(name));
  for (const name of uncovered) console.log(`FAIL coverage: ${name} has no case`);

  const selected = cases.filter(test => !filter || filter.test(test.name));
  let failed = 0;
  await inParallel(selected.map(test => async () => {
    const skip = test.skip?.(context);
    if (skip) {
      console.log(`SKIP ${test.name}: ${skip}`);
      return;
    }
    let problems: string[];
    try {
      problems = await test.run(context);
    } catch (error) {
      problems = [`the case threw: ${(error as Error).stack ?? error}`];
    }
    if (problems.length) {
      failed++;
      console.log(`FAIL ${test.name} (${test.claim})`);
      for (const problem of problems) console.log(problem.replace(/^/gm, "  "));
    } else {
      console.log(`PASS ${test.name}`);
    }
  }), jobs);

  const ran = selected.filter(test => !test.skip?.(context)).length;
  console.log(`${ran - failed} of ${ran} cases passed${ran < selected.length ? `, ${selected.length - ran} skipped` : ""}`);
  if (!failed) rmSync(context.base, { recursive: true, force: true });
  else console.log(`kept: ${context.base}`);
  return failed || uncovered.length ? 1 : 0;
}

process.exit(await main(process.argv.slice(2)));
