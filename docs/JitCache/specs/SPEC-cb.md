# SPEC-cb: CB state

Lane 3 of THREAD Execution: the state a baseline CB learned, captured from a producer's CB and seeded into the consumer's newborn CB. This file is the lane's whole design and has no sub-SPEC. [SPEC-cb-history.md](SPEC-cb-history.md) records why its non-obvious decisions are as they are, and binds nothing.

Capture reads what each piece has accumulated, never a pending sample, at THREAD Capture's two points: after a baseline compile installs its code, and in `delta` (section 4). It writes two sections whose arrays follow native index-space order, so validation compares counts with the CB and seeding walks the CB's metadata in step with the records (section 3). Restoration seeds the newborn CB around native setup: profiles and tier-up history before setup, the counter after it, where the engine's own check re-slices the captured progress against the consumer's pool and the lane keeps the result at least two entry increments short of crossing; a body with a polymorphic site keeps the counter setup armed (section 5). With strict on, the lane also checks every byte it reads and every assumption it makes about the CB (sections 3.4, 4.4 and 5.2).

"VM thread" is the thread that holds the VM's API lock and heap access. The lane's code lives in `Source/JavaScriptCore/jitcache/`, namespace `JSC::JITCache` (THREAD Execution), under `ENABLE(JIT)`; the lazy-operand parts compile under `ENABLE(DFG_JIT)`, which every supported target has.

## 1. Scope

The lane carries these pieces:

| id | piece | native home | what travels |
|---|---|---|---|
| P1 | value profiles | the `ValueProfile` array in front of the CB's `MetadataTable`, offsets 1..N (`MetadataTable::valueProfileForOffset`) | `ValueProfileBase::m_prediction` |
| P2 | argument profiles | `CodeBlock::m_argumentValueProfiles` | `m_prediction` |
| P3 | lazy-operand profiles | `CodeBlock::m_lazyValueProfiles` (`CompressedLazyValueProfileHolder`, `LazyOperandValueProfile`) | each profile's `LazyOperandValueProfileKey` and `m_prediction` |
| P4 | array profiles | `ArrayProfile` in the metadata of 16 opcodes (families A0..A15, section 3.1) | `m_observedArrayModes` and `m_arrayProfileFlags`, the pruning mark included |
| P5 | allocation hints | `ArrayAllocationProfile` in 4 opcodes (F16..F19) | the 16-bit hint: indexing type and largest vector length |
| P6 | iteration modes | `IterationModeMetadata::seenModes` in 4 opcodes (F20..F23) | the bits |
| P7 | enumerator modes | `m_enumeratorMetadata` in 5 opcodes (F24..F28) | the byte |
| P8 | `to_this` status | `OpToThis::Metadata::m_toThisStatus` (F29) | the status |
| P9 | `jneq_ptr` branch bit | `OpJneqPtr::Metadata::m_hasJumped` (F30) | the bit |
| P10 | profile deferrals | `CodeBlock::m_optimizationDelayCounter` | the count |
| P11 | reoptimization count | `CodeBlock::m_reoptimizationRetryCounter` | the count |
| P12 | baseline counter | `BaselineJITData::m_executeCounter` (`BaselineExecutionCounter`) | `m_counter`, `m_totalCount` and `m_activeThreshold` as captured; the consumer keeps the progress P = `m_totalCount + m_counter` and the base threshold T = `m_activeThreshold`, and recomputes a finite threshold's slice (section 5.3). A body with a polymorphic site carries none of it and keeps the counter native setup arms |

The lane also writes its share of THREAD Capture's body summary (section 3.3).

What stays as native code leaves it ([history](SPEC-cb-history.md#what-stays-local)):

| state | native home | rule |
|---|---|---|
| pending samples: `m_buckets` of value, argument and lazy-operand profiles, the standalone failure buckets (`speculationFailureValueProfileBuckets`), `ArrayProfile::m_lastSeenStructureID` and `m_speculationFailureStructureID`, the last array of an `ArrayAllocationProfile` | profiles | THREAD Capture never reads, classifies or clears a pending sample |
| `catch` value profiles | `OpCatch::Metadata::m_buffer` | THREAD's omissions |
| the array profile of `get_by_val_with_this` | `OpGetByValWithThis::Metadata::m_arrayProfile` | nothing reads it (N2), so THREAD keeps it local as state the consumer recomputes before anything reads it |
| CB caches that hold cells and the LLInt caches: `GetByIdModeMetadata`, the `put_by_id`, `get_by_id_direct` and private-name metadata, every `m_cachedCallee`, `OpToThis::Metadata::m_cachedStructureID`, `ObjectAllocationProfile`, the scope metadata, the `new_array_buffer` constant butterfly | metadata and constant pool | THREAD Caches: they start as native linking leaves them |
| `m_shouldAlwaysBeInlined`, `m_didFailFTLCompilation` | `CodeBlock` | THREAD Restoration: local |
| `m_hasBeenCompiledWithFTL` | `CodeBlock` | set only by the DFG's FTL tier-up (`DFG::JITCode::setOptimizationThresholdBasedOnCompilationResult`), so it waits for the DFG capture (THREAD) |
| `m_didFailJITCompilation` | `CodeBlock` | set only when a baseline plan finds no executable memory (`BaselineJITPlan::finalize`), after the integrator's fault on that path (THREAD Execution), so it never reaches a capture (THREAD Restoration); strict capture confirms it clear (SC2) |
| `m_osrExitCounter` | `CodeBlock` | counts exits of the optimized CB that owns it (`handleExitCounts` in `dfg/DFGOSRExitCompilerCommon.cpp`), never of a baseline CB |
| `m_creationTime`, `m_previousCounter` | `CodeBlock` | time leases stay local; `finishCounter` resamples `m_previousCounter` (section 5.3) |
| `m_capabilityLevelState` | `CodeBlock` | a memo of `capabilityLevel()`, which setup recomputes for function code (`setupWithUnlinkedBaselineCode`) and which fills lazily otherwise; the capability class is a baked fact the Image lane compares |
| debugger fields, `m_isJettisoned`, `m_visitChildrenSkippedDueToOldAge`, `m_alternative` | `CodeBlock` | local by nature |

P3 and P11 travel although only optimizing-tier events write them (OSR exit for P3, N7; `countReoptimization` for P11, which runs when optimized code is jettisoned, when a DFG or FTL compile comes back invalidated and when FTL OSR entry fails in `tierUpCommon`), because THREAD names both (Execution, Capture and Restoration) ([history](SPEC-cb-history.md#what-stays-local)). THREAD's wait for the DFG capture covers the DFG- or FTL-set state no THREAD section names, such as `m_hasBeenCompiledWithFTL`.

Other lanes own the neighbouring state: call-link history and property-IC learning (ICs lane); `UnlinkedValueProfile`, `UnlinkedArrayProfile`, the arithmetic profiles, the exit profile, the LLInt counter, the quick tier-up bits and `didOptimize` (UCB lane); the coverage rates `m_livenessRate` and `m_fullnessRate` in `BaselineJITCode` (Image lane).

## 2. Native facts

The rules rely on these facts.

N1. Linking. `UnlinkedMetadataTable::link` (`bytecode/UnlinkedMetadataTableInlines.h`) gives every CB its own zeroed buffer, `[ValueProfile × N][LinkingData][offset tables][metadata]`, and returns null when the UCB has no metadata. `CodeBlock::finishCreation` placement-constructs each opcode's `Metadata` (`INITIALIZE_METADATA`) and links `OpNewArrayBuffer`'s allocation profile with `ArrayAllocationProfile::initializeIndexingMode(m_recommendedIndexingType)`, a copy-on-write type; every other allocation profile starts at `ArrayWithUndecided` with vector length 0. After linking, P1 to P9 are at this link state, and P10 and P11 are zero.

A UCB has no metadata when its generator added no metadata entry and no value profile, since only `UnlinkedMetadataTable::addEntry` and `addValueProfile` set `m_hasMetadata`; the bytecode cache's empty table (`UnlinkedMetadataTable::empty`) has none either, and both report `numValueProfiles()` 0. The CB's `metadataTable()` is then null and no family has an entry, yet the CB can be hot and baseline-compiled, with argument profiles and a baseline counter: `function id(x) { return x; }` is one, since `enter` and `ret` carry no metadata (SPEC-ics.md, section 4.2). `MetadataTable::forEach` reads the offset table through `this` (`get<typename Op::Metadata>()`) and `valueProfileForOffset` reads the `LinkingData` just before `this`, so every native walker tests the table first (`CodeBlock::forEachValueProfile`, `forEachArrayAllocationProfile`, `updateAllArrayProfilePredictions`).

N2. Pairing with the UCB copies. `CodeBlock::updateAllNonLazyValueProfilePredictionsAndCountLiveness` walks `CodeBlock::forEachValueProfile` (argument profiles, then `MetadataTable::forEachValueProfile` from offset 1 to N) with a running index into `UnlinkedCodeBlock::unlinkedValueProfiles()`, and skips the merge for builtins. `CodeBlock::updateAllArrayProfilePredictions` walks `FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE` (`bytecode/Opcode.h`) and then `OpIteratorNext::Metadata::m_iterableProfile`, with a running index into `unlinkedArrayProfiles()`, which `UnlinkedCodeBlock::allocateSharedProfiles` sizes by the same walk. Both merges index the UCB vectors through `std::span::operator[]` and check nothing else; its standard-library assertion stops the process on an index out of range (install.md). The one other array profile, `OpGetByValWithThis::Metadata::m_arrayProfile`, is in neither walk and has no reader: `CodeBlock::getArrayProfile`, through which the DFG parser (`ByteCodeParser::getArrayMode`), the fixup phase and OSR exit (`OSRExit::compileExit`, `FTL::compileStub`) reach array profiles, covers only the two walks' opcodes; the DFG's `op_get_by_val_with_this` case reads only `GetByStatus`; the LLInt goes straight to `slow_path_get_by_val_with_this`, which ignores the profile; nothing drains the samples `JIT::emit_op_get_by_val_with_this` stores into it; and the one read of its modes and pruning mark, `ArrayProfile::computeUpdatedPrediction(CodeBlock*, Structure*)` in `operationGetByValWithThisOptimize`, writes its result back into the same profile.

N3. Threads. The VM thread writes every piece (LLInt, baseline code, slow paths, OSR exit). Markers (`CodeBlock::visitChildren`), compiler threads (`BaselineJITPlan::compileInThreadImpl`, `ByteCodeParser::getArrayMode`, `LazyOperandValueProfileParser::prediction`) and `operationOptimize` drain predictions and array modes without locks, and the stopped-world finalizer (`CodeBlock::reconcileWeakReferencesAtGCEnd`) drains everything; no native reader of a profile takes a lock. The finalizer, which runs on whichever thread drives the collection, also writes P5, since it calls `updateAllPredictions`, which reaches `ArrayAllocationProfile::updateProfile` through `updateAllArrayAllocationProfilePredictions` (`updateProfile` asserts only `!isCompilationThread()`), and P8, since its `reconcileLLIntInlineCachesAtGCEnd` merges in `ToThisClearedByGC`. P6, P7 and P9 change only on the VM thread. No stopped-world writer runs during a capture, which holds heap access on the VM thread and calls nothing that stops for the collector (THREAD Capture), or reaches a CB before `installCode` publishes it (N6). Lazy-operand profiles are appended only on the VM thread (`CompressedLazyValueProfileHolder::addOperandValueProfile`).

N4. Counter. `ExecutionCounter`'s three fields are public (`bytecode/ExecutionCounter.h`), and `count()` is `double(m_totalCount) + m_counter`.

- Slicing. `ExecutionCounter::setThreshold` turns (T, P) into a slice. If T is `INT32_MAX` it calls `deferIndefinitely`, which stores `m_counter = INT32_MIN` and a zero total. Otherwise `threshold = applyMemoryUsageHeuristics(T, codeBlock) - P` crosses at once when not positive; else it is clipped to the slice ceiling (`clippedThreshold`), and `setThreshold` stores `m_counter = int32(-threshold)`, truncated toward zero, and `m_totalCount = P + threshold`, rounded only to float, so an unclipped slice adds its fractional part to `count()`.
- Process dependence. The multiplier comes from `ExecutableAllocator::memoryPressureMultiplier`, which reads the executable pool's `bytesAllocated` and, through `CodeBlock::predictedMachineCodeSize`, the VM statistic `machineCodeBytesPerBytecodeWordForBaselineJIT`. Both follow code sizes, which differ between processes (random constant blinding on x86_64, address-dependent branch compaction on ARM64), so a finite-threshold counter's split between `m_counter` and `m_totalCount`, and the fractions P accumulates, depend on the process. The pool count is an atomic (`FixedVMPoolExecutableAllocator::m_bytesAllocated`) that every thread allocating or freeing executable memory updates, JIT worker threads in `LinkBuffer` and other VMs included, so two calls of the check on the same counter state can disagree even on one thread.
- Bounds. The multiplier M is never below 1.0 (`memoryPressureMultiplier` clamps it, and `applyMemoryUsageHeuristics` asserts it), and the slice ceiling C, `maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, codeBlock)`, depends only on options and the CB's bytecode cost and code type. For a finite T, `ExecutionCounter::checkIfThresholdCrossedAndSet` reports a crossing and leaves the counter as it was when `count() >= M·T - min(T, C) / 2` (`hasCrossedThreshold`); otherwise `setThreshold` reports a crossing when `M·T - count()` is not positive, and else arms the slice `trunc(min(M·T - count(), C))`.
- Exits. `handleExitCounts` (`dfg/DFGOSRExitCompilerCommon.cpp`) writes all three fields directly: `m_activeThreshold = T`, `m_counter = -slice` and `m_totalCount = slice`, where the slice is `applyMemoryUsageHeuristicsAndConvertToInt(T)` clipped by the exiting tier's ceiling, `BaselineExecutionCounter::clippedThreshold` after a DFG exit and `UpperTierExecutionCounter::clippedThreshold` (`maximumExecutionCountsBetweenCheckpointsForUpperTiers`) after an FTL exit. T can therefore be `INT32_MAX` with a positive total, and the slice is then that ceiling, since the converted value saturates at `INT32_MAX`: no pool statistic shapes an infinite threshold's slice, but the producer's ceiling options do. A DFG plan thread zeroes `m_counter` when the plan is ready (`CodeBlock::forceOptimizationSlowPathConcurrently`).
- Setup. `CodeBlock::setupWithUnlinkedBaselineCode` creates the `BaselineJITData`, whose counter the constructor resets to zero (`ExecutionCounter::ExecutionCounter`), sets `m_previousCounter` from that fresh counter under `USE(BUN_JSC_ADDITIONS)`, which our build has, and then calls `optimizeAfterWarmUp`. `CodeBlock::optimizeAfterWarmUpImpl` takes t = `thresholdForOptimizeAfterWarmUp`, multiplied by `quickDFGTierUpThresholdFactor` and truncated to `int32_t` when the UCB's `isQuickDFGTierUp()` holds, and arms `setNewThreshold(adjustedCounterValue(t))`; the public `adjustedCounterValue` folds in the size factor, `evalThresholdMultiplier` and `1 << reoptimizationRetryCounter()`. `setNewThreshold` resets the counter before `setThreshold`, so a fresh arming leaves a progress between 0 and 1 point (the fraction that truncating the slice into `m_counter` leaves against its float copy in `m_totalCount`), or an indefinite deferral when the threshold clips to `INT32_MAX`.
- Aging. `m_previousCounter` is private, and `CodeBlock` offers only its setter, `snapshotExecutionCounterForAging(float)`; the old-age check compares it with `float(count())` (`CodeBlock::shouldJettisonDueToOldAge`).

N5. Tier-up history. `CodeBlock::shouldOptimizeNowFromBaseline` increments `m_optimizationDelayCounter` and returns early once it reaches `maximumOptimizationDelay`, so it never exceeds that option. `CodeBlock::countReoptimization` saturates `m_reoptimizationRetryCounter` at `reoptimizationRetryCounterMax`, which `Options` derives from `thresholdForOptimizeAfterLongWarmUp` (21 at the default 1000; `runtime/Options.cpp`). `CodeBlock::reoptimizationRetryCounter()` asserts that the count does not exceed that maximum, and `adjustedCounterValue` scales the threshold by `1 << reoptimizationRetryCounter()`.

N6. Newborn CB. At both install points of THREAD Restoration (before `setupLLInt` in `ScriptExecutable::prepareForExecutionImpl`, and the top of `JIT::compileSync` reached from `setupJIT`), the CB was created by `newCodeBlockFor` under `prepareForExecutionImpl`'s `DeferGCForAWhile`, its `jitType()` is `None`, `m_jitData` is null, and no cell a marker or compiler thread visits points to it until `installCode` stores it and barriers the executable (install.md, "Conditions around installation").

N7. Lazy-operand keys. Only OSR exit creates a lazy-operand profile: `MethodOfGettingAValueProfile::emitReportValue`, case `LazyOperandValueProfile`, calls `addOperandValueProfile` on the baseline CB of the exiting node's inline frame (`baselineCodeBlockForOriginAndBaselineCodeBlock`), keyed by that origin's bytecode index and the operand of the exiting `GetLocal` (`Graph::methodOfGettingAValueProfileFor`). `ByteCodeParser::get` built that operand through `InlineStackEntry::remapOperand`, so it names a slot of the machine frame: an inlinee's arguments and locals shift by `InlineCallFrame::stackOffset` into machine-frame locals, its tmps shift by `InlineCallFrame::tmpOffset`, which `ByteCodeParser::tmpOffsetForInlineeOf` sets past each caller's tmps and private tmps, and the parser's private tmps (`ByteCodeParser::allocatePrivateTmps`, used by the iterator and sort fast paths) start at `numTmps()` plus the private tmps already taken, in the root frame too. The bytecode index lies in the CB that holds the profile, but the operand belongs to the frame of whichever optimized compile inlined that CB, so an inlined CB can hold locals past its own `numCalleeLocals()` and tmps at `maxNumCheckpointTmps` or above. The engine only hashes and compares keys: `LazyOperandValueProfileParser::initialize` and `prediction`, `addOperandValueProfile`'s search, and `ByteCodeParser::injectLazyOperandSpeculation`, which looks a key up with the same remapped operand. The holder's order carries no meaning: `initialize` builds a hash map, `addOperandValueProfile` returns the one profile whose key matches, and `CompressedLazyValueProfileHolder::computeUpdatedPredictions` visits every profile. Key equality is field equality: `LazyOperandValueProfileKey` compares its `BytecodeIndex` bits and its `Operand`, and `Operand::operator==` compares the kind and the value. Two constructors assert on a key's operand: `Operand(OperandKind, int)` that a non-tmp kind agrees with `VirtualRegister::isLocal()`, and `LazyOperandValueProfileKey(BytecodeIndex, Operand)` that `Operand::isValid()` holds, which for a register excludes `VirtualRegister::invalidVirtualRegister` and for a tmp requires a non-negative value. `ByteCodeParser::newVariableAccessData` asserts that no `GetLocal` names a constant register.

N8. `new_array_buffer` hints. `ArrayNode::emitBytecode` (`bytecompiler/NodesCodegen.cpp`) emits `op_new_array_buffer` only for a non-empty literal whose elements are all constants. Its `m_recommendedIndexingType` is `CopyOnWrite` ORed with the least upper bound of the constants' types (`leastUpperBoundOfIndexingTypeAndValue`), so it is `CopyOnWriteArrayWithInt32`, `CopyOnWriteArrayWithDouble` or `CopyOnWriteArrayWithContiguous`, and the literal's `JSCellButterfly` constant is built in that type. Native bytecode maps `op_new_array_buffer` metadata entries to instructions one to one: `BytecodeGenerator::addMetadataFor` allocates one entry per emitted instruction, and neither `BytecodeGenerator::rewind` nor `BytecodeGeneratorification::run` removes an `op_new_array_buffer`. Linking seeds each entry's hint with its instruction's `m_recommendedIndexingType` (`link_arrayAllocationProfile` in `CodeBlock::finishCreation`). `ArrayAllocationProfile::updateProfile` only raises a hint: `leastUpperBoundOfIndexingTypes` is `std::max`, and a copy-on-write hint is capped at `ArrayWithContiguous` and tagged copy-on-write again. A native hint is therefore never below its instruction's type, in the order `CopyOnWriteArrayWithInt32 < CopyOnWriteArrayWithDouble < CopyOnWriteArrayWithContiguous`. Two readers trust the hint: `slow_path_new_array_buffer`, which the baseline reaches through `DEFINE_SLOW_OP(new_array_buffer)`, and `operationMaterializeObjectInOSR`, case `PhantomNewArrayBuffer`, in the FTL. Both reach it through the executing instruction's own entry, `bytecode.metadata(codeBlock)`, which `CodeBlock::metadata` resolves by indexing the opcode's metadata array with the instruction's `m_metadataID`, unchecked. When the hint differs from the constant butterfly's mode, each copies the literal into a new `JSCellButterfly` of the hint's type with `JSCellButterfly::setIndex` and replaces the CB's constant. `setIndex` never converts: a double butterfly stores `asNumber()` of each value and any other butterfly stores the raw `JSValue`, and `JSCellButterfly::visitChildrenImpl` visits only contiguous butterflies. A hint below the literal's type thus puts cells into an Int32 butterfly the collector never scans, or turns cells into doubles. Raising the type is safe: `JSCellButterfly::get` boxes the doubles of a double butterfly, and every int32 is a number. The DFG reads the constant butterfly's own mode, not the hint (`ByteCodeParser::parseBlock`, `case op_new_array_buffer`).

N9. Iteration modes. Native writers record, per opcode:

| opcode | bits | writers |
|---|---|---|
| `iterator_open` | `Generic`, `FastArray`, `FastMap`, `FastSet`, `FastString` and the eight `FastArray`, `FastMap` and `FastSet` `Values`/`Keys`/`Entries` modes (`0x1fff`) | `iteratorOpenTryFastImpl`, which `RELEASE_ASSERT_NOT_REACHED`s on the two async modes |
| `iterator_next` | `Generic`, `FastString` and the eight `Values`/`Keys`/`Entries` modes (`0x1ff1`) | `iteratorNextTryFastImpl`, `operationIteratorNextTryFast`, the LLInt's `op_iterator_next`, `JIT::emit_op_iterator_next` |
| `async_iterator_open` | `Generic`, `FastAsyncGenerator`, `AsyncFromSync` (`0x6001`) | `asyncIteratorOpenTryFastImpl` |
| `async_iterator_next` | `Generic`, `FastAsyncGenerator` (`0x2001`) | `slow_path_async_iterator_next_with_driver`, the LLInt's `op_async_iterator_next`, `JIT::emit_op_async_iterator_next` |

`ByteCodeParser::handleIteratorOpen` and `handleIteratorNext` mask out only the bits whose protocol watchpoint set is invalid and count `std::popcount(seenModes)` cases. A fast case that is not the last one branches to a new `failedBlock`, which only a later case consumes, and both functions end with `ASSERT(!failedBlock)`, so a set bit with no case leaves a reachable block without a terminal. `handleIteratorOpen` has a case for exactly the 13 `iterator_open` bits, and `handleIteratorNext` for exactly the 10 `iterator_next` bits. `handleAsyncIteratorOpen` and `handleAsyncIteratorNext` mask their input to their opcode's bits. The open slow paths also read the history through `canUseFastIterationMode`, which counts every set bit other than `Generic` toward `maxNumberOfFastIterationModes`.

N10. Realm state behind iteration modes. `handleIteratorOpen`'s `FastMap` and `FastSet` cases assert (`ASSERT_WITH_MESSAGE`) `mapProtoEntriesFunctionConcurrently()` and `setProtoValuesFunctionConcurrently()` and freeze the result. A release build freezes the empty value (`Graph::freeze`), so a site with that single mode exits on every execution. Both functions are lazy (`JSGlobalObject::m_mapProtoEntriesFunction` and `m_setProtoValuesFunction`): `MapPrototype::finishCreation` and `SetPrototype::finishCreation` materialize them for the lazy `Map` and `Set` classes (`FOR_EACH_LAZY_BUILTIN_TYPE_WITH_DECLARATION`), and the getters `mapProtoEntriesFunction()` and `setProtoValuesFunction()` materialize them alone. The map and set protocol watchpoint sets are valid from the realm's creation, so they mask nothing. Natively the bit implies the function: `getIterationMode` (`runtime/IteratorOperations.cpp`) returns `FastMap` only when the symbol iterator equals `mapProtoEntriesFunctionConcurrently()`, and `FastSet` likewise. Every other function or structure the four handlers read exists from the realm's creation: the Array, Iterator, String and AsyncIterator prototypes' functions, the iterator structures and the async link-time constants. Array profiles imply no realm state the DFG needs: a typed-array class the realm has not created yet makes `ArrayMode::originalArrayStructures` return an empty set, because `typedArrayStructureConcurrently` returns null and a `TinyPtrSet` holding null is empty, so `FixupPhase::checkArray` emits `CheckArray` instead of `CheckStructure`.

N11. Marking after publication. On the first `SlotVisitor` visit of a baseline CB in a cycle, `CodeBlock::visitChildren` runs `updateAllNonLazyValueProfilePredictions`, where `UnlinkedValueProfile::update` writes the union of the CB and UCB copies into both, and `updateAllLazyValueProfilePredictions`, which drains P3's pending samples into its predictions. GC deferral holds back only the stopped-world phases, and concurrent marking proceeds, so once `installCode` stores the CB in its executable and barriers the executable, a marker can reach the CB and change P1, P2 and P3.

## 3. Body file sections

The lane writes two sections per body: `cb.state`, which seeds the CB, and `cb.summary`, which scoring reads ([history](SPEC-cb-history.md#two-sections)). Both are opaque to the container; the integrator assigns their section type ids, locates and checksums them, and hands each over as R-INT-1 requires. Integers and floats are stored in native byte order, which every supported target makes little-endian (`static_assert(std::endian::native == std::endian::little)`). No field holds an address, a cell, a `StructureID` or a time (I1).

### 3.1 Families

A family is one profile field of one opcode's metadata. Within a family, entries follow metadata-ID order, the order `MetadataTable::forEach<Op>` visits. The table is fixed, and its position numbers are the format's index space ([history](SPEC-cb-history.md#dense-positional-sections)):

| family | opcode | field | kind | UCB copy |
|---|---|---|---|---|
| A0..A14 | the 15 opcodes of `FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE`, in macro order: `OpGetLength`, `OpGetByVal`, `OpInByVal`, `OpPutByVal`, `OpPutByValDirect`, `OpEnumeratorNext`, `OpEnumeratorGetByVal`, `OpEnumeratorInByVal`, `OpEnumeratorPutByVal`, `OpEnumeratorHasOwnProperty`, `OpNewArrayWithSpecies`, `OpCall`, `OpCallIgnoreResult`, `OpTailCall`, `OpIteratorOpen` | `m_arrayProfile` | array profile | paired |
| A15 | `OpIteratorNext` | `m_iterableProfile` | array profile | paired |
| F16..F19 | the 4 opcodes of `FOR_EACH_OPCODE_WITH_ARRAY_ALLOCATION_PROFILE`, in macro order: `OpNewArray`, `OpNewArrayWithSize`, `OpNewArrayWithSpecies`, `OpNewArrayBuffer` | `m_arrayAllocationProfile` | allocation hint | none |
| F20..F23 | `OpIteratorOpen`, `OpIteratorNext`, `OpAsyncIteratorOpen`, `OpAsyncIteratorNext` | `m_iterationMetadata` | iteration modes | none |
| F24..F28 | `OpEnumeratorNext`, `OpEnumeratorInByVal`, `OpEnumeratorHasOwnProperty`, `OpEnumeratorPutByVal`, `OpEnumeratorGetByVal` | `m_enumeratorMetadata` | enumerator modes | none |
| F29 | `OpToThis` | `m_toThisStatus` | `to_this` status | none |
| F30 | `OpJneqPtr` | `m_hasJumped` | branch bit | none |

The array position p of an array-profile entry is its running index over A0..A15 in table order, and its UCB copy is `unlinkedArrayProfiles()[p]` (N2, I11). `OpGetByValWithThis::Metadata::m_arrayProfile` is no family (section 1).

`jitcache/JITCacheCBFormat.h` generates A0..A14 from `FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE` and F16..F19 from `FOR_EACH_OPCODE_WITH_ARRAY_ALLOCATION_PROFILE`, so a reordered native macro moves both the native merge and this table. It `static_assert`s that the two macros expand to 15 and 4 entries and that every field named above has the expected type (`ArrayProfile`, `ArrayAllocationProfile`, `IterationModeMetadata`, `EnumeratorMetadata`, `ToThisStatus`, `bool`), so a new or renamed field fails the build. A new metadata field of one of these types under a new opcode does not fail the build; section 9.3 makes it a review item for every WebKit bump.

`JITCacheCBFormat.h` defines two walkers, each of which reads `codeBlock.metadataTable()` once and visits nothing when it is null (N1):

```cpp
namespace JSC::JITCache::CBFormat {
// Calls functor(typename Op::Metadata&) for each entry of Op, in metadata-ID order.
template<typename Op, typename Functor> void forEachFamilyEntry(CodeBlock&, const Functor&);
// Calls functor(unsigned offset, ValueProfile&) for offsets 1 to numValueProfiles, in that order.
template<typename Functor> void forEachMetadataValueProfile(CodeBlock&, const Functor&);
}
```

The lane reaches a CB's metadata only through these walkers ([history](SPEC-cb-history.md#bodies-without-a-metadata-table)): capture's counts and reads, `scoreLive`, V4, S2, `seedLinkedState` and the twin check use them, and nothing else in the lane calls `MetadataTable::forEach`, `forEachValueProfile` or `valueProfileForOffset`. The one other read is S2's F19 check, which reaches an entry through its `op_new_array_buffer` instruction's `metadata(CodeBlock*)` accessor; an instruction with metadata exists only when the table does. For a CB without a metadata table, only P2, P3 and P10 to P12 travel.

### 3.2 `cb.state`

```cpp
namespace JSC::JITCache::CBFormat {

static constexpr uint16_t stateLayoutVersion = 1;
static constexpr uint16_t summaryLayoutVersion = 1;
enum class Tier : uint8_t { Baseline = 1 };
// Whether P12 travels. NotCarried marks a body with a polymorphic site (section 5.3).
enum class CounterMode : uint8_t { NotCarried = 0, Carried = 1 };
static constexpr unsigned numberOfFamilies = 31;
static constexpr unsigned numberOfArrayProfileFamilies = 16;       // A0..A15, all paired

struct StateHeader {
    uint16_t layoutVersion;             // stateLayoutVersion
    uint8_t tier;                       // Tier::Baseline
    uint8_t counterMode;                // CounterMode
    uint32_t numArguments;              // CodeBlock::numParameters()
    uint32_t numValueProfiles;          // UnlinkedMetadataTable::numValueProfiles(), 0 without metadata
    uint32_t numLazyOperandProfiles;
    uint16_t optimizationDelayCounter;  // P10
    uint16_t reoptimizationRetryCounter;// P11
    int32_t counterValue;               // P12: ExecutionCounter::m_counter; 0 when NotCarried
    float counterTotalCount;            // P12: ExecutionCounter::m_totalCount; 0 when NotCarried
    int32_t counterActiveThreshold;     // P12: ExecutionCounter::m_activeThreshold; 0 when NotCarried
    std::array<uint32_t, numberOfFamilies> familyEntryCount;
    uint32_t reserved1;                 // 0; keeps the arrays that follow 8-byte aligned
};
static_assert(sizeof(StateHeader) == 160);

struct ArrayProfileRecord {             // P4
    uint32_t observedArrayModes;
    uint32_t flags;                     // OptionSet<ArrayProfileFlag>::toRaw()
};
static_assert(sizeof(ArrayProfileRecord) == 8);

struct LazyOperandRecord {              // P3
    uint32_t bytecodeIndexBits;         // BytecodeIndex::asBits()
    uint32_t operandKind;               // OperandKind
    int32_t operandValue;               // Operand::value()
    uint32_t reserved;                  // 0
    uint64_t prediction;                // SpeculatedType
};
static_assert(sizeof(LazyOperandRecord) == 24);

} // namespace JSC::JITCache::CBFormat
```

After the header come these arrays, back to back, each starting at the offset where the previous one ends:

| # | array | element | count |
|---|---|---|---|
| 1 | argument predictions | `uint64_t` | `numArguments`; element i is argument i |
| 2 | value predictions | `uint64_t` | `numValueProfiles`; element k-1 is the profile at offset k |
| 3 | array profiles | `ArrayProfileRecord` | sum of `familyEntryCount` over A0..A15, families in table order |
| 4 | lazy-operand profiles | `LazyOperandRecord` | `numLazyOperandProfiles`, in the holder's order (N7) |
| 5 | allocation hints | `uint16_t`, `(IndexingType << 8) \| vectorLength` | sum over F16..F19 |
| 6 | iteration modes | `uint16_t` | sum over F20..F23 |
| 7 | enumerator modes | `uint8_t` | sum over F24..F28 |
| 8 | `to_this` statuses | `uint8_t` | F29 |
| 9 | branch bits | `uint8_t` | F30 |
| 10 | padding | zero bytes | up to the next multiple of 8 |

Arrays 1 to 4 have 8-byte elements and follow a 160-byte header, so every element is naturally aligned. The section's length is exactly `stateSectionSize(header)` (I10), which `JITCacheCBFormat.cpp` computes from the header with overflow-checked arithmetic (`CheckedSize`).

### 3.3 `cb.summary`

```cpp
namespace JSC::JITCache::CBFormat {

struct SummaryHeader {
    uint16_t layoutVersion;             // summaryLayoutVersion
    uint8_t tier;                       // Tier::Baseline
    uint8_t counterMode;                // CounterMode, the same as cb.state's
    uint32_t numArguments;
    uint32_t numValueProfiles;
    uint32_t numArrayProfiles;          // all of A0..A15
    uint32_t numLazyOperandProfiles;
    uint32_t counterProgress;           // section 4.3; 0 when the counter does not travel
};
static_assert(sizeof(SummaryHeader) == 24);

} // namespace JSC::JITCache::CBFormat
```

Then: argument categories (`uint64_t` × `numArguments`, each the CB prediction ORed with UCB value profile i); value categories (`uint64_t` × `numValueProfiles`, the CB prediction at offset k ORed with UCB value profile `numArguments + k - 1`); lazy-operand categories (`uint64_t` × `numLazyOperandProfiles`, the CB prediction alone, since these profiles have no UCB copy); array flags (`uint8_t` × `numArrayProfiles`, the CB flags at array position p ORed with the flags of UCB array profile p); zero padding to a multiple of 8. A category is one bit of a `SpeculatedType` (THREAD Capture). For a builtin the UCB copies stay `SpecNone` natively (N2), so the union equals the CB copy, as the native merge leaves it.

### 3.4 Validation

Every rule in this section runs only with strict on (THREAD Session), and section 7 gives each failure's class ([history](SPEC-cb-history.md#validation-is-strict-only)). `validateState` (section 6.1) is the one implementation of V1 to V15: `CBStateImport::prepare` applies it to every `cb.state` it reads and fails with invalid material at the first violation (section 5.2), and SC1 applies it to every `cb.state` a capture builds (section 4.4). In normal mode `prepare` applies none of them and trusts the section the producer wrote for this body: its counts match the CB through the body key and the UCB lane's index-space guarantee (R-UCB-2), and its values are what SC1 confirms whenever the producer ran strict.

Each V-rule bounds a value to what native code produces at that site, or to a superset the engine handles safely; V5's length and padding are rules of the format itself. Every rule reads the span, the CB and its instructions in place, so validation allocates nothing. With the rules applied, no seed can index out of range, reach an enum case its reader has no case for (the DFG's iteration cases included), shift by more than the width, trip an assertion of the engine's own constructors, or make an allocation hint copy an array literal into a butterfly that misrepresents its elements. Validation cannot see what the consumer realm has created; the one seeded value whose native recording implies realm state, `FastMap` or `FastSet` at an `iterator_open` site, is made safe by `seedLinkedState` in both modes (section 5.2, N10).

- V1. The span holds at least a `StateHeader`; `layoutVersion` is 1, `tier` is `Baseline`, `counterMode` is `NotCarried` or `Carried`, and `reserved1` is 0.
- V2. `numArguments == codeBlock.numParameters() == codeBlock.argumentValueProfiles().size()`.
- V3. `numValueProfiles` equals the UCB's `metadata().numValueProfiles()`.
- V4. Each `familyEntryCount[f]` equals the number of entries `forEachFamilyEntry<Op>` (section 3.1) visits for the family's opcode, which is 0 when `metadataTable()` is null.
- V5. The span's length equals `stateSectionSize(header)`, and the padding bytes are zero.
- V6. Every argument and value prediction is a subset of `SpecBytecodeTop`.
- V7. Every array record has `observedArrayModes` within `ALL_ARRAY_MODES` and `flags` within the eight `ArrayProfileFlag` bits (`0xff`).
- V8. Every allocation hint has a vector length of at most `BASE_CONTIGUOUS_VECTOR_LEN_MAX` and an indexing type its site can hold ([history](SPEC-cb-history.md#the-new_array_buffer-hint-bound)):
  - In F16..F18, one of `ArrayWithUndecided`, `ArrayWithInt32`, `ArrayWithDouble`, `ArrayWithContiguous`, `ArrayWithArrayStorage` and `ArrayWithSlowPutArrayStorage`, the values linking and `ArrayAllocationProfile::updateProfile` produce there. A copy-on-write hint at such a site would give a mutable array a copy-on-write structure. A hint below the elements' type is safe there: those allocations store through `JSObject::initializeIndex`, which converts the array to the needed shape, and the DFG raises the hint by the elements' predictions and checks each element (`FixupPhase::fixupNode`, case `NewArray`).
  - In F19, one of `CopyOnWriteArrayWithInt32`, `CopyOnWriteArrayWithDouble` and `CopyOnWriteArrayWithContiguous` (`slow_path_new_array_buffer` asserts copy-on-write). In addition, every `op_new_array_buffer` instruction has an `m_metadataID` below F19's count, and the record at that index holds at least the instruction's `m_recommendedIndexingType`, in the order `CopyOnWriteArrayWithInt32 < CopyOnWriteArrayWithDouble < CopyOnWriteArrayWithContiguous`; a lower hint makes the slow path copy the literal into a butterfly that misrepresents its elements (N8).

  `forEachNewArrayBufferLiteral` checks the F19 bound in one walk over `codeBlock.instructions()`, run only when F19 has entries, reading each instruction and its record in place. Both readers of a hint reach it through the executing instruction's own entry (N8), so the walk covers every hint a reader can reach: an entry that no instruction names is never read, and one that two instructions name is checked against both. The mapping belongs to the instruction stream, whose index spaces the UCB lane guarantees (R-UCB-2), so this lane checks only the bound it relies on.
- V9. Every iteration-mode value is within its opcode's native bits, which are the bits its DFG handler has a case for or keeps (N9): `0x1fff` in F20 (`iterator_open`), `0x1ff1` in F21 (`iterator_next`), `0x6001` in F22 (`async_iterator_open`) and `0x2001` in F23 (`async_iterator_next`) ([history](SPEC-cb-history.md#iteration-mode-masks)).
- V10. Every enumerator byte is within `IndexedMode | OwnStructureMode | GenericMode | HasSeenOwnStructureModeStructureMismatch` (`JSPropertyNameEnumerator::Flag`).
- V11. Every `to_this` status is `ToThisOK`, `ToThisConflicted` or `ToThisClearedByGC`; `merge(ToThisStatus, ToThisStatus)` crashes on anything else.
- V12. Every branch bit is 0 or 1.
- V13. Every lazy-operand record has `reserved == 0`, a prediction within `SpecBytecodeTop`, a bytecode offset `BytecodeIndex::fromBits(bytecodeIndexBits).offset()` below `codeBlock.instructionsSize()`, and an operand the engine's constructors accept (N7): `operandKind` is `Argument`, `Local` or `Tmp`; for `Tmp`, `operandValue >= 0`; for `Argument` and `Local`, `VirtualRegister(operandValue).isLocal()` agrees with the kind, `VirtualRegister(operandValue).isValid()` holds and `VirtualRegister(operandValue).isConstant()` does not ([history](SPEC-cb-history.md#lazy-operand-keys)). The offset bound also rejects the hash table's empty and deleted index values (`invalidOffset` and `deletedValue()`), whose offset, 2^30 - 1, no instruction stream reaches. The checkpoint needs no rule, since `BytecodeIndex` keeps it in its two low bits (`checkpointMask`) and every value decodes to a checkpoint below `numberOfCheckpoints`. Keys may repeat and come in any order: seeding through `addOperandValueProfile` collapses a duplicate into one profile (section 5.2), and no engine reader needs unique keys (N7). No check, strict or not, ties the operand to this CB's frame, since a key can name a slot of an inlining compile's machine frame, which no check against the CB can verify and the engine only hashes and compares (N7).
- V14. `optimizationDelayCounter <= Options::maximumOptimizationDelay()` and `reoptimizationRetryCounter <= Options::reoptimizationRetryCounterMax()` (N5).
- V15. When `counterMode` is `Carried`, `counterActiveThreshold >= 0`, and `counterTotalCount` is finite and not negative; when it is `NotCarried`, `counterValue`, `counterTotalCount` and `counterActiveThreshold` are all 0.

`decodeSummary` applies, through `validateSummary` (section 6.1), V1 (with `SummaryHeader`, which has no `reserved1`), V5 (with `summarySectionSize`), V6 to each category, V7's flag bound to each array flag, `counterProgress <= INT32_MAX`, and a `counterProgress` of 0 when `counterMode` is `NotCarried`. A failure of V1 reports `SummaryHeader`, one of V5 `SummaryLength`, and any other `SummaryValue`. It compares no count with a CodeBlock, since scoring runs without one. In normal mode it only reads.

## 4. Capture

### 4.1 Hooks and context

The integrator's capture glue calls the lane at THREAD Capture's two points, the end of the success path of `BaselineJITPlan::finalize` (`jit/BaselineJITPlan.cpp`), after `installCode` and `jitSoon`, and each eligible CB of `delta`'s walk (`Heap::forEachCodeBlockIgnoringJITPlans`), passing the ICs lane's `hasPolymorphicSite` for the same CB (R-INT-5). The glue decides eligibility; with strict on, the lane rechecks it (SC2, section 4.4).

On entry the caller is the VM thread, holds the API lock and heap access, and JS is paused. At the finalize hook it runs inside door 1 (the `DeferGC` of `JITWorklist::completeAllReadyPlansForVM`, or the caller's `DeferGCForAWhile` on the synchronous routes), where nothing may start or wait for a collection or release heap access (install.md, "Conditions around installation"); in `delta` it runs with no collector phase on the thread. In both, markers and compiler threads may drain this CB's profiles concurrently (N3), which costs only precision (THREAD Capture).

The lane takes no lock, drains no profile, materializes no property table, allocates no cell and never stops for the collector; it allocates only its two section buffers, charged first (I2, I3, R-INT-2). One race is specific to P12: a DFG plan thread that finishes during a capture zeroes `m_counter` (N4), and the capture then records a crossed counter whose progress includes the rest of the slice. That too costs only precision, and the consumer's floor (section 5.3) still applies.

### 4.2 Reads

Each field is read once, with plain aligned loads, the way native readers read it. P1 is reached through `forEachMetadataValueProfile`, and P4 to P9 through `forEachFamilyEntry<Op>` for each family's opcode (section 3.1):

| piece | read |
|---|---|
| P1 | `ValueProfile::m_prediction` at each offset k = 1..N |
| P2 | `argumentValueProfiles()[i].m_prediction` |
| P3 | `lazyValueProfiles().forEachOperandValueProfile` (E3): `key()` and `m_prediction` only |
| P4 | `ArrayProfile::observedArrayModes()` and `arrayProfileFlags()` (E1) |
| P5 | `ArrayAllocationProfile::selectIndexingTypeConcurrently()` and `vectorLengthHintConcurrently()`, which read the hint and never the last array |
| P6..P9 | the metadata fields of section 3.1 |
| P10, P11 | `CodeBlock::optimizationDelayCounter()`, `CodeBlock::reoptimizationRetryCounter()` |
| P12 | `baselineJITData()->executeCounter()`: `m_counter`, `m_totalCount`, `m_activeThreshold`, read only when `hasPolymorphicSite` is false |
| UCB copies | the UCB lane's accessors (R-UCB-1), by the positions of N2 |

### 4.3 Score

```
P = double(counterTotalCount) + counterValue
counterProgress = counterMode == NotCarried || P <= 0 ? 0
                : uint32(min(floor(P), double(counterActiveThreshold)))
richnessUnits = sum of popcount64 over argument, value and lazy-operand categories
              + sum of popcount8(flags & ~DidPerformFirstRunPruning) over array flags
```

`counterProgress` is THREAD Maintenance's P, and the same value is the P the writer stamps in the envelope (R-INT-5). `richnessUnits` is this lane's share of THREAD Capture's richness, each slot taken as the union of its CB and UCB copies; the UCB lane supplies the arithmetic and exit-site units, and the integrator adds them and orders captures as R-INT-5 states. `counterWithheld` is `counterMode == NotCarried`. THREAD Capture ranks a capture whose progress the polymorphic rule withholds above one whose progress travels, just before comparing progress, so the score has to say why a `counterProgress` is 0 ([history](SPEC-cb-history.md#polymorphic-bodies-carry-no-counter-progress)).

### 4.4 Procedure

`CBStateCapture::capture(codeBlock, budget, strict, hasPolymorphicSite)`:

1. Count: `numArguments`, `numValueProfiles`, the 31 family counts (by `forEachFamilyEntry<Op>`, 0 without a metadata table) and `numLazyOperandProfiles`.
2. With strict on, check SC2 and then SC3, before any read beyond step 1's counts, so that nothing dereferences `baselineJITData()` or indexes the argument profiles or a UCB copy first:
   - SC2. The CB is eligible as the lane assumes: `jitType() == JITType::BaselineJIT`, `baselineJITData()` is not null, `isJettisoned()` and `m_didFailJITCompilation` are clear, and `argumentValueProfiles().size() == numParameters()`. On failure return `CBFault { RecordingFault, CaptureStrict }`.
   - SC3. The UCB copies pair with the CB's profiles as the native merges assume (N2): `unlinkedValueProfiles().size() == numParameters() + numValueProfiles`, and `unlinkedArrayProfiles().size()` equals the A0..A15 total. On failure return `CBFault { RecordingFault, CapturePairing }`. The UCB lane guarantees the pairing for an imported UCB (R-UCB-2). Capture reads the copies through the same hardened `operator[]` as those merges, so in normal mode a broken pairing fails as it would natively, and strict turns it into a recording fault before any read.

   In normal mode, `ASSERT` both, since the glue already decided eligibility (section 4.1).
3. Compute both section sizes; charge their sum (R-INT-2); on refusal return `CBFault { RecordingFault, CaptureCharge }` having built nothing. Then allocate the two buffers, the only memory capture allocates.
4. Fill `cb.state` from the reads of section 4.2, array 4 in the holder's order (N7), and `counterMode` with the counter fields as I16 states.
5. Fill `cb.summary` from the `cb.state` bytes, `counterMode` included, and the UCB copies read in the same pause, never from a second read of the live CB (I8), and compute the `CBScore`.
6. With strict on, check SC1:
   - SC1. The bytes just built pass `validateState` (V1 to V15) and S3 (section 5.2) against the CB they came from, and the summary bytes pass `validateSummary`, so a producer never commits material its strict consumers reject. V8's F19 bound comes from the instructions, which no execution changes, so it means the same for the live CB as for the consumer's newborn one, and its walk allocates nothing at capture either. On failure release both buffers and their charge and return `CBFault { RecordingFault, CaptureStrict }`.

The returned `CBStateCapture` owns both buffers and their charge until it dies. The glue scores it, hands the spans to the writer on acceptance and drops it otherwise.

`CBStateCapture::scoreLive(codeBlock, strict, hasPolymorphicSite)` computes the same `CBScore` with the same reads, writing nothing and charging nothing. With strict on it first checks SC2 and SC3 as step 2 does, with the same faults; in normal mode it `ASSERT`s SC2. It lets `delta` rank the CBs of one key before building any section; when the glue then calls `capture` on the winner with the same bit, the two scores agree unless a drain intervened between the calls, which costs only precision.

`decodeSummary(span, strict)` returns a saved body's `CBScore` from its `cb.summary`, validating the section first when strict is on (section 3.4). The glue calls it at the key's first scoring; what it keeps afterwards is the glue's choice, and the score alone suffices.

## 5. Restoration

### 5.1 Call order and context

The integrator's install function calls the lane only for the newborn CB that installs a pending import (THREAD Restoration), in this order, which R-INT-3 requires of the glue:

1. `CBStateImport::prepare(stateSection, codeBlock, strict)`, inside the install function's GC deferral, with every other lane's preparation and after the baked-fact comparison and the `shouldJIT` gate. It reads the section and the CB and writes nothing.
2. `seedLinkedState(codeBlock)`, after linking (`finishCreation` has run) and before the CB has a JIT type, in the same step in which the ICs lane seeds call-link history ([history](SPEC-cb-history.md#tier-up-history-before-setup-aging-after)).
3. Native setup: `CodeBlock::setupWithUnlinkedBaselineCode`, which creates the `BaselineJITData`, samples aging and arms the counter with `optimizeAfterWarmUp` (N4).
4. The ICs lane attaches the IC state.
5. `finishCounter(codeBlock)`: re-slices a carried counter and resamples aging, or leaves setup's arming of a counter that did not travel, and returns the `CBCounterRestore` it decided (section 5.3; [history](SPEC-cb-history.md#tier-up-history-before-setup-aging-after)).
6. Under `ENABLE(JITCACHE_TWINS)`, `verifyTwins(codeBlock, restore, report)` with that result (section 11.1). It must come before `installCode`, after which a concurrent marker can merge into P1 and P2 and drain P3 (N11), so the seeded values are exact only here ([history](SPEC-cb-history.md#twin-checks-run-before-installcode)).
7. `installCode`.

All steps run on the VM thread with the API lock and heap access, inside `prepareForExecutionImpl`'s `DeferGCForAWhile` (or the caller's deferral around `JIT::compileSync`) and the install function's own, so no stopped-world phase starts. The CB is unpublished until step 7 (N6), so no marker or compiler thread can read it, and the lane takes no lock and adds no fence: its seeds are ordinary stores that `installCode` publishes as it publishes what `finishCreation` wrote. The allocations are native: the first lazy-operand append runs `CompressedLazyValueProfileHolder::initializeData`, whose `storeStoreFence` orders the holder before its pointer, and the realm step of section 5.2 runs a lazy property's own initializer, which publishes the function with a write barrier on the global object.

After a baked-fact mismatch, which leaves the CB native and keeps the import (THREAD Restoration), the next newborn CB of the UCB repeats the sequence from step 1 against the same borrowed bytes, since `prepare` wrote nothing (R-INT-4).

### 5.2 `prepare` and `seedLinkedState`

With strict on, `prepare` runs `validateState` (V1 to V15) and then checks what the lane assumes of the CB and of the captured counter, in this order and before any write:

- S1. The CB is newborn: `jitType() == JITType::None` and `baselineJITData()` is null (N6).
- S2. The CB is at its link state (N1): every argument and value profile has `m_prediction == SpecNone` and empty buckets; every array profile of A0..A15 is all zero; every allocation profile in F16..F18 reads `ArrayWithUndecided`, every one in F19 reads the `m_recommendedIndexingType` of the instruction that names it (N8, through V8's walk), and every one reads vector length 0; every iteration, enumerator, `to_this` and branch field is zero; the lazy-operand holder has no operand profile; `optimizationDelayCounter()` and `reoptimizationRetryCounter()` are 0. The value-profile and family parts read through the walkers of section 3.1, so for a CB without a metadata table S2 checks the argument profiles, the lazy-operand holder and the two counts.
- S3. When `counterMode` is `Carried`, the captured counter obeys the native invariant that progress is not negative unless the threshold is infinite: `counterActiveThreshold == INT32_MAX` or `counterTotalCount + counterValue >= 0` (N4).

A failed rule or check is invalid material (section 7). In normal mode `prepare` runs none of them and cannot fail (section 3.4). Either way it returns a `CBStateImport`: the header and the offsets of the arrays inside the borrowed span, computed from the header's counts as `stateSectionSize` computes them. It allocates nothing (I3), since V8's bound and S2's F19 check walk the instructions in place.

`seedLinkedState` writes, field by field:

| piece | write |
|---|---|
| P1 | `ValueProfile::m_prediction = valuePredictions[k-1]` at each offset k |
| P2 | `argumentValueProfiles()[i].m_prediction = argumentPredictions[i]` |
| P3 | for each record, in order: `lazyValueProfiles().addOperandValueProfile(key)->m_prediction = prediction`, the append OSR exit itself uses (N7; [history](SPEC-cb-history.md#lazy-operand-keys)) |
| P4 | `ArrayProfile::restoreAccumulatedState(modes, OptionSet<ArrayProfileFlag>::fromRaw(flags))` (E1) |
| P5 | `ArrayAllocationProfile::restoreHint(bits >> 8, bits & 0xff)` (E2) |
| P6..P9 | the metadata field, assigned |
| P10, P11 | `CodeBlock::seedBaselineTierUpHistory(delay, reoptimization)` (M1) |

It walks P1 with `forEachMetadataValueProfile` and each family with `forEachFamilyEntry<Op>` (section 3.1), in step with the records, and writes no bucket, no sample and no field outside the lane (I4, section 6.4).

It then runs the realm step ([history](SPEC-cb-history.md#the-realm-step)). If any F20 (`iterator_open`) record holds `FastMap`, it calls `codeBlock.globalObject()->mapProtoEntriesFunction()`, and if any holds `FastSet`, `setProtoValuesFunction()`. Native code records these bits only after the realm has materialized that function, and the DFG's `iterator_open` handler asserts it (N10), while a consumer realm may not have created a `Map` or `Set` yet when the body's first DFG compile parses it, which the counter floor can bring as early as the second call. The getters are the native lazy initializers: the first call in a realm allocates the `JSFunction`, the first in the VM also the `NativeExecutable` that `JITThunks::hostFunctionStub` caches, and later calls allocate nothing. They generate no code: `thunkGeneratorForIntrinsic` has no generator for these two intrinsics, and the native-call thunk is one of the VM's eager thunks (`JSC_FOR_EACH_VM_DEPENDENT_EAGER_COMMON_THUNK`). JS cannot reach the function until `MapPrototype::finishCreation` or `SetPrototype::finishCreation` installs that same function, so the heap JS can reach matches a JITCache-off run. These are the lane's only cell allocations (I3). They run on the VM thread under the install deferral, where an allocation cannot start a collection, and only for a seed that carries the bit, which keeps them within THREAD's "no work ahead of demand". The DFG reads the CB's own realm for these cases (`globalObjectFor` on the inline frame's CB), so one realm suffices.

### 5.3 `finishCounter`

THREAD Restoration sets the floor, at least two entry increments short of crossing, and gives a body with a polymorphic site no baseline counter progress; `counterMode` records the ICs lane's bit for that (section 4.1). For a carried counter the lane restores the captured triple, lets the engine's own check re-slice it with the consumer's memory multiplier and slice ceiling, and then applies the floor ([history](SPEC-cb-history.md#the-counter-travels-raw)). For a counter that does not travel it writes nothing: the CB keeps the counter native setup armed with `optimizeAfterWarmUp` from the seeded reoptimization count and the UCB's quick DFG bit (N4) ([history](SPEC-cb-history.md#polymorphic-bodies-carry-no-counter-progress)). Such a body still carries P10, P11 and every profile, and its `counterProgress`, the envelope's P (R-INT-5), is zero. The floor's residue in bodies that carry progress is the bench's to measure (section 11.4).

```
counter = codeBlock.baselineJITData()->executeCounter()            // setup always creates it (RELEASE_ASSERT)
if counterMode == NotCarried:
    armed = -int64(counter.m_counter)                              // the slice setup armed
    return CBCounterRestore { carried = false, nativeSlice = armed, slice = armed }
P = double(counterTotalCount) + counterValue                       // as count() computes it; exact for native pairs
T = counterActiveThreshold
floor = 2 * Options::executionCounterIncrementForEntry()           // THREAD: two entry increments
counter.m_activeThreshold = T
counter.m_totalCount = counterTotalCount
counter.m_counter = counterValue                                   // now counter.count() == P
r = CBCounterRestore { carried = true }
if T == INT32_MAX:
    r.nativeSlice = counterValue < 0 ? -int64(counterValue) : 0    // the captured slice
else if counter.checkIfThresholdCrossedAndSet(&codeBlock):
    r.crossed = true                                               // the consumer's pool says the threshold is reached
    r.nativeSlice = 0
else:
    r.nativeSlice = -int64(counter.m_counter)                      // the slice ExecutionCounter::setThreshold armed from P
r.slice = max(r.nativeSlice, floor)                                // 64-bit; -r.slice never falls below INT32_MIN
counter.m_counter = int32(-r.slice)
counter.m_totalCount = float(P + r.slice)
codeBlock.snapshotExecutionCounterForAging(counter.count())
return r
```

For a finite threshold, `ExecutionCounter::checkIfThresholdCrossedAndSet` applies `applyMemoryUsageHeuristics` and `clippedThreshold` to the consumer's pool and this CB's size, as at any native check. An infinite threshold is kept out of it, because `setThreshold` would call `deferIndefinitely` and drop the progress, which THREAD requires to equal its capture record; its captured slice has no pool-dependent part and keeps the ceiling the producer's exit clipped it with (N4), which decides only when the consumer's next check runs.

The `snapshotExecutionCounterForAging` call resamples aging: setup sampled `m_previousCounter` from the fresh counter, and without the resample the restored progress would look like activity at the next old-age check (`CodeBlock::shouldJettisonDueToOldAge`) and renew a lease the CB never earned. A counter that does not travel keeps setup's sample.

`finishCounter` returns `r` because nothing can recompute the native answer later: the multiplier reads a pool count that worker threads and other VMs change at any moment (N4), so a second call of the same check could arm a different slice or flip the crossing. The glue passes `r` to `verifyTwins` (R-INT-6) and to the bench report (R-INT-11).

## 6. Interface

### 6.1 What the lane exports

`jitcache/JITCacheCBState.h`, under `ENABLE(JIT)`:

```cpp
namespace JSC::JITCache {

class ProducerBudget;                 // integrator, R-INT-2
#if ENABLE(JITCACHE_TWINS)
class TwinReport;                     // integrator, R-INT-6
#endif

enum class CBFaultKind : uint8_t { InvalidMaterial, RecordingFault };

enum class CBCheck : uint8_t {
    StateHeader, ArgumentCount, ValueProfileCount, FamilyCount, StateLength,          // V1..V5
    ValuePrediction, ArrayProfile, AllocationHint, IterationModes, EnumeratorModes,   // V6..V10
    ToThisStatus, BranchBit, LazyOperand, TierUpHistory, Counter,                     // V11..V15
    SummaryHeader, SummaryLength, SummaryValue,                                       // decodeSummary
    StrictNewborn, StrictLinkState, StrictCounter,                                    // S1..S3
    CapturePairing, CaptureCharge, CaptureStrict,                                     // section 4.4
};

struct CBFault {
    CBFaultKind kind;
    CBCheck check;
};

ASCIILiteral description(CBCheck);    // names the failing step for status(vm)

// The structural rules of section 3.4, shared by SC1 and prepare. Each reads the span (and the CB)
// in place, allocates nothing and returns the first failing check, or nothing.
std::optional<CBCheck> validateState(std::span<const uint8_t> stateSection, CodeBlock&);   // V1..V15
std::optional<CBCheck> validateSummary(std::span<const uint8_t> summarySection);           // decodeSummary's rules
bool counterObeysNativeInvariant(const CBFormat::StateHeader&);                            // S3

struct CBScore {
    uint64_t richnessUnits { 0 };
    bool counterWithheld { false };     // CounterMode::NotCarried (section 4.3)
    uint32_t counterProgress { 0 };
};

// What finishCounter decided (section 5.3).
struct CBCounterRestore {
    bool carried { false };     // the section carried the counter (CounterMode::Carried)
    bool crossed { false };     // carried, finite T: checkIfThresholdCrossedAndSet found the consumer's threshold reached
    int64_t nativeSlice { 0 };  // carried, finite T: 0 when crossed, else the slice native code armed;
                                // carried, T == INT32_MAX: the captured slice; not carried: the slice setup armed
    int64_t slice { 0 };        // the slice the counter holds: max(nativeSlice, floor) when carried, else nativeSlice
};

class CBStateCapture {
    WTF_MAKE_NONCOPYABLE(CBStateCapture);
    WTF_MAKE_TZONE_ALLOCATED(CBStateCapture);
public:
    // VM thread, JS paused, API lock and heap access, no collector phase (section 4.1).
    // hasPolymorphicSite comes from the ICs lane for the same CB, in the same pause (R-INT-5).
    static Expected<CBStateCapture, CBFault> capture(CodeBlock&, ProducerBudget&, bool strict, bool hasPolymorphicSite);
    static Expected<CBScore, CBFault> scoreLive(CodeBlock&, bool strict, bool hasPolymorphicSite);

    CBStateCapture(CBStateCapture&&);
    ~CBStateCapture();                // frees both buffers and releases their charge

    const CBScore& score() const;
    std::span<const uint8_t> stateSection() const;
    std::span<const uint8_t> summarySection() const;

private:
    ProducerBudget* m_budget;
    UniqueArray<uint64_t> m_state;    // 8-byte aligned
    UniqueArray<uint64_t> m_summary;
    size_t m_stateSize;
    size_t m_summarySize;
    CBScore m_score;
};

// Any thread that owns the material; reads only the span, validating it first with strict on.
Expected<CBScore, CBFault> decodeSummary(std::span<const uint8_t> summarySection, bool strict);

class CBStateImport {
public:
    // Section 5.1, step 1. Reads the span and the CB; writes neither.
    static Expected<CBStateImport, CBFault> prepare(std::span<const uint8_t> stateSection, CodeBlock&, bool strict);

    void seedLinkedState(CodeBlock&) const;            // step 2; infallible
    CBCounterRestore finishCounter(CodeBlock&) const;  // step 5; infallible

#if ENABLE(JITCACHE_TWINS)
    // Step 6: after finishCounter, with its result, before installCode (section 5.1).
    void verifyTwins(CodeBlock&, const CBCounterRestore&, TwinReport&) const;
#endif

private:
    std::span<const uint8_t> m_bytes;         // borrowed, R-INT-1
    const CBFormat::StateHeader* m_header;
    std::array<uint32_t, 9> m_arrayOffsets;   // arrays 1..9 of section 3.2
};

} // namespace JSC::JITCache
```

`jitcache/JITCacheCBFormat.h` exports the structs and the `CounterMode` enum of sections 3.2 and 3.3, the family table of section 3.1, `stateSectionSize(const StateHeader&)` and `summarySectionSize(const SummaryHeader&)` (both `std::optional<size_t>`, empty on overflow), and the domain predicates V6 to V12 as `constexpr` functions, which `validateState` uses. Among them are `nativeIterationModes(family)`, the four masks of V9 written as ORs of `IterationMode` values, and `allocationHintFits(family, IndexingType hint, IndexingType recommended)`, V8's type rule. Three more helpers serve both sides and allocate nothing: the guarded walkers `forEachFamilyEntry` and `forEachMetadataValueProfile` of section 3.1, and `template<typename Functor> void forEachNewArrayBufferLiteral(CodeBlock&, const Functor&)`, which reads only the instructions, calls `functor(metadataID, recommendedIndexingType)` for each `op_new_array_buffer` in stream order and serves V8's bound and S2. Section 12 assigns each definition to its `.cpp` file.

### 6.2 Requirements on the UCB lane

- R-UCB-1. Non-draining, `const` reads of the UCB copies, callable on the VM thread during a capture: the prediction of each `UnlinkedValueProfile` and the flags of each `UnlinkedArrayProfile`, by their index in `unlinkedValueProfiles()` and `unlinkedArrayProfiles()`. The lane codes against `SpeculatedType UnlinkedValueProfile::prediction() const` and `OptionSet<ArrayProfileFlag> UnlinkedArrayProfile::arrayProfileFlags() const`, which the UCB lane adds to the classes it owns in `bytecode/ValueProfile.h` and `bytecode/ArrayProfile.h` (SPEC-ucb.md section 5.4).
- R-UCB-2. THREAD's index-space guarantee for every imported or reused UCB: the producer's parameter count, value-profile count, metadata entry counts and instruction stream. With strict on, the lane checks the parts it depends on (V2 to V4, V8's instruction walk, V13) and treats a mismatch as invalid material; in normal mode it relies on the guarantee (section 3.4).

### 6.3 Requirements on the integrator

- R-INT-1. Each lane section reaches the lane as a span that starts 8-byte aligned and covers exactly the section, and the payload behind a `cb.state` span stays borrowed from `prepare` until `finishCounter` (and `verifyTwins`) return.
- R-INT-2. A producer budget usable on the VM thread: `[[nodiscard]] bool ProducerBudget::tryCharge(size_t)` and `void ProducerBudget::release(size_t)`, with THREAD's semantics for a charge past the limit (SPEC-integrator.md section 4.4).
- R-INT-3. The install order of section 5.1, with `prepare` inside the install function's GC deferral and before any lane writes the CB, and `seedLinkedState` on the VM thread inside that deferral, since its realm step may allocate a cell (section 5.2).
- R-INT-4. `seedLinkedState` and `finishCounter` run at most once per CB, only after `prepare` succeeded for that same CB; an import kept after a baked-fact mismatch is prepared again from its bytes for the next newborn CB.
- R-INT-5. Capture glue: for each CB it selects, the ICs lane's `hasPolymorphicSite` for that CB, obtained in the same pause before this lane is called (THREAD Restoration), passed to `capture` (or to `scoreLive`, then to `capture` for the winner, with the same bit); richness is this lane's `richnessUnits` plus the UCB lane's units; after the IC-site count, `counterWithheld` breaks a tie, ranking a withheld counter above one that travels, and then `counterProgress` (THREAD Capture), which is also the P stamped in the envelope, zero for a body whose counter does not travel; `cb.state` and `cb.summary` of one capture are always committed together.
- R-INT-6. Under `ENABLE(JITCACHE_TWINS)`: a `TwinReport` sink; for every installed import, a call to `verifyTwins` with the `CBCounterRestore` that `finishCounter` returned, at step 6 of section 5.1, between `finishCounter` and `installCode`; and a runner that executes a JS file as producer and then as consumer in a fresh process, with strict on in both (THREAD Verification), and fails on any difference a twin check reports (section 11). A check that skips itself reports the skip, and THREAD Verification's skip rule decides whether it fails the run.
- R-INT-7. The manifest entries of section 9.2, applied before the tasks of section 12 that depend on them.
- R-INT-8. Fault plumbing: `CBFaultKind` maps to THREAD's invalid material or recording fault, and `description(check)` names the failing step in `status`.
- R-INT-9. The option rows of options.md that name this lane, in `start`'s table of fixed options.
- R-INT-10. A C++ unit-test target that links JavaScriptCore and compiles `jitcache/tests/*.cpp` (M3), for tests that need no producer run. It builds with `ENABLE(JITCACHE_TWINS)`, the test builds THREAD's Execution names, because U4 reads `previousCounterForAging` (M1).
- R-INT-11. Bench reports for THREAD's bench loop, taken by the glue around its own calls: per installed import, the wall time of `prepare`, `seedLinkedState` and `finishCounter`, as the `install` event reads each step (SPEC-integrator.harness.md section 9.2), and the `CBCounterRestore` that `finishCounter` returned; per capture, the VM-thread CPU time of `capture` and, in `delta`, of `scoreLive`, and the sizes of `stateSection()` and `summarySection()`. The integrator defines the output and when it is on (section 11.4).
- R-INT-12. The integrator's `JSC::JITCache::didFailExecutableAllocation` call in `DFG::Plan::finalize` (THREAD Execution) comes before `DFG::Plan::finalize` calls `m_callback->compilationDidComplete` ([history](SPEC-cb-history.md#the-dfg-plan-site-fault-comes-first)). `JITToDFGDeferredCompilationCallback::compilationDidComplete` then calls `setOptimizationThresholdBasedOnCompilationResult(CompilationFailed)` on the baseline alternative, which defers P12 through `CodeBlock::dontOptimizeAnytimeSoon`, and that baseline CB stays its executable's `replacement()`, so without the fault first a later capture would carry the deferral and every consumer that imported it would keep the body out of the DFG.

### 6.4 Shared metadata entries

Nine opcodes have metadata entries that hold this lane's fields beside fields it never writes:

- `OpCall`, `OpCallIgnoreResult`, `OpTailCall`, `OpIteratorOpen`, `OpIteratorNext`, `OpAsyncIteratorOpen` and `OpAsyncIteratorNext` hold a `DataOnlyCallLinkInfo`, which the ICs lane seeds.
- `OpGetLength` (beside A0), `OpIteratorOpen` and `OpAsyncIteratorOpen` hold the LLInt cache `m_modeMetadata`, and `OpIteratorNext` holds two, `m_doneModeMetadata` and `m_valueModeMetadata`. Each is a `GetByIdModeMetadata` that stays native.
- `OpToThis` holds `m_cachedStructureID` beside F29, a cache that holds a cell and stays native under THREAD's omission of those caches.

Metadata seeding is field by field (THREAD Restoration): this lane writes only the fields of section 3.1 (I4), which meets SPEC-ics.md R-CB-1, and asks the same of the ICs lane, and the two seeds have no order between them.

## 7. Failures

| step | failure | THREAD class | effect |
|---|---|---|---|
| `capture`, `scoreLive` | SC2 or SC3 (strict) | recording fault | production ends in the VM; nothing was built |
| `capture` | charge refused | recording fault | as THREAD states for a charge past the limit |
| `capture` | SC1 (strict) | recording fault | buffers and charge released; nothing reaches the writer |
| `decodeSummary` | any check of section 3.4 (strict) | invalid material | cache activity off; the saved body is not replaced |
| `prepare` | V1 to V15 (strict) | invalid material | cache activity off, preparation abandoned; the CB, untouched, continues natively; the pending import dies with its UCB |
| `prepare` | S1 to S3 (strict) | invalid material | as above |
| `seedLinkedState`, `finishCounter` | none | | the lazy-operand holder's allocation and the realm step's crash on exhaustion, as the native append and the native lazy initializers do |

In normal mode no step of this lane fails on the material it reads, since THREAD Session trusts everything past the integrity checks, which the integrator and the UCB lane make before this lane sees a section.

The lane allocates no executable memory, so THREAD's lack-of-memory fault never starts here; a DFG plan that fails for lack of executable memory does write P12, after the integrator's fault (R-INT-12). A commit that fails after a capture changes nothing the lane owns: the live CB keeps its state, and the `CBStateCapture` dies with its charge.

## 8. Invariants

Each is testable; section 11 names the test.

- I1. The lane's sections hold no address, cell pointer, `StructureID` or time. Every field is a count V2 to V5 match against the CB, a bit set or enumerator within the domains V6 to V12 bound, a bytecode index and operand (V13), a tier-up count (V14), or the raw counter triple or the zeros that stand for it when the counter does not travel (V15). The triple is the one place the producer's process reaches the bytes: its split between `counterValue` and `counterTotalCount` holds the producer's memory-pressure slice, and P carries the fractions native re-slicing added (N4). The consumer reads the triple only as P, T and, under an infinite threshold, the remaining slice, which no pool statistic shapes, and recomputes every finite slice (section 5.3), so the producer's VM statistics stay local (THREAD Restoration). Captures are not byte-reproducible across processes, since the split and P's fractions differ with the pool, and predictions with the timing of GC-driven drains (THREAD Capture); no test assumes they are.
- I2. Capture never reads, classifies or clears a pending sample, and calls no draining function: no `computeUpdatedPrediction`, `updateAll*Predictions`, `ArrayAllocationProfile::updateProfile`, `selectIndexingType`, `vectorLengthHint` or `LazyOperandValueProfileParser::prediction`.
- I3. Capture and restoration take no lock, never stop for the collector and never release heap access. Capture allocates no cell and no memory beyond its two section buffers, which it charges before allocating; `prepare` allocates nothing. Restoration allocates cells only in the realm step (section 5.2): per realm at most the `JSFunction` of `mapProtoEntriesFunction` and of `setProtoValuesFunction`, plus each one's `NativeExecutable` the first time in the VM, and no executable memory. Both run inside an `AssertNoGC` scope (`heap/DeferGC.h`). In debug builds that scope fails any allocation or collection check made outside a GC deferral (`tryAllocateCellHelper`, `Heap::collectIfNecessaryOrDefer`), which covers capture in a `delta` that holds no deferral; under the deferrals of the finalize hook and of installation it checks nothing, so U8 runs both outside any deferral.
- I4. In the CB, restoration writes only P1 to P12 and `m_previousCounter`: no bucket, no sample, no `DataOnlyCallLinkInfo`, no LLInt cache, no cell cache and no scope metadata. Outside the CB it writes only the two lazy functions of the realm step.
- I5. `prepare` writes nothing; after any failure the CB is exactly at its link state.
- I6. `seedLinkedState` runs once, on a CB whose `jitType()` is `None` and that never ran; `finishCounter` runs once, after setup.
- I7. When the section carries the counter, after `finishCounter` returns r: `r.carried`; `m_activeThreshold == T`; `r.slice == max(r.nativeSlice, floor)` with floor `2 * executionCounterIncrementForEntry`, and `m_counter == int32(-r.slice)`, so `m_counter <= -floor`; `count() == double(float(P + r.slice)) - r.slice`, which equals P whenever P + r.slice is exactly a float, as it is when P is integral and |P + r.slice| < 2^24, and otherwise differs from P by the one float rounding a native re-slice also applies; `previousCounterForAging() == float(count())`, the comparison the old-age check makes (N4); for T == `INT32_MAX`, `r.crossed` is false and `r.nativeSlice` is the captured slice; and for a finite T, `r.crossed` and `r.nativeSlice` are what `ExecutionCounter::checkIfThresholdCrossedAndSet` decided at that call for the restored P and T against the consumer's pool, so the result depends only on P, T and that pool, whatever split the producer stored. Since M is at least 1 (N4), a finite-threshold r also lies in an envelope no pool can move: `r.crossed` implies `r.nativeSlice == 0`, otherwise `0 <= r.nativeSlice <= C`, and when `P < T - min(T, C) / 2`, `r.crossed` is false and `r.nativeSlice >= trunc(min(T - P, C))`. When the section does not carry the counter, `finishCounter` writes nothing and returns r with `r.carried` and `r.crossed` false and `r.slice == r.nativeSlice == -m_counter`, and the counter is setup's arming (N4): `m_activeThreshold == adjustedCounterValue(t)` for N4's t, read from the seeded reoptimization count and the UCB's quick DFG bit; `0 <= count() <= 1`, or, when that threshold is `INT32_MAX`, the indefinite deferral (`m_counter == INT32_MIN` with a zero total); and `previousCounterForAging() == 0`, setup's sample of the fresh counter.
- I8. Every summary slot equals the matching `cb.state` slot ORed with the UCB copy read in the same pause (lazy-operand slots have no UCB copy); `richnessUnits`, `counterWithheld` and `counterProgress` are functions of the summary bytes alone.
- I9. From `seedLinkedState` until `installCode` publishes the CB, every seeded field equals its `cb.state` record, and from `finishCounter` on, the counter satisfies I7. After publication, native drains and merges change these fields as in any CB (N11).
- I10. Each section's length equals its computed size, and its padding is zero.
- I11. Pairing: argument i with UCB value profile i; value offset k with UCB value profile `numArguments + k - 1`; array position p with UCB array profile p.
- I12. Families A0..A15 follow the native merge order (`FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE`, then `OpIteratorNext`).
- I13. Every `cb.state` a producer commits passes V1 to V15 and S3 for the CB it came from. Strict guarantees it per capture (SC1), and the tests, which all run with strict on, check it always.
- I14. After `seedLinkedState`, if an F20 record holds `FastMap` (`FastSet`), `codeBlock.globalObject()->mapProtoEntriesFunctionConcurrently()` (`setProtoValuesFunctionConcurrently()`) is not null, as native recording guarantees (N10). A section without these bits materializes nothing.
- I15. The lane reaches a CB's metadata only as section 3.1 allows. For a CB whose `metadataTable()` is null, capture writes `numValueProfiles` 0 and every family count 0, and capture, `scoreLive`, `prepare`, `seedLinkedState` and the twin check read and write none of its metadata.
- I16. A capture called with `hasPolymorphicSite` true writes `counterMode` `NotCarried` in both sections, zeros in the three counter fields and a `counterProgress` of 0, and reads no counter field; called with it false, it writes `Carried` in both and the triple it read.

## 9. Native edits and owned paths

### 9.1 Edits this lane owns

Each adds functions, changes no existing function and is edited by no other part, and each is called only from the contexts of sections 4.1 and 5.1. The realm step, V8's walk and the guarded walkers of section 3.1 use existing public API only (`JSGlobalObject::mapProtoEntriesFunction` and `setProtoValuesFunction`, `CodeBlock::instructions`, the generated `OpNewArrayBuffer` decoder, `CodeBlock::metadataTable`, `MetadataTable::forEach` and `valueProfileForOffset`) and need no edit.

E1. `bytecode/ArrayProfile.h`, class `ArrayProfile` only (class `UnlinkedArrayProfile` in the same header belongs to the UCB lane):

```cpp
OptionSet<ArrayProfileFlag> arrayProfileFlags() const { return m_arrayProfileFlags; }

// JITCache: restores the accumulated modes and flags of a profile at its link state.
// The two StructureID samples stay empty.
void restoreAccumulatedState(ArrayModes observedArrayModes, OptionSet<ArrayProfileFlag> flags)
{
    ASSERT(!m_lastSeenStructureID && !m_speculationFailureStructureID);
    m_observedArrayModes = observedArrayModes;
    m_arrayProfileFlags = flags;
}
```

E2. `bytecode/ArrayAllocationProfile.h`, class `ArrayAllocationProfile`:

```cpp
// JITCache: restores the hint of a profile that has no last array yet.
void restoreHint(IndexingType indexingType, unsigned vectorLength)
{
    ASSERT(!isCompilationThread());
    ASSERT(!m_storage.pointer());
    m_storage.setType(IndexingTypeAndVectorLength(indexingType, vectorLength));
}
```

E3. `bytecode/LazyValueProfile.h`, class `CompressedLazyValueProfileHolder`:

```cpp
// Visits each operand profile. The functor reads key() and m_prediction only, never the buckets.
template<typename Functor> void forEachOperandValueProfile(const Functor& functor)
{
    if (!m_data)
        return;
    for (auto& profile : m_data->operandValueProfiles)
        functor(std::as_const(profile));
}
```

Restoration appends through the public `addOperandValueProfile` (section 5.2, P3), so E3 adds only this walker: nothing public iterates the holder. That append's linear search makes restoring n lazy-operand profiles quadratic in n, where n is the number of distinct exit keys of one CB, which native OSR exits already search the same way as they append; a restore-only append would be an engine edit with nothing to measure against ([history](SPEC-cb-history.md#lazy-operand-keys)).

### 9.2 Manifest for `INTEGRATE-cb.md`

The integrator applies these; the lane's implementers do not edit the files.

- M1. `bytecode/CodeBlock.h`, class `CodeBlock`, two additions. In the public section under `ENABLE(JIT)`, next to `optimizationDelayCounter()`:

  ```cpp
  // JITCache: seeds the baseline tier-up history of a CodeBlock that has no JIT type yet.
  void seedBaselineTierUpHistory(uint16_t optimizationDelayCounter, uint16_t reoptimizationRetryCounter)
  {
      ASSERT(jitType() == JITType::None);
      ASSERT(optimizationDelayCounter <= Options::maximumOptimizationDelay());
      ASSERT(reoptimizationRetryCounter <= Options::reoptimizationRetryCounterMax());
      m_optimizationDelayCounter = optimizationDelayCounter;
      m_reoptimizationRetryCounter = reoptimizationRetryCounter;
  }
  ```

  Right after `snapshotExecutionCounterForAging`, in the public section that declares it, the read side the twin check of P12, I7 and U4 need, since `m_previousCounter` is private and has no reader (N4):

  ```cpp
  #if ENABLE(JITCACHE_TWINS)
      // JITCache twins: the aging sample snapshotExecutionCounterForAging() wrote last.
      float previousCounterForAging() const { return m_previousCounter; }
  #endif
  ```
- M2. `Sources.txt`: `jitcache/JITCacheCBFormat.cpp`, `jitcache/JITCacheCBValidate.cpp`, `jitcache/JITCacheCBCapture.cpp`, `jitcache/JITCacheCBSummary.cpp`, `jitcache/JITCacheCBImport.cpp`, `jitcache/JITCacheCBTwins.cpp`.
- M3. `CMakeLists.txt`: `${JAVASCRIPTCORE_DIR}/jitcache` in the JavaScriptCore private include directories, shared by every lane and listed once, and the unit-test target of R-INT-10.

No Bun-side edit: Bun touches CB state only through native APIs, namely the `bun:jsc` test helpers `optimizeNextInvocation`, which re-arms the baseline counter, and `reoptimizationRetryCount`, which reads P11 (`~/bun/src/jsc/modules/BunJSCModule.h`), and the exported `JSC__JSFunction__optimizeSoon` (`~/bun/src/jsc/bindings/bindings.cpp`), which calls `JSC::optimizeNextInvocation` and has no caller in Bun.

### 9.3 WebKit bump review

On every WebKit bump, the reviewer compares the metadata fields of `bytecode/BytecodeList.rb` whose types are `ArrayProfile`, `ArrayAllocationProfile`, `IterationModeMetadata`, `EnumeratorMetadata`, `ToThisStatus` or a profiling `bool` with the family table of section 3.1, and the `CodeBlock` fields with the tables of section 1. The build catches reordered macros and renamed fields (section 3.1), not a new field under a new opcode. The reviewer also rechecks the facts that rules depend on and no build can catch: that `OpGetByValWithThis::Metadata::m_arrayProfile` still has no reader (N2), the per-opcode iteration bits of V9 against the native writers and the DFG handlers (N9), the realm reads behind the realm step (N10), and the facts behind V8's bound and S2's F19 check, among them the one-to-one mapping of `op_new_array_buffer` instructions to metadata entries (N8).

### 9.4 Owned paths

New files: `Source/JavaScriptCore/jitcache/JITCacheCBFormat.h`, `JITCacheCBFormat.cpp`, `JITCacheCBState.h`, `JITCacheCBValidate.cpp`, `JITCacheCBCapture.cpp`, `JITCacheCBSummary.cpp`, `JITCacheCBImport.cpp`, `JITCacheCBTwins.cpp` (all of it under `ENABLE(JITCACHE_TWINS)`); the unit tests `Source/JavaScriptCore/jitcache/tests/JITCacheCBAccessorTests.cpp`, `JITCacheCBFormatTests.cpp`, `JITCacheCBCaptureTests.cpp`, `JITCacheCBSummaryTests.cpp`, `JITCacheCBImportTests.cpp` and `JITCacheCBTwinsTests.cpp`, one per task (section 12); and the corpus directory `JSTests/jitcache/cb/`. Added native functions: `ArrayProfile::arrayProfileFlags`, `ArrayProfile::restoreAccumulatedState`, `ArrayAllocationProfile::restoreHint`, `CompressedLazyValueProfileHolder::forEachOperandValueProfile`. Through the manifest: `CodeBlock::seedBaselineTierUpHistory` and, in builds with `ENABLE(JITCACHE_TWINS)`, `CodeBlock::previousCounterForAging`.

File-local names in these files follow SPEC-integrator.md R-ALL-8 with the prefix `cb`.

## 10. Options

[options.md](../options.md) holds this lane's option rows for `start`'s table, the fixed options whose meaning the transported counters and profiles rest on, and the free options that shape only the consumer's own slice and policy.

## 11. Tests and bench

Every piece of this lane except the slice is state the engine cannot recompute, so its twin is its capture record (THREAD Verification). The slice is recomputed by native code in the consumer (section 5.3), and its twin is the answer native code gave `finishCounter`, which `finishCounter` returns because no later call can reproduce it (N4); a counter that does not travel has setup's native arming as its twin (I7). Every test runs with strict on (THREAD Verification), on `debug-local` with ASan and LSan, and every producer and consumer pair runs in separate processes through the integrator's runner (R-INT-6), which starts every jsc run with `--destroy-vm` (SPEC-integrator.harness.md section 7.3). This lane's sections hold no address (I1), so its twins need nothing from the process layout and hold whatever THREAD's relocation requirement skips in a run. In twins mode the runner turns `useConcurrentJIT` off for `cb/` unless a run's options set it (SPEC-integrator.harness.md section 7.4); a subject that needs it off in plain mode too sets it in its own `jitcache-runs` line.

### 11.1 Twins

`verifyTwins` (`JITCacheCBTwins.cpp`) runs for every installed import at step 6 of section 5.1, while no marker can merge into the CB (N11), reports to the integrator's `TwinReport`, and reads the CB's metadata only through the walkers of section 3.1 (I15):

- P1 to P11: each seeded field equals its `cb.state` record (I9); every bucket and sample is still empty (I4).
- P12: the counter, the aging sample (`previousCounterForAging()`, M1) and the `CBCounterRestore` obey I7: its envelope for a carried counter, setup's arming for one that did not travel ([history](SPEC-cb-history.md#the-counter-travels-raw)). The check calls no pool-dependent code: it compares the counter with the record, the record with an envelope whose bounds come from P, T and C alone, and a native arming's threshold with `adjustedCounterValue`, which reads the CB's size and reoptimization count and no pool statistic, so concurrent compiles and other VMs cannot make it disagree with `finishCounter` (N4).
- Realm step: I14 holds for the CB's realm.
- In the shared metadata entries of section 6.4, the fields that stay native still hold the values linking gave them, so the lane wrote nothing beside its own fields (I4). Each LLInt cache is compared field by field, because `GetByIdModeMetadata` is a union with no `operator==` whose constructor leaves `padding4` unwritten (`bytecode/GetByIdMetadata.h`): `mode == GetByIdMode::Default`, `hitCountForLLIntCaching == Options::prototypeHitCountForLLIntCaching()`, a null `defaultMode.structureID` and `defaultMode.cachedOffset == 0`. `OpToThis`'s `m_cachedStructureID` is null. The `DataOnlyCallLinkInfo` fields are the ICs lane's seeds and stay outside this check.

### 11.2 Unit tests (`jitcache/tests/`, one file per task)

The target builds with `ENABLE(JITCACHE_TWINS)` (R-INT-10); section 12 assigns each test to its task and file.

Live tests get their global object and functions as SPEC-ics.md's live tests do, and build their CBs as follows. A newborn CB comes from `ScriptExecutable::newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope())`, called inside a `DeferGCForAWhile` on a function never called, because `newCodeBlockFor` release-asserts that the executable has no CB of that kind; the test keeps it in a local and never installs it. A baseline CB comes from one `JSC::call`, then `JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed)`. Two functions with the same body text have separate UCBs with the same metadata layout, so a section captured from one's baseline CB pairs with the other's newborn CB. A CB after setup is such a newborn CB taken through `prepare`, `seedLinkedState`, `CodeBlock::setupWithUnlinkedBaselineCode` with the first function's `BaselineJITCode`, and `finishCounter`, from a section that carries the record the test names.

- U1. Layout: `sizeof` and offsets of the structs of sections 3.2 and 3.3; `stateSectionSize` and `summarySectionSize` on edge counts, including overflow (I10).
- U2. Validation, with strict on: starting from a valid section captured from a real CB, every field mutated to just inside and just outside its V-rule yields acceptance or exactly the expected `CBCheck` from both `prepare` and `validateState`, never a crash, and leaves the CB at its link state (I5). A `counterMode` of 2 fails with `StateHeader`, and a `NotCarried` header with any nonzero counter field fails with `Counter`. For V13 the mutations are: a tmp at 0 (accepted) and at -1; an `Argument` kind with a negative value and a `Local` kind with a non-negative one; an `Argument` at `VirtualRegister::invalidVirtualRegister` and at `FirstConstantRegisterIndex`; an `operandKind` of 3; a bytecode offset at `instructionsSize() - 1` (accepted) and at `instructionsSize()`; `bytecodeIndexBits` equal to `UINT32_MAX` and to `UINT32_MAX - 1`, the empty and deleted index values; a checkpoint of 3 at a valid offset (accepted); a nonzero `reserved`; a prediction outside `SpecBytecodeTop`; a duplicated key and two adjacent records in decreasing key order, both accepted. A local past `numCalleeLocals()` and a tmp at `maxNumCheckpointTmps` and far above it are accepted (N7). For V8, on a CB with three literals of each type: `CopyOnWriteArrayWithInt32` at a Double or Contiguous literal and `CopyOnWriteArrayWithDouble` at a Contiguous literal fail with `AllocationHint`; `CopyOnWriteArrayWithDouble` and `CopyOnWriteArrayWithContiguous` at an Int32 literal, and `CopyOnWriteArrayWithContiguous` at a Double literal, are accepted; `ArrayWithContiguous` in F19 and `CopyOnWriteArrayWithInt32` in F16 fail. For V9: each opcode's full native mask is accepted; `FastAsyncGenerator` or `AsyncFromSync` in F20, `FastArray`, `FastMap` or `FastSet` in F21, `FastArray` in F22 and `AsyncFromSync` in F23 fail with `IterationModes`.
- U3. Strict: `prepare` on a CB that already ran fails with `StrictNewborn` or `StrictLinkState`, so `seedLinkedState`, which the glue calls only after `prepare` succeeds for that CB (R-INT-4), never reaches it (I6); S3 rejects negative progress under a finite threshold and accepts it under `INT32_MAX`; a section whose lazy-operand keys lie outside the CB's own frame, as an inlined CB's can (N7), passes `prepare` with strict on.
- U4. Counter: for synthetic records covering T = 0, a finite T below and above the progress, T = `INT32_MAX` with a zero and a positive total, the `deferIndefinitely` state (`INT32_MIN` with a zero total), a fractional P, and a captured `m_counter` already past the floor, `finishCounter` on a CB after setup satisfies I7. For a finite T, its `CBCounterRestore` also equals an independent native reference: a stack `BaselineExecutionCounter` given the same triple and checked with `checkIfThresholdCrossedAndSet` on the same CB right after `finishCounter`. For T = `INT32_MAX` that check would defer indefinitely and drop P (`ExecutionCounter::setThreshold`, section 5.3), so those records, the `deferIndefinitely` state included, are checked against I7 alone: `r.crossed` is false, `r.nativeSlice` is the captured slice, and `count()` is what I7 states. The test's VM runs with `useConcurrentJIT` off and is the only VM in the process, so no thread changes the executable pool between the two calls (N4). Two records with the same finite T and the same P but different splits between `counterValue` and `counterTotalCount` (for example -500 with 1000.5 and -1000 with 1500.5), restored back to back on two CBs after setup, give identical counters, so the producer's slice stays local (I1). A `NotCarried` record, on a CB set up with a seeded reoptimization count of 0 and of 3 and with the UCB's quick DFG bit both set and clear, leaves the counter exactly as setup armed it and returns `carried` false with setup's slice (I7).
- U5. Score: `richnessUnits` equals a recount from the summary bytes, ignores the pruning mark, and is the same through `capture().score()` and `decodeSummary` of the written summary, with strict on and off (I8), as are `counterProgress` and `counterWithheld`, for captures called with `hasPolymorphicSite` true and false. `scoreLive` gives the same score when no drain can run between it and `capture` (section 4.4): the test calls both right after a synchronous full collection, whose finalization drains every visited CB, in a VM with `useConcurrentJIT` off, and runs no JS between them. That finalization also runs the native merges (N2), after which each CB copy equals the UCB copy at its place in the native walk, except that UCB array flags lack the pruning mark. So on a body that has an A15 entry and whose neighbouring profiles in each native walk saw different types (different array modes, for array profiles), every argument and value prediction in that capture's `cb.state` equals the prediction of the UCB copy I11 pairs it with, and every array record holds that copy's modes and, without the pruning mark, its flags (read through `UnlinkedArrayProfile::observedArrayModes()` and `arrayProfileFlags()`, SPEC-ucb.md section 5.4), so a lane walk out of step with the native one fails (I11, I12); every summary slot equals its `cb.state` slot ORed with that copy (I8). With strict on, `decodeSummary` of the written summary, with each header field and value mutated just inside and just outside its rule of section 3.4, returns a score or fails with exactly the expected `CBCheck`: a `counterMode` of 2 fails with `SummaryHeader`, and a `NotCarried` summary with a nonzero `counterProgress` with `SummaryValue`.
- U6. Accessors: E1 to E3 read and write exactly the fields they name, and `forEachOperandValueProfile` leaves buckets untouched (I2).
- U7. Realm step: on a CB of a fresh global object that has created no `Map` and no `Set`, `seedLinkedState` with an F20 record holding `FastMap` (`FastSet`), called inside a `DeferGC` as the install function's deferral surrounds it (R-INT-3), since the realm step allocates inside the lane's `AssertNoGC` scope (I3), leaves `mapProtoEntriesFunctionConcurrently()` (`setProtoValuesFunctionConcurrently()`) non-null, and a later `new Map` (`new Set`) installs that same function as `Map.prototype.entries` (`Set.prototype.values`); with neither bit, both stay null (I14).
- U8. No cells: `capture`, `scoreLive`, `prepare`, `seedLinkedState` without the two bits, and `finishCounter`, each called from the test with no GC deferral and inside the lane's `AssertNoGC` scope, trip no assertion in a debug build (I3). With a test budget that records each charge, `capture` charges exactly the sum of its two section sizes.
- U9. Twin check: `verifyTwins` passes on U4's CBs, which went through `prepare` and `seedLinkedState` with their own sections, and reports a counter that differs from its record, a `nativeSlice` above C, a crossed record whose P is below `T - min(T, C) / 2`, and a counter that did not travel whose threshold differs from setup's arming (I7).
- U10. No metadata table (I15): the newborn CB of `function id(x) { return x; }` has a null `metadataTable()`, and both walkers of section 3.1 call nothing on it. A CB of that body is called with an int and a string and compiled with `JIT::compileSync`. On it, `capture` with strict on returns a `cb.state` with `numValueProfiles` 0, every family count 0 and both argument predictions, and `scoreLive` gives the same score. `prepare` of that section with strict on succeeds on the newborn CB of a never-called function with the same body, and `seedLinkedState`, `finishCounter` and `verifyTwins` then run on that CB and report no difference. `function has(o) { return "x" in o; }`, which has a property IC and no metadata table, passes the same sequence.
- U11. Polymorphic bit: on one baseline CB whose counter has made at least one point of progress, `capture` with `hasPolymorphicSite` true writes `NotCarried` in both sections, three zero counter fields and a `counterProgress` of 0, and its `score()` and `scoreLive` with the same bit give `counterWithheld` true and a `counterProgress` of 0; with the bit false, `capture` writes `Carried` in both sections with the live triple, and both scores give `counterWithheld` false and that triple's `counterProgress`, at least 1 (I16).

### 11.3 Corpus (`JSTests/jitcache/cb/`)

One file per subject, each run as producer, then as consumer with twin checks, under the runner's directives and argument conventions (SPEC-integrator.md R-ALL-4, SPEC-integrator.harness.md section 7). Under the `Off` role a script follows R-ALL-4 and neither reads nor writes `scratch`. The subjects:

- every family of section 3.1, including `new_array_buffer` literals of each type whose hint upgrades past the literal's own type, `for-of` over arrays, maps, sets, strings and async generators, `for-in` with own-structure mismatches, `to_this` conflicts and `jneq_ptr`, plus a hot `get_by_val_with_this` site, whose array profile stays native (section 1);
- the realm step: a body whose `for-of` saw maps and sets in the producer, imported by a consumer that DFG-compiles it before creating any `Map` or `Set`, both in the shell's own realm and in one made by the jsc shell's `createGlobalObject`, with the DFG's assertions on in `debug-local`;
- argument profiles in functions and constructors;
- hot bodies without a metadata table, one with a property IC and one without (`function has(o) { return "x" in o; }`, `function id(x) { return x; }`), captured at both capture points with strict on and installed in the consumer with strict on;
- lazy-operand profiles, produced by forcing DFG OSR exits before the `delta` capture, in the root frame and inside an inlined callee: a callee that runs `for-of` over a user-defined iterator, inlined into a hot caller that iterates too (so the caller's tmps shift the callee's), and made to exit inside the inlinee on a check of one of its arguments and on a structure check of the checkpoint tmp that holds the iterator's next result, so its baseline CB holds machine-frame locals past its `numCalleeLocals()` and tmps at `maxNumCheckpointTmps` or above (N7); the producer commits that CB with strict on, and the consumer installs it and passes its twins;
- profile deferrals (a body with low coverage that defers to the DFG) and reoptimization counts (exit storms that jettison and recompile); the infinite-threshold path, which `adjustedCounterValue` reaches only after about twenty reoptimizations, is covered by U4;
- program, module, eval, `Function`-constructor and builtin bodies;
- the floor: a body that neither loops nor recurses and has no polymorphic site reaches `operationOptimize` on its second invocation and not during the first, even when its captured progress exceeds its threshold; the runner reads the line `operationOptimize` prints on entry under `verboseOSR`, which names the CB and its counter. Its runs set `--useConcurrentJIT=false` in their `jitcache-runs` line;
- a polymorphic body: a body whose property IC the producer saw with several shapes, captured by `delta` with progress past its threshold, commits `NotCarried` in `cb.state` and `cb.summary`, with a zero `counterProgress`, and in the consumer its counter starts from setup's arming, so it reaches `operationOptimize` only after the native warm-up; the runner reads the same `verboseOSR` lines, and its runs set `--useConcurrentJIT=false` the same way;
- concurrency, in two runs, each with captures in `delta` and imports, no crash, and I2 and I3 asserted ([history](SPEC-cb-history.md#concurrency-runs)). The first is a twins run with concurrent marking (`collectContinuously`, free) and `useConcurrentJIT` off: every lane's twins pass, this lane's because they run before `installCode` (N11). The second also turns `useConcurrentJIT` on, so DFG compiles run beside the captures and the imports: the consumer's output equals the JITCache-off run, and this lane's twins still pass, since the P12 check compares with `finishCounter`'s record, which concurrent compiles cannot change (section 11.1). The Image lane's twin check skips every import of this run (SPEC-image.md section 11.3), and the runner treats those skips as no difference, since the run's options turn `useConcurrentJIT` on (SPEC-integrator.harness.md section 7.4);
- I13: every committed `cb.state` passes V1 to V15 and S3 against the CB it came from, which SC1 checks at each capture since every corpus run has strict on; `prepare` is not the check, because its S1 and S2 reject the producer's CB, which has already run.

### 11.4 Bench

The bench loop sets every threshold. The glue takes the lane's share of its measurements around the lane's calls (R-INT-11), so the lane itself has no bench code:

- B1. Import cost per body: VM-thread CPU of `prepare`, `seedLinkedState` (the realm step included) and `finishCounter`, set against the body's native baseline compile after its profile drain plus finalization, for THREAD's installation bound. The bench measures the default, where `prepare` runs no structural check (THREAD Verification).
- B2. Bytes of `cb.state` and `cb.summary` per body and their share of the body file.
- B3. Capture: CPU and charged bytes per CB at both capture points, and `scoreLive` per CB in `delta`, for the capture pause and producer memory.
- B4. Samples a process's last `delta` loses (THREAD Capture): the bench follows that `delta` with a full collection, whose finalization drains the pending samples of every live LLInt and baseline CB natively, and then runs `delta` again; each slot category the second capture adds counts one lost sample of a type its slot never saw.

The floor's residue and the time from import to the first DFG compile of a body are native events after installation, the first `operationOptimize` crossing of an imported CB and the first `DFG::compileImpl` of its body, outside every lane call; the bench loop measures them, and SPEC-integrator.md IB10 sizes the residue. The lane adds no native hook for them ([history](SPEC-cb-history.md#the-bench-needs-no-lane-hook)): its input is each import's `CBCounterRestore`, which says how far from crossing the counter starts, and native code already logs both events under options that leave baseline emission alone: `operationOptimize` prints each entry with the CB and its counter under `verboseOSR`, and `DFG::compileImpl` prints each compile under `logCompilationChanges`. The same logs show how soon a body that carries no progress reaches its first DFG compile (section 5.3).

## 12. Tasks

Each task fits one implementation agent; a task starts when the ones it depends on have landed ([history](SPEC-cb-history.md#tasks-and-edit-ownership)). Every file has exactly one task: task 2 writes both headers in full, so later tasks only add their own `.cpp` file and test file, and tasks 4 and 5, which can run together, touch no common file. Every task that adds a unit test depends on the test target of M3 (R-INT-7, R-INT-10), and every task that adds a `.cpp` file depends on M2, which lists the lane's `.cpp` files in `Sources.txt`.

1. Native accessors E1 to E3 (section 9.1); U6 in `JITCacheCBAccessorTests.cpp`. Depends on M3.
2. The headers: `JITCacheCBFormat.h` with everything section 6.1 lists for it, and `JITCacheCBState.h` exactly as section 6.1 declares it. Then `JITCacheCBFormat.cpp` (the family table with its static checks, the guarded walkers of section 3.1, the size functions, the domain predicates and V8's instruction walk `forEachNewArrayBufferLiteral`) and `JITCacheCBValidate.cpp` (`validateState`, `validateSummary`, `counterObeysNativeInvariant` and `description`); U1 in `JITCacheCBFormatTests.cpp`. Depends on M3.
3. `JITCacheCBCapture.cpp`: `CBStateCapture::capture`, `scoreLive`, the other `CBStateCapture` members and the summary (sections 4.2 to 4.4), with SC1 to SC3 through task 2's validators; U11 in `JITCacheCBCaptureTests.cpp`. Depends on 1, 2, R-UCB-1 and R-INT-2; until the integrator's budget lands, a test budget stands in.
4. `JITCacheCBSummary.cpp`: `decodeSummary`; U5 in `JITCacheCBSummaryTests.cpp`. Depends on 2 and 3.
5. `JITCacheCBImport.cpp`: `CBStateImport::prepare`, `seedLinkedState` with its realm step and `finishCounter` (section 5); U2, U3, U4, U7 and U8 in `JITCacheCBImportTests.cpp`. Depends on 1, 2, 3 (U2 and U8 start from captured sections) and M1.
6. `JITCacheCBTwins.cpp`: `verifyTwins`; U9 and U10 in `JITCacheCBTwinsTests.cpp`. Depends on 3, 5, R-INT-10 and the `TwinReport` of R-INT-6 (SPEC-integrator.md tasks 1 and 2). It needs nothing of the integrator's install glue, which calls `verifyTwins` in twins builds (its task 8), and lands before it.
7. The corpus of section 11.3. Depends on 6 and on the integrator's install and capture glue and runner (R-INT-3 to R-INT-6; SPEC-integrator.md tasks 8, 9 and 13).

The bench reports of section 11.4 need no lane task: the glue takes them (R-INT-11).
