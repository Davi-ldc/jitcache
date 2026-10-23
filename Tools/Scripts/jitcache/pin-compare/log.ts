// A run's standard error read into its allocations in finalization order: the logJIT header of every finalized
// allocation, the per-bytecode dump of each baseline compilation, which JIT::link prints before its allocation's header,
// and the disassembly that follows a header ending with ':', as a MathIC snippet's and an inline rewrite's do.
//
// A header ends at the first line that ends in its range, and a baseline dump's header at the first that ends in its
// instructions size, since a name may span lines: AccessCase::dump starts each of an access stub's cases on a new line,
// and a CodeBlock's name starts with its function's, which may hold newlines. Yarr's own dump header, a `Generated JIT
// code for` line ended by `:` and then `    Code at [s, e):`, opens a listing whose allocation's header comes after it.
//
// An allocation's identity across runs is its name without every address the name prints, not only its hexadecimal
// numbers (identityName), and its ordinal among the run's allocations of that name.

import { hex } from "./numbers.ts";

// What the comparison cannot read in a run's output, which fails the comparison as pin-unreadable.
export class Unreadable extends Error {}

export interface RawLine {
  address: bigint;
  offset: number | null; // ARM64's <offset> from the start of the dumped code
  text: string; // the instruction without its address, annotation and comment
  line: number;
}

export interface DumpBlock {
  key: string; // prologue, main:<bytecode index>, slow:<bytecode index> or tail
  opcode: string | null;
  lines: RawLine[];
}

export interface Allocation {
  index: number;
  name: string;
  identity: string; // the name without the addresses it prints (identityName), and its ordinal among that name's
  start: bigint;
  end: bigint; // the header's range, [start, end)
  codeEnd: bigint; // the end of its code: a body's dump's, an inline rewrite's listing's, or else the header's
  line: number;
  disassembly: RawLine[] | null; // what followed a header that ends with ':'
  dump: DumpBlock[] | null; // a baseline body's per-bytecode dump, which JIT::link prints before the header
}

// A header pattern matches the header's lines joined by newlines, and its end pattern the last of them.
const headerPrefix = "Generated JIT code for ";
const headerPattern = /^Generated JIT code for (.*): \[(0x[0-9a-f]+), (0x[0-9a-f]+)\) (\d+) bytes([.:])$/s;
const headerEndPattern = /: \[0x[0-9a-f]+, 0x[0-9a-f]+\) \d+ bytes[.:]$/;
const dumpPrefix = "Generated Baseline JIT code for ";
const dumpPattern = /^Generated Baseline JIT code for (.*), instructions size = \d+$/s;
const dumpEndPattern = /, instructions size = \d+$/;
// The line after the name in JITDisassembler::dumpHeader and in YarrDisassembler::dumpHeader.
const sourcePrefix = "   Source: ";
const listingCodeAtPattern = /^ {4}Code at \[0x[0-9a-f]+, 0x[0-9a-f]+\):$/;
const codeAtPattern = /^ {3}Code at \[(0x[0-9a-f]+), (0x[0-9a-f]+)\):$/;
const instructionPattern = /^\s+(?:<(\d+)>\s+)?(0x[0-9a-f]+): ?(.*)$/;
const bytecodePattern = /^ {4}(\(S\) )?\[\s*(\d+)\] (\S+)/;

export const clip = (text: string) => JSON.stringify(text.length > 160 ? `${text.slice(0, 160)}…` : text);

// An allocation's name without the addresses it prints, which with its ordinal identifies it across runs.
// Besides hexadecimal numbers, the engine prints an address as a Structure's StringHashDumpContext id (%AQ:), a hash of
// Structure::dump, which prints the Structure's address, and as SlowPathCallKey::dump's indirectOffset, which reads the
// low half of a direct call's operation address through a union and prints it in decimal. Ids go first, since an id
// such as %B0xa: holds a hexadecimal number.
function identityName(name: string): string {
  return name
    .replace(/%[A-Za-z0-9]{2,5}:/g, "%?:")
    .replace(/indirectOffset = -?\d+/g, "indirectOffset = ?")
    .replace(/0x[0-9a-fA-F]+/g, "0x?");
}

const startsHeader = (text: string) => text.startsWith(headerPrefix) || text.startsWith(dumpPrefix);

// The line that ends the header at line n: the first that end matches. The search stops before the next header and
// before a line that stop matches, and the line it then returns does not match end.
function headerEnd(lines: readonly string[], n: number, end: RegExp, stop: (text: string) => boolean): number {
  let last = n;
  while (!end.test(lines[last]) && last + 1 < lines.length && !startsHeader(lines[last + 1]) && !stop(lines[last + 1])) ++last;
  return last;
}

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

export function parseAllocations(stderr: string): Allocation[] {
  const allocations: Allocation[] = [];
  const ordinals = new Map<string, number>();
  let dump: PendingDump | null = null;
  let listing: RawLine[] | null = null;
  const lines = stderr.split("\n");
  for (let n = 0; n < lines.length; ++n) {
    const text = lines[n];
    const line = n + 1;
    if (text.startsWith(headerPrefix)) {
      listing = null;
      const last = headerEnd(lines, n, headerEndPattern, next => listingCodeAtPattern.test(next));
      // A `Code at` line before the range closes Yarr's own dump header instead, whose listing the tool does not read.
      if (!headerEndPattern.test(lines[last]) && lines[last].endsWith(":") && listingCodeAtPattern.test(lines[last + 1] ?? "")) {
        n = last + 1;
        continue;
      }
      const header = lines.slice(n, last + 1).join("\n");
      const match = headerPattern.exec(header);
      if (!match) throw new Unreadable(`line ${line}: an allocation header it cannot parse: ${clip(header)}`);
      n = last;
      const start = BigInt(match[2]);
      const end = BigInt(match[3]);
      if (end - start !== BigInt(match[4])) throw new Unreadable(`line ${line}: an allocation header whose range and size disagree: ${clip(header)}`);
      const name = identityName(match[1]);
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
    if (text.startsWith(dumpPrefix)) {
      listing = null;
      if (dump) throw new Unreadable(`line ${dump.line}: the baseline dump of ${dump.codeBlock} has no allocation header`);
      const last = headerEnd(lines, n, dumpEndPattern, next => next.startsWith(sourcePrefix));
      const header = lines.slice(n, last + 1).join("\n");
      const dumpMatch = dumpPattern.exec(header);
      if (!dumpMatch) throw new Unreadable(`line ${line}: a baseline dump header it cannot parse: ${clip(header)}`);
      n = last;
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
