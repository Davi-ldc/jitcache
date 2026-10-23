// The instructions of a run's code, whichever architecture printed them: registers, operands, which operands an
// instruction reads and writes, and the addresses it names. decode.ts parses them from a listing's lines, through x86.ts
// and arm64.ts.

import { wrap } from "./numbers.ts";

export type Architecture = "x86_64" | "aarch64";

export interface Register {
  name: string;
  family: string; // the 64-bit register it is part of: rax for eax and al, x0 for w0; zr for xzr and wzr
  width: number;
  high: boolean; // x86 ah, bh, ch, dh
}

// The temporaries the assembler reserves: x86_64's scratch register and ARM64's data and memory temporaries.
export const temporaries: Record<Architecture, ReadonlySet<string>> = {
  x86_64: new Set(["r11"]),
  aarch64: new Set(["x16", "x17"]),
};

export type Operand =
  | { kind: "register"; register: Register; indirect: boolean }
  | { kind: "immediate"; value: bigint }
  | { kind: "target"; address: bigint }
  | { kind: "memory"; memory: Memory }
  | { kind: "text"; text: string };

export interface Memory {
  prefix: string; // x86: "*" before an indirect branch's memory operand, and a segment
  base: Register | null;
  baseText: string | null; // a base that is no general-purpose register
  index: Register | null;
  indexText: string | null;
  scale: string; // x86: the scale; ARM64: the extend or shift of the index
  displacement: bigint;
  absolute: boolean; // x86: an absolute address, held in displacement
  writeback: string; // ARM64: "!" for pre-index, or ", <operand>" for post-index
}

export interface Instruction {
  address: bigint;
  length: number;
  block: number;
  mnemonic: string;
  operation: string; // the mnemonic that names its semantics: x86's without its size suffix
  operands: Operand[];
  text: string;
  line: number;
}

// What x86.ts and arm64.ts read from an instruction's text.
export type ParsedInstruction = Pick<Instruction, "mnemonic" | "operation" | "operands">;

export const isNop = (operation: string) => /^nop/.test(operation);

// ---------------------------------------------------------------------------------------------------------------------
// Which operands an instruction reads and writes

export type Role = "source" | "write" | "rmw";

export interface Effects {
  roles: Role[];
  implicitReads: string[];
  implicitWrites: string[];
}

export const readRegister = (value: bigint, register: Register) => (register.high ? (value >> 8n) & 0xffn : wrap(value, register.width));

// ---------------------------------------------------------------------------------------------------------------------
// The addresses an instruction names

export function targetOf(instruction: Instruction): bigint | null {
  const operand = instruction.operands.find(operand => operand.kind === "target");
  return operand?.kind === "target" ? operand.address : null;
}

// The addresses inside the code that a branch, an adr or an absolute operand names: entries a straight-line run ends at.
export function targetsWithin(instructions: readonly Instruction[], start: bigint, end: bigint): Set<bigint> {
  const targets = new Set<bigint>();
  for (const instruction of instructions) {
    for (const operand of instruction.operands) {
      const address = operand.kind === "target" ? operand.address : operand.kind === "memory" && operand.memory.absolute ? operand.memory.displacement : null;
      if (address !== null && address >= start && address < end) targets.add(address);
    }
  }
  return targets;
}
