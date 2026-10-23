#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArrayProfile.h"
#include "BuiltinExecutables.h"
#include "DFGExitProfile.h"
#include "ExecutionCounter.h"
#include "JSCInlines.h"
#include "SpeculatedType.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "jitcache/base/ByteFields.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/ucb/UCBFeedback.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Locker.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of UCBFeedback.h and UCBFeedback.cpp. The ucb.feedback section carries a body's profiles, exit sites, tier-up
// history and LLInt counter, and the UCB's value and array copies exactly when the body has an image and the UCB has them:
// writeFeedbackSection puts each field at its offset, FeedbackSection::parse reads it back, seedFeedback leaves the
// captured feedback in a second generation of the program, allocating the copies for a body with an image only, and
// liveRichness and savedRichness agree. armLLIntCounter arms the counter as native setThreshold would. A body without an
// image carries its LLInt progress through carriedLLIntProgress, and an import arms the counter from it unless
// jitCacheImportLLIntProgress is off.

namespace JSC::JITCache::Tests {

namespace UCBFeedbackTestsInternal {

// The feedback section's layout, which the tests read at these offsets. They are written out apart from the lane's own
// constants, so that a writer or a parser that moves a field fails the tests.
// V, A, B, U, E, D, X and C, in FeedbackCounts order.
static constexpr std::array<size_t, 8> feedbackCountOffsets { 0, 4, 8, 12, 16, 20, 24, 44 };
static constexpr size_t feedbackDidOptimizeOffset = 28;
static constexpr size_t feedbackQuickDFGTierUpOffset = 29;
static constexpr size_t feedbackQuickFTLTierUpOffset = 30;
static constexpr size_t feedbackCopiesOffset = 31;
static constexpr size_t feedbackThresholdOffset = 32;
static constexpr size_t feedbackTotalCountOffset = 36;
static constexpr size_t feedbackCounterOffset = 40;
static constexpr size_t feedbackHeaderSize = 48;
static constexpr size_t exitSiteRecordSize = 8;

// Where each array of ucb.feedback starts for the given counts, and where the section ends. The value predictions and
// array profiles take room only in a section with copies.
struct FeedbackLayout {
    explicit FeedbackLayout(const FeedbackCounts& counts)
        : arrayProfiles(feedbackHeaderSize + (counts.copies ? sizeof(SpeculatedType) * counts.valueProfiles : 0))
        , binaryArithProfiles(arrayProfiles + (counts.copies ? 8 * static_cast<size_t>(counts.arrayProfiles) : 0))
        , unaryArithProfiles(binaryArithProfiles + 2 * static_cast<size_t>(counts.binaryArithProfiles))
        , unaryEnd(unaryArithProfiles + 2 * static_cast<size_t>(counts.unaryArithProfiles))
        , exitSites(roundUpToMultipleOf<4>(unaryEnd))
        , children(exitSites + exitSiteRecordSize * counts.exitSites)
        , constants(children + counts.functionDecls + counts.functionExprs)
        , end(constants + (static_cast<size_t>(counts.constants) + 7) / 8)
        , size(roundUpToMultipleOf<8>(end))
    {
    }

    size_t arrayProfiles;
    size_t binaryArithProfiles;
    size_t unaryArithProfiles;
    size_t unaryEnd;
    size_t exitSites;
    size_t children;
    size_t constants;
    size_t end;
    size_t size;
};

static Vector<DFG::FrequentExitSite> exitSitesOf(UnlinkedCodeBlock& codeBlock)
{
    Vector<DFG::FrequentExitSite> sites;
    ConcurrentJSLocker locker(codeBlock.m_lock);
    codeBlock.exitProfile().forEachFrequentExitSite(locker, [&](const DFG::FrequentExitSite& site) {
        sites.append(site);
    });
    return sites;
}

// The UCB's tier-up history: didOptimize and both quick tier-up bits, read under the cell lock as capture and the twin
// checks read them. The UCBs the tests compare are Strong-held, so a marker can visit one while the test runs, and its
// first visit in a cycle rewrites m_age, which shares a memory location with the quick bits, under that lock
// (UnlinkedCodeBlock::visitChildrenImpl).
struct TierUpHistory {
    TriState didOptimize;
    TriState quickDFGTierUp;
    bool quickFTLTierUp;
    friend bool operator==(const TierUpHistory&, const TierUpHistory&) = default;
};

static TierUpHistory tierUpHistoryOf(UnlinkedCodeBlock& codeBlock)
{
    Locker locker { codeBlock.cellLock() };
    return { codeBlock.didOptimize(), codeBlock.quickDFGTierUp(), codeBlock.isQuickFTLTierUp() };
}

// The precision a counter's float m_totalCount keeps around `magnitude`, with a factor of two to spare.
static bool equalUpToFloatRounding(double a, double b, double magnitude)
{
    return std::abs(a - b) <= std::max(std::abs(magnitude), 1.0) * 0x1p-23;
}

// The seven flags a UCB copy of an array profile can hold: every ArrayProfileFlag but the pruning mark.
static constexpr std::array<ArrayProfileFlag, 7> restorableArrayProfileFlags {
    ArrayProfileFlag::MayStoreHole,
    ArrayProfileFlag::OutOfBounds,
    ArrayProfileFlag::MayBeLargeTypedArray,
    ArrayProfileFlag::MayInterceptIndexedAccesses,
    ArrayProfileFlag::UsesNonOriginalArrayStructures,
    ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray,
    ArrayProfileFlag::MayBeRegExpMatchesArray,
};

// Distinct, well-spread bit patterns for the profile slots.
static uint64_t slotPattern(uint64_t index)
{
    return (index + 1) * 0x9E3779B97F4A7C15ull;
}

static constexpr int32_t fixtureThreshold = 500;
static constexpr double fixtureProgress = 123.25;
static constexpr unsigned fixtureExitSiteCount = 3;

// A UCB copy's prediction and array state, or the zeros writeFeedbackSection writes for a UCB without copies.
static SpeculatedType copiedPrediction(UnlinkedCodeBlock& codeBlock, unsigned index)
{
    auto* profiles = codeBlock.valueAndArrayProfiles();
    return profiles ? profiles->valueProfiles()[index].prediction() : SpecNone;
}

static ArrayModes copiedArrayModes(UnlinkedCodeBlock& codeBlock, unsigned index)
{
    auto* profiles = codeBlock.valueAndArrayProfiles();
    return profiles ? profiles->arrayProfiles()[index].observedArrayModes() : 0;
}

static OptionSet<ArrayProfileFlag> copiedArrayFlags(UnlinkedCodeBlock& codeBlock, unsigned index)
{
    auto* profiles = codeBlock.valueAndArrayProfiles();
    return profiles ? profiles->arrayProfiles()[index].arrayProfileFlags() : OptionSet<ArrayProfileFlag> { };
}

// Gives a generated UCB that no holder has published every kind of feedback a body carries, as a run would leave it. The
// UCB is Strong-held, so markers visit it while the test runs.
static void giveFeedback(VM& vm, UnlinkedCodeBlock& codeBlock, uint32_t symbolTableIndex)
{
    // A generated UCB has value and array copies only once a baseline compilation reaches it, so the fixture allocates them
    // as that compilation would.
    codeBlock.ensureValueAndArrayProfiles();
    auto valueProfiles = codeBlock.valueAndArrayProfiles()->valueProfiles();
    for (unsigned i = 0; i < valueProfiles.size(); ++i)
        valueProfiles[i].restorePrediction(slotPattern(i) & SpecBytecodeTop);
    auto arrayProfiles = codeBlock.valueAndArrayProfiles()->arrayProfiles();
    for (unsigned i = 0; i < arrayProfiles.size(); ++i) {
        OptionSet<ArrayProfileFlag> flags;
        for (unsigned flag = 0; flag < restorableArrayProfileFlags.size(); ++flag) {
            if ((slotPattern(i) >> (32 + flag)) & 1)
                flags.add(restorableArrayProfileFlags[flag]);
        }
        arrayProfiles[i].restoreAccumulatedState(static_cast<ArrayModes>(slotPattern(i) & ALL_ARRAY_MODES), flags);
    }
    for (unsigned i = 0; i < codeBlock.numberOfBinaryArithProfiles(); ++i)
        codeBlock.binaryArithProfile(i).restoreBits(static_cast<uint16_t>(slotPattern(i) & ((1u << 14) - 1)));
    for (unsigned i = 0; i < codeBlock.numberOfUnaryArithProfiles(); ++i)
        codeBlock.unaryArithProfile(i).restoreBits(static_cast<uint16_t>(slotPattern(i) & ((1u << 10) - 1)));
    {
        Vector<DFG::FrequentExitSite> sites {
            DFG::FrequentExitSite(BytecodeIndex(0), BadType, ExitFromDFG, ExitFromNotInlined),
            DFG::FrequentExitSite(BytecodeIndex(1), Overflow, ExitFromFTL, ExitFromInlined),
            DFG::FrequentExitSite(ArgumentsEscaped, ExitFromDFG, ExitFromNotInlined),
        };
        ConcurrentJSLocker locker(codeBlock.m_lock);
        codeBlock.exitProfile().restoreFrequentExitSites(locker, WTF::move(sites));
    }
    {
        // Under the cell lock, as seedFeedback writes them: a marker's first visit in a cycle rewrites m_age, which shares a
        // memory location with the quick tier-up bits, under that lock (UnlinkedCodeBlock::visitChildrenImpl), and an
        // unlocked write could be lost to its read-modify-write.
        Locker locker { codeBlock.cellLock() };
        codeBlock.setDidOptimize(TriState::True);
        codeBlock.setQuickDFGTierUp(TriState::False);
        codeBlock.setQuickFTLTierUp(true);
    }
    armLLIntCounter(codeBlock.llintExecuteCounter(), fixtureThreshold, fixtureProgress);
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); i += 2)
        codeBlock.functionDecl(i)->setSingletonHasBeenInvalidated();
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); i += 2)
        codeBlock.functionExpr(i)->setSingletonHasBeenInvalidated();
    constantCell<SymbolTable>(codeBlock, symbolTableIndex)->singleton().invalidate(vm, StringFireDetail("JITCache test: a scope created twice"));
}

// Whether `bytes` hold the UCB's feedback where the feedback section's layout puts each field, with zero gaps and padding.
static bool feedbackBytesMatch(std::span<const uint8_t> bytes, UnlinkedCodeBlock& codeBlock, const FeedbackCounts& counts)
{
    FeedbackLayout layout { counts };
    if (bytes.size() != layout.size)
        return false;
    std::array<uint32_t, 8> countValues { counts.valueProfiles, counts.arrayProfiles, counts.binaryArithProfiles, counts.unaryArithProfiles, counts.exitSites, counts.functionDecls, counts.functionExprs, counts.constants };
    for (size_t i = 0; i < countValues.size(); ++i) {
        if (readField<uint32_t>(bytes, feedbackCountOffsets[i]) != countValues[i])
            return false;
    }
    TierUpHistory history = tierUpHistoryOf(codeBlock);
    if (bytes[feedbackDidOptimizeOffset] != static_cast<uint8_t>(history.didOptimize) || bytes[feedbackQuickDFGTierUpOffset] != static_cast<uint8_t>(history.quickDFGTierUp)
        || bytes[feedbackQuickFTLTierUpOffset] != static_cast<uint8_t>(history.quickFTLTierUp) || bytes[feedbackCopiesOffset] != static_cast<uint8_t>(counts.copies))
        return false;
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    if (readField<int32_t>(bytes, feedbackThresholdOffset) != counter.m_activeThreshold || readField<uint32_t>(bytes, feedbackTotalCountOffset) != std::bit_cast<uint32_t>(counter.m_totalCount)
        || readField<int32_t>(bytes, feedbackCounterOffset) != counter.m_counter)
        return false;

    if (counts.copies) {
        for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
            if (readField<SpeculatedType>(bytes, feedbackHeaderSize + sizeof(SpeculatedType) * i) != copiedPrediction(codeBlock, i))
                return false;
        }
        for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
            size_t offset = layout.arrayProfiles + 8 * static_cast<size_t>(i);
            if (readField<uint32_t>(bytes, offset) != copiedArrayModes(codeBlock, i) || readField<uint32_t>(bytes, offset + 4) != copiedArrayFlags(codeBlock, i).toRaw())
                return false;
        }
    }
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i) {
        if (readField<uint16_t>(bytes, layout.binaryArithProfiles + 2 * static_cast<size_t>(i)) != codeBlock.binaryArithProfile(i).bits())
            return false;
    }
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i) {
        if (readField<uint16_t>(bytes, layout.unaryArithProfiles + 2 * static_cast<size_t>(i)) != codeBlock.unaryArithProfile(i).bits())
            return false;
    }
    if (!isAllZero(bytes.subspan(layout.unaryEnd, layout.exitSites - layout.unaryEnd)))
        return false;
    Vector<DFG::FrequentExitSite> sites = exitSitesOf(codeBlock);
    if (sites.size() != counts.exitSites)
        return false;
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        size_t offset = layout.exitSites + exitSiteRecordSize * i;
        const DFG::FrequentExitSite& site = sites[i];
        if (readField<uint32_t>(bytes, offset) != site.bytecodeIndex().asBits() || bytes[offset + 4] != site.kind() || bytes[offset + 5] != site.jitType()
            || bytes[offset + 6] != site.inlineKind() || bytes[offset + 7])
            return false;
    }
    for (uint32_t i = 0; i < counts.functionDecls; ++i) {
        if (bytes[layout.children + i] != static_cast<uint8_t>(codeBlock.functionDecl(i)->singletonHasBeenInvalidated()))
            return false;
    }
    for (uint32_t i = 0; i < counts.functionExprs; ++i) {
        if (bytes[layout.children + counts.functionDecls + i] != static_cast<uint8_t>(codeBlock.functionExpr(i)->singletonHasBeenInvalidated()))
            return false;
    }
    for (uint32_t i = 0; i < counts.constants; ++i) {
        auto* symbolTable = constantCell<SymbolTable>(codeBlock, i);
        if (bitIsSet(bytes.subspan(layout.constants), i) != (symbolTable && symbolTable->singleton().hasBeenInvalidated()))
            return false;
    }
    if (counts.constants % 8 && (bytes[layout.end - 1] >> (counts.constants % 8)))
        return false;
    return isAllZero(bytes.subspan(layout.end));
}

// Whether a parsed section reads back the UCB's feedback, for a body with or without an image, through every accessor
// FeedbackSection has.
static bool feedbackSectionDescribes(const FeedbackSection& section, UnlinkedCodeBlock& codeBlock, BodyImage image)
{
    const FeedbackCounts& counts = section.counts();
    if (counts != liveFeedbackCounts(codeBlock, image))
        return false;
    if (counts.copies) {
        for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
            if (section.prediction(i) != copiedPrediction(codeBlock, i))
                return false;
        }
        for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
            if (section.observedArrayModes(i) != copiedArrayModes(codeBlock, i) || section.arrayProfileFlags(i) != copiedArrayFlags(codeBlock, i))
                return false;
        }
    }
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i) {
        if (section.binaryArithBits(i) != codeBlock.binaryArithProfile(i).bits())
            return false;
    }
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i) {
        if (section.unaryArithBits(i) != codeBlock.unaryArithProfile(i).bits())
            return false;
    }
    Vector<DFG::FrequentExitSite> sites = exitSitesOf(codeBlock);
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        if (!(section.exitSite(i) == sites[i]))
            return false;
    }
    for (uint32_t i = 0; i < counts.functionDecls; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Declarations, i) != codeBlock.functionDecl(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < counts.functionExprs; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Expressions, i) != codeBlock.functionExpr(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < counts.constants; ++i) {
        auto* symbolTable = constantCell<SymbolTable>(codeBlock, i);
        if (section.constantSingletonInvalidated(i) != (symbolTable && symbolTable->singleton().hasBeenInvalidated()))
            return false;
    }
    TierUpHistory history = tierUpHistoryOf(codeBlock);
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    return section.didOptimize() == history.didOptimize && section.quickDFGTierUp() == history.quickDFGTierUp && section.quickFTLTierUp() == history.quickFTLTierUp
        && section.llintActiveThreshold() == counter.m_activeThreshold && section.llintProgress() == counter.count();
}

// Whether two UCBs of one program hold the same feedback, the fields the twin check compares: what seeding a body with or
// without an image must leave in the UCB it seeds. The copies compare only for a body with an image, which carries them.
// Exit sites compare as a set, and the counter's progress up to the float rounding of m_totalCount.
static bool sameFeedback(UnlinkedCodeBlock& expected, UnlinkedCodeBlock& actual, BodyImage image)
{
    if (liveFeedbackCounts(expected, image) != liveFeedbackCounts(actual, image))
        return false;
    if (image == BodyImage::Present) {
        for (unsigned i = 0; i < expected.numberOfValueProfiles(); ++i) {
            if (copiedPrediction(expected, i) != copiedPrediction(actual, i))
                return false;
        }
        for (unsigned i = 0; i < expected.numberOfArrayProfiles(); ++i) {
            if (copiedArrayModes(expected, i) != copiedArrayModes(actual, i) || copiedArrayFlags(expected, i) != copiedArrayFlags(actual, i))
                return false;
        }
    }
    for (unsigned i = 0; i < expected.numberOfBinaryArithProfiles(); ++i) {
        if (expected.binaryArithProfile(i).bits() != actual.binaryArithProfile(i).bits())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfUnaryArithProfiles(); ++i) {
        if (expected.unaryArithProfile(i).bits() != actual.unaryArithProfile(i).bits())
            return false;
    }
    Vector<DFG::FrequentExitSite> expectedSites = exitSitesOf(expected);
    Vector<DFG::FrequentExitSite> actualSites = exitSitesOf(actual);
    bool sameSites = expectedSites.size() == actualSites.size() && std::ranges::all_of(expectedSites, [&](const DFG::FrequentExitSite& site) {
        return actualSites.contains(site);
    });
    if (!sameSites)
        return false;
    if (tierUpHistoryOf(expected) != tierUpHistoryOf(actual))
        return false;
    BaselineExecutionCounter& expectedCounter = expected.llintExecuteCounter();
    BaselineExecutionCounter& actualCounter = actual.llintExecuteCounter();
    if (expectedCounter.m_activeThreshold != actualCounter.m_activeThreshold || !equalUpToFloatRounding(expectedCounter.count(), actualCounter.count(), expectedCounter.m_totalCount))
        return false;
    for (unsigned i = 0; i < expected.numberOfFunctionDecls(); ++i) {
        if (expected.functionDecl(i)->singletonHasBeenInvalidated() != actual.functionDecl(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfFunctionExprs(); ++i) {
        if (expected.functionExpr(i)->singletonHasBeenInvalidated() != actual.functionExpr(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < expected.constantRegisters().size(); ++i) {
        auto* expectedTable = constantCell<SymbolTable>(expected, i);
        auto* actualTable = constantCell<SymbolTable>(actual, i);
        if (!!expectedTable != !!actualTable || (expectedTable && expectedTable->singleton().hasBeenInvalidated() != actualTable->singleton().hasBeenInvalidated()))
            return false;
    }
    return true;
}

// ucb.feedback: a UCB given every kind of feedback writes a section at the layout's offsets, with its copies for a body
// with an image and without them, V and A still the UCB's counts, for a body without one. Both round-trip and seed second
// generations of the same program to the same feedback, allocating the copies for the body with an image alone, and the
// richness of the live UCB and of both saved sections agree.
static bool testFeedbackSection(VM& vm, TestPart& part)
{
    auto producer = generateSectionFixture(vm);
    if (!part.check(!!producer, "the section fixture program failed to generate"_s))
        return false;
    ProgramFixture consumer = generateFixture(vm, producer->text);
    ProgramFixture imagelessConsumer = generateFixture(vm, producer->text);
    if (!part.check(consumer.codeBlock && imagelessConsumer.codeBlock, "a test program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& source = *producer->codeBlock.get();
    UnlinkedCodeBlock& target = *consumer.codeBlock.get();
    UnlinkedCodeBlock& imagelessTarget = *imagelessConsumer.codeBlock.get();
    auto symbolTable = symbolTableConstantIndex(source);
    bool coversEveryArray = source.numberOfValueProfiles() && source.numberOfArrayProfiles() && source.numberOfBinaryArithProfiles() && source.numberOfUnaryArithProfiles()
        && source.numberOfFunctionDecls() && source.numberOfFunctionExprs() && symbolTable;
    if (!part.check(coversEveryArray, "the fixture program lacks a profile, a child table or a SymbolTable constant"_s))
        return false;
    if (!part.check(liveFeedbackCounts(source, BodyImage::Absent) == liveFeedbackCounts(target, BodyImage::Absent), "two generations of one program differ in their feedback counts"_s))
        return false;
    // A generated UCB has no copies until a baseline compilation, or an import with an image, reaches it.
    if (!part.check(!target.valueAndArrayProfiles() && !imagelessTarget.valueAndArrayProfiles(), "a generated UCB has value and array copies before any baseline compilation"_s))
        return false;

    giveFeedback(vm, source, *symbolTable);
    FeedbackCounts counts = liveFeedbackCounts(source, BodyImage::Present);
    FeedbackCounts imagelessCounts = liveFeedbackCounts(source, BodyImage::Absent);
    bool copiesFollowTheImage = counts.copies && !imagelessCounts.copies && imagelessCounts.valueProfiles == counts.valueProfiles && imagelessCounts.arrayProfiles == counts.arrayProfiles;
    if (!part.check(copiesFollowTheImage, "liveFeedbackCounts does not carry the copies exactly for a body with an image, or changes V or A without them"_s))
        return false;
    FeedbackLayout layout { counts };
    FeedbackLayout imagelessLayout { imagelessCounts };
    bool sizesFollowTheLayout = counts.exitSites == fixtureExitSiteCount && feedbackSectionSize(counts) == layout.size && feedbackSectionSize(imagelessCounts) == imagelessLayout.size
        && imagelessLayout.size < layout.size;
    if (!part.check(sizesFollowTheLayout, "feedbackSectionSize disagrees with the feedback section's layout"_s))
        return false;
    Vector<uint8_t> section(feedbackSectionSize(counts));
    writeFeedbackSection(section.mutableSpan(), source, counts);
    Vector<uint8_t> imagelessSection(feedbackSectionSize(imagelessCounts));
    writeFeedbackSection(imagelessSection.mutableSpan(), source, imagelessCounts);
    if (!part.check(feedbackBytesMatch(section.span(), source, counts), "writeFeedbackSection did not lay out the feedback and the copies at the feedback section's offsets"_s))
        return false;
    if (!part.check(feedbackBytesMatch(imagelessSection.span(), source, imagelessCounts), "writeFeedbackSection did not lay out the feedback of a body without an image at the feedback section's offsets, with the copies byte 0"_s))
        return false;
    FeedbackSection parsed = FeedbackSection::parse(section.span());
    FeedbackSection parsedImageless = FeedbackSection::parse(imagelessSection.span());
    bool roundTrips = feedbackSectionDescribes(parsed, source, BodyImage::Present) && feedbackSectionDescribes(parsedImageless, source, BodyImage::Absent);
    if (!part.check(roundTrips, "ucb.feedback does not round-trip"_s))
        return false;

    // Richness: a binary profile's 13 low bits, a unary profile's 10, and one unit per exit site; no copy counts, with or
    // without an image.
    UCBRichness expectedRichness;
    for (unsigned i = 0; i < source.numberOfBinaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.binaryArithProfile(i).bits() & ((1u << 13) - 1)));
    for (unsigned i = 0; i < source.numberOfUnaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.unaryArithProfile(i).bits() & ((1u << 10) - 1)));
    expectedRichness.exitSiteUnits = fixtureExitSiteCount;
    auto agrees = [&](const UCBRichness& richness) {
        return richness.arithmeticUnits == expectedRichness.arithmeticUnits && richness.exitSiteUnits == expectedRichness.exitSiteUnits && richness.total() == expectedRichness.total();
    };
    bool richnessAgrees = agrees(liveRichness(source)) && agrees(savedRichness(parsed)) && agrees(savedRichness(parsedImageless));
    if (!part.check(richnessAgrees, "liveRichness or savedRichness does not count the richness units alike, with and without copies"_s))
        return false;

    // Seeding second generations of the program, which no holder has published: with an image, the copies are allocated and
    // seeded; without one, none are allocated.
    seedFeedback(vm, target, parsed, BodyImage::Present);
    if (!part.check(target.valueAndArrayProfiles() && sameFeedback(source, target, BodyImage::Present), "seedFeedback with an image did not allocate the copies, or did not leave the captured feedback in the seeded UCB"_s))
        return false;
    BaselineExecutionCounter& seededCounter = target.llintExecuteCounter();
    if (!part.check(seededCounter.m_activeThreshold == fixtureThreshold && equalUpToFloatRounding(seededCounter.count(), fixtureProgress, seededCounter.m_totalCount), "seedFeedback did not arm the LLInt counter with the captured threshold and progress"_s))
        return false;
    seedFeedback(vm, imagelessTarget, parsedImageless, BodyImage::Absent);
    return part.check(!imagelessTarget.valueAndArrayProfiles() && sameFeedback(source, imagelessTarget, BodyImage::Absent), "seedFeedback without an image allocated copies, or did not leave the captured feedback in the seeded UCB"_s);
}

// A builtin's UCB never has value and array copies: a capture of its body with an image writes none, with the copies byte
// 0 and V and A still the UCB's counts, which round-trips, and seeding a second generation of the body with an image
// leaves its copies null.
static bool testBuiltinFeedback(VM& vm, TestPart& part)
{
    SourceCode source = makeSource("(function (a, b) { return [a, b][0] + b; })"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> builtin { vm, BuiltinExecutables::createExecutable(vm, source, Identifier { }, ImplementationVisibility::Public,
        ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No) };
    if (!part.check(builtin && builtin.get()->isBuiltinFunction(), "the builtin root failed to create"_s))
        return false;
    SourceCode bodySource = builtin.get()->linkedSourceCode(source);
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, *builtin.get(), bodySource, CodeSpecializationKind::CodeForCall);
    Strong<UnlinkedFunctionCodeBlock> consumer = generateBodyOf(vm, *builtin.get(), bodySource, CodeSpecializationKind::CodeForCall);
    if (!part.check(producer && consumer && producer.get()->isBuiltinFunction() && producer.get()->numberOfValueProfiles() && producer.get()->numberOfArrayProfiles(), "the builtin's body failed to generate, or lacks a value or array profile"_s))
        return false;

    // A baseline compilation allocates no copies for a builtin either.
    producer.get()->ensureValueAndArrayProfiles();
    FeedbackCounts counts = liveFeedbackCounts(*producer.get(), BodyImage::Present);
    bool noCopies = !producer.get()->valueAndArrayProfiles() && !counts.copies && counts.valueProfiles == producer.get()->numberOfValueProfiles()
        && counts.arrayProfiles == producer.get()->numberOfArrayProfiles();
    if (!part.check(noCopies, "a builtin's UCB has copies, or its feedback counts are not the UCB's"_s))
        return false;
    Vector<uint8_t> section(feedbackSectionSize(counts));
    writeFeedbackSection(section.mutableSpan(), *producer.get(), counts);
    if (!part.check(section.size() == FeedbackLayout { counts }.size && feedbackBytesMatch(section.span(), *producer.get(), counts), "a builtin's feedback section is not laid out without copies"_s))
        return false;
    FeedbackSection parsed = FeedbackSection::parse(section.span());
    if (!part.check(feedbackSectionDescribes(parsed, *producer.get(), BodyImage::Present), "a builtin's feedback section does not round-trip"_s))
        return false;
    seedFeedback(vm, *consumer.get(), parsed, BodyImage::Present);
    return part.check(!consumer.get()->valueAndArrayProfiles() && sameFeedback(*producer.get(), *consumer.get(), BodyImage::Present), "seeding a builtin's body with an image allocated copies, or did not leave the captured feedback"_s);
}

// armLLIntCounter keeps the captured threshold and progress, arms the slice native setThreshold arms from them, and arms a
// deferred record as deferIndefinitely leaves it.
static bool testLLIntCounter(TestPart& part)
{
    unsigned nativeArmings = 0;
    for (int32_t threshold : { 0, 100, 500 }) {
        double t = threshold;
        for (double progress : { 0.0, 0.37, 1.0, 99.63, t - 0.5, t - 1, t, t + 5, 1e6 + 0.25 }) {
            // Only the progress a counter that is not deferred can hold, which is never negative.
            if (progress < 0)
                continue;
            BaselineExecutionCounter counter;
            armLLIntCounter(counter, threshold, progress);
            double remaining = t - progress;
            int32_t slice = remaining > 0 ? static_cast<int32_t>(BaselineExecutionCounter::clippedThreshold(nullptr, remaining)) : 0;
            bool armed = counter.m_activeThreshold == threshold && counter.m_counter == -slice && equalUpToFloatRounding(counter.count(), progress, progress + slice);
            if (!part.check(armed, makeString("armLLIntCounter with threshold "_s, threshold, " did not keep the progress or arm the slice native setThreshold arms"_s)))
                return false;
            // Native setThreshold from the same threshold and progress, wherever the public path reaches it: a counter
            // that has not crossed re-arms there, and then crosses after the same number of points.
            BaselineExecutionCounter native;
            native.m_activeThreshold = threshold;
            native.m_totalCount = static_cast<float>(progress);
            native.m_counter = 0;
            if (!native.checkIfThresholdCrossedAndSet(nullptr)) {
                ++nativeArmings;
                if (!part.check(native.m_counter == counter.m_counter, makeString("armLLIntCounter with threshold "_s, threshold, " crosses after another number of points than native setThreshold"_s)))
                    return false;
            }
        }
    }
    if (!part.check(!!nativeArmings, "no case reached native setThreshold, so the slice went unchecked against it"_s))
        return false;

    BaselineExecutionCounter deferred;
    armLLIntCounter(deferred, std::numeric_limits<int32_t>::max(), -0x1p31);
    BaselineExecutionCounter reference;
    reference.deferIndefinitely();
    bool deferredExactly = deferred.m_activeThreshold == std::numeric_limits<int32_t>::max() && deferred.m_counter == std::numeric_limits<int32_t>::min()
        && std::bit_cast<uint32_t>(deferred.m_totalCount) == std::bit_cast<uint32_t>(0.0f) && deferred.count() == -0x1p31
        && reference.m_activeThreshold == deferred.m_activeThreshold && reference.m_counter == deferred.m_counter && std::bit_cast<uint32_t>(reference.m_totalCount) == std::bit_cast<uint32_t>(deferred.m_totalCount);
    return part.check(deferredExactly, "armLLIntCounter did not leave a deferred record as deferIndefinitely does"_s);
}

} // namespace UCBFeedbackTestsInternal

using namespace UCBFeedbackTestsInternal;

JITCACHE_TEST(ucbFeedbackSectionLayoutAndSeeding, Yes)
{
    TestPart part { context };
    testFeedbackSection(*context.vm(), part);
}

JITCACHE_TEST(ucbFeedbackBuiltinCarriesNoCopies, Yes)
{
    TestPart part { context };
    testBuiltinFeedback(*context.vm(), part);
}

JITCACHE_TEST(ucbFeedbackArmLLIntCounter, No)
{
    TestPart part { context };
    testLLIntCounter(part);
}

// The progress a body without an image carries, and how an import arms its counter from it.
JITCACHE_TEST(runOnceCarriedProgressAndTiers, No)
{
    constexpr int32_t deferred = std::numeric_limits<int32_t>::max();
    JITCACHE_CHECK(carriedLLIntProgress(500, 30) == 30);
    JITCACHE_CHECK(carriedLLIntProgress(500, 30.5) == 30.5);
    JITCACHE_CHECK(carriedLLIntProgress(500, 485) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(500, 495) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(500, 800) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(100, 0.37) == 0.37);
    JITCACHE_CHECK(!carriedLLIntProgress(10, 7));
    JITCACHE_CHECK(!carriedLLIntProgress(500, 0));
    JITCACHE_CHECK(!carriedLLIntProgress(500, -4));
    JITCACHE_CHECK(!carriedLLIntProgress(500, std::nan("")));
    JITCACHE_CHECK(!carriedLLIntProgress(deferred, -0x1p31));
    JITCACHE_CHECK(carriedLLIntPoints(500, 30.5) == 30);
    JITCACHE_CHECK(carriedLLIntPoints(500, 900) == 485);
    JITCACHE_CHECK(!carriedLLIntPoints(deferred, -0x1p31));

    // A body with an image keeps the captured progress; a deferred counter stays deferred either way.
    JITCACHE_CHECK(importedLLIntProgress(500, 495, BodyImage::Present) == 495);
    JITCACHE_CHECK(importedLLIntProgress(500, 495, BodyImage::Absent) == 485);
    JITCACHE_CHECK(importedLLIntProgress(deferred, -0x1p31, BodyImage::Absent) == -0x1p31);

    // armLLIntCounter from the carried progress crosses after one full LLInt invocation, 5 points at the prologue and 10
    // at the return.
    BaselineExecutionCounter counter;
    armLLIntCounter(counter, 500, importedLLIntProgress(500, 495, BodyImage::Absent));
    JITCACHE_CHECK(counter.m_activeThreshold == 500);
    JITCACHE_CHECK(counter.m_counter == -15);
    JITCACHE_CHECK(std::abs(counter.count() - 485) < 1e-3);
}

// The measurement switch: with jitCacheImportLLIntProgress off, a body without an image arms its counter from zero, and a
// body with one keeps its progress.
JITCACHE_TEST_WITH_OPTIONS(runOnceImportFromZeroWithTheOptionOff, No, "--jitCacheImportLLIntProgress=false")
{
    JITCACHE_CHECK(!importedLLIntProgress(500, 30, BodyImage::Absent));
    JITCACHE_CHECK(importedLLIntProgress(500, 30, BodyImage::Present) == 30);
    JITCACHE_CHECK(importedLLIntProgress(std::numeric_limits<int32_t>::max(), -0x1p31, BodyImage::Absent) == -0x1p31);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
