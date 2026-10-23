#include "config.h"
#include "jitcache/ics/ICSection.h"

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include <algorithm>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

// Parsing of the ICsBaseline section, and the two derivations from a record: the consumer state it restores, and the
// record a capture of that state writes.

namespace JSC::JITCache::ICs {

namespace ICSectionInternal {

static constexpr size_t sectionAlignment = 8;

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

} // namespace ICSectionInternal

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

SectionView parseSection(std::span<const uint8_t> bytes)
{
    // The section is trusted, since the integrator checked its integrity. Counts out of range fail the span's bounds
    // assertions.
    SectionHeader header = ICSectionInternal::readHeader(bytes);
    std::span<const uint8_t> rest = bytes.subspan(sectionHeaderSize);
    auto groupBytes = consumeSpan(rest, callLinkGroupSize * header.callLinkGroupCount);
    auto propertyICBytes = consumeSpan(rest, sizeof(PropertyICRecord) * header.propertyICCount);
    auto callLinkBytes = consumeSpan(rest, sizeof(CallLinkRecord) * header.callLinkSiteCount);
    return SectionView {
        .header = header,
        .groupBytes = groupBytes,
        .propertyICs = spanReinterpretCast<const PropertyICRecord>(propertyICBytes),
        .callLinks = spanReinterpretCast<const CallLinkRecord>(callLinkBytes),
    };
}

Summary readBaselineICsSummary(std::span<const uint8_t> section)
{
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

RestoredCallLink restoredCallLink(const CallLinkOpcode& opcode, const CallLinkRecord& record)
{
    using ICSectionInternal::hasBit;
    auto mode = static_cast<CallLinkModeCode>(record.bits & CallLinkBit::modeMask);
    bool isVirtual = mode == CallLinkModeCode::Virtual;
    // Only the opcodes with a this profile carry one; elsewhere the fields hold nothing.
    uint32_t thisArrayModes = opcode.hasThisProfile ? readThisArrayModes(record) : 0;
    uint32_t thisArrayProfileFlags = opcode.hasThisProfile ? readThisArrayProfileFlags(record) : 0;
    // What only a CallSiteData or CallLinkInfo of the site's own can hold. A this profile with observations has no other
    // place in the consumer: the shared data holds no site's profile, and a builtin's UCB keeps no copy to merge from.
    bool holding = isVirtual
        || hasBit(record.bits, CallLinkBit::hasSeenClosure)
        || hasBit(record.bits, CallLinkBit::clearedByGC)
        || hasBit(record.bits, CallLinkBit::clearedByVirtual)
        || record.maxArgumentCountIncludingThisForVarargs
        || thisArrayModes
        || thisArrayProfileFlags;
    // The site never ran, or a reset left it as linking did, or it is a direct eval whose callee was not eval: either way
    // the producer's next call there would not link.
    bool empty = !holding && mode == CallLinkModeCode::Init && !hasBit(record.bits, CallLinkBit::seenOnce);

    // The site comes back as the producer's last execution left it, ready to link at its next call, with data of its own
    // only when it has something to hold. A tail call that ran is the exception: no shared data holds a tail call's seen
    // bit, and natively it gets data of its own before its first call.
    CallSiteDataState data = CallSiteDataState::ExecutedOnce;
    if (opcode.kind == CallLinkSiteKind::Inline)
        data = CallSiteDataState::Inline;
    else if (empty)
        data = CallSiteDataState::NotExecuted;
    else if (holding || opcode.kind == CallLinkSiteKind::LazyTailCall)
        data = CallSiteDataState::Own;

    return RestoredCallLink {
        .data = data,
        .isVirtual = isVirtual,
        .seenOnce = !empty && !isVirtual,
        .hasSeenClosure = hasBit(record.bits, CallLinkBit::hasSeenClosure),
        .clearedByGC = hasBit(record.bits, CallLinkBit::clearedByGC),
        .clearedByVirtual = hasBit(record.bits, CallLinkBit::clearedByVirtual) || isVirtual,
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
        .thisArrayModes = thisArrayModes,
        .thisArrayProfileFlags = thisArrayProfileFlags,
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

CallLinkRecord recapturedCallLink(const CallLinkOpcode& opcode, const CallLinkRecord& record)
{
    using ICSectionInternal::bitIf;
    RestoredCallLink restored = restoredCallLink(opcode, record);
    CallLinkRecord result { };
    switch (restored.data) {
    case CallSiteDataState::NotExecuted:
        // The site points at what linking gave it, of which a capture reads nothing.
        return result;
    case CallSiteDataState::ExecutedOnce:
        // The site points at the executed-once data, of which a capture reads only the seen bit.
        result.bits = CallLinkBit::seenOnce;
        return result;
    case CallSiteDataState::Inline:
    case CallSiteDataState::Own:
        break;
    }
    CallLinkModeCode mode = restored.isVirtual ? CallLinkModeCode::Virtual : CallLinkModeCode::Init;
    result.bits = static_cast<uint8_t>(static_cast<uint8_t>(mode)
        | bitIf(restored.seenOnce, CallLinkBit::seenOnce)
        | bitIf(restored.hasSeenClosure, CallLinkBit::hasSeenClosure)
        | bitIf(restored.clearedByGC, CallLinkBit::clearedByGC)
        | bitIf(restored.clearedByVirtual, CallLinkBit::clearedByVirtual));
    result.maxArgumentCountIncludingThisForVarargs = restored.maxArgumentCountIncludingThisForVarargs;
    writeThisProfile(result, restored.thisArrayModes, restored.thisArrayProfileFlags);
    return result;
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
