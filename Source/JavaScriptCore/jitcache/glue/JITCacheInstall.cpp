#include "config.h"

#include "BakedFacts.h"
#include "BaselineJITCode.h"
#include "CBState.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ICRestore.h"
#include "ICSection.h"
#include "ICTwins.h"
#include "ImagePrepare.h"
#include "ImageSection.h"
#include "ImageTwins.h"
#include "ImageTypes.h"
#include "JITCacheBench.h"
#include "JITCacheFaults.h"
#include "JITCacheGlue.h"
#include "JITCacheVMState.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "LLIntSlowPaths.h"
#include "ProducerBudget.h"
#include "ScriptExecutable.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <array>
#include <optional>
#include <utility>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

// The install glue: the install function the two install points call for a newborn CodeBlock whose UCB holds a pending
// import, and, in twins builds, the image twin check that prepareForExecutionImpl runs once the install returns.
// Everything runs on the VM thread with the API lock and heap access, under the install function's own GC deferral.

namespace JSC::JITCache {

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

// The image twin check an install leaves for the end of prepareForExecutionImpl, where the scope the twin needs exists.
// The body's reference keeps the bytes the view reads alive until the check returns.
struct PendingImageTwinCheck {
    CodeBlock* codeBlock;
    Ref<BaselineJITCode> code;
    Ref<ValidatedBody> body;
    ImageSectionsView view;
};

// The VM's image twin-check state, which the VMState holds with this file's deleter, so neither JITCacheVMState.h nor the
// state's destructor names an Image-lane type. The install creates it at the VM's first stash, and willDestroyVM
// destroys it, so the Twins object exists no later than the VM's first checkImage and dies during VM destruction.
struct ImageTwinCheckState {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ImageTwinCheckState);

    Twins twins;
    std::optional<PendingImageTwinCheck> pending; // at most one
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ImageTwinCheckState);

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#if ENABLE(JIT)

namespace JITCacheInstallInternal {

// The install's steps whose wall time the install event reports, each lane's call apart.
enum class InstallStep : uint8_t {
    ImageParse,
    ImageValidation,
    BakedFacts,
    Gate,
    PrepareImage,
    CBPrepare,
    PrepareBaselineICs,
    SeedLinkedState,
    SeedCallLinkHistory,
    Commit,
    Setup,
    AttachPropertyICState,
    FinishCounter,
    InstallCode,
};
constexpr unsigned numberOfInstallSteps = static_cast<unsigned>(InstallStep::InstallCode) + 1;

// The install event's measurements, taken only while a bench report is open.
// The total is the thread CPU time of the whole function, one pair of reads of CLOCK_THREAD_CPUTIME_ID, whose every read
// is a system call inside the span; the step breakdown only explains it, so it reads CLOCK_MONOTONIC, which the vDSO
// serves. Each step's time runs from the previous mark to the step's end. The support generations counted at the start
// tell, at the return, whether the install generated per-VM support, which sets the body aside from the installation
// bound, whose two sides must both run with support already generated.
class InstallMeasurement {
    WTF_MAKE_NONCOPYABLE(InstallMeasurement);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    InstallMeasurement(VM& vm, BenchReport* report)
        : m_vm(vm)
        , m_report(report)
        , m_supportGenerationsAtStart(report ? vm.jitStubs->supportGenerations() : 0)
        , m_startNanoseconds(report ? benchThreadCPUNanoseconds() : 0)
    {
    }

    BenchReport* report() const { return m_report; }
    bool supportGenerated() const { return m_vm.jitStubs->supportGenerations() != m_supportGenerationsAtStart; }

    // Marks the start of the next step, or skips work that is no step of the install, such as a twin check.
    void mark()
    {
        if (m_report)
            m_mark = MonotonicTime::now();
    }

    void endStep(InstallStep step)
    {
        if (!m_report)
            return;
        MonotonicTime now = MonotonicTime::now();
        m_stepNanoseconds[static_cast<unsigned>(step)] = (now - m_mark).nanosecondsAs<uint64_t>();
        m_mark = now;
    }

    uint64_t startNanoseconds() const { return m_startNanoseconds; }
    std::optional<uint64_t> stepNanoseconds(InstallStep step) const { return m_stepNanoseconds[static_cast<unsigned>(step)]; }

private:
    VM& m_vm;
    BenchReport* const m_report;
    const uint64_t m_supportGenerationsAtStart;
    const uint64_t m_startNanoseconds;
    MonotonicTime m_mark;
    std::array<std::optional<uint64_t>, numberOfInstallSteps> m_stepNanoseconds { }; // empty for a step that did not run
};

// What the install event reports beside the times. codeSize is the installed
// BaselineJITCode's JITCode::size(), its executable allocation, which the compile event of a native compilation reports
// the same way, although native code and the image can differ in size. imageCodeSize, the image's linked size, is what
// installationBoundSmallestBody reads.
struct InstalledBody {
    BodyKey key;
    CBCounterRestore restore;
    uint64_t codeSize;
    uint32_t imageCodeSize;
    uint32_t imageFixupCount;
    PrepareTimings prepareTimings; // the stages of prepareImage
};

static void recordInstallEvent(const InstallMeasurement& measurement, const InstalledBody& installed, uint64_t totalNanoseconds, uint64_t bodyReleaseNanoseconds, bool supportGenerated)
{
    using BenchValue = decltype(BenchField::value);
    auto step = [&](InstallStep installStep) {
        std::optional<uint64_t> nanoseconds = measurement.stepNanoseconds(installStep);
        return nanoseconds ? BenchValue { *nanoseconds } : BenchValue { nullptr };
    };
    measurement.report()->record("install"_s, {
        { "key"_s, bodyKeyHex(installed.key) },
        { "total"_s, totalNanoseconds },
        { "steps.imageParse"_s, step(InstallStep::ImageParse) },
        { "steps.imageValidation"_s, step(InstallStep::ImageValidation) },
        { "steps.bakedFacts"_s, step(InstallStep::BakedFacts) },
        { "steps.gate"_s, step(InstallStep::Gate) },
        { "steps.prepareImage"_s, step(InstallStep::PrepareImage) },
        { "prepareImage.allocate"_s, installed.prepareTimings.allocateNanoseconds },
        { "prepareImage.copy"_s, installed.prepareTimings.copyNanoseconds },
        { "prepareImage.patch"_s, installed.prepareTimings.patchNanoseconds },
        { "prepareImage.flush"_s, installed.prepareTimings.flushNanoseconds },
        { "steps.cbPrepare"_s, step(InstallStep::CBPrepare) },
        { "steps.prepareBaselineICs"_s, step(InstallStep::PrepareBaselineICs) },
        { "steps.seedLinkedState"_s, step(InstallStep::SeedLinkedState) },
        { "steps.seedCallLinkHistory"_s, step(InstallStep::SeedCallLinkHistory) },
        { "steps.commit"_s, step(InstallStep::Commit) },
        { "steps.setup"_s, step(InstallStep::Setup) },
        { "steps.attachPropertyICState"_s, step(InstallStep::AttachPropertyICState) },
        { "steps.finishCounter"_s, step(InstallStep::FinishCounter) },
        { "steps.installCode"_s, step(InstallStep::InstallCode) },
        { "bodyRelease"_s, bodyReleaseNanoseconds },
        { "restore.carried"_s, installed.restore.carried },
        { "restore.crossed"_s, installed.restore.crossed },
        { "restore.nativeSlice"_s, installed.restore.nativeSlice },
        { "restore.slice"_s, installed.restore.slice },
        { "codeSize"_s, installed.codeSize },
        { "image.codeSize"_s, static_cast<uint64_t>(installed.imageCodeSize) },
        { "image.fixupCount"_s, static_cast<uint64_t>(installed.imageFixupCount) },
        { "supportGenerated"_s, supportGenerated },
    });
}

// The detail of a fault an install raises: the body by its key.
static String importedBodyDetail(const BodyKey& key)
{
    return makeString("body "_s, bodyKeyHex(key));
}

// The enumerator names the ICs lane's checks are raised at, as fault reports name them.
static ASCIILiteral icsRestoreCheckName(ICs::Check check)
{
    switch (check) {
    case ICs::Check::SectionSize:
        return "SectionSize"_s;
    case ICs::Check::SummaryBound:
        return "SummaryBound"_s;
    case ICs::Check::CallLinkGroups:
        return "CallLinkGroups"_s;
    case ICs::Check::ReservedBits:
        return "ReservedBits"_s;
    case ICs::Check::EnumRange:
        return "EnumRange"_s;
    case ICs::Check::SummaryCount:
        return "SummaryCount"_s;
    case ICs::Check::MoldPairing:
        return "MoldPairing"_s;
    case ICs::Check::MetadataLayout:
        return "MetadataLayout"_s;
    case ICs::Check::MoldMegamorphicBit:
        return "MoldMegamorphicBit"_s;
    case ICs::Check::NewbornCallLinks:
        return "NewbornCallLinks"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

#if ENABLE(JITCACHE_TWINS)

// A twin report's detail names the body by its key in hex and the CB by CodeBlock::dump.
static String twinCheckSubject(const BodyKey& key, CodeBlock& codeBlock)
{
    return makeString("body "_s, bodyKeyHex(key), ", "_s, WTF::toString(codeBlock));
}

static ASCIILiteral icsTwinSiteName(ICs::TwinMismatch::Site site)
{
    switch (site) {
    case ICs::TwinMismatch::Site::PropertyIC:
        return "property IC"_s;
    case ICs::TwinMismatch::Site::CallLink:
        return "call link"_s;
    case ICs::TwinMismatch::Site::Capture:
        return "capture"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// The ICs lane's twin check, after it attaches its state: each mismatch it finds becomes a difference named by its
// field, with the site, the index, the expected and the actual value in the detail.
static void reportICsTwinMismatches(TwinReport& report, const BodyKey& key, CodeBlock& codeBlock, const Vector<ICs::TwinMismatch>& mismatches)
{
    if (mismatches.isEmpty())
        return;
    String subject = twinCheckSubject(key, codeBlock);
    for (const ICs::TwinMismatch& mismatch : mismatches)
        report.difference(TwinPart::ICs, mismatch.field, makeString(subject, ": "_s, icsTwinSiteName(mismatch.site), ' ', mismatch.index, ", expected "_s, mismatch.expected, ", actual "_s, mismatch.actual));
}

// The image twin check of a CB that JIT::compileSync installed outside prepareForExecutionImpl, which has no scope to give
// the twin. The CB is named by the body alone, since nothing kept it alive since its install.
static void reportImageTwinCheckWithoutScope(TwinReport& report, const PendingImageTwinCheck& pending)
{
    report.skip(TwinPart::Image, "no-scope"_s, makeString("body "_s, bodyKeyHex(pending.body->key()), ": installed by JIT::compileSync outside ScriptExecutable::prepareForExecutionImpl, so the twin has no scope"_s));
}

// Stashes the image twin check for the end of prepareForExecutionImpl, creating the VM's image
// twin-check state at its first stash. A check still pending belongs to a CB that JIT::compileSync installed outside
// prepareForExecutionImpl, whose scope no later call can give, so it is reported as that skip before it is replaced.
static void stashImageTwinCheck(VMState& state, PendingImageTwinCheck&& check)
{
    ImageTwinCheckState* twinCheckState = state.imageTwinCheckState();
    if (!twinCheckState) {
        VMState::ImageTwinCheckStateHolder::deleter_type deleter = [](ImageTwinCheckState* stateToDelete) {
            delete stateToDelete;
        };
        state.setImageTwinCheckState(VMState::ImageTwinCheckStateHolder { new ImageTwinCheckState, deleter });
        twinCheckState = state.imageTwinCheckState();
    }
    if (twinCheckState->pending) {
        if (TwinReport* report = state.twinReportSink())
            reportImageTwinCheckWithoutScope(*report, *twinCheckState->pending);
    }
    twinCheckState->pending.emplace(WTF::move(check));
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JITCacheInstallInternal

InstallOutcome installAtNewbornCodeBlock(VM& vm, CodeBlock& newborn, InstallPoint point)
{
    using namespace JITCacheInstallInternal;

    VMState* state = vm.jitCacheState();
    if (!state)
        return InstallOutcome::NotInstalled;
    // The total runs from here to the install's bookkeeping, before the body is dropped, while a bench report is open.
    InstallMeasurement measurement(vm, state->benchReport());
    // Frees what an ended production still holds; a flag test otherwise. Each glue entry on the VM thread calls it first.
    state->releaseEndedProductionMemory();

    // Only a newborn CB, whose UCB's sharing slot is empty and holds a pending import. Parked code wins, and a CB that
    // already ran in the LLInt receives no imported state.
    if (!state->importsEnabled())
        return InstallOutcome::NotInstalled;
    UnlinkedCodeBlock& ucb = *newborn.unlinkedCodeBlock();
    if (ucb.m_unlinkedBaselineCode)
        return InstallOutcome::NotInstalled;
    if (newborn.jitType() != JITType::None || newborn.baselineJITData())
        return InstallOutcome::NotInstalled;
    RefPtr<PendingImport> import = state->registry().pendingImport(ucb);
    if (!import)
        return InstallOutcome::NotInstalled;

    // The body pins every section span the lanes borrow through their last call; in twins builds the image twin check's
    // stash keeps a second reference.
    ASSERT(vm.currentThreadIsHoldingAPILock());
    DeferGCForAWhile installDeferral(vm);
    RefPtr<ValidatedBody> body = &import->body();
    const BodyKey key = import->key();
    const bool strict = state->strict();
    measurement.mark();

    // From here to the ICs lane's preparation nothing writes to the CB, the UCB or the VM's statistics: a failure leaves
    // the CB at its link state, and prepareForExecutionImpl goes on natively.

    // The image's sections. Their structure checks run only with strict on.
    ImageSectionSpans spans;
    spans.image = body->section(SectionKind::ImageBaseline);
    spans.bakedFacts = body->section(SectionKind::BakedFactsBaseline);
#if ENABLE(JITCACHE_TWINS)
    spans.twins = body->section(SectionKind::ImageTwinsBaseline);
#endif
    auto parsed = parseImageSections(spans, strict);
    measurement.endStep(InstallStep::ImageParse);
    if (!parsed) {
        state->raiseInvalidMaterial("image"_s, description(parsed.error()), importedBodyDetail(key));
        return InstallOutcome::Abandoned;
    }
    const ImageSectionsView& view = *parsed;

    // The sections against the import's UCB, which normal mode trusts.
    if (strict) {
        auto validated = validateImageSectionsAgainst(view, ucb);
        measurement.endStep(InstallStep::ImageValidation);
        if (!validated) {
            state->raiseInvalidMaterial("image"_s, description(validated.error()), importedBodyDetail(key));
            return InstallOutcome::Abandoned;
        }
    }

    // The facts the code baked against this CB. A mismatch leaves this CB native and keeps the import for the next
    // newborn CB of the UCB, which repeats this sequence against the same bytes.
    BakedFactsResult bakedFacts = compareBakedFacts(view, newborn);
    measurement.endStep(InstallStep::BakedFacts);
    if (bakedFacts == BakedFactsResult::Mismatch) {
        ++state->progress().bakedFactMismatches;
        return InstallOutcome::KeptForNextCodeBlock;
    }

    // The LLInt's own tier-up gate. It depends only on options and the body, so a failure leaves every CB of the body
    // native and drops the import. With the LLInt off there is no gate, as natively.
    if (point == InstallPoint::BeforeSetupLLInt) {
        bool passesGate = LLInt::shouldJIT(&newborn);
        measurement.endStep(InstallStep::Gate);
        if (!passesGate) {
            state->registry().resolvePendingImport(ucb, *import, ImportResolution::DroppedByGate);
            return InstallOutcome::DroppedByGate;
        }
    } else
        ASSERT(!Options::useLLInt());

    // The image's preparation. Only Consumer and ConsumerProducer VMs import, so the budget is non-null exactly in a
    // ConsumerProducer VM whose production is active, where the image rebuilds its record.
    ProducerContext* producerContext = state->producerContextIfActive();
    RefPtr<ProducerBudget> budget = producerContext ? RefPtr<ProducerBudget> { producerContext->budget() } : nullptr;
    PrepareTimings prepareTimings;
    auto prepared = prepareImage(vm, ucb, view, budget.get(), strict, measurement.report() ? &prepareTimings : nullptr);
    measurement.endStep(InstallStep::PrepareImage);
    if (!prepared) {
        switch (prepared.error().outcome) {
        case PrepareOutcome::InvalidMaterial:
            state->raiseInvalidMaterial("image"_s, description(prepared.error().check), importedBodyDetail(key));
            break;
        case PrepareOutcome::ExecutableMemoryExhausted:
            // A pool that cannot hold the image cannot hold a plan either, so this is the executable-allocation fault.
            didFailExecutableAllocation(vm, ExecutableAllocationSite::JITCacheImage);
            break;
        }
        return InstallOutcome::Abandoned;
    }

    // The CB lane's preparation. A failure destroys the prepared image without commit.
    auto cbImport = CBStateImport::prepare(body->section(SectionKind::CBStateBaseline), newborn, strict);
    measurement.endStep(InstallStep::CBPrepare);
    if (!cbImport) {
        ASSERT(cbImport.error().kind == CBFaultKind::InvalidMaterial);
        state->raiseInvalidMaterial("cb"_s, description(cbImport.error().check), importedBodyDetail(key));
        return InstallOutcome::Abandoned;
    }

    // The ICs lane's preparation, which pairs its section with the producer's molds the prepared image holds.
    auto icsPrepared = ICs::prepareBaselineICs(body->section(SectionKind::ICsBaseline), prepared->code(), newborn, strict ? ICs::StrictChecks::Yes : ICs::StrictChecks::No);
    measurement.endStep(InstallStep::PrepareBaselineICs);
    if (!icsPrepared) {
        state->raiseInvalidMaterial("ics"_s, icsRestoreCheckName(icsPrepared.error().check), makeString("site "_s, icsPrepared.error().siteIndex, "; "_s, importedBodyDetail(key)));
        return InstallOutcome::Abandoned;
    }

    // The rebuilt record's charge was refused, by itself or because an earlier refusal fails every later charge, and the
    // image goes on without a record. The recording fault ends production and leaves activity on, so the install
    // continues.
    if (budget && budget->hasRefused())
        state->raiseRecordingFault({ }, "budget.limit"_s, makeString("the rebuilt image record; "_s, importedBodyDetail(key)));

    // Nothing from here on can fail. The only cell allocations, the CB lane's realm step and lazy-operand holder, run
    // under this function's deferral and prepareForExecutionImpl's. The CB stays unpublished until installCode, so no
    // marker or compiler thread reads what the steps before it write.

    // The linked state. The two lanes write disjoint fields of shared metadata entries, after linking and before the CB
    // has a JIT type.
    measurement.mark();
    cbImport->seedLinkedState(newborn);
    measurement.endStep(InstallStep::SeedLinkedState);
    ICs::seedCallLinkHistory(*icsPrepared, newborn);
    measurement.endStep(InstallStep::SeedCallLinkHistory);

    // The image's commit: the code-size sample, the cross-modifying fence and the profiler reports.
    uint32_t imageCodeSize = view.header().codeSize;
    uint32_t imageFixupCount = view.header().fixupCount;
    Ref<BaselineJITCode> code = WTF::move(*prepared).commit(vm, newborn);
    measurement.endStep(InstallStep::Commit);

    // Native setup builds the BaselineJITData from the restored molds, arms the counter and parks the code in the empty
    // sharing slot.
    newborn.setupWithUnlinkedBaselineCode(code.copyRef());
    measurement.endStep(InstallStep::Setup);

    // The property ICs' state, in the BaselineJITData setup just built, and in twins builds the ICs lane's twin check.
    ICs::attachPropertyICState(*icsPrepared, newborn);
    measurement.endStep(InstallStep::AttachPropertyICState);
#if ENABLE(JITCACHE_TWINS)
    if (TwinReport* report = state->twinReportSink()) {
        reportICsTwinMismatches(*report, key, newborn, ICs::checkRestoredBaselineICs(*icsPrepared, newborn));
        measurement.mark();
    }
#endif

    // The counter, over the one setup armed. The CB lane's twin check runs before installCode, after which a marker may
    // merge into what it compares.
    CBCounterRestore restore = cbImport->finishCounter(newborn);
    measurement.endStep(InstallStep::FinishCounter);
#if ENABLE(JITCACHE_TWINS)
    if (TwinReport* report = state->twinReportSink()) {
        cbImport->verifyTwins(newborn, restore, *report);
        measurement.mark();
    }
#endif

    // installCode publishes the CB in its executable, once everything it runs with is in place.
    newborn.ownerExecutable()->installCode(&newborn);
    measurement.endStep(InstallStep::InstallCode);

    // Once the registry has let the import go, this function's reference is its last, and dropping it leaves the body to
    // the reference dropped below.
    state->registry().resolvePendingImport(ucb, *import, ImportResolution::Installed);
    import = nullptr;

    // The install's bookkeeping. The code's size is read before the twins builds' stash takes the code.
    ++state->progress().installs;
    uint64_t codeSize = measurement.report() ? static_cast<uint64_t>(code->size()) : 0;
#if ENABLE(JITCACHE_TWINS)
    stashImageTwinCheck(*state, PendingImageTwinCheck { &newborn, WTF::move(code), Ref<ValidatedBody> { *body }, view });
#endif
    if (!measurement.report()) {
        // Outside twins builds the last reference: its drop frees the body's buffer.
        body = nullptr;
        return InstallOutcome::Installed;
    }
    // The total closes before the drop, which the event reports on its own: releasing the body's buffer belongs with
    // opening it, which the bench measures apart from installation.
    uint64_t totalEnd = benchThreadCPUNanoseconds();
    body = nullptr;
    uint64_t releaseEnd = benchThreadCPUNanoseconds();
    recordInstallEvent(measurement, InstalledBody { key, restore, codeSize, imageCodeSize, imageFixupCount, prepareTimings }, totalEnd - measurement.startNanoseconds(),
        releaseEnd - totalEnd, measurement.supportGenerated());

    return InstallOutcome::Installed;
}

#if ENABLE(JITCACHE_TWINS)

void didFinishPrepareForExecution(VM& vm, CodeBlock& codeBlock, JSScope* scope)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    ImageTwinCheckState* twinCheckState = state->imageTwinCheckState();
    if (!twinCheckState || !twinCheckState->pending)
        return;

    // The check is consumed here whatever follows. The local keeps the body, and with it the bytes the view reads, until
    // the check returns, and a prepareForExecutionImpl the check itself reaches finds nothing pending.
    std::optional<PendingImageTwinCheck> pending = std::exchange(twinCheckState->pending, std::nullopt);
    TwinReport* report = state->twinReportSink();
    if (!report)
        return;

    // The stash holds this CB when the install ran for it, at either install point: no JS ran since, so it still runs the
    // code that install committed. Otherwise JIT::compileSync installed another CB outside prepareForExecutionImpl.
    if (pending->codeBlock != &codeBlock || codeBlock.jitCode().get() != pending->code.ptr()) {
        JITCacheInstallInternal::reportImageTwinCheckWithoutScope(*report, *pending);
        return;
    }
    twinCheckState->twins.checkImage(vm, codeBlock, scope, pending->code.get(), pending->view, *report);
}

#endif // ENABLE(JITCACHE_TWINS)

#else // ENABLE(JIT)

InstallOutcome installAtNewbornCodeBlock(VM&, CodeBlock&, InstallPoint)
{
    // Without the JIT no install point calls this, and start rejects the VM at its platform check.
    return InstallOutcome::NotInstalled;
}

#if ENABLE(JITCACHE_TWINS)
void didFinishPrepareForExecution(VM&, CodeBlock&, JSScope*)
{
}
#endif

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
