# SPEC-integrator: the integrator

This SPEC specifies the integrator that THREAD Execution derives from the frozen lane SPECs. It is the entry point and indexes three sub-SPECs, one per subject that stands apart (section 1); all four files bind. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to the non-obvious decisions of the four files and binds nothing.

Code is cited by symbol and file. New symbols live where THREAD Execution puts JITCache's code, unless a row of section 14 says otherwise. "THREAD" is `docs/JitCache/THREAD.md`, and "VM thread" is the thread that holds the VM's API lock and heap access. The lane SPECs are SPEC-ucb.md, SPEC-image.md, SPEC-cb.md and SPEC-ics.md; "UCB R-INT-3" is requirement R-INT-3 of SPEC-ucb.md, and "Image", "CB" and "ICs" name the other three the same way.

## 1. Scope

The integrator owns:

1. The entry points `start`, `status` and `delta`, the per-VM state behind them, and the two hosts' use of them (the jsc shell and Bun).
2. The option table `start` checks, and the header that records the must-match options, the build IDs, the CPU feature vector and the app the producer's config named.
3. The container, in [SPEC-integrator.container.md](SPEC-integrator.container.md): the artifact's directories and file names, the lock file, the header file, the pack (its prefix and root, the records that hold the bodies and the index blocks) and the body format (envelope, section directory, section type ids, checksums) with their validation, the objects a process keeps for an opened artifact, the index and how a reader follows the root, opening, validating and pinning bodies, and the scoring read of saved bodies.
4. The writer, also in the container sub-SPEC, which appends a body to the pack, rereads and validates it, appends an index block, publishes the commit by storing the new valid end into the root, and compacts the pack.
5. Locks: the producer lock and the integrator's own in-process locks.
6. The install glue, which runs at THREAD's two install points and calls the lanes in THREAD's order, and the capture glue, which runs at the end of `BaselineJITPlan::finalize` and in `delta`, scores candidates and commits bodies.
7. Fault plumbing: invalid material, recording faults, the executable-allocation fault and its plan sites, and the debugger.
8. Maintenance, in [SPEC-integrator.maintenance.md](SPEC-integrator.maintenance.md): `compact` and its command line.
9. The producer budget and producer context the lanes charge against.
10. Test builds and bench reports, in [SPEC-integrator.harness.md](SPEC-integrator.harness.md): the twins build, the twin report and the twin checks' plumbing in a VM, address-space placement, the shell, `bun:jsc` and `$vm` helpers, the description of the heap JavaScript can reach, the runner and its oracle, the C++ test framework and runner, the bench report events, the per-body event counts, the comparison of the off path with a build of the pin, and the producer kills.

Each lane owns its sections (THREAD Storage) and the native edits its SPEC lists; THREAD Execution says what the integrator decides alone and has the glue call the lanes in THREAD's order.

In outline: `start` checks options and process facts, takes the producer lock for a producing role, creates or validates the header, maps the pack and reads its index, which the VMs of its process share, and publishes a `VMState` (sections 3.2 and 4). The UCB lane's request points read index tokens from that state and open bodies through it (section 6.2). When a newborn CodeBlock whose UCB carries a pending import reaches an install point, the install glue prepares every lane's part privately before it writes to the CB and installs it (section 7). After each successful baseline compilation, and in `delta` for every eligible CB and, with the delta capture `all`, for every body that ran without compiling, the capture glue scores the candidate against the saved body and commits a winner through the writer before JS resumes (section 8). Faults turn cache activity or production off for good, and each names its step in `status` (sections 4.2 and 4.5).

## 2. Native facts

The facts only the harness uses, N16 to N27, sit in the harness sub-SPEC's sections that use them.

- N1. `ScriptExecutable::prepareForExecutionImpl` (`runtime/ScriptExecutable.cpp`) runs under `DeferGCForAWhile`, creates the CB with `newCodeBlockFor`, then installs `codeBlock->unlinkedCodeBlock()->m_unlinkedBaselineCode` with `setupWithUnlinkedBaselineCode` when the slot holds code, and otherwise calls the file-static `setupLLInt` when `Options::useLLInt()` holds and `setupJIT` when it does not. It always ends with `installCode(vm, codeBlock, codeBlock->codeType(), codeBlock->specializationKind(), Profiler::JettisonReason::NotJettisoned)`. `UnlinkedCodeBlock::m_unlinkedBaselineCode` is a public member.
- N2. `setupJIT` calls `JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed)` and `RELEASE_ASSERT`s `CompilationSuccessful`. `JIT::compileSync` (`jit/JIT.cpp`, `static CompilationResult compileSync(VM&, CodeBlock*, JITCompilationEffort)` with an unnamed `VM&`) creates a `BaselineJITPlan`, compiles it on the calling thread and returns `plan->finalize()`. Bun's `node:vm` `Script` route also calls it, on a CB whose UCB Bun decoded (install.md, "Embedder routes").
- N3. `BaselineJITPlan::finalize` (`jit/BaselineJITPlan.cpp`) calls `JIT::finalizeOnMainThread`, which returns `CompilationFailed` for a null `BaselineJITCode` and otherwise adds the code-size sample and runs `setupWithUnlinkedBaselineCode`. On `CompilationFailed` it calls `dontJITAnytimeSoon` and sets `m_didFailJITCompilation`; on `CompilationSuccessful` it issues `WTF::crossModifyingCodeFence`, calls `installCode(m_codeBlock)` and `jitSoon`. `CodeBlock::dontJITAnytimeSoon` defers the UCB's LLInt counter indefinitely (`ExecutionCounter::deferIndefinitely`), so with the LLInt on, the LLInt's tier-up (`jitCompileAndSetHeuristics`) compiles no CB of that UCB again, and the body runs in the LLInt for as long as the UCB lives. `BaselineJITPlan::compileInThreadImpl` drains the value and array profiles (`CodeBlock::updatePredictionsConcurrently`) and then compiles inside one `Safepoint`. The plan's constructor calls `JIT::doMainThreadPreparationBeforeCompile` and then `UnlinkedCodeBlock::ensureValueAndArrayProfiles`, which allocates the UCB's value and array profiles when the body's first plan is built.
- N4. `shouldJIT` is a file-static `static inline bool shouldJIT(CodeBlock*)` in `llint/LLIntSlowPaths.cpp`, namespace `JSC::LLInt`, testing `bytecodeRangeToJITCompile`, the `jitAllowlist` and `useBaselineJIT`. Its callers are `entryOSR`, `loop_osr` and `replace`.
- N5. `VM::~VM` (`runtime/VM.cpp`) calls `heap.incrementDeferralDepth()` ("Never GC, ever again"), then, under `ENABLE(JIT)`, `JITWorklist::cancelAllPlansForVM`, then clears `m_perBytecodeProfiler`, then `m_apiLock->willDestroyVM(this)`, then `heap.shutDown()`, then frees the remaining VM structures. `Heap::shutDown` waits for a collection in progress, stops the collector thread and ends with `Heap::lastChanceToFinalize`, which is private and destroys every remaining cell, UCBs and UFEs included.
- N6. `JITWorklist::cancelAllPlansForVM` removes the plans that are not compiling, waits until every remaining plan of the VM is ready and drops the ready plans without finalizing them, so no worklist thread runs code for that VM afterwards.
- N7. `Heap::forEachCodeBlockIgnoringJITPlans(const AbstractLocker&, functor)` (`heap/HeapInlines.h`) calls `CodeBlockSet::iterate(locker, functor)`, which takes no lock itself: its caller must hold `vm.heap.codeBlockSet().getLock()`. The set holds the CBs marked at the last collection's End phase, when `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks` removes the unmarked ones, plus every CB created since: the `CodeBlock` constructors call `CodeBlockSet::add`, which takes the same lock, and nothing calls `CodeBlockSet::remove`. A CB the walk visits stays allocated until a later collection's sweep.
- N8. `JITWorklist::completeAllReadyPlansForVM` finalizes every ready plan of the VM under `DeferGC`. `JITWorklist::waitUntilAllPlansForVMAreReady` releases heap access while it waits.
- N9. A DFG plan that cannot allocate executable memory installs a `FailedFinalizer` at the `linkBuffer.didFailToAllocate()` branches of `SpeculativeJIT::compile` and `SpeculativeJIT::compileFunction` (`dfg/DFGSpeculativeJIT.cpp`), and an FTL plan does so through `FTL::fail` at the two `state.allocationFailed` branches of `DFG::Plan::compileInThreadImpl` (`dfg/DFGPlan.cpp`), after `FTL::compile` and after `FTL::link`. Two paths set `FTL::State::allocationFailed`: `FTL::compile` when `b3CodeLinkBuffer->didFailToAllocate()` (`ftl/FTLCompile.cpp`), and `FTL::LowerDFGToB3::compileCallFFIImpl` when `FFI::Signature::invokeThunk` returns null. On the supported targets that happens only when the `LinkBuffer` of `FFI::generateInvokeThunk` failed to allocate (`ffi/FFIInvokeThunk.cpp`), and `invokeThunk` generates a missing thunk on its first call, so a lowering that finds none allocates it on the compiling thread. `DFG::Plan::finalize` asserts that GC is deferred, computes the result, write-barriers the CB and then calls `m_callback->compilationDidComplete`, which writes the failure's effects (SPEC-ucb.md F24). `JITPlan::isFTL()` tells an FTL plan from a DFG one.
- N10. `Debugger::attach` (`debugger/Debugger.cpp`) calls `m_vm.setShouldBuildPCToCodeOriginMapping()`. It is reached from `JSGlobalObjectDebugger::attachDebugger` when `Debugger::addObserver` adds the first observer. The VM's constructor also turns the mapping on under `useSamplingProfiler` and `alwaysGeneratePCToCodeOriginMap`, and `VM::shouldBuilderPCToCodeOriginMapping()` reads it.
- N11. The x86_64 assembler's predicates are `supportsSSE3`, `supportsSupplementalSSE3`, `supportsSSE4_1`, `supportsFloatingPointRounding`, `supportsCountPopulation`, `supportsAVX`, `supportsAVX2` and `supportsFloat16` (public), and `supportsLZCNT` and `supportsBMI1`, which are protected and return `s_lzcntCheckState == Set` and `s_bmi1CheckState == Set` after `collectCPUFeatures()`; both states and `collectCPUFeatures` are public (`assembler/MacroAssemblerX86_64.h`). The ARM64 predicates, all public, are `supportsFloatingPointRounding`, `supportsCountPopulation`, `supportsFloat16`, `supportsDotProd`, `supportsLSE`, `supportsDoubleToInt32ConversionUsingJavaScriptSemantics`, `supportsRoundFloatToIntegerFloat` and `supportsSHA3` (`assembler/MacroAssemblerARM64.h`).
- N12. Every option of the must-match and fixed tables of options.md exists with the type and compiled-in default they give (`runtime/OptionsList.h`). `useExplicitResourceManagement` and `useImportDefer` are `Bool` options generated from `WTF/Scripts/Preferences/UnifiedWebPreferences.yaml` into `JSCWebPreferenceOptions.h`, which `OptionsList.h` includes. `reoptimizationRetryCounterMax` compiles in 0 and `Options` derives its effective value; `quickDFGTierUpThresholdFactor` compiles in `Options::defaultQuickDFGTierUpThresholdFactor()`, which returns 0.2 outside `PLATFORM(MAC)`. `OptionsStorage::OptionString` is `const char8_t*`.
- N13. `Heap::hasHeapAccess()`, `Heap::currentThreadIsDoingGCWork()` (a GC thread, or a mutator whose `mutatorState()` is not `Running`) and `VM::currentThreadIsHoldingAPILock()` are public.
- N14. The jsc shell parses its command line (`CommandLine::parseArguments`, `jsc.cpp`) before `JSC::initialize()` in `jscmain`. An argument starting with `--` that no earlier test matched is handed to `Options::setOption`, and the arguments after `--` reach the script as `arguments`. `runJSC` creates the VM with `VM::create` and then, inside the run loop and under `JSLockHolder`, creates the shell's `GlobalObject`. With `--destroy-vm` it derefs the VM at the end, which runs `VM::~VM`.
- N15. Bun:
  - `JSCInitialize` (`src/jsc/bindings/ZigGlobalObject.cpp`) sets options once per process inside `std::call_once` and calls `JSC::initialize` with a callback.
  - `Zig__GlobalObject__create` creates the VM of every `VirtualMachine` (main thread, workers, macros, the debugger thread) with `JSC::VM::tryCreate`, calls `vm.heap.acquireAccess()`, takes `JSC::JSLockHolder`, then calls `WebCore::JSVMClientData::create` and creates the global object. For a VM that is not the main thread's it tests `executionContextId == INT32_MAX || executionContextId > 1`; the comment above it, which gives -1 for the main thread, is stale.
  - The `executionContextId` comes from `VirtualMachine::init` (`src/jsc/VirtualMachine.rs`): `opts.context_id`, or 1 when `opts.is_main_thread` holds and `INT32_MAX` otherwise. A worker passes `worker.execution_context_id()`, which `WorkerMessagingProxy` takes from `ScriptExecutionContext::generateIdentifier()`, a process-wide counter that starts at 1 and is incremented before each use, so every worker's id is above 1. The debugger thread's VM (`start_js_debugger_thread`, `src/jsc/Debugger.rs`) and macro VMs pass no id and get `INT32_MAX`. `Macro::init` (`src/js_parser_jsc/Macro.rs`) creates a macro VM on a thread that has no `VirtualMachine` (`VirtualMachine::is_loaded()`), and the bundler, `Bun.build`'s included, parses on the threads of its own pools (`ThreadPool::schedule_with_options`, `src/bundler/ThreadPool.rs`), so the bundler runs every macro in a macro VM.
  - `bun test --isolate` makes its later global objects in the same VM (`Zig__GlobalObject__createForTestIsolation`). Bun also creates VMs for its bytecode builder (`vmForBytecodeCache` in `ZigSourceProvider.cpp`) and for Bake's production global (`BakeCreateProdGlobal`).
  - `RunCommand::boot` (`src/runtime/cli/run_command.rs`) starts every run of a file, `-e`, `-p`, `bun run -` and the Node REPL. Its callers pass the entry path: the file the resolver found or the opened descriptor names, or `<cwd>/[eval]` and `<cwd>/[stdin]`, whose text `ctx.runtime_options.eval.script` holds; an entry ending in `.sh` runs Bun's shell and returns before any VM exists. It then calls `bun_jsc::initialize` and `VirtualMachine::init`, which creates the main VM. `RunCommand::boot_standalone` does the same for a `bun build --compile` executable through `VirtualMachine::init_with_module_graph`, and the entry's embedded text is `graph.entry_point().contents` (`src/standalone_graph/StandaloneModuleGraph.rs`). `bun test` and `bun repl` create their main VM elsewhere (`test_command.rs`, `repl_command.rs`). `ctx` is the process's one `ContextData`, which `bun_options_types::context::try_get` returns.
  - `bun_highway::xxhash3_64(seed, bytes)` (`src/highway/lib.rs`) is XXH3-64, bit-identical to the xxHash reference, and `Bun.hash.xxHash3(data)` returns it with seed 0 (`src/runtime/api/HashObject.rs`).
  - `Zig::SourceProvider::create` (`src/jsc/bindings/ZigSourceProvider.cpp`) makes the provider of every module Bun loads from a `ResolvedSource`, and when the `ResolvedSource`'s `bytecode_cache` is not null it hands those bytes to JSC as the provider's `CachedBytecode`. They come from three places: the `.jsc` file beside a bundle that `bun build --bytecode` wrote, with `already_bundled` set (`src/runtime/jsc_hooks.rs`, `src/jsc/RuntimeTranspilerStore.rs`); a file of a `bun build --compile --bytecode` executable, from the standalone probe of `jsc_hooks.rs`, the only path that sets `origin_path`; and the Node compile cache (`src/jsc/NodeCompileCache.rs`), which `NODE_COMPILE_CACHE` or `module.enableCompileCache()` turn on and `NODE_DISABLE_COMPILE_CACHE` turns off.
  - `VirtualMachine::on_exit` (`src/jsc/VirtualMachine.rs`) clears pending exceptions, forbids script in a worker its parent terminated, writes the CPU and heap profiles when they were requested, runs the user's exit handlers through `ExitHandler::dispatch_on_exit`, runs the deferred tasks, stops the VM's handle and runs the Node-API cleanup hooks when Node would. Then, on the main thread, it persists the Node compile cache (`node_compile_cache::persist_at_exit`) and writes the bytecode order file (`bytecode_order_recorder::write_at_exit`). `global_exit` and the worker teardown come after it.
  - The VM's startup deferral scale, `VM::startupJITDeferralScale`, multiplies the LLInt-to-baseline and baseline-to-DFG thresholds at each check. `VM::VM` starts it from the option `startupJITDeferralScale`. `run_command.rs` sets it with `JSC__VM__setStartupJITDeferralScale` on the main VM of a standalone executable built with `--compile-jit-policy`, after `VirtualMachine::init_with_module_graph` has created that VM and so after `configureVM` (section 11.2) has run, and `Bun.unsafe.setJITPolicy` sets it at run time.
  - Bun's own link passes `-Wl,--build-id=sha1` (`scripts/build/flags.ts`). The local WebKit recipe (`scripts/build/deps/webkit.ts`) builds the `jsc` target with `ENABLE_STATIC_JSC`, `-fno-pic -fno-pie -no-pie`, `CMAKE_POSITION_INDEPENDENT_CODE` off and no build-id flag, and Bun compiles its own code with `-fno-pic -fno-pie` and links with `-fno-pic -Wl,-no-pie` (`scripts/build/flags.ts`), so in the jsc shell and in Bun alike the engine sits at its link-time address in every process of a build (SPEC-image.md N23). The local WebKit recipe also passes Bun's CPU target (`computeCpuTargetFlags`): `-march=nehalem` on x86_64, which includes SSE4.2, and `-march=armv8-a+crc` on Linux ARM64.
  - Bun's build globs its `.cpp` files and bundles them into unified sources (`scripts/build/unified.ts`).
  - Bun's config resolution (`scripts/build/config.ts`) refuses a clang whose LLVM major differs from rustc's. bun-v1.4.3 pins LLVM 23.1.1 (`scripts/build/ci-images/spec.ts`; the builder accepts any 23.1.x) and Rust `nightly-2026-09-15` (`rust-toolchain.toml`). `scripts/build/profiles.ts` lists `ci-release` in `retiredProfiles`, whose message names `--profile=release --ci=on --buildkite=on` as its replacement; `release` builds with LTO by default. Bun's builder takes its CI path (packaging and uploads, the inherited symbol order file) only when the environment says CI or Buildkite (`isCI` in `scripts/buildkite.ts`).

## 3. Public interface

### 3.1 Types

`JITCacheAPI.h` is exported to Bun (section 3.5), includes `wtf/ScopedLambda.h` beside the WTF text headers, and declares:

```cpp
namespace JSC {
class VM;
namespace JITCache {

enum class Role : uint8_t { Consumer = 1, Producer = 2, ConsumerProducer = 3 };

// What delta captures beyond the compiled bodies: with All, also the bodies that ran without compiling; with Compiled,
// nothing more. Without a delta only compiled bodies are captured, whatever it says. A Consumer captures nothing and
// ignores it.
enum class DeltaCapture : uint8_t { All = 1, Compiled = 2 };

struct Config {
    String artifactPath;                      // the directory that holds the lock file and cache/
    Role role { Role::Consumer };
    std::optional<size_t> producerLimitBytes; // producing roles; empty: defaultProducerLimitBytes
    bool strict { false };                    // off unless the host turns it on
    DeltaCapture deltaCapture { DeltaCapture::All };   // All unless the host sets Compiled
    std::optional<uint64_t> appDigest;        // the app the host runs, which JITCache only compares; empty: no app
    String benchReportPath;                   // empty: no bench report
    String twinReportPath;                    // ENABLE(JITCACHE_TWINS) builds; ignored elsewhere
};

// The artifact path of a bare --jitcache and of a maintenance command without one.
static constexpr ASCIILiteral defaultArtifactPath = "./.jitcache"_s;

enum class StartOutcome : uint8_t { Started, Busy, Rejected, Fault };
struct StartResult {
    StartOutcome outcome;
    ASCIILiteral step;   // for Started, empty, or "start.replaced" when a ConsumerProducer replaced an incompatible artifact
                         // or one made for another app
    String detail;
};

enum class FaultClass : uint8_t { StartFault, InvalidMaterial, ExecutableMemory, RecordingFault, DebuggerAttached };
struct FaultReport {
    FaultClass faultClass;
    ASCIILiteral part;   // "ucb", "image", "cb", "ics", or empty when check is a full step name
    ASCIILiteral check;
    String detail;
    String stepName() const;   // part + "." + check, or check when part is empty
};

enum class SessionState : uint8_t { Unconfigured, Started, Faulted };
enum class ProductionState : uint8_t { NotProducing, Active, Ended };

struct Progress {
    uint64_t indexedBodies { 0 };
    uint64_t bodyOpens { 0 };             // openBody calls that reached a body in the pack
    uint64_t transientOpenFailures { 0 };
    uint64_t imports { 0 };               // UCB statistics: imports, attaches, gateDrops, misses (summed)
    uint64_t attaches { 0 };
    uint64_t gateDrops { 0 };
    uint64_t misses { 0 };
    uint64_t installs { 0 };
    uint64_t bakedFactMismatches { 0 };
    uint64_t captureCandidates { 0 };
    uint64_t capturesDeferred { 0 };      // a saved body that a transient error kept from being read
    uint64_t capturesCommitted { 0 };
    uint64_t bytesCommitted { 0 };
    uint64_t deltaRuns { 0 };
};

struct BudgetSnapshot { size_t limitBytes { 0 }; size_t chargedBytes { 0 }; size_t peakBytes { 0 }; bool refused { false }; };

struct Status {
    SessionState state { SessionState::Unconfigured };
    std::optional<Role> role;
    bool strict { false };
    bool activityOn { false };
    std::optional<FaultReport> activityFault;
    ProductionState production { ProductionState::NotProducing };
    std::optional<FaultReport> productionFault;
    std::optional<FaultReport> firstFault;   // the earlier of the two
    Progress progress;
    BudgetSnapshot budget;
};

enum class DeltaOutcome : uint8_t { Completed, Rejected, Faulted };
struct DeltaResult {
    DeltaOutcome outcome;
    ASCIILiteral rejection;            // Rejected: the step that rejected the call
    std::optional<FaultReport> fault;  // Faulted: the fault that stopped production or activity
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 };    // committed by this call, before any fault
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 };       // keys whose saved body a transient error kept from being read
};

JS_EXPORT_PRIVATE StartResult start(VM&, const Config&);
JS_EXPORT_PRIVATE Status status(VM&);
// The caller holds no JSC-internal lock, a documented duty that JSC cannot check.
JS_EXPORT_PRIVATE DeltaResult delta(VM&);
// VM thread. Writes out the bench report's buffered lines; does nothing for a VM with no state or no report.
JS_EXPORT_PRIVATE void flushBenchReport(VM&);
// The end of a host's run on one VM, on the VM's thread with its API lock held and no JSC-internal lock.
JS_EXPORT_PRIVATE void finishHostRun(VM&, bool deltaAtExit, const ScopedLambda<void(const std::optional<DeltaResult>&)>& hostWork);
JS_EXPORT_PRIVATE ASCIILiteral name(StartOutcome);
JS_EXPORT_PRIVATE ASCIILiteral name(Role);
JS_EXPORT_PRIVATE ASCIILiteral name(DeltaCapture);
JS_EXPORT_PRIVATE ASCIILiteral name(FaultClass);
JS_EXPORT_PRIVATE std::optional<Role> parseRole(StringView);   // "consumer", "producer", "consumer-producer"
JS_EXPORT_PRIVATE std::optional<DeltaCapture> parseDeltaCapture(StringView);   // "all", "compiled"

// One-line JSON for hosts' logs: {"jitcache":"start"|"delta"|"status", ...} with every field of the struct by name,
// enums as the names above (lowercase, words joined by "-"), faults as {"class","step","detail"}.
JS_EXPORT_PRIVATE String toJSON(const StartResult&);
JS_EXPORT_PRIVATE String toJSON(const DeltaResult&);
JS_EXPORT_PRIVATE String toJSON(const Status&);

} } // namespace JSC::JITCache
```

`JITCacheBun.h`, exported beside it (section 3.5), declares the entry points only Bun's integration calls, so that `JITCacheAPI.h` stays the interface of any host. It includes only WTF headers and `JavaScriptCore/JSExportMacros.h`, and declares:

```cpp
namespace JSC {
class JSGlobalObject;
namespace JITCache {

struct ModuleAnalysisImport {
    std::array<uint8_t, 32> sourceDigest;   // the digest the module's key was built from; the provider supplies it afterwards
    Vector<uint8_t> moduleInfo;             // the body's analysis in Bun's self-contained module-info format; empty when none
};

// For Bun's provider of an ES module whose analysis Bun would otherwise parse from the text. The bytes it returns are in
// Bun's module-info format, which only Bun's loader reads. VM thread, API lock held; source is the provider's whole text.
// Empty when the VM does not import or a fault stopped the import; Bun then creates the provider as it would without
// JITCache.
JS_EXPORT_PRIVATE std::optional<ModuleAnalysisImport> importModuleAnalysis(JSGlobalObject&, StringView source,
    const std::array<uint8_t, 32>* suppliedDigest);

} } // namespace JSC::JITCache
```

`ModuleAnalysisImport` and `importModuleAnalysis` are SPEC-ucb.analysis.md section 5.1's entry (R-INT-A3), with `std::array<uint8_t, 32>` for the UCB lane's `Digest256`, which names that type. The UCB lane defines the function in `ucb/UCBAnalysis.cpp`, and `ucb/UCBAnalysis.h` includes `JITCacheBun.h` and declares neither again (section 14.1). ([history](SPEC-integrator-history.md#buns-module-analysis-entry-has-a-header-of-its-own))

### 3.2 `start`

`start(vm, config)` runs on the VM thread. A call before the VM's first global object covers every body, and a later one misses the bodies created before it and everything nested in them (THREAD Session). It writes nothing to the VM before step 10, so `Busy` and `Rejected` leave the VM unconfigured and the host may call `start` again. It takes no JSC lock but its callees', allocates no cell and stops for no collector.

`Config::appDigest` names the app (THREAD Session): a value the host computes from the code it runs, which JITCache never computes or interprets. It only compares two of them for equality, the config's with the one an artifact's header holds, and writes the config's into the header of an artifact it creates (container sub-SPEC section 3). The value is 64 bits, the width of the XXH3-64 Bun computes (section 11.2): it only gates, since every body still matches by its own key, so a collision between two apps costs no more than an app left unnamed. ([history](SPEC-integrator-history.md#the-app-digest-is-64-bits-and-only-a-gate))

0. The process runs on a target THREAD names, Linux on x86_64 or ARM64 with the JIT (`OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))`); otherwise `Rejected` at `start.platform` (section 11). On another Linux CPU the header would record no architecture (container sub-SPEC section 3.1), and without the JIT `VM::~VM` never calls the teardown hooks (M4).
1. The caller holds the API lock and heap access (`vm.currentThreadIsHoldingAPILock()`, `vm.heap.hasHeapAccess()`); otherwise `Rejected` at `start.locks`.
2. `vm.jitCacheState()` is null; otherwise `Rejected` at `start.already-configured`, since started and fault configure the VM for good.
3. The config is well formed: a non-empty `artifactPath`, a role in range, a `deltaCapture` in range in every role, for a producing role a limit from `producerLimitBytes` or `defaultProducerLimitBytes` (section 16), a `benchReportPath` that is empty or opens (harness sub-SPEC section 9.1), and in twins builds a `twinReportPath` that is empty or opens (harness sub-SPEC section 2); otherwise `Rejected` at `start.config`, with a detail naming the field. A report opened here is closed again on any outcome but `Started` and `Fault`.
4. Every fixed option has its required value (section 5.1); otherwise `Rejected` at `start.fixed-option`, naming the first option that differs, its required value and its effective value.
5. `vm.shouldBuilderPCToCodeOriginMapping()` is false; otherwise `Rejected` at `start.pc-maps`: a debugger attached before `start` turned the mapping on (N10), and images carry no PC-to-origin maps.
6. The process facts of section 5.2 are known: the build IDs, the must-match option values and the CPU feature vector. A main executable without a build ID is `Rejected` at `start.build-id` (THREAD Storage).
7. `start` reaches the artifact (container sub-SPEC sections 1 to 6) through `openArtifact(path, mode, config.appDigest)` (`store/ArtifactAccess.h`, container sub-SPEC section 1.5), whose mode is the role's: `Create` for a Producer, `Open` for a Consumer and `OpenOrReplace` for a ConsumerProducer. It takes these steps in order and returns an `ArtifactAccess` (the producer lock, the `OpenedArtifact` and, after a replacement, why the old artifact was replaced) or an `ArtifactAccessFailure` whose kind `start` maps to its result:
   - every role opens the parent directory; a Producer creates a missing parent (`mkdir`, one level, mode 0755), and the other roles are `Rejected` at `start.artifact-missing`;
   - a producing role takes the producer lock with `ProducerLock::tryAcquire`, without blocking, and the `VMState` keeps it (container sub-SPEC section 2); `busy` is `Busy` at `start.busy`, and nothing stays open;
   - a Producer finding a header is `Rejected` at `start.artifact-exists`, and one finding a header-less `cache/` whose pack holds a body (`packHoldsBodies`) is `Rejected` at `start.not-an-artifact`; otherwise it creates the artifact: `cache/`, or the remnant with its pack marked retired and its temporaries removed, then an empty pack, then the header, which names the config's app when the config names one, so a reader that finds the header finds the pack (container sub-SPEC section 1.3);
   - a Consumer or ConsumerProducer finding no `cache/` directory or no header is `Rejected` at `start.artifact-missing`; it reads the header with `readHeaderFile` (`store/JITCacheFiles.h`), which opens it with `O_NONBLOCK` and reads a file that is not a regular file, such as a FIFO, as empty, so it fails the header's checks instead of blocking `start`; a corrupt header is a `Fault` at `start.header`; for a Consumer, an incompatible header is `Rejected` at `start.incompatible`, with a detail naming the first field that differs, and a compatible header made for another app, one that names an app other than the config's while the config names one, is `Rejected` at `start.another-app`, with a detail giving both apps (container sub-SPEC section 3.2); a ConsumerProducer replaces either artifact under the producer lock it holds, moving `cache/` aside to `.cache.replaced`, creating a new artifact as a Producer does and then removing the old directory (container sub-SPEC section 1.4), and keeps that detail for step 8 ([history](SPEC-integrator-history.md#a-consumerproducer-replaces-an-incompatible-artifact));
   - every role then takes the artifact's `OpenedArtifact` with `ArtifactRegistry::take` (container sub-SPEC section 5.1), which maps the pack and reads its index, and which the VMs of a process that open one artifact share (II21); a pack that fails its checks, or a `cache/pack` that is not a regular file, is a `Fault` at `start.pack`, with the rejection as its detail (container sub-SPEC section 5.2).

   Any other I/O error, such as a `flock` that fails other than busy, a remnant's listing that fails or a mapping that fails, is a `Fault` at `start.io`. A rejection or busy result taken after the lock was acquired releases it.
8. Outcome: `Started`, at `start.replaced` with the detail of step 7 when a ConsumerProducer replaced the artifact, or the `Fault` of step 7 (`start.header`, `start.pack` or `start.io`). The role says whether the artifact was created or opened.
9. On `Fault`, the producer lock, if taken, is released, since a faulted VM never produces.
10. A `VMState` is created with the role, the config, the outcome and, on `Fault`, activity off with the start fault recorded (section 4.2), and published with `vm.setJITCacheState` (release store). It keeps the bench report of step 3, hands it the `OpenedArtifact` of step 7 with `setArtifact`, whose statistics each flush's `index` line reads, and, for a producing role, the producer budget with `setBudget`, which each flush's `budget` line reads (harness sub-SPEC section 9.2); in twins builds it also creates the twin budget and keeps the twin report of step 3 (section 4.1; harness sub-SPEC section 2).

### 3.3 `status`

`status(vm)` runs on the VM thread and reads only: it opens no file, raises no fault and changes no state (THREAD Session). For a VM without state it returns `SessionState::Unconfigured`. Otherwise it reports the session state (`Faulted` when `start` faulted), the role, strictness, whether activity is on and why it went off, the production state and why it ended, the earlier of the two faults as `firstFault`, the progress counters of `VMState`, the index's size as `Progress::indexedBodies` (`OpenedArtifact::indexedBodies()`, read without following the root), the UCB registry's statistics, and the budget snapshot. It reports no app: the config's is the host's own, and a start that an app decided says so in its `StartResult` (section 3.2). A producer budget that has refused a charge reports production `Ended` with the fault `budget.limit` even when the VM thread has not raised it yet (section 4.5).

### 3.4 `delta`

`delta(vm)` is exposed only in C++ (THREAD Session). It rejects without work, returning `Rejected` with these steps:

- `delta.unconfigured`: the VM has no state;
- `delta.role`: the role is Consumer;
- `delta.locks`: the calling thread does not hold the API lock or heap access;
- `delta.collector`: `vm.heap.currentThreadIsDoingGCWork()` holds.

These are THREAD Session's requirements. A call made while a capture runs needs no rejection, since no capture reaches one (section 8.7).

Once the preconditions pass, `delta` calls `releaseEndedProductionMemory()` (section 4.2), which only frees what production held and is no work in this sense. When activity is off or production has ended, it then returns `Faulted` with the recorded fault and no work. Otherwise it runs section 8.6 and returns `Completed` with the counts, or `Faulted` with the fault and the counts committed before it (THREAD Failures).

### 3.5 Exported headers

Four headers join `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` (section 14, M2), and Bun includes them as `<JavaScriptCore/JITCacheAPI.h>`, `<JavaScriptCore/JITCacheBun.h>`, `<JavaScriptCore/JITCacheMaintenance.h>` and `<JavaScriptCore/JITCacheTwinsHost.h>`. Bun's build sees only the copied headers (SPEC-image.md section 14.4), so every include must resolve among them: no exported JSC header includes any `jitcache/` header, and `VM.h` names the state through a forward declaration only (M3). Test T-HEADERS checks it (section 15.1). ([history](SPEC-integrator-history.md#buns-twins-build-reaches-only-exported-headers))

- `JITCacheAPI.h`, `JITCacheBun.h` and `JITCacheMaintenance.h` include only WTF headers and `JavaScriptCore/JSExportMacros.h` and declare no member that exists only under `ENABLE(JITCACHE_TWINS)`. The first and the third serve any host that embeds JavaScriptCore (THREAD Session); `JITCacheBun.h` holds what only Bun's integration calls (section 3.1).
- `JITCacheTwinsHost.h` holds what a host calls in twins builds, all of it under `ENABLE(JITCACHE_TWINS)`, so a build without twins sees an empty header. The jsc shell includes it as well, so both hosts call the same declarations, each with `JS_EXPORT_PRIVATE`:
  - the four placement and layout functions that Bun's `JSCInitialize` and `JITCacheHost::configureVM` call, with the enumeration `HeapProbes` that `recordVMLayout` takes (harness sub-SPEC section 4), with only WTF and JSC types in their signatures (`VM&`, forward-declared, `const String&` and `const Vector<String>&`);
  - the six host functions that `bun:jsc` registers (harness sub-SPEC section 5.3), declared with `JSC_DECLARE_HOST_FUNCTION`, for which the header also includes `JavaScriptCore/JSCJSValue.h`;
  - `void writeBodyEvents(VM&, const String& path)`, which `Bun__JITCache__atExit` calls (harness sub-SPEC section 10.3);
  - `void verifyModuleAnalysisTwin(VM&, JSModuleRecord& fromArtifact, JSModuleRecord& fromParse)`, with `JSModuleRecord` forward-declared, which Bun's `fallbackParse` calls for a record built from JITCache's module info (SPEC-ucb.analysis.md section 8 and R-INT-A4). `harness/ShellFunctions.cpp` defines it beside the other twins host functions, since it calls the UCB lane's `firstModuleAnalysisDifference`: when the VM's `twinReportSink()` is non-null and `firstModuleAnalysisDifference(fromArtifact, fromParse)` names a field, it reports a difference of part `UCB` and check `analysis` whose detail is that field and the record's `moduleKey()`; otherwise it does nothing.

### 3.6 The end of a host's run

`finishHostRun(vm, deltaAtExit, hostWork)` ends a host's run on one VM, and both hosts call it at exit (section 11). It runs on the VM's thread with the API lock held and no JSC-internal lock, and takes these steps:

1. It calls `delta(vm)` when `deltaAtExit` holds and the VM's `productionState()` is `Active` (section 4.2), so a Consumer, a VM that `start` never configured and a production that ended get no `delta`. A `delta` there would only reject the call or report the fault that ended production, and `willDestroyVM` frees what an ended production held (section 4.6).
2. It calls `hostWork` with that `delta`'s result, or with `std::nullopt` when step 1 called none. The host prints its exit lines there and reads the final status at the point its own order puts it.
3. It calls `flushBenchReport(vm)`, so a process that exits without destroying its VM keeps the events still buffered, in every role.

## 4. Per-VM state

### 4.1 `VMState`

```cpp
namespace JSC::JITCache {

class ArtifactWriter;
class BenchReport;
class OpenedArtifact;
class ProducerLock;
struct ImageTwinCheckState;   // twins builds; defined in JITCacheInstall.cpp
struct KeptSummaries;         // the kept summaries; defined in JITCacheCapture.cpp
class SupportTargetTable;     // the Image lane's resolved support targets; defined in ImageSupport.cpp
class UCBRegistry;            // the UCB lane's registry; its users include UCBRegistry.h

class VMState final {
    WTF_MAKE_NONCOPYABLE(VMState);
    WTF_MAKE_TZONE_ALLOCATED(VMState);
public:
    // The lanes' interface. VM thread unless noted.
    bool tracksKeys() const;          // activity on (any thread)
    bool importsEnabled() const;      // role Consumer or ConsumerProducer, and activity on (any thread)
    bool productionActive() const;    // producing, activity on and production not ended (any thread)
    bool strict() const;              // Config::strict (any thread)
    DeltaCapture deltaCapture() const;      // Config::deltaCapture (any thread)
    bool capturesUncompiledBodies() const;  // the delta capture All and productionActive(): records keep holders
    UCBRegistry& registry();
    // The key the UCB lane recorded for the UCB, if any; it takes only the registry's lock, a leaf. A lane names a body by
    // key through it, so no lane includes the UCB lane's headers.
    std::optional<BodyKey> bodyKeyOf(const UnlinkedCodeBlock&) const;
    uint64_t bodyVersion(const BodyKey&);
    BodyLookup openBody(const BodyKey&);
    SectionLookup readBodySection(const BodyKey&, SectionKind);   // one section, checksummed without the body's others
    void raiseInvalidMaterial(ASCIILiteral step, String detail);                  // step is a full name, such as "ucb.identity"
    void raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail);
    void raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail);
    // The Image lane's table of resolved support targets, which its first image preparation in the VM creates with the
    // lane's own deleter, so neither this header nor the destructor's file names its type.
    using SupportTargetTableHolder = std::unique_ptr<SupportTargetTable, void (*)(SupportTargetTable*)>;
    SupportTargetTable* supportTargetTable();     // null before the VM's first image preparation
    void setSupportTargetTable(SupportTargetTableHolder&&);
#if ENABLE(JITCACHE_TWINS)
    TwinReportSink* twinReportSink();  // non-null while a twin report is open
    Ref<ProducerBudget> twinBudget();  // ProducerBudget::createUnlimited(), for twin compiles
    // Tests: while set, bodyVersion, openBody and readBodySection answer from these alone; token answers index tokens.
    void setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open);
    void clearBodyLookupForTesting();
#endif

    // The integrator's own state.
    Role role() const;
    bool producing() const;           // Producer or ConsumerProducer
    bool activityOn() const;
    ProducerContext* producerContextIfActive();   // any thread
    void releaseEndedProductionMemory();          // VM thread; nothing while production is active or once it is released
    OpenedArtifact* artifact();       // null after a start fault
    BenchReport* benchReport();       // null unless Config::benchReportPath was set
    ~VMState();
};

}
```

Task 0 writes the class as declared here, task 2 adds its private part and task 7 the accessors later tasks use (section 17). The lanes reach it with `vm.jitCacheState()`, which `VM.h` declares inline with `setJITCacheState` (section 14, M3). `start` stores the pointer once; `didFinalizeHeap` stores null and then destroys the state (section 4.6). Every reader runs either on the VM thread, which wrote the pointer, or on a JIT worker through `producerContext` (section 4.4), whose acquire load sees a fully constructed state.

The state owns these parts:

| part | held as | created by | released by |
|---|---|---|---|
| the role, the config, the start outcome | values | the constructor | |
| activity, production and the debugger flag | `std::atomic<bool>` | the constructor | |
| the first fault of each switch, the progress counters | values the VM thread writes | the fault members and the glue | |
| the producer budget and context (producing roles) | `RefPtr<ProducerBudget>`, `ProducerContext` | the constructor | the destructor |
| the bench report | `std::unique_ptr<BenchReport>` | `start`'s step 3, when `Config::benchReportPath` is set | `finishHostRun` and `willDestroyVM` flush it; the destructor |
| the twin report and the twin budget (twins builds) | `std::unique_ptr<TwinReport>`, `Ref<ProducerBudget>` | the constructor | `willDestroyVM` closes the report |
| the UCB lane's `UCBRegistry` | a value | the constructor | the destructor, after every UCB and UFE destructor has run (section 4.6) |
| the producer lock (producing roles) | `std::unique_ptr<ProducerLock>` | `start` | the destructor, which releases the lock |
| the `OpenedArtifact` the VMs of the process share (container sub-SPEC section 5) | `RefPtr<OpenedArtifact>` | `start` | the destructor |
| the body-lookup override (twins builds) | two `Function`s | `setBodyLookupForTesting` | `clearBodyLookupForTesting`; the destructor |
| the writer (producing roles) | `std::unique_ptr<ArtifactWriter>` | `start` | its staging buffer at the end of production; the writer with the state |
| the kept summaries and the index-entry charge (sections 4.4 and 8.3) | `std::unique_ptr<KeptSummaries, void (*)(KeptSummaries*)>`, with the deleter its creator supplies, and a `size_t` | the capture glue, at the first kept entry | the end of production |
| the count of captures in progress (`ASSERT_ENABLED` builds) | an integer the VM thread writes | the constructor | (section 8.7) |
| the image twin-check state (twins builds; harness sub-SPEC section 3) | `std::unique_ptr<ImageTwinCheckState, void (*)(ImageTwinCheckState*)>` | install step 18, at the VM's first stash | `willDestroyVM` |
| the Image lane's support-target table | `SupportTargetTableHolder` | the Image lane's first image preparation in the VM | the destructor |

The destructor destroys the writer before the producer lock, the opened artifact and the budget it refers to. `JITCacheVMState.h` forward-declares the integrator types the state holds through pointers, so a lane that includes it compiles against none of their headers. `JITCacheVMState.cpp` defines the state's members, its constructor and destructor included, and `JITCacheAPI.cpp` defines `start` and the teardown hooks; the constructor, the destructor and both hooks land in task 7 (section 17). ([history](SPEC-integrator-history.md#the-state-is-created-and-destroyed-in-task-7))

### 4.2 Activity and production

Two switches go from on to off and never back:

| switch | starts | goes off when | effect |
|---|---|---|---|
| activity | on, except after a start fault | invalid material, an executable-allocation fault, a debugger attach | no request point records, imports or attaches (UCB `tracksKeys`, `importsEnabled`); no install; no recording, capture or `delta` |
| production | active for a producing role | a recording fault, or activity going off | no recording (`producerContext` returns null), no capture, no direct-eval context digest at a UCB record (UCB `productionActive`; [history](SPEC-integrator-history.md#productionactive-belongs-to-the-lanes-interface)), and `delta` reports the fault |

Both are atomics, because JIT workers read production through `producerContext` and a debugger may attach from another thread. The first fault that turns each switch off is recorded on the VM thread, except a debugger attach, which only sets an atomic flag that `status` reads. After a recording fault a ConsumerProducer goes on importing.

Production memory (kept summaries, the writer's staging buffer and the charge for the index entries the writer added, section 4.4) is released by `releaseEndedProductionMemory()`, which each glue entry on the VM thread calls first: the install function before its step 1 (section 7.2), the finalize capture at its step 2 (section 8.5) and `delta` once its preconditions pass (section 3.4). The call does nothing while production is active or once the memory is released, so it costs a flag test; teardown releases whatever remains ([history](SPEC-integrator-history.md#production-memory-is-released-by-task-7s-code)). Nothing is released inside a fault entry point, so those of section 4.5 free nothing and wait for nothing.

### 4.3 Strict

Both modes check the artifact's integrity: `start` checks the header in full (container sub-SPEC section 3.2), and every read of a body runs the integrity checks of container sub-SPEC section 4.5. Strict adds that section's structure checks, which cover the rest of the container's framing, and the integrator passes the session's `strict` to every lane call that takes it: Image R-INT-3, the CB and ICs calls of sections 7.2 and 8.4, and the summary readers `scoreSections` calls (section 8.2). THREAD Session leaves to the SPECs which assumptions strict covers. The integrator adds no assumption check of its own, because its own mechanisms guarantee in both modes the three things it assumes. A committed body holds the lanes' bytes: the writer's reread validates every committed body in full against the checksums of the bytes it streamed. A section source streams exactly the bytes its size promises: the writer counts them (`writer.section`); both are in container sub-SPEC section 8.2. A kept summary describes its body: its score comes from what the builders returned for the bytes they wrote (section 8.4, step 6), and the producer lock keeps every other writer out (section 8.3). Whether a lane's builder produces bytes its own readers accept is that lane's assumption, and its SPEC decides whether strict covers it (SPEC-cb.md SC1 does for `cb.state` and `cb.summary`); the glue reads none of a capture's own bytes back. ([history](SPEC-integrator-history.md#strict-integrity-in-both-modes-structure-under-strict))

### 4.4 Producer context and budget

`ProducerBudget.h` declares what SPEC-image.md R-INT-1 and R-INT-2, SPEC-ucb.md R-INT-7, SPEC-cb.md R-INT-2 and the integrator charge against:

```cpp
namespace JSC::JITCache {

class ProducerBudget final : public ThreadSafeRefCounted<ProducerBudget> {
    WTF_MAKE_TZONE_ALLOCATED(ProducerBudget);
public:
    static Ref<ProducerBudget> create(size_t limitBytes);
    static Ref<ProducerBudget> createUnlimited();   // twin compiles; never refuses
    [[nodiscard]] bool tryCharge(size_t bytes);     // any thread
    void release(size_t bytes);                     // any thread
    bool hasRefused() const;                        // any thread
    void refuseFurtherCharges();                    // VM thread, when production ends
    size_t limitBytes() const;
    size_t chargedBytes() const;
    size_t peakBytes() const;

private:
    ProducerBudget(size_t limitBytes, bool isUnlimited);

    const size_t m_limitBytes;                      // SIZE_MAX for an unlimited budget
    const bool m_isUnlimited;                       // createUnlimited: no charge is refused, an overflowing one included
    std::atomic<size_t> m_chargedBytes { 0 };
    std::atomic<size_t> m_peakBytes { 0 };
    std::atomic<bool> m_hasRefused { false };
};

class ProducerContext {
public:
    explicit ProducerContext(Ref<ProducerBudget>&&);
    Ref<ProducerBudget> budget() const;

private:
    const Ref<ProducerBudget> m_budget;
};

ProducerContext* producerContext(VM&);   // any thread, no lock

}
```

`tryCharge` returns false at once when the budget has refused before. Otherwise it adds `bytes` to the charged total with a compare-and-swap loop, and a sum that overflows or exceeds the limit sets the refused flag and returns false without changing the total; a success raises the peak. `release` subtracts and asserts that the total does not go below zero. The refused flag is sticky (THREAD Failures). Every member uses atomics, so a charge, a release and a read may run on any thread, and a record destroyed after its VM's session releases into a budget it still references.

`producerContext(vm)` loads `vm.jitCacheState()` with acquire order and returns `state->producerContextIfActive()`: the state's one `ProducerContext` while the role produces, activity is on and production has not ended, and null otherwise. The object lives until the state is destroyed, which happens after `cancelAllPlansForVM` has stopped every compilation of the VM (N5, N6), so a JIT worker that obtained it may use it for its whole compilation.

Whether a baseline compilation records is decided when its plan is built (SPEC-image.md R-INT-12 and section 4.1). `BaselineJITPlan` gains `bool m_jitCacheRecordsImage { false }` and `bool jitCacheRecordsImage() const`. Its constructor, which both callers (`jitCompileAndSetHeuristics` and `JIT::compileSync`) run on the VM thread, sets the member after its native calls (N3) to `JITCache::shouldRecordBaselineImage(vm, *codeBlock)`, which `JITCacheGlue.h` declares and which holds when `producerContext(vm)` returns a context and `vm.jitCacheState()->bodyKeyOf(*codeBlock->unlinkedCodeBlock())` returns a key; the lookup takes only the registry's leaf lock (SPEC-ucb.md section 6.3). ([history](SPEC-integrator-history.md#which-compilations-record-is-decided-at-plan-construction))

The integrator charges, before allocating: the ICs section buffer of each capture (SPEC-ics.md R-INT-2), the writer's staging buffer (container sub-SPEC section 8.1), the buffer of a full index block a commit writes and the plan and index buffers of a compaction (container sub-SPEC sections 8.2 and 8.4), the kept summaries (section 8.3), `delta`'s candidate table (section 8.6), each index entry the writer adds for a key the index lacks (section 8.4, step 8), and in twins builds the copy `rewriteSection` commits (container sub-SPEC section 8.3).

A hash table the integrator owns for production is charged eight bucket sizes with its first entry and six more per entry; each entry's six are released when the entry goes, and the eight when the table goes. That bounds WTF's storage for a table that only grows, rehashes included: `HashTableSizePolicy` (`wtf/HashTable.h`) starts a table at `HashTraits::minimumTableSize`, eight buckets, and doubles it once it is three-quarters full up to 1024 buckets and half full above, so a table of k entries holds at most max(8, 4k) buckets, and at most 6k while a rehash holds the old table beside the new one.

An index entry the writer adds is charged six of the index's bucket sizes (`sizeof(PackIndex::KeyValuePairType)`), its share of a table that grows the same way. The charge lasts until production ends, when the state releases it with the rest of its production memory (section 4.2); the entry itself stays in the index, which the VMs of the process share (container sub-SPEC section 5.1), for the VMs that import. ([history](SPEC-integrator-history.md#charging-hash-tables-and-index-entries))

These allocations are not production memory and go uncharged:

- the parent-key registry and the digests the UCB lane keeps for identity (each TDZ environment's, SPEC-ucb.md section 3.4), which serve every role (THREAD Session);
- the index's other entries, which a reader creates when it reads the index at `start` or applies the index blocks of a moved root (container sub-SPEC section 6): they hold the bodies the pack already held and what other VMs and processes commit, which every importing VM needs whoever produces;
- the pack's mapping (container sub-SPEC section 5.2), which the scoring read borrows (container sub-SPEC section 7.3) and which holds page-cache pages every role reads, and in twins builds the body `rewriteSection` reads through the store's `open` (container sub-SPEC section 8.3), which only test builds hold and which goes before the rewrite's commit starts;
- the bench report's buffer (harness sub-SPEC section 9.1), which exists only in bench runs, whatever the role, and holds measurements.

The registry's and the index's allocations crash when memory runs out, as any native allocation does. Everything else the integrator allocates for production is charged (II4).

### 4.5 Faults

`JITCacheFaults.h` declares the entry points native code and the lanes call:

```cpp
namespace JSC::JITCache {

enum class ExecutableAllocationSite : uint8_t {
    BaselinePlan, DFGPlan, FTLPlan, InlineCacheHandler, MathICSnippet, JITCacheImage,
};

// VM thread. Callable with any JSC lock held, CodeBlock::m_lock included. Allocates nothing, frees nothing, takes no
// lock and waits for no thread. A no-op for a VM start never configured.
void didFailExecutableAllocation(VM&, ExecutableAllocationSite);

// Any thread. Atomic stores only.
void didAttachDebugger(VM&);

}
```

THREAD Execution and Failures fix this entry point's contract and its exhaustive list of callers. Each caller passes its site, which names the fault's step: ([history](SPEC-integrator-history.md#the-fault-entry-point-names-its-site))

| site | caller | step |
|---|---|---|
| `BaselinePlan`, `DFGPlan`, `FTLPlan` | the integrator's plan sites (section 9) | `exec-alloc.baseline-plan`, `exec-alloc.dfg-plan`, `exec-alloc.ftl-plan` |
| `InlineCacheHandler` | the ICs lane, at the three `didFailToAllocate()` branches of `InlineCacheCompiler` (SPEC-ics.md E5) | `exec-alloc.ic-handler` |
| `MathICSnippet` | the Image lane, at both allocation failures of MathIC regeneration in every tier, through `MathICRegeneration::didFailToAllocate` (SPEC-image.md section 6.2) | `exec-alloc.mathic-snippet` |
| `JITCacheImage` | the install glue, at the image's own allocation (section 7.2, step 7) | `exec-alloc.jitcache-image` |

The entry point turns activity off and records `FaultReport { ExecutableMemory, "", step, {} }` as the first activity fault when none is recorded yet. Its writes are plain VM-thread stores and an atomic store, so the call is safe inside an IC slow operation that holds the CB's lock through a `GCSafeConcurrentJSLocker` (SPEC-ics.md R-INT-6) and inside a plan's finalization (SPEC-ucb.md R-INT-11).

`Debugger::attach` calls `JITCache::didAttachDebugger(m_vm)` right after `setShouldBuildPCToCodeOriginMapping` (N10). `didAttachDebugger` sets the state's debugger flag and turns activity off, with atomic stores; `status` reports `FaultReport { DebuggerAttached, "", "debugger.attach", {} }` when no earlier activity fault was recorded.

The `VMState` members of section 4.1 raise the other classes, on the VM thread:

- `raiseInvalidMaterial` turns activity off (which ends production) and records the report. The UCB lane calls the one-step form with its full names (`ucb.identity` to `ucb.supplied-digest`, and `ucb.analysis`), and the capture glue calls it with `ucb.analysis` for `SavedAnalysisInvalid`, with a diagnostic in the form of SPEC-ucb.md section 7.3 whose words say that the saved body's `ucb.analysis` is no analysis the parse can produce, followed by the `UCBCaptureError`'s `detail`, while for `SavedAnalysisUnusable` it raises nothing, since `readBodySection` has raised the fault already (section 8.4). The integrator calls the part form for the other lanes, with part `image` and check `description(ImageCheck)` (SPEC-image.md section 12.1), part `cb`, check `description(CBCheck)` and detail `faultDetail(CBCheck)` with the body's key after it (SPEC-cb.md R-INT-8, R-ALL-6), part `ics`, check `description(ICs::Check)` and the site index in the detail (SPEC-ics.md section 4.4), and for itself with the full names that sections 6.2 and 8.3 raise: `container.io` with `strerror` of the error in the detail, or the name of the failed check in container sub-SPEC section 4.5.
- `raiseRecordingFault` ends production, records the report and calls `budget->refuseFurtherCharges()`, so recorders still running on JIT workers fail their next charge and leave their records incomplete. Every refusal of a production charge is raised as `budget.limit`, the budget's own fault, wherever it lands: when a lane's capture call fails and `budget->hasRefused()` holds afterwards, the glue raises `budget.limit` with the lane's check in the detail. Otherwise the capture glue maps `UCBCaptureFailure::BudgetExceeded` and `StrictCheckFailed` to part `ucb` with checks `capture-budget` and `capture-strict`, `CaptureOutcome::RecordingFault` to part `image` with `description(check)`, `CaptureOutcome::ChargeRefused` to `budget.limit` with no image fault beside it (SPEC-image.md R-INT-6), a `CBFault` of kind `RecordingFault` to part `cb` with `description(check)` and detail `faultDetail(check)`, and a `CaptureError` to part `ics` with `description(ICs::CaptureCheck)` and the site index (SPEC-ics.md section 5.6). ([history](SPEC-integrator-history.md#every-refused-production-charge-is-budgetlimit))
- A budget refusal made on a JIT worker cannot reach the VM thread when it happens, so the capture glue tests `budget->hasRefused()` at the start of every capture and every `delta` and raises `budget.limit` then (THREAD Failures). A refusal made on the VM thread inside a capture fails that capture, which raises the fault at once.

### 4.6 Teardown

`JITCacheGlue.h` declares two hooks that `VM::~VM` calls (section 14, M4), and `JITCacheAPI.cpp` defines them beside `start`, which creates the state:

- `willDestroyVM(VM&)`, right after the `cancelAllPlansForVM` block and before `m_perBytecodeProfiler` is cleared. GC is deferred for good and no compilation of the VM runs (N5, N6). It destroys the image twin-check state (harness sub-SPEC section 3), closes the twin report, flushes the bench report, ends production without recording a fault (no implicit `delta`, THREAD Failures) and calls `releaseEndedProductionMemory()` (section 4.2), which frees the production memory and releases its charges, the index entries' included.
- `didFinalizeHeap(VM&)`, right after `heap.shutDown()`. Every UCB and UFE destructor has run by then, in the last finalization that ends `Heap::shutDown` (N5; SPEC-ucb.md section 6.3). In twins builds it first erases the VM's retired body-event totals, for every VM whether or not `start` configured it, since the UCBs that finalization destroyed retired their counts into them (harness sub-SPEC section 10.3). It stores null into the VM's pointer and destroys the state, which destroys the registry, destroys the `ProducerLock`, which releases the lock, and drops the reference to the opened artifact, which is destroyed with the last VM of the process that holds it.

## 5. Options and process facts

### 5.1 The fixed-option check

[options.md](../options.md) lists the must-match and fixed options of THREAD Storage with their types (N12). The must-match options' effective values go into the header (container sub-SPEC section 3.1), and `start` walks the fixed table in its order. ([history](SPEC-integrator-history.md#the-option-table-lives-in-optionsmd))

`JITCacheOptions.cpp` writes the table as one macro list, `JITCACHE_FOR_EACH_FIXED_OPTION(v)`, with entries `v(name, Type, requiredValue)`, and expands it into a static array of rows: the name as an `ASCIILiteral`, a function that reads `Options::name()`, and the required value. Because each row reads `Options::name()`, a renamed or removed option fails the build. `checkFixedOptions()` walks the rows in table order and returns the first row whose effective value differs: `Bool`, `Unsigned` and `Int32` compare exactly, `Double` compares with `==` (both sides are the same literal or the value `Options` computes from it), and an `OptionString` required to be null passes when `Options::name()` is `nullptr`. The required values are those of options.md for this pin. JSC keeps no defaults once options freeze (THREAD Storage), so test T-OPT (section 15.1) checks, in a process started without option overrides, that every row's required value equals its effective value; a WebKit bump that moves a default fails that test.

### 5.2 Process facts

`JITCachePlatform.cpp` computes, once per process:

- Build IDs. The main executable is the first object `dl_iterate_phdr` lists. The engine object is the one whose loaded segments contain the address of `JITCache::codeSymbolAnchor`, which `base/CodeSymbolAnchor.h` declares under `ENABLE(JIT)` and the Image lane defines (SPEC-image.md section 3.5 and R-INT-8); a build without `ENABLE(JIT)` has no anchor and takes the address of `processFacts`, which lies in the same object. For each, the build ID is the descriptor of the first `NT_GNU_BUILD_ID` note whose owner is `GNU` in one of its `PT_NOTE` segments, 1 to 64 bytes long. The header records the main executable's ID, and the engine object's when it is a different object. A missing ID in either object makes `start` reject at `start.build-id`. In twins builds `JITCachePlatform.h` also declares `void removeMainBuildIDForTesting(bool)`; while it is set, the process facts report the main executable without an ID, which is how T-START reaches `start.build-id`.
- The values of the must-match options (options.md), read with `Options::evalMode()`, `Options::useExplicitResourceManagement()` and `Options::useImportDefer()`.
- The CPU feature vector, a `uint64_t` whose bit i is the answer of predicate i (N11), with every other bit zero:
  - x86_64: 0 `supportsSSE3`, 1 `supportsSupplementalSSE3`, 2 `supportsSSE4_1`, 3 `supportsFloatingPointRounding`, 4 `supportsCountPopulation`, 5 `supportsAVX`, 6 `supportsAVX2`, 7 LZCNT, 8 BMI1, 9 `supportsFloat16`. Bits 7 and 8 are read as `MacroAssemblerX86_64::s_lzcntCheckState == CPUIDCheckState::Set` and `s_bmi1CheckState == CPUIDCheckState::Set` after `MacroAssemblerX86_64::collectCPUFeatures()`, which is what the protected `supportsLZCNT` and `supportsBMI1` return.
  - ARM64: 0 `supportsFloatingPointRounding`, 1 `supportsCountPopulation`, 2 `supportsFloat16`, 3 `supportsDotProd`, 4 `supportsLSE`, 5 `supportsDoubleToInt32ConversionUsingJavaScriptSemantics`, 6 `supportsRoundFloatToIntegerFloat`, 7 `supportsSHA3`.

`JITCachePlatform.h`, which task 0 writes, declares what tasks 3, 4 and 7 share:

```cpp
namespace JSC::JITCache {

struct BuildID {
    std::array<uint8_t, 64> bytes { };   // the ID in the first size bytes, zero after
    uint8_t size { 0 };                  // 0: the object has no ID
};

struct ProcessFacts {
    BuildID mainExecutable;
    std::optional<BuildID> engineObject;   // set when the engine is an object of its own
    std::array<bool, 3> mustMatch { };     // by the option's index in the header's option records
    uint64_t cpuFeatures { 0 };            // bit i holds the answer of the target's i-th assembler predicate
};

// Any thread. Computed once, at the first call, which start makes after the first VM froze the options; while
// removeMainBuildIDForTesting is set, the copy it returns has a main executable without an ID.
ProcessFacts processFacts();
// Any thread. The running CRC-32C state, without the initial value or the final XOR.
uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>);
#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool);
uint32_t crc32cExtendWithTableForTesting(uint32_t state, std::span<const uint8_t>);   // the table path alone
#endif

}
```

`JITCachePlatform.cpp` reads each object's ID through a function with external linkage that no header declares; T-BUILDID declares it itself in `tests/store/PlatformTests.cpp`, under `OS(LINUX)`, and reads crafted objects through it:

```cpp
#if OS(LINUX)
namespace JSC::JITCache::PlatformInternal {
BuildID buildIDOfObject(const struct dl_phdr_info&);   // one loaded object's build ID: its first GNU build-ID note, or none
}
#endif
```

The header's byte layout, its creation and its comparison are in the container sub-SPEC, section 3.

## 6. Bodies: the interface the lanes use

### 6.1 Sections

`ValidatedBody.h` declares the section kinds, which the lanes name when they ask a body for a section:

```cpp
enum class SectionKind : uint8_t {
    UCBIdentity, UCBCore, UCBFeedback, UCBAnalysis,
    ImageBaseline, BakedFactsBaseline, ImageTwinsBaseline,
    CBStateBaseline, CBSummaryBaseline,
    ICsBaseline,
};
constexpr unsigned numberOfSectionKinds = 10;
```

| kind | the lane's name | type id | tier | in a body of highest tier 1 |
|---|---|---|---|---|
| `UCBIdentity` | `ucb.identity` | `0x0101` | 0 | required |
| `UCBCore` | `ucb.core` | `0x0102` | 0 | required |
| `UCBFeedback` | `ucb.feedback` | `0x0103` | 0 | required |
| `UCBAnalysis` | `ucb.analysis` | `0x0104` | 0 | optional at every tier: `isSectionRequired` never requires it, and only a body whose key's `identityKind()` is `Module` may hold it (SPEC-ucb.analysis.md R-INT-A1) |
| `ImageBaseline` | `image.baseline` | `0x0201` | 1 | required |
| `BakedFactsBaseline` | `baked-facts.baseline` | `0x0202` | 1 | required |
| `ImageTwinsBaseline` | `image-twins.baseline` | `0x0203` | 1 | required in `ENABLE(JITCACHE_TWINS)` builds, forbidden elsewhere |
| `CBStateBaseline` | `cb.state` | `0x0301` | 1 | required |
| `CBSummaryBaseline` | `cb.summary` | `0x0302` | 1 | required |
| `ICsBaseline` | `ICsBaseline` | `0x0401` | 1 | required |

The high byte of a type id names the lane (1 UCB, 2 Image, 3 CB, 4 ICs) and the low byte the section family; the tier byte of the directory entry separates tiers, 0 for the sections every tier shares. A later DFG capture adds entries such as (`0x0201`, tier 2) and (`0x0401`, tier 2) under the same layout version, which is how the format admits more than one tier per body (THREAD's opening). Every section's span starts 8-byte aligned in memory and covers exactly the section, because sections start at 8-aligned offsets in their body (container sub-SPEC section 4.3), a body starts 8-aligned in the pack (an 8-aligned record offset plus the 64-byte record header) and the pack is mapped from a page boundary (UCB R-INT-3, Image R-INT-5, CB R-INT-1); the ICs lane needs no alignment (ICs R-INT-1). The writer writes every section the body's highest tier requires, plus `ucb.analysis` when the UCB lane built one, and strict validation checks that the required ones are present and that no other is but that one, in a module's body (container sub-SPEC, check B7); normal mode trusts it (THREAD Session). This satisfies UCB R-INT-4, SPEC-ucb.analysis.md R-INT-A1, Image R-INT-5, ICs R-INT-1 and R-INT-4, and the CB lane's need for its two section ids.

### 6.2 Lookup

`ValidatedBody.h` declares:

```cpp
class ValidatedBody final : public ThreadSafeRefCounted<ValidatedBody> {
    WTF_MAKE_TZONE_ALLOCATED(ValidatedBody);
public:
    const BodyKey& key() const;
    uint64_t version() const;                               // the envelope's commit identifier, which the writer never makes 0
    uint8_t highestTier() const;
    uint32_t llintThreshold() const;                        // the envelope's L; 0 for a test body
    uint32_t counterProgress() const;                       // the envelope's P; 0 for a test body
    std::span<const uint8_t> section(SectionKind) const;    // empty when absent; starts 8-byte aligned
    size_t fileSize() const;                                // the body's extent in the pack, or a test body's buffer size
    ~ValidatedBody();                                       // drops the mapping or frees a test body's buffer; any thread that holds no JITCache lock

#if ENABLE(JITCACHE_TWINS)
    // A body no container check has seen, for the parts' own tests: the sections are copied into one buffer, in the
    // order given (kinds strictly increasing), each at an 8-byte-aligned address; highestTier() is the highest tier of
    // the given kinds. onDestroy runs first in the destructor, on the destroying thread. version is not 0.
    struct TestSection { SectionKind kind; std::span<const uint8_t> bytes; };
    static Ref<ValidatedBody> createForTesting(const BodyKey&, uint64_t version, std::span<const TestSection>,
        Function<void()>&& onDestroy = { });
#endif

private:
    friend class OpenedArtifact;   // open builds a body from the bytes validateBody accepted
    using SectionSpans = std::array<std::span<const uint8_t>, numberOfSectionKinds>;   // by SectionKind; empty when absent
    // open fills the version, the tier, L, P and the spans from validateBody's BodyLayout, so this header needs nothing
    // from JITCacheContainer.h. A test body takes the buffer, whose address moving does not change, so the spans into it
    // stay valid; a body from the pack takes a reference to the mapping and the spans into it.
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress,
        MallocSpan<uint8_t>&& bytes, const SectionSpans&);
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress,
        Ref<MappedPack>&&, std::span<const uint8_t> bytes, const SectionSpans&);

    const BodyKey m_key;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const uint32_t m_llintThreshold;            // the envelope's L; 0 for a test body
    const uint32_t m_counterProgress;           // the envelope's P; 0 for a test body
    const MallocSpan<uint8_t> m_buffer;         // createForTesting's copy of its sections; fastMalloc aligns it to 8 bytes or more
    const RefPtr<MappedPack> m_pack;            // the mapping the body borrows from; null for a test body
    const std::span<const uint8_t> m_bytes;     // the body's extent in m_pack's mapping, 8-aligned in a page-aligned mapping, or m_buffer
    const SectionSpans m_sections;              // spans into m_bytes
#if ENABLE(JITCACHE_TWINS)
    Function<void()> m_onDestroy;
#endif
};

class BodyLookup {
public:
    enum class Kind : uint8_t { Missing, Unusable, Found };
    static BodyLookup missing();
    static BodyLookup unusable();
    static BodyLookup found(Ref<ValidatedBody>&&);
    Kind kind() const;
    RefPtr<ValidatedBody> body() const;   // non-null exactly for Found

private:
    BodyLookup(Kind, RefPtr<ValidatedBody>&&);

    Kind m_kind;
    RefPtr<ValidatedBody> m_body;
};

// One section of a key's newest body, read without checksumming the body's other sections. Movable.
class SectionLookup {
public:
    enum class Kind : uint8_t { Absent, Unusable, Found };
    Kind kind() const;
    std::span<const uint8_t> bytes() const LIFETIME_BOUND;   // Found: the section, empty when the body lacks it; 8-byte aligned
    SectionLookup(SectionLookup&&);
    SectionLookup& operator=(SectionLookup&&);
    ~SectionLookup();   // drops what keeps the bytes; any thread that holds no JITCache lock

private:
    friend class VMState;
    SectionLookup(Kind, RefPtr<MappedPack>&&, RefPtr<ValidatedBody>&&, std::span<const uint8_t>);

    Kind m_kind;
    RefPtr<MappedPack> m_pack;            // the store's mapping the bytes lie in
    RefPtr<ValidatedBody> m_body;         // or the test override's body they lie in
    std::span<const uint8_t> m_bytes;
};
```

A `ValidatedBody` the store opens borrows its bytes from the pack's shared read-only mapping (container sub-SPEC section 5.2), which it keeps alive through a reference, after they passed the container checks its VM runs, the integrity checks always and the structure checks under strict (container sub-SPEC sections 4.5 and 7.2). The pack never overwrites a record, and a compaction writes a new file, so the bytes stay readable and unchanged while the body lives, whatever happens to the path (THREAD Storage). A pending import pins it (SPEC-ucb.md section 6.4). Its constructors are private: the store builds a body from checked bytes of a mapping, and a test builds one with `createForTesting`, whose buffer no container check reads, since such a body exists to hand a part's own checks crafted bytes (SPEC-ucb.md U4 and U8).

`VMState::bodyVersion(key)` and `VMState::openBody(key)` run on the VM thread, allocate no JSC cell, take only the artifact's index lock (container sub-SPEC section 6.4), and answer UCB R-INT-3. Each returns 0, or `Unusable`, when activity is off; then the override's answer while a test has set one (below); and otherwise the opened artifact's:

- `bodyVersion` returns `OpenedArtifact::token(key)` (container sub-SPEC section 7.1): the commit identifier of the key's newest index entry, or 0 when the index lists no body for the key. The token first follows the root (container sub-SPEC section 6): it loads the root, and only when the root moved does it check the new valid end against the pack file's size with one `fstatat` and apply the new index blocks, so an unchanged root costs one acquire load and no system call unless a compaction or a deletion marked the mapped pack retired. It raises nothing: a pack that failed its checks is reported by the next open. ([history](SPEC-integrator-history.md#tokens-are-commit-identifiers))
- `openBody` asks `OpenedArtifact::open(key, mode)`, with `ValidationMode::Full` exactly when strict is on, which follows the root, borrows the newest body for the key from the mapping and checks it (container sub-SPEC sections 4.5 and 7.2); a key the index lacks is `Absent` without a system call. `Found` gives `Found` with the body. `Absent` gives `Missing`. `Unavailable`, which only the store's fault hook of twins builds produces (`EMFILE`, `ENFILE` or `ENOMEM`), gives `Missing` and counts in `Progress::transientOpenFailures`, since the artifact is not at fault. `Invalid` raises invalid material at its check (`container.io` with `strerror` of the error in the detail, a body check's name, or `container.pack` for a pack that failed its own checks or an index entry that does not repeat its envelope) and gives `Unusable`. A call whose open reached a body (`BodyOpen::bodyBytes` nonzero: `Found`, or `Invalid` at a body check) counts in `Progress::bodyOpens` and writes the `open` event with the body's size (harness sub-SPEC section 9.2).
- `VMState::readBodySection(key, kind)` answers SPEC-ucb.analysis.md R-INT-A2, which reads `ucb.analysis` before the module's request opens the whole body: it checksums that one section and leaves the others to the open that follows. It returns `Unusable` when activity is off, answers from the override's `open` while a test has set one (below), `Missing` as `Absent`, `Unusable` as `Unusable` and a body as `Found` with its section, and otherwise asks `OpenedArtifact::readSection(key, kind, mode)` (container sub-SPEC section 7.6), in the mode `openBody` uses. `Absent` gives `Absent`, `Invalid` raises invalid material at its check as `openBody` does and gives `Unusable`, and `Found` gives `Found` with the section's span, which the lookup's reference to the mapping keeps readable. It runs on the VM thread, allocates no cell, takes only the index lock, and counts in neither `bodyOpens` nor the `open` event, since the UCB lane's `module-analysis` event times it (harness sub-SPEC section 9.2).

The token and `ValidatedBody::version()` are both commit identifiers, the body version THREAD Maintenance names: the token is the one the newest index entry lists, and `version()` the one the opened body's envelope stamps, which Full validation checks against its entry. They differ only when a commit lands between a `bodyVersion` and the `openBody` that follows it, so the UCB lane still compares tokens only with tokens and versions only with versions (UCB R-INT-3). A miss it stamps with a token is tried again once the key's newest entry changes (SPEC-ucb.md section 7.3.2). Compaction copies records with their commit identifiers, so tokens survive it.

For the UCB lane a body it cannot read now is as good as none. The capture glue must tell the two apart, so it reads saved scores through a call of its own (section 8.3).

In twins builds, `setBodyLookupForTesting(token, open)` makes `bodyVersion`, `openBody` and `readBodySection` answer from the two functions alone once the activity test has passed, without the store, the index or a progress counter, until `clearBodyLookupForTesting()`. The first function answers index tokens and the second gives bodies whose `version()` is their commit identifier, so a test keeps the two apart as the store does and can make them disagree as a lagging index does. A part's test serves crafted bodies this way (SPEC-ucb.md U8, SPEC-ucb.analysis.md UA7). The override changes neither the VM's role nor its strictness, which `start` fixed, and invalid material raised through it turns activity off for good like any other, so a test that needs several roles, both strict values or activity on again runs once per configuration, as the runner's sequences do (harness sub-SPEC section 7). ([history](SPEC-integrator-history.md#test-bodies-and-a-lookup-override))

### 6.3 Commit

The capture glue hands an accepted capture to the writer as an ordered list of section sources. Each source is a section kind, a size, and either the section's bytes in memory or a function through which an object streams them, so the writer sees no lane type. `ArtifactWriter.h` declares:

```cpp
using SectionSink = ScopedLambda<bool(std::span<const uint8_t>)>;   // false once the commit has failed; the source then stops

struct SectionSource {
    SectionKind kind;
    uint64_t size;                                                       // the bytes the source promises
    std::span<const uint8_t> bytes;                                      // a section held in memory: size == bytes.size()
    bool (*stream)(const void* object, const SectionSink&) { nullptr };  // or a section its object streams
    const void* object { nullptr };

    static SectionSource inMemory(SectionKind, std::span<const uint8_t>);
    static SectionSource streamed(SectionKind, uint64_t size, bool (*)(const void*, const SectionSink&), const void* object);
};

class CommitSections {   // copies its sources; borrows the bytes and objects they name, which outlive the commit
public:
    explicit CommitSections(std::span<const SectionSource>);   // at most numberOfSectionKinds, kinds strictly increasing; copied without allocating
    std::span<const SectionSource> sources() const LIFETIME_BOUND;
private:
    std::array<SectionSource, numberOfSectionKinds> m_sources;
    unsigned m_count { 0 };
};

struct CommitStamp {
    BodyKey key;
    uint32_t llintThreshold;     // L: envelopeLLIntThreshold for a body with an image, carriedLLIntPoints for one without
    uint32_t counterProgress;    // P: the CB lane's counterProgress; 0 when the counter does not travel or there is no image
    uint8_t highestTier;         // 0 without an image, 1 with one
};
struct CommitResult { uint64_t version; uint64_t fileSize; };   // fileSize: the body's size
struct CommitTiming {   // thread CPU nanoseconds of a commit's parts, for the capture event
    std::array<uint64_t, numberOfSectionKinds> streamNanoseconds { };   // by SectionKind; 0 for a kind the commit lacks
    uint64_t rereadNanoseconds { 0 };
    uint64_t publishNanoseconds { 0 };   // the index block, the root's store and the index update
};
struct CommitFailure { ASCIILiteral check; String detail; };   // budget.limit, a writer.* step, or writer.rewrite

struct CompactionResult {
    uint64_t bodies { 0 };                // in the new pack
    uint64_t packBytes { 0 };             // the new pack's valid end
    uint64_t replacedPackBytes { 0 };     // the valid end of the pack it replaced
    uint64_t nanoseconds { 0 };           // thread CPU time, when timed
    Vector<std::pair<BodyKey, ASCIILiteral>> droppedBodies;   // maintenance: each damaged body left out and its check
};
struct CompactionFailure {
    enum class Kind : uint8_t { Storage, Budget, Damaged };   // a compaction.* step, a refused charge, a body that failed its checks
    Kind kind;
    ASCIILiteral check;
    String detail;
};

class ArtifactWriter {
    WTF_MAKE_NONCOPYABLE(ArtifactWriter);
    WTF_MAKE_TZONE_ALLOCATED(ArtifactWriter);
public:
    ArtifactWriter(OpenedArtifact&, ProducerLock&, ProducerBudget&, size_t stagingBytes);   // stagingBytes is at least 1
    ~ArtifactWriter();             // frees the staging buffer, releases its charge and closes the pack
    std::expected<CommitResult, CommitFailure> commit(const CommitStamp&, const CommitSections&, CommitTiming* = nullptr);
    std::optional<CommitFailure> remove(const BodyKey&);   // appends a tombstone for a listed key; nothing in production calls it
    // The end of a delta: compacts unless the pack holds only its live records and one full index.
    std::expected<std::optional<CompactionResult>, CompactionFailure> compactIfNeeded(bool timed);
    void releaseStagingBuffer();   // the end of production
    // The compaction compactIfNeeded and maintenance share; the caller holds the producer lock.
    static std::expected<CompactionResult, CompactionFailure> compactPack(const CompactionTarget&, const CompactionSource&,
        const ScopedLambda<bool(const BodyKey&)>& excludes, bool timed);
    // What a live body must pass before a compaction keeps it: nothing when it passes, else the first check it failed.
    static std::optional<ContainerCheck> checkPackedBody(const MappedPack&, const PackIndexEntry&,
        std::span<const uint8_t, 16> headerDigest);
    // Named in every build, so the commit path marks each point once; only twins builds act on the mark.
    enum class KillPoint : uint8_t { BeforeAppend, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterIndex, AfterRoot,
        CompactionBeforeRename, CompactionAfterRename };

#if ENABLE(JITCACHE_TWINS)
    // n counts this writer's commits from 1, or its compactions for a compaction.* step or point.
    struct FaultForTesting { ASCIILiteral check; uint64_t n; };   // writer.open, writer.write, writer.reread or a compaction.* step
    void setFaultForTesting(std::optional<FaultForTesting>);
    static std::optional<ASCIILiteral> faultCheckNamed(StringView);   // the check of that name, for --jitcache-test-writer-fault; nullopt for any other
    // raise(SIGKILL) at the point of the n-th commit or compaction.
    struct KillForTesting { KillPoint point; uint64_t n; };
    void setKillForTesting(std::optional<KillForTesting>);
    static ASCIILiteral name(KillPoint);                               // the point's name, such as "after-root"
    static std::optional<KillPoint> killPointNamed(StringView);        // the point of that name, for --jitcache-test-kill; nullopt for any other
    std::expected<CommitResult, CommitFailure> rewriteSection(const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
#endif

private:
    bool ensureStagingBuffer();    // charged before it is allocated and kept until production ends
    int ensurePackOpen();          // the pack opened for writing and its prefix mapped shared and writable; clears an abandoned retired mark
    void closePack();
    std::optional<CommitFailure> publish(const PackIndexEntry&, uint64_t end);   // the index block and the root's store

    OpenedArtifact& m_artifact;
    ProducerLock& m_producerLock;
    ProducerBudget& m_budget;
    const size_t m_stagingBytes;
    MallocSpan<uint8_t> m_staging;    // allocated and charged at the first commit; empty before it and once released
    int m_packFd { -1 };
    uint64_t m_packInode { 0 };
    std::span<uint8_t> m_writablePrefix;   // the pack's prefix, MAP_SHARED and writable: the root is stored through it
#if ENABLE(JITCACHE_TWINS)
    uint64_t m_commits { 0 };
    uint64_t m_compactions { 0 };
    std::optional<FaultForTesting> m_faultForTesting;
    std::optional<KillForTesting> m_killForTesting;
    std::optional<ASCIILiteral> m_commitFault;   // the armed fault of the commit in progress
#endif
};
```

`CompactionSource` and `CompactionTarget`, which `compactPack` takes, are declared beside these (container sub-SPEC section 8.4). The writer, the pack's layout, the commit's steps, the compaction and their failure handling are in the container sub-SPEC, section 8. A test builds a `CommitSections` from spans alone (container sub-SPEC test C7). The score of the body the writer publishes is the capture glue's (section 8.4, step 6). ([history](SPEC-integrator-history.md#the-writer-takes-section-sources-and-reads-no-lane-format))

The capture glue builds the list with `CommitSections commitSectionsFor(const UCBSections&, const ImageCapture&, const CBStateCapture&, std::span<const uint8_t> ics)` in `JITCacheCapture.cpp`, in section-kind order. It returns the sources by value, so no array in its frame outlives the call; the bytes and objects they name belong to the three captures and the ICs buffer, which live until step 9 of section 8.4 returns:

| kinds | sources |
|---|---|
| `UCBIdentity`, `UCBCore`, `UCBFeedback` | in memory: `identity`, `core->span()` and `feedback` of `UCBSections` (SPEC-ucb.md section 8.1) |
| `UCBAnalysis` | in memory: `analysis` of `UCBSections`, only when it is not empty (SPEC-ucb.analysis.md R-INT-A5) |
| `ImageBaseline`, `BakedFactsBaseline`, and `ImageTwinsBaseline` in twins builds | streamed: `imageSectionSize()` through `writeImageSection`, `bakedFactsSectionSize()` through `writeBakedFactsSection`, `twinsSectionSize()` through `writeTwinsSection` (SPEC-image.md section 9) |
| `CBStateBaseline`, `CBSummaryBaseline` | in memory: `stateSection()` and `summarySection()` of `CBStateCapture` (SPEC-cb.md section 6.1) |
| `ICsBaseline` | in memory: the ICs buffer of section 8.4, step 4 |

Each streamed source's function is a captureless lambda that casts its object back to the `ImageCapture` and calls the write function, which takes a sink of the same type and returns false once the sink refuses (SPEC-image.md section 9).

## 7. Install glue

### 7.1 Install points

`ScriptExecutable::prepareForExecutionImpl` (N1) holds the first of THREAD Restoration's two install points and becomes, from the native test of the UCB's sharing slot on:

```cpp
    bool installedUnlinkedBaselineCode = false;
    bool installedByJITCache = false;
#if ENABLE(JIT)
    if (RefPtr<BaselineJITCode> baselineRef = codeBlock->unlinkedCodeBlock()->m_unlinkedBaselineCode) {
        codeBlock->setupWithUnlinkedBaselineCode(baselineRef.releaseNonNull());
        installedUnlinkedBaselineCode = true;
    }
#endif
    if (!installedUnlinkedBaselineCode) {
        if (Options::useLLInt()) {
#if ENABLE(JIT)
            // JITCache: an imported body's first CB installs here. The install function calls installCode itself,
            // as its last step.
            installedByJITCache = JITCache::installAtNewbornCodeBlock(vm, *codeBlock, JITCache::InstallPoint::BeforeSetupLLInt)
                == JITCache::InstallOutcome::Installed;
#endif
            if (!installedByJITCache)
                setupLLInt(codeBlock);
        } else
            setupJIT(vm, codeBlock);
    }

    if (!installedByJITCache)
        installCode(vm, codeBlock, codeBlock->codeType(), codeBlock->specializationKind(), Profiler::JettisonReason::NotJettisoned);
#if ENABLE(JITCACHE_TWINS)
    JITCache::didFinishPrepareForExecution(vm, *codeBlock, scope);   // the image twin check of an install above
#endif
```

`JIT::compileSync` (N2) holds the second and gains, after the `USE(PROTECTED_JIT)` block and before the plan is created:

```cpp
    // JITCache: with the LLInt off, an imported body's first CB installs here.
    if (!Options::useLLInt()
        && JITCache::installAtNewbornCodeBlock(codeBlock->vm(), *codeBlock, JITCache::InstallPoint::CompileSync) == JITCache::InstallOutcome::Installed)
        return CompilationResult::CompilationSuccessful;
```

With the LLInt off, `setupJIT` reaches `compileSync` from `prepareForExecutionImpl` after the sharing-slot test, and an installed import returns success, which `setupJIT`'s `RELEASE_ASSERT` accepts. `prepareForExecutionImpl` then calls `installCode` a second time, as it natively does after `setupJIT` (install.md, step 4). `compileSync`'s one other caller is Bun's `vm.Script` route (N2), which makes no sharing-slot test of its own, so step 1 below repeats it; section 10 says what that route reaches. ([history](SPEC-integrator-history.md#one-install-function-with-a-point-argument))

`llint/LLIntSlowPaths.cpp`: `shouldJIT` loses `static inline` and is declared in `llint/LLIntSlowPaths.h`, namespace `JSC::LLInt`, under `ENABLE(JIT)`, as `bool shouldJIT(CodeBlock*);`. Its body and its three callers stay as they are (N4).

### 7.2 The install function

`JITCacheGlue.h` declares:

```cpp
enum class InstallPoint : uint8_t { BeforeSetupLLInt, CompileSync };
enum class InstallOutcome : uint8_t { NotInstalled, Installed, KeptForNextCodeBlock, DroppedByGate, Abandoned };
InstallOutcome installAtNewbornCodeBlock(VM&, CodeBlock& newborn, InstallPoint);
```

Before step 1, when `vm.jitCacheState()` is non-null, the function calls its `releaseEndedProductionMemory()` (section 4.2). The steps, in THREAD's order (THREAD Restoration; Image R-INT-7, CB R-INT-3 and R-INT-4, ICs R-INT-4, UCB R-INT-6):

1. Return `NotInstalled` unless all hold: `vm.jitCacheState()` is non-null and `importsEnabled()`; the UCB's `m_unlinkedBaselineCode` is null, since parked code wins; `newborn.jitType()` is `JITType::None` and `baselineJITData()` is null, since a CB that already ran in the LLInt receives no imported state; and `registry().pendingImport(ucb)` returns an import. The first test is one acquire load and one relaxed load, and the last is one registry lookup, so a newborn CB without an import pays nothing more.
2. Open `DeferGCForAWhile installDeferral(vm)`, the install function's own GC deferral, and hold `RefPtr<ValidatedBody> body = import->body()` until step 18 drops it, which keeps every section span alive through the last lane call (ICs R-INT-4, CB R-INT-1, Image R-UCB-2). In twins builds step 18 keeps a second reference in the twin check's stash (harness sub-SPEC section 3).
3. `parseImageSections(spans, strict)`, where `spans` is an `ImageSectionSpans` holding the body's `image.baseline` and `baked-facts.baseline` and, in twins builds, where alone the struct has that member, its `image-twins.baseline`. It checks their structure only with strict on, apart from the bound that every region fits in its section, which it checks in both modes (SPEC-image.md section 8.5). A failure is invalid material, part `image`, at `description(check)`; return `Abandoned`.
4. With strict on, `validateImageSectionsAgainst(view, ucb)`; normal mode trusts what it checks (Image R-INT-3). A failure is invalid material as in step 3; return `Abandoned`.
5. `compareBakedFacts(view, newborn)`. `Mismatch` leaves the CB native and keeps the import for the next newborn CB of the UCB, which repeats this sequence from step 1 against the same bytes (CB R-INT-4); count it in `Progress::bakedFactMismatches` and return `KeptForNextCodeBlock`.
6. With `InstallPoint::BeforeSetupLLInt`, `LLInt::shouldJIT(&newborn)`. False: `registry().resolvePendingImport(ucb, import, ImportResolution::DroppedByGate)` and return `DroppedByGate` (THREAD Restoration). With the LLInt off there is no gate, as natively.
7. `prepareImage(vm, ucb, view, budget, strict)`, with `budget` the active production's budget, `producerContextIfActive()->budget()` (section 4.1), when `producerContextIfActive()`, read once here, returns a context, and null otherwise. Since only Consumer and ConsumerProducer VMs import, the budget is non-null exactly in a ConsumerProducer VM whose production is active, Image R-INT-7's condition for rebuilding the record. `PrepareOutcome::InvalidMaterial` is invalid material as in step 3; `ExecutableMemoryExhausted` calls `didFailExecutableAllocation(vm, ExecutableAllocationSite::JITCacheImage)`. Either returns `Abandoned`. ([history](SPEC-integrator-history.md#a-consumerproducer-rebuilds-the-image-record-only-while-production-is-active))
8. `CBStateImport::prepare(cbState, newborn, strict)`. A `CBFault` is invalid material, part `cb`, at `description(check)` with detail `faultDetail(check)`; the `PreparedImage` is destroyed without `commit`; return `Abandoned`.
9. `ICs::prepareBaselineICs(icsSection, prepared.code(), newborn, strict ? StrictChecks::Yes : StrictChecks::No)`. An `Invalid` is invalid material, part `ics`, at `description(check)` with the site index in the detail; the `PreparedImage` is destroyed without `commit`; return `Abandoned`.
10. When `budget && budget->hasRefused()` holds, raise the recording fault `budget.limit` with the detail `rebuilt image record` (section 4.5). The rebuilt record's charge was refused, either itself or because an earlier refusal fails every later charge, and the image goes on without a record (SPEC-image.md section 10.3, step 14). A recording fault ends production and leaves activity on, so the install continues.
11. Seed the CB, which now cannot fail: `ICs::seedCallLinkHistory(icsPrepared, newborn)`, then `cbImport.seedLinkedState(newborn)`. The ICs lane gives each restored call site its own `CallSiteData` where THREAD Caches says so; the two calls write disjoint fields of shared metadata entries and the CB lane writes nothing in a `CallSiteData`, so either order is correct (SPEC-cb.md section 6.4, ICs R-CB-1). Both run inside step 2's `DeferGCForAWhile`: `LazyCallLinkInfo::ensureSlow` allocates each `CallSiteData` with `new` and reports extra memory to the heap, which may request a collection that the deferral only records.
12. `Ref<BaselineJITCode> code = WTF::move(prepared).commit(vm, newborn)`: the code-size sample, the cross-modifying fence and the profiler reports (SPEC-image.md section 10.3).
13. `newborn.setupWithUnlinkedBaselineCode(code.copyRef())`, native setup, which builds the `BaselineJITData` from the restored molds and parks the code in the empty sharing slot.
14. `ICs::attachPropertyICState(icsPrepared, newborn)`. Twins builds, while `twinReportSink()` is non-null (R-ALL-2): `ICs::checkRestoredBaselineICs(icsPrepared, newborn)`, each mismatch reported as a difference (ICs R-INT-7).
15. `CBCounterRestore restore = cbImport.finishCounter(newborn)` (SPEC-cb.md section 5.3). Twins builds, while `twinReportSink()` is non-null (R-ALL-2): `cbImport.verifyTwins(newborn, restore, *twinReportSink())` (CB R-INT-6).
16. `newborn.ownerExecutable()->installCode(&newborn)`.
17. `registry().resolvePendingImport(ucb, import, ImportResolution::Installed)`.
18. Count `Progress::installs`. Twins builds: stash the CB, the code, the body and the parsed view for the image twin check that `prepareForExecutionImpl` runs once it returns, in the image twin-check state, which the VM's first stash creates (harness sub-SPEC section 3). Then drop `body`: step 17 released the pending import's reference, so outside twins builds this one is the last and its drop destroys the body, which releases its reference to the pack's mapping (container sub-SPEC section 7.2). When a bench report is open, the function closes the `install` event's total before the drop, times the drop on its own as `bodyRelease` (IB1), and then records the event (harness sub-SPEC section 9.2). ([history](SPEC-integrator-history.md#measurement-choices))
19. Return `Installed`.

Steps 3 to 9 write nothing to the CB, the UCB or the VM's statistics: the image preparation keeps its effects inside the `PreparedImage` until `commit` (SPEC-image.md I11), and the other two preparations only read (SPEC-cb.md I5, SPEC-ics.md I1). A failure there leaves the CB at its link state, and `prepareForExecutionImpl` goes on with `setupLLInt` or `setupJIT`. Steps 11 to 17 cannot fail. Their cell allocations are the CB lane's realm step and the `FunctionExecutable`s that native setup materializes in step 13 (`prepareLazyStateForConcurrentCompilation`, under a deferral of its own), and their other allocations are the CB lane's predictions array and lazy-operand holder and the ICs lane's `CallSiteData`s; all of them run under the deferral of step 2 and crash on exhaustion as their native twins do.

The install builds no plan, so it skips the call of `ensureValueAndArrayProfiles` in `BaselineJITPlan`'s constructor (N3), and it makes none itself: the UCB lane allocates the UCB's value and array copies of every body with an image when it seeds the UCB, before publication or attach (SPEC-ucb.md section 5.3).

### 7.3 Outcomes

| outcome | the CB | the pending import | next |
|---|---|---|---|
| `NotInstalled` | untouched | untouched | native `setupLLInt` or compilation |
| `Installed` | baseline, installed | resolved `Installed` | `prepareForExecutionImpl` skips its trailing `installCode` (LLInt on) or calls it a second time (LLInt off) |
| `KeptForNextCodeBlock` | untouched | kept | native; the next newborn CB of the UCB tries again |
| `DroppedByGate` | untouched | resolved `DroppedByGate`, which stamps the import's index token as missed (SPEC-ucb.md section 6.4) | native |
| `Abandoned` | untouched | kept, and it dies with its UCB, since activity is now off | native |

### 7.4 Context

Both install points run on the VM thread with the API lock and heap access, inside `prepareForExecutionImpl`'s `DeferGCForAWhile` (N1), which the LLInt-off route reaches through `setupJIT`, and inside the install function's own (step 2); section 10 gives the one route that reaches the function without them. No stopped-world GC phase starts while the function runs, and nothing the function calls starts or waits for a collection or releases heap access. The newborn CB is unpublished until step 16 stores it in its executable and write-barriers the executable (install.md, "Conditions around installation"), so no marker or compiler thread reads what steps 11 to 15 write, and the lanes add no fence of their own. The function takes no JSC lock itself; its callees take theirs: `JITThunks::m_lock` and the executable allocator's locks in `prepareImage`, the UCB registry lock in steps 1, 6 and 17, and `CodeBlock::m_lock` in step 14.

## 8. Capture glue

### 8.1 Eligibility

A CB is a candidate when, on the VM thread:

1. `cb.jitType() == JITType::BaselineJIT` and `cb.replacement() == &cb` (THREAD Capture);
2. its `BaselineJITCode`, `static_cast<BaselineJITCode&>(*cb.jitCode())`, satisfies `isImageCapturable` (SPEC-image.md section 9), which is THREAD Capture's "shareable and recorded";
3. `captureRecord(vm, *cb.unlinkedCodeBlock())` returns a record, so its UCB has a recorded key and a context the capture can write (SPEC-ucb.md section 8.1).

The candidate's key is the record's key. Imported CBs qualify like any other: in a ConsumerProducer their code carries the rebuilt record (SPEC-image.md section 10.3, step 14).

### 8.2 Scores

```cpp
constexpr uint8_t imagelessTier = 0;   // a capture without an image: the UCB lane's sections alone, a module's analysis included
constexpr uint8_t baselineTier = 1;
struct CaptureScore {
    uint8_t tier { 0 };              // imagelessTier or baselineTier
    uint64_t richness { 0 };         // the UCB lane's units plus the CB lane's units
    uint32_t icSitesWithCases { 0 };
    bool counterWithheld { false };  // a polymorphic site withheld the counter's progress
    uint32_t counterProgress { 0 };
};
struct SavedScore { CaptureScore score; uint64_t version { 0 }; };   // version: the saved body's commit identifier

// Compares the fields lexicographically in declaration order, a true counterWithheld above a false one. A candidate
// wins only when it is strictly greater; a remaining tie keeps the saved body.
bool beats(const CaptureScore& candidate, const CaptureScore& saved);

// A saved body's score, read from its three summary sections by the lanes' readers with the session's strict flag.
// With strict off no reader rejects. With it on, the rejection names the reader: part "ucb" at "saved-summary"
// (savedRichness names no check), "cb" with description(check) and detail faultDetail(check), or "ics" with
// description(check). The detail is empty except for "cb".
struct SummaryRejection { ASCIILiteral part; ASCIILiteral check; ASCIILiteral detail; };
std::expected<CaptureScore, SummaryRejection> scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics, bool strict);
```

A live candidate's score is tier 1; richness `liveRichness(ucb).total()` plus the `richnessUnits` of `CBStateCapture::scoreLive(cb, strict, ics.hasPolymorphicSite)`; the IC sites with cases of `ics = ICs::summarizeBaselineICs(cb)`, which is `ics.summary.icSitesWithCases`; and the `counterWithheld` and counter progress of the same `scoreLive`. The glue calls the ICs lane before the CB lane, for the same CB in the same pause, and passes its `hasPolymorphicSite` on, so a candidate with a polymorphic site scores its counter as withheld, with no progress, as its capture will carry it (THREAD Restoration; CB R-INT-5, ICs R-INT-3, UCB R-INT-5). A `scoreLive` failure is the CB lane's eligibility or pairing guard under strict (SC2, SC3), a recording fault (SPEC-cb.md section 7).

A body that ran without an image (section 8.6, phase 3) scores tier 0, richness `liveRichness(ucb).total()`, which is its arithmetic categories alone, since a body captured without an image carries no UCB value or array copies (THREAD Capture), no IC sites, no withheld counter and the counter progress `carriedLLIntPoints` of its LLInt counter (SPEC-ucb.md sections 5.5 and 8.3). Tier comes first in `beats`, so it never replaces a body with an image and any capture with an image replaces it. Any candidate beats an absent body (THREAD Capture), a tier-0 one with no richness and no progress included, so the glue compares with `beats` only when a saved score exists.

A saved body's score comes from its three summary sections: the highest tier, richness `savedRichness(ucb.feedback).total()`, the section parsed with `FeedbackSection::parse(ucb.feedback, strict)`, plus `decodeSummary(cb.summary, strict)->richnessUnits`, `readBaselineICsSummary(ICsBaseline, strict ? StrictChecks::Yes : StrictChecks::No)->icSitesWithCases`, and `decodeSummary`'s `counterWithheld` and counter progress. `scoreSections` computes it from the three spans of a saved body (section 8.3). A body of tier 0 holds no CB or ICs summary: its score is tier 0, `savedRichness(ucb.feedback).total()`, no IC sites, no withheld counter and `carriedLLIntPoints` of the section's T and P, and the empty spans are not read. With strict off the readers trust the bytes; with strict on a rejected summary is invalid material at the part, check and detail its `SummaryRejection` names, which stops the capture with nothing written, and the UCB lane leaves the empty answer of `FeedbackSection::parse`, possible only with strict on, to the glue to raise (UCB R-INT-5). A capture about to be committed takes its score from what its builders return instead (section 8.4). A tier-0 capture keeps the live score it was ranked with, read before the build from `liveRichness(ucb)` and the UCB's LLInt counter, which neither JS nor the collector changes during a capture, so it describes the sections the build writes and the glue reads none of a capture's own bytes back (II23). ([history](SPEC-integrator-history.md#the-kept-score-comes-from-the-builders))

### 8.3 Kept summaries

A producing `VMState` keeps its kept summaries in a `KeptSummaries`, a struct `JITCacheCapture.cpp` defines around a `HashMap<BodyKey, SavedScore, BodyKeyHash, BodyKeyHashTraits>` (container sub-SPEC section 6.1), charged per entry (section 4.4) before the entry is added. The capture glue creates it at the first kept entry with a deleter of its own, a captureless lambda that deletes it, so neither `JITCacheVMState.h` nor the state's destructor names the capture glue's types (section 4.1). Destroying a `KeptSummaries` releases, into the budget it charged, the charges of its entries and its buckets (section 4.4), so `releaseEndedProductionMemory()` frees it through the deleter alone. A refused charge raises `budget.limit` and keeps nothing: at a key's first scoring the capture then writes nothing, and a commit charges the entry its step 10 adds at step 8, before writing, so a refusal there also writes nothing (section 8.4). A key with an entry is scored against it. At a key's first scoring the glue reads the saved body once, through the store's scoring read `OpenedArtifact::readSavedSummaries(key, mode)`, in `ValidationMode::Full` exactly when strict is on (container sub-SPEC section 7.3), which follows the root, finds the key's newest entry in the index and borrows that body from the pack's mapping. The producer lock keeps every other writer out, so the index of a producing VM, which follows its own commits, lists every body the pack holds. The read tells four cases apart: ([history](SPEC-integrator-history.md#the-scoring-read-follows-the-root))

| result | meaning | the glue |
|---|---|---|
| `Absent` | no saved body: the index lists none for the key | scores the candidate against nothing, which it beats; keeps nothing |
| `Unavailable` | the store's fault hook of twins builds failed the read with `EMFILE`, `ENFILE` or `ENOMEM` | defers the key's capture: no commit, nothing kept, no fault; counts `Progress::capturesDeferred` and `transientOpenFailures` |
| `Invalid` | the pack failed its own checks (`container.pack`), another injected error (`container.io`), or a container check of the read's mode failed on the envelope, the directory or a summary section | raises invalid material at the check (`container.io` with `strerror` of the error, or the check's name), which turns activity off, and stops the capture |
| `Found` | the envelope, the directory and the three summary sections passed the checks of the read's mode | computes the saved score with `scoreSections`, drops the read's reference to the mapping and keeps the score |

A deferred key is scored again at the next finalize capture of one of its CBs or at the next `delta`. An unreadable body never scores as absent, since any candidate beats an absent body (THREAD Capture).

A commit replaces the entry with the score of the capture it wrote (section 8.4), so a kept summary always describes the body its key's newest index entry names. The producer lock keeps every other process from committing, so an entry never goes stale while the VM produces. The one other way a body changes then is a test's: in twins builds `JITCacheCapture.h` declares `std::expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM&, const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes)`, which requires active production and rewrites the body through the writer's `rewriteSection` (container sub-SPEC section 8.3). A rewrite that succeeds erases the key's entry. A failure at `writer.rewrite` leaves the artifact and production as they were and raises nothing; any other failure is raised as a recording fault at its check, as step 9 of section 8.4 raises a commit's. The shell's `jitcacheRewriteSection` calls it (harness sub-SPEC section 5.2). Twins builds also declare `std::optional<SavedScore> keptScoreForTesting(VM&, const BodyKey&)`, which returns the key's entry, for T-STAMP. ([history](SPEC-integrator-history.md#the-kept-score-comes-from-the-builders))

### 8.4 Building and committing one capture

`commitCapture(state, cb, record, liveScore, saved)` runs only for a candidate whose live score beats the saved body's, `saved` being that score or the empty `CaptureScore` when no body is saved, in this order:

1. `Ref<BaselineJITCode> code` from `cb.jitCode()`, held until the writer returns, so the code the image section reads stays alive (Image R-INT-6).
2. `captureImage(vm, cb, code, budget, strict)`. `NotEligible` ends this CB's capture without a fault, as for an ineligible CB (Image R-INT-6); `ChargeRefused` raises `budget.limit`; `RecordingFault` raises a recording fault, part `image`, at `description(check)`.
3. `buildSections(vm, ucb, holderOf(cb), BodyImage::Present, budget)`, then `ucbRichness = liveRichness(ucb)`. For a module the lane may read the saved body's `ucb.analysis` through `readBodySection` (section 6.2). A `UCBCaptureFailure` raises a recording fault (section 4.5), with two exceptions: `SavedAnalysisInvalid`, a saved `ucb.analysis` that the copy found malformed, raises invalid material at `ucb.analysis` (SPEC-ucb.analysis.md R-INT-A5), and `SavedAnalysisUnusable`, a saved `ucb.analysis` that `readBodySection` answered `Unusable`, raises nothing, because `readBodySection` raised the invalid material itself. When `buildSections` succeeds, the glue then tests `state.productionActive()`, and when it is false the capture stops with nothing written and no fault of its own, so invalid material raised anywhere inside a lane's build ends the capture. ([history](SPEC-integrator-history.md#the-glue-tests-production-after-a-lanes-build))
4. `size = ICs::baselineICsSectionSize(cb)`; `budget.tryCharge(size)`, a refusal raising `budget.limit`; allocate the buffer; `ics = ICs::captureBaselineICs(cb, buffer, strict ? StrictChecks::Yes : StrictChecks::No)`. A `CaptureError` raises a recording fault, part `ics`, at `description(check)` with the site index in the detail (ICs R-INT-2).
5. `CBStateCapture::capture(cb, budget, strict, ics.hasPolymorphicSite)`. A `CBFault` raises a recording fault, part `cb`, at `description(check)` with detail `faultDetail(check)`.
6. `committed = CaptureScore { 1, ucbRichness.total() + cbState.score().richnessUnits, ics.summary.icSitesWithCases, cbState.score().counterWithheld, cbState.score().counterProgress }`: the score of the bytes just built, taken from what the builders return, which equals what `scoreSections` would compute from the committed body, so no lane's bytes are read back (II23). When `!beats(committed, saved)`, the capture stops here, with no fault and no commit: the lanes' outputs are destroyed and the ICs buffer is freed and released as in step 10, and the `capture` event records the capture as beaten (THREAD Capture). The live score and `committed` differ only when a marker's or a compiler thread's drain between the scoring and the build added bits by folding in a pending sample or dropped them in a racing merge (SPEC-cb.md section 4; profiles.md, "Drain"). ([history](SPEC-integrator-history.md#the-kept-score-comes-from-the-builders))
7. `CommitStamp { record.key, envelopeLLIntThreshold(ucb), cbState.score().counterProgress, 1 }`, the L and P compact orders by (UCB R-INT-5, CB R-INT-5, THREAD Maintenance). P is zero for a body whose counter does not travel, as for every body the ICs lane reported a polymorphic site for: ICs R-INT-2 passes the bit, and CB I16 zeroes the progress.
8. Charge, before anything is written, the entries the commit adds. When `artifact.containsKey(record.key)` is false, the writer will add an index entry: `budget.tryCharge` six of the index's bucket sizes (section 4.4), a success adding to the state's index charge. When the kept summaries hold no entry for the key, as after an `Absent` scoring read, step 10 will add one: charge it as section 8.3 does. A refusal of either raises `budget.limit` with nothing written. A commit that fails adds no index entry, and the charge made here is released with the other index charges when production ends.
9. `writer.commit(stamp, commitSectionsFor(ucbSections, image, cbState, buffer))` (section 6.3). A `CommitFailure` raises a recording fault at its check, `budget.limit` or a `writer.*` step, with its detail.
10. On success: the kept summary becomes `SavedScore { committed, result.version }`, in an entry step 8 charged when it is new, `Progress::capturesCommitted` and `bytesCommitted` grow, and the bench event is recorded. The writer's reread checked every section of the record against the checksum computed from these same bytes as they were streamed, so the kept score describes the body. The lanes' outputs are then destroyed, which releases their charges (UCB R-INT-5), and the ICs buffer is freed and released.

A failure at any step writes nothing: the glue destroys the lanes' outputs, which releases their charges, and frees the ICs buffer and releases its charge.

The image comes first because it is the only builder that can declare the CB ineligible. The ICs lane comes before the CB lane because the CB lane's capture takes the polymorphic bit the ICs capture returns, for the same CB in the same pause (THREAD Restoration; CB R-INT-5, ICs R-INT-2). All four lanes' sections of one capture go into one body in one commit, so `ucb.*`, `cb.state` with `cb.summary`, and the image sections are always committed together (UCB R-INT-5, CB R-INT-5).

### 8.5 The finalize capture

`BaselineJITPlan::finalize` calls `JITCache::didFinalizeBaselineCompilation` at the end of its `CompilationSuccessful` case, after `installCode` and `jitSoon` (THREAD Capture; section 9 shows the edit). `JITCacheGlue.h` declares the hook with the timing its `compile` event records, so the signature task 0 declares is the one task 9 calls and task 16 fills:

```cpp
struct BaselineCompileTiming {
    uint64_t compileNanoseconds;    // compileInThreadImpl after its profile drain
    uint64_t finalizeNanoseconds;   // finalize up to this call, without relinking incoming calls
    bool supportGenerated;          // per-VM support generated during either span
};
void didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*);   // null when the plan did not measure
```

The function:

1. records the compilation's `compile` event from the timing when it is non-null, whatever the state's role, so a Consumer measures the native cost of the bodies it compiles; a plan measures, and passes a timing, exactly when its VM had an open bench report at the plan's construction (harness sub-SPEC section 9.3);
2. when the VM has a state, calls its `releaseEndedProductionMemory()` (section 4.2); returns unless that state's production is active;
3. raises `budget.limit` and returns when the budget has refused a charge (section 4.5);
4. returns unless the CB is a candidate (section 8.1); at this point the CB is its executable's `replacement()`, since `installCode` just installed it;
5. scores the candidate (section 8.2) and the saved body (section 8.3), and returns when the saved body is `Unavailable` or the candidate does not beat it;
6. runs `commitCapture` (section 8.4).

Section 10 gives the one route that reaches this hook with no deferral. In twins builds the function first adds one to the `baselineCompiles` of the CB's UCB, in every VM (harness sub-SPEC section 10.1).

### 8.6 `delta`

After the checks of section 3.4:

1. Open `DeferGCForAWhile`.
2. When the budget has refused a charge, raise `budget.limit` and return `Faulted`.
3. Phase 1. Take `vm.heap.codeBlockSet().getLock()` and walk `vm.heap.forEachCodeBlockIgnoringJITPlans(locker, ...)`, which completes no pending plan (THREAD Capture, N7). For each candidate (section 8.1), compute its live score and keep it under its key, the key's candidates in decreasing order under `beats`, a tie after the ones met earlier. The candidate table, a map from key to a vector of its candidates' `CodeBlock*` and scores keyed through `BodyKeyHash` and `BodyKeyHashTraits` (container sub-SPEC section 6.1), is charged per key as section 4.4 charges a table entry. Each key's vector has one inline slot, which that entry's buckets hold, and grows only explicitly: before an append that would exceed its capacity, the glue charges the whole new heap capacity, grows the vector to it, and then releases the old heap capacity. The first growth out of the inline slot releases nothing, because the per-key bucket charge already covers that slot. A refused charge, or a recording fault from a scoring call (the CB lane's eligibility or pairing guard, SC2 or SC3), ends the walk and makes `delta` return `Faulted` after the lock is released. The functor allocates no cell and does no I/O; the only locks it takes are the ones its lane calls take (section 10). Release the lock.
4. Phase 2. For each key of the candidate table, which `commitCapture` does not change: score the saved body once (section 8.3); when the key's first candidate beats it, run `commitCapture`. Commits are independent (THREAD Capture), so their order is the table's. An `Unavailable` saved body defers the key. A `NotEligible` image makes that CB an ineligible one (Image R-INT-6), so the key's next candidate gets the same test, until one commits or does not beat the saved body; another live CB of the key can run a different `BaselineJITCode`, as after the sharing slot was released and the body recompiled. The loop then goes on with the next key. After each `commitCapture`, the glue tests `productionActive()`, which invalid material raised inside that key's build turns off without failing the build (section 8.4, step 3). A recording fault, invalid material or a production found inactive stops the loop, and `delta` returns `Faulted` with the recorded fault and the counts committed before it.
   Phase 3, with the delta capture `All`, once phase 2 has decided every key (SPEC-ucb.md section 8.3). Charge `recordedCodeBlockCount()` copies of `UCBRegistry::RecordedCodeBlock`, refusal raising `budget.limit`, copy the registry's records, and for each UCB skip it when `isPendingDestruction()` holds, when `llintCounterShowsExecution` is false, when `captureRecord` answers none, when phase 1 found an eligible CB of its key that is still eligible (a CB whose image phase 2 found unrecordable is not, Image R-INT-6), and, for function code, when the record keeps no holder or its holder is dead but not swept (`isPendingDestruction()`). Each other UCB is a candidate, counted in `Progress::captureCandidates`, scored as section 8.2 says for a body without an image, and kept in a second candidate table charged as phase 1's. Free the copy and release its charge; then each key of that table, counted in `eligibleKeys`, is committed as in phase 2, its capture building the UCB lane's sections alone (`buildSections` with the record's holder and `BodyImage::Absent`), its committed score the live score it was scored with (section 8.2), its stamp `CommitStamp { key, carriedLLIntPoints(T, P), 0, imagelessTier }` with T and P from the UCB's LLInt counter, and its sources the three UCB sections, with `ucb.analysis` when the lane built one. A `UCBCaptureFailure` is raised, and production tested after the build, as in step 3 of section 8.4.
5. Free the candidate tables. When no phase faulted and production is still active, compact the pack with `writer->compactIfNeeded(timed)` (section 6.3), which rewrites it unless it already holds only its live records and one full index (THREAD Storage; container sub-SPEC section 8.4), so a `delta` that committed nothing leaves a dense pack alone. A body that fails its checks while the compaction copies it (`CompactionFailure::Kind::Damaged`) is invalid material at the check it failed; any other failure is a recording fault at its check, a `compaction.*` step or `budget.limit`. Either fault makes `delta` return `Faulted`, and the bodies committed before it stay committed. With a bench report open, the compaction is timed and the `pack` event records the pack as the commits left it and what the compaction made of it (harness sub-SPEC section 9.2).
6. Record the `delta` event and flush the bench report, as every return from step 2 on does. On success, count `Progress::deltaRuns` and return `Completed` with `eligibleKeys`, `committedBodies`, `committedBytes` and `deferredKeys`.

Every return after step 3 frees the candidate table and releases its charges first, the `Faulted` returns of steps 3 and 4 included: the table lives only inside one call, so `delta` releases what it charged itself and `releaseEndedProductionMemory` (section 4.2) never holds it.

The `CodeBlock*` kept between the phases stay valid: each was in the CodeBlock set, so it was marked at the last End phase or created since (N7), and nothing between the phases can complete a collection, since the VM thread holds heap access, reaches no stop point and allocates no cell, and lazy sweeping runs only in allocation. THREAD Capture states the same condition for the reads themselves. ([history](SPEC-integrator-history.md#delta-scores-in-two-phases))

### 8.7 Context

All capture glue runs on the VM thread with the API lock and heap access, JS paused, and no collector phase on the thread: the finalize capture inside door 1's deferral (N8; install.md, "Conditions around installation") and `delta` inside its own (section 8.6, step 1); section 10 gives the one route that reaches the finalize hook with no deferral. The glue calls nothing that drains a profile, materializes a property table, allocates a cell or stops for the collector, and never finalizes a plan (THREAD Failures). Its only file I/O is the writer's and the saved body's read, and neither touches the heap.

Captures therefore never nest: a capture calls no host or JS function (THREAD Session), no lane's capture call finalizes a plan or walks the CodeBlocks with plan completion (SPEC-ucb.md section 8, SPEC-image.md section 9, SPEC-cb.md section 4, SPEC-ics.md section 5), and the writer does only file I/O. Builds with `ASSERT_ENABLED` check it: the state counts the captures in progress, and the finalize capture and `delta` assert on entry that none is. ([history](SPEC-integrator-history.md#captures-cannot-nest))

## 9. Plan-site faults

These are the plan sites THREAD Execution gives the integrator, and each raises the fault before the failure's effects are written, as THREAD Failures requires. They meet UCB R-INT-11 and CB R-INT-12; section 4.5 lists the other sites.

A baseline plan fails only for lack of executable memory (N3; SPEC-ucb.md F24). `BaselineJITPlan::finalize` becomes, with its comment as written:

```cpp
    case CompilationResult::CompilationFailed:
        // JITCache: a baseline plan fails only when it found no executable memory, so report that before the lines
        // below write the failure's effects.
        JITCache::didFailExecutableAllocation(*m_vm, JITCache::ExecutableAllocationSite::BaselinePlan);
        CODEBLOCK_LOG_EVENT(m_codeBlock, "delayJITCompile", ("compilation failed"));
        dataLogLnIf(Options::verboseOSR(), "    JIT compilation failed.");
        m_codeBlock->dontJITAnytimeSoon();
        m_codeBlock->m_didFailJITCompilation = true;
        break;
    case CompilationResult::CompilationSuccessful:
        WTF::crossModifyingCodeFence();
        dataLogLnIf(Options::verboseOSR(), "    JIT compilation successful.");
        m_codeBlock->ownerExecutable()->installCode(m_codeBlock);
        m_codeBlock->jitSoon();
        JITCache::didFinalizeBaselineCompilation(*m_vm, *m_codeBlock, m_jitCacheMeasure ? &jitCacheTiming : nullptr);
        break;
```

For DFG and FTL plans, `DFG::Plan` (`dfg/DFGPlan.h`) gains `void noteExecutableAllocationFailure() { m_failedForLackOfExecutableMemory = true; }` and `bool m_failedForLackOfExecutableMemory { false };`. The compiling thread calls it, before installing the `FailedFinalizer`, at the `linkBuffer.didFailToAllocate()` branches of `SpeculativeJIT::compile` and `SpeculativeJIT::compileFunction` (as `m_graph.m_plan.noteExecutableAllocationFailure()`) and at the two `state.allocationFailed` branches of `DFG::Plan::compileInThreadImpl`, before `FTL::fail(state)` (N9). `DFG::Plan::finalize` then calls, right before `m_callback->compilationDidComplete(...)`:

```cpp
    if (result == CompilationResult::CompilationFailed && m_failedForLackOfExecutableMemory)
        JITCache::didFailExecutableAllocation(*m_vm, isFTL() ? JITCache::ExecutableAllocationSite::FTLPlan : JITCache::ExecutableAllocationSite::DFGPlan);
```

The callback writes the failure's effects (the baseline counter's deferral and the quick DFG bit for a DFG plan, the quick FTL bit for an FTL plan, SPEC-ucb.md F24), so the fault precedes them, as CB R-INT-12 requires for the counter's deferral. In twins builds the same place adds one to `dfgCompiles` or `ftlCompiles` of `m_codeBlock`'s UCB on success (harness sub-SPEC section 10.1). The worklist's lock, which moves the plan to ready and hands it to the finalizing thread, orders the flag's write before its read. A plan that fails for any other reason records nothing, and its effects travel as captured (THREAD Restoration). Both calls run inside the plan's finalization with no worklist lock held, which `didFailExecutableAllocation`'s contract allows (section 4.5); section 10 gives the GC deferral each route runs them under.

An FTL plan whose lowering could not allocate Bun's FFI invoke thunk sets the same `allocationFailed` flag (N9), so it records the failure and raises `exec-alloc.ftl-plan` like any other FTL plan that found no executable memory, since the thunk is allocated inside the plan. Bun's FFI thunks and stubs outside a plan keep native behavior, as do the other allocations THREAD Failures lists. ([history](SPEC-integrator-history.md#the-executable-allocation-fault-list-is-exhaustive))

## 10. Locks, threads and GC

The integrator's locks:

| lock | protects | taken by |
|---|---|---|
| `ArtifactRegistry::m_lock` (process-wide `Lock`) | the map of opened artifacts | `ArtifactRegistry::take` (container sub-SPEC section 5.1) |
| `OpenedArtifact::m_indexLock` (`Lock`) | the index, the valid end and latest index block it follows, the current mapping, the retired-pack state and the statistics | the store's reads, which follow the root first, `containsKey`, the writer, which reads the latest block and the live bytes and follows its own commits (container sub-SPEC sections 6 and 8), and `statistics()`, which a bench report's flush calls on the VM thread (harness sub-SPEC section 9.2) |
| the producer lock (`flock` on the lock file, held through a `ProducerLock`) | the right to write the artifact | a producing VM from `start` to `didFinalizeHeap`; maintenance while it plans and applies |

Both in-process locks are leaves: nothing that takes a JSC or JITCache lock is called while either is held. Under each run only plain memory work, malloc, atomics and the system calls container sub-SPEC sections 5.1 and 6 list: under the index lock, an `fstatat` of the pack's path and the calls that map the pack again. No body is validated under it. A thread holding one therefore never waits on another JSC or JITCache lock. `VMState` has no lock: its plain fields are written and read on the VM thread, and the fields other threads read are atomics.

Lock order with JSC's locks:

- `delta` phase 1 holds `CodeBlockSet::m_lock` while the lanes' scoring calls take `CodeBlock::m_lock` (`summarizeBaselineICs`), `UnlinkedCodeBlock::m_lock` (`liveRichness`) and the UCB registry lock (`captureRecord`). Native code never takes `CodeBlockSet::m_lock` while holding any of these: `CodeBlockSet::add` runs in the `CodeBlock` constructor on the VM thread, and the conservative scan takes only the set's lock (SPEC-ics.md L2). The integrator's own locks are never taken under `CodeBlockSet::m_lock`.
- The UCB engine calls `bodyVersion` and `openBody` without holding its registry lock (SPEC-ucb.md section 6.3).
- `didFailExecutableAllocation` may run under `CodeBlock::m_lock` and takes no lock.

Threads: the VM thread runs everything except `producerContext` and the budget, which JIT workers use through the Image lane's recorder, `didAttachDebugger`, which may run anywhere, and the destructors of `ValidatedBody` and of the lanes' records, which may run on any thread holding no JITCache lock.

GC: `start`, `status`, the index and the writer allocate no cell. The install glue runs under two deferrals (section 7.4) and the capture glue under door 1's or `delta`'s (section 8.7). On the engine's routes the plan sites run inside a finalization under `DeferGC` in `completeAllReadyPlansForVM` (N8, N9), or, when `JITWorklist::enqueue` finalizes on the spot with concurrent JIT off, under the `DeferGCForAWhile` its caller holds (`jitCompileAndSetHeuristics`, `operationOptimize`, the FTL tier-up operations), as `prepareForExecutionImpl`'s does for `setupJIT`. The teardown hooks run after `VM::~VM` has deferred GC for good (N5).

Bun's `vm.Script` route calls `JIT::compileSync` after its own `DeferGC` has closed (N2; install.md, "Embedder routes"), so the glue it reaches runs with no deferral, and none of it needs one. With the LLInt off it reaches the install function (section 7.1), whose step 1 makes only its loads and one registry lookup and returns `NotInstalled`, because the UCB Bun's decode yields has no record (SPEC-ucb.md section 7.2.5). Its `BaselineJITPlan::finalize` reaches either the finalize hook, whose steps 1 to 4 touch no heap and whose step 4 fails for a CB whose UCB has no record (section 8.1, item 3), or, when the plan fails, the fault entry point, which allocates nothing and touches no heap (section 4.5). Neither function assumes a deferral.

Fences: `VM::setJITCacheState` stores with release order and `jitCacheState` loads with acquire order. Activity and production are `std::atomic<bool>`s turned off with release stores and read with acquire loads by other threads. The budget's counters are atomics. The pack's root is stored with one release store through the writer's shared mapping of the prefix and loaded with acquire by every reader, in this process and in others; the retired word is loaded with acquire (container sub-SPEC sections 4.6 and 8.2).

## 11. Hosts

THREAD Session says when each host calls `start` and what the host decides.

JITCache runs only on Linux (THREAD's opening), but its code compiles wherever Bun builds, so both hosts build and call it on every platform. Every call into the operating system that WTF does not wrap, with its headers, sits behind `OS(LINUX)`, while every function keeps a definition on every platform. Off the targets THREAD names, `start` returns `Rejected` at `start.platform` (section 3.2), so no VM gets a state and no guarded code runs, and off Linux maintenance returns `Failed` with `platform` (maintenance sub-SPEC section 2). Code under `ENABLE(JITCACHE_TWINS)` needs no guard of its own, since only `build.ts` makes twins builds and it runs only on Linux.

### 11.1 The jsc shell

`CommandLine::parseArguments` (`jsc.cpp`) recognizes these flags before its generic `--` option fallback (N14), and `printUsageStatement` lists them:

| flag | effect |
|---|---|
| `--jitcache[=<path>]` | configures the shell's main VM with `artifactPath = path`; bare or with an empty value, `path` is `defaultArtifactPath`, `./.jitcache` |
| `--jitcache-mode=c\|p\|p-c` | the role: `c` Consumer, `p` Producer, `p-c` ConsumerProducer; `c` when absent |
| `--jitcache-max-memory=<bytes>\|unlimited` | the producer limit, in bytes with an optional `K`, `M` or `G` suffix in powers of 1024; `unlimited`, which is also the limit when the flag is absent, is `SIZE_MAX` |
| `--jitcache-strict[=0\|1]` | strictness: bare or `=1` turns it on, `=0` off; off when absent, the default THREAD Session gives |
| `--jitcache-delta-capture=all\|compiled` | `Config::deltaCapture` through `parseDeltaCapture`; `all` when absent, the default THREAD Session gives |
| `--jitcache-delta-at-exit` | bare only: calls `delta` once the scripts and the run loop have finished, while the VM's production is active (section 3.6); off when absent |
| `--jitcache-log[=0\|1]` | bare or `=1` writes `toJSON` of the start result, of each `delta` result the shell gets and of the final `status` to stderr, one line each; `=0` writes nothing |
| `--jitcache-bench-report=<path>` | `Config::benchReportPath` |
| `--jitcache-maintenance` | bare only: runs the maintenance command line on the script arguments and exits (maintenance sub-SPEC section 5) |

Twins builds add the flags and functions of the harness sub-SPEC, section 5. The last occurrence of a flag wins. A malformed value is reported like a bad JSC option and fails the shell under `validateOptions`. Bun takes the same flags, except `--jitcache-maintenance`, `--jitcache-log` and `--jitcache-max-memory`, and gives `--jitcache-delta-at-exit` a value and a default of its own (section 11.2).

`runJSC` calls `JITCache::start` under `JSLockHolder` right after `VM::create` for the main VM, before the loop creates the first `GlobalObject`, when `--jitcache` was given, and with `--jitcache-log` prints the start result. Its config names no app, so the shell's artifacts name none and the shell opens any compatible one; the C++ tests reach the app's outcomes through `Config::appDigest` (T-START). The `$262.agent` workers' VMs are not configured. At the end of the loop's body, which runs once outside Fuzzilli's REPRL mode, once the run loop has returned (`vm.deferredWorkTimer->runRunLoop()`) or the iteration ended on a termination, and while the iteration's `GlobalObject` is still in hand, the file-static `finishJITCacheRun` takes `JSLockHolder` and calls `JITCache::finishHostRun(vm, deltaAtExit, hostWork)` (section 3.6), with `deltaAtExit` true exactly when `--jitcache-delta-at-exit` was given, whether or not `--jitcache` configured the VM. Its `hostWork`, in this order:

1. prints the `delta` result when `finishHostRun` called `delta` and `--jitcache-log` was given;
2. in twins builds, writes the body-event dump (harness sub-SPEC section 10.3) to the path `--jitcache-body-events` names;
3. in twins builds, writes the description of the heap JavaScript can reach (harness sub-SPEC section 6) to the path `--jitcache-describe-heap` names, whose full collection must change no capture of the `delta` and would retire live UCBs before step 2;
4. prints the final status when `--jitcache-log` was given.

`finishHostRun` then flushes the bench report, so a run without `--destroy-vm` keeps its bench events. A script that calls `quit()` exits inside the call and skips all of these, so the runner's scripts end by returning. `jscmain` runs `--jitcache-maintenance` right after the command line is parsed, before `JSC::initialize()`, and exits with its code; the shell requires the ratio (maintenance sub-SPEC section 5).

### 11.2 Bun

`src/jsc/bindings/JITCacheHost.h` and `.cpp` (new, `namespace Bun`) hold Bun's side. Bun's build bundles its `.cpp` files into unified sources (N15), so `JITCacheHost.cpp` keeps its file-local helpers in the named namespace `Bun::JITCacheHostInternal`: ([history](SPEC-integrator-history.md#bun))

- `JITCacheHost::configureVM(JSC::VM&, int32_t executionContextId)`, called from `Zig__GlobalObject__create` right after `JSC::JSLockHolder locker(vm)`, so `start` runs with the API lock and heap access (Bun acquires heap access just before) and before `JSVMClientData::create` and the first global object (N15).
  - It classifies the VM by `executionContextId` (N15): 1 is the main thread's VM; a value above 1 and below `INT32_MAX` is a worker's VM; any other value, `INT32_MAX` for macro VMs and the debugger thread's VM included, marks a VM that is neither, which is not configured. Nor are the bytecode builder's VM and Bake's production global, which do not go through this function.
  - The configuration comes from Bun's command line. `RUNTIME_PARAMS_` (`src/runtime/cli/Arguments.rs`) declares the flags of section 11.1 with the same names and value syntax, with two kinds of exception. Three flags are left out: `--jitcache-maintenance`, since Bun's maintenance is `bun jitcache`, `--jitcache-log`, whose lines serve the shell's runs, and `--jitcache-max-memory`, since the producer limit is the host's to set: Bun leaves `producerLimitBytes` empty once the bench has set `defaultProducerLimitBytes` (section 16), and sets it to `SIZE_MAX` until then, as the shell does without the flag. And `--jitcache-delta-at-exit` takes a value and is on by default: bare or `=1` on, `=0` off, on when absent, so a producing role saves at exit unless told not to, which the macro benches' producer without `delta` does with `=0` ([history](SPEC-integrator-history.md#bun)). `--jitcache`, `--jitcache-strict` and `--jitcache-delta-at-exit` take an optional value, as `--inspect` does. `Arguments::parse` resolves `--jitcache`'s path and every other path flag against `ctx.args.absolute_working_dir`, the working directory after `--cwd`, because every VM `configureVM` configures reads the flags and a relative path would follow a `process.chdir` made before a worker starts. Twins builds alone declare four flags of the harness sub-SPEC's section 5.1, `--jitcache-twins-report`, `--jitcache-twins-record-layout`, `--jitcache-twins-avoid-layout` and `--jitcache-body-events`, the bench's one: their entries in `RUNTIME_PARAMS_` and their parsing compile only under `cfg(jitcache_twins)`, which `scripts/build/rust.ts` sets in twins builds, so no other build of Bun has them. `Bun::JITCacheFlags` keeps one layout in every build, with those four fields empty outside twins builds, so its Rust and C++ definitions agree whatever `cfg(jitcache_twins)` and `ENABLE(JITCACHE_TWINS)` say. Every other flag of that section is the jsc shell's only. ([history](SPEC-integrator-history.md#bun-declares-the-twins-flags-only-in-twins-builds)) `Arguments::parse` reads them once per process with Bun's other runtime flags into `RuntimeOptions` (`src/options_types/context.rs`), and the C++ side reads them through `extern "C" const Bun::JITCacheFlags* Bun__JITCache__flags()`, declared in `JITCacheHost.h`. Every VM `configureVM` configures reads them there, workers included. Without `--jitcache`, JITCache stays off. In twins builds, when `--jitcache-twins-report` is given, the host writes `toJSON` of each configured VM's final status to stderr at exit, one line per VM, which the runner's status checks read (harness sub-SPEC section 7.7). A malformed value is a command-line error, as for any of Bun's flags.
  - The app is the entry file's. `Bun::JITCacheFlags` holds `bool hasAppDigest` and `uint64_t appDigest`, which the Rust `JITCacheFlags` mirrors as `has_app_digest` and `app_digest`, and `configureVM` sets `Config::appDigest` to `appDigest` when `hasAppDigest` holds and leaves it empty otherwise. `RunCommand::boot` (N15) sets both fields in `ctx.runtime_options.jitcache`, right before `VirtualMachine::init` and only when `--jitcache` was given, to XXH3-64 with seed 0, `bun_highway::xxhash3_64(0, bytes)` (N15), over the entry's bytes: the text `ctx.runtime_options.eval.script` holds when it is not empty, which covers `-e`, `-p`, `bun run -` and the Node REPL, and otherwise the whole file at the entry path `boot` received, as it is on disk, before any transpiling. `boot` `stat`s that path and opens it only when it names a regular file, with `O_RDONLY | O_NONBLOCK | O_CLOEXEC`, then `fstat`s the descriptor and hashes it only when it is still a regular file. An entry such as a FIFO, a terminal, or `/dev/stdin` and `/proc/self/fd/<n>` on a pipe is never opened, so `boot` neither blocks, nor takes bytes before Bun's loader reads them, nor completes a FIFO writer's `open` and then leaves it without a reader. Any other kind of entry, or a read that fails, as for an HTML glob, names no app. `RunCommand::boot_standalone`, also only when `--jitcache` was given, hashes the embedded entry's text, `graph.entry_point().contents`, right before `VirtualMachine::init_with_module_graph`. Every other main VM, `bun test`'s and `bun repl`'s among them, names no app. A worker's VM reads the same flags, so it names its process's entry: a worker runs part of the app its main VM started and shares that VM's artifact, so a digest of its own script would reject it. ([history](SPEC-integrator-history.md#bun-names-the-app-by-its-entry-file))
  - A producing role that gets `Busy` falls back to `Consumer` only in a worker whose process's main VM started producing, which `configureVM` records when the main VM's `start` returns `Started` with a producing role. The host then calls `start` once more with role `Consumer` (THREAD Session: busy records nothing), and the worker shares the main VM's opened artifact and index, which each of the main VM's commits updates at once (container sub-SPEC sections 5.1 and 8.2). Code that only workers run is therefore not captured. Any other `Busy` comes from another process holding the producer lock, a producer or maintenance, and leaves the VM unconfigured, as `Rejected` does: a `p` or `p-c` started beside another producer runs with JITCache off, and the host warns. ([history](SPEC-integrator-history.md#bun))
  - When JITCache turns itself off in a VM the host configured, the host writes one warning line to standard error, once per process whatever the number of VMs: `warn: JITCache is off: another process is writing <path>` for a `Busy` it does not fall back from, `<path>` being the resolved artifact path; `warn: JITCache is off: <step>: <detail>` for a `Rejected` start, from its `StartResult`, at `configureVM`; and, at exit, `warn: JITCache is off: <step>: <detail>` for a VM whose status holds an `activityFault`, or `warn: JITCache stopped writing the cache: <step>: <detail>` for one that holds only a `productionFault`, from that `FaultReport`. Faults are read at exit because the API makes no callbacks (THREAD Session). A `Started` at `start.replaced` is said in one line too, also once per process, at `configureVM`: `warn: JITCache replaced an incompatible cache at <path>: <detail>`. The same two lines say that the artifact was made for another app: a Consumer's `start.another-app` is a `Rejected` start, and a ConsumerProducer's replacement of such an artifact is a `start.replaced`, each with the detail of container sub-SPEC section 3.2, which gives both apps.
  - A worker VM's report paths, bench and twin alike, get the suffix `.<executionContextId>`; the main VM's are the configured paths, which is where the runner looks for them (harness sub-SPEC section 7.7).
  - Twins builds give each of those four flags the effect harness sub-SPEC section 5.1 gives it in the shell, at the matching point: per process in `JSCInitialize`, around `JSC::initialize`, for what the shell does in `jscmain`, and per VM in `configureVM` for what it does in `runJSC`. `--jitcache-twins-report` is `Config::twinReportPath` and `--jitcache-body-events` the path of the body-event dump (harness sub-SPEC section 10.3), the latter suffixed for a worker VM as the report paths are. For the main thread's VM the host calls `recordVMLayout` with `--jitcache-twins-record-layout` when it is given (harness sub-SPEC section 4). ([history](SPEC-integrator-history.md#buns-twins-build-reaches-only-exported-headers))
- `void JITCacheHost::didLoadAppBytecode(JSC::VM&, const WTF::String& sourceURL, AppBytecodeSource)`, with `enum class AppBytecodeSource : uint8_t { BytecodeBuild, CompileCache }`, both in `JITCacheHost.h`. JITCache ignores the code of a module that comes with app bytecode (THREAD Restoration), so when a configured VM loads one, the host writes one warning line, once per process whatever the number of VMs and modules. `Zig::SourceProvider::create` (N15) calls the function, on the VM's thread, when the `ResolvedSource`'s `bytecode_cache` is not null. The source it passes is `BytecodeBuild` when `already_bundled` is set or `origin_path` is not empty, and `CompileCache` otherwise (N15). Task 14 makes this edit at the top of the function, before the choice of source type, which at bun-1.4.3 sits above the two transfers: it moves up the text's `transferToWTFString()` and `sourceURLString = resolvedSource.source_url.transferToWTFString()`, then adds this call, which reads `bytecode_cache`, `already_bundled` and `origin_path` before anything takes them, `getSourceOrigin` included. The UCB lane edits the same function for a module's analysis in its task A4, which lands after task 14 and adds `importModuleAnalysis`, which may set `module_info`, right after this call and before the choice of source type (SPEC-ucb.analysis.md section 5.2); the rest of the function stays as it stands. The two edits share no variable and neither changes a field the other reads, so the order only fixes where each sits. The function returns at once when the line was written, when `Bun__JITCache__flags()` holds no artifact path, or when `JITCache::status(vm).state` is `Unconfigured`, so a process without `--jitcache` pays one test per such module. Otherwise it writes `warn: JITCache ignores code built with --bytecode: <path>; build it without --bytecode` for `BytecodeBuild`, and `warn: JITCache ignores code from the Node compile cache: <path>; set NODE_DISABLE_COMPILE_CACHE=1` for `CompileCache`, where `<path>` is the source URL: the module's file path, or its `/$bunfs/` path in a compiled executable. ([history](SPEC-integrator-history.md#bun-warns-where-it-hands-jsc-an-apps-bytecode))
- `extern "C" void Bun__JITCache__atExit(JSC::JSGlobalObject*)`, which takes `JSLockHolder` and calls `finishHostRun(vm, deltaAtExit, hostWork)` (section 3.6) with the flag's value, so `delta` runs unless `--jitcache-delta-at-exit=0`, and only for a VM whose production is still active ([history](SPEC-integrator-history.md#bun)). Its `hostWork` reads the final status, writes the warning of a fault (above), and in twins builds writes the final status line when `--jitcache-twins-report` was given and the VM started, and the body-event dump with `writeBodyEvents` when its path is set, whether or not JITCache is configured (harness sub-SPEC section 10.3). `finishHostRun` then flushes the bench report in every role. Bun destroys its main VM at exit only under `BUN_DESTRUCT_VM_ON_EXIT` (`VirtualMachine::should_destruct_main_thread_on_exit`), so `willDestroyVM` does not run by default, and without this flush a consumer, which never calls `delta`, would lose every event still buffered. `VirtualMachine::on_exit` (`src/jsc/VirtualMachine.rs`) calls it right after `ExitHandler::dispatch_on_exit(self)`, on the main thread and in workers, so the user's exit handlers have run and teardown has not begun: the deferred tasks, the handle's stop, the Node-API cleanup hooks and the main thread's compile cache and bytecode order file all come after it (N15). Process exit is thus Bun's idle point, as it is for the Node compile cache (N15); THREAD's "no implicit `delta`" binds JITCache, and this call is the host's.
- `extern "C" int Bun__JITCache__runMaintenance(int argc, const char* const* argv)` for `bun jitcache compact [<ratio>] [<path>] [--yes | --no]`, which passes `Maintenance::RatioArgument::Optional` to `runCommandLine`, so the ratio may be left out for 0 (maintenance sub-SPEC section 5). Bun has no other maintenance command.

`scripts/build/deps/webkit.ts` passes `-Wl,--build-id=sha1` in `CMAKE_EXE_LINKER_FLAGS` for local Linux builds, so the `jsc` executable carries a build ID as Bun's own executable does (N15, THREAD Storage). No other compile or link flag changes, in any profile, except in twins builds of Bun, which leave `BUN_DYNAMIC_JS_LOAD_PATH` out and pass `--cfg=jitcache_twins` to rustc (harness sub-SPEC section 1): THREAD Verification keeps Bun's build flags, under which the engine sits at its link-time address (N15), and the twins profile otherwise only switches the test builds on, in Rust through that cfg as in C++ through `ENABLE_JITCACHE_TWINS`.

## 12. Requirements on the other parts

The integrator owns these interfaces; each requirement names what the parts provide or the call they make into the integrator's API.

- R-ALL-1. Every call of `didFailExecutableAllocation` passes its site (section 4.5): SPEC-ics.md E5 `ExecutableAllocationSite::InlineCacheHandler`, SPEC-image.md's `MathICRegeneration::didFailToAllocate` `ExecutableAllocationSite::MathICSnippet`.
- R-ALL-2. Every twin check reports through `TwinReport::difference`, `skip` and `relocationCoincidence` (harness sub-SPEC section 2), and only while `VMState::twinReportSink()` is non-null.
- R-ALL-3. Every part's C++ tests register with `JITCACHE_TEST` or `JITCACHE_TEST_WITH_OPTIONS` and live in the files the `testjitcache` target lists (harness sub-SPEC section 8); a test that needs process-wide options declares them in its registration.
- R-ALL-4. Every part's JS tests follow the runner's directives and argument conventions (harness sub-SPEC section 7). Since the oracle compares the heap JavaScript can reach as well as the output (harness sub-SPEC sections 6 and 7.6), a script prints only values that do not depend on its role and leaves none reachable when it ends: a jsc-hosted script runs its body in a function that receives the role and paths as parameters. Under the `Off` role, which the oracle's runs pass (harness sub-SPEC section 7.6), a script runs its Consumer path without `delta` and without any assertion about the state JITCache imported, seeded, attached or captured; assertions about native behavior, such as results, a thrown error or Bun's `cachedDataRejected`, run in every role. Plain builds register none of the jsc functions, `bun:jsc` exports and `$vm` helpers of harness sub-SPEC sections 5.2 to 5.4, so a script that calls one declares `jitcache-requires: twins`, which keeps it out of plain mode, or calls it only after testing that it exists. The pin has none of them either, so a jsc-hosted script whose `Off` run calls one unguarded, or whose optimizing compiles the two native fixes change, declares `jitcache-pin: off` with its reason (harness sub-SPEC sections 7.2 and 11.4). The runner fails a run that reports a fault or a `Consumer` or `ConsumerProducer` run that installs no body (harness sub-SPEC section 7.5), so a script declares with `jitcache-expect-fault` each fault one of its runs reports on purpose, and with `jitcache-expect-no-install` each `Consumer` or `ConsumerProducer` run it leaves with nothing to install. ([history](SPEC-integrator-history.md#the-runner-fails-undeclared-faults-and-runs-that-install-nothing))
- R-ALL-5. The UCB engine calls `bodyVersion` and `openBody` on the VM thread without holding its registry lock, and reads `Unusable` as "the integrator raised the fault" (SPEC-ucb.md sections 6.3 and 7.3.1).
- R-ALL-6. `description(ImageCheck)`, `description(CBCheck)`, `description(ICs::Check)` and `description(ICs::CaptureCheck)` return `ASCIILiteral`s with static storage, which a `FaultReport` keeps (SPEC-cb.md section 6.1 and SPEC-ics.md sections 4.4 and 5.6 declare theirs so). Each is one hyphenated token that says what the check rejects and cites no rule ID, such as `state-allocation-hint`, because `status` shows the step, here `cb.state-allocation-hint`, and a runner directive names it as one word (harness sub-SPEC section 7.2). A fault's detail names its body as SPEC-ucb.md section 7.3's diagnostics do: any words first, then `; body ` and the body's key in hex (`; saved body ` for a saved summary rejected at scoring). A `cb` fault's words are `faultDetail(CBCheck)` (SPEC-cb.md R-INT-8); an `image` fault has none, since its token is the whole description (SPEC-image.md section 8.5), so its detail is `body ` and the key. Runner directives and hosts match on the step alone.
- R-ALL-7. A part's test that needs a body no producer committed builds it with `ValidatedBody::createForTesting` and, when the code under test looks bodies up, serves it with `VMState::setBodyLookupForTesting` (section 6.2), in twins builds: SPEC-ucb.md U4 builds the pending import whose body asserts in its destructor, and U8 stubs the lookup.
- R-ALL-8. Unified sources compile several `jitcache/<part>/*.cpp` files of different parts as one translation unit, where their anonymous namespaces merge and their file-static names meet (M1). Every helper and constant at namespace scope that a part's file keeps to itself, `static` or in an anonymous namespace, therefore has a name that begins with its part's prefix (`ucb`, `image`, `cb`, `ics`, `integrator`) or sits in a per-file named namespace such as `JSC::JITCache::ContainerInternal`. Section 14.1 holds the integrator's files to it.
- R-ALL-9. Every part's code meets the platform rule of section 11, so that Bun's builds for other platforms compile it.

## 13. Invariants

- II1. A VM has at most one `VMState`. `start` creates it for `Started` and `Fault` and for no other outcome, and `didFinalizeHeap` destroys it after `Heap::shutDown` (sections 3.2 and 4.6).
- II2. Activity and production only go from on to off, and production is off whenever activity is (section 4.2).
- II3. `producerContext(vm)` is non-null exactly while the role produces, activity is on and production is active, and the object it returns lives until the state is destroyed (section 4.4).
- II4. Every byte the integrator allocates for production is charged before the allocation and released exactly once; section 4.4 lists what is not production memory.
- II5. Every record below the pack's valid end was published whole: the writer stores a root only after the record it covers was reread and validated and its index block written. The writer reads no lane format (container sub-SPEC section 8.2).
- II6. No byte below a pack's valid end changes in place, apart from the root and the retired word: a key gets a new body by an appended record, and a compaction or a whole deletion writes a new file and renames it over the old one (container sub-SPEC sections 8.2 and 8.4).
- II7. At most one VM or maintenance run holds the producer lock, and a producing VM holds it from `start` until its state is destroyed, unless `start` faulted (section 3.2; container sub-SPEC section 2).
- II8. Unless a test's lookup override is set, `bodyVersion` reads no file: it returns the commit identifier of the key's newest index entry and 0 when the index lists no body for the key, and it makes a system call only when the root moved or the mapped pack is marked retired (section 6.2; container sub-SPEC section 6).
- II9. A `ValidatedBody`'s bytes stay readable and unchanged while it lives: it holds a reference to the mapping they lie in (section 6.2; container sub-SPEC section 7.2).
- II10. The install glue acts only on a newborn CB (JIT type `None`, no `BaselineJITData`, not yet installed) whose UCB's sharing slot is empty and holds a pending import (section 7.2, step 1).
- II11. The install glue writes nothing to the CB before every fallible step has succeeded, and after the first write no step can fail (section 7.2).
- II12. The install glue calls the lanes in the order of section 7.2, which is THREAD's.
- II13. A body is written only when its capture beats the saved body under `beats`, by its live score and again by the score of the bytes built (section 8.4, step 6), and a kept summary always describes the body its key's newest index entry names (section 8.3). A saved body that a transient error keeps from being read defers its key's capture and never scores as absent.
- II14. Capture runs only on the VM thread, with JS paused, heap access and no collector phase on the thread; a capture that writes runs inside a GC deferral, door 1's or `delta`'s, and section 10 gives the one route that reaches the finalize hook without one, which writes nothing. It allocates no cell, stops for no collector, finalizes no plan and calls no host or JS function, so captures never nest; neither the finalize hook nor `didFailExecutableAllocation` asserts a deferral (sections 8.7 and 10).
- II15. Every `didFailExecutableAllocation` call precedes the effects of the failure it reports (sections 4.5 and 9).
- II16. Every VM's lookups see a commit, by a VM of their process or of another, from the first lookup after the commit stored its root, since every read loads the root first (container sub-SPEC section 6). They see the pack a compaction or a whole deletion left from the first lookup after the rename: a reader that finds its mapped pack marked retired looks at the path at every read for one second, and once per `retiredPackReopenIntervalMilliseconds` after that, until the path names another file or none.
- II17. The header's build IDs cover the main executable and the object holding `codeSymbolAnchor` (section 5.2).
- II18. No header in `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` includes an unexported `jitcache/` header (section 3.5; test T-HEADERS).
- II19. Neither VM destruction nor process exit commits a body on JITCache's own initiative: only a `delta` the host asks for, through `finishHostRun` or directly, does (THREAD Failures; sections 3.6 and 4.6).
- II20. Maintenance never leaves an incomplete body: a compaction renames a complete pack over the old one, and deleting a whole artifact empties the pack before it removes the header and keeps the header when the emptying fails (maintenance sub-SPEC section 4.5).
- II21. The VMs of a process that open one artifact share one `OpenedArtifact`, which is built and follows the root the same way whatever their roles (container sub-SPEC section 5.1).
- II22. For each CB the capture glue scores or captures, it calls the ICs lane before the CB lane in the same pause and passes the ICs lane's `hasPolymorphicSite` to the CB lane, so a body with a polymorphic site scores and commits its counter as withheld, carries no baseline counter progress and has an envelope P of zero (sections 8.2 and 8.4).
- II23. A committed body's kept score equals what `scoreSections` computes from its bytes in the pack: it is built from the scores the lanes' builders return for the bytes they wrote, and no capture reads its own bytes back (section 8.4, step 6).

## 14. Files, native edits and the manifest

No other part edits the functions of section 14.2. Implementers may not edit the shared hot files; integrator tasks apply the entries of section 14.4, the manifest for `INTEGRATE-integrator.md`, which gather every part's manifest. Each entry names the one task that applies it (section 17), and that task comes after the tasks that write whatever the entry references, so the tree builds after every task.

### 14.1 New files

JITCache's code lives in `Source/JavaScriptCore/jitcache/`, one directory per lane (`ucb/`, `image/`, `cb/`, `ics/`, which the lane SPECs list) and per integrator part. The integrator's directories and files, all in namespace `JSC::JITCache`:

| directory | file | contents |
|---|---|---|
| `base/`: what every part may include | `BodyKey.h`, `.cpp` | owned by the UCB lane (SPEC-ucb.md section 15.1): `BodyKey`, `Digest256`, `IdentityKind` and `ChildTable`, the types a key is made of (SPEC-ucb.md section 3); `UCBKeys` keeps key derivation |
| | `JITCacheSHA256.h`, `.cpp` | owned by the UCB lane (SPEC-ucb.md section 15.1): SHA-256 (SPEC-ucb.md section 3.8) |
| | `ByteFields.h` | `readField` and `writeField`, the little-endian reads and writes of fixed-offset fields in byte spans, and `isAllZero` |
| | `CodeSymbolAnchor.h` | the declaration of `codeSymbolAnchor`, under `ENABLE(JIT)`, which the Image lane defines (SPEC-image.md section 3.5) and the store reads for the engine's build ID (section 5.2) |
| `store/`: the artifact on disk, VM-independent | `JITCacheContainer.h`, `.cpp` | the header, pack and body formats, the section type ids, validation (container sub-SPEC sections 3 and 4) |
| | `JITCacheFiles.h`, `.cpp` | the file helpers the store, `start` and maintenance share: `openAt`, which adds `O_CLOEXEC` and retries `EINTR`, `readExactly` and `writeExactly` at an offset, `errorText`, and `readHeaderFile`, the one read of `cache/header`, which opens it with `O_NONBLOCK` and reads only a regular file (section 3.2, step 7); no header of another part includes it |
| | `ArtifactAccess.h`, `.cpp` | `openArtifact`, `ArtifactAccessMode`, `ArtifactAccess` and `ArtifactAccessFailure`: how `start` creates, opens or replaces the artifact (section 3.2, step 7; container sub-SPEC sections 1.3 to 1.5) |
| | `ArtifactStore.h`, `.cpp` | `ProducerLock`, `ArtifactRegistry`, `MappedPack`, `OpenedArtifact`, the index, the store's reads (tokens, opened bodies, saved scores), and in twins builds the store's fault hook and registry hook (container sub-SPEC sections 2 and 5 to 7) |
| | `ArtifactWriter.h`, `.cpp` | the writer, the compaction and the types the capture glue hands them (section 6.3), and in twins builds the fault injection, kill points and `rewriteSection` (container sub-SPEC section 8) |
| | `ValidatedBody.h`, `.cpp` | `SectionKind`, `ValidatedBody`, `BodyLookup` and, in twins builds, `ValidatedBody::createForTesting` (sections 6.1 and 6.2) |
| | `JITCacheOptions.h`, `.cpp` | the option table and its check (section 5.1) |
| | `JITCachePlatform.h`, `.cpp` | build IDs, the CPU feature vector, the container's CRC-32C (section 5.2, container sub-SPEC section 4.4) |
| | `JITCacheParameters.h` | the named parameters of section 16 |
| `session/`: one VM's configuration and state | `JITCacheAPI.h`, `.cpp` | the public interface of section 3; the `.cpp` defines `start`, `status`, `flushBenchReport`, `finishHostRun`, the `toJSON` functions and the teardown hooks (sections 3 and 4.6), and `JITCacheCapture.cpp` defines `delta` |
| | `JITCacheBun.h` | the entry points only Bun's integration calls (section 3.1), which the UCB lane defines |
| | `JITCacheVMState.h`, `.cpp` | `VMState` (section 4.1): its constructor and destructor, the switches, the fault records and the members that raise faults, `releaseEndedProductionMemory`, the body lookups of section 6.2 and, in twins builds, their override |
| | `JITCacheFaults.h`, `.cpp` | `ExecutableAllocationSite`, `didFailExecutableAllocation`, `didAttachDebugger` (section 4.5) |
| | `ProducerBudget.h`, `.cpp` | `ProducerBudget`, `ProducerContext`, `producerContext` (section 4.4) |
| | `JITCacheBench.h`, `.cpp` | the bench report (harness sub-SPEC section 9) and `BaselineSpanTimer` (section 16) |
| `glue/`: the only directory that includes several lanes | `JITCacheGlue.h` | the entry points native code calls: install, capture, teardown and bench hooks, and in twins builds the image check's hook (harness sub-SPEC section 3) |
| | `JITCacheInstall.cpp` | the install glue (section 7) and, in twins builds, the image twin-check state (harness sub-SPEC section 3) |
| | `JITCacheCapture.h`, `.cpp` | the capture glue and `delta` (section 8); the header declares the scores of section 8.2, which the tests call, and in twins builds `rewriteSectionForTesting` and `keptScoreForTesting` (section 8.3) |
| `maintenance/` | `JITCacheMaintenance.h`, `.cpp` | the maintenance backend and command line (maintenance sub-SPEC) |
| | `JITCacheMaintenanceTesting.h` | maintenance's test hooks (maintenance sub-SPEC section 7), twins builds only |
| `harness/`: twins builds only | `TwinReport.h`, `.cpp` | `TwinReport` (harness sub-SPEC section 2) |
| | `JITCacheBodyEvents.h`, `.cpp` | the VMs' retired body-event totals, `retireBodyEventCounts` and the totals' erasure at `didFinalizeHeap` (harness sub-SPEC section 10.3) |
| | `JITCacheTwinsHost.h` | what a host calls in twins builds (section 3.5) |
| | `JITCacheTwinsHarness.h` | what only the jsc shell calls, beside `JITCacheTwinsHost.h`: the reachable-heap description, `writeTestFile`, `setForcesBlindingForTesting` and `bool setImageTestHookNamed(const char*)`, which maps a hook's run-flag name to the Image lane's `ImageTestHook`, calls `setImageTestHook` (SPEC-image.md section 11.3) and returns false for an unknown name, so the shell does not include the Image lane's header (harness sub-SPEC sections 5, 6 and 11.5) |
| | `Placement.cpp` | the placement and layout functions of `JITCacheTwinsHost.h` and `writeTestFile` (harness sub-SPEC section 4) |
| | `HeapDescription.cpp` | the description of the heap JavaScript can reach (harness sub-SPEC section 6) |
| | `ShellFunctions.cpp` | the shell's test flags, its start record, the body-event dump, the host functions of harness sub-SPEC section 5.2, the ICs snapshot among them, `setForcesBlindingForTesting`, `forcesBlindingForTesting`, which `MacroAssembler.h` declares, and `setImageTestHookNamed` (harness sub-SPEC sections 5 and 11.5) |
| `tests/` | `testjitcache.cpp`, `support/JITCacheTest.h`, `support/TestArtifact.h`, `support/TestFiles.h`, `support/TestVM.h` | the C++ test framework and runner, and the fixtures every part's tests share (harness sub-SPEC section 8) |
| | `store/ContainerTests.cpp`, `store/PlatformTests.cpp`, `store/StoreTests.cpp`, `store/WriterTests.cpp`, `session/SessionTests.cpp`, `glue/CaptureTests.cpp`, `maintenance/MaintenanceTests.cpp`, `harness/HarnessTests.cpp` | the integrator's C++ tests, one directory per part; each lane's tests live in `tests/<lane>/`, and `tests/base/BaseTests.cpp` holds the UCB lane's tests of `base/` (SPEC-ucb.md section 13.1) |

Every subdirectory of `jitcache/` is on JSC's private include path (M2), so an `#include "X.h"` names a header by its file name alone. The directories set who may include whom:

| directory | headers other parts include | it includes, besides WTF and the engine |
|---|---|---|
| `base/` | all of them | nothing of JITCache |
| `store/` | lanes: `ValidatedBody.h`; session, glue and maintenance: the rest | base; session's `ProducerBudget.h` and `JITCacheBench.h`, which the writer charges and times its buffers and commits with, and which include only WTF besides each other |
| `session/` | lanes and engine: `JITCacheVMState.h`, `JITCacheFaults.h`, `ProducerBudget.h`, `JITCacheBench.h`; hosts: `JITCacheAPI.h`, and Bun's integration `JITCacheBun.h` | base, store, `harness/TwinReport.h`; `ucb/UCBRegistry.h` only from `JITCacheVMState.cpp`; `JITCacheAPI.cpp` also `glue/JITCacheGlue.h`, which declares the teardown hooks it defines, and in twins builds `harness/JITCacheBodyEvents.h` |
| `ucb/`, `image/`, `cb/`, `ics/` | glue and the engine's edit sites: the lane's interface headers | base, store's `ValidatedBody.h`, the lane-facing headers of session, `harness/TwinReport.h`, and from `ucb/UCBAnalysis.h` alone `session/JITCacheBun.h`, whose entry the UCB lane defines; never another lane, so a lane names a body by key through `VMState::bodyKeyOf` and `base/BodyKey.h` |
| `glue/` | engine: `JITCacheGlue.h`; the harness: `JITCacheCapture.h` | everything above |
| `maintenance/` | hosts: `JITCacheMaintenance.h` | base, store, `session/JITCacheAPI.h` |
| `harness/` | lanes and session: `TwinReport.h`; hosts: `JITCacheTwinsHost.h`, `JITCacheTwinsHarness.h` | anything |

Of these, `JITCacheAPI.h`, `JITCacheBun.h`, `JITCacheMaintenance.h` and `JITCacheTwinsHost.h` are exported to Bun (section 3.5).

Outside `jitcache/`: `Tools/Scripts/run-jitcache-tests` (the runner's entry point) and its modules in `Tools/Scripts/jitcache/` (`build.ts`, `scripts.ts`, `runs.ts`, `oracle.ts`, `bench-report.ts`, `microbench.ts`, and `pin-compare.ts`, the pin comparison the runner calls; harness sub-SPEC sections 7 and 11), `JSTests/jitcache/resources/jitcache.js` (the checks, the runner's arguments and the shell wrappers every part's JS tests load), `JSTests/jitcache/integrator/` (the integrator's JS tests, with `resources/` and the runner's own tests in `runner/`), and `PerformanceTests/JITCache/` (the macro bench driver `bench.ts` and the installation-bound bench `bound.ts`, section 16). The integrator's files follow R-ALL-8 with the prefix `integrator` or a per-file named namespace such as `JSC::JITCache::ContainerInternal`, as `Heap.cpp`'s `HeapInternal` does, so nothing at namespace scope has a generic name.

### 14.2 Native edits in this repository

| file | function or member | edit | context |
|---|---|---|---|
| `runtime/ScriptExecutable.cpp` | `ScriptExecutable::prepareForExecutionImpl` | install point before `setupLLInt`, the trailing `installCode` skipped after an import, the twin check at the end (section 7.1) | VM thread, `DeferGCForAWhile` |
| `runtime/ScriptExecutable.cpp` | `ScriptExecutable::installCode(VM&, CodeBlock*, CodeType, CodeSpecializationKind, Profiler::JettisonReason)` | a bench timer around `unlinkOrUpgradeIncomingCalls` (harness sub-SPEC section 9.3) | VM thread, or a collection's End phase (harness sub-SPEC section 9.3) |
| `jit/JIT.cpp` | `JIT::compileSync` | the LLInt-off install point at its top (section 7.1) | VM thread |
| `jit/BaselineJITPlan.h`, `.cpp` | `BaselineJITPlan::BaselineJITPlan(CodeBlock*)`, `BaselineJITPlan::finalize`, `BaselineJITPlan::compileInThreadImpl(JITCompilationEffort)`, four new members and `jitCacheRecordsImage()` | whether the compilation records (section 4.4, SPEC-image.md R-INT-12: the constructor sets `m_jitCacheRecordsImage`), the plan-site fault (section 9), the finalize capture (section 8.5), bench timing (harness sub-SPEC section 9.3: the constructor sets `m_jitCacheMeasure`, and a `BaselineSpanTimer` of `JITCacheBench.h` measures each span) | VM thread for the constructor and `finalize`; compile thread for `compileInThreadImpl` |
| `dfg/DFGPlan.h`, `.cpp` | `DFG::Plan`: new `noteExecutableAllocationFailure()` and `m_failedForLackOfExecutableMemory`; `Plan::compileInThreadImpl` (the two `state.allocationFailed` branches); `Plan::finalize` | the plan-site fault (section 9); in twins builds, `Plan::finalize` also counts a successful optimizing compile (harness sub-SPEC section 10.1) | compile thread; VM thread for `finalize` |
| `dfg/DFGSpeculativeJIT.cpp` | `SpeculativeJIT::compile`, `SpeculativeJIT::compileFunction` (the `linkBuffer.didFailToAllocate()` branches) | note the failure on the plan (section 9) | compile thread |
| `dfg/DFGOSRExitCompilerCommon.cpp` | `handleExitCounts` | twins builds: emit the increment of the body's `osrExits` first, right after the return for exits that cannot jettison (harness sub-SPEC section 10.1) | VM thread, compiling an exit; the code runs on the VM thread |
| `bytecode/CodeBlock.cpp` | `CodeBlock::jettison` | twins builds: count the jettison and the reoptimization (harness sub-SPEC section 10.1) | VM thread, or a collection's End phase on the thread that drives it |
| `llint/LLIntOfflineAsmConfig.h` | new `OFFLINE_ASM_JITCACHE_TWINS`, from `ENABLE(JITCACHE_TWINS)` | the offlineasm setting the LLInt's count tests (harness sub-SPEC section 10.1) | build time |
| `llint/LowLevelInterpreter.asm` | new macro `countJITCacheInstruction`; macro `traceExecution`; labels `_llint_op_call_direct_eval`, `_wide16` and `_wide32` | under `if JITCACHE_TWINS`, add one to the frame's UCB's `llintInstructions`, from `traceExecution` and at the head of the three labels (harness sub-SPEC section 10.1) | VM thread, at every LLInt instruction |
| `assembler/MacroAssembler.h` | `MacroAssembler::shouldConsiderBlinding`; the declaration of `JSC::JITCache::forcesBlindingForTesting()` with `JS_EXPORT_PRIVATE` | twins builds: forced blinding (harness sub-SPEC section 11.5) | any thread that emits code |
| `jit/JITThunks.h`, `.cpp` | `JITThunks`: new `m_supportGenerations`, `noteSupportGeneration()` and `supportGenerations()`; `JITThunks::ctiStubImpl`, `JITThunks::lazyCommonThunk` | count each generation of per-VM support for the bench (harness sub-SPEC section 9.3) | VM thread or compile thread, under `m_lock` or the lazy thunk's lock |
| `bytecode/InlineCacheHandler.cpp` | `InlineCacheCompiler::generateSlowPathHandler` | count a handler the VM lacked through `vm.jitStubs->noteSupportGeneration()` (harness sub-SPEC section 9.3) | VM thread |
| `llint/LLIntSlowPaths.h`, `.cpp` | `shouldJIT` | external linkage, declared in the header (section 7.1) | VM thread |
| `debugger/Debugger.cpp` | `Debugger::attach` | `JITCache::didAttachDebugger(m_vm)` right after `setShouldBuildPCToCodeOriginMapping` (section 4.5) | any thread |
| `jsc.cpp` | `CommandLine` (new fields), `CommandLine::parseArguments`, `printUsageStatement`, `runJSC`, `jscmain`, `GlobalObject::finishCreation`, and the new file-static `startJITCache`, `finishJITCacheRun`, `runJITCacheMaintenance` and `printJITCacheLine` | the jsc host (section 11.1, harness sub-SPEC sections 4 to 6, 10 and 11.5, and the kill flag of harness sub-SPEC section 12) | the shell's main thread |

### 14.3 Edits in `~/bun`

| file | function or member | edit | context |
|---|---|---|---|
| `src/runtime/cli/Arguments.rs`, `src/options_types/context.rs` | `RUNTIME_PARAMS_`, `Arguments::parse`, `RuntimeOptions`, `JITCacheFlags` | the JITCache flags (section 11.2), parsed once per process with Bun's other runtime flags, the app digest's two fields, and `Bun__JITCache__flags`, through which the C++ side reads them | the CLI's thread, before any VM |
| `src/runtime/cli/run_command.rs` | `RunCommand::boot`, `RunCommand::boot_standalone` | the app digest of the entry (section 11.2) | the CLI's thread, before the main VM |
| `src/jsc/bindings/JITCacheHost.h`, `.cpp` (new) | `Bun::JITCacheHost` | configuration from Bun's JITCache flags, the app digest included, `start` with the worker fallback, `delta` and the bench report's flush at exit, the maintenance command, the warning about app bytecode (section 11.2); in twins builds, the main thread's VM layout record (harness sub-SPEC section 4) and the body-event dump at exit (harness sub-SPEC section 10.3) | as its callers below |
| `src/jsc/bindings/ZigSourceProvider.cpp` | `Zig::SourceProvider::create` | `JITCacheHost::didLoadAppBytecode` for a `ResolvedSource` that carries app bytecode, before the UCB lane's analysis import in the order section 11.2 gives (SPEC-ucb.analysis.md section 7 lists the UCB lane's edits of the file) | the VM's thread, loading a module |
| `src/jsc/bindings/ZigGlobalObject.cpp` | `Zig__GlobalObject__create` | `Bun::JITCacheHost::configureVM(vm, executionContextId)` right after `JSC::JSLockHolder locker(vm)` | the new VM's thread, API lock and heap access held, before any global object |
| `src/jsc/bindings/ZigGlobalObject.cpp` | `JSCInitialize` | twins builds: address-space placement around `JSC::initialize` (harness sub-SPEC section 4), through `<JavaScriptCore/JITCacheTwinsHost.h>` (section 3.5) | once per process, inside its `std::call_once`, before any VM |
| `src/jsc/bindings/ZigGlobalObject.cpp` | the comment above `defaultGlobalObject` | the `executionContextId` values `VirtualMachine::init` gives (N15): 1 for the main thread, `INT32_MAX` for macros and the debugger thread, above 1 for workers, which `JITCacheHost` classifies VMs by (section 11.2) | |
| `src/jsc/VirtualMachine.rs` | `VirtualMachine::on_exit` | `Bun__JITCache__atExit(self.global())` right after `ExitHandler::dispatch_on_exit(self)` | the VM's JS thread, after the exit handlers and before teardown |
| `src/options_types/command_tag.rs` | `Tag`, `Tag::char`, `Tag::ALL` | `Tag::JITCacheCommand` (maintenance sub-SPEC section 5) | build time |
| `src/runtime/cli/mod.rs`, `src/runtime/cli/jitcache_command.rs` (new) | `which()`, the dispatch in `Command::start`, `tag_print_help` | `bun jitcache` (maintenance sub-SPEC section 5) | the CLI's thread; no VM |
| `src/jsc/modules/BunJSCModule.h` | `DEFINE_NATIVE_MODULE(BunJSC)` | twins builds: six more exports (harness sub-SPEC section 5.3) | the VM thread |
| `scripts/build/deps/webkit.ts`, `scripts/build/profiles.ts`, `scripts/build/config.ts` | the local WebKit recipe, the profiles and the resolved config | the build-id link flag and the twins profile; Bun's compile and link flags stay as they are (section 11.2, harness sub-SPEC section 1) | build time |
| `scripts/build/config.ts` | `findRepoRoot` | when no Bun checkout lies above the working directory, the walk starts again from `config.ts`'s own directory, `import.meta.dirname`, because ninja's regeneration of `build.ninja` reruns the builder from the build directory, which `build.ts`'s build root puts outside the checkout | build time |
| `scripts/build/deps/webkit.ts` | the local WebKit recipe, in a Linux build against a sysroot (`cfg.crossTarget` set, Android aside) | the `--target` and `--sysroot` that `optFlags` passes, which Bun's global flags give its other dependencies and the recipe's `CMAKE_C_FLAGS` would otherwise drop, and the CMake cross-sysroot settings (`CMAKE_SYSTEM_NAME`, `CMAKE_SYSTEM_PROCESSOR`, `CMAKE_SYSROOT` and the four `CMAKE_FIND_ROOT_PATH_MODE_*`), because `build.ts --arch=aarch64` cross-builds every target through them on an x86_64 host, `bun-ci-release` among them | build time |
| `scripts/build/flags.ts` | the entry that defines `BUN_DYNAMIC_JS_LOAD_PATH` | its condition also requires `!c.jitcacheTwins` (harness sub-SPEC section 1) | build time |
| `scripts/build/rust.ts` | the rustflags beside its other custom cfgs | `--check-cfg=cfg(jitcache_twins)` in every build and `--cfg=jitcache_twins` when `cfg.jitcacheTwins` holds (section 11.2, harness sub-SPEC section 1) | build time |
| `test/js/bun/jitcache/` (new) | Bun tests | section 15.3 | |

The source digests Bun supplies (`ResolvedSource`, the standalone graph's record, the transpiler cache's metadata, the internal modules' digest table and the providers that carry them) are SPEC-ucb.md section 7.2.6's edits, which that SPEC lists; the Bun host adds none.

In this repository's root, `build.ts` gains the `twins` and `bun-twins` targets (harness sub-SPEC section 1) and the `pin` target, which builds the pin from a worktree of this repository (harness sub-SPEC section 11.1). The human edits `CLAUDE.md`, whose build table lists these three targets and the `JITCACHE_PIN_SOURCE` override, and whose toolchain lines follow `build.ts` to LLVM 23 (below): the builder requires LLVM 23.1.x, `JITCACHE_LLVM_PREFIX` defaults to `$HOME/collo-local/tools/llvm-23` in the package layout `usr/lib/llvm-23/bin`, and the ASan paragraph's symbolizer is the prefix's `usr/lib/llvm-23/bin/llvm-symbolizer`, which loads `libLLVM.so.23.1`.

`build.ts` names the pins it checks: `BUN_PIN`, `bun-v1.4.3` `c6da4a4d3010e5553438c60f6bd76d981976867c`, which the Bun checkout's history must contain, and `WEBKIT_PIN`, `0c06faadf65bf8e8c8ad3a5a8aca83e1e9ed653f`, the webkitbun that pin declares, which the `pin` target builds. Every target builds with LLVM 23, since Bun's builder refuses a clang whose major differs from rustc's (N15): `JITCACHE_LLVM_PREFIX` defaults to `~/collo-local/tools/llvm-23`, in the package layout, and `build.ts` puts its `usr/lib/llvm-23/bin` on `PATH` and its `usr/lib/<multiarch>`, which holds `libLLVM.so.23.1`, and `usr/lib/llvm-23/lib` on `LD_LIBRARY_PATH`. The targets `ci-release` and `bun-ci-release` pass `--profile=release --ci=on --buildkite=on`, the replacement Bun names for its retired `ci-release` profile (N15), with `--webkit=local` as every target does, into the build directory `linux-<arch>-ci-release` they used before; `build.ts` keeps clearing `CI`, `BUILDKITE` and `GITHUB_ACTIONS` from the builder's environment, so the flags set the build's semantics (LTO and Bun's target settings) while the builder stays on its local path, without packaging, uploads or an inherited symbol order file.

### 14.4 Hot-file edits

- M1 (task 1). `Source/JavaScriptCore/Sources.txt` gains SPEC-image.md M1's `jit/JITMathIC.cpp` in `jit/`'s sorted place, and one `jitcache/` block, sorted by path, of the files of SPEC-ucb.md M1, SPEC-image.md M1, SPEC-cb.md M2 and SPEC-ics.md M1, each under its lane's directory (SPEC-ucb.md M1 names `base/`'s two `.cpp` files, `BodyKey.cpp` and `JITCacheSHA256.cpp`, which its lane writes), and the integrator's: `jitcache/store/ArtifactAccess.cpp`, `ArtifactStore.cpp`, `ArtifactWriter.cpp`, `JITCacheContainer.cpp`, `JITCacheFiles.cpp`, `JITCacheOptions.cpp`, `JITCachePlatform.cpp` and `ValidatedBody.cpp`; `jitcache/session/JITCacheAPI.cpp`, `JITCacheBench.cpp`, `JITCacheFaults.cpp`, `JITCacheVMState.cpp` and `ProducerBudget.cpp`; `jitcache/glue/JITCacheCapture.cpp` and `JITCacheInstall.cpp`; `jitcache/maintenance/JITCacheMaintenance.cpp`; and `jitcache/harness/HeapDescription.cpp`, `JITCacheBodyEvents.cpp`, `Placement.cpp`, `ShellFunctions.cpp` and `TwinReport.cpp`. The twins-only files compile to nothing without `ENABLE(JITCACHE_TWINS)`. The tests under `jitcache/tests/` are not in `Sources.txt`, and a comment at the block's head says so. Task 1 creates each listed `.cpp` file that does not exist yet as a stub holding `#include "config.h"` only, and the owning part's task replaces it. JITCache's files carry no header, no license and no copyright, only comments that explain code.
- M2 (task 1). `Source/JavaScriptCore/CMakeLists.txt`: each of the ten subdirectories of `jitcache/` (`base`, `cb`, `glue`, `harness`, `ics`, `image`, `maintenance`, `session`, `store`, `ucb`), as `"${JAVASCRIPTCORE_DIR}/jitcache/<directory>"`, in `JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES` in sorted order (UCB M2, Image M2, CB M3), as `b3/air` and `wasm/js` are, so every `#include "X.h"` names a JITCache header by its file name; and `jitcache/harness/JITCacheTwinsHost.h`, `jitcache/maintenance/JITCacheMaintenance.h`, `jitcache/session/JITCacheAPI.h` and `jitcache/session/JITCacheBun.h` in `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`. No lane exports a header (SPEC-ucb.md M2, SPEC-image.md section 14.4, SPEC-ics.md M1). These four are the only headers the build reads before their owners' code lands, so none is stubbed: task 1 runs after the integrator's task 0, which writes them.
- M3 (task 1). `runtime/VM.h` (UCB M3 and R-INT-1): `namespace JITCache { class VMState; }` beside the other forward declarations, the public inline `JITCache::VMState* jitCacheState() const { return m_jitCacheState.load(std::memory_order_acquire); }` and `void setJITCacheState(JITCache::VMState* state) { m_jitCacheState.store(state, std::memory_order_release); }`, and the private member `std::atomic<JITCache::VMState*> m_jitCacheState { nullptr };`, declared as `VM`'s last data member, so that no field offset emitted code bakes moves (compiler.md, "What the code embeds"; harness sub-SPEC section 11). ([history](SPEC-integrator-history.md#the-pin-comparison-names-addresses-itself))
- M4 (task 7). `runtime/VM.cpp`: under `ENABLE(JIT)` and with `#include "JITCacheGlue.h"`, `JITCache::willDestroyVM(*this);` right after the `cancelAllPlansForVM` block of `VM::~VM`, and `JITCache::didFinalizeHeap(*this);` right after `heap.shutDown();` (section 4.6). Task 7 defines both functions in `JITCacheAPI.cpp` with `start` and the state's destructor (section 4.1), so the engine links at every step, and no VM before task 7 has a state to tear down.
- M5 (task 1). `bytecode/CodeBlock.h`: SPEC-cb.md M1, verbatim (`CodeBlock::seedBaselineTierUpHistory`, and `previousCounterForAging` under `ENABLE(JITCACHE_TWINS)`), both defined inline.
- M6 (task 1). `parser/SourceProvider.h`: SPEC-ucb.md M6, verbatim (`SourceProvider::jitCacheSourceDigest()`, whose inline default returns `std::nullopt`).
- M7 (task 12). `tools/JSDollarVM.cpp`: SPEC-ucb.md M4 (`$vm.jitCacheUCBStatistics(options)`), under `ENABLE(JITCACHE_TWINS)`, with `#include "JITCacheTwinsHost.h"`. Its host function calls `JITCache::functionJITCacheUCBStatistics`, which `ShellFunctions.cpp` defines and `bun:jsc` registers too, so both hosts build the object in one place; task 12 runs after the UCB lane's task that defines the statistics it reads.
- M8 (task 1). `Source/cmake/WebKitFeatures.cmake`: `WEBKIT_OPTION_DEFINE(ENABLE_JITCACHE_TWINS "Build JITCache's twin checks and test hooks" PRIVATE OFF)` among the JavaScriptCore options, which puts `ENABLE_JITCACHE_TWINS` into `cmakeconfig.h` for JSC and for Bun's `root.h`.
- M9 (task 1). `Source/JavaScriptCore/shell/CMakeLists.txt`: the two blocks of the `testjitcache` executable that the harness sub-SPEC's section 8.3 writes, beside `testFFI`'s, under `ENABLE_JITCACHE_TWINS`. The first lists the sources, `../jitcache/tests/testjitcache.cpp` and every part's test files under `../jitcache/tests/<part>/` (section 14.1 and the lanes' file lists), adds `"${JAVASCRIPTCORE_DIR}/jitcache/tests/support"` to the target's private include directories, beside `jsc`'s, and ends with `WEBKIT_EXECUTABLE_DECLARE(testjitcache)`; the second, beside `testFFI`'s `WEBKIT_EXECUTABLE`, calls `WEBKIT_EXECUTABLE(testjitcache)`. Task 1 writes `tests/testjitcache.cpp` and stubs every other file the target lists, as for M1, since CMake refuses a missing source when it generates the build.
- M10 (task 10). `bytecode/UnlinkedCodeBlock.h` and `.cpp`, which the UCB lane edits too (SPEC-ucb.md section 15.1): in twins builds, the struct `JSC::JITCache::BodyEventCounts` and `UnlinkedCodeBlock`'s last data member `m_jitCacheEventCounts` with its accessor `jitCacheEventCounts()` (harness sub-SPEC section 10.1), and in `UnlinkedCodeBlock::~UnlinkedCodeBlock` the call `JITCache::retireBodyEventCounts(*this)` right after the UCB lane's `unlinkedCodeBlockWillBeDestroyed` (SPEC-ucb.md section 6.3; harness sub-SPEC section 10.3). The two calls are independent: retiring reads no registry entry. `UnlinkedCodeBlock` is an exported class, and its twins-only member keeps one layout in every translation unit, Bun's included, because M8 puts the switch into the `cmakeconfig.h` that JSC and Bun's `root.h` both read. Task 10 applies it after the UCB lane's task that adds that hook.
- M11 (task 1). `Source/WTF/wtf/PlatformJSCOnly.cmake`: `unix/UnixFileDescriptor.h` among the headers the JSCOnly port exports, which the store, `start` and maintenance use to own their descriptors.
- M12 (task 1). `runtime/OptionsList.h`: the UCB lane's three free measurement switches of its manifest entry M7 (SPEC-ucb.md), `jitCacheImportLLIntProgress`, `jitCacheVouchedHolderDigests` and `jitCacheImportModuleAnalysis`, each `Bool` with default true and a help string that says what turning it off does (SPEC-ucb.md section 11, SPEC-ucb.analysis.md section 9), so every lane task that reads one compiles from task 1 on; options.md gives each its free row. No must-match or fixed option enters `OptionsList.h`.

### 14.5 The lanes' other manifest entries

These name no hot file; each is met where the table says.

| entries | met by | task |
|---|---|---|
| option rows: UCB M5 and M7, Image M4, CB R-INT-9, ICs M3 | `JITCacheOptions.cpp` (section 5.1), from the rows of options.md; `OptionsList.h` gains only the UCB lane's three free measurement switches, through M12 | 3 (the table), 1 (M12) |
| section type ids: UCB R-INT-4, Image M3, ICs M2 | section 6.1 | 4 |
| a module's analysis: SPEC-ucb.analysis.md R-INT-A1 to R-INT-A7 | the kind `UCBAnalysis` (section 6.1) and check B7 (container sub-SPEC section 4.5); `readBodySection` and `OpenedArtifact::readSection` (section 6.2, container sub-SPEC section 7.6); `importModuleAnalysis` in `JITCacheBun.h` (section 3.1); `verifyModuleAnalysisTwin` (section 3.5); the `UCBAnalysis` source of `commitSectionsFor`, `SavedAnalysisInvalid` and `SavedAnalysisUnusable` (sections 6.3 and 8.4); the `module-analysis` event and the section helpers (harness sub-SPEC sections 9.2 and 5.2); the top of `Zig::SourceProvider::create` before the UCB lane's edit (section 11.2) | 0 (the declarations in `ValidatedBody.h`, `JITCacheBun.h` and `JITCacheTwinsHost.h`), 4 (B7), 5 (the reads), 9 (the commit), 12 (`verifyModuleAnalysisTwin`, after the UCB lane's task A2), 14 (before the UCB lane's task A4) |
| `didFailExecutableAllocation`, `producerContext`, `ProducerBudget`: Image M5, ICs M4 | sections 4.4 and 4.5 | 2 |
| `ValidatedBody` and its test factory, the lookup override: UCB R-INT-3, SPEC-ucb.md U4 and U8 | section 6.2 | 2 (`ValidatedBody`), 5 (the lookups and their override) |
| install glue: Image M6, ICs M5, UCB R-INT-6, CB R-INT-3 and R-INT-4 | section 7 | 8 |
| capture glue: Image M6, ICs M5, UCB R-INT-5, CB R-INT-5 | section 8 | 9 |
| test-build harness: Image M7, ICs M6, CB R-INT-6 and R-INT-10, UCB R-INT-10 | the harness sub-SPEC | 1 (the twins build, `testjitcache`), 8 (the image check's stash), 12 (shell helpers), 13 (the runner) |

SPEC-image.md M8 is conditional and needs no action: the UCB lane edits only `ArithProfile.h` (`restoreBits`) and the Image lane only the two `ArithProfile::emitUnconditionalSet` definitions in `ArithProfile.cpp`, so each lane makes its own edit.

## 15. Tests

The integrator restores no piece of its own, so its tests check its contracts and the oracle of THREAD Verification, which the runner applies to every run that configures JITCache (harness sub-SPEC section 7.6) beside the fault and install checks of harness sub-SPEC section 7.5; the lanes' twins run through the integrator's harness. Every C++ test runs in `testjitcache` and every JS test through the runner (harness sub-SPEC sections 7 and 8), in `debug-local` builds with ASan and LSan and with `--destroy-vm`, and in the twins build, on x86_64 and ARM64; a script that requires twins (harness sub-SPEC section 7.4), as every script in `integrator/` does, runs in the twins build only. Strict is on as harness sub-SPEC section 7.3 states the rule for every run the runner starts. The container's, the maintenance's and the harness's own tests are in their sub-SPECs.

### 15.1 C++

Each test lives in the test directory of the part it checks: T-OPT, T-CPU and T-BUILDID in `tests/store/PlatformTests.cpp`; T-BODY in `tests/store/StoreTests.cpp`; T-BUDGET, T-HEADERS, T-FAULTS, T-START, T-LOOKUP and T-BENCH in `tests/session/SessionTests.cpp`; T-SCORE, T-STAMP, T-DELTA and T-CHARGE in `tests/glue/CaptureTests.cpp`; and T-RUN-ONCE in the files its item names.

- T-OPT. In the default option group: every fixed row's required value equals the option's effective value, and the must-match reads return the effective values.
- T-CPU. The CPU feature vector equals the predicates of N11, bit by bit, and its unused bits are zero.
- T-BUILDID. The test executable has a build ID; `PlatformInternal::buildIDOfObject` (section 5.2) of a crafted object whose `PT_NOTE` segment holds the note yields the descriptor, and of an object without the note yields none.
- T-BUDGET. Eight threads charge and release concurrently until the limit refuses: the refusal is sticky, the total never exceeds the limit, an overflowing charge is refused, the peak is the maximum total, and the balance returns to zero. `createUnlimited` never refuses.
- T-HEADERS (`integratorExportedHeadersIncludeOnlyExportedHeaders`), the test of II18, which SPEC-image.md T19 and I21 cite. Over the `PrivateHeaders` directory the build copied, found through the build's CMake cache: no copied header outside JITCache's own four (section 3.5) includes a `jitcache/` header, and every `jitcache/` header that a copied header includes is itself among the copies. The test lists JITCache's headers from every subdirectory of the `jitcache/` source directory the CMake cache names, so a copied header that comes to include an unexported one fails it.
- T-BODY. `ValidatedBody::createForTesting` with sections of sizes 0, 1, 7, 8 and 4097 returns each at an 8-byte-aligned address with its bytes, an empty span for an absent kind, the key and version given and the highest tier of the kinds; its `onDestroy` runs exactly once, from the destructor, on the thread that drops the last reference.
- T-FAULTS. Each fault entry point moves the switches of section 4.2 as specified; `producerContext` is null after each; `didFailExecutableAllocation` called with a CB's `m_lock` held through a `GCSafeConcurrentJSLocker` returns without blocking; a `JSC::Debugger` attached to a global object turns activity off and `status` names `debugger.attach`; a VM without state ignores every entry point.
- T-START. On temporary directories, for each role: `Started`, and `Busy` for a second producing VM of the test process on the same path while the first VM's state lives, then `Started` for a ConsumerProducer once that state is destroyed, which releases the lock. `Rejected` at each step of section 3.2 that a test can reach (`start.already-configured`, `start.config`, `start.pc-maps` after a `JSC::Debugger` attached to the VM's global object, `start.build-id` through `removeMainBuildIDForTesting` (section 5.2), which removes the main executable's computed ID, `start.artifact-exists`, `start.not-an-artifact`, `start.artifact-missing`, and `start.incompatible` for a Consumer), and `Fault` at `start.header` for a corrupt header, at `start.pack` for a damaged pack and at `start.io` for a pack mapping that fails. A Producer creates over a header-less `cache/` whose pack holds no body. A ConsumerProducer facing that incompatible artifact is `Started` at `start.replaced` with the same detail, leaves a new header beside an empty pack and no `.cache.replaced`, and a Consumer then starts on the new artifact; the creation cases of container test C8. Apps (`integratorStartApps`): a Producer whose config names app X writes a header naming X, on which a Consumer naming X and one naming no app start, while one naming Y is `Rejected` at `start.another-app` with a detail giving X and Y and leaves the VM unconfigured; a ConsumerProducer naming Y is `Started` at `start.replaced` with that detail, its header names Y, and a Consumer naming X is then rejected; a ConsumerProducer naming no app opens the artifact of X and keeps its header; a Producer naming no app writes a header naming none, on which Consumers naming X and Y both start; and a Consumer naming Y facing a header that both differs in a must-match option and names X is `Rejected` at `start.incompatible`. A faulted VM reports `Faulted` and rejects a second `start`. Rejected and busy calls leave the VM unconfigured, and a second `start` on it succeeds. Two VMs of the process that open one artifact hold one `OpenedArtifact`. A `Config` left at its defaults gives `status().strict` false. These tests, and those below that count commits and charges, configure the delta capture `Compiled`, so they count the CodeBlock walk and the finalize capture alone; the scope `All` is T-RUN-ONCE's.
- T-LOOKUP. In a Consumer over a temporary artifact holding a body the writer committed, in this order: `bodyVersion` returns a nonzero token for the committed body's key and 0 for a key no body has, and reaches no system call (the store's fault hook, set to fail every open and then every mapping, never fires for it); `openBody` returns `Found` with a body whose `version()`, `fileSize()`, L and P are the commit's, counts one `bodyOpens`, and returns `Missing` for the key without a body; the hook with `EMFILE` on `openBody` gives `Missing` and counts `transientOpenFailures`; with an override set, both calls answer from it, a body from `createForTesting` included, whose commit identifier differs from the token the override gives; clearing the override restores the store's answers; last, the hook with `EIO` on `openBody` raises invalid material at `container.io`, which turns activity off, and both calls then return 0 and `Unusable`, whether or not an override is set. `readBodySection` answers in step with them: `Found` with a committed module body's `ucb.analysis`, `Absent` for the key without a body, the override's body's section while the override is set, and `Unusable` once activity is off; it counts no `bodyOpens`.
- T-BENCH. Bench reports (harness sub-SPEC section 9.2) on temporary directories: a start `Rejected` at `start.artifact-missing` writes its `start` event, with its outcome, its role and 0 indexed bodies, and no `budget` or `index` line; a Consumer over an artifact holding one body the writer committed flushes an `index` line whose `bodies` is 1, since `start` hands every role's report its artifact (section 3.2, step 10); a ConsumerProducer's `start` event says `started`, and its flush writes a `budget` line with the producer limit.
- T-SCORE. `beats` over a table of scores that differ in one field at a time, and ties, including a candidate whose counter is withheld against a saved score with equal richness and IC sites and more counter progress, which the candidate beats, and the same pair reversed, which the candidate loses. With strict on, `scoreSections` on a malformed feedback, CB summary or ICs span names the reader that rejected it, with the part and check section 8.2 raises.
- T-STAMP. In a Producer with strict on, two functions are brought to baseline as SPEC-ics.md T14 brings them, one whose `in` site has seen objects of two shapes, so its property-IC record lists two cases, and one whose every site has seen one shape; once their counters have made progress, `delta` commits both. The first body's envelope holds P 0 and its `cb.state` and `cb.summary` headers `NotCarried`; the second's envelope holds the `counterProgress` of its `cb.summary` and both headers `Carried`. The first body's kept score has `counterWithheld` set and the second's has it clear. For each body, `keptScoreForTesting` (section 8.3) equals `scoreSections` of the sections `openBody` returns for it (II22, II23).
- T-DELTA. `delta` rejects with each step of section 3.4 that a test can reach: `delta.unconfigured` with no state, `delta.role` for a Consumer and `delta.locks` without heap access. In a Producer it commits a body, and it reports `Faulted` after a recording fault. Invalid material raised inside a build (`integratorDeltaStopsAfterInvalidMaterialInABuild`): a Producer with the delta capture `All` holds a module UCB that ran, recorded without an analysis source as SPEC-ucb.analysis.md UA8 records one, and other bodies that ran, under a lookup override whose `open`, asked for that module's `ucb.analysis`, raises invalid material at `container.io`, notes how many bodies the index lists and answers `Unusable`. `delta` returns `Faulted` with that fault, `status` reports no other fault, and the index lists exactly the bodies it listed when the override raised: the module's capture wrote nothing, and no key after it was committed (sections 8.4 and 8.6).
- T-CHARGE. In a Producer that has already committed one key, so that the writer's staging buffer and the kept-summary table exist, a commit of a second key the index lacks raises `status().budget.chargedBytes` by exactly the per-entry charges of section 4.4, six of the kept-summary table's bucket sizes and six of the index's, and a later commit of the same key by neither. A recording fault then releases the kept summaries', the index entries' and the staging buffer's charges at the next glue entry (section 4.2), while both keys stay in the index.
- T-RUN-ONCE, the capture of bodies that ran without compiling, in three tests named `runOnce*` across the parts they check, beside the UCB lane's U4, which covers the registry's walk with each record's holder, and U6, which covers the carried progress, the order of tiers and the arming from zero with `jitCacheImportLLIntProgress` off. The delta capture at `start` (`runOnceDeltaCaptureAtStart`, `tests/session/SessionTests.cpp`): both spellings, `All` by default, a value outside the enumeration `Rejected` at `start.config` with the VM left unconfigured, and `capturesUncompiledBodies()` true only for a producing role with `All`. Container check B2 accepting a highest tier of 0 in Full mode, refusing 2, and B7 refusing an image section in a body of tier 0 (`runOnceContainerAcceptsTierZero`, `tests/store/ContainerTests.cpp`). `delta` with `All` (`runOnceDeltaCommitsAndConsumerImports`, `tests/glue/CaptureTests.cpp`): a program and functions that ran without compiling commit at tier 0 with L their carried progress and P zero, their kept score equal to `scoreSections` of their sections, a body prepared and never called stays out, a second `delta` with nothing run commits nothing, the delta capture `Compiled` commits none; a Consumer imports them with no pending import and no install, their CBs in the LLInt and the counter at the carried progress plus its own calls; and a baseline compilation's finalize capture replaces the tier-0 body with one of tier 1.

### 15.2 JS (`JSTests/jitcache/integrator/`)

| test | checks |
|---|---|
| `finalize-capture.js` | a body compiled in the producer is committed at its finalize capture; the consumer installs it (`jitcacheProgress().installs`), its `jitcacheBodyEvents` (harness sub-SPEC section 10.2) shows no LLInt instruction and no baseline compile where the JITCache-off run shows both, and the output equals the JITCache-off run |
| `delta-scoring.js` | a `delta` after more warm-up commits a richer body (the result's `committedBodies` counts it); a `delta` with nothing new commits nothing; the consumer imports the last commit |
| `consumer-producer.js` | `Producer; ConsumerProducer; Consumer`: the ConsumerProducer imports, learns and commits a richer body that the last consumer imports; in a second sequence, a ConsumerProducer whose production a limit of one page ends at `budget.limit` (`jitcache-expect-fault: 1:1 budget.limit`) keeps importing |
| `install-outcomes.js` | a baked-fact mismatch keeps the import for the next newborn CB of the UCB, which installs it; a consumer whose `--jitAllowlist` excludes a body drops the import at the gate; counters and outputs as expected |
| `llint-off.js` | a consumer with `--useLLInt=false` installs at `JIT::compileSync`, and the output equals the JITCache-off run |
| `exec-alloc-faults.js` | `--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=<n>` over a range of n, each n in a `Producer --jitcache-delta-at-exit` run followed by a `Consumer` run with `jitcache-expect-no-install`. The Producer runs declare `jitcache-require-fault: 0` for each of `exec-alloc.baseline-plan`, `exec-alloc.dfg-plan`, `exec-alloc.ftl-plan`, `exec-alloc.ic-handler` and `exec-alloc.mathic-snippet`, so each step must appear somewhere in the range. For each n, `jitcacheStatus()` names one of them or is `null`, since the fuzzer also fails allocations that keep native behavior, such as Yarr's (`ExecutableAllocator::allocate` counts every `JITCompilationCanFail` request; THREAD Failures). The range fits the runner's 50-line window as long as the script reaches all five sites within it. Unless n fails its own baseline plan, the function whose MathIC regenerates stays in baseline through the `delta`, so the image twin, which replays the regeneration, catches a captured snippet failure (SPEC-image.md section 11.3). A function whose baseline plan n fails runs in the LLInt for the rest of the run (N3), so neither its sites nor its MathIC run baseline code. For each function that `jitcacheBodyEvents` (harness sub-SPEC section 10.2) shows without a baseline compile, the Producer checks that `jitcacheStatus()` names `exec-alloc.baseline-plan`. After each fuzzed Producer run, the Consumer calls each of the script's functions, takes its key with `jitcacheBodyKey` and reads the committed body's sections, when there is one, with `jitcacheReadSection` (harness sub-SPEC section 5.2): no `ucb.feedback` holds a deferred LLInt counter, an `m_activeThreshold` of `INT32_MAX` (SPEC-ucb.md section 5.2), which no native capture holds (SPEC-ucb.md section 5.5); a body without an image, such as the script's run-once top level, holds no `cb.state`; and no `cb.state` carries a deferred baseline counter, a `Carried` counter whose `counterActiveThreshold` is `INT32_MAX` (SPEC-cb.md section 3.2 and R-INT-12). The test's bodies stay below the reoptimization count at which `adjustedCounterValue` clips and have no polymorphic site, so every `cb.state` carries its counter (SPEC-cb.md I16). Every output equals the JITCache-off run. ([history](SPEC-integrator-history.md#what-the-executable-allocation-fault-tests-can-see)) |
| `exec-alloc-ic-marker.js` | the IC-handler site's marker. Like `exec-alloc-faults.js`, with `jitcache-require-fault: 0 exec-alloc.ic-handler`, but every IC site of the script can give up only through that fault: cell bases with at most seven plain, cacheable shapes, no proxies, dictionaries, BigInt arrays, prototype puts or `instanceof`, and a first case that compiles its own stub, as the global-proxy reads of SPEC-ics.md T9 do. Its functions still run baseline code at the `delta`, except one whose own baseline plan n fails, which runs in the LLInt (N3). The script therefore also declares `jitcache-expect-fault: 0 exec-alloc.baseline-plan`, and its Producer checks, as `exec-alloc-faults.js`'s does, that `jitcacheStatus()` names that step when the function shows no baseline compile. After each fuzzed Producer run, the Consumer reads each function's `ICsBaseline` section with `jitcacheReadSection`, and no record holds `tookSlowPath` or a site that gave up (SPEC-ics.md section 4), which would mean the handler's fault came after its effects were written |
| `producer-limit.js` | a `Producer` with a limit of one page, declared with `jitcache-expect-fault: 0 budget.limit`: the limit ends production at `budget.limit`, whichever charge, recording's or a lane's at capture, is refused first (section 4.5); `jitcacheDelta()` throws naming it; the output equals the JITCache-off run |
| `writer-faults.js` | twins builds, one sequence per injectable writer fault, `writer.open`, `writer.write` and `writer.reread`, each set in the Producer for its writer's second commit and declared with `jitcache-expect-fault: 0 <step>`: the recording fault makes `status` name the step and leaves activity on, the second commit's key holds no body, and the first commit stays importable, which the Consumer installs. The root's store makes no system call, so `writer.publish` has no hook |
| `scoring-deferral.js` | twins builds: `Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-test-store-fault=scoring:EMFILE; Consumer`. The ConsumerProducer warms the imported bodies further and calls `jitcacheDelta()`; the store's fault hook fails every scoring read with `EMFILE`, so every key the ConsumerProducer scores is deferred, whether or not it has a body (the result's `deferredKeys` and `jitcacheProgress().capturesDeferred`), no fault is raised, nothing is committed, and the `cb.summary` bytes `jitcacheReadSection` returns for the imported keys are the same after the call as before it. The consumer imports the producer's bodies, and its output equals the JITCache-off run |
| `producer-kill.js` | twins builds: one sequence per commit kill point of harness sub-SPEC section 12, `Producer --jitcache-test-kill=<point>@2` with `jitcache-expect-exit: 0 kill`, so the consumer has a body to install, then `Maintenance compact 0 $ARTIFACT`, then `Consumer`. The consumer imports the body committed before the second commit. The second commit's key holds no body for the six points before the root's store and the new body for `after-root`, and an eighth sequence, whose second commit recaptures a key, finds that key's earlier body unchanged after a kill at `after-reread`. A checker after the kill reads the pack: every point from `mid-stream` to `after-index` leaves bytes past the valid end, and `before-append` and `after-root` leave none (`resources/bytes-past-the-valid-end.ts`, `resources/nothing-past-the-valid-end.ts`). A checker after the compact (`resources/compact-cleanup.ts`) requires its summary to reclaim exactly the bytes the pack lost, to evict nothing, and the pack to end at its valid end |
| `default-mode.js` | the corpus of `finalize-capture.js` in two sequences, one with strict on, as every test runs, and one with `--jitcache-strict=0` in every run, the default the bench measures: each consumer installs the same bodies (`jitcacheProgress().installs`), and every output equals the JITCache-off run |
| `must-match.js` | `Producer --evalMode=true; Consumer` with `jitcache-expect-no-install: 1`: the consumer's start is `Rejected` at `start.incompatible`, and its output equals the JITCache-off run |
| `fixed-option.js` | `Producer --useLOLJIT=true` is `Rejected` at `start.fixed-option`, naming `useLOLJIT`, and its output equals the JITCache-off run |
| `cpu-vector.js` | under QEMU only (harness sub-SPEC section 7.9): `Producer --jitcache-delta-at-exit; Consumer` with `jitcache-qemu-cpu: 0 max`, `jitcache-qemu-cpu: 1 neoverse-n1` and `jitcache-expect-no-install: 1`, whose vectors differ in JSCVT, SHA3 and FRINTTS (harness sub-SPEC N27): the consumer's start is `Rejected` at `start.incompatible`, and its output equals the JITCache-off run |

### 15.3 Bun (`~/bun/test/js/bun/jitcache/jitcache.test.ts`)

The tests spawn `bunExe()` with the flags of section 11.2, `--jitcache-strict` included, and compare each consumer's output with a JITCache-off run: a fixture app imports after a producer run; a worker whose main VM produces falls back to `Consumer`; a `p` and a `p-c` started while another process produces get `Busy`, run with JITCache off, print the output of a JITCache-off run and write the warning of section 11.2 to standard error exactly once, also when one of their workers meets the same `Busy`; a `c` given an artifact another build wrote is `Rejected` at `start.incompatible` and warns once with that step, while a `p-c` given one replaces it, writes the replacement line once and commits into the new artifact, which a later `c` imports; a consumer whose import meets invalid material warns once at exit with the fault's step, each also when a worker meets the same; `--jitcache-log` and `--jitcache-max-memory` are no Bun flags; with `--jitcache-bench-report`, the main VM writes its report at the configured path and each worker at that path suffixed with its id, and a macro VM writes none: in a producing run whose script bundles with `Bun.build` an entry whose macro returns its value only while `Bun.isMainThread` is false, the bundle holds that value, since the bundler runs the macro in a macro VM (N15), standard error holds no warning, and the report directory holds only the main VM's report, whose one `start` event is the main VM's own, `started` as a producer ([history](SPEC-integrator-history.md#the-macro-test-bundles-in-a-process-that-read-the-flags)); a producer without `--jitcache-delta-at-exit` calls `delta` at exit, which commits bodies, writes one `pack` event and leaves a dense pack, while `=0`, also as the last of two occurrences, calls none and a consumer never calls one; `bun jitcache compact` behaves as the maintenance sub-SPEC says (exit codes, `--yes`, `--no`, no terminal, and a terminal asked before the whole artifact goes), a first argument that is no number being the path with ratio 0 and a ratio after the path a usage error; a malformed flag value is a command-line error, `--jitcache-delta-at-exit=2` among them; a bare `--jitcache` opens `.jitcache` in the working directory, the one `--cwd` names included, and a worker started after `process.chdir` opens the main VM's artifact; `bun jitcache compact` without a path works on `./.jitcache`, with ratio 0 or without a ratio; a bare `--jitcache-strict` and `--jitcache-strict=1` turn strict on, `=0` turns it off, and the last occurrence wins; a producer with `--jitcache-delta-capture=compiled` commits only bodies with an image, one with `all` or no flag also commits the app's top level without one (the producer's `capture` events' `committed.tier`), and the last occurrence wins.

The app and app bytecode (section 11.2), each run's output equal to the JITCache-off run's:

- A `c` whose entry's bytes differ from the producer's is `Rejected` at `start.another-app` and warns once with that step and a detail whose two digests are `Bun.hash.xxHash3` of the two entries' bytes, printed as 16 lowercase hex digits; a `c` running a copy of the producer's entry from another path starts without a warning; a `p-c` running the other entry replaces the artifact, writes the replacement line once with the same detail and commits into the new artifact, on which a later `c` of that entry starts without a warning and a `c` of the first entry is refused.
- Two `-e` runs of one text name one app, and two of different texts are refused each other's artifacts.
- A `bun build --compile` executable whose `--compile-exec-argv` names JITCache's flags names its embedded entry: an executable built from another entry is refused the first one's artifact.
- In a `c` run of the producer's entry, a worker whose script is not that entry starts, and the run writes no warning.
- The app-bytecode warning, in runs whose `start` succeeds, since the warning is written only for a configured VM: a `p` on a fresh path, or a `c` after a producer made the artifact. A bundle built with `bun build --bytecode --target=bun` and run as a `p` writes `warn: JITCache ignores code built with --bytecode: <path>` with the bundle's path once, also when a second bytecode module loads, and writes nothing without `--jitcache`; a `bun build --compile --bytecode` executable whose `--compile-exec-argv` makes it a `p` writes it once with its entry's `/$bunfs/` path; a script run with `NODE_COMPILE_CACHE` set, first as a `p` and then as a `c` of that artifact, writes the compile cache's line once in the `c` run, whose modules come from the cache, and none in the `p` run; a bundle built without `--bytecode` writes neither line; and the `--bytecode` bundle run as a `c` with no artifact writes only its rejection line, at `start.artifact-missing`, and no bytecode line.

## 16. Bench obligations and parameters

These serve the bench loop THREAD Verification ends with. The bench report (harness sub-SPEC section 9) carries every measurement below, except what IB10 reads from committed bodies and native logs, the event counts IB11 reads from body-event dumps, and what no VM's report sees, which the bench takes from outside the process: IB8's Bun startup, the process's wall and CPU time in each configuration, and IB9's planning, the time and peak resident set of a `compact` with ratio 1 and no confirmation on an artifact without temporaries, which plans the whole artifact's deletion and changes nothing (THREAD Maintenance).

- IB1. Opening a body: `openBody` per body and per KiB (the borrow from the mapping, the integrity checks, the checksums), with cold and warm page caches, apart from installation as THREAD Verification requires, and the release of an installed body, the `install` event's `bodyRelease` (section 7.2, step 18). ([history](SPEC-integrator-history.md#bodies-are-borrowed-from-the-packs-mapping))
- IB2. The index: the time to read it at `start` and its bytes per body against the number of bodies and against the length of the delta chain `packDeltaChainBound` allows, beside the number of bodies the run requested, since every process keeps the whole index whatever it requests; an absent and a present `bodyVersion`, each a load of the root and a read of the index (SPEC-ucb.md B4); the cost of following a moved root in a consumer process beside a committing producer, since it runs at a request point under `m_indexLock`, and of reopening after a compaction; and the writer's index update in a producing process whose other VMs share the index. The `index` line of each flush carries these counts (harness sub-SPEC section 9.2). ([history](SPEC-integrator-history.md#readers-follow-the-packs-root))
- IB3. The import cost THREAD Verification bounds, set per body against the native cost THREAD Verification defines, for every body at or above `installationBoundSmallestBody`. The import side is everything JITCache does at the body's request point, which the UCB lane measures part by part, divides between the bound and what THREAD measures apart and records in the `request` event (SPEC-ucb.md section 14, B2), plus the install function's VM-thread CPU time, the `install` event's total, whose step breakdown explains it (section 7.2; harness sub-SPEC section 9.1); the bench joins the two events by key, each install with a request written before it (harness sub-SPEC section 9.4). The native side is the `compile` event (harness sub-SPEC section 9.3). Both sides run with per-VM support already generated, as THREAD's bound requires: an `install` or `compile` event that reports `supportGenerated` gives no sample, a body left without a sample on a side is set aside, and the bench reports how many were (harness sub-SPEC sections 9.3 and 9.4). Decoding, reading and faulting the artifact (IB1), and the key digest with the context digests and each TDZ environment's first digest (SPEC-ucb.md B1), are measured apart, as THREAD Verification lists them.
- IB4. Capture pause per body, at the finalize capture and in `delta`: scoring, each lane's build, the writer's stream, reread and publish; and the compaction that ends each `delta`, with the pack's live and dead bytes before it, from the `pack` event.
- IB5. Writer throughput against `writerStagingBytes`.
- IB6. Producer memory: the budget's peak and its parts (the lanes' records and sections, kept summaries, the index entries the writer adds, the staging buffer, the full index and compaction buffers, `delta`'s tables) on the bench's workloads, from which `defaultProducerLimitBytes` is set. The registry and the identity digests, which no limit charges, are measured apart (SPEC-ucb.md B5).
- IB7. `delta` duration and memory against the number of CBs and keys, with the first scorings of the keys a ConsumerProducer imported counted apart: each borrows the body from the mapping and checksums the three summary sections the import already validated. If they weigh, the first change to measure is stamping the committed score in the envelope's reserved bytes, so that a first scoring reads the 128-byte envelope. ([history](SPEC-integrator-history.md#the-scoring-read-follows-the-root))
- IB8. Start: `start` time for each role against the number of bodies, and Bun's startup with JITCache on and off, with an empty and a full artifact. Startup with JITCache on includes reading and hashing the entry for the app digest (section 11.2), which the bench also times alone on each entry, as `Bun.hash.xxHash3` of the bytes a read of the file returns.
- IB9. Maintenance: `compact` planning time and memory against the number of bodies.
- IB10. Generations. THREAD Restoration withholds baseline counter progress from a body whose captured property-IC record lists two or more cases, megamorphic folds aside, and the ICs lane extends the rule to call sites the producer left polymorphic (SPEC-ics.md section 5.3); a capture marks such a body with `hasPolymorphicSite`, the bit below. IB10 checks on the bench's workloads that consumer-induced exit sites do not accumulate across generations under that rule, and sizes what the rule leaves: the floor's residue in bodies that carry progress, which THREAD Restoration gives the bench, and recaptures that drop the bit (SPEC-ics.md B6). It chains one Producer, `benchConsumerProducerGenerations` ConsumerProducer runs and one Consumer, each a fresh process that calls `delta` at exit, as Bun's producing roles do by default (section 11.2), and records per generation and per body: ([history](SPEC-integrator-history.md#exit-sites-across-consumerproducer-generations))
  - from the `install` and `capture` events (harness sub-SPEC section 9.2): whether the import carried counter progress, the committed score with the UCB lane's exit-site units apart, the field that decided each commit, and the `hasPolymorphicSite` each capture passed to the CB lane;
  - from the committed bodies, through the lanes' readers: the exit sites each generation added (SPEC-ucb.md `FeedbackSection`), with those at a bytecode whose property-IC record lists two or more cases counted apart (`parseSection` and `isPolymorphicPropertyIC` over `ICsBaseline`, the record's bytecode index taken from its mold in `image.baseline`), and the CB lane's `counterMode` (`CBFormat::StateHeader`).

  From these it reports SPEC-ics.md B4's share of committed bodies that carry the bit, split between those with a polymorphic property-IC record and those whose bit only a call site set, and B6's bodies whose import carried no progress while their committed recapture carries it. It also compares the last Consumer's calls with the Producer's warmed call W (HARNESS.md, Benches), in time and in each body's DFG compiles and `BadCache` and `BadConstantValue` exits, taken in runs of their own with the free options `logCompilationChanges`, which logs each DFG compile, and `printEachOSRExit`, which logs each exit with its kind and bytecode index.
- IB11. The skipped warm-up (HARNESS.md): the per-body event counts of harness sub-SPEC section 10 on the bench's workloads, from body-event dumps of runs in the twins build, the only build that counts: each run's totals for the calls HARNESS.md's Benches section names N1, W and C1, and, by key, each body's counts in W beside C1.

The bench loop proposes each parameter's value from its first measurement, and a value enters the parameter table of HARNESS.md, which also holds the lanes' parameters, only after human approval. `JITCacheParameters.h` names the parameters code reads, as `constexpr` values; an empty `std::optional` is one the bench has not set yet:

| parameter | meaning | value |
|---|---|---|
| `defaultProducerLimitBytes` | the limit a producing role uses when the host passes none | `std::optional<size_t>`, empty until the bench sets it; until then a producing role must pass a limit (section 3.2, step 3) |
| `writerStagingBytes` | the writer's staging buffer | 64 KiB until IB5 tunes it |
| `packDeltaChainBound` | the index deltas a pack's chain holds since its last full index before a commit writes a full index instead, which bounds the chain a reader walks when it opens the pack (container sub-SPEC section 8.2) | 32 until IB2 tunes it |
| `retiredPackReopenIntervalMilliseconds` | the least time between a reader's looks at `cache/pack` once its mapped pack's retired mark has outlived its first second, which only a compaction that died between its mark and its rename leaves for long (II16) | 100 until tuned |
| `packMappingSlackBytes` | the address space a reader maps past the pack's valid end, so the pack grows that far without a new mapping: a mapping covers the valid end plus the larger of the valid end and this (container sub-SPEC section 5.2) | 256 MiB until tuned |

Code reads none of the bench's own parameters, so they live in HARNESS.md's table alone: `installationBoundFraction`, THREAD's fixed fraction of the native cost an import may take; `installationBoundSmallestBody`, THREAD's smallest body the bound covers, as the linked size in bytes of the body's installed image, which its `install` event reports as `image.codeSize` (harness sub-SPEC section 9.2); and `benchConsumerProducerGenerations`, the number of ConsumerProducer runs IB10 chains between its Producer and its Consumer. The bench reads the first two to raise an alert, which fails nothing.

## 17. Tasks

Ordered; each fits one implementation agent. A task that needs a lane piece builds against the headers in which that lane declares it and lands when the piece does. Every manifest entry of section 14.4 belongs to exactly one task, named below and in the entry. A task calls only functions that it or an earlier task defines, and its tests need only those, so the tree builds and every listed test runs after every task. A header task adds declarations and never changes the signature of anything already defined, so the tree compiles after it. Every task meets section 11's platform rule in the files it writes (R-ALL-9). The state's lifecycle lands with `start` in task 7, the first task by which every part's type exists: destroying a state runs every part's destructor, `ValidatedBody`'s included, which the registry's pending imports hold, so task 7 defines the constructor and the destructor in `JITCacheVMState.cpp` and both teardown hooks in `JITCacheAPI.cpp` beside `start` (section 4.1), and the members `JITCacheVMState.cpp` defines in earlier tasks neither create nor destroy a state. ([history](SPEC-integrator-history.md#the-state-is-created-and-destroyed-in-task-7))

Other parts' code calls the integrator too. Task 2 defines every integrator function another part's code calls (the fault entry points, the budget, `producerContext`, the `VMState` members of the lanes' interface, `ValidatedBody` with its test factory, the twin report and the bench report) except `VMState::bodyVersion`, `VMState::openBody` and the lookup override, which task 5 adds. A lane task that calls those three lands after task 5, and any other after task 2.

The waits run one way. A lane task that an integrator task waits on depends on no integrator task from that one on. The lanes' twin checks, which task 8 calls in twins builds (the CB lane's `verifyTwins` in its task 6, the ICs lane's `checkRestoredBaselineICs` and `functionSnapshotBaselineICs` in its task 6, the Image lane's `Twins::checkImage` in its task 11), need only tasks 1 and 2: the test target, the twin report and the budget. A lane's tests that need the glue, the jsc host or the runner sit in a later task of that lane (CB task 7, ICs task 7, Image task 12), and the UCB lane's JS tests run once tasks 12 and 13 have landed (SPEC-ucb.md section 15.3). ([history](SPEC-integrator-history.md#the-lanes-twin-checks-land-before-the-glue-that-calls-them))

0. Headers, each in its directory of section 14.1: `base/ByteFields.h` and `base/CodeSymbolAnchor.h`; `JITCacheAPI.h`, `JITCacheVMState.h`, `JITCacheFaults.h`, `ProducerBudget.h` and `JITCacheBench.h` in `session/`; `ValidatedBody.h`, `JITCacheContainer.h` (the container sub-SPEC's section 4.5), `JITCachePlatform.h` (section 5.2), `JITCacheFiles.h` and `ArtifactAccess.h` (section 3.2, step 7), `ArtifactStore.h` (the declarations of the container sub-SPEC's sections 2, 5.1, 5.2, 6 and 7), `ArtifactWriter.h` (section 6.3) and `JITCacheParameters.h` in `store/`; `JITCacheGlue.h` and `JITCacheCapture.h` (sections 8.2 and 8.3) in `glue/`; `JITCacheMaintenance.h` in `maintenance/`; `TwinReport.h` and `JITCacheTwinsHost.h` in `harness/`; `JITCacheBun.h` in `session/`; and `tests/support/JITCacheTest.h`; exactly as this file and the sub-SPECs declare them. `base/BodyKey.h` and `base/JITCacheSHA256.h` come with the UCB lane's task 0. `JITCacheVMState.h` ends where section 4.1's declaration does, and task 2 adds its private part. Every part's tests and glue code compile against these.
1. The build, after this list's task 0, whose headers M2 exports: manifest entries M1, M2, M3, M5, M6, M8, M9, M11 and M12 with their stubs, `tests/testjitcache.cpp` and the fixtures of `tests/support/` (harness sub-SPEC section 8), the twins profile in `~/bun`'s build scripts and the `twins` and `bun-twins` targets of `build.ts` (harness sub-SPEC section 1), and the build-id flag of section 11.2. Then a `debug` build and a `twins` build must succeed, and `testjitcache --list` must run, listing no test yet, so every part can build and test from here on.
2. `ProducerBudget.cpp`, `JITCacheFaults.cpp`, `ValidatedBody.cpp` with `createForTesting`, `TwinReport.cpp`, the `BenchReport` of `JITCacheBench.cpp` (open, `record`, the flush with its `budget` line, `benchReport(VM&)`; harness sub-SPEC section 9), the members of `JITCacheVMState.cpp` that neither create nor destroy a state nor read the store (the switches, the fault records and the members that raise faults, `producerContext`), the private part of `JITCacheVMState.h` that these members and task 5's read (a member for each part of section 4.1's table), `Debugger::attach`'s call, and `BaselineJITPlan`'s `m_jitCacheRecordsImage` with `jitCacheRecordsImage()` (section 4.4), false until task 7 computes it, so the Image lane's task 4 compiles against it; T-BUDGET, T-HEADERS and T-BODY. No state exists before task 7 creates the first. Every later task records its bench events through this `BenchReport`.
3. `JITCachePlatform.cpp`, `JITCacheOptions.h` and `JITCacheOptions.cpp`: the container's CRC-32C (container sub-SPEC section 4.4), build IDs, the CPU vector, the option table; T-OPT, T-CPU, T-BUILDID.
4. `JITCacheContainer.cpp`: the header, pack and body formats and their validation (container sub-SPEC sections 3 and 4) and tests C1 to C3. The task adds to `JITCacheContainer.h` the declarations of container sub-SPEC section 3 (the header's bytes, the app record, reading, comparison and digest) and section 6.1's table of section type ids, tiers and required sections, which tasks 5 to 7 call.
5. `JITCacheFiles.cpp`, and `ArtifactStore.cpp`: `ProducerLock`, the empty pack's creation, the registry, the mapped pack, the opened artifact with its index and how it follows the root and a retired pack, its token read, its open, its scoring read and its one-section read, and the twins-build fault and registry hooks (container sub-SPEC sections 2 and 5 to 7); in `JITCacheVMState.cpp`, `VMState::bodyVersion`, `VMState::openBody`, `VMState::readBodySection` with `SectionLookup` and the lookup override, with their `lookup` and `open` bench events; tests C4 to C6, which read the store's own results and need no configured VM. After tasks 2, 3 and 4.
6. `ArtifactWriter.cpp`: the commit, the tombstone, the compaction and its twins-build hooks (container sub-SPEC section 8) and test C7, which builds its sections from crafted spans and streamed sources. After task 5.
7. `JITCacheAPI.cpp`: `start`, with its `start.platform` step (section 3.2, step 0), `status`, `flushBenchReport`, `finishHostRun` and the `toJSON` functions, with the `start` bench event; `ArtifactAccess.cpp`, through which `start` reaches the artifact; every accessor tasks 8, 9, 12, 14 and 16 use on the state's parts, the last edit of `JITCacheVMState.h` (section 4.1); the state's constructor and destructor and `VMState::releaseEndedProductionMemory` (section 4.2) in `JITCacheVMState.cpp`, and the teardown hooks of section 4.6, with manifest entry M4, which calls them from `VM::~VM`; `shouldRecordBaselineImage`, in the `JITCacheCapture.cpp` that task 9 completes, and its call in `BaselineJITPlan`'s constructor (section 4.4), which reads the registry; T-START, which includes container test C8, T-FAULTS and T-LOOKUP, which need a configured VM. After tasks 2 to 6 and the UCB lane's task 6, which defines the registry the state holds and `status` reads.
8. The install glue (section 7): `JITCacheInstall.cpp` with the `install` bench event, the edits to `prepareForExecutionImpl`, `JIT::compileSync` and `shouldJIT`, and in twins builds the image twin-check state with its deleter, its stash and `didFinishPrepareForExecution` (harness sub-SPEC section 3). After task 7, each lane's install functions and, for twins builds, each lane's twin checks: the CB lane's task 6, the ICs lane's task 6 and the Image lane's task 11.
9. The capture glue (section 8): `JITCacheCapture.cpp` with `scoreSections`, the kept summaries, the index-entry charge, `commitSectionsFor`, `delta` with its three phases and the compaction that ends it, `KeptSummaries` and its deleter (section 8.3), `rewriteSectionForTesting`, `keptScoreForTesting`, the assertion that captures never nest, the `delta` bench event and the `capture` event without the writer's fields, which task 16 adds, and the finalize hook in `BaselineJITPlan::finalize`, called with a null timing, without the `compile` event of section 8.5's step 1, which task 16 adds with the timing, and with the twins builds' baseline compile count (harness sub-SPEC section 10.1); `ValidatedBody`'s L and P (section 6.2), with `llintThreshold()`, `counterProgress()`, their members and the constructor's two parameters in `ValidatedBody.h` and `ValidatedBody.cpp`, where `createForTesting` passes 0 for both, the store's `open` in `ArtifactStore.cpp` passing the `BodyLayout`'s values, and `rewriteSection` in `ArtifactWriter.cpp` stamping the copy with the key, tier, L and P of the body it opened (container sub-SPEC section 8.3); T-SCORE, T-DELTA, T-CHARGE, T-STAMP and T-RUN-ONCE, which also need the UCB lane's capture of bodies without an image. After tasks 6, 7 and 10 and each lane's capture functions, Image task 11's `twinsSectionSize` and `writeTwinsSection` among them, since twins builds stream the twins section (section 6.3), so the three tasks that edit `BaselineJITPlan::finalize` land in the order 10, 9, 16.
10. The plan-site faults (section 9): `BaselineJITPlan`, `DFG::Plan`, `SpeculativeJIT`. The engine side of the twins builds' event counts (harness sub-SPEC section 10): `JITCacheBodyEvents.cpp`, manifest entry M10, the counts in the LLInt, `DFG::Plan::finalize`, `handleExitCounts` and `CodeBlock::jettison`, and the erasure of a VM's retired totals in `didFinalizeHeap`. After tasks 2 and 7 and the UCB lane's task that adds its destructor hook (SPEC-ucb.md section 6.3).
11. Maintenance (maintenance sub-SPEC) with its command line, the twins-only test hooks of `JITCacheMaintenanceTesting.h` and tests M1 to M6. After tasks 5, 6 and 7, whose `start` test M3 calls to show that the next Producer reuses a remnant.
12. The jsc host: the flags and calls of section 11.1; in twins builds, `harness/Placement.cpp` (the address-space placement of harness sub-SPEC section 4), `harness/HeapDescription.cpp` (the reachable-heap description of its section 6) and `harness/ShellFunctions.cpp` (the shell helpers and the twins host functions of its section 5, `jitcacheBodyEvents` and the body-event dump of its section 10, and `verifyModuleAnalysisTwin`, section 3.5), the shell flags that set the store's and the writer's hooks, the writer's kill points among them, the image test hook and H1's test entry, and manifest entry M7, `$vm`'s function. After tasks 8, 9 and 11, the UCB lane's tasks 6, 10 and A2 (A2 defines `firstModuleAnalysisDifference`), and the ICs lane's task 6, which defines the `functionSnapshotBaselineICs` that `jitcacheICsSnapshot` registers.
13. The runner, `Tools/Scripts/run-jitcache-tests` with its modules in `Tools/Scripts/jitcache/` and `JSTests/jitcache/resources/jitcache.js` (harness sub-SPEC section 7), with the JITCache-off oracle (its section 7.6), its runs under QEMU (its section 7.9) and its self-tests H1 to H6 and H8, H2 also on a twins build made with `bun build.ts twins --arch=aarch64`. After task 12.
14. The Bun host (section 11.2): `JITCacheHost`, with `--jitcache-delta-at-exit` on by default and `finishHostRun` at exit, the app digest that `RunCommand::boot` and `RunCommand::boot_standalone` compute, the warning about app bytecode with its call in `Zig::SourceProvider::create` and the two transfers it moves up there (section 11.2), on which the UCB lane's task A4 waits, `Zig__GlobalObject__create`'s call, `on_exit`'s call, `JSCInitialize`'s placement, `configureVM`'s `recordVMLayout` call, the twins builds' body-event dump at exit, `bun:jsc`'s twins exports and `bun jitcache compact`, with its optional ratio and its `Tag::JITCacheCommand` in `src/options_types/command_tag.rs`; the condition `!c.jitcacheTwins` on the entry of `scripts/build/flags.ts` that defines `BUN_DYNAMIC_JS_LOAD_PATH`, and the `--cfg=jitcache_twins` that `scripts/build/rust.ts` passes in twins builds, under which alone Bun declares the four twins flags (harness sub-SPEC section 1); and the `OS(LINUX)` guards of section 11's platform rule around the Linux-only code of the Bun host. After tasks 7, 9, 11 and 12, which defines the twins host functions and the placement functions it calls. Then the `bun-debug` and `bun-twins` targets of `build.ts` must build: they compile Bun's side against the copied headers alone, so each JSC declaration it uses must come from a header of section 3.5.
15. The JS tests of section 15.2 and the Bun tests of section 15.3. After tasks 13 and 14.
16. The native-cost hooks of harness sub-SPEC section 9.3 (`BaselineJITPlan`'s three members, the constructor `BaselineJITPlan::BaselineJITPlan(CodeBlock*)` that sets `m_jitCacheMeasure`, and the two `BaselineSpanTimer`s, declared in `JITCacheBench.h`, that measure the compile and finalize spans, the `RelinkTimer` in `ScriptExecutable::installCode`, the support-generation count of `JITThunks` and `generateSlowPathHandler` with its reads in the `install` and `compile` events), the `BaselineCompileTiming` that `finalize` passes to the finalize hook and the `compile` event it feeds, the writer's `CommitTiming` (section 6.3) in `ArtifactWriter.h` and `ArtifactWriter.cpp` with the `capture` event's writer fields and its check in container sub-SPEC test C7 (`tests/store/WriterTests.cpp`), and the measurements of section 16, among them the `index` line with the store's `IndexStatistics` (`ArtifactStore.h`, `ArtifactStore.cpp`), the `setArtifact` call in `start`'s step 10 (`JITCacheVMState.cpp`'s constructor) and `delta`'s `pack` event. After tasks 9 and 13.
17. The pin comparison (harness sub-SPEC section 11): `Tools/Scripts/jitcache/pin-compare.ts`, the `pin` target of `build.ts`, which also accepts `--arch=aarch64`, so that the comparison runs on each architecture (harness sub-SPEC section 11.1), the runner's `--pin` option and `jitcache-pin` directive, the forced blinding of its section 11.5 with its edit to `MacroAssembler::shouldConsiderBlinding`, and self-test H7. It lands after the JITCache-off oracle (task 13) and the twins (task 8, with the lanes' twin checks it waits on).
