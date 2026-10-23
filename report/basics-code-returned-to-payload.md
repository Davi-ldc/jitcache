# basics.md: decoded function code can return to its bytecode payload

Doc: `skills/reference/basics.md`, section "Lifecycle", subsection "UCB", second paragraph.

## Passage

> `VM::deleteAllCode` can also discard the UCBs explicitly, and the Bun host runs one in every long-lived `bun run` process right after the entry finishes ([bun patches](knowledge/common/bunpatches.md#conditions)).

## What changed

A UCB decoded from a persistent bytecode payload records where it came from (`UnlinkedCodeBlock::cachedPayloadIndex`, `cachedRecordOffset`; the payloads are `VM::persistentBytecodePayloads()`, class `PersistentBytecodePayloads` in `CachedBytecode.h`). `Heap::deleteAllUnlinkedCodeBlocks` now takes an `OptionSet<UnlinkedCodeToDelete>` (`DeleteAllCodeEffort.h`):

- `Generated` clears the code of UFEs whose code was generated from source (`UnlinkedFunctionExecutable::clearCode`), as before.
- `RecoverableFromCache` calls `UnlinkedFunctionExecutable::returnCodeToCache` on every live UFE except those of suspended generators and async functions. The UFE drops its UCBs, keeps the record offsets and a `Decoder`, and sets `m_isCached`, so the next `unlinkedCodeBlockFor` decodes the same code again. The new UCB starts its tier-up over; still-live child executables are handed on through `PersistentBytecodePayloads::rememberChildExecutables`.
- `OnlyWithoutLinkedCode` restricts that to UCBs no CB links against, and also keeps the shared baseline slots, which the other modes clear.

`VM::deleteAllCode` passes `Generated` and `RecoverableFromCache`. `VM::shrinkFootprintNow` with no mode calls `deleteAllCode`. With `ShrinkFootprint::KeepCodeThatNeedsParsing` it deletes the CBs and passes only `RecoverableFromCache`; with `KeepCodeInUse` it keeps the CBs and adds `OnlyWithoutLinkedCode`. Both modes also remove decoded code from the `CodeCache` (`clearCodeDecodedFromPersistentPayloads`). It refuses to run under JS or inside the collector.

## Proposed text

Replace the passage with:

> `VM::deleteAllCode` can also discard the UCBs explicitly (`Heap::deleteAllUnlinkedCodeBlocks`), and the Bun host runs one in every long-lived `bun run` process right after the entry finishes ([bun patches](knowledge/common/bunpatches.md#conditions)). A UCB decoded from a persistent payload can instead go back to it: `UnlinkedFunctionExecutable::returnCodeToCache` empties the UFE's slots and keeps the record's offset, so the next request decodes the same code again with its tier-up starting over. `VM::shrinkFootprintNow` with `KeepCodeThatNeedsParsing` or `KeepCodeInUse` does only that to unlinked code, and with `KeepCodeInUse` only for UCBs no CB links against.

Net size change is about +380 bytes; the doc is at 39,973, so condense elsewhere first.
