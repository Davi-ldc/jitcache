# baseline/compiler.md: the plan allocates the UCB's profile copies and prepares lazy state; its drain covers array profiles

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "Trigger and threads", third paragraph.

## Passage

> The compilation (`BaselineJITPlan::compileInThreadImpl`) first drains the value profiles and, except in builtins, merges their predictions into the `UnlinkedCodeBlock`'s copies; then it opens one `Safepoint` that lasts until the artifact exists (`JIT::compileAndLinkWithoutFinalizing`).

## What changed

- On the VM thread, the `JITPlan` constructor calls `baselineAlternative()->prepareLazyStateForConcurrentCompilation()`, which creates the `CodeBlock`'s not-yet-materialized `FunctionExecutable`s and the UFE's deferred scalars before a compiler thread can read them. The `BaselineJITPlan` constructor then calls `UnlinkedCodeBlock::ensureValueAndArrayProfiles`, which allocates the UCB's value and array copies on the body's first baseline compilation (never for builtins).
- `compileInThreadImpl` calls `CodeBlock::updatePredictionsConcurrently`, which drains the value profiles, lazy ones included, and the array profiles, merging both into those copies. For code the optimizing tiers never compile (`valueProfilePredictionsAreNeverRead`), with no predictions array yet, the metadata value profiles keep their samples and merge nothing (`updateAllNonLazyValueProfilePredictionsAndCountLiveness`).

## Proposed text

> The plan is built on the requesting thread, which materializes the `CodeBlock`'s lazy state (`prepareLazyStateForConcurrentCompilation`, in the `JITPlan` constructor) and, on the body's first baseline compilation, allocates the `UnlinkedCodeBlock`'s value and array profile copies (`ensureValueAndArrayProfiles`). The compilation (`BaselineJITPlan::compileInThreadImpl`) first drains the value and array profiles and, except in builtins, merges them into those copies (`updatePredictionsConcurrently`); then it opens one `Safepoint` that lasts until the artifact exists (`JIT::compileAndLinkWithoutFinalizing`).

Net size change is about +330 bytes; the doc is at 39,109 bytes.
