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

#include "BytecodeIndex.h"
#include "CodeBlock.h"
#include "JITCacheCBFormat.h"
#include "Operands.h"
#include "Options.h"
#include "VirtualRegister.h"
#include <cmath>
#include <initializer_list>
#include <limits>
#include <wtf/UnalignedAccess.h>

// The structural rules of SPEC-cb.md section 3.4. They run only with strict on, so that SC1 refuses to commit what a
// strict consumer's prepare would reject, and they read the section with byte-wise loads: a strict consumer must reject
// malformed bytes rather than trust that they arrived aligned.

namespace JSC::JITCache {

namespace CBValidateInternal {

template<typename T>
static T load(std::span<const uint8_t> section, size_t offset)
{
    return WTF::unalignedLoad<T>(section.subspan(offset, sizeof(T)).data());
}

template<typename Layout>
static bool paddingIsZero(std::span<const uint8_t> section, const Layout& layout)
{
    for (size_t offset = layout.paddingOffset; offset < layout.size; ++offset) {
        if (section[offset])
            return false;
    }
    return true;
}

// V13: what the constructors of LazyOperandValueProfileKey and Operand assert, plus no constant register, and an offset
// inside the instruction stream, which also rejects the hash table's empty and deleted index values. The operand is not
// tied to this CB's frame: an inlining compile's machine frame can name any slot (N7).
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

static_assert(static_cast<uint32_t>(lastOperandKind) == static_cast<uint32_t>(OperandKind::Tmp), "V13 accepts exactly Argument, Local and Tmp");

} // namespace CBValidateInternal

std::optional<CBCheck> validateState(std::span<const uint8_t> stateSection, CodeBlock& codeBlock)
{
    using CBFormat::StateArray;
    using CBValidateInternal::load;

    // V1.
    if (stateSection.size() < sizeof(CBFormat::StateHeader))
        return CBCheck::StateHeader;
    auto header = load<CBFormat::StateHeader>(stateSection, 0);
    if (header.layoutVersion != CBFormat::stateLayoutVersion
        || header.tier != static_cast<uint8_t>(CBFormat::Tier::Baseline)
        || !CBFormat::counterModeFits(header.counterMode)
        || header.reserved1)
        return CBCheck::StateHeader;

    // V2.
    if (header.numArguments != codeBlock.numParameters() || header.numArguments != codeBlock.argumentValueProfiles().size())
        return CBCheck::ArgumentCount;

    // V3.
    if (header.numValueProfiles != codeBlock.unlinkedCodeBlock()->metadata().numValueProfiles())
        return CBCheck::ValueProfileCount;

    // V4.
    if (header.familyEntryCount != CBFormat::familyEntryCounts(codeBlock))
        return CBCheck::FamilyCount;

    // V5.
    auto layout = CBFormat::stateLayout(header);
    if (!layout || stateSection.size() != layout->size || !CBValidateInternal::paddingIsZero(stateSection, *layout))
        return CBCheck::StateLength;

    // V6.
    for (auto array : { StateArray::ArgumentPredictions, StateArray::ValuePredictions }) {
        for (size_t index = 0; index < layout->count(array); ++index) {
            if (!CBFormat::predictionFits(load<uint64_t>(stateSection, layout->elementOffset(array, index))))
                return CBCheck::ValuePrediction;
        }
    }

    // V7.
    for (size_t index = 0; index < layout->count(StateArray::ArrayProfiles); ++index) {
        auto record = load<CBFormat::ArrayProfileRecord>(stateSection, layout->elementOffset(StateArray::ArrayProfiles, index));
        if (!CBFormat::arrayModesFit(record.observedArrayModes) || !CBFormat::arrayProfileFlagsFit(record.flags))
            return CBCheck::ArrayProfile;
    }

    // V8: each record's vector length and type set, then each literal's lower bound through its own instruction (N8).
    size_t hintIndex = 0;
    for (unsigned family = CBFormat::firstAllocationHintFamily; family < CBFormat::firstAllocationHintFamily + CBFormat::numberOfAllocationHintFamilies; ++family) {
        for (uint32_t entry = 0; entry < header.familyEntryCount[family]; ++entry, ++hintIndex) {
            auto hint = load<uint16_t>(stateSection, layout->elementOffset(StateArray::AllocationHints, hintIndex));
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
            auto hint = load<uint16_t>(stateSection, layout->elementOffset(StateArray::AllocationHints, firstLiteralHint + metadataID));
            literalsFit = CBFormat::allocationHintFits(CBFormat::newArrayBufferFamily, CBFormat::allocationHintIndexingType(hint), recommendedIndexingType);
        });
        if (!literalsFit)
            return CBCheck::AllocationHint;
    }

    // V9.
    size_t modesIndex = 0;
    for (unsigned family = CBFormat::firstIterationModesFamily; family < CBFormat::firstIterationModesFamily + CBFormat::numberOfIterationModesFamilies; ++family) {
        for (uint32_t entry = 0; entry < header.familyEntryCount[family]; ++entry, ++modesIndex) {
            if (!CBFormat::iterationModesFit(family, load<uint16_t>(stateSection, layout->elementOffset(StateArray::IterationModes, modesIndex))))
                return CBCheck::IterationModes;
        }
    }

    // V10.
    for (size_t index = 0; index < layout->count(StateArray::EnumeratorModes); ++index) {
        if (!CBFormat::enumeratorModesFit(load<uint8_t>(stateSection, layout->elementOffset(StateArray::EnumeratorModes, index))))
            return CBCheck::EnumeratorModes;
    }

    // V11.
    for (size_t index = 0; index < layout->count(StateArray::ToThisStatuses); ++index) {
        if (!CBFormat::toThisStatusFits(load<uint8_t>(stateSection, layout->elementOffset(StateArray::ToThisStatuses, index))))
            return CBCheck::ToThisStatus;
    }

    // V12.
    for (size_t index = 0; index < layout->count(StateArray::BranchBits); ++index) {
        if (!CBFormat::branchBitFits(load<uint8_t>(stateSection, layout->elementOffset(StateArray::BranchBits, index))))
            return CBCheck::BranchBit;
    }

    // V13. Keys may repeat and come in any order (N7).
    unsigned instructionsSize = codeBlock.instructionsSize();
    for (size_t index = 0; index < layout->count(StateArray::LazyOperandProfiles); ++index) {
        auto record = load<CBFormat::LazyOperandRecord>(stateSection, layout->elementOffset(StateArray::LazyOperandProfiles, index));
        if (!CBValidateInternal::lazyOperandRecordFits(record, instructionsSize))
            return CBCheck::LazyOperand;
    }

    // V14 (N5).
    if (header.optimizationDelayCounter > Options::maximumOptimizationDelay() || header.reoptimizationRetryCounter > Options::reoptimizationRetryCounterMax())
        return CBCheck::TierUpHistory;

    // V15.
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
    using CBValidateInternal::load;

    // V1, with SummaryHeader, which has no reserved field.
    if (summarySection.size() < sizeof(CBFormat::SummaryHeader))
        return CBCheck::SummaryHeader;
    auto header = load<CBFormat::SummaryHeader>(summarySection, 0);
    if (header.layoutVersion != CBFormat::summaryLayoutVersion
        || header.tier != static_cast<uint8_t>(CBFormat::Tier::Baseline)
        || !CBFormat::counterModeFits(header.counterMode))
        return CBCheck::SummaryHeader;

    // V5, with summarySectionSize.
    auto layout = CBFormat::summaryLayout(header);
    if (!layout || summarySection.size() != layout->size || !CBValidateInternal::paddingIsZero(summarySection, *layout))
        return CBCheck::SummaryLength;

    // V6 on each category.
    for (auto array : { SummaryArray::ArgumentCategories, SummaryArray::ValueCategories, SummaryArray::LazyOperandCategories }) {
        for (size_t index = 0; index < layout->count(array); ++index) {
            if (!CBFormat::predictionFits(load<uint64_t>(summarySection, layout->elementOffset(array, index))))
                return CBCheck::SummaryValue;
        }
    }

    // V7's flag bound on each array flag.
    for (size_t index = 0; index < layout->count(SummaryArray::ArrayFlags); ++index) {
        if (!CBFormat::arrayProfileFlagsFit(load<uint8_t>(summarySection, layout->elementOffset(SummaryArray::ArrayFlags, index))))
            return CBCheck::SummaryValue;
    }

    // The progress, which THREAD Maintenance caps at the active threshold and which a counter that does not travel lacks.
    if (header.counterProgress > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return CBCheck::SummaryValue;
    if (header.counterMode == static_cast<uint8_t>(CBFormat::CounterMode::NotCarried) && header.counterProgress)
        return CBCheck::SummaryValue;

    return std::nullopt;
}

bool counterObeysNativeInvariant(const CBFormat::StateHeader& header)
{
    // S3: progress is not negative unless the threshold is infinite (N4). P is computed as ExecutionCounter::count() does.
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
        return "cb.state header: layout version, tier, counter mode or reserved field (V1)"_s;
    case CBCheck::ArgumentCount:
        return "cb.state argument count differs from the CodeBlock's (V2)"_s;
    case CBCheck::ValueProfileCount:
        return "cb.state value-profile count differs from the UnlinkedCodeBlock's (V3)"_s;
    case CBCheck::FamilyCount:
        return "cb.state metadata entry counts differ from the CodeBlock's (V4)"_s;
    case CBCheck::StateLength:
        return "cb.state length or padding (V5)"_s;
    case CBCheck::ValuePrediction:
        return "cb.state argument or value prediction outside SpecBytecodeTop (V6)"_s;
    case CBCheck::ArrayProfile:
        return "cb.state array profile modes or flags (V7)"_s;
    case CBCheck::AllocationHint:
        return "cb.state allocation hint (V8)"_s;
    case CBCheck::IterationModes:
        return "cb.state iteration modes outside their opcode's native bits (V9)"_s;
    case CBCheck::EnumeratorModes:
        return "cb.state enumerator modes (V10)"_s;
    case CBCheck::ToThisStatus:
        return "cb.state to_this status (V11)"_s;
    case CBCheck::BranchBit:
        return "cb.state jneq_ptr branch bit (V12)"_s;
    case CBCheck::LazyOperand:
        return "cb.state lazy-operand profile (V13)"_s;
    case CBCheck::TierUpHistory:
        return "cb.state profile deferrals or reoptimization count above its option (V14)"_s;
    case CBCheck::Counter:
        return "cb.state baseline counter fields (V15)"_s;
    case CBCheck::SummaryHeader:
        return "cb.summary header: layout version, tier or counter mode"_s;
    case CBCheck::SummaryLength:
        return "cb.summary length or padding"_s;
    case CBCheck::SummaryValue:
        return "cb.summary category, array flag or counter progress"_s;
    case CBCheck::StrictNewborn:
        return "install: the CodeBlock is not newborn (S1)"_s;
    case CBCheck::StrictLinkState:
        return "install: the CodeBlock is not at its link state (S2)"_s;
    case CBCheck::StrictCounter:
        return "cb.state counter has negative progress under a finite threshold (S3)"_s;
    case CBCheck::CapturePairing:
        return "capture: the UnlinkedCodeBlock's profile copies do not pair with the CodeBlock's (SC3)"_s;
    case CBCheck::CaptureCharge:
        return "capture: the producer budget refused the section buffers"_s;
    case CBCheck::CaptureStrict:
        return "capture: a strict check of the CodeBlock or of the built sections failed (SC1, SC2)"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
