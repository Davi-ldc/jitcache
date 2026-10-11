// The checker of events-optimizing.js (harness sub-SPEC H6): the counts the script printed for its body equal the lines
// the engine printed for it (harness sub-SPEC N24 and N25), each counted where section 10.1 counts:
// - osrExits: "Speculation failure in <CB> @ exit #<n> (<bytecode index>, <kind>)", for every kind that may jettison,
//   which excludes ExceptionCheck and GenericUnwind;
// - dfgCompiles and ftlCompiles: "Optimizing compilation of <CB> ... result: CompilationSuccessful", an FTL compile when
//   the line names the DFG CodeBlock it was for and a DFG compile otherwise;
// - jettisons: "Did invalidate <CB>"; reoptimizations: "Did count reoptimization for <CB>".
// Each event the script drives must also have happened at least once.
//   bun optimizing-counts.ts <stdout> <stderr> <scratch>
import { readFileSync } from "node:fs";

const [stdoutPath, stderrPath] = process.argv.slice(2);
const line = readFileSync(stdoutPath, "utf8").split("\n").find(text => text.startsWith("{"));
if (!line) {
  console.error("the script printed no counts");
  process.exit(1);
}
const { name, counts } = JSON.parse(line) as { name: string; counts: Record<string, number> };

// A CodeBlock prints as <name>#<hash>:[...], so every tier's CodeBlock of the body starts with the same prefix.
const body = `${name}#`;
const logged = { osrExits: 0, dfgCompiles: 0, ftlCompiles: 0, jettisons: 0, reoptimizations: 0 };
for (const text of readFileSync(stderrPath, "utf8").split("\n")) {
  const trimmed = text.trimStart();
  if (trimmed.startsWith(`Speculation failure in ${body}`)) {
    const kind = / @ exit #\d+ \([^,]*, (\w+)\)/.exec(trimmed)?.[1];
    if (kind !== "ExceptionCheck" && kind !== "GenericUnwind") logged.osrExits++;
  } else if (trimmed.startsWith(`Optimizing compilation of ${body}`) && trimmed.endsWith(" result: CompilationSuccessful")) {
    if (trimmed.includes(" (for ")) logged.ftlCompiles++;
    else logged.dfgCompiles++;
  } else if (trimmed.startsWith(`Did invalidate ${body}`)) {
    logged.jettisons++;
  } else if (trimmed.startsWith(`Did count reoptimization for ${body}`)) {
    logged.reoptimizations++;
  }
}

const problems: string[] = [];
for (const [count, value] of Object.entries(logged)) {
  if (counts[count] !== value) problems.push(`${name}: ${count} is ${counts[count]}, and the engine's lines count ${value}`);
}
if (!(logged.dfgCompiles + logged.ftlCompiles)) problems.push(`${name}: no optimizing compile happened`);
for (const count of ["osrExits", "jettisons", "reoptimizations"] as const) {
  if (!logged[count]) problems.push(`${name}: no event of ${count} happened`);
}

if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
