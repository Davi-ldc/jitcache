#!/usr/bin/env bun
import { spawnSync } from "node:child_process";
import { realpathSync, statSync } from "node:fs";
import { availableParallelism, constants, homedir, totalmem } from "node:os";
import { join, resolve } from "node:path";
import { holdMachineLock } from "./Tools/Scripts/jitcache/lock.ts";

const BUN_PIN = "c6da4a4d3010e5553438c60f6bd76d981976867c";
// The webkitbun revision the Bun pin declares (skills/SKILL.md), which the pin comparison builds.
const WEBKIT_PIN = "0c06faadf65bf8e8c8ad3a5a8aca83e1e9ed653f";
// The LLVM major Bun's builder accepts at BUN_PIN, installed in the package layout under JITCACHE_LLVM_PREFIX.
const LLVM_MAJOR = 23;
// Each target selects a Bun profile and a ninja target; without one, Bun builds its
// default targets, the executable and its smoke test. Targets of one profile pass
// identical config flags, so they share build.ninja, the build directory and its WebKit.
// The twins profile is debug-local with JITCache's test builds on (ENABLE_JITCACHE_TWINS):
// its WebKit target builds jsc and testjitcache. The pin target builds debug's jsc from a
// worktree of this repository at WEBKIT_PIN, in a build directory of its own.
// Bun retired its ci-release profile for the release profile with --ci=on --buildkite=on, which set LTO and Bun's target
// settings; the ci targets pass those flags and keep the build directory they had.
const CI_FLAGS = ["--ci=on", "--buildkite=on"];
const TARGETS = new Map<string, { profile: string; ninja?: string; pin?: true; flags?: string[]; dir?: string }>([
  ["debug", { profile: "debug-local", ninja: "WebKit" }],
  ["release", { profile: "release-local", ninja: "WebKit" }],
  ["ci-release", { profile: "release", ninja: "WebKit", flags: CI_FLAGS, dir: "ci-release" }],
  ["bun-debug", { profile: "debug-local" }],
  ["bun-release", { profile: "release-local" }],
  ["bun-ci-release", { profile: "release", flags: CI_FLAGS, dir: "ci-release" }],
  ["twins", { profile: "debug-local-twins", ninja: "WebKit" }],
  ["bun-twins", { profile: "debug-local-twins" }],
  ["pin", { profile: "debug-local", ninja: "WebKit", pin: true }],
]);

function fail(message: string, code = 125): never {
  console.error(message);
  process.exit(code);
}

const args = process.argv.slice(2);
const archOption = args.find(arg => arg.startsWith("--arch="));
const jobsOption = args.find(arg => arg.startsWith("--jobs="));
const keepGoing = args.includes("--keep-going");
const positional = args.filter(arg => arg !== archOption && arg !== jobsOption && arg !== "--keep-going");
const requested = positional[0] || "debug";
if (requested === "-h" || requested === "--help") {
  console.log("Usage: bun build.ts [debug|release|ci-release|bun-debug|bun-release|bun-ci-release|twins|bun-twins|pin] [--arch=aarch64] [--jobs=N] [--keep-going]");
  console.log("Build WebKit/JSC with Bun's build system, from a Bun checkout based on the pin. Default: debug.");
  console.log("bun-debug builds the Bun executable against local WebKit in the debug build directory;");
  console.log("bun-release does the same in the release build directory, for the macro benches, and");
  console.log("bun-ci-release in the ci-release one, the build that ships.");
  console.log("twins builds jsc and testjitcache with JITCache's test builds on (ENABLE_JITCACHE_TWINS);");
  console.log("bun-twins builds the Bun executable against that WebKit, in the same build directory.");
  console.log(`pin builds debug's jsc from the pinned webkitbun ${WEBKIT_PIN.slice(0, 12)} in JITCACHE_PIN_SOURCE`);
  console.log("(a detached worktree, <build-root>/webkit-pin by default) into linux-<arch>-debug-local-pin.");
  console.log("--arch=aarch64 cross-compiles on an x86_64 host against the sysroot in JITCACHE_AARCH64_SYSROOT.");
  console.log("--jobs=N sets the jobs of each level; the default is as many as memory holds, at most one per CPU.");
  console.log("--keep-going reports every compile error in one build instead of stopping at the first.");
  process.exit(0);
}
// A debug translation unit with ASan takes up to about 3 GiB, so the default runs as many jobs as
// memory holds, at most one per CPU.
const JOBS = jobsOption
  ? Number(jobsOption.slice("--jobs=".length))
  : Math.max(1, Math.min(availableParallelism(), Math.floor(totalmem() / (3 * 2 ** 30))));
if (!Number.isInteger(JOBS) || JOBS < 1) fail(`Invalid job count: ${jobsOption}`, 2);
const target = TARGETS.get(requested);
if (!target) fail(`Unknown target: ${requested}`, 2);
if (positional.length > 1) fail("Expected at most one target argument.", 2);

const home = homedir();
const engineDir = realpathSync(import.meta.dirname);
const bunSource = resolve(process.env.JITCACHE_BUN_SOURCE || join(home, "bun"));
const buildRoot = resolve(process.env.JITCACHE_BUILD_ROOT || join(home, "collo-local/build/jitcache"));
const llvmPrefix = resolve(process.env.JITCACHE_LLVM_PREFIX || join(home, `collo-local/tools/llvm-${LLVM_MAJOR}`));
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
// The pin comparison's engine is webkitbun at WEBKIT_PIN exactly: its source is a detached worktree at that commit
// with no change to a tracked file, and its build directory is its own.
let webkitSource = engineDir;
if (target.pin) {
  const pinSource = resolve(process.env.JITCACHE_PIN_SOURCE || join(buildRoot, "webkit-pin"));
  const create = `Create it with: git -C ${engineDir} worktree add --detach ${pinSource} ${WEBKIT_PIN}`;
  if (!statSync(pinSource, { throwIfNoEntry: false })?.isDirectory()) fail(`The pin's source is missing: ${pinSource}\n${create}`);
  const pinGit = (...gitArgs: string[]) => spawnSync("git", ["-C", pinSource, ...gitArgs], { encoding: "utf8" });
  const head = pinGit("rev-parse", "--verify", "HEAD");
  if (head.error || head.status !== 0) fail(`Cannot read the HEAD of ${pinSource}: ${head.error?.message ?? head.stderr.trim()}\n${create}`);
  if (head.stdout.trim() !== WEBKIT_PIN) fail(`${pinSource} is at ${head.stdout.trim()}, not at the pin ${WEBKIT_PIN}.`);
  const changes = pinGit("diff", "--quiet", "HEAD");
  if (changes.error) fail(`Cannot check ${pinSource} for changes: ${changes.error.message}`);
  if (changes.status === 1) fail(`${pinSource} changes tracked files; the pin comparison needs the pin exactly.`);
  if (changes.status !== 0) fail(`Cannot check ${pinSource} for changes: ${changes.stderr.trim() || `git exited with status ${changes.status}`}`);
  webkitSource = realpathSync(pinSource);
}
const buildDir = join(buildRoot, `linux-${architecture}-${target.dir ?? target.profile}${target.pin ? "-pin" : ""}`);
// The build holds the machine lock from here until it exits, waiting while another build, test or bench holds it.
let lock: number | null = null;
try {
  lock = holdMachineLock();
} catch (error) {
  fail((error as Error).message);
}
const env = { ...process.env };
env.PATH = `${join(home, "collo-local/tools/bin")}:${env.PATH || ""}`;

// Bun's builder still validates the selected compiler version.
const llvmBin = join(llvmPrefix, `usr/lib/llvm-${LLVM_MAJOR}/bin`);
if (statSync(llvmBin, { throwIfNoEntry: false })?.isDirectory()) {
  const libdir = hostArchitecture === "x86_64" ? "x86_64-linux-gnu" : "aarch64-linux-gnu";
  env.PATH = `${llvmBin}:${env.PATH}`;
  env.LD_LIBRARY_PATH = [
    join(llvmPrefix, "usr/lib", libdir),
    join(llvmPrefix, `usr/lib/llvm-${LLVM_MAJOR}/lib`),
    env.LD_LIBRARY_PATH,
  ].filter(Boolean).join(":");
}

// Keep profiles separate and bound the outer Ninja and the nested CMake and cargo builds. Without these variables the
// builder stays on its local path, with no packaging, uploads or inherited symbol order file, even where the ci
// targets' flags set the build's semantics.
delete env.CI;
delete env.BUILDKITE;
delete env.GITHUB_ACTIONS;
env.CMAKE_BUILD_PARALLEL_LEVEL = String(JOBS);
env.CARGO_BUILD_JOBS = String(JOBS);
env.BUN_WEBKIT_PATH = webkitSource;
const exitCode = (run: ReturnType<typeof spawnSync>) =>
  run.status ?? (run.signal ? 128 + constants.signals[run.signal] : 1);

const builderArgs = [
  builder,
  `--profile=${target.profile}`,
  ...(target.flags ?? []),
  ...(cross ? [`--arch=${architecture}`, `--linux-sysroot=${sysroot}`] : []),
  "--webkit=local",
  ...(target.ninja ? [`--target=${target.ninja}`] : []),
  `-j${JOBS}`,
  ...(keepGoing ? ["-k0"] : []),
  `--build-dir=${buildDir}`,
];

// The builder and the Ninja rerun receive the locked descriptor as their descriptor 3, so each holds the machine lock
// while it runs, as the builder did when flock started it, and a build that outlives this process still holds it.
const stdio: "inherit" | ("inherit" | number)[] = lock === null ? "inherit" : ["inherit", "inherit", "inherit", lock];
const result = spawnSync(process.execPath, builderArgs, { cwd: bunSource, env, stdio });
if (result.error) fail(`Cannot launch Bun builder: ${result.error.message}`);
let status = exitCode(result);

// The builder runs the nested WebKit build through `cmake --build`, whose Ninja stops at its first
// failure; the outer -k0 does not reach it. After a failure, --keep-going runs that Ninja again with
// -k 0, so one build reports every compile error.
const nested = join(buildDir, "deps/WebKit");
if (status !== 0 && keepGoing && statSync(join(nested, "build.ninja"), { throwIfNoEntry: false })?.isFile()) {
  const nestedTargets = target.profile.endsWith("-twins") ? ["jsc", "testjitcache"] : ["jsc"];
  const rerun = spawnSync("ninja", ["-C", nested, "-k", "0", `-j${JOBS}`, ...nestedTargets], { env, stdio });
  if (rerun.error) fail(`Cannot launch Ninja: ${rerun.error.message}`);
  status = exitCode(rerun) || status;
}
process.exit(status);
