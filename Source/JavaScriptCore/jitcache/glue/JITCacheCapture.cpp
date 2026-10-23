#include "config.h"
#include "jitcache/glue/JITCacheCapture.h"

#include "CodeBlock.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/glue/CaptureEvents.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ucb/UCBFeedback.h"
#include <tuple>

// The capture scores: how a capture ranks against the saved body it would replace, a saved body's score from its summary
// sections, and a live candidate's score, read from the lanes while JS is paused.

namespace JSC::JITCache {

bool beats(const CaptureScore& candidate, const CaptureScore& saved)
{
    // Tier, richness, IC sites with cases, a withheld counter above one that travels, then counter progress. Strictly
    // greater only, so a remaining tie keeps the saved body.
    return std::tie(candidate.tier, candidate.richness, candidate.icSitesWithCases, candidate.counterWithheld, candidate.counterProgress)
        > std::tie(saved.tier, saved.richness, saved.icSitesWithCases, saved.counterWithheld, saved.counterProgress);
}

#if ENABLE(JIT)

LiveScore scoreLiveCandidate(CodeBlock& codeBlock, const CaptureClock& clock)
{
    uint64_t start = clock.now();
    ICs::CaptureSummary ics = ICs::summarizeBaselineICs(codeBlock);
    uint64_t afterICs = clock.now();
    CBScore cbScore = CBStateCapture::scoreLive(codeBlock, ics.hasPolymorphicSite);
    uint64_t afterCB = clock.now();
    UCBRichness ucbRichness = liveRichness(*codeBlock.unlinkedCodeBlock());
    uint64_t end = clock.now();

    LiveScore live;
    live.score = CaptureScore { baselineTier, ucbRichness.total() + cbScore.richnessUnits, ics.summary.icSitesWithCases, cbScore.counterWithheld, cbScore.counterProgress };
    live.exitSiteUnits = ucbRichness.exitSiteUnits;
    live.hasPolymorphicSite = ics.hasPolymorphicSite;
    live.summarizeBaselineICsNanoseconds = afterICs - start;
    live.scoreLiveNanoseconds = afterCB - afterICs;
    live.liveRichnessNanoseconds = end - afterCB;
    return live;
}

LiveScore scoreImagelessCandidate(UnlinkedCodeBlock& ucb, const CaptureClock& clock)
{
    uint64_t start = clock.now();
    UCBRichness richness = liveRichness(ucb);
    BaselineExecutionCounter& counter = ucb.llintExecuteCounter();
    uint32_t progress = carriedLLIntPoints(counter.m_activeThreshold, counter.count());
    uint64_t end = clock.now();

    LiveScore live;
    live.score = CaptureScore { imagelessTier, richness.total(), 0, false, progress };
    live.exitSiteUnits = richness.exitSiteUnits;
    live.liveRichnessNanoseconds = end - start;
    return live;
}

CaptureScore scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback, std::span<const uint8_t> cbSummary,
    std::span<const uint8_t> ics)
{
    FeedbackSection feedback = FeedbackSection::parse(ucbFeedback);
    UCBRichness ucbRichness = savedRichness(feedback);
    // A body without an image holds no CB or ICs summary: the UCB lane's summary is its whole score, its arithmetic
    // categories and exit sites, with the LLInt progress it carries in place of the baseline counter's.
    if (tier == imagelessTier)
        return CaptureScore { imagelessTier, ucbRichness.total(), 0, false, carriedLLIntPoints(feedback.llintActiveThreshold(), feedback.llintProgress()) };
    CBScore cbScore = decodeSummary(cbSummary);
    return CaptureScore { tier, ucbRichness.total() + cbScore.richnessUnits, ICs::readBaselineICsSummary(ics).icSitesWithCases, cbScore.counterWithheld, cbScore.counterProgress };
}

#else // !ENABLE(JIT)

// A build without the JIT configures no VM: start rejects at start.platform before anything else, so no saved body is ever
// scored.
CaptureScore scoreSections(uint8_t, std::span<const uint8_t>, std::span<const uint8_t>, std::span<const uint8_t>)
{
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
