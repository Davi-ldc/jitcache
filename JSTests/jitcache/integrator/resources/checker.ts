// What this directory's checkers share. The runner calls a checker as bun <checker> <stdout file> <stderr file> <scratch>
// after the run a jitcache-check directive names, and fails that run when the checker exits nonzero (harness sub-SPEC
// section 7.2). A jsc run with JITCache writes, with --jitcache-log, its start result and its final status to standard
// error as toJSON lines (SPEC-integrator.md sections 3.1 and 11.1); a Maintenance run writes its summary line to standard
// output (maintenance sub-SPEC section 5). Each sequence's directory holds artifact/ beside scratch/ (harness sub-SPEC
// section 7.3).
import { readdirSync, readFileSync } from "node:fs";
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

// A clean that removed the given number of temporaries, after which cache/ holds none: the .<32 hex>.tmp names of
// container sub-SPEC section 1.1.
export function checkCleanRemoved(temporaries: number) {
  const lines = readFileSync(stdoutPath, "utf8")
    .split("\n")
    .filter(line => line.startsWith("clean: "));
  if (lines.length !== 1) failCheck(`the clean wrote ${lines.length} summary lines, expected one`);
  const summary = /^clean: done, (\d+) temporaries removed, \d+ bytes reclaimed$/.exec(lines[0]);
  if (!summary) failCheck(`the clean's summary is ${JSON.stringify(lines[0])}`);
  if (Number(summary[1]) !== temporaries) {
    failCheck(`the clean removed ${summary[1]} temporaries, expected ${temporaries}`);
  }
  const cache = join(dirname(scratchPath), "artifact", "cache");
  const left = readdirSync(cache).filter(name => /^\.[0-9a-f]{32}\.tmp$/.test(name));
  if (left.length) failCheck(`${cache} still holds ${left.join(", ")} after the clean`);
}
