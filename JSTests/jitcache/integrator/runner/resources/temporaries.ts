// What kill-points.js's checkers share (harness sub-SPEC H8): the body temporaries a killed Producer left in its
// artifact's cache/ directory, the .<32 hex>.tmp names of container sub-SPEC section 1.1. The runner gives each sequence
// one directory that holds artifact/ beside scratch/ (section 7.3), and hands a checker the scratch path.
//   bun <checker> <stdout> <stderr> <scratch>
import { readdirSync } from "node:fs";
import { dirname, join } from "node:path";

export function checkBodyTemporaries(expected: number) {
  const scratch = process.argv[4];
  const cache = join(dirname(scratch), "artifact", "cache");
  const temporaries = readdirSync(cache).filter(name => /^\.[0-9a-f]{32}\.tmp$/.test(name));
  if (temporaries.length !== expected) {
    console.error(`${cache} holds ${temporaries.length} body temporaries (${temporaries.join(", ") || "none"}), expected ${expected}`);
    process.exit(1);
  }
}
