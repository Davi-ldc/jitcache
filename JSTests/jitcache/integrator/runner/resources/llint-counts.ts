// The checker of events-llint.js (harness sub-SPEC H6): each call of h6Straight and h6DirectEval gained as many
// llintInstructions as the instructions $vm.dumpBytecodeFor lists for its body, h6Loop's gains over n = 0, 1 and 2 form
// an arithmetic progression with a positive step, and each function's other counts are 0.
//   bun llint-counts.ts <stdout> <stderr> <scratch>
import { readFileSync } from "node:fs";

const [stdoutPath, stderrPath] = process.argv.slice(2);
const problems: string[] = [];

interface Printed {
  name: string;
  gains: number[];
  others: Record<string, number>;
}
const printed = new Map<string, Printed>();
for (const line of readFileSync(stdoutPath, "utf8").split("\n")) {
  if (!line.startsWith("{")) continue;
  const result = JSON.parse(line) as Printed;
  printed.set(result.name, result);
}

// A dump starts with "<name>#<hash>:[<CodeBlock>]: <n> instructions (" and lists each instruction once, on a line of its
// own that starts with "[<offset>] <opcode>"; the footer's lines (identifiers, constants, handlers) start otherwise.
const dumps = new Map<string, { header: number; listed: number }>();
let current: { header: number; listed: number } | null = null;
for (const line of readFileSync(stderrPath, "utf8").split("\n")) {
  const header = /^([\w$]+)#[^:\s]*:\[.*\]: (\d+) instructions \(/.exec(line);
  if (header) {
    current = { header: Number(header[2]), listed: 0 };
    dumps.set(header[1], current);
  } else if (current && /^\[\s*\d+\] \S/.test(line)) {
    current.listed++;
  }
}

for (const name of ["h6Straight", "h6DirectEval"]) {
  const dump = dumps.get(name);
  const result = printed.get(name);
  if (!dump || !result) {
    problems.push(`${name}: ${dump ? "no counts were printed" : "no bytecode dump was written"}`);
    continue;
  }
  if (dump.listed !== dump.header) problems.push(`${name}: the dump lists ${dump.listed} instructions under a header that counts ${dump.header}`);
  result.gains.forEach((gain, call) => {
    if (gain !== dump.listed) problems.push(`${name}: call ${call + 1} gained ${gain} LLInt instructions, and the body has ${dump.listed}`);
  });
}

const loop = printed.get("h6Loop");
if (!loop) problems.push("h6Loop: no counts were printed");
else {
  const [zero, one, two] = loop.gains;
  if (!(one - zero > 0 && two - one === one - zero)) problems.push(`h6Loop: the gains ${loop.gains.join(", ")} over n = 0, 1 and 2 are no arithmetic progression with a positive step`);
}

for (const [name, result] of printed) {
  for (const [count, value] of Object.entries(result.others)) {
    if (value !== 0) problems.push(`${name}: ${count} is ${value}, expected 0`);
  }
}

if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
