// The engine a run's code names: the build's jsc executable, its loaded segments and its symbols, which name a value
// inside the engine by symbol and offset.

import { spawnSync } from "node:child_process";
import { closeSync, openSync, readSync, realpathSync, statSync } from "node:fs";
import { join } from "node:path";
import { llvmMajor, llvmPrefix } from "../env.ts";
import { hex } from "./numbers.ts";

export interface Segment {
  start: bigint;
  end: bigint;
}

export interface EngineSymbol {
  address: bigint;
  name: string;
}

// The build's jsc, which Bun's build flags load at its link-time address: its loaded segments and its symbols, sorted by
// address and then by name.
export interface EngineImage {
  readonly segments: readonly Segment[];
  readonly symbols: readonly EngineSymbol[];
}

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

// The llvm-nm of JITCACHE_LLVM_PREFIX, the toolchain build.ts builds with, or one on PATH. The prefix's own llvm-nm
// loads libLLVM.so from the prefix, so its multiarch and LLVM library directories go on LD_LIBRARY_PATH.
function llvmNm(): { command: string; env: Record<string, string | undefined> } {
  const local = join(llvmPrefix, `usr/lib/llvm-${llvmMajor}/bin/llvm-nm`);
  if (statSync(local, { throwIfNoEntry: false })?.isFile()) {
    const libraries = join(llvmPrefix, "usr/lib", process.arch === "arm64" ? "aarch64-linux-gnu" : "x86_64-linux-gnu");
    const path = [libraries, join(llvmPrefix, `usr/lib/llvm-${llvmMajor}/lib`), process.env.LD_LIBRARY_PATH].filter(Boolean).join(":");
    return { command: local, env: { ...process.env, LD_LIBRARY_PATH: path } };
  }
  return { command: Bun.which(`llvm-nm-${llvmMajor}`) ?? "llvm-nm", env: { ...process.env } };
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
export function engineName(engine: EngineImage, value: bigint): string | null {
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
