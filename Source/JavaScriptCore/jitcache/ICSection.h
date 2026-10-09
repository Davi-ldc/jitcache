#pragma once

#if ENABLE(JIT)

#include "Opcode.h"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>

// The ICsBaseline section of a body file (SPEC-ics.md section 4): the property-IC learning state and call-link history
// of one baseline CB, as dense records in native index order, and the pure derivation of the consumer's state from
// those records (section 6.1).
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
};
static_assert(sizeof(CallLinkRecord) == 2 && alignof(CallLinkRecord) == 1);
static_assert(std::is_trivially_copyable_v<CallLinkRecord>);

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

// Canonical position of opcodeID in FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, or nullopt.
std::optional<unsigned> canonicalCallLinkPosition(OpcodeID);
// Whether opcodeID is one of the four varargs call opcodes, whose sites record a varargs maximum.
bool isVarargsCallLinkOpcode(OpcodeID);

// nullopt on overflow or when groupCount exceeds numberOfCallLinkOpcodes.
std::optional<size_t> sectionSize(size_t groupCount, size_t propertyICCount, size_t callLinkSiteCount);

// Metadata entries per call-link opcode, indexed by canonical position.
using CallLinkSiteCounts = std::array<uint32_t, numberOfCallLinkOpcodes>;

enum class StrictChecks : bool { No, Yes };

struct Summary {
    uint32_t icSitesWithCases { 0 };
};

enum class Check : uint8_t {
    SectionSize, // A1
    SummaryBound, // A2
    CallLinkGroups, // A3
    ReservedBits, // A4
    EnumRange, // A5
    SummaryCount, // A6
    MoldPairing, // A7
    MetadataLayout, // A8
    MoldMegamorphicBit, // S1
    NewbornCallLinks, // S2
};

struct Invalid {
    Check check;
    uint32_t siteIndex; // record index when the check is per site, 0 otherwise
};

// A parsed section, as views into the bytes it was parsed from.
struct SectionView {
    SectionHeader header;
    std::span<const uint8_t> groupBytes; // callLinkGroupCount entries of callLinkGroupSize bytes
    std::span<const PropertyICRecord> propertyICs; // record i for IC i
    std::span<const CallLinkRecord> callLinks; // canonical order
    CallLinkGroup group(unsigned index) const; // read with memcpy
};

// Slices the section by the counts its header states. With StrictChecks::Yes it checks A1 to A6
// first (section 4.5); otherwise it trusts the counts. Reads only its argument and needs no VM.
std::expected<SectionView, Invalid> parseSection(std::span<const uint8_t>, StrictChecks);

// Reads the summary of a saved body from its ICsBaseline section, checking A1 and A2 under
// strict and trusting the header otherwise. Takes no lock and needs no VM.
std::expected<Summary, Invalid> readBaselineICsSummary(std::span<const uint8_t> section, StrictChecks);

// The property-IC half of the polymorphic bit (section 5.3): two or more cases listed, none of
// them megamorphic.
bool isPolymorphicPropertyIC(const PropertyICRecord&);

// The consumer state a record restores (section 6.1). Seeding and attach apply it, and the twin check compares live
// state with it.
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

struct RestoredCallLink {
    bool isVirtual; // seeded through CallLinkInfo::setVirtualCall
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    friend bool operator==(const RestoredCallLink&, const RestoredCallLink&) = default;
};

RestoredPropertyIC restoredPropertyIC(const PropertyICRecord&);
RestoredCallLink restoredCallLink(const CallLinkRecord&);

// The record a capture of the restored state yields before any site runs (I5). Deriving it again gives back the same
// consumer state: restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r), and likewise for call links.
PropertyICRecord recapturedPropertyIC(const PropertyICRecord&);
CallLinkRecord recapturedCallLink(const CallLinkRecord&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
