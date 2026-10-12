mechanical

Requirement. Three places in docs/JitCache/specs/SPEC-ucb.md put `vm.Script`'s request in the run.
- Section 13.3, the row `vm-script-cached-data.js` (Bun), which has no rule ID: "`vm.Script` with `cachedData`: its block has no record; `runInThisContext` imports at the program request point".
- Section 7.2.5, first bullet (no rule ID): "`vm.Script`'s runs import at the program request point of section 7.2.1".
- Section 2, F11: "`NodeVMScript::evaluate` runs on a `ProgramExecutable` that goes through `getUnlinkedProgramCodeBlock`, never on the block decoded from `cachedData`".

THREAD Restoration says the same: "`vm.Script`'s runs, which never use that UCB, import at the program request point".

Why the code cannot meet it as written. Bun reaches the program request point when it constructs the Script, and the run makes no request. `constructScript` calls `NodeVMScript::unlinkedCodeBlockFor` before it reads `cachedData`, so that a SyntaxError throws at construction as it does in Node. That call is `CodeCache::getUnlinkedProgramCodeBlock` on a fresh `ProgramExecutable`, the request point of section 7.2.1, and the Script keeps the UCB it returns in `m_unlinkedCodeBlock`. `runInThisContext` reaches `NodeVMScript::evaluate`, which hands that UCB to `JSC::evaluate` as a precompiled block. `ProgramExecutable::initializeGlobalProperties` takes it instead of looking in the CodeCache, which is the second bullet of section 7.2.5. A Consumer therefore imports the program when the Script is constructed, and the run only installs that import on the program's newborn CodeBlock. The import could move into the run only if Bun deferred its request, and that needs a Bun edit and loses Node's construction-time SyntaxError. Section 7.2.5 and THREAD both rule that out.

What the code does instead. No engine or Bun code changes. The program is still imported where the engine requests it, and Bun's decode still yields a block with no record, as THREAD requires. JSTests/jitcache/ucb/vm-script-cached-data.js now checks the request where Bun makes it:
- The construction window counts no Generated or Decoded record, seeded decode or attach. In an importing run it counts exactly one import and one Imported record, and none elsewhere.
- The run window counts no request of any kind.
- In twins builds, jitcacheProgress `installs` shows that the run installs exactly the import, one body in an importing run and none elsewhere.

Each window holds only bare statements, because a function's first call inside a window makes a request of its own. Nothing is asked of other tasks. The SPEC only has to say that `vm.Script` requests the program at construction: the 13.3 row ("construction imports at the program request point; `runInThisContext` makes no request and installs that import"), the wording of 7.2.5's first bullet, and F11's description of `NodeVMScript::evaluate`, which runs on the UCB its construction requested through `getUnlinkedProgramCodeBlock`.

Evidence.
- `constructScript` and `NodeVMScript::unlinkedCodeBlockFor` in ~/bun/src/jsc/bindings/NodeVMScript.cpp request the program before `JSC::decodeCodeBlock<UnlinkedProgramCodeBlock>` decodes `cachedData`.
- `NodeVMScript::evaluate` in the same file passes the kept block to `JSC::evaluate`.
- `JSC::evaluate` with `precompiled` in runtime/Completion.cpp calls `Interpreter::executeProgram`, and `ProgramExecutable::initializeGlobalProperties` in runtime/ProgramExecutable.cpp takes the precompiled block when its code generation mode matches, without a CodeCache lookup.
- `decodeCodeBlockImpl` in runtime/CachedTypes.cpp has no JITCache hook.
- `GlobalRequest::tryImport` in jitcache/UCBRequests.cpp calls `importBody` in jitcache/UCBImport.cpp, which adds the Imported record, counts the import and leaves a pending import. `GlobalRequest::didServeLive` does nothing for a Producer, which does not import.
- The failing Consumer run of ladder c2-r1 (run1.stderr) shows this at construction: imports 1, records.Imported 1, counts.pendingImports 1, and Generated, Decoded, seededDecodes and attaches all 0. The Producer passed because its `produceCachedData` Script had already requested the program before the window, so its construction was a CodeCache hit.
