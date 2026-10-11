// The checker of floor.js's Consumer runs (SPEC-cb.md section 11.3, the floor). The body never entered operationOptimize
// during its first invocation and entered it during its second, whose entry increment ended the slice finishCounter
// armed: the floor of two entry increments (section 5.3, I7), so m_counter read 0 there. In twins builds the Producer
// also wrote what it committed to <scratch>/cb-floor.json, and the counter then read that progress plus the two entries'
// increments, past its threshold in the sequence whose Producer rewrote the progress. Plain builds write no such file,
// and the check ends with the invocations and the slice.
//   bun floor-check.ts <stdout> <stderr> <scratch>
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { entryIncrement, finish, readOptimizeLog } from "./verbose-osr.ts";

interface Committed {
  rewritten: boolean;
  threshold: number;
  progress: number;
}

const [, stderrPath, scratch] = process.argv.slice(2);
const committedPath = join(scratch, "cb-floor.json");
const committed = existsSync(committedPath) ? (JSON.parse(readFileSync(committedPath, "utf8")) as Committed) : null;
const log = readOptimizeLog(stderrPath, "cb-floor", "floorBody");
const problems: string[] = [];

for (const invocation of [1, 2]) {
  if (!log.invocations.has(invocation)) problems.push(`the script wrote no marker for invocation ${invocation}`);
}
const early = log.entries.filter(entry => entry.invocation === null || entry.invocation < 2);
if (early.length) problems.push(`floorBody entered operationOptimize before its second invocation: ${JSON.stringify(early)}`);
const [second] = log.entries.filter(entry => entry.invocation === 2);
if (!second) problems.push("floorBody did not enter operationOptimize during its second invocation");
else {
  if (second.counter !== 0) problems.push(`m_counter reads ${second.counter} on the second invocation's entry, not 0, so the slice was not the floor's two increments`);
  if (committed) {
    const expected = committed.progress + 2 * entryIncrement;
    if (second.threshold !== committed.threshold) problems.push(`the counter's threshold is ${second.threshold}, and the Producer committed ${committed.threshold}`);
    if (Math.abs(second.count - expected) > 0.01) problems.push(`the counter reads ${second.count}, expected ${expected}, the committed progress after the floor's two entries`);
    if (committed.rewritten && !(second.count > second.threshold)) problems.push(`the rewritten progress ${second.count} does not exceed the threshold ${second.threshold}`);
  }
}

finish(problems);
