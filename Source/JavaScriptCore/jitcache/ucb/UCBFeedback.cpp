#include "config.h"
#include "jitcache/ucb/UCBFeedback.h"

#include "ArithProfile.h"
#include "JSCInlines.h"
#include "Options.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "jitcache/base/ByteFields.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC::JITCache {

namespace UCBFeedbackInternal {

// The header of ucb.feedback.
static constexpr size_t valueProfileCountOffset = 0;
static constexpr size_t arrayProfileCountOffset = 4;
static constexpr size_t binaryArithProfileCountOffset = 8;
static constexpr size_t unaryArithProfileCountOffset = 12;
static constexpr size_t exitSiteCountOffset = 16;
static constexpr size_t functionDeclCountOffset = 20;
static constexpr size_t functionExprCountOffset = 24;
static constexpr size_t didOptimizeOffset = 28;
static constexpr size_t quickDFGTierUpOffset = 29;
static constexpr size_t quickFTLTierUpOffset = 30;
static constexpr size_t copiesOffset = 31; // 1 when the value predictions and array profiles follow the header, 0 when they are absent
static constexpr size_t llintActiveThresholdOffset = 32;
static constexpr size_t llintTotalCountOffset = 36;
static constexpr size_t llintCounterOffset = 40;
static constexpr size_t constantCountOffset = 44;
static constexpr size_t headerSize = 48;

static constexpr size_t arrayProfileSize = 8; // u32 observedArrayModes, u32 flags
static constexpr size_t arithProfileSize = 2;
static constexpr size_t exitSiteSize = 8; // u32 BytecodeIndex::asBits(), u8 ExitKind, u8 ExitingJITType, u8 ExitingInlineKind, u8 0
static constexpr size_t exitSiteKindOffset = 4;
static constexpr size_t exitSiteJITTypeOffset = 5;
static constexpr size_t exitSiteInlineKindOffset = 6;

// The TriState bytes hold the enumerators' own values.
static_assert(!static_cast<uint8_t>(TriState::False) && static_cast<uint8_t>(TriState::True) == 1 && static_cast<uint8_t>(TriState::Indeterminate) == 2);

// An arithmetic profile's richness categories: a binary profile's seven results and both operand types, below its special
// fast-path bit, and a unary profile's results and argument type.
static constexpr unsigned binaryCategoryBits = ObservedResults::numBitsNeeded + 2 * ObservedType::numBitsNeeded;
static constexpr unsigned unaryCategoryBits = ObservedResults::numBitsNeeded + ObservedType::numBitsNeeded;
static_assert(BinaryArithProfile::specialFastPathBit == 1u << binaryCategoryBits);
static constexpr uint32_t binaryCategoryMask = (1u << binaryCategoryBits) - 1;
static constexpr uint32_t unaryCategoryMask = (1u << unaryCategoryBits) - 1;

// The points one LLInt invocation adds to its body's counter: 5 at the prologue and 10 at the return
// (llint/LowLevelInterpreter.asm, the prologue and checkSwitchToJITForEpilogue macros).
static constexpr double llintInvocationPoints = 15;

static size_t constantBitsSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

// The regions after the header, in layout order. Every count is 32-bit and every element at most 8 bytes, so plain
// size_t arithmetic cannot overflow on the 64-bit targets. The value predictions and the array profiles take room only
// in a section that carries the UCB's copies; V and A are the UCB's counts either way.
static size_t arraysOffset(const FeedbackCounts& counts)
{
    size_t offset = headerSize;
    if (counts.copies)
        offset += counts.valueProfiles * sizeof(SpeculatedType);
    return offset;
}

static size_t binaryOffset(const FeedbackCounts& counts)
{
    size_t offset = arraysOffset(counts);
    if (counts.copies)
        offset += counts.arrayProfiles * arrayProfileSize;
    return offset;
}

static size_t unaryOffset(const FeedbackCounts& counts)
{
    return binaryOffset(counts) + counts.binaryArithProfiles * arithProfileSize;
}

static size_t unaryEnd(const FeedbackCounts& counts)
{
    return unaryOffset(counts) + counts.unaryArithProfiles * arithProfileSize;
}

static size_t exitsOffset(const FeedbackCounts& counts)
{
    return roundUpToMultipleOf<4>(unaryEnd(counts));
}

static size_t childrenOffset(const FeedbackCounts& counts)
{
    return exitsOffset(counts) + counts.exitSites * exitSiteSize;
}

static size_t constantsOffset(const FeedbackCounts& counts)
{
    return childrenOffset(counts) + counts.functionDecls + counts.functionExprs;
}

static size_t constantsEnd(const FeedbackCounts& counts)
{
    return constantsOffset(counts) + constantBitsSize(counts.constants);
}

static size_t sectionSize(const FeedbackCounts& counts)
{
    return roundUpToMultipleOf<8>(constantsEnd(counts));
}

static size_t elementOffset(size_t regionOffset, unsigned index, size_t elementSize)
{
    return regionOffset + index * elementSize;
}

static FeedbackCounts countsIn(std::span<const uint8_t> bytes)
{
    return {
        .valueProfiles = readField<uint32_t>(bytes, valueProfileCountOffset),
        .arrayProfiles = readField<uint32_t>(bytes, arrayProfileCountOffset),
        .binaryArithProfiles = readField<uint32_t>(bytes, binaryArithProfileCountOffset),
        .unaryArithProfiles = readField<uint32_t>(bytes, unaryArithProfileCountOffset),
        .exitSites = readField<uint32_t>(bytes, exitSiteCountOffset),
        .functionDecls = readField<uint32_t>(bytes, functionDeclCountOffset),
        .functionExprs = readField<uint32_t>(bytes, functionExprCountOffset),
        .constants = readField<uint32_t>(bytes, constantCountOffset),
        .copies = !!bytes[copiesOffset],
    };
}

// The site the record at `offset` describes, as the profile stores it: the constructor counts an ArgumentsEscaped site
// at bytecode index 0, as it does natively.
static DFG::FrequentExitSite exitSiteAt(std::span<const uint8_t> bytes, size_t offset)
{
    return DFG::FrequentExitSite(BytecodeIndex::fromBits(readField<uint32_t>(bytes, offset)), static_cast<ExitKind>(bytes[offset + exitSiteKindOffset]),
        static_cast<ExitingJITType>(bytes[offset + exitSiteJITTypeOffset]), static_cast<ExitingInlineKind>(bytes[offset + exitSiteInlineKindOffset]));
}

// The UCB's tier-up history: didOptimize and the quick tier-up bits. m_quickDFGTierUp and m_quickFTLTierUp
// are bit-fields adjacent to m_age, so the three share one memory location, and a marker's first visit to the UCB in a
// cycle rewrites m_age under cellLock() (UnlinkedCodeBlock::visitChildrenImpl). The same visit reads, under that lock,
// the metadata table's bit-fields, which hold m_didOptimize (metadataSizeInBytes). A marker reaches even a UCB that no
// holder has published once a stopped-world phase finds it on the VM thread's stack, as at the end of an import's DeferGC
// scopes. Seeding therefore writes these fields under that lock, since an unlocked seed could
// be undone by the marker's read-modify-write, and capture reads them under it, so that the read never races with that
// write. The lock is not recursive, so no caller may hold it, and nothing else is taken while it is held.
struct TierUpHistory {
    TriState didOptimize;
    TriState quickDFGTierUp;
    bool quickFTLTierUp;
};

static TierUpHistory tierUpHistory(UnlinkedCodeBlock& ucb)
{
    Locker locker { ucb.cellLock() };
    return { ucb.didOptimize(), ucb.quickDFGTierUp(), ucb.isQuickFTLTierUp() };
}

static void restoreTierUpHistory(UnlinkedCodeBlock& ucb, const TierUpHistory& history)
{
    Locker locker { ucb.cellLock() };
    ucb.setDidOptimize(history.didOptimize);
    ucb.setQuickDFGTierUp(history.quickDFGTierUp);
    ucb.setQuickFTLTierUp(history.quickFTLTierUp);
}

static uint32_t exitSiteCount(UnlinkedCodeBlock& ucb)
{
    uint32_t count = 0;
    ConcurrentJSLocker locker(ucb.m_lock);
    ucb.exitProfile().forEachFrequentExitSite(locker, [&](const DFG::FrequentExitSite&) {
        ++count;
    });
    return count;
}

static uint64_t binaryArithUnits(uint32_t bits)
{
    return std::popcount(bits & binaryCategoryMask);
}

static uint64_t unaryArithUnits(uint32_t bits)
{
    return std::popcount(bits & unaryCategoryMask);
}

// A constant register as a SymbolTable, or null. An empty constant, which generation emits for TDZ checks, is no cell.
static SymbolTable* symbolTableAt(UnlinkedCodeBlock& ucb, uint32_t index)
{
    JSValue value = ucb.constantRegisters()[index].get();
    if (!value)
        return nullptr;
    return dynamicDowncast<SymbolTable>(value);
}

#if ASSERT_ENABLED
// Every count the body shares with the UCB's index spaces. The exit sites are feedback, not an index space, and whether the
// copies travel is the body's.
static bool countsFit(UnlinkedCodeBlock& ucb, const FeedbackCounts& counts)
{
    return counts.valueProfiles == ucb.numberOfValueProfiles()
        && counts.arrayProfiles == ucb.numberOfArrayProfiles()
        && counts.binaryArithProfiles == ucb.numberOfBinaryArithProfiles()
        && counts.unaryArithProfiles == ucb.numberOfUnaryArithProfiles()
        && counts.functionDecls == ucb.numberOfFunctionDecls()
        && counts.functionExprs == ucb.numberOfFunctionExprs()
        && counts.constants == ucb.constantRegisters().size();
}
#endif

} // namespace UCBFeedbackInternal

FeedbackSection FeedbackSection::parse(std::span<const uint8_t> bytes)
{
    using namespace UCBFeedbackInternal;

    FeedbackSection section;
    section.m_bytes = bytes;
    section.m_counts = countsIn(bytes);
    return section;
}

const FeedbackCounts& FeedbackSection::counts() const
{
    return m_counts;
}

SpeculatedType FeedbackSection::prediction(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    ASSERT(m_counts.copies);
    return readField<SpeculatedType>(m_bytes, elementOffset(headerSize, index, sizeof(SpeculatedType)));
}

ArrayModes FeedbackSection::observedArrayModes(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    ASSERT(m_counts.copies);
    return readField<uint32_t>(m_bytes, elementOffset(arraysOffset(m_counts), index, arrayProfileSize));
}

OptionSet<ArrayProfileFlag> FeedbackSection::arrayProfileFlags(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    ASSERT(m_counts.copies);
    return OptionSet<ArrayProfileFlag>::fromRaw(readField<uint32_t>(m_bytes, elementOffset(arraysOffset(m_counts), index, arrayProfileSize) + 4));
}

uint16_t FeedbackSection::binaryArithBits(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    return readField<uint16_t>(m_bytes, elementOffset(binaryOffset(m_counts), index, arithProfileSize));
}

uint16_t FeedbackSection::unaryArithBits(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    return readField<uint16_t>(m_bytes, elementOffset(unaryOffset(m_counts), index, arithProfileSize));
}

DFG::FrequentExitSite FeedbackSection::exitSite(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    return exitSiteAt(m_bytes, elementOffset(exitsOffset(m_counts), index, exitSiteSize));
}

bool FeedbackSection::childSingletonInvalidated(ChildTable table, unsigned index) const
{
    using namespace UCBFeedbackInternal;
    size_t tableOffset = childrenOffset(m_counts);
    if (table == ChildTable::Expressions)
        tableOffset += m_counts.functionDecls;
    return m_bytes[elementOffset(tableOffset, index, 1)] & 1;
}

bool FeedbackSection::constantSingletonInvalidated(unsigned constantIndex) const
{
    using namespace UCBFeedbackInternal;
    return (m_bytes[elementOffset(constantsOffset(m_counts), constantIndex / 8, 1)] >> (constantIndex % 8)) & 1;
}

TriState FeedbackSection::didOptimize() const
{
    using namespace UCBFeedbackInternal;
    return static_cast<TriState>(m_bytes[didOptimizeOffset]);
}

TriState FeedbackSection::quickDFGTierUp() const
{
    using namespace UCBFeedbackInternal;
    return static_cast<TriState>(m_bytes[quickDFGTierUpOffset]);
}

bool FeedbackSection::quickFTLTierUp() const
{
    using namespace UCBFeedbackInternal;
    return !!m_bytes[quickFTLTierUpOffset];
}

int32_t FeedbackSection::llintActiveThreshold() const
{
    using namespace UCBFeedbackInternal;
    return readField<int32_t>(m_bytes, llintActiveThresholdOffset);
}

double FeedbackSection::llintProgress() const
{
    using namespace UCBFeedbackInternal;
    return static_cast<double>(readField<float>(m_bytes, llintTotalCountOffset)) + readField<int32_t>(m_bytes, llintCounterOffset);
}

// A body with an image carries the UCB's copies when the UCB has them, which every non-builtin UCB has once a baseline
// compilation or an import with an image reached it. A body without one carries none, even when its UCB had them before
// its CodeBlocks died: a consumer runs it in the LLInt, where the native path allocates none.
FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock& ucb, BodyImage image)
{
    return {
        .valueProfiles = ucb.numberOfValueProfiles(),
        .arrayProfiles = ucb.numberOfArrayProfiles(),
        .binaryArithProfiles = ucb.numberOfBinaryArithProfiles(),
        .unaryArithProfiles = ucb.numberOfUnaryArithProfiles(),
        .exitSites = UCBFeedbackInternal::exitSiteCount(ucb),
        .functionDecls = static_cast<uint32_t>(ucb.numberOfFunctionDecls()),
        .functionExprs = static_cast<uint32_t>(ucb.numberOfFunctionExprs()),
        .constants = static_cast<uint32_t>(ucb.constantRegisters().size()),
        .copies = image == BodyImage::Present && ucb.valueAndArrayProfiles(),
    };
}

// Takes the UCB's cell lock for the tier-up history, so the caller must not hold it.
void seedFeedback(VM& vm, UnlinkedCodeBlock& ucb, const FeedbackSection& section, BodyImage image)
{
    const FeedbackCounts& counts = section.counts();
    // The body is trusted to fit the UCB, whose index spaces the loops below walk with the section's counts.
    ASSERT(UCBFeedbackInternal::countsFit(ucb, counts));

    // A body with an image installs without the BaselineJITPlan that would allocate the UCB's value and array copies, so
    // they are allocated here, before any CodeBlock exists to fold into them; every CodeBlock of the UCB then folds its
    // profiles into them from its first collection. A builtin's UCB never has them, and a body without an image leaves its
    // CodeBlocks in the LLInt, which allocates none.
    if (image == BodyImage::Present)
        ucb.ensureValueAndArrayProfiles();
    if (auto* profiles = ucb.valueAndArrayProfiles(); profiles && counts.copies) {
        auto valueProfiles = profiles->valueProfiles();
        for (uint32_t i = 0; i < counts.valueProfiles; ++i)
            valueProfiles[i].restorePrediction(section.prediction(i));

        auto arrayProfiles = profiles->arrayProfiles();
        for (uint32_t i = 0; i < counts.arrayProfiles; ++i)
            arrayProfiles[i].restoreAccumulatedState(section.observedArrayModes(i), section.arrayProfileFlags(i));
    }

    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i)
        ucb.binaryArithProfile(i).restoreBits(section.binaryArithBits(i));
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i)
        ucb.unaryArithProfile(i).restoreBits(section.unaryArithBits(i));

    {
        Vector<DFG::FrequentExitSite> sites(counts.exitSites, [&](size_t i) {
            return section.exitSite(static_cast<unsigned>(i));
        });
        ConcurrentJSLocker locker(ucb.m_lock);
        ucb.exitProfile().restoreFrequentExitSites(locker, WTF::move(sites));
    }

    // The tier-up history, under the cell lock that a marker's update of m_age takes (TierUpHistory).
    UCBFeedbackInternal::restoreTierUpHistory(ucb, { section.didOptimize(), section.quickDFGTierUp(), section.quickFTLTierUp() });

    int32_t threshold = section.llintActiveThreshold();
    armLLIntCounter(ucb.llintExecuteCounter(), threshold, importedLLIntProgress(threshold, section.llintProgress(), image));

    // No lock for the children: a UFE's visitor and finalizer write none of its bit-fields and only the VM thread writes
    // them, so nothing concurrent can undo the bit. FunctionExecutable::notifyCreation sets it the same way.
    for (uint32_t i = 0; i < counts.functionDecls; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Declarations, i))
            ucb.functionDecl(i)->setSingletonHasBeenInvalidated();
    }
    for (uint32_t i = 0; i < counts.functionExprs; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Expressions, i))
            ucb.functionExpr(i)->setSingletonHasBeenInvalidated();
    }

    for (uint32_t i = 0; i < counts.constants; ++i) {
        if (!section.constantSingletonInvalidated(i))
            continue;
        // No CodeBlock has cloned this table, so its singleton has no watcher and invalidate only stores the state.
        SymbolTable* symbolTable = UCBFeedbackInternal::symbolTableAt(ucb, i);
        ASSERT(symbolTable && !symbolTable->singleton().isBeingWatched());
        symbolTable->singleton().invalidate(vm, StringFireDetail("JITCache: captured singleton invalidation"));
    }
}

size_t feedbackSectionSize(const FeedbackCounts& counts)
{
    return UCBFeedbackInternal::sectionSize(counts);
}

void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock& ucb, const FeedbackCounts& counts)
{
    using namespace UCBFeedbackInternal;

    RELEASE_ASSERT(out.size() == feedbackSectionSize(counts));
    ASSERT(countsFit(ucb, counts));
    auto* profiles = ucb.valueAndArrayProfiles();
    // Only the VM thread allocates the copies, and nothing frees them while the UCB lives, so counts.copies still holds.
    RELEASE_ASSERT(!counts.copies || profiles);

    zeroSpan(out);
    writeField<uint32_t>(out, valueProfileCountOffset, counts.valueProfiles);
    writeField<uint32_t>(out, arrayProfileCountOffset, counts.arrayProfiles);
    writeField<uint32_t>(out, binaryArithProfileCountOffset, counts.binaryArithProfiles);
    writeField<uint32_t>(out, unaryArithProfileCountOffset, counts.unaryArithProfiles);
    writeField<uint32_t>(out, exitSiteCountOffset, counts.exitSites);
    writeField<uint32_t>(out, functionDeclCountOffset, counts.functionDecls);
    writeField<uint32_t>(out, functionExprCountOffset, counts.functionExprs);
    TierUpHistory history = tierUpHistory(ucb);
    out[didOptimizeOffset] = static_cast<uint8_t>(history.didOptimize);
    out[quickDFGTierUpOffset] = static_cast<uint8_t>(history.quickDFGTierUp);
    out[quickFTLTierUpOffset] = history.quickFTLTierUp;
    out[copiesOffset] = counts.copies;
    BaselineExecutionCounter& counter = ucb.llintExecuteCounter();
    writeField<int32_t>(out, llintActiveThresholdOffset, counter.m_activeThreshold);
    writeField<float>(out, llintTotalCountOffset, counter.m_totalCount);
    writeField<int32_t>(out, llintCounterOffset, counter.m_counter);
    writeField<uint32_t>(out, constantCountOffset, counts.constants);

    // Accumulated state only, from its owner: no sample is read, classified or cleared.
    if (counts.copies) {
        auto valueProfiles = profiles->valueProfiles();
        for (uint32_t i = 0; i < counts.valueProfiles; ++i)
            writeField<SpeculatedType>(out, headerSize + i * sizeof(SpeculatedType), valueProfiles[i].prediction());

        auto arrayProfiles = profiles->arrayProfiles();
        size_t arrays = arraysOffset(counts);
        for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
            size_t offset = arrays + i * arrayProfileSize;
            writeField<uint32_t>(out, offset, arrayProfiles[i].observedArrayModes());
            writeField<uint32_t>(out, offset + 4, arrayProfiles[i].arrayProfileFlags().toRaw());
        }
    }

    size_t binary = binaryOffset(counts);
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i)
        writeField<uint16_t>(out, binary + i * arithProfileSize, static_cast<uint16_t>(ucb.binaryArithProfile(i).bits()));
    size_t unary = unaryOffset(counts);
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i)
        writeField<uint16_t>(out, unary + i * arithProfileSize, static_cast<uint16_t>(ucb.unaryArithProfile(i).bits()));

    {
        // The sites cannot change between the count and this write; the bound only keeps the write in its region.
        size_t offset = exitsOffset(counts);
        size_t end = childrenOffset(counts);
        ConcurrentJSLocker locker(ucb.m_lock);
        ucb.exitProfile().forEachFrequentExitSite(locker, [&](const DFG::FrequentExitSite& site) {
            if (offset == end)
                return;
            writeField<uint32_t>(out, offset, site.bytecodeIndex().asBits());
            out[offset + exitSiteKindOffset] = site.kind();
            out[offset + exitSiteJITTypeOffset] = site.jitType();
            out[offset + exitSiteInlineKindOffset] = site.inlineKind();
            offset += exitSiteSize;
        });
        ASSERT(offset == end);
    }

    size_t children = childrenOffset(counts);
    for (uint32_t i = 0; i < counts.functionDecls; ++i)
        out[children + i] = ucb.functionDecl(i)->singletonHasBeenInvalidated();
    children += counts.functionDecls;
    for (uint32_t i = 0; i < counts.functionExprs; ++i)
        out[children + i] = ucb.functionExpr(i)->singletonHasBeenInvalidated();

    size_t constants = constantsOffset(counts);
    for (uint32_t i = 0; i < counts.constants; ++i) {
        if (SymbolTable* symbolTable = symbolTableAt(ucb, i); symbolTable && symbolTable->singleton().hasBeenInvalidated())
            out[constants + i / 8] |= 1u << (i % 8);
    }
}

void armLLIntCounter(BaselineExecutionCounter& counter, int32_t threshold, double progress)
{
    // No CodeBlock: no memory-pressure correction, as UnlinkedCodeBlock's constructor arms it. ExecutionCounter::setThreshold
    // defers indefinitely when the threshold is INT32_MAX, as the native deferral left it.
    counter.setNewThreshold(threshold);
    if (threshold == std::numeric_limits<int32_t>::max() || progress <= 0)
        return;
    double remaining = static_cast<double>(threshold) - progress;
    if (remaining <= 0) {
        counter.m_counter = 0;
        counter.m_totalCount = static_cast<float>(progress);
        return;
    }
    // The slice native setThreshold would arm, truncated first so that the progress is kept exactly.
    auto slice = static_cast<int32_t>(BaselineExecutionCounter::clippedThreshold(nullptr, remaining));
    counter.m_counter = -slice;
    counter.m_totalCount = static_cast<float>(progress + slice);
}

double importedLLIntProgress(int32_t threshold, double progress, BodyImage image)
{
    // armLLIntCounter defers a deferred counter whatever the progress, which then stays the captured -2^31.
    if (image == BodyImage::Present || threshold == std::numeric_limits<int32_t>::max())
        return progress;
    // The measurement switch: the bench compares consumers whose counters keep the carried warm-up with consumers whose
    // counters start from zero, and that measurement decides the default.
    if (!Options::jitCacheImportLLIntProgress())
        return 0;
    return carriedLLIntProgress(threshold, progress);
}

double carriedLLIntProgress(int32_t threshold, double progress)
{
    using namespace UCBFeedbackInternal;
    if (threshold == std::numeric_limits<int32_t>::max())
        return 0;
    // The comparisons send a NaN to zero.
    if (!(progress > 0))
        return 0;
    // The consumer's first invocation of the body then completes in the LLInt, profiling every site it reaches, before
    // its return crosses the threshold, as the producer's warm-up would have.
    double limit = static_cast<double>(threshold) - llintInvocationPoints;
    if (!(limit > 0))
        return 0;
    return std::min(progress, limit);
}

uint32_t carriedLLIntPoints(int32_t threshold, double progress)
{
    // At most INT32_MAX - 15, so it fits.
    return static_cast<uint32_t>(std::floor(carriedLLIntProgress(threshold, progress)));
}

bool llintCounterShowsExecution(UnlinkedCodeBlock& ucb)
{
    // Only the VM thread writes the counter, which holds no cell. Every deferral, deferIndefinitely's INT32_MAX, happens at
    // a crossing (the LLInt's tier-up slow paths when shouldJIT refuses the body) or after a compile that a crossing started.
    BaselineExecutionCounter& counter = ucb.llintExecuteCounter();
    return counter.m_activeThreshold == std::numeric_limits<int32_t>::max() || counter.count() > 0;
}

// The arithmetic profile exists only in the UCB, so each slot is already the union richness scores, and every exit site
// counts.
UCBRichness liveRichness(UnlinkedCodeBlock& ucb)
{
    using namespace UCBFeedbackInternal;

    UCBRichness richness;
    for (unsigned i = 0; i < ucb.numberOfBinaryArithProfiles(); ++i)
        richness.arithmeticUnits += binaryArithUnits(ucb.binaryArithProfile(i).bits());
    for (unsigned i = 0; i < ucb.numberOfUnaryArithProfiles(); ++i)
        richness.arithmeticUnits += unaryArithUnits(ucb.unaryArithProfile(i).bits());
    richness.exitSiteUnits = exitSiteCount(ucb);
    return richness;
}

UCBRichness savedRichness(const FeedbackSection& section)
{
    using namespace UCBFeedbackInternal;

    const FeedbackCounts& counts = section.counts();
    UCBRichness richness;
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i)
        richness.arithmeticUnits += binaryArithUnits(section.binaryArithBits(i));
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i)
        richness.arithmeticUnits += unaryArithUnits(section.unaryArithBits(i));
    richness.exitSiteUnits = counts.exitSites;
    return richness;
}

} // namespace JSC::JITCache
