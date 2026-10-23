#include "config.h"

#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "JSCInlines.h"
#include "LLIntSlowPaths.h"
#include "ScriptExecutable.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "jitcache/base/BenchClock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/glue/InstallEvents.h"
#include "jitcache/glue/InstallTwins.h"
#include "jitcache/glue/JITCacheGlue.h"
#include "jitcache/ics/ICRestore.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/image/BakedFacts.h"
#include "jitcache/image/ImagePrepare.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageSupport.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/session/JITCacheFaults.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/session/ProducerContext.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/text/MakeString.h>

// The install glue: the install function the two install points call for a newborn CodeBlock whose UCB holds a pending
// import. Everything runs on the VM thread with the API lock and heap access, under the install function's own GC
// deferral. InstallEvents.h holds what the install event measures, and InstallTwins.h the twin checks of twins builds.

namespace JSC::JITCache {

#if ENABLE(JIT)

namespace BaselineInstallInternal {

// The detail of a fault an install raises: the body by its key.
static String importedBodyDetail(const BodyKey& key)
{
    return makeString("body "_s, bodyKeyHex(key));
}

} // namespace BaselineInstallInternal

InstallOutcome installAtNewbornCodeBlock(VM& vm, CodeBlock& newborn, InstallPoint point)
{
    using namespace BaselineInstallInternal;

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
    measurement.mark();

    // From here to the ICs lane's preparation nothing writes to the CB, the UCB or the VM's statistics: a failure leaves
    // the CB at its link state, and prepareForExecutionImpl goes on natively.

    // The image's sections.
    ImageSectionSpans spans;
    spans.image = body->section(SectionKind::ImageBaseline);
    spans.bakedFacts = body->section(SectionKind::BakedFactsBaseline);
#if ENABLE(JITCACHE_TWINS)
    spans.twins = body->section(SectionKind::ImageTwinsBaseline);
#endif
    auto parsed = parseImageSections(spans);
    measurement.endStep(InstallStep::ImageParse);
    if (!parsed) {
        state->raiseInvalidMaterial("image"_s, description(parsed.error()), importedBodyDetail(key));
        return InstallOutcome::Abandoned;
    }
    const ImageSectionsView& view = *parsed;

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

    // The image's preparation, with the VM's table of the support targets its earlier preparations resolved. Only
    // Consumer and ConsumerProducer VMs import, so the budget is non-null exactly in a ConsumerProducer VM whose
    // production is active, where the image rebuilds its record.
    ProducerContext* producerContext = state->producerContextIfActive();
    RefPtr<ProducerBudget> budget = producerContext ? RefPtr<ProducerBudget> { producerContext->budget() } : nullptr;
    PrepareTimings prepareTimings;
    auto prepared = prepareImage(vm, ucb, view, state->part<SupportTargetTable>(), budget.get(), measurement.timesSteps() ? &prepareTimings : nullptr);
    measurement.endStep(InstallStep::PrepareImage);
    if (!prepared) {
        // The image or a snippet got no executable memory. A pool that cannot hold the image cannot hold a plan either,
        // so this is the executable-allocation fault.
        didFailExecutableAllocation(vm, ExecutableAllocationSite::JITCacheImage);
        return InstallOutcome::Abandoned;
    }

    // The CB lane's preparation.
    CBStateImport cbImport = CBStateImport::prepare(body->section(SectionKind::CBStateBaseline));
    measurement.endStep(InstallStep::CBPrepare);

    // The ICs lane's preparation, which reads its section alone.
    ICs::PreparedBaselineICs icsPrepared = ICs::prepareBaselineICs(body->section(SectionKind::ICsBaseline));
    measurement.endStep(InstallStep::PrepareBaselineICs);

    // The rebuilt record's charge was refused, by itself or because an earlier refusal fails every later charge, and the
    // image goes on without a record. The recording fault ends production and leaves activity on, so the install
    // continues.
    if (budget && budget->hasRefused())
        state->raiseRecordingFault({ }, "budget.limit"_s, makeString("rebuilt image record; "_s, importedBodyDetail(key)));

    // Nothing from here on can fail. The cell allocations, the CB lane's realm step and the FunctionExecutables native
    // setup materializes, and the other allocations, the CB lane's predictions and lazy-operand holder and the ICs lane's
    // CallSiteData, run under this function's deferral, which only records a collection one of them requests. The CB
    // stays unpublished until installCode, so no marker or compiler thread reads what the steps before it write.

    // The linked state, after linking and before the CB has a JIT type. The ICs lane gives each restored call site its
    // own CallSiteData where the site needs one; the CB lane writes no CallSiteData, and the two write disjoint fields of
    // shared metadata entries.
    measurement.mark();
    ICs::seedCallLinkHistory(icsPrepared, newborn);
    measurement.endStep(InstallStep::SeedCallLinkHistory);
    cbImport.seedLinkedState(newborn);
    measurement.endStep(InstallStep::SeedLinkedState);

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
    ICs::attachPropertyICState(icsPrepared, newborn);
    measurement.endStep(InstallStep::AttachPropertyICState);
#if ENABLE(JITCACHE_TWINS)
    if (TwinReport* report = state->twinReportSink()) {
        reportICsTwinMismatches(*report, key, newborn, ICs::checkRestoredBaselineICs(icsPrepared, newborn));
        measurement.mark();
    }
#endif

    // The counter, over the one setup armed. The CB lane's twin check runs before installCode, after which a marker may
    // merge into what it compares.
    CBCounterRestore restore = cbImport.finishCounter(newborn);
    measurement.endStep(InstallStep::FinishCounter);
#if ENABLE(JITCACHE_TWINS)
    if (TwinReport* report = state->twinReportSink()) {
        cbImport.verifyTwins(newborn, restore, *report);
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
    stashImageTwinCheck(*state, newborn, WTF::move(code), Ref<ValidatedBody> { *body }, view);
#endif
    if (!measurement.report()) {
        // Outside twins builds the last reference: its drop frees the body's buffer.
        body = nullptr;
        return InstallOutcome::Installed;
    }
    // The total closes before the drop, which the event reports on its own, in thread CPU time from the total's last
    // read: releasing the body's buffer belongs with opening it, which the bench measures apart from installation.
    BoundSpanTimer::Times total = measurement.stopTotal();
    body = nullptr;
    uint64_t releaseEnd = benchThreadCPUNanoseconds();
    recordInstallEvent(measurement, InstalledBody { key, restore, codeSize, imageCodeSize, imageFixupCount, prepareTimings }, total.nanoseconds(),
        releaseEnd - total.cpuEnd, measurement.supportGenerated());

    return InstallOutcome::Installed;
}

#else // ENABLE(JIT)

InstallOutcome installAtNewbornCodeBlock(VM&, CodeBlock&, InstallPoint)
{
    // Without the JIT no install point calls this, and start rejects the VM at its platform check.
    return InstallOutcome::NotInstalled;
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
