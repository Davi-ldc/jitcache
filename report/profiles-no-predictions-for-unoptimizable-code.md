# profiles.md: code the optimizing tiers never compile gets no value predictions

Doc: `skills/reference/knowledge/common/profiles.md`, section "Drain", third paragraph (the group update).

## Passage

> `updateAllNonLazyValueProfilePredictionsAndCountLiveness` walks the argument profiles, then the metadata ones (`CodeBlock::forEachValueProfile`), draining each one and merging its prediction with the one kept in the `UnlinkedCodeBlock` [...]

## What changed

`CodeBlock::valueProfilePredictionsAreNeverRead` is true when `useDFGJIT` is off or the CB's `bytecodeCost()` exceeds `maximumOptimizationCandidateBytecodeCost`, since only the optimizing compilers read predictions and they never see such code. For such a CB whose table has no predictions array yet, `updateAllNonLazyValueProfilePredictionsAndCountLiveness` turns every drain into `ValueProfileSamples::Keep`, including the default `Record` drain of the baseline plan (`BaselineJITPlan::compileInThreadImpl` through `updatePredictionsConcurrently`): the metadata buckets keep their last sample, no predictions array is allocated, and nothing merges into the UCB's value copies. `keepsValueProfileSamplesInBuckets` returns true for it in the GC drains too. Requires `useLazyValueProfilePredictions` (default on).

## Proposed text

Add after the passage's sentence:

> With `useLazyValueProfilePredictions`, the metadata profiles of code the DFG would never compile (DFG off, or a bytecode cost above `maximumOptimizationCandidateBytecodeCost`: `valueProfilePredictionsAreNeverRead`) are skipped by every drain until something else allocates the table's predictions: their buckets keep the last sample, and nothing merges into the `UnlinkedCodeBlock`.
