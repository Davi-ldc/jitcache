# baseline/install.md: setup first materializes the CodeBlock's lazy state

Doc: `skills/reference/knowledge/baseline/install.md`, section "Installation", before step 1, and step 3's constant pool sentence.

## Passages

> 1. `setJITCode` stores a reference to the `BaselineJITCode` in the `CodeBlock`'s `m_jitCode`, under `m_lock`. [...]

> [...] and each pool slot receives the `FunctionExecutable` that `finishCreation` linked at the recorded declaration or expression index.

## What changed

`CodeBlock::setupWithUnlinkedBaselineCode` now begins with `prepareLazyStateForConcurrentCompilation()`, commented "shared baseline code can be installed on a block that only ever ran in the LLInt". On a `CodeBlock` not yet prepared, `prepareLazyStateForConcurrentCompilationSlow` runs under `DeferGCForAWhile` on the VM thread. It creates every `FunctionExecutable` the CB has not materialized (`ensureFunctionExecutablesMaterialized`; with `useLazyFunctionExecutables`, `finishCreation` leaves them null). For function code it also decodes the UFE's deferred scalars (`UnlinkedFunctionExecutable::materializeDeferredScalarsIfNeeded`). Then it sets `m_isLazyStatePreparedForConcurrentCompilation` behind a `storeStoreFence`. It allocates GC cells, so it is the one step of setup that allocates in the heap. At door 1 the `JITPlan` constructor has already done it (no-op here); at doors 2 and 3, and for a CB that only ran in the LLInt, it runs here.

Step 3 fills each pool slot through `CodeBlock::functionDecl(index)` and `functionExpr(index)`, which would materialize an entry on demand but find it already created. A module's heap-allocated declarations never get one (`firstLazilyMaterializedFunctionDecl`), and the bytecode never names them in a `new_func*`.

## Proposed text

Before step 1, add:

> 0. `prepareLazyStateForConcurrentCompilation` finishes what linking left lazy, once per `CodeBlock`: the nested functions' `FunctionExecutable`s the CB has not created yet and, for function code, the UFE's members still in the bytecode payload. It allocates cells, under `DeferGCForAWhile`. At door 1 the plan's constructor already did it; at doors 2 and 3 it happens here, since the artifact may reach a `CodeBlock` that never compiled.

Step 3: replace "the `FunctionExecutable` that `finishCreation` linked" with "the `CodeBlock`'s `FunctionExecutable`".
