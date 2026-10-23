// What a run is compared with. The oracle is a JITCache-off run of each distinct option set among a sequence's runs that
// configure JITCache: each of those runs must print what its oracle printed, leak no object its oracle did not, and in
// twins mode leave the same reachable heap. runs.ts starts these runs after each attempt's own (runOracle), and with
// --pin the pin comparison of each option set's Off run (pin.ts).

import { type RunSpec, configuresJITCache } from "./scripts.ts";

// The oracle's option sets, numbered j in this order, each a run's options without the JITCache ones and the runs that
// configure JITCache with them.
export function oracleOptionSets(runs: RunSpec[]): { options: string[]; runs: number[] }[] {
  const sets = new Map<string, { options: string[]; runs: number[] }>();
  runs.forEach((run, i) => {
    if (!configuresJITCache(run.role)) return;
    const options = run.options.filter(option => !option.startsWith("--jitcache"));
    const key = JSON.stringify(options);
    if (!sets.has(key)) sets.set(key, { options, runs: [] });
    sets.get(key)!.runs.push(i);
  });
  return [...sets.values()];
}

// Where two outputs first differ, by line.
export function firstDifference(a: Buffer, b: Buffer): string {
  const left = a.toString("utf8").split("\n");
  const right = b.toString("utf8").split("\n");
  const clip = (line: string | undefined) => (line === undefined ? "(end of output)" : JSON.stringify(line.length > 160 ? `${line.slice(0, 160)}…` : line));
  for (let line = 0; line < Math.max(left.length, right.length); ++line) {
    if (left[line] !== right[line]) return `at line ${line + 1}: ${clip(left[line])}, oracle ${clip(right[line])}`;
  }
  return "in bytes that are not text";
}

// ---------------------------------------------------------------------------------------------------------------------
// Leaks

// The objects LeakSanitizer reported leaked at a process's exit. A record is a header, "Direct leak of 52 byte(s) in 2
// object(s) allocated from:", then one line per frame of the allocation stack, "#0 0x2e7f104 in malloc ...", then, under
// report_objects=1, "Objects leaked above:" and one "0x701a793f5340 (26 bytes)" line per object. LeakSanitizer merges
// every object leaked from one stack into one record and prints their sum, so the runner compares objects: each keeps
// its own size and its record's frames without their program counters, which address randomization moves between
// processes; each frame keeps its function and source line, or its module and offset. A record without its object lines
// counts as one object of its total.
export interface LeakedObject {
  bytes: number;
  stack: string;
}

export function leakedObjects(stderr: string): LeakedObject[] {
  const objects: LeakedObject[] = [];
  const lines = stderr.split("\n");
  for (let n = 0; n < lines.length; ++n) {
    const header = /^(?:Direct|Indirect) leak of (\d+) byte\(s\) in \d+ object\(s\)/.exec(lines[n]);
    if (!header) continue;
    const frames: string[] = [];
    while (/^\s+#\d+ /.test(lines[n + 1] ?? "")) frames.push(lines[++n].trim().replace(/^(#\d+) 0x[0-9a-f]+/, "$1"));
    const stack = frames.join("\n");
    const sizes: number[] = [];
    let m = n + 1;
    while (lines[m] === "") ++m;
    if (lines[m] === "Objects leaked above:") {
      let size: RegExpExecArray | null;
      while ((size = /^0x[0-9a-f]+ \((\d+) bytes\)$/.exec(lines[m + 1] ?? ""))) {
        sizes.push(Number(size[1]));
        ++m;
      }
      n = m;
    }
    for (const bytes of sizes.length ? sizes : [Number(header[1])]) objects.push({ bytes, stack });
  }
  return objects;
}

// The objects a run leaked that its oracle did not, each paired with at most one oracle object of the same size and
// stack. A leak the oracle reports too is the engine's own.
export function leaksBeyond(leaked: LeakedObject[], oracle: LeakedObject[]): LeakedObject[] {
  const key = ({ bytes, stack }: LeakedObject) => `${bytes}\n${stack}`;
  const left = new Map<string, number>();
  for (const object of oracle) left.set(key(object), (left.get(key(object)) ?? 0) + 1);
  return leaked.filter(object => {
    const count = left.get(key(object)) ?? 0;
    if (count) left.set(key(object), count - 1);
    return !count;
  });
}

export const describeLeaks = (leaks: LeakedObject[]) =>
  `${leaks.length} object(s), the first ${leaks[0].bytes} bytes allocated from ${leaks[0].stack.split("\n").slice(0, 8).join(" <- ")}`;
