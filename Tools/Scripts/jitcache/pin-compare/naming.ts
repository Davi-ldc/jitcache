// What the values in a run's code name, and canonical instructions rendered with those names, so the two runs of an
// option set compare by what their code refers to.

import type { CanonicalInstruction, ValueRole } from "./canonical.ts";
import { type EngineImage, type Segment, engineName } from "./engine.ts";
import type { Architecture } from "./instructions.ts";
import type { Allocation } from "./log.ts";
import { anonymousLow, hex, toSigned, userSpaceHigh, wrap } from "./numbers.ts";

// A part of a rendered operand: its text, and for an integer constant inside either build's loaded jsc segments, the
// literal value it stands for. Two parts match when their texts match or their literals do.
export interface RenderedPart {
  text: string;
  literal: string | null;
}

export interface RenderedInstruction {
  mnemonic: string;
  operands: RenderedPart[][];
  text: string;
}

export type CodeNamer = (address: bigint) => string | null;
type ValueNamer = (value: bigint, width: number, role: ValueRole) => RenderedPart;

const plain = (text: string): RenderedPart => ({ text, literal: null });

function renderInstruction(architecture: Architecture, instruction: CanonicalInstruction, name: ValueNamer): RenderedInstruction {
  const x86 = architecture === "x86_64";
  const operands = instruction.operands.map((operand): RenderedPart[] => {
    switch (operand.kind) {
      case "text":
        return [plain(operand.text)];
      case "register":
        return [plain(x86 ? `${operand.indirect ? "*" : ""}%${operand.register.name}` : operand.register.name)];
      case "value": {
        const value = name(operand.value, operand.width, operand.role);
        if (operand.role === "target") return [value];
        if (operand.role === "indirect") return [plain(x86 ? "*" : ""), value];
        return [plain(x86 ? "$" : "#"), value];
      }
      case "memory": {
        const memory = operand.memory;
        const indexed = memory.index !== null || memory.indexText !== null;
        if (memory.absolute || (operand.base !== null && !indexed)) {
          const address = memory.absolute ? memory.displacement : wrap(operand.base! + memory.displacement, 64);
          return [plain(`${memory.prefix}[`), name(address, 64, "address"), plain(`]${memory.writeback}`)];
        }
        const index = memory.index ? (x86 ? `%${memory.index.name}` : memory.index.name) : memory.indexText;
        if (x86) {
          const head = `${memory.prefix}${memory.displacement ? hex(memory.displacement) : ""}(`;
          const tail = `${index ? `,${index}` : ""}${memory.scale ? `,${memory.scale}` : ""})`;
          if (operand.base !== null) return [plain(head), name(operand.base, 64, "address"), plain(tail)];
          return [plain(`${head}${memory.base ? `%${memory.base.name}` : memory.baseText ?? ""}${tail}`)];
        }
        const inner = `${memory.displacement ? `, #${hex(memory.displacement)}` : ""}${index ? `, ${index}` : ""}${memory.scale ? `, ${memory.scale}` : ""}`;
        if (operand.base !== null) return [plain("["), name(operand.base, 64, "address"), plain(`${inner}]${memory.writeback}`)];
        return [plain(`[${memory.base?.name ?? memory.baseText}${inner}]${memory.writeback}`)];
      }
    }
  });
  const text = operands.length ? `${instruction.mnemonic} ${operands.map(parts => parts.map(part => part.text).join("")).join(", ")}` : instruction.mnemonic;
  return { mnemonic: instruction.mnemonic, operands, text };
}

export function sameInstruction(a: RenderedInstruction, b: RenderedInstruction): boolean {
  if (a.mnemonic !== b.mnemonic || a.operands.length !== b.operands.length) return false;
  return a.operands.every((parts, i) => {
    const other = b.operands[i];
    return parts.length === other.length && parts.every((part, k) => part.text === other[k].text || (part.literal !== null && part.literal === other[k].literal));
  });
}

// The names one run gives: its allocations by start address, the engine's symbols, and the h numbers the walk assigns
// to the other addresses in the order it first meets them.
export class RunNames {
  private readonly byStart: Allocation[];
  private readonly reach: bigint[];
  private readonly heapNumbers = new Map<bigint, number>();

  // literalSegments: both builds' loaded jsc segments, inside which an integer constant also matches an equal literal.
  constructor(readonly architecture: Architecture, allocations: readonly Allocation[], private readonly engine: EngineImage, private readonly literalSegments: readonly Segment[]) {
    this.byStart = [...allocations].sort((a, b) => (a.start !== b.start ? (a.start < b.start ? -1 : 1) : a.index - b.index));
    let reach = 0n;
    this.reach = this.byStart.map(allocation => (reach = allocation.end > reach ? allocation.end : reach));
  }

  // The allocation finalized last before limit whose range holds the value, among those accept takes.
  containing(value: bigint, limit: number, accept: (allocation: Allocation) => boolean = () => true): Allocation | null {
    let low = 0;
    let high = this.byStart.length;
    while (low < high) {
      const middle = (low + high) >> 1;
      if (this.byStart[middle].start <= value) low = middle + 1;
      else high = middle;
    }
    let best: Allocation | null = null;
    for (let i = low - 1; i >= 0 && this.reach[i] > value; --i) {
      const candidate = this.byStart[i];
      if (candidate.index < limit && value < candidate.end && (!best || candidate.index > best.index) && accept(candidate)) best = candidate;
    }
    return best;
  }

  // What a value names, tried in this order: a place in the same body's code, by the namers; the allocation that holds
  // it, with the offset; the symbol at or below it in the build's jsc, whose engine sits at its link-time address; and
  // from 2^32 up an h number, in the order the walk first meets the value. A value under 2^32 that none of these names
  // stays literal, and so does one that is negative at its width or from 2^48 up.
  private name(value: bigint, width: number, role: ValueRole, namers: CodeNamer[], limit: number, assign: boolean): RenderedPart {
    const magnitude = width >= 64 ? value : toSigned(value, width);
    const literal = hex(toSigned(value, width));
    if (magnitude < 0n || magnitude >= userSpaceHigh) return { text: literal, literal: null };
    // Only an integer constant inside either build's jsc segments also matches an equal literal, since each build puts
    // another symbol there; branch targets, allocations and h numbers match by name alone.
    const integer = role === "immediate" && this.literalSegments.some(segment => value >= segment.start && value < segment.end);
    const named = (text: string): RenderedPart => ({ text, literal: integer ? literal : null });
    for (const namer of namers) {
      const name = namer(value);
      if (name) return named(name);
    }
    const allocation = this.containing(value, limit);
    if (allocation) return named(`${allocation.identity}+${hex(value - allocation.start)}`);
    const symbol = engineName(this.engine, value);
    if (symbol) return named(symbol);
    if (value < anonymousLow) return named(literal);
    let number = this.heapNumbers.get(value);
    if (number === undefined) {
      if (!assign) return named("h?");
      number = this.heapNumbers.size;
      this.heapNumbers.set(value, number);
    }
    return named(`h${number}`);
  }

  render(instructions: CanonicalInstruction[], namers: CodeNamer[], limit: number, assign: boolean): RenderedInstruction[] {
    const name: ValueNamer = (value, width, role) => this.name(value, width, role, namers, limit, assign);
    return instructions.map(instruction => renderInstruction(this.architecture, instruction, name));
  }
}
