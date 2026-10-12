#!/usr/bin/env bun
// The pin comparison (SPEC-integrator.harness.md section 11): whether the off path emits the pin's code. The runner
// (run-jitcache-tests --pin) starts each Off option set of a jsc-hosted script once with this build's jsc and once with
// the pin's, both with pinRunOptions, and hands the two runs to compareRuns. Neither build is instrumented, so each run
// describes its code only through what the engine prints: the logJIT header of every finalized allocation, the
// per-bytecode dump of each baseline compilation, the disassembly of every MathIC snippet and inline rewrite, and the perf
// JIT dump of the bytes each allocation holds. compareRuns turns each run's code into canonical code (section 11.3),
// walking the run's allocations in finalization order, and compares the two runs (section 11.4).
//
// Where section 11.3 leaves a choice, the tool decides it as follows.
// - Rules 2 and 3 work on the general-purpose registers. A register other than a temporary whose constant no instruction
//   read before something overwrote it gets its set at the end of the run, as an unread one does; a temporary's
//   constant ends with the write. An 8- or 16-bit write leaves the rest of the register holding the constant, which a
//   wider reader then cannot take in its place.
// - A body's code is the range its dump's `Code at [s, e)` gives, which the allocation's header starts at and may
//   exceed, since the JIT heap rounds an allocation up. The dump stops before the breakpoints that pad the code to its
//   allocation granule, so on x86_64 the last dumped instruction, the tail's near jmp, takes its length from its
//   encoding in the JIT dump.
// - A MathIC inline rewrite writes into an earlier baseline body through a LinkBuffer that owns no memory, so its header
//   prints the empty range [p, p) and the tool takes its code from its listing, whose one jump is jumpThunk's: on
//   x86_64 the near jmp.
// - On x86_64 a body's branch displacements come from the JIT dump except where an inline rewrite of the same run covers
//   them, since the dump copies the bytes on a queue of its own, after the rewrite or before it
//   (SPEC-integrator.harness.md N19 and section 11.3, Targets).
//
// Standalone, on the files a runner kept:
//   bun Tools/Scripts/jitcache-pin-compare.ts --this=<this build's WebKit directory> --pin=<the pin's WebKit directory>
//       <scratch>/pin<j>
// compares <scratch>/pin<j>.this.{status,stdout,stderr} and the JIT dump in <scratch>/pin<j>.this/ with the pin's.

import { spawnSync } from "node:child_process";
import { closeSync, openSync, readFileSync, readSync, readdirSync, realpathSync, statSync } from "node:fs";
import { homedir } from "node:os";
import { join, resolve } from "node:path";

export type Architecture = "x86_64" | "aarch64";

export interface RunStatus {
  code: number | null;
  signal: string | null;
}

export interface JITDumpRecord {
  address: bigint;
  size: number;
  name: string;
  bytes: Uint8Array;
}

// What one run left: its end, its standard output and error, and the records of its JIT dump in file order.
export interface RunCapture {
  status: RunStatus;
  stdout: Uint8Array;
  stderr: string;
  jitDump: readonly JITDumpRecord[];
}

export interface Segment {
  start: bigint;
  end: bigint;
}

export interface EngineSymbol {
  address: bigint;
  name: string;
}

// The build's jsc, which Bun's build flags load at its link-time address (SPEC-integrator.md N15): its loaded segments and
// its symbols, sorted by address and then by name.
export interface EngineImage {
  readonly segments: readonly Segment[];
  readonly symbols: readonly EngineSymbol[];
}

export interface PinSide {
  capture: RunCapture;
  engine: EngineImage;
}

export type PinCheck = "pin-status" | "pin-output" | "pin-allocations" | "pin-code" | "pin-unreadable";

export interface PinDifference {
  check: PinCheck;
  detail: string;
}

export interface PinComparison {
  difference: PinDifference | null;
  allocations: number; // the allocations both runs finalized alike
  bodies: number; // the baseline bodies among them
}

// Section 11.2: the options both runs take last, so they override the run's own.
export function pinRunOptions(jitDumpDirectory: string): string[] {
  return [
    "--useConcurrentJIT=false",
    "--useGC=false",
    "--dumpDisassembly=true",
    "--logJIT=true",
    "--useJITDump=true",
    `--jitDumpDirectory=${jitDumpDirectory}`,
  ];
}

// ---------------------------------------------------------------------------------------------------------------------
// Numbers

// The top of user space on both architectures: a value from here up, such as a JSValue tag, stays literal.
const userSpaceHigh = 1n << 48n;
// No heap, VM, executable-pool or structure-reservation object lies below 4 GiB on either architecture, natively or
// under QEMU, while Bun's non-PIE jsc does, so a value under it that names no allocation and no symbol is an integer.
const anonymousLow = 1n << 32n;

const widthMask = (width: number) => (1n << BigInt(width)) - 1n;
const wrap = (value: bigint, width: number) => value & widthMask(width);

function toSigned(value: bigint, width: number): bigint {
  const wrapped = wrap(value, width);
  return wrapped >> BigInt(width - 1) ? wrapped - (1n << BigInt(width)) : wrapped;
}

function parseNumber(text: string): bigint | null {
  const match = /^(-)?(0x[0-9a-fA-F]+|\d+)$/.exec(text);
  if (!match) return null;
  const magnitude = BigInt(match[2]);
  return match[1] ? -magnitude : magnitude;
}

const hex = (value: bigint) => (value < 0n ? `-0x${(-value).toString(16)}` : `0x${value.toString(16)}`);

function rotateRight(value: bigint, amount: number, width: number): bigint {
  const count = BigInt(((amount % width) + width) % width);
  const wrapped = wrap(value, width);
  return count ? wrap((wrapped >> count) | (wrapped << (BigInt(width) - count)), width) : wrapped;
}

// Rule 2's operations on a register's known constant and an immediate, at the operation's width.
function fold(operation: string, left: bigint, right: bigint, width: number): bigint | null {
  switch (operation) {
    case "add":
      return wrap(left + right, width);
    case "sub":
      return wrap(left - right, width);
    case "and":
      return wrap(left & right, width);
    case "or":
    case "orr":
      return wrap(left | right, width);
    case "xor":
    case "eor":
      return wrap(left ^ right, width);
    case "ror":
      return rotateRight(left, Number(wrap(right, 8)), width);
    case "rol":
      return rotateRight(left, width - (Number(wrap(right, 8)) % width), width);
    default:
      return null;
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// The engine's symbols

export function makeEngineImage(segments: readonly Segment[], symbols: readonly EngineSymbol[]): EngineImage {
  const sorted = [...symbols].sort((a, b) => (a.address !== b.address ? (a.address < b.address ? -1 : 1) : a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  return { segments: [...segments], symbols: sorted };
}

const engineImages = new Map<string, EngineImage>();

// The jsc executable's PT_LOAD segments and its llvm-nm symbol table, read once per executable and process.
export function loadEngineImage(executable: string): EngineImage {
  const path = realpathSync(executable);
  const key = `${path}:${statSync(path).mtimeMs}`;
  let image = engineImages.get(key);
  if (!image) {
    image = makeEngineImage(loadSegments(path), loadSymbols(path));
    engineImages.set(key, image);
  }
  return image;
}

function loadSegments(path: string): Segment[] {
  const fd = openSync(path, "r");
  try {
    const header = Buffer.alloc(64);
    if (readSync(fd, header, 0, 64, 0) !== 64 || header.readUInt32BE(0) !== 0x7f454c46 || header[4] !== 2 || header[5] !== 1)
      throw new Error(`${path} is no little-endian 64-bit ELF file`);
    const tableOffset = Number(header.readBigUInt64LE(0x20));
    const entrySize = header.readUInt16LE(0x36);
    const count = header.readUInt16LE(0x38);
    const table = Buffer.alloc(entrySize * count);
    readSync(fd, table, 0, table.length, tableOffset);
    const segments: Segment[] = [];
    for (let i = 0; i < count; ++i) {
      const entry = i * entrySize;
      if (table.readUInt32LE(entry) !== 1) continue; // PT_LOAD
      const start = table.readBigUInt64LE(entry + 16);
      segments.push({ start, end: start + table.readBigUInt64LE(entry + 40) });
    }
    return segments;
  } finally {
    closeSync(fd);
  }
}

// The llvm-nm of JITCACHE_LLVM_PREFIX, the toolchain build.ts builds with, or one on PATH.
function llvmNm(): { command: string; env: Record<string, string | undefined> } {
  const prefix = resolve(process.env.JITCACHE_LLVM_PREFIX || join(homedir(), "collo-local/tools/llvm-21"));
  const local = join(prefix, "usr/lib/llvm-21/bin/llvm-nm");
  if (statSync(local, { throwIfNoEntry: false })?.isFile()) {
    const libraries = join(prefix, "usr/lib", process.arch === "arm64" ? "aarch64-linux-gnu" : "x86_64-linux-gnu");
    const path = [libraries, join(prefix, "usr/lib/llvm-21/lib"), process.env.LD_LIBRARY_PATH].filter(Boolean).join(":");
    return { command: local, env: { ...process.env, LD_LIBRARY_PATH: path } };
  }
  return { command: Bun.which("llvm-nm-21") ?? "llvm-nm", env: { ...process.env } };
}

function loadSymbols(path: string): EngineSymbol[] {
  const { command, env } = llvmNm();
  const run = spawnSync(command, ["--defined-only", "--demangle", path], { env, encoding: "utf8", maxBuffer: 2 ** 31 - 1 });
  if (run.error || run.status !== 0)
    throw new Error(`${command} could not list the symbols of ${path}: ${run.error?.message ?? run.stderr.trim()}`);
  const symbols: EngineSymbol[] = [];
  for (const line of run.stdout.split("\n")) {
    const match = /^([0-9a-f]+) ([A-Za-z]) (.+)$/.exec(line);
    // Absolute and debugging symbols name no address of the loaded image.
    if (match && !"AaNn".includes(match[2])) symbols.push({ address: BigInt(`0x${match[1]}`), name: match[3] });
  }
  return symbols;
}

// The symbol at or below the value, by its first name, and the offset, for a value inside a loaded segment.
function engineName(engine: EngineImage, value: bigint): string | null {
  if (!engine.segments.some(segment => value >= segment.start && value < segment.end)) return null;
  const symbols = engine.symbols;
  let low = 0;
  let high = symbols.length;
  while (low < high) {
    const middle = (low + high) >> 1;
    if (symbols[middle].address <= value) low = middle + 1;
    else high = middle;
  }
  if (!low) return null;
  let index = low - 1;
  while (index > 0 && symbols[index - 1].address === symbols[index].address) --index;
  const offset = value - symbols[index].address;
  return offset ? `${symbols[index].name}+${hex(offset)}` : symbols[index].name;
}

// ---------------------------------------------------------------------------------------------------------------------
// The JIT dump (assembler/PerfLog.cpp): a 40-byte header, then records, each a type, a total size and a timestamp; a
// JIT_CODE_LOAD record adds the pid, tid, vma, code address, code size and index, the name and the code's bytes.

export function readJITDump(directory: string): JITDumpRecord[] {
  const records: JITDumpRecord[] = [];
  let names: string[];
  try {
    names = readdirSync(directory).filter(name => name.endsWith(".dump")).sort();
  } catch {
    return records;
  }
  for (const name of names) {
    const file = readFileSync(join(directory, name));
    if (file.length < 40 || file.readUInt32LE(0) !== 0x4a695444) continue;
    let offset = file.readUInt32LE(8);
    while (offset + 16 <= file.length) {
      const type = file.readUInt32LE(offset);
      const size = file.readUInt32LE(offset + 4);
      if (size < 16 || offset + size > file.length) break; // a record the process did not finish writing
      if (type === 0 && size >= 56) {
        const address = file.readBigUInt64LE(offset + 32);
        const codeSize = Number(file.readBigUInt64LE(offset + 40));
        const nameEnd = file.indexOf(0, offset + 56);
        if (nameEnd < 0 || nameEnd + 1 + codeSize > offset + size) break;
        records.push({ address, size: codeSize, name: file.toString("utf8", offset + 56, nameEnd), bytes: file.subarray(nameEnd + 1, nameEnd + 1 + codeSize) });
      }
      offset += size;
    }
  }
  return records;
}

// ---------------------------------------------------------------------------------------------------------------------
// A run's standard error: allocations in finalization order (section 11.3, Allocations)

class Unreadable extends Error {}

interface RawLine {
  address: bigint;
  offset: number | null; // ARM64's <offset> from the start of the dumped code
  text: string; // the instruction without its address, annotation and comment
  line: number;
}

interface DumpBlock {
  key: string; // prologue, main:<bytecode index>, slow:<bytecode index> or tail
  opcode: string | null;
  lines: RawLine[];
}

interface Allocation {
  index: number;
  name: string;
  identity: string; // the name with every hexadecimal number replaced by 0x?, and its ordinal among that name's
  start: bigint;
  end: bigint; // the header's range, [start, end)
  codeEnd: bigint; // the end of its code: a body's dump's, an inline rewrite's listing's, or else the header's
  line: number;
  disassembly: RawLine[] | null; // what followed a header that ends with ':'
  dump: DumpBlock[] | null; // a baseline body's per-bytecode dump, which JIT::link prints before the header
}

const headerPattern = /^Generated JIT code for (.*): \[(0x[0-9a-f]+), (0x[0-9a-f]+)\) (\d+) bytes([.:])$/;
const dumpPattern = /^Generated Baseline JIT code for (.*), instructions size = \d+$/;
const codeAtPattern = /^ {3}Code at \[(0x[0-9a-f]+), (0x[0-9a-f]+)\):$/;
const instructionPattern = /^\s+(?:<(\d+)>\s+)?(0x[0-9a-f]+): ?(.*)$/;
const bytecodePattern = /^ {4}(\(S\) )?\[\s*(\d+)\] (\S+)/;

const clip = (text: string) => JSON.stringify(text.length > 160 ? `${text.slice(0, 160)}…` : text);

// The instruction as the disassembler printed it, without the annotation A64DOpcode appends and the comment either
// disassembler appends.
function cleanInstruction(text: string): string {
  let end = text.length;
  for (const marker of [" -> ", "; "]) {
    const at = text.indexOf(marker);
    if (at >= 0 && at < end) end = at;
  }
  return text.slice(0, end).trim();
}

function rawLine(match: RegExpExecArray, line: number): RawLine {
  return { address: BigInt(match[2]), offset: match[1] === undefined ? null : Number(match[1]), text: cleanInstruction(match[3]), line };
}

interface PendingDump {
  codeBlock: string;
  start: bigint | null;
  end: bigint | null;
  blocks: DumpBlock[];
  current: DumpBlock | null;
  phase: "header" | "main" | "slow" | "tail";
  line: number;
}

function parseAllocations(stderr: string): Allocation[] {
  const allocations: Allocation[] = [];
  const ordinals = new Map<string, number>();
  let dump: PendingDump | null = null;
  let listing: RawLine[] | null = null;
  const lines = stderr.split("\n");
  for (let n = 0; n < lines.length; ++n) {
    const text = lines[n];
    const line = n + 1;
    if (text.startsWith("Generated JIT code for ")) {
      listing = null;
      const match = headerPattern.exec(text);
      if (!match) throw new Unreadable(`line ${line}: an allocation header it cannot parse: ${clip(text)}`);
      const start = BigInt(match[2]);
      const end = BigInt(match[3]);
      if (end - start !== BigInt(match[4])) throw new Unreadable(`line ${line}: an allocation header whose range and size disagree: ${clip(text)}`);
      const name = match[1].replace(/0x[0-9a-fA-F]+/g, "0x?");
      const ordinal = ordinals.get(name) ?? 0;
      ordinals.set(name, ordinal + 1);
      const allocation: Allocation = { index: allocations.length, name: match[1], identity: `${name} #${ordinal}`, start, end, codeEnd: end, line, disassembly: null, dump: null };
      if (dump) {
        // The dump covers LinkBuffer::size() bytes and the header the executable memory, which the JIT heap rounds up.
        if (dump.start === null || dump.end === null) throw new Unreadable(`line ${dump.line}: the baseline dump of ${dump.codeBlock} has no Code at line`);
        if (dump.start !== start || dump.end > end)
          throw new Unreadable(`line ${dump.line}: the baseline dump of ${dump.codeBlock}, [${hex(dump.start)}, ${hex(dump.end)}), lies outside the allocation whose header follows it at line ${line}, [${hex(start)}, ${hex(end)})`);
        allocation.codeEnd = dump.end;
        allocation.dump = dump.blocks;
        dump = null;
      }
      if (match[5] === ":") listing = allocation.disassembly = [];
      allocations.push(allocation);
      continue;
    }
    const dumpMatch = dumpPattern.exec(text);
    if (dumpMatch) {
      listing = null;
      if (dump) throw new Unreadable(`line ${dump.line}: the baseline dump of ${dump.codeBlock} has no allocation header`);
      dump = { codeBlock: dumpMatch[1], start: null, end: null, blocks: [], current: null, phase: "header", line };
      continue;
    }
    if (dump) {
      parseDumpLine(dump, text, line);
      continue;
    }
    if (listing) {
      const instruction = instructionPattern.exec(text);
      if (instruction) listing.push(rawLine(instruction, line));
      else listing = null;
    }
  }
  if (dump) throw new Unreadable(`line ${dump.line}: the baseline dump of ${dump.codeBlock} has no allocation header`);
  return allocations;
}

// JITDisassembler::dump: the header, Source and Code at lines, the prologue, one block per bytecode headed by its index
// and opcode, (End Of Main Path), the slow-path blocks marked (S), (End Of Slow Path) and the tail. A line that is none
// of these continues the bytecode line above it, as a string constant with a newline does.
function parseDumpLine(dump: PendingDump, text: string, line: number) {
  if (dump.phase === "header") {
    const range = codeAtPattern.exec(text);
    if (range) {
      dump.start = BigInt(range[1]);
      dump.end = BigInt(range[2]);
      dump.phase = "main";
      dump.current = { key: "prologue", opcode: null, lines: [] };
      dump.blocks.push(dump.current);
    }
    return;
  }
  if (text === "    (End Of Main Path)") {
    if (dump.phase !== "main") throw new Unreadable(`line ${line}: a second (End Of Main Path) in the baseline dump of ${dump.codeBlock}`);
    dump.phase = "slow";
    dump.current = null;
    return;
  }
  if (text === "    (End Of Slow Path)") {
    if (dump.phase !== "slow") throw new Unreadable(`line ${line}: (End Of Slow Path) out of place in the baseline dump of ${dump.codeBlock}`);
    dump.phase = "tail";
    dump.current = { key: "tail", opcode: null, lines: [] };
    dump.blocks.push(dump.current);
    return;
  }
  const bytecode = dump.phase === "tail" ? null : bytecodePattern.exec(text);
  if (bytecode) {
    const slow = Boolean(bytecode[1]);
    if (slow !== (dump.phase === "slow")) throw new Unreadable(`line ${line}: a ${slow ? "slow" : "main"}-path block out of place in the baseline dump of ${dump.codeBlock}`);
    dump.current = { key: `${slow ? "slow" : "main"}:${bytecode[2]}`, opcode: bytecode[3].replace(/^\*+/, ""), lines: [] };
    dump.blocks.push(dump.current);
    return;
  }
  const instruction = instructionPattern.exec(text);
  if (!instruction) return;
  if (!dump.current) throw new Unreadable(`line ${line}: an instruction outside every block of the baseline dump of ${dump.codeBlock}`);
  dump.current.lines.push(rawLine(instruction, line));
}

// ---------------------------------------------------------------------------------------------------------------------
// Instructions

interface Register {
  name: string;
  family: string; // the 64-bit register it is part of: rax for eax and al, x0 for w0; zr for xzr and wzr
  width: number;
  high: boolean; // x86 ah, bh, ch, dh
}

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

// The temporaries the assembler reserves: x86_64's scratch register and ARM64's data and memory temporaries.
const temporaries: Record<Architecture, ReadonlySet<string>> = {
  x86_64: new Set(["r11"]),
  aarch64: new Set(["x16", "x17"]),
};

type Operand =
  | { kind: "register"; register: Register; indirect: boolean }
  | { kind: "immediate"; value: bigint }
  | { kind: "target"; address: bigint }
  | { kind: "memory"; memory: Memory }
  | { kind: "text"; text: string };

interface Memory {
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

interface Instruction {
  address: bigint;
  length: number;
  block: number;
  mnemonic: string;
  operation: string; // the mnemonic that names its semantics: x86's without its size suffix
  operands: Operand[];
  text: string;
  line: number;
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

const isX86Branch = (operation: string) => /^(jmp|call|ret|j[a-z]+|loop[a-z]*)$/.test(operation);
const isARM64Branch = (operation: string) => /^(b|bl|br|blr|ret|cbn?z|tbn?z|brk|udf|hlt|bra[a-z]*|blra[a-z]*|reta[a-z]*)$/.test(operation) || /^b\./.test(operation);

function isTerminator(architecture: Architecture, operation: string): boolean {
  return architecture === "x86_64" ? isX86Branch(operation) || operation === "ud2" || operation === "int3" || operation === "hlt" : isARM64Branch(operation);
}

function isConditional(architecture: Architecture, operation: string): boolean {
  if (architecture === "x86_64") return /^j/.test(operation) && operation !== "jmp";
  return (/^b\./.test(operation) && operation !== "b.al" && operation !== "b.nv") || /^(cbn?z|tbn?z)$/.test(operation);
}

const isNop = (operation: string) => /^nop/.test(operation);

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

function parseInstruction(architecture: Architecture, line: RawLine, block: number, start: bigint): Instruction {
  const address = architecture === "aarch64" && line.offset !== null ? start + BigInt(line.offset) : line.address;
  const length = architecture === "aarch64" ? 4 : 0; // x86_64's lengths come from the addresses (RunContext.measure)
  if (!line.text || line.text === "failed-to-format")
    return { address, length, block, mnemonic: "<undecoded>", operation: "<undecoded>", operands: [], text: line.text, line: line.line };
  const unreadable = (what: string) => new Unreadable(`line ${line.line}: ${what} it cannot classify: ${clip(line.text)}`);
  if (architecture === "x86_64") {
    const match = x86Prefixes.exec(line.text);
    if (!match) throw unreadable("an instruction");
    const mnemonic = `${match[1]}${match[2]}`;
    const operation = x86Operation(match[2]);
    const branch = isX86Branch(operation);
    const operands: Operand[] = [];
    // Rule 1 drops a nop whatever its encoding, so its operands need no reading.
    for (const text of match[3] && !isNop(operation) ? match[3].split(", ") : []) {
      const operand = parseX86Operand(text, branch);
      if (!operand) throw unreadable(`the operand ${clip(text)} of`);
      operands.push(operand);
    }
    return { address, length, block, mnemonic, operation, operands, text: line.text, line: line.line };
  }
  const tab = line.text.indexOf("\t");
  const mnemonic = (tab < 0 ? line.text : line.text.slice(0, tab)).trim();
  const branch = isARM64Branch(mnemonic);
  const operands: Operand[] = [];
  for (const text of tab < 0 ? [] : splitOperands(line.text.slice(tab + 1))) {
    const operand = parseARM64Operand(text, branch);
    if (!operand) throw unreadable(`the operand ${clip(text)} of`);
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
  return { address, length, block, mnemonic, operation: mnemonic, operands, text: line.text, line: line.line };
}

// ---------------------------------------------------------------------------------------------------------------------
// Which operands an instruction reads and writes

type Role = "source" | "write" | "rmw";

interface Effects {
  roles: Role[];
  implicitReads: string[];
  implicitWrites: string[];
}

function x86Effects(operation: string, count: number): Effects {
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

function arm64Effects(operation: string, count: number): Effects {
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

// The width of an instruction's operation, which its immediates take: its destination's, or its size suffix's.
function instructionWidth(architecture: Architecture, instruction: Instruction): number {
  if (architecture === "x86_64") {
    const last = instruction.operands.at(-1);
    if (last?.kind === "register") return last.register.high ? 8 : last.register.width;
    if (instruction.mnemonic !== instruction.operation) return { b: 8, w: 16, l: 32, q: 64 }[instruction.mnemonic.at(-1) as "b" | "w" | "l" | "q"] ?? 64;
    return 64;
  }
  const register = instruction.operands.find(operand => operand.kind === "register");
  return register?.kind === "register" && register.register.width === 32 ? 32 : 64;
}

const readRegister = (value: bigint, register: Register) => (register.high ? (value >> 8n) & 0xffn : wrap(value, register.width));

// ---------------------------------------------------------------------------------------------------------------------
// Canonical code (section 11.3, Instructions)

type ValueRole = "immediate" | "address" | "target" | "indirect";

type CanonicalOperand =
  | { kind: "text"; text: string }
  | { kind: "register"; register: Register; indirect: boolean }
  | { kind: "value"; value: bigint; width: number; role: ValueRole }
  | { kind: "memory"; memory: Memory; base: bigint | null }; // base: the base register's constant, which rule 2 substituted

interface CanonicalInstruction {
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

class Canonicalizer {
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

function targetOf(instruction: Instruction): bigint | null {
  const operand = instruction.operands.find(operand => operand.kind === "target");
  return operand?.kind === "target" ? operand.address : null;
}

function compactBranchPairs(instructions: Instruction[]): Instruction[] {
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

// The addresses inside the code that a branch, an adr or an absolute operand names: entries a straight-line run ends at.
function targetsWithin(instructions: readonly Instruction[], start: bigint, end: bigint): Set<bigint> {
  const targets = new Set<bigint>();
  for (const instruction of instructions) {
    for (const operand of instruction.operands) {
      const address = operand.kind === "target" ? operand.address : operand.kind === "memory" && operand.memory.absolute ? operand.memory.displacement : null;
      if (address !== null && address >= start && address < end) targets.add(address);
    }
  }
  return targets;
}

// ---------------------------------------------------------------------------------------------------------------------
// Names (section 11.3, Names) and rendering

// A part of a rendered operand: its text, and for an integer constant inside either build's loaded jsc segments, the
// literal value it stands for. Two parts match when their texts match or their literals do.
interface RenderedPart {
  text: string;
  literal: string | null;
}

interface RenderedInstruction {
  mnemonic: string;
  operands: RenderedPart[][];
  text: string;
}

type CodeNamer = (address: bigint) => string | null;
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

function sameInstruction(a: RenderedInstruction, b: RenderedInstruction): boolean {
  if (a.mnemonic !== b.mnemonic || a.operands.length !== b.operands.length) return false;
  return a.operands.every((parts, i) => {
    const other = b.operands[i];
    return parts.length === other.length && parts.every((part, k) => part.text === other[k].text || (part.literal !== null && part.literal === other[k].literal));
  });
}

// ---------------------------------------------------------------------------------------------------------------------
// One run's units: its baseline bodies, and the MathIC snippets and inline rewrites that belong to them

interface UnitBlock {
  key: string;
  opcode: string | null;
  exempt: boolean; // a main-path block of super_construct or super_construct_varargs (section 11.4, item 3)
  instructions: RenderedInstruction[];
}

interface Unit {
  kind: "body" | "snippet" | "rewrite";
  allocation: Allocation;
  ordinal: number; // a body's, among the run's baseline bodies from 1; a snippet's or a rewrite's body's
  place: string; // a snippet's or a rewrite's block in its body
  blocks: UnitBlock[];
  namer: CodeNamer;
  blockAt: (address: bigint) => string | null;
}

class RunContext {
  readonly allocations: Allocation[];
  private readonly byStart: Allocation[];
  private readonly reach: bigint[];
  private readonly bodyOrdinals = new Map<number, number>();
  private readonly rewriteOwners = new Map<number, Allocation>();
  private readonly rewritesOf = new Map<number, Allocation[]>();
  private readonly records = new Map<string, JITDumpRecord[]>();
  private readonly units = new Map<number, Unit>();
  private readonly heapNumbers = new Map<bigint, number>();

  // literalSegments: both builds' loaded jsc segments, inside which an integer constant also matches an equal literal.
  constructor(readonly architecture: Architecture, capture: RunCapture, private readonly engine: EngineImage, private readonly literalSegments: readonly Segment[]) {
    this.allocations = parseAllocations(capture.stderr);
    this.byStart = [...this.allocations].sort((a, b) => (a.start !== b.start ? (a.start < b.start ? -1 : 1) : a.index - b.index));
    let reach = 0n;
    this.reach = this.byStart.map(allocation => (reach = allocation.end > reach ? allocation.end : reach));
    let ordinal = 0;
    for (const allocation of this.allocations) {
      if (allocation.dump) {
        this.bodyOrdinals.set(allocation.index, ++ordinal);
        continue;
      }
      // An allocation whose header starts inside an earlier baseline body's code is an inline rewrite of it: nothing dies
      // during the run, so no other allocation can start there.
      const owner = this.containing(allocation.start, allocation.index, candidate => candidate.dump !== null && allocation.start < candidate.codeEnd);
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

  // The allocation finalized last before limit whose range holds the value, among those accept takes.
  private containing(value: bigint, limit: number, accept: (allocation: Allocation) => boolean = () => true): Allocation | null {
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

  // Section 11.3's names, in the order of its table. A value under 2^32 that no row names stays literal, and so does one
  // that is negative at its width or from 2^48 up.
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

  private render(instructions: CanonicalInstruction[], namers: CodeNamer[], limit: number, assign: boolean): RenderedInstruction[] {
    const name: ValueNamer = (value, width, role) => this.name(value, width, role, namers, limit, assign);
    return instructions.map(instruction => renderInstruction(this.architecture, instruction, name));
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

  // Section 11.3, Targets: x86_64's dump runs before the link tasks that write calls and jumps to thunks, so every
  // call, jmp and conditional jump with a 32-bit displacement takes its displacement from the body's JIT dump record.
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
  // block and the ordinal of the canonical instruction there (section 11.3, Names).
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
      return { key: block.key, opcode: block.opcode, exempt, instructions: this.render(blocks[b], [namer], allocation.index + 1, !exempt) };
    });
    return { kind: "body", allocation, ordinal: this.bodyOrdinals.get(allocation.index)!, place: "", blocks: rendered, namer, blockAt };
  }

  private listing(allocation: Allocation): Instruction[] {
    if (!allocation.disassembly) throw new Unreadable(`line ${allocation.line}: ${allocation.identity} printed no disassembly`);
    const instructions = this.instructionsOf(allocation.disassembly, 0, allocation);
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
      const body = this.containing(target, allocation.index, candidate => candidate.dump !== null && target < candidate.codeEnd);
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
    const block: UnitBlock = { key: kind, opcode: null, exempt: false, instructions: this.render(blocks[0], [namer, owner.namer], allocation.index + 1, true) };
    return { kind, allocation, ordinal: owner.ordinal, place, blocks: [block], namer, blockAt };
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// The comparison (section 11.4)

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
  // Item 1: with JITCache off, this build behaves as the pin.
  if (current.capture.status.code !== pin.capture.status.code || current.capture.status.signal !== pin.capture.status.signal)
    return result("pin-status", `this build's run ${describeStatus(current.capture.status)}, the pin's ${describeStatus(pin.capture.status)}`);
  const output = outputDifference(current.capture.stdout, pin.capture.stdout);
  if (output) return result("pin-output", `the outputs differ at ${output}`);

  const contexts: RunContext[] = [];
  const literalSegments = [...current.engine.segments, ...pin.engine.segments];
  for (const [side, label] of [[current, "this build's run"], [pin, "the pin's run"]] as const) {
    try {
      contexts.push(new RunContext(architecture, side.capture, side.engine, literalSegments));
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
    if (!a || !b || a.identity !== b.identity)
      return result("pin-allocations", `allocation ${i}: this build finalized ${a ? a.identity : "nothing more"}, the pin ${b ? b.identity : "nothing more"}`, i, bodies);
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

// ---------------------------------------------------------------------------------------------------------------------
// Standalone use

export function architectureOf(buildDirectory: string): Architecture {
  const named = /(?:^|\/)linux-(x86_64|aarch64)-/.exec(resolve(buildDirectory))?.[1];
  return (named ?? (process.arch === "arm64" ? "aarch64" : "x86_64")) as Architecture;
}

export function readCapture(prefix: string): RunCapture {
  return {
    status: JSON.parse(readFileSync(`${prefix}.status`, "utf8")) as RunStatus,
    stdout: readFileSync(`${prefix}.stdout`),
    stderr: readFileSync(`${prefix}.stderr`, "utf8"),
    jitDump: readJITDump(prefix),
  };
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
      console.error(`jitcache-pin-compare: unknown option ${argument}`);
      return 2;
    }
  }
  if (!current || !pin || prefixes.length !== 1) {
    console.error("Usage: jitcache-pin-compare.ts --this=<this build's WebKit directory> --pin=<the pin's WebKit directory> <scratch>/pin<j>");
    return 2;
  }
  const architecture = architectureOf(current);
  if (architectureOf(pin) !== architecture) {
    console.error(`jitcache-pin-compare: ${current} builds ${architecture} and ${pin} builds ${architectureOf(pin)}`);
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
