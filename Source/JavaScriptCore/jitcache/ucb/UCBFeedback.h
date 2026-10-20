#pragma once

#include "ArrayProfile.h"
#include "BodyKey.h"
#include "DFGExitProfile.h"
#include "ExecutionCounter.h"
#include "SpeculatedType.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/TriState.h>

namespace JSC {

class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The ucb.feedback section, its checks, seeding, richness and the LLInt counter.

// Whether the body holds an image: highest tier 1 or 0. A body without one holds the lane's sections alone: its import
// leaves the CodeBlocks in the LLInt, its LLInt counter keeps the captured progress only up to one invocation short of
// crossing, and it carries no UCB value or array copies, which exist only once a baseline compilation reached the UCB.
enum class BodyImage : uint8_t { Present, Absent };

// The lane's share of a capture's richness score: the arithmetic categories and the exit sites. A body without an image
// carries no UCB copies, so these are its whole richness; for a body with one, the CB lane counts the value categories and
// array flags, each unioned with the UCB copy it pairs with.
struct UCBRichness {
    uint64_t arithmeticUnits { 0 }; // one per set category bit of each arithmetic profile slot
    uint64_t exitSiteUnits { 0 }; // one per exit site
    uint64_t total() const { return arithmeticUnits + exitSiteUnits; }
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
    bool copies { false }; // the section holds the UCB's value and array copies
    friend bool operator==(const FeedbackCounts&, const FeedbackCounts&) = default;
};

class FeedbackSection { // a parsed ucb.feedback section; borrows its bytes
public:
    // With strict, empty when the section breaks a layout rule. Without, never empty: the counts and arrays are read where
    // the layout puts them and trusted.
    static std::optional<FeedbackSection> parse(std::span<const uint8_t>, bool strict);
    const FeedbackCounts& counts() const;
    SpeculatedType prediction(unsigned) const; // these three only when counts().copies
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

// VM thread; exit sites under UnlinkedCodeBlock::m_lock. copies: Present and valueAndArrayProfiles() non-null.
FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock&, BodyImage);
// The section's counts equal the UCB's, its copies are there exactly when the body has an image and the UCB is no
// builtin's, and every exit site lies inside the UCB's instructions.
bool feedbackFits(UnlinkedCodeBlock&, const FeedbackSection&, BodyImage);
// Every constant whose singleton bit the section sets is a SymbolTable.
bool constantBitsFit(UnlinkedCodeBlock&, const FeedbackSection&);
// Allocates the copies for Present, seeds them when the section carries them, and arms the LLInt counter with
// importedLLIntProgress. The UCB is one no holder has published, whose counts the section fits.
void seedFeedback(VM&, UnlinkedCodeBlock&, const FeedbackSection&, BodyImage);
size_t feedbackSectionSize(const FeedbackCounts&);
// Fills exactly feedbackSectionSize(counts) bytes from the UCB's accumulated state, the copies when counts.copies.
void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock&, const FeedbackCounts&);
// Arms a counter that no CodeBlock owns from a captured threshold and progress.
void armLLIntCounter(BaselineExecutionCounter&, int32_t threshold, double progress);
// The progress an import arms from: as captured for a body with an image, carried for one without, or zero while the free
// option jitCacheImportLLIntProgress is off. A deferred counter's progress stays as captured, and armLLIntCounter defers it
// again.
double importedLLIntProgress(int32_t threshold, double progress, BodyImage);
// min(progress, threshold - 15), so that one whole LLInt invocation runs before the counter crosses; zero when either is
// not positive, for a NaN progress and for a deferred counter.
double carriedLLIntProgress(int32_t threshold, double progress);
// carriedLLIntProgress in whole points: the counter progress that scores a body without an image and its envelope's L.
uint32_t carriedLLIntPoints(int32_t threshold, double progress);
// True when the counter shows progress, or the deferral only a crossing leaves. A UCB proves only a request: a module is
// generated at link time and a CachedCall callee when prepared.
bool llintCounterShowsExecution(UnlinkedCodeBlock&);
UCBRichness liveRichness(UnlinkedCodeBlock&); // VM thread, during a capture
UCBRichness savedRichness(const FeedbackSection&); // the saved body's ucb.feedback, its summary in the file

} // namespace JSC::JITCache
