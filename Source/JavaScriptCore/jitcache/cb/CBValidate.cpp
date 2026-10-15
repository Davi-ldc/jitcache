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

// The structural rules of cb.state and cb.summary, the native counter invariant, and the description of every CBCheck.
// The rules run only with strict on. A strict capture applies them to what it built, so it never commits what a strict
// consumer's prepare would reject. They read the section with byte-wise loads, since a strict consumer must reject
// malformed bytes and cannot assume they arrived aligned.

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
        || !CBFormat::counterModeFits(header.counterMode)
        || header.reserved1)
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

    // The header, which has no reserved field. The summary's counts match no CodeBlock, since scoring runs without one.
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
    // Native counters keep a carried counter's progress non-negative unless the threshold is infinite. The progress is
    // computed as ExecutionCounter::count() computes it.
    if (header.counterMode != static_cast<uint8_t>(CBFormat::CounterMode::Carried))
        return true;
    if (header.counterActiveThreshold == std::numeric_limits<int32_t>::max())
        return true;
    return static_cast<double>(header.counterTotalCount) + header.counterValue >= 0;
}

ASCIILiteral description(CBCheck check)
{
    switch (check) {
    case CBCheck::StateHeader:
        return "cb.state header: layout version, tier, counter mode or reserved field"_s;
    case CBCheck::ArgumentCount:
        return "cb.state argument count differs from the CodeBlock's"_s;
    case CBCheck::ValueProfileCount:
        return "cb.state value-profile count differs from the UnlinkedCodeBlock's"_s;
    case CBCheck::FamilyCount:
        return "cb.state metadata entry counts differ from the CodeBlock's"_s;
    case CBCheck::StateLength:
        return "cb.state length or padding"_s;
    case CBCheck::ValuePrediction:
        return "cb.state argument or value prediction outside SpecBytecodeTop"_s;
    case CBCheck::ArrayProfile:
        return "cb.state array profile modes or flags"_s;
    case CBCheck::AllocationHint:
        return "cb.state allocation hint"_s;
    case CBCheck::IterationModes:
        return "cb.state iteration modes outside their opcode's native bits"_s;
    case CBCheck::EnumeratorModes:
        return "cb.state enumerator modes"_s;
    case CBCheck::ToThisStatus:
        return "cb.state to_this status"_s;
    case CBCheck::BranchBit:
        return "cb.state jneq_ptr branch bit"_s;
    case CBCheck::LazyOperand:
        return "cb.state lazy-operand profile"_s;
    case CBCheck::TierUpHistory:
        return "cb.state profile deferrals or reoptimization count above its option"_s;
    case CBCheck::Counter:
        return "cb.state baseline counter fields"_s;
    case CBCheck::SummaryHeader:
        return "cb.summary header: layout version, tier or counter mode"_s;
    case CBCheck::SummaryLength:
        return "cb.summary length or padding"_s;
    case CBCheck::SummaryValue:
        return "cb.summary category, array flag or counter progress"_s;
    case CBCheck::StrictNewborn:
        return "install: the CodeBlock is not newborn"_s;
    case CBCheck::StrictLinkState:
        return "install: the CodeBlock is not at its link state"_s;
    case CBCheck::StrictCounter:
        return "cb.state counter has negative progress under a finite threshold"_s;
    case CBCheck::CapturePairing:
        return "capture: the UnlinkedCodeBlock's profile copies do not pair with the CodeBlock's"_s;
    case CBCheck::CaptureCharge:
        return "capture: the producer budget refused the section buffers"_s;
    case CBCheck::CaptureStrict:
        return "capture: a strict check of the CodeBlock or of the built sections failed"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
