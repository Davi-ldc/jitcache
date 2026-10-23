# baseline/compiler.md: references the new iterator, RegExp and tail-call code embeds

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "What the code embeds", the owner table.

## Passages

Row "the `UnlinkedCodeBlock`": "[...] the pointer of each cell constant it owns, as an immediate (`emitGetVirtualRegister`, `op_mov`) or an operation argument (`new_reg_exp`'s `RegExp`); [...]".

Row "the `VM`: itself and its fields": "[...] the pending exception (after an operation returning a register pair, such as `iterator_next`'s `operationIteratorNextTryFast`; the others return the exception in a register) [...]".

Row "the `VM`: objects it owns": "the empty string (`iterator_next`), the small-strings sentinel (`enumerator_next`), `syncResumeCallCache` (`async_iterator_next`); [...]".

Row "the process": "the C++ operations, each a far call (the ShadowChicken and exception-fuzz helpers call theirs through a pointer immediate instead); [...]".

## What changed

- `JIT::emit_op_iterator_next` has an inline fast-array path. When `next` is not a cell (an index) and the iterator register holds `VM::fastArraySentinel()`, compared as a pointer immediate, the template loads the element of an `ArrayWithInt32` or `ArrayWithContiguous` iterable inline. It writes the iterable's array profile and the value profile (under `shouldEmitProfiling`) and ORs `IterationMode::FastArray` into `seenModes` unconditionally. Its slow cases call `operationIteratorNextFastArray`; an iterator equal to `VM::fastStringSentinel()` calls `operationIteratorNextFastString`. Both return a register pair, like `operationIteratorNextTryFast`, so the exception check after them reads the `VM`'s exception word.
- `JIT::emit_op_iterator_close_check` (new opcode) compares the iterator with the same two sentinels as pointer immediates, ORs the matching mode into its `seenModes`, tests the global object's array or string iterator protocol watchpoint set through `loadGlobalObject` plus `JSGlobalObject::offsetOfArrayIteratorProtocolWatchpointSet` (or the string one), and otherwise calls `slow_path_iterator_close_check` through its thunk.
- `JIT::emit_op_new_reg_exp_shared` (new opcode, `useSharedRegExpLiteralObjects`) passes the UCB's `RegExp` constant as an immediate argument to `operationNewRegExpShared`, with the address of the site's `cachedObject` metadata.
- The tail-call path of `CallLinkInfo::emitFastPathImpl` moves `operationEnsureCallLinkInfoForTailCall` into a register as a pointer immediate and calls through it; no far-call record carries it.

## Proposed text

Row "the `UnlinkedCodeBlock`": replace "(`new_reg_exp`'s `RegExp`)" with "(the `RegExp` of `new_reg_exp` and `new_reg_exp_shared`)".

Row "the `VM`: itself and its fields": replace "such as `iterator_next`'s `operationIteratorNextTryFast`" with "such as `iterator_next`'s `operationIteratorNextTryFast`, `operationIteratorNextFastArray` and `operationIteratorNextFastString`".

Row "the `VM`: objects it owns": add "the fast-array and fast-string sentinels, compared as pointer immediates (`iterator_next`, `iterator_close_check`)".

Row "the process": replace the parenthesis with "(the ShadowChicken and exception-fuzz helpers, and `operationEnsureCallLinkInfoForTailCall` in a tail call's fast path, call theirs through a pointer immediate instead)".

Net size change is about +250 bytes; the doc is at 39,109 bytes.
