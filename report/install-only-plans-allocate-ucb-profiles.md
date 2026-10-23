# baseline/install.md: only a compilation allocates the UCB's value and array copies

Doc: `skills/reference/knowledge/baseline/install.md`, section "Doors", "Door 1" first paragraph and the doors table; or the step 1 sentence about the marker's drain.

## Passage

> 1. `setJITCode` [...] From then on `jitType()` answers `BaselineJIT`, and the concurrent marker's first visit to the `CodeBlock` in each cycle drains its value profiles, as for any LLInt or baseline `CodeBlock`.

## What changed

`UnlinkedCodeBlock::ensureValueAndArrayProfiles` runs only in the `BaselineJITPlan` constructor, on the VM thread. That covers door 1 and the embedder route through `JIT::compileSync`, which builds a plan. Setup itself never calls it, so doors 2 and 3 allocate nothing; the parked artifact they install was compiled by a plan for the same `UnlinkedCodeBlock`, so its copies exist already. Every drain merges into the copies only when they exist (`updateAllNonLazyValueProfilePredictionsAndCountLiveness`, `updateAllArrayProfilePredictions`).

Also, the marker's drain now covers the array profiles too (`updatePredictionsConcurrently`), and with `useLazyValueProfilePredictions` it can leave the value samples in their buckets (profiles.md has those reports). For a CB in baseline the bucket-keeping rule applies only to code the DFG never compiles, since `keepsValueProfileSamplesInBuckets` requires an interpreted CB otherwise.

## Proposed text

Door 1, end of the second paragraph, add:

> The plan's constructor, on the requesting thread, also allocates the `UnlinkedCodeBlock`'s value and array profile copies on the body's first compilation (`ensureValueAndArrayProfiles`); doors 2 and 3 find them allocated by the plan that compiled the parked artifact.

Step 1: replace "drains its value profiles" with "drains its value and array profiles".
