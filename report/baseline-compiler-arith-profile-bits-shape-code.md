# baseline/compiler.md: the arithmetic profile's result bits also decide which profiling writes are emitted

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "Inputs", the `arithmetic profiles` row.

## Passage

> | arithmetic profiles | UCB | the fast path each arithmetic template emits, or a patchable jump when the profile is empty |

## What changed

The result bits read at compile time now decide more of the emitted bytes. `JITMulGenerator::generateFastPath`, on its double path, emits the Int52 overflow test (`ArithProfile::emitSetInt52OverflowIfNeeded`) only while the profile has not recorded `Int52Overflow`. `emitSetDouble` (in `add`, `sub` and `negate`) emits its write only while `shouldEmitSetDouble` holds, that is, until all four double bits are set. Two compilations of one body with different profile bits therefore differ in these sequences even when they choose the same fast path. (`emitObserveResult` makes the same kind of choice, but OSR exit code uses it, not the baseline.)

## Proposed text

> | arithmetic profiles | UCB | the fast path each arithmetic template emits, or a patchable jump when the profile is empty; and, from the result bits already set, which profiling writes and overflow tests the double paths still emit (`shouldEmitSetDouble`, `emitSetInt52OverflowIfNeeded` in `mul`) |

Net size change is about +170 bytes; the doc is at 39,109 bytes.
