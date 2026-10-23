// x86_64 instructions as Zydis prints them: the registers, the operation a mnemonic names, the operands, and which of
// them an instruction reads and writes.

import { type Effects, type Memory, type Operand, type ParsedInstruction, type Register, type Role, isNop } from "./instructions.ts";
import { clip } from "./log.ts";
import { parseNumber, wrap } from "./numbers.ts";

const x86Registers = new Map<string, Register>();
{
  const families: [string, string[]][] = [
    ["rax", ["rax", "eax", "ax", "al", "ah"]],
    ["rbx", ["rbx", "ebx", "bx", "bl", "bh"]],
    ["rcx", ["rcx", "ecx", "cx", "cl", "ch"]],
    ["rdx", ["rdx", "edx", "dx", "dl", "dh"]],
    ["rsi", ["rsi", "esi", "si", "sil"]],
    ["rdi", ["rdi", "edi", "di", "dil"]],
    ["rbp", ["rbp", "ebp", "bp", "bpl"]],
    ["rsp", ["rsp", "esp", "sp", "spl"]],
  ];
  for (let n = 8; n < 16; ++n) families.push([`r${n}`, [`r${n}`, `r${n}d`, `r${n}w`, `r${n}b`]]);
  const widths = [64, 32, 16, 8, 8];
  for (const [family, names] of families)
    names.forEach((name, i) => x86Registers.set(name, { name, family, width: widths[i], high: i === 4 }));
  for (let n = 8; n < 16; ++n) x86Registers.set(`r${n}l`, { name: `r${n}l`, family: `r${n}`, width: 8, high: false });
}

const x86MoveOperations = new Set([
  "mov", "movabs", "movzx", "movsx", "movsxd", "lea", "movd", "movss", "movsd", "movaps", "movapd", "movups", "movupd",
  "movdqa", "movdqu", "cvtsi2sd", "cvtsi2ss", "cvttsd2si", "cvttss2si", "cvtsd2si", "cvtss2si", "cvtsd2ss", "cvtss2sd",
  "sqrtsd", "sqrtss", "popcnt", "lzcnt", "tzcnt", "bsf", "bsr", "pop", "movmskpd", "movmskps", "pmovmskb", "pextrb",
  "pextrw", "pextrd", "pextrq", "andn", "bextr", "pdep", "pext", "rorx", "sarx", "shlx", "shrx", "blsi", "blsr", "blsmsk",
]);
const x86ReadOperations = new Set(["cmp", "test", "bt", "ucomisd", "ucomiss", "comisd", "comiss", "ptest", "push"]);
const x86UnaryOperations = new Set(["neg", "not", "inc", "dec", "bswap"]);
const x86AlgebraOperations = new Set([
  "add", "adc", "sub", "sbb", "and", "or", "xor", "imul", "mul", "div", "idiv", "shl", "shr", "sar", "sal", "rol", "ror",
  "rcl", "rcr", "shld", "shrd", "btr", "bts", "btc", "xchg", "xadd", "cmpxchg", "cdq", "cqo", "cwd", "cdqe", "cwde",
  "cbw", "jmp", "call", "ret", "nop",
]);
const x86Prefixes = /^((?:(?:lock|rep|repe|repz|repne|repnz|bnd|notrack|data16) )*)(\S+)(?: (.*))?$/;

// The operation that names an x86 mnemonic's semantics: the mnemonic, or without the size suffix Zydis appends when an
// operand is in memory. Only a known operation loses its last letter, so jb and setb keep theirs.
function x86Operation(mnemonic: string): string {
  const known = (name: string) => x86MoveOperations.has(name) || x86ReadOperations.has(name) || x86UnaryOperations.has(name) || x86AlgebraOperations.has(name);
  if (known(mnemonic)) return mnemonic;
  const stripped = mnemonic.slice(0, -1);
  return "bwlq".includes(mnemonic.at(-1) ?? "") && known(stripped) ? stripped : mnemonic;
}

export const isX86Branch = (operation: string) => /^(jmp|call|ret|j[a-z]+|loop[a-z]*)$/.test(operation);

function parseX86Operand(text: string, branch: boolean): Operand | null {
  let prefix = "";
  let body = text;
  if (body.startsWith("*")) {
    prefix = "*";
    body = body.slice(1);
  }
  if (body.startsWith("$")) {
    const value = parseNumber(body.slice(1));
    return value === null || prefix ? null : { kind: "immediate", value };
  }
  const register = /^%([a-z][a-z0-9]*)$/.exec(body);
  if (register) {
    const general = x86Registers.get(register[1]);
    return general ? { kind: "register", register: general, indirect: prefix === "*" } : { kind: "text", text };
  }
  const memory = /^(%[a-z]+:)?(-?(?:0x[0-9a-f]+|\d+))?\((%[a-z0-9]+)?(?:,(%[a-z0-9]+))?(?:,(\d+))?\)$/.exec(body);
  if (memory) {
    const base = memory[3] ? x86Registers.get(memory[3].slice(1)) ?? null : null;
    const index = memory[4] ? x86Registers.get(memory[4].slice(1)) ?? null : null;
    return {
      kind: "memory",
      memory: {
        prefix: prefix + (memory[1] ?? ""),
        base,
        baseText: memory[3] && !base ? memory[3] : null,
        index,
        indexText: memory[4] && !index ? memory[4] : null,
        scale: memory[5] ?? "",
        displacement: memory[2] ? parseNumber(memory[2])! : 0n,
        absolute: false,
        writeback: "",
      },
    };
  }
  const absolute = /^(%[a-z]+:)?(-?(?:0x[0-9a-f]+|\d+))$/.exec(body);
  if (absolute) {
    const value = wrap(parseNumber(absolute[2])!, 64);
    // Zydis prints a branch's relative target, and a RIP-relative operand, as the address it reaches.
    if (branch && !prefix && !absolute[1]) return { kind: "target", address: value };
    const memoryOperand: Memory = { prefix: prefix + (absolute[1] ?? ""), base: null, baseText: null, index: null, indexText: null, scale: "", displacement: value, absolute: true, writeback: "" };
    return { kind: "memory", memory: memoryOperand };
  }
  return null;
}

// The mnemonic, the operation and the operands of an x86_64 instruction's text. unreadable makes the error for what the
// tool cannot classify.
export function parseX86Instruction(text: string, unreadable: (what: string) => Error): ParsedInstruction {
  const match = x86Prefixes.exec(text);
  if (!match) throw unreadable("an instruction");
  const mnemonic = `${match[1]}${match[2]}`;
  const operation = x86Operation(match[2]);
  const branch = isX86Branch(operation);
  const operands: Operand[] = [];
  // canonical.ts's rule 1 drops a nop whatever its encoding, so its operands need no reading.
  for (const operandText of match[3] && !isNop(operation) ? match[3].split(", ") : []) {
    const operand = parseX86Operand(operandText, branch);
    if (!operand) throw unreadable(`the operand ${clip(operandText)} of`);
    operands.push(operand);
  }
  return { mnemonic, operation, operands };
}

export function x86Effects(operation: string, count: number): Effects {
  const every = (role: Role): Role[] => Array.from({ length: count }, () => role);
  const last = (role: Role): Role[] => Array.from({ length: count }, (_, i) => (i === count - 1 ? role : "source"));
  const effects = (roles: Role[], implicitReads: string[] = [], implicitWrites: string[] = []): Effects => ({ roles, implicitReads, implicitWrites });
  if (isX86Branch(operation) || x86ReadOperations.has(operation)) return effects(every("source"));
  switch (operation) {
    case "imul":
      return count === 1 ? effects(["source"], ["rax"], ["rax", "rdx"]) : effects(last(count === 3 ? "write" : "rmw"));
    case "mul":
    case "div":
    case "idiv":
      return effects(every("source"), ["rax", "rdx"], ["rax", "rdx"]);
    case "cdq":
    case "cqo":
    case "cwd":
      return effects([], ["rax"], ["rdx"]);
    case "cdqe":
    case "cwde":
    case "cbw":
      return effects([], ["rax"], ["rax"]);
    case "xchg":
    case "xadd":
      return effects(every("rmw"));
    case "cmpxchg":
      return effects(last("rmw"), ["rax"], ["rax"]);
  }
  if (x86UnaryOperations.has(operation)) return effects(every("rmw"));
  if (/^set/.test(operation)) return effects(every("write"));
  if (/^cmov/.test(operation)) return effects(last("rmw"));
  if (x86MoveOperations.has(operation) || (/^v/.test(operation) && count >= 3)) return effects(last("write"));
  return effects(last("rmw"));
}
