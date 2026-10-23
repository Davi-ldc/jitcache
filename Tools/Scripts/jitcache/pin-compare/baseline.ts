// One run's baseline units: each baseline body, and the MathIC snippets and inline rewrites that belong to it, as
// canonical code rendered with the run's names.
//
// A body's code is the range its dump's `Code at [s, e)` gives, which the allocation's header starts at and may exceed,
// since the JIT heap rounds an allocation up. The dump stops before the breakpoints that pad the code to its allocation
// granule, so on x86_64 the last dumped instruction, the tail's near jmp, takes its length from its encoding in the JIT
// dump.
//
// A MathIC inline rewrite writes into an earlier baseline body through a LinkBuffer that owns no memory, so its header
// prints the empty range [p, p) and the tool takes its code from its listing, whose one jump is jumpThunk's: on x86_64
// the near jmp.
//
// On x86_64 a body's branch displacements come from the JIT dump except where an inline rewrite of the same run covers
// them, since the dump copies the bytes on a queue of its own, after the rewrite or before it.

import { Canonicalizer, compactBranchPairs } from "./canonical.ts";
import type { JITDumpRecord, RunCapture } from "./capture.ts";
import { isBreakpoint, parseInstruction } from "./decode.ts";
import type { EngineImage, Segment } from "./engine.ts";
import { type Architecture, type Instruction, targetOf, targetsWithin } from "./instructions.ts";
import { type Allocation, type RawLine, Unreadable, clip, parseAllocations } from "./log.ts";
import { type CodeNamer, type RenderedInstruction, RunNames } from "./naming.ts";
import { hex } from "./numbers.ts";

export interface UnitBlock {
  key: string;
  opcode: string | null;
  exempt: boolean; // a main-path block of super_construct or super_construct_varargs, which the comparison skips
  instructions: RenderedInstruction[];
}

export interface Unit {
  kind: "body" | "snippet" | "rewrite";
  allocation: Allocation;
  ordinal: number; // a body's, among the run's baseline bodies from 1; a snippet's or a rewrite's body's
  place: string; // a snippet's or a rewrite's block in its body
  blocks: UnitBlock[];
  namer: CodeNamer;
  blockAt: (address: bigint) => string | null;
}

export class BaselineUnits {
  readonly allocations: Allocation[];
  private readonly names: RunNames;
  private readonly bodyOrdinals = new Map<number, number>();
  private readonly rewriteOwners = new Map<number, Allocation>();
  private readonly rewritesOf = new Map<number, Allocation[]>();
  private readonly records = new Map<string, JITDumpRecord[]>();
  private readonly units = new Map<number, Unit>();

  // literalSegments: both builds' loaded jsc segments, inside which an integer constant also matches an equal literal.
  constructor(readonly architecture: Architecture, capture: RunCapture, engine: EngineImage, literalSegments: readonly Segment[]) {
    this.allocations = parseAllocations(capture.stderr);
    this.names = new RunNames(architecture, this.allocations, engine, literalSegments);
    let ordinal = 0;
    for (const allocation of this.allocations) {
      if (allocation.dump) {
        this.bodyOrdinals.set(allocation.index, ++ordinal);
        continue;
      }
      // An allocation whose header starts inside an earlier baseline body's code is an inline rewrite of it: nothing dies
      // during the run, so no other allocation can start there.
      const owner = this.names.containing(allocation.start, allocation.index, candidate => candidate.dump !== null && allocation.start < candidate.codeEnd);
      if (!owner) continue;
      allocation.codeEnd = this.listingEnd(allocation);
      this.rewriteOwners.set(allocation.index, owner);
      this.rewritesOf.set(owner.index, [...(this.rewritesOf.get(owner.index) ?? []), allocation]);
    }
    for (const record of capture.jitDump) {
      const key = `${record.address}:${record.size}`;
      this.records.set(key, [...(this.records.get(key) ?? []), record]);
    }
  }

  // The end of an inline rewrite's code, which its header does not give: JITMathIC's LinkBuffer writes into the body
  // and owns no memory, so the header prints [p, p) 0 bytes, while the listing shows the bytes written. The last of them
  // is the jump jumpThunk emits, a 4-byte b on ARM64 and the near jmp on x86_64.
  private listingEnd(allocation: Allocation): bigint {
    const lines = allocation.disassembly;
    if (!lines?.length) throw new Unreadable(`line ${allocation.line}: the inline rewrite ${allocation.identity} printed no listing`);
    const last = parseInstruction(this.architecture, lines.at(-1)!, 0, allocation.start);
    if (this.architecture === "aarch64") return last.address + 4n;
    if (last.operation !== "jmp" || targetOf(last) === null)
      throw new Unreadable(`line ${last.line}: the inline rewrite ${allocation.identity} ends in an instruction whose length its listing does not give: ${clip(last.text)}`);
    return last.address + 5n;
  }

  // The unit allocation index stands for, canonicalized and rendered in the walk's order, or null for an allocation
  // the comparison names only by identity.
  unitAt(index: number): Unit | null {
    const allocation = this.allocations[index];
    let unit: Unit | null = null;
    if (allocation.dump) unit = this.body(allocation);
    else if (this.rewriteOwners.has(index)) unit = this.rewrite(allocation, this.units.get(this.rewriteOwners.get(index)!.index)!);
    else if (allocation.identity.startsWith("JITMathIC: generating out of line")) unit = this.snippet(allocation);
    if (unit) this.units.set(index, unit);
    return unit;
  }

  private instructionsOf(lines: readonly RawLine[], block: number, allocation: Allocation): Instruction[] {
    return lines.map(line => parseInstruction(this.architecture, line, block, allocation.start));
  }

  // Each instruction's length: ARM64's 4 bytes; on x86_64 the distance to the next instruction, the last one's at most
  // the distance to the end of the code, which readDisplacements narrows to a body's encoded near branch.
  private measure(instructions: Instruction[], allocation: Allocation) {
    instructions.forEach((instruction, i) => {
      const next = i + 1 < instructions.length ? instructions[i + 1].address : allocation.codeEnd;
      if (instruction.address < allocation.start || next <= instruction.address || next > allocation.codeEnd)
        throw new Unreadable(`line ${instruction.line}: an instruction at ${hex(instruction.address)} out of the order of ${allocation.identity}'s code, [${hex(allocation.start)}, ${hex(allocation.codeEnd)})`);
      if (this.architecture === "x86_64") instruction.length = Number(next - instruction.address);
    });
  }

  // x86_64's dump runs before the link tasks that write calls and jumps to thunks, so every call, jmp and conditional
  // jump with a 32-bit displacement takes its displacement from the body's JIT dump record.
  private readDisplacements(allocation: Allocation, instructions: Instruction[]) {
    const size = Number(allocation.end - allocation.start);
    // The record holds the allocation's executable memory, the header's range, and an allocation of no size writes none,
    // so the earlier allocations at this range hold the earlier records.
    const occurrence = this.allocations.slice(0, allocation.index).filter(other => other.start === allocation.start && other.end === allocation.end).length;
    const record = this.records.get(`${allocation.start}:${size}`)?.[occurrence];
    if (!record) throw new Unreadable(`the JIT dump holds no record of baseline body ${this.bodyOrdinals.get(allocation.index)} (${allocation.identity}) at [${hex(allocation.start)}, ${hex(allocation.end)})`);
    const rewrites = this.rewritesOf.get(allocation.index) ?? [];
    const bytes = record.bytes;
    instructions.forEach((instruction, i) => {
      const at = instruction.operands.findIndex(operand => operand.kind === "target");
      if (at < 0 || !/^(call|jmp|j[a-z]+)$/.test(instruction.operation)) return;
      const offset = Number(instruction.address - allocation.start);
      const opcode = bytes[offset];
      const near = opcode === 0xe8 || opcode === 0xe9;
      const conditional = opcode === 0x0f && (bytes[offset + 1] & 0xf0) === 0x80;
      if (!near && !conditional) return; // a short jump, which the dump printed linked
      // The dump ends before the breakpoints that pad the code, so the last instruction measured up to them.
      const length = near ? 5 : 6;
      if (i === instructions.length - 1 ? length > instruction.length : length !== instruction.length)
        throw new Unreadable(`line ${instruction.line}: the JIT dump's bytes at ${hex(instruction.address)} encode a ${length}-byte branch where the dump measured ${instruction.length} bytes`);
      instruction.length = length;
      // The dump copies the bytes on a queue of its own, possibly after an inline rewrite overwrote them; the
      // disassembly printed the bytes before it, and a jump an inline rewrite overwrites is linked internally.
      const field = instruction.address + BigInt(length - 4);
      if (rewrites.some(rewrite => rewrite.start < field + 4n && field < rewrite.codeEnd)) return;
      const displacement = new DataView(bytes.buffer, bytes.byteOffset + offset + length - 4, 4).getInt32(0, true);
      instruction.operands[at] = { kind: "target", address: instruction.address + BigInt(length) + BigInt(displacement) };
    });
  }

  // Canonicalizes a unit's instructions and returns, with the canonical lists, the namer of addresses in its code: the
  // block and the ordinal of the canonical instruction there.
  private canonicalize(instructions: Instruction[], keys: string[], allocation: Allocation) {
    const code = this.architecture === "aarch64" ? compactBranchPairs(instructions) : instructions;
    const targets = targetsWithin(code, allocation.start, allocation.codeEnd);
    const blocks = new Canonicalizer(this.architecture).canonicalize(code, targets, keys.length);
    const containingInstruction = (address: bigint): number => {
      let low = 0;
      let high = code.length;
      while (low < high) {
        const middle = (low + high) >> 1;
        if (code[middle].address <= address) low = middle + 1;
        else high = middle;
      }
      return low - 1;
    };
    const namer: CodeNamer = address => {
      if (address < allocation.start || address > allocation.codeEnd) return null;
      if (address === allocation.codeEnd) return "end";
      const i = containingInstruction(address);
      if (i < 0) return null;
      const instruction = code[i];
      const list = blocks[instruction.block];
      let ordinal = list.findIndex(canonical => canonical.key >= i);
      if (ordinal < 0) ordinal = list.length;
      const offset = address - instruction.address;
      return `${keys[instruction.block]}#${ordinal}${offset ? `+${hex(offset)}` : ""}`;
    };
    const blockAt = (address: bigint): string | null => {
      if (address < allocation.start || address >= allocation.codeEnd) return null;
      const i = containingInstruction(address);
      return i < 0 ? null : keys[code[i].block];
    };
    return { blocks, namer, blockAt };
  }

  private body(allocation: Allocation): Unit {
    const dump = allocation.dump!;
    const instructions = dump.flatMap((block, b) => this.instructionsOf(block.lines, b, allocation));
    this.measure(instructions, allocation);
    if (this.architecture === "x86_64") this.readDisplacements(allocation, instructions);
    const keys = dump.map(block => block.key);
    const { blocks, namer, blockAt } = this.canonicalize(instructions, keys, allocation);
    const rendered = dump.map((block, b): UnitBlock => {
      const exempt = block.key.startsWith("main:") && (block.opcode === "super_construct" || block.opcode === "super_construct_varargs");
      // An exempt block's values take no number, so the fix's own values cannot move the walk's numbering. A value past
      // the code, in the allocation's padding, names the allocation itself.
      return { key: block.key, opcode: block.opcode, exempt, instructions: this.names.render(blocks[b], [namer], allocation.index + 1, !exempt) };
    });
    return { kind: "body", allocation, ordinal: this.bodyOrdinals.get(allocation.index)!, place: "", blocks: rendered, namer, blockAt };
  }

  // canonical.ts's rule 1: a listing prints its LinkBuffer whole, and the breakpoints after its last other instruction
  // are those LinkBuffer::allocate padded it with, as many as the code's length leaves room for.
  private listing(allocation: Allocation): Instruction[] {
    if (!allocation.disassembly) throw new Unreadable(`line ${allocation.line}: ${allocation.identity} printed no disassembly`);
    const instructions = this.instructionsOf(allocation.disassembly, 0, allocation);
    while (instructions.length && isBreakpoint(this.architecture, instructions.at(-1)!.operation)) instructions.pop();
    this.measure(instructions, allocation);
    return instructions;
  }

  // A MathIC snippet belongs to the body its jumps return into, at the block they return to.
  private snippet(allocation: Allocation): Unit | null {
    const instructions = this.listing(allocation);
    let owner: Unit | null = null;
    let place = "";
    for (const instruction of instructions) {
      const target = targetOf(instruction);
      if (target === null || (target >= allocation.start && target < allocation.codeEnd)) continue;
      const body = this.names.containing(target, allocation.index, candidate => candidate.dump !== null && target < candidate.codeEnd);
      if (body) {
        owner = this.units.get(body.index)!;
        place = owner.blockAt(target) ?? "";
        break;
      }
    }
    if (!owner) return null; // a snippet of optimized code
    return this.attached("snippet", allocation, instructions, owner, place);
  }

  // An inline rewrite belongs to the body and block that contain it, and its code is its listing's (listingEnd).
  private rewrite(allocation: Allocation, owner: Unit): Unit {
    return this.attached("rewrite", allocation, this.listing(allocation), owner, owner.blockAt(allocation.start) ?? "");
  }

  private attached(kind: "snippet" | "rewrite", allocation: Allocation, instructions: Instruction[], owner: Unit, place: string): Unit {
    const { blocks, namer, blockAt } = this.canonicalize(instructions, [kind], allocation);
    const block: UnitBlock = { key: kind, opcode: null, exempt: false, instructions: this.names.render(blocks[0], [namer, owner.namer], allocation.index + 1, true) };
    return { kind, allocation, ordinal: owner.ordinal, place, blocks: [block], namer, blockAt };
  }
}
