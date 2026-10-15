#include "config.h"
#include "JITCacheAPI.h"

#include "ArtifactAccess.h"
#include "ArtifactStore.h"
#include "JITCacheBench.h"
#include "JITCacheBodyEvents.h"
#include "JITCacheFiles.h"
#include "JITCacheGlue.h"
#include "JITCacheOptions.h"
#include "JITCacheParameters.h"
#include "JITCachePlatform.h"
#include "JITCacheVMState.h"
#include "ProducerBudget.h"
#include "TwinReport.h"
#include "VM.h"
#include <expected>
#include <memory>
#include <optional>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(LINUX)
#include <errno.h>
#endif

// The public interface JITCacheAPI.h declares, apart from delta, which JITCacheCapture.cpp defines, and the per-VM state's
// lifecycle: start checks the call and the process, reaches the artifact through ArtifactAccess.h and creates the one
// VMState a configured VM has, and the teardown hooks VM::~VM calls end its production and destroy it.
// JITCacheVMState.cpp defines the state's members, its constructor and destructor included.

namespace JSC::JITCache {

namespace JITCacheAPIInternal {

static ASCIILiteral sessionStateName(SessionState state)
{
    switch (state) {
    case SessionState::Unconfigured:
        return "unconfigured"_s;
    case SessionState::Started:
        return "started"_s;
    case SessionState::Faulted:
        return "faulted"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral productionStateName(ProductionState state)
{
    switch (state) {
    case ProductionState::NotProducing:
        return "not-producing"_s;
    case ProductionState::Active:
        return "active"_s;
    case ProductionState::Ended:
        return "ended"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral deltaOutcomeName(DeltaOutcome outcome)
{
    switch (outcome) {
    case DeltaOutcome::Completed:
        return "completed"_s;
    case DeltaOutcome::Rejected:
        return "rejected"_s;
    case DeltaOutcome::Faulted:
        return "faulted"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// The JSON lines toJSON writes: every field of the struct by name, enums by their names, an absent optional as null and
// a fault as {"class","step","detail"}.

static void appendKey(StringBuilder& json, ASCIILiteral key)
{
    json.append(",\""_s, key, "\":"_s);
}

static void appendString(StringBuilder& json, ASCIILiteral key, const String& value)
{
    appendKey(json, key);
    json.appendQuotedJSONString(value);
}

static void appendBoolean(StringBuilder& json, ASCIILiteral key, bool value)
{
    appendKey(json, key);
    json.append(value ? "true"_s : "false"_s);
}

static void appendNumber(StringBuilder& json, ASCIILiteral key, uint64_t value)
{
    appendKey(json, key);
    json.append(value);
}

static void appendFault(StringBuilder& json, ASCIILiteral key, const std::optional<FaultReport>& fault)
{
    appendKey(json, key);
    if (!fault) {
        json.append("null"_s);
        return;
    }
    json.append("{\"class\":"_s);
    json.appendQuotedJSONString(String { name(fault->faultClass) });
    json.append(",\"step\":"_s);
    json.appendQuotedJSONString(fault->stepName());
    json.append(",\"detail\":"_s);
    json.appendQuotedJSONString(fault->detail);
    json.append('}');
}

static void appendProgress(StringBuilder& json, const Progress& progress)
{
    // The first field opens the object, so appendNumber's leading comma comes after the brace.
    json.append(",\"progress\":{\"indexedBodies\":"_s, progress.indexedBodies);
    appendNumber(json, "bodyOpens"_s, progress.bodyOpens);
    appendNumber(json, "transientOpenFailures"_s, progress.transientOpenFailures);
    appendNumber(json, "imports"_s, progress.imports);
    appendNumber(json, "seededDecodes"_s, progress.seededDecodes);
    appendNumber(json, "attaches"_s, progress.attaches);
    appendNumber(json, "gateDrops"_s, progress.gateDrops);
    appendNumber(json, "misses"_s, progress.misses);
    appendNumber(json, "installs"_s, progress.installs);
    appendNumber(json, "bakedFactMismatches"_s, progress.bakedFactMismatches);
    appendNumber(json, "captureCandidates"_s, progress.captureCandidates);
    appendNumber(json, "capturesDeferred"_s, progress.capturesDeferred);
    appendNumber(json, "capturesCommitted"_s, progress.capturesCommitted);
    appendNumber(json, "bytesCommitted"_s, progress.bytesCommitted);
    appendNumber(json, "deltaRuns"_s, progress.deltaRuns);
    json.append('}');
}

static void appendBudget(StringBuilder& json, const BudgetSnapshot& budget)
{
    json.append(",\"budget\":{\"limitBytes\":"_s, static_cast<uint64_t>(budget.limitBytes));
    appendNumber(json, "chargedBytes"_s, budget.chargedBytes);
    appendNumber(json, "peakBytes"_s, budget.peakBytes);
    appendBoolean(json, "refused"_s, budget.refused);
    json.append('}');
}

#if OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))

static bool isRole(Role role)
{
    return role == Role::Consumer || role == Role::Producer || role == Role::ConsumerProducer;
}

static bool isDeltaCapture(DeltaCapture capture)
{
    return capture == DeltaCapture::All || capture == DeltaCapture::Compiled;
}

// What each role asks of the artifact.
static ArtifactAccessMode accessModeOf(Role role)
{
    switch (role) {
    case Role::Consumer:
        return ArtifactAccessMode::Open;
    case Role::Producer:
        return ArtifactAccessMode::Create;
    case Role::ConsumerProducer:
        return ArtifactAccessMode::OpenOrReplace;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return ArtifactAccessMode::Open;
}

// The result start gives when the artifact cannot be reached: Rejected or Busy leave the VM unconfigured, and Fault
// configures it with activity off.
static StartResult startResultOf(ArtifactAccessFailure&& failure)
{
    using Kind = ArtifactAccessFailure::Kind;
    switch (failure.kind) {
    case Kind::Missing:
        return { StartOutcome::Rejected, "start.artifact-missing"_s, WTF::move(failure.detail) };
    case Kind::Exists:
        return { StartOutcome::Rejected, "start.artifact-exists"_s, WTF::move(failure.detail) };
    case Kind::NotAnArtifact:
        return { StartOutcome::Rejected, "start.not-an-artifact"_s, WTF::move(failure.detail) };
    case Kind::Incompatible:
        return { StartOutcome::Rejected, "start.incompatible"_s, WTF::move(failure.detail) };
    case Kind::Busy:
        return { StartOutcome::Busy, "start.busy"_s, WTF::move(failure.detail) };
    case Kind::IO:
        return { StartOutcome::Fault, "start.io"_s, WTF::move(failure.detail) };
    case Kind::CorruptHeader:
        return { StartOutcome::Fault, "start.header"_s, WTF::move(failure.detail) };
    case Kind::DamagedPack:
        return { StartOutcome::Fault, "start.pack"_s, WTF::move(failure.detail) };
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { StartOutcome::Fault, "start.io"_s, WTF::move(failure.detail) };
}

#endif // OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))

} // namespace JITCacheAPIInternal

String FaultReport::stepName() const
{
    if (part.isEmpty())
        return check;
    return makeString(part, '.', check);
}

ASCIILiteral name(StartOutcome outcome)
{
    switch (outcome) {
    case StartOutcome::Started:
        return "started"_s;
    case StartOutcome::Busy:
        return "busy"_s;
    case StartOutcome::Rejected:
        return "rejected"_s;
    case StartOutcome::Fault:
        return "fault"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

ASCIILiteral name(Role role)
{
    switch (role) {
    case Role::Consumer:
        return "consumer"_s;
    case Role::Producer:
        return "producer"_s;
    case Role::ConsumerProducer:
        return "consumer-producer"_s;
    }
    // A host can hand start any value; start rejects one outside the enumeration at start.config, and it has no name.
    return { };
}

ASCIILiteral name(DeltaCapture capture)
{
    switch (capture) {
    case DeltaCapture::All:
        return "all"_s;
    case DeltaCapture::Compiled:
        return "compiled"_s;
    }
    // As for a role: start rejects a value outside the enumeration at start.config.
    return { };
}

ASCIILiteral name(FaultClass faultClass)
{
    switch (faultClass) {
    case FaultClass::StartFault:
        return "start-fault"_s;
    case FaultClass::InvalidMaterial:
        return "invalid-material"_s;
    case FaultClass::ExecutableMemory:
        return "executable-memory"_s;
    case FaultClass::RecordingFault:
        return "recording-fault"_s;
    case FaultClass::DebuggerAttached:
        return "debugger-attached"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

std::optional<Role> parseRole(StringView text)
{
    for (Role role : { Role::Consumer, Role::Producer, Role::ConsumerProducer }) {
        if (text == name(role))
            return role;
    }
    return std::nullopt;
}

std::optional<DeltaCapture> parseDeltaCapture(StringView text)
{
    for (DeltaCapture capture : { DeltaCapture::All, DeltaCapture::Compiled }) {
        if (text == name(capture))
            return capture;
    }
    return std::nullopt;
}

StartResult start(VM& vm, const Config& config)
{
#if !(OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64)))
    // JITCache serves Linux on x86_64 and ARM64, and VM::~VM tears a state down only with the JIT enabled.
    UNUSED_PARAM(vm);
    UNUSED_PARAM(config);
    return { StartOutcome::Rejected, "start.platform"_s, "JITCache runs only on Linux, on x86_64 and ARM64, with the JIT enabled"_s };
#else
    using namespace JITCacheAPIInternal;

    if (!vm.currentThreadIsHoldingAPILock() || !vm.heap.hasHeapAccess())
        return { StartOutcome::Rejected, "start.locks"_s, "the calling thread does not hold the VM's API lock and heap access"_s };

    // A start that created or opened an artifact, or faulted, configured the VM for good.
    if (vm.jitCacheState())
        return { StartOutcome::Rejected, "start.already-configured"_s, "start already configured this VM"_s };

    // The bench report's start event measures the whole call, so a call that names a report reads the clock here. Every
    // outcome from the config's checks on records it while a report is open.
    uint64_t startNanoseconds = config.benchReportPath.isEmpty() ? 0 : benchThreadCPUNanoseconds();
    auto recordStartEvent = [&](BenchReport* report, const StartResult& result, uint64_t indexedBodies) {
        if (!report)
            return;
        using BenchValue = decltype(BenchField::value);
        BenchValue role = isRole(config.role) ? BenchValue { String { name(config.role) } } : BenchValue { nullptr };
        report->record("start"_s, {
            { "outcome"_s, String { name(result.outcome) } },
            { "role"_s, WTF::move(role) },
            { "nanoseconds"_s, benchThreadCPUNanoseconds() - startNanoseconds },
            { "indexedBodies"_s, indexedBodies },
        });
    };

    // The config's own checks. The detail names the field.
    if (config.artifactPath.isEmpty())
        return { StartOutcome::Rejected, "start.config"_s, "artifactPath is empty"_s };
    if (!isRole(config.role))
        return { StartOutcome::Rejected, "start.config"_s, makeString("role "_s, static_cast<unsigned>(config.role), " is none of Consumer, Producer and ConsumerProducer"_s) };
    if (!isDeltaCapture(config.deltaCapture))
        return { StartOutcome::Rejected, "start.config"_s, makeString("deltaCapture "_s, static_cast<unsigned>(config.deltaCapture), " is neither All nor Compiled"_s) };
    bool producing = config.role != Role::Consumer;
    std::optional<size_t> producerLimitBytes;
    if (producing) {
        producerLimitBytes = config.producerLimitBytes ? config.producerLimitBytes : defaultProducerLimitBytes;
        if (!producerLimitBytes)
            return { StartOutcome::Rejected, "start.config"_s, "producerLimitBytes is required for a producing role until defaultProducerLimitBytes is set"_s };
    }
    std::unique_ptr<BenchReport> benchReport;
    if (!config.benchReportPath.isEmpty()) {
        benchReport = BenchReport::open(config.benchReportPath);
        if (!benchReport) {
            int error = errno;
            return { StartOutcome::Rejected, "start.config"_s, makeString("benchReportPath "_s, config.benchReportPath, " does not open: "_s, errorText(error)) };
        }
    }

    // From here on, an outcome that does not configure the VM records the start event and closes the reports again,
    // which stay locals until the state takes them.
    auto rejectWithoutState = [&](StartResult&& result) -> StartResult {
        recordStartEvent(benchReport.get(), result, 0);
        return WTF::move(result);
    };

#if ENABLE(JITCACHE_TWINS)
    std::unique_ptr<TwinReport> twinReport;
    if (!config.twinReportPath.isEmpty()) {
        twinReport = TwinReport::open(config.twinReportPath);
        if (!twinReport) {
            int error = errno;
            return rejectWithoutState({ StartOutcome::Rejected, "start.config"_s, makeString("twinReportPath "_s, config.twinReportPath, " does not open: "_s, errorText(error)) });
        }
    }
#endif

    // Every fixed option must hold its required value.
    if (const FixedOptionRow* row = checkFixedOptions())
        return rejectWithoutState({ StartOutcome::Rejected, "start.fixed-option"_s, describeFixedOptionMismatch(*row) });

    // A debugger attached before start turned the PC-to-origin maps on, and images carry none.
    if (vm.shouldBuilderPCToCodeOriginMapping())
        return rejectWithoutState({ StartOutcome::Rejected, "start.pc-maps"_s, "the VM builds PC-to-origin maps, which an attached debugger or an option turned on"_s });

    // The artifact's header records the binaries' build IDs, so a binary without one cannot use the cache.
    ProcessFacts facts = processFacts();
    if (!facts.mainExecutable.size)
        return rejectWithoutState({ StartOutcome::Rejected, "start.build-id"_s, "the main executable has no GNU build ID"_s });
    if (facts.engineObject && !facts.engineObject->size)
        return rejectWithoutState({ StartOutcome::Rejected, "start.build-id"_s, "the engine object has no GNU build ID"_s });

    // Started, at start.replaced when a ConsumerProducer replaced an incompatible artifact, or a Fault, which configures
    // the VM with activity off. A Fault releases the producer lock the access took, since a faulted VM never produces.
    auto access = openArtifact(config.artifactPath, accessModeOf(config.role));
    StartResult result = !access ? startResultOf(WTF::move(access.error()))
        : access->replaced.isNull() ? StartResult { StartOutcome::Started, { }, { } }
        : StartResult { StartOutcome::Started, "start.replaced"_s, access->replaced };
    if (result.outcome != StartOutcome::Started && result.outcome != StartOutcome::Fault)
        return rejectWithoutState(WTF::move(result));

    VMState::StartParts parts;
    parts.outcome = result.outcome;
    parts.producerLimitBytes = producerLimitBytes;
    if (access) {
        parts.producerLock = WTF::move(access->producerLock);
        parts.artifact = WTF::move(access->artifact);
    } else
        parts.startFault = FaultReport { FaultClass::StartFault, { }, result.step, result.detail };
    parts.benchReport = WTF::move(benchReport);
#if ENABLE(JITCACHE_TWINS)
    parts.twinReport = WTF::move(twinReport);
#endif
    std::unique_ptr<VMState> state { new VMState(config, WTF::move(parts)) };
    recordStartEvent(state->benchReport(), result, state->artifact() ? state->artifact()->indexedBodies() : 0);
    vm.setJITCacheState(state.release());
    return result;
#endif // OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))
}

Status status(VM& vm)
{
    Status result;
    VMState* state = vm.jitCacheState();
    if (!state)
        return result;
    ASSERT(vm.currentThreadIsHoldingAPILock());

    // Reads only: no file opens, no fault is raised and no state changes.
    switch (state->startOutcome()) {
    case StartOutcome::Started:
        result.state = SessionState::Started;
        break;
    case StartOutcome::Fault:
        result.state = SessionState::Faulted;
        break;
    case StartOutcome::Busy:
    case StartOutcome::Rejected:
        // Neither outcome creates a state.
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    result.role = state->role();
    result.strict = state->strict();
    result.activityOn = state->activityOn();
    result.activityFault = state->activityFault();
    result.production = state->productionState();
    result.productionFault = state->productionFault();
    result.firstFault = state->firstFault();

    result.progress = state->statusProgress();

    if (ProducerBudget* budget = state->producerBudget())
        result.budget = BudgetSnapshot { budget->limitBytes(), budget->chargedBytes(), budget->peakBytes(), budget->hasRefused() };
    return result;
}

void flushBenchReport(VM& vm)
{
    if (BenchReport* report = benchReport(vm))
        report->flush();
}

void finishHostRun(VM& vm, bool deltaAtExit, const ScopedLambda<void(const std::optional<DeltaResult>&)>& hostWork)
{
    ASSERT(vm.currentThreadIsHoldingAPILock());
    // Production that is not active has nothing to capture: delta would only reject the VM or report the fault that
    // ended production, and what an ended production held goes in willDestroyVM.
    std::optional<DeltaResult> deltaResult;
    VMState* state = vm.jitCacheState();
    if (deltaAtExit && state && state->productionState() == ProductionState::Active)
        deltaResult = delta(vm);
    hostWork(deltaResult);
    flushBenchReport(vm);
}

String toJSON(const StartResult& result)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"start\""_s);
    appendString(json, "outcome"_s, name(result.outcome));
    appendString(json, "step"_s, result.step);
    appendString(json, "detail"_s, result.detail);
    json.append('}');
    return json.toString();
}

String toJSON(const DeltaResult& result)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"delta\""_s);
    appendString(json, "outcome"_s, deltaOutcomeName(result.outcome));
    appendString(json, "rejection"_s, result.rejection);
    appendFault(json, "fault"_s, result.fault);
    appendNumber(json, "eligibleKeys"_s, result.eligibleKeys);
    appendNumber(json, "committedBodies"_s, result.committedBodies);
    appendNumber(json, "committedBytes"_s, result.committedBytes);
    appendNumber(json, "deferredKeys"_s, result.deferredKeys);
    json.append('}');
    return json.toString();
}

String toJSON(const Status& status)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"status\""_s);
    appendString(json, "state"_s, sessionStateName(status.state));
    appendKey(json, "role"_s);
    if (status.role)
        json.appendQuotedJSONString(String { name(*status.role) });
    else
        json.append("null"_s);
    appendBoolean(json, "strict"_s, status.strict);
    appendBoolean(json, "activityOn"_s, status.activityOn);
    appendFault(json, "activityFault"_s, status.activityFault);
    appendString(json, "production"_s, productionStateName(status.production));
    appendFault(json, "productionFault"_s, status.productionFault);
    appendFault(json, "firstFault"_s, status.firstFault);
    appendProgress(json, status.progress);
    appendBudget(json, status.budget);
    json.append('}');
    return json.toString();
}

void willDestroyVM(VM& vm)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // GC is deferred for good and no compilation of the VM runs.
#if ENABLE(JITCACHE_TWINS)
    state->m_imageTwinCheckState.reset();
    state->m_twinReport = nullptr;
#endif
    if (state->m_benchReport)
        state->m_benchReport->flush();
    // Neither VM destruction nor process exit performs an implicit delta: production ends without a fault, and the memory
    // it held goes now.
    state->m_productionActive.store(false, std::memory_order_release);
    state->releaseEndedProductionMemory();
}

void didFinalizeHeap(VM& vm)
{
#if ENABLE(JITCACHE_TWINS)
    // The UCBs lastChanceToFinalize destroyed retired their counts into the VM's totals, which no dump reads any more.
    // They go for every VM, configured or not, so a later VM at this address starts from zero.
    forgetRetiredBodyEvents(vm);
#endif
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // Every UCB and UFE destructor has run, so the registry outlived every cell of the VM.
    // Destroying the state releases the producer lock and drops the opened artifact, which goes with the last VM of the
    // process that holds it.
    vm.setJITCacheState(nullptr);
    delete state;
}

} // namespace JSC::JITCache
