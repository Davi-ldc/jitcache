// ARM64 instructions as the disassembler prints them: the registers, the operands, and which of them an instruction
// reads and writes.

import type { Effects, Memory, Operand, ParsedInstruction, Register, Role } from "./instructions.ts";
import { clip } from "./log.ts";
import { hex, parseNumber } from "./numbers.ts";

const arm64Registers = new Map<string, Register>();
{
  for (let n = 0; n <= 30; ++n) {
    arm64Registers.set(`x${n}`, { name: `x${n}`, family: `x${n}`, width: 64, high: false });
    arm64Registers.set(`w${n}`, { name: `w${n}`, family: `x${n}`, width: 32, high: false });
  }
  arm64Registers.set("fp", { name: "fp", family: "x29", width: 64, high: false });
  arm64Registers.set("lr", { name: "lr", family: "x30", width: 64, high: false });
  arm64Registers.set("sp", { name: "sp", family: "sp", width: 64, high: false });
  arm64Registers.set("wsp", { name: "wsp", family: "sp", width: 32, high: false });
  arm64Registers.set("xzr", { name: "xzr", family: "zr", width: 64, high: false });
  arm64Registers.set("wzr", { name: "wzr", family: "zr", width: 32, high: false });
}

export const isARM64Branch = (operation: string) => /^(b|bl|br|blr|ret|cbn?z|tbn?z|brk|udf|hlt|bra[a-z]*|blra[a-z]*|reta[a-z]*)$/.test(operation) || /^b\./.test(operation);

// Binja separates operands with ", ", which ARM64 memory operands and register lists also contain.
function splitOperands(text: string): string[] {
  const pieces: string[] = [];
  let depth = 0;
  let start = 0;
  for (let i = 0; i < text.length; ++i) {
    const c = text[i];
    if (c === "[" || c === "{") ++depth;
    else if (c === "]" || c === "}") --depth;
    else if (c === "," && !depth && text[i + 1] === " ") {
      pieces.push(text.slice(start, i));
      start = i + 2;
      ++i;
    }
  }
  pieces.push(text.slice(start));
  return pieces.map(piece => piece.trim()).filter(piece => piece.length);
}

function parseARM64Operand(text: string, branch: boolean): Operand | null {
  if (text.startsWith("[")) {
    const match = /^\[([a-z0-9]+)(?:, (.*))?\](!?)$/.exec(text);
    if (!match) return null;
    const base = arm64Registers.get(match[1]) ?? null;
    const memory: Memory = { prefix: "", base, baseText: base ? null : match[1], index: null, indexText: null, scale: "", displacement: 0n, absolute: false, writeback: match[3] };
    if (match[2] !== undefined) {
      const parts = splitOperands(match[2]);
      if (parts[0].startsWith("#")) {
        const value = parseNumber(parts[0].slice(1));
        if (value === null) return null;
        memory.displacement = value;
      } else {
        memory.index = arm64Registers.get(parts[0]) ?? null;
        memory.indexText = memory.index ? null : parts[0];
      }
      memory.scale = parts.slice(1).join(", ");
    }
    return { kind: "memory", memory };
  }
  if (text.startsWith("#")) {
    const value = parseNumber(text.slice(1));
    return value === null ? { kind: "text", text } : { kind: "immediate", value };
  }
  if (/^0x[0-9a-f]+$/.test(text)) return { kind: "target", address: BigInt(text) };
  const register = arm64Registers.get(text);
  if (register) return { kind: "register", register, indirect: branch };
  return { kind: "text", text };
}

// The mnemonic, the operation and the operands of an ARM64 instruction's text. unreadable makes the error for what the
// tool cannot classify.
export function parseARM64Instruction(text: string, unreadable: (what: string) => Error): ParsedInstruction {
  const tab = text.indexOf("\t");
  const mnemonic = (tab < 0 ? text : text.slice(0, tab)).trim();
  const branch = isARM64Branch(mnemonic);
  const operands: Operand[] = [];
  for (const operandText of tab < 0 ? [] : splitOperands(text.slice(tab + 1))) {
    const operand = parseARM64Operand(operandText, branch);
    if (!operand) throw unreadable(`the operand ${clip(operandText)} of`);
    operands.push(operand);
  }
  // A post-index access prints its base alone in brackets and its increment as the next operand.
  if (/^(ld|st)/.test(mnemonic)) {
    for (let i = 0; i + 1 < operands.length; ++i) {
      const operand = operands[i];
      if (operand.kind !== "memory" || operand.memory.writeback || operand.memory.index || operand.memory.indexText || operand.memory.displacement) continue;
      const next = operands[i + 1];
      if (next.kind !== "immediate" && next.kind !== "register") continue;
      operand.memory.writeback = `, ${next.kind === "immediate" ? `#${hex(next.value)}` : next.register.name}`;
      operands.splice(i + 1, 1);
    }
  }
  return { mnemonic, operation: mnemonic, operands };
}

export function arm64Effects(operation: string, count: number): Effects {
  const roles: Role[] = Array.from({ length: count }, () => "source");
  const first = (role: Role, n = 1) => {
    for (let i = 0; i < Math.min(n, count); ++i) roles[i] = role;
  };
  if (isARM64Branch(operation) || /^(cmp|cmn|tst|ccmp|ccmn|fcmp|fcmpe|fccmp|fccmpe|prfm|prfum)$/.test(operation)) {
    // every register operand is read
  } else if (/^stl?x[rp][bh]?$/.test(operation)) first("write"); // the status of an exclusive store
  else if (/^st/.test(operation)) {
    // a store reads every register it names
  } else if (/^cas/.test(operation)) first("rmw");
  else if (/^(ld(add|clr|eor|set|smax|smin|umax|umin)|swp)/.test(operation)) {
    if (count > 1) roles[1] = "write";
  } else if (/^ld(n|x|ax|iap)?p(sw)?$/.test(operation)) first("write", 2);
  else if (/^(movk|bfi|bfxil|bfm|bfc)$/.test(operation)) first("rmw");
  else if (count) first("write");
  return { roles, implicitReads: [], implicitWrites: [] };
}
