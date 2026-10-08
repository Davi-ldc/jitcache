#!/usr/bin/env bun
import { spawnSync } from "node:child_process";
import { mkdirSync, realpathSync, statSync } from "node:fs";
import { constants, homedir } from "node:os";
import { join, resolve } from "node:path";

const BUN_PIN = "744846f844374847c902b5e7fd59b4342a51ef99";
const JOBS = 5;
// Each target selects a Bun profile and a ninja target; without one, Bun builds its
// default targets, the executable and its smoke test. Targets of one profile pass
// identical config flags, so they share build.ninja, the build directory and its WebKit.
// The twins profile is debug-local with JITCache's test builds on (ENABLE_JITCACHE_TWINS):
// its WebKit target builds jsc and testjitcache.
const TARGETS = new Map<string, { profile: string; ninja?: string }>([
  ["debug", { profile: "debug-local", ninja: "WebKit" }],
  ["release", { profile: "release-local", ninja: "WebKit" }],
  ["ci-release", { profile: "ci-release", ninja: "WebKit" }],
  ["bun-debug", { profile: "debug-local" }],
  ["twins", { profile: "debug-local-twins", ninja: "WebKit" }],
  ["bun-twins", { profile: "debug-local-twins" }],
]);

function fail(message: string, code = 125): never {
  console.error(message);
  process.exit(code);
}

const args = process.argv.slice(2);
const archOption = args.find(arg => arg.startsWith("--arch="));
const positional = args.filter(arg => arg !== archOption);
const requested = positional[0] || "debug";
if (requested === "-h" || requested === "--help") {
  console.log("Usage: bun build.ts [debug|release|ci-release|bun-debug|twins|bun-twins] [--arch=aarch64]");
  console.log("Build WebKit/JSC with Bun's build system, from a Bun checkout based on the pin. Default: debug.");
  console.log("bun-debug builds the Bun executable against local WebKit in the debug build directory.");
  console.log("twins builds jsc and testjitcache with JITCache's test builds on (ENABLE_JITCACHE_TWINS);");
  console.log("bun-twins builds the Bun executable against that WebKit, in the same build directory.");
  console.log("--arch=aarch64 cross-compiles on an x86_64 host against the sysroot in JITCACHE_AARCH64_SYSROOT.");
  process.exit(0);
}
const target = TARGETS.get(requested);
if (!target) fail(`Unknown target: ${requested}`, 2);
if (positional.length > 1) fail("Expected at most one target argument.", 2);

const home = homedir();
const engineDir = realpathSync(import.meta.dirname);
const bunSource = resolve(process.env.JITCACHE_BUN_SOURCE || join(home, "bun"));
const buildRoot = resolve(process.env.JITCACHE_BUILD_ROOT || join(home, "collo-local/build/jitcache"));
const llvmPrefix = resolve(process.env.JITCACHE_LLVM_PREFIX || join(home, "collo-local/tools/llvm-21"));
const builder = join(bunSource, "scripts/build.ts");

if (!statSync(builder, { throwIfNoEntry: false })?.isFile()) {
  fail(`Bun builder is missing: ${builder}\nPoint JITCACHE_BUN_SOURCE at a Bun checkout whose history contains ${BUN_PIN}.`);
}
// The checkout is the pin itself or our commits on top of it; any other history fails.
const git = (...gitArgs: string[]) => spawnSync("git", ["-C", bunSource, ...gitArgs], { encoding: "utf8" });
const pin = git("merge-base", "--is-ancestor", BUN_PIN, "HEAD");
if (pin.error) fail(`Cannot verify the Bun pin: ${pin.error.message}`);
if (pin.status === 1) {
  const head = git("rev-parse", "--verify", "HEAD").stdout.trim();
  fail(`Bun checkout ${bunSource} is not based on the pin: ${BUN_PIN} is not an ancestor of HEAD ${head}.`);
}
if (pin.status !== 0) {
  const reason = pin.stderr.trim() || `git exited with status ${pin.status}`;
  fail(`Cannot verify that ${bunSource} contains the pin ${BUN_PIN}: ${reason}`);
}

const hostArchitecture = process.arch === "x64" ? "x86_64" : process.arch === "arm64" ? "aarch64" : null;
if (process.platform !== "linux" || !hostArchitecture) {
  fail(`Unsupported native build host: ${process.platform}/${process.arch}`);
}
// x86_64 hosts can cross-compile for aarch64. The sysroot has the glibc 2.31 and gcc-13 of
// Bun's CI sysroot, plus the ICU that local WebKit links.
const architecture = archOption?.slice("--arch=".length) ?? hostArchitecture;
const cross = architecture !== hostArchitecture;
if (cross && (hostArchitecture !== "x86_64" || architecture !== "aarch64")) {
  fail(`Cannot build for ${architecture} on ${hostArchitecture}; only x86_64 hosts cross-compile, to aarch64.`, 2);
}
const sysroot = resolve(process.env.JITCACHE_AARCH64_SYSROOT || join(home, "collo-local/tools/linux-sysroot-glibc-arm64"));
if (cross && !statSync(join(sysroot, "usr/include/c++/13"), { throwIfNoEntry: false })?.isDirectory()) {
  fail(`aarch64 sysroot is missing: ${sysroot}\nPoint JITCACHE_AARCH64_SYSROOT at a sysroot like Bun's /opt/linux-sysroot-glibc-arm64, plus ICU 70.1 or newer.`);
}
const buildDir = join(buildRoot, `linux-${architecture}-${target.profile}`);
const env = { ...process.env };
env.PATH = `${join(home, "collo-local/tools/bin")}:${env.PATH || ""}`;

// Bun's builder still validates the selected compiler version.
const llvmBin = join(llvmPrefix, "usr/lib/llvm-21/bin");
if (statSync(llvmBin, { throwIfNoEntry: false })?.isDirectory()) {
  const libdir = hostArchitecture === "x86_64" ? "x86_64-linux-gnu" : "aarch64-linux-gnu";
  env.PATH = `${llvmBin}:${env.PATH}`;
  env.LD_LIBRARY_PATH = [
    join(llvmPrefix, "usr/lib", libdir),
    join(llvmPrefix, "usr/lib/llvm-21/lib"),
    env.LD_LIBRARY_PATH,
  ].filter(Boolean).join(":");
}

// Keep profiles separate and bound the outer Ninja and the nested CMake and cargo builds.
delete env.CI;
delete env.BUILDKITE;
delete env.GITHUB_ACTIONS;
env.CMAKE_BUILD_PARALLEL_LEVEL = String(JOBS);
env.CARGO_BUILD_JOBS = String(JOBS);
env.BUN_WEBKIT_PATH = engineDir;
const lockDir = resolve(process.env.XDG_CACHE_HOME || join(home, ".cache"), "jitcache");
mkdirSync(lockDir, { recursive: true });

// flock waits for any other build and retains the lock while the builder runs.
const result = spawnSync("flock", [
  "--exclusive", "--no-fork", join(lockDir, "build.lock"),
  process.execPath, builder,
  `--profile=${target.profile}`,
  ...(cross ? [`--arch=${architecture}`, `--linux-sysroot=${sysroot}`] : []),
  "--webkit=local",
  ...(target.ninja ? [`--target=${target.ninja}`] : []),
  `-j${JOBS}`,
  `--build-dir=${buildDir}`,
], { cwd: bunSource, env, stdio: "inherit" });
if (result.error) fail(`Cannot launch Bun builder: ${result.error.message}`);
process.exit(result.status ?? (result.signal ? 128 + constants.signals[result.signal] : 1));
