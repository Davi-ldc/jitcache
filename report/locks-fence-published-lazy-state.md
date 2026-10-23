# locks.md: lazily created per-site and per-body state is published with fences, not locks

Doc: `skills/reference/knowledge/common/locks.md`, the third paragraph (on `CodeBlock::m_lock`); possibly a closing paragraph before the safepoints paragraph.

## Passage

> `CodeBlock::m_lock` coordinates changes to the ICs with the reads done by compilation and marking, but it does not protect every field. Profiles are written and drained without it, [...]

## What changed

Several structures that linking used to create are now created later by the mutator, while compiler threads and markers may already read their owners. None of them takes a lock. Each is published by a fence, or a compare-and-swap, that orders its contents before the pointer that makes it visible:

- A call site's own `CallSiteData`: `LazyCallLinkInfo::ensureSlow` initializes it, issues `WTF::storeStoreFence()` and only then stores `m_data`. Readers (`LazyCallLinkInfo::ownData`, behind `get` and `arrayProfile`) load the pointer with `Dependency::loadAndFence` and read through the consumed pointer, so they see a finished one or the VM's shared one, which has no owner and counts as none.
- The UCB's value and array copies: `UnlinkedCodeBlock::ensureValueAndArrayProfiles`, mutator only, builds the `ValueAndArrayProfiles` block, issues `storeStoreFence` and stores `m_valueAndArrayProfiles`. The collector and compiler threads read the pointer once per walk.
- A metadata table's predictions array: `MetadataTable::ensureValueProfilePredictions` can run on the mutator and a compiler thread at once. It allocates a zeroed array and installs it with `compare_exchange_strong` (`acq_rel`), freeing its own on a lost race; readers load with `memory_order_acquire` (`valueProfilePredictions`).
- The `CodeBlock`'s lazy state: `prepareLazyStateForConcurrentCompilationSlow` creates the missing `FunctionExecutable`s, issues `storeStoreFence` and sets `m_isLazyStatePreparedForConcurrentCompilation`; `isLazyStatePreparedForConcurrentCompilation` reads it followed by `loadLoadFence`. Compiler threads read only CBs that passed it.
- A UFE's code slots, which now go back and forth between UCBs and a payload decoder (`decodeCachedCodeBlocks`, `returnCodeToCache`): both sides publish around `m_isCached` with fences, and `UnlinkedFunctionExecutable::visitChildrenImpl` reads the flag, the slots and the flag again with `loadLoadFence`s between.

## Proposed text

After the third paragraph, add:

> State the mutator now creates after linking, while other threads can already reach its owner, is published without a lock: the contents first, then a fence, then the pointer. That covers a call site's own `CallSiteData` (`LazyCallLinkInfo::ensureSlow`; readers go through `Dependency::loadAndFence`), the `UnlinkedCodeBlock`'s value and array copies (`ensureValueAndArrayProfiles`), a `CodeBlock`'s nested `FunctionExecutable`s (`prepareLazyStateForConcurrentCompilation`, whose flag compiler threads test) and a UFE's code slots when they return to a bytecode payload. A metadata table's predictions array, which a compiler thread may also create, is installed by compare-and-swap (`MetadataTable::ensureValueProfilePredictions`).
