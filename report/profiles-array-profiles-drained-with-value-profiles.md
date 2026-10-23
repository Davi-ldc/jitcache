# profiles.md: the baseline plan and the GC marker also drain the array profiles

Doc: `skills/reference/knowledge/common/profiles.md`: section "Drain", fourth paragraph, and the `UnlinkedArrayProfile` row of the table.

## Passages

> The two also differ in scope: the LLInt→baseline one drains only the value profiles, lazy ones included, while the baseline→DFG one also drains the array and array allocation profiles. The third is GC marking: [...] the marker drains the value profiles on its own thread, even with the world running.

Row `UnlinkedArrayProfile`: "the merge does not run at the LLInt→baseline tier-up, only at GC finalization and at the tier-up to the DFG".

## What changed

`CodeBlock::updatePredictionsConcurrently` now runs `updateAllNonLazyValueProfilePredictions`, `updateAllLazyValueProfilePredictions` and `updateAllArrayProfilePredictions`. Both the baseline plan (`BaselineJITPlan::compileInThreadImpl`) and the marker's first visit (`CodeBlock::visitChildren`, `SlotVisitor` only) call it. So both drain the array profiles too, on their own threads, and merge them into the UCB's `UnlinkedArrayProfile`s when those exist (`ensureValueAndArrayProfiles`, which the plan's constructor calls). Only the array allocation profiles remain specific to the baseline→DFG drain and to finalization (`updateAllPredictions` adds `updateAllArrayAllocationProfilePredictions`, which reads butterflies and runs only on the thread that executes JS or in finalization).

## Proposed text

Drain, fourth paragraph:

> The two also differ in scope: the LLInt→baseline one drains the value profiles, lazy ones included, and the array profiles, while the baseline→DFG one also drains the array allocation profiles. The third is GC marking: on the first visit to a `CodeBlock` in LLInt or baseline (`CodeBlock::visitChildren`, only through the `SlotVisitor`), the marker drains the value and array profiles on its own thread (`updatePredictionsConcurrently`), even with the world running.

Row `UnlinkedArrayProfile`: "the merge runs wherever the array profiles drain: the baseline plan, the GC marker's first visit, GC finalization and the tier-up to the DFG".
