// The corpus's scripts and how their runs start: the directories' defaults, a script's directives parsed into a Script,
// the scripts a run selects and the sequences of each that run, the exit each run expects, and each run's command line
// and environment, for a jsc-hosted and a Bun-hosted run.
//
// A test path is a script, or a directory whose scripts are the .js and .mjs files below it, at any depth, outside
// every resources/ directory; the default test path is the corpus, JSTests/jitcache. A corpus script takes the defaults
// of the nearest directory between it and the corpus that holds a defaults.txt, whose lines are comments and two
// directives: the jitcache-runs lines a script that declares none runs, and jitcache-requires. Every run of a script
// that takes defaults also gets --useConcurrentJIT=false, in either mode, unless its options set useConcurrentJIT. A
// corpus script that no defaults file covers is malformed, and a script outside the corpus takes no defaults.
//
// A script declares its directives in comment lines within its first 50 lines. A <run> is a run index, which names that
// run in every sequence, or <sequence>:<run>, which names one sequence's; sequences count from 0 in the order of their
// jitcache-runs lines, and every run receives its sequence's index. A script's arguments are its role, scratch
// directory, artifact path and sequence index; the bench's native copy of a Consumer run, which imports nothing, also
// receives "native" after them, so a check that assumes an import can skip itself there.
//   jitcache-runs: [<n>x] <role> <options>; <role> <options>; ...   one sequence, or n alike after <n>x. A Maintenance
//       run takes a maintenance command line, in which $ARTIFACT stands for the sequence's artifact path.
//   jitcache-host: jsc or bun
//   jitcache-requires: twins
//   jitcache-expect-exit: <run> <exit code, abort or kill>
//   jitcache-expect-fault: <run> <step>         the run may report the fault
//   jitcache-require-fault: <run> <step>        as expect-fault, and a run it names in a sequence that ran reports it
//   jitcache-expect-no-install: <run>           the Consumer or ConsumerProducer run need not install a body
//   jitcache-expect-twin: <run> <difference or skip> <part>, or <run> coincidence <domain>
//   jitcache-check: <run> <checker> [<argument>...]   after the run, bun <checker> <stdout> <stderr> <scratch>
//       <argument>..., with the checker's path relative to the script
//   jitcache-heap: off <reason>                 the oracle compares the script's output alone
//   jitcache-pin: off <reason>                  the pin comparison skips the script
//   jitcache-qemu-cpu: <run> <model>            the run's CPU model under QEMU; the script runs only under QEMU
//   jitcache-cadence: <condition>... [<sequence> or <first>-<last>...]   the sequences it names, or every one, run only
//       when each condition holds: milestone, under --milestone or in the bench; release, in a build whose CMake build
//       type is a release one; aarch64 or x86_64, in a build of that architecture
//   jitcache-fixture: what the runner's self-test expects of a fixture (parseFixture). A fixture runs only when a test
//       path names its file.

import { existsSync, readFileSync, readdirSync, realpathSync, statSync } from "node:fs";
import { dirname, isAbsolute, join, relative, resolve } from "node:path";
import { repositoryRoot } from "./env.ts";
import { type Build, type ProcessEnd, describeEnd } from "./launcher.ts";

export const corpusRoot = join(repositoryRoot, "JSTests/jitcache");

// Directives are comment lines within a script's first lines.
const directiveLines = 50;

// The file that holds a directory's defaults.
const defaultsFileName = "defaults.txt";

export type Role = "Off" | "Producer" | "ConsumerProducer" | "Consumer" | "Maintenance";
const roles: readonly Role[] = ["Off", "Producer", "ConsumerProducer", "Consumer", "Maintenance"];
const modeFlags = { Producer: "p", ConsumerProducer: "p-c", Consumer: "c" } as const;

export const configuresJITCache = (role: Role) => role === "Producer" || role === "ConsumerProducer" || role === "Consumer";
export const produces = (role: Role) => role === "Producer" || role === "ConsumerProducer";
export const imports = (role: Role) => role === "Consumer" || role === "ConsumerProducer";

// The domains a twin report names, spelled as the report spells them. A difference or a skip names its part instead,
// and any lane may add one, so a part is any word.
export const twinDomains = ["engine-image", "executable-pool", "structure-reservation", "heap"] as const;

// The conditions a cadence states, and the CMake build types the release condition accepts.
export type CadenceCondition = "milestone" | "release" | "aarch64" | "x86_64";
const cadenceConditions: readonly CadenceCondition[] = ["milestone", "release", "aarch64", "x86_64"];
const releaseBuildTypes = ["Release", "RelWithDebInfo", "MinSizeRel"];

// Of the JITCache flags, those a Bun-hosted run passes to Bun and those it drops. Bun takes no other JITCache flag, so a
// Bun-hosted script whose runs name one is malformed.
const bunPassedFlags = new Set([
  "--jitcache",
  "--jitcache-mode",
  "--jitcache-delta-capture",
  "--jitcache-delta-at-exit",
  "--jitcache-bench-report",
  "--jitcache-twins-report",
  "--jitcache-twins-record-layout",
  "--jitcache-twins-avoid-layout",
  "--jitcache-body-events",
]);
const bunDroppedFlags = new Set(["--jitcache-log", "--jitcache-describe-heap"]);

// ---------------------------------------------------------------------------------------------------------------------
// Scripts and directives

export type ExpectedExit = number | "abort" | "kill";
export type TwinKind = "difference" | "skip" | "coincidence";

// A <run index>, alone naming that run of every sequence, or <sequence>:<run> naming one sequence's.
export interface RunAddress {
  sequence: number | null;
  run: number;
}

export interface RunSpec {
  role: Role;
  options: string[]; // a Maintenance run's maintenance command line instead
}

// A failure the runner's self-test expects of a fixture, by sequence, attempt, run (null for the sequence as a whole)
// and check.
export interface FixtureFailure {
  sequence: number;
  run: number | null;
  check: string;
  attempt: number;
}

// What the runner's self-test expects when the runner runs a fixture.
export interface Fixture {
  outcome: "passes" | "fails" | "skipped";
  failures: FixtureFailure[]; // exactly the failures a fixture that fails shows
  skipReason: RegExp | null; // what a skipped fixture's reason says
  lists: RegExp[]; // notes the runner must list, in a sequence's notes or the summary's
  details: RegExp[]; // what the detail of some failure says
  runner: string[]; // the runner options the self-test adds, with $BUILD for the build it tests
  wide: boolean; // the fixture's sequences run with the self-test case's whole width
}

export interface Script {
  path: string;
  display: string;
  defaults: string | null; // the defaults file the script takes, null outside the corpus
  host: "jsc" | "bun";
  sequences: RunSpec[][];
  cadence: Set<CadenceCondition>[]; // by sequence, the conditions under which it runs
  requiresTwins: boolean;
  heapOff: string | null;
  pinOff: string | null;
  expectExit: { at: RunAddress; exit: ExpectedExit }[];
  faults: { at: RunAddress; step: string; required: boolean }[];
  expectNoInstall: RunAddress[];
  expectTwin: { at: RunAddress; kind: TwinKind; subject: string }[];
  checks: { at: RunAddress; checker: string; arguments: string[] }[];
  qemuCpu: { at: RunAddress; model: string }[];
  fixture: Fixture | null; // what the self-test expects of a fixture of the runner
}

export class MalformedScript extends Error {}

export const addresses = (at: RunAddress, sequence: number, run: number) =>
  at.run === run && (at.sequence === null || at.sequence === sequence);

export function displayPath(path: string): string {
  const inCorpus = relative(corpusRoot, path);
  if (!inCorpus.startsWith("..") && !isAbsolute(inCorpus)) return inCorpus;
  const inWorkingDirectory = relative(process.cwd(), path);
  return inWorkingDirectory.startsWith("..") || isAbsolute(inWorkingDirectory) ? path : inWorkingDirectory;
}

// One directive: the line it is on, from 1, its name after jitcache-, its value and the value's words.
interface Directive {
  line: number;
  name: string;
  value: string;
  words: string[];
}

function directivesOf(lines: string[]): Directive[] {
  const directives: Directive[] = [];
  lines.forEach((line, index) => {
    if (!/^\s*\/\/\s*jitcache-/.test(line)) return;
    const match = /^\s*\/\/\s*jitcache-([a-z0-9-]+):(.*)$/.exec(line);
    if (!match) throw new MalformedScript(`line ${index + 1} is no directive: ${line.trim()}`);
    const value = match[2].trim();
    directives.push({ line: index + 1, name: match[1], value, words: value.split(/\s+/).filter(Boolean) });
  });
  return directives;
}

// ---------------------------------------------------------------------------------------------------------------------
// Directory defaults and the scripts of test paths

interface DirectoryDefaults {
  file: string;
  sequences: RunSpec[][]; // what a script that declares no jitcache-runs runs
  requiresTwins: boolean;
}

const requiresTwins = (value: string) => {
  if (value !== "twins") throw new MalformedScript(`jitcache-requires takes twins, not ${value}`);
  return true;
};

function readDefaults(file: string): DirectoryDefaults {
  const defaults: DirectoryDefaults = { file, sequences: [], requiresTwins: false };
  try {
    const lines = readFileSync(file, "utf8").split("\n");
    lines.forEach((line, index) => {
      if (line.trim() && !/^\s*\/\//.test(line)) throw new MalformedScript(`line ${index + 1} is no comment: ${line.trim()}`);
    });
    for (const { line, name, value } of directivesOf(lines)) {
      if (name === "runs") defaults.sequences.push(...parseSequences(value));
      else if (name === "requires" && !defaults.requiresTwins) defaults.requiresTwins = requiresTwins(value);
      else if (name === "requires") throw new MalformedScript("jitcache-requires appears twice");
      else throw new MalformedScript(`line ${line} holds jitcache-${name}, and a defaults file holds jitcache-runs and jitcache-requires only`);
    }
  } catch (error) {
    if (!(error instanceof MalformedScript)) throw error;
    throw new MalformedScript(`its defaults file ${displayPath(file)} is malformed: ${error.message}`);
  }
  return defaults;
}

// The corpus's real path, which the defaults lookup compares with a script's, so that a script a test path reaches
// through a symbolic link, such as ~/bun/vendor/WebKit, keeps its directory's defaults.
let realCorpusRoot: string | null = null;
const realCorpus = () => (realCorpusRoot ??= existsSync(corpusRoot) ? realpathSync(corpusRoot) : corpusRoot);

// The defaults of a script: those of the nearest directory between it and the corpus that holds a defaults file. A
// script outside the corpus takes none.
function defaultsOf(path: string): DirectoryDefaults | null {
  const root = realCorpus();
  let directory = realpathSync(dirname(path));
  const inCorpus = relative(root, directory);
  if (inCorpus.startsWith("..") || isAbsolute(inCorpus)) return null;
  for (;;) {
    const file = join(directory, defaultsFileName);
    if (existsSync(file)) return readDefaults(file);
    if (directory === root) throw new MalformedScript(`no directory between the script and JSTests/jitcache holds a ${defaultsFileName}`);
    directory = dirname(directory);
  }
}

// The scripts of the test paths main checked exist, each once and with whether a test path names its file: a path that
// names a file, and the .js and .mjs files below a directory, at any depth, outside every resources/ directory, which
// holds what scripts load, import or check with.
function collectScripts(paths: string[]): { path: string; named: boolean }[] {
  const scripts = new Map<string, boolean>();
  const below = (directory: string): string[] =>
    readdirSync(directory, { withFileTypes: true }).flatMap(entry => {
      const path = join(directory, entry.name);
      if (entry.isDirectory()) return entry.name === "resources" ? [] : below(path);
      return entry.isFile() && /\.m?js$/.test(entry.name) ? [path] : [];
    });
  for (const path of paths) {
    const status = statSync(path, { throwIfNoEntry: false });
    if (status?.isFile()) scripts.set(path, true);
    else if (status) for (const script of below(path).sort()) scripts.set(script, scripts.get(script) ?? false);
  }
  return [...scripts].map(([path, named]) => ({ path, named }));
}

// A jitcache-runs line's sequences: n alike when the line starts with <n>x, one otherwise.
function parseSequences(text: string): RunSpec[][] {
  const repeated = /^(\d+)x\s+(.*)$/.exec(text);
  if (!repeated) return [parseSequence(text)];
  const count = Number(repeated[1]);
  if (count < 1) throw new MalformedScript(`jitcache-runs repeats its sequence ${repeated[1]} times`);
  return Array.from({ length: count }, () => parseSequence(repeated[2]));
}

function parseSequence(text: string): RunSpec[] {
  const runs = text.split(";").map(run => run.trim());
  if (runs.some(run => !run)) throw new MalformedScript(`jitcache-runs holds an empty run: ${text}`);
  return runs.map(run => {
    const [role, ...options] = run.split(/\s+/);
    if (!roles.includes(role as Role)) throw new MalformedScript(`unknown role ${role} in jitcache-runs`);
    if (role === "Maintenance") {
      if (!options.length) throw new MalformedScript("a Maintenance run needs a maintenance command line");
      return { role: "Maintenance" as Role, options };
    }
    for (const option of options) {
      if (!option.startsWith("--")) throw new MalformedScript(`run option ${option} is no --option`);
    }
    return { role: role as Role, options };
  });
}

function parseAddress(text: string): RunAddress {
  const match = /^(?:(\d+):)?(\d+)$/.exec(text);
  if (!match) throw new MalformedScript(`${text} is no run index`);
  return { sequence: match[1] === undefined ? null : Number(match[1]), run: Number(match[2]) };
}

function parseExpectedExit(text: string): ExpectedExit {
  if (text === "abort" || text === "kill") return text;
  if (!/^\d+$/.test(text) || Number(text) > 255) throw new MalformedScript(`${text} is no exit code, abort or kill`);
  return Number(text);
}

// A jitcache-cadence directive: its conditions, and the ranges of sequences it names, none when it names every one.
function parseCadence(words: string[]): { conditions: CadenceCondition[]; ranges: [number, number][] } {
  const conditions: CadenceCondition[] = [];
  const ranges: [number, number][] = [];
  for (const word of words) {
    const range = /^(\d+)(?:-(\d+))?$/.exec(word);
    if (range) {
      const first = Number(range[1]);
      const last = range[2] === undefined ? first : Number(range[2]);
      if (last < first) throw new MalformedScript(`jitcache-cadence names the empty range ${word}`);
      ranges.push([first, last]);
    } else if (cadenceConditions.includes(word as CadenceCondition)) {
      conditions.push(word as CadenceCondition);
    } else {
      throw new MalformedScript(`jitcache-cadence takes ${cadenceConditions.join(", ")} and sequences, not ${word}`);
    }
  }
  if (!conditions.length) throw new MalformedScript("jitcache-cadence states no condition");
  return { conditions, ranges };
}

// A fixture's jitcache-fixture lines, each of which reads:
//   passes, or fails <sequence>[:<run>] <check> [attempt <n>], or skipped <regex>: the outcome, with one fails line per
//       failure, the run left out for a failure of the whole sequence, such as the pin comparison's
//   lists <regex>: a note the runner lists
//   detail <regex>: the detail of some failure
//   runner <option>...: options the self-test gives the runner, in which $BUILD stands for the build it tests
//   wide: the fixture's sequences run with the self-test case's whole width
function parseFixture(directives: Directive[]): Fixture | null {
  if (!directives.length) return null;
  const fixture: Fixture = { outcome: "passes", failures: [], skipReason: null, lists: [], details: [], runner: [], wide: false };
  const outcomes = new Set<Fixture["outcome"]>();
  const pattern = (form: string, text: string) => {
    try {
      if (text) return new RegExp(text);
    } catch (error) {
      throw new MalformedScript(`jitcache-fixture: ${form} holds no regular expression: ${(error as Error).message}`);
    }
    throw new MalformedScript(`jitcache-fixture: ${form} takes a regular expression`);
  };
  for (const { value, words } of directives) {
    const form = words[0] ?? "";
    const rest = value.slice(form.length).trim();
    switch (form) {
      case "passes":
      case "skipped":
        if (outcomes.has(form)) throw new MalformedScript(`jitcache-fixture: ${form} appears twice`);
        if (form === "passes" && words.length !== 1) throw new MalformedScript(`jitcache-fixture: passes takes no words: ${value}`);
        if (form === "skipped") fixture.skipReason = pattern(form, rest);
        outcomes.add(form);
        break;
      case "fails": {
        const address = /^(\d+)(?::(\d+))?$/.exec(words[1] ?? "");
        const attempt = words.length === 3 ? 1 : words.length === 5 && words[3] === "attempt" && /^[1-9]\d*$/.test(words[4]) ? Number(words[4]) : null;
        if (!address || attempt === null) throw new MalformedScript(`jitcache-fixture: fails takes <sequence>[:<run>] <check> [attempt <n>]: ${value}`);
        fixture.failures.push({ sequence: Number(address[1]), run: address[2] === undefined ? null : Number(address[2]), check: words[2], attempt });
        outcomes.add(form);
        break;
      }
      case "lists":
        fixture.lists.push(pattern(form, rest));
        break;
      case "detail":
        fixture.details.push(pattern(form, rest));
        break;
      case "runner":
        if (words.length < 2 || words.slice(1).some(option => !option.startsWith("--"))) throw new MalformedScript(`jitcache-fixture: runner takes --options: ${value}`);
        fixture.runner.push(...words.slice(1));
        break;
      case "wide":
        if (words.length !== 1) throw new MalformedScript(`jitcache-fixture: wide takes no words: ${value}`);
        fixture.wide = true;
        break;
      default:
        throw new MalformedScript(`jitcache-fixture: unknown form ${form}`);
    }
  }
  if (outcomes.size !== 1) throw new MalformedScript(`jitcache-fixture declares ${outcomes.size ? [...outcomes].join(" and ") : "no outcome"}, and a fixture declares one: passes, fails or skipped`);
  fixture.outcome = [...outcomes][0];
  if (fixture.details.length && fixture.outcome !== "fails") throw new MalformedScript("jitcache-fixture: detail describes a failure, and the fixture declares none");
  return fixture;
}

export function parseScript(path: string): Script {
  const absolute = resolve(path);
  const defaults = defaultsOf(absolute);
  const script: Script = {
    path: absolute,
    display: displayPath(absolute),
    defaults: defaults?.file ?? null,
    host: "jsc",
    sequences: [],
    cadence: [],
    requiresTwins: false,
    heapOff: null,
    pinOff: null,
    expectExit: [],
    faults: [],
    expectNoInstall: [],
    expectTwin: [],
    checks: [],
    qemuCpu: [],
    fixture: null,
  };
  const singletons = new Set<string>();
  const once = (name: string) => {
    if (singletons.has(name)) throw new MalformedScript(`jitcache-${name} appears twice`);
    singletons.add(name);
  };
  const cadences: ReturnType<typeof parseCadence>[] = [];
  const fixtureLines: Directive[] = [];
  for (const directive of directivesOf(readFileSync(absolute, "utf8").split("\n").slice(0, directiveLines))) {
    const { name, value, words } = directive;
    const arity = (count: number) => {
      if (words.length !== count) throw new MalformedScript(`jitcache-${name} takes ${count} words: ${value}`);
    };
    const reason = () => {
      if (words[0] !== "off" || words.length < 2) throw new MalformedScript(`jitcache-${name} reads off <reason>`);
      return words.slice(1).join(" ");
    };
    switch (name) {
      case "runs":
        script.sequences.push(...parseSequences(value));
        break;
      case "host":
        once(name);
        if (value !== "jsc" && value !== "bun") throw new MalformedScript(`jitcache-host is jsc or bun, not ${value}`);
        script.host = value;
        break;
      case "requires":
        once(name);
        script.requiresTwins = requiresTwins(value);
        break;
      case "expect-exit":
        arity(2);
        script.expectExit.push({ at: parseAddress(words[0]), exit: parseExpectedExit(words[1]) });
        break;
      case "expect-fault":
      case "require-fault":
        arity(2);
        script.faults.push({ at: parseAddress(words[0]), step: words[1], required: name === "require-fault" });
        break;
      case "expect-no-install":
        arity(1);
        script.expectNoInstall.push(parseAddress(words[0]));
        break;
      case "expect-twin": {
        arity(3);
        const kind = words[1] as TwinKind;
        const declarable = kind === "coincidence" ? (twinDomains as readonly string[]).includes(words[2]) : (kind === "difference" || kind === "skip") && /^[a-z][a-z0-9-]*$/.test(words[2]);
        if (!declarable) throw new MalformedScript(`jitcache-expect-twin cannot declare ${words[1]} ${words[2]}`);
        script.expectTwin.push({ at: parseAddress(words[0]), kind, subject: words[2] });
        break;
      }
      case "check": {
        if (words.length < 2) throw new MalformedScript(`jitcache-check takes a run index, a checker and the checker's arguments: ${value}`);
        const checker = resolve(dirname(absolute), words[1]);
        if (!statSync(checker, { throwIfNoEntry: false })?.isFile()) throw new MalformedScript(`checker ${words[1]} does not exist`);
        script.checks.push({ at: parseAddress(words[0]), checker, arguments: words.slice(2) });
        break;
      }
      case "heap":
        once(name);
        script.heapOff = reason();
        break;
      case "pin":
        once(name);
        script.pinOff = reason();
        break;
      case "qemu-cpu":
        arity(2);
        script.qemuCpu.push({ at: parseAddress(words[0]), model: words[1] });
        break;
      case "cadence":
        cadences.push(parseCadence(words));
        break;
      case "fixture":
        fixtureLines.push(directive);
        break;
      default:
        throw new MalformedScript(`unknown directive jitcache-${name}`);
    }
  }

  if (!script.sequences.length) {
    if (!defaults?.sequences.length) throw new MalformedScript(`the script declares no jitcache-runs, and ${defaults ? `${displayPath(defaults.file)} gives no default sequence` : "it takes no defaults file"}`);
    script.sequences.push(...defaults.sequences);
  }
  if (defaults?.requiresTwins) script.requiresTwins = true;

  // Each sequence's conditions, from every cadence that names it.
  script.cadence = script.sequences.map(() => new Set<CadenceCondition>());
  for (const { conditions, ranges } of cadences) {
    const beyond = ranges.find(([, last]) => last >= script.sequences.length);
    if (beyond) throw new MalformedScript(`jitcache-cadence names sequence ${beyond[1]}, and the script has ${script.sequences.length}`);
    script.cadence.forEach((set, k) => {
      if (!ranges.length || ranges.some(([first, last]) => first <= k && k <= last)) for (const condition of conditions) set.add(condition);
    });
  }
  if (script.cadence.some(set => set.has("aarch64") && set.has("x86_64"))) throw new MalformedScript("jitcache-cadence asks one sequence for both architectures");
  script.fixture = parseFixture(fixtureLines);

  const named = (at: RunAddress) =>
    script.sequences.flatMap((runs, k) => runs.flatMap((run, i) => (addresses(at, k, i) ? [run] : [])));
  const allAddresses = [
    ...script.expectExit.map(e => e.at),
    ...script.faults.map(f => f.at),
    ...script.expectNoInstall,
    ...script.expectTwin.map(t => t.at),
    ...script.checks.map(c => c.at),
    ...script.qemuCpu.map(q => q.at),
  ];
  for (const at of allAddresses) {
    if (!named(at).length) throw new MalformedScript(`run ${at.sequence === null ? "" : `${at.sequence}:`}${at.run} names no run`);
  }
  for (const at of script.expectNoInstall) {
    if (named(at).some(run => !imports(run.role))) throw new MalformedScript("jitcache-expect-no-install names a run that is neither a Consumer nor a ConsumerProducer");
  }
  for (const twin of script.expectTwin) {
    if (named(twin.at).some(run => !configuresJITCache(run.role))) throw new MalformedScript("jitcache-expect-twin names a run without JITCache, which writes no twin report");
  }
  for (const { sequence, run } of script.fixture?.failures ?? []) {
    const exists = run === null ? sequence < script.sequences.length : named({ sequence, run }).length > 0;
    if (!exists) throw new MalformedScript(`jitcache-fixture: fails names ${sequence}${run === null ? "" : `:${run}`}, which is no sequence or run of the script`);
  }
  return script;
}

// The scripts of the test paths, or of the corpus when none is given, whose display path the filter matches: each
// parsed, or the reason it is malformed. A fixture of the runner's self-test is left out unless a test path names it.
export function selectScripts(testPaths: string[], filter: RegExp | null): { path: string; display: string; script: Script | string }[] {
  const selected: { path: string; display: string; script: Script | string }[] = [];
  for (const { path, named } of collectScripts(testPaths.length ? testPaths : [corpusRoot])) {
    const display = displayPath(path);
    if (filter && !filter.test(display)) continue;
    try {
      const script = parseScript(path);
      if (named || !script.fixture) selected.push({ path, display, script });
    } catch (error) {
      if (!(error instanceof MalformedScript)) throw error;
      selected.push({ path, display, script: error.message });
    }
  }
  return selected;
}

// What a cadence condition depends on: whether the milestone sequences run, and the build.
export interface CadenceContext {
  readonly milestone: boolean;
  readonly build: Build;
}

// The sequences of a script that run in a context. Each group of the others gets a note, such as "sequences 1-14, 16-39
// run only with --milestone", and when none runs, reason says why, as a skipped script's reason. awaitingMilestone
// counts the sequences that --milestone alone would run.
export interface SequenceSelection {
  selected: number[];
  skipped: string[];
  reason: string | null;
  awaitingMilestone: number;
}

export function selectSequences(script: Script, context: CadenceContext): SequenceSelection {
  const met = (condition: CadenceCondition) =>
    condition === "milestone" ? context.milestone : condition === "release" ? releaseBuildTypes.includes(context.build.buildType) : context.build.architecture === condition;
  const selected: number[] = [];
  const waiting = new Map<string, number[]>(); // the sequences that do not run, by the cadence they wait for
  let awaitingMilestone = 0;
  script.cadence.forEach((conditions, k) => {
    const unmet = [...conditions].filter(condition => !met(condition));
    if (!unmet.length) {
      selected.push(k);
      return;
    }
    if (unmet.length === 1 && unmet[0] === "milestone") ++awaitingMilestone;
    const builds = (["release", "aarch64", "x86_64"] as const).filter(condition => conditions.has(condition));
    const only = `only${conditions.has("milestone") ? " with --milestone" : ""}${builds.length ? ` on ${builds.join(" ")} builds` : ""}`;
    waiting.set(only, [...(waiting.get(only) ?? []), k]);
  });
  const skipped = [...waiting].map(([only, sequences]) => `${describeSequences(sequences)} ${sequences.length === 1 ? "runs" : "run"} ${only}`);
  const reason = selected.length ? null : waiting.size === 1 ? `runs ${[...waiting.keys()][0]}` : skipped.join("; ");
  return { selected, skipped, reason, awaitingMilestone };
}

// Sequence numbers as a note writes them, such as "sequence 2" or "sequences 1-14, 16-39".
function describeSequences(sequences: number[]): string {
  const ranges: string[] = [];
  for (let i = 0; i < sequences.length; ) {
    let j = i;
    while (j + 1 < sequences.length && sequences[j + 1] === sequences[j] + 1) ++j;
    ranges.push(j > i ? `${sequences[i]}-${sequences[j]}` : `${sequences[i]}`);
    i = j + 1;
  }
  return `${sequences.length === 1 ? "sequence" : "sequences"} ${ranges.join(", ")}`;
}

// The option problems that make a Bun-hosted script malformed, given the --extra-options of this run.
export function bunOptionProblem(script: Script, extraOptions: string[]): string | null {
  if (script.host !== "bun") return null;
  for (const runs of script.sequences) {
    for (const run of runs) {
      if (run.role === "Maintenance") continue;
      for (const option of [...extraOptions, ...run.options]) {
        const name = flagName(option);
        if (name.startsWith("--jitcache")) {
          if (!bunPassedFlags.has(name) && !bunDroppedFlags.has(name)) return `Bun takes no ${name}`;
        } else if (!option.includes("=")) {
          return `${option} has no value to pass to Bun as BUN_JSC_${option.slice(2)}`;
        }
      }
    }
  }
  return null;
}

export const flagName = (option: string) => {
  const equals = option.indexOf("=");
  return equals < 0 ? option : option.slice(0, equals);
};

// The last value an option list gives a boolean JSC option, or null when none sets it.
function booleanOption(options: string[], name: string): boolean | null {
  let value: boolean | null = null;
  for (const option of options) {
    if (option === `--${name}`) value = true;
    else if (option.startsWith(`--${name}=`)) value = /^(true|yes|1)$/i.test(option.slice(name.length + 3));
  }
  return value;
}

export const expectedExit = (script: Script, sequence: number, run: number): ExpectedExit =>
  script.expectExit.findLast(e => addresses(e.at, sequence, run))?.exit ?? 0;

export const endsBySignal = (exit: ExpectedExit) => exit === "abort" || exit === "kill";

export function exitProblem(end: ProcessEnd, expected: ExpectedExit): string | null {
  if (end.error || end.timedOut) return describeEnd(end);
  const signal = expected === "abort" ? "SIGABRT" : expected === "kill" ? "SIGKILL" : null;
  if (signal ? end.signal === signal : end.code === expected && !end.signal) return null;
  return `${describeEnd(end)}, expected ${signal ? `to end by ${signal}` : `to exit with ${expected}`}`;
}

// ---------------------------------------------------------------------------------------------------------------------
// Run command lines

// What a run's command line depends on beyond the script and the run.
export interface CommandContext {
  readonly twins: boolean;
  readonly extraOptions: string[];
  readonly mallocForceEnabled: boolean; // the twins-mode calibration chose WebKitMallocForceEnabled=1
}

// The environment of every jsc or Bun process; twins-mode processes get the calibration's variable.
export function engineEnvironment(context: CommandContext): Record<string, string | undefined> {
  return context.mallocForceEnabled ? { ...process.env, WebKitMallocForceEnabled: "1" } : { ...process.env };
}

export interface SequencePaths {
  artifact: string;
  scratch: string;
  layouts: string[]; // the layout files earlier runs of the sequence wrote
}

// The default of every script that takes a directory's defaults, in either mode: --useConcurrentJIT=false unless the
// run's options set useConcurrentJIT. A producer captures only what has finalized, so with concurrent JIT on, what a
// fast release run commits depends on whether its plans finished before it exited.
function directoryDefaults(context: CommandContext, script: Script, options: string[]): string[] {
  if (!script.defaults || booleanOption(options, "useConcurrentJIT") !== null) return [];
  return ["--useConcurrentJIT=false"];
}

// Whether concurrent JIT is off in a run, by the last setting its command line gives, or JSC's default, on.
export function concurrentJITOff(context: CommandContext, script: Script, options: string[]): boolean {
  const effective = booleanOption([...directoryDefaults(context, script, options), ...context.extraOptions, ...options], "useConcurrentJIT");
  return effective === false;
}

// What a caller adds to a jsc run's command line.
export interface CommandExtras {
  benchReport?: string; // the bench's --jitcache-bench-report, after --jitcache-log, for a run that configures JITCache
  scriptArguments?: string[]; // after the sequence index, as the bench's native copy passes "native"
  describeHeap?: boolean; // false leaves out twins mode's --jitcache-describe-heap, as the pin comparison's runs do
  overrides?: string[]; // options after the run's own, which override them
}

// <jsc> --destroy-vm [JITCache flags] [twins-mode flags] [directory defaults] [extra options] <options> [overrides]
// <script> -- <role> <scratch> <artifact> <sequence> [script arguments]. name is run<i> or oracle<j>, which names the
// files the run writes in the scratch directory.
export function jscArguments(context: CommandContext, script: Script, sequence: number, role: Role, options: string[], paths: SequencePaths, name: string, extras: CommandExtras = {}): string[] {
  const { artifact, scratch } = paths;
  const args = ["--destroy-vm"];
  const jitcache = configuresJITCache(role);
  if (jitcache) {
    args.push(`--jitcache=${artifact}`, `--jitcache-mode=${modeFlags[role as keyof typeof modeFlags]}`, "--jitcache-log");
    if (extras.benchReport) args.push(`--jitcache-bench-report=${extras.benchReport}`);
  }
  if (context.twins) {
    args.push("--useDollarVM=true");
    if (extras.describeHeap !== false) args.push(`--jitcache-describe-heap=${join(scratch, `${name}.heap`)}`);
    if (jitcache) {
      args.push(
        `--jitcache-twins-report=${join(scratch, `${name}.twins`)}`,
        `--jitcache-twins-record-layout=${join(scratch, `${name}.layout`)}`,
        `--jitcache-twins-avoid-layout=${paths.layouts.join(",")}`,
      );
    }
  }
  args.push(...directoryDefaults(context, script, options), ...context.extraOptions, ...options, ...(extras.overrides ?? []));
  args.push(script.path, "--", role, scratch, artifact, String(sequence), ...(extras.scriptArguments ?? []));
  return args;
}

// <bun> <JITCache flags> <script> <role> <scratch> <artifact> <sequence>, with the JSC options as BUN_JSC_ variables.
export function bunInvocation(context: CommandContext, script: Script, sequence: number, role: Role, options: string[], paths: SequencePaths, name: string) {
  const { artifact, scratch } = paths;
  const args: string[] = [];
  if (configuresJITCache(role)) {
    // Bun calls delta at exit by default, while a run gets it only by naming --jitcache-delta-at-exit, as in the jsc
    // shell: =0 comes first, and a run's own flag, later on the command line, wins.
    args.push(`--jitcache=${artifact}`, `--jitcache-mode=${modeFlags[role as keyof typeof modeFlags]}`, "--jitcache-delta-at-exit=0");
    if (context.twins) {
      args.push(
        `--jitcache-twins-report=${join(scratch, `${name}.twins`)}`,
        `--jitcache-twins-record-layout=${join(scratch, `${name}.layout`)}`,
        `--jitcache-twins-avoid-layout=${paths.layouts.join(",")}`,
      );
    }
  }
  // BUN_DESTRUCT_VM_ON_EXIT=1 is --destroy-vm's counterpart, and BUN_JSC_useConcurrentJIT=0 below the directory's
  // default. BUN_DEBUG_QUIET_LOGS=1 turns off the scoped logs a debug Bun writes to standard output, as Bun's own test
  // harness does: they name descriptors, pids, addresses and the process's own output file, which differ between any
  // two processes, so the oracle would compare them rather than what the script prints. The apps a script starts
  // inherit it, a release Bun ignores it, and a BUN_DEBUG_<tag> or BUN_DEBUG_ALL the caller exports still wins.
  const env: Record<string, string | undefined> = { ...engineEnvironment(context), BUN_DESTRUCT_VM_ON_EXIT: "1", BUN_DEBUG_QUIET_LOGS: "1" };
  if (directoryDefaults(context, script, options).length) env.BUN_JSC_useConcurrentJIT = "0";
  // Bun hands a BUN_JSC_ variable that Options::setOption rejects to onCrash, so no JITCache flag goes that way.
  for (const option of [...context.extraOptions, ...options]) {
    const flag = flagName(option);
    if (flag.startsWith("--jitcache")) {
      if (bunPassedFlags.has(flag)) args.push(option);
      continue;
    }
    env[`BUN_JSC_${flag.slice(2)}`] = option.slice(flag.length + 1);
  }
  args.push(script.path, role, scratch, artifact, String(sequence));
  return { args, env };
}
