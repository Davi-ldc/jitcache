#include "config.h"
#include "ICSection.h"

#if ENABLE(JIT)

#include "ArrayProfile.h"
#include "BytecodeStructs.h"
#include "PropertyInlineCache.h"
#include <algorithm>
#include <wtf/Assertions.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>

// Parsing of the ICsBaseline section and its structural checks, and the two derivations from a record: the consumer
// state it restores, and the record a capture of that state writes.

namespace JSC::JITCache::ICs {

namespace ICSectionInternal {

static constexpr size_t sectionAlignment = 8;

// The eight ArrayProfileFlag bits, the pruning mark included: the flags any array profile can hold.
static constexpr uint32_t allArrayProfileFlags = static_cast<uint32_t>(ArrayProfileFlag::MayStoreHole)
    | static_cast<uint32_t>(ArrayProfileFlag::OutOfBounds)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeLargeTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::MayInterceptIndexedAccesses)
    | static_cast<uint32_t>(ArrayProfileFlag::UsesNonOriginalArrayStructures)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray)
    | static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning)
    | static_cast<uint32_t>(ArrayProfileFlag::MayBeRegExpMatchesArray);
static_assert(allArrayProfileFlags == 0xff);

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

// Slices by the header's counts. Out-of-range counts fail the span's bounds assertions, so the caller checks the section
// size first or trusts the section.
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

// SectionSize and SummaryBound, which read the header alone.
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

// A call-link record sets no unused bit, holds this words only at an opcode with a this profile, and there only modes
// and flags an array profile can hold.
static bool hasOnlyKnownBits(const CallLinkOpcode& opcode, const CallLinkRecord& record)
{
    if (record.bits & ~CallLinkBit::mask)
        return false;
    uint32_t modes = readThisArrayModes(record);
    uint32_t flags = readThisArrayProfileFlags(record);
    if (!opcode.hasThisProfile)
        return !modes && !flags;
    return !(modes & ~static_cast<uint32_t>(ALL_ARRAY_MODES)) && !(flags & ~allArrayProfileFlags);
}

// The checks from CallLinkGroups to SummaryCount, on a section whose size checkHeader accepted.
static std::optional<Invalid> checkBody(const SlicedSection& section)
{
    const SectionView& view = section.view;

    // A section has at most numberOfCallLinkOpcodes groups, so the 64-bit sum of their 32-bit counts cannot overflow,
    // and a sum that does not fit 32 bits differs from callLinkSiteCount.
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

    for (size_t index = 0; index < view.propertyICs.size(); ++index) {
        const PropertyICRecord& record = view.propertyICs[index];
        if ((record.learningBits & ~LearningBit::mask) || (record.stateBits & ~StateBit::mask) || record.reserved[0] || record.reserved[1])
            return Invalid { Check::ReservedBits, static_cast<uint32_t>(index) };
    }
    // The groups passed CallLinkGroups, so they give each record its opcode, and their counts add up to the records.
    size_t recordIndex = 0;
    for (unsigned groupIndex = 0; groupIndex < view.header.callLinkGroupCount; ++groupIndex) {
        CallLinkGroup group = view.group(groupIndex);
        const CallLinkOpcode& opcode = callLinkOpcodes[*canonicalCallLinkPosition(static_cast<OpcodeID>(group.opcodeID))];
        for (uint32_t siteInGroup = 0; siteInGroup < group.siteCount; ++siteInGroup, ++recordIndex) {
            if (!hasOnlyKnownBits(opcode, view.callLinks[recordIndex]))
                return Invalid { Check::ReservedBits, static_cast<uint32_t>(recordIndex) };
        }
    }
    if (std::ranges::any_of(section.padding, [](uint8_t byte) -> bool { return byte; }))
        return Invalid { Check::ReservedBits, 0 };

    for (size_t index = 0; index < view.propertyICs.size(); ++index) {
        if (static_cast<unsigned>(view.propertyICs[index].accessType) >= numberOfAccessTypes)
            return Invalid { Check::EnumRange, static_cast<uint32_t>(index) };
    }

    size_t sitesWithCases = 0;
    for (const PropertyICRecord& record : view.propertyICs) {
        if (record.caseCount)
            ++sitesWithCases;
    }
    if (sitesWithCases != view.header.icSitesWithCases)
        return Invalid { Check::SummaryCount, 0 };

    return std::nullopt;
}

// The checks that read only the section, in order, stopping at the first failure.
static std::optional<Invalid> checkSection(std::span<const uint8_t> bytes)
{
    if (auto invalid = checkHeader(bytes))
        return invalid;
    return checkBody(slice(bytes));
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

ASCIILiteral description(Check check)
{
    switch (check) {
    case Check::SectionSize:
        return "section-size"_s;
    case Check::SummaryBound:
        return "summary-bound"_s;
    case Check::CallLinkGroups:
        return "call-link-groups"_s;
    case Check::ReservedBits:
        return "reserved-bits"_s;
    case Check::EnumRange:
        return "enum-range"_s;
    case Check::SummaryCount:
        return "summary-count"_s;
    case Check::MoldPairing:
        return "mold-pairing"_s;
    case Check::MetadataLayout:
        return "metadata-layout"_s;
    case Check::MoldMegamorphicBit:
        return "mold-megamorphic-bit"_s;
    case Check::NewbornCallLinks:
        return "newborn-call-links"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
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
        // structure.
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
