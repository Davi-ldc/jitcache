#!/usr/bin/env bun
// The runner's self-tests. A fixture case runs Tools/Scripts/run-jitcache-tests on one fixture of this directory and
// compares the outcome the runner records in its results with what the fixture's jitcache-fixture lines declare: a
// fixture that must pass passes, one that must be skipped is skipped for the reason given, and one that must fail fails
// with exactly the failures listed, by sequence, attempt, run and check, and with no other. Other cases drive processes
// of their own: the runner's calibration with heap probes that never move, testjitcache's command line and its
// twin-report lines, the determinism of the end-of-run description, and the pin comparison of a build with itself, with
// the crafted output pairs of pin-compare-tests.ts. Every fixture of this directory declares its outcome.
//
//   bun JSTests/jitcache/integrator/runner/self-test.ts --build=<twins WebKit build directory>
//       [--plain-build=<plain WebKit build directory>] [--jobs=<n>] [--timeout=<seconds>] [--qemu-cpu=<model>]
//       [--filter=<regex over case names>]
//
// The placement case and the pin cases run on each architecture by naming that architecture's builds, such as those
// bun build.ts twins --arch=aarch64 and bun build.ts --arch=aarch64 made, whose processes run under QEMU. The pin
// comparison's plain half needs --plain-build and is skipped without it.
//
// The self-tests hold the machine lock (Tools/Scripts/jitcache/lock.ts) for their whole run, and the runners they start
// skip it. Up to --jobs cases run at once, so a case that runs work of its own in parallel gets the --jobs width only
// when it is the one case selected, and otherwise runs that work one process at a time.

import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, join } from "node:path";
import { repositoryRoot } from "../../../../Tools/Scripts/jitcache/env.ts";
import { type Build, Launcher, type ProcessEnd, StackSizes, describeEnd, loadBuild, readText, runProcess } from "../../../../Tools/Scripts/jitcache/launcher.ts";
import { holdMachineLock } from "../../../../Tools/Scripts/jitcache/lock.ts";
import { oracleOptionSets } from "../../../../Tools/Scripts/jitcache/oracle.ts";
import { type PinSetup, comparePinOptionSet } from "../../../../Tools/Scripts/jitcache/pin.ts";
import { type CommandContext, type Fixture, MalformedScript, corpusRoot, jscArguments, parseScript, selectScripts } from "../../../../Tools/Scripts/jitcache/scripts.ts";
import { runCraftedPinTests } from "./pin-compare-tests.ts";

const fixtures = import.meta.dirname;
const runner = join(repositoryRoot, "Tools/Scripts/run-jitcache-tests");

// The fixtures of this directory, its .js and .mjs files. The runner runs one only when a test path names its file.
const fixtureFiles = () => readdirSync(fixtures).filter(name => /\.m?js$/.test(name)).sort().map(name => join(fixtures, name));

// For the pin cases: this build stands for the pin, and the options given reach this build's side of the comparison
// alone.
const againstItself = (...pinOptions: string[]) => (context: Context) => [`--pin=${context.build.directory}`, ...(pinOptions.length ? [`--pin-options=${pinOptions.join(" ")}`] : [])];

interface Context {
  build: Build;
  plainBuild: Build | null;
  base: string;
  jobs: number; // the case's width: the processes its own work may run at once
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

// The case of the fixture at path, whose jitcache-fixture lines say what the runner must do with it: the runner runs it
// alone, with the options those lines add, $BUILD standing for the build under test, and one process at a time unless
// they ask for the case's width.
function fixtureCase(path: string, fixture: Fixture): Case {
  const claim = fixture.outcome === "passes" ? "the runner must pass it"
    : fixture.outcome === "skipped" ? `the runner must skip it for a reason that matches ${fixture.skipReason}`
    : `the runner must fail it with exactly ${fixture.failures.map(failureKey).join("; ")}`;
  return {
    name: basename(path).replace(/\.m?js$/, ""),
    claim,
    async run(context) {
      const runnerOptions = fixture.runner.map(option => option.replaceAll("$BUILD", context.build.directory));
      const outcome = await runRunner(context, basename(path), [`--jobs=${fixture.wide ? context.jobs : 1}`, ...runnerOptions, path]);
      const problems: string[] = [];
      const script = outcome.records.find(record => record.kind === "script");
      if (!outcome.resultsPath) problems.push(`the runner wrote no results (${describeEnd(outcome.end)}):\n${outcome.output}`);
      else if (!script) problems.push(`the runner recorded no outcome for the fixture:\n${outcome.output}`);
      else if (fixture.outcome === "passes") {
        if (script.outcome !== "pass") problems.push(`the fixture must pass, and the runner reports ${script.outcome}: ${(script.failures ?? []).map(failureKey).join("; ") || script.reason}`);
        if (outcome.end.code !== 0) problems.push(`the runner ${describeEnd(outcome.end)}, expected 0`);
      } else if (fixture.outcome === "skipped") {
        if (script.outcome !== "skip") problems.push(`the fixture must be skipped, and the runner reports ${script.outcome}: ${(script.failures ?? []).map(failureKey).join("; ")}`);
        else if (!fixture.skipReason!.test(script.reason)) problems.push(`the runner skipped the fixture because it ${script.reason}, which does not match ${fixture.skipReason}`);
        if (outcome.end.code !== 0) problems.push(`the runner ${describeEnd(outcome.end)}, expected 0`);
      } else {
        const actual = new Set<string>((script.failures ?? []).map(failureKey));
        const expected = new Set<string>(fixture.failures.map(failureKey));
        for (const key of expected) {
          if (!actual.has(key)) problems.push(`missing the failure ${key}`);
        }
        for (const failure of script.failures ?? []) {
          if (!expected.has(failureKey(failure))) problems.push(`an unexpected failure ${failureKey(failure)}: ${failure.detail}`);
        }
        for (const detail of fixture.details) {
          if (!(script.failures ?? []).some((failure: { detail: string }) => detail.test(failure.detail)))
            problems.push(`no failure's detail matches ${detail}: ${(script.failures ?? []).map((failure: { detail: string }) => failure.detail).join("; ")}`);
        }
        if (outcome.end.code !== 1) problems.push(`the runner ${describeEnd(outcome.end)}, expected 1`);
      }
      // A note is listed with a sequence, or in the summary, which holds what the runner skipped.
      const notes: string[] = outcome.records.flatMap(record => (record.kind === "sequence" ? record.notes ?? [] : record.kind === "summary" ? record.listed ?? [] : []));
      for (const listed of fixture.lists) {
        if (!notes.some(note => listed.test(note))) problems.push(`the runner listed no note matching ${listed}`);
      }
      cleanUp(outcome, problems);
      return problems;
    },
  };
}

// With heap probes that never move, the runner calibrates twice, the second time with WebKitMallocForceEnabled=1, and
// stops before any sequence with an error naming the heap domain.
const calibrationCase: Case = {
  name: "calibration-fixed-heap-probes",
  claim: "heap probes that never move stop the runner before any sequence, after a second calibration with WebKitMallocForceEnabled=1",
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
// testjitcache's command line and its twin-report lines

const testjitcacheCase: Case = {
  name: "testjitcache",
  claim: "--list, parts, group options, exact test names, a failing check and an unknown option, and twin-report lines that parse as JSON",
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
    const passed = (stdout: string) => [...stdout.matchAll(/^PASS (\S+)$/gm)].map(match => match[1]);

    // --list prints one name<TAB>options<TAB>part<TAB>file line per test: each name unique in its part, options a list of
    // --name=value options, and file the registering file's path from jitcache/, in the part's directory.
    const listing = await testjitcache(["--list"]);
    if (listing.end.code !== 0) problems.push(`testjitcache --list ${describeEnd(listing.end)}`);
    const listed = new Map<string, string>(); // the options of each <part>/<name>
    for (const line of listing.stdout.split("\n").filter(Boolean)) {
      const match = /^([A-Za-z_]\w*)\t([^\t]*)\t(\w+)\tjitcache\/tests\/(\w+)\/[^\t]+$/.exec(line);
      if (!match) {
        problems.push(`--list printed a line that is no name<TAB>options<TAB>part<TAB>file: ${JSON.stringify(line)}`);
        continue;
      }
      const [, name, options, part, directoryOfFile] = match;
      if (part !== directoryOfFile) problems.push(`--list puts ${name} in part ${part}, and its file in ${directoryOfFile}`);
      if (listed.has(`${part}/${name}`)) problems.push(`--list names ${part}/${name} twice`);
      if (options && !options.split(" ").every(option => /^--\w+=\S*$/.test(option))) problems.push(`${name}'s options are no list of --name=value options: ${options}`);
      listed.set(`${part}/${name}`, options);
    }
    const registered: [string, string, string][] = [
      ["harness", "integratorHarnessReadsGroupOptions", "--useConcurrentJIT=false --numberOfGCMarkers=1"],
      ["harness", "integratorHarnessDerivesDependentOptions", "--useJIT=false"],
      ["harness", "integratorHarnessReportsAFailingCheck", ""],
      ["twins", "integratorTwinReportLinesParseAsJSON", ""],
    ];
    for (const [part, name, options] of registered) {
      if (listed.get(`${part}/${name}`) !== options) problems.push(`--list gives ${part}/${name} ${JSON.stringify(listed.get(`${part}/${name}`))}, expected ${JSON.stringify(options)}`);
    }

    // A test registered with options reads them back, and so does the group that turns the JIT off; --part, --group and
    // --test select that test alone.
    for (const [part, name, options] of registered.filter(([, name]) => name !== "integratorHarnessReportsAFailingCheck")) {
      const run = await testjitcache([`--part=${part}`, `--group=${options}`, `--test=${name}`]);
      if (run.end.code !== 0 || passed(run.stdout).join() !== name) problems.push(`${part}/${name} did not pass alone in its group (${describeEnd(run.end)}):\n${run.stdout}${run.stderr}`);
    }

    // --test takes a whole name, so a prefix of one selects nothing, and --part selects exactly its part's tests.
    const prefix = await testjitcache(["--list", "--test=integratorHarness"]);
    if (prefix.end.code !== 0 || prefix.stdout.trim()) problems.push(`--list --test=integratorHarness ${describeEnd(prefix.end)} and selected ${JSON.stringify(prefix.stdout)}, expected nothing`);
    const harness = await testjitcache(["--list", "--part=harness"]);
    const harnessNames = harness.stdout.split("\n").filter(Boolean).map(line => line.split("\t")[0]);
    const expectedNames = [...listed.keys()].filter(key => key.startsWith("harness/")).map(key => key.slice("harness/".length));
    if (harness.end.code !== 0 || harnessNames.join() !== expectedNames.join()) problems.push(`--list --part=harness gave ${JSON.stringify(harnessNames)}, expected ${JSON.stringify(expectedNames)}`);

    // A failing check prints FAIL, then its file, line and message, and the process exits with 1.
    const failing = await testjitcache(["--part=harness", "--test=integratorHarnessReportsAFailingCheck"], { ...process.env, JITCACHE_TEST_FAIL_ON_PURPOSE: "integratorHarnessReportsAFailingCheck" });
    if (failing.end.code !== 1) problems.push(`a failing check left testjitcache ${describeEnd(failing.end)}, expected 1`);
    if (!/^FAIL integratorHarnessReportsAFailingCheck\n {4}\S*HarnessTests\.cpp:\d+: check failed: !failsOnPurpose$/m.test(failing.stdout)) {
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
// Two Off runs of each integrator JS test, and of each description fixture here, write identical end-of-run
// descriptions. Each sequence's Off runs are the ones its oracle starts: one per distinct option set of its runs,
// without the JITCache options.

const determinismCase: Case = {
  name: "describe-determinism",
  claim: "two Off runs of each integrator JS test write identical end-of-run descriptions",
  async run(context) {
    const problems: string[] = [];
    // The integrator's scripts as the runner collects them, without its fixtures, and the description fixtures.
    const paths = [join(corpusRoot, "integrator"), ...fixtureFiles().filter(path => basename(path).startsWith("describe-"))];
    // The command lines of a twins-mode Off run, without --extra-options and the calibration's environment.
    const commands: CommandContext = { twins: true, extraOptions: [], mallocForceEnabled: false };
    const work: (() => Promise<void>)[] = [];
    for (const { path, script } of selectScripts(paths, null)) {
      if (typeof script === "string") {
        problems.push(`${path} is malformed: ${script}`);
        continue;
      }
      // A Bun-hosted script's oracle compares its output alone, and it describes roots of its own.
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
// The pin comparison: a build compared with itself finds no difference on the runner's fixtures, the integrator corpus
// and cb/.

// The test paths the pin cases compare: each fixture, which a test path names by its file, the integrator corpus and
// cb/. cb/'s Off runs cache ICs, so they print the access stubs' headers, which span lines; no fixture and no
// integrator script caches one.
const comparedPaths = () => [...fixtureFiles(), join(corpusRoot, "integrator"), join(corpusRoot, "cb")];

// Through the runner, in the twins build: --pin names the build itself, and pinOptions reach this build's side alone. The
// fixtures fail on purpose in other ways; only the comparison's records count here.
function pinSelfCase(name: string, claim: string, pinOptions: string[]): Case {
  return {
    name,
    claim,
    async run(context) {
      const outcome = await runRunner(context, name, [`--jobs=${context.jobs}`, ...againstItself(...pinOptions)(context), ...comparedPaths()]);
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

// In a plain build the runner skips every script of integrator/, which requires twins, so this half drives the runner's
// own comparison of each Off option set directly.
const pinSelfPlainCase: Case = {
  name: "pin-self-plain",
  claim: "a plain build compared with itself on the runner's fixtures, the integrator corpus and cb/ finds no difference",
  skip: context => (context.plainBuild ? null : "no --plain-build names a plain build"),
  async run(context) {
    const build = context.plainBuild!;
    const launcher = new Launcher(build, context.timeoutSeconds, context.qemuCpu ?? "max");
    const commands: CommandContext = { twins: false, extraOptions: [], mallocForceEnabled: false };
    const setup: PinSetup = { context: commands, build, launcher, pin: build, pinLauncher: launcher, pinOptions: [] };
    const problems: string[] = [];
    let comparisons = 0;
    const work: (() => Promise<void>)[] = [];
    for (const { path, script } of selectScripts(comparedPaths(), null)) {
      if (typeof script === "string") {
        problems.push(`${path} is malformed: ${script}`);
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
  claim: "pin-compare.ts reports the crafted pairs that differ and finds the same code in those that differ only in what canonical code removes",
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
  holdMachineLock();
  const context: Context = {
    build,
    plainBuild,
    base: mkdtempSync(join(tmpdir(), "jitcache-runner-self-test-")),
    jobs,
    timeoutSeconds,
    qemuCpu,
    launcher: new Launcher(build, timeoutSeconds, qemuCpu ?? "max"),
  };

  // Every fixture of this directory declares its outcome, which makes its case, so none sits here untested.
  const fixtureCases: Case[] = [];
  const uncovered: string[] = [];
  for (const path of fixtureFiles()) {
    try {
      const { fixture } = parseScript(path);
      if (fixture) fixtureCases.push(fixtureCase(path, fixture));
      else uncovered.push(`${basename(path)} declares no jitcache-fixture outcome`);
    } catch (error) {
      if (!(error instanceof MalformedScript)) throw error;
      uncovered.push(`${basename(path)} is malformed: ${error.message}`);
    }
  }
  for (const problem of uncovered) console.log(`FAIL coverage: ${problem}`);
  const cases = [
    ...fixtureCases,
    calibrationCase,
    testjitcacheCase,
    determinismCase,
    pinSelfCase("pin-self", "the twins build compared with itself on the runner's fixtures, the integrator corpus and cb/ finds no difference", []),
    pinSelfCase("pin-self-forced-blinding", "the twins build compared with itself on the runner's fixtures, the integrator corpus and cb/ finds no difference either when this build's side blinds every immediate it considers", ["--jitcache-test-force-blinding"]),
    pinSelfPlainCase,
    pinCraftedCase,
  ];

  const selected = cases.filter(test => !filter || filter.test(test.name));
  // Beside other cases, a case's runner and its pool run one process at a time, so the whole run starts about --jobs
  // processes at once.
  if (selected.length > 1) context.jobs = 1;
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
