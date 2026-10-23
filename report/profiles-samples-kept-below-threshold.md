# profiles.md: collections leave the samples of cold interpreted code in their buckets

Doc: `skills/reference/knowledge/common/profiles.md`, section "Drain", fourth paragraph (the four contexts), and section "Stale samples".

## Passages

> The third is GC marking: on the first visit to a `CodeBlock` in LLInt or baseline (`CodeBlock::visitChildren`, only through the `SlotVisitor`), the marker drains the value profiles on its own thread, even with the world running. The fourth is the finalization of that same cycle, which drains everything (`CodeBlock::reconcileWeakReferencesAtGCEnd`) [...]

## What changed

With `useLazyValueProfilePredictions` (default on), the marker's visit drains with `ValueProfileSamples::Keep` and the finalizer with `KeepIfLive` (`CodeBlock::ValueProfileSamples`; `CodeBlock::visitChildren`, `reconcileWeakReferencesAtGCEnd`). `updateAllNonLazyValueProfilePredictionsAndCountLiveness` turns either back into a full drain (`Record`) unless `CodeBlock::keepsValueProfileSamplesInBuckets` holds:

- the metadata table has no predictions array yet, and
- either the CB is still in the LLInt and its body's LLInt count (`llintExecuteCounter().count()`) is below `thresholdForValueProfilePredictions` (default 40, in tier-up points: 5 per call, 10 per return, 1 per loop iteration), or the code is never read by an optimizing tier (separate report).

When it holds, the metadata value profiles keep their samples: no classification, no merge into the UCB copies. Under `KeepIfLive`, the finalizer clears a bucket whose sample is a cell the collection did not mark, because nothing marks it and it would otherwise dangle. Argument profiles and `op_catch` buffers are always drained, and the array and array allocation profiles still drain as before. The comment on `keepsValueProfileSamplesInBuckets` gives the reason: the predictions cost as much memory as the buckets, and code that ran a few times may never run again.

This also narrows "Stale samples": a kept sample can no longer outlive its cell through a finalizer that visits the CB.

## Proposed text

Replace the two sentences with:

> The third is GC marking: on the first visit to a `CodeBlock` in LLInt or baseline (`CodeBlock::visitChildren`, only through the `SlotVisitor`), the marker drains the value profiles on its own thread, even with the world running. The fourth is the finalization of that same cycle, which drains everything (`CodeBlock::reconcileWeakReferencesAtGCEnd`) [rest unchanged]. Both GC drains skip the metadata value profiles of a CB still in the LLInt whose body has counted fewer than `thresholdForValueProfilePredictions` (40) points, as long as its table has no predictions yet (`keepsValueProfileSamplesInBuckets`): the samples stay in their buckets, and the finalizer only clears one whose cell died.

In "Stale samples", after "The next drain that reaches the `CodeBlock` reads those stale samples", add: "A sample kept in its bucket by the rule above is cleared by any finalizer that visits the `CodeBlock` once its cell dies."
