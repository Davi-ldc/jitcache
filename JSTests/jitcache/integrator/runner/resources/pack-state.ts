// kill-points.js's checker: the state a killed Producer left in its artifact's cache/ directory, read from cache/pack
// itself, against the expectations the jitcache-check directive gives after the checker:
//   bun pack-state.ts <stdout> <stderr> <scratch> <expectation>...
// The expectations are retired=yes or retired=no and temporaries=<n>, the pack temporaries beside the pack, which every
// directive gives, and where it decides them bodies=<n> or bodies>=<n>, the bodies the index lists, and valid-end=<n>
// or valid-end=empty, the root, where empty is a new artifact's. The pack's root, 8 bytes at 56, is its valid end; the
// 8 bytes before the valid end are the trailer of the latest index block and give its offset; the index is the full
// index that block's chain ends at, followed by the deltas applied in order. The runner gives each sequence one
// directory that holds artifact/ beside scratch/.
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";

interface PackState {
  validEnd: number;
  liveBytes: number; // the prefix and the records of the bodies listed, as the engine counts them
  deadBytes: number; // the rest of the valid bytes: superseded records and index blocks
  fileSize: number;
  bodies: number;
  retired: boolean;
  temporaries: number;
}

function fail(message: string): never {
  console.error(message);
  process.exit(1);
}

function readPackState(cache: string): PackState {
  const bytes = readFileSync(join(cache, "pack"));
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const u64 = (offset: number) => Number(view.getBigUint64(offset, true));
  const validEnd = u64(56);
  if (validEnd < 64 + 40 || validEnd > bytes.byteLength) fail(`${cache}/pack has no valid end inside the file`);

  const chain: { at: number; kind: number; count: number }[] = [];
  for (let at = u64(validEnd - 8); ; ) {
    if (bytes.toString("latin1", at, at + 8) !== "JITCINDX") fail(`${cache}/pack has no index block at ${at}`);
    const kind = view.getUint32(at + 16, true);
    chain.push({ at, kind, count: view.getUint32(at + 20, true) });
    if (kind === 0) break;
    at = u64(at + 8);
  }
  // Each listed key's body size; a record takes its 64-byte header and the body, padded to 8.
  const sizes = new Map<string, number>();
  for (const block of chain.reverse()) {
    if (block.kind === 0) sizes.clear();
    for (let index = 0; index < block.count; ++index) {
      const entry = block.at + 32 + 80 * index;
      const key = bytes.toString("hex", entry, entry + 40);
      sizes.set(key, u64(entry + 48));
    }
  }
  let liveBytes = 64;
  for (const size of sizes.values()) liveBytes += Math.ceil((64 + size) / 8) * 8;
  return {
    validEnd,
    liveBytes,
    deadBytes: validEnd - liveBytes,
    fileSize: bytes.byteLength,
    bodies: sizes.size,
    retired: u64(48) !== 0,
    temporaries: readdirSync(cache).filter(name => /^\.pack\.[0-9a-f]{32}\.tmp$/.test(name)).length,
  };
}

// A new artifact's pack: the 64-byte prefix and an empty full index of 40 bytes, its header and its trailer.
const emptyPackValidEnd = 64 + 40;

interface Expectations {
  bodies?: number;
  atLeastBodies?: number;
  validEnd?: number;
  retired: boolean;
  temporaries: number;
}

// The directive's expectations. Every directive states the retired mark and the temporaries, which any kill can leave
// behind, and the bodies and the valid end where its kill point decides them.
function parseExpectations(words: string[]): Expectations {
  const expected: Partial<Expectations> = {};
  for (const word of words) {
    const [, name, operator, value] = /^([a-z-]+)(>=|=)(\S+)$/.exec(word) ?? [];
    const count = () => {
      if (!/^\d+$/.test(value)) fail(`pack-state.ts's ${word} holds no count`);
      return Number(value);
    };
    switch (`${name}${operator}`) {
      case "bodies=":
        expected.bodies = count();
        break;
      case "bodies>=":
        expected.atLeastBodies = count();
        break;
      case "valid-end=":
        expected.validEnd = value === "empty" ? emptyPackValidEnd : count();
        break;
      case "retired=":
        if (value !== "yes" && value !== "no") fail(`pack-state.ts's ${word} is neither yes nor no`);
        expected.retired = value === "yes";
        break;
      case "temporaries=":
        expected.temporaries = count();
        break;
      default:
        fail(`pack-state.ts has no expectation ${word}`);
    }
  }
  if (expected.retired === undefined || expected.temporaries === undefined) fail("pack-state.ts takes retired=yes or no and temporaries=<n>, and may add bodies=<n>, bodies>=<n> and valid-end=<n> or empty");
  return { ...expected, retired: expected.retired, temporaries: expected.temporaries };
}

function checkPackState(expected: Expectations) {
  const cache = join(dirname(process.argv[4]), "artifact", "cache");
  const state = readPackState(cache);
  const problems: string[] = [];
  if (expected.validEnd !== undefined && state.validEnd !== expected.validEnd) problems.push(`a valid end of ${state.validEnd} where ${expected.validEnd} was expected`);
  if (expected.bodies !== undefined && state.bodies !== expected.bodies) problems.push(`${state.bodies} bodies where ${expected.bodies} were expected`);
  if (expected.atLeastBodies !== undefined && state.bodies < expected.atLeastBodies) problems.push(`${state.bodies} bodies where at least ${expected.atLeastBodies} were expected`);
  if (state.retired !== expected.retired) problems.push(state.retired ? "a retired pack" : "a pack that is not retired");
  if (state.temporaries !== expected.temporaries) problems.push(`${state.temporaries} pack temporaries where ${expected.temporaries} were expected`);
  if (problems.length) fail(`${cache} holds ${problems.join(", ")} (${JSON.stringify(state)})`);
}

checkPackState(parseExpectations(process.argv.slice(5)));
