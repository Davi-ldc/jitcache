# basics.md: a module with its own module scope gets a UCB outside the CodeCache

Doc: `skills/reference/basics.md`, section "How UCBs and CBs relate", second paragraph.

## Passage

> With `useCodeCache` on, a program or module UCB decoded from the provider's cache is also inserted into the map (`CodeCache::getUnlinkedGlobalCodeBlock`).

## What changed

In `CodeCache::getUnlinkedGlobalCodeBlock`, a `ModuleProgramExecutable` whose loader has a module scope of its own (`!resolvesInGlobalScope()`, that is, non-empty `m_moduleScopeSymbolTables`) sets `privateToExecutable`. Its code resolves the scope's variables as closure variables where other records of the same source resolve globals, so it cannot share their UCB. It neither looks up nor fills the map: it decodes with `fetchFromDisk(..., Decoder::RecoverableCode::No)` or generates, and the publication after generation skips it (`!privateToExecutable`). It is not registered with `PersistentBytecodePayloads` either, so it never returns to its payload. The records that share its executable use that UCB.

## Proposed text

Append to the passage:

> A module whose loader has a module scope of its own is the exception: its code resolves that scope as closure variables, so it gets a UCB of its own that never enters the map (`privateToExecutable`).

Net size change is about +200 bytes; the doc is at 39,973, so condense elsewhere first.
