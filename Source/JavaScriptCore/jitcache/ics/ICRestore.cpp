#include "config.h"
#include "jitcache/ics/ICRestore.h"

#if ENABLE(JIT)

#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "Instruction.h"
#include "JITCode.h"
#include "jitcache/ics/ICOperations.h"
#include "jitcache/ics/ICSites.h"
#include <array>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

// The three restoration steps of an ICsBaseline section: prepare, the seeding of call-link history, and the attaching
// of property-IC state.

namespace JSC::JITCache::ICs {

namespace ICRestoreInternal {

// The index in the section's call-link records of each canonical position's first record. The group table is trusted;
// debug builds assert that each group names a call-link opcode.
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

PreparedBaselineICs prepareBaselineICs(std::span<const uint8_t> section)
{
    // parseSection trusts the counts; seeding and attach assert that the records pair with the CB's ICs and call-link
    // sites.
    SectionView view = parseSection(section);
    PreparedBaselineICs prepared;
    prepared.m_propertyICs = view.propertyICs;
    prepared.m_callLinks = view.callLinks;
    prepared.m_callLinkGroupStarts = ICRestoreInternal::callLinkGroupStarts(view);
    return prepared;
}

void seedCallLinkHistory(const PreparedBaselineICs& prepared, CodeBlock& codeBlock)
{
    ASSERT(codeBlock.jitType() == JITType::None);

    // Native linking writes these fields without a lock, and nothing else reaches the newborn CB, so seeding takes none.
    // Within a group, records follow metadata IDs, and an imported UCB keeps the producer's metadata layout and gives each
    // instruction the producer's metadata ID, so record k of a site's group is the site whose metadata ID is k. Seeding
    // walks instructions rather than metadata because data of a site's own needs the site's bytecode index, which only
    // its instruction knows.
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
    ASSERT(codeBlock.jitType() == JITType::BaselineJIT);

    // Compiler threads read IC state under the CB's lock, so attach writes under it too; other threads reach the CB
    // only after installCode stores and barriers it. Attach writes no cell, so it needs no barrier.
    ConcurrentJSLocker locker(codeBlock.m_lock);
    BaselineJITData* jitData = codeBlock.baselineJITData();
    ASSERT(jitData);
    std::span<const PropertyICRecord> records = prepared.propertyICs();
    // Setup builds IC i from mold i, which the Image lane keeps in the producer's order.
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
