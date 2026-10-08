/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "UCBFeedback.h"

#include "ArithProfile.h"
#include "JSCInlines.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC::JITCache {

namespace UCBFeedbackInternal {

static_assert(std::endian::native == std::endian::little, "ucb.feedback is little-endian, as every JITCache target is");

// The header of ucb.feedback (SPEC-ucb.md section 5.2).
static constexpr uint32_t sectionMagic = 0x46424355;
static constexpr uint16_t layoutVersion = 1;
static constexpr size_t magicOffset = 0;
static constexpr size_t versionOffset = 4;
static constexpr size_t reservedOffset = 6;
static constexpr size_t valueProfileCountOffset = 8;
static constexpr size_t arrayProfileCountOffset = 12;
static constexpr size_t binaryArithProfileCountOffset = 16;
static constexpr size_t unaryArithProfileCountOffset = 20;
static constexpr size_t exitSiteCountOffset = 24;
static constexpr size_t functionDeclCountOffset = 28;
static constexpr size_t functionExprCountOffset = 32;
static constexpr size_t didOptimizeOffset = 36;
static constexpr size_t quickDFGTierUpOffset = 37;
static constexpr size_t quickFTLTierUpOffset = 38;
static constexpr size_t reservedByteOffset = 39;
static constexpr size_t llintActiveThresholdOffset = 40;
static constexpr size_t llintTotalCountOffset = 44;
static constexpr size_t llintCounterOffset = 48;
static constexpr size_t constantCountOffset = 52;
static constexpr size_t headerSize = 56;

static constexpr uint32_t countLimit = 1u << 28;
static constexpr size_t arrayProfileSize = 8; // u32 observedArrayModes, u32 flags
static constexpr size_t arithProfileSize = 2;
static constexpr size_t exitSiteSize = 8; // u32 BytecodeIndex::asBits(), u8 ExitKind, u8 ExitingJITType, u8 ExitingInlineKind, u8 0
static constexpr size_t exitSiteKindOffset = 4;
static constexpr size_t exitSiteJITTypeOffset = 5;
static constexpr size_t exitSiteInlineKindOffset = 6;
static constexpr size_t exitSitePaddingOffset = 7;

// The TriState bytes hold the enumerators' own values.
static_assert(!static_cast<uint8_t>(TriState::False) && static_cast<uint8_t>(TriState::True) == 1 && static_cast<uint8_t>(TriState::Indeterminate) == 2);

// The last ExitKind the strict check accepts. A WebKit bump that adds a kind must move it.
static constexpr ExitKind lastExitKind = UnexpectedResizableArrayBufferView;
static_assert(static_cast<unsigned>(lastExitKind) == 31, "ExitKind changed: make lastExitKind its last enumerator");

// The flags a UCB copy can hold: every ArrayProfileFlag but the pruning mark, which UnlinkedArrayProfile::update keeps out.
static constexpr OptionSet<ArrayProfileFlag> restorableArrayProfileFlags {
    ArrayProfileFlag::MayStoreHole,
    ArrayProfileFlag::OutOfBounds,
    ArrayProfileFlag::MayBeLargeTypedArray,
    ArrayProfileFlag::MayInterceptIndexedAccesses,
    ArrayProfileFlag::UsesNonOriginalArrayStructures,
    ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray,
    ArrayProfileFlag::MayBeRegExpMatchesArray,
};

// An arithmetic profile's categories (section 5.6): a binary profile's seven results and both operand types, below its
// special fast-path bit, and a unary profile's results and argument type.
static constexpr unsigned binaryCategoryBits = ObservedResults::numBitsNeeded + 2 * ObservedType::numBitsNeeded;
static constexpr unsigned unaryCategoryBits = ObservedResults::numBitsNeeded + ObservedType::numBitsNeeded;
static_assert(BinaryArithProfile::specialFastPathBit == 1u << binaryCategoryBits);
static constexpr uint32_t binaryCategoryMask = (1u << binaryCategoryBits) - 1;
static constexpr uint32_t unaryCategoryMask = (1u << unaryCategoryBits) - 1;
static constexpr uint32_t binaryBitsLimit = BinaryArithProfile::specialFastPathBit << 1;
static constexpr uint32_t unaryBitsLimit = 1u << unaryCategoryBits;

template<typename T>
static T readField(std::span<const uint8_t> bytes, size_t offset)
{
    std::array<uint8_t, sizeof(T)> raw;
    memcpySpan(std::span { raw }, bytes.subspan(offset, sizeof(T)));
    return std::bit_cast<T>(raw);
}

template<typename T>
static void writeField(std::span<uint8_t> bytes, size_t offset, const T& value)
{
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

static bool isZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

template<size_t alignment>
static CheckedSize alignedTo(CheckedSize offset)
{
    static_assert(isPowerOfTwo(alignment));
    CheckedSize padded = offset + (alignment - 1);
    if (padded.hasOverflowed())
        return padded;
    return padded.value() & ~(alignment - 1);
}

static size_t constantBitsSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

// The regions after the header, in layout order, with checked arithmetic. The counts are 32-bit, so no offset overflows
// a 64-bit size_t; a strict parse checks anyway.
static CheckedSize arraysOffset(const FeedbackCounts& counts)
{
    return CheckedSize(headerSize) + CheckedSize(counts.valueProfiles) * sizeof(SpeculatedType);
}

static CheckedSize binaryOffset(const FeedbackCounts& counts)
{
    return arraysOffset(counts) + CheckedSize(counts.arrayProfiles) * arrayProfileSize;
}

static CheckedSize unaryOffset(const FeedbackCounts& counts)
{
    return binaryOffset(counts) + CheckedSize(counts.binaryArithProfiles) * arithProfileSize;
}

static CheckedSize unaryEnd(const FeedbackCounts& counts)
{
    return unaryOffset(counts) + CheckedSize(counts.unaryArithProfiles) * arithProfileSize;
}

static CheckedSize exitsOffset(const FeedbackCounts& counts)
{
    return alignedTo<4>(unaryEnd(counts));
}

static CheckedSize childrenOffset(const FeedbackCounts& counts)
{
    return exitsOffset(counts) + CheckedSize(counts.exitSites) * exitSiteSize;
}

static CheckedSize constantsOffset(const FeedbackCounts& counts)
{
    return childrenOffset(counts) + CheckedSize(counts.functionDecls) + CheckedSize(counts.functionExprs);
}

static CheckedSize constantsEnd(const FeedbackCounts& counts)
{
    return constantsOffset(counts) + constantBitsSize(counts.constants);
}

static CheckedSize sectionSize(const FeedbackCounts& counts)
{
    return alignedTo<8>(constantsEnd(counts));
}

static size_t elementOffset(CheckedSize regionOffset, unsigned index, size_t elementSize)
{
    return (regionOffset + CheckedSize(index) * elementSize).value();
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
    };
}

// The site the record at `offset` describes, as the profile stores it: the constructor counts an ArgumentsEscaped site
// at bytecode index 0, as it does natively.
static DFG::FrequentExitSite exitSiteAt(std::span<const uint8_t> bytes, size_t offset)
{
    return DFG::FrequentExitSite(BytecodeIndex::fromBits(readField<uint32_t>(bytes, offset)), static_cast<ExitKind>(bytes[offset + exitSiteKindOffset]),
        static_cast<ExitingJITType>(bytes[offset + exitSiteJITTypeOffset]), static_cast<ExitingInlineKind>(bytes[offset + exitSiteInlineKindOffset]));
}

static bool isRestorableExitSiteRecord(std::span<const uint8_t> bytes, size_t offset)
{
    uint8_t kind = bytes[offset + exitSiteKindOffset];
    uint8_t jitType = bytes[offset + exitSiteJITTypeOffset];
    uint8_t inlineKind = bytes[offset + exitSiteInlineKindOffset];
    return kind != ExitKindUnset && kind <= lastExitKind
        && (jitType == ExitFromDFG || jitType == ExitFromFTL)
        && (inlineKind == ExitFromNotInlined || inlineKind == ExitFromInlined)
        && !bytes[offset + exitSitePaddingOffset];
}

static bool isTriState(uint8_t byte)
{
    return byte <= static_cast<uint8_t>(TriState::Indeterminate);
}

// The rules of section 5.2, which strict checks before anything reads the section.
static bool obeysFeedbackRules(std::span<const uint8_t> bytes)
{
    if (bytes.size() < headerSize)
        return false;
    if (readField<uint32_t>(bytes, magicOffset) != sectionMagic || readField<uint16_t>(bytes, versionOffset) != layoutVersion || readField<uint16_t>(bytes, reservedOffset))
        return false;
    FeedbackCounts counts = countsIn(bytes);
    for (uint32_t count : { counts.valueProfiles, counts.arrayProfiles, counts.binaryArithProfiles, counts.unaryArithProfiles, counts.exitSites, counts.functionDecls, counts.functionExprs, counts.constants }) {
        if (count >= countLimit)
            return false;
    }
    CheckedSize size = sectionSize(counts);
    if (size.hasOverflowed() || size.value() != bytes.size())
        return false;

    // Native value profiles see only boxed values, so no prediction carries SpecInt52Any or SpecDoubleImpureNaN.
    for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
        if (readField<SpeculatedType>(bytes, headerSize + i * sizeof(SpeculatedType)) & ~SpecBytecodeTop)
            return false;
    }

    size_t arrays = arraysOffset(counts).value();
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        size_t offset = arrays + i * arrayProfileSize;
        if ((readField<uint32_t>(bytes, offset) & ~ALL_ARRAY_MODES) || (readField<uint32_t>(bytes, offset + 4) & ~restorableArrayProfileFlags.toRaw()))
            return false;
    }

    size_t binary = binaryOffset(counts).value();
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i) {
        if (readField<uint16_t>(bytes, binary + i * arithProfileSize) >= binaryBitsLimit)
            return false;
    }
    size_t unary = unaryOffset(counts).value();
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i) {
        if (readField<uint16_t>(bytes, unary + i * arithProfileSize) >= unaryBitsLimit)
            return false;
    }

    size_t exits = exitsOffset(counts).value();
    size_t alignmentGap = unaryEnd(counts).value();
    if (!isZero(bytes.subspan(alignmentGap, exits - alignmentGap)))
        return false;
    // Quadratic, as ExitProfile::add is when it builds the list, which keeps native lists short.
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        size_t offset = exits + i * exitSiteSize;
        if (!isRestorableExitSiteRecord(bytes, offset))
            return false;
        DFG::FrequentExitSite site = exitSiteAt(bytes, offset);
        for (uint32_t j = 0; j < i; ++j) {
            if (exitSiteAt(bytes, exits + j * exitSiteSize) == site)
                return false;
        }
    }

    if (!isTriState(bytes[didOptimizeOffset]) || !isTriState(bytes[quickDFGTierUpOffset]) || bytes[quickFTLTierUpOffset] > 1 || bytes[reservedByteOffset])
        return false;

    // The counter is either in the state ExecutionCounter::deferIndefinitely leaves or armed with a progress in [0, 2^31).
    auto threshold = readField<int32_t>(bytes, llintActiveThresholdOffset);
    auto totalCount = readField<float>(bytes, llintTotalCountOffset);
    auto counter = readField<int32_t>(bytes, llintCounterOffset);
    bool deferred = threshold == std::numeric_limits<int32_t>::max() && !totalCount && counter == std::numeric_limits<int32_t>::min();
    double progress = static_cast<double>(totalCount) + counter;
    bool armed = threshold >= 0 && std::isfinite(totalCount) && progress >= 0 && progress < 0x1p31;
    if (!deferred && !armed)
        return false;

    size_t children = childrenOffset(counts).value();
    for (uint8_t byte : bytes.subspan(children, static_cast<size_t>(counts.functionDecls) + counts.functionExprs)) {
        if (byte > 1)
            return false;
    }

    size_t constants = constantsOffset(counts).value();
    size_t constantBytes = constantBitsSize(counts.constants);
    if (uint32_t usedBits = counts.constants % 8; usedBits && (bytes[constants + constantBytes - 1] >> usedBits))
        return false;
    return isZero(bytes.subspan(constants + constantBytes));
}

// The UCB's tier-up history (section 5.1): didOptimize and the quick tier-up bits. m_quickDFGTierUp and m_quickFTLTierUp
// are bit-fields adjacent to m_age, so the three share one memory location, and a marker's first visit to the UCB in a
// cycle rewrites m_age under cellLock() (UnlinkedCodeBlock::visitChildrenImpl). The same visit reads, under that lock,
// the metadata table's bit-fields, which hold m_didOptimize (metadataSizeInBytes). A marker reaches even a UCB that no
// holder has published once a stopped-world phase finds it on the VM thread's stack, as at the end of an import's DeferGC
// scopes (SPEC-ucb.md section 7.6). Seeding therefore writes these fields under that lock, since an unlocked seed could
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

// C3: every count the body shares with the UCB's index spaces. The exit sites are feedback, not an index space.
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

} // namespace UCBFeedbackInternal

std::optional<FeedbackSection> FeedbackSection::parse(std::span<const uint8_t> bytes, bool strict)
{
    using namespace UCBFeedbackInternal;

    if (strict && !obeysFeedbackRules(bytes))
        return std::nullopt;

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
    return readField<SpeculatedType>(m_bytes, elementOffset(headerSize, index, sizeof(SpeculatedType)));
}

ArrayModes FeedbackSection::observedArrayModes(unsigned index) const
{
    using namespace UCBFeedbackInternal;
    return readField<uint32_t>(m_bytes, elementOffset(arraysOffset(m_counts), index, arrayProfileSize));
}

OptionSet<ArrayProfileFlag> FeedbackSection::arrayProfileFlags(unsigned index) const
{
    using namespace UCBFeedbackInternal;
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
    CheckedSize tableOffset = childrenOffset(m_counts);
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

FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock& ucb)
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
    };
}

bool feedbackFits(UnlinkedCodeBlock& ucb, const FeedbackSection& section)
{
    const FeedbackCounts& counts = section.counts();
    if (!UCBFeedbackInternal::countsFit(ucb, counts))
        return false;
    // C7.
    unsigned instructionsSize = ucb.instructionsSize();
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        if (section.exitSite(i).bytecodeIndex().offset() >= instructionsSize)
            return false;
    }
    return true;
}

bool constantBitsFit(UnlinkedCodeBlock& ucb, const FeedbackSection& section)
{
    uint32_t constantCount = section.counts().constants;
    size_t registerCount = ucb.constantRegisters().size();
    for (uint32_t i = 0; i < constantCount; ++i) {
        if (!section.constantSingletonInvalidated(i))
            continue;
        if (i >= registerCount || !UCBFeedbackInternal::symbolTableAt(ucb, i))
            return false;
    }
    return true;
}

// Takes the UCB's cell lock for step 5, so the caller must not hold it.
void seedFeedback(VM& vm, UnlinkedCodeBlock& ucb, const FeedbackSection& section)
{
    const FeedbackCounts& counts = section.counts();
    // With strict on, C3 and C9 have checked the counts and the constants; normal mode trusts the body (THREAD Session).
    ASSERT(UCBFeedbackInternal::countsFit(ucb, counts));

    auto& valueProfiles = ucb.unlinkedValueProfiles();
    for (uint32_t i = 0; i < counts.valueProfiles; ++i)
        valueProfiles[i].restorePrediction(section.prediction(i));

    auto& arrayProfiles = ucb.unlinkedArrayProfiles();
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i)
        arrayProfiles[i].restoreAccumulatedState(section.observedArrayModes(i), section.arrayProfileFlags(i));

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

    // Step 5, under the cell lock that a marker's update of m_age takes (TierUpHistory).
    UCBFeedbackInternal::restoreTierUpHistory(ucb, { section.didOptimize(), section.quickDFGTierUp(), section.quickFTLTierUp() });

    armLLIntCounter(ucb.llintExecuteCounter(), section.llintActiveThreshold(), section.llintProgress());

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
        // No CodeBlock has cloned this table, so its singleton has no watcher and invalidate only stores the state (F14).
        SymbolTable* symbolTable = UCBFeedbackInternal::symbolTableAt(ucb, i);
        ASSERT(symbolTable && !symbolTable->singleton().isBeingWatched());
        symbolTable->singleton().invalidate(vm, StringFireDetail("JITCache: captured singleton invalidation"));
    }
}

size_t feedbackSectionSize(const FeedbackCounts& counts)
{
    return UCBFeedbackInternal::sectionSize(counts).value();
}

void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock& ucb, const FeedbackCounts& counts)
{
    using namespace UCBFeedbackInternal;

    RELEASE_ASSERT(out.size() == feedbackSectionSize(counts));
    ASSERT(countsFit(ucb, counts));

    zeroSpan(out);
    writeField<uint32_t>(out, magicOffset, sectionMagic);
    writeField<uint16_t>(out, versionOffset, layoutVersion);
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
    BaselineExecutionCounter& counter = ucb.llintExecuteCounter();
    writeField<int32_t>(out, llintActiveThresholdOffset, counter.m_activeThreshold);
    writeField<float>(out, llintTotalCountOffset, counter.m_totalCount);
    writeField<int32_t>(out, llintCounterOffset, counter.m_counter);
    writeField<uint32_t>(out, constantCountOffset, counts.constants);

    // Accumulated state only, from its owner: no sample is read, classified or cleared (THREAD Capture).
    auto& valueProfiles = ucb.unlinkedValueProfiles();
    for (uint32_t i = 0; i < counts.valueProfiles; ++i)
        writeField<SpeculatedType>(out, headerSize + i * sizeof(SpeculatedType), valueProfiles[i].prediction());

    auto& arrayProfiles = ucb.unlinkedArrayProfiles();
    size_t arrays = arraysOffset(counts).value();
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        size_t offset = arrays + i * arrayProfileSize;
        writeField<uint32_t>(out, offset, arrayProfiles[i].observedArrayModes());
        writeField<uint32_t>(out, offset + 4, arrayProfiles[i].arrayProfileFlags().toRaw());
    }

    size_t binary = binaryOffset(counts).value();
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i)
        writeField<uint16_t>(out, binary + i * arithProfileSize, static_cast<uint16_t>(ucb.binaryArithProfile(i).bits()));
    size_t unary = unaryOffset(counts).value();
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i)
        writeField<uint16_t>(out, unary + i * arithProfileSize, static_cast<uint16_t>(ucb.unaryArithProfile(i).bits()));

    {
        // The sites cannot change between the count and this write (F17); the bound only keeps the write in its region.
        size_t offset = exitsOffset(counts).value();
        size_t end = childrenOffset(counts).value();
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

    size_t children = childrenOffset(counts).value();
    for (uint32_t i = 0; i < counts.functionDecls; ++i)
        out[children + i] = ucb.functionDecl(i)->singletonHasBeenInvalidated();
    children += counts.functionDecls;
    for (uint32_t i = 0; i < counts.functionExprs; ++i)
        out[children + i] = ucb.functionExpr(i)->singletonHasBeenInvalidated();

    size_t constants = constantsOffset(counts).value();
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
    // The slice native setThreshold would arm, truncated first so that the progress is kept exactly (section 5.5).
    auto slice = static_cast<int32_t>(BaselineExecutionCounter::clippedThreshold(nullptr, remaining));
    counter.m_counter = -slice;
    counter.m_totalCount = static_cast<float>(progress + slice);
}

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

std::optional<UCBRichness> savedRichness(std::span<const uint8_t> feedbackSection, bool strict)
{
    using namespace UCBFeedbackInternal;

    auto section = FeedbackSection::parse(feedbackSection, strict);
    if (!section)
        return std::nullopt;
    const FeedbackCounts& counts = section->counts();
    UCBRichness richness;
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i)
        richness.arithmeticUnits += binaryArithUnits(section->binaryArithBits(i));
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i)
        richness.arithmeticUnits += unaryArithUnits(section->unaryArithBits(i));
    richness.exitSiteUnits = counts.exitSites;
    return richness;
}

} // namespace JSC::JITCache
