# inlinecaches.md: `get_from_scope` metadata has no watchpoint set

Doc: `skills/reference/knowledge/common/inlinecaches.md`, the `get_from_scope` and `put_to_scope` metadata row of the table.

## Passage

> `get_from_scope` and `put_to_scope` metadata | `CodeBlock` metadata | `GetPutInfo`; a union of structure and watchpoint set; an operand that is a property offset, a scope offset or a pointer to the variable | [...]

## What changed

`BytecodeList.rb`: `op :get_from_scope` metadata is `getPutInfo`, `structureID` and `operand`, with no union and no watchpoint set. `op :put_to_scope` keeps the union of `structureID` and `watchpointSet` (an `InlineWatchpointSet*`), which `CodeBlock::finishCreation` fills for the global variable types and for a `ResolvedClosureVar` write, and the baseline's write fast path tests (the "Scope caches" paragraph on global types stays correct).

## Proposed text

Table row, "what it holds":

> `GetPutInfo`; a structure (for `put_to_scope`, in a union with the variable's watchpoint set); an operand that is a property offset, a scope offset or a pointer to the variable
