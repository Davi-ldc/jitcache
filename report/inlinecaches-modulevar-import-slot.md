# inlinecaches.md: `resolve_scope` with `ModuleVar` loads the exporting environment from an import slot

Doc: `skills/reference/knowledge/common/inlinecaches.md`, "Scope caches", second paragraph, and the `resolve_scope` metadata row of the table.

## Passages

> `resolve_scope` with `ModuleVar` stores the module environment in the metadata and also holds it as a constant of the `CodeBlock`.

Table row `resolve_scope` metadata, "what it holds": "`ResolveType`; a union of scope depth and global lexical binding epoch; a union of lexical environment, symbol table, constant scope, global lexical environment and global object".

## What changed

- `BytecodeList.rb`, `op :resolve_scope`: the second union is `symbolTable` (for `ClosureVar`), `moduleImportSlot`, `constantScope`, `globalLexicalEnvironment`, `globalObject`; there is no `lexicalEnvironment`.
- `JSScope::abstractResolve`, on a module environment, resolves the import with `JSModuleRecord::resolveImportWithSlot` and returns `ModuleVar` with the scope depth and `moduleImportSlot`: the index of the import slot in the importing module environment, addressed like one of its variables past the symbol table's own (`JSModuleEnvironment::importSlotScopeOffset`). `CodeBlock::finishCreation` stores `m_moduleImportSlot` and nothing else for `ModuleVar`. `JSScope::constantScopeForCodeBlock` returns null for it, so no constant scope either.
- `JIT::emit_op_resolve_scope` with `ModuleVar` walks `m_localScopeDepth` scopes to the importing module environment like `ClosureVar`, then loads the import slot at a displacement baked from `m_moduleImportSlot`. The slot holds the exporting module environment; while it is still empty the template takes a slow case, whose runtime fills it.

## Proposed text

> `resolve_scope` with `ModuleVar` stores the scope depth and an import slot of the importing module environment (`m_moduleImportSlot`, from `JSModuleRecord::resolveImportWithSlot`); the code walks to that environment and loads the exporting one from the slot, taking the slow path while the slot is still empty.

Table row: "`ResolveType`; a union of scope depth and global lexical binding epoch; a union of symbol table, module import slot, constant scope, global lexical environment and global object".
