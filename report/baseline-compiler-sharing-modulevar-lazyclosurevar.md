# baseline/compiler.md: the sharing table's `ModuleVar` row and the new `LazyClosureVar` shape

Doc: `skills/reference/knowledge/baseline/compiler.md`, section "Sharing between CodeBlocks", the table.

## Passage

> | `ModuleVar`, in `resolve_scope` | a `loadPtr` of the environment at a fixed metadata offset, with no type check | the member at that offset is whatever that `CodeBlock`'s link stored for its own resolve type |

## What changed

- `resolve_scope` metadata no longer holds the module environment for `ModuleVar`; linking stores the scope depth and `m_moduleImportSlot` (inlinecaches.md has the report). `JIT::emit_op_resolve_scope` treats `ModuleVar` like `ClosureVar`: below depth eight one `loadPtr` per scope link, from eight up a loop over the depth read from metadata. It then loads the import slot of the importing module environment at a displacement baked from the compiled `CodeBlock`'s `m_moduleImportSlot` (`JSLexicalEnvironment::offsetOfVariables` plus the slot times 8), with no type check, and takes a slow case while that slot is empty.
- `get_from_scope` has a new baked shape, `LazyClosureVar` (`JIT::emit_op_get_from_scope`): the same load as `ClosureVar` at the metadata operand's index, with no type check, plus a slow case on an empty value. The comment there says the other `CodeBlock`s of the UCB link the instruction as `LazyClosureVar` or `ClosureVar`, and the code is right for both; the choice depends on the module's code and the option only (`CodeBlock::finishCreation`). The baseline's slow operation also handles a `ClosureVar` link (`JITOperations.cpp`: "The shared baseline code for LazyClosureVar also runs in CodeBlocks that linked this as ClosureVar").

## Proposed text

Replace the `ModuleVar` row with:

> | `ModuleVar`, in `resolve_scope` | the `ClosureVar` walk to the importing module environment, then a `loadPtr` of its import slot at a baked offset, with no type check; an empty slot takes the slow path | its depth below eight and its import slot are ignored |

Add after the `ClosureVar` row of `get_from_scope`:

> | `LazyClosureVar`, in `get_from_scope` | the `ClosureVar` load, plus a slow path when the slot is empty | a `CodeBlock` that linked the site as `ClosureVar` runs it unchanged; the slow operation serves both |

In "Inputs", row "LLInt caches and scope metadata", add `m_moduleImportSlot` to the listed fields; it decides a displacement in the code rather than a variant.

Net size change is about +230 bytes; the doc is at 39,109 bytes.
