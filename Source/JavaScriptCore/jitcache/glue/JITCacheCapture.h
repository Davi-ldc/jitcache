#pragma once

#include "ArtifactWriter.h"
#include "BodyKey.h"
#include "ValidatedBody.h"
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/text/ASCIILiteral.h>

// How a capture scores against the saved body it would replace, and the twins builds' hooks into the commit path, which
// the harness's shell functions and the C++ tests call. JITCacheCapture.cpp defines them beside the commit pipeline the
// finalize capture and delta share.

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// The tiers a body's envelope names: a baseline capture is tier 1, and a capture without an image, which holds the UCB
// lane's three sections alone, is tier 0, below every capture with one.
constexpr uint8_t imagelessTier = 0;
constexpr uint8_t baselineTier = 1;

// Capture scores. A live candidate's score is tier 1, the UCB lane's liveRichness total plus the CB lane's scoreLive
// richness units, the ICs lane's IC sites with cases, and scoreLive's counterWithheld and counter progress, with the ICs
// lane called before the CB lane for the same CB in the same pause. A body that ran without an image scores tier 0, the
// UCB lane's liveRichness with its UCB copies, and its carried LLInt progress. A score about to be committed comes from
// what the builders returned for the bytes they wrote, so it describes exactly what the body file holds.
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

// A saved body's score, read from its three summary sections by the lanes' readers with the session's strict flag.
// With strict off no reader rejects. With it on, the rejection names the reader: part "ucb" at "saved-summary"
// (savedRichness names no check), "cb" with description(check), or "ics" with the enumerator name of the ICs check.
struct SummaryRejection {
    ASCIILiteral part;
    ASCIILiteral check;
};
std::expected<CaptureScore, SummaryRejection> scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics, bool strict);

#if ENABLE(JITCACHE_TWINS)
// Requires active production; rewrites the key's body through the writer's rewriteSection and erases the key's kept
// summary. The shell's jitcacheRewriteSection calls it.
std::expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM&, const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
// The key's kept summary, for the tests that check which score a commit kept.
std::optional<SavedScore> keptScoreForTesting(VM&, const BodyKey&);
#endif

} // namespace JSC::JITCache
