# bunpatches.md: aging refreshes the counter snapshot on every look, and aged-out code takes the old-age jettison

Doc: `skills/reference/knowledge/common/bunpatches.md`, section "Code aging (PR #557, `CodeBlock::shouldJettisonDueToOldAge`)", both paragraphs.

## Passages

> So, under `USE(BUN_JSC_ADDITIONS)` and with `useExecutionCountForCodeBlockAging` on, once the protection has run out the GC reads the tier's counter during marking [...] and compares it with `m_previousCounter`. If it changed (a loop iteration also counts), the GC updates the sample and sets `m_creationTime` to `now + 2 × TTL` [...]

> These cases cannot age through this system, so they can only die of old age in a collection that Bun marked as idle [...]

## What changed

- `CodeBlock::shouldJettisonDueToOldAge` compares the counter with `m_previousCounter` before the lease test, so it runs on every look, inside the lease too. The comment gives the reason: "moved" must always mean "since the previous collection that looked", or idle code kept one extra lease after each burst of work, and forever when the embedder's idle collections came in pairs. A look is a marker visiting an executable's edge to a `CodeBlock` not yet marked (`ScriptExecutable::visitCodeBlockEdge`, `CodeBlock::shouldVisitStrongly`). The renewal is `m_creationTime = now + ttl × (codeBlockAgingLeaseMultiplier − 1)`, with `now` read once per collection (`Heap::currentGCStartApproximateTime`).
- `CodeBlock::setupWithUnlinkedBaselineCode` samples the fresh baseline counter into `m_previousCounter` (install.md already says so).
- A block the check let go is flagged (`m_visitChildrenSkippedDueToOldAge`, `CodeBlock::agedOut`). `ScriptExecutable::jettisonCodeBlockEdgeIfDead` jettisons such a block with `JettisonDueToOldAge` even when one of its weak references also died. It therefore takes the old-age path in `CodeBlock::jettison`: no exit-site tally, no reoptimization count, and the baseline code parked in the UCB is released.
- The FTL path now has its Bun caller: `GarbageCollectionController::idle_tick` requests the idle-tagged collections (`JSC__VM__collectAsyncIdle`) after 10 s, 2 min and 10 min of cumulative quiet (`BUN_IDLE_GC_SECONDS`). Which code ages by quietness is `CodeBlock::agesByMutatorQuietness`: FTL code, and DFG code when `useExecutionCountForCodeBlockAging` is off, when it has no `DFG::JITData`, or when its baseline's FTL compilation failed. The quiet test is `now − max(m_creationTime, Heap::lastActiveCollectionTime()) ≥ optimizedCodeAgingQuietSeconds` (30). A collection marks the mutator active once the bytes allocated since the last active one exceed `optimizedCodeAgingQuietAllocationMB` (1; 0 means such code never ages), accumulated in `Collector` at each collection's start.

## Proposed text

First paragraph, replace "once the protection has run out the GC reads the tier's counter during marking" with "every time marking reaches a CB through its executable, inside its protection too, the GC reads the tier's counter". Replace "sets `m_creationTime` to `now + 2 × TTL`" with "sets `m_creationTime` to `now + (codeBlockAgingLeaseMultiplier − 1) × TTL`". Add at the end: "Refreshing the sample on every look makes a change mean 'since the previous collection that looked'. A CB the check lets go is jettisoned for old age even if a weak reference of it also died, so it leaves no exit sites and releases the UCB's parked baseline code."

Second paragraph, replace "in a collection that Bun marked as idle" with "in a collection Bun tags idle, which its GC timer requests after 10 s, 2 min and 10 min of quiet (`BUN_IDLE_GC_SECONDS`; `GarbageCollectionController::idle_tick`)", and "if they were created at least 30 seconds ago and the VM has gone 30 seconds without a signal" with "once 30 s (`optimizedCodeAgingQuietSeconds`) have passed since both their creation and the last signal".
