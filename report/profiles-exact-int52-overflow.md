# profiles.md: `Int52Overflow` is set only for an actual Int52 overflow

Doc: `skills/reference/knowledge/common/profiles.md`, section "Arithmetic profile", third paragraph.

## Passage

> What gets recorded depends on the path: `observeResult` and `emitSetDouble` classify any double conservatively, setting four bits at once, `Int32Overflow | Int52Overflow | NonNegZeroDouble | NegZeroDouble`. `updateArithProfileForBinaryArithOp` and the double path of `JITMulGenerator::generateFastPath`, on the other hand, test some of these conditions separately, including `-0` and int52 overflow; both int52 tests allow an intentional false positive at `-2^51`.

## What changed

- `ObservedResults::isInt52Overflow` (`ArithProfile.h`) is exact: a double is outside Int52 when its magnitude is at least `2^51` (`int52OverflowPoint`), except `-2^51` itself; NaN and the infinities count as overflow. It compares the raw bits with the sign shifted out.
- `ArithProfile::observeResult` sets `Int32Overflow | NonNegZeroDouble | NegZeroDouble` for any double and adds `Int52Overflow` only when `isInt52Overflow` holds. `emitObserveResult`, which OSR exit uses through `MethodOfGettingAValueProfile::emitReportValue`, records the same bits: it emits the three-bit set only if those bits are not all set already, and the Int52 test (`emitSetInt52OverflowIfNeeded`) only while the profile has not seen `Int52Overflow`. These choices are made at compile time from the profile's current bits.
- The slow paths (`updateArithProfileForBinaryArithOp`, `updateArithProfileForUnaryArithOp` in `CommonSlowPaths.cpp`) and the double path of `JITMulGenerator::generateFastPath` use the same exact test, so the false positive at `-2^51` is gone.
- `emitSetDouble`, used by `JITAddGenerator`, `JITSubGenerator` and `JITNegGenerator`, still sets all four bits whenever `shouldEmitSetDouble` holds.

## Proposed text

> What gets recorded depends on the path. `emitSetDouble`, in `add`, `sub` and `negate`, classifies any double conservatively, setting four bits at once, `Int32Overflow | Int52Overflow | NonNegZeroDouble | NegZeroDouble`. `observeResult` and its emitted twin `emitObserveResult` set the other three for any double but `Int52Overflow` only for an actual overflow (`ObservedResults::isInt52Overflow`: magnitude at least `2^51`, except `-2^51`, or NaN or infinite), and the emitted form tests only for bits the profile lacks when the code is generated. `updateArithProfileForBinaryArithOp` and the double path of `JITMulGenerator::generateFastPath` test `-0` separately and use the same exact int52 test.
