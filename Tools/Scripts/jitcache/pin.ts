// The pin comparison of one option set: its Off run once on this build and once on the pin's, whose code the modules of
// pin-compare/ compare. runs.ts starts it after each attempt's oracle (runPinComparison), and the self-tests call
// comparePinOptionSet directly.

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { type Build, type Launcher, type ProcessIO, type StackSizes, describeEnd, keepingExitStatus, readBytes, readText } from "./launcher.ts";
import { type RunCapture, pinRunOptions, readJITDump } from "./pin-compare/capture.ts";
import { type PinComparison, compareRuns } from "./pin-compare/compare.ts";
import { type EngineImage, loadEngineImage } from "./pin-compare/engine.ts";
import type { Architecture } from "./pin-compare/instructions.ts";
import { type CommandContext, type Script, type SequencePaths, engineEnvironment, jscArguments } from "./scripts.ts";

// What the pin comparison starts its two runs with: this build's side and the pin's.
export interface PinSetup {
  readonly context: CommandContext;
  readonly build: Build;
  readonly launcher: Launcher;
  readonly pin: Build;
  readonly pinLauncher: Launcher;
  readonly pinOptions: string[]; // --pin-options, which this build's side alone takes
}

// Runs option set j of a sequence with this build's jsc and with the pin's, keeping each side's status, output, error and
// JIT dump as <scratch>/pin<j>.<this|pin>.*, which pin-compare.ts also reads when run alone, and compares them. Each side
// runs the oracle run's command line without --jitcache-describe-heap, with the comparison's options last, so they
// override the run's own. Each side whose build is a twins build runs with --jitcache-test-no-shell-functions, so its
// global object has the pin shell's properties; the side's build decides, not the session's mode, because a twins build
// registers the shell functions in either mode and a plain build rejects the flag. Both sides take the environment of
// the runs whose leaks the runner judges, where a sanitizer error ends the run by SIGABRT, which compareRuns fails as a
// crash, and a leak keeps the run's own status.
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
    const io: ProcessIO = { env: keepingExitStatus(engineEnvironment(setup.context)), cwd: paths.scratch, stdout: join(paths.scratch, `${name}.stdout`), stderr: join(paths.scratch, `${name}.stderr`) };
    const noShellFunctions = build.twins ? ["--jitcache-test-no-shell-functions"] : [];
    const overrides = [...pinRunOptions(dumpDirectory), ...noShellFunctions, ...extra];
    const args = jscArguments(setup.context, script, sequence, "Off", options, paths, name, { describeHeap: false, overrides });
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
