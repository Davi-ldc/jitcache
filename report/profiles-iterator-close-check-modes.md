# profiles.md: `iterator_close_check` records iteration modes

Doc: `skills/reference/knowledge/common/profiles.md`, the `m_iterationMetadata.seenModes` row of the table.

## Passage

> `m_iterationMetadata.seenModes` | opcode metadata | which `IterationMode` bits went through [...] | LLInt (handlers `op_iterator_next` and `op_async_iterator_next`), slow paths [...] | DFG (`ByteCodeParser::handleIteratorOpen`, `handleIteratorNext`, `handleAsyncIteratorOpen`, `handleAsyncIteratorNext`) | [...]

## What changed

The new opcode `iterator_close_check` (`BytecodeList.rb`) carries its own `seenModes: uint8_t` metadata field, outside `IterationModeMetadata`. It runs where a `for-of` exits early: when the iterator register holds one of the VM's sentinels (`VM::fastArraySentinel`, `fastStringSentinel`), the fast iteration never materialized an iterator. The site ORs `IterationMode::FastArray` or `FastString` into `seenModes`. If the global object's array or string iterator protocol watchpoint set is still valid, it jumps over the close; otherwise `slow_path_iterator_close_check` materializes a `JSArrayIterator` or `JSStringIterator` at the current index so the close can run. Writers: the LLInt handler `op_iterator_close_check` and the emitted baseline code (`JIT::emit_op_iterator_close_check`). Reader: `ByteCodeParser::parseBlock`, `case op_iterator_close_check`, which emits a sentinel comparison and a lazy watchpoint only for the modes seen whose watchpoint set is still valid; with none, it emits only the object check.

## Proposed text

Add a row after `m_iterationMetadata.seenModes`:

> | `OpIteratorCloseCheck::m_seenModes` | opcode metadata | which fast `IterationMode`s (`FastArray`, `FastString`) reached an early loop exit with the iterator still a VM sentinel | LLInt (handler `op_iterator_close_check`) and emitted baseline code (`JIT::emit_op_iterator_close_check`), by OR | DFG (`ByteCodeParser::parseBlock`, `case op_iterator_close_check`), which tests only the sentinels of modes seen whose iterator protocol watchpoint set is still valid | nobody |
