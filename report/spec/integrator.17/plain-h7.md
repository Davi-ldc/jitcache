mechanical

## Requirement

SPEC-integrator.harness.md section 13, H7: "With `--pin` naming this build's own directory, the runner compares this build with itself on the runner's fixtures and the integrator corpus and finds no difference, in plain and twins builds on each architecture".

## Why the code cannot meet it as written

The runner's fixtures and the integrator corpus are in `JSTests/jitcache/integrator/`, and section 7.4 makes every script there require twins. In a plain build, or with `--mode=plain`, the runner therefore skips and lists each one, so the plain half of H7 compares nothing.

## What the code does instead

The twins half runs through the runner, as the `pin-self` and `pin-self-forced-blinding` cases of `self-test.ts`.

The plain half is the `pin-self-plain` case, which needs `--plain-build=<plain WebKit build directory>`. For every Off option set of those same scripts, it calls `comparePinOptionSet`, the function the runner's `--pin` calls. It uses the plain build on both sides and the plain-mode command lines that `jscArguments` builds with twins off. Without `--plain-build` the case prints SKIP.

This asks something of the Verify phase. On each architecture, it must give `self-test.ts` the `debug-local` build as `--plain-build`, beside the twins build as `--build`.

## Evidence

- `laneDefaults`, `laneOf` and the skip in `runScripts` (`Tools/Scripts/run-jitcache-tests`).
- `pinSelfPlainCase` (`JSTests/jitcache/integrator/runner/self-test.ts`).
