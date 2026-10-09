#pragma once

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ICSection.h"
#include "MetadataTable.h"

// The guarded metadata walkers (SPEC-ics.md section 4.6). A CB whose bytecode adds no metadata entry and no value
// profile has no metadata table, and MetadataTable::forEach reads the offset table through its this pointer, so the
// lane walks metadata only through these functions, which test the table first, as
// CodeBlock::forEachLLIntOrBaselineCallLinkInfo does. They read the offset table and the entries, take no lock and
// allocate nothing; the layout is fixed once UnlinkedMetadataTable::link has run.

namespace JSC::JITCache::ICs {

// Calls functor(Op::Metadata&) for each metadata entry of Op, in metadata ID order.
// Calls nothing when codeBlock.metadataTable() is null.
template<typename Op, typename Functor>
void forEachMetadataEntry(CodeBlock& codeBlock, const Functor& functor)
{
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return;
    metadataTable->forEach<Op>(functor);
}

// Calls functor(unsigned canonicalPosition, unsigned metadataID, CallLinkInfo&) for every
// call-link site, in the order of section 4.2. Calls nothing for a CB without a metadata table.
template<typename Functor>
void forEachCallLinkSite(CodeBlock& codeBlock, const Functor& functor)
{
    unsigned canonicalPosition = 0;
#define JITCACHE_ICS_VISIT_CALL_LINK_SITES(opcodeStruct) \
    do { \
        unsigned metadataID = 0; \
        forEachMetadataEntry<opcodeStruct>(codeBlock, [&](opcodeStruct::Metadata& metadata) { \
            CallLinkInfo& callLinkInfo = metadata.m_callLinkInfo; \
            functor(canonicalPosition, metadataID++, callLinkInfo); \
        }); \
        ++canonicalPosition; \
    } while (false);

    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_VISIT_CALL_LINK_SITES)

#undef JITCACHE_ICS_VISIT_CALL_LINK_SITES
    ASSERT(canonicalPosition == numberOfCallLinkOpcodes);
}

// Entries per call-link opcode; all zero for a CB without a metadata table.
inline CallLinkSiteCounts callLinkSiteCounts(CodeBlock& codeBlock)
{
    CallLinkSiteCounts counts { };
    forEachCallLinkSite(codeBlock, [&](unsigned canonicalPosition, unsigned, CallLinkInfo&) {
        ++counts[canonicalPosition];
    });
    return counts;
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
