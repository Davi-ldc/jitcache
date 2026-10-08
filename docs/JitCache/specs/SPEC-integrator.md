# SPEC-integrator: the integrator

This SPEC specifies the integrator that THREAD Execution derives from the frozen lane SPECs. It is the entry point and indexes three sub-SPECs, one per subject that stands apart (section 1); all four files bind. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to the non-obvious decisions of the four files and binds nothing.

Code is cited by symbol and file. New symbols live where THREAD Execution puts JITCache's code, unless a row of section 14 says otherwise. "THREAD" is `docs/JitCache/THREAD.md`, and "VM thread" is the thread that holds the VM's API lock and heap access. The lane SPECs are SPEC-ucb.md, SPEC-image.md, SPEC-cb.md and SPEC-ics.md; "UCB R-INT-3" is requirement R-INT-3 of SPEC-ucb.md, and "Image", "CB" and "ICs" name the other three the same way.

## 1. Scope

The integrator owns:

1. The entry points `start`, `status` and `delta`, the per-VM state behind them, and the two hosts' use of them (the jsc shell and Bun).
2. The option table `start` checks, and the header that records the must-match options, the build IDs and the CPU feature vector.
3. The container, in [SPEC-integrator.container.md](SPEC-integrator.container.md): the artifact's directories and file names, the lock file, the header file and the body file format (envelope, section directory, section type ids, checksums) with their validation, the objects a process keeps for an opened artifact, the in-memory index and its refresh, opening, validating and pinning bodies, and the scoring read of saved bodies.
4. The writer, also in the container sub-SPEC, which streams a body to a temporary file, rereads and validates it, and publishes it with `renameat`.
5. Locks: the producer lock and the integrator's own in-process locks.
6. The install glue, which runs at THREAD's two install points and calls the lanes in THREAD's order, and the capture glue, which runs at the end of `BaselineJITPlan::finalize` and in `delta`, scores candidates and commits bodies.
7. Fault plumbing: invalid material, recording faults, the executable-allocation fault and its plan sites, and the debugger.
8. Maintenance, in [SPEC-integrator.maintenance.md](SPEC-integrator.maintenance.md): `clean`, `compact` and their command line.
9. The producer budget and producer context the lanes charge against.
10. Test builds and bench reports, in [SPEC-integrator.harness.md](SPEC-integrator.harness.md): the twins build, the twin report and the twin checks' plumbing in a VM, address-space placement, the shell, `bun:jsc` and `$vm` helpers, the description of the heap JavaScript can reach, the runner and its oracle, the C++ test framework and runner, the bench report events, the per-body event counts, the comparison of the off path with a build of the pin, and the producer kills.

Each lane owns its sections (THREAD Storage) and the native edits its SPEC lists; THREAD Execution says what the integrator decides alone and has the glue call the lanes in THREAD's order.

In outline: `start` checks options and process facts, takes the producer lock for a producing role, creates or validates the header, takes the index of the body directory that the VMs of its process share, and publishes a `VMState` (sections 3.2 and 4). The UCB lane's request points read index tokens from that state and open bodies through it (section 6.2). When a newborn CodeBlock whose UCB carries a pending import reaches an install point, the install glue prepares every lane's part privately before it writes to the CB and installs it (section 7). After each successful baseline compilation, and for every eligible CB in `delta`, the capture glue scores the candidate against the saved body and commits a winner through the writer before JS resumes (section 8). Faults turn cache activity or production off for good, and each names its step in `status` (sections 4.2 and 4.5).

## 2. Native facts

Each was verified by reading the code at this pin. The facts only the harness uses, N16 to N25, sit in the harness sub-SPEC's sections that use them.

- N1. `ScriptExecutable::prepareForExecutionImpl` (`runtime/ScriptExecutable.cpp`) runs under `DeferGCForAWhile`, creates the CB with `newCodeBlockFor`, then installs `codeBlock->unlinkedCodeBlock()->m_unlinkedBaselineCode` with `setupWithUnlinkedBaselineCode` when the slot holds code, and otherwise calls the file-static `setupLLInt` when `Options::useLLInt()` holds and `setupJIT` when it does not. It always ends with `installCode(vm, codeBlock, codeBlock->codeType(), codeBlock->specializationKind(), Profiler::JettisonReason::NotJettisoned)`. `UnlinkedCodeBlock::m_unlinkedBaselineCode` is a public member.
- N2. `setupJIT` calls `JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed)` and `RELEASE_ASSERT`s `CompilationSuccessful`. `JIT::compileSync` (`jit/JIT.cpp`, `static CompilationResult compileSync(VM&, CodeBlock*, JITCompilationEffort)` with an unnamed `VM&`) creates a `BaselineJITPlan`, compiles it on the calling thread and returns `plan->finalize()`. Bun's `node:vm` `Script` route also calls it, on a CB whose UCB Bun decoded (install.md, "Embedder routes").
- N3. `BaselineJITPlan::finalize` (`jit/BaselineJITPlan.cpp`) calls `JIT::finalizeOnMainThread`, which returns `CompilationFailed` for a null `BaselineJITCode` and otherwise adds the code-size sample and runs `setupWithUnlinkedBaselineCode`. On `CompilationFailed` it calls `dontJITAnytimeSoon` and sets `m_didFailJITCompilation`; on `CompilationSuccessful` it issues `WTF::crossModifyingCodeFence`, calls `installCode(m_codeBlock)` and `jitSoon`. `BaselineJITPlan::compileInThreadImpl` drains the value profiles and then compiles inside one `Safepoint`.
- N4. `shouldJIT` is a file-static `static inline bool shouldJIT(CodeBlock*)` in `llint/LLIntSlowPaths.cpp`, namespace `JSC::LLInt`, testing `bytecodeRangeToJITCompile`, the `jitAllowlist` and `useBaselineJIT`. Its callers are `entryOSR`, `loop_osr` and `replace`.
- N5. `VM::~VM` (`runtime/VM.cpp`) calls `heap.incrementDeferralDepth()` ("Never GC, ever again"), then, under `ENABLE(JIT)`, `JITWorklist::cancelAllPlansForVM`, then clears `m_perBytecodeProfiler`, then `m_apiLock->willDestroyVM(this)`, then `heap.lastChanceToFinalize()`, then frees the remaining VM structures.
- N6. `JITWorklist::cancelAllPlansForVM` removes the plans that are not compiling, waits until every remaining plan of the VM is ready and drops the ready plans without finalizing them, so no worklist thread runs code for that VM afterwards.
- N7. `Heap::forEachCodeBlockIgnoringJITPlans(const AbstractLocker&, functor)` (`heap/HeapInlines.h`) calls `CodeBlockSet::iterate(locker, functor)`, which takes no lock itself: its caller must hold `vm.heap.codeBlockSet().getLock()`. The set holds the CBs marked at the last collection's End phase, when `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks` removes the unmarked ones, plus every CB created since: the `CodeBlock` constructors call `CodeBlockSet::add`, which takes the same lock, and nothing calls `CodeBlockSet::remove`. A CB the walk visits stays allocated until a later collection's sweep.
- N8. `JITWorklist::completeAllReadyPlansForVM` finalizes every ready plan of the VM under `DeferGC`. `JITWorklist::waitUntilAllPlansForVMAreReady` releases heap access while it waits.
- N9. A DFG plan that cannot allocate executable memory installs a `FailedFinalizer` at the `linkBuffer.didFailToAllocate()` branches of `SpeculativeJIT::compile` and `SpeculativeJIT::compileFunction` (`dfg/DFGSpeculativeJIT.cpp`), and an FTL plan does so through `FTL::fail` at the two `state.allocationFailed` branches of `DFG::Plan::compileInThreadImpl` (`dfg/DFGPlan.cpp`), after `FTL::compile` and after `FTL::link`. Two paths set `FTL::State::allocationFailed`: `FTL::compile` when `b3CodeLinkBuffer->didFailToAllocate()` (`ftl/FTLCompile.cpp`), and `FTL::LowerDFGToB3::compileCallFFIImpl` when `FFI::Signature::invokeThunk` returns null. On the supported targets that happens only when the `LinkBuffer` of `FFI::generateInvokeThunk` failed to allocate (`ffi/FFIInvokeThunk.cpp`), and `invokeThunk` generates a missing thunk on its first call, so a lowering that finds none allocates it on the compiling thread. `DFG::Plan::finalize` asserts that GC is deferred, computes the result, write-barriers the CB and then calls `m_callback->compilationDidComplete`, which writes the failure's effects (SPEC-ucb.md F24). `JITPlan::isFTL()` tells an FTL plan from a DFG one.
- N10. `Debugger::attach` (`debugger/Debugger.cpp`) calls `m_vm.setShouldBuildPCToCodeOriginMapping()`. It is reached from `JSGlobalObjectDebugger::attachDebugger` when `Debugger::addObserver` adds the first observer. The VM's constructor also turns the mapping on under `useSamplingProfiler` and `alwaysGeneratePCToCodeOriginMap`, and `VM::shouldBuilderPCToCodeOriginMapping()` reads it.
- N11. The x86_64 assembler's predicates are `supportsSSE3`, `supportsSupplementalSSE3`, `supportsSSE4_1`, `supportsFloatingPointRounding`, `supportsCountPopulation`, `supportsAVX`, `supportsAVX2` and `supportsFloat16` (public), and `supportsLZCNT` and `supportsBMI1`, which are protected and return `s_lzcntCheckState == Set` and `s_bmi1CheckState == Set` after `collectCPUFeatures()`; both states and `collectCPUFeatures` are public (`assembler/MacroAssemblerX86_64.h`). The ARM64 predicates, all public, are `supportsFloatingPointRounding`, `supportsCountPopulation`, `supportsFloat16`, `supportsDotProd`, `supportsLSE`, `supportsDoubleToInt32ConversionUsingJavaScriptSemantics`, `supportsRoundFloatToIntegerFloat` and `supportsSHA3` (`assembler/MacroAssemblerARM64.h`).
- N12. Every option of the must-match and fixed tables of options.md exists with the type and compiled-in default they give (`runtime/OptionsList.h`). `useExplicitResourceManagement` and `useImportDefer` are `Bool` options generated from `WTF/Scripts/Preferences/UnifiedWebPreferences.yaml` into `JSCWebPreferenceOptions.h`, which `OptionsList.h` includes. `reoptimizationRetryCounterMax` compiles in 0 and `Options` derives its effective value; `quickDFGTierUpThresholdFactor` compiles in `Options::defaultQuickDFGTierUpThresholdFactor()`, which returns 0.2 outside `PLATFORM(MAC)`.
- N13. `Heap::hasHeapAccess()`, `Heap::currentThreadIsDoingGCWork()` (a GC thread, or a mutator whose `mutatorState()` is not `Running`) and `VM::currentThreadIsHoldingAPILock()` are public.
- N14. The jsc shell parses its command line (`CommandLine::parseArguments`, `jsc.cpp`) before `JSC::initialize()` in `jscmain`. An argument starting with `--` that no earlier test matched is handed to `Options::setOption`, and the arguments after `--` reach the script as `arguments`. `runJSC` creates the VM with `VM::create` and then, inside the run loop and under `JSLockHolder`, creates the shell's `GlobalObject`. With `--destroy-vm` it derefs the VM at the end, which runs `VM::~VM`.
- N15. Bun:
  - `JSCInitialize` (`src/jsc/bindings/ZigGlobalObject.cpp`) sets options once per process inside `std::call_once` and calls `JSC::initialize` with a callback.
  - `Zig__GlobalObject__create` creates the VM of every `VirtualMachine` (main thread, workers, macros, the debugger thread) with `JSC::VM::tryCreate`, calls `vm.heap.acquireAccess()`, takes `JSC::JSLockHolder`, then calls `WebCore::JSVMClientData::create` and creates the global object. For a VM that is not the main thread's it tests `executionContextId == INT32_MAX || executionContextId > 1`; the comment above it, which gives -1 for the main thread, is stale.
  - The `executionContextId` comes from `VirtualMachine::init` (`src/jsc/VirtualMachine.rs`): `opts.context_id`, or 1 when `opts.is_main_thread` holds and `INT32_MAX` otherwise. A worker passes `worker.execution_context_id()`, which `WorkerMessagingProxy` takes from `ScriptExecutionContext::generateIdentifier()`, a process-wide counter that starts at 1 and is incremented before each use, so every worker's id is above 1. The debugger thread's VM (`start_js_debugger_thread`, `src/jsc/Debugger.rs`) and macro VMs (`src/js_parser_jsc/Macro.rs`) pass no id and get `INT32_MAX`.
  - `bun test --isolate` makes its later global objects in the same VM (`Zig__GlobalObject__createForTestIsolation`). Bun also creates VMs for its bytecode builder (`vmForBytecodeCache` in `ZigSourceProvider.cpp`) and for Bake's production global (`BakeCreateProdGlobal`).
  - `VirtualMachine::on_exit` (`src/jsc/VirtualMachine.rs`) runs the user's exit handlers through `ExitHandler::dispatch_on_exit` and then, on the main thread, persists the Node compile cache (`node_compile_cache::persist_at_exit`); `global_exit` and the worker teardown come after it.
  - Bun's own link passes `-Wl,--build-id=sha1` (`scripts/build/flags.ts`). The local WebKit recipe (`scripts/build/deps/webkit.ts`) builds the `jsc` target with `ENABLE_STATIC_JSC`, `-fno-pic -fno-pie -no-pie`, `CMAKE_POSITION_INDEPENDENT_CODE` off and no build-id flag, and Bun compiles its own code with `-fno-pic -fno-pie` and links with `-fno-pic -Wl,-no-pie` (`scripts/build/flags.ts`), so in the jsc shell and in Bun alike the engine sits at its link-time address in every process of a build (SPEC-image.md N23).
  - Bun's build globs its `.cpp` files and bundles them into unified sources (`scripts/build/unified.ts`).

## 3. Public interface

### 3.1 Types

`JITCacheAPI.h` is exported to Bun (section 3.5) and declares:

```cpp
namespace JSC {
class VM;
namespace JITCache {

enum class Role : uint8_t { Consumer = 1, Producer = 2, ConsumerProducer = 3 };

struct Config {
    String artifactPath;                      // the parent directory of THREAD Storage
    Role role { Role::Consumer };
    std::optional<size_t> producerLimitBytes; // producing roles; empty: defaultProducerLimitBytes (section 16)
    bool strict { false };                    // off unless the host turns it on (THREAD Session; section 4.3)
    String benchReportPath;                   // empty: no bench report
    String twinReportPath;                    // ENABLE(JITCACHE_TWINS) builds; ignored elsewhere
};

enum class StartOutcome : uint8_t { Created, Opened, Busy, Rejected, Fault };
struct StartResult {
    StartOutcome outcome;
    ASCIILiteral step;   // empty for Created and Opened
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

enum class SessionState : uint8_t { Unconfigured, Created, Opened, Faulted };
enum class ProductionState : uint8_t { NotProducing, Active, Ended };

struct Progress {
    uint64_t indexedBodies { 0 };
    uint64_t bodyOpens { 0 };
    uint64_t transientOpenFailures { 0 };
    uint64_t imports { 0 };               // UCB statistics: imports, seededDecodes, attaches, gateDrops, misses (summed)
    uint64_t seededDecodes { 0 };
    uint64_t attaches { 0 };
    uint64_t gateDrops { 0 };
    uint64_t misses { 0 };
    uint64_t installs { 0 };
    uint64_t bakedFactMismatches { 0 };
    uint64_t captureCandidates { 0 };
    uint64_t capturesDeferred { 0 };      // a saved body that a transient error kept from being read (section 8.3)
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
    ASCIILiteral rejection;            // Rejected: the step of section 3.4
    std::optional<FaultReport> fault;  // Faulted: the fault that stopped production or activity
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 };    // committed by this call, before any fault
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 };       // keys whose saved body a transient error kept from being read (section 8.3)
};

JS_EXPORT_PRIVATE StartResult start(VM&, const Config&);
JS_EXPORT_PRIVATE Status status(VM&);
// The caller holds no JSC-internal lock, a documented duty that JSC cannot check (THREAD Session).
JS_EXPORT_PRIVATE DeltaResult delta(VM&);
// VM thread. Writes out the bench report's buffered lines (harness sub-SPEC section 9.1); does nothing for a VM with no
// state or no report. The hosts call it at exit in every role (section 11).
JS_EXPORT_PRIVATE void flushBenchReport(VM&);
JS_EXPORT_PRIVATE ASCIILiteral name(StartOutcome);
JS_EXPORT_PRIVATE ASCIILiteral name(Role);
JS_EXPORT_PRIVATE ASCIILiteral name(FaultClass);
JS_EXPORT_PRIVATE std::optional<Role> parseRole(StringView);   // "consumer", "producer", "consumer-producer"

// One-line JSON for hosts' logs: {"jitcache":"start"|"delta"|"status", ...} with every field of the struct by name,
// enums as the names above (lowercase, words joined by "-"), faults as {"class","step","detail"}.
JS_EXPORT_PRIVATE String toJSON(const StartResult&);
JS_EXPORT_PRIVATE String toJSON(const DeltaResult&);
JS_EXPORT_PRIVATE String toJSON(const Status&);

} } // namespace JSC::JITCache
```

### 3.2 `start`

`start(vm, config)` runs on the VM thread before the VM's first global object (THREAD Session). It writes nothing to the VM before step 10, so `Busy` and `Rejected` leave the VM unconfigured and the host may call `start` again. It takes no JSC lock but its callees', allocates no cell and stops for no collector.

1. The caller holds the API lock and heap access (`vm.currentThreadIsHoldingAPILock()`, `vm.heap.hasHeapAccess()`); otherwise `Rejected` at `start.locks`.
2. `vm.jitCacheState()` is null; otherwise `Rejected` at `start.already-configured`, since created, opened and fault configure the VM for good.
3. The config is well formed: a non-empty `artifactPath`, a role in range, for a producing role a limit from `producerLimitBytes` or `defaultProducerLimitBytes` (section 16), a `benchReportPath` that is empty or opens (harness sub-SPEC section 9.1), and in twins builds a `twinReportPath` that is empty or opens (harness sub-SPEC section 2); otherwise `Rejected` at `start.config`, with a detail naming the field. A report opened here is closed again on any outcome but `Created`, `Opened` and `Fault`.
4. Every fixed option has its required value (section 5.1); otherwise `Rejected` at `start.fixed-option`, naming the first option that differs, its required value and its effective value.
5. `vm.shouldBuilderPCToCodeOriginMapping()` is false; otherwise `Rejected` at `start.pc-maps`: a debugger attached before `start` turned the mapping on (N10), and images carry no PC-to-origin maps.
6. The process facts of section 5.2 are known: the build IDs, the must-match option values and the CPU feature vector. A main executable without a build ID is `Rejected` at `start.build-id` (THREAD Storage).
7. `start` opens the artifact (container sub-SPEC sections 1 to 6) with the role's rules, in this order:
   - every role opens the parent directory; a Producer creates a missing parent (`mkdir`, one level, mode 0755), and the other roles are `Rejected` at `start.artifact-missing`;
   - a producing role takes the producer lock with `ProducerLock::tryAcquire`, without blocking, and the `VMState` keeps it (container sub-SPEC section 2); `busy` is `Busy` at `start.busy`, and nothing stays open;
   - a Producer finding a header is `Rejected` at `start.artifact-exists`, and one finding a header-less `cache/` that holds a committed body name is `Rejected` at `start.not-an-artifact`; otherwise it creates the artifact, reusing a remnant without bodies (container sub-SPEC section 1.3);
   - a Consumer or ConsumerProducer finding no `cache/` directory or no header is `Rejected` at `start.artifact-missing`; a corrupt header is a `Fault` at `start.header`, and an incompatible one is `Rejected` at `start.incompatible`, with a detail naming the first field that differs (container sub-SPEC section 3.2);
   - every role then takes the artifact's `OpenedArtifact` with `ArtifactRegistry::take` (container sub-SPEC section 5.1), which the VMs of a process that open one artifact share with its index (II21).

   Any other I/O error, such as a `flock` that fails other than busy or a listing that fails, is a `Fault` at `start.io`. A rejection or busy result taken after the lock was acquired releases it.
8. Outcome: `Created` for a Producer, `Opened` for the other roles, or the `Fault` of step 7.
9. On `Fault`, the producer lock, if taken, is released, since a faulted VM never produces.
10. A `VMState` is created with the role, the config, the outcome and, on `Fault`, activity off with the start fault recorded (section 4.2), and published with `vm.setJITCacheState` (release store). It keeps the bench report of step 3; in twins builds it also creates the twin budget and keeps the twin report of step 3 (section 4.1; harness sub-SPEC section 2).

### 3.3 `status`

`status(vm)` runs on the VM thread and reads only: it opens no file, raises no fault and changes no state (THREAD Session). For a VM without state it returns `SessionState::Unconfigured`. Otherwise it reports the session state (`Faulted` when `start` faulted), the role, strictness, whether activity is on and why it went off, the production state and why it ended, the earlier of the two faults as `firstFault`, the progress counters of `VMState` and the UCB registry's statistics, and the budget snapshot. A producer budget that has refused a charge reports production `Ended` with the fault `budget.limit` even when the VM thread has not raised it yet (section 4.5).

### 3.4 `delta`

`delta(vm)` is exposed only in C++ (THREAD Session). It rejects without work, returning `Rejected` with these steps:

- `delta.unconfigured`: the VM has no state;
- `delta.role`: the role is Consumer;
- `delta.locks`: the calling thread does not hold the API lock or heap access;
- `delta.collector`: `vm.heap.currentThreadIsDoingGCWork()` holds.

These are THREAD Session's requirements. A call made while a capture runs needs no rejection, since no capture reaches one (section 8.7).

Once the preconditions pass, `delta` calls `releaseEndedProductionMemory()` (section 4.2), which only frees what production held and is no work in this sense. When activity is off or production has ended, it then returns `Faulted` with the recorded fault and no work. Otherwise it runs section 8.6 and returns `Completed` with the counts, or `Faulted` with the fault and the counts committed before it (THREAD Failures).

### 3.5 Exported headers

Three headers join `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` (section 14, M2), and Bun includes them as `<JavaScriptCore/JITCacheAPI.h>`, `<JavaScriptCore/JITCacheMaintenance.h>` and `<JavaScriptCore/JITCacheTwinsHost.h>`. Bun's build sees only the copied headers (SPEC-image.md section 14.4), so every include must resolve among them: no exported JSC header includes any `jitcache/` header, and `VM.h` names the state through a forward declaration only (M3). ([history](SPEC-integrator-history.md#buns-twins-build-reaches-only-exported-headers))

- `JITCacheAPI.h` and `JITCacheMaintenance.h` include only WTF headers and `JavaScriptCore/JSExportMacros.h` and declare no member that exists only under `ENABLE(JITCACHE_TWINS)`.
- `JITCacheTwinsHost.h` holds what a host calls in twins builds, all of it under `ENABLE(JITCACHE_TWINS)`, so a build without twins sees an empty header. The jsc shell includes it as well, so both hosts call the same declarations, each with `JS_EXPORT_PRIVATE`:
  - the four placement and layout functions that Bun's `JSCInitialize` and `JITCacheHost::configureVM` call, with the enumeration `HeapProbes` that `recordVMLayout` takes (harness sub-SPEC section 4), with only WTF and JSC types in their signatures (`VM&`, forward-declared, `const String&` and `const Vector<String>&`);
  - the five host functions that `bun:jsc` registers (harness sub-SPEC section 5.3), declared with `JSC_DECLARE_HOST_FUNCTION`, for which the header also includes `JavaScriptCore/JSCJSValue.h`;
  - `void writeBodyEvents(VM&, const String& path)`, which `Bun__JITCache__atExit` calls (harness sub-SPEC section 10.3);
  - `bool setImageTestHookNamed(const char*)`, which maps a hook's run-flag name to the Image lane's `ImageTestHook`, calls `setImageTestHook` (SPEC-image.md section 11.3) and returns false for an unknown name, so neither host includes the Image lane's header.

## 4. Per-VM state

### 4.1 `VMState`

```cpp
namespace JSC::JITCache {

class ArtifactWriter;
class BenchReport;
class OpenedArtifact;
class ProducerLock;
struct ImageTwinCheckState;   // twins builds; defined in JITCacheInstall.cpp (harness sub-SPEC section 3)

class VMState final {
    WTF_MAKE_NONCOPYABLE(VMState);
    WTF_MAKE_TZONE_ALLOCATED(VMState);
public:
    // The lanes' interface (UCB R-INT-1 to R-INT-3 and R-INT-10). VM thread unless noted.
    bool tracksKeys() const;          // activity on (any thread)
    bool importsEnabled() const;      // role Consumer or ConsumerProducer, and activity on (any thread)
    bool productionActive() const;    // producing, activity on and production not ended (any thread)
    bool strict() const;              // Config::strict (any thread)
    UCBRegistry& registry();
    uint64_t bodyVersion(const BodyKey&);
    BodyLookup openBody(const BodyKey&);
    void raiseInvalidMaterial(ASCIILiteral step, String detail);                  // step is a full name, such as "ucb.identity"
    void raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail);
    void raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail);
#if ENABLE(JITCACHE_TWINS)
    TwinReportSink* twinReportSink();  // non-null while a twin report is open
    Ref<ProducerBudget> twinBudget();  // ProducerBudget::createUnlimited(), for twin compiles
    // Tests (section 6.2): while set, bodyVersion and openBody answer from these alone; token answers index tokens.
    void setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open);
    void clearBodyLookupForTesting();
#endif

    // The integrator's own state.
    Role role() const;
    bool producing() const;           // Producer or ConsumerProducer
    bool activityOn() const;
    ProducerContext* producerContextIfActive();   // any thread
    void releaseEndedProductionMemory();          // VM thread; nothing while production is active or once it is released (section 4.2)
    OpenedArtifact* artifact();       // null after a start fault
    BenchReport* benchReport();       // null unless Config::benchReportPath was set
    ~VMState();                       // defined in JITCacheAPI.cpp
    // Task 7 (section 17) completes the private part.
};

}
```

The lanes reach it with `vm.jitCacheState()`, which `VM.h` declares inline with `setJITCacheState` (section 14, M3). `start` stores the pointer once; `didFinalizeHeap` stores null and then destroys the state (section 4.6). Every reader runs either on the VM thread, which wrote the pointer, or on a JIT worker through `producerContext` (section 4.4), whose acquire load sees a fully constructed state.

The state owns these parts:

| part | held as | created by | released by |
|---|---|---|---|
| the role, the config, the start outcome | values | the constructor | |
| activity, production and the debugger flag | `std::atomic<bool>` | the constructor | |
| the first fault of each switch, the progress counters | values the VM thread writes | the fault members and the glue | |
| the producer budget and context (producing roles) | `RefPtr<ProducerBudget>`, `ProducerContext` | the constructor | the destructor |
| the bench report | `std::unique_ptr<BenchReport>` | `start`'s step 3, when `Config::benchReportPath` is set | the hosts' exit calls and `willDestroyVM` flush it; the destructor |
| the twin report and the twin budget (twins builds) | `std::unique_ptr<TwinReport>`, `Ref<ProducerBudget>` | the constructor | `willDestroyVM` closes the report |
| the UCB lane's `UCBRegistry` | a value | the constructor | the destructor, after every UCB and UFE destructor has run (section 4.6) |
| the producer lock (producing roles) | `std::unique_ptr<ProducerLock>` | `start` | the destructor, which releases the lock |
| the `OpenedArtifact` the VMs of the process share (container sub-SPEC section 5) | `RefPtr<OpenedArtifact>` | `start` | the destructor |
| the body-lookup override (twins builds) | two `Function`s | `setBodyLookupForTesting` | `clearBodyLookupForTesting`; the destructor |
| the writer (producing roles) | `std::unique_ptr<ArtifactWriter>` | `start` | its staging buffer at the end of production; the writer with the state |
| the kept summaries and the index-entry charge (sections 4.4 and 8.3) | a map and a byte count | the capture glue | the end of production |
| the count of captures in progress (`ASSERT_ENABLED` builds) | an integer the VM thread writes | the constructor | (section 8.7) |
| the image twin-check state (twins builds; harness sub-SPEC section 3) | `std::unique_ptr<ImageTwinCheckState, void (*)(ImageTwinCheckState*)>` | install step 18, at the VM's first stash | `willDestroyVM` |

The destructor destroys the writer before the producer lock, the opened artifact and the budget it refers to. `JITCacheVMState.h` forward-declares the integrator types the state holds through pointers, so a lane that includes it compiles against none of their headers. The constructor, the destructor and both teardown hooks land in task 7 (section 17). ([history](SPEC-integrator-history.md#the-state-is-created-and-destroyed-in-task-7s-file))

### 4.2 Activity and production

Two switches go from on to off and never back:

| switch | starts | goes off when | effect |
|---|---|---|---|
| activity | on, except after a start fault | invalid material, an executable-allocation fault, a debugger attach | no request point records, imports, seeds or attaches (UCB `tracksKeys`, `importsEnabled`); no install; no recording, capture or `delta` |
| production | active for a producing role | a recording fault, or activity going off | no recording (`producerContext` returns null), no capture, no direct-eval context digest at a UCB record (UCB `productionActive`), and `delta` reports the fault |

Both are atomics, because JIT workers read production through `producerContext` and a debugger may attach from another thread. The first fault that turns each switch off is recorded on the VM thread, except a debugger attach, which only sets an atomic flag that `status` reads. After a recording fault a ConsumerProducer goes on importing. ([history](SPEC-integrator-history.md#productionactive-belongs-to-the-lanes-interface))

Production memory (kept summaries, the writer's staging buffer, `delta`'s candidates, and the charge for the index entries the writer added, section 4.4) is released by `releaseEndedProductionMemory()`, which each glue entry on the VM thread calls first: the install function before its step 1 (section 7.2), the finalize capture at its step 2 (section 8.5) and `delta` once its preconditions pass (section 3.4). The call does nothing while production is active or once the memory is released, so it costs a flag test; teardown releases whatever remains. Nothing is released inside a fault entry point, so those of section 4.5 free nothing and wait for nothing.

### 4.3 Strict

Both modes check the artifact's integrity: `start` checks the header in full (container sub-SPEC section 3.2), and every read of a body runs the integrity checks of container sub-SPEC section 4.5. Strict adds that section's structure checks, which cover the rest of the container's framing, and the integrator passes the session's `strict` to every lane call that takes it: Image R-INT-3, the CB and ICs calls of sections 7.2 and 8.4, and the summary readers `scoreSections` calls (section 8.2). THREAD Session leaves to the SPECs which assumptions strict covers. The integrator adds no assumption check of its own, because its own mechanisms guarantee in both modes what it assumes: the writer's reread validates every committed file in full against the checksums of the bytes it streamed, and the writer counts every source's bytes against its size (`writer.section`; container sub-SPEC section 8.2); and a kept summary describes its file, because its score comes from what the builders returned for the bytes they wrote (section 8.4, step 6) and the producer lock keeps every other writer out (section 8.3). Whether a lane's builder produces bytes its own readers accept is that lane's assumption, and its SPEC decides whether strict covers it (SPEC-cb.md SC1 does for `cb.state` and `cb.summary`); the glue reads none of a capture's own bytes back. ([history](SPEC-integrator-history.md#strict-integrity-in-both-modes-structure-under-strict))

### 4.4 Producer context and budget

`ProducerBudget.h` declares what SPEC-image.md R-INT-1 and R-INT-2, SPEC-ucb.md R-INT-7, SPEC-cb.md R-INT-2 and the integrator charge against:

```cpp
namespace JSC::JITCache {

class ProducerBudget final : public ThreadSafeRefCounted<ProducerBudget> {
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
};

class ProducerContext {
public:
    Ref<ProducerBudget> budget() const;
};

ProducerContext* producerContext(VM&);   // any thread, no lock

}
```

`tryCharge` returns false at once when the budget has refused before. Otherwise it adds `bytes` to the charged total with a compare-and-swap loop, and a sum that overflows or exceeds the limit sets the refused flag and returns false without changing the total; a success raises the peak. `release` subtracts and asserts that the total does not go below zero. The refused flag is sticky (THREAD Failures). Every member uses atomics, so a charge, a release and a read may run on any thread, and a record destroyed after its VM's session releases into a budget it still references.

`producerContext(vm)` loads `vm.jitCacheState()` with acquire order and returns `state->producerContextIfActive()`: the state's one `ProducerContext` while the role produces, activity is on and production has not ended, and null otherwise. The object lives until the state is destroyed, which happens after `cancelAllPlansForVM` has stopped every compilation of the VM (N5, N6), so a JIT worker that obtained it may use it for its whole compilation.

Whether a baseline compilation records is decided when its plan is built (SPEC-image.md R-INT-12 and section 4.1). `BaselineJITPlan` gains `bool m_jitCacheRecordsImage { false }` and `bool jitCacheRecordsImage() const`. Its constructor, which both callers (`jitCompileAndSetHeuristics` and `JIT::compileSync`) run on the VM thread, sets the member when `producerContext(vm)` returns a context and `vm.jitCacheState()->registry().keyOf(*codeBlock->unlinkedCodeBlock())` returns a key; the lookup takes only the registry's leaf lock (SPEC-ucb.md section 6.3). ([history](SPEC-integrator-history.md#which-compilations-record-is-decided-at-plan-construction))

The integrator charges, before allocating: the ICs section buffer of each capture (SPEC-ics.md R-INT-2), the writer's staging buffer (container sub-SPEC section 8.1), the kept summaries (section 8.3), `delta`'s candidate table (section 8.6), and each index entry the writer adds for a key the index lacks (section 8.4, step 8).

A hash table the integrator owns for production is charged eight bucket sizes with its first entry and six more per entry; each entry's six are released when the entry goes, and the eight when the table goes. That bounds WTF's storage for a table that only grows, rehashes included: `HashTableSizePolicy` (`wtf/HashTable.h`) starts a table at `HashTraits::minimumTableSize`, eight buckets, and doubles it once it is three-quarters full up to 1024 buckets and half full above, so a table of k entries holds at most max(8, 4k) buckets, and at most 6k while a rehash holds the old table beside the new one.

An index entry the writer adds is charged six of the index's bucket sizes, its share of a table that grows the same way. The charge lasts until production ends, when the state releases it with the rest of its production memory (section 4.2); the entry itself stays in the index, which the VMs of the process share (container sub-SPEC section 5.1), for the VMs that import. ([history](SPEC-integrator-history.md#charging-hash-tables-and-index-entries))

These allocations are not production memory and go uncharged:

- the parent-key registry and the digests the UCB lane keeps for identity (each TDZ environment's, SPEC-ucb.md section 3.4), which serve every role (THREAD Session);
- the index's other entries, which listings and refreshes create, and the writer too when it applies the queued events of changes made before its VM took the lock: they hold the bodies on disk and what other processes commit, which every importing VM needs whoever produces;
- the scoring read's mapping of a saved body (container sub-SPEC section 7.3), which borrows page-cache pages of a file JITCache does not own for production and is unmapped before the scoring returns;
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

- `raiseInvalidMaterial` turns activity off (which ends production) and records the report. The UCB lane calls the one-step form with its full names (`ucb.identity` to `ucb.supplied-digest`). The integrator calls the part form for the other lanes, with part `image` and check `description(ImageCheck)` (SPEC-image.md section 12.1), part `cb` and check `description(CBCheck)`, part `ics` and check the enumerator name of `ICs::Check` with the site index in the detail, and for itself with the full names that sections 6.2 and 8.3 raise: `container.io` with `strerror` of the error in the detail, or the name of the failed check in container sub-SPEC section 4.5.
- `raiseRecordingFault` ends production, records the report and calls `budget->refuseFurtherCharges()`, so recorders still running on JIT workers fail their next charge and leave their records incomplete. Every refusal of a production charge is raised as `budget.limit`, the budget's own fault, wherever it lands: when a lane's capture call fails and `budget->hasRefused()` holds afterwards, the glue raises `budget.limit` with the lane's check in the detail. Otherwise the capture glue maps `UCBCaptureFailure::BudgetExceeded` and `StrictCheckFailed` to part `ucb` with checks `capture-budget` and `capture-strict`, `CaptureOutcome::RecordingFault` to part `image` with `description(check)`, `CaptureOutcome::ChargeRefused` to `budget.limit` with no image fault beside it (SPEC-image.md R-INT-6), a `CBFault` of kind `RecordingFault` to part `cb` with `description(check)`, and a `CaptureError` to part `ics` with the enumerator name of `ICs::CaptureCheck` and the site index. ([history](SPEC-integrator-history.md#every-refused-production-charge-is-budgetlimit))
- A budget refusal made on a JIT worker cannot reach the VM thread when it happens, so the capture glue tests `budget->hasRefused()` at the start of every capture and every `delta` and raises `budget.limit` then (THREAD Failures). A refusal made on the VM thread inside a capture fails that capture, which raises the fault at once.

### 4.6 Teardown

`JITCacheGlue.h` declares two hooks that `VM::~VM` calls (section 14, M4), and `JITCacheAPI.cpp` defines them with the state's constructor and destructor (section 4.1):

- `willDestroyVM(VM&)`, right after the `cancelAllPlansForVM` block and before `m_perBytecodeProfiler` is cleared. GC is deferred for good and no compilation of the VM runs (N5, N6). It destroys the image twin-check state (harness sub-SPEC section 3), closes the twin report, flushes the bench report, ends production without recording a fault (no implicit `delta`, THREAD Failures), frees the production memory and releases its charges, the index entries' included.
- `didFinalizeHeap(VM&)`, right after `heap.lastChanceToFinalize()`. Every UCB and UFE destructor has run by then (SPEC-ucb.md section 6.3). In twins builds it first erases the VM's retired body-event totals, for every VM whether or not `start` configured it, since the UCBs `lastChanceToFinalize` destroyed retired their counts into them (harness sub-SPEC section 10.3). It stores null into the VM's pointer and destroys the state, which destroys the registry, destroys the `ProducerLock`, which releases the lock, and drops the reference to the opened artifact, which is destroyed with the last VM of the process that holds it.

## 5. Options and process facts

### 5.1 The fixed-option check

[options.md](../options.md) lists the must-match and fixed options of THREAD Storage with their types (N12). The must-match options' effective values go into the header (container sub-SPEC section 3.1), and `start` walks the fixed table in its order. ([history](SPEC-integrator-history.md#the-option-table-lives-in-optionsmd))

`JITCacheOptions.cpp` writes the table as one macro list, `JITCACHE_FOR_EACH_FIXED_OPTION(v)`, with entries `v(name, Type, requiredValue)`, and expands it into a static array of rows: the name as an `ASCIILiteral`, a function that reads `Options::name()`, and the required value. Because each row reads `Options::name()`, a renamed or removed option fails the build. `checkFixedOptions()` walks the rows in table order and returns the first row whose effective value differs: `Bool`, `Unsigned` and `Int32` compare exactly, `Double` compares with `==` (both sides are the same literal or the value `Options` computes from it), and an `OptionString` required to be null passes when `Options::name()` is `nullptr`. The required values are those of options.md for this pin. JSC keeps no defaults once options freeze (THREAD Storage), so test T-OPT (section 15.1) checks, in a process started without option overrides, that every row's required value equals its effective value; a WebKit bump that moves a default fails that test.

### 5.2 Process facts

`JITCachePlatform.cpp` computes, once per process:

- Build IDs. The main executable is the first object `dl_iterate_phdr` lists. The engine object is the one whose loaded segments contain the address of `JITCache::codeSymbolAnchor` (SPEC-image.md section 3.5 and R-INT-8). For each, the build ID is the descriptor of the first `NT_GNU_BUILD_ID` note whose owner is `GNU` in one of its `PT_NOTE` segments, 1 to 64 bytes long. The header records the main executable's ID, and the engine object's when it is a different object. A missing ID in either object makes `start` reject at `start.build-id`. In twins builds `JITCachePlatform.h` also declares `void removeMainBuildIDForTesting(bool)`; while it is set, the process facts report the main executable without an ID, which is how T-START reaches `start.build-id`.
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
    std::array<bool, 3> mustMatch { };     // by the option index of container sub-SPEC section 3.1
    uint64_t cpuFeatures { 0 };            // bit i is predicate i above
};

// Any thread. Computed once, at the first call, which start makes after the first VM froze the options; while
// removeMainBuildIDForTesting is set, the copy it returns has a main executable without an ID.
ProcessFacts processFacts();
uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>);   // container sub-SPEC section 4.4
#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool);
#endif

}
```

The header's byte layout, its creation and its comparison are in the container sub-SPEC, section 3.

## 6. Bodies: the interface the lanes use

### 6.1 Sections

`ValidatedBody.h` declares the section kinds, which the lanes name when they ask a body for a section:

```cpp
enum class SectionKind : uint8_t {
    UCBIdentity, UCBCore, UCBFeedback,
    ImageBaseline, BakedFactsBaseline, ImageTwinsBaseline,
    CBStateBaseline, CBSummaryBaseline,
    ICsBaseline,
};
constexpr unsigned numberOfSectionKinds = 9;
```

| kind | the lane's name | type id | tier | in a body of highest tier 1 |
|---|---|---|---|---|
| `UCBIdentity` | `ucb.identity` | `0x0101` | 0 | required |
| `UCBCore` | `ucb.core` | `0x0102` | 0 | required |
| `UCBFeedback` | `ucb.feedback` | `0x0103` | 0 | required |
| `ImageBaseline` | `image.baseline` | `0x0201` | 1 | required |
| `BakedFactsBaseline` | `baked-facts.baseline` | `0x0202` | 1 | required |
| `ImageTwinsBaseline` | `image-twins.baseline` | `0x0203` | 1 | required in `ENABLE(JITCACHE_TWINS)` builds, forbidden elsewhere |
| `CBStateBaseline` | `cb.state` | `0x0301` | 1 | required |
| `CBSummaryBaseline` | `cb.summary` | `0x0302` | 1 | required |
| `ICsBaseline` | `ICsBaseline` | `0x0401` | 1 | required |

The high byte of a type id names the lane (1 UCB, 2 Image, 3 CB, 4 ICs) and the low byte the section family; the tier byte of the directory entry separates tiers, 0 for the sections every tier shares. A later DFG capture adds entries such as (`0x0201`, tier 2) and (`0x0401`, tier 2) under the same layout version, which is how the format admits more than one tier per body (THREAD's opening). Every section's span starts 8-byte aligned in memory and covers exactly the section, because sections start at 8-aligned file offsets (container sub-SPEC section 4.3) and an opened body is mapped from a page boundary (UCB R-INT-3, Image R-INT-5, CB R-INT-1); the ICs lane needs no alignment (ICs R-INT-1). The writer writes every section the body's highest tier requires, and strict validation checks that they are present (container sub-SPEC, check B7); normal mode trusts it (THREAD Session). This satisfies UCB R-INT-4, Image R-INT-5, ICs R-INT-1 and R-INT-4, and the CB lane's need for its two section ids.

### 6.2 Lookup

`ValidatedBody.h` declares:

```cpp
class ValidatedBody final : public ThreadSafeRefCounted<ValidatedBody> {
    WTF_MAKE_TZONE_ALLOCATED(ValidatedBody);
public:
    const BodyKey& key() const;
    uint64_t version() const;                               // the file's commit identifier, which the writer never makes 0
    uint8_t highestTier() const;
    std::span<const uint8_t> section(SectionKind) const;    // empty when absent; starts 8-byte aligned
    size_t fileSize() const;                                // the mapped file's size, or a test body's buffer size
    ~ValidatedBody();                                       // unmaps or frees; any thread that holds no JITCache lock

#if ENABLE(JITCACHE_TWINS)
    // A body no container check has seen, for the parts' own tests: the sections are copied into one buffer, in the
    // order given (kinds strictly increasing), each at an 8-byte-aligned address; highestTier() is the highest tier of
    // the given kinds (section 6.1). onDestroy runs first in the destructor, on the destroying thread. version is not 0.
    struct TestSection { SectionKind kind; std::span<const uint8_t> bytes; };
    static Ref<ValidatedBody> createForTesting(const BodyKey&, uint64_t version, std::span<const TestSection>,
        Function<void()>&& onDestroy = { });
#endif

private:
    friend class OpenedArtifact;   // open builds a body from a mapping validateBody accepted (container sub-SPEC section 7.2)
    using SectionSpans = std::array<std::span<const uint8_t>, numberOfSectionKinds>;   // by SectionKind; empty when absent
    // Takes the mapping, which the destructor unmaps. open fills the spans from validateBody's BodyLayout, so this
    // header needs nothing from JITCacheContainer.h.
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, std::span<const uint8_t> mapping, const SectionSpans&);
};

class BodyLookup {
public:
    enum class Kind : uint8_t { Missing, Unusable, Found };
    static BodyLookup missing();
    static BodyLookup unusable();
    static BodyLookup found(Ref<ValidatedBody>&&);
    Kind kind() const;
    RefPtr<ValidatedBody> body() const;   // non-null exactly for Found
};
```

A `ValidatedBody` the store opens holds a private read-only mapping of one body file that passed the container checks its VM runs, the integrity checks always and the structure checks under strict (container sub-SPEC sections 4.5 and 7.2), so its bytes stay readable and unchanged while it lives, whatever happens to the file (THREAD Storage). A pending import pins it (SPEC-ucb.md section 6.4). Its constructor is private: the store builds a body from a checked mapping, and a test builds one with `createForTesting`, whose buffer no container check reads, since such a body exists to hand a part's own checks crafted bytes (SPEC-ucb.md U4 and U8).

`VMState::bodyVersion(key)` and `VMState::openBody(key)` run on the VM thread, allocate no JSC cell, take only the artifact's index lock (container sub-SPEC section 6.4), and answer UCB R-INT-3. Each returns 0, or `Unusable`, when activity is off; then the override's answer while a test has set one (below); and otherwise the opened artifact's:

- `bodyVersion` returns `OpenedArtifact::token(key)` (container sub-SPEC section 7.1): the key's index token, nonzero while the index lists a body for the key and changed whenever the index learns of another body there (container sub-SPEC section 6.1), or 0 while it lists none. It reads no file and raises nothing. It may lag other processes' commits and maintenance runs by the cadence of container sub-SPEC section 6.3 and II16, which UCB R-INT-3 leaves to the integrator. ([history](SPEC-integrator-history.md#the-index-holds-tokens))
- `openBody` asks `OpenedArtifact::open(key, mode)`, with `ValidationMode::Full` exactly when strict is on, which maps the file and checks it (container sub-SPEC sections 4.5 and 7.2); a key the index lacks is `Absent` without a filesystem call. `Found` gives `Found` with the body. `Absent`, a file that vanished included, gives `Missing`, and the open's `ENOENT` takes the key out of the index unless its token changed meanwhile (container sub-SPEC section 6.4); `Unavailable` (`EMFILE`, `ENFILE` or `ENOMEM`) gives `Missing` and counts in `Progress::transientOpenFailures`, since the artifact is not at fault; `Invalid` raises invalid material at its check, `container.io` with `strerror` of the error in the detail or a container check's name, and gives `Unusable`.

A token is no commit identifier. `ValidatedBody::version()` returns the opened file's commit identifier, the body version THREAD Maintenance names, which the index does not hold, so the two can disagree while the index lags; the UCB lane compares tokens only with tokens and commit identifiers only with commit identifiers (UCB R-INT-3). A miss it stamps with a token is tried again once the index learns of another body at the key (SPEC-ucb.md section 7.3.2).

For the UCB lane a body it cannot read now is as good as none. The capture glue must tell the two apart, so it reads saved scores through a call of its own (section 8.3).

In twins builds, `setBodyLookupForTesting(token, open)` makes `bodyVersion` and `openBody` answer from the two functions alone once the activity test has passed, without the store, the index or a progress counter, until `clearBodyLookupForTesting()`. The first function answers index tokens and the second gives bodies whose `version()` is their commit identifier, so a test keeps the two apart as the store does and can make them disagree as a lagging index does. A part's test serves crafted bodies this way (SPEC-ucb.md U8). The override changes neither the VM's role nor its strictness, which `start` fixed, and invalid material raised through it turns activity off for good like any other, so a test that needs several roles, both strict values or activity on again runs once per configuration, as the runner's sequences do (harness sub-SPEC section 7). ([history](SPEC-integrator-history.md#test-bodies-and-a-lookup-override))

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
    uint32_t llintThreshold;     // L: UCB envelopeLLIntThreshold (THREAD Maintenance)
    uint32_t counterProgress;    // P: the CB lane's counterProgress (SPEC-cb.md section 4.3), 0 when the counter does not travel
    uint8_t highestTier;         // 1
};
struct CommitResult { uint64_t version; uint64_t fileSize; };
struct CommitFailure { ASCIILiteral check; String detail; };   // budget.limit, or a writer.* step of container sub-SPEC section 8.2

class ArtifactWriter {
    WTF_MAKE_NONCOPYABLE(ArtifactWriter);
    WTF_MAKE_TZONE_ALLOCATED(ArtifactWriter);
public:
    ArtifactWriter(OpenedArtifact&, ProducerLock&, ProducerBudget&, size_t stagingBytes);
    ~ArtifactWriter();             // frees the staging buffer and releases its charge
    Expected<CommitResult, CommitFailure> commit(const CommitStamp&, const CommitSections&);
    void releaseStagingBuffer();   // the end of production

#if ENABLE(JITCACHE_TWINS)
    // Container sub-SPEC section 8.3. n counts this writer's commits from 1.
    struct FaultForTesting { ASCIILiteral check; uint64_t n; };   // writer.create, writer.write, writer.reread or writer.publish
    void setFaultForTesting(std::optional<FaultForTesting>);
    // Harness sub-SPEC section 12: raise(SIGKILL) at the point of the n-th commit.
    enum class KillPoint : uint8_t { BeforeCreate, AfterCreate, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterRename };
    struct KillForTesting { KillPoint point; uint64_t n; };
    void setKillForTesting(std::optional<KillForTesting>);
    Expected<CommitResult, CommitFailure> rewriteSection(const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
#endif
};
```

The writer, its file layout and its failure handling are in the container sub-SPEC, section 8. A test builds a `CommitSections` from spans alone (container sub-SPEC test C7). The score of the body the writer publishes is the capture glue's (section 8.4, step 6). ([history](SPEC-integrator-history.md#the-writer-takes-section-sources-and-reads-no-lane-format))

The capture glue builds the list with `CommitSections commitSectionsFor(const UCBSections&, const ImageCapture&, const CBStateCapture&, std::span<const uint8_t> ics)` in `JITCacheCapture.cpp`, in section-kind order. It returns the sources by value, so no array in its frame outlives the call; the bytes and objects they name belong to the three captures and the ICs buffer, which live until step 9 of section 8.4 returns:

| kinds | sources |
|---|---|
| `UCBIdentity`, `UCBCore`, `UCBFeedback` | in memory: `identity`, `core->span()` and `feedback` of `UCBSections` (SPEC-ucb.md section 8.1) |
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
            // last in THREAD Restoration's order.
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
    JITCache::didFinishPrepareForExecution(vm, *codeBlock, scope);   // harness sub-SPEC section 3
#endif
```

`JIT::compileSync` (N2) holds the second and gains, after the `USE(PROTECTED_JIT)` block and before the plan is created:

```cpp
    // JITCache: with the LLInt off, an imported body's first CB installs here.
    if (!Options::useLLInt()
        && JITCache::installAtNewbornCodeBlock(codeBlock->vm(), *codeBlock, JITCache::InstallPoint::CompileSync) == JITCache::InstallOutcome::Installed)
        return CompilationResult::CompilationSuccessful;
```

With the LLInt off, `setupJIT` reaches `compileSync` from `prepareForExecutionImpl` after the sharing-slot test, and an installed import returns success, which `setupJIT`'s `RELEASE_ASSERT` accepts. `prepareForExecutionImpl` then calls `installCode` a second time, as it natively does after `setupJIT` (install.md, step 4). `compileSync`'s other callers (Bun's `vm.Script`, tests) reach the install function too, which does nothing unless every condition of step 1 below holds; the LLInt-on test keeps it out of Bun's route, and the slot test of step 1 covers callers that skipped the native one. ([history](SPEC-integrator-history.md#one-install-function-with-a-point-argument))

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
3. `parseImageSections({ image, bakedFacts, twins }, strict)` with the body's spans; it checks their structure only with strict on (SPEC-image.md section 8.5). A failure is invalid material, part `image`, at `description(check)`; return `Abandoned`.
4. With strict on, `validateImageSectionsAgainst(view, ucb)`; normal mode trusts what it checks (Image R-INT-3). A failure is invalid material as in step 3; return `Abandoned`.
5. `compareBakedFacts(view, newborn)`. `Mismatch` leaves the CB native and keeps the import for the next newborn CB of the UCB, which repeats this sequence from step 1 against the same bytes (CB R-INT-4); count it in `Progress::bakedFactMismatches` and return `KeptForNextCodeBlock`.
6. With `InstallPoint::BeforeSetupLLInt`, `LLInt::shouldJIT(&newborn)`. False: `registry().resolvePendingImport(ucb, import, ImportResolution::DroppedByGate)` and return `DroppedByGate` (THREAD Restoration). With the LLInt off there is no gate, as natively.
7. `prepareImage(vm, ucb, view, budget, strict)`, with `budget` the active production's budget, `producerContextIfActive()->budget()` (section 4.1), when `producerContextIfActive()`, read once here, returns a context, and null otherwise. Since only Consumer and ConsumerProducer VMs import, the budget is non-null exactly in a ConsumerProducer VM whose production is active, Image R-INT-7's condition for rebuilding the record. `PrepareOutcome::InvalidMaterial` is invalid material as in step 3; `ExecutableMemoryExhausted` calls `didFailExecutableAllocation(vm, ExecutableAllocationSite::JITCacheImage)`. Either returns `Abandoned`. ([history](SPEC-integrator-history.md#a-consumerproducer-rebuilds-the-image-record-only-while-production-is-active))
8. `CBStateImport::prepare(cbState, newborn, strict)`. A `CBFault` is invalid material, part `cb`, at `description(check)`; the `PreparedImage` is destroyed without `commit`; return `Abandoned`.
9. `ICs::prepareBaselineICs(icsSection, prepared.code(), newborn, strict ? StrictChecks::Yes : StrictChecks::No)`. An `Invalid` is invalid material, part `ics`, at the check's enumerator name with the site index in the detail; the `PreparedImage` is destroyed without `commit`; return `Abandoned`.
10. When `budget && budget->hasRefused()` holds, raise the recording fault `budget.limit` (section 4.5). The rebuilt record's charge was refused, either itself or because an earlier refusal fails every later charge, and the image goes on without a record (SPEC-image.md section 10.3, step 14). A recording fault ends production and leaves activity on, so the install continues.
11. Seed the CB, which now cannot fail: `cbImport.seedLinkedState(newborn)` and `ICs::seedCallLinkHistory(icsPrepared, newborn)`. The two write disjoint fields of shared metadata entries, in either order (SPEC-cb.md section 6.4, ICs R-CB-1).
12. `Ref<BaselineJITCode> code = WTF::move(prepared).commit(vm, newborn)`: the code-size sample, the cross-modifying fence and the profiler reports (SPEC-image.md section 10.3).
13. `newborn.setupWithUnlinkedBaselineCode(code.copyRef())`, native setup, which builds the `BaselineJITData` from the restored molds and parks the code in the empty sharing slot.
14. `ICs::attachPropertyICState(icsPrepared, newborn)`. Twins builds, while `twinReportSink()` is non-null (R-ALL-2): `ICs::checkRestoredBaselineICs(icsPrepared, newborn)`, each mismatch reported as a difference (ICs R-INT-7).
15. `CBCounterRestore restore = cbImport.finishCounter(newborn)` (SPEC-cb.md section 5.3). Twins builds, while `twinReportSink()` is non-null (R-ALL-2): `cbImport.verifyTwins(newborn, restore, *twinReportSink())` (CB R-INT-6).
16. `newborn.ownerExecutable()->installCode(&newborn)`.
17. `registry().resolvePendingImport(ucb, import, ImportResolution::Installed)`.
18. Count `Progress::installs`. Twins builds: stash the CB, the code, the body and the parsed view for the image twin check that `prepareForExecutionImpl` runs once it returns, in the image twin-check state, which the VM's first stash creates (harness sub-SPEC section 3). Then drop `body`: step 17 released the pending import's reference, so outside twins builds this one is the last and its drop unmaps the body (container sub-SPEC section 7.2). When a bench report is open, the function closes the `install` event's total before the drop, times the drop on its own as `bodyRelease` (IB1), and then records the event (harness sub-SPEC section 9.2). ([history](SPEC-integrator-history.md#measurement-choices))
19. Return `Installed`.

Steps 3 to 9 write nothing to the CB, the UCB or the VM's statistics: the image preparation keeps its effects inside the `PreparedImage` until `commit` (SPEC-image.md I11), and the other two preparations only read (SPEC-cb.md I5, SPEC-ics.md I1). A failure there leaves the CB at its link state, and `prepareForExecutionImpl` goes on with `setupLLInt` or `setupJIT`. Steps 11 to 17 cannot fail: their only cell allocations are the CB lane's realm step and lazy-operand holder, which run under the deferrals of step 2 and crash on exhaustion as their native twins do.

### 7.3 Outcomes

| outcome | the CB | the pending import | next |
|---|---|---|---|
| `NotInstalled` | untouched | untouched | native `setupLLInt` or compilation |
| `Installed` | baseline, installed | resolved `Installed` | `prepareForExecutionImpl` skips its trailing `installCode` (LLInt on) or calls it a second time (LLInt off) |
| `KeptForNextCodeBlock` | untouched | kept | native; the next newborn CB of the UCB tries again |
| `DroppedByGate` | untouched | resolved `DroppedByGate`, which stamps the import's index token as missed (SPEC-ucb.md section 6.4) | native |
| `Abandoned` | untouched | kept, and it dies with its UCB, since activity is now off | native |

### 7.4 Context

Both install points run on the VM thread with the API lock and heap access, inside `prepareForExecutionImpl`'s `DeferGCForAWhile` (N1), which the LLInt-off route reaches through `setupJIT`, and inside the install function's own (step 2). Bun's `vm.Script` route reaches the function with no deferral and returns at step 1 (section 10). No stopped-world GC phase starts while the function runs, and nothing the function calls starts or waits for a collection or releases heap access. The newborn CB is unpublished until step 16 stores it in its executable and write-barriers the executable (install.md, "Conditions around installation"), so no marker or compiler thread reads what steps 11 to 15 write, and the lanes add no fence of their own. The function takes no JSC lock itself; its callees take theirs: `JITThunks::m_lock` and the executable allocator's locks in `prepareImage`, the UCB registry lock in steps 1, 6 and 17, and `CodeBlock::m_lock` in step 14.

## 8. Capture glue

### 8.1 Eligibility

A CB is a candidate when, on the VM thread:

1. `cb.jitType() == JITType::BaselineJIT` and `cb.replacement() == &cb` (THREAD Capture);
2. its `BaselineJITCode`, `static_cast<BaselineJITCode&>(*cb.jitCode())`, satisfies `isImageCapturable` (SPEC-image.md section 9), which is THREAD Capture's "shareable and recorded";
3. `captureRecord(vm, *cb.unlinkedCodeBlock())` returns a record, so its UCB has a recorded key and a context the capture can write (SPEC-ucb.md section 8.1).

The candidate's key is the record's key. Imported CBs qualify like any other: in a ConsumerProducer their code carries the rebuilt record (SPEC-image.md section 10.3, step 14).

### 8.2 Scores

```cpp
struct CaptureScore {
    uint8_t tier { 0 };              // 1 for a baseline capture
    uint64_t richness { 0 };         // the UCB lane's units plus the CB lane's units
    uint32_t icSitesWithCases { 0 };
    bool counterWithheld { false };  // the polymorphic rule withheld the counter (SPEC-cb.md section 4.3)
    uint32_t counterProgress { 0 };
};
struct SavedScore { CaptureScore score; uint64_t version { 0 }; };   // version: the body file's commit identifier

// Compares the fields lexicographically in declaration order, which is THREAD Capture's order, a true counterWithheld
// above a false one. A candidate wins only when it is strictly greater; a remaining tie keeps the saved body.
bool beats(const CaptureScore& candidate, const CaptureScore& saved);

// A saved body's score, read from its three summary sections by the lanes' readers with the session's strict flag.
// With strict off no reader rejects. With it on, the rejection names the reader: part "ucb" at "saved-summary"
// (savedRichness names no check), "cb" with description(check), or "ics" with the enumerator name of the ICs check.
struct SummaryRejection { ASCIILiteral part; ASCIILiteral check; };
Expected<CaptureScore, SummaryRejection> scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics, bool strict);
```

A live candidate's score is tier 1; richness `liveRichness(ucb).total()` plus the `richnessUnits` of `CBStateCapture::scoreLive(cb, strict, ics.hasPolymorphicSite)`; the IC sites with cases of `ics = ICs::summarizeBaselineICs(cb)`, which is `ics.summary.icSitesWithCases`; and the `counterWithheld` and counter progress of the same `scoreLive`. The glue calls the ICs lane before the CB lane, for the same CB in the same pause, and passes its `hasPolymorphicSite` on, so a candidate with a polymorphic site scores its counter as withheld, with no progress, as its capture will carry it (THREAD Restoration; CB R-INT-5, ICs R-INT-3, UCB R-INT-5). A `scoreLive` failure is the CB lane's eligibility or pairing guard under strict (SC2, SC3), a recording fault (SPEC-cb.md section 7).

A saved body's score comes from its three summary sections: the highest tier, richness `savedRichness(ucb.feedback, strict)->total()` plus `decodeSummary(cb.summary, strict)->richnessUnits`, `readBaselineICsSummary(ICsBaseline, strict ? StrictChecks::Yes : StrictChecks::No)->icSitesWithCases`, and `decodeSummary`'s `counterWithheld` and counter progress. `scoreSections` computes it from the three spans of a saved body (section 8.3). With strict off the readers trust the bytes; with strict on a rejected summary is invalid material at the part and check its `SummaryRejection` names, which stops the capture with nothing written, and the UCB lane leaves the empty answer of `savedRichness`, possible only with strict on, to the glue to raise (UCB R-INT-5). A capture about to be committed takes its score from what its builders return instead (section 8.4).

### 8.3 Kept summaries

A producing `VMState` keeps a `HashMap<BodyKey, SavedScore, BodyKeyHash, BodyKeyHashTraits>` of kept summaries (container sub-SPEC section 6.1), charged per entry (section 4.4) before the entry is added. A refused charge raises `budget.limit` and keeps nothing: at a key's first scoring the capture then writes nothing, and a commit charges the entry its step 10 adds at step 8, before writing, so a refusal there also writes nothing (section 8.4). A key with an entry is scored against it. At a key's first scoring the glue reads the saved body once, through the store's scoring read `OpenedArtifact::readSavedSummaries(key, mode)`, in `ValidationMode::Full` exactly when strict is on (container sub-SPEC section 7.3), which opens the body file by its name, whatever the index holds, and tells four cases apart: ([history](SPEC-integrator-history.md#the-scoring-read-opens-by-name))

| result | meaning | the glue |
|---|---|---|
| `Absent` | no saved body: the file does not exist (`ENOENT`) | scores the candidate against nothing, which it beats; keeps nothing |
| `Unavailable` | the body exists, but `EMFILE`, `ENFILE` or `ENOMEM` kept it from being read now | defers the key's capture: no commit, nothing kept, no fault; counts `Progress::capturesDeferred` and `transientOpenFailures` |
| `Invalid` | another I/O error, or a container check of the read's mode failed on the envelope, the directory or a summary section | raises invalid material at the check (`container.io` with `strerror` of the error, or the check's name), which turns activity off, and stops the capture |
| `Found` | the envelope, the directory and the three summary sections passed the checks of the read's mode | computes the saved score with `scoreSections`, releases the mapping and keeps the score |

A deferred key is scored again at the next finalize capture of one of its CBs or at the next `delta`. An unreadable body never scores as absent, since any candidate beats an absent body (THREAD Capture).

A commit replaces the entry with the score of the capture it wrote (section 8.4), so a kept summary always describes the file its key names. The producer lock keeps every other process from committing, so an entry never goes stale while the VM produces. The one other way a body changes then is a test's: in twins builds `JITCacheCapture.h` declares `Expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM&, const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes)`, which requires active production, rewrites the body through the writer's `rewriteSection` (container sub-SPEC section 8.3) and erases the key's entry; the shell's `jitcacheRewriteSection` calls it (harness sub-SPEC section 5.2). Twins builds also declare `std::optional<SavedScore> keptScoreForTesting(VM&, const BodyKey&)`, which returns the key's entry, for T-STAMP. ([history](SPEC-integrator-history.md#the-kept-score-comes-from-the-builders))

### 8.4 Building and committing one capture

`commitCapture(state, cb, record, liveScore, saved)` runs only for a candidate whose live score beats the saved body's, `saved` being that score or the empty `CaptureScore` when no body is saved, in this order:

1. `Ref<BaselineJITCode> code` from `cb.jitCode()`, held until the writer returns, so the code the image section reads stays alive (Image R-INT-6).
2. `captureImage(vm, cb, code, budget, strict)`. `NotEligible` ends this CB's capture without a fault, as for an ineligible CB (Image R-INT-6); `ChargeRefused` raises `budget.limit`; `RecordingFault` raises a recording fault, part `image`, at `description(check)`.
3. `buildSections(vm, cb, budget)`, then `ucbRichness = liveRichness(ucb)`. A `UCBCaptureFailure` raises a recording fault (section 4.5).
4. `size = ICs::baselineICsSectionSize(cb)`; `budget.tryCharge(size)`, a refusal raising `budget.limit`; allocate the buffer; `ics = ICs::captureBaselineICs(cb, buffer, strict ? StrictChecks::Yes : StrictChecks::No)`. A `CaptureError` raises a recording fault, part `ics`, at the enumerator name with the site index (ICs R-INT-2).
5. `CBStateCapture::capture(cb, budget, strict, ics.hasPolymorphicSite)`. A `CBFault` raises a recording fault, part `cb`, at `description(check)`.
6. `committed = CaptureScore { 1, ucbRichness.total() + cbState.score().richnessUnits, ics.summary.icSitesWithCases, cbState.score().counterWithheld, cbState.score().counterProgress }`: the score of the bytes just built, taken from what the builders return, which equals what `scoreSections` would compute from the committed file, so no lane's bytes are read back (II23). When `!beats(committed, saved)`, the capture stops here, with no fault and no commit: the lanes' outputs are destroyed and the ICs buffer is freed and released as in step 10, and the `capture` event records the capture as beaten (THREAD Capture). The live score and `committed` differ only when a marker's or a compiler thread's drain between the scoring and the build added bits by folding in a pending sample or dropped them in a racing merge (SPEC-cb.md section 4; profiles.md, "Drain"). ([history](SPEC-integrator-history.md#the-kept-score-comes-from-the-builders))
7. `CommitStamp { record.key, envelopeLLIntThreshold(ucb), cbState.score().counterProgress, 1 }`, the L and P compact orders by (UCB R-INT-5, CB R-INT-5, THREAD Maintenance). P is zero for a body whose counter does not travel, as for every body the ICs lane reported a polymorphic site for (ICs R-INT-2).
8. Charge, before anything is written, the entries the commit adds. When `artifact.containsKey(record.key)` is false, the writer will add an index entry: `budget.tryCharge` six of the index's bucket sizes (section 4.4), a success adding to the state's index charge. When the kept summaries hold no entry for the key, as after an `Absent` scoring read, step 10 will add one: charge it as section 8.3 does. A refusal of either raises `budget.limit` with nothing written.
9. `writer.commit(stamp, commitSectionsFor(ucbSections, image, cbState, buffer))` (section 6.3). A `CommitFailure` raises a recording fault at its check, `budget.limit` or a `writer.*` step, with its detail.
10. On success: the kept summary becomes `SavedScore { committed, result.version }`, in an entry step 8 charged when it is new, `Progress::capturesCommitted` and `bytesCommitted` grow, and the bench event is recorded. The writer's reread checked every section of the file against the checksum computed from these same bytes as they were streamed, so the kept score describes the file. The lanes' outputs are then destroyed, which releases their charges (UCB R-INT-5), and the ICs buffer is freed and released.

A failure at any step writes nothing: the glue destroys the lanes' outputs, which releases their charges, and frees the ICs buffer and releases its charge.

The image comes first because it is the only builder that can declare the CB ineligible. The ICs lane comes before the CB lane because the CB lane's capture takes the polymorphic bit the ICs capture returns, for the same CB in the same pause (THREAD Restoration; CB R-INT-5, ICs R-INT-2); otherwise the order is THREAD's. All four lanes' sections of one capture go into one body in one commit, so `ucb.*`, `cb.state` with `cb.summary`, and the image sections are always committed together (UCB R-INT-5, CB R-INT-5).

### 8.5 The finalize capture

`BaselineJITPlan::finalize` calls `JITCache::didFinalizeBaselineCompilation` at the end of its `CompilationSuccessful` case, after `installCode` and `jitSoon` (THREAD Capture; section 9 shows the edit). `JITCacheGlue.h` declares the hook with the timing its `compile` event records, so the signature task 0 declares is the one task 9 calls and task 16 fills:

```cpp
struct BaselineCompileTiming {
    uint64_t compileNanoseconds;    // compileInThreadImpl after its profile drain (harness sub-SPEC section 9.3)
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

Bun's `vm.Script` route reaches this hook with no deferral and stops at its step 4 (section 10). In twins builds the function first adds one to the `baselineCompiles` of the CB's UCB, in every VM (harness sub-SPEC section 10.1).

### 8.6 `delta`

After the checks of section 3.4:

1. Open `DeferGCForAWhile`.
2. When the budget has refused a charge, raise `budget.limit` and return `Faulted`.
3. Phase 1. Take `vm.heap.codeBlockSet().getLock()` and walk `vm.heap.forEachCodeBlockIgnoringJITPlans(locker, ...)`, which completes no pending plan (THREAD Capture, N7). For each candidate (section 8.1), compute its live score and keep it under its key, the key's candidates in decreasing order under `beats`, a tie after the ones met earlier. The candidate table, a map from key to a vector of its candidates' `CodeBlock*` and scores keyed through `BodyKeyHash` and `BodyKeyHashTraits` (container sub-SPEC section 6.1), is charged per key as section 4.4 charges a table entry, and per candidate for the vector slot it takes; a refused charge, or a recording fault from a scoring call (the CB lane's eligibility or pairing guard, SC2 or SC3), ends the walk and makes `delta` return `Faulted` after the lock is released. The functor allocates no cell and does no I/O; the only locks it takes are the ones its lane calls take (section 10). Release the lock.
4. Phase 2. For each key of the candidate table, which `commitCapture` does not change: score the saved body once (section 8.3); when the key's first candidate beats it, run `commitCapture`. Commits are independent (THREAD Capture), so their order is the table's. An `Unavailable` saved body defers the key. A `NotEligible` image makes that CB an ineligible one (Image R-INT-6), so the key's next candidate gets the same test, until one commits or does not beat the saved body; another live CB of the key can run a different `BaselineJITCode`, as after the sharing slot was released and the body recompiled. The loop then goes on with the next key. A recording fault or invalid material stops the loop, and `delta` returns `Faulted` with the counts committed before the fault.
5. Free the candidate table, flush the bench report, count `Progress::deltaRuns`, and return `Completed` with `eligibleKeys`, `committedBodies`, `committedBytes` and `deferredKeys`.

The `CodeBlock*` kept between the phases stay valid: each was in the CodeBlock set, so it was marked at the last End phase or created since (N7), and nothing between the phases can complete a collection, since the VM thread holds heap access, reaches no stop point and allocates no cell, and lazy sweeping runs only in allocation. THREAD Capture states the same condition for the reads themselves. ([history](SPEC-integrator-history.md#delta-scores-in-two-phases))

### 8.7 Context

All capture glue runs on the VM thread with the API lock and heap access, JS paused, and no collector phase on the thread: the finalize capture inside door 1's deferral (N8; install.md, "Conditions around installation") and `delta` inside its own (section 8.6, step 1). Bun's `vm.Script` route reaches the finalize hook with no deferral and stops at its step 4 (section 10). The glue calls nothing that drains a profile, materializes a property table, allocates a cell or stops for the collector, and never finalizes a plan (THREAD Failures). Its only file I/O is the writer's and the saved body's read, and neither touches the heap.

Captures therefore never nest: a capture calls no host or JS function (THREAD Session), no lane's capture call finalizes a plan or walks the CodeBlocks with plan completion (SPEC-ucb.md section 8, SPEC-image.md section 9, SPEC-cb.md section 4, SPEC-ics.md section 5), and the writer does only file I/O. Builds with `ASSERT_ENABLED` check it: the state counts the captures in progress, and the finalize capture and `delta` assert on entry that none is. ([history](SPEC-integrator-history.md#captures-cannot-nest))

## 9. Plan-site faults

These are the plan sites THREAD Execution gives the integrator, and each raises the fault before the failure's effects are written, as THREAD Failures requires. They meet UCB R-INT-11 and CB R-INT-12; section 4.5 lists the other sites.

`BaselineJITPlan::finalize` becomes:

```cpp
    case CompilationResult::CompilationFailed:
        // JITCache: a baseline plan fails only for lack of executable memory (N3; SPEC-ucb.md F24); the fault comes before the effects below.
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
        JITCache::didFinalizeBaselineCompilation(*m_vm, *m_codeBlock, m_jitCacheMeasure ? &jitCacheTiming : nullptr);   // section 8.5
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
| `OpenedArtifact::m_indexLock` (`Lock`) | the index, its refresh state and the reads of the inotify descriptor | the store's reads, the listings, `containsKey` and the writer's index update (container sub-SPEC section 6.4) |
| the producer lock (`flock` on the lock file, held through a `ProducerLock`) | the right to write the artifact | a producing VM from `start` to `didFinalizeHeap`; maintenance while it plans and applies |

Both in-process locks are leaves: nothing that takes another lock is called while either is held (container sub-SPEC sections 5.1 and 6.4 list what runs under each). A thread holding one therefore never waits on another JSC or JITCache lock. `VMState` has no lock: its plain fields are written and read on the VM thread, and the fields other threads read are atomics.

Lock order with JSC's locks:

- `delta` phase 1 holds `CodeBlockSet::m_lock` while the lanes' scoring calls take `CodeBlock::m_lock` (`summarizeBaselineICs`), `UnlinkedCodeBlock::m_lock` (`liveRichness`) and the UCB registry lock (`captureRecord`). Native code never takes `CodeBlockSet::m_lock` while holding any of these: `CodeBlockSet::add` runs in the `CodeBlock` constructor on the VM thread, and the conservative scan takes only the set's lock (SPEC-ics.md L2). The integrator's own locks are never taken under `CodeBlockSet::m_lock`.
- The UCB engine calls `bodyVersion` and `openBody` without holding its registry lock (SPEC-ucb.md section 6.3).
- `didFailExecutableAllocation` may run under `CodeBlock::m_lock` and takes no lock.

Threads: the VM thread runs everything except `producerContext` and the budget, which JIT workers use through the Image lane's recorder, `didAttachDebugger`, which may run anywhere, and the destructors of `ValidatedBody` and of the lanes' records, which may run on any thread holding no JITCache lock.

GC: `start`, `status`, the index and the writer allocate no cell. The install glue runs under two deferrals (section 7.4) and the capture glue under door 1's or `delta`'s (section 8.7). On the engine's routes the plan sites run inside a finalization under `DeferGC` in `completeAllReadyPlansForVM` (N8, N9), or, when `JITWorklist::enqueue` finalizes on the spot with concurrent JIT off, under the `DeferGCForAWhile` its caller holds (`jitCompileAndSetHeuristics`, `operationOptimize`, the FTL tier-up operations), as `prepareForExecutionImpl`'s does for `setupJIT`. The teardown hooks run after `VM::~VM` has deferred GC for good (N5).

Bun's `vm.Script` route calls `JIT::compileSync` after its own `DeferGC` has closed (N2; install.md, "Embedder routes"), so the glue it reaches runs with no deferral, and none of it needs one. With the LLInt off it reaches the install function (section 7.1), whose step 1 makes only its loads and one registry lookup and returns `NotInstalled`, because the UCB Bun's decode yields has no record (SPEC-ucb.md section 7.2.5). Its `BaselineJITPlan::finalize` reaches either the finalize hook, whose steps 1 to 4 touch no heap and whose step 4 fails for a CB whose UCB has no record (section 8.1, item 3), or, when the plan fails, the fault entry point, which allocates nothing and touches no heap (section 4.5). Neither function assumes a deferral.

Fences: `VM::setJITCacheState` stores with release order and `jitCacheState` loads with acquire order. Activity and production are `std::atomic<bool>`s turned off with release stores and read with acquire loads by other threads. The budget's counters are atomics. The lock file's commit epoch is written with release and read with acquire across processes (container sub-SPEC section 2).

## 11. Hosts

THREAD Session says when each host calls `start` and what the host decides.

### 11.1 The jsc shell

`CommandLine::parseArguments` (`jsc.cpp`) recognizes these flags before its generic `--` option fallback (N14), and `printUsageStatement` lists them:

| flag | effect |
|---|---|
| `--jitcache=<path>` | configures the shell's main VM with `artifactPath = path` |
| `--jitcache-role=consumer\|producer\|consumer-producer` | the role; `consumer` when absent |
| `--jitcache-producer-limit=<bytes>\|max` | the producer limit, in bytes with an optional `K`, `M` or `G` suffix in powers of 1024; `max` is `SIZE_MAX` |
| `--jitcache-strict[=0\|1]` | strictness: bare or `=1` turns it on, `=0` off; off when absent, the default THREAD Session gives |
| `--jitcache-delta-at-exit` | calls `delta` once the scripts and the run loop have finished |
| `--jitcache-log[=0\|1]` | bare or `=1` writes `toJSON` of the start result, of each `delta` result the shell gets and of the final `status` to stderr, one line each; `=0` writes nothing |
| `--jitcache-bench-report=<path>` | `Config::benchReportPath` |
| `--jitcache-maintenance` | runs the maintenance command line on the script arguments and exits (maintenance sub-SPEC section 5) |

Twins builds add the flags and functions of the harness sub-SPEC, section 5. The last occurrence of a flag wins. A malformed value is reported like a bad JSC option and fails the shell under `validateOptions`. Bun takes the same flags with the same syntax, except `--jitcache-delta-at-exit` and `--jitcache-maintenance` (section 11.2).

`runJSC` calls `JITCache::start` under `JSLockHolder` right after `VM::create` for the main VM, before the loop creates the first `GlobalObject`, when `--jitcache` was given; the `$262.agent` workers' VMs are not configured. At the end of the loop's body, which runs once outside Fuzzilli's REPRL mode, once the run loop has returned (`vm.deferredWorkTimer->runRunLoop()`) or the iteration ended on a termination, and while the iteration's `GlobalObject` is still in hand, it takes `JSLockHolder` and, in this order:

1. calls `JITCache::delta` when `--jitcache-delta-at-exit` was given;
2. in twins builds, writes the body-event dump (harness sub-SPEC section 10.3) to the path `--jitcache-body-events` names;
3. in twins builds, writes the description of the heap JavaScript can reach (harness sub-SPEC section 6) to the path `--jitcache-describe-heap` names, whose full collection must change no capture of step 1 and would retire live UCBs before step 2;
4. prints the final status when `--jitcache-log` was given;
5. calls `JITCache::flushBenchReport`, so a run without `--destroy-vm` keeps its bench events.

A script that calls `quit()` exits inside the call and skips all of these, so the runner's scripts end by returning. `jscmain` runs `--jitcache-maintenance` right after the command line is parsed, before `JSC::initialize()`, and exits with its code.

### 11.2 Bun

`src/jsc/bindings/JITCacheHost.h` and `.cpp` (new, `namespace Bun`) hold Bun's side: ([history](SPEC-integrator-history.md#bun))

- `JITCacheHost::configureVM(JSC::VM&, int32_t executionContextId)`, called from `Zig__GlobalObject__create` right after `JSC::JSLockHolder locker(vm)`, so `start` runs with the API lock and heap access (Bun acquires heap access just before) and before `JSVMClientData::create` and the first global object (N15).
  - It classifies the VM by `executionContextId` (N15): 1 is the main thread's VM; a value above 1 and below `INT32_MAX` is a worker's VM; any other value, `INT32_MAX` for macro VMs and the debugger thread's VM included, marks a VM that is neither, which is not configured. Nor are the bytecode builder's VM and Bake's production global, which do not go through this function.
  - The configuration comes from Bun's command line. `RUNTIME_PARAMS_` (`src/runtime/cli/Arguments.rs`) declares the flags of section 11.1 with the same names and value syntax, except `--jitcache-delta-at-exit`, since Bun calls `delta` at exit, and `--jitcache-maintenance`, since Bun's maintenance is `bun jitcache`; the two booleans take an optional value, as `--inspect` does. Twins builds also declare every test and bench flag of the harness sub-SPEC, section 5.1. `Arguments::parse` reads them once per process with Bun's other runtime flags into `RuntimeOptions` (`src/options_types/context.rs`), and the C++ side reads them through `extern "C" const Bun::JITCacheFlags* Bun__JITCache__flags()`, declared in `JITCacheHost.h`. Every VM `configureVM` configures reads them there, workers included. Without `--jitcache`, JITCache stays off. With `--jitcache-log` on, the host writes `toJSON` of the start result, of the `delta` result at exit and of the final status at exit, the last in every role, to stderr, one line each. A malformed value is a command-line error, as for any of Bun's flags.
  - When a producing role gets `Busy`, which a worker meets while its process's main VM produces, the host calls `start` once more with role `Consumer` (THREAD Session: busy records nothing); the worker then shares the main VM's opened artifact and index, which each of the main VM's commits updates at once (container sub-SPEC sections 5.1 and 8.2). Code that only workers run is therefore not captured.
  - A worker VM's report paths, bench and twin alike, get the suffix `.<executionContextId>`; the main VM's are the configured paths, which is where the runner looks for them (harness sub-SPEC section 7.7).
  - Twins builds give each test flag the effect harness sub-SPEC section 5.1 gives it in the shell, at the matching point: per process in `JSCInitialize`, around `JSC::initialize`, for what the shell does in `jscmain`, and per VM in `configureVM` for what it does in `runJSC`. `--jitcache-twins-report` is `Config::twinReportPath` and `--jitcache-body-events` the path of the body-event dump (harness sub-SPEC section 10.3), the latter suffixed for a worker VM as the report paths are. For the main thread's VM the host calls `recordVMLayout` with `--jitcache-twins-record-layout` when it is given (harness sub-SPEC section 4), and `JSCInitialize` passes `--jitcache-test-image-hook` to `setImageTestHookNamed` before `JSC::initialize`. ([history](SPEC-integrator-history.md#buns-twins-build-reaches-only-exported-headers))
- `extern "C" void Bun__JITCache__atExit(JSC::JSGlobalObject*)`, which takes `JSLockHolder`, calls `delta` when the VM's state produces with production active, writes the log lines of that `delta` result, if any, and of the final status when `--jitcache-log` is on, in twins builds writes the body-event dump with `writeBodyEvents` when its path is set, whether or not JITCache is configured (harness sub-SPEC section 10.3), and then calls `flushBenchReport` in every role. Bun destroys its main VM at exit only under `BUN_DESTRUCT_VM_ON_EXIT` (`VirtualMachine::should_destruct_main_thread_on_exit`), so `willDestroyVM` does not run by default, and without this flush a consumer, which never calls `delta`, would lose every event still buffered. `VirtualMachine::on_exit` (`src/jsc/VirtualMachine.rs`) calls it right after `ExitHandler::dispatch_on_exit(self)`, on the main thread and in workers, so the user's exit handlers have run and teardown has not begun. Process exit is thus Bun's idle point, as it is for the Node compile cache (N15); THREAD's "no implicit `delta`" binds JITCache, and this call is the host's.
- `extern "C" int Bun__JITCache__runMaintenance(int argc, const char* const* argv)` for `bun jitcache` (maintenance sub-SPEC section 5).

`scripts/build/deps/webkit.ts` passes `-Wl,--build-id=sha1` in `CMAKE_EXE_LINKER_FLAGS` for local Linux builds, so the `jsc` executable carries a build ID as Bun's own executable does (N15, THREAD Storage). No other compile or link flag changes, in any profile: THREAD Verification keeps Bun's build flags, under which the engine sits at its link-time address (N15), and the twins profile only switches the test builds on (harness sub-SPEC, section 1).

## 12. Requirements on the other parts

The integrator owns these interfaces; each requirement names what the parts provide or the call they make into the integrator's API.

- R-ALL-1. Every call of `didFailExecutableAllocation` passes its site (section 4.5): SPEC-ics.md E5 `ExecutableAllocationSite::InlineCacheHandler`, SPEC-image.md's `MathICRegeneration::didFailToAllocate` `ExecutableAllocationSite::MathICSnippet`.
- R-ALL-2. Every twin check reports through `TwinReport::difference`, `skip` and `relocationCoincidence` (harness sub-SPEC section 2), and only while `VMState::twinReportSink()` is non-null.
- R-ALL-3. Every part's C++ tests register with `JITCACHE_TEST` or `JITCACHE_TEST_WITH_OPTIONS` and live in the files the `testjitcache` target lists (harness sub-SPEC section 8); a test that needs process-wide options declares them in its registration.
- R-ALL-4. Every part's JS tests follow the runner's directives and argument conventions (harness sub-SPEC section 7). Since the oracle compares the heap JavaScript can reach as well as the output (harness sub-SPEC sections 6 and 7.6), a script prints only values that do not depend on its role and leaves none reachable when it ends: a jsc-hosted script runs its body in a function that receives the role and paths as parameters. Under the `Off` role, which the oracle's runs pass (harness sub-SPEC section 7.6), a script runs its Consumer path without `delta` and without any assertion about the state JITCache imported, seeded, attached or captured; assertions about native behavior, such as results, a thrown error or Bun's `cachedDataRejected`, run in every role. Plain builds register none of the jsc functions, `bun:jsc` exports and `$vm` helpers of harness sub-SPEC sections 5.2 to 5.4, so a script that calls one declares `jitcache-requires: twins`, which keeps it out of plain mode, or calls it only after testing that it exists. The pin has none of them either, so a jsc-hosted script whose `Off` run calls one unguarded, or whose optimizing compiles the two native fixes change, declares `jitcache-pin: off` with its reason (harness sub-SPEC sections 7.2 and 11.4). The runner fails a run that reports a fault or a `Consumer` or `ConsumerProducer` run that installs no body (harness sub-SPEC section 7.5), so a script declares with `jitcache-expect-fault` each fault one of its runs reports on purpose, and with `jitcache-expect-no-install` each `Consumer` or `ConsumerProducer` run it leaves with nothing to install. ([history](SPEC-integrator-history.md#the-runner-fails-undeclared-faults-and-runs-that-install-nothing))
- R-ALL-5. The UCB engine calls `bodyVersion` and `openBody` on the VM thread without holding its registry lock, and reads `Unusable` as "the integrator raised the fault" (SPEC-ucb.md sections 6.3 and 7.3.1).
- R-ALL-6. `description(ImageCheck)` and `description(CBCheck)` return `ASCIILiteral`s with static storage, which a `FaultReport` keeps (SPEC-cb.md section 6.1 declares its function so).
- R-ALL-7. A part's test that needs a body no producer committed builds it with `ValidatedBody::createForTesting` and, when the code under test looks bodies up, serves it with `VMState::setBodyLookupForTesting` (section 6.2), in twins builds: SPEC-ucb.md U4 builds the pending import whose body asserts in its destructor, and U8 stubs the lookup.
- R-ALL-8. Unified sources compile several `jitcache/*.cpp` files of different parts as one translation unit, where their anonymous namespaces merge and their file-static names meet (M1). Every helper and constant at namespace scope that a part's file keeps to itself, `static` or in an anonymous namespace, therefore has a name that begins with its part's prefix (`ucb`, `image`, `cb`, `ics`, `integrator`) or sits in a per-file named namespace such as `JSC::JITCache::ContainerInternal`. Section 14.1 holds the integrator's files to it.

## 13. Invariants

- II1. A VM has at most one `VMState`. `start` creates it for `Created`, `Opened` and `Fault` and for no other outcome, and `didFinalizeHeap` destroys it after `Heap::lastChanceToFinalize` (sections 3.2 and 4.6).
- II2. Activity and production only go from on to off, and production is off whenever activity is (section 4.2).
- II3. `producerContext(vm)` is non-null exactly while the role produces, activity is on and production is active, and the object it returns lives until the state is destroyed (section 4.4).
- II4. Every byte the integrator allocates for production is charged before the allocation and released exactly once; section 4.4 lists what is not production memory.
- II5. Every file whose name has the body pattern in `cache/bodies/` was published whole: the writer renames only a temporary that it reread and validated. The writer reads no lane format (container sub-SPEC section 8.2).
- II6. No published body changes in place; a body is replaced only by `renameat` over it (container sub-SPEC section 8.2).
- II7. At most one VM or maintenance run holds the producer lock, and a producing VM holds it from `start` until its state is destroyed, unless `start` faulted (section 3.2; container sub-SPEC section 2).
- II8. Unless a test's lookup override is set, `bodyVersion` reads no file: it returns the key's index token while the index lists a body for the key and 0 otherwise, and a key's token changes whenever the index learns of another body at that key. A token is never compared with a commit identifier (section 6.2; container sub-SPEC section 6).
- II9. A `ValidatedBody`'s bytes stay readable and unchanged while it lives (section 6.2; container sub-SPEC section 7.2).
- II10. The install glue acts only on a newborn CB (JIT type `None`, no `BaselineJITData`, not yet installed) whose UCB's sharing slot is empty and holds a pending import (section 7.2, step 1).
- II11. The install glue writes nothing to the CB before every fallible step has succeeded, and after the first write no step can fail (section 7.2).
- II12. The install glue calls the lanes in the order of section 7.2, which is THREAD's.
- II13. A body is written only when its capture beats the saved body under `beats`, by its live score and again by the score of the bytes built (section 8.4, step 6), and a kept summary always describes the file its key names (section 8.3). A saved body that a transient error keeps from being read defers its key's capture and never scores as absent.
- II14. Capture runs only on the VM thread, with JS paused, heap access and no collector phase on the thread; a capture that writes runs inside a GC deferral, door 1's or `delta`'s, while Bun's `vm.Script` route reaches the finalize hook with none and stops at its step 4. It allocates no cell, stops for no collector, finalizes no plan and calls no host or JS function, so captures never nest; neither the finalize hook nor `didFailExecutableAllocation` asserts a deferral (sections 8.7 and 10).
- II15. Every `didFailExecutableAllocation` call precedes the effects of the failure it reports (sections 4.5 and 9).
- II16. In a process whose opened artifact found the lock file at its build, every VM's lookups see another process's commit no later than the first lookup after that process bumps the epoch, unless a listing failed since; an object without inotify, or with a listing pending, sees it no later than the first lookup after both that bump and `fallbackListingIntervalMilliseconds` from its last listing's start (container sub-SPEC section 6.3). Lookups see a commit by a VM of the same process from its publication on.
- II17. The header's build IDs cover the main executable and the object holding `codeSymbolAnchor` (section 5.2).
- II18. No header in `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS` includes an unexported `jitcache/` header (section 3.5).
- II19. Neither VM destruction nor process exit commits a body (THREAD Failures; section 4.6).
- II20. Maintenance never leaves an incomplete body; deleting a whole artifact removes the header only after every body is gone, and keeps it when a body's unlink fails (maintenance sub-SPEC section 4.5).
- II21. The VMs of a process that open one artifact share one `OpenedArtifact`, which is built and refreshed the same way whatever their roles (container sub-SPEC section 5.1).
- II22. For each CB the capture glue scores or captures, it calls the ICs lane before the CB lane in the same pause and passes the ICs lane's `hasPolymorphicSite` to the CB lane, so a body with a polymorphic site scores and commits its counter as withheld, carries no baseline counter progress and has an envelope P of zero (sections 8.2 and 8.4).
- II23. A committed body's kept score equals what `scoreSections` computes from its file: it is built from the scores the lanes' builders return for the bytes they wrote, and no capture reads its own bytes back (section 8.4, step 6).

## 14. Files, native edits and the manifest

No other part edits the functions of section 14.2. Implementers may not edit the shared hot files; integrator tasks apply the entries of section 14.4, the manifest for `INTEGRATE-integrator.md`, which gather every part's manifest. Each entry names the one task that applies it (section 17), and that task comes after the tasks that write whatever the entry references, so the tree builds after every task.

### 14.1 New files

| file | contents |
|---|---|
| `JITCacheAPI.h`, `.cpp` | `Config`, `start`, `status`, `delta` and their result types (section 3); the `.cpp` defines `start`, `status`, the `toJSON` functions, the state's constructor and destructor and the teardown hooks (sections 4.1 and 4.6), and `JITCacheCapture.cpp` defines `delta` |
| `JITCacheMaintenance.h`, `.cpp` | the maintenance backend and command line (maintenance sub-SPEC) |
| `JITCacheVMState.h`, `.cpp` | `VMState` (section 4.1): the switches, the fault records and the members that raise faults, `producerContext`, the body lookups of section 6.2 and, in twins builds, their override |
| `JITCacheFaults.h`, `.cpp` | `ExecutableAllocationSite`, `didFailExecutableAllocation`, `didAttachDebugger` (section 4.5) |
| `ProducerBudget.h`, `.cpp` | `ProducerBudget`, `ProducerContext`, `producerContext` (section 4.4) |
| `ValidatedBody.h`, `.cpp` | `SectionKind`, `ValidatedBody`, `BodyLookup` and, in twins builds, `ValidatedBody::createForTesting` (sections 6.1 and 6.2) |
| `JITCacheOptions.h`, `.cpp` | the option table and its check (section 5.1) |
| `JITCachePlatform.h`, `.cpp` | build IDs, the CPU feature vector, CRC32C (section 5.2, container sub-SPEC section 4.4) |
| `JITCacheContainer.h`, `.cpp` | header and body formats, the section type ids, validation (container sub-SPEC sections 3 and 4) |
| `ArtifactStore.h`, `.cpp` | `ProducerLock`, `ArtifactRegistry`, `OpenedArtifact`, the index, the store's reads (index tokens, opened bodies, saved scores), and in twins builds the store's fault hook and registry hook (container sub-SPEC sections 2 and 5 to 7) |
| `ArtifactWriter.h`, `.cpp` | the writer and the types the capture glue hands it (section 6.3), and in twins builds its fault injection, kill points and `rewriteSection` (container sub-SPEC section 8) |
| `JITCacheGlue.h` | the entry points native code calls: install, capture, teardown and bench hooks, and in twins builds the image check's hook (harness sub-SPEC section 3) |
| `JITCacheInstall.cpp` | the install glue (section 7) and, in twins builds, the image twin-check state (harness sub-SPEC section 3) |
| `JITCacheCapture.h`, `.cpp` | the capture glue and `delta` (section 8); the header declares the scores of section 8.2, which the tests call, and in twins builds `rewriteSectionForTesting` and `keptScoreForTesting` (section 8.3) |
| `JITCacheParameters.h` | the named parameters of section 16 |
| `JITCacheBench.h`, `.cpp` | the bench report (harness sub-SPEC section 9) |
| `TwinReport.h`, `.cpp` | `TwinReport` (harness sub-SPEC section 2), twins builds only |
| `JITCacheBodyEvents.h`, `.cpp` | the VMs' retired body-event totals, `retireBodyEventCounts` and the totals' erasure at `didFinalizeHeap` (harness sub-SPEC section 10.3), twins builds only |
| `JITCacheTwinsHarness.h`, `.cpp` | the header declares the reachable-heap description and what only the jsc shell calls, `setForcesBlindingForTesting` among it (harness sub-SPEC sections 5, 6 and 11.5); the `.cpp` defines those, `forcesBlindingForTesting`, which `MacroAssembler.h` declares, and everything `JITCacheTwinsHost.h` declares; twins builds only |
| `JITCacheTwinsHost.h` | what a host calls in twins builds (section 3.5); declared only in twins builds and defined in `JITCacheTwinsHarness.cpp` |
| `tests/JITCacheTest.h`, `tests/testjitcache.cpp` | the C++ test framework and runner (harness sub-SPEC section 8) |
| `tests/IntegratorTests.cpp`, `tests/ContainerTests.cpp`, `tests/StoreTests.cpp`, `tests/WriterTests.cpp`, `tests/MaintenanceTests.cpp` | the integrator's C++ tests |

Of these, `JITCacheAPI.h`, `JITCacheMaintenance.h` and `JITCacheTwinsHost.h` are exported to Bun (section 3.5). `ArtifactStore.h`, `ArtifactWriter.h`, `JITCacheCapture.h`, `JITCacheContainer.h`, `JITCacheOptions.h` and `JITCachePlatform.h` are the integrator's private headers; the lanes include `JITCacheVMState.h`, `JITCacheFaults.h`, `ProducerBudget.h`, `ValidatedBody.h`, `JITCacheBench.h`, `TwinReport.h` and `tests/JITCacheTest.h`.

Outside `jitcache/`: `Tools/Scripts/run-jitcache-tests` (the runner), `Tools/Scripts/jitcache-pin-compare.ts` (the pin comparison the runner calls, harness sub-SPEC section 11) and `JSTests/jitcache/integrator/` (the integrator's JS tests). The integrator's files follow R-ALL-8 with the prefix `integrator` or a per-file named namespace such as `JSC::JITCache::ContainerInternal`, as `Heap.cpp`'s `HeapInternal` does, so nothing at namespace scope has a generic name.

### 14.2 Native edits in this repository

| file | function or member | edit | context |
|---|---|---|---|
| `runtime/ScriptExecutable.cpp` | `ScriptExecutable::prepareForExecutionImpl` | install point before `setupLLInt`, the trailing `installCode` skipped after an import, the twin check at the end (section 7.1) | VM thread, `DeferGCForAWhile` |
| `runtime/ScriptExecutable.cpp` | `ScriptExecutable::installCode(VM&, CodeBlock*, CodeType, CodeSpecializationKind, Profiler::JettisonReason)` | a bench timer around `unlinkOrUpgradeIncomingCalls` (harness sub-SPEC section 9.3) | VM thread, or a collection's End phase (harness sub-SPEC section 9.3) |
| `jit/JIT.cpp` | `JIT::compileSync` | the LLInt-off install point at its top (section 7.1) | VM thread |
| `jit/BaselineJITPlan.h`, `.cpp` | `BaselineJITPlan::BaselineJITPlan(CodeBlock*)`, `BaselineJITPlan::finalize`, `BaselineJITPlan::compileInThreadImpl(JITCompilationEffort)`, four new members and `jitCacheRecordsImage()` | whether the compilation records (section 4.4, SPEC-image.md R-INT-12: the constructor sets `m_jitCacheRecordsImage`), the plan-site fault (section 9), the finalize capture (section 8.5), bench timing (harness sub-SPEC section 9.3: the constructor sets `m_jitCacheMeasure`) | VM thread for the constructor and `finalize`; compile thread for `compileInThreadImpl` |
| `dfg/DFGPlan.h`, `.cpp` | `DFG::Plan`: new `noteExecutableAllocationFailure()` and `m_failedForLackOfExecutableMemory`; `Plan::compileInThreadImpl` (the two `state.allocationFailed` branches); `Plan::finalize` | the plan-site fault (section 9); in twins builds, `Plan::finalize` also counts a successful optimizing compile (harness sub-SPEC section 10.1) | compile thread; VM thread for `finalize` |
| `dfg/DFGSpeculativeJIT.cpp` | `SpeculativeJIT::compile`, `SpeculativeJIT::compileFunction` (the `linkBuffer.didFailToAllocate()` branches) | note the failure on the plan (section 9) | compile thread |
| `dfg/DFGOSRExitCompilerCommon.cpp` | `handleExitCounts` | twins builds: emit the increment of the body's `osrExits` (harness sub-SPEC section 10.1) | VM thread, compiling an exit; the code runs on the VM thread |
| `bytecode/CodeBlock.cpp` | `CodeBlock::jettison` | twins builds: count the jettison and the reoptimization (harness sub-SPEC section 10.1) | VM thread, or a collection's End phase on the thread that drives it |
| `llint/LLIntOfflineAsmConfig.h` | new `OFFLINE_ASM_JITCACHE_TWINS`, from `ENABLE(JITCACHE_TWINS)` | the offlineasm setting the LLInt's count tests (harness sub-SPEC section 10.1) | build time |
| `llint/LowLevelInterpreter.asm` | new macro `countJITCacheInstruction`; macro `traceExecution`; labels `_llint_op_call_direct_eval`, `_wide16` and `_wide32` | under `if JITCACHE_TWINS`, add one to the frame's UCB's `llintInstructions`, from `traceExecution` and at the head of the three labels (harness sub-SPEC section 10.1) | VM thread, at every LLInt instruction |
| `assembler/MacroAssembler.h` | `MacroAssembler::shouldConsiderBlinding`; the declaration of `JSC::JITCache::forcesBlindingForTesting()` with `JS_EXPORT_PRIVATE` | twins builds: forced blinding (harness sub-SPEC section 11.5) | any thread that emits code |
| `jit/JITThunks.h`, `.cpp` | `JITThunks`: new `m_supportGenerations`, `noteSupportGeneration()` and `supportGenerations()`; `JITThunks::ctiStubImpl`, `JITThunks::lazyCommonThunk` | count each generation of per-VM support for the bench (harness sub-SPEC section 9.3) | VM thread or compile thread, under `m_lock` or the lazy thunk's lock |
| `bytecode/InlineCacheHandler.cpp` | `InlineCacheCompiler::generateSlowPathHandler` | count a handler the VM lacked through `vm.jitStubs->noteSupportGeneration()` (harness sub-SPEC section 9.3) | VM thread |
| `runtime/CachedTypes.cpp` | `crc32c` | external linkage: `static` removed, so the container's checksum is this function (container sub-SPEC section 4.4) | any thread |
| `llint/LLIntSlowPaths.h`, `.cpp` | `shouldJIT` | external linkage, declared in the header (section 7.1) | VM thread |
| `debugger/Debugger.cpp` | `Debugger::attach` | `JITCache::didAttachDebugger(m_vm)` right after `setShouldBuildPCToCodeOriginMapping` (section 4.5) | any thread |
| `jsc.cpp` | `CommandLine` (new fields), `CommandLine::parseArguments`, `printUsageStatement`, `runJSC`, `jscmain`, `GlobalObject::finishCreation` | the jsc host (section 11.1, harness sub-SPEC sections 4 to 6, 10 and 11.5, and the kill flag of harness sub-SPEC section 12) | the shell's main thread |

### 14.3 Edits in `~/bun`

| file | function or member | edit | context |
|---|---|---|---|
| `src/runtime/cli/Arguments.rs`, `src/options_types/context.rs` | `RUNTIME_PARAMS_`, `Arguments::parse`, `RuntimeOptions` | the JITCache flags (section 11.2), parsed once per process with Bun's other runtime flags, and `Bun__JITCache__flags`, through which the C++ side reads them | the CLI's thread, before any VM |
| `src/jsc/bindings/JITCacheHost.h`, `.cpp` (new) | `Bun::JITCacheHost` | configuration from Bun's JITCache flags, `start` with the worker fallback, `delta` and the bench report's flush at exit, the maintenance command (section 11.2); in twins builds, the main thread's VM layout record (harness sub-SPEC section 4) and the body-event dump at exit (harness sub-SPEC section 10.3) | as its callers below |
| `src/jsc/bindings/ZigGlobalObject.cpp` | `Zig__GlobalObject__create` | `Bun::JITCacheHost::configureVM(vm, executionContextId)` right after `JSC::JSLockHolder locker(vm)` | the new VM's thread, API lock and heap access held, before any global object |
| `src/jsc/bindings/ZigGlobalObject.cpp` | `JSCInitialize` | twins builds: address-space placement around `JSC::initialize` (harness sub-SPEC section 4) and the image test hook from `--jitcache-test-image-hook` (section 11.2), through `<JavaScriptCore/JITCacheTwinsHost.h>` (section 3.5) | once per process, inside its `std::call_once`, before any VM |
| `src/jsc/VirtualMachine.rs` | `VirtualMachine::on_exit` | `Bun__JITCache__atExit(self.global())` right after `ExitHandler::dispatch_on_exit(self)` | the VM's JS thread, after the exit handlers and before teardown |
| `src/runtime/cli/mod.rs`, `src/runtime/cli/jitcache_command.rs` (new) | the root command matcher, `Tag`, the dispatch, the help text | `bun jitcache` (maintenance sub-SPEC section 5) | the CLI's thread; no VM |
| `src/jsc/modules/BunJSCModule.h` | `DEFINE_NATIVE_MODULE(BunJSC)` | twins builds: five more exports (harness sub-SPEC section 5.3) | the VM thread |
| `scripts/build/deps/webkit.ts`, `scripts/build/profiles.ts`, `scripts/build/config.ts` | the local WebKit recipe, the profiles and the resolved config | the build-id link flag and the twins profile; Bun's compile and link flags stay as they are (section 11.2, harness sub-SPEC section 1) | build time |
| `test/js/bun/jitcache/` (new) | Bun tests | section 15.3 | |

In this repository's root, `build.ts` gains the `twins` and `bun-twins` targets (harness sub-SPEC section 1) and the `pin` target, which builds the pin from a worktree of this repository (harness sub-SPEC section 11.1). The build table of `CLAUDE.md` gains these three targets and the `JITCACHE_PIN_SOURCE` override; the human edits that file.

### 14.4 Hot-file edits

- M1 (task 1). `Source/JavaScriptCore/Sources.txt` gains, under a `jitcache/` block, the files of SPEC-ucb.md M1, SPEC-image.md M1, SPEC-cb.md M2 and SPEC-ics.md M1, and the integrator's `jitcache/JITCacheAPI.cpp`, `JITCacheMaintenance.cpp`, `JITCacheVMState.cpp`, `JITCacheFaults.cpp`, `ProducerBudget.cpp`, `ValidatedBody.cpp`, `JITCacheOptions.cpp`, `JITCachePlatform.cpp`, `JITCacheContainer.cpp`, `ArtifactStore.cpp`, `ArtifactWriter.cpp`, `JITCacheInstall.cpp`, `JITCacheCapture.cpp`, `JITCacheBench.cpp`, `TwinReport.cpp`, `JITCacheBodyEvents.cpp` and `JITCacheTwinsHarness.cpp`. The twins-only files compile to nothing without `ENABLE(JITCACHE_TWINS)`. The tests under `jitcache/tests/` are not in `Sources.txt`. Task 1 creates each listed `.cpp` file that does not exist yet as a stub holding the license header and `#include "config.h"` only, and the owning part's task replaces it.
- M2 (task 1). `Source/JavaScriptCore/CMakeLists.txt`: `"${JAVASCRIPTCORE_DIR}/jitcache"` once in `JavaScriptCore_PRIVATE_INCLUDE_DIRECTORIES` (UCB M2, Image M2, CB M3), and `jitcache/JITCacheAPI.h`, `jitcache/JITCacheMaintenance.h` and `jitcache/JITCacheTwinsHost.h` in `JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS`. No lane exports a header (SPEC-ucb.md M2, SPEC-image.md section 14.4, SPEC-ics.md M1). These three are the only headers the build reads before their owners' code lands, so none is stubbed: task 1 runs after the integrator's task 0, which writes them.
- M3 (task 1). `runtime/VM.h` (UCB M3 and R-INT-1): `namespace JITCache { class VMState; }` beside the other forward declarations, the public inline `JITCache::VMState* jitCacheState() const { return m_jitCacheState.load(std::memory_order_acquire); }` and `void setJITCacheState(JITCache::VMState* state) { m_jitCacheState.store(state, std::memory_order_release); }`, and the private member `std::atomic<JITCache::VMState*> m_jitCacheState { nullptr };`, declared as `VM`'s last data member, so that no field offset emitted code bakes moves (compiler.md, "What the code embeds"; harness sub-SPEC section 11). ([history](SPEC-integrator-history.md#the-pin-comparison-names-addresses-itself))
- M4 (task 7). `runtime/VM.cpp`: under `ENABLE(JIT)` and with `#include "JITCacheGlue.h"`, `JITCache::willDestroyVM(*this);` right after the `cancelAllPlansForVM` block of `VM::~VM`, and `JITCache::didFinalizeHeap(*this);` right after `heap.lastChanceToFinalize();` (section 4.6). Task 7 defines both functions in `JITCacheAPI.cpp` with `start` and the state's destructor (section 4.1), so the engine links at every step, and no VM before task 7 has a state to tear down.
- M5 (task 1). `bytecode/CodeBlock.h`: SPEC-cb.md M1, verbatim (`CodeBlock::seedBaselineTierUpHistory`, and `previousCounterForAging` under `ENABLE(JITCACHE_TWINS)`), both defined inline.
- M6 (task 1). `parser/SourceProvider.h`: SPEC-ucb.md M6, verbatim (`SourceProvider::jitCacheSourceDigest()`, whose inline default returns `std::nullopt`).
- M7 (task 12). `tools/JSDollarVM.cpp`: SPEC-ucb.md M4 (`$vm.jitCacheUCBSelfTest()`, `$vm.jitCacheUCBStatistics()`), under `ENABLE(JITCACHE_TWINS)`. They call `JITCache::runUCBSelfTest`, `JITCache::verifyRegistry` and `UCBRegistry`, so task 12 runs after the UCB lane's tasks 6 and 10, which define them.
- M8 (task 1). `Source/cmake/WebKitFeatures.cmake`: `WEBKIT_OPTION_DEFINE(ENABLE_JITCACHE_TWINS "Build JITCache's twin checks and test hooks" PRIVATE OFF)` among the JavaScriptCore options, which puts `ENABLE_JITCACHE_TWINS` into `cmakeconfig.h` for JSC and for Bun's `root.h`.
- M9 (task 1). `Source/JavaScriptCore/shell/CMakeLists.txt`: the two blocks of the `testjitcache` executable that the harness sub-SPEC's section 8.3 writes, beside `testFFI`'s. Task 1 writes `tests/testjitcache.cpp` and stubs every other file the target lists, as for M1, since CMake refuses a missing source when it generates the build.
- M10 (task 10). `bytecode/UnlinkedCodeBlock.h` and `.cpp`, which the UCB lane edits too (SPEC-ucb.md section 15.1): in twins builds, the struct `JSC::JITCache::BodyEventCounts` and `UnlinkedCodeBlock`'s last data member `m_jitCacheEventCounts` with its accessor `jitCacheEventCounts()` (harness sub-SPEC section 10.1), and in `UnlinkedCodeBlock::~UnlinkedCodeBlock` the call `JITCache::retireBodyEventCounts(*this)` right after the UCB lane's `unlinkedCodeBlockWillBeDestroyed` (SPEC-ucb.md section 6.3; harness sub-SPEC section 10.3). The two calls are independent: retiring reads no registry entry. Task 10 applies it after the UCB lane's task that adds that hook.

### 14.5 The lanes' other manifest entries

These name no hot file; each is met where the table says.

| entries | met by | task |
|---|---|---|
| option rows: UCB M5, Image M4, CB R-INT-9, ICs M3 | `JITCacheOptions.cpp` (section 5.1), from the rows of options.md; `OptionsList.h` does not change | 3 |
| section type ids: UCB R-INT-4, Image M3, ICs M2 | section 6.1 | 4 |
| `didFailExecutableAllocation`, `producerContext`, `ProducerBudget`: Image M5, ICs M4 | sections 4.4 and 4.5 | 2 |
| `ValidatedBody` and its test factory, the lookup override: UCB R-INT-3, SPEC-ucb.md U4 and U8 | section 6.2 | 2 (`ValidatedBody`), 5 (the lookups and their override) |
| install glue: Image M6, ICs M5, UCB R-INT-6, CB R-INT-3 and R-INT-4 | section 7 | 8 |
| capture glue: Image M6, ICs M5, UCB R-INT-5, CB R-INT-5 | section 8 | 9 |
| test-build harness: Image M7, ICs M6, CB R-INT-6 and R-INT-10, UCB R-INT-10 | the harness sub-SPEC | 1 (the twins build, `testjitcache`), 8 (the image check's stash), 12 (shell helpers), 13 (the runner) |

SPEC-image.md M8 is conditional and needs no action: the UCB lane edits only `ArithProfile.h` (`restoreBits`) and the Image lane only the two `ArithProfile::emitUnconditionalSet` definitions in `ArithProfile.cpp`, so each lane makes its own edit.

## 15. Tests

The integrator restores no piece of its own, so its tests check its contracts and the oracle of THREAD Verification, which the runner applies to every run that configures JITCache (harness sub-SPEC section 7.6) beside the fault and install checks of harness sub-SPEC section 7.5; the lanes' twins run through the integrator's harness. Every C++ test runs in `testjitcache` and every JS test through the runner (harness sub-SPEC sections 7 and 8), in `debug-local` builds with ASan and LSan and with `--destroy-vm`, and in the twins build, on x86_64 and ARM64; a script that requires twins (harness sub-SPEC section 7.4), as every script in `integrator/` does, runs in the twins build only. The runner passes `--jitcache-strict=1` to every run (harness sub-SPEC section 7.3), and a C++ test that configures a VM sets `Config::strict`. A test that also checks the default adds runs with strict off and says so. The container's, the maintenance's and the harness's own tests are in their sub-SPECs.

### 15.1 C++ (`tests/IntegratorTests.cpp`)

- T-OPT. In the default option group: every fixed row's required value equals the option's effective value, and the must-match reads return the effective values.
- T-CPU. The CPU feature vector equals the predicates of N11, bit by bit, and its unused bits are zero.
- T-BUILDID. The test executable has a build ID; a crafted `PT_NOTE` segment yields the descriptor; an object without the note yields none.
- T-BUDGET. Eight threads charge and release concurrently until the limit refuses: the refusal is sticky, the total never exceeds the limit, an overflowing charge is refused, the peak is the maximum total, and the balance returns to zero. `createUnlimited` never refuses.
- T-BODY. `ValidatedBody::createForTesting` with sections of sizes 0, 1, 7, 8 and 4097 returns each at an 8-byte-aligned address with its bytes, an empty span for an absent kind, the key and version given and the highest tier of the kinds; its `onDestroy` runs exactly once, from the destructor, on the thread that drops the last reference.
- T-FAULTS. Each fault entry point moves the switches of section 4.2 as specified; `producerContext` is null after each; `didFailExecutableAllocation` called with a CB's `m_lock` held through a `GCSafeConcurrentJSLocker` returns without blocking; a `JSC::Debugger` attached to a global object turns activity off and `status` names `debugger.attach`; a VM without state ignores every entry point.
- T-START. On temporary directories, for each role: `Created`, `Opened`, and `Busy` for a second producing VM of the test process on the same path while the first VM's state lives, then `Opened` for a ConsumerProducer once that state is destroyed, which releases the lock. `Rejected` at each step of section 3.2 that a test can reach (`start.already-configured`, `start.config`, `start.pc-maps` after a `JSC::Debugger` attached to the VM's global object, `start.build-id` through `removeMainBuildIDForTesting` (section 5.2), which removes the main executable's computed ID, `start.artifact-exists`, `start.not-an-artifact`, `start.artifact-missing`, `start.incompatible`), and `Fault` at `start.header` and `start.io`; the creation cases of container test C8. A faulted VM reports `Faulted` and rejects a second `start`. Rejected and busy calls leave the VM unconfigured, and a second `start` on it succeeds. Two VMs of the process that open one artifact hold one `OpenedArtifact`. A `Config` left at its defaults gives `status().strict` false.
- T-LOOKUP. In a Consumer over a temporary artifact holding bodies the writer committed, in this order: `bodyVersion` returns a nonzero token for a committed body's key and 0 for a key no body has, and reaches no system call (the store's fault hook, set to fail every call, never fires for it); `openBody` returns `Found` with a body whose `version()` is its file's commit identifier, and `Missing` for the key without a body; the hook with `EMFILE` on `openBody` gives `Missing` and counts `transientOpenFailures`; with an override set, both calls answer from it, a body from `createForTesting` included, whose commit identifier differs from the token the override gives; clearing the override restores the store's answers; last, the hook with `EIO` on `openBody` raises invalid material at `container.io`, which turns activity off, and both calls then return 0 and `Unusable`, whether or not an override is set.
- T-SCORE. `beats` over a table of scores that differ in one field at a time, and ties, including a candidate whose counter is withheld against a saved score with equal richness and IC sites and more counter progress, which the candidate beats, and the same pair reversed, which the candidate loses. With strict on, `scoreSections` on a malformed feedback, CB summary or ICs span names the reader that rejected it, with the part and check section 8.2 raises.
- T-STAMP. In a Producer with strict on, two functions are brought to baseline as SPEC-ics.md T14 brings them, one whose `in` site has seen objects of two shapes, so its property-IC record lists two cases, and one whose every site has seen one shape; once their counters have made progress, `delta` commits both. The first body's envelope holds P 0 and its `cb.state` and `cb.summary` headers `NotCarried`; the second's envelope holds the `counterProgress` of its `cb.summary` and both headers `Carried`. The first body's kept score has `counterWithheld` set and the second's has it clear. For each body, `keptScoreForTesting` (section 8.3) equals `scoreSections` of the sections `openBody` returns for it (II22, II23).
- T-DELTA. `delta` rejects with each step of section 3.4 that a test can reach: `delta.unconfigured` with no state, `delta.role` for a Consumer and `delta.locks` without heap access. In a Producer it commits a body, and it reports `Faulted` after a recording fault.
- T-CHARGE. In a Producer that has already committed one key, so that the writer's staging buffer and the kept-summary table exist, a commit of a second key the index lacks raises `status().budget.chargedBytes` by exactly the per-entry charges of section 4.4, six of the kept-summary table's bucket sizes and six of the index's, and a later commit of the same key by neither. A recording fault then releases the kept summaries', the index entries' and the staging buffer's charges at the next glue entry (section 4.2), while both keys stay in the index.

### 15.2 JS (`JSTests/jitcache/integrator/`)

| test | checks |
|---|---|
| `finalize-capture.js` | a body compiled in the producer is committed at its finalize capture; the consumer installs it (`jitcacheProgress().installs`), its `jitcacheBodyEvents` (harness sub-SPEC section 10.2) shows no LLInt instruction and no baseline compile where the JITCache-off run shows both, and the output equals the JITCache-off run |
| `delta-scoring.js` | a `delta` after more warm-up commits a richer body (the result's `committedBodies` counts it); a `delta` with nothing new commits nothing; the consumer imports the last commit |
| `consumer-producer.js` | `Producer; ConsumerProducer; Consumer`: the ConsumerProducer imports, learns and commits a richer body that the last consumer imports; in a second sequence, a ConsumerProducer whose production a limit of one page ends at `budget.limit` (`jitcache-expect-fault: 1:1 budget.limit`) keeps importing |
| `install-outcomes.js` | a baked-fact mismatch keeps the import for the next newborn CB of the UCB, which installs it; a consumer whose `--jitAllowlist` excludes a body drops the import at the gate; counters and outputs as expected |
| `llint-off.js` | a consumer with `--useLLInt=false` installs at `JIT::compileSync`, and the output equals the JITCache-off run |
| `exec-alloc-faults.js` | `--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=<n>` over a range of n, each n in a `Producer --jitcache-delta-at-exit` run followed by a `Consumer` run with `jitcache-expect-no-install`. The Producer runs declare `jitcache-require-fault: 0` for each of `exec-alloc.baseline-plan`, `exec-alloc.dfg-plan`, `exec-alloc.ftl-plan`, `exec-alloc.ic-handler` and `exec-alloc.mathic-snippet`, so each step must appear somewhere in the range. For each n, `jitcacheStatus()` names one of them or is `null`, since the fuzzer also fails allocations that keep native behavior, such as Yarr's (`ExecutableAllocator::allocate` counts every `JITCompilationCanFail` request; THREAD Failures). The range fits the runner's 50-line window as long as the script reaches all five sites within it. The function whose MathIC regenerates stays in baseline through the `delta`, so the image twin, which replays the regeneration, catches a captured snippet failure (SPEC-image.md section 11.3). After each fuzzed Producer run, the Consumer calls each of the script's functions, takes its key with `jitcacheBodyKey` and reads the committed body's sections, when there is one, with `jitcacheReadSection` (harness sub-SPEC section 5.2): no `ucb.feedback` holds a deferred LLInt counter, an `m_activeThreshold` of `INT32_MAX` (SPEC-ucb.md section 5.2), which no native capture holds (SPEC-ucb.md section 5.5); and no `cb.state` carries a deferred baseline counter, a `Carried` counter whose `counterActiveThreshold` is `INT32_MAX` (SPEC-cb.md section 3.2 and R-INT-12). The test's bodies stay below the reoptimization count at which `adjustedCounterValue` clips and have no polymorphic site, so every `cb.state` carries its counter (SPEC-cb.md I16). Every output equals the JITCache-off run. ([history](SPEC-integrator-history.md#what-the-executable-allocation-fault-tests-can-see)) |
| `exec-alloc-ic-marker.js` | the IC-handler site's marker. Like `exec-alloc-faults.js`, with `jitcache-require-fault: 0 exec-alloc.ic-handler`, but every IC site of the script can give up only through that fault: cell bases with at most seven plain, cacheable shapes, no proxies, dictionaries, BigInt arrays, prototype puts or `instanceof`, and a first case that compiles its own stub, as the global-proxy reads of SPEC-ics.md T9 do. Its functions still run baseline code at the `delta`. After each fuzzed Producer run, the Consumer reads each function's `ICsBaseline` section with `jitcacheReadSection`, and no record holds `tookSlowPath` or a site that gave up (SPEC-ics.md section 4), which would mean the handler's fault came after its effects were written |
| `producer-limit.js` | a `Producer` with a limit of one page, declared with `jitcache-expect-fault: 0 budget.limit`: the limit ends production at `budget.limit`, whichever charge, recording's or a lane's at capture, is refused first (section 4.5); `jitcacheDelta()` throws naming it; the output equals the JITCache-off run |
| `writer-faults.js` | twins builds, with one `jitcache-expect-fault: 0 <step>` for each of `writer.create`, `writer.write`, `writer.reread` and `writer.publish`: each writer fault hook, set in the Producer, makes `status` name its `writer.*` step, publishes nothing for that commit, and keeps the earlier commits importable |
| `scoring-deferral.js` | twins builds: `Producer --jitcache-delta-at-exit; ConsumerProducer --jitcache-test-store-fault=scoring:EMFILE; Consumer`. The ConsumerProducer warms the imported bodies further and calls `jitcacheDelta()`; every scoring read fails at its `openat`, so every key the ConsumerProducer scores is deferred, whether or not it has a body (the result's `deferredKeys` and `jitcacheProgress().capturesDeferred`), no fault is raised, nothing is committed, and the `cb.summary` bytes `jitcacheReadSection` returns for the imported keys are the same after the call as before it. The consumer imports the producer's bodies, and its output equals the JITCache-off run |
| `producer-kill.js` | twins builds: one sequence per kill point of harness sub-SPEC section 12, `Producer --jitcache-test-kill=<point>@<n>` with `jitcache-expect-exit: 0 kill` and n of at least 2, so the consumer has a body to install, then `Maintenance clean $ARTIFACT`, then `Consumer`. The consumer imports every body committed before the n-th commit. The n-th commit's key holds no body for the six points before the rename and the new body for `after-rename`, and another sequence, whose n-th commit recaptures a key, finds that key's earlier body unchanged after a kill before the rename. `clean` reports one temporary for the points from `after-create` to `after-reread` and none for the other two |
| `default-mode.js` | the corpus of `finalize-capture.js` in two sequences, one with strict on, as every test runs, and one with `--jitcache-strict=0` in every run, the default the bench measures: each consumer installs the same bodies (`jitcacheProgress().installs`), and every output equals the JITCache-off run |
| `must-match.js` | `Producer --evalMode=true; Consumer` with `jitcache-expect-no-install: 1`: the consumer's start is `Rejected` at `start.incompatible`, and its output equals the JITCache-off run |
| `fixed-option.js` | `Producer --useLOLJIT=true` is `Rejected` at `start.fixed-option`, naming `useLOLJIT`, and its output equals the JITCache-off run |

### 15.3 Bun (`~/bun/test/js/bun/jitcache/jitcache.test.ts`)

The tests spawn `bunExe()` with the flags of section 11.2, `--jitcache-strict` included, and compare each consumer's output with a JITCache-off run: a fixture app imports after a producer run; a worker whose main VM produces falls back to `Consumer`; with `--jitcache-bench-report`, the main VM writes its report at the configured path and each worker at that path suffixed with its id, and a macro run during `bun build` writes none; `delta` at exit commits bodies; `bun jitcache clean` and `compact` behave as the maintenance sub-SPEC says (exit codes, `--yes`, `--no`, and no TTY); a malformed flag value is a command-line error; a bare `--jitcache-strict` and `--jitcache-strict=1` turn strict on, `=0` turns it off, and the last occurrence wins.

## 16. Bench obligations and parameters

These serve the bench loop THREAD Execution ends with. The bench report (harness sub-SPEC section 9) carries every measurement below, except what IB10 reads from committed bodies and native logs and the event counts IB11 reads from body-event dumps.

- IB1. Opening a body: `openBody` per body and per KiB (mapping, integrity checks, checksums), with cold and warm page caches, apart from installation as THREAD's opening requires, and the release of an installed body's mapping, the `install` event's `bodyRelease` (section 7.2, step 18). If the release weighs for small bodies, as an unmap's TLB shootdown in a process with JIT workers and markers can, the first change to measure is reading a body with one `pread` into an owned buffer in place of the mapping.
- IB2. The index: build time and the index's bytes per body against the number of bodies, beside the number of bodies the run requested, since every process lists and keeps the whole artifact whatever it requests; an absent and a present `bodyVersion`, each a read of the index (SPEC-ucb.md B4); the epoch check and refresh cost in a consumer process beside a committing producer, the relistings an inotify overflow forces among them counted apart, since each runs at a request point under `m_indexLock`, and, for an object without inotify, the listings per second and the bodies a deferred listing left as misses against `fallbackListingIntervalMilliseconds`; and the writer's index update in a producing process whose other VMs share the index. If listing or index memory weighs, the first change to measure is compact entries: a 16-byte key fingerprint beside the `IndexEntry` in an open-addressing table, where the store's `openat` by full name confirms a hit and a false one costs an `ENOENT`. ([history](SPEC-integrator-history.md#the-index-holds-tokens))
- IB3. The import cost THREAD's opening bounds, set per body against the native cost THREAD's opening defines, for every body at or above `installationBoundSmallestBody`. The import side is everything JITCache does at the body's request point, which the UCB lane measures part by part, divides between the bound and what THREAD measures apart and records in the `request` event (SPEC-ucb.md section 14, B2), plus the install function's VM-thread CPU time, the `install` event's total, whose step breakdown explains it (section 7.2; harness sub-SPEC section 9.1); the bench joins the two events by key (harness sub-SPEC section 9.2). The native side is the `compile` event (harness sub-SPEC section 9.3). Both sides run with per-VM support already generated, as THREAD's bound requires: a body whose `install` or `compile` event reports `supportGenerated` is set aside, and the bench reports how many were (harness sub-SPEC section 9.3). Decoding, reading and faulting the artifact (IB1), and the key digest with the context digests and each TDZ environment's first digest (SPEC-ucb.md B1), are measured apart, as THREAD's opening lists them.
- IB4. Capture pause per body, at the finalize capture and in `delta`: scoring, each lane's build, the writer's stream, reread and publish.
- IB5. Writer throughput against `writerStagingBytes`.
- IB6. Producer memory: the budget's peak and its parts (the lanes' records and sections, kept summaries, the index entries the writer adds, the staging buffer, `delta`'s table) on the bench's workloads, from which `defaultProducerLimitBytes` is set. The registry and the identity digests, which no limit charges, are measured apart (SPEC-ucb.md B5).
- IB7. `delta` duration and memory against the number of CBs and keys, with the first scorings of the keys a ConsumerProducer imported counted apart: each maps the body and checksums the three summary sections the import already validated. If they weigh, the first change to measure is stamping the committed score in the envelope's reserved bytes, so that a first scoring reads the 128-byte envelope. ([history](SPEC-integrator-history.md#the-scoring-read-opens-by-name))
- IB8. Start: `start` time for each role against the number of bodies, and Bun's startup with JITCache on and off, with an empty and a full artifact.
- IB9. Maintenance: `compact` planning time and memory against the number of bodies.
- IB10. Generations. IB10 checks on the bench's workloads that consumer-induced exit sites no longer accumulate under THREAD Restoration's polymorphic rule, which the ICs lane extends to call sites the producer left polymorphic (SPEC-ics.md section 5.3), and sizes what the rule leaves: the floor's residue in bodies that carry progress, which THREAD Restoration gives the bench, and recaptures that drop the bit (SPEC-ics.md B6). It chains one Producer, `benchConsumerProducerGenerations` ConsumerProducer runs and one Consumer, each a fresh process that calls `delta` at exit as Bun does (section 11.2), and records per generation and per body: ([history](SPEC-integrator-history.md#exit-sites-across-consumerproducer-generations))
  - from the `install` and `capture` events (harness sub-SPEC section 9.2): whether the import carried counter progress, the committed score with the UCB lane's exit-site units apart, the field that decided each commit, and the `hasPolymorphicSite` each capture passed to the CB lane;
  - from the committed body files, through the lanes' readers: the exit sites each generation added (SPEC-ucb.md `FeedbackSection`), with those at a bytecode whose property-IC record lists two or more cases counted apart (`parseSection` and `isPolymorphicPropertyIC` over `ICsBaseline`, the record's bytecode index taken from its mold in `image.baseline`), and the CB lane's `counterMode` (`CBFormat::StateHeader`).

  From these it reports SPEC-ics.md B4's share of committed bodies that carry the bit, split between those with a polymorphic property-IC record and those whose bit only a call site set, and B6's bodies whose import carried no progress while their committed recapture carries it. It also compares the last Consumer's passes with the Producer's warmed pass W (HARNESS.md, Benches), in time and in each body's DFG compiles and `BadCache` and `BadConstantValue` exits, taken in runs of their own with the free options `logCompilationChanges`, which logs each DFG compile, and `printEachOSRExit`, which logs each exit with its kind and bytecode index.
- IB11. The skipped warm-up (HARNESS.md): the per-body event counts of harness sub-SPEC section 10 on the bench's workloads, from body-event dumps of runs in the twins build, the only build that counts: each run's totals for the passes HARNESS.md's Benches section names N1, W and C1, and, by key, each body's counts in W beside C1.

`JITCacheParameters.h` names the parameters; the bench loop proposes each value from its first measurement, and a value enters the parameter table of HARNESS.md, which also holds the lanes' parameters, only after human approval:

| parameter | meaning | value |
|---|---|---|
| `defaultProducerLimitBytes` | the limit a producing role uses when the host passes none | `std::optional<size_t>`, empty until the bench sets it; until then a producing role must pass a limit (section 3.2, step 3) |
| `installationBoundFraction` | THREAD's fixed fraction of the native cost an import may take | set by the bench; read only by the bench, which raises an alert and fails nothing |
| `installationBoundSmallestBody` | THREAD's smallest body the bound covers | set by the bench; read only by the bench, which raises an alert and fails nothing |
| `writerStagingBytes` | the writer's staging buffer | 64 KiB until IB5 tunes it |
| `fallbackListingIntervalMilliseconds` | the least time between the starts of two listings that a refresh without inotify, or with a listing pending, makes (container sub-SPEC section 6.3) | 1000 until IB2 tunes it |
| `benchConsumerProducerGenerations` | the number of ConsumerProducer runs IB10 chains between its Producer and its Consumer | set by the bench; read only by the bench |

## 17. Tasks

Ordered; each fits one implementation agent. A task that needs a lane piece builds against the headers in which that lane declares it and lands when the piece does. Every manifest entry of section 14.4 belongs to exactly one task, named below and in the entry. A task calls only functions that it or an earlier task defines, and its tests need only those, so the tree builds and every listed test runs after every task. The state's lifecycle lands with `start` in task 7, the first task by which every part's type exists: destroying a state runs every part's destructor, `ValidatedBody`'s included, which the registry's pending imports hold, so the constructor, the destructor and both teardown hooks are defined in `JITCacheAPI.cpp` beside `start` (section 4.1), and the members `JITCacheVMState.cpp` defines in earlier tasks neither create nor destroy a state. ([history](SPEC-integrator-history.md#the-state-is-created-and-destroyed-in-task-7s-file))

Other parts' code calls the integrator too. Task 2 defines every integrator function another part's code calls (the fault entry points, the budget, `producerContext`, the `VMState` members of the lanes' interface, `ValidatedBody` with its test factory, the twin report and the bench report) except `VMState::bodyVersion`, `VMState::openBody` and the lookup override, which task 5 adds. A lane task that calls those three lands after task 5, and any other after task 2.

The waits run one way. A lane task that an integrator task waits on depends on no integrator task from that one on. The lanes' twin checks, which task 8 calls in twins builds (the CB lane's `verifyTwins` in its task 6, the ICs lane's `checkRestoredBaselineICs` and `functionSnapshotBaselineICs` in its task 6, the Image lane's `Twins::checkImage` in its task 11), need only tasks 1 and 2: the test target, the twin report and the budget. A lane's tests that need the glue, the jsc host or the runner sit in a later task of that lane (CB task 7, ICs task 7, Image task 12), and the UCB lane's self-tests and JS tests run once tasks 12 and 13 have landed (SPEC-ucb.md section 15.3). ([history](SPEC-integrator-history.md#the-lanes-twin-checks-land-before-the-glue-that-calls-them))

0. Headers: `JITCacheAPI.h`, `JITCacheMaintenance.h`, `JITCacheVMState.h`, `JITCacheFaults.h`, `ProducerBudget.h`, `ValidatedBody.h`, `JITCacheGlue.h`, `JITCacheContainer.h` (the container sub-SPEC's section 4.5), `JITCachePlatform.h` (section 5.2), `ArtifactStore.h` (the declarations of the container sub-SPEC's sections 2, 5.1, 5.2, 6.1 and 7), `ArtifactWriter.h` (section 6.3), `JITCacheCapture.h` (sections 8.2 and 8.3), `JITCacheParameters.h`, `JITCacheBench.h`, `TwinReport.h`, `JITCacheTwinsHost.h` and `tests/JITCacheTest.h`, exactly as this file and the sub-SPECs declare them; `JITCacheVMState.h` ends where section 4.1's declaration does, and task 7 completes it. Every part's tests and glue code compile against these.
1. The build, after this list's task 0, whose headers M2 exports: manifest entries M1, M2, M3, M5, M6, M8 and M9 with their stubs, `tests/testjitcache.cpp` (harness sub-SPEC section 8), the twins profile in `~/bun`'s build scripts and the `twins` and `bun-twins` targets of `build.ts` (harness sub-SPEC section 1), and the build-id flag of section 11.2. Then a `debug` build and a `twins` build must succeed, and `testjitcache --list` must run, listing no test yet, so every part can build and test from here on.
2. `ProducerBudget.cpp`, `JITCacheFaults.cpp`, `ValidatedBody.cpp` with `createForTesting`, `TwinReport.cpp`, the `BenchReport` of `JITCacheBench.cpp` (open, `record`, the flush with its `budget` line, `benchReport(VM&)`; harness sub-SPEC section 9), the members of `JITCacheVMState.cpp` that neither create nor destroy a state nor read the store (the switches, the fault records and the members that raise faults, `producerContext`), `Debugger::attach`'s call, and `BaselineJITPlan`'s `m_jitCacheRecordsImage` with `jitCacheRecordsImage()` (section 4.4), false until task 7 computes it, so the Image lane's task 4 compiles against it; T-BUDGET and T-BODY. No state exists before task 7 creates the first. Every later task records its bench events through this `BenchReport`.
3. `JITCachePlatform.cpp` and `JITCacheOptions.cpp`: CRC32C over `crc32c`, whose `static` this task removes (section 14.2), build IDs, the CPU vector, the option table; T-OPT, T-CPU, T-BUILDID.
4. `JITCacheContainer.cpp`: the header and body formats and their validation (container sub-SPEC sections 3 and 4) and tests C1 to C3.
5. `ArtifactStore.cpp`: `ProducerLock`, the registry, the opened artifact with its index, listings and refresh, its token read, its open and its scoring read, and the twins-build fault and registry hooks (container sub-SPEC sections 2 and 5 to 7); in `JITCacheVMState.cpp`, `VMState::bodyVersion`, `VMState::openBody` and the lookup override, with their `lookup` and `open` bench events; tests C4 to C6, which read the store's own results and need no configured VM. After tasks 2, 3 and 4.
6. `ArtifactWriter.cpp` with its twins-build hooks (container sub-SPEC section 8) and test C7, which builds its sections from crafted spans and streamed sources. After task 5.
7. `JITCacheAPI.cpp`: `start`, `status`, `flushBenchReport` and the `toJSON` functions, with the `start` bench event; the rest of `JITCacheVMState.h` (section 4.1: a private member for each part of its table and every accessor tasks 8, 9, 12, 14 and 16 use on them, so no later task edits the header); and the state's constructor and destructor and the teardown hooks of section 4.6, with manifest entry M4, which calls them from `VM::~VM`; the computation of `m_jitCacheRecordsImage` in `BaselineJITPlan`'s constructor (section 4.4), which reads the registry; T-START, which includes container test C8, T-FAULTS and T-LOOKUP, which need a configured VM. After tasks 2 to 6 and the UCB lane's task 6, which defines the registry the state holds and `status` reads.
8. The install glue (section 7): `JITCacheInstall.cpp` with the `install` bench event, the edits to `prepareForExecutionImpl`, `JIT::compileSync` and `shouldJIT`, and in twins builds the image twin-check state with its deleter, its stash and `didFinishPrepareForExecution` (harness sub-SPEC section 3). After task 7, each lane's install functions and, for twins builds, each lane's twin checks: the CB lane's task 6, the ICs lane's task 6 and the Image lane's task 11.
9. The capture glue (section 8): `JITCacheCapture.cpp` with `scoreSections`, the kept summaries, the index-entry charge, `commitSectionsFor`, `delta`, `rewriteSectionForTesting`, `keptScoreForTesting`, the assertion that captures never nest, and the `capture` and `delta` bench events, and the finalize hook in `BaselineJITPlan::finalize`, called with a null timing, without the `compile` event of section 8.5's step 1, which task 16 adds with the timing, and with the twins builds' baseline compile count (harness sub-SPEC section 10.1); T-SCORE, T-DELTA, T-CHARGE and T-STAMP. After tasks 6, 7 and 10 and each lane's capture functions, so the three tasks that edit `BaselineJITPlan::finalize` land in the order 10, 9, 16.
10. The plan-site faults (section 9): `BaselineJITPlan`, `DFG::Plan`, `SpeculativeJIT`. The engine side of the twins builds' event counts (harness sub-SPEC section 10): `JITCacheBodyEvents.cpp`, manifest entry M10, the counts in the LLInt, `DFG::Plan::finalize`, `handleExitCounts` and `CodeBlock::jettison`, and the erasure of a VM's retired totals in `didFinalizeHeap`. After tasks 2 and 7 and the UCB lane's task that adds its destructor hook (SPEC-ucb.md section 6.3).
11. Maintenance (maintenance sub-SPEC) with its command line and tests M1 to M6. After tasks 5 and 6.
12. The jsc host: the flags and calls of section 11.1; in twins builds, `JITCacheTwinsHarness.cpp` (the address-space placement, the shell helpers and the twins host functions of harness sub-SPEC sections 4 and 5, the reachable-heap description of its section 6, `jitcacheBodyEvents` and the body-event dump of its section 10), the shell flags that set the store's and the writer's hooks, the writer's kill points among them, the image test hook and H1's test entry, and manifest entry M7, `$vm`'s two functions. After tasks 8, 9 and 11, the UCB lane's tasks 6 and 10, and the ICs lane's task 6, which defines the `functionSnapshotBaselineICs` that `jitcacheICsSnapshot` registers.
13. The runner, `Tools/Scripts/run-jitcache-tests` (harness sub-SPEC section 7), with the JITCache-off oracle (its section 7.6) and its self-tests H1 to H6 and H8. After task 12.
14. The Bun host (section 11.2): `JITCacheHost`, `Zig__GlobalObject__create`'s call, `on_exit`'s call, `JSCInitialize`'s placement and image test hook, `configureVM`'s `recordVMLayout` call, the twins builds' body-event dump at exit, `bun:jsc`'s twins exports and `bun jitcache`. After tasks 7, 9, 11 and 12, which defines the twins host functions and the placement functions it calls. Then the `bun-debug` and `bun-twins` targets of `build.ts` must build: they compile Bun's side against the copied headers alone, so each JSC declaration it uses must come from a header of section 3.5.
15. The JS tests of section 15.2 and the Bun tests of section 15.3. After tasks 13 and 14.
16. The native-cost hooks of harness sub-SPEC section 9.3 (`BaselineJITPlan`'s three members, the constructor `BaselineJITPlan::BaselineJITPlan(CodeBlock*)` that sets `m_jitCacheMeasure`, and the clock reads, the `RelinkTimer` in `ScriptExecutable::installCode`, the support-generation count of `JITThunks` and `generateSlowPathHandler` with its reads in the `install` and `compile` events), the `BaselineCompileTiming` that `finalize` passes to the finalize hook and the `compile` event it feeds, and the measurements of section 16. After tasks 9 and 13.
17. The pin comparison (harness sub-SPEC section 11): `Tools/Scripts/jitcache-pin-compare.ts`, the `pin` target of `build.ts`, the runner's `--pin` option and `jitcache-pin` directive, the forced blinding of its section 11.5 with its edit to `MacroAssembler::shouldConsiderBlinding`, and self-test H7. It lands after the JITCache-off oracle (task 13) and the twins (task 8, with the lanes' twin checks it waits on).
