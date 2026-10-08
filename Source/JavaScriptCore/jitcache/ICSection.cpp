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
#include "ICSection.h"

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "PropertyInlineCache.h"
#include <algorithm>
#include <wtf/Assertions.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache::ICs {

namespace ICSectionInternal {

static constexpr size_t sectionAlignment = 8;

static constexpr std::array<OpcodeID, numberOfCallLinkOpcodes> callLinkOpcodes {
#define JITCACHE_ICS_CALL_LINK_OPCODE_ID(opcodeStruct) opcodeStruct::opcodeID,
    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_CALL_LINK_OPCODE_ID)
#undef JITCACHE_ICS_CALL_LINK_OPCODE_ID
};

static bool hasBit(uint8_t bits, uint8_t bit)
{
    return bits & bit;
}

static uint8_t bitIf(bool condition, uint8_t bit)
{
    return condition ? bit : static_cast<uint8_t>(0);
}

// The caller guarantees at least sectionHeaderSize bytes.
static SectionHeader readHeader(std::span<const uint8_t> bytes)
{
    SectionHeader header { };
    memcpySpan(asMutableByteSpan(header), bytes.first(sectionHeaderSize));
    return header;
}

struct SlicedSection {
    SectionView view;
    std::span<const uint8_t> padding;
};

// Slices by the header's counts. Out-of-range counts fail the span's bounds assertions, so the caller checks A1 first
// or trusts the section.
static SlicedSection slice(std::span<const uint8_t> bytes)
{
    SectionHeader header = readHeader(bytes);
    std::span<const uint8_t> rest = bytes.subspan(sectionHeaderSize);
    auto groupBytes = consumeSpan(rest, callLinkGroupSize * header.callLinkGroupCount);
    auto propertyICBytes = consumeSpan(rest, sizeof(PropertyICRecord) * header.propertyICCount);
    auto callLinkBytes = consumeSpan(rest, sizeof(CallLinkRecord) * header.callLinkSiteCount);
    return SlicedSection {
        .view = SectionView {
            .header = header,
            .groupBytes = groupBytes,
            .propertyICs = spanReinterpretCast<const PropertyICRecord>(propertyICBytes),
            .callLinks = spanReinterpretCast<const CallLinkRecord>(callLinkBytes),
        },
        .padding = rest,
    };
}

// A1 and A2, which read the header alone.
static std::optional<Invalid> checkHeader(std::span<const uint8_t> bytes)
{
    if (bytes.size() < sectionHeaderSize)
        return Invalid { Check::SectionSize, 0 };
    SectionHeader header = readHeader(bytes);
    if (header.callLinkGroupCount > numberOfCallLinkOpcodes)
        return Invalid { Check::SectionSize, 0 };
    auto size = sectionSize(header.callLinkGroupCount, header.propertyICCount, header.callLinkSiteCount);
    if (!size || *size != bytes.size())
        return Invalid { Check::SectionSize, 0 };

    if (header.icSitesWithCases > header.propertyICCount)
        return Invalid { Check::SummaryBound, 0 };

    return std::nullopt;
}

// A3 to A6, on a section that passed A1.
static std::optional<Invalid> checkBody(const SlicedSection& section)
{
    const SectionView& view = section.view;

    // A3. A section has at most numberOfCallLinkOpcodes groups (A1), so the 64-bit sum of their 32-bit counts cannot
    // overflow, and a sum that does not fit 32 bits differs from callLinkSiteCount.
    uint64_t siteCountSum = 0;
    std::optional<unsigned> previousPosition;
    for (unsigned index = 0; index < view.header.callLinkGroupCount; ++index) {
        CallLinkGroup group = view.group(index);
        auto position = canonicalCallLinkPosition(static_cast<OpcodeID>(group.opcodeID));
        if (!position || (previousPosition && *position <= *previousPosition) || !group.siteCount)
            return Invalid { Check::CallLinkGroups, 0 };
        previousPosition = position;
        siteCountSum += group.siteCount;
    }
    if (siteCountSum != view.header.callLinkSiteCount)
        return Invalid { Check::CallLinkGroups, 0 };

    // A4.
    for (size_t index = 0; index < view.propertyICs.size(); ++index) {
        const PropertyICRecord& record = view.propertyICs[index];
        if ((record.learningBits & ~LearningBit::mask) || (record.stateBits & ~StateBit::mask) || record.reserved[0] || record.reserved[1])
            return Invalid { Check::ReservedBits, static_cast<uint32_t>(index) };
    }
    for (size_t index = 0; index < view.callLinks.size(); ++index) {
        if (view.callLinks[index].bits & ~CallLinkBit::mask)
            return Invalid { Check::ReservedBits, static_cast<uint32_t>(index) };
    }
    if (std::ranges::any_of(section.padding, [](uint8_t byte) -> bool { return byte; }))
        return Invalid { Check::ReservedBits, 0 };

    // A5.
    for (size_t index = 0; index < view.propertyICs.size(); ++index) {
        if (static_cast<unsigned>(view.propertyICs[index].accessType) >= numberOfAccessTypes)
            return Invalid { Check::EnumRange, static_cast<uint32_t>(index) };
    }

    // A6.
    size_t sitesWithCases = 0;
    for (const PropertyICRecord& record : view.propertyICs) {
        if (record.caseCount)
            ++sitesWithCases;
    }
    if (sitesWithCases != view.header.icSitesWithCases)
        return Invalid { Check::SummaryCount, 0 };

    return std::nullopt;
}

// A1 to A6, in order, stopping at the first failure.
static std::optional<Invalid> checkSection(std::span<const uint8_t> bytes)
{
    if (auto invalid = checkHeader(bytes))
        return invalid;
    return checkBody(slice(bytes));
}

} // namespace ICSectionInternal

std::optional<unsigned> canonicalCallLinkPosition(OpcodeID opcodeID)
{
    for (unsigned position = 0; position < numberOfCallLinkOpcodes; ++position) {
        if (ICSectionInternal::callLinkOpcodes[position] == opcodeID)
            return position;
    }
    return std::nullopt;
}

bool isVarargsCallLinkOpcode(OpcodeID opcodeID)
{
    switch (opcodeID) {
    case OpCallVarargs::opcodeID:
    case OpTailCallVarargs::opcodeID:
    case OpConstructVarargs::opcodeID:
    case OpSuperConstructVarargs::opcodeID:
        return true;
    default:
        return false;
    }
}

std::optional<size_t> sectionSize(size_t groupCount, size_t propertyICCount, size_t callLinkSiteCount)
{
    if (groupCount > numberOfCallLinkOpcodes)
        return std::nullopt;
    CheckedSize size = sectionHeaderSize;
    size += CheckedSize(callLinkGroupSize) * groupCount;
    size += CheckedSize(sizeof(PropertyICRecord)) * propertyICCount;
    size += CheckedSize(sizeof(CallLinkRecord)) * callLinkSiteCount;
    // Rounding up adds at most sectionAlignment - 1, and that addition overflows exactly when the rounded size does not
    // fit.
    size += ICSectionInternal::sectionAlignment - 1;
    if (size.hasOverflowed())
        return std::nullopt;
    return size.value() & ~(ICSectionInternal::sectionAlignment - 1);
}

CallLinkGroup SectionView::group(unsigned index) const
{
    CallLinkGroup result { };
    memcpySpan(asMutableByteSpan(result), groupBytes.subspan(callLinkGroupSize * index, callLinkGroupSize));
    return result;
}

std::expected<SectionView, Invalid> parseSection(std::span<const uint8_t> bytes, StrictChecks strict)
{
    if (strict == StrictChecks::Yes) {
        if (auto invalid = ICSectionInternal::checkSection(bytes))
            return makeUnexpected(*invalid);
    } else {
        // Normal mode trusts the section, whose integrity the integrator checked; debug builds still assert its
        // structure (section 4.5).
        ASSERT(!ICSectionInternal::checkSection(bytes));
    }
    return ICSectionInternal::slice(bytes).view;
}

std::expected<Summary, Invalid> readBaselineICsSummary(std::span<const uint8_t> section, StrictChecks strict)
{
    if (strict == StrictChecks::Yes) {
        if (auto invalid = ICSectionInternal::checkHeader(section))
            return makeUnexpected(*invalid);
    } else
        ASSERT(!ICSectionInternal::checkHeader(section));
    return Summary { ICSectionInternal::readHeader(section).icSitesWithCases };
}

bool isPolymorphicPropertyIC(const PropertyICRecord& record)
{
    return record.caseCount >= 2 && !ICSectionInternal::hasBit(record.stateBits, StateBit::megamorphicCaseListed);
}

RestoredPropertyIC restoredPropertyIC(const PropertyICRecord& record)
{
    using ICSectionInternal::hasBit;
    bool folded = hasBit(record.stateBits, StateBit::megamorphicCaseListed) || hasBit(record.stateBits, StateBit::canBeMegamorphic);
    // Natively a give-up outlives only the cases beside it, and cases never travel, so only a give-up that lists no
    // case survives the import.
    bool givenUp = hasBit(record.stateBits, StateBit::holdsGaveUp) && !record.caseCount;
    return RestoredPropertyIC {
        .everConsidered = hasBit(record.learningBits, LearningBit::everConsidered),
        .sawNonCell = hasBit(record.learningBits, LearningBit::sawNonCell),
        .tookSlowPath = hasBit(record.learningBits, LearningBit::tookSlowPath) || givenUp,
        .resetByGC = hasBit(record.learningBits, LearningBit::resetByGC),
        .foldsAtFirstCase = folded && !givenUp,
        .givenUp = givenUp,
        .countdown = 0,
        // The consumer counts one repatch for each case it re-caches, so the captured cases are not counted twice
        // toward a cool-down.
        .repatchCount = static_cast<uint8_t>(record.repatchCount - std::min(record.repatchCount, record.caseCount)),
        .numberOfCoolDowns = record.numberOfCoolDowns,
    };
}

RestoredCallLink restoredCallLink(const CallLinkRecord& record)
{
    using ICSectionInternal::hasBit;
    bool isVirtual = static_cast<CallLinkModeCode>(record.bits & CallLinkBit::modeMask) == CallLinkModeCode::Virtual;
    return RestoredCallLink {
        .isVirtual = isVirtual,
        .seenOnce = !isVirtual,
        .hasSeenClosure = hasBit(record.bits, CallLinkBit::hasSeenClosure),
        .clearedByGC = hasBit(record.bits, CallLinkBit::clearedByGC),
        .clearedByVirtual = hasBit(record.bits, CallLinkBit::clearedByVirtual) || isVirtual,
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
    };
}

PropertyICRecord recapturedPropertyIC(const PropertyICRecord& record)
{
    using ICSectionInternal::bitIf;
    RestoredPropertyIC restored = restoredPropertyIC(record);
    return PropertyICRecord {
        .accessType = record.accessType,
        .learningBits = static_cast<uint8_t>(bitIf(restored.everConsidered, LearningBit::everConsidered)
            | bitIf(restored.sawNonCell, LearningBit::sawNonCell)
            | bitIf(restored.tookSlowPath, LearningBit::tookSlowPath)
            | bitIf(restored.resetByGC, LearningBit::resetByGC)),
        .repatchCount = restored.repatchCount,
        .numberOfCoolDowns = record.numberOfCoolDowns,
        .caseCount = 0,
        .stateBits = static_cast<uint8_t>(bitIf(restored.givenUp, StateBit::holdsGaveUp)
            | bitIf(restored.foldsAtFirstCase, StateBit::canBeMegamorphic)),
        .reserved = { 0, 0 },
    };
}

CallLinkRecord recapturedCallLink(const CallLinkRecord& record)
{
    using ICSectionInternal::bitIf;
    RestoredCallLink restored = restoredCallLink(record);
    CallLinkModeCode mode = restored.isVirtual ? CallLinkModeCode::Virtual : CallLinkModeCode::Init;
    return CallLinkRecord {
        .bits = static_cast<uint8_t>(static_cast<uint8_t>(mode)
            | bitIf(restored.seenOnce, CallLinkBit::seenOnce)
            | bitIf(restored.hasSeenClosure, CallLinkBit::hasSeenClosure)
            | bitIf(restored.clearedByGC, CallLinkBit::clearedByGC)
            | bitIf(restored.clearedByVirtual, CallLinkBit::clearedByVirtual)),
        .maxArgumentCountIncludingThisForVarargs = restored.maxArgumentCountIncludingThisForVarargs,
    };
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
