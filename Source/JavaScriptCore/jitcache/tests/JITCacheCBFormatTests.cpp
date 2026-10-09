#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "JITCacheCBFormat.h"
#include "JITCacheTest.h"
#include <array>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <type_traits>
#include <wtf/text/MakeString.h>

// U1 of SPEC-cb.md section 11.2: the layouts of sections 3.2 and 3.3 and the sizes of both sections on edge counts (I10),
// plus the family table and the domain predicates that index and bound those layouts.

namespace JSC::JITCache::Tests {

namespace CBFormatTestsInternal {

// The largest value of a count field, and the same value for 64-bit arithmetic on the expected sizes.
static constexpr uint32_t largestCount = std::numeric_limits<uint32_t>::max();
static constexpr uint64_t maxCount = largestCount;

static void checkEqual(TestContext& context, const char* file, int line, ASCIILiteral what, uint64_t actual, uint64_t expected)
{
    if (actual != expected)
        context.fail(file, line, makeString(what, ": "_s, actual, " instead of "_s, expected));
}

#define JITCACHE_CB_CHECK_EQUAL(what, actual, expected) \
    CBFormatTestsInternal::checkEqual(context, __FILE__, __LINE__, what ""_s, static_cast<uint64_t>(actual), static_cast<uint64_t>(expected))

static uint64_t paddedSize(uint64_t unpadded)
{
    return (unpadded + 7) / 8 * 8;
}

static CBFormat::StateHeader emptyStateHeader()
{
    CBFormat::StateHeader header { };
    header.layoutVersion = CBFormat::stateLayoutVersion;
    header.tier = static_cast<uint8_t>(CBFormat::Tier::Baseline);
    header.counterMode = static_cast<uint8_t>(CBFormat::CounterMode::Carried);
    return header;
}

static CBFormat::SummaryHeader emptySummaryHeader()
{
    CBFormat::SummaryHeader header { };
    header.layoutVersion = CBFormat::summaryLayoutVersion;
    header.tier = static_cast<uint8_t>(CBFormat::Tier::Baseline);
    header.counterMode = static_cast<uint8_t>(CBFormat::CounterMode::Carried);
    return header;
}

// Expected arrays are given by their byte lengths, in section order, so each check recomputes every offset from the
// SPEC's element sizes rather than from the code under test.
template<typename ArrayID, unsigned numberOfArrays>
static void checkLayout(TestContext& context, const char* file, int line, const std::optional<CBFormat::SectionLayout<ArrayID, numberOfArrays>>& layout,
    uint64_t headerSize, const std::array<uint64_t, numberOfArrays>& expectedCounts, const std::array<uint64_t, numberOfArrays>& expectedBytes)
{
    if (!layout) {
        context.fail(file, line, "the layout overflowed"_s);
        return;
    }
    uint64_t end = headerSize;
    for (unsigned index = 0; index < numberOfArrays; ++index) {
        auto array = static_cast<ArrayID>(index);
        checkEqual(context, file, line, "array offset"_s, layout->offset(array), end);
        checkEqual(context, file, line, "array count"_s, layout->count(array), expectedCounts[index]);
        if (expectedCounts[index])
            checkEqual(context, file, line, "last element offset"_s, layout->elementOffset(array, expectedCounts[index] - 1) + CBFormat::elementSize(array), end + expectedBytes[index]);
        end += expectedBytes[index];
    }
    checkEqual(context, file, line, "padding offset"_s, layout->paddingOffset, end);
    checkEqual(context, file, line, "size"_s, layout->size, paddedSize(end));
}

} // namespace CBFormatTestsInternal

JITCACHE_TEST(cbFormatStructLayout, No)
{
    using CBFormat::ArrayProfileRecord;
    using CBFormat::LazyOperandRecord;
    using CBFormat::StateHeader;
    using CBFormat::SummaryHeader;

    JITCACHE_CB_CHECK_EQUAL("sizeof(StateHeader)", sizeof(StateHeader), 160);
    JITCACHE_CB_CHECK_EQUAL("alignof(StateHeader)", alignof(StateHeader), 4);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::layoutVersion", offsetof(StateHeader, layoutVersion), 0);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::tier", offsetof(StateHeader, tier), 2);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::counterMode", offsetof(StateHeader, counterMode), 3);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::numArguments", offsetof(StateHeader, numArguments), 4);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::numValueProfiles", offsetof(StateHeader, numValueProfiles), 8);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::numLazyOperandProfiles", offsetof(StateHeader, numLazyOperandProfiles), 12);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::optimizationDelayCounter", offsetof(StateHeader, optimizationDelayCounter), 16);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::reoptimizationRetryCounter", offsetof(StateHeader, reoptimizationRetryCounter), 18);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::counterValue", offsetof(StateHeader, counterValue), 20);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::counterTotalCount", offsetof(StateHeader, counterTotalCount), 24);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::counterActiveThreshold", offsetof(StateHeader, counterActiveThreshold), 28);
    JITCACHE_CB_CHECK_EQUAL("StateHeader::familyEntryCount", offsetof(StateHeader, familyEntryCount), 32);
    JITCACHE_CB_CHECK_EQUAL("sizeof(StateHeader::familyEntryCount)", sizeof(StateHeader::familyEntryCount), 31 * sizeof(uint32_t));
    JITCACHE_CB_CHECK_EQUAL("StateHeader::reserved1", offsetof(StateHeader, reserved1), 156);

    JITCACHE_CB_CHECK_EQUAL("sizeof(ArrayProfileRecord)", sizeof(ArrayProfileRecord), 8);
    JITCACHE_CB_CHECK_EQUAL("ArrayProfileRecord::observedArrayModes", offsetof(ArrayProfileRecord, observedArrayModes), 0);
    JITCACHE_CB_CHECK_EQUAL("ArrayProfileRecord::flags", offsetof(ArrayProfileRecord, flags), 4);

    JITCACHE_CB_CHECK_EQUAL("sizeof(LazyOperandRecord)", sizeof(LazyOperandRecord), 24);
    JITCACHE_CB_CHECK_EQUAL("alignof(LazyOperandRecord)", alignof(LazyOperandRecord), 8);
    JITCACHE_CB_CHECK_EQUAL("LazyOperandRecord::bytecodeIndexBits", offsetof(LazyOperandRecord, bytecodeIndexBits), 0);
    JITCACHE_CB_CHECK_EQUAL("LazyOperandRecord::operandKind", offsetof(LazyOperandRecord, operandKind), 4);
    JITCACHE_CB_CHECK_EQUAL("LazyOperandRecord::operandValue", offsetof(LazyOperandRecord, operandValue), 8);
    JITCACHE_CB_CHECK_EQUAL("LazyOperandRecord::reserved", offsetof(LazyOperandRecord, reserved), 12);
    JITCACHE_CB_CHECK_EQUAL("LazyOperandRecord::prediction", offsetof(LazyOperandRecord, prediction), 16);

    JITCACHE_CB_CHECK_EQUAL("sizeof(SummaryHeader)", sizeof(SummaryHeader), 24);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::layoutVersion", offsetof(SummaryHeader, layoutVersion), 0);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::tier", offsetof(SummaryHeader, tier), 2);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::counterMode", offsetof(SummaryHeader, counterMode), 3);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::numArguments", offsetof(SummaryHeader, numArguments), 4);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::numValueProfiles", offsetof(SummaryHeader, numValueProfiles), 8);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::numArrayProfiles", offsetof(SummaryHeader, numArrayProfiles), 12);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::numLazyOperandProfiles", offsetof(SummaryHeader, numLazyOperandProfiles), 16);
    JITCACHE_CB_CHECK_EQUAL("SummaryHeader::counterProgress", offsetof(SummaryHeader, counterProgress), 20);

    JITCACHE_CB_CHECK_EQUAL("stateLayoutVersion", CBFormat::stateLayoutVersion, 1);
    JITCACHE_CB_CHECK_EQUAL("summaryLayoutVersion", CBFormat::summaryLayoutVersion, 1);
    JITCACHE_CB_CHECK_EQUAL("Tier::Baseline", static_cast<uint8_t>(CBFormat::Tier::Baseline), 1);
    JITCACHE_CB_CHECK_EQUAL("CounterMode::NotCarried", static_cast<uint8_t>(CBFormat::CounterMode::NotCarried), 0);
    JITCACHE_CB_CHECK_EQUAL("CounterMode::Carried", static_cast<uint8_t>(CBFormat::CounterMode::Carried), 1);
}

JITCACHE_TEST(cbFormatStateSectionSize, No)
{
    using CBFormat::StateArray;
    using CBFormatTestsInternal::checkLayout;
    using CBFormatTestsInternal::largestCount;
    using CBFormatTestsInternal::maxCount;

    // No counts: the header alone, already a multiple of 8.
    auto header = CBFormatTestsInternal::emptyStateHeader();
    checkLayout<StateArray, CBFormat::numberOfStateArrays>(context, __FILE__, __LINE__, CBFormat::stateLayout(header), 160, { }, { });
    JITCACHE_CB_CHECK_EQUAL("stateSectionSize, no counts", CBFormat::stateSectionSize(header).value_or(0), 160);

    // One or more records in every array, with the kinds' families spread over their ranges.
    header.numArguments = 2;
    header.numValueProfiles = 3;
    header.numLazyOperandProfiles = 1;
    header.familyEntryCount[CBFormat::firstArrayProfileFamily] = 1; // A0
    header.familyEntryCount[CBFormat::numberOfArrayProfileFamilies - 1] = 2; // A15
    header.familyEntryCount[CBFormat::firstAllocationHintFamily] = 1; // F16
    header.familyEntryCount[CBFormat::firstAllocationHintFamily + 3] = 2; // F19
    header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily] = 1; // F20
    header.familyEntryCount[CBFormat::asyncIteratorNextIterationModesFamily] = 1; // F23
    header.familyEntryCount[CBFormat::firstEnumeratorModesFamily] = 1; // F24
    header.familyEntryCount[CBFormat::firstEnumeratorModesFamily + 4] = 2; // F28
    header.familyEntryCount[CBFormat::toThisStatusFamily] = 1; // F29
    header.familyEntryCount[CBFormat::branchBitFamily] = 1; // F30
    checkLayout<StateArray, CBFormat::numberOfStateArrays>(context, __FILE__, __LINE__, CBFormat::stateLayout(header), 160,
        { 2, 3, 3, 1, 3, 2, 3, 1, 1 },
        { 16, 24, 24, 24, 6, 4, 3, 1, 1 });
    JITCACHE_CB_CHECK_EQUAL("stateSectionSize, every array", CBFormat::stateSectionSize(header).value_or(0), 264);
    JITCACHE_CB_CHECK_EQUAL("first record of F19", CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::firstAllocationHintFamily + 3), 1);
    JITCACHE_CB_CHECK_EQUAL("first record of F28", CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::firstEnumeratorModesFamily + 4), 1);
    JITCACHE_CB_CHECK_EQUAL("first record of A15", CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::numberOfArrayProfileFamilies - 1), 1);
    JITCACHE_CB_CHECK_EQUAL("first record of F29", CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::toThisStatusFamily), 0);

    // Padding: the byte arrays end the section, so their total decides it.
    for (uint32_t branchBits : { 7u, 8u, 9u }) {
        auto padded = CBFormatTestsInternal::emptyStateHeader();
        padded.familyEntryCount[CBFormat::branchBitFamily] = branchBits;
        auto layout = CBFormat::stateLayout(padded);
        JITCACHE_CB_CHECK_EQUAL("padding offset", layout ? layout->paddingOffset : 0, 160 + branchBits);
        JITCACHE_CB_CHECK_EQUAL("padded size", CBFormat::stateSectionSize(padded).value_or(0), CBFormatTestsInternal::paddedSize(160 + branchBits));
    }

    // Every count at its maximum. Every count is a uint32_t, so on the 64-bit targets JITCache supports the size stays below
    // 2^40 and the overflow path cannot be reached; the computation must still give the exact value.
    static_assert(sizeof(size_t) == sizeof(uint64_t));
    auto maximal = CBFormatTestsInternal::emptyStateHeader();
    maximal.numArguments = largestCount;
    maximal.numValueProfiles = largestCount;
    maximal.numLazyOperandProfiles = largestCount;
    maximal.familyEntryCount.fill(largestCount);
    checkLayout<StateArray, CBFormat::numberOfStateArrays>(context, __FILE__, __LINE__, CBFormat::stateLayout(maximal), 160,
        { maxCount, maxCount, 16 * maxCount, maxCount, 4 * maxCount, 4 * maxCount, 5 * maxCount, maxCount, maxCount },
        { 8 * maxCount, 8 * maxCount, 16 * 8 * maxCount, 24 * maxCount, 4 * 2 * maxCount, 4 * 2 * maxCount, 5 * maxCount, maxCount, maxCount });
    JITCACHE_CB_CHECK_EQUAL("stateSectionSize, maximal counts", CBFormat::stateSectionSize(maximal).value_or(0), CBFormatTestsInternal::paddedSize(160 + 191 * maxCount));
}

JITCACHE_TEST(cbFormatSummarySectionSize, No)
{
    using CBFormat::SummaryArray;
    using CBFormatTestsInternal::checkLayout;
    using CBFormatTestsInternal::largestCount;
    using CBFormatTestsInternal::maxCount;

    auto header = CBFormatTestsInternal::emptySummaryHeader();
    checkLayout<SummaryArray, CBFormat::numberOfSummaryArrays>(context, __FILE__, __LINE__, CBFormat::summaryLayout(header), 24, { }, { });
    JITCACHE_CB_CHECK_EQUAL("summarySectionSize, no counts", CBFormat::summarySectionSize(header).value_or(0), 24);

    header.numArguments = 1;
    header.numValueProfiles = 2;
    header.numLazyOperandProfiles = 1;
    header.numArrayProfiles = 3;
    checkLayout<SummaryArray, CBFormat::numberOfSummaryArrays>(context, __FILE__, __LINE__, CBFormat::summaryLayout(header), 24,
        { 1, 2, 1, 3 },
        { 8, 16, 8, 3 });
    JITCACHE_CB_CHECK_EQUAL("summarySectionSize, every array", CBFormat::summarySectionSize(header).value_or(0), 64);

    for (uint32_t arrayProfiles : { 7u, 8u, 9u }) {
        auto padded = CBFormatTestsInternal::emptySummaryHeader();
        padded.numArrayProfiles = arrayProfiles;
        JITCACHE_CB_CHECK_EQUAL("padded summary size", CBFormat::summarySectionSize(padded).value_or(0), CBFormatTestsInternal::paddedSize(24 + arrayProfiles));
    }

    auto maximal = CBFormatTestsInternal::emptySummaryHeader();
    maximal.numArguments = largestCount;
    maximal.numValueProfiles = largestCount;
    maximal.numLazyOperandProfiles = largestCount;
    maximal.numArrayProfiles = largestCount;
    checkLayout<SummaryArray, CBFormat::numberOfSummaryArrays>(context, __FILE__, __LINE__, CBFormat::summaryLayout(maximal), 24,
        { maxCount, maxCount, maxCount, maxCount },
        { 8 * maxCount, 8 * maxCount, 8 * maxCount, maxCount });
    JITCACHE_CB_CHECK_EQUAL("summarySectionSize, maximal counts", CBFormat::summarySectionSize(maximal).value_or(0), CBFormatTestsInternal::paddedSize(24 + 25 * maxCount));
}

JITCACHE_TEST(cbFormatFamilyTable, No)
{
    using CBFormat::FamilyKind;
    using CBFormat::StateArray;
    using CBFormat::SummaryArray;

    // The kinds occupy the ranges of section 3.1, in table order.
    auto expectedKind = [](unsigned family) {
        if (family < 16)
            return FamilyKind::ArrayProfile;
        if (family < 20)
            return FamilyKind::AllocationHint;
        if (family < 24)
            return FamilyKind::IterationModes;
        if (family < 29)
            return FamilyKind::EnumeratorModes;
        if (family == 29)
            return FamilyKind::ToThisStatus;
        return FamilyKind::BranchBit;
    };
    JITCACHE_CB_CHECK_EQUAL("numberOfFamilies", CBFormat::numberOfFamilies, 31);
    JITCACHE_CB_CHECK_EQUAL("numberOfArrayProfileFamilies", CBFormat::numberOfArrayProfileFamilies, 16);
    unsigned visits = 0;
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        JITCACHE_CB_CHECK_EQUAL("family visited out of order", FamilyType::index, visits);
        JITCACHE_CHECK(FamilyType::kind == expectedKind(FamilyType::index));
        JITCACHE_CHECK(CBFormat::familyKind(FamilyType::index) == expectedKind(FamilyType::index));
        ++visits;
    });
    JITCACHE_CB_CHECK_EQUAL("families visited", visits, 31);

    unsigned families = 0;
    for (auto kind : { FamilyKind::ArrayProfile, FamilyKind::AllocationHint, FamilyKind::IterationModes, FamilyKind::EnumeratorModes, FamilyKind::ToThisStatus, FamilyKind::BranchBit }) {
        JITCACHE_CB_CHECK_EQUAL("first family of a kind", CBFormat::firstFamilyOf(kind), families);
        families += CBFormat::numberOfFamiliesOf(kind);
    }
    JITCACHE_CB_CHECK_EQUAL("families of all kinds", families, 31);

    // The families the SPEC places by opcode.
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<15>::Op, OpIteratorNext>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<CBFormat::newArrayBufferFamily>::Op, OpNewArrayBuffer>));
    JITCACHE_CB_CHECK_EQUAL("newArrayBufferFamily", CBFormat::newArrayBufferFamily, 19);
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<20>::Op, OpIteratorOpen>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<21>::Op, OpIteratorNext>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<22>::Op, OpAsyncIteratorOpen>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<23>::Op, OpAsyncIteratorNext>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<24>::Op, OpEnumeratorNext>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<28>::Op, OpEnumeratorGetByVal>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<29>::Op, OpToThis>));
    JITCACHE_CHECK((std::is_same_v<CBFormat::Family<30>::Op, OpJneqPtr>));

    // Element sizes and the array that holds each kind (sections 3.2 and 3.3).
    JITCACHE_CB_CHECK_EQUAL("argument predictions", CBFormat::elementSize(StateArray::ArgumentPredictions), 8);
    JITCACHE_CB_CHECK_EQUAL("value predictions", CBFormat::elementSize(StateArray::ValuePredictions), 8);
    JITCACHE_CB_CHECK_EQUAL("array profiles", CBFormat::elementSize(StateArray::ArrayProfiles), 8);
    JITCACHE_CB_CHECK_EQUAL("lazy-operand profiles", CBFormat::elementSize(StateArray::LazyOperandProfiles), 24);
    JITCACHE_CB_CHECK_EQUAL("allocation hints", CBFormat::elementSize(StateArray::AllocationHints), 2);
    JITCACHE_CB_CHECK_EQUAL("iteration modes", CBFormat::elementSize(StateArray::IterationModes), 2);
    JITCACHE_CB_CHECK_EQUAL("enumerator modes", CBFormat::elementSize(StateArray::EnumeratorModes), 1);
    JITCACHE_CB_CHECK_EQUAL("to_this statuses", CBFormat::elementSize(StateArray::ToThisStatuses), 1);
    JITCACHE_CB_CHECK_EQUAL("branch bits", CBFormat::elementSize(StateArray::BranchBits), 1);
    JITCACHE_CB_CHECK_EQUAL("argument categories", CBFormat::elementSize(SummaryArray::ArgumentCategories), 8);
    JITCACHE_CB_CHECK_EQUAL("value categories", CBFormat::elementSize(SummaryArray::ValueCategories), 8);
    JITCACHE_CB_CHECK_EQUAL("lazy-operand categories", CBFormat::elementSize(SummaryArray::LazyOperandCategories), 8);
    JITCACHE_CB_CHECK_EQUAL("array flags", CBFormat::elementSize(SummaryArray::ArrayFlags), 1);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::ArrayProfile) == StateArray::ArrayProfiles);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::AllocationHint) == StateArray::AllocationHints);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::IterationModes) == StateArray::IterationModes);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::EnumeratorModes) == StateArray::EnumeratorModes);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::ToThisStatus) == StateArray::ToThisStatuses);
    JITCACHE_CHECK(CBFormat::stateArrayOf(FamilyKind::BranchBit) == StateArray::BranchBits);
}

JITCACHE_TEST(cbFormatDomainPredicates, No)
{
    using CBFormat::allocationHintFits;
    using CBFormat::iterationModesFit;
    using CBFormat::newArrayBufferFamily;

    // V1 and V6 to V7.
    JITCACHE_CHECK(CBFormat::counterModeFits(0) && CBFormat::counterModeFits(1) && !CBFormat::counterModeFits(2));
    JITCACHE_CHECK(CBFormat::predictionFits(SpecNone) && CBFormat::predictionFits(SpecBytecodeTop));
    JITCACHE_CHECK(!CBFormat::predictionFits(SpecFullTop) && !CBFormat::predictionFits(SpecNonInt32AsInt52) && !CBFormat::predictionFits(SpecDoubleImpureNaN));
    JITCACHE_CHECK(CBFormat::arrayModesFit(ALL_ARRAY_MODES) && !CBFormat::arrayModesFit(1u << 2) && !CBFormat::arrayModesFit(1u << 24));
    JITCACHE_CHECK(CBFormat::arrayProfileFlagsFit(0xff) && !CBFormat::arrayProfileFlagsFit(0x100));

    // V8: the vector length, the sets of F16..F19 and the literal order in F19 (U2's mutations).
    unsigned newArrayFamily = CBFormat::firstAllocationHintFamily;
    JITCACHE_CHECK(CBFormat::allocationHintVectorLengthFits(BASE_CONTIGUOUS_VECTOR_LEN_MAX) && !CBFormat::allocationHintVectorLengthFits(BASE_CONTIGUOUS_VECTOR_LEN_MAX + 1));
    uint16_t hint = CBFormat::encodeAllocationHint(CopyOnWriteArrayWithDouble, 17);
    JITCACHE_CHECK(CBFormat::allocationHintIndexingType(hint) == CopyOnWriteArrayWithDouble && CBFormat::allocationHintVectorLength(hint) == 17);
    JITCACHE_CHECK(!allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble));
    JITCACHE_CHECK(!allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithContiguous));
    JITCACHE_CHECK(!allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithContiguous));
    JITCACHE_CHECK(allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithDouble, CopyOnWriteArrayWithInt32));
    JITCACHE_CHECK(allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithContiguous, CopyOnWriteArrayWithInt32));
    JITCACHE_CHECK(allocationHintFits(newArrayBufferFamily, CopyOnWriteArrayWithContiguous, CopyOnWriteArrayWithDouble));
    JITCACHE_CHECK(!allocationHintFits(newArrayBufferFamily, ArrayWithContiguous, CopyOnWriteArrayWithInt32));
    JITCACHE_CHECK(!CBFormat::allocationHintTypeFits(newArrayBufferFamily, ArrayWithContiguous));
    JITCACHE_CHECK(!allocationHintFits(newArrayFamily, CopyOnWriteArrayWithInt32, ArrayWithUndecided));
    JITCACHE_CHECK(allocationHintFits(newArrayFamily, ArrayWithUndecided, ArrayWithUndecided));
    JITCACHE_CHECK(allocationHintFits(newArrayFamily, ArrayWithSlowPutArrayStorage, ArrayWithUndecided));
    JITCACHE_CHECK(!allocationHintFits(newArrayFamily, ArrayClass, ArrayWithUndecided));
    JITCACHE_CHECK(!allocationHintFits(CBFormat::firstIterationModesFamily, ArrayWithInt32, ArrayWithUndecided));

    // V9: each opcode's full native mask, and the bits U2 rejects.
    auto bit = [](IterationMode mode) {
        return static_cast<uint16_t>(mode);
    };
    for (unsigned family = CBFormat::firstIterationModesFamily; family < CBFormat::firstIterationModesFamily + CBFormat::numberOfIterationModesFamilies; ++family)
        JITCACHE_CHECK(iterationModesFit(family, CBFormat::nativeIterationModes(family)));
    JITCACHE_CB_CHECK_EQUAL("iterator_open mask", CBFormat::nativeIterationModes(CBFormat::iteratorOpenIterationModesFamily), 0x1fff);
    JITCACHE_CB_CHECK_EQUAL("iterator_next mask", CBFormat::nativeIterationModes(CBFormat::iteratorNextIterationModesFamily), 0x1ff1);
    JITCACHE_CB_CHECK_EQUAL("async_iterator_open mask", CBFormat::nativeIterationModes(CBFormat::asyncIteratorOpenIterationModesFamily), 0x6001);
    JITCACHE_CB_CHECK_EQUAL("async_iterator_next mask", CBFormat::nativeIterationModes(CBFormat::asyncIteratorNextIterationModesFamily), 0x2001);
    JITCACHE_CHECK(!iterationModesFit(CBFormat::iteratorOpenIterationModesFamily, bit(IterationMode::FastAsyncGenerator)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::iteratorOpenIterationModesFamily, bit(IterationMode::AsyncFromSync)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::iteratorNextIterationModesFamily, bit(IterationMode::FastArray)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::iteratorNextIterationModesFamily, bit(IterationMode::FastMap)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::iteratorNextIterationModesFamily, bit(IterationMode::FastSet)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::asyncIteratorOpenIterationModesFamily, bit(IterationMode::FastArray)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::asyncIteratorNextIterationModesFamily, bit(IterationMode::AsyncFromSync)));
    JITCACHE_CHECK(!iterationModesFit(CBFormat::firstAllocationHintFamily, bit(IterationMode::Generic)));

    // V10 to V12.
    JITCACHE_CHECK(CBFormat::enumeratorModesFit(0x0f) && !CBFormat::enumeratorModesFit(0x10));
    JITCACHE_CHECK(CBFormat::toThisStatusFits(ToThisOK) && CBFormat::toThisStatusFits(ToThisConflicted) && CBFormat::toThisStatusFits(ToThisClearedByGC));
    JITCACHE_CHECK(!CBFormat::toThisStatusFits(3));
    JITCACHE_CHECK(CBFormat::branchBitFits(0) && CBFormat::branchBitFits(1) && !CBFormat::branchBitFits(2));
}

#undef JITCACHE_CB_CHECK_EQUAL

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
