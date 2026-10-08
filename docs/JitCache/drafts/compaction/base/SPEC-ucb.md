# SPEC-ucb: the UCB lane

Lane 1 of THREAD's Execution: keys, the parent-key registry, every request point, the UCB core with its parse fields and child descriptors, UCB feedback with the LLInt counter, and pending imports. One subject has its own file:

- [SPEC-ucb.codec.md](SPEC-ucb.codec.md): the core codec, the JITCache mode of the bytecode cache in `runtime/CachedTypes.cpp` and the exact-layout transport it needs from `WTF::InlineMap`.

Rationale and review records live in [SPEC-ucb-history.md](SPEC-ucb-history.md), which binds nothing. Code is cited by symbol and file. "THREAD" is `docs/JitCache/THREAD.md`.

## 1. Scope and summary

The lane makes a UCB importable and capturable, and nothing else: how a body is named (its key, section 4), how a VM remembers the names of its UCBs and UFEs (the registry, section 5), where the engine asks for a UCB and what happens there (request points, section 6), what of a UCB travels and how it comes back (sections 7 and 8), and where the rest of an import waits until a CodeBlock takes it (pending imports, section 5.2). It writes three sections of each body file and reads them back. Everything else belongs to the other lanes and the integrator (THREAD Execution). The install points in `ScriptExecutable::prepareForExecutionImpl` and `JIT::compileSync` are the integrator's; this lane only stores and hands out pending imports.

Overview. Every request point computes the key of the UCB it is about to obtain from inputs it snapshots before any native step (section 6.1), and digests its context only when something reads it (section 4.3). Where the native path would generate, it imports a matching body instead: it checks key, context, provenance and the generation inputs the key does not hold, decodes the core with the bytecode cache's codec in its JITCache mode, rebuilds the butterflies generation makes from atom strings, seeds the feedback, restores the parse fields, records the UCB and its children with a pending import and returns the UCB to its native holder, which publishes it as a generated one (section 6.3.1). Where the native path decodes an embedder's bytecode cache, the decode runs unchanged, and the request matches the decoded UCB to a body by core digest, atomizes its marked string constants and seeds it before publication (section 6.3.3). Where the holder already has a live UCB, the request may attach a pending import without touching it (section 6.3.4). A miss leaves the native path as it is and records what it produced. Normal mode trusts a validated body and runs only the comparisons that choose one; strict also validates structure and references (section 11.2). At capture, the lane encodes the core, its digest and the feedback, and reports its share of the richness score (section 9).

## 2. Native facts the design rests on

Each was verified in this pin.

- F1. `UnlinkedFunctionExecutable::unlinkedCodeBlockFor` (`bytecode/UnlinkedFunctionExecutable.cpp`) first runs `decodeCachedCodeBlocks` when `m_isCached`, returns a filled slot without checking its code-generation mode, and otherwise calls the static `generateUnlinkedFunctionCodeBlock`, which parses, calls `UnlinkedFunctionExecutable::recordParse`, creates the UCB, runs `BytecodeGenerator::generate` and calls `CodeCache::updateCache`; it then sets the slot with a barrier and adds the UFE to `Heap::unlinkedFunctionExecutableSpaceAndSet.set`. `decodeCachedCodeBlocks` has no other caller. It moves `m_decoder` out of the storage it shares with `m_unlinkedCodeBlockForCall`, clears the construct slot (which shares storage with the two cached offsets), decodes each cached slot into place with `decodeFunctionCodeBlock` under its own `DeferGC`, and only then issues `WTF::storeStoreFence()`, clears `m_isCached` and write-barriers the UFE; it never adds the UFE to the clearable set. A UFE decoded from a bytecode cache has strong UCB edges (F9), `UnlinkedFunctionExecutable::visitChildrenImpl` skips both slots while `m_isCached` is set, and no compiler thread reads them. The UCBs the decode writes are therefore unpublished until `m_isCached` clears.
- F2. Eager generation for bytecode caches (`recursivelyGenerateUnlinkedCodeBlocksForFunction` and `generateUnlinkedCodeBlockForFunctions` in `runtime/CodeCache.cpp`) passes children an empty code-generation mode and generates one specialization per function, `CodeForConstruct` for a class constructor and `CodeForCall` otherwise, so a decoded UFE's other slot is empty. A class-field initializer's UFE takes its parent's whole source (`BytecodeGenerator::emitNewClassFieldInitializerFunction`), a default class constructor is created through `BuiltinExecutables::createDefaultConstructor` and appended to the parent's expression table (`BytecodeGenerator::emitNewDefaultConstructor`), and a parent's call and construct UCBs each create their own child UFEs. Nested functions therefore cannot be keyed by source range.
- F3. `ScriptExecutable::newCodeBlockFor` (`runtime/ScriptExecutable.cpp`) copies the UFE's features, lexically scoped features and captured-variables bit onto a `FunctionExecutable` right after `unlinkedCodeBlockFor` returns. `ClonedArguments::getOwnPropertySlot` and `ClonedArguments::materializeSpecials` test `usesNonSimpleParameterList()` on the executable, so in `function f(a = 0) { return arguments.callee; }` a missing feature turns the throwing callee accessor into a plain read.
- F4. `CodeCache::getUnlinkedGlobalCodeBlock` (`runtime/CodeCache.cpp`) builds a `SourceCodeKey` (`parser/SourceCodeKey.h`) whose flags keep only the strict bit of the lexically scoped features, so the with-scope bit does not enter the key; under `USE(BUN_JSC_ADDITIONS)` its `operator==` does not compare source text. `CodeCacheMap::findCacheAndUpdateAge` (`runtime/CodeCache.h`) decodes the provider's cached bytecode inside the map lookup (`fetchFromDisk`) and adds the block to the map when `useCodeCache` is on; with it off, `getUnlinkedGlobalCodeBlock` drops a decoded block and generates. A hit calls `recordParseFromUnlinkedCodeBlock`, which records the executable's parse results and copies the source-URL directives back to the provider; a generated block is added to the map and handed to `SourceProvider::cacheBytecode` and `SourceProvider::didGenerateUnlinkedCodeBlock`, which Bun's providers do not override. A decode compares the payload's stored `SourceCodeKey` with the request's (`decodeCodeBlockImpl` in `runtime/CachedTypes.cpp`), so a decoded block was generated under the request's strict bit and every other flag but possibly under the other with-scope bit, which the decoded root keeps in its own `lexicallyScopedFeatures()` (F18). `GenericCacheEntry::isUpToDate` pins the payload's build through `computeJSCBytecodeCacheVersion`; nothing records the option values or the `evalMode` its generator ran with.
- F5. Direct eval does not use the CodeCache: `JSC::eval` (`interpreter/Interpreter.cpp`) consults the caller CodeBlock's `DirectEvalCodeCache`, collects the TDZ and private-name sets from the scope chain and, under its own `DeferGC`, calls `DirectEvalExecutable::create` (`runtime/DirectEvalExecutable.cpp`), which calls `generateUnlinkedCodeBlockForDirectEval`. Its only other caller is `DebuggerCallFrame::evaluateWithScopeExtension` (`debugger/DebuggerCallFrame.cpp`), which runs with a debugger attached and so with cache activity off (THREAD Session). The Function constructor's root UFE is created by `CodeCache::getUnlinkedGlobalFunctionExecutable`; builtin roots by `BuiltinExecutables::createExecutable` (`builtins/BuiltinExecutables.cpp`, which `createBuiltinExecutable` in `builtins/BuiltinExecutableCreator.cpp` also reaches) and by `decodeBuiltinFunction` (`runtime/CachedTypes.cpp`), which Bun's `generateInternalModule` (`src/jsc/bindings/InternalModuleRegistry.cpp`) uses in standalone executables.
- F6. The decoding constructor of `UnlinkedCodeBlock` (`runtime/CachedTypes.cpp`) recreates the profile vectors zeroed and arms the LLInt counter with `thresholdForJIT(Options::thresholdForJITAfterWarmUp())`, as the generating constructor does (`bytecode/UnlinkedCodeBlock.cpp`). Neither the bytecode cache nor any constructor restores the exit profile, `didOptimize` (held by `UnlinkedMetadataTable`), the quick tier-up bits, a UFE's `m_singletonHasBeenInvalidated` (the decoding constructor of `UnlinkedFunctionExecutable` sets it false) or a `SymbolTable`'s singleton state (F14).
- F7. A native bytecode-cache encoding is not a self-contained body: it deduplicates across block regions (`Encoder::beginBlockRegion`), can write strings as ordinals into an embedder's string table (`VariableLengthObject::tryEncodeExternalString`), and encodes every generated child UCB (`CachedFunctionExecutable::encode`). The `Encoder` keeps every page in memory until `Encoder::release`. `CachedCodeBlock::create` asserts that the instruction stream is not borrowed.
- F8. Under `USE(BUN_JSC_ADDITIONS)`, `Decoder::verifiesChecksums` returns false for a payload marked persistent and for every payload when `verifyBytecodeCacheChecksums` is off, and `Decoder::regionChecksumMatches` then accepts regions unchecked. `CachedUniquedStringImplBase::decode` resolves a non-registered symbol through `BuiltinNames::lookUpPrivateName` or `lookUpWellKnownSymbol` and `RELEASE_ASSERT`s the result.
- F9. The decoding constructor of `UnlinkedFunctionExecutable` sets `m_isGeneratedFromCache`, which makes `codeBlockEdgeMayBeWeak` false; generated UFEs keep weak UCB edges under `useUnlinkedCodeBlockJettisoning` or in mini mode.
- F10. `CachedInlineMap::decode` (`runtime/CachedTypes.cpp`) calls `reserveInitialCapacity` and re-adds the entries in encoded order. Above `VariableEnvironment::inlineMapCapacity` (9) a `WTF::InlineMap` uses open addressing with triangular probing over a power-of-two table (`InlineMap::findKeyOrEmptyOrDeleted`), so a decoded map can iterate in another order than the generated one. `ProgramExecutable::initializeGlobalProperties` creates global var bindings in the iteration order of `variableDeclarations()`, which is observable through the global object's property order and through which redeclaration error is reported first.
- F11. Bun's `constructAnonymousFunction` (`src/jsc/bindings/NodeVM.cpp` in `~/bun`), behind `vm.compileFunction`, decodes `cachedData` with `JSC::decodeCodeBlock` outside the CodeCache, reports `cachedDataRejected`, links a wrapper program CodeBlock it never installs and returns `functionExpr(0)`; without `cachedData` it calls `CodeCache::getUnlinkedProgramCodeBlock`. `NodeVMScript::evaluate` runs on a `ProgramExecutable` that goes through `getUnlinkedProgramCodeBlock`, never on the block decoded from `cachedData`. The decode reads `CachedBytecode::create(std::span(options.cachedData), nullptr, {})`, a span over the host function's local `CompileFunctionOptions`, whose `Vector` is freed when the host function returns, while the decoded wrapper's cached child UFEs keep the decoder (`m_decoder`, set by the decoding constructor of `UnlinkedFunctionExecutable` in `runtime/CachedTypes.cpp`); the user function's body, decoded at its first call, therefore reads freed memory, with JITCache off as well. The host function sets the parsing context's global scope extension before calling `constructAnonymousFunction` and never clears it (`setGlobalScopeExtension`), so later requests in that realm carry the with-scope bit, which reaches every child UFE's lexically scoped features and the operand of every `call_direct_eval` (`BytecodeGenerator::computeFeaturesForCallDirectEval`).
- F12. JSC destroys a UCB or UFE while sweeping, on the thread that holds heap access: lazily in allocation, in the `IncrementalSweeper`, in `Heap::sweepSynchronously` from the collection epilogue, and in `Heap::lastChanceToFinalize`. A JITCache step that allocates a cell or ends a `DeferGC` scope can therefore run such a destructor on its own thread.
- F13. The engine has SHA-1 (`wtf/SHA1.h`) and no SHA-256.
- F14. For non-builtin code, `CodeBlock::finishCreation` clones each `SymbolTable` constant of its UCB once per realm, through `JSGlobalObject::symbolTableCache` (a `WeakGCMap`, so a dead clone is cloned again), with `SymbolTable::PropagateCloneInvalidationToOriginal::Yes`; `ModuleProgramExecutable::getUnlinkedCodeBlock` clones the module environment's table the same way. When a second scope of a clone's table is created, `SymbolTable::notifyCreation` (`runtime/SymbolTableInlines.h`) invalidates the clone's `m_singleton` and then the original's in the UCB, and `SymbolTable::cloneScopePart` makes every later clone of an invalidated original start invalidated. The DFG folds a closure scope through `singleton().inferredValue()` (`ByteCodeParser::parseBlock`, `op_resolve_scope`), and `SpeculativeJIT::compileCreateActivation` reads `singleton().isStillValid()`. `CachedSymbolTable::encode` writes no singleton state. On a thin `InferredValue`, `InferredValue::invalidate` only stores the state.
- F15. A root UFE is linked before any of its bodies is requested: `FunctionExecutable::fromGlobalCode` calls `UnlinkedFunctionExecutable::link` right after `getUnlinkedGlobalFunctionExecutable` returns, generated builtin code links `codeName##Executable()` with `codeName##Source()` (`Scripts/wkbuiltins/builtins_templates.py`), and Bun's `generateInternalModule` links the created or decoded executable with the source it made it from. `link` invalidates the new `FunctionExecutable`'s singleton when the UFE's `m_singletonHasBeenInvalidated` is set, and `FunctionExecutable::notifyCreation` sets that bit.
- F16. `JSGlobalObject::defaultCodeGenerationMode` adds `Debugger` only with an interactive debugger or with the debugger options that options.md fixes off, and `TypeProfiler` or `ControlFlowProfiler` only with those profilers, whose opcodes make baseline code unshareable and so never eligible for capture (THREAD Capture). Every captured body therefore has the empty code-generation mode.
- F17. Exit sites are added only by `ExitProfile::add`, from `OSRExitBase::considerAddingAsFrequentExitSiteSlow`, which `CodeBlock::tallyFrequentExitSites` reaches when a CodeBlock is jettisoned. Jettisons run on the VM thread, either from running code or in the collector's stopped-world phases, so a capture, which runs with JS paused and outside those phases (THREAD Capture), sees a fixed set of exit sites.
- F18. A request's native steps overwrite the lexically scoped features of its executable. `CodeCache::getUnlinkedGlobalCodeBlock` builds its `SourceCodeKey` from `executable->lexicallyScopedFeatures()` on entry; a map hit then runs `recordParseFromUnlinkedCodeBlock` and generation runs `generateUnlinkedCodeBlockImpl`, both of which call `GlobalExecutable::recordParse`, and `ScriptExecutable::recordParse` assigns `m_lexicallyScopedFeatures` from the UCB or the root node. `DirectEvalExecutable::create` receives the call site's features and generates through the same function. The root node's features are the request's plus the strict bit of a `"use strict"` directive: the `Parser` constructor seeds the root scope with the request's features, `Parser::parseSourceElements` adds the strict bit, and `Parser::parseWithStatement` taints only the `with` statement's own scope. A `ProgramExecutable` starts with no features (`ProgramExecutable::ProgramExecutable`) and gains the with-scope bit when `Interpreter::executeProgram` sees a global scope extension; a `ModuleProgramExecutable` is strict; every creator of an indirect eval passes the with-scope test of its global object, `DerivedContextType::None`, no arrow context and `EvalContextType::None`.
- F19. A string constant's atom-ness only grows. Generation makes every string constant register from an `Identifier` (`BytecodeGenerator::addStringConstant`, `BytecodeGenerator::emitLoad`), so a generated UCB's are all atoms. The native decoder makes most long string constants plain strings (SPEC-ucb.codec.md, E10). `JSString::toIdentifier`, `toAtomString` and `toExistingAtomString` (`runtime/JSString.h`), which property-key uses reach, and `DecoderStringTable::atomFor` (`runtime/CachedTypes.cpp`) later turn a resolved `JSString`'s value into its atom in place: `AtomStringImpl::add` either makes the `StringImpl` itself the atom or returns another, and then `JSString::swapToAtomString` swaps it in. `toAtomString` always leaves the cell's per-cell bit set (`JSString::markAsAtom`, through `existingAtomOrNull` or the swap, which marks even when the add made the `StringImpl` itself the atom), and value profiling reads that bit (`isDefinitelyAtom`, `speculationFromCell`). The swap allocates no cell and keeps the old `StringImpl` alive for concurrent readers (`Heap::appendPossiblyAccessedStringFromConcurrentThreadsOrGCOwnedDataScope`); it is private, and `DecoderStringTable` is a friend.
- F20. `ExecutionCounter::setThreshold` (`bytecode/ExecutionCounter.cpp`) arms `m_counter = int32(-threshold)`, truncating, and `m_totalCount = count + threshold`. The counter thus crosses once the increments add up to `trunc(threshold)` points, and `count()` right after the arming exceeds the progress it started from by the fraction it truncated. `CodeBlock::jitSoon`, which ends every successful `BaselineJITPlan::finalize`, arms through `setNewThreshold(..., this)`, whose threshold `applyMemoryUsageHeuristics` scales by `ExecutableAllocator::memoryPressureMultiplier`, a `double` of at least 1, so the progress a capture reads right after finalization is usually fractional.
- F21. A UCB's child UFE descriptors hold absolute line numbers in three fields. The lexer numbers lines from `SourceCode::firstLine()` and measures columns from the line start, which on the source's first line is the source's start (`Lexer::setCode`). Most of a UCB is relative to that start: expression info subtracts the scope's `source().firstLine()` (`BytecodeGenerator::emitExpressionInfo`), a UFE keeps `m_firstLineOffset` relative to its parent's source, and `recordParseFromUnlinkedCodeBlock` (`runtime/CodeCache.cpp`) rebases a global UCB's line count and end column on the requesting source. Three fields are absolute. A class constructor's `m_classSource` takes its first line from the class token (`Parser::parseClass`, `ASTBuilder::createClassExpr`), and `CachedFunctionExecutableRareData::packClassSource` writes it. Each `ClassElementDefinition` position keeps its absolute line, which `CachedJSTextPosition::encode` writes and `Parser::parseClassFieldInitializerSourceElements` restores into the lexer. A class field initializer's `m_firstLineOffset` comes from a default `JSTokenLocation` (line 0, `BytecodeGenerator::emitNewClassFieldInitializerFunction`), so it is minus the parent source's first line, modulo 2^31; the initializer's linked source then starts at line 1 after the clamp in `SourceCode`'s constructor, and its expression info holds the stored absolute lines. The source's start column enters no UCB: `Parser::parse` gives it only to the root node, whose start column `generateUnlinkedCodeBlockImpl` uses only for the executable's end column, and `UnlinkedFunctionExecutable::linkedStartColumn` adds the parent's column at link time. `SourceCodeKey` holds no position, so the CodeCache natively serves one UCB to sources whose first lines differ.
- F22. A `RegExp` constant is a cell the whole VM shares. Generation creates it with `RegExp::create`, and only for a valid pattern (`RegExpNode::emitBytecode`); the native decode uses `RegExp::createFromCache` for a record marked parsed (`CachedRegExp::decode`). Both return the live cell `RegExpCache::lookupOrCreate` holds for the pattern and flags. `RegExp::deleteCode` clears that cell's `m_atom` and `m_specificPattern` until its next compile, and `RegExpCache::deleteAllCode` runs it on every live `RegExp`, from `VM::deleteAllCode`, which `VM::shrinkFootprintWhenIdle` (Bun's `Bun.shrink()`, through `JSC__VM__shrinkFootprint`) and `$vm.deleteAllCodeWhenIdle()` reach. `CachedRegExp::encode` writes `m_atom`, `m_specificPattern`, `m_numSubpatterns` and `m_parsed`, which it derives from `isValid()` and `m_rareData`.
- F23. `ArrayNode::emitBytecode` (`bytecompiler/NodesCodegen.cpp`) builds a constant `JSCellButterfly` with the structure `vm.cellButterflyOnlyAtomStringsStructure` when the literal has no spread or elision and every element is an atom string, and makes each element the VM's canonical `JSString` for its atom (`vm.atomStringToJSStringMap.ensureValue`); the prefix it builds before a spread or elision takes `vm.cellButterflyStructure(indexingType)` whatever its elements, and the generator fills both under its `DeferGC`. On that structure, `Array.prototype.indexOf` (`arrayProtoFuncIndexOf`), `operationCopyOnWriteArrayIndexOfString` and the DFG and FTL lowerings of `indexOf` and `includes` (`compileArrayIndexOfOrArrayIncludes` in `SpeculativeJIT` and `FTL::LowerDFGToB3`) compare element atoms by pointer, and the runtime paths return -1 unless the search's atom is in `atomStringToJSStringMap`. `CachedImmutableButterfly::decode` creates every butterfly with `JSCellButterfly::create(vm, indexingType, length)`, which gives the plain structure, so a natively decoded UCB has no butterfly in the atom form. The baseline never embeds a butterfly: `new_array_buffer` is a slow op, and `slow_path_new_array_buffer` reads it from the CodeBlock's constant registers. That slow path and the FTL's exit materialization (`operationMaterializeObjectInOSR`) replace the CodeBlock's constant when the indexing mode has changed, never the UCB's, so a UCB's butterflies keep the structure they were created with.
- F24. A plan that finds no executable memory writes this lane's state when the VM thread finalizes it. `JIT::finalizeOnMainThread` returns `CompilationFailed` only for a null `BaselineJITCode`, which `JIT::link` returns only after `LinkBuffer::didFailToAllocate()`; `BaselineJITPlan::finalize` then calls `CodeBlock::dontJITAnytimeSoon`, which defers the body's LLInt counter indefinitely, and sets `m_didFailJITCompilation`. A DFG plan that cannot allocate gets a `FailedFinalizer` in `SpeculativeJIT::compile` or `SpeculativeJIT::compileFunction`. An FTL plan sets `FTL::State::allocationFailed`, in `FTL::compile` after `b3CodeLinkBuffer->didFailToAllocate()` and in `FTL::LowerDFGToB3::compileCallFFIImpl` when Bun's FFI invoke thunk could not be allocated (`Signature::invokeThunk` is null only then on the supported targets), and `DFG::Plan::compileInThreadImpl` then calls `FTL::fail`, which installs the same finalizer. `FailedFinalizer` records no cause, and `FTL::canCompile`, `performOSREntrypointCreation`, `canCompileUnlinked` and the `b3AlwaysFails*` options install it too. `DFG::Plan::finalize` returns `CompilationFailed` for it and then calls `m_callback->compilationDidComplete`. For a DFG plan, `JITToDFGDeferredCompilationCallback::compilationDidComplete` runs `CodeBlock::setOptimizationThresholdBasedOnCompilationResult`, whose failure case calls `dontOptimizeAnytimeSoon` on the baseline counter and `didFailDFGCompilation`, which sets the quick DFG bit to `TriState::False`. For an FTL plan, `DFG::ToFTLDeferredCompilationCallback::compilationDidComplete` and `DFG::ToFTLForOSREntryDeferredCompilationCallback::compilationDidComplete` run `DFG::JITCode::setOptimizationThresholdBasedOnCompilationResult`, which defers the DFG counter, sets the baseline CB's `m_didFailFTLCompilation` and calls `didFailFTLCompilation`, which clears the quick FTL bit. After a failed DFG plan the baseline CB is still its executable's replacement, and after a failed FTL plan it becomes the replacement again once the DFG code is jettisoned, so a later capture reads these writes.
- F25. Several native paths never read a root's text. `bun build --compile` records each JavaScript module's `StringImpl` hash (`source_hash`, under `Flags::HAS_SOURCE_HASHES` in `src/standalone_graph/StandaloneModuleGraph.rs`, whose comment says loading a module from bytecode then never has to page in its source text). The runtime wraps the text as a static external string over the executable's section (`File::to_wtf_string`) and, with `--bytecode`, passes a persistent borrowed payload and the embedded `module_info` (`src/runtime/jsc_hooks.rs`); `Zig::SourceProvider` returns the recorded hash from `hash()`. `SourceCodeKey` takes `UnlinkedSourceCode::hash()`, which is `m_provider->hash()`, and under `USE(BUN_JSC_ADDITIONS)` compares no text, so neither the CodeCache lookup nor `decodeCodeBlockImpl`'s key check reads any. In standalone executables Bun's `generateInternalModule` decodes each internal module with `decodeBuiltinFunction` from a persistent payload, which reads only the provider's length. Given generated `BuiltinSourceMetadata`, `BuiltinExecutables::createExecutable` reads only the view's length and parses the text only under `ASSERT_ENABLED || Options::validateBytecode()`; the builtins generator computes that metadata at build time from the characters it emits (`compute_builtin_source_metadata` in `Scripts/wkbuiltins/builtins_generator.py`, over the `originalSource` it concatenates into `s_JSCCombinedCode`), and global object setup creates builtins whose bodies may never run (section 6.3.6). The length of `SourceProvider::source()` is a field of the `StringImpl`, so reading it touches no character.
- F26. A UFE's `m_parentScopeTDZVariables` is a chain of `TDZEnvironmentLink`s (`parser/VariableEnvironment.h`), each holding a handle on a `CompactTDZEnvironment` that the VM interns by content in `vm.m_compactVariableMap` (`CompactTDZEnvironmentMap::get`): two live environments with the same names are one object, freed when its last handle goes. `BytecodeGenerator::getVariablesUnderTDZ` (`bytecompiler/BytecodeGenerator.cpp`) builds the chain a child UFE gets from one link per entry of the generation's TDZ stack, parented on `m_cachedParentTDZ`, and caches each link in its stack entry, so siblings share it. A function body's generation takes `m_cachedParentTDZ` from its own UFE's `parentScopeTDZVariables()` (`generateUnlinkedFunctionCodeBlock`), a direct eval's from a link over the set `JSC::eval` collected (`generateUnlinkedCodeBlockImpl`), and a program, a module or an indirect eval starts from none. Every child UFE a function body's generation creates therefore holds a chain that ends in its holder's chain object. A module's generation pushes every lexical declaration that is not a function, imports included, onto the TDZ stack before it creates the module's function declarations (`BytecodeGenerator::BytecodeGenerator` for a `ModuleProgramNode`, `pushTDZVariables`), so every function of a module, and every function nested in one, holds a chain ending in a link that names all of them; block and parameter scopes add links the same way. The bytecode cache writes a chain whole, link by link (`CachedTDZEnvironmentLink::encode`), with each environment's names sorted by content (`CachedCompactTDZEnvironment::encode`, `EncodingOrder::sort`), and shares a link or an environment only within one `Encoder`; its decode rebuilds each environment, sorts it by address and interns it (`CachedCompactTDZEnvironmentMapHandle::decode`). Only the parser, the bytecode generator and the codec read or write these objects, on the thread that holds the VM's API lock, and an environment dies with the last link that holds it, on whichever thread drops that link, which for a UFE's chain is the thread sweeping the UFE (F12).

## 3. Owned paths

New files, all in `Source/JavaScriptCore/jitcache/`, namespace `JSC::JITCache`:

| file | contents |
|---|---|
| `JITCacheSHA256.h`, `.cpp` | SHA-256 (section 4.6) |
| `UCBKeys.h`, `.cpp` | body keys, identity, context and holder digests, source digests (section 4) |
| `UCBRegistry.h`, `.cpp` | the registry, pending imports and statistics (section 5) |
| `UCBSections.h`, `.cpp` | the identity section, the atom-map checks C8 and C10 and the atomization of section 6.3.3 (section 7) |
| `UCBFeedback.h`, `.cpp` | the feedback section, its checks, seeding, richness and the LLInt counter (section 8) |
| `UCBImport.h`, `.cpp` | the engine: import, seeding of decoded UCBs, attach, records and roots (sections 6.3 and 6.7) |
| `UCBRequests.h`, `.cpp` | the request objects and hooks native call sites use (section 6.1) |
| `UCBCapture.h`, `.cpp` | the sections a capture writes (section 9) |
| `DirectEvalSite.h` | the call-site record `JSC::eval` passes down (section 6.2.4) |
| `UCBTwinGeneration.h` | declarations of the twin generation helpers, `ENABLE(JITCACHE_TWINS)` only (section 13.2) |
| `UCBTwins.h`, `.cpp` | twin comparison, `ENABLE(JITCACHE_TWINS)` only (section 13.2) |
| `UCBSelfTest.cpp` | unit self-tests, `ENABLE(JITCACHE_TWINS)` only (section 13.1) |

Unified sources bundle these files with the other parts' `jitcache/*.cpp` files (SPEC-integrator.md R-ALL-8), so every helper and constant one of them keeps to itself, `static` or in an anonymous namespace, has a name that begins with `ucb` or sits in a per-file named namespace.

Native edits this lane owns. No other part edits the members listed unless the row names that edit; where a row names a class, only the members it lists are the lane's.

| file | members | section |
|---|---|---|
| `runtime/CodeCache.h` | `CodeCacheMap::findCacheAndUpdateAge` (loses its disk branch), `CodeCacheMap::fetchFromDisk` (moves to public, unchanged), new private `CodeCache::publishGeneratedCodeBlock` | 6.2.1 |
| `runtime/CodeCache.cpp` | `CodeCache::getUnlinkedGlobalCodeBlock`, `CodeCache::getUnlinkedGlobalFunctionExecutable`, `CodeCache::publishGeneratedCodeBlock`, `generateUnlinkedCodeBlockImpl` (a parse-results out-parameter for twins), `JITCache::generateGlobalTwin` | 6.2.1, 6.2.5, 13.2 |
| `bytecode/UnlinkedFunctionExecutable.h` | private `decodeCachedCodeBlocks` (takes the request) | 6.2.2 |
| `bytecode/UnlinkedFunctionExecutable.cpp` | `unlinkedCodeBlockFor`, `decodeCachedCodeBlocks`, `~UnlinkedFunctionExecutable`, the static `generateUnlinkedFunctionCodeBlock` (a parse-results out-parameter for twins), `JITCache::generateFunctionBodyTwin` | 6.2.2, 5.5, 13.2 |
| `bytecode/UnlinkedCodeBlock.h` | new `numberOfBinaryArithProfiles`, `numberOfUnaryArithProfiles` | 8.4 |
| `bytecode/UnlinkedCodeBlock.cpp` | `~UnlinkedCodeBlock`, shared with the integrator, which adds a twins-only `JITCache::retireBodyEventCounts(*this)` right after this lane's hook (SPEC-integrator.md M10) | 5.5 |
| `runtime/DirectEvalExecutable.h`, `.cpp` | `DirectEvalExecutable::create` | 6.2.4 |
| `interpreter/Interpreter.cpp` | `JSC::eval` | 6.2.4 |
| `builtins/BuiltinExecutables.cpp` | `BuiltinExecutables::createExecutable`, the overload taking `BuiltinSourceMetadata` | 6.2.5 |
| `builtins/BuiltinExecutables.h` | `BuiltinSourceMetadata`: new `sourceDigest`, `hasSourceDigest` | 6.2.8 |
| `Scripts/wkbuiltins/builtins_generator.py`, `Scripts/wkbuiltins/builtins_generate_combined_implementation.py` | `compute_builtin_source_metadata`, `generate_source_metadata_table` | 6.2.8 |
| `bytecode/ValueProfile.h` | `UnlinkedValueProfile`: new `prediction`, `restorePrediction` | 8.4 |
| `bytecode/ArrayProfile.h` | `UnlinkedArrayProfile`: new `observedArrayModes`, `arrayProfileFlags`, `restoreAccumulatedState` | 8.4 |
| `bytecode/ArithProfile.h` | `ArithProfile`: new `restoreBits` | 8.4 |
| `bytecode/DFGExitProfile.h`, `.cpp` | `DFG::ExitProfile`: new `forEachFrequentExitSite`, `restoreFrequentExitSites` | 8.4 |
| `runtime/CachedTypes.h`, `.cpp` | the core codec, the UFE descriptor, `tdzChainDigest`, `decodeBuiltinFunction` | SPEC-ucb.codec.md |
| `parser/VariableEnvironment.h` | `CompactTDZEnvironment`: new `m_contentDigest` | 4.3 |
| `runtime/JSString.h` | `JSString`: a friend declaration of `JITCache::atomizeStringConstant`, with its forward declaration, and nothing else | 6.3.3, 7.5 |
| `Source/WTF/wtf/InlineMap.h` | `InlineMap`: the members SPEC-ucb.codec.md section 5 adds | SPEC-ucb.codec.md |
| `~/bun/src/standalone_graph/StandaloneModuleGraph.rs` | `Flags::HAS_JITCACHE_SOURCE_DIGESTS`, `File`, `to_bytes`, `from_bytes` | 6.2.8 |
| `~/bun/src/jsc/ResolvedSource.rs`, `~/bun/src/jsc/bindings/headers-handwritten.h` | `ResolvedSource`: new `jitcache_source_digest` | 6.2.8 |
| `~/bun/src/runtime/jsc_hooks.rs` | the standalone module's `ResolvedSource` | 6.2.8 |
| `~/bun/src/jsc/bindings/ZigSourceProvider.h`, `.cpp` | `Zig::SourceProvider`: the digest it keeps and `jitCacheSourceDigest` | 6.2.8 |
| `~/bun/src/codegen/bundle-modules.ts` | the builtins section it writes: a digest table and the header's `digestsOffset` | 6.2.8 |
| `~/bun/src/jsc/bindings/InternalModuleRegistry.cpp` | `makeInternalModuleSource` | 6.2.8 |

Tests: `JSTests/jitcache/ucb/` (section 13). Changes to shared hot files are manifest entries (section 15).

## 4. Keys, identities and contexts

### 4.1 The body key

A body key is 40 canonical bytes. The integrator's body-key hash reads exactly these bytes; this lane owns them (THREAD Execution).

| offset | size | field | value |
|---|---|---|---|
| 0 | 1 | `version` | 1 |
| 1 | 1 | `identityKind` | `IdentityKind` below |
| 2 | 1 | `specialization` | 0 for `CodeForCall`, 1 for `CodeForConstruct` |
| 3 | 1 | `codeGenerationMode` | `OptionSet<CodeGenerationMode>::toRaw()` of the mode the UCB stores |
| 4 | 4 | `reserved` | 0 |
| 8 | 32 | `identityDigest` | SHA-256 of the identity record of section 4.2 |

```cpp
enum class IdentityKind : uint8_t {
    Program = 1, Module = 2, IndirectEval = 3, FunctionConstructor = 4, Builtin = 5, Child = 6, DirectEval = 7,
};
enum class ChildTable : uint8_t { Declarations = 0, Expressions = 1 };
enum class CoreProvenance : uint8_t { Generated = 0, EmbedderDecoded = 1 };   // section 7.2
using Digest256 = std::array<uint8_t, 32>;

class BodyKey {
public:
    static constexpr size_t byteSize = 40;
    static BodyKey make(IdentityKind, CodeSpecializationKind, OptionSet<CodeGenerationMode>, const Digest256& identityDigest);
    // Empty when version, kind, specialization, mode bits or reserved bytes are out of range.
    static std::optional<BodyKey> fromBytes(std::span<const uint8_t, byteSize>);
    std::span<const uint8_t, byteSize> bytes() const LIFETIME_BOUND;
    IdentityKind identityKind() const;
    CodeSpecializationKind specialization() const;
    OptionSet<CodeGenerationMode> codeGenerationMode() const;
    const Digest256& identityDigest() const LIFETIME_BOUND;
    friend bool operator==(const BodyKey&, const BodyKey&) = default;
private:
    std::array<uint8_t, byteSize> m_bytes;
};
```

Program, Module, IndirectEval and DirectEval keys always have specialization 0. `fromBytes` accepts mode bits only within `CodeGenerationMode`'s defined values. The key names a body, never a tier: every tier's sections of one body share it (THREAD Capture).

### 4.2 Identity records

Each record starts with a 16-byte ASCII label without terminator, then fixed-width little-endian fields. Its SHA-256 is the key's `identityDigest`.

| kind | label | fields after the label |
|---|---|---|
| Program, Module, IndirectEval, FunctionConstructor, Builtin | `JITCache.root.v1` | `u8 kind`, `u8 strict`, `u8 withScope`, `u8 hasParameterEnd`, `i32 parameterEnd` (0 when absent), `Digest256 sourceDigest` |
| Child | `JITCache.chld.v1` | `u8[40] parentKey`, `u8 table`, `u32 index` |
| DirectEval | `JITCache.deva.v1` | `u8[40] callerKey`, `u32 bytecodeOffset`, `Digest256 textDigest` |

- `strict` and `withScope` are the `StrictModeLexicallyScopedFeature` and `TaintedByWithScopeLexicallyScopedFeature` bits of the request that generated the UCB. For program, module and indirect eval they are the executable's `lexicallyScopedFeatures()` as the request object snapshots it before any native step overwrites it (section 6.1, F18), also for a program or module the native path decoded, which gets no key when its own with-scope bit differs (section 6.3.3). For the Function constructor they come from the `lexicallyScopedFeatures` argument of `getUnlinkedGlobalFunctionExecutable`; for a builtin, from the created UFE's `lexicallyScopedFeatures()`.
- `parameterEnd` is `functionConstructorParametersEndPosition` for a Function-constructor body and absent otherwise.
- `sourceDigest` is the root source digest (section 4.4) of the root's transformed source: the request's `SourceCode` for program, module and indirect eval; the `SourceCode` passed to `getUnlinkedGlobalFunctionExecutable`, which spans the synthesized text, for the Function constructor; the `SourceCode` passed to `BuiltinExecutables::createExecutable`, or the provider's whole source in `decodeBuiltinFunction`, for a builtin.
- `parentKey` is the parent UCB's key; `table` and `index` locate the child UFE in the parent's `m_functionDecls` or `m_functionExprs`.
- `callerKey` is the key of the call site's caller UCB; `bytecodeOffset` is the call site's `BytecodeIndex::offset()`; `textDigest` is the source digest of the evaluated string.

### 4.3 Contexts

A request whose context digest differs from the body's misses (THREAD Identity). The context record is a label, then fields; its SHA-256 is the context digest. It holds every input that shapes the UCB and is in neither the key nor a child's holder UFE (below). How the UCB was produced is recorded beside it (section 7.2) and decides which UCBs the body may serve (section 6.3.2).

| request | label | fields after the label |
|---|---|---|
| Program, Module, IndirectEval | `JITCache.gctx.v1` | `u8 kind`, `u32 providerOffset`, `u32 firstLine`, `u8 scriptMode`, `u8 derivedContextType`, `u8 evalContextType`, `u8 isArrowFunctionContext` |
| body of a UFE (FunctionConstructor, Builtin, Child) | `JITCache.fctx.v1` | `u8 kind`, `u32 providerOffset`, `u32 firstLine`, `u8 isRoot`, `Digest256 rootHolderDigest` (the holder digest of a root's UFE; zero for a child) |
| DirectEval | `JITCache.dctx.v1` | `u32 providerOffset`, `u32 firstLine`, `u8 lexicallyScopedFeatures`, `u8 derivedContextType`, `u8 needsClassFieldInitializer`, `u8 privateBrandRequirement`, `u8 isArrowFunctionContext`, `u8 isInsideOrdinaryFunction`, `u8 evalContextType`, `u32 tdzCount`, TDZ names, `u32 privateNameCount`, private-name entries |

- `providerOffset` is `SourceCode::startOffset()` of the source the body is produced from: the request's source for globals and direct eval, and the `source` argument of `unlinkedCodeBlockFor` (the `FunctionExecutable`'s linked source) for UFE bodies.
- `firstLine` is `SourceCode::firstLine().oneBasedInt()` of the same source. The lexer numbers lines from it and a body's child descriptors keep three absolute lines (F21), so a body serves only requests under its own first line. The start column shapes no UCB (F21), so no context records it.
- A direct eval's `lexicallyScopedFeatures` is the request's snapshot (section 6.1), which is the call site's.
- The holder digest of a UFE is the SHA-256 of `JITCache.ufed.v1`, then its descriptor, then its TDZ chain digest. The descriptor is the bytes the core codec writes for that UFE alone, with its own TDZ chain as one start record (SPEC-ucb.codec.md, section 2 and E14); it holds the name, flags, parse mode, lexically scoped features, source positions, parameter count, the parent's private names, class element definitions and class source range, and a generator or async wrapper's parameter names (`CachedFunctionExecutable` and its rare data in `runtime/CachedTypes.cpp`), and the chain digest holds the TDZ variables, so the two cover every field generation reads from the UFE. Codec rules E1 and E2 keep out its slots and the fields its own parse sets, and the chain is fixed at creation, so the digest is the same before and after the body is generated. For a root it is THREAD's "creation parameters of the UFE that holds a root" and enters the context; for a child, C11 compares it (section 7.4).
- A TDZ chain digest is 32 zero bytes for a UFE without a chain and otherwise the digest of the chain's first link. A link's digest is the SHA-256 of `JITCache.tdzl.v1`, its environment's digest and the digest of the link it is parented on, 32 zero bytes after the last. An environment's digest is the SHA-256 of `JITCache.tdze.v1`, `u32` its name count, then each name in the order `EncodingOrder` gives (code points, then symbol kind), written as `u8 EncodingOrder::kind` and a canonical string. `tdzChainDigest` computes it (SPEC-ucb.codec.md, section 2). An environment's digest is computed when a holder digest first reaches it and kept in a new `std::unique_ptr<std::array<uint8_t, 32>> m_contentDigest` of `CompactTDZEnvironment`, which the environment frees with its names (F26, I25). The member is the only change to `parser/VariableEnvironment.h`, a header JSC exports, and has standard types only.
- A direct eval's TDZ names are sorted by their characters (`codePointCompare`), each written as a canonical string. Its private-name entries are sorted the same way by name, each a canonical string then `u16 PrivateNameEntry::bits()`.
- A canonical string is `u8 encoding` then the code units: encoding 1 is Latin-1, one byte per unit, used exactly when every unit is at most `0xFF`; encoding 2 is UTF-16LE. A length `u32` precedes the units. A null identifier is encoding 0 with length 0.

The kept environment digest serves every role and, like the registry, is not charged to the production limit (THREAD Session); a capture still charges the transient sort buffer of each environment it digests first (section 9.2).

A child's context holds only its kind, offset and first line; everything else its generation reads comes from its holder UFE, which its parent's generation made. A generated or imported parent's key and context pin that UFE, and so does a natively decoded parent's key (section 4.2) only if the payload's generator ran with the option values `start` fixes, which no key records (F4). C11 therefore compares the holder digest with the producer's wherever a match relies on generation inputs rather than on the UCB's content: at an import, and at an attach to a generated or imported UCB.

A context digest is computed only where something reads it, at most once per request, so a request whose key has no body pays nothing for its context (THREAD's opening: no work ahead of demand). Two kinds of step read one: an import, a seeding or an attach that has found a body at its key and compares contexts (sections 6.3.1 to 6.3.4), and a capture, which writes the record's (section 9.2). A record keeps a context's inputs rather than its digest (`RecordedContext`, section 4.5): the offset and first line, and for a global the four other fields of its row. The digest is a pure function of those inputs and, for a root UFE body, of its UFE's holder digest, which the reader takes from that UFE and which does not change while the UFE lives: E1 and E2 leave out its slots and the fields its own parse sets (SPEC-ucb.codec.md), and its creator sets every other field the descriptor holds before publishing it (U3). A direct eval is the exception, since its TDZ and private-name sets exist only while its request runs: its request computes the context when an import has found a body and, in a VM whose production is active, when it records the UCB, and the record keeps that digest. No request attaches to a direct eval, which never reaches a CodeCache or a UFE slot (section 6.3.4), and a VM without active production never captures, so there a record keeps the digest only when an import computed it (section 6.3.5).

### 4.4 Source digests

`Digest256 sourceDigest(StringView)` is SHA-256 of `u8 encoding` then the code units of the view, with encoding 1 (Latin-1) exactly when every unit is at most `0xFF` and encoding 2 (UTF-16LE) otherwise, so an 8-bit and a 16-bit string with the same characters digest alike. A 16-bit view is tested with `charactersAreAllLatin1` first and then streamed in narrowed chunks of 4 KiB; no copy of the source is made. Every digest of a text uses this encoding, whoever computes it.

A root's source digest comes from where its text was produced when that place recorded one, and from the text only otherwise. `rootSourceDigest` (section 4.5) takes the first form that applies:

1. A JSC builtin created from generated metadata: `BuiltinSourceMetadata::sourceDigest`, which the builtins generator computes at build time from the characters `name##Source()` spans (section 6.2.8).
2. A root whose source spans its provider (`startOffset()` 0 and `endOffset()` the length of `provider()->source()`), when `SourceProvider::jitCacheSourceDigest()` returns a digest. Bun returns one for the modules `bun build --compile` embeds and for its internal modules (section 6.2.8).
3. Otherwise `sourceDigest` of the root's view, which reads every character.

The first two forms keep the paths of F25 from reading text the native path never reads. A direct eval's text digest and a Function-constructor root's are always computed, since those texts are made at run time.

A supplied digest is trusted as JSC natively trusts `SourceProvider::hash()` and an embedder's bytecode payload; it is no part of the artifact, so it is an assumption, and THREAD Session lets the SPEC decide whether strict verifies it. The lane verifies each form where it can fail. Form 1 belongs to the build: self-test U9 checks every builtin's entry and debug builds assert it whenever a builtin root is recorded; strict leaves it alone, as it leaves the codec's round trip (section 11.2). Form 2 belongs to the embedder's output, where a mistake could give two texts one key. A natively decoded UCB is matched by its core, so a wrong key can only make it miss (section 6.3.2), and S1 compares the core of an attached generated or imported UCB, but an import replaces generation on the key alone. With strict on, an import therefore verifies the supplied digest its key rests on before it decodes anything, once per provider in a VM (S2, section 6.3.1 step 4); with strict off the lane trusts it. In a VM that imports with strict on, the registry keeps beside every key derived from such a root a reference to the provider whose digest the root used (section 5.1), so a function body of that tree is verified at its own import. A direct eval needs no such check: its key ends in the digest of its own text, which its request computes, and its generation reads only that text and its context (section 4.3), so a wrong supplied digest upstream can change only the caller key it is found under; the functions nested in its text are pinned by that digest too. Twins builds with a twin report open check every supplied digest they take, whatever strict says (T7).

### 4.5 Functions

`UCBKeys.h` declares:

```cpp
Digest256 rootIdentityDigest(IdentityKind, const Digest256& sourceDigest, LexicallyScopedFeatures, std::optional<int32_t> parameterEnd);
Digest256 childIdentityDigest(const BodyKey& parent, ChildTable, uint32_t index);
Digest256 directEvalIdentityDigest(const BodyKey& caller, BytecodeIndex, const Digest256& textDigest);
Digest256 globalContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, JSParserScriptMode, DerivedContextType, EvalContextType, bool isArrowFunctionContext);
Digest256 executableBodyContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, const std::optional<Digest256>& rootHolderDigest);
// Empty only when a budget refused a page of the descriptor encoding or the sort buffer of an environment it digested first
// (SPEC-ucb.codec.md, section 2); requests pass none. `environmentsDigested`, when given, gains the number of environments
// this call digested first (section 4.3).
std::optional<Digest256> holderDigest(VM&, const UnlinkedFunctionExecutable&, CoreEncodingBudget* = nullptr, unsigned* environmentsDigested = nullptr);
Digest256 directEvalContextDigest(unsigned providerOffset, unsigned firstLine, LexicallyScopedFeatures, DerivedContextType, NeedsClassFieldInitializer, PrivateBrandRequirement,
    bool isArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType, const TDZEnvironment&, const PrivateNameEnvironment&);

// What a record keeps of its context (section 4.3): a global's or a UFE body's inputs, or a direct eval's digest.
struct GlobalContextInputs {   // Program, Module, IndirectEval: globalContextDigest's arguments after the kind
    unsigned providerOffset;
    unsigned firstLine;
    JSParserScriptMode scriptMode;
    DerivedContextType derivedContextType;
    EvalContextType evalContextType;
    bool isArrowFunctionContext;
};
struct BodyContextInputs {     // FunctionConstructor, Builtin, Child; a root's holder digest comes from its UFE
    unsigned providerOffset;
    unsigned firstLine;
};
struct DirectEvalContext {
    std::optional<Digest256> digest;   // the digest its request computed, if it computed one
};
using RecordedContext = std::variant<GlobalContextInputs, BodyContextInputs, DirectEvalContext>;
// The context digest of a record whose key has the given identity kind: globalContextDigest or executableBodyContextDigest
// of the inputs, with rootHolderDigest for a FunctionConstructor or Builtin body, or a direct eval's stored digest. Empty
// for a direct eval that stored none and for a root body passed no holder digest.
std::optional<Digest256> contextDigestOf(IdentityKind, const RecordedContext&, const std::optional<Digest256>& rootHolderDigest);

Digest256 sourceDigest(StringView);

enum class SourceDigestOrigin : uint8_t { Computed, BuiltinMetadata, Provider };
struct RootSourceDigest { Digest256 digest; SourceDigestOrigin origin; };
// Section 4.4: *builtinMetadataDigest when given (form 1), else the provider's jitCacheSourceDigest() when rootSource
// spans its provider (form 2), else sourceDigest(rootSource.view()) (form 3).
RootSourceDigest rootSourceDigest(const SourceCode& rootSource, const Digest256* builtinMetadataDigest = nullptr);
```

All but `holderDigest` are pure, take no lock, allocate no cell and run on any thread; `rootSourceDigest` calls only the provider's const accessors. `holderDigest` hashes the codec's descriptor encoding page by page (SPEC-ucb.codec.md, E13) and then the chain digest `tdzChainDigest` returns, on the VM thread; it allocates no cell, and the only state it writes is the digests it keeps in environments (section 4.3).

### 4.6 SHA-256

`JITCacheSHA256.h` declares a streaming FIPS 180-4 SHA-256: `SHA256()`, `void update(std::span<const uint8_t>)`, `Digest256 finalize()` and `static Digest256 hash(std::span<const uint8_t>)`. The portable implementation is the reference. On x86_64 a path using the SHA extensions runs when CPUID reports SHA, SSSE3 and SSE4.1; on ARM64 a path using the SHA-256 instructions runs when `getauxval(AT_HWCAP)` reports `HWCAP_SHA2`. The choice is made once per process. Self-test U1 (section 13.1) checks the FIPS 180-4 examples on every path the CPU offers and compares the paths over random lengths. Other parts may use this class. `Digest256` is declared in `UCBKeys.h`, which `JITCacheSHA256.h` includes.

## 5. The parent-key registry, pending imports and statistics

### 5.1 What it holds

One `UCBRegistry` per VM, owned by the integrator's per-VM state (R-INT-1), created when `start` configures the VM and destroyed after `Heap::lastChanceToFinalize`. It serves every role and is not charged to the production limit (THREAD Session). It holds three maps from raw cell pointers, none of which is a GC root or holds a cell, and the set of supplied digests it has verified:

```cpp
// suppliedDigestProvider, in each type below: the provider whose supplied digest (form 2 of section 4.4) an import at the
// key relies on, kept only in a VM that imports with strict on, so that the import can verify it (section 6.3.1, step 4);
// else null, and always null for a direct eval and everything nested in one (section 4.4).
// What the children of one recorded UCB share: its key and suppliedDigestProvider, kept once per parent and alive while
// any of its child UFEs has an entry.
class ParentIdentity final : public ThreadSafeRefCounted<ParentIdentity> {
public:
    static Ref<ParentIdentity> create(const BodyKey&, RefPtr<SourceProvider>&& suppliedDigestProvider);
    const BodyKey& key() const LIFETIME_BOUND;
    SourceProvider* suppliedDigestProvider() const;
private:
    ParentIdentity(const BodyKey&, RefPtr<SourceProvider>&&);
    BodyKey m_key;
    RefPtr<SourceProvider> m_suppliedDigestProvider;
};
struct ChildIdentity { RefPtr<ParentIdentity> parent; ChildTable table; uint32_t index; };   // parent never null; 16 bytes
struct RootIdentity { IdentityKind kind; Digest256 identityDigest; RefPtr<SourceProvider> suppliedDigestProvider; };   // FunctionConstructor or Builtin
using ExecutableIdentity = std::variant<ChildIdentity, RootIdentity>;

enum class UCBOrigin : uint8_t { Generated, Decoded, Imported };   // what produced the UCB in this VM; a capture of a Decoded one writes provenance EmbedderDecoded unless its record keeps generatedButterflyMap (section 9.2)
enum class ImportResolution : uint8_t { Installed, DroppedByGate };

struct UCBRecord {
    BodyKey key;
    RecordedContext context;   // the context's inputs, or a direct eval's digest (section 4.3)
    UCBOrigin origin;
    LexicallyScopedFeatures keyFeatures { NoLexicallyScopedFeatures };   // a program, module or indirect eval: the strict and with-scope bits of its key
    std::optional<uint64_t> missedBodyVersion;    // the index token at which no body fitted the UCB (section 6.3.2, R-INT-3)
    std::optional<uint64_t> matchedBodyVersion;   // the commit identifier of the body file the UCB was imported, seeded or attached from, or matched in every check but C10 (sections 6.3.2 and 6.3.4)
    RefPtr<PendingImport> pendingImport;          // at most one
    RefPtr<SourceProvider> suppliedDigestProvider;
    // Origin Decoded, in a VM whose production is active: the butterfly map of the body of provenance Generated the UCB
    // was last seeded or attached from (sections 6.3.3, 6.3.4 and 9.2); ceil(N / 8) bytes.
    std::optional<Vector<uint8_t>> generatedButterflyMap;
};

class UCBRegistry {
    WTF_MAKE_NONCOPYABLE(UCBRegistry);
    WTF_MAKE_TZONE_ALLOCATED(UCBRegistry);
public:
    // WTF_MAKE_NONCOPYABLE suppresses the implicit default constructor; the integrator's VM state holds the registry by value
    // (SPEC-integrator.md section 5.1).
    UCBRegistry() = default;

    // UFE identities. VM thread.
    void recordRootExecutable(const UnlinkedFunctionExecutable&, const RootIdentity&);
    std::optional<ExecutableIdentity> identityOf(const UnlinkedFunctionExecutable&) const;

    // UCB records. VM thread.
    // Records the UCB and gives each child UFE of its two tables a ChildIdentity, in table order, all referencing one
    // ParentIdentity made from the record's key and suppliedDigestProvider (none for a UCB without children).
    // A UCB that already has a record keeps it (THREAD: a UCB keeps the key it was recorded under); returns false then.
    bool recordCodeBlock(UnlinkedCodeBlock&, UCBRecord&&);
    struct RecordView {
        BodyKey key;
        RecordedContext context;
        UCBOrigin origin;
        LexicallyScopedFeatures keyFeatures;
        std::optional<uint64_t> missedBodyVersion;
        std::optional<uint64_t> matchedBodyVersion;
        bool hasPendingImport;
        RefPtr<SourceProvider> suppliedDigestProvider;
    };
    std::optional<RecordView> recordOf(const UnlinkedCodeBlock&) const;
    std::optional<BodyKey> keyOf(const UnlinkedCodeBlock&) const;
    void setMissedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // A miss at C10 alone (section 6.3.2): the commit identifier of the file the UCB matched in every other check.
    void setMatchedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // False when the UCB has no record or already has a pending import. On success the record's matchedBodyVersion becomes
    // the commit identifier of the import's body file, and its generatedButterflyMap becomes the argument (section 6.3.4).
    bool attachPendingImport(const UnlinkedCodeBlock&, Ref<PendingImport>&&, std::optional<Vector<uint8_t>>&& generatedButterflyMap);
    // Section 9.2: copies the record's generatedButterflyMap into `out`, whose size is ceil(N / 8), and returns true;
    // returns false, copying nothing, when the record keeps none.
    bool copyGeneratedButterflyMap(const UnlinkedCodeBlock&, std::span<uint8_t> out) const;
    RefPtr<PendingImport> pendingImport(const UnlinkedCodeBlock&) const;
    // Detaches the import if it is still the attached one. DroppedByGate also sets missedBodyVersion to the import's index
    // token and counts statistics().gateDrops (section 5.2).
    void resolvePendingImport(const UnlinkedCodeBlock&, const PendingImport&, ImportResolution);

    // Supplied digests an import verified (section 6.3.1, step 4), by the provider's SourceID, which no other provider
    // ever takes. VM thread.
    bool suppliedDigestVerified(SourceID) const;
    void markSuppliedDigestVerified(SourceID);

    // Destructors: the thread sweeping the VM's heap (F12).
    void unlinkedCodeBlockDestroyed(const UnlinkedCodeBlock*);
    void unlinkedFunctionExecutableDestroyed(const UnlinkedFunctionExecutable*);

    struct Counts { size_t children; size_t roots; size_t codeBlocks; size_t pendingImports; };
    Counts counts() const;

    UCBStatistics& statistics() LIFETIME_BOUND;   // VM thread only (section 5.6)

private:
    mutable Lock m_lock;
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, ChildIdentity> m_children WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, RootIdentity> m_roots WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedCodeBlock*, UCBRecord> m_codeBlocks WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashSet<SourceID> m_verifiedSuppliedDigests WTF_GUARDED_BY_LOCK(m_lock);
    UCBStatistics m_statistics;   // not under m_lock: only the VM thread's engine and readers touch it
};
```

A child's identity digest is computed when its body is requested (section 6.7), from the stored parent key, table and index. `recordCodeBlock` keeps the record's key and `suppliedDigestProvider` once, in a `ParentIdentity` all its children reference, so a child's entry is a reference, its table and its index, where a copied 40-byte key would cost about as much as the UFE cell (`static_assert(sizeof(UnlinkedFunctionExecutable) <= 96)`) in a table at most half full (`largeMaxLoadNumerator` and `largeMaxLoadDenominator` in `wtf/HashTable.h`). Since a child UFE can outlive its parent UCB (the executable of a function that escaped keeps it), the node outlives the parent's record while a child references it. These references keep a provider alive while a UFE or UCB of its tree lives; the providers that supply digests (section 6.2.8) hold text that lives in the executable's image, so only the provider object stays. `SourceProvider::asID()` comes from a process-wide counter that never repeats a value (`SourceProvider::getID`), so a verified `SourceID` can never stand for another provider. `recordCodeBlock` reads the UCB's tables with `functionDecl(i)` and `functionExpr(i)` before taking the lock and inserts under it; a child that already has an identity keeps it, and in debug builds the stored and derived identities are asserted equal.

### 5.2 Pending imports

```cpp
class PendingImport final : public ThreadSafeRefCounted<PendingImport> {
public:
    enum class Origin : uint8_t { Seeded, Reused };   // Seeded: the UCB's feedback came from this body before publication. Reused: a live UCB, not seeded.
    static Ref<PendingImport> create(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    ValidatedBody& body() const;          // pins the validated payload (THREAD Storage)
    const BodyKey& key() const;
    uint64_t indexToken() const;          // the bodyVersion token the import was made at (R-INT-3)
    Origin origin() const;
    ~PendingImport();                     // out of line, in UCBRegistry.cpp, where ValidatedBody is complete
private:
    PendingImport(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    const Ref<ValidatedBody> m_body;
    const BodyKey m_key;
    const uint64_t m_indexToken;
    const Origin m_origin;
};
```

`ValidatedBody` is the integrator's handle on a validated body file (R-INT-3). A pending import lives in its UCB's record from the moment the request point records it until the install glue resolves it, and otherwise goes with the UCB (THREAD Restoration). It never enters `UnlinkedCodeBlock::m_unlinkedBaselineCode`.

The install glue (integrator) uses it this way, on the VM thread, inside the door's GC deferral:

- `pendingImport(ucb)` at an install point, for a newborn CodeBlock whose UCB's sharing slot is empty;
- `resolvePendingImport(ucb, import, Installed)` after `installCode` succeeded;
- `resolvePendingImport(ucb, import, DroppedByGate)` after the `shouldJIT` gate dropped the import. The gate would drop a re-attached import again (THREAD Restoration), so the registry stamps the import's index token as missed and later requests for that UCB read nothing until the index lists another body at its key (section 6.3.4);
- nothing after a baked-fact mismatch, which keeps it for the next newborn CodeBlock, or after invalid material, which leaves it to die with its UCB.

Both origins install the same way. A `Seeded` one finished its UCB part (core, seeds and, for an import, parse fields) before the UCB was published; a `Reused` one has no UCB part.

### 5.3 Lock rules

`m_lock` is a leaf: no other lock is taken while it is held, so a thread that holds it never waits on anything but the CPU. Inside its critical sections no code allocates a cell, enters or leaves a `DeferGC` or `DeferGCForAWhile` scope, runs a write barrier, calls into the collector or the integrator, destroys a `PendingImport` or drops a reference to a `SourceProvider` or a `ParentIdentity`, whose last reference drops a provider: a destructor hook moves such references out under the lock and lets them go after unlocking. Taking a reference under the lock is an atomic increment and is allowed, and so are malloc and free. These rules keep a JITCache step from deadlocking on its own lock when a sweep runs a UCB or UFE destructor on its thread (F12). The SHA-256 work of a request runs before the lock is taken.

No fence is needed beyond the lock. Every map access holds `m_lock`; the activity answers of R-INT-1 and the statistics change and are read on the VM thread, and the destructor hooks read only whether the state exists. A UCB the lane imports or seeds reaches other threads only through its native publication, whose barriers and fences are unchanged.

### 5.4 Who records what

| event | records |
|---|---|
| a Function-constructor UFE is created (`CodeCache::getUnlinkedGlobalFunctionExecutable`) | its `RootIdentity` (section 6.3.6) |
| a builtin UFE is created or decoded (`BuiltinExecutables::createExecutable`, `decodeBuiltinFunction`), except a default class constructor | likewise |
| a request point generates, decodes or imports a UCB whose key it knows | its `UCBRecord` (origin, key features of a global root, and the index token it missed at or the commit identifier it was matched at, if any), its children's `ChildIdentity` |
| `decodeCachedCodeBlocks` decodes a slot the request did not ask for | that UCB's `UCBRecord` with origin `Decoded`, under its own specialization and mode, without settling the request (section 6.1) |

A UFE or UCB created before `start`, or nested in one, has no record and misses forever (THREAD Session and Identity). A UCB served to a request with another key keeps its first record. Recording stops for good when cache activity turns off; existing records stay until their owners die.

### 5.5 Destructor hooks

`UnlinkedCodeBlock::~UnlinkedCodeBlock` (`bytecode/UnlinkedCodeBlock.cpp`) calls `JITCache::unlinkedCodeBlockWillBeDestroyed(*this)` first; `UnlinkedFunctionExecutable::~UnlinkedFunctionExecutable` calls `JITCache::unlinkedFunctionExecutableWillBeDestroyed(*this)` first. Each reads `vm()` (valid during the sweep), returns when the VM has no JITCache state, and otherwise removes the cell's entries from every map. The registry outlives every cell of its VM, which R-INT-1 guarantees by destroying the state after `Heap::lastChanceToFinalize`.

### 5.6 Statistics

```cpp
enum class MissReason : uint8_t { NoKey, NoBody, BodyUnchanged, RequestKey, Context, Provenance, Holder, AtomMap, CoreDigest, Stack };
enum class InvalidMaterialStep : uint8_t { Identity, Feedback, Decode, Closure, StrictCore, SuppliedDigest };   // ucb.identity ... ucb.supplied-digest

struct UCBStatistics {
    uint64_t imports { 0 };        // section 6.3.1 succeeded
    uint64_t seededDecodes { 0 };  // section 6.3.3 succeeded
    uint64_t attaches { 0 };       // section 6.3.4 succeeded
    uint64_t gateDrops { 0 };      // pending imports the shouldJIT gate dropped (section 5.2)
    uint64_t sourceDigests { 0 };  // root source digests computed from text (form 3 of section 4.4); a direct eval's is not counted
    uint64_t suppliedSourceDigests { 0 };        // root digests taken from builtin metadata or a provider (forms 1 and 2)
    uint64_t suppliedDigestVerifications { 0 };  // supplied digests an import verified (section 6.3.1, step 4)
    uint64_t contextDigests { 0 };               // context digests computed, by requests and captures (section 4.3)
    uint64_t holderDigests { 0 };                // holder digests computed, by requests (C11, a root's context) and captures
    uint64_t tdzEnvironmentDigests { 0 };        // TDZ environments digested, each once while it lives (section 4.3)
    std::array<uint64_t, 10> misses { };           // indexed by MissReason
    std::array<uint64_t, 3> records { };           // indexed by UCBOrigin
    std::array<uint64_t, 6> invalidMaterial { };   // indexed by InvalidMaterialStep
};
```

The engine (section 6.7) increments the counters on the VM thread, at the outcome each section names, `resolvePendingImport` counts `gateDrops`, and `buildSections` counts the context and holder digests a capture computes and the environments they digest first (section 9.2); nothing else writes them. The integrator's `status` reads `UCBRegistry::statistics()` and `counts()` (section 10.1), and tests read both through `$vm.jitCacheUCBStatistics()` (manifest M4).

## 6. Request points

### 6.1 Request objects

Native call sites create one request object per request and tell it what happened. `UCBRequests.h` declares them; each member is a thin call into the engine of section 6.7, defined in `UCBRequests.cpp`. Every member is a no-op unless the request is active (the VM had JITCache state that tracks keys when the request began: one load and one test) and not settled. The engine re-checks the VM's state at every step, so a request that sees cache activity turn off midway stops acting.

A request settles once it has recorded, imported, seeded or attached the UCB it asked for: the one UCB of a global or direct-eval request, and the UCB of the requested specialization for a UFE body. `didDecodeCachedSlots` also records the other decoded slot (section 6.3.5), which neither reads nor sets `settled`. A request whose requested slot the decode left empty, the common case when a function cached for call is first constructed (F2), therefore goes on to import or generate that slot and records it.

The constructors of `GlobalRequest` and `DirectEvalRequest` run before any native step of their request and snapshot the executable's `lexicallyScopedFeatures()` into `RequestState::requestFeatures`. That is the value the native `SourceCodeKey` is built from (F4) and the call site's features for a direct eval, and the native steps that follow overwrite the executable's field with the parse's (F18). Keys and contexts read only the snapshot, so every role computes the same key for a request, before or after generation and after a CodeCache hit.

```cpp
enum class RequestKind : uint8_t { Program, Module, IndirectEval, FunctionBody, DirectEval };

// One per request, inside the request object on the native call site's stack. The constructors below fill the
// inputs; only the engine (section 6.7) reads them and writes the rest.
struct RequestState {
    WTF_MAKE_NONCOPYABLE(RequestState);
    RequestState(VM&, RequestKind, const SourceCode&, OptionSet<CodeGenerationMode> requestMode, LexicallyScopedFeatures requestFeatures);

    VM& vm;
    const RequestKind kind;
    const SourceCode& source;
    const OptionSet<CodeGenerationMode> requestMode;
    const LexicallyScopedFeatures requestFeatures;               // every kind but FunctionBody: the snapshot above; FunctionBody: none
    GlobalExecutable* globalExecutable { nullptr };              // every kind but FunctionBody
    UnlinkedFunctionExecutable* functionExecutable { nullptr };  // FunctionBody
    CodeSpecializationKind specialization { CodeSpecializationKind::CodeForCall };
    JSParserScriptMode scriptMode { JSParserScriptMode::Classic };
    EvalContextType evalContextType { EvalContextType::None };
    const DirectEvalSite* site { nullptr };                      // DirectEval; null means no key
    const TDZEnvironment* variablesUnderTDZ { nullptr };         // DirectEval
    const PrivateNameEnvironment* privateNameEnvironment { nullptr };   // DirectEval

    bool active { false };
    bool settled { false };
    std::optional<uint64_t> missedBodyVersion;   // the index token tryImport missed at; didGenerate records it
    std::optional<Digest256> sourceDigest;       // roots: the root source digest of section 4.4; direct eval: of the evaluated text; taken once
    std::optional<std::optional<BodyKey>> key;   // the request's key, computed once (section 6.7): outer empty, not yet; inner empty, no key
    RefPtr<SourceProvider> suppliedDigestProvider;   // set with the key: the provider an import at the key relies on (section 5.1); none for a direct eval
    std::optional<Digest256> context;            // computed at most once, only when read (section 4.3)
    std::optional<Digest256> holderDigest;       // FunctionBody: holderDigest of functionExecutable, computed at most once (C11, a root's context)
};

class GlobalRequest {   // program, module, indirect eval
    WTF_MAKE_NONCOPYABLE(GlobalRequest);
public:
    GlobalRequest(VM&, GlobalExecutable&, const SourceCode&, SourceCodeType, JSParserScriptMode, OptionSet<CodeGenerationMode>, EvalContextType);
    bool isActive() const { return m_state.active; }
    void didServeLive(UnlinkedGlobalCodeBlock&);   // a CodeCache map hit (section 6.3.4)
    void didDecode(UnlinkedGlobalCodeBlock&);      // the provider's cached bytecode, before the map holds it (section 6.3.3)
    // The native path would generate. Returns an imported UCB with parse fields restored, feedback seeded and its record and
    // pending import in place, or null for a miss or after invalid material. Its class is the request's (check C1).
    UnlinkedGlobalCodeBlock* tryImport();
    void didGenerate(UnlinkedGlobalCodeBlock&);
private:
    RequestState m_state;
};

class FunctionBodyRequest {   // the body of any UFE: child, Function constructor, builtin
    WTF_MAKE_NONCOPYABLE(FunctionBodyRequest);
public:
    FunctionBodyRequest(VM&, UnlinkedFunctionExecutable&, const SourceCode&, CodeSpecializationKind, OptionSet<CodeGenerationMode>);
    bool isActive() const { return m_state.active; }
    // Inside decodeCachedCodeBlocks, after both cached slots are decoded and before m_isCached clears (F1). Seeds the requested
    // specialization's UCB (section 6.3.3) and records the other one as Decoded without settling the request.
    void didDecodeCachedSlots(UnlinkedFunctionCodeBlock* forCall, UnlinkedFunctionCodeBlock* forConstruct);
    void didServeLive(UnlinkedFunctionCodeBlock&);   // the requested slot was filled
    UnlinkedFunctionCodeBlock* tryImport();          // as GlobalRequest::tryImport; restores the UFE's parse fields
    void didGenerate(UnlinkedFunctionCodeBlock&);
private:
    RequestState m_state;
};

class DirectEvalRequest {
    WTF_MAKE_NONCOPYABLE(DirectEvalRequest);
public:
    // Inactive when site is null.
    DirectEvalRequest(VM&, DirectEvalExecutable&, const SourceCode&, const DirectEvalSite*, OptionSet<CodeGenerationMode>, EvalContextType,
        const TDZEnvironment* variablesUnderTDZ, const PrivateNameEnvironment*);
    bool isActive() const { return m_state.active; }
    UnlinkedEvalCodeBlock* tryImport();
    void didGenerate(UnlinkedEvalCodeBlock&);
private:
    RequestState m_state;
};

void didCreateFunctionConstructorExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, LexicallyScopedFeatures, std::optional<int> parametersEndPosition);
void didCreateBuiltinExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, const BuiltinSourceMetadata&);
void didDecodeBuiltinExecutable(VM&, UnlinkedFunctionExecutable&, SourceProvider&);
void unlinkedCodeBlockWillBeDestroyed(UnlinkedCodeBlock&);
void unlinkedFunctionExecutableWillBeDestroyed(UnlinkedFunctionExecutable&);

// DirectEvalSite.h
struct DirectEvalSite {
    UnlinkedCodeBlock* callerUnlinkedCodeBlock;
    BytecodeIndex bytecodeIndex;
};
```

`GlobalRequest` maps `SourceCodeType::ProgramType`, `ModuleType` and `EvalType` to `Program`, `Module` and `IndirectEval`. A request takes its source digest and computes its key once, on first use, so a request that ends in a map hit does neither unless an attach is about to follow (section 6.3.4); its context and holder digest follow section 4.3. `UCBRequests.h` forward-declares `BuiltinSourceMetadata`.

### 6.2 Hooks

#### 6.2.1 Program, module and indirect eval

`CodeCacheMap::findCacheAndUpdateAge` (`runtime/CodeCache.h`) keeps its signature and loses the disk branch: it prunes, looks the key up and updates ages. `CodeCacheMap::fetchFromDisk` becomes public, unchanged; it decodes and adds nothing to the map. `CodeCache::getUnlinkedGlobalFunctionExecutable` keeps calling `findCacheAndUpdateAge` only, since `fetchFromDisk` never decodes a UFE. `CodeCache::getUnlinkedGlobalCodeBlock` becomes, under `USE(BUN_JSC_ADDITIONS)`:

```cpp
SourceCodeKey key(...);   // unchanged
// Snapshots executable->lexicallyScopedFeatures(), the features the key above was built from, before any step below overwrites them.
JITCache::GlobalRequest jitCacheRequest(vm, *executable, source, CacheTypes<UnlinkedCodeBlockType>::codeType, scriptMode, codeGenerationMode, evalContextType);
UnlinkedCodeBlockType* unlinkedCodeBlock = m_sourceCode.findCacheAndUpdateAge<UnlinkedCodeBlockType>(vm, key);
if (unlinkedCodeBlock && Options::useCodeCache()) {
    recordParseFromUnlinkedCodeBlock(executable, source, unlinkedCodeBlock);
    jitCacheRequest.didServeLive(*unlinkedCodeBlock);
    return unlinkedCodeBlock;
}
if (!unlinkedCodeBlock) {
    unlinkedCodeBlock = m_sourceCode.fetchFromDisk<UnlinkedCodeBlockType>(vm, key);
    if (unlinkedCodeBlock && Options::useCodeCache()) {
        jitCacheRequest.didDecode(*unlinkedCodeBlock);   // seeds it before the map publishes it (section 6.3.3)
        m_sourceCode.addCache(key, SourceCodeValue(vm, unlinkedCodeBlock, m_sourceCode.age()));
        recordParseFromUnlinkedCodeBlock(executable, source, unlinkedCodeBlock);
        return unlinkedCodeBlock;
    }
}
if (auto* imported = jitCacheRequest.tryImport()) {
    auto* result = uncheckedDowncast<UnlinkedCodeBlockType>(imported);
    publishGeneratedCodeBlock(vm, key, result);   // the three steps below, shared with generation
    return result;
}
unlinkedCodeBlock = generateUnlinkedCodeBlock<UnlinkedCodeBlockType, ExecutableType>(vm, executable, source, scriptMode, codeGenerationMode, error, evalContextType);
if (unlinkedCodeBlock)
    jitCacheRequest.didGenerate(*unlinkedCodeBlock);
publishGeneratedCodeBlock(vm, key, unlinkedCodeBlock);   // no-op for null
return unlinkedCodeBlock;
```

`publishGeneratedCodeBlock` is the existing tail factored out: when `useCodeCache` is on, `addCache`, `SourceProvider::cacheBytecode` with `encodeCodeBlock`, and `SourceProvider::didGenerateUnlinkedCodeBlock`. With the VM not tracking keys, the sequence is the native one, map, then disk, then generation, including the native drop of a decoded block when `useCodeCache` is off.

#### 6.2.2 UFE bodies

`UnlinkedFunctionExecutable::unlinkedCodeBlockFor` becomes:

```cpp
JITCache::FunctionBodyRequest jitCacheRequest(vm, *this, source, specializationKind, codeGenerationMode);
if (m_isCached)
    decodeCachedCodeBlocks(vm, jitCacheRequest);
if (auto* codeBlock = (specializationKind == CodeSpecializationKind::CodeForCall ? m_unlinkedCodeBlockForCall : m_unlinkedCodeBlockForConstruct).get()) {
    jitCacheRequest.didServeLive(*codeBlock);   // returns at once when the decode above settled the request
    return codeBlock;
}
UnlinkedFunctionCodeBlock* result = jitCacheRequest.tryImport();
if (result)
    vm.codeCache()->updateCache(this, source, specializationKind, result);
else {
    result = generateUnlinkedFunctionCodeBlock(vm, this, source, specializationKind, codeGenerationMode,
        isBuiltinFunction() ? UnlinkedBuiltinFunction : UnlinkedNormalFunction, error, parseMode);
    if (error.isValid())
        return nullptr;
    jitCacheRequest.didGenerate(*result);
}
// unchanged: set the slot with a barrier, add this UFE to unlinkedFunctionExecutableSpaceAndSet.set, return result
```

The private `decodeCachedCodeBlocks(VM&)` becomes `decodeCachedCodeBlocks(VM&, JITCache::FunctionBodyRequest&)` (the header forward-declares the class) and gains one call between the decodes and publication:

```cpp
// unchanged: move the decoder out, DeferGC, clear the construct slot, decode each cached slot into place
jitCacheRequest.didDecodeCachedSlots(m_unlinkedCodeBlockForCall.get(), m_unlinkedCodeBlockForConstruct.get());
WTF::storeStoreFence();
m_isCached = false;
vm.writeBarrier(this);
```

At that call both slots hold their decoded UCBs, or null where nothing was cached or a damaged block was rejected, and neither is published (F1). The requested specialization's UCB is seeded there (section 6.3.3). The other is recorded as `Decoded` without settling the request and meets a later request of its own as a live UCB, which attaches (section 6.3.4): its pending import still brings the image, the CB state and the IC state, but that body's UCB feedback (arithmetic profiles, exit sites, quick tier-up bits, `didOptimize`, the LLInt counter and its children's singleton bits) never arrives, since seeding it here would read the body of a specialization nobody has requested, which THREAD rules out ("does no work ahead of demand"). A requested slot the decode left empty goes on to the import or generation below and is recorded there (section 6.1).

#### 6.2.3 Request keys

| request | key | context |
|---|---|---|
| program | `Program`, call, the UCB's mode, root identity of `source` with the snapshot's strict and with-scope bits | `globalContextDigest(Program, source.startOffset(), source.firstLine().oneBasedInt(), Classic, executable.derivedContextType(), None, executable.isArrowFunctionContext())` |
| module | `Module`, likewise | the same with `Module` and script mode `Module` |
| indirect eval | `IndirectEval`, likewise | the same with the request's `evalContextType` |
| UFE body | the UFE's identity kind, the requested specialization, the UCB's mode; identity digest from `RootIdentity::identityDigest` or `childIdentityDigest(parent, table, index)` | `executableBodyContextDigest(kind, source.startOffset(), source.firstLine().oneBasedInt(), holderDigest(ufe) for a root)` |
| direct eval | `DirectEval`, call, the UCB's mode, `directEvalIdentityDigest(callerKey, site->bytecodeIndex, sourceDigest(source.view()))` | `directEvalContextDigest` over the source's offset and first line, the snapshot, the executable's other parameters and the TDZ and private-name sets passed to `create` |

The UCB's mode is the request mode where the request imports or generates, and the mode the UCB itself stores where it was decoded or found live. A program or module the native path decoded takes the request's key when its own with-scope bit equals the snapshot's, and no key otherwise (sections 4.2 and 6.3.3); a live UCB is compared through its record (section 6.3.4). A UFE without identity, a direct eval without a site, or one whose caller UCB has no record, has no key: it neither imports nor records.

The context column says what each digest covers; when it is computed and what a record keeps follow section 4.3. The `holderDigest(ufe)` of a root's row is the request's holder digest, which C11 reads too.

A global root's source digest is `rootSourceDigest(source)` (section 4.4). When it came from the provider (form 2) and the VM imports with strict on, the request keeps that provider in `suppliedDigestProvider`; a UFE body takes the provider from its identity, a direct eval takes none (section 4.4), and every record the request makes stores what the request holds (section 5.1).

#### 6.2.4 Direct eval

`DirectEvalExecutable::create` gains a last parameter `const JITCache::DirectEvalSite* = nullptr` (the header forward-declares the struct). `JSC::eval` builds `JITCache::DirectEvalSite site { callerUnlinkedCodeBlock, bytecodeIndex }` and passes `&site`. `DebuggerCallFrame::evaluateWithScopeExtension` keeps its call and so passes none: its request is inactive, which costs nothing, since an attached debugger has already turned cache activity off (F5). In `create`, after the executable exists:

```cpp
JITCache::DirectEvalRequest jitCacheRequest(vm, *executable, executable->source(), site, codeGenerationMode, evalContextType, variablesUnderTDZ, privateNameEnvironment);
UnlinkedEvalCodeBlock* unlinkedEvalCode = jitCacheRequest.tryImport();
if (!unlinkedEvalCode) {
    unlinkedEvalCode = generateUnlinkedCodeBlockForDirectEval(...);   // unchanged
    if (unlinkedEvalCode)
        jitCacheRequest.didGenerate(*unlinkedEvalCode);
}
// unchanged: debugger notification, error handling, m_unlinkedCodeBlock.set
```

The request is constructed after the executable and before `generateUnlinkedCodeBlockForDirectEval`, which overwrites the executable's features with the parse's (F18), so its snapshot is the call site's features that `create` received. `JSC::eval` runs this under its own `DeferGC`, which the import's decode nests inside. `DirectEvalCodeCache` stays as it is: a hit there reuses an executable and is no request.

#### 6.2.5 Root UFEs

- `CodeCache::getUnlinkedGlobalFunctionExecutable`, on the miss path after `functionExecutable->recordParse(...)` and before `addCache`: `JITCache::didCreateFunctionConstructorExecutable(vm, *functionExecutable, source, lexicallyScopedFeatures, functionConstructorParametersEndPosition)`. A map hit returns a UFE that already has its identity, and its bodies keep it whatever the hitting request's with-scope bit, since it is the identity of what generation produces from that UFE.
- `BuiltinExecutables::createExecutable`, the overload taking `BuiltinSourceMetadata`, before returning: `if (!isBuiltinDefaultClassConstructor) JITCache::didCreateBuiltinExecutable(vm, *functionExecutable, source, scanned)`. The hook takes the source digest from `scanned` when the generator recorded one (form 1 of section 4.4), which every JSC builtin's generated entry has; the runtime scan `computeBuiltinSourceMetadata` records none, so Bun's internal modules created from text take form 2 or 3. A default class constructor is a child of the class's UCB (F2) and gets its identity there.
- `decodeBuiltinFunction` (`runtime/CachedTypes.cpp`), on success: `JITCache::didDecodeBuiltinExecutable(vm, *executable, provider)`, with the provider's whole source as the root source; it spans the provider, so it takes the digest the created form takes.

Each runs section 6.3.6 before `link` reads the UFE and before any holder publishes it (F15), so the root's first body request finds its identity.

#### 6.2.6 Bun's `vm.Script` and `vm.compileFunction` with `cachedData`

Both keep Bun's own decode, which sets `cachedDataRejected` and runs outside the CodeCache, so no request point sees it (F11). The UCB it yields gets no record and its children no identity (THREAD Restoration): a child's request misses `NoKey`, and `captureRecord` finds no record. `vm.Script`'s runs import at the program request point of section 6.2.1, and so does `vm.compileFunction` without `cachedData`, or with `cachedData` its decode rejects, through `CodeCache::getUnlinkedProgramCodeBlock`, its user function at that function's first call. The lane edits nothing in Bun for either.

#### 6.2.7 Paths without a request point

- Bun's decodes of section 6.2.6.
- `JSC::evaluate` with a precompiled `UnlinkedProgramCodeBlock` (`runtime/Completion.cpp`) skips the CodeCache; the block keeps whatever record it has.
- Generation without an executable (`recursivelyGenerateUnlinkedCodeBlockForProgram` and its module twin, Bun's bytecode builders) records no root, so its children have no identity and every request inside it misses. A builtin root recorded at creation still imports through `recursivelyGenerateUnlinkedCodeBlocksForFunction`.
- `DebuggerCallFrame::evaluateWithScopeExtension` passes no site (section 6.2.4).

#### 6.2.8 Supplied digests

Section 4.4's first two forms need a digest recorded where the text is produced. In JSC:

- `parser/SourceProvider.h`, class `SourceProvider` (manifest M6), gains one virtual, with only standard types since the header is exported to Bun:

  ```cpp
  // JITCache: SHA-256 of source() in the encoding SPEC-ucb.md section 4.4 defines, recorded where the text was produced,
  // or nullopt. A provider that returns one vouches for it as it vouches for hash().
  virtual std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const { return std::nullopt; }
  ```
- `builtins/BuiltinExecutables.h`: `BuiltinSourceMetadata` gains `std::array<uint8_t, 32> sourceDigest { }` and `bool hasSourceDigest { false }`, after its other fields. `computeBuiltinSourceMetadata` leaves both as they are.
- `Scripts/wkbuiltins/builtins_generator.py`: `compute_builtin_source_metadata` also returns the source digest of its characters, SHA-256 (`hashlib.sha256`) of the byte 1 followed by the characters as Latin-1 bytes, since builtin sources are 8-bit: the generator emits them as a `char` array, and the overload of `BuiltinExecutables::createExecutable` that scans at run time asserts `is8Bit()`. `Scripts/wkbuiltins/builtins_generate_combined_implementation.py`: `generate_source_metadata_table` writes the 32 bytes and `true` into each entry. The generator hashes the `originalSource` it concatenates into `s_JSCCombinedCode` and measures `sourceLength` on, which is exactly the text `name##Source()` spans.

In Bun (`~/bun`):

- `src/standalone_graph/StandaloneModuleGraph.rs`: `to_bytes` records, beside each module's `source_hash`, the source digest of the text the module's provider will hold, for each module stored as a string with a JavaScript-like loader. That text is the code units `encode_text_module` writes, which `File::to_wtf_string` wraps without copying, and the digest narrows them to encoding 1 when every unit is at most `0xFF`, as section 4.4 says. The digests form a new optional record of 32 bytes per module, flagged by `Flags::HAS_JITCACHE_SOURCE_DIGESTS` (the next free bit) and chained after the other records in `Flags` bit order; 32 zero bytes mean no digest. `from_bytes` reads it into a new `File` field. A graph written by a Bun without the record loads as before, and its modules' digests are computed.
- `src/jsc/ResolvedSource.rs` and `src/jsc/bindings/headers-handwritten.h`: `ResolvedSource` gains `jitcache_source_digest`, a pointer to the module's 32 bytes in the executable's section, or null.
- `src/runtime/jsc_hooks.rs`: the standalone module's `ResolvedSource` passes the module's digest, with or without bytecode.
- `src/jsc/bindings/ZigSourceProvider.h` and `.cpp`: `Zig::SourceProvider` copies the 32 bytes when it is created from a `ResolvedSource` that has them, and returns them from `jitCacheSourceDigest()`.
- `src/codegen/bundle-modules.ts`: the section that starts at `bun_internal_modules_header` gains a table of 32-byte source digests, one per JS module in record order, each the digest of the text its record's `codeOffset` and `codeLength` span, computed the same way. The header's first reserved word, which the generated `Header` struct renames `digestsOffset`, holds the table's offset from the section start. The module records keep their size and the format its version, so `src/exe_format/builtins.rs`, which reads the section of another platform's executable and ignores the reserved words, stays as it is.
- `src/jsc/bindings/InternalModuleRegistry.cpp`: `makeInternalModuleSource` creates the module's provider as a subclass of `StringSourceProvider` (whose constructor is protected) that returns the module's entry of that table, except under `BUN_DYNAMIC_JS_LOAD_PATH`, where the text is read from disk and has none.

Bun's other providers return none, and their roots are hashed from the text (form 3). The JITCache-off run reads those texts too, to parse them or to compute `hash()`, so the hash touches no page that run leaves untouched.

### 6.3 The engine

`UCBImport.cpp` implements these algorithms behind the entry points of section 6.7. `S` is the VM's JITCache state (R-INT-1). Each outcome below also increments the statistic section 5.6 names for it. Invalid material calls `S.raiseInvalidMaterial(step, diagnostic)`, which turns cache activity off for good, and then the native path goes on as if JITCache were off.

#### 6.3.1 Import

Runs where the native path would generate, only when `S.importsEnabled()` (role Consumer or ConsumerProducer, cache activity on), on the VM thread holding the API lock and heap access.

Before step 1, when `!vm.isSafeToRecurse()`, miss `Stack` and return null, computing nothing and stamping nothing. The parser tests the same limit before it recurses (`Parser::canRecurse`, the `failIfStackOverflow` checks in `parser/Parser.cpp`) from deeper in the stack, so the native generation that follows throws the stack-overflow `RangeError` it throws with JITCache off, and a later request at a shallower depth imports. An import does not parse, so a request that passes this test while its own parse would recurse past the limit imports and runs where the JITCache-off run throws; only parsing measures that depth, and this holds for every request kind, UFE bodies and direct evals included. THREAD Verification exempts such results from the oracle, and the lane tracks no parse depth.

1. Compute the key with the request mode and the snapshot's bits (section 6.2.3). No key: miss `NoKey`, return null. The context waits for step 4, the first step that reads it (section 4.3).
2. `v = S.bodyVersion(key)`. Zero: miss `NoBody`, return null. Then `S.openBody(key)`: `Missing`, miss `NoBody`, remember `v` in the request (the index lists a body the artifact no longer holds, as after `compact`) and return null; `Unusable`, return null (the integrator has raised the fault); `Found(body)`, continue.
3. Parse the identity section (section 7.2). With strict on, its rules are checked first, and a section that breaks one is invalid material at `ucb.identity`.
4. A stored key other than `body.key()` is invalid material at `ucb.identity`, in both modes: a body file is named by its whole key and container check B3 has matched `body.key()` to the request key (SPEC-integrator.container.md sections 1.1 and 4.5), so the body contradicts its own envelope, and THREAD Session counts keys among what normal mode checks. Provenance `EmbedderDecoded`: miss `Provenance`, since such a core was generated under option values no key records (F4), with plain constant butterflies (F23) and, for a program or module, a decoder's declaration-map layout (F10). Then the request computes its context (section 6.2.3), and a stored context that differs misses `Context`. A function body whose `holderDigest` differs from the request's holder digest (C11), which a root's context has already computed, misses `Holder`. Each miss remembers `v` in the request and returns null. With strict on, parsing has already checked the identity section's core kind against the key (section 7.2); in both modes the decode of step 6 asks for the kind the key implies. Then S2, with strict on: when the request's `suppliedDigestProvider` is set and `suppliedDigestVerified(provider.asID())` is false, `sourceDigest(provider.source())` must equal `provider.jitCacheSourceDigest()`; a match calls `markSuppliedDigestVerified` and counts `suppliedDigestVerifications`, and a difference is invalid material at `ucb.supplied-digest`. The hash runs before the registry lock is taken (section 5.3), once per provider in the VM, and only for a request about to import.
5. Parse the feedback section (section 8.2). With strict on, its rules are checked and its constant count must equal the identity section's; a failure is invalid material at `ucb.feedback`.
6. Decode the core (SPEC-ucb.codec.md) from the core section, wrapped in a `CachedBytecode` whose destructor releases a reference to `body`, with `coreKindFor(key.identityKind())` as the kind, so that a payload of another kind fails as `KindMismatch` (codec section 3), `source.provider()` as the decoder's provider and, for a function body, the requesting UFE as the holder, whose TDZ chain the core's start record stands for (codec E14). The decode runs under `DeferGC`, and validates the core's structure when strict is on (codec E15). Null or a decoder failure: invalid material at `ucb.decode`.
7. With strict on, check C1 to C5 and C7 to C9 (section 7.4; C6 is step 6's own report); a failure is invalid material at `ucb.closure`. With strict off the lane trusts the decoded core, and in neither mode does it re-encode it to compare with `coreDigest` (section 11.2).
8. Under a `DeferGC` of its own, `rebuildAtomStringButterflies(vm, ucb, identity)` (section 7.5) replaces each constant the butterfly map marks with a `JSCellButterfly` built as generation builds it (F23), since the decode gives every butterfly the plain structure and long string elements as plain strings (SPEC-ucb.codec.md, E10). The core encoding stays the same, since the codec writes neither a butterfly's structure nor its strings' atom-ness.
9. Seed the UCB feedback (section 8.3).
10. Restore the parse fields (section 6.4).
11. `vm.heap.reportExtraMemoryAllocated(ucb, instructionsSize + metadataSizeInBytes())`, the amount `UnlinkedCodeBlockGenerator::finalize` reports for a generated UCB.
12. `recordCodeBlock(ucb, { key, recordedContext(state), Imported, the snapshot's strict and with-scope bits for a global root, std::nullopt, body.version(), PendingImport::create(body, key, v, Seeded), the request's suppliedDigestProvider })`; the request settles.
13. Under `ENABLE(JITCACHE_TWINS)`, when `S.twinReportSink()` is non-null: `verifyImport` (section 13.2).
14. Return the UCB; the caller publishes it (section 6.5).

A decoded UCB abandoned by invalid material is unreachable and the collector frees it.

#### 6.3.2 Matching a UCB its body did not produce

Sections 6.3.3 and 6.3.4 share this procedure. It decides whether a UCB the native path produced, which here is `U` with key `K`, context `C` and origin `O`, is the UCB a body describes. For a live `U`, `K` and `O` come from its record and `C` is `contextDigestOf` the record's context inputs; for a `U` being seeded, `O` is `Decoded` and `C` is the request's context. Step 4 is the first to read `C` and computes it (section 4.3). A root UFE body's `C` and C11 both use the requesting UFE's holder digest, which the request computes once. The answer rests on what made `U`:

- A natively decoded `U` came from an embedder's payload, which no key pins (F4), so beyond key and context it matches only by content: its core encoding has the body's `coreDigest`, whether or not strict is on, and a difference is a miss.
- A generated or imported `U` matches by its generation inputs: key, context, a body of provenance `Generated` and, for a function body, the holder UFE (C11). From equal inputs the engine's generation makes the producer's UCB; strict checks that on the content (S1), and a failure is invalid material.

A body of provenance `EmbedderDecoded`, of any core kind, matches only a natively decoded `U`, by its core, and never a generated or imported one. The procedure reads the artifact only when the answer can have changed since the last time.

1. `v = S.bodyVersion(K)`. Zero: miss `NoBody`.
2. `U`'s record holds `missedBodyVersion == v`: miss `BodyUnchanged`; nothing is read.
3. `S.openBody(K)`: `Missing`, miss `NoBody` (the index lists a body the artifact no longer holds); `Unusable`, stop; `Found(body)`, continue. Section 6.3.4 opens the body itself and enters at step 4.
4. Parse the identity section, as in section 6.3.1 step 3. A stored key other than `body.key()` is invalid material at `ucb.identity`, as in section 6.3.1 step 4. Then compute `C`; a stored context other than `C`: miss `Context`.
5. Parse the feedback section, as in section 6.3.1 step 5.
6. When `O` is `Decoded`: `coreDigestOf(vm, U, h)` (section 7.5) equals `coreDigest`, else miss `CoreDigest`. `h` is the UFE whose slot holds `U`, the requesting one, for a function body, and null otherwise.
7. When `O` is `Generated` or `Imported`: the body has provenance `Generated`, else miss `Provenance` (F4, F10); a function body passes C11, else miss `Holder`.
8. With strict on, check C3, C4, C7 and C8 against `U`; a failure is invalid material at `ucb.closure` (section 11.2).
9. When `O` is `Decoded` and `U` is live: C10, else miss `AtomMap`. A `U` being seeded has its marked constants atomized after the match instead (section 6.3.3).
10. When `O` is `Generated` or `Imported` and strict is on, `coreDigestOf(vm, U, h)` equals `coreDigest` (S1); otherwise invalid material at `ucb.strict-core`.
11. Matched: the caller continues with `body` and its parsed sections.

A miss at step 3 or later, except at step 9, stamps `U`'s record with `missedBodyVersion = v`, the index token of step 1, never the opened file's commit identifier, so step 2 compares a token with a token whatever the index knows of the files (R-INT-3); for a `U` being seeded, the stamp goes on the record section 6.3.3 gives it. Later requests for `U` then skip the artifact until the index lists another body at `K`, so a body committed later, by this VM or, at the integrator's cadence, by another producer, is tried at the next request, and a body that `compact` deleted costs one failed open per UCB until the index forgets it. A miss at step 9 can turn into a match with the same file, since atom-ness only grows (F19): it is not stamped and sets the record's `matchedBodyVersion` to `body.version()`, so section 6.3.4 step 5 repeats only C10 at the next request. Step 1 stamps nothing.

#### 6.3.3 Seeding a decoded UCB before publication

Runs where the native path decodes the UCB from an embedder's bytecode cache, which THREAD Restoration keeps: `didDecode` at the CodeCache, before `addCache`, and `didDecodeCachedSlots` for the requested specialization, before `m_isCached` clears. No holder has published the decoded UCB `U` and no CodeBlock references it (F1, F4).

1. Compute `U`'s key `K` with its own mode (section 6.2.3); the request's context waits for the match's step 4 (section 6.3.2). For a global request, `K` takes the snapshot's strict and with-scope bits, and its source digest is the root source digest of section 4.4. When the snapshot's with-scope bit differs from `U`'s own, which the decode's `SourceCodeKey` check ignores (F4), THREAD Identity gives `U` no key: it is not recorded, its children get no identity and nothing in it imports or is captured; the miss is `RequestKey`, and the request settles. Any other request without a key: miss `NoKey`, nothing recorded.
2. When `S.importsEnabled()` is false, record `U` (section 6.3.5) and stop.
3. Match `U` with origin `Decoded` (section 6.3.2). A miss records `U` under `K`, stamped as section 6.3.2 says, and the request settles; nothing else happens. When the match stops instead, at `Unusable` or at invalid material, cache activity is off and nothing is recorded (SPEC-integrator.md section 5.2).
4. With strict on, check C9 against `U`; a failure is invalid material at `ucb.closure`.
5. `atomizeMarkedConstants(vm, U, identity)`: each constant the atom map marks becomes its atom and is marked as one, as `JSString::toAtomString` would leave it (F19), with no cell allocated; the image's constant-string comparisons rely on those atoms (THREAD Restoration; SPEC-ucb.codec.md, E10). `U` is unpublished, but a constant decoded from an embedder's string table is the one VM-wide `JSString` that `DecoderStringTable::jsStringFor` keeps for its ordinal, which published UCBs and CodeBlocks share (`CachedJSValue::decode`). The change is safe for their readers because `JSString::swapToAtomString` keeps the old `StringImpl` alive for concurrent readers, as every native property-key use of a published constant does (F19).
6. Seed the UCB feedback (section 8.3). The parse fields are the decode's own: the CodeCache path copies them with `recordParseFromUnlinkedCodeBlock` right after, and a lazily decoded UFE already holds the values its encoder wrote.
7. `recordCodeBlock(U, { K, recordedContext(state), Decoded, the snapshot's bits for a global root, std::nullopt, body.version(), PendingImport::create(body, K, v, Seeded), the request's suppliedDigestProvider, generatedButterflyMap })`, where `generatedButterflyMap` is a copy of the identity section's butterfly map when the body has provenance `Generated` and `S.productionActive()`, and empty otherwise, so that a later capture of `U` keeps the body's provenance (section 9.2); the request settles.
8. Twins: `verifyMatched(U, h, true, body, sink)`, with `h` as in section 6.3.2.

The decode itself is unchanged, so the decoded UCB's children keep their lazily cached slots, and each meets this section again at its own first call.

#### 6.3.4 Attaching to a live UCB

`didServeLive(U)` runs when the holder already has a UCB, at every CodeCache hit and every request for a filled UFE slot. It is how an import reuses a live UCB for its body (THREAD Identity): it attaches a pending import to the UCB the holder already has, never looks for a live UCB elsewhere and never gives one UCB to two holders, so native ownership and sharing stay as they are. `U` is compared with the body through its record; the request's own context does not enter, since the UCB is shared natively whatever the request's provider offset. Each step does only the work the earlier ones proved necessary: a request whose UCB has parked code, no body in the index, or a miss stamped at the key's current index token costs one registry lookup and one index lookup and computes no digest (bench B3). Every check that fails here returns without effect unless the step says otherwise.

1. `S.importsEnabled()`.
2. `recordOf(U)` exists and has no pending import, and `U.m_unlinkedBaselineCode` is null, since a parked `BaselineJITCode` wins anyway (THREAD Restoration).
3. `v = S.bodyVersion(record.key)` is nonzero, and the record's `missedBodyVersion` is not `v`: otherwise miss `NoBody` or `BodyUnchanged`, reading nothing.
4. The request's key equals the record's. For a UFE body this holds by construction: the record was made for this UFE's identity, this slot's specialization and the UCB's own mode (I17), so nothing is computed. For a global request, the snapshot's strict and with-scope bits must equal the record's `keyFeatures`, which is where a CodeCache hit whose request differs in the with-scope bit misses (THREAD Identity, F4). Then the request key, computed only now because it needs the source digest, must equal the record's key; they differ only when the CodeCache served a block whose `SourceCodeKey` merely collided with this request's (F4). Either failure is a miss `RequestKey` and is not stamped, since it belongs to the request rather than to `U`.
5. `S.openBody(record.key)`, which the pending import needs anyway: `Missing`, miss `NoBody`, stamped with `v`; `Unusable`, return; `Found(body)`. When the record's `matchedBodyVersion` equals `body.version()`, `U` already passed every check of section 6.3.2 against this very file, except perhaps C10: for origin `Decoded`, parse the identity section as in section 6.3.1 step 3 and check C10, a failure being a miss `AtomMap` that is not stamped; then go to step 7. Nothing else the match read can have changed since: `U`'s core encoding and its holder UFE are immutable (the encoding leaves out the shared `RegExp` state that code deletion changes, F22 and SPEC-ucb.codec.md E11), the file is the same, and a constant can only have become an atom (F19).
6. Match `U` with the record's key, context inputs and origin (section 6.3.2), entering at its step 4 with this body and `v`. A miss marks the record as section 6.3.2 says and returns.
7. `attachPendingImport(U, PendingImport::create(body, record.key, v, Reused), generatedButterflyMap)`, which also sets `matchedBodyVersion = body.version()`. For origin `Decoded`, `generatedButterflyMap` is a copy of the identity section's butterfly map, parsed at step 5 or by the match, when the body has provenance `Generated` and `S.productionActive()`; it is empty otherwise, and always for origin `Generated` or `Imported`, whose own butterflies have generation's form. An attach to a body of the other provenance thus clears an earlier map. The request settles.
8. Twins: `verifyMatched(U, h, false, body, sink)`, with `h` as in section 6.3.2.

Nothing is written to `U`, its UFE or its executable, which were published before, and THREAD seeds a UCB only before publication; section 8.3's seeds stay with the body.

#### 6.3.5 Records

`didGenerate` records the UCB with origin `Generated`, the request's key and `recordedContext(state)`, the snapshot's bits for a global root, the index token the request's import attempt missed at, if any, and the request's `suppliedDigestProvider`. A decoded UCB that section 6.3.3 does not seed (imports disabled or a miss at matching) is recorded with origin `Decoded` under the key that section's step 1 computes. `didDecodeCachedSlots` records the slot the request did not ask for the same way, through `recordDecodedSlot`, under the UFE's identity, that slot's specialization and the slot UCB's own mode, with the request's offset and first line, since both slots of one UFE share its source; `recordDecodedSlot` neither reads nor sets `settled` (section 6.1). Without a key nothing is recorded.

Every record stores the provider and the context inputs of the request that made it, and no context digest, except a direct eval's, which keeps the digest its import computed or, in a VM whose production is active, the one `recordedContext` computes while the TDZ and private-name sets still exist, and otherwise none (section 4.3).

#### 6.3.6 Roots

`didCreateFunctionConstructorExecutable`, `didCreateBuiltinExecutable` and `didDecodeBuiltinExecutable` call `recordRoot` with the root's UFE, kind, root source, lexically scoped features and parameter end, and `didCreateBuiltinExecutable` also with the generator's digest when `BuiltinSourceMetadata::hasSourceDigest` is set. While `S.tracksKeys()`, `recordRoot` takes the root source digest of section 4.4, computes `rootIdentityDigest`, THREAD's key digest of the root (B1), and calls `recordRootExecutable` with the provider of a form-2 digest when the VM imports with strict on; debug builds assert that a builtin's metadata digest equals the computed one (section 4.4). It looks up and opens no body and leaves the root UFE's own singleton bit native (section 8.1): global object setup creates many builtins eagerly (`ArrayPrototype::finishCreation` alone creates thirteen through `JSC_BUILTIN_FUNCTION_WITHOUT_TRANSITION`), and each body is looked up at its own first request, as any UFE body is (section 6.3.1).

### 6.4 Parse fields

- Program, module, indirect eval and direct eval: `recordParseFromUnlinkedCodeBlock(&executable, source, ucb)` (`runtime/CodeCache.cpp`), as a CodeCache hit does. The core carries the global parse fields (`UnlinkedGlobalCodeBlock::m_features`, `m_lexicallyScopedFeatures`, `m_hasCapturedVariables`, `m_lineCount`, `m_endColumn` and both directives), so this sets the executable's features, last line and end column and copies the directives back to the provider.
- UFE bodies: `ufe.recordParse(features, lexicallyScopedFeatures, hasCapturedVariables)` from the identity section's function parse fields, which are the values `generateUnlinkedFunctionCodeBlock` records. `ScriptExecutable::newCodeBlockFor` then copies them to the `FunctionExecutable` (F3).

Both happen inside `tryImport`, before it returns. A seeded decoded UCB keeps the parse fields of its native decode (section 6.3.3).

### 6.5 Publication

The native holder publishes an imported UCB as it publishes a generated one: the CodeCache through `publishGeneratedCodeBlock`; a UFE slot with its barrier, its entry in the clearable set and `CodeCache::updateCache`; a direct eval through `DirectEvalExecutable::m_unlinkedCodeBlock`. A seeded decoded UCB is published as the native decode publishes it: by `addCache` at the CodeCache, and by `decodeCachedCodeBlocks` clearing `m_isCached` for a UFE slot, which keeps the UFE out of the clearable set. Bun's providers implement none of the three provider hooks; the jsc shell's may, and they then encode an imported UCB like any other.

### 6.6 Threads, locks and GC

| hook | thread | locks held on entry | GC | notes |
|---|---|---|---|---|
| `GlobalRequest` in `getUnlinkedGlobalCodeBlock` | VM | API lock | not deferred by the callers (`ProgramExecutable::initializeGlobalProperties` holds `DeferTermination`); the import's decode and its butterfly rebuild each defer it | a provider-decoded block stays on the stack until the map holds it |
| `FunctionBodyRequest::didDecodeCachedSlots` | VM | API lock | inside `decodeCachedCodeBlocks`'s `DeferGC` | `m_isCached` still set; the steps allocate no cell, and the atomization swaps strings without one (F19) |
| `FunctionBodyRequest`, other members | VM | API lock | `DeferGCForAWhile` when reached from `prepareForExecutionImpl` through `newCodeBlockFor`; none from eager generation | `newCodeBlockFor` then calls `recordParse` with the UFE's fields |
| `DirectEvalRequest` in `DirectEvalExecutable::create` | VM | API lock | `DeferGC` in `JSC::eval` | the TDZ and private-name sets `JSC::eval` passes live until `create` returns, so `didGenerate` can still digest them; the debugger's route passes no site and does nothing |
| root hooks | VM | API lock | as the caller: none in `FunctionExecutable::fromGlobalCode`, `DeferGC` in `decodeBuiltinFunction` | builtin creation can run during global object setup; the hook computes the identity digest, and a source digest only for a root whose text supplied none (section 4.4); it opens no body and allocates no cell |
| destructor hooks | the thread sweeping the VM's heap | whatever the sweep holds | inside the sweep | registry lock only, section 5.3 |

No hook runs on a compiler thread or a marker. No hook is reached from JIT code. Matching, seeding, atomization, holder digests and the root hooks allocate no cell; only the import's decode and its butterfly rebuild do, each under its own `DeferGC`, and the imported UCB stays on the VM thread's stack between them, where the conservative scan finds it. The core and descriptor encodings that matching and holder digests run use malloc only and hash the encoder's pages in place, without assembling a payload (SPEC-ucb.codec.md, section 2 and E13). A holder digest writes an environment's digest into the environment once, on the VM thread, the only thread that reads or writes environments; the digest is freed with the environment (F26).

### 6.7 Engine interface

`UCBImport.h` declares the entry points the request objects call. All run on the VM thread holding the API lock and heap access.

```cpp
// The request's key with the given mode and the snapshot's bits, computed once per request and cached in the state with the
// source digest it rests on and the provider of a supplied digest (section 6.2.3); empty when the request has no key.
std::optional<BodyKey> requestKey(RequestState&, OptionSet<CodeGenerationMode> keyMode);
// The request's context digest and a UFE body's holder digest, each computed at most once, by the first step that reads it
// (section 4.3). requestContext requires a key; a root body's context reads requestHolderDigest.
const Digest256& requestContext(RequestState&);
const Digest256& requestHolderDigest(RequestState&);
// What a record made by the request keeps of its context (section 4.3): the inputs of section 6.2.3's context column, or for
// a direct eval the digest requestContext computed, which it computes now if it has not and production is active (R-INT-1).
RecordedContext recordedContext(RequestState&);
// The key of a program or module the native path decoded: the request's key with the decoded UCB's own mode, or empty when
// the decoded UCB's own with-scope bit differs from the snapshot's (THREAD Identity, section 6.3.3).
std::optional<BodyKey> decodedRootKey(RequestState&, const UnlinkedGlobalCodeBlock&);

UnlinkedCodeBlock* importBody(RequestState&);                                                  // section 6.3.1
void seedDecoded(RequestState&, UnlinkedCodeBlock&);                                           // section 6.3.3, or a record when it does not seed
void attachLive(RequestState&, UnlinkedCodeBlock&);                                            // section 6.3.4
void recordGenerated(RequestState&, UnlinkedCodeBlock&);                                       // section 6.3.5
void recordDecodedSlot(RequestState&, UnlinkedFunctionCodeBlock&, CodeSpecializationKind);    // section 6.3.5; never settles
void recordRoot(VM&, UnlinkedFunctionExecutable&, IdentityKind, const SourceCode& rootSource, LexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest = nullptr);   // section 6.3.6
```

A request's key mode is fixed by the UCB it acts on (section 6.2.3), so `requestKey` is asked for one mode per request; `recordDecodedSlot` computes the other slot's key itself. The child identity digest of a UFE body is computed inside `requestKey`. Every root source digest computed from text (form 3 of section 4.4) increments `sourceDigests`, and every root digest taken from builtin metadata or a provider increments `suppliedSourceDigests`; a direct eval's text digest counts in neither. Every context digest the engine computes, the request's or an attached UCB's record's, increments `contextDigests`, every holder digest `holderDigests`, and every environment a holder digest digests first `tdzEnvironmentDigests`. `UCBRequests.cpp` maps the request members onto these calls: `tryImport` to `importBody`, `didDecode` and the requested slot of `didDecodeCachedSlots` to `seedDecoded`, the other slot to `recordDecodedSlot`, `didServeLive` to `attachLive`, `didGenerate` to `recordGenerated` and the root hooks to `recordRoot`. The destructor hooks call the registry directly.

## 7. The lane's sections of a body file

### 7.1 Sections

The lane writes three sections into every body and reads them back. The integrator assigns their type ids, locates and checksums them and treats them as opaque bytes (THREAD Storage). All integers are little-endian; offsets are from the section start, which the integrator places 8-byte aligned in memory (R-INT-3).

| section | contents | read at |
|---|---|---|
| `ucb.identity` | key, context, provenance, core digest, holder digest, function parse fields, the atom and butterfly maps of the constants | import, matching |
| `ucb.core` | the core codec's payload: the UCB with its child descriptors, without child bodies (SPEC-ucb.codec.md) | import; matching compares a UCB's encoding with its digest, which the identity section carries |
| `ucb.feedback` | the UCB feedback, the children's and constants' learned bits and the lane's summary | import, matching, the key's first scoring |

A section refers to other lanes' content only through the UCB's index spaces, and to another body only by key (THREAD Storage). The children appear as descriptors in the core; each child's bytecode is in its own body.

### 7.2 `ucb.identity`

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `0x49424355` |
| 4 | 2 | layout version, 1 |
| 6 | 1 | core kind: 0 program, 1 module, 2 eval, 3 function |
| 7 | 1 | provenance (`CoreProvenance`): 0 when the captured UCB was generated or imported, or decoded and last seeded or attached from a body of provenance 0 (section 9.2); 1 for any other UCB the native decoder produced from an embedder's bytecode cache |
| 8 | 40 | body key |
| 48 | 32 | context digest |
| 80 | 32 | `coreDigest`: SHA-256 of the whole `ucb.core` section |
| 112 | 32 | `holderDigest`: for a function core, the holder digest (section 4.3) of the UFE that held the captured UCB; zero otherwise |
| 144 | 2 | function features (`CodeFeatures`), below `1 << bitWidthOfCodeFeatures`; 0 unless the core kind is function |
| 146 | 1 | function lexically scoped features, at most `AllLexicallyScopedFeatures`; 0 unless the core kind is function |
| 147 | 1 | function has-captured-variables, 0 or 1; 0 unless the core kind is function |
| 148 | 4 | constant count N, below `1 << 28` |
| 152 | ⌈N/8⌉ | atom map: bit `i % 8` of byte `i / 8` is set when constant `i` was, at capture, a string whose `StringImpl` is an atom |
| 152 + ⌈N/8⌉ | ⌈N/8⌉ | butterfly map: bit `i % 8` of byte `i / 8` is set when constant `i` is a butterfly generation built from atom strings: at capture, a `JSCellButterfly` with the structure `vm.cellButterflyOnlyAtomStringsStructure`, or, for a decoded UCB, a constant marked in the map its record keeps (section 9.2) |

The section ends at the butterfly map rounded up to 8 bytes, with zero padding. With strict on, parsing checks that size exactly, every listed range, `BodyKey::fromBytes`, that the core kind is the one the key's identity kind implies, that a core kind other than function has a zero `holderDigest`, that neither map sets a bit at or above N, and that no constant is marked in both maps; with strict off it reads each field where the layout puts it and trusts it (THREAD Session). The core's bytes record neither a string's atom-ness nor a butterfly's structure (SPEC-ucb.codec.md, E10), so the two maps carry them. The atom map lets a request compare or atomize a UCB's string constants without decoding the core (checks C8 and C10, section 6.3.3). The butterfly map marks the butterflies generation built from atom strings (F23), which an import builds again in that form (section 6.3.1, step 8); a natively decoded UCB has none (F23), so a capture of one writes an empty map, unless its record keeps the map of the `Generated` body it matched (section 9.2). Provenance decides which UCBs a body may serve (section 6.3.2).

### 7.3 `ucb.core`

The payload of `encodeUnlinkedCodeBlockCore` (SPEC-ucb.codec.md), at the section start. Its bytes are a deterministic function of the UCB's content: a function core writes its children's TDZ chains relative to its holder UFE's chain (codec E14), which the identity section's holder digest pins, and the encoding leaves out a string's atom-ness, a butterfly's structure and a `RegExp` cell's compiled state (codec E10 and E11). Two processes encoding equal UCBs write equal bytes (I14), so `coreDigest` identifies the core content and a UCB the body did not produce can be compared with it by encoding.

### 7.4 Checks on a UCB

"Imported" is a UCB decoded from the body's core; "decoded" a UCB the native decoder produced, being seeded (section 6.3.3); "live" a UCB a holder already published (section 6.3.4), whose record gives its origin.

| check | applies to | condition | failure |
|---|---|---|---|
| C1 | imported | the cell's class is `UnlinkedProgramCodeBlock`, `UnlinkedModuleProgramCodeBlock`, `UnlinkedEvalCodeBlock` or `UnlinkedFunctionCodeBlock` as the core kind says | invalid material |
| C2 | imported | `codeGenerationMode()` equals the key's mode; `isConstructor()` equals the key's specialization being construct; `codeType()` matches the core kind | invalid material |
| C3 | all | `numberOfValueProfiles()`, `numberOfArrayProfiles()`, `numberOfBinaryArithProfiles()`, `numberOfUnaryArithProfiles()`, `numberOfFunctionDecls()`, `numberOfFunctionExprs()` and the number of constant registers equal the feedback counts | invalid material |
| C4 | all, function kind | `isBuiltinFunction()`, `parseMode()`, `constructorKind()` and `isBuiltinDefaultClassConstructor()` equal the requesting UFE's | invalid material |
| C5 | imported | every constant whose `constantSourceCodeRepresentation` is `LinkTimeConstant` is an int32 with 0 <= value < `numberOfLinkTimeConstants`, since `JSGlobalObject::linkTimeConstant` indexes its array with `static_cast<unsigned>(value)` and a negative value would wrap; every `RegExp` constant is valid (`RegExp::isValid`), as generation emits only valid ones (F22) | invalid material |
| C6 | imported | the decode reported no failure: the payload's root checks, every symbol resolved, every map layout restored, every start record well formed and, with strict on, every check of the validating decode (SPEC-ucb.codec.md, section 3, E4, E5, E14 and E15) | invalid material at `ucb.decode` |
| C7 | all | every exit site's bytecode offset is below `instructionsSize()` | invalid material |
| C8 | all | the UCB has N constants; every constant the atom map marks is a resolved string; every constant the butterfly map marks is a `JSCellButterfly` of indexing type `CopyOnWriteArrayWithContiguous` whose elements are all resolved strings | invalid material |
| C9 | imported, decoded | every constant whose singleton bit the feedback sets is a `SymbolTable` | invalid material |
| C10 | live with origin `Decoded` | every constant the atom map marks is a string whose `StringImpl` is an atom | miss `AtomMap` |
| C11 | imported function core; live function UCB with origin `Generated` or `Imported` | the requesting UFE's holder digest (`requestHolderDigest`, computed at most once per request, section 6.7) equals the identity section's `holderDigest` | miss `Holder` |

C1 to C9 check the reference closure THREAD Restoration names and the structure around it: what the core names outside itself resolves in this VM, and what other parts index into has the counts the body promises. C6 is the decode's own report and runs at every import (section 6.3.1, step 6); only its E15 part needs strict. The others run only with strict on (THREAD Session and section 11.2), and only once the UCB is known to be the body's, by its core or by its generation inputs (section 6.3.2). C10 and C11 decide whether a UCB fits the body, so they run in both modes and a failure is a miss. An imported UCB needs no C10: every string constant register of one is an atom (SPEC-ucb.codec.md, E10), as is every one of a generated UCB (F19), and section 6.3.3 atomizes a seeded UCB's marked constants before publication. The butterfly map acts only at an import, which rebuilds the marked butterflies after the decode, once C8 has passed when strict is on (section 6.3.1); a decoded UCB keeps the plain butterflies of its native decode and a live one the forms its native path gave it, which are their native twins' forms (F23).

### 7.5 Interface

`UCBSections.h` declares:

```cpp
struct FunctionParseFields {
    CodeFeatures features;
    LexicallyScopedFeatures lexicallyScopedFeatures;
    bool hasCapturedVariables;
};

struct IdentitySection {   // a parsed ucb.identity section; the two maps borrow the section's bytes
    UnlinkedCodeBlockCoreKind coreKind;
    CoreProvenance provenance;
    BodyKey key;
    Digest256 contextDigest;
    Digest256 coreDigest;
    std::optional<Digest256> holderDigest;                    // core kind Function only
    std::optional<FunctionParseFields> functionParseFields;   // core kind Function only
    uint32_t constantCount;
    std::span<const uint8_t> atomMap;
    std::span<const uint8_t> butterflyMap;
};

// With strict, empty when the section breaks a rule of section 7.2. Without, never empty: it reads each field where the
// layout puts it and trusts it (THREAD Session).
std::optional<IdentitySection> parseIdentitySection(std::span<const uint8_t>, bool strict);
UnlinkedCodeBlockCoreKind coreKindFor(IdentityKind);
bool constantMapsFit(UnlinkedCodeBlock&, const IdentitySection&);          // C8; allocates nothing
bool markedConstantsAreAtoms(UnlinkedCodeBlock&, const IdentitySection&);  // C10; allocates nothing
// SHA-256 of the bytes encodeUnlinkedCodeBlockCore would return for the UCB and holder, fed page by page through
// forEachUnlinkedCodeBlockCoreChunk (SPEC-ucb.codec.md, E13), so no payload is assembled. `holder` is the UFE whose slot
// holds a function UCB, and null for any other (codec E14). Matching (section 6.3.2). VM thread; allocates no cell.
Digest256 coreDigestOf(VM&, const UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder);
// Section 6.3.3 step 5, on an unpublished UCB matched to the body, which passed C8 with strict on and is trusted to fit it
// otherwise: atomizeStringConstant on each constant the atom map marks.
void atomizeMarkedConstants(VM&, UnlinkedCodeBlock&, const IdentitySection&);
// Makes a resolved JSString's value its atom and sets its isDefinitelyAtom bit, as JSString::toAtomString does:
// existingAtomOrNull() when the value is already an atom (it calls markAsAtom), otherwise AtomStringImpl::add and then
// JSString::swapToAtomString unconditionally, which ends in markAsAtom even when the add made the StringImpl itself the
// atom. Value profiling reads that bit (speculationFromCell gives SpecStringIdent only for a marked cell). VM thread;
// allocates no cell (F19).
void atomizeStringConstant(VM&, const JSString&);
// Section 6.3.1 step 8, on an unpublished imported UCB, which passed C8 with strict on and is trusted to fit its body
// otherwise; the caller holds a DeferGC. For each constant the butterfly map marks, as ArrayNode::emitBytecode builds one
// (F23): JSCellButterfly::tryCreate with vm.cellButterflyOnlyAtomStringsStructure and the decoded butterfly's length; for
// each element, atomizeStringConstant, then setIndex with vm.atomStringToJSStringMap.ensureValue(atom, [&] { return
// element; }), the VM's canonical JSString for that atom; then the constant register takes the new butterfly through
// UnlinkedCodeBlock::constantRegister, with a write barrier on the UCB. VM thread; allocates one cell per marked constant,
// and a failed tryCreate crashes, as it does in generation.
void rebuildAtomStringButterflies(VM&, UnlinkedCodeBlock&, const IdentitySection&);
size_t identitySectionSize(uint32_t constantCount);
// Fills exactly identitySectionSize(constant count of the UCB) bytes: the atom map from the UCB's constants, and the
// butterfly map from them too unless keptButterflyMap, a record's kept map of ceil(N / 8) bytes, replaces it (section 9.2).
void writeIdentitySection(std::span<uint8_t> out, VM&, const BodyKey&, const Digest256& contextDigest, CoreProvenance, const Digest256& coreDigest,
    const std::optional<Digest256>& holderDigest, UnlinkedCodeBlockCoreKind, const std::optional<FunctionParseFields>&, UnlinkedCodeBlock&,
    std::optional<std::span<const uint8_t>> keptButterflyMap = std::nullopt);
```

`UnlinkedCodeBlockCoreKind` is the codec's (SPEC-ucb.codec.md, section 2). `atomizeStringConstant` needs `JSString::swapToAtomString`, which is private, so `runtime/JSString.h` declares it a friend (section 3), as it does `DecoderStringTable`. `rebuildAtomStringButterflies` writes the constant through the public non-const `UnlinkedCodeBlock::constantRegister` and needs no native edit. The decoded butterfly it replaces had the unpublished UCB as its only referrer, so the collector frees it.

## 8. UCB feedback

### 8.1 What travels

| state | owner | travels |
|---|---|---|
| value predictions | `UnlinkedValueProfile` in `UnlinkedCodeBlock::m_valueProfiles` | as captured |
| array modes and flags | `UnlinkedArrayProfile` in `m_arrayProfiles` | as captured; the pruning mark is never in the UCB copy |
| arithmetic profiles | `BinaryArithProfile`, `UnaryArithProfile` in the UCB | the 16 bits as captured |
| exit sites | `DFG::ExitProfile` in `m_exitProfile` | the site list as captured, read under `UnlinkedCodeBlock::m_lock` |
| LLInt counter | `m_llintExecuteCounter` | base threshold and progress as captured; the slice is armed anew (section 8.5) |
| `didOptimize` | `UnlinkedMetadataTable` | as captured |
| quick tier-up bits | `m_quickDFGTierUp`, `m_quickFTLTierUp` | as captured |
| scope singletons | each `SymbolTable` constant's `m_singleton` (F14) | one bit per constant: invalidated or not |
| children's singleton bits | each child UFE's `m_singletonHasBeenInvalidated` | one bit per child, seeded with the parent |
| a root's own singleton bit | the `m_singletonHasBeenInvalidated` of a Function-constructor or builtin UFE (F15) | local (THREAD Restoration; below) |
| age | `m_age` | local: the collector's own measure |
| liveness, sharing slot | `m_liveness`, `m_unlinkedBaselineCode` | local: recomputed, or the install's |

A plan that fails for lack of executable memory writes three of these, the LLInt counter and the two quick tier-up bits (F24), and under R-INT-11 the integrator raises the fault before those writes. A capture therefore holds a plan failure's effects only when the plan failed for another reason, such as `FTL::canCompile`, and those travel as captured (THREAD Restoration: "failure effects included").

The children's singleton bits are set while the parent runs (`FunctionExecutable::notifyCreation`) and held by the parent's tree, so they travel with the parent; a `FunctionExecutable` created by linking an imported parent reads them in `UnlinkedFunctionExecutable::link`. The scope singletons propagate from the CodeBlocks' realm clones into the UCB's own tables (F14), where each later clone reads them.

A root UFE's own bit, on a Function-constructor or builtin UFE (F15), stays local (THREAD Restoration): `link` reads it right after the root UFE is created, so carrying it would read a captured body at every root's creation (section 6.3.6). It has two readers, both through the executable's singleton, which `link` derives from it. The root's own DFG compile folds `GetCallee` to a constant while the singleton holds (`ByteCodeParser::get` for the callee slot and the `GetCallee` case of `AbstractInterpreter`), so a consumer without the bit can jettison that code once, when a second function of the executable is created after the compile. `ObjectAllocationProfileBase::initializeProfile` (`bytecode/ObjectAllocationProfileInlines.h`) allocates `this` poly-proto only when both the executable's poly-proto set and its singleton have been invalidated, so until a second function of the executable exists in the consumer, a `new` of such a root constructor allocates mono-proto objects where the producer allocated poly-proto ones. Both costs are speed only, and a JITCache-off run pays them at the same points. No other part carries the bit, nor the `FunctionExecutable`'s `m_singleton`.

### 8.2 `ucb.feedback`

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `0x46424355` |
| 4 | 2 | layout version, 1 |
| 6 | 2 | reserved, 0 |
| 8 | 4 | value profile count V |
| 12 | 4 | array profile count A |
| 16 | 4 | binary arithmetic profile count B |
| 20 | 4 | unary arithmetic profile count U |
| 24 | 4 | exit site count E |
| 28 | 4 | function declaration count D |
| 32 | 4 | function expression count X |
| 36 | 1 | `didOptimize` (`TriState`: 0 False, 1 True, 2 Indeterminate) |
| 37 | 1 | quick DFG tier-up (`TriState`) |
| 38 | 1 | quick FTL tier-up, 0 or 1 |
| 39 | 1 | reserved, 0 |
| 40 | 4 | LLInt `m_activeThreshold` (`int32_t`) |
| 44 | 4 | LLInt `m_totalCount` (the `float`'s bits) |
| 48 | 4 | LLInt `m_counter` (`int32_t`) |
| 52 | 4 | constant count C |
| 56 | 8V | value predictions, `SpeculatedType` each |
| | 8A | array profiles: `u32 observedArrayModes`, `u32 flags` (`OptionSet<ArrayProfileFlag>::toRaw()`) |
| | 2B | binary arithmetic bits |
| | 2U | unary arithmetic bits |
| (4-aligned) | 8E | exit sites: `u32 BytecodeIndex::asBits()`, `u8 ExitKind`, `u8 ExitingJITType`, `u8 ExitingInlineKind`, `u8` 0 |
| | D + X | child bits, declarations then expressions: bit 0 singleton invalidated, other bits 0 |
| | ⌈C/8⌉ | constant bits: bit `i % 8` of byte `i / 8` set when constant `i` is a `SymbolTable` whose singleton has been invalidated |

The section size is the end of the last array rounded up to 8; alignment gaps and the final padding are zero; every count is below `1 << 28`. With strict on, the lane checks all of that, with overflow-checked size arithmetic, and the rules below; with strict off it reads each field where the layout puts it and trusts it (THREAD Session). The rules:

- every prediction `p` has `(p & ~SpecBytecodeTop) == 0`: native value profiles only see boxed values (`ValueProfileBase::computeUpdatedPrediction` merges `speculationFromValueForProfiling`), so no prediction carries `SpecInt52Any` or `SpecDoubleImpureNaN`, and the CB lane bounds the copies the native merge unions with these the same way (SPEC-cb.md V6);
- array modes are within `ALL_ARRAY_MODES`; flags are within the eight `ArrayProfileFlag`s and exclude `DidPerformFirstRunPruning`;
- binary bits are below `1 << 14` (results, both operand types and the special fast-path bit); unary bits below `1 << 10`;
- each exit site has a defined `ExitKind` other than `ExitKindUnset` (the implementation `static_assert`s the last enumerator it checks against), a JIT type of `ExitFromDFG` or `ExitFromFTL`, an inline kind of `ExitFromNotInlined` or `ExitFromInlined`, and no two sites are equal;
- the `TriState`s are at most 2, the FTL bit at most 1 and byte 39 zero;
- the counter has `m_activeThreshold == INT32_MAX` with `m_totalCount` 0 and `m_counter` `INT32_MIN`, the state `ExecutionCounter::deferIndefinitely` leaves, or `m_activeThreshold >= 0` with a finite total and a progress `double(m_totalCount) + m_counter` in `[0, 2^31)`;
- child bits are 0 or 1, and no constant bit at or above C is set.

What needs the identity section or the UCB is checked by the engine, with strict on: C equals the identity section's N (sections 6.3.1 and 6.3.2), and C3, C7 and C9 once the UCB is known.

### 8.3 Seeding

Runs at import step 9 on the decoded UCB, and at step 6 of section 6.3.3 on a natively decoded one. In both cases no holder has published the UCB and no CodeBlock references it; the VM thread:

1. `unlinkedValueProfiles()[i].restorePrediction(p[i])`.
2. `unlinkedArrayProfiles()[i].restoreAccumulatedState(modes[i], flags[i])`.
3. `binaryArithProfile(i).restoreBits(b[i])`, `unaryArithProfile(i).restoreBits(u[i])`.
4. Under `ConcurrentJSLocker` on `UnlinkedCodeBlock::m_lock`: `exitProfile().restoreFrequentExitSites(locker, sites)`.
5. `setDidOptimize`, `setQuickDFGTierUp`, `setQuickFTLTierUp`.
6. Arm the LLInt counter (section 8.5).
7. For each child whose bit is set, `setSingletonHasBeenInvalidated()` on the UFE at that table and index.
8. For each constant whose bit is set, `singleton().invalidate(vm, StringFireDetail("JITCache: captured singleton invalidation"))` on that `SymbolTable`, which no CodeBlock has cloned, so it has no watcher and the call only stores the state (F14).

Every seed lands in its native owner before any reader or writer (THREAD Restoration), and no `jitSoon` or `jitNextInvocation` follows. A child UFE's own singleton bit is seeded by step 7, with its parent; a root UFE's stays native (section 8.1).

### 8.4 Native accessors

Added to classes this lane edits; each is a one-line member with a `// JITCache:` comment, and no existing function changes. The `UnlinkedCodeBlock` accessors the lane reads (`functionDecl`, `functionExpr`, `numberOfFunctionDecls`, `constantRegisters`, `exitProfile`, `unlinkedValueProfiles`, `unlinkedArrayProfiles`, `binaryArithProfile`, `unaryArithProfile`, `thresholdForJIT`, `metadata`, `llintExecuteCounter`) and its `m_lock` are non-const natively, so every lane function that reads a UCB through them takes `UnlinkedCodeBlock&`; none modifies the UCB unless its name says so.

- `bytecode/ValueProfile.h`, `UnlinkedValueProfile`: `SpeculatedType prediction() const` and `void restorePrediction(SpeculatedType)`. The first is R-UCB-1 of SPEC-cb.md.
- `bytecode/ArrayProfile.h`, `UnlinkedArrayProfile`: `ArrayModes observedArrayModes() const`, `OptionSet<ArrayProfileFlag> arrayProfileFlags() const` (R-UCB-1 of SPEC-cb.md) and `void restoreAccumulatedState(ArrayModes, OptionSet<ArrayProfileFlag>)`, which asserts the pruning mark is absent.
- `bytecode/ArithProfile.h`, `ArithProfile`: `void restoreBits(BitfieldType bits) { m_bits = bits; }`, which is also R-UCB-4 of SPEC-image.md.
- `bytecode/DFGExitProfile.h` and `.cpp`, `DFG::ExitProfile`: `template<typename Functor> void forEachFrequentExitSite(const ConcurrentJSLocker&, const Functor&) const`, inline in the header, which passes each stored site to the functor in stored order and copies nothing, so the capture reads (sections 8.7 and 9.2) allocate nothing for exit sites; and `void restoreFrequentExitSites(const ConcurrentJSLocker&, Vector<FrequentExitSite>&&)`, which asserts the profile is empty and leaves `m_frequentExitSites` null for an empty vector.
- `bytecode/UnlinkedCodeBlock.h`, `UnlinkedCodeBlock`: `unsigned numberOfBinaryArithProfiles() const` and `unsigned numberOfUnaryArithProfiles() const` (R-UCB-3 of SPEC-image.md).

### 8.5 The LLInt counter

THREAD Restoration: the counter travels as native finalization leaves it, and the request point arms it before publication without a slice, as the native initial arming does. With T the captured `m_activeThreshold` and P the captured `double(m_totalCount) + m_counter`:

```cpp
void armLLIntCounter(BaselineExecutionCounter& counter, int32_t T, double P)
{
    // No CodeBlock: no memory-pressure correction, as UnlinkedCodeBlock's constructor arms it. ExecutionCounter::setThreshold
    // defers indefinitely when T is INT32_MAX, as the native deferral left it.
    counter.setNewThreshold(T);
    if (T == std::numeric_limits<int32_t>::max() || P <= 0)
        return;
    double remaining = static_cast<double>(T) - P;
    if (remaining <= 0) {
        counter.m_counter = 0;
        counter.m_totalCount = static_cast<float>(P);
        return;
    }
    // The slice native setThreshold would arm, truncated first so that the progress P is kept exactly.
    int32_t slice = static_cast<int32_t>(BaselineExecutionCounter::clippedThreshold(nullptr, remaining));
    counter.m_counter = -slice;
    counter.m_totalCount = static_cast<float>(P + slice);
}
```

This is `ExecutionCounter::setThreshold` with no CodeBlock applied to the captured progress, through the counter's public fields so that no native function changes, except that it truncates the slice before adding it to `m_totalCount`: native `setThreshold` stores `int32(-remaining)` but adds the untruncated `remaining`, so its `count()` gains the truncated fraction (F20), while storing `P + slice` keeps `count()` at P. Both cross once the increments add up to `trunc(clipped(T - P))` points. Afterwards `m_activeThreshold == T` and `count()` equals P up to the `float` rounding of `m_totalCount`, the precision the native counter itself keeps; twins compare within it (T4).

A deferred counter is armed exactly: `setNewThreshold(INT32_MAX)` reaches `deferIndefinitely`, which leaves the state section 8.2 requires of a deferred record, so `count()` is the recorded P, `-2^31`. No native capture holds one: the LLInt defers its counter only when `shouldJIT` fails (`entryOSR`, `loop_osr` and `replace` in `llint/LLIntSlowPaths.cpp`), which then fails for every CodeBlock of the body, so no baseline compilation of it is ever captured, and `BaselineJITPlan::finalize` defers it only after the fault R-INT-11 raises first. The format still admits the state.

### 8.6 Richness

THREAD Capture gives this lane the arithmetic categories and the exit sites.

```cpp
struct UCBRichness {
    uint64_t arithmeticUnits { 0 };   // one per set category bit of each arithmetic profile slot
    uint64_t exitSiteUnits { 0 };     // one per exit site
    uint64_t total() const { return arithmeticUnits + exitSiteUnits; }
};
```

A binary profile's categories are its 13 low bits: the seven observed results and both operand types, without the special fast-path bit. A unary profile's are its 10 low bits: results and the argument type. The arithmetic profile exists only in the UCB, so its slot is already the union THREAD scores. `liveRichness` reads the exit sites under `m_lock`; `savedRichness` reads the feedback section of the saved body, which is the lane's summary in the body file (THREAD Capture), at the key's first scoring.

Every exit site counts, as THREAD Capture says. The `BadCache` sites a consumer could induce itself, which `CodeBlock::tallyFrequentExitSites` would add after an early DFG compile at a site whose IC holds only the one shape a consumer invocation met, are kept out at their source by THREAD Restoration's rule that a body with a property IC whose captured record lists two or more cases carries no baseline counter progress: the ICs lane's capture returns `hasPolymorphicSite`, and the CB lane's capture then writes no counter progress. A recapture of an imported body still carries every exit site its import seeded, since `DFG::ExitProfile` only appends.

### 8.7 Interface

`UCBFeedback.h` declares:

```cpp
struct FeedbackCounts {
    uint32_t valueProfiles { 0 };
    uint32_t arrayProfiles { 0 };
    uint32_t binaryArithProfiles { 0 };
    uint32_t unaryArithProfiles { 0 };
    uint32_t exitSites { 0 };
    uint32_t functionDecls { 0 };
    uint32_t functionExprs { 0 };
    uint32_t constants { 0 };
    friend bool operator==(const FeedbackCounts&, const FeedbackCounts&) = default;
};

class FeedbackSection {   // a parsed ucb.feedback section; borrows its bytes
public:
    // With strict, empty when the section breaks a rule of section 8.2. Without, never empty: the counts and arrays are read
    // where the layout puts them and trusted (THREAD Session).
    static std::optional<FeedbackSection> parse(std::span<const uint8_t>, bool strict);
    const FeedbackCounts& counts() const;
    SpeculatedType prediction(unsigned) const;
    ArrayModes observedArrayModes(unsigned) const;
    OptionSet<ArrayProfileFlag> arrayProfileFlags(unsigned) const;
    uint16_t binaryArithBits(unsigned) const;
    uint16_t unaryArithBits(unsigned) const;
    DFG::FrequentExitSite exitSite(unsigned) const;
    bool childSingletonInvalidated(ChildTable, unsigned index) const;
    bool constantSingletonInvalidated(unsigned constantIndex) const;
    TriState didOptimize() const;
    TriState quickDFGTierUp() const;
    bool quickFTLTierUp() const;
    int32_t llintActiveThreshold() const;
    double llintProgress() const;   // double(m_totalCount) + m_counter
private:
    std::span<const uint8_t> m_bytes;
    FeedbackCounts m_counts;
};

FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock&);                  // VM thread; exit sites under UnlinkedCodeBlock::m_lock
bool feedbackFits(UnlinkedCodeBlock&, const FeedbackSection&);          // C3 and C7
bool constantBitsFit(UnlinkedCodeBlock&, const FeedbackSection&);       // C9
void seedFeedback(VM&, UnlinkedCodeBlock&, const FeedbackSection&);           // section 8.3
size_t feedbackSectionSize(const FeedbackCounts&);
// Fills exactly feedbackSectionSize(counts) bytes from the UCB's accumulated state (section 9.2).
void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock&, const FeedbackCounts&);
void armLLIntCounter(BaselineExecutionCounter&, int32_t threshold, double progress);   // section 8.5
UCBRichness liveRichness(UnlinkedCodeBlock&);                                     // VM thread, during a capture
std::optional<UCBRichness> savedRichness(std::span<const uint8_t> feedbackSection, bool strict);   // empty as parse is
```

## 9. Capture

### 9.1 Calls

`UCBCapture.h` declares what the integrator's capture glue calls, on the VM thread with JS paused, for an eligible CodeBlock whose UCB has a record:

```cpp
// Empty, and the UCB is not captured, when it has no record, or when it is a direct eval whose record keeps no context
// digest, which no record made while production was active lacks (section 6.3.5).
std::optional<UCBRegistry::RecordView> captureRecord(VM&, const UnlinkedCodeBlock&);
uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock&);   // L: ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp())
Expected<UCBSections, UCBCaptureFailure> buildSections(VM&, const CodeBlock&, ProducerBudget&);   // only for an accepted capture

struct UCBSections {   // move-only
    UCBSections(Vector<uint8_t>&& identity, Ref<CachedBytecode>&& core, Vector<uint8_t>&& feedback, Ref<ProducerBudget>&&, size_t chargedBytes);
    UCBSections(UCBSections&&);              // takes the source's budget and charge; the source keeps neither
    UCBSections& operator=(UCBSections&&) = delete;
    ~UCBSections();                          // releases chargedBytes from budget when budget is non-null

    Vector<uint8_t> identity;      // 152 bytes plus the atom and butterfly maps
    Ref<CachedBytecode> core;
    Vector<uint8_t> feedback;
    RefPtr<ProducerBudget> budget; // the budget buildSections charged; null after a move
    size_t chargedBytes;
};
enum class UCBCaptureFailure : uint8_t { BudgetExceeded, StrictCheckFailed };
```

`liveRichness` and `savedRichness` come from `UCBFeedback.h` (section 8.7). L is the LLInt threshold the import skips, as the UCB's history scales it at capture (THREAD Maintenance).

### 9.2 `buildSections`

1. With strict on: SC1, every child UFE of the UCB's tables has the `ChildIdentity` (key, table, index) in the registry; SC2, `numberOfValueProfiles() == numParameters() + metadata().numValueProfiles()`. Failure: `StrictCheckFailed`, which the integrator raises as a recording fault (THREAD Session).
2. `counts = liveFeedbackCounts(ucb)`; `tryCharge(feedbackSectionSize(counts))`, allocate and `writeFeedbackSection`. The exit sites cannot change between the count and the write (F17).
3. Encode the core with `encodeUnlinkedCodeBlockCore`, passing the budget as its charger (SPEC-ucb.codec.md, E8) and, for function code, the UFE that holds the UCB, `uncheckedDowncast<FunctionExecutable>(codeBlock.ownerExecutable())->unlinkedExecutable()`, as the holder (SPEC-ucb.codec.md, E14). A refused charge: `BudgetExceeded`.
4. `coreDigest` = SHA-256 of the core bytes.
5. `tryCharge(identitySectionSize(N))`, allocate and `writeIdentitySection` with the record's key, the context digest, the provenance its origin gives, `coreDigest`, the core kind, and for function code the holder digest and the parse fields of the holder of step 3, whose request produced the UCB, and the record's kept butterfly map when `copyGeneratedButterflyMap` finds one, copied into a ⌈N/8⌉-byte buffer charged to the budget and released after the write.
   - Provenance. `Generated` and `Imported` give `Generated`, since an import takes only a body of provenance `Generated` and rebuilds its butterflies. `Decoded` gives `EmbedderDecoded`, unless `copyGeneratedButterflyMap` finds a map in the record: such a UCB was seeded or attached from a body of provenance `Generated` after section 6.3.2 step 6 found its core encoding equal to that body's `coreDigest`, and a UCB's core never changes after it is made (the premise of section 6.3.4 step 5), so the capture writes provenance `Generated` with the kept map as its butterfly map, in place of the map its own constants give, whose butterflies keep the decoder's plain structure (F23; SPEC-ucb.codec.md, E10). Otherwise a ConsumerProducer's richer recapture of such a UCB would replace a `Generated` body with an `EmbedderDecoded` one, which no consumer that generates the body can import (section 6.3.1, step 4).
   - Digests. The holder digest is computed before the charge, with the budget as the descriptor encoder's charger (SPEC-ucb.codec.md, E8) and as the charger of the sort buffer of each environment it digests first; its charges are released once hashed, the environment digests it keeps are not charged (section 4.3), and a refused charge is `BudgetExceeded`. The context digest is `contextDigestOf` the record's context inputs (section 4.3), with that holder digest for a root UFE body, so a capture computes each digest once.
   - Maps. The atom map comes from each string constant's `JSString::tryGetValueImpl()->isAtom()`, and the butterfly map from each `JSCellButterfly` constant's structure compared with `vm.cellButterflyOnlyAtomStringsStructure`, unless the record keeps one (Provenance, above); string constants are resolved and a structure read decodes an ID, so both read without allocating. A constant can only become an atom (F19), so every constant the producer's image compared as an atom when it was compiled is marked; a UCB's butterflies keep the structure they were created with (F23), so a generated or imported UCB's butterfly map records generation's form.

A refused charge releases what the call charged and returns `BudgetExceeded`, which the integrator raises as a recording fault. The reads are those THREAD Capture allows (I15): accumulated state from its owner, the exit sites under the UCB's lock, no sample and no drain; the scope singletons and the singleton bits are single loads on the VM thread. The UCB's core is immutable after generation, so encoding it races with no compiler thread; a decoded UCB's children whose slots are still lazily cached are encoded without reading those slots (SPEC-ucb.codec.md, E1).

## 10. Interfaces

### 10.1 What the lane provides

To the integrator:

- the registry's pending-import calls of section 5.2, for the install glue;
- `captureRecord`, `liveRichness`, `savedRichness`, `envelopeLLIntThreshold` and `buildSections` (sections 8 and 9), for the capture glue;
- `UCBRegistry::statistics()` and `UCBRegistry::counts()` (section 5.6), for `status`;
- the option rows of options.md that name this lane.

To the CB lane (its R-UCB-1): `UnlinkedValueProfile::prediction()` and `UnlinkedArrayProfile::arrayProfileFlags()`, plus `observedArrayModes()`. Reads are non-draining and safe on the VM thread during a capture; a concurrent drain costs only precision (THREAD Capture).

To the Image lane (its R-UCB-3): `numberOfBinaryArithProfiles()` and `numberOfUnaryArithProfiles()`.

To the CB, ICs and Image lanes (their index-space requirements): every UCB that carries a pending import has the producer's index spaces, that is everything SPEC-image.md R-UCB-1 lists: its instruction stream and out-of-line jump targets, metadata layout, profile counts, constant pool, identifiers, function tables with each entry's builtin, arrow and strict bits, jump tables, exception handlers, `numParameters`, `numCalleeLocals`, `numVars`, scope register, `codeType` and `isConstructor`, all of which the core encodes (SPEC-ucb.codec.md, section 1). It holds by I7: an imported UCB is the producer's core decoded; a seeded decoded UCB, and a reused one with origin `Decoded`, has the body's core encoding (section 6.3.2, step 6); and a reused generated or imported UCB has the generation inputs from which generation makes the producer's UCB. With strict off, these rest on THREAD Session's integrity checks (section 11.2). With strict on, an imported UCB also passes C1 to C9 and its instruction stream walks from its start to its end one whole instruction at a time (SPEC-ucb.codec.md, E15), so the other lanes' strict walks over the stream stay inside it; a reused generated or imported UCB's core encoding equals `coreDigest` (S1); and every one of them passes C3, C4, C7 and C8.

String constants (THREAD Restoration): every constant the identity section's atom map marks (section 7.2) is an atom before a CodeBlock installs the body, and stays one (F19). An imported UCB's string constant registers all decode as atoms (SPEC-ucb.codec.md, E10), section 6.3.3 atomizes a seeded decoded UCB's marked constants before publication, a reused generated or imported UCB's string constants are all atoms, as generation makes them (F19), and a reused decoded one passes C10 or misses. The map marks every constant that was an atom when the image compiled its constant-string fast paths (section 9.2), which is exactly what the Image lane relies on. A UCB can hold more atoms than the producer's did, never fewer (F19); the Image lane's twins record constant atom-ness among a compilation's inputs (THREAD Verification), so the extra atoms change neither an import nor its twin.

Constant cells: an imported UCB's constants have the forms generation gives them (I22). A seeded or reused decoded UCB keeps its native decode's forms, and a reused generated or imported one the forms its native path gave it. No image reads a butterfly's form: the baseline reaches butterflies only through `slow_path_new_array_buffer` (F23).

To every part: `SHA256`, `BodyKey` and `sourceDigest`.

### 10.2 What the lane requires of the integrator

The names are the lane's working names; the integrator owns the API and may rename it.

- R-INT-1. Per-VM state reachable as `JITCache::VMState* VM::jitCacheState()`, inline, null for an unconfigured VM. It owns one `UCBRegistry`, is created by `start` and destroyed after `Heap::lastChanceToFinalize`. It answers `tracksKeys()` (a started VM whose cache activity is on), `importsEnabled()` (role Consumer or ConsumerProducer and activity on), `productionActive()` (role Producer or ConsumerProducer, activity on and production not ended), which decides whether a direct eval's record keeps its context digest (section 4.3), and `strict()`.
- R-INT-2. Fault plumbing: `raiseInvalidMaterial(ASCIILiteral step, String diagnostic)` turns cache activity off for good and names the step in `status`; the steps are `ucb.identity`, `ucb.feedback`, `ucb.decode`, `ucb.closure`, `ucb.strict-core` and `ucb.supplied-digest`. `UCBCaptureFailure` maps to a recording fault.
- R-INT-3. Body lookup. `uint64_t bodyVersion(const BodyKey&)` answers from the integrator's in-memory index, with no filesystem call: a nonzero token while the index lists a body for the key, which changes whenever the index learns of another body at that key, and 0 while it lists none. The index may lag commits and evictions made by other processes, and needs no envelope read to list a key; the lane asks for no fresher answer than the integrator's cadence (THREAD Storage). `BodyLookup openBody(const BodyKey&)` opens the current file and returns `Missing`, `Unusable` (the integrator has already raised the fault or invalid material) or `Found(Ref<ValidatedBody>)`. It returns `Found` only for a file that passed, in both modes, the integrity checks THREAD Session gives normal mode: its envelope carries the requested key and the digest of the header `start` checked, and its envelope, directory and sections match their checksums. With strict on, the file has passed the container's structure checks as well (SPEC-integrator.container.md section 4.5). Normal mode trusts the rest of a body on these checks (section 11.2). `uint64_t ValidatedBody::version() const` returns the opened file's commit identifier (THREAD Maintenance). The lane compares a token only with tokens and a commit identifier only with commit identifiers (sections 6.3.2 and 6.3.4), so the two may disagree while the index lags. `ValidatedBody` is `ThreadSafeRefCounted`, pins its validated payload for as long as it lives, may be destroyed on any thread that holds no JITCache lock, and returns each section as a span starting 8-byte aligned, empty when absent. Neither call allocates a JSC cell; `bodyVersion`, which the lane asks at every request point of every body and at every CodeCache hit and filled-slot request whose UCB has no parked code, costs no filesystem call (bench obligation B4).
- R-INT-4. Section type ids for `ucb.identity`, `ucb.core` and `ucb.feedback`, each required in every body (SPEC-integrator.container.md, check B7). Every capture writes all three; a body without one is invalid material when strict validates the directory (`container.required`), and normal mode trusts that they are there (section 11.2), as SPEC-ics.md R-INT-4 says of `ICsBaseline`.
- R-INT-5. Capture glue: `captureRecord` decides capturability; richness adds `liveRichness(...).total()` and, for the saved body, `savedRichness(feedback, strict)->total()` read from its `ucb.feedback`, whose empty answer, possible only with strict on, is the glue's to raise; `buildSections` runs for the accepted capture only; L goes into the envelope; the three sections commit with the other lanes' in one body; `UCBSections` is destroyed once written.
- R-INT-6. Install glue: the calls of section 5.2, with `ImportResolution::DroppedByGate` when the `shouldJIT` gate dropped the import and `Installed` after `installCode` succeeded.
- R-INT-7. `[[nodiscard]] bool ProducerBudget::tryCharge(size_t)` and `void ProducerBudget::release(size_t)` on the VM thread, with THREAD's semantics for a charge past the limit (the same requirement as SPEC-cb.md R-INT-2).
- R-INT-8. The manifest entries of section 15.
- R-INT-9. The option rows of options.md that name this lane, in `start`'s table.
- R-INT-10. The producer-then-consumer runner the other lanes also require, in every build, since section 13 runs the lane's tests with twins and without them. Under `ENABLE(JITCACHE_TWINS)`, also `TwinReportSink* VMState::twinReportSink()`, non-null while a twin report is open, which the lane's engine passes to its own verify calls (section 13.2). The integrator makes no verify call for this lane.
- R-INT-11. The executable-allocation fault at every tier's plan site, which THREAD Execution gives the integrator, raised on the VM thread through `JSC::JITCache::didFailExecutableAllocation`, whose parameters the integrator defines (its SPEC: `(VM&, ExecutableAllocationSite)`, with `BaselinePlan`, `DFGPlan` and `FTLPlan` for these sites), before the failed plan's effects are written (F24):
  - In `BaselineJITPlan::finalize` (`jit/BaselineJITPlan.cpp`), first thing in the `CompilationFailed` case, before `dontJITAnytimeSoon` defers this lane's LLInt counter and before `m_didFailJITCompilation` is set. The case needs no cause, since a baseline plan fails only for lack of executable memory. Bun's `vm.Script` route reaches the same function through `JIT::compileSync`.
  - In `DFG::Plan::finalize` (`dfg/DFGPlan.cpp`), once the result is `CompilationFailed` and before `m_callback->compilationDidComplete`, when the plan recorded that it failed for lack of executable memory. The callback is what writes the effects: for a DFG plan the baseline counter's deferral, which is the CB lane's state, and the quick DFG bit, for an FTL plan the quick FTL bit. The integrator adds the record to `DFG::Plan`, a flag that the compiling thread sets before the plan becomes ready, at the two `linkBuffer.didFailToAllocate()` branches of `SpeculativeJIT::compile` and `SpeculativeJIT::compileFunction` and at the two `state.allocationFailed` branches of `DFG::Plan::compileInThreadImpl`; the worklist's lock, which moves the plan to ready and hands it to the finalizing thread, orders the write before the read. A plan that fails for any other reason records nothing, so its effects stay native and travel as captured, as THREAD Restoration says of a failed FTL compile clearing the quick FTL bit.
  - At both sites the call runs inside the plan's finalization, under the `DeferGC` of `JITWorklist::completeAllReadyPlansForVM` or the deferral of a synchronous route, with no worklist lock held, which the entry point's contract allows: it allocates no cell, does not stop for the collector and waits for no thread.

THREAD Execution names these two plan sites as the integrator's, the IC stub and handler sites as the ICs lane's and MathIC regeneration in every tier as the Image lane's. THREAD Failures makes its list exhaustive: the other `JITCompilationCanFail` sites, in Yarr, in Bun's FFI thunks and stubs and in WebAssembly, keep native behavior and write nothing a capture reads. An FFI invoke thunk that an FTL plan cannot allocate still reaches the plan site, through `FTL::State::allocationFailed` (F24).

## 11. Failures and strict checks

### 11.1 Outcomes

Under THREAD Failures:

| step | outcome |
|---|---|
| no JITCache state, or key tracking off | native path, nothing recorded |
| imports disabled (Producer, activity off) | no import, seeding or attach; recording continues while tracking is on |
| no key: UFE without identity, direct eval without a site or with an unrecorded caller | miss `NoKey`; nothing recorded for that UCB |
| an import whose stack is past the limit the parser tests (`!vm.isSafeToRecurse()`) | miss `Stack`; nothing stamped; the native parse throws its stack-overflow `RangeError` |
| `bodyVersion` 0 | miss `NoBody`; nothing stamped |
| `openBody` returns `Missing` while the index lists a body | miss `NoBody`; stamped with the index token |
| live UCB whose record missed at the current index token, or whose import the `shouldJIT` gate dropped at it | miss `BodyUnchanged`; the artifact is not read |
| `openBody` returns `Unusable` | the integrator's fault or invalid material; native path |
| a CodeCache hit whose request differs from the UCB's record in the with-scope bit, or whose `SourceCodeKey` merely collided with the request's | miss `RequestKey`; not stamped |
| a decoded root whose with-scope bit differs from its request's | miss `RequestKey`; no key and no record, so nothing in it imports or is captured |
| stored key differs from `body.key()` | invalid material at `ucb.identity`, in both modes (section 6.3.1, step 4) |
| stored context differs | miss `Context`; stamped with the index token |
| a body of provenance `EmbedderDecoded` at an import, or against a generated or imported UCB (section 6.3.1, step 4; section 6.3.2, step 7) | miss `Provenance`; stamped with the index token |
| C11 | miss `Holder`; stamped with the index token |
| C10 on a live decoded UCB | miss `AtomMap`, not stamped; the record's `matchedBodyVersion` takes the file's commit identifier (section 6.3.2), so the next request repeats only C10 (section 6.3.4, step 5) |
| a decoded UCB's core encoding differs from `coreDigest` | miss `CoreDigest`; stamped with the index token |
| a core that fails the checks every decode makes (codec section 3, E4, E5 and E14) | invalid material at `ucb.decode`: cache activity off, preparation abandoned, native path |
| with strict on: a section that breaks its rules, a failure of the validating decode (codec E15), C1 to C9 | invalid material at the step that found it |
| strict S1 | invalid material |
| strict S2: a supplied digest an import relies on differs from its text | invalid material at `ucb.supplied-digest` |
| `buildSections`: refused charge | recording fault |
| `buildSections`: SC1 or SC2 | recording fault |
| registry allocation failure | crash, as any native allocation |

The lane allocates no executable memory, so THREAD's executable-allocation fault never starts here; the writes a plan failure makes to this lane's state follow section 8.1. An imported UCB abandoned by invalid material is freed by the collector (section 6.3.1); a natively decoded one goes on to be published natively without seeds. A pending import attached before cache activity turned off stays and dies with its UCB.

### 11.2 Strict

Strict is off unless the host turns it on, every test runs with it on and the bench measures the default (THREAD Session and Verification). Normal mode checks what THREAD Session calls the artifact's integrity: the integrator validates the header and the section checksums (R-INT-3), and the lane compares the body's key with the request's. It trusts the rest of a body from a matching build: the integrator's checks of the build ID, the must-match options and the checksums are what make every reference a core holds resolve in this VM, which is how normal mode reads THREAD Restoration's "validates its key and reference closure". In both modes the lane still decides whether a body fits the UCB at hand (key, context, provenance, C10, C11 and a decoded UCB's core digest), since those comparisons choose a body rather than check one. Strict adds the structure and the assumptions:

| check | guards | condition | failure |
|---|---|---|---|
| SV1 | an import, a seeding, an attach | `ucb.identity` and `ucb.feedback` obey every rule of sections 7.2 and 8.2, and their constant counts agree | invalid material at `ucb.identity` or `ucb.feedback` |
| SV2 | an import | the validating decode of codec E15 succeeds: every record inside the payload, every tag in range, no native assertion reachable, consistent strings and an instruction stream made of whole instructions | invalid material at `ucb.decode` |
| SV3 | an import, a seeding, an attach | C1 to C9 as section 7.4 assigns them | invalid material at `ucb.closure` |
| S1 | an attach to a generated or imported UCB | the UCB's core encoding has `coreDigest` | invalid material at `ucb.strict-core` |
| S2 | an import of a root or a function body whose key rests on a provider's supplied digest (section 4.4) | `sourceDigest(provider.source())` equals `provider.jitCacheSourceDigest()`, checked once per provider in the VM (section 6.3.1, step 4) | invalid material at `ucb.supplied-digest` |
| SC1 | a capture | each child UFE has the identity (key, table, index) | recording fault |
| SC2 | a capture | `numberOfValueProfiles() == numParameters() + metadata().numValueProfiles()` | recording fault |

An import is not re-encoded: whether decoding a core and encoding the result gives the core back depends only on the codec and the validated bytes (E4, E5, E10 and E11 of SPEC-ucb.codec.md), a property of the build that twin T1 checks on every import of a twins build and U7 over a corpus. S1 is the check THREAD Identity requires of a reused UCB ("with strict on, the import also checks a reused UCB against the digest of the captured core"): that a generated or imported UCB with the body's key, context, `Generated` provenance and holder UFE is the body's UCB. A natively decoded UCB is compared by its core encoding in both modes instead, since nothing pins an embedder's payload (F4, section 6.3.2).

S2 checks that a provider's supplied digest is the digest of its text, where an import would act on it (section 4.4). It runs at the import, never at a decode, an attach or a capture, so a compiled app whose modules are decoded from bytecode reads no source text for it unless an import replaces one of their generations. The builtins generator's digests are a property of the build, as the codec's round trip is: U9 and debug builds check them, and strict does not.

## 12. Options

[options.md](../options.md) holds this lane's option rows (the three must-match options and the fixed options that name this lane) and the free options the lane reasons about (THREAD Storage). The required value of each fixed row that names this lane is the value `FOR_EACH_JSC_OPTION` or `JSCWebPreferenceOptions.h` compiles in, captured by the integrator's table at build time.

## 13. Test obligations

The oracle, the native twins and the capture records are THREAD Verification's. Every JS test below runs as producer, then as consumer in a fresh process, through the integrator's runner (R-INT-10), in a debug build with twins on and, unless it declares `// jitcache-requires: twins` (section 13.3), in a release build without them, with strict on (THREAD Verification); a test that also checks the default says so and adds a run with strict off. The runner compares every run that configures JITCache, producers included, with a JITCache-off run: its output, which carries the results, exceptions and stack traces, and, for a jsc-hosted script in twins mode, the heap JavaScript can reach (SPEC-integrator.harness.md section 7.6).

### 13.1 Self-tests

`UCBSelfTest.cpp` defines `enum class UCBSelfTestScope : uint8_t { Configured, InvalidMaterial };` and `bool runUCBSelfTest(VM&, UCBSelfTestScope, String& failure)`, reached as `$vm.jitCacheUCBSelfTest()` and `$vm.jitCacheUCBSelfTest({ invalidMaterial: true })` (manifest M4), under `ENABLE(JITCACHE_TWINS)`. `start` fixes a VM's role and strictness for good, and invalid material turns its cache activity off for good (THREAD Session), so no single call can cover U8. `Configured` runs U1 to U7, U9 and the parts of U8 whose configuration the VM has, each part naming its own; `InvalidMaterial` runs only U8's case that makes an import invalid material, and nothing after it. `self-test.js` (section 13.3) calls them once per configuration.

- U1. SHA-256: the FIPS 180-4 examples (empty, `abc`, the 448- and 896-bit messages, one million `a`), on every implementation path the CPU offers, and 4096 random lengths compared across paths.
- U2. `sourceDigest`: an 8-bit and a 16-bit string with the same characters digest alike; a unit above `0xFF` changes the encoding; lengths 4095, 4096 and 4097 cross the chunk boundary. `rootSourceDigest`, with a test provider that supplies a digest: a builtin metadata digest wins (`BuiltinMetadata`); otherwise a source that spans the provider takes the supplied digest (`Provider`) without reading a character, which a provider whose `source()` asserts proves; a source over part of that provider computes its digest (`Computed`); a provider that supplies none computes it.
- U3. Keys: identity, child, direct-eval and context digests for fixed inputs equal literal vectors pinned in the test from the portable path; `BodyKey::fromBytes` rejects each out-of-range field; TDZ and private-name sets built in different insertion orders give one context digest, while a direct eval's context digest changes with each name added to its TDZ set or to its private-name set (no script can reach these); a program request whose source starts at another offset of its provider has another context digest (no jsc or Bun API evaluates one program text at two offsets); each of the three context digests changes with the source's first line, and the request contexts of section 6.2.3 do not change with its start column; `holderDigest` is the same before and after `recordParse` and after a body fills the UFE's slots, and changes with each field the descriptor holds, with each TDZ name of any link of its chain and with the order of its links; two UFEs whose chains are distinct link objects over the same environments give one holder digest; the environment digest equals a reference computed from its names in insertion order and sorted afterwards, and does not depend on whether the environment is in its compact or inflated form; a holder digest over a chain whose environments are already digested digests none (`tdzEnvironmentDigests` unchanged), and an environment freed and made again is digested again; `contextDigestOf` the context inputs `recordedContext` gives equals `requestContext` for each kind of request, a root UFE body's included, computed before and after the body is generated; a request's key and context stay the same when its executable's features are overwritten after the request object was constructed; a decoded program whose own with-scope bit equals the snapshot's takes the request's key with its own mode, and one whose bit differs gets none.
- U4. Registry: record, look up and remove for both maps; a second `recordCodeBlock` keeps the first record; stamping a missed index token; an attach sets the matched commit identifier, and so does an `AtomMap` miss, which stamps nothing; an attach replaces the record's `generatedButterflyMap` with its argument, an empty one included, and `copyGeneratedButterflyMap` copies a kept map and returns false, copying nothing, without one; `resolvePendingImport` with `Installed` detaches only, and with `DroppedByGate` also stamps the import's index token as missed and counts `gateDrops`; a destructor callback drops a pending import whose `ValidatedBody` stub asserts in its destructor that the registry lock is not held; the children of one recorded UCB reference one `ParentIdentity`, whose key and provider each child's `identityOf` returns, and which outlives the parent's record while a child lives and goes with the last child's destructor callback, outside the lock.
- U5. Sections: build then parse round-trips for both, with strict on and off; with strict on, each rule of sections 7.2 and 8.2 rejects a crafted value, including a provenance above 1, a nonzero holder digest on a core that is not a function's, a map bit at N and a constant marked in both maps; a size off by one and counts at `1 << 28` are rejected; `atomizeMarkedConstants` turns a plain-string constant into an atom, leaves an atom and unmarked constants as they are, and allocates no cell; on a UCB decoded from a generated one's core, `rebuildAtomStringButterflies` gives each marked butterfly `cellButterflyOnlyAtomStringsStructure` with every element the `JSString` `atomStringToJSStringMap` holds for its atom, as in the generated UCB, leaves the unmarked butterflies plain, and leaves the core encoding unchanged; `Array.prototype.indexOf` and `includes` on an array made from a rebuilt butterfly find every element and miss a string the map does not hold.
- U6. Counter: for T in {0, 100, 500} and each P in {0, 0.37, 1, 99.63, T - 0.5, T - 1, T, T + 5, 10^6 + 0.25} that is not negative, the progress section 8.2 accepts, `armLLIntCounter` leaves `m_activeThreshold == T` and `count()` equal to P up to `float` rounding, with a slice of `trunc(clippedThreshold(nullptr, T - P))` when `T - P` is positive and 0 otherwise, which is the number of points after which native `setThreshold` from the same T and P crosses. For the deferred record, T = `INT32_MAX` and P = `-2^31`, it leaves `m_activeThreshold == INT32_MAX`, `m_totalCount` 0 and `m_counter` `INT32_MIN`, the state `deferIndefinitely` leaves, so `count()` equals P exactly.
- U7. `InlineMap` layout transport and the codec's determinism, including `RegExp` constants after `RegExpCache::deleteAllCode`, as SPEC-ucb.codec.md lists.
- U8. Engine, with the integrator's body lookup stubbed, each part in the configuration it needs: the parts that import, seed or attach in a Consumer or ConsumerProducer, with the strictness a part states; the butterfly-map provenance written by `buildSections` in a ConsumerProducer, and its "seeded in a Consumer" case in a Consumer; the provider-record check in a Producer; and the supplied digest that differs from its text, which makes an import invalid material, only under `InvalidMaterial`. `attachLive` on a recorded program UCB whose `keyFeatures` differ from the request's snapshot misses `RequestKey` and stamps nothing; on one whose sharing slot holds code, whose key has no body or whose miss is current, it computes no source, context or holder digest (`sourceDigests`, `contextDigests` and `holderDigests` unchanged); `importBody` for a key with no body or a body that misses `Provenance`, and `seedDecoded` for a key with no body, compute no context and no holder digest, and a root body's import that reaches C11 computes its holder digest once (`holderDigests` grows by one); at a matched commit identifier it attaches without parsing a section, except that a decoded UCB parses only the identity section for C10; a decoded UCB that misses `AtomMap` stamps nothing, computes no digest at its next request and attaches there once its marked constants are atoms; and under an index token that lags the opened file's commit identifier it stamps and skips with tokens and matches with identifiers, never mixing the two; an index that lists a body whose file is gone makes one failed open, stamps the token and reads nothing at the next request; `didDecodeCachedSlots` with the requested slot empty and the other decoded records the other slot and leaves the request unsettled, so `didGenerate` records the generated slot; a body whose provenance is `EmbedderDecoded` misses `Provenance` for a program import, a function import and an attach to a generated function UCB, and seeds a decoded function whose core matches; a body of provenance `Generated` seeds a decoded function whose core matches. In a VM whose production is active, a decoded function seeded or attached from a body of provenance `Generated` keeps that body's butterfly map, and `buildSections` writes provenance `Generated` with that map; one seeded from a body of provenance `EmbedderDecoded`, or seeded in a Consumer, keeps none and is written `EmbedderDecoded`. Supplied digests, with a test provider whose program spans it: with strict on, an import of the program and of one of its functions verify the provider once in all (`suppliedDigestVerifications` 1), and a provider whose digest differs from its text makes the first of them invalid material at `ucb.supplied-digest` before anything is decoded; a direct eval inside that function imports without a verification, also when the program and the function were seeded rather than imported, and its record keeps no provider; with strict off, the same imports verify nothing; a decoded program under a wrong supplied digest is matched by its core and never verified; in a Producer no record keeps a provider.
- U9. Builtin metadata: for every `BuiltinCodeIndex`, `s_JSCBuiltinSourceMetadata[i]` has `hasSourceDigest` set, and its `sourceDigest` equals `sourceDigest` of the view `name##Source()` spans.

### 13.2 Twins

The engine calls these itself under `ENABLE(JITCACHE_TWINS)`, after the step that succeeded, whenever `S.twinReportSink()` is non-null (R-INT-10). `UCBTwins.h` declares them; each reports every difference to the sink and returns.

```cpp
void verifyImport(const RequestState&, UnlinkedCodeBlock& imported, const ValidatedBody&, TwinReportSink&);
// A seeded decoded UCB, or an attach; holder as for coreDigestOf (section 7.5).
void verifyMatched(UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder, bool seeded, const ValidatedBody&, TwinReportSink&);
// T7: a root digest taken from builtin metadata or a provider equals the digest of its text.
void verifySuppliedDigest(const SourceCode& rootSource, const RootSourceDigest&, TwinReportSink&);
unsigned verifyRegistry(VM&, TwinReportSink*);   // the number of violations; each is also reported when the sink is non-null
```

`verifyImport` generates the twin in the consumer from the same request with the helpers `UCBTwinGeneration.h` declares, and checks:

- T1. The twin's core encoding equals the imported UCB's and the body's `ucb.core` bytes, both encoded with the requesting UFE as the holder for a function body. An import only ever replaces generation, so the three are equal byte for byte, and every string constant register of the import is an atom, as in the twin.
- T2. The restored parse fields equal the twin's: the executable's features, lexically scoped features, captured-variables bit, last line and end column, and for globals and direct eval the twin UCB's directives and the provider's `sourceURL` and `sourceMappingURL` directives as the import left them (compared as the last paragraph of this section says); the UFE's three fields for function bodies.
- T3. For a program, `variableDeclarations()` and `lexicalDeclarations()` iterate in the same order in the twin and the import.
- T4. The UCB feedback equals the capture record: predictions, array state, arithmetic bits, the exit sites as a set, `didOptimize`, both quick bits, the counter's T and P, the children's singleton bits and the scope singletons.
- T5. The child UFEs' fields are the twin's, parse fields unparsed in both. Each child's TDZ chain has the twin child's links, environment by environment, and ends in the requesting UFE's own chain object wherever the twin child's does (codec E14).
- T6. The constant cells have the twin's forms, which the core bytes do not show: each `JSCellButterfly` constant has the twin's structure, the elements of one with `cellButterflyOnlyAtomStringsStructure` are the `JSString`s `atomStringToJSStringMap` holds for their atoms, as the twin's are, and each `RegExp` constant is the cell the twin holds at that index.

`verifyMatched` checks that the UCB's core encoding, with its slot's UFE as the holder for a function UCB, equals the body's `ucb.core` bytes, the natively decoded or live UCB being its own native twin, that every constant the atom map marks is an atom, and for a seeded one T4. `verifySuppliedDigest` checks T7, that a supplied root digest equals `sourceDigest` of the root's text, whatever strict says. The engine calls it whenever `rootSourceDigest` returns a digest of origin `BuiltinMetadata` or `Provider`; it leaves the registry's verified set alone, so S2 behaves in a twins build as in any other. `verifyRegistry` first runs `vm.heap.collectNow(Sync, CollectionScope::Full)`, which ends by sweeping synchronously (`Heap::collectNow` calls `sweepSynchronously` and `assertNoUnswept`), so every cell that died has run its destructor and dropped its entries (F12) and no entry of a dead but unswept cell reads as a violation. It then checks I2, I3 and I8 against the live heap: every registry entry names a live cell of the VM, found by walking the heap's live `UnlinkedCodeBlock` and `UnlinkedFunctionExecutable` cells, and every child UFE of a recorded UCB has the identity its parent's record gives it. It returns the number of violations whether or not a twin report is open, and reports each one through the sink only when the sink is non-null (SPEC-integrator.md R-ALL-2). `$vm.jitCacheUCBStatistics({ verifyRegistry: true })` runs it (manifest M4); without the argument the call collects nothing and returns no `registryViolations`.

`UCBTwinGeneration.h` declares, under `ENABLE(JITCACHE_TWINS)`:

```cpp
struct TwinParseResults {
    CodeFeatures features { 0 };
    LexicallyScopedFeatures lexicallyScopedFeatures { NoLexicallyScopedFeatures };
    bool hasCapturedVariables { false };
    int lastLine { 0 };        // globals and direct eval: what recordParse would give the executable
    unsigned endColumn { 0 };  // likewise
};
// Each generates what the request's native generation would, but writes its parse results only into `results`: no
// executable, UFE, CodeCache, provider cache hook or registry changes. The parse itself still writes the provider's
// directives and interns TDZ environments, as described below. Null, with `error` set, on a parse or generation error.
UnlinkedCodeBlock* generateGlobalTwin(const RequestState&, TwinParseResults&, ParserError&);                // programs, modules, both evals
UnlinkedFunctionCodeBlock* generateFunctionBodyTwin(const RequestState&, TwinParseResults&, ParserError&);   // UFE bodies
```

`generateGlobalTwin` is defined in `runtime/CodeCache.cpp` on `generateUnlinkedCodeBlockImpl`, which gains a `TwinParseResults*` that, when non-null, receives what `executable->recordParse` would have. It passes the request's snapshot as the lexically scoped features, since the import has already overwritten the executable's with the parse's (F18), and reads the direct-eval flags (`needsClassFieldInitializer`, `privateBrandRequirement`, `isInsideOrdinaryFunction`) from `RequestState::globalExecutable`, as the native generation does, since no import changes them on the `DirectEvalExecutable`. `generateFunctionBodyTwin` is defined in `bytecode/UnlinkedFunctionExecutable.cpp` on `generateUnlinkedFunctionCodeBlock`, with the same out-parameter in place of `executable->recordParse` and without `CodeCache::updateCache`.

The twin's parse writes two kinds of native state. Every global parse sets the provider's `sourceURL` and `sourceMappingURL` directives (`Parser::parse`, when the parse mode is not a function's), so before it generates a global or direct-eval twin `verifyImport` snapshots the provider's two directives, the values the import left; after the twin it compares the provider's directives, which are now what the native parse writes, with the snapshot for T2, and then restores the snapshot, so the provider ends as it does in a run without twins. A direct eval's twin also interns its TDZ environments in `vm.m_compactVariableMap` (`generateUnlinkedCodeBlockImpl`), as its native generation does; those entries go when the twin's handles do (`CompactTDZEnvironmentMap::Handle::~Handle`).

### 13.3 JS tests

In `JSTests/jitcache/ucb/`; Bun-only tests are marked. "Statistics" means `$vm.jitCacheUCBStatistics()`.

The scripts follow the runner's directives and argument conventions (SPEC-integrator.md R-ALL-4; harness sub-SPEC sections 7.2 to 7.7), since the oracle compares each of their JITCache runs with an `Off` run (section 13).

- A jsc-hosted script holds its body in `(function main(role, scratch, artifact) { ... })(...arguments)`. A Bun-hosted one reads the same three values from `process.argv[2]` to `[4]` inside a function of its own, and Bun's oracle compares its output only (harness sub-SPEC section 7.7).
- A script prints only values that do not depend on its role and leaves nothing that does reachable when that function returns: no global binding or property holds the role, a statistics object or a value read from the scratch directory.
- Under the `Off` role a script runs its Consumer path without `delta`, without statistics and without any assertion about imported, seeded, attached or captured state. Assertions about native behavior, such as results, a thrown error or `cachedDataRejected`, run in every role.
- A script that cannot keep its role out of its heap declares `// jitcache-heap: off` with its reason (harness sub-SPEC section 7.2).
- Statistics, `$vm`, the shell's `jitcache*` functions and Bun's test exports exist only in twins builds (manifest M4; harness sub-SPEC sections 5.2 to 5.4). A script asserts on them only where they exist, so it also runs in the release build, where the oracle comparison is its check; a script whose sequence cannot run without one, such as one that rewrites a section with `jitcacheRewriteSection` or calls `jitcacheDelta`, declares `// jitcache-requires: twins`.

| test | checks |
|---|---|
| `self-test.js` | twins only: `$vm.jitCacheUCBSelfTest()` passes in one run of each configuration, `Producer`, `Consumer` with strict on, `Consumer` with strict off and `ConsumerProducer`, in that order; a last `Consumer` run calls `$vm.jitCacheUCBSelfTest({ invalidMaterial: true })`, which passes and leaves cache activity off with the fault at `ucb.supplied-digest` (`jitcacheStatus()`; `jitcache-expect-fault: 4 ucb.supplied-digest`). Runs 1 to 4 exist only to run the self-test, so each declares `jitcache-expect-no-install` |
| `request-kinds.js`, `request-kinds.mjs` | program, module, indirect eval, nested direct eval, Function constructor, JSC builtins, class fields, base and derived default constructors, generator, async and async-generator bodies, one function both called and constructed: each imports (statistics) and its twins pass |
| `parse-fields.js` | F3: `function f(a = 0) { return arguments.callee; }` throws in the consumer as in the JITCache-off run; `f.caller` and `arguments` behave alike for strict and sloppy imported functions |
| `global-var-order.js` | F10: a program with 40 `var`s and 20 `let`s, then one that redeclares several: `Object.keys(globalThis)` and the error message equal the JITCache-off run |
| `role-keys.js` | a `"use strict"` program, an indirect eval of `"use strict"` text, a sloppy function's direct eval of `"use strict"` text, and, in a variant marked Bun, a program evaluated after `vm.compileFunction` has left its context's global scope extension set (as in `with-scope.js`), each captured by a Producer: each imports in a Consumer and in a ConsumerProducer (statistics), and a ConsumerProducer that generated them commits bodies a Consumer imports; that ConsumerProducer has nothing to install and declares `jitcache-expect-no-install` |
| `context-miss.js` | a program and its functions generated in the producer and decoded from the provider's cached bytecode in the consumer: the decoded functions are seeded, a program with more than nine `var`s misses `CoreDigest` where its decoded declaration layout differs, and one with nine or fewer is seeded; the reverse, decoded in the producer and generated in the consumer: every program and function import, and every attach to a generated UCB, misses `Provenance`, so that consumer installs nothing (`jitcache-expect-no-install: 1:1`); a third sequence, generated in the producer, decoded in a ConsumerProducer that seeds the functions, runs them further and recaptures them with `delta`, then generated in a Consumer: each recaptured function body imports in the Consumer (statistics), and in twins runs its `ucb.identity`, read with `jitcacheReadSection`, holds provenance `Generated`; runs identical |
| `context-on-demand.js` | sequences `Producer; Consumer` and `Producer; ConsumerProducer`, with a part that only the second run executes: a program, a `new Function` body, the first call of a JSC builtin, and direct evals of fresh strings under a lexical scope with a thousand bindings, none with a body in the artifact. Across that part the Consumer computes no context and no holder digest (statistics `contextDigests` and `holderDigests` unchanged), and the ConsumerProducer computes one context per direct eval, at its record, and nothing else; in both, each root body the Producer captured computes its holder digest once, at its import; runs identical |
| `tdz-scale.mjs` | a module with 5,000 top-level `const`s and imports and 200 small exported functions, each holding a nested closure, and a function wrapping the same bindings as `const`s around the same functions, as Bun wraps a CommonJS module: in the Consumer every function and closure imports (statistics), and `tdzEnvironmentDigests` grows by the number of distinct environments the holder digests reach, not by the number of functions; in twins runs each small function's `ucb.core`, read with `jitcacheReadSection`, has the size it has in a module of ten bindings, and T1 and T5 hold; the run equals the JITCache-off run |
| `start-line.js` (Bun) | one text holding a class with instance fields, a static block and a nested class, in two `Producer; Consumer` sequences: each producer runs it through `vm.Script` under `lineOffset` 0, and each consumer under the `lineOffset` its sequence index picks, 0 then 10. Each body imports under 0. Under 10 each misses `Context`, except a field initializer, whose linked source starts on line 1 under any first line (F21) and which misses `Holder`, so the consumer under 10 has nothing to install (`jitcache-expect-no-install: 1:1`); the twins pass, and the stack traces of errors thrown in the field initializers and the static block equal the JITCache-off run in both consumers |
| `with-scope.js` (Bun) | the jsc shell can set no global scope extension, so the extension comes from `vm.compileFunction`, which installs one on its context's global object and never removes it, and programs and indirect evals evaluated after it carry the with-scope bit (`Interpreter::executeProgram`, `globalFuncEval`). One program text evaluated before and after that call, with a body captured for each bit: no body is ever installed on a UCB whose record holds the other bit, including when a CodeCache hit serves a UCB recorded under the other bit (U8 covers the request-key test of an attach directly); a provider payload generated under the other bit decodes natively, gets no record, the request misses `RequestKey`, and none of its functions imports or is captured (statistics) |
| `decoded-slots.js` | a program whose provider carries cached bytecode with nested function bodies, in the producer and the consumer (the jsc shell's bytecode cache, and Bun's `--compile --bytecode` in a marked variant): the requested slot of each lazily decoded function is seeded at its first call (statistics), its own children keep their lazily cached slots and are seeded at theirs, a slot it did not ask for is recorded as decoded; at its own first request it attaches when every constant the body's atom map marks decoded as an atom (only inline strings, SPEC-ucb.codec.md E10), and with a longer marked string constant it misses `AtomMap` without a stamp and attaches at a later request once a property-key use has made that constant an atom; T4 holds; `new F()` first on an ordinary function whose call body alone was cached records the decoded call slot without settling the request, and its empty construct slot imports or is generated and recorded |
| `compile-function.js` (Bun) | `vm.compileFunction` without `cachedData` in producer and consumer: the wrapper comes through the program request point and is recorded there (it never runs, so no body holds it), and the user function and its construct body import at their first call and first `new` (statistics); with the same `cachedData` in both: the wrapper and the user function have no record, nothing in them imports or is captured, and `cachedDataRejected` is unchanged; every run equals the JITCache-off run. The `cachedData` case calls the user function only in release runs, since ASan reports its use-after-free (F11) with JITCache off as well. A consumer that runs the `cachedData` case alone has nothing to install and declares `jitcache-expect-no-install` |
| `vm-script-cached-data.js` (Bun) | `vm.Script` with `cachedData`: its block has no record; `runInThisContext` imports at the program request point |
| `drop-and-reimport.js` (Bun) | `Bun.gc(true)` between calls: dropped UCBs import again at the next request; the registry counts drop with them. The runs keep the Image lane's twin check, whose twin CB keeps nothing alive past it (SPEC-image.md I20) |
| `live-attach.js` | in a ConsumerProducer VM, a body generated natively while its file was absent, then committed by this VM: while its code is parked in the sharing slot, a filled-slot request does not attach and a loop of `loadString` over one text (`vm.runInThisContext` under Bun) computes no source digest after its first request (statistics `sourceDigests`); once the parked code is released, the next CodeCache hit or filled-slot request attaches and its newborn CodeBlock installs the body, and a later release and attach at the same commit identifier re-checks nothing; a request at an unchanged index token after a miss reads nothing (statistics `BodyUnchanged`); with `jitAllowlist` excluding the body, the gate drops the import (`gateDrops`) and later requests read nothing, and the run with that allowlist, which has nothing to install, declares `jitcache-expect-no-install` |
| `invalid-material.js` | with strict on and `// jitcache-requires: twins`, through `jitcacheRewriteSection` (harness sub-SPEC section 5.2), which rewrites a section and reseals the container: each malformed field of sections 7 and 8, a core naming an unknown private name, a link-time constant out of range, a `RegExp` constant with an invalid pattern, a count mismatch, an atom map that marks a constant that is not a string, a butterfly map that marks a constant that is not a butterfly of strings, a constant bit on a constant that is not a `SymbolTable`, a nonzero byte 39 in `ucb.feedback`, a holder digest on a program body, and in `ucb.core` a truncation, a nested offset shifted outside the payload, a string length past its record, an unknown `CachedJSValue` kind, an instruction with an unknown opcode and one that runs past the stream, each resealed; cache activity turns off at the named step and the run equals the JITCache-off run. Each case is a sequence of its own, picked by the sequence index, whose Producer captures and rewrites the body and whose Consumer, run 1, imports it, so run 1 declares `jitcache-expect-fault` for each step these cases reach, `ucb.identity`, `ucb.feedback`, `ucb.decode` and `ucb.closure`, and `jitcache-expect-no-install: 1` |
| `holder-miss.js` | twins only, through the same helper, a function body whose `holderDigest` is rewritten: its import and its attach to a generated UCB miss `Holder`, the miss is stamped, cache activity stays on, and the run equals the JITCache-off run; since that body never installs, each `Consumer` or `ConsumerProducer` run declares `jitcache-expect-no-install` |
| `strict-reuse.js` | a body whose `coreDigest` differs from a live generated UCB's encoding, rewritten through `jitcacheRewriteSection` (twins only, as `holder-miss.js`): invalid material at `ucb.strict-core` with strict on, attach with strict off; the same body against a live decoded UCB: miss `CoreDigest` with strict on and off. The run that meets the generated UCB with strict on declares `jitcache-expect-fault` at `ucb.strict-core`; the body installs neither there nor where its live UCB is decoded, so those runs declare `jitcache-expect-no-install` |
| `feedback-roundtrip.js` | exit sites from a DFG speculation failure, arithmetic observations, array flags, both quick bits, `didOptimize` True and False, closures created twice, a closure scope and a module environment created twice: T4 holds, and no DFG compile in the consumer folds a scope or function the producer saw created twice |
| `root-holder.js` | a `new Function` text whose function is created twice from one executable, and a builtin whose function is created twice per realm, in the producer: in the consumer each root's bodies import at their first call (statistics), the root UFE's own singleton bit is false until the consumer creates a second function of one executable, as in the JITCache-off run, and the results equal that run |
| `llint-counter.js` | bodies captured right after finalization, whose progress is fractional under memory pressure, and after `delta`: T and P restored up to `float` rounding (T4), the crossing after the same number of points as native `setThreshold`, no `jitSoon` after the import |
| `array-literals.js` | functions whose all-string array literals feed `includes` and `indexOf`, a literal whose strings precede a spread, one with an elision and one mixing strings and numbers: every imported constant passes T6, and the results, including searches for a string no literal holds and for one built at run time, equal the JITCache-off run; in a consumer that decodes the same functions from the provider's cached bytecode, the seeded butterflies stay plain, as in the JITCache-off run |
| `regexp-constants.js` | regular expression literals whose pattern and flags a live `RegExp` object also holds; in the producer that object runs and `$vm.deleteAllCodeWhenIdle()` clears its compiled state before the bodies are generated and captured, and in the consumer the same happens before they are imported: no invalid material with strict on, T1 and T6 hold, and results equal the JITCache-off run; a Bun variant clears the state with `Bun.shrink()` |
| `atom-constants.js` (Bun) | `===` against string constants longer than an inline string, some also used as property names elsewhere in the program, in bodies captured from a generated UCB and from one Bun decoded from bytecode: every string constant of a UCB imported from the first is an atom, the second never imports; a Bun-decoded function is seeded from a body of either provenance, with each marked constant an atom before publication; two consumer runs that first call the functions in different orders both seed; with `useConcurrentJIT` off in every run, which the Image lane's twin check needs (SPEC-image.md, section 17.2), every install passes that check without a skip, including where a constant became an atom after the producer compiled (SPEC-image.md, T18); results equal the JITCache-off run |
| `borrowed-bytecode.js` (Bun) | a `--compile --bytecode` executable whose blocks borrow their instructions: capture encodes them, and a consumer seeds them |
| `builtin-roots.js` (Bun) | JSC builtins, and Bun internal modules both created and decoded by `decodeBuiltinFunction`: each root gets one identity either way, and its bodies import or are seeded at their first call (statistics); creating a global object computes no source digest for a JSC builtin (`sourceDigests` unchanged while `suppliedSourceDigests` grows), and an internal module's digest comes from its provider whether it is created or decoded |
| `supplied-digests.js` (Bun) | a `--compile --bytecode` executable run as producer and then as consumer, with strict on: no source digest is computed while its modules and the internal modules it loads are decoded (`sourceDigests` unchanged, `suppliedSourceDigests` counts them), its decoded functions are seeded, and `new` on a function written as an ES5 constructor imports its construct body after one verification of its module (`suppliedDigestVerifications`), while a direct eval in a decoded function imports with none; the same consumer with strict off verifies nothing; the same app built without `--bytecode` imports its roots, verifying each module once with strict on; an executable written without the digest record computes its modules' digests and imports as before; T7 holds in twins runs, and every run equals the JITCache-off run |

Stress: the same tests under `--collectContinuously=true` and `--useUnlinkedCodeBlockJettisoning=true`, and every debug run with `--destroy-vm`, ASan and LSan clean; `$vm.jitCacheUCBStatistics({ verifyRegistry: true })` reports no violation at the end of each.

## 14. Bench obligations

- B1. The key-side digests THREAD's opening measures apart from installation, with cold and warm page caches: SHA-256 time per KiB of root source computed from text; the share of root digests that were supplied (`suppliedSourceDigests` against `sourceDigests`); each context digest, a root body's with the holder digest it covers; each TDZ environment's first digest against its name count; and, with strict on, S2's verification per provider.
- B2. Request-point work per body, each part apart, against the native compile THREAD's bound uses, with strict off as the bench measures the default (THREAD Verification) and with strict on beside it: for an import, the identity and feedback parses, the decode, the closure checks, the butterfly rebuild, the seeding, the parse fields and the records, with the decode and the butterfly rebuild also against native generation of the same body; for a seeded decoded UCB, its matching (whose core digest runs whether or not strict is on), its atomization and its seeding, with the matching also against the native decode; for an attach, its matching. The accounting at the end of this section says which parts count toward the bound. When `VM::jitCacheState()->benchReport()` is non-null, the request point records these measurements with `BenchReport::record` in the integrator's `request` event (SPEC-integrator.harness.md section 9.2): the key, what it did (`import`, `seed` or `attach`), the time of each part above, the total of the parts that count toward the bound and, apart, the total of those THREAD measures separately; IB3 joins that event with the `install` event by key. The clocks follow SPEC-integrator.harness.md section 9.1: a total reads the thread CPU clock only where its counted span begins and ends, and the per-part breakdown reads `CLOCK_MONOTONIC` (`MonotonicTime::now()`), so no system call lands inside a span the bound counts.
- B3. Overhead on native paths with JITCache on, against JITCache off, per generated UCB, per decoded slot, per CodeCache hit and per filled-slot request, with an empty artifact and with a full one whose bodies' code is parked; a hit or request that ends before an attach computes no digest, and a request whose key has no body computes its key and no context or holder digest. The one context a request computes without a body, a direct eval's in a VM whose production is active (section 4.3), is measured per direct eval against the sizes of its TDZ and private-name sets and against the native `JSC::eval` miss it rides on.
- B4. A missed `openBody` and a `bodyVersion` lookup per request, in the steady state of R-INT-3.
- B5. Registry memory per UFE and per UCB, and in total for the largest workload of the bench, with the environment digests kept in environments (section 4.3) beside it, since neither is charged to the producer limit. The per-UFE figure, child entries and `ParentIdentity` nodes included, is reported beside `sizeof(UnlinkedFunctionExecutable)`.
- B6. Capture: `buildSections` time and peak memory per body, the encoder's tables and scratch included; the largest overshoot of a refused charge, against I16's bound of the rest of one encode.
- B7. Core digests: `coreDigestOf` per body, as S1 runs it at an attach to a generated or imported UCB and as every decoded UCB's match runs it, split into the encoder's work and SHA-256, beside the body's native compile.
- B8. Root creation: global object setup with a full artifact against JITCache off; the difference is one identity digest per builtin root (section 6.3.6), since every JSC builtin's source digest comes from its metadata, and root creation opens no body.
- B9. A compiled app (Bun's `--compile`, with and without `--bytecode`): startup with JITCache on and an artifact against JITCache off, with cold and warm page caches and with strict on and off, counting the pages of the executable's source text each run faults (`mincore` over the section, beside the process's fault counts), to confirm that with strict off a startup reads no source text the JITCache-off run leaves untouched, that descendants the producer never captured still decode, and, with strict on, that the only extra pages are those S2 reads, once per provider an import relies on.
- B10. Enclosing scopes: a module with 5,000 top-level `const`s and imports and 1,000 small functions, the same bindings and functions inside a CommonJS-style wrapper function, and the same functions in a module of ten bindings. Per function: C11's holder digest, the import's decode, the `ucb.core` bytes, `buildSections`, and `coreDigestOf` as a seeded decode runs it; per environment, its first digest. Apart from each environment's first digest, none of them may grow with the number of bindings (I25), and each is set beside the function's native compile for B2's accounting.

Accounting. THREAD's opening counts toward the bound everything JITCache does at a body's request point, which runs before the native link that creates the body's first CB (`ScriptExecutable::newCodeBlockFor` calls `unlinkedCodeBlockFor` before `FunctionCodeBlock::create`, and a global executable holds its UCB before its CB exists), and leaves out what it measures separately. For this lane, the bound covers parsing `ucb.identity` and `ucb.feedback`, matching (a decoded UCB's core digest, C10, and a child's C11 holder digest beyond its environments' first digests), the butterfly rebuild, the atomization, the seeding, the parse fields and the records. Apart from it are the import's decode of the core, which counts with decoding, `openBody`, which counts with reading the artifact, and the key digest with the context digests, the holder digest a root's context covers and each TDZ environment's first digest. The bound is measured with strict off (THREAD Verification), so strict's checks (SV1 to SV3, S1 and S2) never enter it; B2 measures each part apart, strict's checks beside the default, and B1 and B10 measure the key-side digests.

## 15. Manifest for `INTEGRATE-ucb.md`

The integrator applies these; the lane's implementers do not edit the files.

- M1. `Source/JavaScriptCore/Sources.txt`: `jitcache/JITCacheSHA256.cpp`, `jitcache/UCBKeys.cpp`, `jitcache/UCBRegistry.cpp`, `jitcache/UCBSections.cpp`, `jitcache/UCBFeedback.cpp`, `jitcache/UCBImport.cpp`, `jitcache/UCBRequests.cpp`, `jitcache/UCBCapture.cpp`, `jitcache/UCBTwins.cpp`, `jitcache/UCBSelfTest.cpp` (the last two are empty without `ENABLE(JITCACHE_TWINS)`).
- M2. `Source/JavaScriptCore/CMakeLists.txt`: the `jitcache` directory in the include paths. Bun includes none of the lane's headers, so none joins `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`.
- M3. `runtime/VM.h` and `runtime/VM.cpp`: R-INT-1.
- M4. `tools/JSDollarVM.cpp`, under `ENABLE(JITCACHE_TWINS)`: `$vm.jitCacheUCBSelfTest(options)`, which calls `JITCache::runUCBSelfTest` with `UCBSelfTestScope::InvalidMaterial` when `options.invalidMaterial` is true and `Configured` otherwise (section 13.1), and throws an `Error` carrying the failure; and `$vm.jitCacheUCBStatistics()`, which returns an object with every counter of `UCBStatistics` by name, the registry's `counts()`, and, when called as `$vm.jitCacheUCBStatistics({ verifyRegistry: true })`, `registryViolations`, the count `JITCache::verifyRegistry` returns when called with `VMState::twinReportSink()`, which is null when no twin report is open, so the count is there without a report path; the call runs a full synchronous collection first (section 13.2).
- M5. The option rows of options.md that name this lane, in the integrator's table; `OptionsList.h` itself is unchanged.
- M6. `parser/SourceProvider.h`: the virtual `SourceProvider::jitCacheSourceDigest()` of section 6.2.8, whose default returns `std::nullopt`. The header is exported and every provider includes it, so the edit adds only that member, with standard types, and no `jitcache/` include.

## 16. Invariants

- I1. A UCB has at most one record; its key and recorded context never change while it lives, and the context digest computed from that record is the same whenever it is computed, since a root body's holder UFE keeps its holder digest while it lives (section 4.3).
- I2. A UFE has at most one identity, which never changes while it lives.
- I3. An owner's destructor removes its record or identity, so no map entry outlives its cell's destruction.
- I4. The registry lock is a leaf, held only where no cell is allocated, no GC deferral begins or ends, no barrier runs, no pending import is destroyed and no reference to a `SourceProvider` is dropped.
- I5. UCB feedback, the children's singleton bits and the scope singletons are written only by section 8.3, on a UCB that no holder has published and no CodeBlock references. The one exception is in `ENABLE(JITCACHE_TWINS)` builds: the Image lane's twin check saves, overwrites and restores a published UCB's arithmetic profiles through `ArithProfile::restoreBits` (SPEC-image.md section 17.2 and I20, R-UCB-4). No JITCache step writes a root UFE's own singleton bit. Parse fields are restored only by an import, before `tryImport` returns.
- I6. A UCB has at most one pending import, attached only while its sharing slot is empty and only from a body whose key and context equal the UCB's record, for a request whose key equals the record's.
- I7. A UCB with a pending import fits its body: an imported one comes from a body of provenance `Generated`, passes C11 for a function and, with strict on, C1 to C9; a seeded decoded one has the body's core encoding and, with strict on, passes C3, C4, C7, C8 and C9; a reused decoded one has the body's core encoding, passes C10 and, with strict on, C3, C4, C7 and C8; a reused generated or imported one has a body of provenance `Generated`, passes C11 for a function and, with strict on, C3, C4, C7 and C8, and its core encoding equals `coreDigest` (S1). With strict off, what C1 to C9 would check rests on the integrity checks of THREAD Session (section 11.2). An imported one's core encoding equals `coreDigest` by the codec's round trip, which U7 and twin T1 check. A body of provenance `EmbedderDecoded` fits only a natively decoded UCB.
- I8. Every child UFE of a UCB with a record has the identity (that key, its table, its index).
- I9. With no JITCache state, or with key tracking off, every hooked native function behaves exactly as before the edit.
- I10. No import, seeding or attach runs unless imports are enabled.
- I11. An imported body's parse fields are restored before `tryImport` returns, so before publication and before `newCodeBlockFor` reads them.
- I12. After an import or a seeding the LLInt counter has the captured base threshold and progress, up to the `float` rounding of `m_totalCount` and exactly for a deferred record, crosses after the points native `setThreshold` would arm from them, and no `jitSoon` or `jitNextInvocation` follows.
- I13. In its JITCache mode the codec writes no child code block, writes each child UFE's parse fields as unparsed, preserves hashed `InlineMap` layouts, writes no string's atom-ness and no butterfly's structure, writes a `RegExp` as its pattern and flags only, writes a function core's TDZ chains relative to its holder's chain and a descriptor's chain as one start record, decodes every string constant register as an atom and decodes UFEs with `m_isGeneratedFromCache` false.
- I14. Equal UCBs encode to equal core bytes in any process, whatever the state of the VM's shared `RegExp` cells, when function UCBs are encoded with holders whose chains hold the same names; so do UCBs the native decoder produced from equal bytecode-cache bytes, whatever their decode history, and a natively decoded function UCB and a generated one from the same inputs when the payload's generator ran with the option values `start` fixes.
- I15. A capture allocates no cell, reads no profile sample and stops for no collector.
- I16. A capture charges every byte it allocates for production (section 9.2; SPEC-ucb.codec.md, E8), except the environment digests it keeps, which serve every role (section 4.3). Each charge comes before its allocation, except the growth of the encoder's tables and scratch, which is charged as it happens. After a refused charge, the encode in flight cannot unwind and runs to its end, so the overshoot is the rest of that one encode: its later pages, each up to 64 MiB, and its later growth of tables and scratch. Every other allocation of the capture is refused before it happens.
- I17. A UCB is recorded only under the key and context the current request gives it, with its holder's identity, its specialization and its own mode; a natively decoded root whose with-scope bit differs from its request's is not recorded at all (section 6.3.3).
- I18. A holder receives the UCB its native path produces: an import replaces only a generation, and a native decode always runs where the native path decodes. The lane changes a natively decoded UCB before publication only by seeding it and by making marked string constants atoms. A marked constant decoded from an embedder's string table is a VM-wide cell that published UCBs share, so their constant becomes an atom too, the change a property-key use of it makes natively (F19).
- I19. A request settles on at most one UCB, the one it asked for; `didDecodeCachedSlots` also records the other decoded slot without settling.
- I20. Keys and contexts read the request's lexically scoped features only from the snapshot its request object took before any native step.
- I21. A CodeCache hit or filled-slot request whose UCB has parked code, no body in the index at its key, a miss stamped at the key's current index token, or a match in every check but C10 against the current file computes no digest. Any request computes its context and holder digests at most once each, and only once a body at its key has passed the key comparison, except a direct eval's context in a VM whose production is active, which its record computes (section 4.3).
- I22. An imported UCB's constant cells have the forms generation gives them: every string constant register is an atom, every `RegExp` constant is the cell `RegExp::create` returns, and every butterfly the butterfly map marks has `cellButterflyOnlyAtomStringsStructure` and holds the `JSString`s `atomStringToJSStringMap` holds for its atoms, before the UCB is published.
- I23. A body's context records the first line of the source it was produced from: an import or a seeding serves only a request whose source starts on that line, and an attach only a UCB recorded for such a source.
- I24. A root's source digest is read from its text only when neither its builtin metadata nor a provider it spans supplies one (section 4.4). With strict on, no import relies on a provider's supplied digest before the registry has verified it against the text, once per provider in the VM (S2); a direct eval's import relies on none (section 4.4).
- I25. Apart from the first digest of each TDZ environment, what a body's own steps cost does not grow with the names its enclosing scopes declare (F26): a holder digest hashes its UFE's own fields and one record per link of its chain, and a function core, with its encode, its decode and `coreDigestOf`, holds only the TDZ links its own generation created. Each environment's names are sorted and hashed at most once while it lives, by the first holder digest that reaches it.
- I26. With strict on, an import's decode reads nothing outside the core section and hands back an instruction stream made of whole instructions that ends exactly at the stream's end, or it fails and the import is invalid material (SPEC-ucb.codec.md, E15). With strict off, the lane trusts a checksum-valid core from a matching build to be both.

## 17. Tasks

Task 0 writes every interface first; the other tasks then touch disjoint files and compile against task 0's headers alone, except task 4, which also needs task 3's `InlineMap` members and task 1's `SHA256`. The self-tests run through `$vm.jitCacheUCBSelfTest()`, so they run once tasks 1 to 10 and the integrator's task 12, which applies manifest M4, have landed; the twins run from task 10 on, in twins builds with a twin report (R-INT-10); and the JS tests also need the integrator's runner (its task 13). No task of this lane waits on an integrator task that waits on it. Integrator requirements are stubbed in tests until the integrator lands.

0. Headers: `JITCacheSHA256.h`, `UCBKeys.h`, `UCBRegistry.h`, `UCBSections.h`, `UCBFeedback.h`, `UCBImport.h`, `UCBRequests.h`, `UCBCapture.h`, `DirectEvalSite.h`, `UCBTwinGeneration.h` and `UCBTwins.h`, exactly as sections 4 to 10 and 13.2 declare them, with no definitions beyond the inline members those sections write out; and the declaration edits of native headers: the accessors of section 8.4 in `ValueProfile.h`, `ArrayProfile.h`, `ArithProfile.h`, `UnlinkedCodeBlock.h` and `DFGExitProfile.h`, the codec entry points and the new `Decoder` members in `runtime/CachedTypes.h` (SPEC-ucb.codec.md, sections 2 and 4), `CompactTDZEnvironment::m_contentDigest` in `parser/VariableEnvironment.h` (section 4.3), the friend declaration of `JITCache::atomizeStringConstant` in `runtime/JSString.h`, the new parameter of `DirectEvalExecutable::create` in `runtime/DirectEvalExecutable.h`, the new parameter of `decodeCachedCodeBlocks` in `bytecode/UnlinkedFunctionExecutable.h`, and the two new fields of `BuiltinSourceMetadata` in `builtins/BuiltinExecutables.h`.
1. SHA-256: `JITCacheSHA256.cpp`, portable and accelerated paths, and U1.
2. Keys: `UCBKeys.cpp` (section 4), and U2, U3; U3's holder-digest cases run once task 4 has landed. `rootSourceDigest` needs manifest entry M6.
3. `InlineMap` layout transport in `Source/WTF/wtf/InlineMap.h` (SPEC-ucb.codec.md), and its part of U7.
4. Core codec, UFE descriptor and `tdzChainDigest` in `runtime/CachedTypes.cpp` (SPEC-ucb.codec.md), including E14, the validating decode of E15 and the `decodeBuiltinFunction` hook call, and the codec part of U7. Needs task 1, whose `SHA256` the chain digest uses, and task 3.
5. Sections and feedback: `UCBSections.cpp`, `UCBFeedback.cpp` and the definition of `DFG::ExitProfile::restoreFrequentExitSites` in `DFGExitProfile.cpp` (sections 7, 8); U5, U6.
6. Registry: `UCBRegistry.cpp` (section 5), and U4.
7. Engine: `UCBImport.cpp` (sections 6.3 to 6.5 and 6.7), with its twin calls, and U8.
8. Call sites: `UCBRequests.cpp` and every native edit of sections 5.5 and 6.2 with the twin generation helpers: `runtime/CodeCache.h`, `runtime/CodeCache.cpp`, `bytecode/UnlinkedFunctionExecutable.cpp`, `bytecode/UnlinkedCodeBlock.cpp`, `runtime/DirectEvalExecutable.cpp`, `interpreter/Interpreter.cpp` and `builtins/BuiltinExecutables.cpp`.
9. Capture: `UCBCapture.cpp` (section 9).
10. Twins and self-test: `UCBTwins.cpp` and `UCBSelfTest.cpp` (sections 13.1 and 13.2).
11. JS tests: `JSTests/jitcache/ucb/` (section 13.3).
12. Supplied digests (section 6.2.8): the builtins generator's two scripts, then, in `~/bun`, the standalone graph's record, `ResolvedSource`, the standalone `ResolvedSource` in `jsc_hooks.rs`, `Zig::SourceProvider`, the builtins section's digest table and `makeInternalModuleSource`. Needs task 0; U9 and `supplied-digests.js` test it.
