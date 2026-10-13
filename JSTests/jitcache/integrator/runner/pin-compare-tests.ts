#!/usr/bin/env bun
// Harness sub-SPEC H7: jitcache-pin-compare.ts's own tests, on crafted output pairs for both architectures. Each case
// writes two runs as the engine prints them (logJIT headers, baseline dumps, listings, JIT dump records) and checks that
// compareRuns reports pairs that differ in an operation's symbol, an allocation's identity, an h number, an immediate, a
// displacement, a register, a mnemonic, the order of finalized allocations or a breakpoint inside a listing's code, and
// finds the same code in pairs that differ only in the addresses of the same objects, in nops, in the breakpoints that
// pad a listing, in blinded forms, in ARM64 materialization lengths and temporary reuse, in the two forms of an ARM64
// conditional branch to a thunk, or in a super_construct main path. Other
// cases cover what the engine prints around the code: a header the JIT heap rounds up past the dump, the padding after a
// body's last instruction, a MathIC inline rewrite, whose header prints an empty range, names that span lines, Yarr's
// own dump header before its allocation's, names that print addresses as Structure ids and in decimal, and a header that
// never reaches its range.
//
//   bun JSTests/jitcache/integrator/runner/pin-compare-tests.ts
// self-test.ts runs them too, as its pin-compare-crafted case.

import {
  type Architecture,
  type EngineImage,
  type JITDumpRecord,
  type PinCheck,
  type PinSide,
  type RunStatus,
  compareRuns,
  makeEngineImage,
} from "../../../../Tools/Scripts/jitcache-pin-compare.ts";

// ---------------------------------------------------------------------------------------------------------------------
// Crafting a run

// One instruction. Its text may name {@label}, a line's address; {=value}, a value of the run's layout, with an optional
// +0x offset; and {next}, the address after it. rel32 names the target an x86_64 call or jump's JIT dump bytes hold.
interface Line {
  text: string;
  length?: number; // x86_64 only; ARM64 instructions take 4 bytes
  label?: string;
  rel32?: string; // "@label" or "=value"
}

interface Block {
  header: string; // "prologue", a bytecode line, or a marker such as "    (End Of Main Path)"
  lines: Line[];
}

// rewrite: a MathIC inline rewrite, whose header prints [p, p) 0 bytes and which writes no JIT dump record. preamble:
// the name Yarr's own dump header gives the listing, which it prints with its `Code at` line before the allocation's
// header, which then ends in '.'. pad: the breakpoint bytes past a body's last instruction that its `Code at` range holds
// and its dump does not print. round: the granule its header's range rounds up to, as the JIT heap rounds an allocation.
// A name may hold newlines, as an access stub's does.
type Item =
  | { kind: "listing"; name: string; at: string; lines: Line[]; rewrite?: boolean; preamble?: string }
  | { kind: "body"; codeBlock: string; at: string; blocks: Block[]; pad?: number; round?: number };

interface Run {
  architecture: Architecture;
  items: Item[];
  values: Record<string, bigint>;
  engine: EngineImage;
  status?: RunStatus;
  stdout?: string;
  dropRecords?: boolean; // a JIT dump without the bodies' records
  preface?: string[]; // lines the run prints before its first allocation
}

const hex = (value: bigint) => (value < 0n ? `-0x${(-value).toString(16)}` : `0x${value.toString(16)}`);

function craft(run: Run): PinSide {
  const { architecture, values } = run;
  const labels = new Map<string, bigint>();
  const lookup = (key: string): bigint => {
    const value = key.startsWith("@") ? labels.get(key.slice(1)) : values[key.slice(1)];
    if (value === undefined) throw new Error(`the crafted run names no ${key}`);
    return value;
  };
  interface Placed {
    item: Item;
    start: bigint;
    end: bigint;
    lines: { line: Line; address: bigint; length: number; block: number }[];
  }
  const placed: Placed[] = [];
  for (const item of run.items) {
    const start = lookup(item.at.startsWith("@") ? item.at : `=${item.at}`);
    let address = start;
    const lines: Placed["lines"] = [];
    const add = (line: Line, block: number) => {
      const length = architecture === "aarch64" ? 4 : line.length ?? 1;
      if (line.label) labels.set(line.label, address);
      lines.push({ line, address, length, block });
      address += BigInt(length);
    };
    if (item.kind === "listing") item.lines.forEach(line => add(line, 0));
    else item.blocks.forEach((block, b) => block.lines.forEach(line => add(line, b)));
    placed.push({ item, start, end: address, lines });
  }
  const substitute = (text: string, address = 0n, length = 0) =>
    text.replace(/\{(next|[@=][\w]+)(?:\+(0x[0-9a-f]+))?\}/g, (_, key: string, offset?: string) =>
      hex((key === "next" ? address + BigInt(length) : lookup(key)) + (offset ? BigInt(offset) : 0n)));
  const instructionLine = (prefix: string, start: bigint, address: bigint, text: string) =>
    architecture === "x86_64"
      ? `${prefix}${hex(address).padStart(16)}: ${text}`
      : `${prefix}${`<${address - start}> ${hex(address)}`.padStart(24)}:    ${text}`;

  const stderr: string[] = [...(run.preface ?? [])];
  const records: JITDumpRecord[] = [];
  for (const { item, start, end, lines } of placed) {
    const codeEnd = item.kind === "body" ? end + BigInt(item.pad ?? 0) : end;
    const round = BigInt(item.kind === "body" ? item.round ?? 1 : 1);
    const headerEnd = item.kind === "listing" && item.rewrite ? start : start + ((codeEnd - start + round - 1n) / round) * round;
    const size = headerEnd - start;
    const range = `[${hex(start)}, ${hex(headerEnd)}) ${size} bytes`;
    let name: string;
    if (item.kind === "body") {
      const codeBlock = substitute(item.codeBlock);
      name = `Baseline JIT code for ${codeBlock}`;
      stderr.push(`Generated Baseline JIT code for ${codeBlock}, instructions size = 42`, "   Source: function f() { }", `   Code at [${hex(start)}, ${hex(codeEnd)}):`);
      item.blocks.forEach((block, b) => {
        if (block.header !== "prologue") stderr.push(block.header);
        for (const placedLine of lines.filter(entry => entry.block === b))
          stderr.push(instructionLine("        ", start, placedLine.address, substitute(placedLine.line.text, placedLine.address, placedLine.length)));
      });
      stderr.push(`Generated JIT code for ${name}: ${range}.`);
    } else if (item.preamble !== undefined) {
      name = substitute(item.name);
      stderr.push(`Generated JIT code for ${substitute(item.preamble)}:`, `    Code at [${hex(start)}, ${hex(end)}):`);
      for (const placedLine of lines) stderr.push(instructionLine("      ", start, placedLine.address, substitute(placedLine.line.text, placedLine.address, placedLine.length)));
      stderr.push(`Generated JIT code for ${name}: ${range}.`);
    } else {
      name = substitute(item.name);
      stderr.push(`Generated JIT code for ${name}: ${range}:`);
      for (const placedLine of lines) stderr.push(instructionLine("    ", start, placedLine.address, substitute(placedLine.line.text, placedLine.address, placedLine.length)));
    }
    if ((item.kind === "listing" && item.rewrite) || (item.kind === "body" && run.dropRecords)) continue;
    // The record holds the header's range: the code, the int3 padding up to `Code at`'s end, and what lies past it.
    const bytes = new Uint8Array(Number(size));
    bytes.fill(0x90, 0, Number(end - start));
    bytes.fill(0xcc, Number(end - start), Number(codeEnd - start));
    for (const { line, address, length } of lines) {
      if (!line.rel32) continue;
      const offset = Number(address - start);
      const mnemonic = line.text.split(" ")[0];
      if (mnemonic === "call") bytes[offset] = 0xe8;
      else if (mnemonic === "jmp") bytes[offset] = 0xe9;
      else bytes.set([0x0f, 0x84], offset);
      const displacement = lookup(line.rel32) - (address + BigInt(length));
      new DataView(bytes.buffer).setInt32(offset + length - 4, Number(displacement), true);
    }
    records.push({ address: start, size: Number(size), name, bytes });
  }
  return {
    capture: { status: run.status ?? { code: 0, signal: null }, stdout: new TextEncoder().encode(run.stdout ?? ""), stderr: `${stderr.join("\n")}\n`, jitDump: records },
    engine: run.engine,
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// Two processes of two builds: the same objects at other addresses, and the same operations at other symbol addresses.

const segments = [{ start: 0x200000n, end: 0x9000000n }];
const engineThis = makeEngineImage(segments, [
  { address: 0x2c00010n, name: "JSC::operationValueAdd" },
  { address: 0x2c00100n, name: "JSC::operationFoo" },
  { address: 0x2c00200n, name: "JSC::operationBar" },
]);
const enginePin = makeEngineImage(segments, [
  { address: 0x2d40030n, name: "JSC::operationValueAdd" },
  { address: 0x2d40150n, name: "JSC::operationFoo" },
  { address: 0x2d40270n, name: "JSC::operationBar" },
]);

const layoutThis: Record<string, bigint> = {
  thunkA: 0x7f3a10000000n,
  thunkB: 0x7f3a10000100n,
  body: 0x7f3a10001000n,
  snippet: 0x7f3a10003000n,
  stub: 0x7f3a10005000n,
  exit: 0x7f3a10005400n,
  slowPathThunk: 0x7f3a10005800n,
  regexp: 0x7f3a10006000n,
  matchOnlyRegexp: 0x7f3a10006400n,
  codeBlock: 0x7f3aa0000000n,
  executable: 0x7f3aa0000200n,
  vm: 0x7f3ab0000000n,
  heapA: 0x7f3ac0001000n,
  heapB: 0x7f3ac0002000n,
  add: 0x2c00010n,
  foo: 0x2c00100n,
  bar: 0x2c00200n,
};
const layoutPin: Record<string, bigint> = {
  thunkA: 0x7f9150000000n,
  thunkB: 0x7f9150000200n,
  body: 0x7f9150004000n,
  snippet: 0x7f9150008000n,
  stub: 0x7f915000c000n,
  exit: 0x7f915000c800n,
  slowPathThunk: 0x7f915000d000n,
  regexp: 0x7f915000e000n,
  matchOnlyRegexp: 0x7f915000e800n,
  codeBlock: 0x7f91e0000000n,
  executable: 0x7f91e0000300n,
  vm: 0x7f91f0000000n,
  heapA: 0x7f9200007000n,
  heapB: 0x7f9200009000n,
  add: 0x2d40030n,
  foo: 0x2d40150n,
  bar: 0x2d40270n,
};

const line = (text: string, length: number, extra: Partial<Line> = {}): Line => ({ text, length, ...extra });

interface X86Shape {
  main: Line[]; // block [2]
  codeBlock?: string; // the body's CodeBlock, f#AbCdEf by default
  entryNop?: boolean;
  thunks?: "AB" | "BA";
  extraBlocks?: Block[]; // after block [2]
  afterBody?: Item[];
  overflow?: "=thunkA" | "=thunkB"; // the tail's near jmp, thunkB's by default
  pad?: number;
  round?: number;
}

function x86Items(shape: X86Shape): Item[] {
  const thunkA: Item = { kind: "listing", name: "thunk: Baseline: op_add slow path", at: "thunkA", lines: [line("push %rbp", 1), line("ret", 1)] };
  const thunkB: Item = { kind: "listing", name: "thunk: ThrowStackOverflowAtPrologue", at: "thunkB", lines: [line("ud2", 2)] };
  const body: Item = {
    kind: "body",
    codeBlock: shape.codeBlock ?? "f#AbCdEf:[{=codeBlock}->{=executable}, BaselineFunctionCall, 12]",
    at: "body",
    blocks: [
      {
        header: "prologue",
        lines: [
          ...(shape.entryNop ? [line("nop", 1)] : []),
          line("push %rbp", 1),
          line("mov %rsp, %rbp", 3),
          line("mov ${=vm+0x40}, %r11", 10),
          line("cmpq (%r11), %rsp", 3),
          line("ja {@overflow}", 6, { rel32: "@overflow" }),
        ],
      },
      { header: "    [   0] enter              ", lines: [line("movq %r14, -0x30(%rbp)", 4)] },
      { header: "    [   2] add                loc5, arg1, Int32: 100000(const0)", lines: shape.main },
      ...(shape.extraBlocks ?? []),
      {
        header: "    [   7] ret                loc5",
        lines: [
          line("mov ${=add}, %r11", 10, { label: "ret7" }),
          line("call *%r11", 3),
          line("call {next}", 5, { rel32: "=thunkA" }),
          line("ret", 1),
        ],
      },
      { header: "    (End Of Main Path)", lines: [] },
      { header: "    (S) [   2] add                loc5, arg1, Int32: 100000(const0)", lines: [line("mov ${=add}, %r11", 10, { label: "slow2" }), line("call *%r11", 3), line("jmp {@ret7}", 5, { rel32: "@ret7" })] },
      { header: "    (End Of Slow Path)", lines: [line("jmp {next}", 5, { label: "overflow", rel32: shape.overflow ?? "=thunkB" })] },
    ],
    pad: shape.pad,
    round: shape.round,
  };
  return [...(shape.thunks === "BA" ? [thunkB, thunkA] : [thunkA, thunkB]), body, ...(shape.afterBody ?? [])];
}

function x86Run(shape: X86Shape, side: "this" | "pin", extra: Partial<Run> = {}): Run {
  return { architecture: "x86_64", items: x86Items(shape), values: side === "this" ? layoutThis : layoutPin, engine: side === "this" ? engineThis : enginePin, ...extra };
}

const rotateLeft = (value: bigint, amount: number) => ((value << BigInt(amount)) | (value >> BigInt(64 - amount))) & ((1n << 64n) - 1n);

// The address bits a materialization writes, shortest first, as MacroAssemblerARM64::moveInternal emits them.
function materialize(register: string, value: bigint): Line[] {
  const lines: Line[] = [];
  for (let i = 0; i < 4; ++i) {
    const half = (value >> BigInt(16 * i)) & 0xffffn;
    if (!half) continue;
    lines.push({ text: lines.length ? `movk\t${register}, #${hex(half)}, lsl #${hex(BigInt(16 * i))}` : `mov\t${register}, #${hex(half << BigInt(16 * i))}` });
  }
  return lines;
}

const arm64Layouts = {
  this: { ...layoutThis, heapA: 0xffff80001000n },
  pin: { ...layoutPin, heapA: 0xffff00002000n }, // one halfword fewer to materialize
};

function arm64Items(main: Line[], layout: { pad?: number; round?: number }, afterBody: Item[]): Item[] {
  return [
    { kind: "listing", name: "thunk: Baseline: op_add slow path", at: "thunkA", lines: [{ text: "ret" }] },
    { kind: "listing", name: "thunk: ThrowStackOverflowAtPrologue", at: "thunkB", lines: [{ text: "brk\t#0x1" }] },
    {
      kind: "body",
      codeBlock: "f#AbCdEf:[{=codeBlock}->{=executable}, BaselineFunctionCall, 12]",
      at: "body",
      blocks: [
        { header: "prologue", lines: [{ text: "stp\tx29, x30, [sp, #-0x10]!" }, { text: "mov\tx29, sp" }] },
        { header: "    [   0] enter              ", lines: [{ text: "stur\tx0, [x29, #-0x30]" }] },
        { header: "    [   2] add                loc5, arg1, Int32: 100000(const0)", lines: main },
        { header: "    [   7] ret                loc5", lines: [{ text: "ldp\tx29, x30, [sp], #0x10", label: "ret7" }, { text: "ret" }] },
        { header: "    (End Of Main Path)", lines: [] },
        { header: "    (S) [   2] add                loc5, arg1, Int32: 100000(const0)", lines: [{ text: "bl\t{=thunkA}", label: "slow2" }, { text: "b\t{@ret7}" }] },
        { header: "    (End Of Slow Path)", lines: [{ text: "b\t{=thunkB}", label: "overflow" }] },
      ],
      ...layout,
    },
    ...afterBody,
  ];
}

function arm64Run(main: Line[], side: "this" | "pin", layout: { pad?: number; round?: number } = {}, afterBody: Item[] = []): Run {
  return { architecture: "aarch64", items: arm64Items(main, layout, afterBody), values: arm64Layouts[side], engine: side === "this" ? engineThis : enginePin };
}

// ---------------------------------------------------------------------------------------------------------------------
// The cases

interface Case {
  name: string;
  expect: "same" | PinCheck;
  current: Run;
  pin: Run;
}

const pinHeapA = layoutPin.heapA;
const pair = (name: string, expect: Case["expect"], current: X86Shape, pin: X86Shape, extraThis: Partial<Run> = {}, extraPin: Partial<Run> = {}): Case =>
  ({ name, expect, current: x86Run(current, "this", extraThis), pin: x86Run(pin, "pin", extraPin) });
const same = (lines: Line[]): X86Shape => ({ main: lines });

// A MathIC snippet, and the inline rewrite that overwrites the start of its inline region with a jump to it, whose
// header prints [p, p) 0 bytes.
const snippetAndRewrite = (rewriteTarget = "=snippet"): Item[] => [
  { kind: "listing", name: "JITMathIC: generating out of line IC snippet", at: "snippet", lines: [line("add $0x1, %eax", 3), line("jmp {@ret7}", 5)] },
  { kind: "listing", name: "JITMathIC: linking constant jump to out of line stub", at: "@inline2", lines: [line(`jmp {${rewriteTarget}}`, 5)], rewrite: true },
];
// The inline region: a jump to the slow path, as an empty profile leaves it, whose JIT dump record holds the bytes
// before the rewrite or after it.
const inlineRegion = (rel32: string): Line[] => [line("jmp {@slow2}", 5, { label: "inline2", rel32 }), line("movq %rax, -0x38(%rbp)", 4)];

// A MathIC snippet's listing prints its whole LinkBuffer: the code, then the breakpoints LinkBuffer::allocate pads it
// with up to the 32-byte allocation granule, as many as the code's length leaves room for. Its last jump returns into
// the body, which makes it the body's snippet, so its code is compared.
const granule = 32;
const breakpoint = (architecture: Architecture): Line => (architecture === "x86_64" ? line("int3", 1) : { text: "brk\t#0" });
function paddedSnippet(architecture: Architecture, code: Line[]): Item {
  const size = code.reduce((total, entry) => total + (architecture === "x86_64" ? entry.length ?? 1 : 4), 0);
  const padding = ((granule - (size % granule)) % granule) / (architecture === "x86_64" ? 1 : 4);
  return { kind: "listing", name: "JITMathIC: generating out of line IC snippet", at: "snippet", lines: [...code, ...Array.from({ length: padding }, () => breakpoint(architecture))] };
}

// Allocations whose names the engine prints on several lines or with addresses that are not hexadecimal.
// InlineCacheCompiler names a stub "Access stub handler for <CB> <origin>: " and its cases, and AccessCase::dump starts
// each case on a new line.
const accessStub = (offset = 0): Item => ({
  kind: "listing",
  name: `Access stub handler for f#AbCdEf:[{=codeBlock}->{=executable}, BaselineFunctionCall, 12] bc#2: \nLoad: {ident = 'x', offset = ${offset}\n  structure = {=heapA}:[0x1c0/448, Object, (1/6, 0/0){x:${offset}}, NonArray, PropertyAddition, Proto:{=heapB}, Leaf]}`,
  at: "stub",
  lines: [line("push %rbp", 1), line("ret", 1)],
});
// YarrDisassembler::dumpHeader prints the pattern as written, so a pattern made with new RegExp can hold a newline.
const regexps: Item[] = [
  { kind: "listing", name: "8-bit regular expression", preamble: "8-bit regular expression /a(b+)c/g", at: "regexp", lines: [line("push %rbp", 1), line("ret", 1)] },
  { kind: "listing", name: "Match-only 8-bit regular expression", preamble: "Match-only 8-bit regular expression /a\nb/", at: "matchOnlyRegexp", lines: [line("ret", 1)] },
];
// A DFG OSR exit names each operand's Structure by its StringHashDumpContext id, a hash of a dump that prints the
// Structure's address; SlowPathCallKey::dump prints indirectOffset, which for a direct call is the low half of the
// operation's address, in decimal.
const exitAndSlowPathThunk = (id: string, layout: Record<string, bigint>, className = "global"): Item[] => [
  { kind: "listing", name: `DFG OSR exit #2 (D@25, bc#7, BadCache) from f#AbCdEf:[{=codeBlock}->{=executable}, DFGFunctionCall, 12], with operands = arg0:*this loc5:[Object: {=heapA} with butterfly (nil) (Structure %${id}:${className}), StructureID: 448]`, at: "exit", lines: [line("ret", 1)] },
  { kind: "listing", name: `thunk: FTL slow path call thunk for <usedRegisters = [], offset = 8, indirectOffset = ${layout.foo & 0xffffffffn}, callTarget = {=foo}>`, at: "slowPathThunk", lines: [line("ret", 1)] },
];
// A CodeBlock's name starts with its function's, which a string or computed key can give a newline.
const multiLineCodeBlock = "a\nb#AbCdEf:[{=codeBlock}->{=executable}, BaselineFunctionCall, 12]";

const cases: Case[] = [
  // Pairs that compare equal.
  pair("addresses of the same objects", "same",
    same([line("mov ${=heapA}, %rdx", 10), line("movq %rax, 0x10(%rdx)", 4), line("mov ${=foo}, %r11", 10), line("call *%r11", 3)]),
    same([line("mov ${=heapA}, %rdx", 10), line("movq %rax, 0x10(%rdx)", 4), line("mov ${=foo}, %r11", 10), line("call *%r11", 3)])),
  pair("nops", "same",
    same([line("add $0x1, %eax", 3), line("movq %rax, -0x38(%rbp)", 4)]),
    { entryNop: true, main: [line("nop", 1), line("add $0x1, %eax", 3), line("nopw 0x0(%rax,%rax,1)", 6), line("movq %rax, -0x38(%rbp)", 4)] }),
  pair("a blinded move of an int32", "same",
    same([line("mov $0x12345, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)]),
    same([line("mov $0x1b345, %eax", 5), line("xor $0x9000, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)])),
  pair("a blinded move whose first half is zero", "same",
    same([line("mov $0x12345, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)]),
    same([line("xor %eax, %eax", 2), line("xor $0x12345, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)])),
  pair("a rotation-blinded pointer", "same",
    same([line("mov ${=heapA}, %rdx", 10), line("movq %rax, 0x10(%rdx)", 4)]),
    same([line(`mov $${hex(rotateLeft(pinHeapA, 8))}, %rdx`, 10), line("ror $0x8, %rdx", 4), line("movq %rax, 0x10(%rdx)", 4)])),
  pair("a store of a blinded 64-bit constant through r11", "same",
    same([line("movq $0x12345, -0x40(%rbp)", 8)]),
    same([line(`mov $${hex(rotateLeft(0x12345n, 4))}, %r11`, 10), line("ror $0x4, %r11", 4), line("movq %r11, -0x40(%rbp)", 4)])),
  // An integer inside the jsc segments names a symbol in one build and none in the other, and matches by its literal.
  pair("an integer inside the engines' segments", "same", same([line("add $0x2c00150, %eax", 5)]), same([line("add $0x2c00150, %eax", 5)])),
  pair("a split addition", "same", same([line("add $0x1000, %eax", 5)]), same([line("add $0xa00, %eax", 5), line("add $0x600, %eax", 5)])),
  pair("a split and", "same", same([line("and $0x7ff, %ecx", 6)]), same([line("and $0xfff, %ecx", 6), line("and $0x17ff, %ecx", 6)])),
  pair("a split lea", "same", same([line("lea 0x1000(%rsi), %edx", 6)]), same([line("lea 0xa00(%rsi), %edx", 6), line("add $0x600, %edx", 6)])),
  pair("a comparison through r11", "same",
    same([line("cmp $0x186a0, %eax", 5), line("jz {@ret7}", 6, { rel32: "@ret7" })]),
    same([line("mov $0x1f6a0, %r11d", 6), line("xor $0x7000, %r11d", 7), line("cmp %r11d, %eax", 3), line("jz {@ret7}", 6, { rel32: "@ret7" })])),
  pair("branchAdd32's constant built in its destination", "same",
    same([line("mov %esi, %edx", 2), line("add $0x2710, %edx", 6), line("jo {@slow2}", 6, { rel32: "@slow2" })]),
    same([line("mov $0x3710, %edx", 5), line("xor $0x1000, %edx", 6), line("add %esi, %edx", 2), line("jo {@slow2}", 6, { rel32: "@slow2" })])),
  pair("branchMul32's constant built in its destination", "same",
    same([line("imul $0x2710, %esi, %edx", 6), line("jo {@slow2}", 6, { rel32: "@slow2" })]),
    same([line("mov $0x3710, %edx", 5), line("xor $0x1000, %edx", 6), line("imul %esi, %edx", 3), line("jo {@slow2}", 6, { rel32: "@slow2" })])),
  pair("a super_construct main path", "same",
    { main: [line("add $0x1, %eax", 3)], extraBlocks: [{ header: "    [   4] super_construct    loc6, loc7, 1, 16", lines: [line("mov %rax, %rcx", 3)] }] },
    { main: [line("add $0x1, %eax", 3)], extraBlocks: [{ header: "    [   4] super_construct    loc6, loc7, 1, 16", lines: [line("mov %rax, %rcx", 3), line("test %rcx, %rcx", 3)] }] }),
  // The dump's `Code at` ends at the int3 padding of the allocation granule, the header past it, where the JIT heap
  // rounds the allocation up; the tail's near jmp takes its length from its encoding.
  pair("a header larger than the dump, after a final near jmp and int3 padding", "same",
    { main: [line("add $0x1, %eax", 3)], pad: 24, round: 256 },
    { main: [line("add $0x1, %eax", 3)], pad: 24, round: 256 }),
  {
    // The JIT dump copies a body's bytes on a queue of its own: in one run before the inline rewrite, in the other after.
    name: "a JIT dump copied after an inline rewrite",
    expect: "same",
    current: x86Run({ main: inlineRegion("@slow2"), afterBody: snippetAndRewrite() }, "this"),
    pin: x86Run({ main: inlineRegion("=snippet"), afterBody: snippetAndRewrite() }, "pin"),
  },
  // The blinded pointer is 4 bytes longer, so its listing holds 4 fewer padding breakpoints.
  pair("a snippet whose blinded form leaves other padding", "same",
    { main: [], afterBody: [paddedSnippet("x86_64", [line("mov ${=heapA}, %r11", 10), line("orw $0x1b, (%r11)", 5), line("jmp {@ret7}", 5)])] },
    { main: [], afterBody: [paddedSnippet("x86_64", [line(`mov $${hex(rotateLeft(pinHeapA, 8))}, %r11`, 10), line("ror $0x8, %r11", 4), line("orw $0x1b, (%r11)", 5), line("jmp {@ret7}", 5)])] }),
  pair("an allocation header whose name spans lines", "same", { main: [], afterBody: [accessStub()] }, { main: [], afterBody: [accessStub()] }),
  pair("Yarr's own dump header before its allocation's", "same", { main: [], afterBody: regexps }, { main: [], afterBody: regexps }),
  // An id is 2 to 5 characters of [A-Za-z0-9], the first from A to E, so it can hold a hexadecimal number.
  pair("names that print addresses as Structure ids and in decimal", "same",
    { main: [], afterBody: exitAndSlowPathThunk("B0xa", layoutThis) },
    { main: [], afterBody: exitAndSlowPathThunk("Bl", layoutPin) }),

  // Pairs that differ.
  pair("an operation's symbol", "pin-code",
    same([line("mov ${=foo}, %r11", 10), line("call *%r11", 3)]),
    same([line("mov ${=bar}, %r11", 10), line("call *%r11", 3)])),
  pair("an allocation's identity", "pin-code",
    same([line("call {next}", 5, { rel32: "=thunkA" })]),
    same([line("call {next}", 5, { rel32: "=thunkB" })])),
  pair("an h number", "pin-code",
    same([line("mov ${=heapA}, %rdx", 10), line("mov ${=heapB}, %rcx", 10), line("mov ${=heapA}, %rsi", 10)]),
    same([line("mov ${=heapA}, %rdx", 10), line("mov ${=heapB}, %rcx", 10), line("mov ${=heapB}, %rsi", 10)])),
  {
    // h numbers match by name alone: the third operand names the first object in one run and the second in the other,
    // although both runs placed that object at one address.
    name: "an h number at an address both runs share",
    expect: "pin-code",
    current: x86Run(same([line("mov ${=heapA}, %rdx", 10), line("mov ${=heapB}, %rcx", 10), line("mov ${=heapA}, %rsi", 10)]), "this"),
    pin: x86Run(same([line("mov ${=heapB}, %rdx", 10), line("mov ${=heapA}, %rcx", 10), line("mov ${=heapA}, %rsi", 10)]), "this"),
  },
  pair("an immediate", "pin-code", same([line("add $0x1, %eax", 3)]), same([line("add $0x2, %eax", 3)])),
  pair("a displacement", "pin-code", same([line("movq %rax, 0x10(%rbx)", 4)]), same([line("movq %rax, 0x18(%rbx)", 4)])),
  pair("a register", "pin-code", same([line("movq -0x30(%rbp), %rax", 4)]), same([line("movq -0x30(%rbp), %rcx", 4)])),
  pair("a mnemonic", "pin-code", same([line("add $0x1, %eax", 3)]), same([line("sub $0x1, %eax", 3)])),
  pair("the order of finalized allocations", "pin-allocations", { main: [line("add $0x1, %eax", 3)] }, { main: [line("add $0x1, %eax", 3)], thunks: "BA" }),
  pair("a name that spans lines and holds another offset", "pin-allocations", { main: [], afterBody: [accessStub(0)] }, { main: [], afterBody: [accessStub(1)] }),
  pair("a name with a Structure of another class", "pin-allocations",
    { main: [], afterBody: exitAndSlowPathThunk("AQ", layoutThis) },
    { main: [], afterBody: exitAndSlowPathThunk("AQ", layoutPin, "Object") }),
  // Both of a body's headers span lines when its CodeBlock's name does, and its code is still compared.
  pair("a body whose name spans lines", "pin-code",
    { codeBlock: multiLineCodeBlock, main: [line("add $0x1, %eax", 3)] },
    { codeBlock: multiLineCodeBlock, main: [line("add $0x2, %eax", 3)] }),
  // A header ends at its range, so one that the next header interrupts is output the tool cannot read.
  pair("an allocation header that never reaches its range", "pin-unreadable", same([]), same([]), { preface: ["Generated JIT code for an allocation whose header ends early"] }),
  // The folding rules keep what the blinded forms compute: a form that computes another constant differs.
  pair("a blinded move of another int32", "pin-code",
    same([line("mov $0x12345, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)]),
    same([line("mov $0x1b345, %eax", 5), line("xor $0x9001, %eax", 5), line("movq %rax, -0x38(%rbp)", 4)])),
  pair("branchAdd32 with another constant", "pin-code",
    same([line("mov %esi, %edx", 2), line("add $0x2710, %edx", 6), line("jo {@slow2}", 6, { rel32: "@slow2" })]),
    same([line("mov $0x3711, %edx", 5), line("xor $0x1000, %edx", 6), line("add %esi, %edx", 2), line("jo {@slow2}", 6, { rel32: "@slow2" })])),
  // Rule 2 writes the set of a register other than a temporary before its reader, so a constant unfolded through one,
  // as only recording may emit it, differs from the folded immediate even when the run overwrites the register later.
  pair("a constant unfolded through a register overwritten later in its run", "pin-code",
    same([line("mov $0x2710, %edx", 5), line("add %edx, %eax", 2), line("mov %esi, %edx", 2)]),
    same([line("add $0x2710, %eax", 5), line("mov %esi, %edx", 2)])),
  // Only the breakpoints that end a listing pad it; one before its last other instruction is code.
  pair("a breakpoint inside a snippet's code", "pin-code",
    { main: [], afterBody: [paddedSnippet("x86_64", [line("add $0x1, %eax", 3), line("jmp {@ret7}", 5)])] },
    { main: [], afterBody: [paddedSnippet("x86_64", [line("add $0x1, %eax", 3), line("int3", 1), line("jmp {@ret7}", 5)])] }),
  pair("a final near jmp to another thunk, after int3 padding", "pin-code",
    { main: [line("add $0x1, %eax", 3)], pad: 24, round: 256 },
    { main: [line("add $0x1, %eax", 3)], pad: 24, round: 256, overflow: "=thunkA" }),
  {
    // An inline rewrite is compared instruction by instruction, not by its identity alone.
    name: "an inline rewrite to another place in its snippet",
    expect: "pin-code",
    current: x86Run({ main: inlineRegion("@slow2"), afterBody: snippetAndRewrite() }, "this"),
    pin: x86Run({ main: inlineRegion("@slow2"), afterBody: snippetAndRewrite("=snippet+0x3") }, "pin"),
  },
  {
    name: "ARM64 conditional branch to another thunk",
    expect: "pin-code",
    current: arm64Run([{ text: "cmp\tx0, #0x5" }, { text: "b.eq\t{=thunkA}" }, { text: "ldr\tx1, [x2]" }], "this"),
    pin: arm64Run([{ text: "cmp\tx0, #0x5" }, { text: "b.ne\t{@skip}" }, { text: "b\t{=thunkB}" }, { text: "ldr\tx1, [x2]", label: "skip" }], "pin"),
  },
  {
    // Without an inline rewrite that covers it, a branch's displacement is the JIT dump's.
    name: "a JIT dump branch no rewrite explains",
    expect: "pin-code",
    current: x86Run({ main: [line("jmp {@slow2}", 5, { rel32: "@slow2" }), line("movq %rax, -0x38(%rbp)", 4)] }, "this"),
    pin: x86Run({ main: [line("jmp {@slow2}", 5, { rel32: "@ret7" }), line("movq %rax, -0x38(%rbp)", 4)] }, "pin"),
  },
  {
    // Section 11.3: another field shows a different number at the next use of either field.
    name: "ARM64 reuse that reaches another field",
    expect: "pin-code",
    current: arm64Run([...materialize("x17", arm64Layouts.this.heapA), { text: "ldr\tx0, [x17, xzr]" }, { text: "ldur\tx1, [x17, #0x8]" }, { text: "ldur\tx2, [x17, #0x8]" }], "this"),
    pin: arm64Run([...materialize("x17", arm64Layouts.pin.heapA), { text: "ldr\tx0, [x17, xzr]" }, { text: "ldur\tx1, [x17, #0x10]" }, { text: "ldur\tx2, [x17, #0x8]" }], "pin"),
  },
  pair("another status", "pin-status", same([line("add $0x1, %eax", 3)]), same([line("add $0x1, %eax", 3)]), {}, { status: { code: 3, signal: null } }),
  pair("another output", "pin-output", same([line("add $0x1, %eax", 3)]), same([line("add $0x1, %eax", 3)]), { stdout: "1\n" }, { stdout: "2\n" }),
  pair("a body without its JIT dump record", "pin-unreadable", same([line("add $0x1, %eax", 3)]), same([line("add $0x1, %eax", 3)]), {}, { dropRecords: true }),

  // ARM64.
  {
    name: "ARM64 materialization lengths",
    expect: "same",
    current: arm64Run([...materialize("x16", arm64Layouts.this.heapA), { text: "ldr\tx0, [x16]" }], "this"),
    pin: arm64Run([...materialize("x16", arm64Layouts.pin.heapA), { text: "ldr\tx0, [x16]" }], "pin"),
  },
  {
    name: "ARM64 reuse of the cached memory temporary",
    expect: "same",
    current: arm64Run([...materialize("x17", arm64Layouts.this.heapA), { text: "ldr\tx0, [x17, xzr]" }, { text: "ldur\tx1, [x17, #0x8]" }, { text: "add\tx17, x17, #0x10" }, { text: "ldr\tx2, [x17, xzr]" }], "this"),
    pin: arm64Run([...materialize("x17", arm64Layouts.pin.heapA), { text: "ldr\tx0, [x17, xzr]" }, ...materialize("x17", arm64Layouts.pin.heapA + 8n), { text: "ldr\tx1, [x17, xzr]" }, ...materialize("x17", arm64Layouts.pin.heapA + 16n), { text: "ldr\tx2, [x17, xzr]" }], "pin"),
  },
  {
    name: "ARM64 reuse across a conditional branch's fall-through",
    expect: "same",
    current: arm64Run([...materialize("x17", arm64Layouts.this.heapA), { text: "ldr\tx0, [x17, xzr]" }, { text: "cbz\tx0, {@ret7}" }, { text: "ldur\tx1, [x17, #0x8]" }], "this"),
    pin: arm64Run([...materialize("x17", arm64Layouts.pin.heapA), { text: "ldr\tx0, [x17, xzr]" }, { text: "cbz\tx0, {@ret7}" }, ...materialize("x17", arm64Layouts.pin.heapA + 8n), { text: "ldr\tx1, [x17, xzr]" }], "pin"),
  },
  {
    // The brk padding inside `Code at`, and a header the JIT heap rounds up past it.
    name: "ARM64 header larger than the dump",
    expect: "same",
    current: arm64Run([{ text: "add\tx0, x1, #0x8" }], "this", { pad: 8, round: 512 }),
    pin: arm64Run([{ text: "add\tx0, x1, #0x8" }], "pin", { pad: 8, round: 512 }),
  },
  {
    // The pin's address takes one movk fewer, so its listing holds one more padding brk.
    name: "ARM64 snippet whose materialization leaves other padding",
    expect: "same",
    current: arm64Run([], "this", {}, [paddedSnippet("aarch64", [...materialize("x17", arm64Layouts.this.heapA), { text: "ldrh\tw16, [x17, xzr]" }, { text: "orr\tw16, w16, w5" }, { text: "sturh\tw16, [x17]" }, { text: "b\t{@ret7}" }])]),
    pin: arm64Run([], "pin", {}, [paddedSnippet("aarch64", [...materialize("x17", arm64Layouts.pin.heapA), { text: "ldrh\tw16, [x17, xzr]" }, { text: "orr\tw16, w16, w5" }, { text: "sturh\tw16, [x17]" }, { text: "b\t{@ret7}" }])]),
  },
  {
    name: "ARM64 breakpoint inside a snippet's code",
    expect: "pin-code",
    current: arm64Run([], "this", {}, [paddedSnippet("aarch64", [{ text: "ldr\tx0, [x2]" }, { text: "b\t{@ret7}" }])]),
    pin: arm64Run([], "pin", {}, [paddedSnippet("aarch64", [{ text: "ldr\tx0, [x2]" }, { text: "brk\t#0" }, { text: "b\t{@ret7}" }])]),
  },
  {
    name: "ARM64 conditional branch to a thunk, near and far",
    expect: "same",
    current: arm64Run([{ text: "cmp\tx0, #0x5" }, { text: "b.eq\t{=thunkA}" }, { text: "ldr\tx1, [x2]" }], "this"),
    pin: arm64Run([{ text: "cmp\tx0, #0x5" }, { text: "b.ne\t{@skip}" }, { text: "b\t{=thunkA}" }, { text: "ldr\tx1, [x2]", label: "skip" }], "pin"),
  },
  {
    name: "ARM64 immediate",
    expect: "pin-code",
    current: arm64Run([{ text: "add\tx0, x1, #0x8" }], "this"),
    pin: arm64Run([{ text: "add\tx0, x1, #0x10" }], "pin"),
  },
  {
    name: "ARM64 register",
    expect: "pin-code",
    current: arm64Run([{ text: "ldr\tx0, [x1]" }], "this"),
    pin: arm64Run([{ text: "ldr\tx2, [x1]" }], "pin"),
  },
];

// The problems of every case, none when they all pass.
export function runCraftedPinTests(): string[] {
  const problems: string[] = [];
  for (const test of cases) {
    try {
      const { difference } = compareRuns(test.current.architecture, craft(test.current), craft(test.pin));
      const outcome = difference ? difference.check : "same";
      if (outcome !== test.expect) problems.push(`${test.name}: expected ${test.expect}, got ${outcome}${difference ? `: ${difference.detail}` : ""}`);
    } catch (error) {
      problems.push(`${test.name}: threw ${(error as Error).stack ?? error}`);
    }
  }
  return problems;
}

if (import.meta.main) {
  const problems = runCraftedPinTests();
  for (const problem of problems) console.log(`FAIL ${problem}`);
  console.log(`${cases.length - problems.length} of ${cases.length} crafted pairs as expected`);
  process.exit(problems.length ? 1 : 0);
}
