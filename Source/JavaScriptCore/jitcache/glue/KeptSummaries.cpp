#include "config.h"
#include "jitcache/glue/KeptSummaries.h"

#include "VM.h"
#include "jitcache/glue/CaptureEvents.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ArtifactWriter.h"
#include <wtf/SafeStrerror.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

#if ENABLE(JIT)

namespace JSC::JITCache {

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(KeptSummaries);

namespace KeptSummariesInternal {

static constexpr ASCIILiteral budgetLimit = "budget.limit"_s;

} // namespace KeptSummariesInternal

String bodyDetail(const BodyKey& key)
{
    return makeString("body "_s, bodyKeyHex(key));
}

void raiseBudgetLimit(VMState& state, String detail)
{
    state.raiseRecordingFault({ }, KeptSummariesInternal::budgetLimit, WTF::move(detail));
}

void addKeptEntry(VMState& state, ProducerBudget& budget, const BodyKey& key, const SavedScore& saved, size_t chargedBytes)
{
    KeptSummaries& kept = state.part<KeptSummaries>(budget);
    bool isNewEntry = kept.scores.add(key, saved).isNewEntry;
    ASSERT_UNUSED(isNewEntry, isNewEntry);
    kept.chargedBytes += chargedBytes;
}

size_t keptEntryCharge(VMState& state)
{
    KeptSummaries* kept = state.partIfExists<KeptSummaries>();
    return newEntryCharge(KeptSummaries::bucketBytes, kept && kept->chargedBytes);
}

SavedScoring scoreSavedBody(VMState& state, ProducerBudget& budget, const BodyKey& key, const CaptureClock& clock)
{
    using Outcome = SavedScoring::Outcome;
    if (KeptSummaries* kept = state.partIfExists<KeptSummaries>()) {
        auto iterator = kept->scores.find(key);
        if (iterator != kept->scores.end())
            return { Outcome::Scored, iterator->value, std::nullopt };
    }

    OpenedArtifact* artifact = state.artifact();
    ASSERT(artifact);
    uint64_t start = clock.now();
    SavedSummaryRead read = artifact->readSavedSummaries(key, scoredSectionKinds);
    switch (read.outcome) {
    case StoreOutcome::Absent:
        return { Outcome::Absent, std::nullopt, clock.now() - start };
    case StoreOutcome::Unavailable:
        ++state.progress().capturesDeferred;
        ++state.progress().transientOpenFailures;
        return { Outcome::Deferred, std::nullopt, clock.now() - start };
    case StoreOutcome::Invalid: {
        String detail = read.failure.error ? makeString(String::fromUTF8(safeStrerror(read.failure.error).span()), "; "_s, bodyDetail(key)) : bodyDetail(key);
        state.raiseInvalidMaterial(read.failure.check, WTF::move(detail));
        return { Outcome::Faulted, std::nullopt, std::nullopt };
    }
    case StoreOutcome::Found:
        break;
    }

    SavedScore saved { scoreSections(read.summaries->highestTier(), read.summaries->section(SectionKind::UCBFeedback), read.summaries->section(SectionKind::CBSummaryBaseline), read.summaries->section(SectionKind::ICsBaseline)), read.summaries->version() };
    // The mapping goes as soon as the summaries are read.
    read.summaries = nullptr;
    uint64_t readNanoseconds = clock.now() - start;

    ScopedCharge entryCharge(budget);
    if (!entryCharge.tryCharge(keptEntryCharge(state))) {
        raiseBudgetLimit(state, makeString("a kept summary; "_s, bodyDetail(key)));
        return { Outcome::Faulted, std::nullopt, std::nullopt };
    }
    addKeptEntry(state, budget, key, saved, entryCharge.take());
    return { Outcome::Scored, saved, readNanoseconds };
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
    if (KeptSummaries* kept = state->partIfExists<KeptSummaries>())
        kept->erase(key);
    return result;
}

std::optional<SavedScore> keptScoreForTesting(VM& vm, const BodyKey& key)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return std::nullopt;
    KeptSummaries* kept = state->partIfExists<KeptSummaries>();
    if (!kept)
        return std::nullopt;
    auto iterator = kept->scores.find(key);
    if (iterator == kept->scores.end())
        return std::nullopt;
    return iterator->value;
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
