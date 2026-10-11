// The checker of polymorphic.js's Consumer run (SPEC-cb.md section 11.3, a polymorphic body). Its counter did not travel,
// so it starts from setup's arming, with at most one point of progress (N4, I7): the body first enters operationOptimize
// after the native warm-up, at an invocation k past the second, the one where a carried counter at its threshold would
// cross, and its counter then reads the k entries' increments and that fraction alone. The fraction is what truncating the
// slice into m_counter leaves against its float copy in m_totalCount, which float rounding can carry up to a whole point.
//   bun polymorphic-check.ts <stdout> <stderr> <scratch>
import { entryIncrement, finish, readOptimizeLog } from "./verbose-osr.ts";

const [, stderrPath] = process.argv.slice(2);
const log = readOptimizeLog(stderrPath, "cb-polymorphic", "polyBody");
const problems: string[] = [];

if (!log.invocations.has(1)) problems.push("the script wrote no invocation markers");
const [first] = log.entries;
if (!first) problems.push(`polyBody never entered operationOptimize in ${log.invocations.size} invocations`);
else if (first.invocation === null || first.invocation <= 2) problems.push(`polyBody entered operationOptimize at invocation ${first.invocation}, before the native warm-up`);
else {
  const fromArming = first.count - entryIncrement * first.invocation;
  if (fromArming < 0 || fromArming > 1) problems.push(`at invocation ${first.invocation} the counter reads ${first.count}, which is not ${entryIncrement} per entry from setup's arming`);
}

finish(problems);
