# locks.md: `UnlinkedCodeBlock::m_lock` also guards the lazy decode of expression info

Doc: `skills/reference/knowledge/common/locks.md`, the `UnlinkedCodeBlock::m_lock` row.

## Passage

> | `UnlinkedCodeBlock::m_lock` | initialization of `m_liveness`, and the exit profile | Compiler threads in `livenessAnalysisSlow`; writes to the `DFG::ExitProfile` in `add`, [...] It does not cover all of the UCB's profiles. |

## What changed

A UCB decoded from a persistent bytecode payload keeps its expression info in the payload (`m_cachedExpressionInfo`) until first use. `UnlinkedCodeBlock::expressionInfo()` returns `m_expressionInfo` without a lock when it is set; otherwise `expressionInfoSlow` takes `m_lock`, rechecks, decodes (`decodeBorrowedExpressionInfo`, one `fastMalloc`, no GC allocation and no safepoint), issues `storeStoreFence` and stores the pointer. The comment on `expressionInfo()` says any thread may call it, including the collector's end phase (`ErrorInstance::computeErrorInfo`). `expressionInfoForBytecodeIndex` then takes `m_lock` again around `entryForInstPC`, which fills a cache. A caller that can neither lock nor allocate, such as a sampling hook inside malloc, uses `expressionInfoIfDecoded`, which never decodes.

## Proposed text

Row `UnlinkedCodeBlock::m_lock`, "what it protects": "initialization of `m_liveness`, the decode of the expression info, and the exit profile". "Who takes it": add "any thread on the first use of the expression info of a block decoded from a bytecode payload (`expressionInfoSlow`, which publishes the result behind a fence for lock-free readers), and around `entryForInstPC` in `expressionInfoForBytecodeIndex`".
