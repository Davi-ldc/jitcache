#pragma once

#include "jitcache/base/BodyKey.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/ValidatedBody.h"
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>

// How a capture scores against the saved body it would replace, and the twins builds' hooks into the commit path, which
// the harness's shell functions and the C++ tests call. JITCacheCapture.cpp defines the scores, and KeptSummaries.cpp
// defines the hooks, beside the kept summaries they read and erase.

namespace JSC {

class CodeBlock;
class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The tiers a body's envelope names: a baseline capture is tier 1, and a capture without an image, which holds the UCB
// lane's sections alone, a module's analysis included, is tier 0, below every capture with one.
constexpr uint8_t imagelessTier = 0;
constexpr uint8_t baselineTier = 1;

// Capture scores. A live candidate's score is tier 1, the UCB lane's liveRichness total plus the CB lane's scoreLive
// richness units, the ICs lane's IC sites with cases, and scoreLive's counterWithheld and counter progress, with the ICs
// lane called before the CB lane for the same CB in the same pause. A body that ran without an image scores tier 0, the
// UCB lane's liveRichness, and its carried LLInt progress. A score about to be committed comes from what the builders
// returned for the bytes they wrote, or, for a capture without an image, is the live score it was ranked with, which reads
// state nothing changes during a capture, so it describes exactly what the body file holds.
struct CaptureScore {
    uint8_t tier { 0 }; // imagelessTier or baselineTier
    uint64_t richness { 0 }; // the UCB lane's units plus the CB lane's units
    uint32_t icSitesWithCases { 0 };
    bool counterWithheld { false }; // the CB lane withheld the counter because a property IC is polymorphic
    uint32_t counterProgress { 0 }; // tier 0: the LLInt progress the body carries (carriedLLIntPoints)
};
struct SavedScore {
    CaptureScore score;
    uint64_t version { 0 }; // the body file's commit identifier
};

// Compares the fields lexicographically in declaration order: the higher tier wins, then the richer capture, then more IC
// sites with cases, then a withheld counter over one that travels, then more counter progress. A candidate wins only when
// it is strictly greater; a remaining tie keeps the saved body.
bool beats(const CaptureScore& candidate, const CaptureScore& saved);

// A saved body's score, read from its three summary sections by the lanes' readers.
CaptureScore scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback, std::span<const uint8_t> cbSummary,
    std::span<const uint8_t> ics);

// The sections scoreSections reads, which the scoring read of a saved body checks.
inline constexpr std::array<SectionKind, 3> scoredSectionKinds { SectionKind::UCBFeedback, SectionKind::CBSummaryBaseline, SectionKind::ICsBaseline };

// Any candidate beats an absent body, a tier-0 capture with no richness and no carried progress included; otherwise
// beats decides.
inline bool beatsSavedBody(const CaptureScore& candidate, const std::optional<CaptureScore>& saved)
{
    return !saved || beats(candidate, *saved);
}

#if ENABLE(JIT)

class CaptureClock;

// A live candidate's score, with what its bench event reports beside it.
struct LiveScore {
    CaptureScore score;
    uint64_t exitSiteUnits { 0 }; // UCBRichness::exitSiteUnits, which the capture event reports apart
    bool hasPolymorphicSite { false }; // the bit the CB lane's scoreLive received
    uint64_t summarizeBaselineICsNanoseconds { 0 };
    uint64_t scoreLiveNanoseconds { 0 };
    uint64_t liveRichnessNanoseconds { 0 };
};

// An eligible CB's live score. The ICs lane comes before the CB lane, for the same CB in the same pause, and its
// polymorphic bit goes to scoreLive, so a candidate with a polymorphic site scores its counter as withheld.
LiveScore scoreLiveCandidate(CodeBlock&, const CaptureClock&);

// A body that ran without an image: tier 0, below every capture with an image, scored by the UCB lane alone, whose
// richness is its arithmetic categories and exit sites since such a body carries no UCB copies, with the LLInt progress it
// carries in place of the baseline counter's. Neither IC sites nor a withheld counter apply.
LiveScore scoreImagelessCandidate(UnlinkedCodeBlock&, const CaptureClock&);

#endif // ENABLE(JIT)

#if ENABLE(JITCACHE_TWINS)
// Requires active production; rewrites the key's body through the writer's rewriteSection and erases the key's kept
// summary. The shell's jitcacheRewriteSection calls it.
std::expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM&, const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
// The key's kept summary, for the tests that check which score a commit kept.
std::optional<SavedScore> keptScoreForTesting(VM&, const BodyKey&);
#endif

} // namespace JSC::JITCache
