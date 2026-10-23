# profiles.md: the UCB's value and array copies are allocated at the first baseline compilation

Doc: `skills/reference/knowledge/common/profiles.md`: the Abstract ("Predictions also accumulate in the `UnlinkedCodeBlock`"), section "Drain", third paragraph, section "Lifetime", second paragraph, and the `UnlinkedValueProfile` and `UnlinkedArrayProfile` rows.

## Passages

> A new UCB starts with zeroed profiles, both when it is generated from source [...] and when it is decoded. The on-disk bytecode cache carries no observations at all: it serializes the four counts (value, array, binary arith and unary arith), and the `UnlinkedCodeBlock`'s decode constructor rebuilds the four vectors zeroed.

Rows `UnlinkedValueProfile` and `UnlinkedArrayProfile`, "where it lives": `UnlinkedCodeBlock`.

## What changed

- `UnlinkedCodeBlock` holds the value and array copies in one `ValueAndArrayProfiles` block (`m_valueAndArrayProfiles`), null until `UnlinkedCodeBlock::ensureValueAndArrayProfiles` runs. The `BaselineJITPlan` constructor calls it on the VM thread; builtins never get one. It is published with a `storeStoreFence` for the collector and compiler threads, which read the pointer once per walk. `unlinkedValueProfiles()` and `unlinkedArrayProfiles()` are gone; the accessor is `valueAndArrayProfiles()`.
- The drains merge into the copies only when the block exists (`updateAllNonLazyValueProfilePredictionsAndCountLiveness`, `updateAllArrayProfilePredictions`). A body none of whose CBs reached the baseline has none, and its CBs' drains merge nowhere. The arithmetic profiles are allocated at generation and decode as before (`allocateSharedProfiles`; `CachedTypes.cpp` sets `m_binaryArithProfiles` and `m_unaryArithProfiles` from the payload's counts).
- The payload records the value profile count inside the metadata layout and the array profile count as `m_numberOfArrayProfiles`; decode allocates neither copy.

## Proposed text

Abstract: after "Predictions also accumulate in the `UnlinkedCodeBlock`, which outlives its `CodeBlock`s", add "once one of them has been handed to the Baseline JIT".

Lifetime, replace the two sentences with:

> A new UCB starts with zeroed arithmetic profiles and no value or array copies, both when it is generated from source (`generateUnlinkedFunctionCodeBlock`, inside `unlinkedCodeBlockFor`) and when it is decoded; the on-disk bytecode cache carries only the counts. The copies are allocated, zeroed, when a `CodeBlock` of the body is first handed to the Baseline JIT (`ensureValueAndArrayProfiles`, in the `BaselineJITPlan` constructor); until then the drains have nothing to merge into.

Rows `UnlinkedValueProfile` and `UnlinkedArrayProfile`, "where it lives": "`UnlinkedCodeBlock` (`m_valueAndArrayProfiles`), from the body's first baseline compilation".
