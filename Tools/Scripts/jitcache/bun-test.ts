#!/usr/bin/env bun
// Bun's JITCache tests under the machine lock (lock.ts). From the Bun checkout, JITCACHE_BUN_SOURCE or ~/bun as build.ts
// finds it, this runs
//
//   <Bun executable> test --timeout 120000 --max-concurrency=<n> <test files>
//
// with n = 4 unless given, and every test file of test/js/bun/jitcache/ unless other test files, paths in the Bun
// checkout, are given. Some of these tests take longer than Bun's 5 s default in an ASan build, and each starts Bun
// processes of its own, so only a few run at a time. The runner's ladder runs the same command, with a JUnit report.
//
//   bun Tools/Scripts/jitcache/bun-test.ts --bun=<Bun executable> [--max-concurrency=<n>] [<test files>...]

import { spawnSync } from "node:child_process";
import { statSync } from "node:fs";
import { constants } from "node:os";
import { resolve } from "node:path";
import { bunSource } from "./env.ts";
import { holdMachineLock } from "./lock.ts";

const usage = "Usage: bun-test.ts --bun=<Bun executable> [--max-concurrency=<n>] [<test files in the Bun checkout>...]";

// The tests that run when no file is named, every test file of the directory, and how many run at once. The leading ./
// makes bun test scan the directory as a path; without it, an argument is a filter over the whole checkout.
export const defaultBunTests = ["./test/js/bun/jitcache/"];
export const defaultMaxConcurrency = 4;

// bun test's arguments; junit names the JUnit report it also writes.
export function bunTestArguments(maxConcurrency: number, files: string[], junit: string | null = null): string[] {
  return ["test", "--timeout", "120000", `--max-concurrency=${maxConcurrency}`, ...(junit ? ["--reporter=junit", `--reporter-outfile=${junit}`] : []), ...files];
}

function main(argv: string[]): number {
  let bun: string | null = null;
  let maxConcurrency = defaultMaxConcurrency;
  const files: string[] = [];
  for (const argument of argv) {
    const equals = argument.indexOf("=");
    const [name, value] = equals < 0 ? [argument, ""] : [argument.slice(0, equals), argument.slice(equals + 1)];
    if (!argument.startsWith("-")) files.push(argument);
    else if (name === "--bun" && value) bun = resolve(value);
    else if (name === "--max-concurrency" && /^[1-9]\d*$/.test(value)) maxConcurrency = Number(value);
    else {
      console.error(`bun-test.ts: unknown or malformed argument ${argument}\n${usage}`);
      return 2;
    }
  }
  if (!bun || !statSync(bun, { throwIfNoEntry: false })?.isFile()) {
    console.error(`bun-test.ts: ${bun ? `--bun names no file: ${bun}` : "--bun names the Bun executable that runs the tests"}\n${usage}`);
    return 2;
  }
  if (!statSync(bunSource, { throwIfNoEntry: false })?.isDirectory()) {
    console.error(`bun-test.ts: the Bun checkout ${bunSource} is missing; JITCACHE_BUN_SOURCE names another`);
    return 2;
  }
  holdMachineLock();
  const result = spawnSync(bun, bunTestArguments(maxConcurrency, files.length ? files : defaultBunTests), { cwd: bunSource, stdio: "inherit" });
  if (result.error) {
    console.error(`bun-test.ts: ${bun} could not start: ${result.error.message}`);
    return 1;
  }
  return result.status ?? (result.signal ? 128 + constants.signals[result.signal] : 1);
}

if (import.meta.main) process.exit(main(process.argv.slice(2)));
