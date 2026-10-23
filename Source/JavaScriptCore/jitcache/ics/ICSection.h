#pragma once

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "Opcode.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/StdLibExtras.h>

// The ICsBaseline section of a body file: the property-IC learning state and call-link history of one baseline CB, a
// call site's this profile included, as dense records in native index order, and the pure derivation of the consumer's
// state from those records.
//
// Layout, all integers little-endian, no alignment required of the section start:
//   0                     u32 propertyICCount
//   4                     u32 icSitesWithCases
//   8                     u32 callLinkGroupCount, at most numberOfCallLinkOpcodes
//   12                    u32 callLinkSiteCount
//   16                    CallLinkGroup[callLinkGroupCount], in increasing canonical position
//   then                  PropertyICRecord[propertyICCount], record i for IC i (mold order)
//   then                  CallLinkRecord[callLinkSiteCount], group by group, metadata ID order within a group
//   then                  zero bytes up to a multiple of 8

namespace JSC::JITCache::ICs {

static_assert(std::endian::native == std::endian::little, "the ICsBaseline section is read and written in native byte order");

enum class CallLinkModeCode : uint8_t { Init = 0, Monomorphic = 1, Polymorphic = 2, Virtual = 3 };

struct PropertyICRecord {
    uint8_t accessType; // JSC::AccessType
    uint8_t learningBits; // LearningBit
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;
    uint8_t caseCount; // entries of PropertyInlineCache::listedAccessCases
    uint8_t stateBits; // StateBit
    uint8_t reserved[2];
};
static_assert(sizeof(PropertyICRecord) == 8 && alignof(PropertyICRecord) == 1);
static_assert(std::is_trivially_copyable_v<PropertyICRecord>);

struct CallLinkRecord {
    uint8_t bits; // CallLinkBit, with the CallLinkModeCode in modeMask
    uint8_t maxArgumentCountIncludingThisForVarargs;
    uint8_t thisArrayModes[4]; // ArrayModes, little-endian, through memcpy
    uint8_t thisArrayProfileFlags[4]; // OptionSet<ArrayProfileFlag>::toRaw(), little-endian, through memcpy
};
static_assert(sizeof(CallLinkRecord) == 10 && alignof(CallLinkRecord) == 1);
static_assert(std::is_trivially_copyable_v<CallLinkRecord>);

// The two this words of a call-link record. A record has alignment 1, so they are read and written with memcpy.
inline uint32_t readThisArrayModes(const CallLinkRecord& record)
{
    uint32_t modes { 0 };
    memcpySpan(asMutableByteSpan(modes), asByteSpan(record.thisArrayModes));
    return modes;
}

inline uint32_t readThisArrayProfileFlags(const CallLinkRecord& record)
{
    uint32_t flags { 0 };
    memcpySpan(asMutableByteSpan(flags), asByteSpan(record.thisArrayProfileFlags));
    return flags;
}

inline void writeThisProfile(CallLinkRecord& record, uint32_t modes, uint32_t flags)
{
    memcpySpan(asMutableByteSpan(record.thisArrayModes), asByteSpan(modes));
    memcpySpan(asMutableByteSpan(record.thisArrayProfileFlags), asByteSpan(flags));
}

struct SectionHeader {
    uint32_t propertyICCount;
    uint32_t icSitesWithCases;
    uint32_t callLinkGroupCount;
    uint32_t callLinkSiteCount;
};

struct CallLinkGroup {
    uint32_t opcodeID;
    uint32_t siteCount;
};

namespace LearningBit {
inline constexpr uint8_t everConsidered = 1 << 0;
inline constexpr uint8_t sawNonCell = 1 << 1;
inline constexpr uint8_t tookSlowPath = 1 << 2;
inline constexpr uint8_t resetByGC = 1 << 3;
inline constexpr uint8_t mask = 0x0f;
}

namespace StateBit {
inline constexpr uint8_t megamorphicCaseListed = 1 << 0;
inline constexpr uint8_t canBeMegamorphic = 1 << 1;
inline constexpr uint8_t holdsGaveUp = 1 << 2;
inline constexpr uint8_t mask = 0x07;
}

namespace CallLinkBit {
inline constexpr uint8_t modeMask = 0x03;
inline constexpr uint8_t seenOnce = 1 << 2;
inline constexpr uint8_t hasSeenClosure = 1 << 3;
inline constexpr uint8_t clearedByGC = 1 << 4;
inline constexpr uint8_t clearedByVirtual = 1 << 5;
inline constexpr uint8_t mask = 0x3f;
}

inline constexpr size_t sectionHeaderSize = 16;
inline constexpr size_t callLinkGroupSize = 8;
inline constexpr unsigned numberOfCallLinkOpcodes = 14;

static_assert(sizeof(SectionHeader) == sectionHeaderSize && std::is_trivially_copyable_v<SectionHeader>);
static_assert(sizeof(CallLinkGroup) == callLinkGroupSize && std::is_trivially_copyable_v<CallLinkGroup>);

#define JITCACHE_ICS_COUNT_CALL_LINK_OPCODE(opcodeStruct) +1
static_assert(0 FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_COUNT_CALL_LINK_OPCODE) == numberOfCallLinkOpcodes,
    "the canonical call-link opcode list is FOR_EACH_OPCODE_WITH_CALL_LINK_INFO");
#undef JITCACHE_ICS_COUNT_CALL_LINK_OPCODE

// How an opcode's metadata entry holds its call link.
enum class CallLinkSiteKind : uint8_t {
    Inline, // a DataOnlyCallLinkInfo in the entry: the four varargs opcodes
    Lazy, // a LazyCallLinkInfo: FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO but OpTailCall
    LazyTailCall, // OpTailCall's LazyCallLinkInfo, linked to the not-executed tail-call data
};

struct CallLinkOpcode {
    OpcodeID opcodeID;
    CallLinkSiteKind kind;
    bool hasThisProfile; // the site's CallSiteData holds the ArrayProfile of its this value
};

namespace ICSectionInternal {

#define JITCACHE_ICS_IS_LISTED(opcodeStruct) || opcodeID == opcodeStruct::opcodeID
constexpr bool hasLazyCallLinkInfo(OpcodeID opcodeID)
{
    return false FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO(JITCACHE_ICS_IS_LISTED);
}

constexpr bool hasSimpleArrayProfile(OpcodeID opcodeID)
{
    return false FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE(JITCACHE_ICS_IS_LISTED);
}
#undef JITCACHE_ICS_IS_LISTED

// The entry of Op. The lazy opcodes hold a LazyCallLinkInfo and the others a DataOnlyCallLinkInfo, which the site
// walkers rely on. An opcode with a simple array profile and no m_arrayProfile in its metadata keeps that profile in
// the site's CallSiteData, where CodeBlock::getArrayProfile finds it.
template<typename Op>
consteval CallLinkOpcode callLinkOpcodeOf()
{
    constexpr bool isLazy = hasLazyCallLinkInfo(Op::opcodeID);
    static_assert(std::is_same_v<decltype(Op::Metadata::m_callLinkInfo), std::conditional_t<isLazy, LazyCallLinkInfo, DataOnlyCallLinkInfo>>,
        "the lazy call-link opcodes hold a LazyCallLinkInfo and the others a DataOnlyCallLinkInfo");
    constexpr bool hasArrayProfileField = requires(typename Op::Metadata& metadata) { metadata.m_arrayProfile; };
    CallLinkSiteKind kind = CallLinkSiteKind::Inline;
    if (isLazy)
        kind = Op::opcodeID == OpTailCall::opcodeID ? CallLinkSiteKind::LazyTailCall : CallLinkSiteKind::Lazy;
    return CallLinkOpcode { Op::opcodeID, kind, hasSimpleArrayProfile(Op::opcodeID) && !hasArrayProfileField };
}

} // namespace ICSectionInternal

// The opcode at each canonical call-link position, generated in macro order from FOR_EACH_OPCODE_WITH_CALL_LINK_INFO.
// kind is Lazy or LazyTailCall exactly for the members of FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO. hasThisProfile is
// true exactly for the members of FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE whose Metadata has no m_arrayProfile, the
// test arrayProfileFor in bytecode/CodeBlock.cpp makes: OpCall, OpCallIgnoreResult and OpTailCall. A section's group word
// names its opcode by position, and capture, restore and the twin check take a site's opcode, shape and this profile
// from this table.
inline constexpr std::array<CallLinkOpcode, numberOfCallLinkOpcodes> callLinkOpcodes {
#define JITCACHE_ICS_CALL_LINK_OPCODE(opcodeStruct) ICSectionInternal::callLinkOpcodeOf<opcodeStruct>(),
    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_CALL_LINK_OPCODE)
#undef JITCACHE_ICS_CALL_LINK_OPCODE
};

// Canonical position of opcodeID in FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, or nullopt. The site walkers also use it at
// compile time.
constexpr std::optional<unsigned> canonicalCallLinkPosition(OpcodeID opcodeID)
{
    for (unsigned position = 0; position < numberOfCallLinkOpcodes; ++position) {
        if (callLinkOpcodes[position].opcodeID == opcodeID)
            return position;
    }
    return std::nullopt;
}

// nullopt on overflow or when groupCount exceeds numberOfCallLinkOpcodes.
std::optional<size_t> sectionSize(size_t groupCount, size_t propertyICCount, size_t callLinkSiteCount);

// Metadata entries per call-link opcode, indexed by canonical position.
using CallLinkSiteCounts = std::array<uint32_t, numberOfCallLinkOpcodes>;

struct Summary {
    uint32_t icSitesWithCases { 0 };
};

// A parsed section, as views into the bytes it was parsed from.
struct SectionView {
    SectionHeader header;
    std::span<const uint8_t> groupBytes; // callLinkGroupCount entries of callLinkGroupSize bytes
    std::span<const PropertyICRecord> propertyICs; // record i for IC i
    std::span<const CallLinkRecord> callLinks; // canonical order
    CallLinkGroup group(unsigned index) const; // read with memcpy
};

// Slices the section by the counts its header states, which it trusts. Reads only its argument and needs no VM.
SectionView parseSection(std::span<const uint8_t>);

// Reads the summary of a saved body from the header of its ICsBaseline section, which it trusts. Takes no lock and
// needs no VM.
Summary readBaselineICsSummary(std::span<const uint8_t> section);

// The property-IC half of the polymorphic bit: two or more cases listed, none of them megamorphic.
bool isPolymorphicPropertyIC(const PropertyICRecord&);

// The consumer state a record restores. Seeding and attach apply it, and the twin check compares live state with it.
struct RestoredPropertyIC {
    bool everConsidered;
    bool sawNonCell;
    bool tookSlowPath;
    bool resetByGC;
    bool foldsAtFirstCase; // ORed into canBeMegamorphic
    bool givenUp; // m_slowOperation becomes the access type's *GaveUp operation
    uint8_t countdown; // always 0
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;
    friend bool operator==(const RestoredPropertyIC&, const RestoredPropertyIC&) = default;
};

// Which CallSiteData a site points at.
enum class CallSiteDataState : uint8_t {
    Inline, // an Inline site, which has no CallSiteData
    NotExecuted, // what linking left: the VM's never-executed data, or its not-executed tail-call data
    ExecutedOnce, // the VM's executed-once data
    Own, // a CallSiteData of its own
};

struct RestoredCallLink {
    CallSiteDataState data;
    // The rest describes the site's own CallLinkInfo, which exists when data is Inline or Own.
    bool isVirtual; // seeded through CallLinkInfo::setVirtualCall
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    // The accumulated state of the site's this profile, restored into its own data when data is Own.
    uint32_t thisArrayModes;
    uint32_t thisArrayProfileFlags;
    friend bool operator==(const RestoredCallLink&, const RestoredCallLink&) = default;
};

RestoredPropertyIC restoredPropertyIC(const PropertyICRecord&);
RestoredCallLink restoredCallLink(const CallLinkOpcode&, const CallLinkRecord&);

// The record a capture of the restored state yields before any site runs. Deriving it again gives back the same consumer
// state, restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r), and likewise for call links at a site of
// the same opcode, so a body recaptured before its sites run restores what the original record did.
PropertyICRecord recapturedPropertyIC(const PropertyICRecord&);
CallLinkRecord recapturedCallLink(const CallLinkOpcode&, const CallLinkRecord&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
