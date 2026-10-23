# basics.md: collections leave the samples of cold interpreted code in their buckets

Doc: `skills/reference/basics.md`, section "GC", fourth paragraph.

## Passage

> During marking, the `SlotVisitor`'s first visit to a `CodeBlock` in LLInt or baseline already drains the value profiles, even with the world running (`CodeBlock::visitChildren`). [...] In LLInt and baseline, that finalizer redoes the value profile drain and also covers the array and array allocation profiles, classifying the samples and emptying the buckets even when they are still alive;

## What changed

With `useLazyValueProfilePredictions` (default on), the visit drains with `ValueProfileSamples::Keep` and the finalizer (`CodeBlock::reconcileWeakReferencesAtGCEnd`) with `KeepIfLive`. `updateAllNonLazyValueProfilePredictionsAndCountLiveness` turns either into a normal drain (`Record`) unless `CodeBlock::keepsValueProfileSamplesInBuckets` holds. That holds when the metadata table has no predictions array yet and either the optimizing tiers never read this code (`valueProfilePredictionsAreNeverRead`: DFG off, or a bytecode cost above `maximumOptimizationCandidateBytecodeCost`) or the CB is still interpreted and its body's LLInt count is below `thresholdForValueProfilePredictions` (40). In that case the metadata value profiles keep their sample, and the finalizer only clears a sample whose cell died, since nothing marks it. Argument profiles and `op_catch` buffers are always drained.

## Proposed text

Replace "In LLInt and baseline, that finalizer redoes the value profile drain [...] even when they are still alive;" with:

> In LLInt and baseline, that finalizer redoes the value profile drain and also covers the array and array allocation profiles, classifying the samples and emptying the buckets even when they are still alive. Both drains leave the samples of a CB still in the LLInt below `thresholdForValueProfilePredictions` (40 points), or of code the DFG would never compile, in their buckets, and the finalizer only clears a sample whose cell died (`keepsValueProfileSamplesInBuckets`);

Net size change is about +250 bytes; the doc is at 39,973, so condense elsewhere first. `profiles.md` covers the same mechanism in depth (separate report).
