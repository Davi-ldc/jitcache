#include "config.h"
#include "ICRestore.h"

#if ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "ICSites.h"
#include "JITCode.h"
#include "Repatch.h"
#include <array>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache::ICs {

namespace ICRestoreInternal {

// The opcode at each canonical call-link position (section 4.2), whose CallLinkInfo::callTypeFor linking stored.
static constexpr std::array<OpcodeID, numberOfCallLinkOpcodes> callLinkOpcodes {
#define JITCACHE_ICS_RESTORE_CALL_LINK_OPCODE_ID(opcodeStruct) opcodeStruct::opcodeID,
    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_RESTORE_CALL_LINK_OPCODE_ID)
#undef JITCACHE_ICS_RESTORE_CALL_LINK_OPCODE_ID
};

// S2 for one site: every field seeding writes over is as DataOnlyCallLinkInfo::initialize left it, and the site holds
// no callee, stub or incoming-call link. useLLIntICs is fixed on, so linking starts every site in Init.
static bool isAsLinkingLeftIt(OpcodeID opcodeID, CallLinkInfo& callLinkInfo)
{
    return callLinkInfo.type() == CallLinkInfo::Type::DataOnly
        && callLinkInfo.callType() == CallLinkInfo::callTypeFor(opcodeID)
        && callLinkInfo.mode() == CallLinkInfo::Mode::Init
        && !callLinkInfo.seenOnce()
        && !callLinkInfo.hasSeenClosure()
        && !callLinkInfo.clearedByGC()
        && !callLinkInfo.clearedByVirtual()
        && !callLinkInfo.maxArgumentCountIncludingThisForVarargs()
        && !callLinkInfo.stub()
        && !callLinkInfo.haveLastSeenCallee()
        && !callLinkInfo.isOnList();
}

} // namespace ICRestoreInternal

std::optional<Invalid> checkMoldPairing(const SectionView& view, std::span<const BaselineUnlinkedPropertyInlineCache> molds)
{
    // CodeBlock::setupWithUnlinkedBaselineCode builds IC i from mold i of the code it installs, and attach writes record
    // i into IC i, so the counts must agree and each record must stand for its mold's access.
    if (view.header.propertyICCount != molds.size() || view.propertyICs.size() != molds.size())
        return Invalid { Check::MoldPairing, 0 };
    for (size_t index = 0; index < molds.size(); ++index) {
        if (view.propertyICs[index].accessType != static_cast<uint8_t>(molds[index].accessType))
            return Invalid { Check::MoldPairing, static_cast<uint32_t>(index) };
    }
    return std::nullopt;
}

std::optional<Invalid> checkMetadataLayout(const SectionView& view, const CallLinkSiteCounts& siteCounts)
{
    // The section's per-opcode counts, from its groups. A3 gives a parsed section one group per opcode at most, each
    // with a canonical position; a view that breaks that rule cannot pair with any CB.
    CallLinkSiteCounts sectionCounts { };
    for (unsigned index = 0; index < view.header.callLinkGroupCount; ++index) {
        CallLinkGroup group = view.group(index);
        auto position = canonicalCallLinkPosition(static_cast<OpcodeID>(group.opcodeID));
        if (!position || sectionCounts[*position])
            return Invalid { Check::MetadataLayout, 0 };
        sectionCounts[*position] = group.siteCount;
    }
    if (sectionCounts != siteCounts)
        return Invalid { Check::MetadataLayout, 0 };

    // Seeding walks the CB's sites beside the records, so the walk must visit exactly as many sites as there are
    // records. In a parsed section A3 already makes this follow from the per-opcode counts; comparing the total keeps
    // the seeding cursor in bounds for any view.
    uint64_t siteTotal = 0;
    for (uint32_t siteCount : siteCounts)
        siteTotal += siteCount;
    if (siteTotal != view.callLinks.size())
        return Invalid { Check::MetadataLayout, 0 };
    return std::nullopt;
}

std::optional<Invalid> checkMolds(std::span<const BaselineUnlinkedPropertyInlineCache> molds)
{
    // Nothing sets a baseline mold's canBeMegamorphic (JIT::addUnlinkedPropertyInlineCache default-constructs each
    // mold), and installation copies it into the IC, so with every mold clear the IC's bit after attach is attach's
    // alone, which the round trip of section 6.1 relies on.
    for (size_t index = 0; index < molds.size(); ++index) {
        if (molds[index].canBeMegamorphic)
            return Invalid { Check::MoldMegamorphicBit, static_cast<uint32_t>(index) };
    }
    return std::nullopt;
}

std::optional<Invalid> checkNewbornCodeBlock(CodeBlock& codeBlock)
{
    // A CB with a JIT type has run, or been set up, and THREAD Restoration gives no imported state to a CB that ran in
    // the LLInt.
    if (codeBlock.jitType() != JITType::None)
        return Invalid { Check::NewbornCallLinks, 0 };

    std::optional<Invalid> result;
    uint32_t siteIndex = 0;
    forEachCallLinkSite(codeBlock, [&](unsigned canonicalPosition, unsigned, CallLinkInfo& callLinkInfo) {
        uint32_t index = siteIndex++;
        if (result)
            return;
        if (!ICRestoreInternal::isAsLinkingLeftIt(ICRestoreInternal::callLinkOpcodes[canonicalPosition], callLinkInfo))
            result = Invalid { Check::NewbornCallLinks, index };
    });
    return result;
}

std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks strict)
{
    // Without strict, parseSection trusts the counts and debug builds assert A1 to A6; seeding and attach assert the A7
    // and A8 counts (section 4.5).
    auto view = parseSection(section, strict);
    if (!view)
        return makeUnexpected(view.error());

    if (strict == StrictChecks::Yes) {
        std::span<const BaselineUnlinkedPropertyInlineCache> molds = preparedCode.m_unlinkedPropertyInlineCaches.span();
        if (auto invalid = checkMoldPairing(*view, molds))
            return makeUnexpected(*invalid);
        if (auto invalid = checkMetadataLayout(*view, callLinkSiteCounts(newbornCodeBlock)))
            return makeUnexpected(*invalid);
        if (auto invalid = checkMolds(molds))
            return makeUnexpected(*invalid);
        if (auto invalid = checkNewbornCodeBlock(newbornCodeBlock))
            return makeUnexpected(*invalid);
    }

    PreparedBaselineICs prepared;
    prepared.m_propertyICs = view->propertyICs;
    prepared.m_callLinks = view->callLinks;
    prepared.m_summary = Summary { view->header.icSitesWithCases };
#if ASSERT_ENABLED
    prepared.m_codeBlock = &newbornCodeBlock;
#endif
    return prepared;
}

void seedCallLinkHistory(const PreparedBaselineICs& prepared, CodeBlock& codeBlock)
{
    ASSERT(prepared.m_codeBlock == &codeBlock);
    ASSERT(codeBlock.jitType() == JITType::None);

    // Native linking writes these fields without a lock, and nothing else reaches the newborn CB, so seeding takes none.
    // R-UCB-1 makes the record count equal the number of sites the walk visits (A8 under strict).
    VM& vm = codeBlock.vm();
    std::span<const CallLinkRecord> records = prepared.callLinks();
    size_t cursor = 0;
    forEachCallLinkSite(codeBlock, [&](unsigned, unsigned, CallLinkInfo& callLinkInfo) {
        ASSERT(cursor < records.size());
        RestoredCallLink restored = restoredCallLink(records[cursor++]);
        // setVirtualCall comes first, because the reset it runs clears the seen bit. It allocates nothing and takes no
        // lock: the virtual-call thunks exist from the VM's construction.
        if (restored.isVirtual)
            callLinkInfo.setVirtualCall(vm);
        else if (restored.seenOnce)
            callLinkInfo.setSeen();
        if (restored.hasSeenClosure)
            callLinkInfo.setHasSeenClosure();
        if (restored.clearedByGC)
            callLinkInfo.setClearedByGC();
        if (restored.clearedByVirtual)
            callLinkInfo.setClearedByVirtual();
        if (restored.maxArgumentCountIncludingThisForVarargs)
            callLinkInfo.updateMaxArgumentCountIncludingThisForVarargs(restored.maxArgumentCountIncludingThisForVarargs);
    });
    ASSERT(cursor == records.size());
}

void attachPropertyICState(const PreparedBaselineICs& prepared, CodeBlock& codeBlock)
{
    ASSERT(prepared.m_codeBlock == &codeBlock);
    ASSERT(codeBlock.jitType() == JITType::BaselineJIT);

    // Compiler threads read IC state under the CB's lock, so attach writes under it too; other threads reach the CB
    // only after installCode stores and barriers it. Attach writes no cell, so it needs no barrier.
    ConcurrentJSLocker locker(codeBlock.m_lock);
    BaselineJITData* jitData = codeBlock.baselineJITData();
    ASSERT(jitData);
    std::span<const PropertyICRecord> records = prepared.propertyICs();
    // Setup builds IC i from mold i, which the Image lane keeps in the producer's order (A7 under strict).
    ASSERT(records.size() == jitData->propertyInlineCaches().size());
    for (size_t index = 0; index < records.size(); ++index) {
        HandlerPropertyInlineCache& propertyCache = jitData->propertyCache(static_cast<unsigned>(index));
        RestoredPropertyIC restored = restoredPropertyIC(records[index]);
        propertyCache.everConsidered = restored.everConsidered;
        propertyCache.sawNonCell = restored.sawNonCell;
        propertyCache.tookSlowPath = restored.tookSlowPath;
        propertyCache.resetByGC = restored.resetByGC;
        // PropertyInlineCache::reset leaves the bit alone, so a restored fold applies to every first case of the IC's
        // life.
        propertyCache.canBeMegamorphic = propertyCache.canBeMegamorphic || restored.foldsAtFirstCase;
        propertyCache.countdown = restored.countdown;
        propertyCache.repatchCount = restored.repatchCount;
        propertyCache.numberOfCoolDowns = restored.numberOfCoolDowns;
        // The shape group and the handler chain stay as installation built them; only a site given up with no case
        // leaves its *Optimize operation.
        if (restored.givenUp)
            propertyCache.m_slowOperation = gaveUpOperationFor(propertyCache.accessType);
    }
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
