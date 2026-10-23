// The machine lock: an exclusive flock on build.lock in the jitcache cache directory, which every build, test and bench
// command holds for its whole run, so no two of them load this machine at once. A command that finds it held prints the
// note its holder wrote beside it, which names the holder, and waits. The processes a holder starts see
// JITCACHE_MACHINE_LOCK and skip the lock, so a runner's nested runners never wait on their parent; they hold nothing
// themselves, and the lock ends when the holder exits, unless the holder hands them its descriptor, as build.ts does for
// its builder.

import { spawnSync } from "node:child_process";
import { mkdirSync, openSync, readFileSync, writeFileSync } from "node:fs";
import { homedir } from "node:os";
import { dirname, join, resolve } from "node:path";

const inheritedLock = "JITCACHE_MACHINE_LOCK";

// The holder's note, or null when there is none. A note whose process has exited while the lock is still held says
// so: a process it handed its descriptor to may hold the lock, as build.ts's builder does, or a holder that writes no
// note took the lock after it.
function readNote(path: string): string | null {
  let note: string;
  try {
    note = readFileSync(path, "utf8").trim();
  } catch {
    return null;
  }
  const pid = Number(/^pid (\d+) /.exec(note)?.[1]);
  if (!pid) return null;
  try {
    process.kill(pid, 0);
  } catch {
    return `${note}, which has exited; a process it started may still hold the lock`;
  }
  return note;
}

// Waits until this process holds the machine lock, which it keeps until it exits, and returns the descriptor it locked.
// A process that a holder started returns null at once and holds nothing, and so does a second call. flock(1) locks
// this process's own descriptor of the lock file, passed as its standard input. A flock lock belongs to the open file,
// so it outlives that child and lasts until every descriptor of that open file is closed: this process's at its exit,
// and the one any child received as a descriptor of its own.
export function holdMachineLock(): number | null {
  if (process.env[inheritedLock]) return null;
  const path = resolve(process.env.XDG_CACHE_HOME || join(homedir(), ".cache"), "jitcache", "build.lock");
  const notePath = `${path}.holder`;
  mkdirSync(dirname(path), { recursive: true });
  const descriptor = openSync(path, "a");
  const flock = (...options: string[]) => {
    const result = spawnSync("flock", ["--exclusive", ...options, "0"], { stdio: [descriptor, "inherit", "inherit"] });
    if (result.error) throw new Error(`flock cannot run: ${result.error.message}`);
    return result;
  };
  // flock exits with 1 when --nonblock finds the lock held, and with 64 or more on an error.
  let result = flock("--nonblock");
  if (result.status === 1) {
    console.log(`waiting for the machine lock ${path}, held by ${readNote(notePath) ?? "a process that left no note"}`);
    result = flock();
  }
  if (result.status !== 0) {
    const ending = result.signal ? `ended by ${result.signal}` : `exited with ${result.status}`;
    throw new Error(`flock could not take ${path}: it ${ending}`);
  }
  writeFileSync(notePath, `pid ${process.pid} since ${new Date().toISOString()}: ${process.argv.join(" ")}\n`);
  process.env[inheritedLock] = String(process.pid);
  return descriptor;
}
