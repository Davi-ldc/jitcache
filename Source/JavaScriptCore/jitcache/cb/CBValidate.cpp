#include "config.h"
#include "CBState.h"

#if ENABLE(JIT)

#include "ByteFields.h"
#include "BytecodeIndex.h"
#include "CBFormat.h"
#include "CodeBlock.h"
#include "Operands.h"
#include "Options.h"
#include "VirtualRegister.h"
#include <cmath>
#include <initializer_list>
#include <limits>

// The structural rules of cb.state and cb.summary, the native counter invariant, and the token and detail of every
// CBCheck. The rules run only with strict on. A strict capture applies them to what it built, so it never commits what a
// strict consumer's prepare would reject. They read the section with byte-wise loads, since a strict consumer must
// reject malformed bytes and cannot assume they arrived aligned.

namespace JSC::JITCache {

namespace CBValidateInternal {

template<typename Layout>
static bool paddingIsZero(std::span<const uint8_t> section, const Layout& layout)
{
    for (size_t offset = layout.paddingOffset; offset < layout.size; ++offset) {
        if (section[offset])
            return false;
    }
    return true;
}

// A lazy-operand record: what the constructors of LazyOperandValueProfileKey and Operand assert, plus no constant
// register, and an offset inside the instruction stream, which also rejects the hash table's empty and deleted index
// values. The operand is not tied to this CB's frame, since the key can name any slot of an inlining compile's machine
// frame, which the engine only hashes and compares.
static bool lazyOperandRecordFits(const CBFormat::LazyOperandRecord& record, unsigned instructionsSize)
{
    if (record.reserved || !CBFormat::predictionFits(record.prediction))
        return false;
    if (BytecodeIndex::fromBits(record.bytecodeIndexBits).offset() >= instructionsSize)
        return false;
    switch (record.operandKind) {
    case static_cast<uint32_t>(OperandKind::Tmp):
        return record.operandValue >= 0;
    case static_cast<uint32_t>(OperandKind::Argument):
    case static_cast<uint32_t>(OperandKind::Local): {
        VirtualRegister virtualRegister(record.operandValue);
        bool isLocalKind = record.operandKind == static_cast<uint32_t>(OperandKind::Local);
        return virtualRegister.isLocal() == isLocalKind && virtualRegister.isValid() && !virtualRegister.isConstant();
    }
    default:
        return false;
    }
}

static_assert(static_cast<uint32_t>(lastOperandKind) == static_cast<uint32_t>(OperandKind::Tmp), "lazyOperandRecordFits accepts exactly Argument, Local and Tmp");

} // namespace CBValidateInternal

std::optional<CBCheck> validateState(std::span<const uint8_t> stateSection, CodeBlock& codeBlock)
{
    using CBFormat::StateArray;

    if (stateSection.size() < sizeof(CBFormat::StateHeader))
        return CBCheck::StateHeader;
    auto header = readField<CBFormat::StateHeader>(stateSection, 0);
    if (header.layoutVersion != CBFormat::stateLayoutVersion
        || header.tier != static_cast<uint8_t>(CBFormat::Tier::Baseline)
        || !CBFormat::counterModeFits(header.counterMode))
        return CBCheck::StateHeader;

    // The counts match the CB, so every array the seeds walk in step with the CB has one record per slot.
    if (header.numArguments != codeBlock.numParameters() || header.numArguments != codeBlock.argumentValueProfiles().size())
        return CBCheck::ArgumentCount;

    if (header.numValueProfiles != codeBlock.unlinkedCodeBlock()->metadata().numValueProfiles())
        return CBCheck::ValueProfileCount;

    if (header.familyEntryCount != CBFormat::familyEntryCounts(codeBlock))
        return CBCheck::FamilyCount;

    auto layout = CBFormat::stateLayout(header);
    if (!layout || stateSection.size() != layout->size || !CBValidateInternal::paddingIsZero(stateSection, *layout))
        return CBCheck::StateLength;

    // Every value lies in the domain native code produces at its site.
    for (auto array : { StateArray::ArgumentPredictions, StateArray::ValuePredictions }) {
        for (size_t index = 0; index < layout->count(array); ++index) {
            if (!CBFormat::predictionFits(readField<uint64_t>(stateSection, layout->elementOffset(array, index))))
                return CBCheck::ValuePrediction;
        }
    }

    for (size_t index = 0; index < layout->count(StateArray::ArrayProfiles); ++index) {
        auto record = readField<CBFormat::ArrayProfileRecord>(stateSection, layout->elementOffset(StateArray::ArrayProfiles, index));
        if (!CBFormat::arrayModesFit(record.observedArrayModes) || !CBFormat::arrayProfileFlagsFit(record.flags))
            return CBCheck::ArrayProfile;
    }

    // Allocation hints: each record's vector length and type set, then each literal's lower bound through its own
    // instruction. Both native readers of an op_new_array_buffer hint reach it through the executing instruction's own
    // entry, so this walk covers every hint a reader can reach.
    size_t hintIndex = 0;
    for (unsigned family = CBFormat::firstAllocationHintFamily; family < CBFormat::firstAllocationHintFamily + CBFormat::numberOfAllocationHintFamilies; ++family) {
        for (uint32_t entry = 0; entry < header.familyEntryCount[family]; ++entry, ++hintIndex) {
            auto hint = readField<uint16_t>(stateSection, layout->elementOffset(StateArray::AllocationHints, hintIndex));
            if (!CBFormat::allocationHintVectorLengthFits(CBFormat::allocationHintVectorLength(hint))
                || !CBFormat::allocationHintTypeFits(family, CBFormat::allocationHintIndexingType(hint)))
                return CBCheck::AllocationHint;
        }
    }
    if (uint32_t literalCount = header.familyEntryCount[CBFormat::newArrayBufferFamily]) {
        size_t firstLiteralHint = CBFormat::firstRecordOfFamily(header.familyEntryCount, CBFormat::newArrayBufferFamily);
        bool literalsFit = true;
        CBFormat::forEachNewArrayBufferLiteral(codeBlock, [&](unsigned metadataID, IndexingType recommendedIndexingType) {
            if (!literalsFit)
                return;
            if (metadataID >= literalCount) {
                literalsFit = false;
                return;
            }
            auto hint = readField<uint16_t>(stateSection, layout->elementOffset(StateArray::AllocationHints, firstLiteralHint + metadataID));
            literalsFit = CBFormat::allocationHintFits(CBFormat::newArrayBufferFamily, CBFormat::allocationHintIndexingType(hint), recommendedIndexingType);
        });
        if (!literalsFit)
            return CBCheck::AllocationHint;
    }

    size_t modesIndex = 0;
    for (unsigned family = CBFormat::firstIterationModesFamily; family < CBFormat::firstIterationModesFamily + CBFormat::numberOfIterationModesFamilies; ++family) {
        for (uint32_t entry = 0; entry < header.familyEntryCount[family]; ++entry, ++modesIndex) {
            if (!CBFormat::iterationModesFit(family, readField<uint16_t>(stateSection, layout->elementOffset(StateArray::IterationModes, modesIndex))))
                return CBCheck::IterationModes;
        }
    }

    for (size_t index = 0; index < layout->count(StateArray::EnumeratorModes); ++index) {
        if (!CBFormat::enumeratorModesFit(readField<uint8_t>(stateSection, layout->elementOffset(StateArray::EnumeratorModes, index))))
            return CBCheck::EnumeratorModes;
    }

    for (size_t index = 0; index < layout->count(StateArray::ToThisStatuses); ++index) {
        if (!CBFormat::toThisStatusFits(readField<uint8_t>(stateSection, layout->elementOffset(StateArray::ToThisStatuses, index))))
            return CBCheck::ToThisStatus;
    }

    for (size_t index = 0; index < layout->count(StateArray::BranchBits); ++index) {
        if (!CBFormat::branchBitFits(readField<uint8_t>(stateSection, layout->elementOffset(StateArray::BranchBits, index))))
            return CBCheck::BranchBit;
    }

    // Lazy-operand keys may repeat and come in any order: seeding collapses a repeated key into one profile, and no
    // engine reader needs unique keys.
    unsigned instructionsSize = codeBlock.instructionsSize();
    for (size_t index = 0; index < layout->count(StateArray::LazyOperandProfiles); ++index) {
        auto record = readField<CBFormat::LazyOperandRecord>(stateSection, layout->elementOffset(StateArray::LazyOperandProfiles, index));
        if (!CBValidateInternal::lazyOperandRecordFits(record, instructionsSize))
            return CBCheck::LazyOperand;
    }

    // Native code keeps both counts within their options: shouldOptimizeNowFromBaseline stops counting deferrals at
    // maximumOptimizationDelay, and countReoptimization saturates at reoptimizationRetryCounterMax.
    if (header.optimizationDelayCounter > Options::maximumOptimizationDelay() || header.reoptimizationRetryCounter > Options::reoptimizationRetryCounterMax())
        return CBCheck::TierUpHistory;

    // A carried counter has a non-negative threshold and a finite, non-negative total; one that does not travel is all
    // zeros.
    if (header.counterMode == static_cast<uint8_t>(CBFormat::CounterMode::Carried)) {
        if (header.counterActiveThreshold < 0 || !std::isfinite(header.counterTotalCount) || header.counterTotalCount < 0)
            return CBCheck::Counter;
    } else if (header.counterValue || header.counterTotalCount || header.counterActiveThreshold)
        return CBCheck::Counter;

    return std::nullopt;
}

std::optional<CBCheck> validateSummary(std::span<const uint8_t> summarySection)
{
    using CBFormat::SummaryArray;

    // The summary's counts match no CodeBlock, since scoring runs without one.
    if (summarySection.size() < sizeof(CBFormat::SummaryHeader))
        return CBCheck::SummaryHeader;
    auto header = readField<CBFormat::SummaryHeader>(summarySection, 0);
    if (header.layoutVersion != CBFormat::summaryLayoutVersion
        || header.tier != static_cast<uint8_t>(CBFormat::Tier::Baseline)
        || !CBFormat::counterModeFits(header.counterMode))
        return CBCheck::SummaryHeader;

    auto layout = CBFormat::summaryLayout(header);
    if (!layout || summarySection.size() != layout->size || !CBValidateInternal::paddingIsZero(summarySection, *layout))
        return CBCheck::SummaryLength;

    for (auto array : { SummaryArray::ArgumentCategories, SummaryArray::ValueCategories, SummaryArray::LazyOperandCategories }) {
        for (size_t index = 0; index < layout->count(array); ++index) {
            if (!CBFormat::predictionFits(readField<uint64_t>(summarySection, layout->elementOffset(array, index))))
                return CBCheck::SummaryValue;
        }
    }

    for (size_t index = 0; index < layout->count(SummaryArray::ArrayFlags); ++index) {
        if (!CBFormat::arrayProfileFlagsFit(readField<uint8_t>(summarySection, layout->elementOffset(SummaryArray::ArrayFlags, index))))
            return CBCheck::SummaryValue;
    }

    // The progress, which capture caps at the active threshold, an int32_t, and leaves at zero for a counter that does
    // not travel.
    if (header.counterProgress > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return CBCheck::SummaryValue;
    if (header.counterMode == static_cast<uint8_t>(CBFormat::CounterMode::NotCarried) && header.counterProgress)
        return CBCheck::SummaryValue;

    return std::nullopt;
}

bool counterObeysNativeInvariant(const CBFormat::StateHeader& header)
{
    // Native counters keep a carried counter's progress from going negative unless the threshold is infinite, up to one
    // float rounding. ExecutionCounter::setThreshold arms a slice from a progress P that is not negative: it stores
    // -trunc(slice) in m_counter and the float nearest P + slice in m_totalCount. Past 2^24, which a slice scaled by the
    // startup deferral scale can reach, that float may lie below trunc(slice), so count() is slightly negative. Rounding
    // to float keeps order, so the total is never below the float nearest -m_counter, and the counter's increments only
    // raise m_counter.
    if (header.counterMode != static_cast<uint8_t>(CBFormat::CounterMode::Carried))
        return true;
    if (header.counterActiveThreshold == std::numeric_limits<int32_t>::max())
        return true;
    return header.counterTotalCount >= static_cast<float>(-static_cast<double>(header.counterValue));
}

ASCIILiteral description(CBCheck check)
{
    switch (check) {
    case CBCheck::StateHeader:
        return "state-header"_s;
    case CBCheck::ArgumentCount:
        return "state-argument-count"_s;
    case CBCheck::ValueProfileCount:
        return "state-value-profile-count"_s;
    case CBCheck::FamilyCount:
        return "state-family-count"_s;
    case CBCheck::StateLength:
        return "state-length"_s;
    case CBCheck::ValuePrediction:
        return "state-value-prediction"_s;
    case CBCheck::ArrayProfile:
        return "state-array-profile"_s;
    case CBCheck::AllocationHint:
        return "state-allocation-hint"_s;
    case CBCheck::IterationModes:
        return "state-iteration-modes"_s;
    case CBCheck::EnumeratorModes:
        return "state-enumerator-modes"_s;
    case CBCheck::ToThisStatus:
        return "state-to-this-status"_s;
    case CBCheck::BranchBit:
        return "state-branch-bit"_s;
    case CBCheck::LazyOperand:
        return "state-lazy-operand"_s;
    case CBCheck::TierUpHistory:
        return "state-tier-up-history"_s;
    case CBCheck::Counter:
        return "state-counter"_s;
    case CBCheck::SummaryHeader:
        return "summary-header"_s;
    case CBCheck::SummaryLength:
        return "summary-length"_s;
    case CBCheck::SummaryValue:
        return "summary-value"_s;
    case CBCheck::StrictNewborn:
        return "strict-newborn"_s;
    case CBCheck::StrictLinkState:
        return "strict-link-state"_s;
    case CBCheck::StrictCounter:
        return "strict-counter"_s;
    case CBCheck::CapturePairing:
        return "capture-pairing"_s;
    case CBCheck::CaptureCharge:
        return "capture-charge"_s;
    case CBCheck::CaptureStrict:
        return "capture-strict"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ASCIILiteral faultDetail(CBCheck check)
{
    switch (check) {
    case CBCheck::StateHeader:
        return "a cb.state too short for its header, or of another layout version, tier or counter mode"_s;
    case CBCheck::ArgumentCount:
        return "an argument count that differs from the CodeBlock's"_s;
    case CBCheck::ValueProfileCount:
        return "a value-profile count that differs from the CodeBlock's"_s;
    case CBCheck::FamilyCount:
        return "metadata entry counts that differ from the CodeBlock's"_s;
    case CBCheck::StateLength:
        return "a cb.state length or padding other than its counts lay out"_s;
    case CBCheck::ValuePrediction:
        return "an argument or value prediction outside the types bytecode sees"_s;
    case CBCheck::ArrayProfile:
        return "array modes or flags no array profile records"_s;
    case CBCheck::AllocationHint:
        return "an allocation hint its site cannot hold"_s;
    case CBCheck::IterationModes:
        return "iteration modes its opcode never records"_s;
    case CBCheck::EnumeratorModes:
        return "enumerator modes outside the enumerator's flags"_s;
    case CBCheck::ToThisStatus:
        return "an unknown to_this status"_s;
    case CBCheck::BranchBit:
        return "a jneq_ptr bit other than 0 or 1"_s;
    case CBCheck::LazyOperand:
        return "a lazy-operand key or prediction the engine cannot hold"_s;
    case CBCheck::TierUpHistory:
        return "profile deferrals or reoptimizations above their option's bound"_s;
    case CBCheck::Counter:
        return "baseline counter fields no native counter holds"_s;
    case CBCheck::SummaryHeader:
        return "a cb.summary too short for its header, or of another layout version, tier or counter mode"_s;
    case CBCheck::SummaryLength:
        return "a cb.summary length or padding other than its counts lay out"_s;
    case CBCheck::SummaryValue:
        return "a cb.summary category, array flag or counter progress out of its bounds"_s;
    case CBCheck::StrictNewborn:
        return "a CodeBlock that is not newborn"_s;
    case CBCheck::StrictLinkState:
        return "a CodeBlock that is no longer at its link state"_s;
    case CBCheck::StrictCounter:
        return "counter progress under a finite threshold, negative beyond its float total's rounding"_s;
    case CBCheck::CapturePairing:
        return "UnlinkedCodeBlock profile copies that do not pair with the CodeBlock's profiles"_s;
    case CBCheck::CaptureCharge:
        return "section buffers the producer budget refused"_s;
    case CBCheck::CaptureStrict:
        return "a CodeBlock the capture cannot read, or sections that fail their own validation"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
