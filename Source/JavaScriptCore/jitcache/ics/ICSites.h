#pragma once

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Instruction.h"
#include "MetadataTable.h"
#include "jitcache/ics/ICSection.h"
#include <type_traits>

// The site walkers. A CB whose bytecode adds no metadata entry and no value profile has no metadata table, and
// MetadataTable::forEach reads the offset table through its this pointer, so the lane walks metadata only through these
// functions, which test the table first, as CodeBlock::forEachLLIntOrBaselineCallLinkInfo does; nothing else in the lane
// walks call-link sites or switches over call-link opcodes. They read the instruction stream, the offset table and the
// entries, take no lock and allocate nothing; the layout is fixed once UnlinkedMetadataTable::link has run.

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

// One call-link site: exactly one of the two pointers is set, as callLinkOpcodes[canonicalPosition].kind says. The four
// varargs opcodes hold a DataOnlyCallLinkInfo in the metadata entry, and the others a LazyCallLinkInfo, which points at a
// CallSiteData of the site's own once the site has one and at data the VM's sites share until then.
struct CallLinkSite {
    unsigned canonicalPosition;
    unsigned metadataID;
    DataOnlyCallLinkInfo* inlineInfo; // an Inline site's entry field
    LazyCallLinkInfo* lazyInfo; // a lazy site's entry field

    const CallLinkOpcode& opcode() const { return callLinkOpcodes[canonicalPosition]; }

    // The CallLinkInfo that belongs to this site: the inline one, or a lazy site's own (LazyCallLinkInfo::get()); null
    // for a lazy site without data of its own, whose shared CallLinkInfo describes no site and must never be written.
    CallLinkInfo* ownCallLinkInfo() const
    {
        if (inlineInfo)
            return inlineInfo;
        return lazyInfo->get();
    }
};

namespace ICSitesInternal {

// The site of Op's metadata entry with the given ID, at Op's position in callLinkOpcodes. The field's type tells the
// two shapes apart, as in CodeBlock::forEachLLIntOrBaselineCallLinkInfo; callLinkOpcodes records the same shape.
template<typename Op>
CallLinkSite callLinkSiteOf(unsigned metadataID, typename Op::Metadata& metadata)
{
    constexpr unsigned canonicalPosition = *canonicalCallLinkPosition(Op::opcodeID);
    constexpr bool isInline = std::is_same_v<decltype(Op::Metadata::m_callLinkInfo), DataOnlyCallLinkInfo>;
    static_assert(isInline == (callLinkOpcodes[canonicalPosition].kind == CallLinkSiteKind::Inline));
    CallLinkSite site { canonicalPosition, metadataID, nullptr, nullptr };
    if constexpr (isInline)
        site.inlineInfo = &metadata.m_callLinkInfo;
    else
        site.lazyInfo = &metadata.m_callLinkInfo;
    return site;
}

} // namespace ICSitesInternal

// Calls functor(const CallLinkSite&) for every call-link site, by canonical position and, within a position, in
// metadata ID order. Calls nothing for a CB without a metadata table.
template<typename Functor>
void forEachCallLinkSite(CodeBlock& codeBlock, const Functor& functor)
{
#define JITCACHE_ICS_VISIT_CALL_LINK_SITES(opcodeStruct) \
    do { \
        unsigned metadataID = 0; \
        forEachMetadataEntry<opcodeStruct>(codeBlock, [&](opcodeStruct::Metadata& metadata) { \
            const CallLinkSite site = ICSitesInternal::callLinkSiteOf<opcodeStruct>(metadataID++, metadata); \
            functor(site); \
        }); \
    } while (false);

    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_VISIT_CALL_LINK_SITES)

#undef JITCACHE_ICS_VISIT_CALL_LINK_SITES
}

// Calls functor(const CallLinkSite&, const JSInstruction*) for every instruction of the CB whose opcode is in
// FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, in instruction order, with that instruction's metadata entry. Seeding walks sites
// this way, because a site's own CallSiteData needs the bytecode index that only its instruction knows. A CB without a
// metadata table has no such instruction, since every call-link opcode adds a metadata entry.
template<typename Functor>
void forEachCallLinkInstruction(CodeBlock& codeBlock, const Functor& functor)
{
    for (const auto& instruction : codeBlock.instructions()) {
        switch (instruction->opcodeID()) {
#define JITCACHE_ICS_VISIT_CALL_LINK_INSTRUCTION(opcodeStruct) \
        case opcodeStruct::opcodeID: { \
            auto bytecode = instruction->as<opcodeStruct>(); \
            const CallLinkSite site = ICSitesInternal::callLinkSiteOf<opcodeStruct>(bytecode.m_metadataID, bytecode.metadata(&codeBlock)); \
            functor(site, instruction.ptr()); \
            break; \
        }

        FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_VISIT_CALL_LINK_INSTRUCTION)

#undef JITCACHE_ICS_VISIT_CALL_LINK_INSTRUCTION
        default:
            break;
        }
    }
}

// Entries per call-link opcode; all zero for a CB without a metadata table.
inline CallLinkSiteCounts callLinkSiteCounts(CodeBlock& codeBlock)
{
    CallLinkSiteCounts counts { };
    forEachCallLinkSite(codeBlock, [&](const CallLinkSite& site) {
        ++counts[site.canonicalPosition];
    });
    return counts;
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
