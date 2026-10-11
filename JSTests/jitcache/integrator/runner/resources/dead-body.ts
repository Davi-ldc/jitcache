// The checker of events-dead-body.js (harness sub-SPEC H6). It requires the function made from h6Dying's text after the
// collections to show fewer LLInt instructions than h6Dying, so that its UnlinkedCodeBlock is new and h6Dying's died: a
// reused one would hold h6Dying's counts plus that function's call. It then requires the dump's total to exceed the
// printed counts of the bodies still alive by at least h6Dying's counts. The total is the live UnlinkedCodeBlocks'
// counts plus the retired ones'. Of the live ones, the script prints all but the program's and main's last
// instructions, a few dozen against the hundreds of h6Dying's loops, so a total that lost h6Dying's counts falls short.
//   bun dead-body.ts <stdout> <stderr> <scratch>
import { readFileSync } from "node:fs";
import { join } from "node:path";

type Counts = Record<string, number>;
const counts = ["llintInstructions", "baselineCompiles", "dfgCompiles", "ftlCompiles", "osrExits", "jettisons", "reoptimizations"];
const [stdoutPath, , scratch] = process.argv.slice(2);
const problems: string[] = [];

const line = readFileSync(stdoutPath, "utf8").split("\n").find(text => text.startsWith("{"));
const printed = line ? (JSON.parse(line) as { dead: Counts | null; reborn: Counts | null; alive: Record<string, Counts | null> }) : null;
const dump = readFileSync(join(scratch, "dead-body-events.jsonl"), "utf8").split("\n").filter(Boolean).map(text => JSON.parse(text));
const total = dump.find(entry => entry.total)?.total as Counts | undefined;

if (!printed) problems.push("the script printed no counts");
else if (!printed.dead || !printed.reborn || Object.values(printed.alive).some(body => !body)) {
  problems.push(`a function the script read had no CodeBlock: ${line}`);
} else {
  const { dead, reborn, alive } = printed;
  if (!dead.llintInstructions) problems.push("h6Dying ran no LLInt instruction");
  if (!reborn.llintInstructions) problems.push("the function made from h6Dying's text ran no LLInt instruction");
  if (reborn.llintInstructions >= dead.llintInstructions) {
    problems.push(`the function made from h6Dying's text after the collections shows ${reborn.llintInstructions} LLInt instructions, not fewer than h6Dying's ${dead.llintInstructions}: it reused h6Dying's UnlinkedCodeBlock, which did not die`);
  }
  if (!total) problems.push("the dump holds no total");
  else {
    for (const count of counts) {
      const live = [reborn, ...Object.values(alive)].reduce((sum, body) => sum + body![count], 0);
      // A count missing on either side compares as NaN, which fails here too.
      if (!(total[count] - live >= dead[count])) {
        problems.push(`the total's ${count}, ${total[count]}, exceeds the printed live bodies' ${live} by less than h6Dying's ${dead[count]}`);
      }
    }
  }
}

if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
