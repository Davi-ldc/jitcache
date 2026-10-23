#include "config.h"
#include "jitcache/cb/CBFormat.h"

#if ENABLE(JIT)

#include "ArrayConventions.h"
#include "CallLinkInfo.h"
#include "JSPropertyNameEnumerator.h"
#include "SpeculatedType.h"
#include "ToThisStatus.h"
#include <stdint.h>
#include <type_traits>
#include <wtf/CheckedArithmetic.h>

// The family table's static checks, which fail the build when a native macro or metadata field no longer matches what
// cb.state stores, the family counts of a CodeBlock, and the layouts and sizes of both sections, computed from their
// headers with overflow-checked arithmetic.

namespace JSC::JITCache::CBFormat {

namespace CBFormatInternal {

// The type of each kind's metadata field, which the kind's cb.state record stores, and of iterator_close_check's, the one
// iteration-mode field that is a byte.
template<FamilyKind> struct ExpectedField;
template<> struct ExpectedField<FamilyKind::ArrayProfile> {
    using Type = ArrayProfile;
};
template<> struct ExpectedField<FamilyKind::AllocationHint> {
    using Type = ArrayAllocationProfile;
};
template<> struct ExpectedField<FamilyKind::IterationModes> {
    using Type = IterationModeMetadata;
};
template<> struct ExpectedField<FamilyKind::EnumeratorModes> {
    using Type = EnumeratorMetadata;
};
template<> struct ExpectedField<FamilyKind::ToThisStatus> {
    using Type = ToThisStatus;
};
template<> struct ExpectedField<FamilyKind::BranchBit> {
    using Type = bool;
};

template<unsigned index>
using ExpectedFieldOf = std::conditional_t<index == iteratorCloseCheckIterationModesFamily, uint8_t, typename ExpectedField<familyKind(index)>::Type>;

// The three call opcodes whose this profile lives in the call site's own CallSiteData: the only families without a
// field.
template<typename Op>
constexpr bool isCallWithThisProfileInCallSiteData = std::is_same_v<Op, OpCall> || std::is_same_v<Op, OpCallIgnoreResult> || std::is_same_v<Op, OpTailCall>;

template<unsigned index>
consteval bool familyIsWellFormed()
{
    using FamilyType = Family<index>;
    if constexpr (FamilyType::hasField) {
        using Field = std::remove_reference_t<decltype(FamilyType::field(std::declval<typename FamilyType::Op::Metadata&>()))>;
        static_assert(std::is_same_v<Field, ExpectedFieldOf<index>>, "a family's metadata field no longer has the type cb.state stores");
        static_assert(!isCallWithThisProfileInCallSiteData<typename FamilyType::Op>, "a call opcode's metadata holds its this profile again");
    } else {
        static_assert(FamilyType::kind == FamilyKind::ArrayProfile, "a family without a field is not an array-profile family");
        static_assert(isCallWithThisProfileInCallSiteData<typename FamilyType::Op>, "an opcode other than OpCall, OpCallIgnoreResult and OpTailCall lost its m_arrayProfile");
        static_assert(std::is_same_v<decltype(std::declval<typename FamilyType::Op::Metadata&>().m_callLinkInfo), LazyCallLinkInfo>, "a call opcode without m_arrayProfile no longer holds a LazyCallLinkInfo");
    }
    static_assert(FamilyType::kind == familyKind(index), "the family table and the kind ranges disagree");
    return true;
}

template<unsigned... indices>
consteval bool everyFamilyIsWellFormed(std::integer_sequence<unsigned, indices...>)
{
    return (familyIsWellFormed<indices>() && ...);
}

consteval unsigned numberOfFamiliesWithoutRecords()
{
    unsigned count = 0;
    for (bool hasRecords : familiesWithRecords)
        count += !hasRecords;
    return count;
}

} // namespace CBFormatInternal

// The family table. A reordered native macro moves the table with the native merge, and a new, moved or renamed field
// fails the build. A new field of one of these types under a new opcode does not, so each WebKit bump compares the
// profile fields of BytecodeList.rb, and every profile reached through another field, with the table by hand.
static_assert(Internal::SimpleArrayProfileOps::size == 15, "FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE no longer lists the 15 opcodes of A0..A14");
static_assert(Internal::AllocationHintOps::size == 4, "FOR_EACH_OPCODE_WITH_ARRAY_ALLOCATION_PROFILE no longer lists the 4 opcodes of F16..F19");
static_assert(Internal::SimpleArrayProfileOps::size + 1 == numberOfArrayProfileFamilies);
static_assert(Internal::AllocationHintOps::size == numberOfAllocationHintFamilies);
static_assert(Internal::IterationModesOps::size + 1 == numberOfIterationModesFamilies);
static_assert(Internal::EnumeratorModesOps::size == numberOfEnumeratorModesFamilies);
static_assert(Internal::Fields::size == numberOfFamilies);
static_assert(!firstArrayProfileFamily);
static_assert(firstAllocationHintFamily == firstArrayProfileFamily + numberOfArrayProfileFamilies);
static_assert(firstIterationModesFamily == firstAllocationHintFamily + numberOfAllocationHintFamilies);
static_assert(firstEnumeratorModesFamily == firstIterationModesFamily + numberOfIterationModesFamilies);
static_assert(toThisStatusFamily == firstEnumeratorModesFamily + numberOfEnumeratorModesFamilies);
static_assert(branchBitFamily == toThisStatusFamily + 1);
static_assert(branchBitFamily + 1 == numberOfFamilies);
static_assert(CBFormatInternal::everyFamilyIsWellFormed(std::make_integer_sequence<unsigned, numberOfFamilies>()));
// Among the 15 opcodes of the macro, exactly OpCall, OpCallIgnoreResult and OpTailCall lack m_arrayProfile.
static_assert(CBFormatInternal::numberOfFamiliesWithoutRecords() == 3, "the families without records are not exactly the three call opcodes");

// The families this table places by hand, and A15 after the macro's opcodes, where
// CodeBlock::updateAllArrayProfilePredictions merges it.
static_assert(std::is_same_v<Family<numberOfArrayProfileFamilies - 1>::Op, OpIteratorNext>);
static_assert(std::is_same_v<Family<iteratorOpenIterationModesFamily>::Op, OpIteratorOpen>);
static_assert(std::is_same_v<Family<iteratorNextIterationModesFamily>::Op, OpIteratorNext>);
static_assert(std::is_same_v<Family<asyncIteratorOpenIterationModesFamily>::Op, OpAsyncIteratorOpen>);
static_assert(std::is_same_v<Family<asyncIteratorNextIterationModesFamily>::Op, OpAsyncIteratorNext>);
static_assert(std::is_same_v<Family<iteratorCloseCheckIterationModesFamily>::Op, OpIteratorCloseCheck>);
static_assert(std::is_same_v<Family<toThisStatusFamily>::Op, OpToThis>);
static_assert(std::is_same_v<Family<branchBitFamily>::Op, OpJneqPtr>);

// Every native value fits the record that stores it.
static_assert(sizeof(ArrayModes) == sizeof(uint32_t));
static_assert(std::is_same_v<std::underlying_type_t<ArrayProfileFlag>, uint32_t>);
static_assert(sizeof(SpeculatedType) == sizeof(uint64_t));
static_assert(sizeof(IndexingType) == sizeof(uint8_t));
static_assert(BASE_CONTIGUOUS_VECTOR_LEN_MAX <= 0xff);
static_assert(std::is_same_v<decltype(IterationModeMetadata::seenModes), uint16_t>);
static_assert(std::is_same_v<EnumeratorMetadata, uint8_t>);
static_assert(ToThisOK <= UINT8_MAX && ToThisConflicted <= UINT8_MAX && ToThisClearedByGC <= UINT8_MAX);

// cb.state's first four arrays and cb.summary's first three have 8-byte elements and follow headers whose sizes are
// multiples of 8, so every element of a section that starts 8-byte aligned is naturally aligned.
static_assert(!(sizeof(StateHeader) % sectionAlignment) && !(sizeof(SummaryHeader) % sectionAlignment));
static_assert(alignof(ArrayProfileRecord) <= sectionAlignment && alignof(LazyOperandRecord) <= sectionAlignment);

FamilyEntryCounts familyEntryCounts(CodeBlock& codeBlock)
{
    FamilyEntryCounts counts { };
    forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        uint32_t count = 0;
        forEachFamilyEntry<typename FamilyType::Op>(codeBlock, [&](typename FamilyType::Op::Metadata&) {
            ++count;
        });
        counts[FamilyType::index] = count;
    });
    return counts;
}

namespace CBFormatInternal {

// The records of a kind's array: the entries of its families that have records, which leaves out A11..A13.
static CheckedSize familyTotal(const StateHeader& header, FamilyKind kind)
{
    CheckedSize total;
    unsigned first = firstFamilyOf(kind);
    for (unsigned family = first; family < first + numberOfFamiliesOf(kind); ++family) {
        if (familyHasRecords(family))
            total += header.familyEntryCount[family];
    }
    return total;
}

static CheckedSize elementCount(const StateHeader& header, StateArray array)
{
    switch (array) {
    case StateArray::ArgumentPredictions:
        return header.numArguments;
    case StateArray::ValuePredictions:
        return header.numValueProfiles;
    case StateArray::ArrayProfiles:
        return familyTotal(header, FamilyKind::ArrayProfile);
    case StateArray::LazyOperandProfiles:
        return header.numLazyOperandProfiles;
    case StateArray::AllocationHints:
        return familyTotal(header, FamilyKind::AllocationHint);
    case StateArray::IterationModes:
        return familyTotal(header, FamilyKind::IterationModes);
    case StateArray::EnumeratorModes:
        return familyTotal(header, FamilyKind::EnumeratorModes);
    case StateArray::ToThisStatuses:
        return familyTotal(header, FamilyKind::ToThisStatus);
    case StateArray::BranchBits:
        return familyTotal(header, FamilyKind::BranchBit);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static CheckedSize elementCount(const SummaryHeader& header, SummaryArray array)
{
    switch (array) {
    case SummaryArray::ArgumentCategories:
        return header.numArguments;
    case SummaryArray::ValueCategories:
        return header.numValueProfiles;
    case SummaryArray::LazyOperandCategories:
        return header.numLazyOperandProfiles;
    case SummaryArray::ArrayFlags:
        return header.numArrayProfiles;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Lays the arrays out after the header, back to back in section order, then pads to sectionAlignment.
template<typename Header, typename ArrayID, unsigned numberOfArrays>
static std::optional<SectionLayout<ArrayID, numberOfArrays>> layOut(const Header& header)
{
    SectionLayout<ArrayID, numberOfArrays> layout;
    CheckedSize end = sizeof(Header);
    for (unsigned index = 0; index < numberOfArrays; ++index) {
        auto array = static_cast<ArrayID>(index);
        CheckedSize count = elementCount(header, array);
        CheckedSize bytes = count * elementSize(array);
        if (end.hasOverflowed() || bytes.hasOverflowed())
            return std::nullopt;
        layout.offsets[index] = end.value();
        layout.counts[index] = count.value();
        end += bytes;
    }
    if (end.hasOverflowed())
        return std::nullopt;
    layout.paddingOffset = end.value();
    end += (sectionAlignment - layout.paddingOffset % sectionAlignment) % sectionAlignment;
    if (end.hasOverflowed())
        return std::nullopt;
    layout.size = end.value();
    return layout;
}

} // namespace CBFormatInternal

std::optional<StateLayout> stateLayout(const StateHeader& header)
{
    return CBFormatInternal::layOut<StateHeader, StateArray, numberOfStateArrays>(header);
}

std::optional<SummaryLayout> summaryLayout(const SummaryHeader& header)
{
    return CBFormatInternal::layOut<SummaryHeader, SummaryArray, numberOfSummaryArrays>(header);
}

std::optional<size_t> stateSectionSize(const StateHeader& header)
{
    auto layout = stateLayout(header);
    if (!layout)
        return std::nullopt;
    return layout->size;
}

std::optional<size_t> summarySectionSize(const SummaryHeader& header)
{
    auto layout = summaryLayout(header);
    if (!layout)
        return std::nullopt;
    return layout->size;
}

} // namespace JSC::JITCache::CBFormat

#endif // ENABLE(JIT)
