#!/usr/bin/env bun
import { spawnSync } from "node:child_process";
import { mkdirSync, realpathSync, statSync } from "node:fs";
import { availableParallelism, constants, homedir, totalmem } from "node:os";
import { join, resolve } from "node:path";

const BUN_PIN = "744846f844374847c902b5e7fd59b4342a51ef99";
// The webkitbun revision the Bun pin declares (skills/SKILL.md), which the pin comparison builds.
const WEBKIT_PIN = "2e2aa2290fac856d6f451ceacb58f7f5b44dd057";
// Each target selects a Bun profile and a ninja target; without one, Bun builds its
// default targets, the executable and its smoke test. Targets of one profile pass
// identical config flags, so they share build.ninja, the build directory and its WebKit.
// The twins profile is debug-local with JITCache's test builds on (ENABLE_JITCACHE_TWINS):
// its WebKit target builds jsc and testjitcache. The pin target builds debug's jsc from a
// worktree of this repository at WEBKIT_PIN, in a build directory of its own
// (SPEC-integrator.harness.md section 11.1).
const TARGETS = new Map<string, { profile: string; ninja?: string; pin?: true }>([
  ["debug", { profile: "debug-local", ninja: "WebKit" }],
  ["release", { profile: "release-local", ninja: "WebKit" }],
  ["ci-release", { profile: "ci-release", ninja: "WebKit" }],
  ["bun-debug", { profile: "debug-local" }],
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
  console.log("Usage: bun build.ts [debug|release|ci-release|bun-debug|twins|bun-twins|pin] [--arch=aarch64] [--jobs=N] [--keep-going]");
  console.log("Build WebKit/JSC with Bun's build system, from a Bun checkout based on the pin. Default: debug.");
  console.log("bun-debug builds the Bun executable against local WebKit in the debug build directory.");
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
const buildDir = join(buildRoot, `linux-${architecture}-${target.profile}${target.pin ? "-pin" : ""}`);
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
env.BUN_WEBKIT_PATH = webkitSource;
const lockDir = resolve(process.env.XDG_CACHE_HOME || join(home, ".cache"), "jitcache");
mkdirSync(lockDir, { recursive: true });
const lock = ["--exclusive", "--no-fork", join(lockDir, "build.lock")];
const exitCode = (run: ReturnType<typeof spawnSync>) =>
  run.status ?? (run.signal ? 128 + constants.signals[run.signal] : 1);

// flock waits for any other build and retains the lock while the builder runs.
const result = spawnSync("flock", [
  ...lock,
  process.execPath, builder,
  `--profile=${target.profile}`,
  ...(cross ? [`--arch=${architecture}`, `--linux-sysroot=${sysroot}`] : []),
  "--webkit=local",
  ...(target.ninja ? [`--target=${target.ninja}`] : []),
  `-j${JOBS}`,
  ...(keepGoing ? ["-k0"] : []),
  `--build-dir=${buildDir}`,
], { cwd: bunSource, env, stdio: "inherit" });
if (result.error) fail(`Cannot launch Bun builder: ${result.error.message}`);
let status = exitCode(result);

// The builder runs the nested WebKit build through `cmake --build`, whose Ninja stops at its first
// failure; the outer -k0 does not reach it. After a failure, --keep-going runs that Ninja again with
// -k 0, so one build reports every compile error.
const nested = join(buildDir, "deps/WebKit");
if (status !== 0 && keepGoing && statSync(join(nested, "build.ninja"), { throwIfNoEntry: false })?.isFile()) {
  const nestedTargets = target.profile.endsWith("-twins") ? ["jsc", "testjitcache"] : ["jsc"];
  const rerun = spawnSync("flock", [...lock, "ninja", "-C", nested, "-k", "0", `-j${JOBS}`, ...nestedTargets],
    { env, stdio: "inherit" });
  if (rerun.error) fail(`Cannot launch Ninja: ${rerun.error.message}`);
  status = exitCode(rerun) || status;
}
process.exit(status);
