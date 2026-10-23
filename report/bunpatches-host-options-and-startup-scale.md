# bunpatches.md: two more host-set options, and the startup JIT deferral scale an executable can bake in

Doc: `skills/reference/knowledge/common/bunpatches.md`, section "Options set by the host", first paragraph.

## Passage

> Fixed values come first, such as `useSharedArrayBuffer`, `useShadowRealm`, `useJITCage=false` and `useWasmMemory64=false`. Values that depend on the command follow: [...] Last, each `BUN_JSC_<option>` environment variable overrides any option, restricted ones included.

## What changed

In `JSCInitialize` (`~/bun/src/jsc/bindings/ZigGlobalObject.cpp`):

- Among the fixed values, `defaultErrorStackTraceLimit` is set to `DEFAULT_ERROR_STACK_TRACE_LIMIT` (10, Node's default for `Error.stackTraceLimit`).
- After the `BUN_JSC_*` loop, `useDollarVM` is forced off when code generation from strings is not allowed (`Bun::codeGenerationFromStrings() != Allowed`), so `BUN_JSC_useDollarVM` cannot bring it back: `$vm` evaluates strings and creates global objects that are not Bun's.

A third heuristic is VM state, not an option: the startup JIT deferral scale (`VM::setStartupJITDeferralScale`, basics.md has the report). A standalone executable built with `--compile-jit-policy <n>` (n ≥ 1, `--compile` only) stores it in its module graph's runtime options, and `run_command.rs` applies it to the main VM at start (`graph.runtime_options.jit_policy`). Code can change it with `Bun.unsafe.setJITPolicy` (`src/runtime/api/UnsafeObject.rs`). Both go through `JSC__VM__setStartupJITDeferralScale`. The engine option `startupJITDeferralScale`, which seeds every VM, stays at 1 unless `BUN_JSC_startupJITDeferralScale` sets it. Since this is per-VM state set after the options are frozen, the same options can tier up at different points.

## Proposed text

Fixed values: add "`defaultErrorStackTraceLimit=10`" to the list. After "restricted ones included", add: "One override follows the loop: when code generation from strings is disallowed, `useDollarVM` goes off whatever `BUN_JSC_useDollarVM` says." At the end of the paragraph, add:

> One tier-up heuristic is per-VM state rather than an option: the startup deferral scale ([basics.md](../../basics.md#tier-up)), which a `bun build --compile --compile-jit-policy <n>` executable applies to its main VM at start and which `Bun.unsafe.setJITPolicy` changes at run time (`JSC__VM__setStartupJITDeferralScale`).
