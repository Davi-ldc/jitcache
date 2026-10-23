// What one run of the pin comparison leaves, and the options that make it leave that: its end, its standard output and
// error, and the perf JIT dump of the bytes each of its allocations holds.

import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";

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

// The options both runs take last, so they override the run's own. Concurrent JIT off compiles every body at a fixed
// point of the script, and with the GC off nothing the code references dies or moves, so each address names one object
// for the whole run; the dump and log options leave the emitted bytes unchanged. A debug build poisons the dead
// operands of an optimizing tier's exit, and the exit label the log options print reads that poison as a cell and
// crashes, so the runs recover dead operands as undefined, as a release build does. Only optimizing exits and the FTL's
// OSR entry read that option, never baseline code or a thunk.
export function pinRunOptions(jitDumpDirectory: string): string[] {
  return [
    "--useConcurrentJIT=false",
    "--useGC=false",
    "--dumpDisassembly=true",
    "--logJIT=true",
    "--poisonDeadOSRExitVariables=false",
    "--useJITDump=true",
    `--jitDumpDirectory=${jitDumpDirectory}`,
  ];
}

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

// One side's run as the runner keeps it: <prefix>.status, .stdout and .stderr, and the JIT dump in the directory
// <prefix>.
export function readCapture(prefix: string): RunCapture {
  return {
    status: JSON.parse(readFileSync(`${prefix}.status`, "utf8")) as RunStatus,
    stdout: readFileSync(`${prefix}.stdout`),
    stderr: readFileSync(`${prefix}.stderr`, "utf8"),
    jitDump: readJITDump(prefix),
  };
}
