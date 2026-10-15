#include "config.h"
#include "JITCacheCapture.h"

#include "ArtifactStore.h"
#include "BaselineJITCode.h"
#include "CBState.h"
#include "CodeBlock.h"
#include "CodeBlockSet.h"
#include "DeferGC.h"
#include "HeapInlines.h"
#include "ICCapture.h"
#include "ICSection.h"
#include "ImageCapture.h"
#include "JITCacheAPI.h"
#include "JITCacheBench.h"
#include "JITCacheContainer.h"
#include "JITCacheGlue.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "ProducerBudget.h"
#include "UCBCapture.h"
#include "UCBFeedback.h"
#include "UCBRegistry.h"
#include "UCBSections.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include <array>
#include <expected>
#include <optional>
#include <tuple>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/Locker.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/SafeStrerror.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

// The capture glue: the test BaselineJITPlan's constructor makes for whether a compilation records its image, the finalize
// capture BaselineJITPlan::finalize calls, delta with its walk of the UCB registry for the bodies that ran without
// compiling, the scores that order captures, the kept summaries, and the commit of one capture through the writer.
// Everything runs on the VM thread with JS paused; nothing here allocates a cell, drains a profile, stops for the
// collector or finalizes a plan, so captures never nest.

namespace JSC::JITCache {

bool beats(const CaptureScore& candidate, const CaptureScore& saved)
{
    // Tier, richness, IC sites with cases, a withheld counter above one that travels, then counter progress. Strictly
    // greater only, so a remaining tie keeps the saved body.
    return std::tie(candidate.tier, candidate.richness, candidate.icSitesWithCases, candidate.counterWithheld, candidate.counterProgress)
        > std::tie(saved.tier, saved.richness, saved.icSitesWithCases, saved.counterWithheld, saved.counterProgress);
}

#if ENABLE(JIT)

namespace JITCacheCaptureInternal {

// A hash table the glue owns for production is charged eight bucket sizes with its first entry and six more per entry,
// which bounds WTF's storage for a table that only grows, rehashes included.
constexpr size_t tableChargeBuckets = 8;
constexpr size_t entryChargeBuckets = 6;
static_assert(BodyKeyHashTraits::minimumTableSize == tableChargeBuckets);

} // namespace JITCacheCaptureInternal

// The kept summaries: the score of each key's saved body, read once at the key's first scoring and replaced at each
// commit, so a kept summary always describes the file its key names. The table is production memory, charged by the
// rule above before each entry is added, and destroying it releases every charge it holds.
struct KeptSummaries {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(KeptSummaries);
    WTF_MAKE_NONCOPYABLE(KeptSummaries);

    using Map = HashMap<BodyKey, SavedScore, BodyKeyHash, BodyKeyHashTraits>;
    static constexpr size_t bucketBytes = sizeof(Map::KeyValuePairType);
    static constexpr size_t entryBytes = JITCacheCaptureInternal::entryChargeBuckets * bucketBytes;

    explicit KeptSummaries(ProducerBudget& producerBudget)
        : budget(producerBudget)
    {
    }

    ~KeptSummaries()
    {
        // The table is freed before its charge is released.
        scores.clear();
        if (chargedBytes)
            budget->release(chargedBytes);
    }

    // An entry's charge goes with the entry; the table's stays until the table goes.
    void erase(const BodyKey& key)
    {
        if (!scores.remove(key))
            return;
        chargedBytes -= entryBytes;
        budget->release(entryBytes);
    }

    const Ref<ProducerBudget> budget;
    Map scores;
    size_t chargedBytes { 0 }; // the table's eight buckets once it held an entry, and six per entry it holds
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(KeptSummaries);

namespace JITCacheCaptureInternal {

// An index entry the writer adds for a key the index lacks is charged six of the index's bucket sizes.
constexpr size_t indexBucketBytes = sizeof(PackIndex::KeyValuePairType);

static size_t newEntryCharge(size_t bucketBytes, bool tableCharged)
{
    return (tableCharged ? 0 : tableChargeBuckets * bucketBytes) + entryChargeBuckets * bucketBytes;
}

static constexpr ASCIILiteral budgetLimit = "budget.limit"_s;

// A charge against the producer budget that the scope releases unless something takes it over. A buffer the charge pays
// for is declared after it, so the buffer is freed before the charge is released.
class ScopedCharge {
    WTF_MAKE_NONCOPYABLE(ScopedCharge);
public:
    explicit ScopedCharge(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~ScopedCharge()
    {
        if (m_bytes)
            m_budget.release(m_bytes);
    }

    [[nodiscard]] bool tryCharge(size_t bytes)
    {
        if (!m_budget.tryCharge(bytes))
            return false;
        m_bytes += bytes;
        return true;
    }

    size_t take() { return std::exchange(m_bytes, 0); }

private:
    ProducerBudget& m_budget;
    size_t m_bytes { 0 };
};

// Reads the bench report's clock, the thread CPU time, only while a report is open, so a run without one pays a load and
// a test.
class CaptureClock {
public:
    explicit CaptureClock(BenchReport* report)
        : m_report(report)
    {
    }

    BenchReport* report() const { return m_report; }
    uint64_t now() const { return m_report ? benchThreadCPUNanoseconds() : 0; }

private:
    BenchReport* const m_report;
};

// Captures never nest, since nothing a capture does can start another. Debug builds count the captures in progress, and
// the finalize capture and delta assert on entry that none is.
class CaptureInProgressScope {
    WTF_MAKE_NONCOPYABLE(CaptureInProgressScope);
public:
    explicit CaptureInProgressScope(VMState& state)
#if ASSERT_ENABLED
        : m_state(state)
#endif
    {
#if ASSERT_ENABLED
        ASSERT(!m_state.capturesInProgress());
        ++m_state.capturesInProgress();
#else
        UNUSED_PARAM(state);
#endif
    }

    ~CaptureInProgressScope()
    {
#if ASSERT_ENABLED
        --m_state.capturesInProgress();
#endif
    }

private:
#if ASSERT_ENABLED
    VMState& m_state;
#endif
};

// The enumerator names the ICs lane's checks are raised at, as the fault's check.
static ASCIILiteral icsCheckName(ICs::Check check)
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

static ASCIILiteral icsCaptureCheckName(ICs::CaptureCheck check)
{
    switch (check) {
    case ICs::CaptureCheck::NoBaselineJITData:
        return "NoBaselineJITData"_s;
    case ICs::CaptureCheck::OutputSizeMismatch:
        return "OutputSizeMismatch"_s;
    case ICs::CaptureCheck::MoldMismatch:
        return "MoldMismatch"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral ucbCaptureCheckName(UCBCaptureFailure failure)
{
    switch (failure) {
    case UCBCaptureFailure::BudgetExceeded:
        return "capture-budget"_s;
    case UCBCaptureFailure::StrictCheckFailed:
        return "capture-strict"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static String bodyDetail(const BodyKey& key)
{
    return makeString("body "_s, bodyKeyHex(key));
}

static void raiseBudgetLimit(VMState& state, String detail)
{
    state.raiseRecordingFault({ }, budgetLimit, WTF::move(detail));
}

// A lane's capture or scoring call failed. Every refusal of a production charge is raised as budget.limit, the budget's
// own fault, with the lane's check in the detail; any other failure is that lane's recording fault.
static void raiseCaptureFault(VMState& state, ProducerBudget& budget, ASCIILiteral part, ASCIILiteral check, String detail)
{
    if (budget.hasRefused()) {
        raiseBudgetLimit(state, makeString(part, '.', check, "; "_s, detail));
        return;
    }
    state.raiseRecordingFault(part, check, WTF::move(detail));
}

// The CB is a candidate when it runs its own capturable baseline code and the UCB lane recorded its key; the record is
// that key's.
static std::optional<UCBRegistry::RecordView> candidateRecord(VM& vm, CodeBlock& codeBlock)
{
    if (codeBlock.jitType() != JITType::BaselineJIT || codeBlock.replacement() != &codeBlock)
        return std::nullopt;
    RefPtr<JITCode> code = codeBlock.jitCode();
    if (!code || !isImageCapturable(static_cast<const BaselineJITCode&>(*code)))
        return std::nullopt;
    return captureRecord(vm, *codeBlock.unlinkedCodeBlock());
}

// A live candidate's score, with what its bench event reports beside it.
struct LiveScore {
    CaptureScore score;
    uint64_t exitSiteUnits { 0 }; // UCBRichness::exitSiteUnits, which the capture event reports apart
    bool hasPolymorphicSite { false }; // the bit the CB lane's scoreLive received
    uint64_t summarizeBaselineICsNanoseconds { 0 };
    uint64_t scoreLiveNanoseconds { 0 };
    uint64_t liveRichnessNanoseconds { 0 };
};

// The ICs lane comes before the CB lane, for the same CB in the same pause, and its polymorphic bit goes to scoreLive, so
// a candidate with a polymorphic site scores its counter as withheld. A failure is the CB lane's eligibility or pairing
// guard under strict, CBCheck::CaptureStrict or CapturePairing.
static std::expected<LiveScore, CBFault> scoreLiveCandidate(CodeBlock& codeBlock, bool strict, const CaptureClock& clock)
{
    uint64_t start = clock.now();
    ICs::CaptureSummary ics = ICs::summarizeBaselineICs(codeBlock);
    uint64_t afterICs = clock.now();
    auto cbScore = CBStateCapture::scoreLive(codeBlock, strict, ics.hasPolymorphicSite);
    uint64_t afterCB = clock.now();
    if (!cbScore)
        return std::unexpected(cbScore.error());
    UCBRichness ucbRichness = liveRichness(*codeBlock.unlinkedCodeBlock(), BodyImage::Present);
    uint64_t end = clock.now();

    LiveScore live;
    live.score = CaptureScore { baselineTier, ucbRichness.total() + cbScore->richnessUnits, ics.summary.icSitesWithCases, cbScore->counterWithheld, cbScore->counterProgress };
    live.exitSiteUnits = ucbRichness.exitSiteUnits;
    live.hasPolymorphicSite = ics.hasPolymorphicSite;
    live.summarizeBaselineICsNanoseconds = afterICs - start;
    live.scoreLiveNanoseconds = afterCB - afterICs;
    live.liveRichnessNanoseconds = end - afterCB;
    return live;
}

// A body that ran without an image: tier 0, below every capture with an image, scored by the UCB lane
// alone, its UCB copies included, with the LLInt progress it carries in place of the baseline counter's. Neither IC sites
// nor a withheld counter apply.
static LiveScore scoreImagelessCandidate(UnlinkedCodeBlock& ucb, const CaptureClock& clock)
{
    uint64_t start = clock.now();
    UCBRichness richness = liveRichness(ucb, BodyImage::Absent);
    BaselineExecutionCounter& counter = ucb.llintExecuteCounter();
    uint32_t progress = carriedLLIntPoints(counter.m_activeThreshold, counter.count());
    uint64_t end = clock.now();

    LiveScore live;
    live.score = CaptureScore { imagelessTier, richness.total(), 0, false, progress };
    live.exitSiteUnits = richness.exitSiteUnits;
    live.liveRichnessNanoseconds = end - start;
    return live;
}

// What one scored capture's bench event reports.
struct CaptureTrace {
    ASCIILiteral trigger; // "finalize" or "delta"
    BodyKey key;
    LiveScore live;
    std::optional<CaptureScore> saved; // set when a saved body was scored, from the kept summary or the scoring read
    std::optional<uint64_t> savedReadNanoseconds; // the scoring read with its scoreSections; none when the kept summary answered
    std::optional<CaptureScore> committed; // the score of the bytes built, once the capture built its sections
    uint64_t committedExitSiteUnits { 0 };
    bool hasPolymorphicSite { false }; // the bit last passed to the CB lane
    struct BuildTimes {
        uint64_t captureImage { 0 };
        uint64_t buildSections { 0 }; // with liveRichness
        uint64_t captureBaselineICs { 0 };
        uint64_t cbStateCapture { 0 };
    };
    std::optional<BuildTimes> build; // once every lane built its sections
    // What a commit adds, once the writer published while the report is open: the time of each of the writer's parts
    // beside each section's size, and the producer memory the commit holds.
    struct CommitTrace {
        CommitTiming timing;
        std::array<uint64_t, numberOfSectionKinds> sectionBytes { }; // by SectionKind; 0 for a kind the body lacks
        uint64_t budgetChargedBytes { 0 }; // the budget's charge at the commit, the capture's sections and records still held
        uint64_t keptSummariesBytes { 0 }; // the kept summaries' charge once the commit's entry is kept
        uint64_t indexEntryBytes { 0 }; // what commitBody charged for an index entry the writer added; 0 when the index had the key
    };
    std::optional<CommitTrace> commit;
};

// The first field in beats order in which two scores differ, or none.
static ASCIILiteral firstDifference(const CaptureScore& a, const CaptureScore& b)
{
    if (a.tier != b.tier)
        return "tier"_s;
    if (a.richness != b.richness)
        return "richness"_s;
    if (a.icSitesWithCases != b.icSitesWithCases)
        return "icSitesWithCases"_s;
    if (a.counterWithheld != b.counterWithheld)
        return "counterWithheld"_s;
    if (a.counterProgress != b.counterProgress)
        return "counterProgress"_s;
    return { };
}

// The capture event: the trigger, the key, how the capture ended, the candidate's score
// with the UCB lane's exit-site units apart, the committed score once the sections were built, the saved score with the
// first field in which the candidate's differs from it, the polymorphic bit the CB lane received, the thread CPU time of
// each lane's scoring and build call and of the scoring read, and, for a commit, the writer's parts from its CommitTiming
// beside each section's size and the producer memory the commit holds. A field the capture did not reach is null.
static void recordCaptureEvent(BenchReport* report, const CaptureTrace& trace, ASCIILiteral outcome)
{
    if (!report)
        return;
    using BenchValue = decltype(BenchField::value);
    auto number = [](std::optional<uint64_t> value) {
        return value ? BenchValue { *value } : BenchValue { nullptr };
    };
    auto field = [&](const std::optional<CaptureScore>& score, auto member) {
        return score ? number(static_cast<uint64_t>((*score).*member)) : BenchValue { nullptr };
    };
    auto flag = [](const std::optional<CaptureScore>& score) {
        return score ? BenchValue { score->counterWithheld } : BenchValue { nullptr };
    };
    std::optional<CaptureScore> candidate = trace.live.score;
    ASCIILiteral difference = trace.saved ? firstDifference(*candidate, *trace.saved) : ASCIILiteral { };
    auto build = [&](uint64_t CaptureTrace::BuildTimes::* member) {
        return trace.build ? number((*trace.build).*member) : BenchValue { nullptr };
    };
    auto stream = [&](SectionKind kind) {
        return trace.commit ? number(trace.commit->timing.streamNanoseconds[static_cast<size_t>(kind)]) : BenchValue { nullptr };
    };
    auto bytes = [&](SectionKind kind) {
        return trace.commit ? number(trace.commit->sectionBytes[static_cast<size_t>(kind)]) : BenchValue { nullptr };
    };
    auto commit = [&](uint64_t CaptureTrace::CommitTrace::* member) {
        return trace.commit ? number((*trace.commit).*member) : BenchValue { nullptr };
    };
    report->record("capture"_s, {
        { "trigger"_s, String { trace.trigger } },
        { "key"_s, bodyKeyHex(trace.key) },
        { "outcome"_s, String { outcome } },
        { "candidate.tier"_s, field(candidate, &CaptureScore::tier) },
        { "candidate.richness"_s, field(candidate, &CaptureScore::richness) },
        { "candidate.exitSiteUnits"_s, trace.live.exitSiteUnits },
        { "candidate.icSitesWithCases"_s, field(candidate, &CaptureScore::icSitesWithCases) },
        { "candidate.counterWithheld"_s, flag(candidate) },
        { "candidate.counterProgress"_s, field(candidate, &CaptureScore::counterProgress) },
        { "committed.tier"_s, field(trace.committed, &CaptureScore::tier) },
        { "committed.richness"_s, field(trace.committed, &CaptureScore::richness) },
        { "committed.exitSiteUnits"_s, number(trace.committed ? std::optional<uint64_t> { trace.committedExitSiteUnits } : std::nullopt) },
        { "committed.icSitesWithCases"_s, field(trace.committed, &CaptureScore::icSitesWithCases) },
        { "committed.counterWithheld"_s, flag(trace.committed) },
        { "committed.counterProgress"_s, field(trace.committed, &CaptureScore::counterProgress) },
        { "saved.tier"_s, field(trace.saved, &CaptureScore::tier) },
        { "saved.richness"_s, field(trace.saved, &CaptureScore::richness) },
        { "saved.icSitesWithCases"_s, field(trace.saved, &CaptureScore::icSitesWithCases) },
        { "saved.counterWithheld"_s, flag(trace.saved) },
        { "saved.counterProgress"_s, field(trace.saved, &CaptureScore::counterProgress) },
        { "decidingField"_s, difference.isNull() ? BenchValue { nullptr } : BenchValue { String { difference } } },
        { "hasPolymorphicSite"_s, trace.hasPolymorphicSite },
        { "scoring.summarizeBaselineICs"_s, trace.live.summarizeBaselineICsNanoseconds },
        { "scoring.scoreLive"_s, trace.live.scoreLiveNanoseconds },
        { "scoring.liveRichness"_s, trace.live.liveRichnessNanoseconds },
        { "scoring.savedRead"_s, number(trace.savedReadNanoseconds) },
        { "build.captureImage"_s, build(&CaptureTrace::BuildTimes::captureImage) },
        { "build.buildSections"_s, build(&CaptureTrace::BuildTimes::buildSections) },
        { "build.captureBaselineICs"_s, build(&CaptureTrace::BuildTimes::captureBaselineICs) },
        { "build.cbStateCapture"_s, build(&CaptureTrace::BuildTimes::cbStateCapture) },
        { "writer.ucb.identity.stream"_s, stream(SectionKind::UCBIdentity) },
        { "writer.ucb.identity.bytes"_s, bytes(SectionKind::UCBIdentity) },
        { "writer.ucb.core.stream"_s, stream(SectionKind::UCBCore) },
        { "writer.ucb.core.bytes"_s, bytes(SectionKind::UCBCore) },
        { "writer.ucb.feedback.stream"_s, stream(SectionKind::UCBFeedback) },
        { "writer.ucb.feedback.bytes"_s, bytes(SectionKind::UCBFeedback) },
        { "writer.image.baseline.stream"_s, stream(SectionKind::ImageBaseline) },
        { "writer.image.baseline.bytes"_s, bytes(SectionKind::ImageBaseline) },
        { "writer.baked-facts.baseline.stream"_s, stream(SectionKind::BakedFactsBaseline) },
        { "writer.baked-facts.baseline.bytes"_s, bytes(SectionKind::BakedFactsBaseline) },
        { "writer.image-twins.baseline.stream"_s, stream(SectionKind::ImageTwinsBaseline) },
        { "writer.image-twins.baseline.bytes"_s, bytes(SectionKind::ImageTwinsBaseline) },
        { "writer.cb.state.stream"_s, stream(SectionKind::CBStateBaseline) },
        { "writer.cb.state.bytes"_s, bytes(SectionKind::CBStateBaseline) },
        { "writer.cb.summary.stream"_s, stream(SectionKind::CBSummaryBaseline) },
        { "writer.cb.summary.bytes"_s, bytes(SectionKind::CBSummaryBaseline) },
        { "writer.ICsBaseline.stream"_s, stream(SectionKind::ICsBaseline) },
        { "writer.ICsBaseline.bytes"_s, bytes(SectionKind::ICsBaseline) },
        { "writer.reread"_s, trace.commit ? number(trace.commit->timing.rereadNanoseconds) : BenchValue { nullptr } },
        { "writer.publish"_s, trace.commit ? number(trace.commit->timing.publishNanoseconds) : BenchValue { nullptr } },
        { "charges.budget"_s, commit(&CaptureTrace::CommitTrace::budgetChargedBytes) },
        { "charges.keptSummaries"_s, commit(&CaptureTrace::CommitTrace::keptSummariesBytes) },
        { "charges.indexEntry"_s, commit(&CaptureTrace::CommitTrace::indexEntryBytes) },
    });
}

// The compile event, the native cost the bench compares an import's installation with: the key the parent-key registry
// holds for the CB's UCB, or null, whether or not the code carries an image record; the compilation's thread CPU time
// after its profile drain; the finalization's without relinking incoming calls; the code's size, its executable
// allocation as the native code-size sample counts it; and whether per-VM support was generated during either span,
// which sets the body aside in that comparison.
static void recordCompileEvent(VMState& state, CodeBlock& codeBlock, const BaselineCompileTiming& timing)
{
    BenchReport* report = state.benchReport();
    if (!report)
        return;
    using BenchValue = decltype(BenchField::value);
    std::optional<BodyKey> key = state.registry().keyOf(*codeBlock.unlinkedCodeBlock());
    RefPtr<JITCode> code = codeBlock.jitCode();
    report->record("compile"_s, {
        { "key"_s, key ? BenchValue { bodyKeyHex(*key) } : BenchValue { nullptr } },
        { "compileNanoseconds"_s, timing.compileNanoseconds },
        { "finalizeNanoseconds"_s, timing.finalizeNanoseconds },
        { "codeSize"_s, static_cast<uint64_t>(code ? code->size() : 0) },
        { "supportGenerated"_s, timing.supportGenerated },
    });
}

// Adds a key's first kept entry, whose charge (newEntryCharge) the caller made and the entry now holds. The capture glue
// creates the kept summaries at the first kept entry, with a deleter of its own, so neither JITCacheVMState.h nor the
// state's destructor names this file's types.
static void addKeptEntry(VMState& state, ProducerBudget& budget, const BodyKey& key, const SavedScore& saved, size_t chargedBytes)
{
    KeptSummaries* kept = state.keptSummaries();
    if (!kept) {
        VMState::KeptSummariesHolder::deleter_type deleter = [](KeptSummaries* summaries) {
            delete summaries;
        };
        state.setKeptSummaries(VMState::KeptSummariesHolder { new KeptSummaries(budget), deleter });
        kept = state.keptSummaries();
    }
    bool isNewEntry = kept->scores.add(key, saved).isNewEntry;
    ASSERT_UNUSED(isNewEntry, isNewEntry);
    kept->chargedBytes += chargedBytes;
}

static size_t keptEntryCharge(VMState& state)
{
    KeptSummaries* kept = state.keptSummaries();
    return newEntryCharge(KeptSummaries::bucketBytes, kept && kept->chargedBytes);
}

// A saved body's score at one scoring.
struct SavedScoring {
    enum class Outcome : uint8_t { Scored, Absent, Deferred, Faulted };
    Outcome outcome;
    std::optional<SavedScore> saved; // Scored only
    std::optional<uint64_t> readNanoseconds; // when the scoring read ran
};

// The key's kept entry when it has one. Otherwise the store's scoring read opens the body by its name, whatever the
// index holds, in Full mode exactly when strict is on, and tells four cases apart: no body (Absent), a transient error
// that defers the key's capture (Unavailable), invalid material (Invalid), and a body whose summaries score it (Found),
// which is kept. An unreadable body never scores as absent, since any candidate beats an absent body.
static SavedScoring scoreSavedBody(VMState& state, ProducerBudget& budget, const BodyKey& key, const CaptureClock& clock)
{
    using Outcome = SavedScoring::Outcome;
    if (KeptSummaries* kept = state.keptSummaries()) {
        auto iterator = kept->scores.find(key);
        if (iterator != kept->scores.end())
            return { Outcome::Scored, iterator->value, std::nullopt };
    }

    OpenedArtifact* artifact = state.artifact();
    ASSERT(artifact);
    bool strict = state.strict();
    uint64_t start = clock.now();
    SavedSummaryRead read = artifact->readSavedSummaries(key, strict ? ValidationMode::Full : ValidationMode::Integrity);
    switch (read.outcome) {
    case StoreOutcome::Absent:
        return { Outcome::Absent, std::nullopt, clock.now() - start };
    case StoreOutcome::Unavailable:
        ++state.progress().capturesDeferred;
        ++state.progress().transientOpenFailures;
        return { Outcome::Deferred, std::nullopt, clock.now() - start };
    case StoreOutcome::Invalid: {
        String detail = read.failure.error ? makeString(String::fromUTF8(safeStrerror(read.failure.error).data()), "; "_s, bodyDetail(key)) : bodyDetail(key);
        state.raiseInvalidMaterial(read.failure.check, WTF::move(detail));
        return { Outcome::Faulted, std::nullopt, std::nullopt };
    }
    case StoreOutcome::Found:
        break;
    }

    auto score = scoreSections(read.summaries->highestTier(), read.summaries->ucbFeedback(), read.summaries->cbSummary(), read.summaries->ics(), strict);
    SavedScore saved { score ? *score : CaptureScore { }, read.summaries->version() };
    // The mapping goes as soon as the summaries are read.
    read.summaries = nullptr;
    uint64_t readNanoseconds = clock.now() - start;
    if (!score) {
        state.raiseInvalidMaterial(score.error().part, score.error().check, makeString("saved "_s, bodyDetail(key)));
        return { Outcome::Faulted, std::nullopt, std::nullopt };
    }

    ScopedCharge entryCharge(budget);
    if (!entryCharge.tryCharge(keptEntryCharge(state))) {
        raiseBudgetLimit(state, makeString("a kept summary; "_s, bodyDetail(key)));
        return { Outcome::Faulted, std::nullopt, std::nullopt };
    }
    addKeptEntry(state, budget, key, saved, entryCharge.take());
    return { Outcome::Scored, saved, readNanoseconds };
}

// The ordered list of section sources the writer streams, in section-kind order. The bytes and objects the sources name
// belong to the three captures and the ICs buffer, which outlive the commit.
static CommitSections commitSectionsFor(const UCBSections& ucb, const ImageCapture& image, const CBStateCapture& cbState, std::span<const uint8_t> ics)
{
    std::array sources {
        SectionSource::inMemory(SectionKind::UCBIdentity, ucb.identity.span()),
        SectionSource::inMemory(SectionKind::UCBCore, ucb.core->span()),
        SectionSource::inMemory(SectionKind::UCBFeedback, ucb.feedback.span()),
        SectionSource::streamed(SectionKind::ImageBaseline, image.imageSectionSize(), [](const void* object, const SectionSink& sink) {
            return static_cast<const ImageCapture*>(object)->writeImageSection(sink);
        }, &image),
        SectionSource::streamed(SectionKind::BakedFactsBaseline, image.bakedFactsSectionSize(), [](const void* object, const SectionSink& sink) {
            return static_cast<const ImageCapture*>(object)->writeBakedFactsSection(sink);
        }, &image),
#if ENABLE(JITCACHE_TWINS)
        SectionSource::streamed(SectionKind::ImageTwinsBaseline, image.twinsSectionSize(), [](const void* object, const SectionSink& sink) {
            return static_cast<const ImageCapture*>(object)->writeTwinsSection(sink);
        }, &image),
#endif
        SectionSource::inMemory(SectionKind::CBStateBaseline, cbState.stateSection()),
        SectionSource::inMemory(SectionKind::CBSummaryBaseline, cbState.summarySection()),
        SectionSource::inMemory(SectionKind::ICsBaseline, ics),
    };
    return CommitSections { std::span { sources } };
}

// A body without an image: the UCB lane's three sections alone, the sections the container requires at tier 0.
static CommitSections commitSectionsFor(const UCBSections& ucb)
{
    std::array sources {
        SectionSource::inMemory(SectionKind::UCBIdentity, ucb.identity.span()),
        SectionSource::inMemory(SectionKind::UCBCore, ucb.core->span()),
        SectionSource::inMemory(SectionKind::UCBFeedback, ucb.feedback.span()),
    };
    return CommitSections { std::span { sources } };
}

// Any candidate beats an absent body, a tier-0 capture with no richness and no carried progress included; otherwise
// beats decides.
static bool beatsSavedBody(const CaptureScore& candidate, const std::optional<CaptureScore>& saved)
{
    return !saved || beats(candidate, *saved);
}

struct CommitOutcome {
    enum class Kind : uint8_t { Committed, Beaten, NotEligible, Faulted };
    Kind kind;
    uint64_t fileSize { 0 }; // Committed only
};

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
    KeptSummaries* kept = state.keptSummaries();
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
        CaptureTrace::CommitTrace commitTrace { timing, { }, budget.chargedBytes(), state.keptSummaries()->chargedBytes, indexCharge };
        for (const SectionSource& source : sections.sources())
            commitTrace.sectionBytes[static_cast<size_t>(source.kind)] = source.size;
        trace.commit = commitTrace;
    }
    return { Kind::Committed, result->fileSize };
}

// Builds one capture whose live score beats the saved body's and commits it. Each lane's output holds its own
// charges and releases them when it goes, and the ICs buffer is freed before its charge is released, so every return
// leaves only what a commit keeps: the kept entry, the index entry's charge and the body.
static CommitOutcome commitCapture(VM& vm, VMState& state, CodeBlock& codeBlock, const BodyKey& key, const std::optional<CaptureScore>& saved,
    const CaptureClock& clock, CaptureTrace& trace)
{
    using Kind = CommitOutcome::Kind;
    ProducerBudget& budget = *state.producerBudget();
    bool strict = state.strict();
    UnlinkedCodeBlock& ucb = *codeBlock.unlinkedCodeBlock();
    CaptureTrace::BuildTimes times;

    // The image's write functions stream from the live code, which stays alive until the writer returns.
    Ref<BaselineJITCode> code { static_cast<BaselineJITCode&>(*codeBlock.jitCode()) };

    // The image is the only builder that can declare the CB ineligible, so it comes first.
    uint64_t start = clock.now();
    auto image = captureImage(vm, codeBlock, code.get(), budget, strict);
    times.captureImage = clock.now() - start;
    if (!image) {
        switch (image.error().outcome) {
        case CaptureOutcome::NotEligible:
            return { Kind::NotEligible };
        case CaptureOutcome::ChargeRefused:
            raiseBudgetLimit(state, makeString("image.capture; "_s, bodyDetail(key)));
            return { Kind::Faulted };
        case CaptureOutcome::RecordingFault:
            raiseCaptureFault(state, budget, "image"_s, description(image.error().check), bodyDetail(key));
            return { Kind::Faulted };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The UCB lane's sections and the richness of what they hold.
    start = clock.now();
    auto ucbSections = buildSections(vm, ucb, holderOf(codeBlock), budget);
    UCBRichness ucbRichness;
    if (ucbSections)
        ucbRichness = liveRichness(ucb, BodyImage::Present);
    times.buildSections = clock.now() - start;
    if (!ucbSections) {
        raiseCaptureFault(state, budget, "ucb"_s, ucbCaptureCheckName(ucbSections.error()), bodyDetail(key));
        return { Kind::Faulted };
    }

    // The ICs lane comes before the CB lane, whose capture takes its polymorphic bit for the same CB in the same pause.
    start = clock.now();
    size_t icsSize = ICs::baselineICsSectionSize(codeBlock);
    ScopedCharge icsCharge(budget);
    if (!icsCharge.tryCharge(icsSize)) {
        raiseBudgetLimit(state, makeString("the "_s, icsSize, "-byte ICsBaseline section; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }
    MallocSpan<uint8_t> icsBuffer = MallocSpan<uint8_t>::malloc(icsSize);
    auto ics = ICs::captureBaselineICs(codeBlock, icsBuffer.mutableSpan(), strict ? ICs::StrictChecks::Yes : ICs::StrictChecks::No);
    times.captureBaselineICs = clock.now() - start;
    if (!ics) {
        raiseCaptureFault(state, budget, "ics"_s, icsCaptureCheckName(ics.error().check), makeString("site "_s, ics.error().siteIndex, "; "_s, bodyDetail(key)));
        return { Kind::Faulted };
    }

    // The CB lane's state, with the ICs lane's polymorphic bit.
    trace.hasPolymorphicSite = ics->hasPolymorphicSite;
    start = clock.now();
    auto cbState = CBStateCapture::capture(codeBlock, budget, strict, ics->hasPolymorphicSite);
    times.cbStateCapture = clock.now() - start;
    if (!cbState) {
        raiseCaptureFault(state, budget, "cb"_s, description(cbState.error().check), bodyDetail(key));
        return { Kind::Faulted };
    }
    trace.build = times;

    // The score of the bytes just built, from what the builders returned, which equals what scoreSections would compute
    // from the committed file, so no lane's bytes are read back. A drain between the scoring and the build can move it, so
    // the capture stops when the build no longer wins.
    const CBScore& cbScore = cbState->score();
    CaptureScore committed { baselineTier, ucbRichness.total() + cbScore.richnessUnits, ics->summary.icSitesWithCases, cbScore.counterWithheld, cbScore.counterProgress };
    trace.committed = committed;
    trace.committedExitSiteUnits = ucbRichness.exitSiteUnits;
    if (!beatsSavedBody(committed, saved))
        return { Kind::Beaten };

    // The LLInt threshold (L) and counter progress (P) that compact orders bodies by. P is zero for a body whose counter
    // does not travel.
    CommitStamp stamp { key, envelopeLLIntThreshold(ucb), cbScore.counterProgress, baselineTier };

    return commitBody(state, budget, stamp, commitSectionsFor(*ucbSections, *image, *cbState, icsBuffer.span()), committed, clock, trace);
}

// commitCapture for a body that ran without an image, which delta's phase 3 finds: the UCB lane's sections alone, built
// from the UCB and the holder its record keeps, at tier 0. Its L is the LLInt progress it carries and its P zero, so
// compact orders it by that progress.
static CommitOutcome commitImagelessCapture(VM& vm, VMState& state, UnlinkedCodeBlock& ucb, const UnlinkedFunctionExecutable* holder, const BodyKey& key,
    const std::optional<CaptureScore>& saved, const CaptureClock& clock, CaptureTrace& trace)
{
    using Kind = CommitOutcome::Kind;
    ProducerBudget& budget = *state.producerBudget();
    CaptureTrace::BuildTimes times;

    // The UCB lane's sections; there is no image, ICs or CB state to build.
    uint64_t start = clock.now();
    auto ucbSections = buildSections(vm, ucb, holder, budget);
    times.buildSections = clock.now() - start;
    if (!ucbSections) {
        raiseCaptureFault(state, budget, "ucb"_s, ucbCaptureCheckName(ucbSections.error()), bodyDetail(key));
        return { Kind::Faulted };
    }
    trace.build = times;

    // The score of the bytes just built. Markers merge into the UCB copies of the value predictions without a lock, so the
    // score is read from the feedback section the builder wrote, the lane's summary in the file, rather than from the UCB
    // again.
    auto feedback = FeedbackSection::parse(ucbSections->feedback.span(), false);
    RELEASE_ASSERT(feedback);
    UCBRichness richness = savedRichness(*feedback, BodyImage::Absent);
    uint32_t progress = carriedLLIntPoints(feedback->llintActiveThreshold(), feedback->llintProgress());
    CaptureScore committed { imagelessTier, richness.total(), 0, false, progress };
    trace.committed = committed;
    trace.committedExitSiteUnits = richness.exitSiteUnits;
    if (!beatsSavedBody(committed, saved))
        return { Kind::Beaten };

    CommitStamp stamp { key, progress, 0, imagelessTier };

    return commitBody(state, budget, stamp, commitSectionsFor(*ucbSections), committed, clock, trace);
}

static ASCIILiteral eventOutcome(CommitOutcome::Kind kind)
{
    return kind == CommitOutcome::Kind::Committed ? "committed"_s : "beaten"_s;
}

// A candidate of phase 1: an eligible CodeBlock.
struct CodeBlockCandidate {
    CodeBlock* codeBlock;
    LiveScore live;
};

// A candidate of phase 3: a recorded UCB whose body ran without an eligible CodeBlock, with the holder
// its record keeps for function code.
struct ImagelessCandidate {
    UnlinkedCodeBlock* codeBlock;
    const UnlinkedFunctionExecutable* holder;
    LiveScore live;
};

// The candidates of phase 1 or phase 3: each key's candidates in decreasing order under beats, a tie after the ones met
// earlier. The table is charged per key as a production table's entry. A key's vector has one inline slot, which the
// entry's buckets hold, and grows only explicitly: the whole new heap capacity is charged before the growth and the old
// heap capacity released after it. Destroying the table frees it and then releases every charge, since the table lives
// only inside one delta and is no production memory.
template<typename Candidate>
class DeltaCandidates {
    WTF_MAKE_NONCOPYABLE(DeltaCandidates);
public:
    static constexpr size_t inlineCandidates = 1;
    using Candidates = Vector<Candidate, inlineCandidates>;
    using Table = HashMap<BodyKey, Candidates, BodyKeyHash, BodyKeyHashTraits>;
    static constexpr size_t bucketBytes = sizeof(typename Table::KeyValuePairType);

    explicit DeltaCandidates(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~DeltaCandidates()
    {
        m_table.clear();
        if (m_chargedBytes)
            m_budget.release(m_chargedBytes);
    }

    // False when a charge was refused, with nothing added.
    [[nodiscard]] bool add(const BodyKey& key, const Candidate& candidate)
    {
        auto iterator = m_table.find(key);
        if (iterator == m_table.end()) {
            size_t charge = newEntryCharge(bucketBytes, !!m_chargedBytes);
            if (!m_budget.tryCharge(charge))
                return false;
            m_chargedBytes += charge;
            m_table.add(key, Candidates { }).iterator->value.append(candidate);
            return true;
        }

        Candidates& candidates = iterator->value;
        if (candidates.size() == candidates.capacity()) {
            size_t oldCapacity = candidates.capacity();
            CheckedSize newHeapBytes = oldCapacity;
            newHeapBytes *= 2u;
            newHeapBytes *= sizeof(Candidate);
            if (newHeapBytes.hasOverflowed() || !m_budget.tryCharge(newHeapBytes.value()))
                return false;
            candidates.reserveCapacity(oldCapacity * 2);
            // The first growth out of the inline slot releases nothing: the entry's bucket charge covers that slot.
            size_t oldHeapBytes = oldCapacity > inlineCandidates ? oldCapacity * sizeof(Candidate) : 0;
            if (oldHeapBytes)
                m_budget.release(oldHeapBytes);
            m_chargedBytes += newHeapBytes.value() - oldHeapBytes;
        }
        size_t position = candidates.size();
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (beats(candidate.live.score, candidates[index].live.score)) {
                position = index;
                break;
            }
        }
        candidates.insert(position, candidate);
        return true;
    }

    const Table& table() const { return m_table; }
    // The table only grows, so its charge once its phase's walk ends is its largest, which the delta event reports.
    size_t chargedBytes() const { return m_chargedBytes; }

private:
    ProducerBudget& m_budget;
    Table m_table;
    size_t m_chargedBytes { 0 };
};

struct DeltaCounts {
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 };
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 };
    uint64_t phase1Nanoseconds { 0 };
    uint64_t phase2Nanoseconds { 0 };
    uint64_t phase3Nanoseconds { 0 };
    // What delta's duration and memory scale with: the CodeBlocks phase 1 walked, the candidates among them, and the
    // candidate table's charge.
    uint64_t codeBlocks { 0 };
    uint64_t candidates { 0 };
    uint64_t candidateTableBytes { 0 };
    // Phase 3's: the recorded UCBs it walked, those whose body never ran, the candidates among them, and the charges of
    // the registry's copy and of the candidate table.
    uint64_t recordedCodeBlocks { 0 };
    uint64_t neverRan { 0 };
    uint64_t imagelessCandidates { 0 };
    uint64_t registryCopyBytes { 0 };
    uint64_t imagelessTableBytes { 0 };
    bool faulted { false };
};

// Phase 2 and phase 3 commit a key's candidates the same way: score the saved body once, then take the candidates in
// order until one commits or does not beat it. A candidate whose capture is NotEligible is passed over, as an ineligible
// CodeBlock is. Returns false after a fault.
template<typename Candidate, typename Commit>
static bool commitKey(VMState& state, ProducerBudget& budget, const BodyKey& key, const Vector<Candidate, 1>& candidates,
    const CaptureClock& clock, DeltaCounts& counts, const Commit& commit)
{
    SavedScoring saved = scoreSavedBody(state, budget, key, clock);
    if (saved.outcome == SavedScoring::Outcome::Faulted)
        return false;
    if (saved.outcome == SavedScoring::Outcome::Deferred) {
        ++counts.deferredKeys;
        const auto& first = candidates.first();
        CaptureTrace trace { "delta"_s, key, first.live, std::nullopt, saved.readNanoseconds, std::nullopt, 0, first.live.hasPolymorphicSite, std::nullopt, std::nullopt };
        recordCaptureEvent(clock.report(), trace, "deferred"_s);
        return true;
    }

    std::optional<CaptureScore> scoredSaved = saved.saved ? std::optional { saved.saved->score } : std::nullopt;
    for (auto& candidate : candidates) {
        CaptureTrace trace { "delta"_s, key, candidate.live, scoredSaved, saved.readNanoseconds, std::nullopt, 0, candidate.live.hasPolymorphicSite, std::nullopt, std::nullopt };
        if (!beatsSavedBody(candidate.live.score, scoredSaved)) {
            recordCaptureEvent(clock.report(), trace, "beaten"_s);
            return true;
        }
        CommitOutcome outcome = commit(candidate, scoredSaved, trace);
        if (outcome.kind == CommitOutcome::Kind::NotEligible)
            continue;
        if (outcome.kind == CommitOutcome::Kind::Faulted)
            return false;
        if (outcome.kind == CommitOutcome::Kind::Committed) {
            ++counts.committedBodies;
            counts.committedBytes += outcome.fileSize;
        }
        recordCaptureEvent(clock.report(), trace, eventOutcome(outcome.kind));
        return true;
    }
    return true;
}

// Whether phase 1 found an eligible CodeBlock of the key. A CodeBlock whose image phase 2 found unrecordable is no longer
// eligible, since its code is no longer image-capturable, so a key whose every candidate turned out so is captured
// without an image.
static bool hasEligibleCodeBlock(const DeltaCandidates<CodeBlockCandidate>& compiled, const BodyKey& key)
{
    auto iterator = compiled.table().find(key);
    if (iterator == compiled.table().end())
        return false;
    for (auto& candidate : iterator->value) {
        RefPtr<JITCode> code = candidate.codeBlock->jitCode();
        if (code && isImageCapturable(static_cast<const BaselineJITCode&>(*code)))
            return true;
    }
    return false;
}

// Delta's phase 3, with the delta capture All: the bodies that ran without an eligible CodeBlock, reached through the UCB
// registry, so that bodies whose CodeBlocks died are reached too, and captured without an image. The registry's copy and
// the candidate table live only inside this call.
static void runImagelessPhase(VM& vm, VMState& state, ProducerBudget& budget, const DeltaCandidates<CodeBlockCandidate>& compiled,
    const CaptureClock& clock, DeltaCounts& counts)
{
    uint64_t start = clock.now();
    DeltaCandidates<ImagelessCandidate> candidates(budget);
    {
        // The registry copies under its leaf lock, and each cell is tested afterwards, outside it. Nothing between the
        // count and the copy can add or remove a record (UCBRegistry::recordedCodeBlockCount).
        UCBRegistry& registry = state.registry();
        size_t count = registry.recordedCodeBlockCount();
        CheckedSize copyBytes = count;
        copyBytes *= sizeof(UCBRegistry::RecordedCodeBlock);
        ScopedCharge copyCharge(budget);
        if (copyBytes.hasOverflowed() || !copyCharge.tryCharge(copyBytes.value())) {
            raiseBudgetLimit(state, makeString("delta's copy of "_s, count, " UCB records"_s));
            counts.faulted = true;
            return;
        }
        counts.registryCopyBytes = copyBytes.value();
        Vector<UCBRegistry::RecordedCodeBlock> recorded(count);
        recorded.shrink(registry.copyRecordedCodeBlocks(recorded.mutableSpan()));
        counts.recordedCodeBlocks = recorded.size();

        for (auto& entry : recorded) {
            UnlinkedCodeBlock& ucb = *entry.codeBlock;
            // A UCB the last collection found dead is not swept yet, and its constants and child UFEs may name cells already
            // swept. A live one keeps them, and nothing in delta can complete a collection.
            if (ucb.isPendingDestruction())
                continue;
            // A UCB proves only a request, such as a module's link or a CachedCall callee's preparation.
            if (!llintCounterShowsExecution(ucb)) {
                ++counts.neverRan;
                continue;
            }
            auto record = captureRecord(vm, ucb);
            if (!record || hasEligibleCodeBlock(compiled, record->key))
                continue;
            // Function code is encoded through its holder, which the record keeps while the UFE lives; one that is dead but
            // not swept yet is skipped with its body.
            UnlinkedFunctionExecutable* holder = entry.holder;
            if (coreKindFor(record->key.identityKind()) == UnlinkedCodeBlockCoreKind::Function && (!holder || holder->isPendingDestruction()))
                continue;
            ++state.progress().captureCandidates;
            ++counts.imagelessCandidates;
            if (!candidates.add(record->key, ImagelessCandidate { &ucb, holder, scoreImagelessCandidate(ucb, clock) })) {
                raiseBudgetLimit(state, makeString("delta's table of bodies without an image; "_s, bodyDetail(record->key)));
                counts.faulted = true;
                return;
            }
        }
    }
    counts.imagelessTableBytes = candidates.chargedBytes();
    counts.eligibleKeys += candidates.table().size();

    // Commits are independent, so their order is the table's.
    for (auto& entry : candidates.table()) {
        bool committedOrBeaten = commitKey(state, budget, entry.key, entry.value, clock, counts, [&](const ImagelessCandidate& candidate, const std::optional<CaptureScore>& saved, CaptureTrace& trace) {
            return commitImagelessCapture(vm, state, *candidate.codeBlock, candidate.holder, entry.key, saved, clock, trace);
        });
        if (!committedOrBeaten) {
            counts.faulted = true;
            break;
        }
    }
    counts.phase3Nanoseconds = clock.now() - start;
}

// Delta's three phases: phase 1 scores every eligible CodeBlock into a candidate table, phase 2 commits each key's
// candidates, and phase 3, with the delta capture All, captures without an image the bodies that ran without an eligible
// CodeBlock. The candidate tables live only inside this call.
static DeltaCounts runDeltaPhases(VM& vm, VMState& state, ProducerBudget& budget, const CaptureClock& clock)
{
    DeltaCounts counts;
    DeltaCandidates<CodeBlockCandidate> candidates(budget);
    bool strict = state.strict();

    // Phase 1, under the CodeBlock set's lock, which forEachCodeBlockIgnoringJITPlans requires and which completes no
    // pending plan. The functor allocates no cell and does no I/O; a fault it meets ends the walk and is raised once
    // the lock is released.
    struct PendingFault {
        bool refusedCharge; // the table's charge, raised as budget.limit; otherwise the CB lane's strict guard
        ASCIILiteral check;
        String detail;
    };
    std::optional<PendingFault> pendingFault;
    uint64_t start = clock.now();
    {
        Locker locker { vm.heap.codeBlockSet().getLock() };
        vm.heap.forEachCodeBlockIgnoringJITPlans(locker, [&](CodeBlock* codeBlock) {
            if (pendingFault)
                return;
            ++counts.codeBlocks;
            auto record = candidateRecord(vm, *codeBlock);
            if (!record)
                return;
            ++state.progress().captureCandidates;
            ++counts.candidates;
            auto live = scoreLiveCandidate(*codeBlock, strict, clock);
            if (!live) {
                pendingFault = PendingFault { false, description(live.error().check), bodyDetail(record->key) };
                return;
            }
            if (!candidates.add(record->key, CodeBlockCandidate { codeBlock, *live }))
                pendingFault = PendingFault { true, budgetLimit, makeString("delta's candidate table; "_s, bodyDetail(record->key)) };
        });
    }
    counts.phase1Nanoseconds = clock.now() - start;
    counts.candidateTableBytes = candidates.chargedBytes();
    if (pendingFault) {
        if (pendingFault->refusedCharge)
            raiseBudgetLimit(state, WTF::move(pendingFault->detail));
        else
            raiseCaptureFault(state, budget, "cb"_s, pendingFault->check, WTF::move(pendingFault->detail));
        counts.faulted = true;
        return counts;
    }
    counts.eligibleKeys = candidates.table().size();

    // Phase 2, without the lock. The CodeBlock pointers stay valid: each was in the CodeBlock set, and nothing between the
    // phases can complete a collection, since the VM thread holds heap access, reaches no stop point and allocates no cell.
    // Commits are independent, so their order is the table's. A NotEligible image makes that CB an ineligible one, so the
    // key's next candidate gets the same test, until one commits or does not beat the saved body.
    start = clock.now();
    for (auto& entry : candidates.table()) {
        bool committedOrBeaten = commitKey(state, budget, entry.key, entry.value, clock, counts, [&](const CodeBlockCandidate& candidate, const std::optional<CaptureScore>& saved, CaptureTrace& trace) {
            return commitCapture(vm, state, *candidate.codeBlock, entry.key, saved, clock, trace);
        });
        if (!committedOrBeaten) {
            counts.faulted = true;
            break;
        }
    }
    counts.phase2Nanoseconds = clock.now() - start;

    // Phase 3, with the delta capture All, after every key of phase 2 has been decided, so that only a key whose every
    // CodeBlock turned out ineligible can still meet a body without an image.
    if (!counts.faulted && state.deltaCapture() == DeltaCapture::All)
        runImagelessPhase(vm, state, budget, candidates, clock, counts);
    return counts;
}

} // namespace JITCacheCaptureInternal

std::expected<CaptureScore, SummaryRejection> scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics, bool strict)
{
    using namespace JITCacheCaptureInternal;
    // With strict off no reader rejects. With it on, the UCB lane's parse fails without naming a check, so the glue names
    // the rejection.
    auto feedback = FeedbackSection::parse(ucbFeedback, strict);
    if (!feedback)
        return std::unexpected(SummaryRejection { "ucb"_s, "saved-summary"_s });
    // A body without an image holds no CB or ICs summary: the UCB lane's summary is its whole score, its UCB copies
    // included, with the LLInt progress it carries in place of the baseline counter's.
    if (tier == imagelessTier) {
        UCBRichness richness = savedRichness(*feedback, BodyImage::Absent);
        return CaptureScore { imagelessTier, richness.total(), 0, false, carriedLLIntPoints(feedback->llintActiveThreshold(), feedback->llintProgress()) };
    }
    UCBRichness ucbRichness = savedRichness(*feedback, BodyImage::Present);
    auto cbScore = decodeSummary(cbSummary, strict);
    if (!cbScore)
        return std::unexpected(SummaryRejection { "cb"_s, description(cbScore.error().check) });
    auto icsSummary = ICs::readBaselineICsSummary(ics, strict ? ICs::StrictChecks::Yes : ICs::StrictChecks::No);
    if (!icsSummary)
        return std::unexpected(SummaryRejection { "ics"_s, icsCheckName(icsSummary.error().check) });
    return CaptureScore { tier, ucbRichness.total() + cbScore->richnessUnits, icsSummary->icSitesWithCases, cbScore->counterWithheld, cbScore->counterProgress };
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
    using namespace JITCacheCaptureInternal;
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
    auto live = scoreLiveCandidate(codeBlock, state->strict(), clock);
    if (!live) {
        raiseCaptureFault(*state, budget, "cb"_s, description(live.error().check), bodyDetail(record->key));
        return;
    }
    SavedScoring saved = scoreSavedBody(*state, budget, record->key, clock);
    if (saved.outcome == SavedScoring::Outcome::Faulted)
        return;
    std::optional<CaptureScore> scoredSaved = saved.saved ? std::optional { saved.saved->score } : std::nullopt;
    CaptureTrace trace { "finalize"_s, record->key, *live, scoredSaved, saved.readNanoseconds, std::nullopt, 0, live->hasPolymorphicSite, std::nullopt, std::nullopt };
    if (saved.outcome == SavedScoring::Outcome::Deferred) {
        recordCaptureEvent(clock.report(), trace, "deferred"_s);
        return;
    }
    if (!beatsSavedBody(live->score, scoredSaved)) {
        recordCaptureEvent(clock.report(), trace, "beaten"_s);
        return;
    }

    // A NotEligible image ends this CB's capture without a fault, as for an ineligible CB.
    CommitOutcome outcome = commitCapture(vm, *state, codeBlock, record->key, scoredSaved, clock, trace);
    if (outcome.kind == CommitOutcome::Kind::Committed || outcome.kind == CommitOutcome::Kind::Beaten)
        recordCaptureEvent(clock.report(), trace, eventOutcome(outcome.kind));
}

DeltaResult delta(VM& vm)
{
    using namespace JITCacheCaptureInternal;

    // What delta requires of its caller: a configured producing VM, the API lock with heap access, and no collector work
    // on this thread. Each failure is rejected without work.
    VMState* state = vm.jitCacheState();
    if (!state)
        return { DeltaOutcome::Rejected, "delta.unconfigured"_s, std::nullopt };
    if (!state->producing())
        return { DeltaOutcome::Rejected, "delta.role"_s, std::nullopt };
    if (!vm.currentThreadIsHoldingAPILock() || !vm.heap.hasHeapAccess())
        return { DeltaOutcome::Rejected, "delta.locks"_s, std::nullopt };
    if (vm.heap.currentThreadIsDoingGCWork())
        return { DeltaOutcome::Rejected, "delta.collector"_s, std::nullopt };

    CaptureInProgressScope captureInProgress(*state);
    // Freeing what an ended production held is no work in the sense above.
    state->releaseEndedProductionMemory();
    auto faulted = [&](const DeltaCounts& counts) {
        std::optional<FaultReport> fault = state->productionFault();
        if (!fault)
            fault = state->firstFault();
        return DeltaResult { DeltaOutcome::Faulted, { }, WTF::move(fault), counts.eligibleKeys, counts.committedBodies, counts.committedBytes, counts.deferredKeys };
    };
    if (!state->productionActive())
        return faulted({ });

    // No collection completes during delta, so the CodeBlocks and UCBs its phases hold stay valid.
    DeferGCForAWhile deferGC(vm);
    BenchReport* report = state->benchReport();
    auto finish = [&](const DeltaCounts& counts) {
        // The delta event, and a flush after each delta.
        if (!report)
            return;
        report->record("delta"_s, {
            { "outcome"_s, String { counts.faulted ? "faulted"_s : "completed"_s } },
            { "phase1Nanoseconds"_s, counts.phase1Nanoseconds },
            { "phase2Nanoseconds"_s, counts.phase2Nanoseconds },
            { "phase3Nanoseconds"_s, counts.phase3Nanoseconds },
            { "codeBlocks"_s, counts.codeBlocks },
            { "candidates"_s, counts.candidates },
            { "candidateTableBytes"_s, counts.candidateTableBytes },
            { "recordedCodeBlocks"_s, counts.recordedCodeBlocks },
            { "neverRan"_s, counts.neverRan },
            { "imagelessCandidates"_s, counts.imagelessCandidates },
            { "registryCopyBytes"_s, counts.registryCopyBytes },
            { "imagelessTableBytes"_s, counts.imagelessTableBytes },
            { "eligibleKeys"_s, counts.eligibleKeys },
            { "committedBodies"_s, counts.committedBodies },
            { "committedBytes"_s, counts.committedBytes },
            { "deferredKeys"_s, counts.deferredKeys },
        });
        report->flush();
    };

    // A refusal made on a JIT worker only marks the budget there; it reaches the VM thread here.
    ProducerBudget& budget = *state->producerBudget();
    if (budget.hasRefused()) {
        raiseBudgetLimit(*state, "a charge refused before delta"_s);
        DeltaCounts counts;
        counts.faulted = true;
        finish(counts);
        return faulted(counts);
    }

    // The three phases free their candidate tables and release the charges before they return.
    DeltaCounts counts = runDeltaPhases(vm, *state, budget, CaptureClock(report));

    // After its commits, the delta compacts the pack whenever it holds dead bytes. A body that fails its checks on
    // the way is invalid material and any other failure a recording fault; the bodies committed above stay valid either
    // way.
    if (!counts.faulted && state->productionActive()) {
        // The pack as the delta's commits left it, and what the compaction made of it, for the bench's pack event.
        IndexStatistics before = report && state->artifact() ? state->artifact()->statistics() : IndexStatistics { };
        auto compacted = state->writer()->compactIfNeeded(!!report);
        if (!compacted) {
            counts.faulted = true;
            if (compacted.error().kind == CompactionFailure::Kind::Damaged)
                state->raiseInvalidMaterial(compacted.error().check, WTF::move(compacted.error().detail));
            else
                state->raiseRecordingFault({ }, compacted.error().check, WTF::move(compacted.error().detail));
        } else if (report) {
            const CompactionResult* result = *compacted ? &**compacted : nullptr;
            report->record("pack"_s, { { "bodies"_s, before.bodies }, { "bytes"_s, before.packBytes }, { "liveBytes"_s, before.liveBytes },
                { "deadBytes"_s, before.packBytes - std::min(before.packBytes, before.liveBytes) }, { "compacted"_s, static_cast<uint64_t>(!!result) },
                { "compactedBytes"_s, result ? result->packBytes : 0 }, { "compactionNanoseconds"_s, result ? result->nanoseconds : 0 } });
        }
    }
    finish(counts);
    if (counts.faulted)
        return faulted(counts);

    // Only a delta that completes counts as a run.
    ++state->progress().deltaRuns;
    return { DeltaOutcome::Completed, { }, std::nullopt, counts.eligibleKeys, counts.committedBodies, counts.committedBytes, counts.deferredKeys };
}

#if ENABLE(JITCACHE_TWINS)

std::expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM& vm, const BodyKey& key, SectionKind kind, uint64_t offset, std::span<const uint8_t> bytes)
{
    // The rewrite uses the producing VM's writer and lock while production is active; otherwise it writes nothing, as a
    // failure at writer.rewrite does.
    VMState* state = vm.jitCacheState();
    if (!state || !state->productionActive())
        return std::unexpected(CommitFailure { WriterChecks::rewrite, "production is not active"_s });
    ArtifactWriter* writer = state->writer();
    ASSERT(writer);
    auto result = writer->rewriteSection(key, kind, offset, bytes);
    if (!result) {
        // A failure at writer.rewrite leaves the artifact and production as they were; any other is raised as a commit's.
        if (result.error().check != WriterChecks::rewrite)
            state->raiseRecordingFault({ }, result.error().check, result.error().detail);
        return result;
    }
    // The body no longer matches its kept summary, so the key's next scoring reads it again.
    if (KeptSummaries* kept = state->keptSummaries())
        kept->erase(key);
    return result;
}

std::optional<SavedScore> keptScoreForTesting(VM& vm, const BodyKey& key)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return std::nullopt;
    KeptSummaries* kept = state->keptSummaries();
    if (!kept)
        return std::nullopt;
    auto iterator = kept->scores.find(key);
    if (iterator == kept->scores.end())
        return std::nullopt;
    return iterator->value;
}

#endif // ENABLE(JITCACHE_TWINS)

#else // !ENABLE(JIT)

// A build without the JIT configures no VM: start rejects at start.platform before anything else, so nothing here has a
// state to capture for.

std::expected<CaptureScore, SummaryRejection> scoreSections(uint8_t, std::span<const uint8_t>, std::span<const uint8_t>, std::span<const uint8_t>, bool)
{
    RELEASE_ASSERT_NOT_REACHED();
    return std::unexpected(SummaryRejection { });
}

void didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*)
{
}

DeltaResult delta(VM& vm)
{
    ASSERT_UNUSED(vm, !vm.jitCacheState());
    return { DeltaOutcome::Rejected, "delta.unconfigured"_s, std::nullopt };
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
