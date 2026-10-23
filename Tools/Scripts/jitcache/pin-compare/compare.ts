// The comparison of the two runs of an option set, this build's and the pin's. compareRuns turns each run's code into
// canonical code (canonical.ts), walking the run's allocations in finalization order, and compares the two runs, in
// order:
// 1. Both end with the same status and print the same standard output.
// 2. Both finalize the same allocations in the same order, by identity. A body's code depends on what ran before it, so
//    the comparison is exact only while the two runs compile alike, and the first divergence is reported as such.
// 3. Each baseline body, each of its snippets and each of its inline rewrites has the same canonical instructions, block
//    by block, except the main-path blocks of super_construct and super_construct_varargs, where this build's fix of the
//    stored callee changes the code.

import { BaselineUnits, type Unit, type UnitBlock } from "./baseline.ts";
import type { RunCapture, RunStatus } from "./capture.ts";
import type { EngineImage } from "./engine.ts";
import type { Architecture } from "./instructions.ts";
import { Unreadable, clip } from "./log.ts";
import { sameInstruction } from "./naming.ts";

export interface PinSide {
  capture: RunCapture;
  engine: EngineImage;
}

export type PinCheck = "pin-crash" | "pin-status" | "pin-output" | "pin-allocations" | "pin-code" | "pin-unreadable";

export interface PinDifference {
  check: PinCheck;
  detail: string;
}

export interface PinComparison {
  difference: PinDifference | null;
  allocations: number; // the allocations both runs finalized alike
  bodies: number; // the baseline bodies among them
}

function describeUnit(unit: Unit): string {
  return unit.kind === "body" ? `body ${unit.ordinal} (${unit.allocation.identity})` : `${unit.kind} ${unit.allocation.identity} of body ${unit.ordinal} at ${unit.place}`;
}

const blockName = (block: UnitBlock | undefined) => (!block ? "(none)" : block.opcode ? `${block.key} (${block.opcode})` : block.key);

function compareUnits(index: number, a: Unit | null, b: Unit | null): string | null {
  if (!a || !b) {
    if (!a && !b) return null;
    const unit = (a ?? b)!;
    return `allocation ${index}, ${unit.allocation.identity}, is the ${describeUnit(unit)} in ${a ? "this build's run" : "the pin's run"} only`;
  }
  if (a.kind !== b.kind || a.ordinal !== b.ordinal || a.place !== b.place) return `allocation ${index} is this build's ${describeUnit(a)} and the pin's ${describeUnit(b)}`;
  const blocks = Math.max(a.blocks.length, b.blocks.length);
  for (let k = 0; k < blocks; ++k) {
    const x = a.blocks[k];
    const y = b.blocks[k];
    if (!x || !y || x.key !== y.key || x.opcode !== y.opcode) return `${describeUnit(a)}: its block ${k} is ${blockName(x)} in this build and ${blockName(y)} in the pin`;
    if (x.exempt) continue;
    const length = Math.max(x.instructions.length, y.instructions.length);
    for (let n = 0; n < length; ++n) {
      const p = x.instructions[n];
      const q = y.instructions[n];
      if (p && q && sameInstruction(p, q)) continue;
      return `${describeUnit(a)}, block ${blockName(x)}, instruction ${n}: this build ${p ? JSON.stringify(p.text) : "(none)"}, the pin ${q ? JSON.stringify(q.text) : "(none)"}`;
    }
  }
  return null;
}

const describeStatus = (status: RunStatus) => (status.signal ? `ended by ${status.signal}` : `exited with ${status.code}`);

// A run that ended by a signal crashed. The runner's environment ends a sanitizer error by SIGABRT, and a crash runs no
// atexit handler, so its JIT dump lacks the records the profiler's queue had not yet written. The description gives the
// signal and the line that opens the sanitizer report, when the run printed one.
function describeCrash(label: string, capture: RunCapture): string | null {
  if (!capture.status.signal) return null;
  const report = /^==\d+==ERROR: .*$/m.exec(capture.stderr)?.[0];
  return `${label} ended by ${capture.status.signal}${report ? `: ${clip(report)}` : ""}`;
}

function outputDifference(a: Uint8Array, b: Uint8Array): string | null {
  if (Buffer.from(a.buffer, a.byteOffset, a.byteLength).equals(Buffer.from(b.buffer, b.byteOffset, b.byteLength))) return null;
  const left = Buffer.from(a).toString("utf8").split("\n");
  const right = Buffer.from(b).toString("utf8").split("\n");
  const show = (line: string | undefined) => (line === undefined ? "(end of output)" : clip(line));
  for (let n = 0; n < Math.max(left.length, right.length); ++n) {
    if (left[n] !== right[n]) return `line ${n + 1}: this build ${show(left[n])}, the pin ${show(right[n])}`;
  }
  return "in bytes that are not text";
}

export function compareRuns(architecture: Architecture, current: PinSide, pin: PinSide): PinComparison {
  const result = (check: PinCheck, detail: string, allocations = 0, bodies = 0): PinComparison => ({ difference: { check, detail }, allocations, bodies });
  // With JITCache off, this build behaves as the pin: neither run crashes, and both end alike and print the same output.
  // A crash fails even when the other run crashed alike, since a crashed run's dump does not hold the whole run.
  const crashes = [describeCrash("this build's run", current.capture), describeCrash("the pin's run", pin.capture)].filter(crash => crash !== null);
  if (crashes.length) return result("pin-crash", crashes.join("; "));
  if (current.capture.status.code !== pin.capture.status.code || current.capture.status.signal !== pin.capture.status.signal)
    return result("pin-status", `this build's run ${describeStatus(current.capture.status)}, the pin's ${describeStatus(pin.capture.status)}`);
  const output = outputDifference(current.capture.stdout, pin.capture.stdout);
  if (output) return result("pin-output", `the outputs differ at ${output}`);

  const contexts: BaselineUnits[] = [];
  const literalSegments = [...current.engine.segments, ...pin.engine.segments];
  for (const [side, label] of [[current, "this build's run"], [pin, "the pin's run"]] as const) {
    try {
      contexts.push(new BaselineUnits(architecture, side.capture, side.engine, literalSegments));
    } catch (error) {
      if (error instanceof Unreadable) return result("pin-unreadable", `${label}: ${error.message}`);
      throw error;
    }
  }
  const [left, right] = contexts;
  // Items 2 and 3, in finalization order, so the first divergence is the one reported.
  const count = Math.max(left.allocations.length, right.allocations.length);
  let bodies = 0;
  for (let i = 0; i < count; ++i) {
    const a = left.allocations[i];
    const b = right.allocations[i];
    // An identity may hold newlines, which the quotes print escaped.
    if (!a || !b || a.identity !== b.identity)
      return result("pin-allocations", `allocation ${i}: this build finalized ${a ? JSON.stringify(a.identity) : "nothing more"}, the pin ${b ? JSON.stringify(b.identity) : "nothing more"}`, i, bodies);
    let units: [Unit | null, Unit | null];
    try {
      units = [left.unitAt(i), null];
    } catch (error) {
      if (error instanceof Unreadable) return result("pin-unreadable", `this build's run: ${error.message}`, i, bodies);
      throw error;
    }
    try {
      units[1] = right.unitAt(i);
    } catch (error) {
      if (error instanceof Unreadable) return result("pin-unreadable", `the pin's run: ${error.message}`, i, bodies);
      throw error;
    }
    const difference = compareUnits(i, units[0], units[1]);
    if (difference) return result("pin-code", difference, i, bodies);
    if (units[0]?.kind === "body") ++bodies;
  }
  return { difference: null, allocations: count, bodies };
}
