# gc.md: marking decides code aging, refreshing the counter snapshot on every look

Doc: `skills/reference/knowledge/common/gc.md`, Abstract, third paragraph (or a sentence after the second, where marking is described).

## Passage

> Then, with the world still stopped, the GC walks the revisit list: in the marked code blocks it drains the profiles [...]; for the ones left unmarked, the owner executable is the one that acts, jettisoning the code block and clearing the edge to it.

## What changed

The doc does not say why a `CodeBlock` is left unmarked when its executable is alive. When a marker visits an executable's edge to an unmarked `CodeBlock` (`ScriptExecutable::visitCodeBlockEdge`), `CodeBlock::shouldVisitStrongly` asks `shouldJettisonDueToOldAge`. Under `USE(BUN_JSC_ADDITIONS)` with `useExecutionCountForCodeBlockAging`, that function reads the tier's counter on every such look and compares it with `m_previousCounter`: the UCB's LLInt counter, the `BaselineJITData`'s or the `DFG::JITData`'s tier-up counter. That includes looks while the block is still inside its lease. If the counter moved, it stores the new count and renews the lease (`m_creationTime = now + ttl × (codeBlockAgingLeaseMultiplier − 1)`). The comment explains why the snapshot is refreshed inside the lease too: "moved" then always means "since the previous collection that looked", which stops idle code from surviving an extra lease after each burst of work, and forever when the embedder's idle collections come in pairs. A block that aged out is skipped (`m_visitChildrenSkippedDueToOldAge`, readable as `CodeBlock::agedOut`). At the end phase, `ScriptExecutable::jettisonCodeBlockEdgeIfDead` jettisons it with `JettisonDueToOldAge`, so it takes the old-age path (no exit-site tally, and the baseline code parked in the UCB is released) even when it also has a dead weak reference.

## Proposed text

Before "for the ones left unmarked", add:

> A `CodeBlock` stays unmarked when only its executable reaches it and it has aged out: on each visit of that edge the marker compares the tier's execution counter with the snapshot the block keeps, refreshing the snapshot each time, and renews the block's lease only if the counter moved since the previous look (`CodeBlock::shouldJettisonDueToOldAge`; Bun's aging, [bunpatches.md](./bunpatches.md#code-aging-pr-557-codeblockshouldjettisonduetooldage)).

And make the last clause "jettisoning the code block, for old age if it aged out, and clearing the edge to it".
