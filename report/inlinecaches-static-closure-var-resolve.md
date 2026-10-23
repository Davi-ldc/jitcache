# inlinecaches.md: optimized bytecode links some `resolve_scope`s without `abstractResolve`

Doc: `skills/reference/knowledge/common/inlinecaches.md`, "Scope caches", first and second paragraphs.

## Passage

> Linking runs `JSScope::abstractResolve` in `CodeBlock::finishCreation` and keeps its answer in the metadata of `resolve_scope`, `get_from_scope` and `put_to_scope` [...]

## What changed

The bytecode optimizer (`BytecodeOptimizer.cpp`; it runs under `useBytecodeOptimizer`, off by default, or when an embedder generates a bytecode cache with `OptimizeBytecode::Yes`) can resolve an environment-record variable statically (`useBytecodeOptimizerStaticScopes`, default on). It then writes a static closure-variable type into the unlinked `resolve_scope`'s `resolveType` operand: a value from `firstStaticClosureVarResolveType` (32) up, encoding how many environment records out the variable lives (`staticClosureVarHops`, `GetPutInfo.h`). No other consumer sees these values. In `CodeBlock::finishCreation`, `case op_resolve_scope`, such an instruction skips `abstractResolve`: linking walks that many `next()` hops from the scope, `RELEASE_ASSERT`s that the record is a `JSLexicalEnvironment` whose symbol table holds the name, and stores `ClosureVar`, the depth `m_localScopeDepth + hops` and the symbol table. `validateBytecodeOptimizerStaticScopes` cross-checks against `abstractResolve`.

## Proposed text

Append to the first paragraph:

> Bytecode from the bytecode optimizer can carry a static closure type in an unlinked `resolve_scope` (`isStaticClosureVarResolveType`); linking turns it into `ClosureVar` by walking the given number of scopes, without `abstractResolve`, and crashes if the record found does not hold the name.
