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
#include "ICCapture.h"

#if ENABLE(JIT)

#include "AccessCase.h"
#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "ICSites.h"
#include "JITCode.h"
#include "PolymorphicCallStubRoutine.h"
#include "PropertyInlineCache.h"
#include "Repatch.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <wtf/Assertions.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache::ICs {

namespace ICCaptureInternal {

// The opcode at each canonical call-link position (section 4.2), which a group word names.
static constexpr std::array<OpcodeID, numberOfCallLinkOpcodes> callLinkOpcodes {
#define JITCACHE_ICS_CAPTURE_CALL_LINK_OPCODE_ID(opcodeStruct) opcodeStruct::opcodeID,
    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_CAPTURE_CALL_LINK_OPCODE_ID)
#undef JITCACHE_ICS_CAPTURE_CALL_LINK_OPCODE_ID
};

static uint8_t bitIf(bool condition, uint8_t bit)
{
    return condition ? bit : static_cast<uint8_t>(0);
}

static CallLinkModeCode modeCode(CallLinkInfo::Mode mode)
{
    switch (mode) {
    case CallLinkInfo::Mode::Init:
        return CallLinkModeCode::Init;
    case CallLinkInfo::Mode::Monomorphic:
        return CallLinkModeCode::Monomorphic;
    case CallLinkInfo::Mode::Polymorphic:
        return CallLinkModeCode::Polymorphic;
    case CallLinkInfo::Mode::Virtual:
        return CallLinkModeCode::Virtual;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The call-link half of the polymorphic bit (section 5.3): a Polymorphic site that GC has not cleared and whose stub
// holds two or more slots. A stub with one slot holds closures of one executable, which a despecified status serves, so
// the slots are counted only up to two. Counting reads the slots' cell pointers and dereferences none; the stub stays
// alive under the CB's lock, since only a collection frees it (L4).
static bool isPolymorphicCallLinkSite(CallLinkInfo& callLinkInfo)
{
    if (callLinkInfo.mode() != CallLinkInfo::Mode::Polymorphic || callLinkInfo.clearedByGC())
        return false;
    PolymorphicCallStubRoutine* stub = callLinkInfo.stub();
    if (!stub)
        return false;
    unsigned slotCount = 0;
    stub->forEachDependentCell([&](JSCell*) {
        slotCount = std::min(slotCount + 1, 2u);
    });
    return slotCount >= 2;
}

static uint32_t propertyICCount(BaselineJITData* jitData)
{
    // BaselineJITData::create sizes the IC array with an unsigned count.
    return jitData ? static_cast<uint32_t>(jitData->propertyInlineCaches().size()) : 0;
}

// The counts of the section a CB captures now: one group per call-link opcode with at least one metadata entry, in
// canonical order (section 4.2), and one record per IC and per call-link site.
struct SectionShape {
    CallLinkSiteCounts siteCounts { };
    uint32_t propertyICCount { 0 };
    uint32_t callLinkGroupCount { 0 };
    uint32_t callLinkSiteCount { 0 };
    size_t size { 0 };
};

// A CB without BaselineJITData counts no IC, so that a strict capture of it can still report NoBaselineJITData.
static SectionShape sectionShape(CodeBlock& codeBlock, BaselineJITData* jitData)
{
    SectionShape shape;
    shape.siteCounts = callLinkSiteCounts(codeBlock);
    Checked<uint32_t> callLinkSiteCount = 0;
    for (uint32_t siteCount : shape.siteCounts) {
        if (!siteCount)
            continue;
        ++shape.callLinkGroupCount;
        callLinkSiteCount += siteCount;
    }
    shape.callLinkSiteCount = callLinkSiteCount.value();
    shape.propertyICCount = propertyICCount(jitData);
    // At most numberOfCallLinkOpcodes groups and 32-bit counts, so the size cannot overflow on the 64-bit targets.
    auto size = sectionSize(shape.callLinkGroupCount, shape.propertyICCount, shape.callLinkSiteCount);
    RELEASE_ASSERT(size);
    shape.size = *size;
    return shape;
}

// C1 (section 5.5): CodeBlock::setupWithUnlinkedBaselineCode builds IC i from mold i of the BaselineJITCode it
// installs, so record i stands for IC i and restore pairs it with mold i (A7). A published BaselineJITData implies that
// installed code is the CB's JIT code. An IC without a mold at its index fails as well.
static std::optional<CaptureError> checkMolds(CodeBlock& codeBlock, BaselineJITData& jitData)
{
    RefPtr jitCode = codeBlock.jitCode();
    ASSERT(jitCode && jitCode->jitType() == JITType::BaselineJIT);
    const auto& molds = static_cast<BaselineJITCode&>(*jitCode).m_unlinkedPropertyInlineCaches;
    for (uint32_t index = 0, count = propertyICCount(&jitData); index < count; ++index) {
        if (index >= molds.size() || molds[index].accessType != jitData.propertyCache(index).accessType)
            return CaptureError { CaptureCheck::MoldMismatch, index };
    }
    return std::nullopt;
}

// Reads the record of every IC, in IC order, then of every call-link site, in canonical order, under the CB's lock the
// locker witnesses; hands each record to its sink; and adds up the summary and the polymorphic bit (section 5.3).
// summarizeBaselineICs and captureBaselineICs both read through it, so they agree whenever no JS and no stopped-world
// GC phase runs between them.
template<typename PropertyICSink, typename CallLinkSink>
static CaptureSummary readRecords(const ConcurrentJSLocker& locker, CodeBlock& codeBlock, BaselineJITData* jitData, const PropertyICSink& propertyICSink, const CallLinkSink& callLinkSink)
{
    CaptureSummary result;
    for (uint32_t index = 0, count = propertyICCount(jitData); index < count; ++index) {
        PropertyICRecord record = readPropertyICRecord(locker, jitData->propertyCache(index));
        if (record.caseCount)
            ++result.summary.icSitesWithCases;
        if (isPolymorphicPropertyIC(record))
            result.hasPolymorphicSite = true;
        propertyICSink(record);
    }
    forEachCallLinkSite(codeBlock, [&](unsigned, unsigned, CallLinkInfo& callLinkInfo) {
        callLinkSink(readCallLinkRecord(locker, callLinkInfo));
        if (!result.hasPolymorphicSite && isPolymorphicCallLinkSite(callLinkInfo))
            result.hasPolymorphicSite = true;
    });
    return result;
}

} // namespace ICCaptureInternal

PropertyICRecord readPropertyICRecord(const ConcurrentJSLocker& locker, const HandlerPropertyInlineCache& propertyCache)
{
    using ICCaptureInternal::bitIf;

    // The fixed maxAccessVariantListSize keeps the list at eight cases at most, within the vector's inline capacity, so
    // listing allocates nothing (section 5.1). After a fold the megamorphic case is the only one listed.
    auto cases = propertyCache.listedAccessCases(locker);
    bool megamorphicCaseListed = false;
    for (AccessCase* accessCase : cases) {
        if (AccessCase::isMegamorphic(accessCase->type())) {
            megamorphicCaseListed = true;
            break;
        }
    }
    ASSERT(cases.size() <= std::numeric_limits<uint8_t>::max());

    // Of the other values m_slowOperation takes, *Optimize needs no record and a *Megamorphic operation implies a listed
    // megamorphic case (section 5.2).
    bool holdsGaveUp = propertyCache.m_slowOperation == gaveUpOperationFor(propertyCache.accessType);

    return PropertyICRecord {
        .accessType = static_cast<uint8_t>(propertyCache.accessType),
        .learningBits = static_cast<uint8_t>(bitIf(propertyCache.everConsidered, LearningBit::everConsidered)
            | bitIf(propertyCache.sawNonCell, LearningBit::sawNonCell)
            | bitIf(propertyCache.tookSlowPath, LearningBit::tookSlowPath)
            | bitIf(propertyCache.resetByGC, LearningBit::resetByGC)),
        .repatchCount = propertyCache.repatchCount,
        .numberOfCoolDowns = propertyCache.numberOfCoolDowns,
        .caseCount = static_cast<uint8_t>(std::min<size_t>(cases.size(), std::numeric_limits<uint8_t>::max())),
        .stateBits = static_cast<uint8_t>(bitIf(megamorphicCaseListed, StateBit::megamorphicCaseListed)
            | bitIf(propertyCache.canBeMegamorphic, StateBit::canBeMegamorphic)
            | bitIf(holdsGaveUp, StateBit::holdsGaveUp)),
        .reserved = { 0, 0 },
    };
}

CallLinkRecord readCallLinkRecord(const ConcurrentJSLocker&, CallLinkInfo& callLinkInfo)
{
    using ICCaptureInternal::bitIf;

    return CallLinkRecord {
        .bits = static_cast<uint8_t>(static_cast<uint8_t>(ICCaptureInternal::modeCode(callLinkInfo.mode()))
            | bitIf(callLinkInfo.seenOnce(), CallLinkBit::seenOnce)
            | bitIf(callLinkInfo.hasSeenClosure(), CallLinkBit::hasSeenClosure)
            | bitIf(callLinkInfo.clearedByGC(), CallLinkBit::clearedByGC)
            | bitIf(callLinkInfo.clearedByVirtual(), CallLinkBit::clearedByVirtual)),
        // The field is a byte, saturated at maxProfiledArgumentCountIncludingThisForVarargs.
        .maxArgumentCountIncludingThisForVarargs = static_cast<uint8_t>(callLinkInfo.maxArgumentCountIncludingThisForVarargs()),
    };
}

size_t baselineICsSectionSize(CodeBlock& codeBlock)
{
    ASSERT(codeBlock.jitType() == JITType::BaselineJIT);
    BaselineJITData* jitData = codeBlock.baselineJITData();
    ASSERT(jitData);
    return ICCaptureInternal::sectionShape(codeBlock, jitData).size;
}

CaptureSummary summarizeBaselineICs(CodeBlock& codeBlock)
{
    ASSERT(codeBlock.jitType() == JITType::BaselineJIT);
    BaselineJITData* jitData = codeBlock.baselineJITData();
    ASSERT(jitData);
    ConcurrentJSLocker locker(codeBlock.m_lock);
    ASSERT(!jitData || !ICCaptureInternal::checkMolds(codeBlock, *jitData));
    return ICCaptureInternal::readRecords(locker, codeBlock, jitData, [](const PropertyICRecord&) { }, [](const CallLinkRecord&) { });
}

std::expected<CaptureSummary, CaptureError> captureBaselineICs(CodeBlock& codeBlock, std::span<uint8_t> output, StrictChecks strict)
{
    // The integrator's call is checked first, because a record written past either bound would corrupt memory. Normal
    // mode trusts it and debug builds assert it (section 5.5).
    BaselineJITData* jitData = codeBlock.baselineJITData();
    if (strict == StrictChecks::Yes && !jitData)
        return makeUnexpected(CaptureError { CaptureCheck::NoBaselineJITData, 0 });
    ASSERT(jitData);
    ASSERT(codeBlock.jitType() == JITType::BaselineJIT);

    ICCaptureInternal::SectionShape shape = ICCaptureInternal::sectionShape(codeBlock, jitData);
    if (strict == StrictChecks::Yes && output.size() != shape.size)
        return makeUnexpected(CaptureError { CaptureCheck::OutputSizeMismatch, 0 });
    ASSERT(output.size() == shape.size);

    ConcurrentJSLocker locker(codeBlock.m_lock);

    if (strict == StrictChecks::Yes) {
        if (auto error = ICCaptureInternal::checkMolds(codeBlock, *jitData))
            return makeUnexpected(*error);
    } else
        ASSERT(!jitData || !ICCaptureInternal::checkMolds(codeBlock, *jitData));

    // The header comes first in the section but last in the writing, once the property ICs have given its summary
    // count. Every other byte is written in order, the padding included.
    std::span<uint8_t> cursor = output;
    std::span<uint8_t> headerBytes = consumeSpan(cursor, sectionHeaderSize);
    for (unsigned position = 0; position < numberOfCallLinkOpcodes; ++position) {
        uint32_t siteCount = shape.siteCounts[position];
        if (!siteCount)
            continue;
        CallLinkGroup group {
            .opcodeID = static_cast<uint32_t>(ICCaptureInternal::callLinkOpcodes[position]),
            .siteCount = siteCount,
        };
        memcpySpan(consumeSpan(cursor, callLinkGroupSize), asByteSpan(group));
    }
    std::span<uint8_t> propertyICBytes = consumeSpan(cursor, sizeof(PropertyICRecord) * shape.propertyICCount);
    std::span<uint8_t> callLinkBytes = consumeSpan(cursor, sizeof(CallLinkRecord) * shape.callLinkSiteCount);
    zeroSpan(cursor);

    auto writePropertyIC = [&](const PropertyICRecord& record) {
        memcpySpan(consumeSpan(propertyICBytes, sizeof(record)), asByteSpan(record));
    };
    auto writeCallLink = [&](const CallLinkRecord& record) {
        memcpySpan(consumeSpan(callLinkBytes, sizeof(record)), asByteSpan(record));
    };
    CaptureSummary result = ICCaptureInternal::readRecords(locker, codeBlock, jitData, writePropertyIC, writeCallLink);
    // Nothing runs between the counts and the walk, so the walk visits exactly the counted sites.
    ASSERT(propertyICBytes.empty() && callLinkBytes.empty());

    SectionHeader header {
        .propertyICCount = shape.propertyICCount,
        .icSitesWithCases = result.summary.icSitesWithCases,
        .callLinkGroupCount = shape.callLinkGroupCount,
        .callLinkSiteCount = shape.callLinkSiteCount,
    };
    memcpySpan(headerBytes, asByteSpan(header));

    // What capture writes always passes the structural checks a strict import runs on it (A1 to A6).
    ASSERT(parseSection(output, StrictChecks::Yes).has_value());
    return result;
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
