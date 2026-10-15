#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArrayProfile.h"
#include "ByteFields.h"
#include "DFGExitProfile.h"
#include "ExecutionCounter.h"
#include "JITCacheCapture.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "SpeculatedType.h"
#include "SymbolTable.h"
#include "UCBFeedback.h"
#include "UCBTestSupport.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
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
// history and LLInt counter: writeFeedbackSection puts each field at its offset, FeedbackSection::parse reads it back and
// rejects each rule of the layout broken once, feedbackFits and constantBitsFit refuse a UCB the section does not fit,
// seedFeedback leaves the captured feedback in a second generation of the program, and liveRichness and savedRichness
// agree. armLLIntCounter arms the counter as native setThreshold would. A body without an image carries its LLInt
// progress through carriedLLIntProgress; an import arms the counter from it unless jitCacheImportLLIntProgress is off,
// and JITCacheCapture's beats ranks such a capture below any capture with an image.

namespace JSC::JITCache::Tests {

namespace UCBFeedbackTestsInternal {

// The feedback section's layout, which the tests read at these offsets. They are written out apart from the lane's own
// constants, so that a writer or a parser that moves a field fails the tests.
static constexpr uint32_t feedbackSectionMagic = 0x46424355;
static constexpr size_t feedbackVersionOffset = 4;
static constexpr size_t feedbackReservedOffset = 6;
// V, A, B, U, E, D, X and C, in FeedbackCounts order.
static constexpr std::array<size_t, 8> feedbackCountOffsets { 8, 12, 16, 20, 24, 28, 32, 52 };
static constexpr size_t feedbackDidOptimizeOffset = 36;
static constexpr size_t feedbackQuickDFGTierUpOffset = 37;
static constexpr size_t feedbackQuickFTLTierUpOffset = 38;
static constexpr size_t feedbackReservedByteOffset = 39;
static constexpr size_t feedbackThresholdOffset = 40;
static constexpr size_t feedbackTotalCountOffset = 44;
static constexpr size_t feedbackCounterOffset = 48;
static constexpr size_t feedbackHeaderSize = 56;
static constexpr size_t exitSiteRecordSize = 8;

// Where each array of ucb.feedback starts for the given counts, and where the section ends.
struct FeedbackLayout {
    explicit FeedbackLayout(const FeedbackCounts& counts)
        : arrayProfiles(feedbackHeaderSize + sizeof(SpeculatedType) * counts.valueProfiles)
        , binaryArithProfiles(arrayProfiles + 8 * static_cast<size_t>(counts.arrayProfiles))
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

// Gives a generated UCB that no holder has published every kind of feedback a body carries, as a run would leave it. The
// UCB is Strong-held, so markers visit it while the test runs.
static void giveFeedback(VM& vm, UnlinkedCodeBlock& codeBlock, uint32_t symbolTableIndex)
{
    auto& valueProfiles = codeBlock.unlinkedValueProfiles();
    for (unsigned i = 0; i < valueProfiles.size(); ++i)
        valueProfiles[i].restorePrediction(slotPattern(i) & SpecBytecodeTop);
    auto& arrayProfiles = codeBlock.unlinkedArrayProfiles();
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
    if (bytes.size() != layout.size || readField<uint32_t>(bytes, 0) != feedbackSectionMagic || readField<uint16_t>(bytes, feedbackVersionOffset) != 1 || readField<uint16_t>(bytes, feedbackReservedOffset))
        return false;
    std::array<uint32_t, 8> countValues { counts.valueProfiles, counts.arrayProfiles, counts.binaryArithProfiles, counts.unaryArithProfiles, counts.exitSites, counts.functionDecls, counts.functionExprs, counts.constants };
    for (size_t i = 0; i < countValues.size(); ++i) {
        if (readField<uint32_t>(bytes, feedbackCountOffsets[i]) != countValues[i])
            return false;
    }
    TierUpHistory history = tierUpHistoryOf(codeBlock);
    if (bytes[feedbackDidOptimizeOffset] != static_cast<uint8_t>(history.didOptimize) || bytes[feedbackQuickDFGTierUpOffset] != static_cast<uint8_t>(history.quickDFGTierUp)
        || bytes[feedbackQuickFTLTierUpOffset] != static_cast<uint8_t>(history.quickFTLTierUp) || bytes[feedbackReservedByteOffset])
        return false;
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    if (readField<int32_t>(bytes, feedbackThresholdOffset) != counter.m_activeThreshold || readField<uint32_t>(bytes, feedbackTotalCountOffset) != std::bit_cast<uint32_t>(counter.m_totalCount)
        || readField<int32_t>(bytes, feedbackCounterOffset) != counter.m_counter)
        return false;

    for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
        if (readField<SpeculatedType>(bytes, feedbackHeaderSize + sizeof(SpeculatedType) * i) != codeBlock.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        auto& profile = codeBlock.unlinkedArrayProfiles()[i];
        size_t offset = layout.arrayProfiles + 8 * static_cast<size_t>(i);
        if (readField<uint32_t>(bytes, offset) != profile.observedArrayModes() || readField<uint32_t>(bytes, offset + 4) != profile.arrayProfileFlags().toRaw())
            return false;
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

// Whether a parsed section reads back the UCB's feedback through every accessor FeedbackSection has.
static bool feedbackSectionDescribes(const FeedbackSection& section, UnlinkedCodeBlock& codeBlock)
{
    const FeedbackCounts& counts = section.counts();
    if (counts != liveFeedbackCounts(codeBlock))
        return false;
    for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
        if (section.prediction(i) != codeBlock.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        auto& profile = codeBlock.unlinkedArrayProfiles()[i];
        if (section.observedArrayModes(i) != profile.observedArrayModes() || section.arrayProfileFlags(i) != profile.arrayProfileFlags())
            return false;
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

// Whether two UCBs of one program hold the same feedback, the fields the twin check compares: what seeding must leave in
// the UCB it seeds. Exit sites compare as a set, and the counter's progress up to the float rounding of m_totalCount.
static bool sameFeedback(UnlinkedCodeBlock& expected, UnlinkedCodeBlock& actual)
{
    if (liveFeedbackCounts(expected) != liveFeedbackCounts(actual))
        return false;
    for (unsigned i = 0; i < expected.numberOfValueProfiles(); ++i) {
        if (expected.unlinkedValueProfiles()[i].prediction() != actual.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfArrayProfiles(); ++i) {
        auto& expectedProfile = expected.unlinkedArrayProfiles()[i];
        auto& actualProfile = actual.unlinkedArrayProfiles()[i];
        if (expectedProfile.observedArrayModes() != actualProfile.observedArrayModes() || expectedProfile.arrayProfileFlags() != actualProfile.arrayProfileFlags())
            return false;
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

// ucb.feedback: a UCB given every kind of feedback writes a section at the layout's offsets, which round-trips with strict
// on and off and seeds a second generation of the same program to the same feedback; the richness of the live UCB and of
// the saved section agree; feedbackFits and constantBitsFit refuse a UCB the section does not fit; and strict parsing
// rejects each rule of the layout broken once.
static bool testFeedbackSection(VM& vm, TestPart& part)
{
    auto producer = generateSectionFixture(vm);
    if (!part.check(!!producer, "the section fixture program failed to generate"_s))
        return false;
    ProgramFixture consumer = generateFixture(vm, producer->text);
    ProgramFixture unrelated = generateFixture(vm, "0;"_s);
    if (!part.check(consumer.codeBlock && unrelated.codeBlock, "a test program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& source = *producer->codeBlock.get();
    UnlinkedCodeBlock& target = *consumer.codeBlock.get();
    auto symbolTable = symbolTableConstantIndex(source);
    bool coversEveryArray = source.numberOfValueProfiles() && source.numberOfArrayProfiles() && source.numberOfBinaryArithProfiles() && source.numberOfUnaryArithProfiles()
        && source.numberOfFunctionDecls() && source.numberOfFunctionExprs() && symbolTable;
    if (!part.check(coversEveryArray, "the fixture program lacks a profile, a child table or a SymbolTable constant"_s))
        return false;
    if (!part.check(liveFeedbackCounts(source) == liveFeedbackCounts(target), "two generations of one program differ in their feedback counts"_s))
        return false;

    giveFeedback(vm, source, *symbolTable);
    FeedbackCounts counts = liveFeedbackCounts(source);
    FeedbackLayout layout { counts };
    if (!part.check(counts.exitSites == fixtureExitSiteCount && feedbackSectionSize(counts) == layout.size, "feedbackSectionSize disagrees with the feedback section's layout"_s))
        return false;
    Vector<uint8_t> section(feedbackSectionSize(counts));
    writeFeedbackSection(section.mutableSpan(), source, counts);
    if (!part.check(feedbackBytesMatch(section.span(), source, counts), "writeFeedbackSection did not lay out the feedback at the feedback section's offsets"_s))
        return false;
    for (bool strict : { true, false }) {
        auto parsed = FeedbackSection::parse(section.span(), strict);
        if (!part.check(parsed && feedbackSectionDescribes(*parsed, source), strict ? "ucb.feedback does not round-trip with strict on"_s : "ucb.feedback does not round-trip with strict off"_s))
            return false;
    }

    // Richness: a binary profile's 13 low bits, a unary profile's 10, and one unit per exit site.
    UCBRichness expectedRichness;
    for (unsigned i = 0; i < source.numberOfBinaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.binaryArithProfile(i).bits() & ((1u << 13) - 1)));
    for (unsigned i = 0; i < source.numberOfUnaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.unaryArithProfile(i).bits() & ((1u << 10) - 1)));
    expectedRichness.exitSiteUnits = fixtureExitSiteCount;
    UCBRichness live = liveRichness(source, BodyImage::Present);
    auto savedSection = FeedbackSection::parse(section.span(), true);
    std::optional<UCBRichness> saved;
    if (savedSection)
        saved = savedRichness(*savedSection, BodyImage::Present);
    bool richnessAgrees = live.arithmeticUnits == expectedRichness.arithmeticUnits && live.exitSiteUnits == expectedRichness.exitSiteUnits && !live.copyUnits
        && saved && saved->arithmeticUnits == expectedRichness.arithmeticUnits && saved->exitSiteUnits == expectedRichness.exitSiteUnits && !saved->copyUnits;
    if (!part.check(richnessAgrees, "liveRichness or savedRichness does not count the richness units"_s))
        return false;
    // Without an image the UCB copies count too, one unit per prediction bit and per array flag, alike live and saved.
    UCBRichness liveWithoutImage = liveRichness(source, BodyImage::Absent);
    UCBRichness savedWithoutImage = savedSection ? savedRichness(*savedSection, BodyImage::Absent) : UCBRichness { };
    uint64_t expectedCopyUnits = 0;
    for (auto& profile : source.unlinkedValueProfiles())
        expectedCopyUnits += static_cast<uint64_t>(std::popcount(static_cast<uint64_t>(profile.prediction())));
    for (auto& profile : source.unlinkedArrayProfiles())
        expectedCopyUnits += static_cast<uint64_t>(std::popcount(profile.arrayProfileFlags().toRaw()));
    if (!part.check(liveWithoutImage.copyUnits == expectedCopyUnits && savedWithoutImage.copyUnits == expectedCopyUnits && liveWithoutImage.total() == savedWithoutImage.total(),
        "the richness of a body without an image does not count its UCB copies alike live and saved"_s))
        return false;
    Vector<uint8_t> brokenMagic = section;
    brokenMagic[0] ^= 0xff;
    if (!part.check(!FeedbackSection::parse(brokenMagic.span(), true) && FeedbackSection::parse(brokenMagic.span(), false), "FeedbackSection::parse does not reject a broken magic with strict on alone"_s))
        return false;

    // Seeding a second generation of the program, which no holder has published.
    auto parsed = FeedbackSection::parse(section.span(), true);
    if (!part.check(parsed && feedbackFits(target, *parsed) && constantBitsFit(target, *parsed), "feedbackFits or constantBitsFit refuses a UCB its section fits"_s))
        return false;
    if (!part.check(!feedbackFits(*unrelated.codeBlock.get(), *parsed), "feedbackFits accepts a UCB with other counts"_s))
        return false;
    seedFeedback(vm, target, *parsed, BodyImage::Present);
    if (!part.check(sameFeedback(source, target), "seedFeedback did not leave the captured feedback in the seeded UCB"_s))
        return false;
    BaselineExecutionCounter& seededCounter = target.llintExecuteCounter();
    if (!part.check(seededCounter.m_activeThreshold == fixtureThreshold && equalUpToFloatRounding(seededCounter.count(), fixtureProgress, seededCounter.m_totalCount), "seedFeedback did not arm the LLInt counter with the captured threshold and progress"_s))
        return false;

    // An exit site at or past the end of the instruction stream.
    Vector<uint8_t> pastTheStream = section;
    writeField<uint32_t>(pastTheStream.mutableSpan(), layout.exitSites, BytecodeIndex(source.instructionsSize()).asBits());
    auto pastTheStreamSection = FeedbackSection::parse(pastTheStream.span(), true);
    if (!part.check(pastTheStreamSection && !feedbackFits(source, *pastTheStreamSection), "feedbackFits accepts an exit site past the instruction stream"_s))
        return false;
    // A singleton bit on a constant that is not a SymbolTable.
    auto stringIndex = stringConstantIndex(source, "jitcache-u5-atom-constant"_s);
    if (!part.check(!!stringIndex, "the fixture program lacks its string constants"_s))
        return false;
    Vector<uint8_t> notATable = section;
    setBit(notATable.mutableSpan().subspan(layout.constants), *stringIndex);
    auto notATableSection = FeedbackSection::parse(notATable.span(), true);
    if (!part.check(notATableSection && !constantBitsFit(source, *notATableSection), "constantBitsFit accepts a singleton bit on a string constant"_s))
        return false;

    // The two counter states the layout admits besides the fixture's.
    auto setCounter = [](Vector<uint8_t>& bytes, int32_t threshold, float totalCount, int32_t counter) {
        writeField<int32_t>(bytes.mutableSpan(), feedbackThresholdOffset, threshold);
        writeField<float>(bytes.mutableSpan(), feedbackTotalCountOffset, totalCount);
        writeField<int32_t>(bytes.mutableSpan(), feedbackCounterOffset, counter);
    };
    auto acceptsCounter = [&](int32_t threshold, float totalCount, int32_t counter) {
        Vector<uint8_t> crafted = section;
        setCounter(crafted, threshold, totalCount, counter);
        return !!FeedbackSection::parse(crafted.span(), true);
    };
    if (!part.check(acceptsCounter(std::numeric_limits<int32_t>::max(), 0, std::numeric_limits<int32_t>::min()) && acceptsCounter(0, 0, 0), "strict ucb.feedback parsing refuses a deferred or a zero counter"_s))
        return false;

    auto parseFeedback = [](std::span<const uint8_t> bytes, bool strict) {
        return FeedbackSection::parse(bytes, strict);
    };
    // Parsing with strict off reads only the header, so every crafted section below keeps its layout readable.
    auto rejects = [&](ASCIILiteral rule, const auto& mutate) {
        return strictRejects(part, "ucb.feedback"_s, parseFeedback, section, rule, true, mutate);
    };
    for (size_t offset : feedbackCountOffsets) {
        if (!rejects("a count of 1 << 28"_s, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), offset, sectionCountLimit); }))
            return false;
    }
    ArrayModes modesOutside = ~static_cast<ArrayModes>(ALL_ARRAY_MODES);
    size_t firstSite = layout.exitSites;
    size_t secondSite = layout.exitSites + exitSiteRecordSize;
    return rejects("a wrong magic"_s, [](auto& bytes) { bytes[0] ^= 0xff; })
        && rejects("layout version 2"_s, [](auto& bytes) { writeField<uint16_t>(bytes.mutableSpan(), feedbackVersionOffset, 2); })
        && rejects("a nonzero reserved half-word"_s, [](auto& bytes) { bytes[feedbackReservedOffset + 1] = 1; })
        && rejects("one byte more"_s, [](auto& bytes) { bytes.append(static_cast<uint8_t>(0)); })
        && rejects("one byte fewer"_s, [](auto& bytes) { bytes.removeLast(); })
        && rejects("a prediction with SpecInt52Any"_s, [](auto& bytes) { writeField<SpeculatedType>(bytes.mutableSpan(), feedbackHeaderSize, readField<SpeculatedType>(bytes.span(), feedbackHeaderSize) | SpecInt52Any); })
        && rejects("a prediction with SpecDoubleImpureNaN"_s, [](auto& bytes) { writeField<SpeculatedType>(bytes.mutableSpan(), feedbackHeaderSize, readField<SpeculatedType>(bytes.span(), feedbackHeaderSize) | SpecDoubleImpureNaN); })
        && (!modesOutside || rejects("array modes outside ALL_ARRAY_MODES"_s, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), layout.arrayProfiles, readField<uint32_t>(bytes.span(), layout.arrayProfiles) | (1u << std::countr_zero(modesOutside))); }))
        && rejects("the pruning mark among array flags"_s, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), layout.arrayProfiles + 4, readField<uint32_t>(bytes.span(), layout.arrayProfiles + 4) | static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning)); })
        && rejects("an array flag past the eight"_s, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), layout.arrayProfiles + 4, readField<uint32_t>(bytes.span(), layout.arrayProfiles + 4) | (1u << 8)); })
        && rejects("binary arithmetic bits at 1 << 14"_s, [&](auto& bytes) { writeField<uint16_t>(bytes.mutableSpan(), layout.binaryArithProfiles, 1u << 14); })
        && rejects("unary arithmetic bits at 1 << 10"_s, [&](auto& bytes) { writeField<uint16_t>(bytes.mutableSpan(), layout.unaryArithProfiles, 1u << 10); })
        && rejects("exit kind ExitKindUnset"_s, [&](auto& bytes) { bytes[firstSite + 4] = ExitKindUnset; })
        && rejects("an exit kind past the last"_s, [&](auto& bytes) { bytes[firstSite + 4] = static_cast<uint8_t>(UnexpectedResizableArrayBufferView) + 1; })
        && rejects("exiting JIT type ExitFromAnything"_s, [&](auto& bytes) { bytes[firstSite + 5] = ExitFromAnything; })
        && rejects("an exiting JIT type past ExitFromFTL"_s, [&](auto& bytes) { bytes[firstSite + 5] = static_cast<uint8_t>(ExitFromFTL) + 1; })
        && rejects("inline kind ExitFromAnyInlineKind"_s, [&](auto& bytes) { bytes[firstSite + 6] = ExitFromAnyInlineKind; })
        && rejects("an inline kind past ExitFromInlined"_s, [&](auto& bytes) { bytes[firstSite + 6] = static_cast<uint8_t>(ExitFromInlined) + 1; })
        && rejects("a nonzero exit-site pad byte"_s, [&](auto& bytes) { bytes[firstSite + 7] = 1; })
        && rejects("two equal exit sites"_s, [&](auto& bytes) { memcpySpan(bytes.mutableSpan().subspan(secondSite, exitSiteRecordSize), bytes.span().subspan(firstSite, exitSiteRecordSize)); })
        && (layout.exitSites == layout.unaryEnd || rejects("a nonzero alignment gap"_s, [&](auto& bytes) { bytes[layout.unaryEnd] = 1; }))
        && rejects("didOptimize 3"_s, [](auto& bytes) { bytes[feedbackDidOptimizeOffset] = 3; })
        && rejects("quick DFG tier-up 3"_s, [](auto& bytes) { bytes[feedbackQuickDFGTierUpOffset] = 3; })
        && rejects("quick FTL tier-up 2"_s, [](auto& bytes) { bytes[feedbackQuickFTLTierUpOffset] = 2; })
        && rejects("a nonzero byte 39"_s, [](auto& bytes) { bytes[feedbackReservedByteOffset] = 1; })
        && rejects("a negative counter threshold"_s, [&](auto& bytes) { setCounter(bytes, -1, 499.25f, -376); })
        && rejects("a NaN counter total"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, std::numeric_limits<float>::quiet_NaN(), 0); })
        && rejects("an infinite counter total"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, std::numeric_limits<float>::infinity(), 0); })
        && rejects("a negative counter progress"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, 1, -5); })
        && rejects("a counter progress of 2^31"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, 0x1p31f, 0); })
        && rejects("a deferred threshold with a nonzero total"_s, [&](auto& bytes) { setCounter(bytes, std::numeric_limits<int32_t>::max(), 1, std::numeric_limits<int32_t>::min()); })
        && rejects("a child bit of 2"_s, [&](auto& bytes) { bytes[layout.children] = 2; })
        && rejects("a constant bit at C"_s, [&](auto& bytes) { setBit(bytes.mutableSpan().subspan(layout.constants), counts.constants); })
        && (layout.size == layout.end || rejects("nonzero final padding"_s, [&](auto& bytes) {
            bytes[layout.end] = 1;
        }));
}

// armLLIntCounter keeps the captured threshold and progress, arms the slice native setThreshold arms from them, and arms a
// deferred record as deferIndefinitely leaves it.
static bool testLLIntCounter(TestPart& part)
{
    unsigned nativeArmings = 0;
    for (int32_t threshold : { 0, 100, 500 }) {
        double t = threshold;
        for (double progress : { 0.0, 0.37, 1.0, 99.63, t - 0.5, t - 1, t, t + 5, 1e6 + 0.25 }) {
            // Only the progress the feedback section admits.
            if (progress < 0 || progress >= 0x1p31)
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

JITCACHE_TEST(ucbFeedbackArmLLIntCounter, No)
{
    TestPart part { context };
    testLLIntCounter(part);
}

// The progress a body without an image carries, how an import arms its counter from it, and the order of scores across
// tiers.
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

    // Tier comes first: no capture without an image beats one with an image, and any capture with one beats it.
    const CaptureScore imageless { imagelessTier, 1000, 0, false, 485 };
    const CaptureScore baseline { baselineTier, 0, 0, false, 0 };
    JITCACHE_CHECK(beats(baseline, imageless));
    JITCACHE_CHECK(!beats(imageless, baseline));
    // Between captures without an image, richness, then the carried progress; a tie keeps the saved body.
    JITCACHE_CHECK(beats({ imagelessTier, 1001, 0, false, 0 }, imageless));
    JITCACHE_CHECK(beats({ imagelessTier, 1000, 0, false, 486 }, imageless));
    JITCACHE_CHECK(!beats(imageless, imageless));
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
