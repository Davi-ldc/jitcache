// Canonical code keeps each instruction's mnemonic and operands without its address, annotation and comment. Then,
// within each straight-line run, which ends at every branch, before every branch target and at every block boundary,
// four rules remove what random draws and value-dependent encodings vary:
// 1. Every nop is dropped, whatever its encoding, and so is every breakpoint after a listing's last other instruction,
//    the padding up to the code's allocation granule.
// 2. Constants fold. An instruction that sets a register from immediates alone, or from the register's known constant
//    and an immediate, is dropped, and the next instruction that reads the register gets the constant in its place.
//    The assembler's temporaries (r11 on x86_64, x16 and x17 on ARM64) keep their constant until something writes them;
//    any other register gets a canonical `set <register>, <c>` before its reader, or at the end of the run when nothing
//    reads it, and its knowledge ends there.
// 3. Split immediates merge: two consecutive add, sub, and, or or xor of an immediate into one register become one, and
//    `lea a(%s), %d` followed by `add $b, %d` becomes `lea (a+b)(%s), %d`.
// 4. On ARM64, a conditional branch over a single `b T` becomes the inverted conditional branch to T.
// Every branch target, immediate, folded constant and address operand is then replaced by what it names (RunNames.name
// in naming.ts), so the two runs compare by what their code refers to.
//
// Rules 2 and 3 work on the general-purpose registers. A register other than a temporary whose constant no instruction
// read before something overwrote it gets its set at the end of the run, as an unread one does; a temporary's constant
// ends with the write. An 8- or 16-bit write leaves the rest of the register holding the constant, which a wider reader
// then cannot take in its place.

import { arm64Effects } from "./arm64.ts";
import { instructionWidth, isConditional, isTerminator } from "./decode.ts";
import { type Architecture, type Instruction, type Memory, type Register, isNop, readRegister, targetOf, temporaries } from "./instructions.ts";
import { fold, toSigned, wrap } from "./numbers.ts";
import { x86Effects } from "./x86.ts";

export type ValueRole = "immediate" | "address" | "target" | "indirect";

export type CanonicalOperand =
  | { kind: "text"; text: string }
  | { kind: "register"; register: Register; indirect: boolean }
  | { kind: "value"; value: bigint; width: number; role: ValueRole }
  | { kind: "memory"; memory: Memory; base: bigint | null }; // base: the base register's constant, which rule 2 substituted

export interface CanonicalInstruction {
  key: number; // the raw instruction it stands for, which branch targets name it by
  mnemonic: string;
  operands: CanonicalOperand[];
}

interface Known {
  value: bigint;
  temporary: boolean;
  read: boolean; // a temporary's, once an instruction read its constant; another register's knowledge ends at that point
  fresh: number | null; // null while the whole register holds the constant; after an 8- or 16-bit write, the bits it wrote
}

type FoldOutcome =
  | { kind: "set"; family: string; value: bigint; own: boolean } // own: built from the register's own known constant
  | { kind: "rewrite"; family: string; instructions: { mnemonic: string; operands: CanonicalOperand[] }[] };

const registerOperand = (register: Register): CanonicalOperand => ({ kind: "register", register, indirect: false });

// x86_64's zeroing `xor %r, %r`, which sets the register to 0 without reading it.
function isZeroingXor(architecture: Architecture, instruction: Instruction): boolean {
  const [a, b] = instruction.operands;
  return architecture === "x86_64" && instruction.operation === "xor" && instruction.operands.length === 2 && a.kind === "register" && b.kind === "register" && a.register.name === b.register.name;
}

export class Canonicalizer {
  private readonly state = new Map<string, Known>();
  private entries: CanonicalInstruction[] = [];
  private runEnd: CanonicalInstruction[] = [];

  constructor(private readonly architecture: Architecture) {}

  // One canonical list per block. A straight-line run ends at every branch and before every branch target and block.
  canonicalize(instructions: readonly Instruction[], targets: ReadonlySet<bigint>, blockCount: number): CanonicalInstruction[][] {
    const blocks: CanonicalInstruction[][] = Array.from({ length: blockCount }, () => []);
    for (let i = 0; i < instructions.length; ++i) {
      const instruction = instructions[i];
      const previous = instructions[i - 1];
      if (previous && (instruction.block !== previous.block || targets.has(instruction.address) || isTerminator(this.architecture, previous.operation))) {
        // Only ARM64's temporaries, which its assembler caches until a Label or Jump::link, keep their constants across
        // the fall-through of a conditional branch.
        const persist = this.architecture === "aarch64" && isConditional(this.architecture, previous.operation) && instruction.block === previous.block && !targets.has(instruction.address);
        blocks[previous.block].push(...this.finishRun(previous, i - 1, persist));
      }
      this.process(instruction, i);
    }
    const last = instructions.at(-1);
    if (last) blocks[last.block].push(...this.finishRun(last, instructions.length - 1, false));
    return blocks;
  }

  // Rule 2's canonical `set <register>, <c>`, here, before the instruction being processed, or at the end of the run.
  // The register's knowledge ends with it.
  private materialize(family: string, key: number, where: "here" | "end") {
    const known = this.state.get(family)!;
    const name = this.architecture === "x86_64" ? `%${family}` : family;
    const set: CanonicalInstruction = { key, mnemonic: "set", operands: [{ kind: "text", text: name }, { kind: "value", value: known.value, width: 64, role: "immediate" }] };
    (where === "here" ? this.entries : this.runEnd).push(set);
    this.state.delete(family);
  }

  private observe(known: Known, register: Register): "value" | "fresh" | "stale" {
    if (known.fresh === null) return "value";
    return !register.high && register.width <= known.fresh ? "fresh" : "stale";
  }

  // The instruction at index reads these registers. A temporary keeps its constant; any other register's set goes before
  // its reader, here, and its knowledge ends, as does that of every register the reader cannot take as a constant.
  private read(consumed: ReadonlySet<string>, unsubstitutable: ReadonlySet<string>, index: number, own: string | null) {
    for (const family of unsubstitutable) {
      if (family !== own && this.state.has(family)) this.materialize(family, index, "here");
    }
    for (const family of consumed) {
      const known = family === own ? undefined : this.state.get(family);
      if (!known) continue;
      if (known.temporary) known.read = true;
      else this.materialize(family, index, "here");
    }
  }

  // A full write ends a register's constant: a temporary's with it, and another register's, which no instruction read,
  // with its set at the end of the run.
  private overwritten(family: string, index: number) {
    const known = this.state.get(family);
    if (!known) return;
    if (known.temporary) this.state.delete(family);
    else this.materialize(family, index, "end");
  }

  // An 8- or 16-bit write leaves the other bits holding the constant.
  private written(register: Register, index: number) {
    const known = this.state.get(register.family);
    if (!known) return;
    if (this.architecture === "aarch64" || register.width >= 32) this.overwritten(register.family, index);
    else known.fresh = register.high ? 0 : Math.max(known.fresh ?? 0, register.width);
  }

  private process(instruction: Instruction, index: number) {
    if (isNop(instruction.operation)) return; // rule 1
    const architecture = this.architecture;
    const effects = architecture === "x86_64" ? x86Effects(instruction.operation, instruction.operands.length) : arm64Effects(instruction.operation, instruction.operands.length);
    const width = instructionWidth(architecture, instruction);
    const zeroing = isZeroingXor(architecture, instruction);
    const operands: CanonicalOperand[] = [];
    const consumed = new Set<string>(); // the registers whose constant the instruction takes in their place
    const unsubstitutable = new Set<string>(); // the known registers it reads in a way no constant can stand for
    let absoluteAccess = false;
    instruction.operands.forEach((operand, position) => {
      const role = effects.roles[position] ?? "source";
      switch (operand.kind) {
        case "register": {
          const register = operand.register;
          const known = this.state.get(register.family);
          if (known && role !== "write" && !zeroing) {
            const view = this.observe(known, register);
            if (view === "value" && role === "source") {
              // Rule 2: the reader gets the constant in its place.
              operands.push({ kind: "value", value: readRegister(known.value, register), width: register.high ? 8 : register.width, role: operand.indirect ? "indirect" : "immediate" });
              consumed.add(register.family);
              return;
            }
            if (view === "stale") unsubstitutable.add(register.family);
          }
          operands.push({ kind: "register", register, indirect: operand.indirect });
          return;
        }
        case "memory": {
          let memory = operand.memory;
          let base: bigint | null = null;
          const known = memory.base ? this.state.get(memory.base.family) : undefined;
          if (memory.base && known) {
            const view = this.observe(known, memory.base);
            if (view === "value" && !memory.writeback) {
              base = readRegister(known.value, memory.base);
              consumed.add(memory.base.family);
              // ARM64's uncached access through the memory temporary adds xzr, which adds nothing.
              if (memory.index?.family === "zr") memory = { ...memory, index: null, indexText: null, scale: "" };
              absoluteAccess = !memory.index && !memory.indexText;
            } else if (view === "stale" || memory.writeback) unsubstitutable.add(memory.base.family);
          }
          if (memory.index && this.state.has(memory.index.family)) unsubstitutable.add(memory.index.family);
          operands.push({ kind: "memory", memory, base });
          return;
        }
        case "immediate":
          operands.push({ kind: "value", value: wrap(operand.value, width), width, role: "immediate" });
          return;
        case "target":
          operands.push({ kind: "value", value: operand.address, width: 64, role: "target" });
          return;
        case "text":
          operands.push(operand);
          return;
      }
    });
    for (const family of effects.implicitReads) {
      if (this.state.has(family)) unsubstitutable.add(family);
    }

    const outcome = architecture === "x86_64" ? this.foldX86(instruction, operands) : this.foldARM64(instruction, operands);
    if (outcome?.kind === "set") {
      // Rule 2 drops the instruction. One that builds the register's next constant from its own reads no constant; any
      // other register's constant it read gets its set in the instruction's place.
      this.read(consumed, unsubstitutable, index, outcome.own ? outcome.family : null);
      if (!outcome.own) this.overwritten(outcome.family, index);
      this.state.set(outcome.family, { value: outcome.value, temporary: temporaries[architecture].has(outcome.family), read: false, fresh: null });
      return;
    }

    if (outcome?.kind === "rewrite") this.state.delete(outcome.family); // the rewritten instruction holds the constant
    else {
      // A known register that the instruction both reads and writes keeps no constant a rule could fold.
      instruction.operands.forEach((operand, position) => {
        if (operand.kind === "register" && effects.roles[position] === "rmw" && this.state.has(operand.register.family)) unsubstitutable.add(operand.register.family);
      });
    }
    this.read(consumed, unsubstitutable, index, null);
    // An access at a constant address is one access, whichever encoding reached it: ARM64's cached memory temporary
    // reaches it with an unscaled offset (ldur, stur) where a fresh materialization adds nothing to the address.
    const mnemonic = absoluteAccess && architecture === "aarch64" ? instruction.mnemonic.replace(/^(ld|st)ur/, "$1r") : instruction.mnemonic;
    const emitted = outcome?.kind === "rewrite" ? outcome.instructions : [{ mnemonic, operands }];
    for (const item of emitted) this.entries.push({ key: index, mnemonic: item.mnemonic, operands: item.operands });
    if (isTerminator(architecture, instruction.operation)) return; // the run ends here, with the state it leaves
    instruction.operands.forEach((operand, position) => {
      const role = effects.roles[position];
      if (operand.kind === "register" && (role === "write" || role === "rmw")) this.written(operand.register, index);
      else if (operand.kind === "memory" && operand.memory.writeback && operand.memory.base) this.written(operand.memory.base, index);
    });
    for (const family of effects.implicitWrites) this.overwritten(family, index);
  }

  // Rule 2 and the blinded forms of their own, on x86_64.
  private foldX86(instruction: Instruction, operands: CanonicalOperand[]): FoldOutcome | null {
    const operation = instruction.operation;
    const destination = instruction.operands.at(-1);
    if (destination?.kind !== "register" || destination.indirect) return null;
    const d = destination.register;
    if (d.width < 32 || d.high) return null;
    const set = (value: bigint, own: boolean): FoldOutcome => ({ kind: "set", family: d.family, value: wrap(value, d.width), own });
    // A blinded move(Imm32) whose key equals the value emits the zeroing xor.
    if (isZeroingXor("x86_64", instruction)) return set(0n, false);
    const source = operands.length === 2 ? operands[0] : null;
    const immediate = source?.kind === "value" && source.role === "immediate" ? source.value : null;
    if ((operation === "mov" || operation === "movabs") && immediate !== null) return set(immediate, false);
    const known = this.state.get(d.family);
    const value = known && known.fresh === null ? wrap(known.value, d.width) : null;
    if (value === null) return null;
    if (immediate !== null && /^(xor|add|sub|and|or|rol|ror)$/.test(operation)) return set(fold(operation, value, immediate, d.width)!, true);
    if ((operation === "rol" || operation === "ror") && operands.length === 1) return set(fold(operation, value, 1n, d.width)!, true);
    if (source?.kind === "register" && !source.indirect) {
      const s = source.register;
      if (s.family === d.family || s.width !== d.width || s.high) return null;
      const constant: CanonicalOperand = { kind: "value", value, width: d.width, role: "immediate" };
      // The commutative forms that build the constant in their destination (branchAdd32, and32, or32, xor32).
      if (/^(add|and|or|xor)$/.test(operation))
        return { kind: "rewrite", family: d.family, instructions: [{ mnemonic: "mov", operands: [source, registerOperand(d)] }, { mnemonic: instruction.mnemonic, operands: [constant, registerOperand(d)] }] };
      // branchMul32's and mul32's.
      if (operation === "imul") return { kind: "rewrite", family: d.family, instructions: [{ mnemonic: instruction.mnemonic, operands: [constant, source, registerOperand(d)] }] };
    }
    return null;
  }

  // Rule 2 on ARM64: the materializations of a constant, value-dependent in length, and the cached temporaries' reuse.
  private foldARM64(instruction: Instruction, operands: CanonicalOperand[]): FoldOutcome | null {
    const operation = instruction.operation;
    const destination = operands[0];
    if (destination?.kind !== "register" || destination.register.family === "zr" || destination.register.family === "sp") return null;
    const d = destination.register;
    const set = (value: bigint, own: boolean): FoldOutcome => ({ kind: "set", family: d.family, value: wrap(value, d.width), own });
    const valueOf = (operand: CanonicalOperand | undefined): bigint | null => {
      if (operand?.kind === "value" && operand.role === "immediate") return operand.value;
      if (operand?.kind === "register" && operand.register.family === "zr") return 0n;
      return null;
    };
    const shiftOf = (operand: CanonicalOperand | undefined): bigint | null => {
      if (!operand) return 0n;
      const match = operand.kind === "text" ? /^lsl #(0x[0-9a-f]+|\d+)$/.exec(operand.text) : null;
      return match ? BigInt(match[1]) : null;
    };
    if (operation === "mov" && operands.length === 2) {
      const value = valueOf(operands[1]);
      return value === null ? null : set(value, false);
    }
    if ((operation === "movz" || operation === "movn") && operands.length <= 3) {
      const value = valueOf(operands[1]);
      const shift = shiftOf(operands[2]);
      if (value === null || shift === null) return null;
      const shifted = wrap(wrap(value, 16) << shift, d.width);
      return set(operation === "movz" ? shifted : ~shifted, false);
    }
    if (operation === "movk" && operands.length <= 3) {
      const known = this.state.get(d.family);
      const value = valueOf(operands[1]);
      const shift = shiftOf(operands[2]);
      if (!known || known.fresh !== null || value === null || shift === null) return null;
      return set((known.value & ~(0xffffn << shift)) | (wrap(value, 16) << shift), true);
    }
    if (operation === "orr" && operands.length === 3 && operands[1].kind === "register" && operands[1].register.family === "zr") {
      const value = valueOf(operands[2]);
      return value === null ? null : set(value, false);
    }
    // From the register's own known constant, which its reader took in its place, and an immediate.
    const own = instruction.operands[1];
    if (/^(add|sub|and|orr|eor|ror)$/.test(operation) && (operands.length === 3 || operands.length === 4) && own?.kind === "register" && own.register.family === d.family) {
      const left = valueOf(operands[1]);
      const right = valueOf(operands[2]);
      const shift = shiftOf(operands[3]);
      if (left === null || right === null || shift === null) return null;
      return set(fold(operation, left, operation === "ror" ? right : right << shift, d.width)!, true);
    }
    return null;
  }

  private finishRun(last: Instruction, lastIndex: number, persistTemporaries: boolean): CanonicalInstruction[] {
    // The sets the end of the run writes go before its branch, which reads what the run left, or after its last
    // instruction. A branch is never folded, so its own instruction is the last the run emitted.
    const terminated = isTerminator(this.architecture, last.operation) && this.entries.length > 0;
    const endKey = terminated ? lastIndex : lastIndex + 0.5;
    for (const [family, known] of [...this.state]) {
      if (known.temporary && known.fresh === null && (persistTemporaries || known.read)) {
        if (!persistTemporaries) this.state.delete(family);
        continue;
      }
      this.materialize(family, endKey, "end");
    }
    const ends = this.runEnd.map(set => ({ ...set, key: endKey }));
    const output = terminated ? [...this.entries.slice(0, -1), ...ends, this.entries.at(-1)!] : [...this.entries, ...ends];
    this.entries = [];
    this.runEnd = [];
    return this.architecture === "x86_64" ? mergeSplitImmediates(output) : output;
  }
}

// Rule 3: x86_64's addition, and and or blinding of live registers, and lea's.
function mergeSplitImmediates(list: CanonicalInstruction[]): CanonicalInstruction[] {
  const output: CanonicalInstruction[] = [];
  for (const instruction of list) {
    const previous = output.at(-1);
    const merged = previous ? mergePair(previous, instruction) : null;
    if (merged) output[output.length - 1] = merged;
    else output.push(instruction);
  }
  return output;
}

function mergePair(a: CanonicalInstruction, b: CanonicalInstruction): CanonicalInstruction | null {
  const destination = (instruction: CanonicalInstruction) => {
    const operand = instruction.operands.length === 2 ? instruction.operands[1] : null;
    return operand?.kind === "register" && !operand.indirect ? operand.register : null;
  };
  const immediate = (instruction: CanonicalInstruction) => {
    const operand = instruction.operands[0];
    return operand?.kind === "value" && operand.role === "immediate" ? operand : null;
  };
  const da = destination(a);
  const db = destination(b);
  const ib = immediate(b);
  if (!da || !db || !ib || da.name !== db.name || da.width < 32) return null;
  if (a.mnemonic === b.mnemonic && /^(add|sub|and|or|xor)$/.test(a.mnemonic)) {
    const ia = immediate(a);
    if (!ia) return null;
    const value = fold(a.mnemonic === "sub" ? "add" : a.mnemonic, ia.value, ib.value, da.width)!;
    return { key: a.key, mnemonic: a.mnemonic, operands: [{ ...ia, value, width: da.width }, a.operands[1]] };
  }
  const address = a.operands[0];
  if (a.mnemonic === "lea" && b.mnemonic === "add" && address.kind === "memory" && address.base === null) {
    const memory = address.memory;
    if (!memory.base || memory.index || memory.indexText || memory.absolute || memory.prefix) return null;
    const merged: Memory = { ...memory, displacement: memory.displacement + toSigned(ib.value, db.width) };
    return { key: a.key, mnemonic: "lea", operands: [{ kind: "memory", memory: merged, base: null }, a.operands[1]] };
  }
  return null;
}

// Rule 4 on ARM64: a conditional branch over a single `b T` is the inverted conditional branch to T.
const inversions = new Map<string, string>([
  ["b.eq", "b.ne"], ["b.hs", "b.lo"], ["b.mi", "b.pl"], ["b.vs", "b.vc"], ["b.hi", "b.ls"], ["b.ge", "b.lt"], ["b.gt", "b.le"],
  ["cbz", "cbnz"], ["tbz", "tbnz"],
]);
for (const [a, b] of [...inversions]) inversions.set(b, a);

export function compactBranchPairs(instructions: Instruction[]): Instruction[] {
  const targeted = new Set<bigint>();
  for (const instruction of instructions) {
    const target = targetOf(instruction);
    if (target !== null) targeted.add(target);
  }
  const output: Instruction[] = [];
  for (let i = 0; i < instructions.length; ++i) {
    const branch = instructions[i];
    const jump = instructions[i + 1];
    const inverted = inversions.get(branch.operation);
    if (inverted && jump && jump.operation === "b" && jump.block === branch.block && !targeted.has(jump.address) && targetOf(branch) === jump.address + 4n) {
      const target = targetOf(jump);
      if (target !== null) {
        output.push({ ...branch, mnemonic: inverted, operation: inverted, operands: branch.operands.map(operand => (operand.kind === "target" ? { kind: "target", address: target } : operand)) });
        ++i;
        continue;
      }
    }
    output.push(branch);
  }
  return output;
}
