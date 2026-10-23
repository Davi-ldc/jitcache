#!/usr/bin/env bun
// The pin comparison: whether the off path emits the pin's code. The runner (run-jitcache-tests --pin) starts each Off
// option set of a jsc-hosted script once with this build's jsc and once with the pin's, both with pinRunOptions, and
// hands the two runs to compareRuns. Neither build is instrumented, so each run describes its code only through what the
// engine prints: the logJIT header of every finalized allocation, the per-bytecode dump of each baseline compilation, the
// disassembly of every MathIC snippet and inline rewrite, and the perf JIT dump of the bytes each allocation holds.
//
// The modules of pin-compare/ hold the comparison: what a run leaves (capture.ts) and the engine it names (engine.ts),
// its standard error read into allocations (log.ts), its instructions decoded (decode.ts, over instructions.ts, x86.ts
// and arm64.ts), canonical code (canonical.ts), the names values take (naming.ts), the baseline units (baseline.ts)
// and the comparison itself (compare.ts).
//
// Standalone, on the files a runner kept:
//   bun Tools/Scripts/jitcache/pin-compare.ts --this=<this build's WebKit directory> --pin=<the pin's WebKit directory>
//       <scratch>/pin<j>
// compares <scratch>/pin<j>.this.{status,stdout,stderr} and the JIT dump in <scratch>/pin<j>.this/ with the pin's.

import { join, resolve } from "node:path";
import { readCapture } from "./pin-compare/capture.ts";
import { compareRuns } from "./pin-compare/compare.ts";
import { loadEngineImage } from "./pin-compare/engine.ts";
import type { Architecture } from "./pin-compare/instructions.ts";

function architectureOf(buildDirectory: string): Architecture {
  const named = /(?:^|\/)linux-(x86_64|aarch64)-/.exec(resolve(buildDirectory))?.[1];
  return (named ?? (process.arch === "arm64" ? "aarch64" : "x86_64")) as Architecture;
}

function main(argv: string[]): number {
  let current: string | null = null;
  let pin: string | null = null;
  const prefixes: string[] = [];
  for (const argument of argv) {
    if (argument.startsWith("--this=")) current = resolve(argument.slice("--this=".length));
    else if (argument.startsWith("--pin=")) pin = resolve(argument.slice("--pin=".length));
    else if (!argument.startsWith("--")) prefixes.push(resolve(argument));
    else {
      console.error(`pin-compare.ts: unknown option ${argument}`);
      return 2;
    }
  }
  if (!current || !pin || prefixes.length !== 1) {
    console.error("Usage: pin-compare.ts --this=<this build's WebKit directory> --pin=<the pin's WebKit directory> <scratch>/pin<j>");
    return 2;
  }
  const architecture = architectureOf(current);
  if (architectureOf(pin) !== architecture) {
    console.error(`pin-compare.ts: ${current} builds ${architecture} and ${pin} builds ${architectureOf(pin)}`);
    return 2;
  }
  const comparison = compareRuns(
    architecture,
    { capture: readCapture(`${prefixes[0]}.this`), engine: loadEngineImage(join(current, "bin/jsc")) },
    { capture: readCapture(`${prefixes[0]}.pin`), engine: loadEngineImage(join(pin, "bin/jsc")) },
  );
  if (comparison.difference) {
    console.log(`${comparison.difference.check}: ${comparison.difference.detail}`);
    return 1;
  }
  console.log(`same: ${comparison.allocations} allocations, ${comparison.bodies} baseline bodies`);
  return 0;
}

if (import.meta.main) process.exit(main(process.argv.slice(2)));
