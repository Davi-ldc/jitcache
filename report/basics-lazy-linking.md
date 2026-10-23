# basics.md: nested functions and call sites are linked lazily

Doc: `skills/reference/basics.md`, section "UCB generation", second paragraph, and the `[^codeblock]` footnote.

## Passage

> Then `finishCreation` builds the rest: the constants become values of that realm, with the `SymbolTable` cloned; each nested function gets its own `FunctionExecutable` (in a module declaration it reuses the executable created at instantiation); the exception handler table is copied with each `nativeCode` pointing to the LLInt's `op_catch` dispatcher; and a pass over the bytecode initializes each call's `CallLinkInfo` and the profiles.

## What changed

Linking no longer creates the nested functions' executables or the call sites' `CallLinkInfo`s.

- `CodeBlock::finishCreation` sizes `m_functionDecls` and `m_functionExprs` and leaves them null unless `linkFunctionsEagerly` holds: `useLazyFunctionExecutables` off (default on), eval code, or a type or control-flow profiler. `CodeBlock::functionDecl` and `functionExpr` create an entry on first request (`materializeFunctionDeclSlow`, `materializeFunctionExprSlow`), from a `new_func*` slow path. Compiler threads use `functionDeclIfMaterialized` and see only CBs that `prepareLazyStateForConcurrentCompilation` finished; `setupWithUnlinkedBaselineCode` and `newReplacement` call it, and `JITPlan` construction too (comment on `prepareLazyStateForConcurrentCompilation` in `CodeBlock.h`). `m_numberOfUnmaterializedFunctionExecutables` counts the null entries.
- A module's heap-allocated declarations never get an entry in the CB (`firstLazilyMaterializedFunctionDecl`). With `useLazyModuleFunctionDeclarations` (default on, off under a profiler), `CyclicModuleRecord`'s environment initialization leaves their environment slots empty (`JSModuleRecord::setFunctionDeclarationSlots`), and the function and its executable are created when the binding is first read, through `ModuleProgramExecutable::functionDeclaration`. Readers in bytecode use the resolve type `LazyClosureVar` (`GetPutInfo.h`).
- `link_callLinkInfo` in `finishCreation`: with `useLazyLLIntCallLinkInfos` (default on), a `LazyCallLinkInfo` site is pointed at a VM-wide shared `CallSiteData` (`LazyCallLinkInfo::setNeverExecuted`, or `setTailCallNotExecuted` for `op_tail_call`). The site gets its own `CallSiteData` at its second execution (`LazyCallLinkInfo::ensureSlow`). Only the inline `DataOnlyCallLinkInfo` of the varargs opcodes is initialized at link.

## Proposed text

Replace the sentence with:

> Then `finishCreation` builds the rest: the constants become values of that realm, with the `SymbolTable` cloned; the exception handler table is copied with each `nativeCode` pointing to the LLInt's `op_catch` dispatcher; and a pass over the bytecode initializes the profiles and points each call site at a `CallSiteData` the whole VM shares, which the site trades for its own at its second execution (`useLazyLLIntCallLinkInfos`; varargs calls keep an inline `CallLinkInfo`). The nested functions' `FunctionExecutable`s come later, when a `new_func*` first asks for one or before the CB's first compilation (`useLazyFunctionExecutables`; eval code and the profilers link them all here). A module's heap-allocated declarations belong to the module environment, whose slot stays empty until the binding is first read (`useLazyModuleFunctionDeclarations`).

In the `[^codeblock]` footnote, mark the two vectors as lazy:

```cpp
FixedVector<WriteBarrier<FunctionExecutable>> m_functionDecls;  // null until first asked for
FixedVector<WriteBarrier<FunctionExecutable>> m_functionExprs;
```

The doc is at 39,973 bytes and this adds about 300; condense elsewhere first (for example the `add` disassembly table) to stay under 40,000.
