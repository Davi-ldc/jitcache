// The checker of events-live-bodies.js (harness sub-SPEC H6): for each function the Producer recorded, the body-event
// dump has a line with its key and exactly the counts jitcacheBodyEvents gave for it at the end of the script.
//   bun live-bodies.ts <stdout> <stderr> <scratch>
import { readFileSync } from "node:fs";
import { join } from "node:path";

const scratch = process.argv[4];
const counts = ["llintInstructions", "baselineCompiles", "dfgCompiles", "ftlCompiles", "osrExits", "jettisons", "reoptimizations"];
const bodies = JSON.parse(readFileSync(join(scratch, "live-bodies.json"), "utf8")) as { name: string; key: string | null; counts: Record<string, number> }[];
const lines = readFileSync(join(scratch, "live-body-events.jsonl"), "utf8").split("\n").filter(Boolean).map(line => JSON.parse(line));
const problems: string[] = [];

if (!lines.some(line => line.total)) problems.push("the dump holds no total");
for (const body of bodies) {
  if (!body.key) {
    problems.push(`the Producer recorded no key for ${body.name}`);
    continue;
  }
  const keyed = lines.filter(line => line.key === body.key);
  if (!keyed.length) problems.push(`the dump has no line for ${body.name}'s key ${body.key}`);
  else if (!keyed.some(line => counts.every(count => line[count] === body.counts[count]))) {
    problems.push(`${body.name}'s line ${JSON.stringify(keyed[0])} differs from the counts the script read last, ${JSON.stringify(body.counts)}`);
  }
}
if (!bodies.some(body => body.counts?.baselineCompiles)) problems.push("no recorded function compiled to baseline");

if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
