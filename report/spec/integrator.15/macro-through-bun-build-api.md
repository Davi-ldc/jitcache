mechanical

## Requirement

SPEC-integrator.md section 15.3, for `~/bun/test/js/bun/jitcache/jitcache.test.ts`: "with `--jitcache-bench-report`, the main VM writes its report at the configured path and each worker at that path suffixed with its id, and a macro run during `bun build` writes none". The property behind it is in section 11.2 and N15: `configureVM` leaves unconfigured a VM whose execution context id is `INT32_MAX`, as a macro VM's is.

## Why the code cannot meet it as written

`bun build` never reads the JITCache flags. It parses its command line with `BUILD_TABLE`, built from `BUILD_PARAMS` (`BUILD_ONLY_PARAMS`, `TRANSPILER_PARAMS_` and `BASE_PARAMS_`), which leaves out `RUNTIME_PARAMS_`, where section 11.2 declares the flags. Bun's clap skips a long flag it does not know and goes on parsing, and `Arguments::parse` calls `parse_jitcache_flags` only for the Auto, Run, Test and RunAsNode commands. In a `bun build` process `Bun__JITCache__flags()` therefore holds no artifact path, and `configureVM` returns at `artifactPath.isEmpty()` for every VM. Such a run writes no report whatever `kindOf` makes of the macro VM's id, so a test built on `bun build` passes even when `configureVM` configures the macro VM.

## What the code does instead

The test runs `bun --jitcache=<artifact> --jitcache-mode=p --jitcache-strict --jitcache-bench-report=<reports>/report.jsonl macro-build-fixture.js`, whose `Bun.build` bundles the same entry with the same bundler. The bundler parses on the threads of its worker pool, where `VirtualMachine::is_loaded()` is false, so `Macro::init` creates the macro's VM on that thread with `is_main_thread: false` and no context id, which `VirtualMachine::init` turns into `INT32_MAX`. `configureVM` sees that id in a process whose flags Bun read. The test requires no warning, a bundle that prints 42, which the macro returns only while `Bun.isMainThread` is false, that is, in a VM other than the main thread's, and a reports directory holding `report.jsonl` alone, whose only `start` event is the main VM's (`started`, `producer`). A macro VM taken for a worker's would write `report.jsonl.<id>`, and one taken for the main VM's would meet the main VM's producer lock, warn and add a busy `start` event.

Other tasks need do nothing. Section 15.3 should say that the macro runs in the bundler through `Bun.build`, in a process given the flags.

## Evidence

- `BUILD_PARAMS`, `BUILD_TABLE`, `RUNTIME_PARAMS_`, `parse_jitcache_flags` and the runtime-commands block of `Arguments::parse` that calls it (`src/runtime/cli/Arguments.rs`).
- The unrecognized-flag branch of `StreamingClap::next` (`src/clap/streaming.rs`).
- `Macro::init` (`src/js_parser_jsc/Macro.rs`); `VirtualMachine::is_loaded`, the thread-local `VM` and the context-id default of `VirtualMachine::init` (`src/jsc/VirtualMachine.rs`).
- `ThreadPool::schedule_with_options` (`src/bundler/ThreadPool.rs`), which hands every parse task to the worker pool.
- `JITCacheHostInternal::kindOf` and `JITCacheHost::configureVM` (`src/jsc/bindings/JITCacheHost.cpp`).
- `ScriptExecutionContext::isMainThread` (`src/jsc/bindings/ScriptExecutionContext.h`), which `Bun.isMainThread` reads.
- The test "a macro the bundler runs writes no bench report", `macro-build-fixture.js` and `macro-fixture.ts` (`test/js/bun/jitcache/`).
