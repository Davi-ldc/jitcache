// The paths and the LLVM version the runner, its self-tests, the benches and the pin comparison share. The repository's
// root comes from this module's place in it, and each path outside the repository from its JITCACHE_ variable or its
// default under the home directory.

import { homedir } from "node:os";
import { isAbsolute, join, relative, resolve } from "node:path";

export const repositoryRoot = resolve(import.meta.dirname, "../../..");

// A file as a report names it: its path from the repository's root, or its absolute path when it lies outside.
export function repositoryPath(path: string): string {
  const inside = relative(repositoryRoot, resolve(path));
  return inside.startsWith("..") || isAbsolute(inside) ? resolve(path) : inside;
}

// The LLVM major the repository's build.ts builds with, installed in the package layout under JITCACHE_LLVM_PREFIX.
export const llvmMajor = 23;
export const llvmPrefix = resolve(process.env.JITCACHE_LLVM_PREFIX || join(homedir(), `collo-local/tools/llvm-${llvmMajor}`));

// The Bun checkout, whose history holds the Bun pin, and whose tests Bun's own test run reads.
export const bunSource = resolve(process.env.JITCACHE_BUN_SOURCE || join(homedir(), "bun"));

// The root an aarch64 build's processes run in under QEMU user mode: the sysroot build.ts links against, or for a build
// with ASan, whose runtime needs glibc 2.34 or newer, which that sysroot lacks, a root of its own.
export const aarch64Sysroot = resolve(process.env.JITCACHE_AARCH64_SYSROOT || join(homedir(), "collo-local/tools/linux-sysroot-glibc-arm64"));
export const aarch64AsanRoot = resolve(process.env.JITCACHE_AARCH64_ASAN_ROOT || join(homedir(), "jitcachearm/arm64-glibc-root"));
