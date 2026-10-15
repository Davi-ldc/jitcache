// The corpus's scripts and how their runs start: the lane directories and their defaults, a script's directives parsed
// into a Script, the exit each run expects, and each run's command line and environment, for a jsc-hosted and a
// Bun-hosted run. A test path is a script, or a directory whose own .js and .mjs files are scripts.

import { readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, isAbsolute, join, relative, resolve, sep } from "node:path";
import { type ProcessEnd, describeEnd, repositoryRoot } from "./build.ts";

export const corpusRoot = join(repositoryRoot, "JSTests/jitcache");

// Directives are comment lines within a script's first lines.
const directiveLines = 50;

export const lanes = ["ucb", "image", "cb", "ics", "integrator"] as const;
export type Lane = (typeof lanes)[number];
export type Role = "Off" | "Producer" | "ConsumerProducer" | "Consumer" | "Maintenance";
const roles: readonly Role[] = ["Off", "Producer", "ConsumerProducer", "Consumer", "Maintenance"];
const modeFlags = { Producer: "p", ConsumerProducer: "p-c", Consumer: "c" } as const;

export const configuresJITCache = (role: Role) => role === "Producer" || role === "ConsumerProducer" || role === "Consumer";
export const produces = (role: Role) => role === "Producer" || role === "ConsumerProducer";
export const imports = (role: Role) => role === "Consumer" || role === "ConsumerProducer";

// Each lane directory's defaults: the sequence a script that declares none runs, and whether the directory's scripts
// need a twins build. In either mode every directory's runs also get --useConcurrentJIT=false unless their options set
// it.
const laneDefaults: Record<Lane, { sequence: string; twinsRequired: boolean }> = {
  ucb: { sequence: "Producer --jitcache-delta-at-exit; Consumer", twinsRequired: false },
  image: { sequence: "Producer --jitcache-delta-at-exit; Consumer", twinsRequired: false },
  cb: { sequence: "Producer --jitcache-delta-at-exit; Consumer", twinsRequired: false },
  ics: { sequence: "Producer; Consumer", twinsRequired: true },
  integrator: { sequence: "Producer --jitcache-delta-at-exit; Consumer", twinsRequired: true },
};

// The parts and the domains a twin report names, spelled as the report spells them.
export const twinParts = ["ucb", "image", "cb", "ics", "integrator"] as const;
export const twinDomains = ["engine-image", "executable-pool", "structure-reservation", "heap"] as const;

// Of the JITCache flags, those a Bun-hosted run passes to Bun and those it drops. Bun takes no other JITCache flag, so a
// Bun-hosted script whose runs name one is malformed.
const bunPassedFlags = new Set([
  "--jitcache",
  "--jitcache-mode",
  "--jitcache-strict",
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

export interface Script {
  path: string;
  display: string;
  lane: Lane | null;
  host: "jsc" | "bun";
  sequences: RunSpec[][];
  requiresTwins: boolean;
  heapOff: string | null;
  pinOff: string | null;
  expectExit: { at: RunAddress; exit: ExpectedExit }[];
  faults: { at: RunAddress; step: string; required: boolean }[];
  expectNoInstall: RunAddress[];
  expectTwin: { at: RunAddress; kind: TwinKind; subject: string }[];
  checks: { at: RunAddress; checker: string }[];
  qemuCpu: { at: RunAddress; model: string }[];
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

// The lane directory a script sits in, at any depth below it: JSTests/jitcache/<lane>/.
export function laneOf(path: string): Lane | null {
  const directories = dirname(resolve(path)).split(sep);
  for (let i = directories.length - 2; i >= 0; --i) {
    const lane = directories[i + 1] as Lane;
    if (directories[i] === "jitcache" && lanes.includes(lane)) return lane;
  }
  return null;
}

// The scripts of the test paths main checked exist; a lane whose default directory does not exist yet has no script.
export function collectScripts(paths: string[]): string[] {
  const scripts: string[] = [];
  for (const path of paths) {
    const status = statSync(path, { throwIfNoEntry: false });
    if (!status) continue;
    if (status.isFile()) {
      scripts.push(path);
      continue;
    }
    // A directory's scripts are its own .js and .mjs files; helpers and checkers sit in subdirectories or have
    // other extensions.
    for (const name of readdirSync(path).sort()) {
      const file = join(path, name);
      if (/\.m?js$/.test(name) && statSync(file).isFile()) scripts.push(file);
    }
  }
  return scripts;
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

export function parseScript(path: string): Script {
  const absolute = resolve(path);
  const lane = laneOf(absolute);
  const script: Script = {
    path: absolute,
    display: displayPath(absolute),
    lane,
    host: "jsc",
    sequences: [],
    requiresTwins: false,
    heapOff: null,
    pinOff: null,
    expectExit: [],
    faults: [],
    expectNoInstall: [],
    expectTwin: [],
    checks: [],
    qemuCpu: [],
  };
  const singletons = new Set<string>();
  const once = (name: string) => {
    if (singletons.has(name)) throw new MalformedScript(`jitcache-${name} appears twice`);
    singletons.add(name);
  };
  const lines = readFileSync(absolute, "utf8").split("\n").slice(0, directiveLines);
  lines.forEach((line, index) => {
    if (!/^\s*\/\/\s*jitcache-/.test(line)) return;
    const match = /^\s*\/\/\s*jitcache-([a-z0-9-]+):(.*)$/.exec(line);
    if (!match) throw new MalformedScript(`line ${index + 1} is no directive: ${line.trim()}`);
    const [, name, rest] = match;
    const value = rest.trim();
    const words = value.split(/\s+/).filter(Boolean);
    const arity = (count: number) => {
      if (words.length !== count) throw new MalformedScript(`jitcache-${name} takes ${count} words: ${value}`);
    };
    const reason = () => {
      if (words[0] !== "off" || words.length < 2) throw new MalformedScript(`jitcache-${name} reads off <reason>`);
      return words.slice(1).join(" ");
    };
    switch (name) {
      case "runs":
        script.sequences.push(parseSequence(value));
        break;
      case "host":
        once(name);
        if (value !== "jsc" && value !== "bun") throw new MalformedScript(`jitcache-host is jsc or bun, not ${value}`);
        script.host = value;
        break;
      case "requires":
        once(name);
        if (value !== "twins") throw new MalformedScript(`jitcache-requires takes twins, not ${value}`);
        script.requiresTwins = true;
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
        const subjects: readonly string[] = kind === "coincidence" ? twinDomains : kind === "difference" || kind === "skip" ? twinParts : [];
        if (!subjects.includes(words[2])) throw new MalformedScript(`jitcache-expect-twin cannot declare ${words[1]} ${words[2]}`);
        script.expectTwin.push({ at: parseAddress(words[0]), kind, subject: words[2] });
        break;
      }
      case "check": {
        arity(2);
        const checker = resolve(dirname(absolute), words[1]);
        if (!statSync(checker, { throwIfNoEntry: false })?.isFile()) throw new MalformedScript(`checker ${words[1]} does not exist`);
        script.checks.push({ at: parseAddress(words[0]), checker });
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
      default:
        throw new MalformedScript(`unknown directive jitcache-${name}`);
    }
  });

  if (!script.sequences.length) {
    if (!lane) throw new MalformedScript("the script declares no jitcache-runs, and its directory gives no default sequence");
    script.sequences.push(parseSequence(laneDefaults[lane].sequence));
  }
  if (lane && laneDefaults[lane].twinsRequired) script.requiresTwins = true;

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
  return script;
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

// Every lane directory's default, in either mode: --useConcurrentJIT=false unless the run's options set
// useConcurrentJIT. A producer captures only what has finalized, so with concurrent JIT on, what a fast release run
// commits depends on whether its plans finished before it exited.
function directoryDefaults(context: CommandContext, script: Script, options: string[]): string[] {
  if (!script.lane || booleanOption(options, "useConcurrentJIT") !== null) return [];
  return ["--useConcurrentJIT=false"];
}

// Whether concurrent JIT is off in a run, by the last setting its command line gives, or JSC's default, on.
export function concurrentJITOff(context: CommandContext, script: Script, options: string[]): boolean {
  const effective = booleanOption([...directoryDefaults(context, script, options), ...context.extraOptions, ...options], "useConcurrentJIT");
  return effective === false;
}

// <jsc> --destroy-vm [JITCache flags] [twins-mode flags] [directory defaults] [extra options] <options> <script> -- ...
// name is run<i> or oracle<j>, which names the files the run writes in the scratch directory.
export function jscArguments(context: CommandContext, script: Script, sequence: number, role: Role, options: string[], paths: SequencePaths, name: string): string[] {
  const { artifact, scratch } = paths;
  const args = ["--destroy-vm"];
  const jitcache = configuresJITCache(role);
  if (jitcache) args.push(`--jitcache=${artifact}`, `--jitcache-mode=${modeFlags[role as keyof typeof modeFlags]}`, "--jitcache-strict=1", "--jitcache-log");
  if (context.twins) {
    args.push("--useDollarVM=true", `--jitcache-describe-heap=${join(scratch, `${name}.heap`)}`);
    if (jitcache) {
      args.push(
        `--jitcache-twins-report=${join(scratch, `${name}.twins`)}`,
        `--jitcache-twins-record-layout=${join(scratch, `${name}.layout`)}`,
        `--jitcache-twins-avoid-layout=${paths.layouts.join(",")}`,
      );
    }
  }
  args.push(...directoryDefaults(context, script, options), ...context.extraOptions, ...options);
  args.push(script.path, "--", role, scratch, artifact, String(sequence));
  return args;
}

// <bun> <JITCache flags> <script> <role> <scratch> <artifact> <sequence>, with the JSC options as BUN_JSC_ variables.
export function bunInvocation(context: CommandContext, script: Script, sequence: number, role: Role, options: string[], paths: SequencePaths, name: string) {
  const { artifact, scratch } = paths;
  const args: string[] = [];
  if (configuresJITCache(role)) {
    // Bun calls delta at exit by default, while a run gets it only by naming --jitcache-delta-at-exit, as in the jsc
    // shell: =0 comes first, and a run's own flag, later on the command line, wins.
    args.push(`--jitcache=${artifact}`, `--jitcache-mode=${modeFlags[role as keyof typeof modeFlags]}`, "--jitcache-strict=1", "--jitcache-delta-at-exit=0");
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
