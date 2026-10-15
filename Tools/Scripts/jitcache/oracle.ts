// What a run is compared with. The oracle is a JITCache-off run of each distinct option set among a sequence's runs that
// configure JITCache: each of those runs must print what its oracle printed, leak no object its oracle did not, and in
// twins mode leave the same reachable heap. With --pin, each option set's Off run also runs once on this build and once
// on the pin's, and pin-compare.ts compares the code the two emitted. runs.ts starts these runs after each attempt's own
// (runOracle, runPinComparison); the self-tests call comparePinOptionSet directly.

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { type Build, type Launcher, type ProcessIO, type StackSizes, describeEnd, readBytes, readText } from "./build.ts";
import {
  type Architecture,
  type EngineImage,
  type PinComparison,
  type RunCapture,
  compareRuns,
  loadEngineImage,
  pinRunOptions,
  readJITDump,
} from "./pin-compare.ts";
import { type CommandContext, type RunSpec, type Script, type SequencePaths, configuresJITCache, engineEnvironment, jscArguments } from "./scripts.ts";

// The oracle's option sets, numbered j in this order, each a run's options without the JITCache ones and the runs that
// configure JITCache with them.
export function oracleOptionSets(runs: RunSpec[]): { options: string[]; runs: number[] }[] {
  const sets = new Map<string, { options: string[]; runs: number[] }>();
  runs.forEach((run, i) => {
    if (!configuresJITCache(run.role)) return;
    const options = run.options.filter(option => !option.startsWith("--jitcache"));
    const key = JSON.stringify(options);
    if (!sets.has(key)) sets.set(key, { options, runs: [] });
    sets.get(key)!.runs.push(i);
  });
  return [...sets.values()];
}

// Where two outputs first differ, by line.
export function firstDifference(a: Buffer, b: Buffer): string {
  const left = a.toString("utf8").split("\n");
  const right = b.toString("utf8").split("\n");
  const clip = (line: string | undefined) => (line === undefined ? "(end of output)" : JSON.stringify(line.length > 160 ? `${line.slice(0, 160)}…` : line));
  for (let line = 0; line < Math.max(left.length, right.length); ++line) {
    if (left[line] !== right[line]) return `at line ${line + 1}: ${clip(left[line])}, oracle ${clip(right[line])}`;
  }
  return "in bytes that are not text";
}

// ---------------------------------------------------------------------------------------------------------------------
// Leaks

// The objects LeakSanitizer reported leaked at a process's exit. A record is a header, "Direct leak of 52 byte(s) in 2
// object(s) allocated from:", then one line per frame of the allocation stack, "#0 0x2e7f104 in malloc ...", then, under
// report_objects=1, "Objects leaked above:" and one "0x701a793f5340 (26 bytes)" line per object. LeakSanitizer merges
// every object leaked from one stack into one record and prints their sum, so the runner compares objects: each keeps
// its own size and its record's frames without their program counters, which address randomization moves between
// processes; each frame keeps its function and source line, or its module and offset. A record without its object lines
// counts as one object of its total.
export interface LeakedObject {
  bytes: number;
  stack: string;
}

export function leakedObjects(stderr: string): LeakedObject[] {
  const objects: LeakedObject[] = [];
  const lines = stderr.split("\n");
  for (let n = 0; n < lines.length; ++n) {
    const header = /^(?:Direct|Indirect) leak of (\d+) byte\(s\) in \d+ object\(s\)/.exec(lines[n]);
    if (!header) continue;
    const frames: string[] = [];
    while (/^\s+#\d+ /.test(lines[n + 1] ?? "")) frames.push(lines[++n].trim().replace(/^(#\d+) 0x[0-9a-f]+/, "$1"));
    const stack = frames.join("\n");
    const sizes: number[] = [];
    let m = n + 1;
    while (lines[m] === "") ++m;
    if (lines[m] === "Objects leaked above:") {
      let size: RegExpExecArray | null;
      while ((size = /^0x[0-9a-f]+ \((\d+) bytes\)$/.exec(lines[m + 1] ?? ""))) {
        sizes.push(Number(size[1]));
        ++m;
      }
      n = m;
    }
    for (const bytes of sizes.length ? sizes : [Number(header[1])]) objects.push({ bytes, stack });
  }
  return objects;
}

// The objects a run leaked that its oracle did not, each paired with at most one oracle object of the same size and
// stack. A leak the oracle reports too is the engine's own.
export function leaksBeyond(leaked: LeakedObject[], oracle: LeakedObject[]): LeakedObject[] {
  const key = ({ bytes, stack }: LeakedObject) => `${bytes}\n${stack}`;
  const left = new Map<string, number>();
  for (const object of oracle) left.set(key(object), (left.get(key(object)) ?? 0) + 1);
  return leaked.filter(object => {
    const count = left.get(key(object)) ?? 0;
    if (count) left.set(key(object), count - 1);
    return !count;
  });
}

export const describeLeaks = (leaks: LeakedObject[]) =>
  `${leaks.length} object(s), the first ${leaks[0].bytes} bytes allocated from ${leaks[0].stack.split("\n").slice(0, 8).join(" <- ")}`;

// ---------------------------------------------------------------------------------------------------------------------
// The pin comparison of one option set

// What the pin comparison starts its two runs with: this build's side and the pin's.
export interface PinSetup {
  readonly context: CommandContext;
  readonly build: Build;
  readonly launcher: Launcher;
  readonly pin: Build;
  readonly pinLauncher: Launcher;
  readonly pinOptions: string[]; // --pin-options, which this build's side alone takes
}

// The oracle run's command line without --jitcache-describe-heap, with the comparison's options last, so they override
// the run's own.
export function pinArguments(context: CommandContext, script: Script, sequence: number, options: string[], paths: SequencePaths, name: string, comparisonOptions: string[]): string[] {
  const args = jscArguments(context, script, sequence, "Off", options, paths, name).filter(arg => !arg.startsWith("--jitcache-describe-heap="));
  // jscArguments ends with <script> -- <role> <scratch> <artifact> <sequence>.
  args.splice(args.length - 6, 0, ...comparisonOptions);
  return args;
}

// Runs option set j of a sequence with this build's jsc and with the pin's, keeping each side's status, output, error and
// JIT dump as <scratch>/pin<j>.<this|pin>.*, which pin-compare.ts also reads when run alone, and compares them. Each side
// whose build is a twins build runs with --jitcache-test-no-shell-functions, so its global object has the pin shell's
// properties; the side's build decides, not the session's mode, because a twins build registers the shell functions in
// either mode and a plain build rejects the flag.
export async function comparePinOptionSet(setup: PinSetup, script: Script, sequence: number, options: string[], paths: SequencePaths, j: number, stacks: StackSizes | null): Promise<PinComparison> {
  const sides = [
    { side: "this", label: "this build's", build: setup.build, launcher: setup.launcher, extra: setup.pinOptions },
    { side: "pin", label: "the pin's", build: setup.pin, launcher: setup.pinLauncher, extra: [] as string[] },
  ];
  const captures: RunCapture[] = [];
  for (const { side, label, build, launcher, extra } of sides) {
    const name = `pin${j}.${side}`;
    const dumpDirectory = join(paths.scratch, name);
    mkdirSync(dumpDirectory, { recursive: true }); // PerfLog writes its file there and cannot create the directory
    const io: ProcessIO = { env: engineEnvironment(setup.context), cwd: paths.scratch, stdout: join(paths.scratch, `${name}.stdout`), stderr: join(paths.scratch, `${name}.stderr`) };
    const noShellFunctions = build.twins ? ["--jitcache-test-no-shell-functions"] : [];
    const args = pinArguments(setup.context, script, sequence, options, paths, name, [...pinRunOptions(dumpDirectory), ...noShellFunctions, ...extra]);
    const end = await launcher.engine(build.jsc, args, io, stacks);
    if (end.error || end.timedOut) return { difference: { check: "pin-status", detail: `${label} run ${describeEnd(end)}` }, allocations: 0, bodies: 0 };
    const status = { code: end.code, signal: end.signal };
    writeFileSync(join(paths.scratch, `${name}.status`), JSON.stringify(status));
    captures.push({ status, stdout: readBytes(io.stdout), stderr: readText(io.stderr) ?? "", jitDump: readJITDump(dumpDirectory) });
  }
  let engines: [EngineImage, EngineImage];
  try {
    engines = [loadEngineImage(setup.build.jsc), loadEngineImage(setup.pin.jsc)];
  } catch (error) {
    return { difference: { check: "pin-unreadable", detail: (error as Error).message }, allocations: 0, bodies: 0 };
  }
  return compareRuns(setup.build.architecture as Architecture, { capture: captures[0], engine: engines[0] }, { capture: captures[1], engine: engines[1] });
}
