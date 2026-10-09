#pragma once

#include <wtf/Platform.h>

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayConventions.h"
#include "ArrayProfile.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "IndexingType.h"
#include "IterationModeMetadata.h"
#include "JSPropertyNameEnumerator.h"
#include "MetadataTable.h"
#include "Opcode.h"
#include "SpeculatedType.h"
#include "ToThisStatus.h"
#include "ValueProfile.h"
#include <array>
#include <bit>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <tuple>
#include <type_traits>
#include <utility>

// The CB lane's two body-file sections, cb.state and cb.summary (SPEC-cb.md section 3): their layouts, the family table
// that fixes the order of the metadata records (section 3.1), the guarded walkers through which the lane reaches a
// CodeBlock's metadata, and the domain predicates of validation (section 3.4). Integers and floats are stored in native
// byte order, and no field holds an address, a cell, a StructureID or a time (I1). JITCacheCBFormat.cpp holds the table's
// static checks and the size functions; the templates and the constexpr predicates live here, where their callers can
// instantiate and evaluate them.

namespace JSC::JITCache::CBFormat {

static_assert(std::endian::native == std::endian::little, "cb.state and cb.summary store integers and floats in native byte order");

static constexpr uint16_t stateLayoutVersion = 1;
static constexpr uint16_t summaryLayoutVersion = 1;
enum class Tier : uint8_t { Baseline = 1 };
// Whether P12 travels. NotCarried marks a body with a polymorphic site (section 5.3).
enum class CounterMode : uint8_t { NotCarried = 0, Carried = 1 };
static constexpr unsigned numberOfFamilies = 31;
static constexpr unsigned numberOfArrayProfileFamilies = 16; // A0..A15, all paired

// Both sections start 8-byte aligned (R-INT-1) and end with zero padding up to a multiple of this.
static constexpr size_t sectionAlignment = 8;

using FamilyEntryCounts = std::array<uint32_t, numberOfFamilies>;

struct StateHeader {
    uint16_t layoutVersion; // stateLayoutVersion
    uint8_t tier; // Tier::Baseline
    uint8_t counterMode; // CounterMode
    uint32_t numArguments; // CodeBlock::numParameters()
    uint32_t numValueProfiles; // UnlinkedMetadataTable::numValueProfiles(), 0 without metadata
    uint32_t numLazyOperandProfiles;
    uint16_t optimizationDelayCounter; // P10
    uint16_t reoptimizationRetryCounter; // P11
    int32_t counterValue; // P12: ExecutionCounter::m_counter; 0 when NotCarried
    float counterTotalCount; // P12: ExecutionCounter::m_totalCount; 0 when NotCarried
    int32_t counterActiveThreshold; // P12: ExecutionCounter::m_activeThreshold; 0 when NotCarried
    std::array<uint32_t, numberOfFamilies> familyEntryCount;
    uint32_t reserved1; // 0; keeps the arrays that follow 8-byte aligned
};
static_assert(sizeof(StateHeader) == 160);

struct ArrayProfileRecord { // P4
    uint32_t observedArrayModes;
    uint32_t flags; // OptionSet<ArrayProfileFlag>::toRaw()
};
static_assert(sizeof(ArrayProfileRecord) == 8);

struct LazyOperandRecord { // P3
    uint32_t bytecodeIndexBits; // BytecodeIndex::asBits()
    uint32_t operandKind; // OperandKind
    int32_t operandValue; // Operand::value()
    uint32_t reserved; // 0
    uint64_t prediction; // SpeculatedType
};
static_assert(sizeof(LazyOperandRecord) == 24);

struct SummaryHeader {
    uint16_t layoutVersion; // summaryLayoutVersion
    uint8_t tier; // Tier::Baseline
    uint8_t counterMode; // CounterMode, the same as cb.state's
    uint32_t numArguments;
    uint32_t numValueProfiles;
    uint32_t numArrayProfiles; // all of A0..A15
    uint32_t numLazyOperandProfiles;
    uint32_t counterProgress; // section 4.3; 0 when the counter does not travel
};
static_assert(sizeof(SummaryHeader) == 24);

// Section 3.1. A family is one profile field of one opcode's metadata, and its position in this table is the format's
// index space. The kinds occupy contiguous ranges, and each kind's records form one array of cb.state, families in table
// order and each family's entries in metadata-ID order.
enum class FamilyKind : uint8_t { ArrayProfile, AllocationHint, IterationModes, EnumeratorModes, ToThisStatus, BranchBit };

static constexpr unsigned firstArrayProfileFamily = 0; // A0..A15
static constexpr unsigned firstAllocationHintFamily = 16; // F16..F19
static constexpr unsigned numberOfAllocationHintFamilies = 4;
static constexpr unsigned firstIterationModesFamily = 20; // F20..F23
static constexpr unsigned numberOfIterationModesFamilies = 4;
static constexpr unsigned firstEnumeratorModesFamily = 24; // F24..F28
static constexpr unsigned numberOfEnumeratorModesFamilies = 5;
static constexpr unsigned toThisStatusFamily = 29; // F29
static constexpr unsigned branchBitFamily = 30; // F30
static constexpr unsigned iteratorOpenIterationModesFamily = 20; // F20, OpIteratorOpen
static constexpr unsigned iteratorNextIterationModesFamily = 21; // F21, OpIteratorNext
static constexpr unsigned asyncIteratorOpenIterationModesFamily = 22; // F22, OpAsyncIteratorOpen
static constexpr unsigned asyncIteratorNextIterationModesFamily = 23; // F23, OpAsyncIteratorNext

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

// The index, within its kind's cb.state array, of the family's first record.
constexpr size_t firstRecordOfFamily(const FamilyEntryCounts& counts, unsigned family)
{
    size_t index = 0;
    for (unsigned earlier = firstFamilyOf(familyKind(family)); earlier < family; ++earlier)
        index += counts[earlier];
    return index;
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
// together (I12).
#define JITCACHE_CB_OP_LIST_ENTRY(op) , op
using SimpleArrayProfileOps = OpListAfterFirst<void FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE(JITCACHE_CB_OP_LIST_ENTRY)>::Type;
using AllocationHintOps = OpListAfterFirst<void FOR_EACH_OPCODE_WITH_ARRAY_ALLOCATION_PROFILE(JITCACHE_CB_OP_LIST_ENTRY)>::Type;
#undef JITCACHE_CB_OP_LIST_ENTRY
using IterationModesOps = OpList<OpIteratorOpen, OpIteratorNext, OpAsyncIteratorOpen, OpAsyncIteratorNext>;
using EnumeratorModesOps = OpList<OpEnumeratorNext, OpEnumeratorInByVal, OpEnumeratorHasOwnProperty, OpEnumeratorPutByVal, OpEnumeratorGetByVal>;

template<typename T, typename... Ops>
consteval unsigned indexOf(OpList<Ops...>)
{
    constexpr std::array<bool, sizeof...(Ops)> matches { std::is_same_v<T, Ops>... };
    for (unsigned index = 0; index < matches.size(); ++index) {
        if (matches[index])
            return index;
    }
    return matches.size();
}

// One descriptor per field: the opcode whose metadata holds it, its kind, and field(metadata), the field itself.
template<typename OpType>
struct ArrayProfileField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::ArrayProfile;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_arrayProfile; }
};

struct IterableProfileField {
    using Op = OpIteratorNext;
    static constexpr FamilyKind kind = FamilyKind::ArrayProfile;
    static auto& field(Op::Metadata& metadata) { return metadata.m_iterableProfile; }
};

template<typename OpType>
struct AllocationHintField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::AllocationHint;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_arrayAllocationProfile; }
};

template<typename OpType>
struct IterationModesField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::IterationModes;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_iterationMetadata; }
};

template<typename OpType>
struct EnumeratorModesField {
    using Op = OpType;
    static constexpr FamilyKind kind = FamilyKind::EnumeratorModes;
    static auto& field(typename Op::Metadata& metadata) { return metadata.m_enumeratorMetadata; }
};

struct ToThisStatusField {
    using Op = OpToThis;
    static constexpr FamilyKind kind = FamilyKind::ToThisStatus;
    static auto& field(Op::Metadata& metadata) { return metadata.m_toThisStatus; }
};

struct BranchBitField {
    using Op = OpJneqPtr;
    static constexpr FamilyKind kind = FamilyKind::BranchBit;
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
    FieldsOf<EnumeratorModesField, EnumeratorModesOps>::Type, // F24..F28
    FieldList<ToThisStatusField, BranchBitField>>::Type; // F29, F30

} // namespace Internal

// Family f of the table: Op, the opcode; kind; field(Op::Metadata&), the profile field; and index, which is f.
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

} // namespace Internal

// Calls functor(Family<f> { }) for each family f, in table order.
template<typename Functor>
void forEachFamily(const Functor& functor)
{
    Internal::forEachFamily(functor, std::make_integer_sequence<unsigned, numberOfFamilies>());
}

// F19, the allocation profile of op_new_array_buffer, found by opcode so that V8's literal rule follows the macro's order.
static constexpr unsigned newArrayBufferFamily = firstAllocationHintFamily + Internal::indexOf<OpNewArrayBuffer>(Internal::AllocationHintOps { });

// The guarded walkers (section 3.1). Each reads codeBlock.metadataTable() once and visits nothing when it is null (N1);
// the lane reaches a CodeBlock's metadata only through them (I15).

// Calls functor(typename Op::Metadata&) for each entry of Op, in metadata-ID order.
template<typename Op, typename Functor>
void forEachFamilyEntry(CodeBlock& codeBlock, const Functor& functor)
{
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return;
    metadataTable->forEach<Op>(functor);
}

// Calls functor(unsigned offset, ValueProfile&) for offsets 1 to numValueProfiles, in that order.
template<typename Functor>
void forEachMetadataValueProfile(CodeBlock& codeBlock, const Functor& functor)
{
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return;
    unsigned numValueProfiles = codeBlock.unlinkedCodeBlock()->metadata().numValueProfiles();
    for (unsigned offset = 1; offset <= numValueProfiles; ++offset)
        functor(offset, metadataTable->valueProfileForOffset(offset));
}

// Calls functor(field) with the profile field of each entry of the family, in metadata-ID order, through
// forEachFamilyEntry.
template<typename FamilyType, typename Functor>
void forEachFamilyField(CodeBlock& codeBlock, const Functor& functor)
{
    forEachFamilyEntry<typename FamilyType::Op>(codeBlock, [&](typename FamilyType::Op::Metadata& metadata) {
        functor(FamilyType::field(metadata));
    });
}

// The entries forEachFamilyEntry visits for each family's opcode, all zero without a metadata table (capture step 1, V4).
FamilyEntryCounts familyEntryCounts(CodeBlock&);

// Calls functor(unsigned metadataID, IndexingType recommendedIndexingType) for each op_new_array_buffer instruction of the
// CodeBlock, in stream order. It reads only the instructions and allocates nothing (V8, S2).
template<typename Functor>
void forEachNewArrayBufferLiteral(CodeBlock& codeBlock, const Functor& functor)
{
    for (const auto& instruction : codeBlock.instructions()) {
        if (!instruction->is<OpNewArrayBuffer>())
            continue;
        auto bytecode = instruction->as<OpNewArrayBuffer>();
        functor(bytecode.m_metadataID, bytecode.m_recommendedIndexingType);
    }
}

// Section 3.2: the arrays of cb.state, in section order (arrays 1 to 9; array 10 is the padding).
enum class StateArray : uint8_t {
    ArgumentPredictions, // uint64_t
    ValuePredictions, // uint64_t
    ArrayProfiles, // ArrayProfileRecord, A0..A15
    LazyOperandProfiles, // LazyOperandRecord
    AllocationHints, // uint16_t, F16..F19
    IterationModes, // uint16_t, F20..F23
    EnumeratorModes, // uint8_t, F24..F28
    ToThisStatuses, // uint8_t, F29
    BranchBits, // uint8_t, F30
};
static constexpr unsigned numberOfStateArrays = 9;

// Section 3.3: the arrays of cb.summary, in section order.
enum class SummaryArray : uint8_t {
    ArgumentCategories, // uint64_t
    ValueCategories, // uint64_t
    LazyOperandCategories, // uint64_t
    ArrayFlags, // uint8_t, A0..A15
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
// summarySectionSize, which every section's length must equal (I10).
std::optional<StateLayout> stateLayout(const StateHeader&);
std::optional<SummaryLayout> summaryLayout(const SummaryHeader&);
std::optional<size_t> stateSectionSize(const StateHeader&);
std::optional<size_t> summarySectionSize(const SummaryHeader&);

// The domain predicates of section 3.4, which validateState and validateSummary apply.

// V1.
constexpr bool counterModeFits(uint8_t counterMode)
{
    return counterMode == static_cast<uint8_t>(CounterMode::NotCarried) || counterMode == static_cast<uint8_t>(CounterMode::Carried);
}

// V6: a subset of SpecBytecodeTop.
constexpr bool predictionFits(SpeculatedType prediction)
{
    return !(prediction & ~SpecBytecodeTop);
}

// V7: modes within ALL_ARRAY_MODES and flags within the eight ArrayProfileFlag bits.
static constexpr uint32_t allArrayProfileFlags = static_cast<uint32_t>(ArrayProfileFlag::MayStoreHole)
    | static_cast<uint32_t>(ArrayProfileFlag::OutOfBounds)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeLargeTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::MayInterceptIndexedAccesses)
    | static_cast<uint32_t>(ArrayProfileFlag::UsesNonOriginalArrayStructures)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeRegExpMatchesArray);
static_assert(allArrayProfileFlags == 0xff);

constexpr bool arrayModesFit(uint32_t observedArrayModes)
{
    return !(observedArrayModes & ~static_cast<uint32_t>(ALL_ARRAY_MODES));
}

constexpr bool arrayProfileFlagsFit(uint32_t flags)
{
    return !(flags & ~allArrayProfileFlags);
}

// V8. An allocation hint is stored as ArrayAllocationProfile stores it, (IndexingType << 8) | vectorLength.
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

constexpr bool allocationHintVectorLengthFits(unsigned vectorLength)
{
    return vectorLength <= BASE_CONTIGUOUS_VECTOR_LEN_MAX;
}

// The rank of an array literal's type in the order CopyOnWriteArrayWithInt32 < CopyOnWriteArrayWithDouble <
// CopyOnWriteArrayWithContiguous, the only types op_new_array_buffer links and updateProfile leaves (N8).
constexpr std::optional<unsigned> copyOnWriteLiteralRank(IndexingType indexingType)
{
    switch (indexingType) {
    case CopyOnWriteArrayWithInt32:
        return 0;
    case CopyOnWriteArrayWithDouble:
        return 1;
    case CopyOnWriteArrayWithContiguous:
        return 2;
    default:
        return std::nullopt;
    }
}

// V8's set: the indexing types linking and ArrayAllocationProfile::updateProfile leave at a site of the family.
constexpr bool allocationHintTypeFits(unsigned family, IndexingType hint)
{
    if (family == newArrayBufferFamily)
        return !!copyOnWriteLiteralRank(hint);
    if (familyKind(family) != FamilyKind::AllocationHint)
        return false;
    switch (hint) {
    case ArrayWithUndecided:
    case ArrayWithInt32:
    case ArrayWithDouble:
    case ArrayWithContiguous:
    case ArrayWithArrayStorage:
    case ArrayWithSlowPutArrayStorage:
        return true;
    default:
        return false;
    }
}

// V8's type rule: the family's set and, in F19, at least `recommended`, the m_recommendedIndexingType of the literal whose
// entry holds the hint. A hint below the elements' type is safe in F16..F18, which ignore `recommended`.
constexpr bool allocationHintFits(unsigned family, IndexingType hint, IndexingType recommended)
{
    if (!allocationHintTypeFits(family, hint))
        return false;
    if (family != newArrayBufferFamily)
        return true;
    auto recommendedRank = copyOnWriteLiteralRank(recommended);
    return recommendedRank && *copyOnWriteLiteralRank(hint) >= *recommendedRank;
}

// V9: each opcode's native bits, the bits its DFG handler has a case for or keeps (N9).
template<typename... Modes>
constexpr uint16_t iterationModeBits(Modes... modes)
{
    return static_cast<uint16_t>((0u | ... | static_cast<unsigned>(modes)));
}

constexpr uint16_t nativeIterationModes(unsigned family)
{
    switch (family) {
    case iteratorOpenIterationModesFamily:
        return iterationModeBits(IterationMode::Generic, IterationMode::FastArray, IterationMode::FastMap, IterationMode::FastSet, IterationMode::FastString,
            IterationMode::FastArrayValues, IterationMode::FastArrayKeys, IterationMode::FastArrayEntries, IterationMode::FastMapKeys, IterationMode::FastMapValues,
            IterationMode::FastMapEntries, IterationMode::FastSetValues, IterationMode::FastSetEntries);
    case iteratorNextIterationModesFamily:
        return iterationModeBits(IterationMode::Generic, IterationMode::FastString,
            IterationMode::FastArrayValues, IterationMode::FastArrayKeys, IterationMode::FastArrayEntries, IterationMode::FastMapKeys, IterationMode::FastMapValues,
            IterationMode::FastMapEntries, IterationMode::FastSetValues, IterationMode::FastSetEntries);
    case asyncIteratorOpenIterationModesFamily:
        return iterationModeBits(IterationMode::Generic, IterationMode::FastAsyncGenerator, IterationMode::AsyncFromSync);
    case asyncIteratorNextIterationModesFamily:
        return iterationModeBits(IterationMode::Generic, IterationMode::FastAsyncGenerator);
    default:
        return 0;
    }
}

constexpr bool iterationModesFit(unsigned family, uint16_t seenModes)
{
    return !(seenModes & ~nativeIterationModes(family));
}

// V10: within JSPropertyNameEnumerator::Flag.
static constexpr uint8_t allEnumeratorModes = static_cast<uint8_t>(JSPropertyNameEnumerator::IndexedMode | JSPropertyNameEnumerator::OwnStructureMode
    | JSPropertyNameEnumerator::GenericMode | JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch);

constexpr bool enumeratorModesFit(uint8_t modes)
{
    return !(modes & ~allEnumeratorModes);
}

// V11: merge(ToThisStatus, ToThisStatus) crashes on any other value.
constexpr bool toThisStatusFits(uint8_t status)
{
    return status == static_cast<uint8_t>(ToThisOK) || status == static_cast<uint8_t>(ToThisConflicted) || status == static_cast<uint8_t>(ToThisClearedByGC);
}

// V12.
constexpr bool branchBitFits(uint8_t bit)
{
    return bit <= 1;
}

} // namespace JSC::JITCache::CBFormat

#endif // ENABLE(JIT)
