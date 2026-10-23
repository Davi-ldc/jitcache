#include "config.h"
#include "jitcache/glue/BaselineCapture.h"

#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/glue/CaptureEvents.h"
#include "jitcache/glue/JITCacheGlue.h"
#include "jitcache/glue/KeptSummaries.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/image/ImageCapture.h"
#include "jitcache/session/ProducerContext.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/ucb/UCBCapture.h"
#include <array>
#include <wtf/MallocSpan.h>
#include <wtf/text/MakeString.h>

namespace JSC::JITCache {

#if ENABLE(JIT)

namespace BaselineCaptureInternal {

// An index entry the writer adds for a key the index lacks is charged six of the index's bucket sizes.
constexpr size_t indexBucketBytes = sizeof(PackIndex::KeyValuePairType);

// The UCB lane's capture failed. A refused charge is raised as budget.limit, the budget's own fault, with the lane's
// step in the detail. A saved body the section read found damaged has had its fault raised by that read.
static void raiseUCBCaptureFailure(VMState& state, UCBCaptureFailure failure, const BodyKey& key)
{
    switch (failure) {
    case UCBCaptureFailure::BudgetExceeded:
        raiseBudgetLimit(state, makeString("ucb.capture-budget; "_s, bodyDetail(key)));
        return;
    case UCBCaptureFailure::SavedAnalysisUnusable:
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

using SectionSources = std::array<SectionSource, numberOfSectionKinds>;

// The UCB lane's sources, which come first in section-kind order: its three sections, then a module's analysis when the
// lane built one. Returns how many it wrote.
static size_t appendUCBSources(const UCBSections& ucb, SectionSources& sources)
{
    size_t count = 0;
    sources[count++] = SectionSource::inMemory(SectionKind::UCBIdentity, ucb.identity.span());
    sources[count++] = SectionSource::inMemory(SectionKind::UCBCore, ucb.core->span());
    sources[count++] = SectionSource::inMemory(SectionKind::UCBFeedback, ucb.feedback.span());
    if (!ucb.analysis.isEmpty())
        sources[count++] = SectionSource::inMemory(SectionKind::UCBAnalysis, ucb.analysis.span());
    return count;
}

// The ordered list of section sources the writer streams, in section-kind order. The bytes and objects the sources name
// belong to the three captures and the ICs buffer, which outlive the commit.
static CommitSections commitSectionsFor(const UCBSections& ucb, const ImageCapture& image, const CBStateCapture& cbState, std::span<const uint8_t> ics)
{
    SectionSources sources { };
    size_t count = appendUCBSources(ucb, sources);
    sources[count++] = SectionSource::streamed(SectionKind::ImageBaseline, image.imageSectionSize(), [](const void* object, const SectionSink& sink) {
        return static_cast<const ImageCapture*>(object)->writeImageSection(sink);
    }, &image);
    sources[count++] = SectionSource::streamed(SectionKind::BakedFactsBaseline, image.bakedFactsSectionSize(), [](const void* object, const SectionSink& sink) {
        return static_cast<const ImageCapture*>(object)->writeBakedFactsSection(sink);
    }, &image);
#if ENABLE(JITCACHE_TWINS)
    sources[count++] = SectionSource::streamed(SectionKind::ImageTwinsBaseline, image.twinsSectionSize(), [](const void* object, const SectionSink& sink) {
        return static_cast<const ImageCapture*>(object)->writeTwinsSection(sink);
    }, &image);
#endif
    sources[count++] = SectionSource::inMemory(SectionKind::CBStateBaseline, cbState.stateSection());
    sources[count++] = SectionSource::inMemory(SectionKind::CBSummaryBaseline, cbState.summarySection());
    sources[count++] = SectionSource::inMemory(SectionKind::ICsBaseline, ics);
    return CommitSections { std::span { sources }.first(count) };
}

// A body without an image: the UCB lane's sections alone, the three the container requires at tier 0 and a module's
// analysis.
static CommitSections commitSectionsFor(const UCBSections& ucb)
{
    SectionSources sources { };
    size_t count = appendUCBSources(ucb, sources);
    return CommitSections { std::span { sources }.first(count) };
}

// The end of a commit, for a capture whose built sections still beat the saved body: charges the entries the commit
// adds, commits through the writer and keeps the committed score.
static CommitOutcome commitBody(VMState& state, ProducerBudget& budget, const CommitStamp& stamp, const CommitSections& sections,
    const CaptureScore& committed, const CaptureClock& clock, CaptureTrace& trace)
{
    using Kind = CommitOutcome::Kind;
    const BodyKey& key = stamp.key;

    // Charge the entries the commit adds before anything is written. An index entry the writer adds joins the
    // state's index charge at once, so a commit that fails, or whose index update finds the object gone, leaves its
    // charge for the end of production to release.
    OpenedArtifact* artifact = state.artifact();
    ASSERT(artifact);
    size_t indexCharge = 0;
    if (!artifact->containsKey(key)) {
        indexCharge = entryChargeBuckets * indexBucketBytes;
        if (!budget.tryCharge(indexCharge)) {
            raiseBudgetLimit(state, makeString("an index entry; "_s, bodyDetail(key)));
            return { Kind::Faulted };
        }
        state.addIndexEntryCharge(indexCharge);
    }
    KeptSummaries* kept = state.partIfExists<KeptSummaries>();
    bool keptHasEntry = kept && kept->scores.contains(key);
    ScopedCharge keptCharge(budget);
    if (!keptHasEntry && !keptCharge.tryCharge(keptEntryCharge(state))) {
        raiseBudgetLimit(state, makeString("a kept summary; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }

    // Commit through the writer. While the bench report is open, the writer times its parts for the capture event.
    ArtifactWriter* writer = state.writer();
    ASSERT(writer);
    CommitTiming timing;
    auto result = writer->commit(stamp, sections, clock.report() ? &timing : nullptr);
    if (!result) {
        state.raiseRecordingFault({ }, result.error().check, makeString(result.error().detail, "; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }

    // Keep the committed score. The writer's reread checked every section of the file against the checksums of these same
    // bytes, so the kept score describes the file.
    SavedScore keptScore { committed, result->version };
    if (keptHasEntry)
        kept->scores.set(key, keptScore);
    else
        addKeptEntry(state, budget, key, keptScore, keptCharge.take());
    ++state.progress().capturesCommitted;
    state.progress().bytesCommitted += result->fileSize;
    if (clock.report()) {
        // Every charge this capture made is still held here, the lanes' outputs and any ICs buffer included.
        CaptureTrace::CommitTrace commitTrace { timing, { }, budget.chargedBytes(), state.partIfExists<KeptSummaries>()->chargedBytes, indexCharge };
        for (const SectionSource& source : sections.sources())
            commitTrace.sectionBytes[static_cast<size_t>(source.kind)] = source.size;
        trace.commit = commitTrace;
    }
    return { Kind::Committed, result->fileSize };
}

} // namespace BaselineCaptureInternal

std::optional<UCBRegistry::RecordView> candidateRecord(VM& vm, CodeBlock& codeBlock)
{
    if (codeBlock.jitType() != JITType::BaselineJIT || codeBlock.replacement() != &codeBlock)
        return std::nullopt;
    RefPtr<JITCode> code = codeBlock.jitCode();
    if (!code || !isImageCapturable(static_cast<const BaselineJITCode&>(*code)))
        return std::nullopt;
    return captureRecord(vm, *codeBlock.unlinkedCodeBlock());
}

CommitOutcome commitCapture(VM& vm, VMState& state, CodeBlock& codeBlock, const BodyKey& key, const std::optional<CaptureScore>& saved,
    const CaptureClock& clock, CaptureTrace& trace)
{
    using namespace BaselineCaptureInternal;
    using Kind = CommitOutcome::Kind;
    ProducerBudget& budget = *state.producerBudget();
    UnlinkedCodeBlock& ucb = *codeBlock.unlinkedCodeBlock();
    CaptureTrace::BuildTimes times;

    // The image's write functions stream from the live code, which stays alive until the writer returns.
    Ref<BaselineJITCode> code { static_cast<BaselineJITCode&>(*codeBlock.jitCode()) };

    // The image is the only builder that can declare the CB ineligible, so it comes first.
    uint64_t start = clock.now();
    auto image = captureImage(vm, codeBlock, code.get(), budget);
    times.captureImage = clock.now() - start;
    if (!image) {
        switch (image.error()) {
        case CaptureOutcome::NotEligible:
            return { Kind::NotEligible };
        case CaptureOutcome::ChargeRefused:
            raiseBudgetLimit(state, makeString("image.capture; "_s, bodyDetail(key)));
            return { Kind::Faulted };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The UCB lane's sections and the richness of what they hold.
    start = clock.now();
    auto ucbSections = buildSections(vm, ucb, holderOf(codeBlock), BodyImage::Present, budget);
    UCBRichness ucbRichness;
    if (ucbSections)
        ucbRichness = liveRichness(ucb);
    times.buildSections = clock.now() - start;
    if (!ucbSections) {
        raiseUCBCaptureFailure(state, ucbSections.error(), key);
        return { Kind::Faulted };
    }
    // A section read inside the build may have raised invalid material without failing the build; the capture then
    // stops with nothing written.
    if (!state.productionActive())
        return { Kind::Faulted };

    // The ICs lane comes before the CB lane, whose capture takes its polymorphic bit for the same CB in the same pause.
    start = clock.now();
    size_t icsSize = ICs::baselineICsSectionSize(codeBlock);
    ScopedCharge icsCharge(budget);
    if (!icsCharge.tryCharge(icsSize)) {
        raiseBudgetLimit(state, makeString("the "_s, icsSize, "-byte ICsBaseline section; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }
    MallocSpan<uint8_t> icsBuffer = MallocSpan<uint8_t>::malloc(icsSize);
    ICs::CaptureSummary ics = ICs::captureBaselineICs(codeBlock, icsBuffer.mutableSpan());
    times.captureBaselineICs = clock.now() - start;

    // The CB lane's state, with the ICs lane's polymorphic bit.
    trace.hasPolymorphicSite = ics.hasPolymorphicSite;
    start = clock.now();
    auto cbState = CBStateCapture::capture(codeBlock, budget, ics.hasPolymorphicSite);
    times.cbStateCapture = clock.now() - start;
    if (!cbState) {
        // The budget refused the two section buffers.
        raiseBudgetLimit(state, makeString("cb.capture-charge; section buffers the producer budget refused; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }
    trace.build = times;

    // The score of the bytes just built, from what the builders returned, which equals what scoreSections would compute
    // from the committed file, so no lane's bytes are read back. A drain between the scoring and the build can move it, so
    // the capture stops when the build no longer wins.
    const CBScore& cbScore = cbState->score();
    CaptureScore committed { baselineTier, ucbRichness.total() + cbScore.richnessUnits, ics.summary.icSitesWithCases, cbScore.counterWithheld, cbScore.counterProgress };
    trace.committed = committed;
    trace.committedExitSiteUnits = ucbRichness.exitSiteUnits;
    if (!beatsSavedBody(committed, saved))
        return { Kind::Beaten };

    // The LLInt threshold (L) and counter progress (P) that compact orders bodies by. P is zero for a body whose counter
    // does not travel.
    CommitStamp stamp { key, envelopeLLIntThreshold(ucb), cbScore.counterProgress, baselineTier };

    return commitBody(state, budget, stamp, commitSectionsFor(*ucbSections, *image, *cbState, icsBuffer.span()), committed, clock, trace);
}

CommitOutcome commitImagelessCapture(VM& vm, VMState& state, UnlinkedCodeBlock& ucb, const UnlinkedFunctionExecutable* holder, const BodyKey& key,
    const CaptureClock& clock, CaptureTrace& trace)
{
    using namespace BaselineCaptureInternal;
    using Kind = CommitOutcome::Kind;
    ProducerBudget& budget = *state.producerBudget();
    CaptureTrace::BuildTimes times;

    // The UCB lane's sections; there is no image, ICs or CB state to build.
    uint64_t start = clock.now();
    auto ucbSections = buildSections(vm, ucb, holder, BodyImage::Absent, budget);
    times.buildSections = clock.now() - start;
    if (!ucbSections) {
        raiseUCBCaptureFailure(state, ucbSections.error(), key);
        return { Kind::Faulted };
    }
    // As for a capture with an image, invalid material raised inside the build stops the capture.
    if (!state.productionActive())
        return { Kind::Faulted };
    trace.build = times;

    // The score of the bytes just built is the live score the capture was ranked with: it reads the arithmetic profiles, the
    // exit sites and the LLInt counter, which no drain touches and nothing changes during a capture, so the glue reads none
    // of the capture's own bytes back.
    CaptureScore committed = trace.live.score;
    trace.committed = committed;
    trace.committedExitSiteUnits = trace.live.exitSiteUnits;

    CommitStamp stamp { key, committed.counterProgress, 0, imagelessTier };

    return commitBody(state, budget, stamp, commitSectionsFor(*ucbSections), committed, clock, trace);
}

bool shouldRecordBaselineImage(VM& vm, CodeBlock& codeBlock)
{
    // A producer context exists only while the VM's state does.
    if (!producerContext(vm))
        return false;
    return vm.jitCacheState()->bodyKeyOf(*codeBlock.unlinkedCodeBlock()).has_value();
}

void didFinalizeBaselineCompilation(VM& vm, CodeBlock& codeBlock, const BaselineCompileTiming* timing)
{
#if ENABLE(JITCACHE_TWINS)
    // The body's event counts: a baseline compilation installed its code, whatever the VM's JITCache state.
    ++codeBlock.unlinkedCodeBlock()->jitCacheEventCounts().baselineCompiles;
#endif

    VMState* state = vm.jitCacheState();
    if (!state)
        return;

    // A plan passes a timing exactly when its VM had an open bench report at the plan's construction, so a Consumer
    // measures the native cost of the bodies it compiles too.
    if (timing)
        recordCompileEvent(*state, codeBlock, *timing);

    CaptureInProgressScope captureInProgress(*state);
    state->releaseEndedProductionMemory();
    if (!state->productionActive())
        return;

    // A refusal made on a JIT worker only marks the budget there; it reaches the VM thread here.
    ProducerBudget& budget = *state->producerBudget();
    if (budget.hasRefused()) {
        raiseBudgetLimit(*state, "a charge refused before the finalize capture"_s);
        return;
    }

    // installCode has just made the CB its executable's replacement.
    auto record = candidateRecord(vm, codeBlock);
    if (!record)
        return;
    ++state->progress().captureCandidates;

    // The candidate's live score against the saved body's.
    CaptureClock clock(state->benchReport());
    LiveScore live = scoreLiveCandidate(codeBlock, clock);
    SavedScoring saved = scoreSavedBody(*state, budget, record->key, clock);
    if (saved.outcome == SavedScoring::Outcome::Faulted)
        return;
    std::optional<CaptureScore> scoredSaved = saved.saved ? std::optional { saved.saved->score } : std::nullopt;
    CaptureTrace trace { "finalize"_s, record->key, live, scoredSaved, saved.readNanoseconds, std::nullopt, 0, live.hasPolymorphicSite, std::nullopt, std::nullopt };
    if (saved.outcome == SavedScoring::Outcome::Deferred) {
        recordCaptureEvent(clock.report(), trace, "deferred"_s);
        return;
    }
    if (!beatsSavedBody(live.score, scoredSaved)) {
        recordCaptureEvent(clock.report(), trace, "beaten"_s);
        return;
    }

    // A NotEligible image ends this CB's capture without a fault, as for an ineligible CB.
    CommitOutcome outcome = commitCapture(vm, *state, codeBlock, record->key, scoredSaved, clock, trace);
    if (outcome.kind == CommitOutcome::Kind::Committed || outcome.kind == CommitOutcome::Kind::Beaten)
        recordCaptureEvent(clock.report(), trace, eventOutcome(outcome.kind));
}

#else // !ENABLE(JIT)

// A build without the JIT configures no VM: start rejects at start.platform before anything else, so nothing here has a
// state to capture for.
void didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*)
{
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
