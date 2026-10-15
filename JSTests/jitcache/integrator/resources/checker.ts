// What this directory's checkers share. The runner calls a checker as bun <checker> <stdout file> <stderr file> <scratch>
// after the run a jitcache-check directive names, and fails that run when the checker exits nonzero. A jsc run with
// JITCache writes, with --jitcache-log, its start result and its final status to standard error as toJSON lines; a
// Maintenance run writes its summary line to standard output. Each sequence's directory holds artifact/ beside
// scratch/.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";

const [stdoutPath, stderrPath, scratchPath] = process.argv.slice(2);

export function failCheck(message: string): never {
  console.error(message);
  process.exit(1);
}

// Every {"jitcache":"<kind>", ...} line of the run's standard error, parsed.
function jitcacheLines(kind: "start" | "delta" | "status"): Record<string, any>[] {
  const prefix = `{"jitcache":"${kind}"`;
  return readFileSync(stderrPath, "utf8")
    .split("\n")
    .filter(line => line.startsWith(prefix))
    .map(line => JSON.parse(line));
}

function onlyLine(kind: "start" | "status"): Record<string, any> {
  const lines = jitcacheLines(kind);
  if (lines.length !== 1) failCheck(`the run wrote ${lines.length} ${kind} lines, expected one`);
  return lines[0];
}

// The start result's outcome and step, and a detail that names one of detailNames when any are given.
export function checkStartResult(outcome: string, step: string, detailNames: string[] = []) {
  const start = onlyLine("start");
  if (start.outcome !== outcome || start.step !== step) {
    const found = `${start.outcome} at ${JSON.stringify(start.step)}`;
    failCheck(`start returned ${found}, expected ${outcome} at ${step}: ${JSON.stringify(start)}`);
  }
  if (detailNames.length && !detailNames.some(name => String(start.detail).includes(name)))
    failCheck(`the start detail ${JSON.stringify(start.detail)} names none of ${detailNames.join(", ")}`);
}

// The final status's fields, each compared with ===.
export function checkFinalStatus(expected: Record<string, unknown>) {
  const status = onlyLine("status");
  for (const [field, value] of Object.entries(expected)) {
    if (status[field] === value) continue;
    const found = JSON.stringify(status[field]);
    failCheck(`the final status's ${field} is ${found}, expected ${JSON.stringify(value)}: ${JSON.stringify(status)}`);
  }
}

// cache/pack's size and its valid end, the root: 8 bytes little-endian at 56.
function packEnds(): { fileSize: number; validEnd: number } {
  const pack = readFileSync(join(dirname(scratchPath), "artifact", "cache", "pack"));
  const view = new DataView(pack.buffer, pack.byteOffset, pack.byteLength);
  return { fileSize: pack.byteLength, validEnd: Number(view.getBigUint64(56, true)) };
}

// Where the check after a kill records the pack's size for the check after the compact that follows.
const packSizeAfterKill = () => join(scratchPath, "pack-size-after-kill");

// After a killed commit: cache/pack holds bytes past its valid end exactly when the kill came after the commit appended
// some and before it stored them in the root.
export function checkBytesPastValidEnd(expected: boolean) {
  const { fileSize, validEnd } = packEnds();
  if ((fileSize > validEnd) !== expected)
    failCheck(`cache/pack holds ${fileSize - validEnd} bytes past its valid end of ${validEnd}, expected ${expected ? "some" : "none"}`);
  writeFileSync(packSizeAfterKill(), String(fileSize));
}

// The compact at ratio 0 after a killed commit: it removed no temporary, since commits create none, and evicted no body;
// it reclaimed exactly the bytes cache/pack lost since the kill, and the pack now ends at its valid end.
export function checkCompactCleanup() {
  const lines = readFileSync(stdoutPath, "utf8")
    .split("\n")
    .filter(line => line.startsWith("compact: "));
  if (lines.length !== 1) failCheck(`the compact wrote ${lines.length} summary lines, expected one`);
  const summary = /^compact: done, (\d+) temporaries removed, (\d+) bodies evicted, (\d+) bytes reclaimed$/.exec(lines[0]);
  if (!summary) failCheck(`the compact's summary is ${JSON.stringify(lines[0])}`);
  if (Number(summary[1])) failCheck(`the compact removed ${summary[1]} temporaries, expected none`);
  if (Number(summary[2])) failCheck(`the compact evicted ${summary[2]} bodies, expected none`);
  const sizeAfterKill = Number(readFileSync(packSizeAfterKill(), "utf8"));
  const { fileSize, validEnd } = packEnds();
  if (fileSize !== validEnd) failCheck(`cache/pack holds ${fileSize - validEnd} bytes past its valid end of ${validEnd}`);
  if (Number(summary[3]) !== sizeAfterKill - fileSize)
    failCheck(`the compact reclaimed ${summary[3]} bytes, but cache/pack went from ${sizeAfterKill} to ${fileSize} bytes`);
}
