#!/usr/bin/env bun
// The installation bound's bench (THREAD's opening; SPEC-integrator.md section 16, IB3): each body's import cost against
// its native compilation's, beside its baseline code size, the curve from which installationBoundFraction and
// installationBoundSmallestBody are chosen. Each workload runs as fresh jsc processes with strict off and every JSC option
// at its default, each writing a bench report of its own (SPEC-integrator.harness.md section 9): one Producer, which
// commits the workload's bodies at exit, then N Consumers that import them from its artifact, alternating with N
// Consumers whose artifact holds none of them, where every body compiles natively without recording. A body's import cost
// is the bound total of its import or seed request (SPEC-ucb.md B2) plus its install event's total, joined by key; its
// native cost is its compile event's compilation plus finalization (SPEC-integrator.md section 8.5, step 1). Each side's
// value is the minimum over its N samples, with the median beside it (HARNESS.md, Benches). A sample whose compile or
// install reports supportGenerated does not count, and a body left with no counting sample on a side is set aside.
//
//   bun Tools/Scripts/jitcache-bound-bench.ts --build=<WebKit build directory> [--out=<directory>] [--processes=<n>]
//       [--workloads=jetstream2,sweep] [--filter=<regex>] [--iterations=<n>]
//
// The workloads are JetStream2's JS-only tests, each run alone, and a size sweep of generated functions. --out receives
// the sweep's scripts, the artifacts, each process's report, standard output and error under reports/, bodies.json and
// bound.md.

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { loadavg } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import {
  type Build,
  Launcher,
  type ProcessEnd,
  UsageError,
  describeEnd,
  loadBuild,
  readText,
  repositoryRoot,
} from "./run-jitcache-tests";

const jetStreamRoot = join(repositoryRoot, "PerformanceTests/JetStream2");

// The values installationBoundSmallestBody may take, in bytes of the installed image's linked size, the install
// event's image.codeSize (SPEC-integrator.md section 16).
const smallestBodyCandidates = [0, 128, 256, 512, 1024, 2048, 4096];
const highestRatioCount = 10;
// Only stops a hung process: one JetStream2 test takes minutes in a debug build.
const processTimeoutSeconds = 3600;

const usage = `Usage: jitcache-bound-bench.ts --build=<WebKit build directory> [--out=<directory>] [--processes=<n>]
                             [--workloads=jetstream2,sweep] [--filter=<regex>] [--iterations=<n>]
--build names <build-root>/linux-<arch>-<profile>/deps/WebKit, whose bin/jsc runs every process; the bound is measured
in a release build. --out defaults to a new directory under the build root's jitcache-results. --processes is N, the
Consumers on each side of every workload (10 by default). --workloads picks the groups, and --filter, a regular
expression, picks workloads by name: jetstream2/<test> or sweep/<kind>-<statements>. --iterations replaces every
JetStream2 test's own iteration count, for a short run.`;

class BenchError extends Error {}

// ---------------------------------------------------------------------------------------------------------------------
// Command line

type Group = "jetstream2" | "sweep";
const groups: readonly Group[] = ["jetstream2", "sweep"];

interface BenchOptions {
  build: string;
  out: string | null;
  processes: number;
  groups: Group[];
  filter: RegExp | null;
  iterations: number | null; // JetStream2's testIterationCount; null keeps each test's own
}

function parseCommandLine(argv: string[]): BenchOptions {
  const options: BenchOptions = { build: "", out: null, processes: 10, groups: [...groups], filter: null, iterations: null };
  for (const argument of argv) {
    const equals = argument.indexOf("=");
    const name = equals < 0 ? argument : argument.slice(0, equals);
    const value = equals < 0 ? null : argument.slice(equals + 1);
    const required = () => {
      if (value === null || value === "") throw new UsageError(`${name} needs a value`);
      return value;
    };
    switch (name) {
      case "--build":
        options.build = resolve(required());
        break;
      case "--out":
        options.out = resolve(required());
        break;
      case "--processes":
        options.processes = positiveInteger(required(), name);
        break;
      case "--workloads": {
        const named = required().split(",");
        for (const group of named) {
          if (!groups.includes(group as Group)) throw new UsageError(`--workloads takes ${groups.join(" and ")}, not ${group}`);
        }
        options.groups = groups.filter(group => named.includes(group));
        break;
      }
      case "--filter":
        try {
          options.filter = new RegExp(required());
        } catch (error) {
          throw new UsageError(`--filter is no regular expression: ${(error as Error).message}`);
        }
        break;
      case "--iterations":
        options.iterations = positiveInteger(required(), name);
        break;
      default:
        throw new UsageError(`unknown option ${argument}`);
    }
  }
  if (!options.build) throw new UsageError("--build is required");
  return options;
}

function positiveInteger(text: string, name: string): number {
  const number = Number(text);
  if (!Number.isInteger(number) || number < 1) throw new UsageError(`${name} takes a positive integer, not ${text}`);
  return number;
}

// ---------------------------------------------------------------------------------------------------------------------
// Workloads

interface Workload {
  name: string; // <group>/<test or point>, which --filter matches
  cwd: string;
  scriptArguments: string[]; // jsc's arguments after the JITCache flags
  script: { path: string; text: string } | null; // a sweep point's generated script
}

interface Context {
  options: BenchOptions;
  build: Build;
  launcher: Launcher;
  out: string;
  nativeArtifact: string; // holds no body, so the native side's Consumers compile every body natively
}

// JetStream2's JS-only tests are those JetStreamDriver.js adds in the jsc shell when no testList is given, which leaves
// out its worker tests, less the WasmBenchmark ones, which instantiate a WebAssembly module. The build's own shell lists
// them.
async function jetStreamTests(context: Context): Promise<string[]> {
  const listing = "for (const benchmark of JetStream.benchmarks) { if (!(benchmark instanceof WasmBenchmark)) print(benchmark.name); }";
  const prefix = join(context.out, "reports", "jetstream2-tests");
  const io = { env: engineEnvironment(), cwd: jetStreamRoot, stdout: `${prefix}.stdout`, stderr: `${prefix}.stderr` };
  const end = await context.launcher.engine(context.build.jsc, ["--destroy-vm", "-e", "const isInBrowser = false;", "JetStreamDriver.js", "-e", listing], io, null);
  const tests = (readText(io.stdout) ?? "").split("\n").filter(Boolean);
  if (!cleanEnd(end) || !tests.length) throw new BenchError(`the shell that lists JetStream2's tests ${describeEnd(end)} and printed ${tests.length} names; see ${prefix}.stderr`);
  return tests;
}

// cli.js keeps the testList and testIterationCount it finds, and the driver loads every file relative to the working
// directory.
function jetStreamWorkload(test: string, iterations: number | null): Workload {
  const globals = `testList = [${JSON.stringify(test)}];${iterations === null ? "" : ` testIterationCount = ${iterations};`}`;
  return { name: `jetstream2/${test}`, cwd: jetStreamRoot, scriptArguments: ["-e", globals, "cli.js"], script: null };
}

// One kind of statement in the size sweep. Statement j of a function is forms[j % forms.length](j).
interface SweepKind {
  name: string;
  forms: ((j: number) => string)[];
  bytecodesPerStatement: number; // as --dumpGeneratedBytecodes counts them, which bounds the largest point
  parameters: string;
  locals: string | null;
  result: string;
  declarations: string[]; // the script-level code the functions use
  callArguments: string; // what the driving loop passes, with i its counter
}

const sweepKinds: readonly SweepKind[] = [
  {
    // get_by_id and put_by_id over eight properties of one object, so every site stays monomorphic.
    name: "property",
    forms: [j => `x = o.p${j % 8};`, j => `o.p${(j + 3) % 8} = x;`],
    bytecodesPerStatement: 1,
    parameters: "o",
    locals: "let x = 0;",
    result: "return x;",
    declarations: ["const object = { p0: 0, p1: 1, p2: 2, p3: 3, p4: 4, p5: 5, p6: 6, p7: 7 };"],
    callArguments: "object",
  },
  {
    // The four MathICs, add, sub, negate and mul, over integers that stay small: b keeps its value and c flips its sign.
    name: "arithmetic",
    forms: [() => "a = b + c;", () => "b = a - c;", () => "c = -c;", () => "d = c * e;"],
    bytecodesPerStatement: 1,
    parameters: "b, c, e",
    locals: "let a = 0, d = 0;",
    result: "return a + d;",
    declarations: [],
    callArguments: "3, 5, -1",
  },
  {
    // Calls with one, two and three arguments, each site to its own global function: resolve_scope, get_from_scope,
    // the argument moves and call.
    name: "call",
    forms: [() => "x = callee1(x);", () => "x = callee2(x, y);", () => "x = callee3(y, x, y);"],
    bytecodesPerStatement: 5,
    parameters: "x, y",
    locals: null,
    result: "return x;",
    declarations: ["function callee1(a) { return a; }", "function callee2(a, b) { return a; }", "function callee3(a, b, c) { return b; }"],
    callArguments: "i, 7",
  },
  {
    // switch_imm over four dense cases: a jump table of four code offsets, which prepareImage rebuilds at install.
    name: "switch",
    forms: [() => "switch (k) { case 0: r = 1; break; case 1: r = 2; break; case 2: r = 3; break; case 3: r = 4; break; }"],
    bytecodesPerStatement: 9,
    parameters: "k",
    locals: "let r = 0;",
    result: "return r;",
    declarations: [],
    callArguments: "i & 3",
  },
];

// Statements double from sweepSmallestStatements while a body stays within sweepLargestBytecodes.
const sweepSmallestStatements = 2;
const sweepLargestBytecodes = 4096;
// The calls that drive each function. The LLInt asks for a baseline compilation once a function has 500 points, 15 per
// call, and with concurrent JIT on, a later check, one every 100 points, installs the code once the compilation is done;
// the count leaves the largest body's compilation room to finish while the loop still calls it.
const sweepCalls = 4000;

// warmUp, one cycle of the kind's forms, reaches baseline before measured is first called, so measured compiles and
// installs in a VM that has already generated the per-VM support its sites need, as IB3 requires of a counted sample.
function sweepScript(kind: SweepKind, statements: number): string {
  const definition = (name: string, count: number) => [
    `function ${name}(${kind.parameters}) {`,
    ...(kind.locals ? [`    ${kind.locals}`] : []),
    ...Array.from({ length: count }, (_, j) => `    ${kind.forms[j % kind.forms.length](j)}`),
    `    ${kind.result}`,
    "}",
  ];
  const drive = (name: string) => [`for (let i = 0; i < ${sweepCalls}; ++i)`, `    ${name}(${kind.callArguments});`];
  return [
    `// jitcache-bound-bench.ts: the ${kind.name} sweep at ${statements} statements.`,
    ...kind.declarations,
    ...definition("warmUp", kind.forms.length),
    ...definition("measured", statements),
    ...drive("warmUp"),
    ...drive("measured"),
    "",
  ].join("\n");
}

function sweepWorkloads(out: string): Workload[] {
  return sweepKinds.flatMap(kind => {
    const points: Workload[] = [];
    for (let statements = sweepSmallestStatements; statements * kind.bytecodesPerStatement <= sweepLargestBytecodes; statements *= 2) {
      const path = join(out, "scripts", "sweep", `${kind.name}-${statements}.js`);
      points.push({ name: `sweep/${kind.name}-${statements}`, cwd: out, scriptArguments: [path], script: { path, text: sweepScript(kind, statements) } });
    }
    return points;
  });
}

async function selectWorkloads(context: Context): Promise<Workload[]> {
  const { options } = context;
  const workloads: Workload[] = [];
  if (options.groups.includes("jetstream2")) workloads.push(...(await jetStreamTests(context)).map(test => jetStreamWorkload(test, options.iterations)));
  if (options.groups.includes("sweep")) workloads.push(...sweepWorkloads(context.out));
  const selected = workloads.filter(workload => !options.filter || options.filter.test(workload.name));
  if (!selected.length) throw new UsageError(`no workload of ${options.groups.join(" or ")} matches --filter=${options.filter?.source}`);
  return selected;
}

// ---------------------------------------------------------------------------------------------------------------------
// Processes

type Role = "producer" | "import" | "native";

type BenchEvent = Record<string, unknown> & { event: string };

interface ProcessRecord {
  workload: string | null; // null for the Producer that creates the native side's artifact
  role: Role;
  sample: number | null; // a Consumer's index on its side
  files: string; // the path of its .jsonl report, .stdout and .stderr, without the extension
  command: string[];
  cwd: string;
  end: string;
  milliseconds: number;
  progress: unknown; // the progress of its final status (--jitcache-log)
  problems: string[];
}

// The bench measures the default (THREAD Verification), so no JSC_ option reaches a process from the environment.
function engineEnvironment(): Record<string, string | undefined> {
  return Object.fromEntries(Object.entries(process.env).filter(([name]) => !name.startsWith("JSC_")));
}

const cleanEnd = (end: ProcessEnd) => !end.error && !end.timedOut && !end.signal && end.code === 0;

function readReport(path: string): { events: BenchEvent[]; unreadable: number } {
  const events: BenchEvent[] = [];
  let unreadable = 0;
  for (const line of (readText(path) ?? "").split("\n")) {
    if (!line) continue;
    try {
      const event = JSON.parse(line);
      if (typeof event?.event === "string") events.push(event);
      else ++unreadable;
    } catch {
      ++unreadable;
    }
  }
  return { events, unreadable };
}

// --jitcache-log's lines on standard error: the start result, each delta result and the final status.
function jitCacheLog(path: string): Record<string, unknown>[] {
  const lines: Record<string, unknown>[] = [];
  for (const line of (readText(path) ?? "").split("\n")) {
    if (!line.startsWith('{"jitcache":')) continue;
    try {
      lines.push(JSON.parse(line));
    } catch {
      // A line the process did not finish; the missing final status reports it.
    }
  }
  return lines;
}

// One jsc process in a role. --destroy-vm, as in every shell run of the harness, releases the VM, and --jitcache-log's
// final status names a fault. The problems found are listed, and the process's samples count all the same, since each
// event measures a span that ended.
async function runRole(context: Context, workload: Workload | null, role: Role, sample: number | null, artifact: string, prefix: string) {
  const command = [
    context.build.jsc,
    "--destroy-vm",
    `--jitcache=${artifact}`,
    `--jitcache-mode=${role === "producer" ? "p" : "c"}`,
    ...(role === "producer" && workload ? ["--jitcache-delta-at-exit"] : []),
    "--jitcache-log",
    `--jitcache-bench-report=${prefix}.jsonl`,
    ...(workload ? workload.scriptArguments : ["-e", ""]),
  ];
  const cwd = workload?.cwd ?? context.out;
  const started = performance.now();
  const end = await context.launcher.engine(command[0], command.slice(1), { env: engineEnvironment(), cwd, stdout: `${prefix}.stdout`, stderr: `${prefix}.stderr` }, null);
  const milliseconds = Math.round(performance.now() - started);

  const problems: string[] = [];
  if (!cleanEnd(end)) problems.push(describeEnd(end));
  const { events, unreadable } = readReport(`${prefix}.jsonl`);
  if (unreadable) problems.push(`${unreadable} lines of the report are no event`);
  const start = events.find(event => event.event === "start");
  const startedAs = role === "producer" ? "producer" : "consumer";
  if (!start) problems.push("the report holds no start event");
  else if (start.outcome !== "started" || start.role !== startedAs) problems.push(`start ended ${start.outcome} as ${start.role}`);
  else if (role === "native" && start.indexedBodies !== 0) problems.push(`the native side's artifact indexes ${start.indexedBodies} bodies`);
  const log = jitCacheLog(`${prefix}.stderr`);
  const status = log.findLast(line => line.jitcache === "status");
  if (!status) problems.push("standard error holds no final status");
  else if (status.firstFault) problems.push(`fault ${JSON.stringify(status.firstFault)}`);
  if (command.includes("--jitcache-delta-at-exit")) {
    const delta = log.find(line => line.jitcache === "delta");
    if (delta?.outcome !== "completed") problems.push(`delta at exit ${delta ? `ended ${delta.outcome}` : "wrote no result"}`);
  }
  // cli.js catches a test's exception and prints it, so the process still exits with 0.
  if (workload?.name.startsWith("jetstream2/") && /^(JetStream2 failed|Error in runCode)/m.test(readText(`${prefix}.stdout`) ?? ""))
    problems.push("the JetStream2 test failed, as its standard output says");

  const record: ProcessRecord = {
    workload: workload?.name ?? null,
    role,
    sample,
    files: prefix,
    command,
    cwd,
    end: describeEnd(end),
    milliseconds,
    progress: status?.progress ?? null,
    problems,
  };
  return { record, events };
}

// The native side's artifact: a Producer run of an empty script, which reaches no baseline compilation and so commits no
// body.
async function createNativeArtifact(context: Context): Promise<ProcessRecord> {
  rmSync(context.nativeArtifact, { recursive: true, force: true });
  mkdirSync(dirname(context.nativeArtifact), { recursive: true });
  const { record } = await runRole(context, null, "producer", null, context.nativeArtifact, join(context.out, "reports", "native-artifact"));
  if (record.problems.length) throw new BenchError(`the Producer of the native side's artifact failed: ${record.problems.join("; ")}`);
  return record;
}

// ---------------------------------------------------------------------------------------------------------------------
// Samples

interface ImportSample {
  nanoseconds: number; // request + install
  request: number; // the request event's bound total, its counted field
  install: number; // the install event's total, the install function's thread CPU time
  action: string; // import or seed
  imageCodeSize: number; // the image's linked size
  allocation: number; // the installed code's executable allocation, the install event's codeSize
  fixups: number; // the image's fixup count
  steps: Record<string, number>; // the install's steps that ran, wall time that explains the total (harness section 9.1)
}

interface NativeSample {
  nanoseconds: number; // compile + finalize
  compile: number;
  finalize: number;
  allocation: number; // the native code's executable allocation, the compile event's codeSize
}

interface BodyTally {
  key: string;
  importSamples: ImportSample[];
  nativeSamples: NativeSample[];
  installSupportGenerated: number;
  installWithoutRequest: number; // installs that no import or seed request of the key preceded in their process
  compileSupportGenerated: number;
  requestedWithoutInstall: number; // import-side processes that imported the body and never installed it
  compiledOnImportSide: number; // import-side processes that compiled the body natively
  installedImageSize: number | null; // the first install's image.codeSize, the image's linked size
  installedAllocation: number | null; // the first install's codeSize
  compiledAllocation: number | null; // the first native-side compile's codeSize
}

const numberField = (event: BenchEvent, field: string) => (typeof event[field] === "number" ? (event[field] as number) : 0);

function tallyOf(tallies: Map<string, BodyTally>, key: string): BodyTally {
  let tally = tallies.get(key);
  if (!tally) {
    tally = {
      key,
      importSamples: [],
      nativeSamples: [],
      installSupportGenerated: 0,
      installWithoutRequest: 0,
      compileSupportGenerated: 0,
      requestedWithoutInstall: 0,
      compiledOnImportSide: 0,
      installedImageSize: null,
      installedAllocation: null,
      compiledAllocation: null,
    };
    tallies.set(key, tally);
  }
  return tally;
}

// An import takes the place of a body's first compilation, so a process gives a body at most one sample per side: its
// first compile on the native side, and on the import side its first import or seed request with its first install.
function firstByKey(events: BenchEvent[], test: (event: BenchEvent) => boolean): Map<string, BenchEvent> {
  const first = new Map<string, BenchEvent>();
  for (const event of events) {
    if (typeof event.key === "string" && test(event) && !first.has(event.key)) first.set(event.key, event);
  }
  return first;
}

function addImportSide(tallies: Map<string, BodyTally>, events: BenchEvent[]) {
  const requests = firstByKey(events, event => event.event === "request" && (event.action === "import" || event.action === "seed"));
  const installs = firstByKey(events, event => event.event === "install");
  for (const [key, install] of installs) {
    const tally = tallyOf(tallies, key);
    tally.installedImageSize ??= numberField(install, "image.codeSize");
    tally.installedAllocation ??= numberField(install, "codeSize");
    const request = requests.get(key);
    if (install.supportGenerated === true) {
      ++tally.installSupportGenerated;
      continue;
    }
    if (!request) {
      ++tally.installWithoutRequest;
      continue;
    }
    const steps: Record<string, number> = {};
    for (const [field, value] of Object.entries(install)) {
      if (field.startsWith("steps.") && typeof value === "number") steps[field.slice("steps.".length)] = value;
    }
    const requestNanoseconds = numberField(request, "counted");
    const installNanoseconds = numberField(install, "total");
    tally.importSamples.push({
      nanoseconds: requestNanoseconds + installNanoseconds,
      request: requestNanoseconds,
      install: installNanoseconds,
      action: String(request.action),
      imageCodeSize: numberField(install, "image.codeSize"),
      allocation: numberField(install, "codeSize"),
      fixups: numberField(install, "image.fixupCount"),
      steps,
    });
  }
  for (const key of requests.keys()) {
    if (!installs.has(key)) ++tallyOf(tallies, key).requestedWithoutInstall;
  }
  for (const key of firstByKey(events, event => event.event === "compile").keys()) ++tallyOf(tallies, key).compiledOnImportSide;
}

// The native side counts only these Consumers' compiles: a Producer's carry the cost of recording. Returns the number of
// compiles without a key, whose UCB the parent-key registry did not hold.
function addNativeSide(tallies: Map<string, BodyTally>, events: BenchEvent[]): number {
  for (const [key, compile] of firstByKey(events, event => event.event === "compile")) {
    const tally = tallyOf(tallies, key);
    tally.compiledAllocation ??= numberField(compile, "codeSize");
    if (compile.supportGenerated === true) {
      ++tally.compileSupportGenerated;
      continue;
    }
    const compileNanoseconds = numberField(compile, "compileNanoseconds");
    const finalizeNanoseconds = numberField(compile, "finalizeNanoseconds");
    tally.nativeSamples.push({ nanoseconds: compileNanoseconds + finalizeNanoseconds, compile: compileNanoseconds, finalize: finalizeNanoseconds, allocation: numberField(compile, "codeSize") });
  }
  return events.filter(event => event.event === "compile" && event.key === null).length;
}

// ---------------------------------------------------------------------------------------------------------------------
// Bodies

type BodyStatus = "joined" | "set aside" | "native only" | "import only";

// Why a side has no counting sample: per-VM support generated in each of its samples (IB3), or installs that no import or
// seed request preceded, as an attach's.
interface SetAside {
  side: "import" | "native";
  reason: "supportGenerated" | "installWithoutImportRequest";
  samples: number;
}

interface SideValue<Sample> {
  samples: number; // the counting ones
  min: number | null;
  median: number | null;
  minimumSample: Sample | null; // the sample that gave the minimum
}

interface BodyResult {
  workload: string;
  key: string;
  status: BodyStatus;
  codeSize: number | null; // the installed image's linked size, which S reads; null for a body never installed
  installedAllocation: number | null; // the installed code's executable allocation
  nativeAllocation: number | null; // the first native-side compilation's, which may differ from the installed one
  ratio: number | null; // the import minimum over the native minimum
  import: SideValue<ImportSample> & { supportGenerated: number; withoutImportRequest: number; requestedWithoutInstall: number; compiledNatively: number };
  native: SideValue<NativeSample> & { supportGenerated: number };
  setAside: SetAside[];
}

function median(values: number[]): number {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

// The nearest-rank percentile of sorted values.
const percentile = (sorted: number[], p: number) => (sorted.length ? sorted[Math.ceil((p / 100) * sorted.length) - 1] : null);

function sideValue<Sample extends { nanoseconds: number }>(samples: Sample[]): SideValue<Sample> {
  if (!samples.length) return { samples: 0, min: null, median: null, minimumSample: null };
  const minimumSample = samples.reduce((least, sample) => (sample.nanoseconds < least.nanoseconds ? sample : least));
  return { samples: samples.length, min: minimumSample.nanoseconds, median: median(samples.map(sample => sample.nanoseconds)), minimumSample };
}

function bodyResult(workload: string, tally: BodyTally): BodyResult {
  const imported = sideValue(tally.importSamples);
  const compiled = sideValue(tally.nativeSamples);
  const setAside: SetAside[] = [];
  if (!imported.samples) {
    if (tally.installSupportGenerated) setAside.push({ side: "import", reason: "supportGenerated", samples: tally.installSupportGenerated });
    if (tally.installWithoutRequest) setAside.push({ side: "import", reason: "installWithoutImportRequest", samples: tally.installWithoutRequest });
  }
  if (!compiled.samples && tally.compileSupportGenerated) setAside.push({ side: "native", reason: "supportGenerated", samples: tally.compileSupportGenerated });
  const status: BodyStatus = imported.samples && compiled.samples ? "joined" : setAside.length ? "set aside" : compiled.samples ? "native only" : "import only";
  return {
    workload,
    key: tally.key,
    status,
    codeSize: tally.installedImageSize,
    installedAllocation: tally.installedAllocation,
    nativeAllocation: tally.compiledAllocation,
    ratio: status === "joined" && compiled.min ? imported.min! / compiled.min : null,
    import: {
      ...imported,
      supportGenerated: tally.installSupportGenerated,
      withoutImportRequest: tally.installWithoutRequest,
      requestedWithoutInstall: tally.requestedWithoutInstall,
      compiledNatively: tally.compiledOnImportSide,
    },
    native: { ...compiled, supportGenerated: tally.compileSupportGenerated },
    setAside,
  };
}

interface Coverage {
  smallestBody: number;
  covered: number;
  nativeShare: number | null; // the covered bodies' native cost over every joined body's
  p50: number | null;
  p99: number | null;
  max: number | null;
}

// The joined bodies a smallest body S covers, those whose installed image's linked size is at least S, and their ratios.
function coverage(bodies: BodyResult[], smallestBody: number): Coverage {
  const joined = bodies.filter(body => body.status === "joined");
  const covered = joined.filter(body => body.codeSize! >= smallestBody);
  const nativeCost = (list: BodyResult[]) => list.reduce((total, body) => total + body.native.min!, 0);
  const ratios = covered.map(body => body.ratio!).sort((a, b) => a - b);
  const total = nativeCost(joined);
  return {
    smallestBody,
    covered: covered.length,
    nativeShare: total ? nativeCost(covered) / total : null,
    p50: percentile(ratios, 50),
    p99: percentile(ratios, 99),
    max: ratios.length ? ratios[ratios.length - 1] : null,
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// One workload

interface WorkloadResult {
  name: string;
  artifactBodies: number | null; // what the import side's start indexed
  keylessCompiles: number; // native-side compiles without a key, over its N processes
  processes: ProcessRecord[];
  bodies: BodyResult[];
}

async function runWorkload(context: Context, workload: Workload): Promise<WorkloadResult> {
  const reports = join(context.out, "reports", workload.name);
  const artifact = join(context.out, "artifacts", workload.name);
  // A run into an earlier --out starts the workload over: a report appends, and a Producer refuses an existing artifact.
  rmSync(reports, { recursive: true, force: true });
  rmSync(artifact, { recursive: true, force: true });
  mkdirSync(reports, { recursive: true });
  mkdirSync(dirname(artifact), { recursive: true });

  const started = performance.now();
  const processes = [(await runRole(context, workload, "producer", null, artifact, join(reports, "producer"))).record];
  const tallies = new Map<string, BodyTally>();
  let artifactBodies: number | null = null;
  let keylessCompiles = 0;
  // The sides alternate, so drift on a shared machine spreads over both (HARNESS.md, Benches).
  for (let sample = 0; sample < context.options.processes; ++sample) {
    const imports = await runRole(context, workload, "import", sample, artifact, join(reports, `import-${sample}`));
    addImportSide(tallies, imports.events);
    const start = imports.events.find(event => event.event === "start");
    if (typeof start?.indexedBodies === "number") artifactBodies ??= start.indexedBodies;
    const native = await runRole(context, workload, "native", sample, context.nativeArtifact, join(reports, `native-${sample}`));
    keylessCompiles += addNativeSide(tallies, native.events);
    processes.push(imports.record, native.record);
  }
  const bodies = [...tallies.values()].map(tally => bodyResult(workload.name, tally));

  const count = (status: BodyStatus) => bodies.filter(body => body.status === status).length;
  const seconds = ((performance.now() - started) / 1000).toFixed(0);
  console.log(`${workload.name}: ${count("joined")} joined, ${count("set aside")} set aside, ${count("native only")} native only, ${count("import only")} import only, from an artifact of ${artifactBodies ?? "?"} bodies (${seconds} s)`);
  for (const record of processes) {
    for (const problem of record.problems) console.log(`  ${record.files}: ${problem}`);
  }
  return { name: workload.name, artifactBodies, keylessCompiles, processes, bodies };
}

// ---------------------------------------------------------------------------------------------------------------------
// Output

interface LoadAverages {
  before: number[];
  after: number[];
}

const microseconds = (nanoseconds: number) => (nanoseconds / 1000).toFixed(nanoseconds < 10_000 ? 2 : 1);
const ratioText = (ratio: number | null) => (ratio === null ? "n/a" : ratio.toFixed(3));
const percentText = (share: number | null) => (share === null ? "n/a" : `${(100 * share).toFixed(1)}%`);
const loadText = (load: number[]) => load.map(value => value.toFixed(2)).join(" ");
const buildProfile = (build: Build) => basename(resolve(build.directory, "../.."));
// A key's version, kind, specialization and mode bytes, then the start of its identity digest, which follows the eight
// header bytes (BodyKey in UCBKeys.h).
const shortKey = (key: string) => `${key.slice(0, 8)}-${key.slice(16, 28)}`;

// A Markdown table; align holds one letter per column, l or r.
function markdownTable(header: string[], rows: (string | number)[][], align: string): string[] {
  return [
    `| ${header.join(" | ")} |`,
    `|${header.map((_, column) => (align[column] === "r" ? "---:" : "---")).join("|")}|`,
    ...rows.map(row => `| ${row.join(" | ")} |`),
  ];
}

// The groups that ran, as rows labelled jetstream2/* and sweep/*, after the row of every workload.
function scopes(results: WorkloadResult[]): [string, BodyResult[]][] {
  const all = results.flatMap(result => result.bodies);
  const ran = groups.filter(group => results.some(result => result.name.startsWith(`${group}/`)));
  const perGroup = ran.map(group => [`${group}/*`, all.filter(body => body.workload.startsWith(`${group}/`))] as [string, BodyResult[]]);
  return [["all", all], ...(ran.length > 1 ? perGroup : [])];
}

function boundMarkdown(context: Context, results: WorkloadResult[], loads: LoadAverages, nativeArtifact: ProcessRecord): string {
  const { build, options } = context;
  const bodies = results.flatMap(result => result.bodies);
  const coverageHeader = ["workloads", "S (bytes)", "bodies covered", "share of native cost", "p50 ratio", "p99 ratio", "max ratio"];
  const coverageRows = (label: string, scope: BodyResult[]) =>
    smallestBodyCandidates.map(smallestBody => {
      const covered = coverage(scope, smallestBody);
      return [label, smallestBody, covered.covered, percentText(covered.nativeShare), ratioText(covered.p50), ratioText(covered.p99), ratioText(covered.max)];
    });
  const iterations = options.iterations === null ? "" : `, every JetStream2 test at ${options.iterations} iterations`;
  const lines = [
    "# Installation bound",
    "",
    `Build \`${build.directory}\` (${buildProfile(build)}), ${options.processes} processes per side${iterations}. The load average was ${loadText(loads.before)} before the runs and ${loadText(loads.after)} after.`,
    "",
  ];
  if (build.twins || build.sanitized || buildProfile(build).includes("debug"))
    lines.push("This build is a debug, sanitized or twins build, whose costs say nothing about the bound a release build meets.", "");
  lines.push(
    "A body's import cost is its import or seed request's bound total (the request event's `counted`) plus its install event's total; its native cost is its compile event's compilation plus finalization, in Consumers whose artifact holds none of the workload's bodies. Each cost is the minimum over the processes that gave the body a counting sample, and the ratio divides the import minimum by the native one; bodies.json also holds the medians. A joined body is covered at S when its image size, the installed image's linked size (the install event's `image.codeSize`), is at least S. The share is the covered bodies' native cost over that of every joined body, and p50 and p99 are nearest-rank percentiles of the covered bodies' ratios.",
    "",
    "## All workloads",
    "",
    ...markdownTable(coverageHeader, scopes(results).flatMap(([label, scope]) => coverageRows(label, scope)), "lrrrrrr"),
    "",
    "## Per workload",
    "",
    ...markdownTable(coverageHeader, results.flatMap(result => coverageRows(result.name, result.bodies)), "lrrrrrr"),
    "",
    "## Highest ratios",
    "",
    "The covered bodies with the highest ratios, each with its install's steps in the sample that gave the import minimum, longest first.",
    "",
  );
  const stepsText = (steps: Record<string, number>) =>
    Object.entries(steps)
      .sort((a, b) => b[1] - a[1])
      .map(([step, nanoseconds]) => `${step} ${microseconds(nanoseconds)}`)
      .join(", ");
  for (const smallestBody of smallestBodyCandidates) {
    const highest = bodies
      .filter(body => body.status === "joined" && body.codeSize! >= smallestBody)
      .sort((a, b) => b.ratio! - a.ratio!)
      .slice(0, highestRatioCount);
    lines.push(`### S = ${smallestBody} bytes`, "");
    if (!highest.length) {
      lines.push("No joined body is this large.", "");
      continue;
    }
    const rows = highest.map(body => {
      const sample = body.import.minimumSample!;
      return [body.workload, `\`${shortKey(body.key)}\``, body.codeSize!, microseconds(body.native.min!), microseconds(body.import.min!), ratioText(body.ratio), microseconds(sample.request), microseconds(sample.install), sample.fixups, stepsText(sample.steps)];
    });
    const header = ["workload", "key", "image size", "native (µs)", "import (µs)", "ratio", "request (µs)", "install (µs)", "fixups", "install steps (µs, wall time)"];
    lines.push(...markdownTable(header, rows, "llrrrrrrrl"), "");
  }

  lines.push(
    "## Bodies set aside or joined on one side only",
    "",
    "A body is set aside when each of its samples on one side was excluded: per-VM support was generated during its compile or install (IB3), or no import or seed request preceded its install. A body joined on one side only was never installed on the import side or never compiled on the native side. Compiles without a key are native-side compiles whose UCB the parent-key registry did not hold, over all of a workload's native-side processes.",
    "",
  );
  const tallyRow = (label: string, scope: BodyResult[], artifactBodies: string, keyless: number) => {
    const count = (status: BodyStatus) => scope.filter(body => body.status === status).length;
    const sizesDiffer = scope.filter(body => body.status === "joined" && body.nativeAllocation !== body.installedAllocation).length;
    return [label, artifactBodies, count("joined"), count("set aside"), count("native only"), count("import only"), keyless, sizesDiffer];
  };
  const tallyRows = [
    ...results.map(result => tallyRow(result.name, result.bodies, String(result.artifactBodies ?? "?"), result.keylessCompiles)),
    tallyRow("all", bodies, "", results.reduce((total, result) => total + result.keylessCompiles, 0)),
  ];
  const tallyHeader = ["workload", "artifact bodies", "joined", "set aside", "native only", "import only", "compiles without a key", "joined, native and installed allocations differ"];
  lines.push(...markdownTable(tallyHeader, tallyRows, "lrrrrrrr"), "");
  const reasons = new Map<string, number>();
  for (const body of bodies) {
    for (const { side, reason } of body.setAside) reasons.set(`${side} ${reason}`, (reasons.get(`${side} ${reason}`) ?? 0) + 1);
  }
  if (reasons.size) lines.push(...markdownTable(["side", "reason", "bodies"], [...reasons].map(([reason, count]) => [...reason.split(" "), count]), "llr"), "");

  const troubled = [nativeArtifact, ...results.flatMap(result => result.processes)].filter(record => record.problems.length);
  lines.push("## Processes with problems", "");
  if (!troubled.length) lines.push("Every process exited with 0, started in its role and ended without a fault.");
  else lines.push(...markdownTable(["process", "problems"], troubled.map(record => [`\`${record.files}\``, record.problems.join("; ")]), "ll"));
  lines.push("");
  return lines.join("\n");
}

// ---------------------------------------------------------------------------------------------------------------------

export async function main(argv: string[]): Promise<number> {
  let options: BenchOptions;
  let build: Build;
  try {
    options = parseCommandLine(argv);
    build = loadBuild(options.build);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    console.error(`jitcache-bound-bench: ${error.message}\n${usage}`);
    return 2;
  }
  const out = options.out ?? join(build.resultsDirectory, `bound-${new Date().toISOString().replace(/[:.]/g, "-")}`);
  mkdirSync(join(out, "reports"), { recursive: true });
  const context: Context = { options, build, launcher: new Launcher(build, processTimeoutSeconds, "max"), out, nativeArtifact: join(out, "artifacts", "native") };

  let workloads: Workload[];
  let nativeArtifact: ProcessRecord;
  const before = loadavg();
  try {
    workloads = await selectWorkloads(context);
    for (const { script } of workloads) {
      if (!script) continue;
      mkdirSync(dirname(script.path), { recursive: true });
      writeFileSync(script.path, script.text);
    }
    console.log(`build ${build.directory} (${buildProfile(build)}): ${workloads.length} workloads, ${options.processes} processes per side, into ${out}`);
    nativeArtifact = await createNativeArtifact(context);
  } catch (error) {
    if (error instanceof UsageError) {
      console.error(`jitcache-bound-bench: ${error.message}\n${usage}`);
      return 2;
    }
    if (!(error instanceof BenchError)) throw error;
    console.error(`jitcache-bound-bench: ${error.message}`);
    return 1;
  }

  const results: WorkloadResult[] = [];
  for (const workload of workloads) results.push(await runWorkload(context, workload));
  const loads = { before, after: loadavg() };

  const profile = { directory: build.directory, profile: buildProfile(build), architecture: build.architecture, twins: build.twins, sanitized: build.sanitized };
  const bench = {
    build: profile,
    processes: options.processes,
    iterations: options.iterations,
    loadAverage: loads,
    nativeArtifact,
    workloads: results.map(({ bodies, ...result }) => result),
    bodies: results.flatMap(result => result.bodies),
  };
  writeFileSync(join(out, "bodies.json"), `${JSON.stringify(bench, null, 2)}\n`);
  writeFileSync(join(out, "bound.md"), boundMarkdown(context, results, loads, nativeArtifact));

  for (const [label, scope] of scopes(results)) {
    console.log(`${label}:`);
    for (const smallestBody of smallestBodyCandidates) {
      const covered = coverage(scope, smallestBody);
      console.log(`  S ${String(smallestBody).padStart(4)}: ${String(covered.covered).padStart(5)} bodies, ${percentText(covered.nativeShare).padStart(6)} of the native cost, ratio p50 ${ratioText(covered.p50)}, p99 ${ratioText(covered.p99)}, max ${ratioText(covered.max)}`);
    }
  }
  console.log(`load average ${loadText(loads.before)} before, ${loadText(loads.after)} after`);
  console.log(`wrote ${join(out, "bound.md")} and ${join(out, "bodies.json")}`);
  return results.some(result => result.processes.some(record => record.problems.length)) ? 1 : 0;
}

if (import.meta.main) process.exit(await main(process.argv.slice(2)));
