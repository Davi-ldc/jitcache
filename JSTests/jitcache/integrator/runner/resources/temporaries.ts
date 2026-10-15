// What kill-points.js's checkers share: the state a killed Producer left in its artifact's cache/ directory, read from
// cache/pack itself. The pack's root, 8 bytes at 56, is its valid end; the 8 bytes before the valid end are the trailer
// of the latest index block and give its offset; the index is the full index that block's chain ends at, followed by the
// deltas applied in order. The runner gives each sequence one directory that holds artifact/ beside scratch/, and hands a
// checker the scratch path.
//   bun <checker> <stdout> <stderr> <scratch>
import { readdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";

export interface PackState {
  validEnd: number;
  liveBytes: number; // the prefix and the records of the bodies listed, as the engine counts them
  deadBytes: number; // the rest of the valid bytes: superseded records, tombstoned ones and index blocks
  fileSize: number;
  bodies: number;
  retired: boolean;
  temporaries: number;
}

function fail(message: string): never {
  console.error(message);
  process.exit(1);
}

export function readPackState(cache: string): PackState {
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
      if (u64(entry + 40)) sizes.set(key, u64(entry + 48));
      else sizes.delete(key);
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

// The checker's artifact cache/ directory.
export function cacheDirectory(): string {
  return join(dirname(process.argv[4]), "artifact", "cache");
}

// A new artifact's pack: the 64-byte prefix and an empty full index of 40 bytes, its header and its trailer.
export const emptyPackValidEnd = 64 + 40;

export function checkPackState(expected: { bodies?: number; atLeastBodies?: number; validEnd?: number; retired: boolean; temporaries: number }) {
  const cache = cacheDirectory();
  const state = readPackState(cache);
  const problems: string[] = [];
  if (expected.validEnd !== undefined && state.validEnd !== expected.validEnd) problems.push(`a valid end of ${state.validEnd} where ${expected.validEnd} was expected`);
  if (expected.bodies !== undefined && state.bodies !== expected.bodies) problems.push(`${state.bodies} bodies where ${expected.bodies} were expected`);
  if (expected.atLeastBodies !== undefined && state.bodies < expected.atLeastBodies) problems.push(`${state.bodies} bodies where at least ${expected.atLeastBodies} were expected`);
  if (state.retired !== expected.retired) problems.push(state.retired ? "a retired pack" : "a pack that is not retired");
  if (state.temporaries !== expected.temporaries) problems.push(`${state.temporaries} pack temporaries where ${expected.temporaries} were expected`);
  if (problems.length) fail(`${cache} holds ${problems.join(", ")} (${JSON.stringify(state)})`);
}
