# profiles.md: `catch` buffers are created when the function first crosses its DFG threshold

Doc: `skills/reference/knowledge/common/profiles.md`: the `ValueProfileAndVirtualRegisterBuffer` row of the table, and section "Drain", fourth paragraph (the baseline→DFG context).

## Passage

Table row `ValueProfileAndVirtualRegisterBuffer`, "where it lives": "heap, pointed to by the `OpCatch` metadata, allocated by `CodeBlock::ensureCatchLivenessIsComputedForBytecodeIndex`"; "who writes it": "the `catch` slow path, in the LLInt (`slow_path_profile_catch`) and in the JIT (`operationTryOSREnterAtCatchAndValueProfile`)".

## What changed

With `useLazyCatchLiveness` (default on):

- `CodeBlock::ensureCatchLivenessIsComputedForBytecodeIndex` no longer runs the liveness analysis on a catch's first execution. It sets the metadata bit `hasExecutedWithoutBuffer` (`BytecodeList.rb`, `op_catch`) and the CB's `m_hasCatchThatExecutedWithoutBuffer`, and returns null, so the slow paths profile nothing.
- `operationOptimize`, once the baseline CB has crossed its DFG threshold and before `shouldOptimizeNowFromBaseline`, calls `ensureCatchLivenessIsComputedForExecutedCatches`. That creates the buffers of every catch that ran without one (`ensureCatchLivenessIsComputedForExecutedCatchesSlow`, which clears the CB's flag but leaves the per-catch bits). If it created any, the handler drains and rearms for a warm-up (`updateAllPredictionsAndOptimizeAfterWarmUp`) instead of compiling, so the new buffers can collect samples: the tier-up is delayed once.
- A catch that never ran gets no buffer. The DFG parser (`ByteCodeParser::parseBlock`, `case op_catch`) does not make a catch an OSR entrypoint unless every operand of its buffer has been sampled, since a buffer can now exist before its first sample.

## Proposed text

Row `ValueProfileAndVirtualRegisterBuffer`, "where it lives": "heap, pointed to by the `OpCatch` metadata. With `useLazyCatchLiveness`, a catch's first execution only sets `hasExecutedWithoutBuffer` and the CB's `m_hasCatchThatExecutedWithoutBuffer`; the buffers of the catches that ran are allocated when the baseline CB first crosses its DFG threshold (`ensureCatchLivenessIsComputedForExecutedCatches`, from `operationOptimize`), which then defers that tier-up once for a warm-up".

Drain, fourth paragraph, after "past the threshold inside `shouldOptimizeNowFromBaseline`": add "or, when that crossing has just created `catch` buffers, in the warm-up that defers it (`updateAllPredictionsAndOptimizeAfterWarmUp`)".
