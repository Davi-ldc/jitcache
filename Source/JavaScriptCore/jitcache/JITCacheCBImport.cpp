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
#include "JITCacheCBState.h"

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "IterationModeMetadata.h"
#include "JITCacheCBFormat.h"
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

// Restoration of a newborn CodeBlock's state from its cb.state section (SPEC-cb.md section 5). prepare reads the borrowed
// section and the CB and writes neither; with strict on it first applies the V-rules, S1, S2 and S3. seedLinkedState then
// writes P1 to P11 field by field into the linked CB and runs the realm step, and finishCounter, after native setup, puts
// the captured counter back and lets the engine's own check re-slice a finite threshold against this process's pool.

namespace JSC::JITCache {

namespace CBImportInternal {

using CBFormat::FamilyKind;
using CBFormat::StateArray;

// The cb.state element that holds one record of each family kind (section 3.2).
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

// The records of a family kind: the sum of its families' entry counts.
static size_t familyRecordCount(const CBFormat::StateHeader& header, FamilyKind kind)
{
    size_t count = 0;
    unsigned first = CBFormat::firstFamilyOf(kind);
    for (unsigned family = first; family < first + CBFormat::numberOfFamiliesOf(kind); ++family)
        count += header.familyEntryCount[family];
    return count;
}

// One array of a prepared section as a typed span. The section starts 8-byte aligned (R-INT-1) and the layout aligns every
// element (section 3.2), and subspan bounds the view by the borrowed span.
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

// S1 (N6): no JIT type and no BaselineJITData yet.
static bool isNewborn(CodeBlock& codeBlock)
{
    return codeBlock.jitType() == JITType::None && !codeBlock.baselineJITData();
}

template<typename Profile>
static bool valueProfileIsAtLinkState(const Profile& profile)
{
    return profile.m_prediction == SpecNone && !profile.numberOfSamples();
}

// An ArrayProfile is its two StructureID samples, its flags and its modes, with no padding, so comparing its bytes with
// zero compares exactly those four fields.
static_assert(sizeof(ArrayProfile) == 2 * sizeof(StructureID) + sizeof(OptionSet<ArrayProfileFlag>) + sizeof(ArrayModes));
static_assert(!ToThisOK, "S2 reads a to_this status at its link state as zero");

static bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

// S2 for one field of a family. An allocation hint's type is checked by the caller, which knows the site.
template<FamilyKind kind, typename Field>
static bool familyFieldIsAtLinkState(Field& field)
{
    if constexpr (kind == FamilyKind::ArrayProfile)
        return isAllZero(asByteSpan(field));
    else if constexpr (kind == FamilyKind::AllocationHint)
        return !field.vectorLengthHintConcurrently();
    else if constexpr (kind == FamilyKind::IterationModes)
        return !field.seenModes;
    else if constexpr (kind == FamilyKind::EnumeratorModes)
        return !field;
    else if constexpr (kind == FamilyKind::ToThisStatus)
        return field == ToThisOK;
    else {
        static_assert(kind == FamilyKind::BranchBit);
        return !field;
    }
}

// S2 (N1): every piece is at the value linking gave it. It reads the CB in place and allocates nothing. It runs after
// validateState, whose V4 matched each family count with this CB and whose V8 bounded every op_new_array_buffer's
// metadata ID by F19's count, so the F19 check below indexes the metadata in range.
static bool isAtLinkState(CodeBlock& codeBlock)
{
    for (const auto& profile : codeBlock.argumentValueProfiles().span()) {
        if (!valueProfileIsAtLinkState(profile))
            return false;
    }

    bool atLinkState = true;
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned, ValueProfile& profile) {
        atLinkState &= valueProfileIsAtLinkState(profile);
    });

    unsigned newArrayBufferEntries = 0;
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
            atLinkState &= familyFieldIsAtLinkState<FamilyType::kind>(field);
            if constexpr (FamilyType::index == CBFormat::newArrayBufferFamily)
                ++newArrayBufferEntries;
            else if constexpr (FamilyType::kind == FamilyKind::AllocationHint)
                atLinkState &= field.selectIndexingTypeConcurrently() == ArrayWithUndecided;
        });
    });

    // F19 holds its instruction's type (N8), reached through the instruction's own entry, as
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

// The strict checks of section 5.2, in its order: V1 to V15, then S1 and S2 on the CB, then S3 on the captured counter.
static std::optional<CBCheck> strictFailure(std::span<const uint8_t> stateSection, CodeBlock& codeBlock)
{
    if (auto check = validateState(stateSection, codeBlock))
        return check;
    if (!isNewborn(codeBlock))
        return CBCheck::StrictNewborn;
    if (!isAtLinkState(codeBlock))
        return CBCheck::StrictLinkState;
    // V1 accepted the header, so the span holds one; copied, as validateState reads it, without assuming alignment.
    CBFormat::StateHeader header { };
    memcpySpan(asMutableByteSpan(header), stateSection.first(sizeof(CBFormat::StateHeader)));
    if (!counterObeysNativeInvariant(header))
        return CBCheck::StrictCounter;
    return std::nullopt;
}

// P4 to P9, one field from its record (section 5.2): the array profile and allocation hint through E1 and E2, which
// assert the samples they leave empty, and every other field assigned.
template<FamilyKind kind, typename Field, typename Record>
static void seedFamilyField(Field& field, const Record& record)
{
    if constexpr (kind == FamilyKind::ArrayProfile)
        field.restoreAccumulatedState(record.observedArrayModes, OptionSet<ArrayProfileFlag>::fromRaw(record.flags));
    else if constexpr (kind == FamilyKind::AllocationHint)
        field.restoreHint(CBFormat::allocationHintIndexingType(record), CBFormat::allocationHintVectorLength(record));
    else if constexpr (kind == FamilyKind::IterationModes)
        field.seenModes = record;
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
    ASSERT(!(reinterpret_cast<uintptr_t>(stateSection.data()) % CBFormat::sectionAlignment)); // R-INT-1
    AssertNoGC assertNoGC;

    if (strict) {
        if (auto check = strictFailure(stateSection, codeBlock))
            return std::unexpected(CBFault { CBFaultKind::InvalidMaterial, *check });
    }

    // Normal mode trusts the section (section 3.4). The assertions keep a span that broke R-INT-1 from being read past its
    // end; with strict on, V1 and V5 have already matched the header and the length exactly.
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
    ASSERT(isNewborn(codeBlock)); // I6: linked, no JIT type, never ran.
    AssertNoGC assertNoGC;

    const CBFormat::StateHeader& header = *m_header;
    StateArrays arrays(m_bytes, header, m_arrayOffsets);

    // P1: offset k from element k - 1.
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, ValueProfile& profile) {
        profile.m_prediction = static_cast<SpeculatedType>(arrays.valuePredictions[offset - 1]);
    });

    // P2.
    std::span<ArgumentValueProfile> argumentProfiles = codeBlock.argumentValueProfiles().mutableSpan();
    ASSERT(argumentProfiles.size() == arrays.argumentPredictions.size());
    for (size_t index = 0; index < arrays.argumentPredictions.size(); ++index)
        argumentProfiles[index].m_prediction = static_cast<SpeculatedType>(arrays.argumentPredictions[index]);

    // P3: through the append OSR exit itself uses (N7), in the records' order, so a repeated key lands in one profile.
#if ENABLE(DFG_JIT)
    CompressedLazyValueProfileHolder& lazyProfiles = codeBlock.lazyValueProfiles();
    for (const auto& record : arrays.lazyOperandProfiles) {
        LazyOperandValueProfileKey key(BytecodeIndex::fromBits(record.bytecodeIndexBits), Operand(static_cast<OperandKind>(record.operandKind), record.operandValue));
        lazyProfiles.addOperandValueProfile(key)->m_prediction = static_cast<SpeculatedType>(record.prediction);
    }
#else
    ASSERT(arrays.lazyOperandProfiles.empty());
#endif

    // P4 to P9: each family's entries in metadata-ID order, in step with its records.
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        auto records = arrays.records<FamilyType::kind>();
        size_t index = CBFormat::firstRecordOfFamily(header.familyEntryCount, FamilyType::index);
        CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
            seedFamilyField<FamilyType::kind>(field, records[index++]);
        });
        ASSERT(index == CBFormat::firstRecordOfFamily(header.familyEntryCount, FamilyType::index) + header.familyEntryCount[FamilyType::index]);
    });

    // P10, P11 (M1), before setup, whose optimizeAfterWarmUp reads the reoptimization count.
    codeBlock.seedBaselineTierUpHistory(header.optimizationDelayCounter, header.reoptimizationRetryCounter);

    // The realm step (N10): a seeded FastMap or FastSet at an iterator_open site implies, as native recording does, that
    // the CB's realm has materialized the function the DFG's handler freezes. The native lazy getters allocate it under
    // the install deferral, only for a seed that carries the bit (I3, I14).
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

    // Setup always creates the BaselineJITData (section 5.1, step 3).
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

    // P as count() computes it, exact for every pair native code leaves.
    double progress = static_cast<double>(header.counterTotalCount) + header.counterValue;
    int32_t threshold = header.counterActiveThreshold;
    int64_t sliceFloor = 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());

    counter.m_activeThreshold = threshold;
    counter.m_totalCount = header.counterTotalCount;
    counter.m_counter = header.counterValue;

    CBCounterRestore restore { .carried = true };
    if (threshold == std::numeric_limits<int32_t>::max()) {
        // setThreshold would defer indefinitely and drop P, so the captured slice stays, which no pool statistic shaped.
        restore.nativeSlice = header.counterValue < 0 ? -static_cast<int64_t>(header.counterValue) : 0;
    } else if (counter.checkIfThresholdCrossedAndSet(&codeBlock)) {
        // This process's pool and this CB's size say the threshold is reached.
        restore.crossed = true;
        restore.nativeSlice = 0;
    } else
        restore.nativeSlice = -static_cast<int64_t>(counter.m_counter); // the slice ExecutionCounter::setThreshold armed from P

    // The floor of THREAD Restoration: at least two entry increments short of crossing. A slice is at most 2^31, so
    // -slice fits int32_t.
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
