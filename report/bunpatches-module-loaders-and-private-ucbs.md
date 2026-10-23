# bunpatches.md: several module loaders per global object, shared module executables and private module UCBs

Doc: `skills/reference/knowledge/common/bunpatches.md`, section "Async context and modules", first paragraph (the sentence on modules).

## Passage

> For modules, `VM::m_synchronousModuleQueue` makes `require(esm)` load, link and evaluate without yielding to user microtasks and without C++ recursion proportional to the number of modules.

## What changed

- A global object can host several module loaders. `JSModuleLoader::create` takes a `moduleScope`: the global lexical environment, or a chain of lexical environments ending in it, where that loader's module environments are created. Bun uses this for `Bun.ModuleGraph` (`~/bun/src/jsc/bindings/ModuleGraph.cpp`).
- Records of different loaders for the same module key and observably equal source share one `ModuleProgramExecutable` when their imports resolve alike and their loaders' module scopes have the same symbol tables (`JSModuleRecord::getOrMakeExecutable`, keyed in `JSGlobalObject::moduleProgramExecutables()` by key and first module-scope symbol table). They then share its `CodeBlock`s, JIT code and function declarations' executables. Such shared code is never released by run-once release (`ModuleProgramExecutable::m_isShared`, `m_recordsYetToFinishEvaluation` in `didFinishEvaluation`).
- A module whose loader has a module scope of its own (`ModuleProgramExecutable::resolvesInGlobalScope()` false) resolves that scope's variables as closure variables where records of other loaders resolve globals. So `CodeCache::getUnlinkedGlobalCodeBlock` gives it a private UCB: decoded with `RecoverableCode::No` or generated, never looked up in or added to the `CodeCache` (`privateToExecutable`), and never registered for returning to its payload.

## Proposed text

After the `m_synchronousModuleQueue` sentence, add:

> A global object can hold several module loaders, each creating its module environments under its own module scope (`JSModuleLoader::create`, used by `Bun.ModuleGraph`). Records of several loaders share a module's executable, and with it its code, when the source, the import resolutions and the module scopes' symbol tables agree (`JSModuleRecord::getOrMakeExecutable`). A module under a loader with a module scope of its own resolves that scope as closure variables, so it gets an `UnlinkedCodeBlock` of its own that never enters the `CodeCache` (`privateToExecutable` in `CodeCache::getUnlinkedGlobalCodeBlock`).
