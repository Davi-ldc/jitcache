# bunpatches.md: the conditions table and the host's code discarding miss run-once release and the idle shrink

Doc: `skills/reference/knowledge/common/bunpatches.md`, section "Conditions": the table and the paragraph after it.

## Passages

> | heap heuristic, page decommit, THP and `MADV_DONTFORK`, assembler buffer cap, C API locks, Yarr and BigInt, engine fixes | unconditional |

> The host also discards code on its own schedule. In `bun run`/`bun <file>` and in workers, [...] Bun calls `JSC__VM__runGC(false)` [...]

## What changed

- Run-once release is a fork option, absent upstream (`useRunOnceCodeRelease`, default on), declared in the `USE(BUN_JSC_ADDITIONS)` option list (`FOR_EACH_JSC_CODEBLOCK_AGING_OPTION`): `Interpreter::executeProgram` and `ModuleProgramExecutable::didFinishEvaluation` release the code of top-level bodies that ran once and never left the LLInt, with no embedder call (basics.md has the report). The other lazy-linking options are ordinary options, on by default for every embedder (separate report).
- Returning decoded code to its payload is engine code reached through `Heap::deleteAllUnlinkedCodeBlocks` with `UnlinkedCodeToDelete::RecoverableFromCache`. A module's release of its decoded UCB after evaluation (`releaseUnlinkedCodeIfRecoverable`) happens on its own, but only for a payload the embedder marked persistent. The rest needs an embedder call: `VM::shrinkFootprintNow`, or `deleteAllCode`.
- Bun makes that call: before its last idle full collection, `GarbageCollectionController::idle_tick` calls `JSC__VM__shrinkFootprintNow`, which runs `VM::shrinkFootprintNow` with `LeaveCollectionToCaller` and `KeepCodeInUse`. That returns to their payloads the UCBs decoded from persistent payloads that no `CodeBlock` links against, drops them from the `CodeCache` (`clearCodeDecodedFromPersistentPayloads`) and, with `releaseIdleRegExpCodeWhenShrinkingFootprint`, drops RegExp code unused this cycle; linked code stays. The idle collections themselves are tagged (`GCRequest::isIdle`; gc.md has the report). `JSC__VM__runGC` is unchanged.

## Proposed text

Table: add a row

> | lazy function executables, module declarations, call data, profile predictions and catch buffers | unconditional (fork options, on by default) |

add "run-once release of top-level code" to the "guarded, active in our build" row, and in the "depends on the embedder" row add "returning decoded code to a persistent payload (`shrinkFootprintNow`), idle-tagged collections".

After the `JSC__VM__runGC` paragraph, add:

> Bun's GC timer also runs idle-tagged full collections after 10 s, 2 min and 10 min of quiet (`BUN_IDLE_GC_SECONDS`), and before the last one calls `JSC__VM__shrinkFootprintNow` (`KeepCodeInUse`): UCBs decoded from a persistent payload that no `CodeBlock` links against go back to it and leave the `CodeCache`, and the next request decodes them again, with fresh profiles and an empty sharing slot.
