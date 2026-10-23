#pragma once

#include <wtf/Platform.h>

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "IndexingType.h"
#include "IterationModeMetadata.h"
#include "MetadataTable.h"
#include "Opcode.h"
#include "ValueProfile.h"
#include <array>
#include <bit>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <tuple>
#include <utility>

// The CB lane's two body-file sections, cb.state and cb.summary: their layouts, the family table that fixes the order of
// the metadata records, the guarded walkers through which the lane reaches a CodeBlock's metadata, and the encoding of
// the fields cb.state does not store as the engine does. Integers and floats are stored in native byte order, and no
// field holds an address, a cell, a StructureID or a time. CBFormat.cpp holds the table's static checks and the size
// functions; the templates and the constexpr helpers live here, where their callers can instantiate and evaluate them.

namespace JSC::JITCache::CBFormat {

static_assert(std::endian::native == std::endian::little, "cb.state and cb.summary store integers and floats in native byte order");

enum class Tier : uint8_t { Baseline = 1 };
// Whether the baseline counter travels. NotCarried marks a body with a polymorphic site, whose consumer keeps the counter
// native setup arms.
enum class CounterMode : uint8_t { NotCarried = 0, Carried = 1 };
static constexpr unsigned numberOfFamilies = 32;
static constexpr unsigned numberOfArrayProfileFamilies = 16; // the first 16 families; each pairs with a UCB copy

// Both sections reach the lane 8-byte aligned and end with zero padding up to a multiple of this.
static constexpr size_t sectionAlignment = 8;

using FamilyEntryCounts = std::array<uint32_t, numberOfFamilies>;

struct StateHeader {
    uint8_t tier; // Tier::Baseline
    uint8_t counterMode; // CounterMode
    uint16_t reserved; // 0, so that the header has no implicit padding
    uint32_t numArguments; // CodeBlock::numParameters()
    uint32_t numValueProfiles; // UnlinkedMetadataTable::numValueProfiles(), 0 without metadata
    uint32_t numLazyOperandProfiles;
    uint16_t optimizationDelayCounter; // CodeBlock::optimizationDelayCounter()
    uint16_t reoptimizationRetryCounter; // CodeBlock::reoptimizationRetryCounter()
    int32_t counterValue; // the baseline counter's ExecutionCounter::m_counter; 0 when NotCarried
    float counterTotalCount; // its ExecutionCounter::m_totalCount; 0 when NotCarried
    int32_t counterActiveThreshold; // its ExecutionCounter::m_activeThreshold; 0 when NotCarried
    std::array<uint32_t, numberOfFamilies> familyEntryCount;
};
static_assert(sizeof(StateHeader) == 160);

struct ArrayProfileRecord { // an ArrayProfile's accumulated modes and flags
    uint32_t observedArrayModes;
    uint32_t flags; // OptionSet<ArrayProfileFlag>::toRaw()
};
static_assert(sizeof(ArrayProfileRecord) == 8);

struct LazyOperandRecord { // one lazy-operand profile's key and prediction
    uint32_t bytecodeIndexBits; // BytecodeIndex::asBits()
    uint32_t operandKind; // OperandKind
    int32_t operandValue; // Operand::value()
    uint32_t reserved; // 0
    uint64_t prediction; // SpeculatedType
};
static_assert(sizeof(LazyOperandRecord) == 24);

struct SummaryHeader {
    uint8_t tier; // Tier::Baseline
    uint8_t counterMode; // CounterMode, the same as cb.state's
    uint16_t reserved; // 0, so that the header has no implicit padding
    uint32_t numArguments;
    uint32_t numValueProfiles;
    uint32_t numArrayProfiles; // the entries of every array-profile family, call sites included
    uint32_t numLazyOperandProfiles;
    uint32_t counterProgress; // progress floored at 0 and capped at the active threshold; 0 when NotCarried
};
static_assert(sizeof(SummaryHeader) == 24);

// A family is one profile field of one opcode's metadata, and its position in this table is the format's index space.
// The kinds occupy contiguous ranges, and each kind's records form one array of cb.state, families in table order and
// each family's entries in metadata-ID order. The three call-site families A11..A13 have no field and no records; they
// keep only their entries' array positions (familyHasRecords).
enum class FamilyKind : uint8_t { ArrayProfile, AllocationHint, IterationModes, EnumeratorModes, ToThisStatus, BranchBit };

static constexpr unsigned firstArrayProfileFamily = 0; // A0..A15
static constexpr unsigned firstAllocationHintFamily = 16; // F16..F19
static constexpr unsigned numberOfAllocationHintFamilies = 4;
static constexpr unsigned firstIterationModesFamily = 20; // F20..F24
static constexpr unsigned numberOfIterationModesFamilies = 5;
static constexpr unsigned firstEnumeratorModesFamily = 25; // F25..F29
static constexpr unsigned numberOfEnumeratorModesFamilies = 5;
static constexpr unsigned toThisStatusFamily = 30; // F30
static constexpr unsigned branchBitFamily = 31; // F31, the jneq_ptr bit
static constexpr unsigned iteratorOpenIterationModesFamily = 20; // F20, OpIteratorOpen
static constexpr unsigned iteratorNextIterationModesFamily = 21; // F21, OpIteratorNext
static constexpr unsigned asyncIteratorOpenIterationModesFamily = 22; // F22, OpAsyncIteratorOpen
static constexpr unsigned asyncIteratorNextIterationModesFamily = 23; // F23, OpAsyncIteratorNext
static constexpr unsigned iteratorCloseCheckIterationModesFamily = 24; // F24, OpIteratorCloseCheck

constexpr unsigned firstFamilyOf(FamilyKind kind)
{
    switch (kind) {
    case FamilyKind::ArrayProfile:
        return firstArrayProfileFamily;
    case FamilyKind::AllocationHint:
        return firstAllocationHintFamily;
    case FamilyKind::IterationModes:
        return firstIterationModesFamily;
    case FamilyKind::EnumeratorModes:
        return firstEnumeratorModesFamily;
    case FamilyKind::ToThisStatus:
        return toThisStatusFamily;
    case FamilyKind::BranchBit:
        return branchBitFamily;
    }
    return numberOfFamilies;
}

constexpr unsigned numberOfFamiliesOf(FamilyKind kind)
{
    switch (kind) {
    case FamilyKind::ArrayProfile:
        return numberOfArrayProfileFamilies;
    case FamilyKind::AllocationHint:
        return numberOfAllocationHintFamilies;
    case FamilyKind::IterationModes:
        return numberOfIterationModesFamilies;
    case FamilyKind::EnumeratorModes:
        return numberOfEnumeratorModesFamilies;
    case FamilyKind::ToThisStatus:
    case FamilyKind::BranchBit:
        return 1;
    }
    return 0;
}

constexpr FamilyKind familyKind(unsigned family)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(family < numberOfFamilies);
    if (family < firstAllocationHintFamily)
        return FamilyKind::ArrayProfile;
    if (family < firstIterationModesFamily)
        return FamilyKind::AllocationHint;
    if (family < firstEnumeratorModesFamily)
        return FamilyKind::IterationModes;
    if (family < toThisStatusFamily)
        return FamilyKind::EnumeratorModes;
    if (family == toThisStatusFamily)
        return FamilyKind::ToThisStatus;
    return FamilyKind::BranchBit;
}

namespace Internal {

template<typename... Ops>
struct OpList {
    static constexpr unsigned size = sizeof...(Ops);
};

template<typename Ignored, typename... Ops>
struct OpListAfterFirst {
    using Type = OpList<Ops...>;
};

// The native macros expand to `macro(OpA) macro(OpB) ...`. Each entry becomes `, OpX` after a leading `void` that
// OpListAfterFirst drops, so these lists follow the macros, and a reordered macro moves the native merge and this table
// together.
#define JITCACHE_CB_OP_LIST_ENTRY(op) , op
using SimpleArrayProfileOps = OpListAfterFirst<void FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE(JITCACHE_CB_OP_LIST_ENTRY)>::Type;
using AllocationHintOps = OpListAfterFirst<void FOR_EACH_OPCODE_WITH_ARRAY_ALLOCATION_PROFILE(JITCACHE_CB_OP_LIST_ENTRY)>::Type;
#undef JITCACHE_CB_OP_LIST_ENTRY
using IterationModesOps = OpList<OpIteratorOpen, OpIteratorNext, OpAsyncIteratorOpen, OpAsyncIteratorNext>;
using EnumeratorModesOps = OpList<OpEnumeratorNext, OpEnumeratorInByVal, OpEnumeratorHasOwnProperty, OpEnumeratorPutByVal, OpEnumeratorGetByVal>;

// One descriptor per field: the opcode whose metadata holds it, its kind, and field(metadata), the field itself, which
// exists when hasField is true. Three opcodes of FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE, OpCall, OpCallIgnoreResult
// and OpTailCall, keep the ArrayProfile of their this value in the call site's own CallSiteData, which the ICs lane
// carries, so their metadata has no m_arrayProfile. Their families have no field and keep only their entries' array
// positions, which pair with the UCB copies as the native merge pairs them.
template<typename OpType>
struct ArrayProfileField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::ArrayProfile;
    static constexpr bool hasField = requires(typename Op::Metadata& metadata) { metadata.m_arrayProfile; };
    static auto& field(typename Op::Metadata& metadata) requires hasField { return metadata.m_arrayProfile; }
};

struct IterableProfileField {
    using Op = OpIteratorNext;
    static constexpr FamilyKind kind = FamilyKind::ArrayProfile;
    static constexpr bool hasField = true;
    static auto& field(Op::Metadata& metadata) { return metadata.m_iterableProfile; }
};

template<typename OpType>
struct AllocationHintField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::AllocationHint;
    static constexpr bool hasField = true;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_arrayAllocationProfile; }
};

template<typename OpType>
struct IterationModesField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::IterationModes;
    static constexpr bool hasField = true;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_iterationMetadata; }
};

// iterator_close_check keeps the modes it saw in a byte of its own rather than an IterationModeMetadata.
struct IteratorCloseCheckModesField {
    using Op = OpIteratorCloseCheck;
    static constexpr FamilyKind kind = FamilyKind::IterationModes;
    static constexpr bool hasField = true;
    static auto& field(Op::Metadata& metadata) { return metadata.m_seenModes; }
};

template<typename OpType>
struct EnumeratorModesField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::EnumeratorModes;
    static constexpr bool hasField = true;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_enumeratorMetadata; }
};

struct ToThisStatusField {
    using Op = OpToThis;
    static constexpr FamilyKind kind = FamilyKind::ToThisStatus;
    static constexpr bool hasField = true;
    static auto& field(Op::Metadata& metadata) { return metadata.m_toThisStatus; }
};

struct BranchBitField {
    using Op = OpJneqPtr;
    static constexpr FamilyKind kind = FamilyKind::BranchBit;
    static constexpr bool hasField = true;
    static auto& field(Op::Metadata& metadata) { return metadata.m_hasJumped; }
};

template<typename... Fields>
struct FieldList {
    static constexpr unsigned size = sizeof...(Fields);
    template<unsigned index> using At = std::tuple_element_t<index, std::tuple<Fields...>>;
};

template<template<typename> class Field, typename Ops> struct FieldsOf;
template<template<typename> class Field, typename... Ops>
struct FieldsOf<Field, OpList<Ops...>> {
    using Type = FieldList<Field<Ops>...>;
};

template<typename... Lists> struct Concatenation;
template<typename... Fields>
struct Concatenation<FieldList<Fields...>> {
    using Type = FieldList<Fields...>;
};
template<typename... First, typename... Second, typename... Rest>
struct Concatenation<FieldList<First...>, FieldList<Second...>, Rest...> : Concatenation<FieldList<First..., Second...>, Rest...> { };

using Fields = Concatenation<
    FieldsOf<ArrayProfileField, SimpleArrayProfileOps>::Type, // A0..A14
    FieldList<IterableProfileField>, // A15
    FieldsOf<AllocationHintField, AllocationHintOps>::Type, // F16..F19
    FieldsOf<IterationModesField, IterationModesOps>::Type, // F20..F23
    FieldList<IteratorCloseCheckModesField>, // F24
    FieldsOf<EnumeratorModesField, EnumeratorModesOps>::Type, // F25..F29
    FieldList<ToThisStatusField, BranchBitField>>::Type; // F30, F31

} // namespace Internal

// Family f of the table: Op, the opcode; kind; hasField and field(Op::Metadata&), the profile field, which every family
// but A11..A13 has; and index, f.
template<unsigned familyIndex>
struct Family : Internal::Fields::At<familyIndex> {
    static constexpr unsigned index = familyIndex;
};

namespace Internal {

template<typename Functor, unsigned... indices>
void forEachFamily(const Functor& functor, std::integer_sequence<unsigned, indices...>)
{
    (functor(Family<indices> { }), ...);
}

template<unsigned... indices>
consteval std::array<bool, sizeof...(indices)> familiesWithField(std::integer_sequence<unsigned, indices...>)
{
    return { Family<indices>::hasField... };
}

} // namespace Internal

// Whether each family has cb.state records: every family but A11..A13. Those call sites keep their this profile in their
// own CallSiteData, which the ICs lane carries, so their entries keep only their array positions.
static constexpr std::array<bool, numberOfFamilies> familiesWithRecords = Internal::familiesWithField(std::make_integer_sequence<unsigned, numberOfFamilies>());

constexpr bool familyHasRecords(unsigned family)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(family < numberOfFamilies);
    return familiesWithRecords[family];
}

// The index, within its kind's cb.state array, of the family's first record: the records of the earlier families of its
// kind that have records.
constexpr size_t firstRecordOfFamily(const FamilyEntryCounts& counts, unsigned family)
{
    size_t index = 0;
    for (unsigned earlier = firstFamilyOf(familyKind(family)); earlier < family; ++earlier) {
        if (familyHasRecords(earlier))
            index += counts[earlier];
    }
    return index;
}

// The records a kind's cb.state array holds: the entries of the kind's families that have records.
constexpr size_t recordCountOfKind(const FamilyEntryCounts& counts, FamilyKind kind)
{
    size_t count = 0;
    unsigned first = firstFamilyOf(kind);
    for (unsigned family = first; family < first + numberOfFamiliesOf(kind); ++family) {
        if (familyHasRecords(family))
            count += counts[family];
    }
    return count;
}

// The array position of an array-profile family's first entry: the running index over A0..A15 in table order, call
// sites included, by which the native merge pairs the entry with UnlinkedCodeBlock's arrayProfiles().
constexpr size_t firstArrayPositionOfFamily(const FamilyEntryCounts& counts, unsigned family)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(familyKind(family) == FamilyKind::ArrayProfile);
    size_t position = 0;
    for (unsigned earlier = firstArrayProfileFamily; earlier < family; ++earlier)
        position += counts[earlier];
    return position;
}

// The array positions of all of A0..A15, which cb.summary's array flags hold one slot each.
constexpr size_t arrayPositionCount(const FamilyEntryCounts& counts)
{
    size_t count = 0;
    for (unsigned family = firstArrayProfileFamily; family < firstArrayProfileFamily + numberOfArrayProfileFamilies; ++family)
        count += counts[family];
    return count;
}

// Calls functor(Family<f> { }) for each family f, in table order.
template<typename Functor>
void forEachFamily(const Functor& functor)
{
    Internal::forEachFamily(functor, std::make_integer_sequence<unsigned, numberOfFamilies>());
}

// The guarded walkers. A CodeBlock whose bytecode adds no metadata entry and no value profile has no metadata table, and
// MetadataTable's own walkers read through their this pointer, so each walker here reads codeBlock.metadataTable() once
// and visits nothing when it is null. The lane reaches a CodeBlock's metadata only through them.

// Calls functor(typename Op::Metadata&) for each entry of Op, in metadata-ID order.
template<typename Op, typename Functor>
void forEachFamilyEntry(CodeBlock& codeBlock, NOESCAPE const Functor& functor)
{
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return;
    metadataTable->forEach<Op>(functor);
}

// Calls functor(unsigned offset, const ValueProfileRef&) for offsets 1 to numValueProfiles, in that order, through
// MetadataTable::forEachValueProfile. A profile's prediction reads SpecNone until the table allocates its predictions,
// which the first mergePrediction of something other than SpecNone does.
template<typename Functor>
void forEachMetadataValueProfile(CodeBlock& codeBlock, NOESCAPE const Functor& functor)
{
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return;
    unsigned offset = 0;
    metadataTable->forEachValueProfile([&](const ValueProfileRef& profile) {
        functor(++offset, profile);
    });
}

// Calls functor(field) with the profile field of each entry of the family, in metadata-ID order, through
// forEachFamilyEntry. Only for a family that has a field.
template<typename FamilyType, typename Functor>
void forEachFamilyField(CodeBlock& codeBlock, const Functor& functor)
{
    static_assert(FamilyType::hasField);
    forEachFamilyEntry<typename FamilyType::Op>(codeBlock, [&](typename FamilyType::Op::Metadata& metadata) {
        functor(FamilyType::field(metadata));
    });
}

// The entries forEachFamilyEntry visits for each family's opcode, all zero without a metadata table. Capture records
// them.
FamilyEntryCounts familyEntryCounts(CodeBlock&);

// The arrays of cb.state, in section order. The zero padding follows the last one.
enum class StateArray : uint8_t {
    ArgumentPredictions, // uint64_t
    ValuePredictions, // uint64_t
    ArrayProfiles, // ArrayProfileRecord, A0..A10, A14 and A15
    LazyOperandProfiles, // LazyOperandRecord
    AllocationHints, // uint16_t, F16..F19
    IterationModes, // uint16_t, F20..F24
    EnumeratorModes, // uint8_t, F25..F29
    ToThisStatuses, // uint8_t, F30
    BranchBits, // uint8_t, F31
};
static constexpr unsigned numberOfStateArrays = 9;

// The arrays of cb.summary, in section order. Each slot is the CB value ORed with its UCB copy; a lazy-operand slot has
// no UCB copy, and neither has any slot of a UCB without copies, so those hold the CB value alone. The CB value at a
// call site's array position is its own this profile's flags.
enum class SummaryArray : uint8_t {
    ArgumentCategories, // uint64_t
    ValueCategories, // uint64_t
    LazyOperandCategories, // uint64_t
    ArrayFlags, // uint8_t, one per array position of A0..A15
};
static constexpr unsigned numberOfSummaryArrays = 4;

constexpr size_t elementSize(StateArray array)
{
    switch (array) {
    case StateArray::ArgumentPredictions:
    case StateArray::ValuePredictions:
        return sizeof(uint64_t);
    case StateArray::ArrayProfiles:
        return sizeof(ArrayProfileRecord);
    case StateArray::LazyOperandProfiles:
        return sizeof(LazyOperandRecord);
    case StateArray::AllocationHints:
    case StateArray::IterationModes:
        return sizeof(uint16_t);
    case StateArray::EnumeratorModes:
    case StateArray::ToThisStatuses:
    case StateArray::BranchBits:
        return sizeof(uint8_t);
    }
    return 0;
}

constexpr size_t elementSize(SummaryArray array)
{
    switch (array) {
    case SummaryArray::ArgumentCategories:
    case SummaryArray::ValueCategories:
    case SummaryArray::LazyOperandCategories:
        return sizeof(uint64_t);
    case SummaryArray::ArrayFlags:
        return sizeof(uint8_t);
    }
    return 0;
}

// The cb.state array that holds a family kind's records.
constexpr StateArray stateArrayOf(FamilyKind kind)
{
    switch (kind) {
    case FamilyKind::ArrayProfile:
        return StateArray::ArrayProfiles;
    case FamilyKind::AllocationHint:
        return StateArray::AllocationHints;
    case FamilyKind::IterationModes:
        return StateArray::IterationModes;
    case FamilyKind::EnumeratorModes:
        return StateArray::EnumeratorModes;
    case FamilyKind::ToThisStatus:
        return StateArray::ToThisStatuses;
    case FamilyKind::BranchBit:
        return StateArray::BranchBits;
    }
    return StateArray::BranchBits;
}

// Where a section's arrays lie, computed from its header's counts. Arrays follow the header back to back, each starting
// where the previous one ends, and zero padding runs from paddingOffset to size, a multiple of sectionAlignment.
template<typename ArrayID, unsigned numberOfArrays>
struct SectionLayout {
    std::array<size_t, numberOfArrays> offsets { };
    std::array<size_t, numberOfArrays> counts { };
    size_t paddingOffset { 0 };
    size_t size { 0 };

    size_t offset(ArrayID array) const { return offsets[static_cast<unsigned>(array)]; }
    size_t count(ArrayID array) const { return counts[static_cast<unsigned>(array)]; }
    size_t elementOffset(ArrayID array, size_t index) const { return offset(array) + index * elementSize(array); }
};
using StateLayout = SectionLayout<StateArray, numberOfStateArrays>;
using SummaryLayout = SectionLayout<SummaryArray, numberOfSummaryArrays>;

// Computed with overflow-checked arithmetic (CheckedSize); empty on overflow. The sizes are stateSectionSize and
// summarySectionSize, which every section's length must equal.
std::optional<StateLayout> stateLayout(const StateHeader&);
std::optional<SummaryLayout> summaryLayout(const SummaryHeader&);
std::optional<size_t> stateSectionSize(const StateHeader&);
std::optional<size_t> summarySectionSize(const SummaryHeader&);

// The eight ArrayProfileFlag bits, which fit the byte cb.summary keeps for each array position.
static constexpr uint32_t allArrayProfileFlags = static_cast<uint32_t>(ArrayProfileFlag::MayStoreHole)
    | static_cast<uint32_t>(ArrayProfileFlag::OutOfBounds)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeLargeTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::MayInterceptIndexedAccesses)
    | static_cast<uint32_t>(ArrayProfileFlag::UsesNonOriginalArrayStructures)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeRegExpMatchesArray);
static_assert(allArrayProfileFlags == 0xff);

// An allocation hint is stored as ArrayAllocationProfile stores it, (IndexingType << 8) | vectorLength.
constexpr uint16_t encodeAllocationHint(IndexingType indexingType, unsigned vectorLength)
{
    return static_cast<uint16_t>((static_cast<unsigned>(indexingType) << 8) | vectorLength);
}

constexpr IndexingType allocationHintIndexingType(uint16_t hint)
{
    return static_cast<IndexingType>(hint >> 8);
}

constexpr unsigned allocationHintVectorLength(uint16_t hint)
{
    return hint & 0xff;
}

// An iteration-mode field's bits as cb.state stores them: IterationModeMetadata::seenModes, or the byte
// iterator_close_check keeps, widened.
constexpr uint16_t iterationModesOf(const IterationModeMetadata& field)
{
    return field.seenModes;
}

constexpr uint16_t iterationModesOf(uint8_t field)
{
    return field;
}

// Writes a record's bits into an iteration-mode field. iterator_close_check's byte takes the low byte, which holds every
// bit its writers record.
constexpr void setIterationModes(IterationModeMetadata& field, uint16_t modes)
{
    field.seenModes = modes;
}

constexpr void setIterationModes(uint8_t& field, uint16_t modes)
{
    field = static_cast<uint8_t>(modes);
}

} // namespace JSC::JITCache::CBFormat

#endif // ENABLE(JIT)
