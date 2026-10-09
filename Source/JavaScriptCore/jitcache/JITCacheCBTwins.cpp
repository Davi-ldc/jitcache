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

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "GetByIdMetadata.h"
#include "IndexingType.h"
#include "IterationModeMetadata.h"
#include "JITCacheBench.h"
#include "JITCacheCBFormat.h"
#include "JITCacheVMState.h"
#include "JSGlobalObject.h"
#include "LazyOperandValueProfile.h"
#include "LazyValueProfile.h"
#include "Operands.h"
#include "Options.h"
#include "SpeculatedType.h"
#include "StructureID.h"
#include "ToThisStatus.h"
#include "TwinReport.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "ValueProfile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/PrintStream.h>
#include <wtf/RawHex.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The CB lane's twin check (SPEC-cb.md section 11.1). Every piece the lane restores is state the engine cannot recompute,
// so its twin is its cb.state record; the counter's slice is the exception, and its twin is the answer native code gave
// finishCounter, which the check holds to I7's envelope. The check runs between finishCounter and installCode, while no
// marker can reach the CB (N11), reads the CB's metadata only through the guarded walkers (I15), calls no code that reads
// the executable pool, and reports each difference to the integrator's TwinReport. It never skips: nothing it compares
// depends on the process, so no run can lack what it needs.

namespace JSC::JITCache {

namespace CBTwinsInternal {

using CBFormat::FamilyKind;
using CBFormat::StateArray;

// Every detail names the body by its key in hex, when the VM records keys and the CB's UCB has one, and the CB as
// CodeBlock::dump prints it (harness sub-SPEC section 2). The names are built at the first difference, so a check that
// passes looks nothing up.
class Reporter {
public:
    Reporter(TwinReport& report, CodeBlock& codeBlock)
        : m_report(report)
        , m_codeBlock(codeBlock)
    {
    }

    template<typename... Values>
    void difference(ASCIILiteral check, const Values&... values)
    {
        m_report.difference(TwinPart::CB, check, WTF::toString(subject(), ": ", values...));
    }

private:
    const String& subject()
    {
        if (m_subject.isNull()) {
            String body;
            if (VMState* state = m_codeBlock.vm().jitCacheState()) {
                if (auto key = state->registry().keyOf(*m_codeBlock.unlinkedCodeBlock()))
                    body = makeString("body "_s, bodyKeyHex(*key), ", "_s);
            }
            m_subject = makeString(body, WTF::toString(m_codeBlock));
        }
        return m_subject;
    }

    TwinReport& m_report;
    CodeBlock& m_codeBlock;
    String m_subject;
};

// One array of the section as a typed span, at the offset and with the count the layout computes from the header.
template<typename T>
static std::span<const T> stateArray(std::span<const uint8_t> bytes, const CBFormat::StateLayout& layout, StateArray array)
{
    ASSERT(sizeof(T) == CBFormat::elementSize(array));
    return spanReinterpretCast<const T>(bytes.subspan(layout.offset(array), layout.count(array) * sizeof(T)));
}

// The records of the section, arrays 1 to 9 of section 3.2.
struct StateRecords {
    StateRecords(std::span<const uint8_t> bytes, const CBFormat::StateLayout& layout)
        : argumentPredictions(stateArray<uint64_t>(bytes, layout, StateArray::ArgumentPredictions))
        , valuePredictions(stateArray<uint64_t>(bytes, layout, StateArray::ValuePredictions))
        , arrayProfiles(stateArray<CBFormat::ArrayProfileRecord>(bytes, layout, StateArray::ArrayProfiles))
        , lazyOperandProfiles(stateArray<CBFormat::LazyOperandRecord>(bytes, layout, StateArray::LazyOperandProfiles))
        , allocationHints(stateArray<uint16_t>(bytes, layout, StateArray::AllocationHints))
        , iterationModes(stateArray<uint16_t>(bytes, layout, StateArray::IterationModes))
        , enumeratorModes(stateArray<uint8_t>(bytes, layout, StateArray::EnumeratorModes))
        , toThisStatuses(stateArray<uint8_t>(bytes, layout, StateArray::ToThisStatuses))
        , branchBits(stateArray<uint8_t>(bytes, layout, StateArray::BranchBits))
    {
    }

    // The records of a family kind, its families in table order.
    template<FamilyKind kind>
    auto familyRecords() const
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

// P1 and P2: every prediction is its record, and no bucket holds a sample (I4, I9).
static void checkValueProfiles(CodeBlock& codeBlock, const CBFormat::StateHeader& header, const StateRecords& records, Reporter& reporter)
{
    std::span<const ArgumentValueProfile> argumentProfiles = codeBlock.argumentValueProfiles().span();
    if (argumentProfiles.size() != header.numArguments)
        reporter.difference("argument-count"_s, "the CB has ", argumentProfiles.size(), " argument profiles, its record ", header.numArguments);
    size_t comparedArguments = std::min(argumentProfiles.size(), records.argumentPredictions.size());
    for (size_t index = 0; index < comparedArguments; ++index) {
        const ArgumentValueProfile& profile = argumentProfiles[index];
        auto expected = static_cast<SpeculatedType>(records.argumentPredictions[index]);
        if (profile.m_prediction != expected)
            reporter.difference("argument-prediction"_s, "argument ", index, " predicts ", SpeculationDump { profile.m_prediction }, ", its record ", SpeculationDump { expected });
        if (unsigned samples = profile.numberOfSamples())
            reporter.difference("argument-samples"_s, "argument ", index, " holds ", samples, " pending samples");
    }

    uint32_t visited = 0;
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, ValueProfile& profile) {
        ++visited;
        if (offset > records.valuePredictions.size())
            return;
        auto expected = static_cast<SpeculatedType>(records.valuePredictions[offset - 1]);
        if (profile.m_prediction != expected)
            reporter.difference("value-prediction"_s, "the value profile at offset ", offset, " predicts ", SpeculationDump { profile.m_prediction }, ", its record ", SpeculationDump { expected });
        if (unsigned samples = profile.numberOfSamples())
            reporter.difference("value-samples"_s, "the value profile at offset ", offset, " holds ", samples, " pending samples");
    });
    if (visited != header.numValueProfiles)
        reporter.difference("value-count"_s, "the CB has ", visited, " metadata value profiles, its record ", header.numValueProfiles);
}

#if ENABLE(DFG_JIT)

// Key equality is field equality (N7): the bytecode index's bits, the operand's kind and its value.
static bool recordNamesKey(const CBFormat::LazyOperandRecord& record, const LazyOperandValueProfileKey& key)
{
    Operand operand = key.operand();
    return record.bytecodeIndexBits == key.bytecodeIndex().asBits()
        && record.operandKind == static_cast<uint32_t>(operand.kind())
        && record.operandValue == operand.value();
}

static bool recordsShareKey(const CBFormat::LazyOperandRecord& first, const CBFormat::LazyOperandRecord& second)
{
    return first.bytecodeIndexBits == second.bytecodeIndexBits && first.operandKind == second.operandKind && first.operandValue == second.operandValue;
}

#endif // ENABLE(DFG_JIT)

// P3. seedLinkedState appends each record, in order, through addOperandValueProfile, which returns the profile a repeated
// key already has (N7). The newborn's holder was empty (S2), so it lists each key once, at the position of the key's first
// record, with the prediction of the key's last record. The check walks that expected list without building it, since
// the records are few: the distinct exit keys of one CB.
static void checkLazyOperandProfiles(CodeBlock& codeBlock, const StateRecords& records, Reporter& reporter)
{
#if ENABLE(DFG_JIT)
    std::span<const CBFormat::LazyOperandRecord> lazyRecords = records.lazyOperandProfiles;
    auto isFirstOfItsKey = [&](size_t index) {
        for (size_t earlier = 0; earlier < index; ++earlier) {
            if (recordsShareKey(lazyRecords[earlier], lazyRecords[index]))
                return false;
        }
        return true;
    };
    auto lastPredictionOfItsKey = [&](size_t index) {
        uint64_t prediction = lazyRecords[index].prediction;
        for (size_t later = index + 1; later < lazyRecords.size(); ++later) {
            if (recordsShareKey(lazyRecords[later], lazyRecords[index]))
                prediction = lazyRecords[later].prediction;
        }
        return static_cast<SpeculatedType>(prediction);
    };

    size_t record = 0;
    auto advanceToFirstOfAKey = [&] {
        while (record < lazyRecords.size() && !isFirstOfItsKey(record))
            ++record;
    };

    CompressedLazyValueProfileHolder& holder = codeBlock.lazyValueProfiles();
    size_t position = 0;
    holder.forEachOperandValueProfile([&](const LazyOperandValueProfile& profile) {
        advanceToFirstOfAKey();
        LazyOperandValueProfileKey key = profile.key();
        Operand operand = key.operand();
        if (record == lazyRecords.size())
            reporter.difference("lazy-operand-profile"_s, "the holder's profile ", position, " (bytecode index bits ", key.bytecodeIndex().asBits(), ", operand kind ", static_cast<unsigned>(operand.kind()), ", value ", operand.value(), ") has no record");
        else if (!recordNamesKey(lazyRecords[record], key)) {
            const CBFormat::LazyOperandRecord& expected = lazyRecords[record];
            reporter.difference("lazy-operand-profile"_s, "the holder's profile ", position, " has bytecode index bits ", key.bytecodeIndex().asBits(), ", operand kind ", static_cast<unsigned>(operand.kind()), " and value ", operand.value(),
                ", its record ", record, " bits ", expected.bytecodeIndexBits, ", kind ", expected.operandKind, " and value ", expected.operandValue);
        } else {
            SpeculatedType expected = lastPredictionOfItsKey(record);
            if (profile.m_prediction != expected)
                reporter.difference("lazy-operand-profile"_s, "the holder's profile ", position, " predicts ", SpeculationDump { profile.m_prediction }, ", its records ", SpeculationDump { expected });
        }
        if (unsigned samples = profile.numberOfSamples())
            reporter.difference("lazy-operand-samples"_s, "the holder's profile ", position, " holds ", samples, " pending samples");
        if (record < lazyRecords.size())
            ++record;
        ++position;
    });
    advanceToFirstOfAKey();
    while (record < lazyRecords.size()) {
        const CBFormat::LazyOperandRecord& missing = lazyRecords[record];
        reporter.difference("lazy-operand-profile"_s, "record ", record, " (bytecode index bits ", missing.bytecodeIndexBits, ", operand kind ", missing.operandKind, ", value ", missing.operandValue, ") has no profile in the holder");
        ++record;
        advanceToFirstOfAKey();
    }

    // The standalone failure buckets are pending samples too (section 1), and only OSR exit allocates them.
    if (!holder.speculationFailureValueProfileBucketsMap().isEmpty())
        reporter.difference("speculation-failure-buckets"_s, "the lazy-operand holder has standalone failure buckets");
#else
    UNUSED_PARAM(codeBlock);
    if (!records.lazyOperandProfiles.empty())
        reporter.difference("lazy-operand-profile"_s, "the section holds ", records.lazyOperandProfiles.size(), " lazy-operand records, which a build without the DFG cannot seed");
#endif
}

static bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

// The two StructureID samples of an array profile, which ArrayProfile exposes only by offset, as the emitted code
// reaches them. A null StructureID is zero bits.
static bool arrayProfileSamplesAreEmpty(const ArrayProfile& profile)
{
    std::span<const uint8_t> bytes = asByteSpan(profile);
    return isAllZero(bytes.subspan(static_cast<size_t>(ArrayProfile::offsetOfLastSeenStructureID()), sizeof(StructureID)))
        && isAllZero(bytes.subspan(static_cast<size_t>(ArrayProfile::offsetOfSpeculationFailureStructureID()), sizeof(StructureID)));
}

// An allocation profile's last array is a pending sample (section 1) that only the profile's private storage holds. On
// 64-bit targets that storage is one word packing the pointer with the hint, so the profile holds no last array exactly
// when its bytes equal those of a profile given the same hint and no array.
static bool allocationProfileHoldsNoLastArray(const ArrayAllocationProfile& profile, IndexingType indexingType, unsigned vectorLength)
{
#if CPU(ADDRESS64)
    static_assert(sizeof(ArrayAllocationProfile) == sizeof(uint64_t));
    ArrayAllocationProfile withoutArray;
    withoutArray.restoreHint(indexingType, vectorLength);
    return equalSpans(asByteSpan(profile), asByteSpan(withoutArray));
#else
    UNUSED_PARAM(profile);
    UNUSED_PARAM(indexingType);
    UNUSED_PARAM(vectorLength);
    return true;
#endif
}

// P4 to P9 for one field: the value seedLinkedState wrote from the record, and no sample beside it (I4, I9).
template<FamilyKind kind, typename Field, typename Record>
static void checkFamilyField(Reporter& reporter, unsigned family, uint32_t entry, Field& field, const Record& record)
{
    if constexpr (kind == FamilyKind::ArrayProfile) {
        const ArrayProfile& profile = field;
        uint32_t modes = profile.observedArrayModes();
        uint32_t flags = profile.arrayProfileFlags().toRaw();
        if (modes != record.observedArrayModes || flags != record.flags) {
            reporter.difference("array-profile"_s, "family ", family, " entry ", entry, " has modes ", RawHex(modes), " and flags ", RawHex(flags),
                ", its record modes ", RawHex(record.observedArrayModes), " and flags ", RawHex(record.flags));
        }
        if (!arrayProfileSamplesAreEmpty(profile))
            reporter.difference("array-profile-samples"_s, "family ", family, " entry ", entry, " holds a StructureID sample");
    } else if constexpr (kind == FamilyKind::AllocationHint) {
        IndexingType indexingType = CBFormat::allocationHintIndexingType(record);
        unsigned vectorLength = CBFormat::allocationHintVectorLength(record);
        IndexingType seededType = field.selectIndexingTypeConcurrently();
        unsigned seededLength = field.vectorLengthHintConcurrently();
        if (seededType != indexingType || seededLength != vectorLength) {
            reporter.difference("allocation-hint"_s, "family ", family, " entry ", entry, " hints indexing type ", RawHex(seededType), " and vector length ", seededLength,
                ", its record ", RawHex(indexingType), " and ", vectorLength);
        } else if (!allocationProfileHoldsNoLastArray(field, indexingType, vectorLength))
            reporter.difference("allocation-last-array"_s, "family ", family, " entry ", entry, " holds a last array");
    } else if constexpr (kind == FamilyKind::IterationModes) {
        if (field.seenModes != record)
            reporter.difference("iteration-modes"_s, "family ", family, " entry ", entry, " has modes ", RawHex(field.seenModes), ", its record ", RawHex(record));
    } else if constexpr (kind == FamilyKind::EnumeratorModes) {
        if (static_cast<uint8_t>(field) != record)
            reporter.difference("enumerator-modes"_s, "family ", family, " entry ", entry, " has modes ", RawHex(static_cast<uint8_t>(field)), ", its record ", RawHex(record));
    } else if constexpr (kind == FamilyKind::ToThisStatus) {
        if (static_cast<uint8_t>(field) != record)
            reporter.difference("to-this-status"_s, "family ", family, " entry ", entry, " has status ", static_cast<unsigned>(field), ", its record ", static_cast<unsigned>(record));
    } else {
        static_assert(kind == FamilyKind::BranchBit);
        if (static_cast<uint8_t>(field) != record)
            reporter.difference("branch-bit"_s, "family ", family, " entry ", entry, " has bit ", static_cast<unsigned>(field), ", its record ", static_cast<unsigned>(record));
    }
}

// P4 to P9: each family's entries in metadata-ID order, in step with its records, as seedLinkedState wrote them.
static void checkFamilies(CodeBlock& codeBlock, const CBFormat::StateHeader& header, const StateRecords& records, Reporter& reporter)
{
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        auto familyRecords = records.familyRecords<FamilyType::kind>();
        size_t firstRecord = CBFormat::firstRecordOfFamily(header.familyEntryCount, FamilyType::index);
        uint32_t recordedEntries = header.familyEntryCount[FamilyType::index];
        uint32_t entry = 0;
        CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
            if (entry < recordedEntries && firstRecord + entry < familyRecords.size())
                checkFamilyField<FamilyType::kind>(reporter, FamilyType::index, entry, field, familyRecords[firstRecord + entry]);
            ++entry;
        });
        if (entry != recordedEntries)
            reporter.difference("family-count"_s, "family ", FamilyType::index, " has ", entry, " entries, its record ", recordedEntries);
    });
}

// P10 and P11.
static void checkTierUpHistory(CodeBlock& codeBlock, const CBFormat::StateHeader& header, Reporter& reporter)
{
    unsigned recordedDelays = header.optimizationDelayCounter;
    unsigned recordedReoptimizations = header.reoptimizationRetryCounter;
    if (codeBlock.optimizationDelayCounter() != recordedDelays)
        reporter.difference("optimization-delay"_s, "the CB has ", codeBlock.optimizationDelayCounter(), " profile deferrals, its record ", recordedDelays);
    if (codeBlock.reoptimizationRetryCounter() != recordedReoptimizations)
        reporter.difference("reoptimization-count"_s, "the CB has ", codeBlock.reoptimizationRetryCounter(), " reoptimizations, its record ", recordedReoptimizations);
}

// The counter and finishCounter's result, printed after each counter difference.
struct CounterDump {
    void dump(PrintStream& out) const
    {
        out.print(" (counter ", counter.m_counter, ", total ", counter.m_totalCount, ", threshold ", counter.m_activeThreshold,
            "; restore carried ", restore.carried, ", crossed ", restore.crossed, ", nativeSlice ", restore.nativeSlice, ", slice ", restore.slice, ")");
    }

    const BaselineExecutionCounter& counter;
    const CBCounterRestore& restore;
};

// THREAD Restoration's floor: two entry increments short of crossing.
static int64_t entrySliceFloor()
{
    return 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());
}

// I7 for a carried counter. The counter holds the captured threshold and the slice finishCounter reports, at least the
// floor short of crossing, with the captured progress as the float total keeps it, and aging samples that count. The
// slice itself is checked against what no pool can move: an infinite threshold keeps the captured slice, and a finite
// one lies in the envelope that M >= 1 leaves, whose bounds come from P, T and C alone (N4). C reads only options and the
// CB's size and code type, so nothing here depends on the executable pool.
static void checkCarriedCounter(CodeBlock& codeBlock, const CBFormat::StateHeader& header, const BaselineExecutionCounter& counter, const CBCounterRestore& restore, Reporter& reporter)
{
    CounterDump state { counter, restore };
    double progress = static_cast<double>(header.counterTotalCount) + header.counterValue;
    int32_t threshold = header.counterActiveThreshold;
    int64_t floorSlice = entrySliceFloor();

    if (!restore.carried)
        reporter.difference("counter-mode"_s, "the section carries the counter, finishCounter's result says it does not", state);
    if (counter.m_activeThreshold != threshold)
        reporter.difference("counter-threshold"_s, "the active threshold is not the record's ", threshold, state);
    if (restore.slice != std::max(restore.nativeSlice, floorSlice))
        reporter.difference("counter-floor"_s, "the slice is not the larger of the native slice and the floor ", floorSlice, state);
    if (static_cast<int64_t>(counter.m_counter) != -restore.slice)
        reporter.difference("counter-slice"_s, "the counter does not hold the reported slice", state);
    double expectedCount = static_cast<double>(static_cast<float>(progress + static_cast<double>(restore.slice))) - static_cast<double>(restore.slice);
    if (counter.count() != expectedCount)
        reporter.difference("counter-progress"_s, "count() is ", counter.count(), ", the record's progress ", progress, " kept by the float total is ", expectedCount, state);
    if (codeBlock.previousCounterForAging() != static_cast<float>(counter.count()))
        reporter.difference("counter-aging"_s, "the aging sample is ", codeBlock.previousCounterForAging(), ", not the restored count", state);

    if (threshold == std::numeric_limits<int32_t>::max()) {
        int64_t capturedSlice = header.counterValue < 0 ? -static_cast<int64_t>(header.counterValue) : 0;
        if (restore.crossed || restore.nativeSlice != capturedSlice)
            reporter.difference("counter-captured-slice"_s, "an infinite threshold did not keep the captured slice ", capturedSlice, state);
        return;
    }

    int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
    bool sliceInRange = restore.crossed ? !restore.nativeSlice : restore.nativeSlice >= 0 && restore.nativeSlice <= ceiling;
    if (!sliceInRange)
        reporter.difference("counter-slice-range"_s, "the native slice is not 0 after a crossing or lies outside [0, ", ceiling, "]", state);
    double earlyBound = threshold - static_cast<double>(std::min<int64_t>(threshold, ceiling)) / 2;
    if (progress < earlyBound) {
        auto shortestSlice = static_cast<int64_t>(std::trunc(std::min(threshold - progress, static_cast<double>(ceiling))));
        if (restore.crossed || restore.nativeSlice < shortestSlice)
            reporter.difference("counter-early-crossing"_s, "progress ", progress, " below ", earlyBound, " crossed or armed less than ", shortestSlice, state);
    }
}

// I7 for a counter that did not travel: finishCounter wrote nothing, so the counter is setup's arming by
// CodeBlock::optimizeAfterWarmUpImpl from the seeded reoptimization count and the UCB's quick DFG bit (N4), and the aging
// sample is setup's sample of the fresh counter. adjustedCounterValue reads the CB's size and reoptimization count, never
// the pool.
static void checkSetupArming(CodeBlock& codeBlock, const BaselineExecutionCounter& counter, const CBCounterRestore& restore, Reporter& reporter)
{
    CounterDump state { counter, restore };
    if (restore.carried || restore.crossed)
        reporter.difference("counter-mode"_s, "the section does not carry the counter, finishCounter's result says it was carried or crossed", state);
    if (restore.nativeSlice != -static_cast<int64_t>(counter.m_counter) || restore.slice != restore.nativeSlice)
        reporter.difference("counter-slice"_s, "the result does not report the slice setup armed", state);

#if ENABLE(DFG_JIT)
    int32_t warmUpThreshold = Options::thresholdForOptimizeAfterWarmUp();
    if (codeBlock.unlinkedCodeBlock()->isQuickDFGTierUp())
        warmUpThreshold = static_cast<int32_t>(warmUpThreshold * Options::quickDFGTierUpThresholdFactor());
    int32_t armedThreshold = codeBlock.adjustedCounterValue(warmUpThreshold);
#else
    // Without the DFG, setup arms nothing and the counter stays as BaselineJITData's constructor reset it.
    int32_t armedThreshold = 0;
#endif
    if (counter.m_activeThreshold != armedThreshold)
        reporter.difference("counter-arming"_s, "the active threshold is not setup's arming ", armedThreshold, state);
    if (armedThreshold == std::numeric_limits<int32_t>::max()) {
        if (counter.m_counter != std::numeric_limits<int32_t>::min() || counter.m_totalCount)
            reporter.difference("counter-progress"_s, "an infinite arming is not the indefinite deferral", state);
    } else {
        double count = counter.count();
        if (!(count >= 0 && count <= 1))
            reporter.difference("counter-progress"_s, "a fresh arming holds ", count, " points of progress", state);
    }
    if (codeBlock.previousCounterForAging())
        reporter.difference("counter-aging"_s, "the aging sample is ", codeBlock.previousCounterForAging(), ", not setup's sample of the fresh counter", state);
}

// P12 (I7).
static void checkCounter(CodeBlock& codeBlock, const CBFormat::StateHeader& header, const CBCounterRestore& restore, Reporter& reporter)
{
    BaselineJITData* jitData = codeBlock.baselineJITData();
    if (!jitData) {
        reporter.difference("counter-setup"_s, "the CB has no BaselineJITData after setup");
        return;
    }
    const BaselineExecutionCounter& counter = jitData->executeCounter();
    // finishCounter carries the counter only for CounterMode::Carried and leaves setup's arming otherwise.
    if (header.counterMode == static_cast<uint8_t>(CBFormat::CounterMode::Carried))
        checkCarriedCounter(codeBlock, header, counter, restore, reporter);
    else
        checkSetupArming(codeBlock, counter, restore, reporter);
}

// I14: a FastMap or FastSet record at an iterator_open site left the realm's function materialized.
static void checkRealmStep(CodeBlock& codeBlock, const CBFormat::StateHeader& header, const StateRecords& records, Reporter& reporter)
{
    size_t firstRecord = CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::iteratorOpenIterationModesFamily);
    uint16_t iteratorOpenModes = 0;
    for (uint16_t modes : records.iterationModes.subspan(firstRecord, header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily]))
        iteratorOpenModes |= modes;
    JSGlobalObject* globalObject = codeBlock.globalObject();
    if ((iteratorOpenModes & static_cast<uint16_t>(IterationMode::FastMap)) && !globalObject->mapProtoEntriesFunctionConcurrently())
        reporter.difference("realm-step"_s, "an iterator_open record holds FastMap, and the realm has no Map.prototype.entries function");
    if ((iteratorOpenModes & static_cast<uint16_t>(IterationMode::FastSet)) && !globalObject->setProtoValuesFunctionConcurrently())
        reporter.difference("realm-step"_s, "an iterator_open record holds FastSet, and the realm has no Set.prototype.values function");
}

// GetByIdModeMetadata is a union with no operator== whose constructor leaves padding4 unwritten, so the LLInt cache is
// compared field by field with what its constructor stores (section 11.1). The hit count is the option as the uint8_t
// field narrows it.
static bool llintCacheIsAtLinkState(const GetByIdModeMetadata& cache)
{
    return cache.mode == GetByIdMode::Default
        && cache.hitCountForLLIntCaching == static_cast<uint8_t>(Options::prototypeHitCountForLLIntCaching())
        && !cache.defaultMode.structureID
        && !cache.defaultMode.cachedOffset;
}

// Section 6.4: in the metadata entries that hold this lane's fields beside native ones, the native LLInt caches and the
// to_this cached structure still hold what linking gave them, so the lane wrote nothing beside its own fields (I4). The
// DataOnlyCallLinkInfo fields are the ICs lane's seeds and stay outside this check.
static void checkNativeEntries(CodeBlock& codeBlock, Reporter& reporter)
{
    auto checkLLIntCache = [&](const GetByIdModeMetadata& cache, const char* opcode, const char* field, unsigned entry) {
        if (llintCacheIsAtLinkState(cache))
            return;
        reporter.difference("llint-cache"_s, opcode, " entry ", entry, ' ', field, " has mode ", static_cast<unsigned>(cache.mode), ", hit count ", static_cast<unsigned>(cache.hitCountForLLIntCaching),
            ", StructureID ", RawHex(cache.defaultMode.structureID.bits()), " and offset ", cache.defaultMode.cachedOffset);
    };

    unsigned entry = 0;
    CBFormat::forEachFamilyEntry<OpGetLength>(codeBlock, [&](OpGetLength::Metadata& metadata) {
        checkLLIntCache(metadata.m_modeMetadata, "get_length", "m_modeMetadata", entry++);
    });
    entry = 0;
    CBFormat::forEachFamilyEntry<OpIteratorOpen>(codeBlock, [&](OpIteratorOpen::Metadata& metadata) {
        checkLLIntCache(metadata.m_modeMetadata, "iterator_open", "m_modeMetadata", entry++);
    });
    entry = 0;
    CBFormat::forEachFamilyEntry<OpAsyncIteratorOpen>(codeBlock, [&](OpAsyncIteratorOpen::Metadata& metadata) {
        checkLLIntCache(metadata.m_modeMetadata, "async_iterator_open", "m_modeMetadata", entry++);
    });
    entry = 0;
    CBFormat::forEachFamilyEntry<OpIteratorNext>(codeBlock, [&](OpIteratorNext::Metadata& metadata) {
        checkLLIntCache(metadata.m_doneModeMetadata, "iterator_next", "m_doneModeMetadata", entry);
        checkLLIntCache(metadata.m_valueModeMetadata, "iterator_next", "m_valueModeMetadata", entry);
        ++entry;
    });
    entry = 0;
    CBFormat::forEachFamilyEntry<OpToThis>(codeBlock, [&](OpToThis::Metadata& metadata) {
        if (metadata.m_cachedStructureID)
            reporter.difference("to-this-structure"_s, "to_this entry ", entry, " caches StructureID ", RawHex(metadata.m_cachedStructureID.bits()));
        ++entry;
    });
}

} // namespace CBTwinsInternal

void CBStateImport::verifyTwins(CodeBlock& codeBlock, const CBCounterRestore& restore, TwinReport& report) const
{
    using namespace CBTwinsInternal;

    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    // The check only reads the CB; what it allocates is the malloc'd text of a difference, never a cell.
    AssertNoGC assertNoGC;

    const CBFormat::StateHeader& header = *m_header;
    // prepare laid the section out from this header, checked that it fits the borrowed span and kept the offsets.
    auto layout = CBFormat::stateLayout(header);
    RELEASE_ASSERT(layout && layout->size <= m_bytes.size());
    ASSERT(std::ranges::equal(layout->offsets, m_arrayOffsets));
    StateRecords records(m_bytes, *layout);
    Reporter reporter(report, codeBlock);

    checkValueProfiles(codeBlock, header, records, reporter);
    checkLazyOperandProfiles(codeBlock, records, reporter);
    checkFamilies(codeBlock, header, records, reporter);
    checkTierUpHistory(codeBlock, header, reporter);
    checkCounter(codeBlock, header, restore, reporter);
    checkRealmStep(codeBlock, header, records, reporter);
    checkNativeEntries(codeBlock, reporter);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
