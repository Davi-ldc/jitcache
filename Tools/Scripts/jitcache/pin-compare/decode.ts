// A listing's lines read into instructions, and what canonical code asks of an instruction whatever its architecture:
// whether it ends a straight-line run, branches conditionally or is a breakpoint, and the width its immediates take.

import { isARM64Branch, parseARM64Instruction } from "./arm64.ts";
import type { Architecture, Instruction } from "./instructions.ts";
import { type RawLine, Unreadable, clip } from "./log.ts";
import { isX86Branch, parseX86Instruction } from "./x86.ts";

export function isTerminator(architecture: Architecture, operation: string): boolean {
  return architecture === "x86_64" ? isX86Branch(operation) || operation === "ud2" || operation === "int3" || operation === "hlt" : isARM64Branch(operation);
}

export function isConditional(architecture: Architecture, operation: string): boolean {
  if (architecture === "x86_64") return /^j/.test(operation) && operation !== "jmp";
  return (/^b\./.test(operation) && operation !== "b.al" && operation !== "b.nv") || /^(cbn?z|tbn?z)$/.test(operation);
}

// MacroAssembler::breakpoint: x86_64's int3, and ARM64's brk, whose immediate is the build's WTF_FATAL_CRASH_CODE.
export const isBreakpoint = (architecture: Architecture, operation: string) => operation === (architecture === "x86_64" ? "int3" : "brk");

export function parseInstruction(architecture: Architecture, line: RawLine, block: number, start: bigint): Instruction {
  const address = architecture === "aarch64" && line.offset !== null ? start + BigInt(line.offset) : line.address;
  const length = architecture === "aarch64" ? 4 : 0; // x86_64's lengths come from the addresses (BaselineUnits.measure)
  if (!line.text || line.text === "failed-to-format")
    return { address, length, block, mnemonic: "<undecoded>", operation: "<undecoded>", operands: [], text: line.text, line: line.line };
  const unreadable = (what: string) => new Unreadable(`line ${line.line}: ${what} it cannot classify: ${clip(line.text)}`);
  const { mnemonic, operation, operands } = architecture === "x86_64" ? parseX86Instruction(line.text, unreadable) : parseARM64Instruction(line.text, unreadable);
  return { address, length, block, mnemonic, operation, operands, text: line.text, line: line.line };
}

// The width of an instruction's operation, which its immediates take: its destination's, or its size suffix's.
export function instructionWidth(architecture: Architecture, instruction: Instruction): number {
  if (architecture === "x86_64") {
    const last = instruction.operands.at(-1);
    if (last?.kind === "register") return last.register.high ? 8 : last.register.width;
    if (instruction.mnemonic !== instruction.operation) return { b: 8, w: 16, l: 32, q: 64 }[instruction.mnemonic.at(-1) as "b" | "w" | "l" | "q"] ?? 64;
    return 64;
  }
  const register = instruction.operands.find(operand => operand.kind === "register");
  return register?.kind === "register" && register.register.width === 32 ? 32 : 64;
}
