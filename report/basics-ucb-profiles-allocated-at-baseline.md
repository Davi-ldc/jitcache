# basics.md: the UCB's value and array profiles exist only after a baseline compilation

Doc: `skills/reference/basics.md`, the `[^unlinked]` footnote, and the Abstract's description of what the UCB carries.

## Passage

```cpp
OutOfLineJumpTargets                     m_outOfLineJumpTargets; // hash; the jump that did not fit in the operand
FixedVector<UnlinkedValueProfile>        m_valueProfiles;     // prediction, no buckets
FixedVector<UnlinkedArrayProfile>        m_arrayProfiles;
```

## What changed

- `UnlinkedCodeBlock` has no `m_valueProfiles` or `m_arrayProfiles`. Both live in one `ValueAndArrayProfiles` block (a `ButterflyArray` of `UnlinkedArrayProfile` and `UnlinkedValueProfile`) held by `m_valueAndArrayProfiles`. It is null until `UnlinkedCodeBlock::ensureValueAndArrayProfiles` runs, which the `BaselineJITPlan` constructor calls on the VM thread. Builtins never get one (`isBuiltinFunction` returns early). The allocation is published with a `storeStoreFence`, because the collector and compiler threads read the pointer without a lock.
- The CB's drains fold into these copies only when they exist: `updateAllNonLazyValueProfilePredictionsAndCountLiveness` and `updateAllArrayProfilePredictions` read `valueAndArrayProfiles()` and skip the fold when it is null. A body that never reached the Baseline JIT therefore keeps no UCB value or array profiles. The arithmetic profiles (`m_binaryArithProfiles`, `m_unaryArithProfiles`) are unchanged.
- `m_outOfLineJumpTargets` moved into `UnlinkedCodeBlock::RareData`.
- The expression info is decoded on first use for a block from a persistent bytecode payload (`expressionInfo`, `expressionInfoSlow`, `m_cachedExpressionInfo`).

## Proposed text

Replace those footnote lines with:

```cpp
std::unique_ptr<ValueAndArrayProfiles>   m_valueAndArrayProfiles; // predictions, no buckets; null until a CB of the body
                                                                   // reaches the Baseline JIT (ensureValueAndArrayProfiles)
```

and extend the last line:

```cpp
std::unique_ptr<RareData> m_rareData;   // exception handlers, jump tables and the jumps that did not fit in their operand
```

In the Abstract, after "what has been learned about types during execution", add: "(value and array types only once one of its `CodeBlock`s has reached the baseline)".

Net size change is about +60 bytes; the doc is at 39,973, so condense elsewhere first.
