#include "config.h"
#include "CBState.h"

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "ByteFields.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CBFormat.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "IterationModeMetadata.h"
#include "JSGlobalObject.h"
#include "LazyOperandValueProfile.h"
#include "LazyValueProfile.h"
#include "Operands.h"
#include "Options.h"
#include "SpeculatedType.h"
#include "StructureID.h"
#include "ToThisStatus.h"
#include "VM.h"
#include "ValueProfile.h"
#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

// Restoration of a newborn CodeBlock's state from its cb.state section. prepare reads the borrowed section and the CB and
// writes neither; with strict on it first runs validateState, checks that the CB is newborn and at its link state, and
// checks the captured counter. seedLinkedState then writes every profile and the tier-up history field by field into the
// linked CB and runs the realm step, and finishCounter, after native setup, puts the captured counter back and lets the
// engine's own check re-slice a finite threshold against this process's pool and the VM's startup deferral scale.

namespace JSC::JITCache {

namespace CBImportInternal {

using CBFormat::FamilyKind;
using CBFormat::StateArray;

// The cb.state element that holds one record of each family kind.
template<FamilyKind> struct FamilyRecord;
template<> struct FamilyRecord<FamilyKind::ArrayProfile> {
    using Type = CBFormat::ArrayProfileRecord;
};
template<> struct FamilyRecord<FamilyKind::AllocationHint> {
    using Type = uint16_t;
};
template<> struct FamilyRecord<FamilyKind::IterationModes> {
    using Type = uint16_t;
};
template<> struct FamilyRecord<FamilyKind::EnumeratorModes> {
    using Type = uint8_t;
};
template<> struct FamilyRecord<FamilyKind::ToThisStatus> {
    using Type = uint8_t;
};
template<> struct FamilyRecord<FamilyKind::BranchBit> {
    using Type = uint8_t;
};

using ArrayOffsets = std::array<uint32_t, CBFormat::numberOfStateArrays>;

// The records of a family kind: the entry counts of its families that have records.
static size_t familyRecordCount(const CBFormat::StateHeader& header, FamilyKind kind)
{
    return CBFormat::recordCountOfKind(header.familyEntryCount, kind);
}

// One array of a prepared section as a typed span. The section starts 8-byte aligned and the layout aligns every element,
// and subspan bounds the view by the borrowed span.
template<typename T>
static std::span<const T> stateArray(std::span<const uint8_t> bytes, const ArrayOffsets& offsets, StateArray array, size_t count)
{
    ASSERT(sizeof(T) == CBFormat::elementSize(array));
    return spanReinterpretCast<const T>(bytes.subspan(offsets[static_cast<unsigned>(array)], count * sizeof(T)));
}

// The nine arrays of a prepared section, at the offsets prepare computed and with the header's counts.
struct StateArrays {
    StateArrays(std::span<const uint8_t> bytes, const CBFormat::StateHeader& header, const ArrayOffsets& offsets)
        : argumentPredictions(stateArray<uint64_t>(bytes, offsets, StateArray::ArgumentPredictions, header.numArguments))
        , valuePredictions(stateArray<uint64_t>(bytes, offsets, StateArray::ValuePredictions, header.numValueProfiles))
        , arrayProfiles(stateArray<CBFormat::ArrayProfileRecord>(bytes, offsets, StateArray::ArrayProfiles, familyRecordCount(header, FamilyKind::ArrayProfile)))
        , lazyOperandProfiles(stateArray<CBFormat::LazyOperandRecord>(bytes, offsets, StateArray::LazyOperandProfiles, header.numLazyOperandProfiles))
        , allocationHints(stateArray<uint16_t>(bytes, offsets, StateArray::AllocationHints, familyRecordCount(header, FamilyKind::AllocationHint)))
        , iterationModes(stateArray<uint16_t>(bytes, offsets, StateArray::IterationModes, familyRecordCount(header, FamilyKind::IterationModes)))
        , enumeratorModes(stateArray<uint8_t>(bytes, offsets, StateArray::EnumeratorModes, familyRecordCount(header, FamilyKind::EnumeratorModes)))
        , toThisStatuses(stateArray<uint8_t>(bytes, offsets, StateArray::ToThisStatuses, familyRecordCount(header, FamilyKind::ToThisStatus)))
        , branchBits(stateArray<uint8_t>(bytes, offsets, StateArray::BranchBits, familyRecordCount(header, FamilyKind::BranchBit)))
    {
    }

    // The records of a family kind, its families in table order.
    template<FamilyKind kind>
    std::span<const typename FamilyRecord<kind>::Type> records() const
    {
        if constexpr (kind == FamilyKind::ArrayProfile)
            return arrayProfiles;
        else if constexpr (kind == FamilyKind::AllocationHint)
            return allocationHints;
        else if constexpr (kind == FamilyKind::IterationModes)
            return iterationModes;
        else if constexpr (kind == FamilyKind::EnumeratorModes)
            return enumeratorModes;
        else if constexpr (kind == FamilyKind::ToThisStatus)
            return toThisStatuses;
        else {
            static_assert(kind == FamilyKind::BranchBit);
            return branchBits;
        }
    }

    const std::span<const uint64_t> argumentPredictions;
    const std::span<const uint64_t> valuePredictions;
    const std::span<const CBFormat::ArrayProfileRecord> arrayProfiles;
    const std::span<const CBFormat::LazyOperandRecord> lazyOperandProfiles;
    const std::span<const uint16_t> allocationHints;
    const std::span<const uint16_t> iterationModes;
    const std::span<const uint8_t> enumeratorModes;
    const std::span<const uint8_t> toThisStatuses;
    const std::span<const uint8_t> branchBits;
};

// Newborn: no JIT type and no BaselineJITData yet. At both install points the CB was just created, and no cell a marker
// or compiler thread visits points to it before installCode.
static bool isNewborn(CodeBlock& codeBlock)
{
    return codeBlock.jitType() == JITType::None && !codeBlock.baselineJITData();
}

// An argument profile or a ValueProfileRef, whose prediction reads SpecNone while its table has no predictions.
template<typename Profile>
static bool valueProfileIsAtLinkState(const Profile& profile)
{
    return profile.prediction() == SpecNone && !profile.numberOfSamples();
}

// An ArrayProfile is its two StructureID samples, its flags and its modes, with no padding, so comparing its bytes with
// zero compares exactly those four fields.
static_assert(sizeof(ArrayProfile) == 2 * sizeof(StructureID) + sizeof(OptionSet<ArrayProfileFlag>) + sizeof(ArrayModes));
static_assert(!ToThisOK, "the link-state check reads a to_this status at its link state as zero");

// Whether one field of a family is at its link state. An allocation hint's type is checked by the caller, which knows
// the site.
template<FamilyKind kind, typename Field>
static bool familyFieldIsAtLinkState(Field& field)
{
    if constexpr (kind == FamilyKind::ArrayProfile)
        return isAllZero(asByteSpan(field));
    else if constexpr (kind == FamilyKind::AllocationHint)
        return !field.vectorLengthHintConcurrently();
    else if constexpr (kind == FamilyKind::IterationModes)
        return !CBFormat::iterationModesOf(field);
    else if constexpr (kind == FamilyKind::EnumeratorModes)
        return !field;
    else if constexpr (kind == FamilyKind::ToThisStatus)
        return field == ToThisOK;
    else {
        static_assert(kind == FamilyKind::BranchBit);
        return !field;
    }
}

// The CB is at its link state: every piece is at the value linking gave it, and the tier-up history is zero. It reads the
// CB in place and allocates nothing. It runs after validateState, which matched each family count with this CB and
// bounded every op_new_array_buffer's metadata ID by F19's count, so the F19 check below indexes the metadata in range.
static bool isAtLinkState(CodeBlock& codeBlock)
{
    for (const auto& profile : codeBlock.argumentValueProfiles().span()) {
        if (!valueProfileIsAtLinkState(profile))
            return false;
    }

    bool atLinkState = true;
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned, const ValueProfileRef& profile) {
        atLinkState &= valueProfileIsAtLinkState(profile);
    });

    // A family without a field holds call sites, whose call data the ICs lane checks.
    unsigned newArrayBufferEntries = 0;
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        if constexpr (FamilyType::hasField) {
            CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
                atLinkState &= familyFieldIsAtLinkState<FamilyType::kind>(field);
                if constexpr (FamilyType::index == CBFormat::newArrayBufferFamily)
                    ++newArrayBufferEntries;
                else if constexpr (FamilyType::kind == FamilyKind::AllocationHint)
                    atLinkState &= field.selectIndexingTypeConcurrently() == ArrayWithUndecided;
            });
        }
    });

    // Linking gives each F19 entry its instruction's type. The check reaches the entry through the instruction, as
    // OpNewArrayBuffer::metadata(CodeBlock*) reaches it.
    if (newArrayBufferEntries) {
        CBFormat::forEachNewArrayBufferLiteral(codeBlock, [&](unsigned metadataID, IndexingType recommendedIndexingType) {
            auto& metadata = codeBlock.metadata<OpNewArrayBuffer::Metadata>(OpNewArrayBuffer::opcodeID, metadataID);
            atLinkState &= metadata.m_arrayAllocationProfile.selectIndexingTypeConcurrently() == recommendedIndexingType;
        });
    }

#if ENABLE(DFG_JIT)
    codeBlock.lazyValueProfiles().forEachOperandValueProfile([&](const LazyOperandValueProfile&) {
        atLinkState = false;
    });
#endif

    return atLinkState && !codeBlock.optimizationDelayCounter() && !codeBlock.reoptimizationRetryCounter();
}

// prepare's strict checks, in order: validateState, then that the CB is newborn and at its link state, then the captured
// counter's invariant.
static std::optional<CBCheck> strictFailure(std::span<const uint8_t> stateSection, CodeBlock& codeBlock)
{
    if (auto check = validateState(stateSection, codeBlock))
        return check;
    if (!isNewborn(codeBlock))
        return CBCheck::StrictNewborn;
    if (!isAtLinkState(codeBlock))
        return CBCheck::StrictLinkState;
    // validateState accepted the header, so the span holds one.
    auto header = readField<CBFormat::StateHeader>(stateSection, 0);
    if (!counterObeysNativeInvariant(header))
        return CBCheck::StrictCounter;
    return std::nullopt;
}

// One family field from its record: the array profile and allocation hint through restoreAccumulatedState and
// restoreHint, which assert that the samples they leave are empty, iterator_close_check's byte narrowed, and every other
// field assigned.
template<FamilyKind kind, typename Field, typename Record>
static void seedFamilyField(Field& field, const Record& record)
{
    if constexpr (kind == FamilyKind::ArrayProfile)
        field.restoreAccumulatedState(record.observedArrayModes, OptionSet<ArrayProfileFlag>::fromRaw(record.flags));
    else if constexpr (kind == FamilyKind::AllocationHint)
        field.restoreHint(CBFormat::allocationHintIndexingType(record), CBFormat::allocationHintVectorLength(record));
    else if constexpr (kind == FamilyKind::IterationModes)
        CBFormat::setIterationModes(field, record);
    else if constexpr (kind == FamilyKind::EnumeratorModes)
        field = record;
    else if constexpr (kind == FamilyKind::ToThisStatus)
        field = static_cast<ToThisStatus>(record);
    else {
        static_assert(kind == FamilyKind::BranchBit);
        field = static_cast<bool>(record);
    }
}

} // namespace CBImportInternal

std::expected<CBStateImport, CBFault> CBStateImport::prepare(std::span<const uint8_t> stateSection, CodeBlock& codeBlock, bool strict)
{
    using namespace CBImportInternal;

    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    // The integrator hands every section over 8-byte aligned.
    ASSERT(!(reinterpret_cast<uintptr_t>(stateSection.data()) % CBFormat::sectionAlignment));
    AssertNoGC assertNoGC;

    if (strict) {
        if (auto check = strictFailure(stateSection, codeBlock))
            return std::unexpected(CBFault { CBFaultKind::InvalidMaterial, *check });
    }

    // Normal mode trusts the section. The release assertions keep a span that does not cover the whole section from being
    // read past its end; with strict on, validateState has already matched the header and the length exactly.
    RELEASE_ASSERT(stateSection.size() >= sizeof(CBFormat::StateHeader));
    const auto& header = reinterpretCastSpanStartTo<CBFormat::StateHeader>(stateSection);
    auto layout = CBFormat::stateLayout(header);
    RELEASE_ASSERT(layout && layout->size <= stateSection.size() && layout->size <= std::numeric_limits<uint32_t>::max());
    ASSERT(layout->size == stateSection.size());

    CBStateImport result;
    static_assert(std::is_same_v<decltype(result.m_arrayOffsets), ArrayOffsets>);
    result.m_bytes = stateSection;
    result.m_header = &header;
    for (unsigned array = 0; array < CBFormat::numberOfStateArrays; ++array)
        result.m_arrayOffsets[array] = static_cast<uint32_t>(layout->offsets[array]);
    return result;
}

void CBStateImport::seedLinkedState(CodeBlock& codeBlock) const
{
    using namespace CBImportInternal;

    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    ASSERT(isNewborn(codeBlock)); // linked, no JIT type, never ran
    AssertNoGC assertNoGC;

    const CBFormat::StateHeader& header = *m_header;
    StateArrays arrays(m_bytes, header, m_arrayOffsets);

    // Value profiles: offset k from element k - 1, through the native writer, for each record that is not SpecNone. On a
    // link-state profile a merge is an assignment, and the first one allocates the table's predictions, so a CB whose
    // records are all SpecNone gets none, as natively.
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, const ValueProfileRef& profile) {
        auto prediction = static_cast<SpeculatedType>(arrays.valuePredictions[offset - 1]);
        if (prediction != SpecNone)
            profile.mergePrediction(prediction);
    });

    // Argument profiles.
    std::span<ArgumentValueProfile> argumentProfiles = codeBlock.argumentValueProfiles().mutableSpan();
    ASSERT(argumentProfiles.size() == arrays.argumentPredictions.size());
    for (size_t index = 0; index < arrays.argumentPredictions.size(); ++index)
        argumentProfiles[index].m_prediction = static_cast<SpeculatedType>(arrays.argumentPredictions[index]);

    // Lazy-operand profiles, through the append OSR exit itself uses, in the records' order, so a repeated key lands in
    // one profile.
#if ENABLE(DFG_JIT)
    CompressedLazyValueProfileHolder& lazyProfiles = codeBlock.lazyValueProfiles();
    for (const auto& record : arrays.lazyOperandProfiles) {
        LazyOperandValueProfileKey key(BytecodeIndex::fromBits(record.bytecodeIndexBits), Operand(static_cast<OperandKind>(record.operandKind), record.operandValue));
        lazyProfiles.addOperandValueProfile(key)->m_prediction = static_cast<SpeculatedType>(record.prediction);
    }
#else
    ASSERT(arrays.lazyOperandProfiles.empty());
#endif

    // The families: each family's entries in metadata-ID order, in step with its records. A family without a field holds
    // call sites, whose this profile the ICs lane restores; it has no records.
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        if constexpr (FamilyType::hasField) {
            auto records = arrays.records<FamilyType::kind>();
            size_t index = CBFormat::firstRecordOfFamily(header.familyEntryCount, FamilyType::index);
            CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
                seedFamilyField<FamilyType::kind>(field, records[index++]);
            });
            ASSERT(index == CBFormat::firstRecordOfFamily(header.familyEntryCount, FamilyType::index) + header.familyEntryCount[FamilyType::index]);
        }
    });

    // The tier-up history, before setup, whose optimizeAfterWarmUp reads the reoptimization count.
    codeBlock.seedBaselineTierUpHistory(header.optimizationDelayCounter, header.reoptimizationRetryCounter);

    // The realm step: a seeded FastMap or FastSet at an iterator_open site implies, as native recording does, that the
    // CB's realm has materialized the function the DFG's handler freezes. The native lazy getters allocate it under the
    // install deferral, only for a seed that carries the bit. These are the lane's only cell allocations.
    size_t firstIteratorOpenRecord = CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::iteratorOpenIterationModesFamily);
    uint16_t iteratorOpenModes = 0;
    for (uint16_t modes : arrays.iterationModes.subspan(firstIteratorOpenRecord, header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily]))
        iteratorOpenModes |= modes;
    JSGlobalObject* globalObject = codeBlock.globalObject();
    if (iteratorOpenModes & static_cast<uint16_t>(IterationMode::FastMap))
        globalObject->mapProtoEntriesFunction();
    if (iteratorOpenModes & static_cast<uint16_t>(IterationMode::FastSet))
        globalObject->setProtoValuesFunction();
}

CBCounterRestore CBStateImport::finishCounter(CodeBlock& codeBlock) const
{
    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    AssertNoGC assertNoGC;

    // Native setup, which runs before this call, always creates the BaselineJITData.
    BaselineJITData* jitData = codeBlock.baselineJITData();
    RELEASE_ASSERT(jitData);
    BaselineExecutionCounter& counter = jitData->executeCounter();
    const CBFormat::StateHeader& header = *m_header;

    // A counter that does not travel keeps setup's arming from the seeded reoptimization count and the UCB's quick DFG
    // bit, and setup's aging sample.
    if (header.counterMode != static_cast<uint8_t>(CBFormat::CounterMode::Carried)) {
        int64_t armed = -static_cast<int64_t>(counter.m_counter);
        return CBCounterRestore { .carried = false, .crossed = false, .nativeSlice = armed, .slice = armed };
    }

    // The captured progress, as count() computes it, exact for every pair native code leaves.
    double progress = static_cast<double>(header.counterTotalCount) + header.counterValue;
    int32_t threshold = header.counterActiveThreshold;
    int64_t sliceFloor = 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());

    counter.m_activeThreshold = threshold;
    counter.m_totalCount = header.counterTotalCount;
    counter.m_counter = header.counterValue;

    // The scale CodeBlock::checkIfOptimizationThresholdReached passes, which withholds it only from a CB that already has
    // an optimized replacement; a newborn CB is not yet its executable's code, so it has none. That function is not
    // called, since it would take the JIT worklist's lock to look for a DFG plan this CB cannot have.
    double startupDeferralScale = codeBlock.vm().startupJITDeferralScale();
    CBCounterRestore restore { .carried = true, .startupDeferralScale = startupDeferralScale };
    if (threshold == std::numeric_limits<int32_t>::max()) {
        // setThreshold would defer indefinitely and drop the progress, so the captured slice stays, which no pool
        // statistic shaped.
        restore.nativeSlice = header.counterValue < 0 ? -static_cast<int64_t>(header.counterValue) : 0;
    } else if (counter.checkIfThresholdCrossedAndSet(&codeBlock, startupDeferralScale)) {
        // This process's pool, this CB's size and the VM's scale say the threshold is reached.
        restore.crossed = true;
        restore.nativeSlice = 0;
    } else
        restore.nativeSlice = -static_cast<int64_t>(counter.m_counter); // the slice ExecutionCounter::setThreshold armed

    // The floor: at least two entry increments short of crossing. A slice is at most 2^31, so -slice fits int32_t.
    restore.slice = std::max(restore.nativeSlice, sliceFloor);
    counter.m_counter = static_cast<int32_t>(-restore.slice);
    counter.m_totalCount = static_cast<float>(progress + restore.slice);

    // Setup sampled aging from the fresh counter; without this the restored progress would read as activity at the next
    // old-age check.
    codeBlock.snapshotExecutionCounterForAging(static_cast<float>(counter.count()));
    return restore;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
