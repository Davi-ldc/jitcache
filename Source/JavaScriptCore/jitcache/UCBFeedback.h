#pragma once

#include "ArrayProfile.h"
#include "DFGExitProfile.h"
#include "ExecutionCounter.h"
#include "SpeculatedType.h"
#include "UCBKeys.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/TriState.h>

namespace JSC {

class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The ucb.feedback section, its checks, seeding, richness and the LLInt counter (SPEC-ucb.md section 5).

// Whether the body holds an image (THREAD Capture). A body without one, of tier 0, holds the lane's three sections alone:
// its import leaves the CodeBlocks in the LLInt, its LLInt counter keeps the captured progress only up to one invocation
// short of crossing (section 5.5), and the lane scores its whole richness (section 5.6).
enum class BodyImage : uint8_t { Present, Absent };

// The lane's share of THREAD Capture's richness score (section 5.6): the arithmetic categories and the exit sites, and,
// for a body without an image, the categories of the UCB copies of its value predictions and array flags, which the CB
// lane counts, unioned with the CB's, for a body with one.
struct UCBRichness {
    uint64_t arithmeticUnits { 0 }; // one per set category bit of each arithmetic profile slot
    uint64_t exitSiteUnits { 0 }; // one per exit site
    uint64_t copyUnits { 0 }; // BodyImage::Absent: one per category bit of each value prediction and per array flag
    uint64_t total() const { return arithmeticUnits + exitSiteUnits + copyUnits; }
};

struct FeedbackCounts {
    uint32_t valueProfiles { 0 };
    uint32_t arrayProfiles { 0 };
    uint32_t binaryArithProfiles { 0 };
    uint32_t unaryArithProfiles { 0 };
    uint32_t exitSites { 0 };
    uint32_t functionDecls { 0 };
    uint32_t functionExprs { 0 };
    uint32_t constants { 0 };
    friend bool operator==(const FeedbackCounts&, const FeedbackCounts&) = default;
};

class FeedbackSection { // a parsed ucb.feedback section; borrows its bytes
public:
    // With strict, empty when the section breaks a rule of section 5.2. Without, never empty: the counts and arrays are read
    // where the layout puts them and trusted (THREAD Session).
    static std::optional<FeedbackSection> parse(std::span<const uint8_t>, bool strict);
    const FeedbackCounts& counts() const;
    SpeculatedType prediction(unsigned) const;
    ArrayModes observedArrayModes(unsigned) const;
    OptionSet<ArrayProfileFlag> arrayProfileFlags(unsigned) const;
    uint16_t binaryArithBits(unsigned) const;
    uint16_t unaryArithBits(unsigned) const;
    DFG::FrequentExitSite exitSite(unsigned) const;
    bool childSingletonInvalidated(ChildTable, unsigned index) const;
    bool constantSingletonInvalidated(unsigned constantIndex) const;
    TriState didOptimize() const;
    TriState quickDFGTierUp() const;
    bool quickFTLTierUp() const;
    int32_t llintActiveThreshold() const;
    double llintProgress() const; // double(m_totalCount) + m_counter

private:
    std::span<const uint8_t> m_bytes;
    FeedbackCounts m_counts;
};

FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock&); // VM thread; exit sites under UnlinkedCodeBlock::m_lock
bool feedbackFits(UnlinkedCodeBlock&, const FeedbackSection&); // C3 and C7
bool constantBitsFit(UnlinkedCodeBlock&, const FeedbackSection&); // C9
void seedFeedback(VM&, UnlinkedCodeBlock&, const FeedbackSection&, BodyImage); // section 5.3
size_t feedbackSectionSize(const FeedbackCounts&);
// Fills exactly feedbackSectionSize(counts) bytes from the UCB's accumulated state (section 8.2).
void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock&, const FeedbackCounts&);
void armLLIntCounter(BaselineExecutionCounter&, int32_t threshold, double progress); // section 5.5
// The progress an import arms the LLInt counter with (section 5.5): the captured one for a body with an image; for a body
// without one, carriedLLIntProgress, or zero while the free option jitCacheImportLLIntProgress is off. A deferred
// counter's progress stays as captured, and armLLIntCounter defers it again.
double importedLLIntProgress(int32_t threshold, double progress, BodyImage);
// What a body without an image carries of the captured progress: at most one LLInt invocation, 15 points, short of
// crossing the threshold, and never below zero. A deferred counter carries nothing.
double carriedLLIntProgress(int32_t threshold, double progress);
// carriedLLIntProgress in whole points: the counter progress that scores a body without an image and its envelope's L
// (THREAD Capture and Maintenance).
uint32_t carriedLLIntPoints(int32_t threshold, double progress);
// Whether the UCB's body ran, as its LLInt counter shows: progress, or the deferral that only a crossing leaves (section
// 8.3). A UCB proves only a request: a module is generated at link time and a CachedCall callee when prepared.
bool llintCounterShowsExecution(UnlinkedCodeBlock&);
UCBRichness liveRichness(UnlinkedCodeBlock&, BodyImage); // VM thread, during a capture
UCBRichness savedRichness(const FeedbackSection&, BodyImage); // the saved body's ucb.feedback, its summary in the file

} // namespace JSC::JITCache
