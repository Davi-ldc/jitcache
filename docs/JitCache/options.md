# JSC option classes

Every JSC option is must-match, fixed or free; an option this file does not list is free. `start` checks the fixed rows and rejects any call made while one of them differs from its required value.

## Must-match options

| option | type | why | lanes |
|---|---|---|---|
| `evalMode` | Bool | Bun sets it, and it changes bytecode (call-ignore-result and completion values) without entering any key. | UCB |
| `useExplicitResourceManagement` | Bool | Bun sets it, and it changes parsing. | UCB |
| `useImportDefer` | Bool | Bun sets it, and it changes parsing. | UCB |

## Fixed options

| option | type | required value | why | lanes |
|---|---|---|---|---|
| `useJIT` | Bool | true | THREAD Storage relies on it. `Options::notifyOptionsChanged` turns it off when `allowDoubleShape` is off, so this row also rejects that value (free table). | Image, CB |
| `useBaselineJIT` | Bool | true | THREAD Storage relies on it, and `shouldJIT`, the gate an install applies with the LLInt on, tests it (SPEC-integrator.md N4). | Image |
| `useDFGJIT` | Bool | true | THREAD Storage relies on it. Without it there is no baseline counter to carry, and every CB's capability class, a baked fact, is `CannotCompile` (compiler.md, "Profiling and tier-up counters"). | Image, CB |
| `useBaselineJITCodeSharing` | Bool | true | THREAD Storage relies on it: the CBs after the one that installs an import share its image natively (THREAD Restoration), through the sharing slot, which installation fills only with this option on (install.md, "Installation", step 3). | Image |
| `useLOLJIT` | Bool | false | THREAD Storage relies on it: with it on, a baseline plan compiles with `LOL::LOLJIT` in place of `JIT` (`BaselineJITPlan::compileInThreadImpl`). | Image |
| `forceUnlinkedDFG` | Bool | false | With it off, the only baseline or DFG failures that leave a persistent record come from lack of memory, which never reaches a capture (THREAD Restoration). `Options::notifyOptionsChanged` also forces it off at this pin. | UCB, CB |
| `forceDebuggerBytecodeGeneration` | Bool | false | One of THREAD Storage's debugger options: it adds `Debugger` to the code-generation mode (`JSGlobalObject::defaultCodeGenerationMode`, SPEC-ucb.md F16), whose bytecode holds `op_debug` and the ShadowChicken opcodes, which no recording compilation meets (SPEC-image.md N29 and section 4.1). | UCB, Image |
| `debuggerTriggersBreakpointException` | Bool | false | One of THREAD Storage's debugger options: it adds `Debugger` to the code-generation mode, as `forceDebuggerBytecodeGeneration` does. | UCB, Image |
| `alwaysUseShadowChicken` | Bool | false | THREAD Storage's ShadowChicken option. It changes bytecode: the generator emits the ShadowChicken opcodes outside Debugger mode too (SPEC-image.md N29), and their code calls C++ through an unrecorded pointer immediate (`CCallHelpers::ensureShadowChickenPacket`, SPEC-image.md N3) and reaches the ShadowChicken log, which images avoid (THREAD Restoration). | UCB, Image |
| `useSamplingProfiler` | Bool | false | One of THREAD Storage's options that make a new VM build PC-to-origin maps (`VM::VM`; SPEC-integrator.md N10, SPEC-image.md N15), which images do not carry (SPEC-integrator.md section 3.2). Bun's runtime CPU profilers call `VM::ensureSamplingProfiler`, which leaves the maps off. | Image |
| `alwaysGeneratePCToCodeOriginMap` | Bool | false | It makes a new VM build PC-to-origin maps, as `useSamplingProfiler` does. | Image |
| `switchJumpTableAmountThreshold` | Unsigned | 15 | It chooses switch-table forms in `NodesCodegen.cpp`. | UCB |
| `useTailCalls` | Bool | true | It feeds `BytecodeGenerator::m_allowTailCallOptimization`. | UCB |
| `optimizeRecursiveTailCalls` | Bool | true | It decides the bytecode shape of recursive tail calls. | UCB |
| `exposePrivateIdentifiers` | Bool | false | It governs the lexing of `@` names in non-builtin code. With it off, the keys of the `VariableEnvironment`s a program or module UCB holds are atoms (SPEC-ucb.codec.md section 6), and only builtin-mode parsing produces the super-sampler opcodes (SPEC-image.md section 4.1, census C20). | UCB, Image |
| `functionOverrides` | OptionString | null | It replaces function source at link time, breaking identity. | UCB |
| `returnEarlyFromInfiniteLoopsForFuzzing` | Bool | false | It registers loop-hint counters at UCB creation, which a decode does not, and the image reaches the VM's loop-hint counters (`VM::getLoopHintExecutionCounter`) by absolute address. | UCB, Image |
| `thresholdForJITAfterWarmUp` | Int32 | 500 | It is folded into the transported base threshold and into compact's L (THREAD Maintenance). | UCB |
| `thresholdForJITSoon` | Int32 | 100 | It is folded into the transported base threshold after finalization. | UCB |
| `useExceptionFuzz` | Bool | false | `AssemblyHelpers::callExceptionFuzz` (SPEC-image.md N3): an unrecorded operation pointer and the VM's exception-fuzz buffer (`VM::exceptionFuzzingBuffer`). | Image |
| `traceBaselineJITExecution` | Bool | false | `probeDebug` with a heap callback pointer at every bytecode and slow path (SPEC-image.md N3). | Image |
| `useProfiler` | Bool | false | Per-bytecode profiler counters by absolute address, in the profiler only this option creates (`VM::m_perBytecodeProfiler`, SPEC-image.md N29). | Image |
| `useTypeProfiler` | Bool | false | `op_profile_type`, the type profiler's log and `TypeLocation`s, an unshareable image. The type profiler adds `TypeProfiler` to the code-generation mode, which is in the key (SPEC-ucb.md F16); a host that turns it on at run time needs no row, as `useControlFlowProfiler`'s row says. | Image, UCB |
| `useControlFlowProfiler` | Bool | false | `op_profile_control_flow`, `BasicBlockLocation` counters, an unshareable image. A profiler a host turns on at run time, as Bun's coverage turns this one on through `VM::enableControlFlowProfiler` (`JSC__VM__enableControlFlowProfiler` in ~/bun `src/jsc/bindings/bindings.cpp`), needs no row: it changes the code-generation mode, which is in the key, and its opcodes make baseline code unshareable and so never captured (SPEC-ucb.md F16). | Image, UCB |
| `forceGCSlowPaths` | Bool | false | Inline allocations become slow-path jumps. | Image |
| `useJITAsserts` | Bool | `ASSERT_ENABLED` of the build | Entry, argument and arithmetic checks. | Image |
| `useJITDebugAssertions` | Bool | `ASSERT_ENABLED` of the build | `breakpoint()` checks in `put_to_scope` and the `get_from_scope` thunk. | Image |
| `eagerlyUpdateTopCallFrame` | Bool | false | A call-site store at every bytecode. | Image |
| `maximumInlineStringSwitchCaseCount` | Unsigned | 64 | It decides which `switch_string`s get the inline tree. | Image |
| `maximumOptimizationCandidateBytecodeCost` | Unsigned | 100000 | It decides the capability class, hence profiling writes and counters. | Image |
| `executionCounterIncrementForEntry` | Int32 | 15 | An immediate in `op_enter`, and the unit of the counter floor. | Image, CB |
| `executionCounterIncrementForLoop` | Int32 | 1 | An immediate in `loop_hint`. | Image |
| `thresholdForOptimizeAfterWarmUp` | Int32 | 1000 | It is folded into the transported base threshold T by `optimizeAfterWarmUp`. | CB |
| `thresholdForOptimizeAfterLongWarmUp` | Int32 | 1000 | It is folded into T by `handleExitCounts` and `optimizeAfterLongWarmUp`, and `reoptimizationRetryCounterMax` derives from it. | CB |
| `quickDFGTierUpThresholdFactor` | Double | 0.2, the Linux value of `Options::defaultQuickDFGTierUpThresholdFactor()` | It is folded into T while the UCB's quick DFG bit is set. | CB |
| `evalThresholdMultiplier` | Int32 | 10 | It is folded into T for eval code (`codeTypeThresholdMultiplier`). | CB |
| `reoptimizationRetryCounterMax` | Unsigned | 21, derived by `Options` from the default `thresholdForOptimizeAfterLongWarmUp` | It bounds the transported reoptimization count (SPEC-cb.md V14) and its shift in T. | CB |
| `minimumOptimizationDelay` | Unsigned | 1 | It gives the transported deferral count its meaning in `shouldOptimizeNowFromBaseline`. | CB |
| `maximumOptimizationDelay` | Unsigned | 5 | It gives the transported deferral count its meaning in `shouldOptimizeNowFromBaseline` and bounds that count (SPEC-cb.md V14). | CB |
| `useArrayAllocationProfiling` | Bool | true | It gives the transported hints their meaning (`ArrayAllocationProfile::updateProfile`). | CB |
| `jitPolicyScale` | Double | 1.0 | It rescales `thresholdForOptimizeAfterWarmUp` and `thresholdForOptimizeAfterLongWarmUp` before any compilation (`scaleJITPolicy`), and with them `thresholdForJITAfterWarmUp` and `thresholdForJITSoon`. | CB, UCB |
| `forceEagerCompilation` | Bool | false | It rewrites `thresholdForJITAfterWarmUp`, `thresholdForJITSoon`, `thresholdForOptimizeAfterWarmUp` and `thresholdForOptimizeAfterLongWarmUp` (`Options::notifyOptionsChanged`). | CB, UCB |
| `repatchCountForCoolDown` | Unsigned | 8 | One half of the cool-down pair THREAD Storage names; the carried `repatchCount` counts toward this threshold. | ICs |
| `initialCoolDownCount` | Unsigned | 20 | The other half of the cool-down pair; the carried `numberOfCoolDowns` scales it. | ICs |
| `useLLIntICs` | Bool | true | With it off, `DataOnlyCallLinkInfo::initialize` starts every site `Virtual` and `CallLinkInfo::revertCall` virtualizes on unlink, so a captured `Virtual` would record the option, and SPEC-ics.md S2 would reject every newborn CB. `performLLIntGetByID` also fills the LLInt's `get_by_id` mode cache only with it on, and that mode picks the shape the image compiles. | ICs, Image |
| `forceICFailure` | Bool | false | With it on, `tryCacheGetBy` and its siblings answer `GiveUpOnCache` at once and `linkPolymorphicCall` goes virtual, so a captured give-up or `Virtual` would record the option instead of the site. | ICs |
| `maxAccessVariantListSize` | Unsigned | 8 | A list that reaches it folds when the fold rules allow (`InlineCacheCompiler::tryFoldToMegamorphic`), so a carried fold (`canBeMegamorphic`) would record the option instead of the site; it also caps the listed cases, and so `caseCount`, at 8 (SPEC-ics.md section 5.2). | ICs |
| `thresholdForUndesiredMegamorphicAccessVariantListSize` | Double | 0.5 | `tryFoldToMegamorphic` refuses a get or `in` fold when at least this share of the listed cases is poly-proto, so a carried fold would record the option. | ICs |
| `maxPolymorphicCallVariantListSize` | Unsigned | 8 | `linkPolymorphicCall` sends a call site whose caller is neither top tier nor WebAssembly, every site the ICs lane carries, `Virtual` once its variant list exceeds it, so a carried `Virtual` would record the option. | ICs |
| `prototypeHitCountForLLIntCaching` | Unsigned | 2 | It seeds each `get_by_id` site's hit count (`GetByIdModeMetadata`), and `performLLIntGetByID` caches a prototype load only when that count reaches zero. The mode that results picks between `GetByIdPrototype` and `GetByIdSelf` in the image (`JIT::emit_op_get_by_id`), and a consumer CB born in baseline keeps the producer's choice. The shape is guarded, so a different value changes only speed. | Image |

## Free options the SPECs reason about

| options | why they stay free | lanes |
|---|---|---|
| `useCodeCache`, `useUnlinkedCodeBlockJettisoning`, `useBorrowedBytecodeFromCache`, `verifyBytecodeCacheChecksums`, `useSourceProviderCache` | The request points keep their native behaviour under any value. | UCB |
| `maximumExecutionCountsBetweenCheckpointsForBaseline`, `maximumExecutionCountsBetweenCheckpointsForUpperTiers`, `highCostBaselineProfilingFunctionBytecodeCost` | They shape the baseline slice ceiling, which the consumer recomputes for a finite threshold; the slice of an infinite threshold keeps the producer's ceiling, the upper-tier one after an FTL exit (SPEC-cb.md N4), which decides only when the next check runs, never P or T. The first also bounds the LLInt counter's slice, which the request point arms anew (SPEC-ucb.md section 5.5). | UCB, CB |
| `desiredProfileLivenessRate`, `desiredProfileFullnessRate`, `relaxedProfileCoverageFactorForQuickDFGTierUp`, `valueProfileFillingRateMonitoringBytecodeCost`, `useExecutionCountForCodeBlockAging`, `codeBlockAgingLeaseMultiplier`, `testTheFTL` | The profile coverage options, the aging options and `testTheFTL` shape only the consumer's own policy. | CB |
| `thresholdForOptimizeSoon` | Free in this version: `optimizeSoon`'s only caller, `operationOptimize`, arms it after `DFG::prepareOSREntry` succeeds, while the CB has an optimizing replacement and so is not eligible, and `CodeBlock::jettison` re-arms the CB with `optimizeAfterWarmUp` before it is the replacement again, so no captured T contains it. A DFG capture, which captures baseline CBs that still have a replacement, will need its row. | CB |
| `allowDoubleShape` | It decides whether a Double hint or array mode can exist at all, and needs no row: `Options::notifyOptionsChanged` turns `useJIT` off without it, and `useJIT` is fixed on. | CB |
| `dumpDisassembly`, `dumpBaselineDisassembly`, `dumpBaselineJITSizeStatistics`, `logJIT`, `useJITDump`, `useGdbJITInfo`, `useSourceCodeDump`, `needDisassemblySupport`, `disassembleBaselineForProfiler`, `maximumCachedAssemblerBufferSize`, `verboseOSR`, `reportTotalCompileTimes` | Baseline emission reads them, and they leave the bytes unchanged. | Image |
| `maxPolymorphicCallVariantListSizeForTopTier`, `maxPolymorphicCallVariantListSizeForWasmToJS`, `usePolymorphicCallInlining`, `useHandlerICInFTL`, `initialRepatchBufferingCountdown`, `repatchBufferingCountdown` | No state the ICs lane carries depends on them: `linkPolymorphicCall` reads the first two only for a top-tier or WebAssembly caller, option initialization forces `useHandlerICInFTL` off, and the last two serve the FTL only. | ICs |
| `useConcurrentJIT` | It changes when compilations run, not what they emit (SPEC-integrator.harness.md section 11.2), and Bun turns it off for one-shot runs (bunpatches.md, "Options set by the host"). The image twin check skips an import while it is on, so twins runs turn it off (SPEC-image.md section 11.3). | Image |
| `numberOfGCMarkers` | Bun sets it for one-shot runs (bunpatches.md); it decides how many threads mark, and a marker's drain costs only precision (THREAD Capture). | none |
| `thresholdForFTLOptimizeAfterWarmUp` | Bun sets it for `bun test --isolate` (bunpatches.md); it arms the DFG-to-FTL counter, which this version does not capture (THREAD's opening). | none |
| `useLLInt` | With it off, a newborn CB installs an import at the top of `JIT::compileSync` (THREAD Restoration; SPEC-integrator.md section 7.1). | none |
| `useDollarVM` | It installs the `$vm` test tool, which the runner passes to every twins-mode jsc run (SPEC-integrator.harness.md section 5.4). | none |
| `useExecutableAllocationFuzz` | The fault tests use it to fail executable allocations on purpose (SPEC-integrator.md section 15.2, SPEC-ics.md T9, SPEC-image.md T8). | ICs, Image |
| `jitMemoryReservationAddress`, `jitMemoryReservationSize` | They place and size the executable pool. Twins runs move the pool between processes, with the address or with placeholder mappings (SPEC-image.md R-INT-11), and THREAD Restoration recomputes memory-pressure slices against the consumer's own pool. | Image |
| `jitAllowlist`, `bytecodeRangeToJITCompile` | With `useBaselineJIT` they make up the `shouldJIT` gate (SPEC-integrator.md N4), which the consumer applies to its own options at install; a body the gate refuses stays native. | none |
| `osrExitCountForReoptimization`, `osrExitCountForReoptimizationFromLoop`, `ftlOSREntryFailureCountForReoptimization`, `useOSREntryToDFG`, `forceOSRExitToLLInt` | They decide when optimized code is entered or jettisoned, and so when a carried reoptimization count, exit site or `didOptimize` changes in the producer. A carried value means the same whatever policy produced it, and the consumer's optimizing tiers apply their own policy natively. | CB, UCB |
