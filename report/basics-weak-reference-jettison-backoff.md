# basics.md: weak-reference jettisons count toward the reoptimization back-off

Doc: `skills/reference/basics.md`, section "Jettison", fourth paragraph.

## Passage

> Speculation failures (too many exits, watchpoint, failed OSR entry) raise the future threshold: they increment the reoptimization counter and clear that tier's quick tier-up bit in the `UnlinkedCodeBlock`, which records that the body has already proven worth the tier-up.

## What changed

`ScriptExecutable::jettisonCodeBlockEdgeIfDead` (`ScriptExecutableInlines.h`) jettisons optimized code whose weak reference died with `CountReoptimization` while the baseline alternative's `reoptimizationRetryCounter()` is below `Options::weakReferenceJettisonReoptimizationLimit()` (default 4; 0 never counts), and with `DontCountReoptimization` after that. `CodeBlock::jettison` then calls `baselineAlternative()->countReoptimization()`, which doubles the next thresholds like any other counted jettison. The quick tier-up bits are untouched, because `Profiler::isSpeculationFailure` is false for `JettisonDueToWeakReference`, so `didDFGJettison` and `didFTLJettison` leave them set.

## Proposed text

Append to the passage:

> A jettison for a dead weak reference also increments the counter, but only while it is below `weakReferenceJettisonReoptimizationLimit` (4), and leaves the bit set.

Net size change is about +150 bytes; the doc is at 39,973, so condense elsewhere first.
