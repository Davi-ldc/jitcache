#include "config.h"
#include "ICRestore.h"

#if ENABLE(JIT)

#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "ICSites.h"
#include "Instruction.h"
#include "JITCode.h"
#include "Repatch.h"
#include <array>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

// The strict checks of an ICsBaseline section against the prepared code and the newborn CB, and the three restoration
// steps: prepare, the seeding of call-link history, and the attaching of property-IC state.

namespace JSC::JITCache::ICs {

namespace ICRestoreInternal {

// Whether a call-link site is as linking left it, under the fixed useLazyLLIntCallLinkInfos and useLLIntICs. A lazy site
// has no data of its own and does not point at the executed-once data; every lazy site but an op_tail_call one points at
// the never-executed data, and an op_tail_call one does not, which leaves it on the not-executed tail-call data, the only
// other data linking gives. An inline site has every field seeding writes over as DataOnlyCallLinkInfo::initialize left
// it, in Init, and holds no callee, stub or incoming-call link.
static bool isAsLinkingLeftIt(VM& vm, const CallLinkSite& site)
{
    if (!site.inlineInfo) {
        const LazyCallLinkInfo& lazyInfo = *site.lazyInfo;
        if (lazyInfo.get() || lazyInfo.hasExecutedOnce())
            return false;
        bool neverExecuted = lazyInfo.hasNeverExecuted(vm);
        if (site.opcode().kind == CallLinkSiteKind::LazyTailCall)
            return !neverExecuted;
        return neverExecuted;
    }

    DataOnlyCallLinkInfo& callLinkInfo = *site.inlineInfo;
    return callLinkInfo.type() == CallLinkInfo::Type::DataOnly
        && callLinkInfo.callType() == CallLinkInfo::callTypeFor(site.opcode().opcodeID)
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

// The index in the section's call-link records of each canonical position's first record. A strict parse checked the
// group table; normal mode trusts it, as debug builds assert.
static CallLinkSiteCounts callLinkGroupStarts(const SectionView& view)
{
    CallLinkSiteCounts counts { };
    for (unsigned index = 0; index < view.header.callLinkGroupCount; ++index) {
        CallLinkGroup group = view.group(index);
        auto position = canonicalCallLinkPosition(static_cast<OpcodeID>(group.opcodeID));
        ASSERT(position);
        if (position)
            counts[*position] += group.siteCount;
    }
    CallLinkSiteCounts starts { };
    uint32_t start = 0;
    for (unsigned position = 0; position < numberOfCallLinkOpcodes; ++position) {
        starts[position] = start;
        start += counts[position];
    }
    return starts;
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
    // The section's per-opcode counts, from its groups. A strictly parsed section has one group per opcode at most, each
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
    // records. In a strictly parsed section, whose groups sum to callLinkSiteCount, this already follows from the
    // per-opcode counts; comparing the total keeps the seeding cursor in bounds for any view.
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
    // alone, which the round trip through recapturedPropertyIC relies on.
    for (size_t index = 0; index < molds.size(); ++index) {
        if (molds[index].canBeMegamorphic)
            return Invalid { Check::MoldMegamorphicBit, static_cast<uint32_t>(index) };
    }
    return std::nullopt;
}

std::optional<Invalid> checkNewbornCodeBlock(CodeBlock& codeBlock)
{
    // A CB with a JIT type has run, or been set up, and imported state goes only to a CB that never ran.
    if (codeBlock.jitType() != JITType::None)
        return Invalid { Check::NewbornCallLinks, 0 };

    VM& vm = codeBlock.vm();
    std::optional<Invalid> result;
    uint32_t siteIndex = 0;
    forEachCallLinkSite(codeBlock, [&](const CallLinkSite& site) {
        uint32_t index = siteIndex++;
        if (result)
            return;
        if (!ICRestoreInternal::isAsLinkingLeftIt(vm, site))
            result = Invalid { Check::NewbornCallLinks, index };
    });
    return result;
}

std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks strict)
{
    // Without strict, parseSection trusts the counts and debug builds assert its checks; seeding and attach assert that
    // the records pair with the CB's ICs and call-link sites.
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
    prepared.m_callLinkGroupStarts = ICRestoreInternal::callLinkGroupStarts(*view);
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
    // Within a group, records follow metadata IDs, and an imported UCB keeps the producer's metadata layout and gives each
    // instruction the producer's metadata ID, so record k of a site's group is the site whose metadata ID is k;
    // checkMetadataLayout confirms the counts under strict. Seeding walks instructions rather than metadata because data
    // of a site's own needs the site's bytecode index, which only its instruction knows.
    VM& vm = codeBlock.vm();
    std::span<const CallLinkRecord> records = prepared.callLinks();
    const CallLinkSiteCounts& groupStarts = prepared.callLinkGroupStarts();
    forEachCallLinkInstruction(codeBlock, [&](const CallLinkSite& site, const JSInstruction* instruction) {
        size_t recordIndex = static_cast<size_t>(groupStarts[site.canonicalPosition]) + site.metadataID;
        ASSERT(recordIndex < (site.canonicalPosition + 1 < numberOfCallLinkOpcodes ? groupStarts[site.canonicalPosition + 1] : records.size()));
        RestoredCallLink restored = restoredCallLink(site.opcode(), records[recordIndex]);

        CallLinkInfo* callLinkInfo = nullptr;
        switch (restored.data) {
        case CallSiteDataState::NotExecuted:
            // The site keeps what linking gave it.
            return;
        case CallSiteDataState::ExecutedOnce:
            // The site ran and holds nothing, so its next call gives it data of its own and links it, as the producer's
            // next call would have. Only the site's pointer moves; the shared data never changes.
            site.lazyInfo->setExecutedOnce(vm);
            return;
        case CallSiteDataState::Own:
            // The native slow paths' route to a site's data, which derives the call type and the code origin from the
            // instruction as linking would. The new data starts unseen, is published after a store-store fence and is
            // reported to the heap in batches, which under the install function's GC deferral only records deferred
            // work.
            callLinkInfo = &codeBlock.ensureCallLinkInfoAt(instruction);
            break;
        case CallSiteDataState::Inline:
            callLinkInfo = site.inlineInfo;
            break;
        }

        // Every setter below runs on a CallLinkInfo of the site's own, never on one the VM's sites share. setVirtualCall
        // comes first, because the reset it runs clears the seen bit. It allocates nothing and takes no lock: the
        // virtual-call thunks exist from the VM's construction.
        if (restored.isVirtual)
            callLinkInfo->setVirtualCall(vm);
        else if (restored.seenOnce)
            callLinkInfo->setSeen();
        if (restored.hasSeenClosure)
            callLinkInfo->setHasSeenClosure();
        if (restored.clearedByGC)
            callLinkInfo->setClearedByGC();
        if (restored.clearedByVirtual)
            callLinkInfo->setClearedByVirtual();
        if (restored.maxArgumentCountIncludingThisForVarargs)
            callLinkInfo->updateMaxArgumentCountIncludingThisForVarargs(restored.maxArgumentCountIncludingThisForVarargs);

        // The this profile last, into the data the site now owns, whose two structure samples start empty.
        if (restored.data == CallSiteDataState::Own && site.opcode().hasThisProfile)
            site.lazyInfo->arrayProfile()->restoreAccumulatedState(restored.thisArrayModes, OptionSet<ArrayProfileFlag>::fromRaw(restored.thisArrayProfileFlags));
    });
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
    // Setup builds IC i from mold i, which the Image lane keeps in the producer's order; checkMoldPairing confirms it
    // under strict.
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
