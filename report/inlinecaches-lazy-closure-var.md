# inlinecaches.md: `get_from_scope` links module reads as `LazyClosureVar`

Doc: `skills/reference/knowledge/common/inlinecaches.md`, "Scope caches", second and third paragraphs; the Abstract, item 3.

## Passages

> `get_from_scope` turns `ResolvedClosureVar` into `ClosureVar` without resolving, and `ModuleVar` into `ClosureVar` over the module environment.

> Most closure and module types are baked into the code with no type check.

## What changed

`GetPutInfo.h` adds the resolve types `LazyClosureVar` and `ResolvedLazyClosureVar`: a `ClosureVar` whose slot may still hold the empty value because it belongs to a module function declaration not instantiated yet (`useLazyModuleFunctionDeclarations`), or because it is an import whose exporter may be such a slot. `ResolvedLazyClosureVar` exists only in unlinked bytecode. In `CodeBlock::finishCreation`, `case op_get_from_scope`, with `useLazyModuleFunctionDeclarations` on (default):

- `ResolvedLazyClosureVar` links as `LazyClosureVar` without resolving;
- `ModuleVar` links as `LazyClosureVar` (as `ClosureVar` with the option off);
- `ClosureVar` and `ClosureVarWithVarInjectionChecks` over a module environment slot that `JSModuleEnvironment::isFunctionDeclarationSlot` names link as the lazy form, keeping the injection-check bit.

The choice depends only on the module's code, never on what has been instantiated, so every `CodeBlock` of a UCB links the instruction the same way (comment in `finishCreation`, and on `JSModuleRecord::isFunctionDeclarationSlot`). `JIT::emit_op_get_from_scope` bakes `LazyClosureVar` like `ClosureVar`, without a type check, and adds a slow case when the loaded value is empty. The slow path, in the LLInt and in the baseline's operation, reads through `JSModuleEnvironment::readLazyClosureVar`, which instantiates the declaration into its slot (`JSModuleRecord::readFunctionDeclarationSlot`).

## Proposed text

Second paragraph, replace the `get_from_scope` sentence with:

> `get_from_scope` turns `ResolvedClosureVar` into `ClosureVar` without resolving. With `useLazyModuleFunctionDeclarations`, it links `ModuleVar`, `ResolvedLazyClosureVar` and any closure read of a module function declaration's slot as `LazyClosureVar`, a `ClosureVar` whose slot may still be empty because the declaration has not been instantiated yet.

Third paragraph, append: "`LazyClosureVar` is baked the same way, with one more test: an empty slot takes the slow path, which instantiates the declaration."
