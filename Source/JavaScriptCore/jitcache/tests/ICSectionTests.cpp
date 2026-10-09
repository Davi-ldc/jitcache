#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BytecodeStructs.h"
#include "ICRestore.h"
#include "ICSection.h"
#include "JITCacheTest.h"
#include "PropertyInlineCache.h"
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// T7 of SPEC-ics.md: the ICsBaseline format, its structural checks (A1 to A6 in parseSection; A7, A8 and S1, which
// prepare composes, on plain molds and site counts), the derivation of section 6.1 and its round trip. None of these
// tests needs a VM.

namespace JSC::JITCache::Tests {

namespace ICSectionTestsInternal {

// A section as the format of SPEC-ics.md section 4.1 lays it out. encode() writes the header words as they stand, so a
// test can make them disagree with the arrays.
struct TestSection {
    uint32_t propertyICCount { 0 };
    uint32_t icSitesWithCases { 0 };
    uint32_t callLinkGroupCount { 0 };
    uint32_t callLinkSiteCount { 0 };
    Vector<ICs::CallLinkGroup> groups;
    Vector<ICs::PropertyICRecord> propertyICs;
    Vector<ICs::CallLinkRecord> callLinks;
};

static void appendUInt32(Vector<uint8_t>& bytes, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.append(static_cast<uint8_t>(value >> shift));
}

// Writes the format byte by byte, independently of the lane's structs, so a parse checks their layout.
static Vector<uint8_t> encode(const TestSection& section)
{
    Vector<uint8_t> bytes;
    appendUInt32(bytes, section.propertyICCount);
    appendUInt32(bytes, section.icSitesWithCases);
    appendUInt32(bytes, section.callLinkGroupCount);
    appendUInt32(bytes, section.callLinkSiteCount);
    for (auto& group : section.groups) {
        appendUInt32(bytes, group.opcodeID);
        appendUInt32(bytes, group.siteCount);
    }
    for (auto& record : section.propertyICs) {
        bytes.append(record.accessType);
        bytes.append(record.learningBits);
        bytes.append(record.repatchCount);
        bytes.append(record.numberOfCoolDowns);
        bytes.append(record.caseCount);
        bytes.append(record.stateBits);
        bytes.append(record.reserved[0]);
        bytes.append(record.reserved[1]);
    }
    for (auto& record : section.callLinks) {
        bytes.append(record.bits);
        bytes.append(record.maxArgumentCountIncludingThisForVarargs);
    }
    while (bytes.size() % 8)
        bytes.append(0);
    return bytes;
}

static ICs::PropertyICRecord propertyIC(AccessType accessType, uint8_t learningBits, uint8_t repatchCount, uint8_t numberOfCoolDowns, uint8_t caseCount, uint8_t stateBits)
{
    return ICs::PropertyICRecord {
        .accessType = static_cast<uint8_t>(accessType),
        .learningBits = learningBits,
        .repatchCount = repatchCount,
        .numberOfCoolDowns = numberOfCoolDowns,
        .caseCount = caseCount,
        .stateBits = stateBits,
        .reserved = { 0, 0 },
    };
}

static ICs::CallLinkRecord callLink(ICs::CallLinkModeCode mode, uint8_t historyBits, uint8_t maxArgumentCountIncludingThisForVarargs)
{
    return ICs::CallLinkRecord {
        .bits = static_cast<uint8_t>(static_cast<uint8_t>(mode) | historyBits),
        .maxArgumentCountIncludingThisForVarargs = maxArgumentCountIncludingThisForVarargs,
    };
}

// Three property ICs (two with cases), and three call-link sites in two groups: 16 + 16 + 24 + 6 bytes, padded with two
// zero bytes to 64.
static constexpr size_t validSectionSize = 64;
static constexpr size_t validSectionPaddingOffset = 62;

static TestSection validSection()
{
    TestSection section;
    section.propertyICs.append(propertyIC(AccessType::GetById, ICs::LearningBit::everConsidered | ICs::LearningBit::tookSlowPath, 3, 1, 2, 0));
    section.propertyICs.append(propertyIC(AccessType::PutByIdStrict, 0, 0, 0, 0, 0));
    section.propertyICs.append(propertyIC(AccessType::InstanceOf, ICs::LearningBit::everConsidered, 9, 2, 1, ICs::StateBit::megamorphicCaseListed | ICs::StateBit::holdsGaveUp));
    section.groups.append(ICs::CallLinkGroup { OpCall::opcodeID, 2 });
    section.groups.append(ICs::CallLinkGroup { OpConstruct::opcodeID, 1 });
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Monomorphic, ICs::CallLinkBit::seenOnce, 0));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Virtual, ICs::CallLinkBit::clearedByVirtual, 0));
    section.propertyICCount = static_cast<uint32_t>(section.propertyICs.size());
    section.icSitesWithCases = 2;
    section.callLinkGroupCount = static_cast<uint32_t>(section.groups.size());
    section.callLinkSiteCount = static_cast<uint32_t>(section.callLinks.size());
    return section;
}

// The canonical call-link order as SPEC-ics.md section 4.2 lists it.
static constexpr std::array<OpcodeID, ICs::numberOfCallLinkOpcodes> canonicalCallLinkOrder {
    OpCall::opcodeID,
    OpTailCall::opcodeID,
    OpCallDirectEval::opcodeID,
    OpConstruct::opcodeID,
    OpSuperConstruct::opcodeID,
    OpIteratorOpen::opcodeID,
    OpIteratorNext::opcodeID,
    OpAsyncIteratorOpen::opcodeID,
    OpAsyncIteratorNext::opcodeID,
    OpCallVarargs::opcodeID,
    OpTailCallVarargs::opcodeID,
    OpConstructVarargs::opcodeID,
    OpSuperConstructVarargs::opcodeID,
    OpCallIgnoreResult::opcodeID,
};

template<typename Functor>
static Vector<uint8_t> encodeValidSectionWith(const Functor& change)
{
    TestSection section = validSection();
    change(section);
    return encode(section);
}

static ASCIILiteral checkName(ICs::Check check)
{
    switch (check) {
    case ICs::Check::SectionSize:
        return "SectionSize"_s;
    case ICs::Check::SummaryBound:
        return "SummaryBound"_s;
    case ICs::Check::CallLinkGroups:
        return "CallLinkGroups"_s;
    case ICs::Check::ReservedBits:
        return "ReservedBits"_s;
    case ICs::Check::EnumRange:
        return "EnumRange"_s;
    case ICs::Check::SummaryCount:
        return "SummaryCount"_s;
    case ICs::Check::MoldPairing:
        return "MoldPairing"_s;
    case ICs::Check::MetadataLayout:
        return "MetadataLayout"_s;
    case ICs::Check::MoldMegamorphicBit:
        return "MoldMegamorphicBit"_s;
    case ICs::Check::NewbornCallLinks:
        return "NewbornCallLinks"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral bit(bool value)
{
    return value ? "1"_s : "0"_s;
}

static String describe(const ICs::PropertyICRecord& record)
{
    return makeString("{accessType "_s, static_cast<unsigned>(record.accessType), ", learningBits "_s, static_cast<unsigned>(record.learningBits),
        ", repatchCount "_s, static_cast<unsigned>(record.repatchCount), ", numberOfCoolDowns "_s, static_cast<unsigned>(record.numberOfCoolDowns),
        ", caseCount "_s, static_cast<unsigned>(record.caseCount), ", stateBits "_s, static_cast<unsigned>(record.stateBits),
        ", reserved "_s, static_cast<unsigned>(record.reserved[0]), ' ', static_cast<unsigned>(record.reserved[1]), '}');
}

static String describe(const ICs::CallLinkRecord& record)
{
    return makeString("{bits "_s, static_cast<unsigned>(record.bits), ", maxArgumentCountIncludingThisForVarargs "_s, static_cast<unsigned>(record.maxArgumentCountIncludingThisForVarargs), '}');
}

static String describe(const ICs::RestoredPropertyIC& state)
{
    return makeString("{everConsidered "_s, bit(state.everConsidered), ", sawNonCell "_s, bit(state.sawNonCell), ", tookSlowPath "_s, bit(state.tookSlowPath),
        ", resetByGC "_s, bit(state.resetByGC), ", foldsAtFirstCase "_s, bit(state.foldsAtFirstCase), ", givenUp "_s, bit(state.givenUp),
        ", countdown "_s, static_cast<unsigned>(state.countdown), ", repatchCount "_s, static_cast<unsigned>(state.repatchCount),
        ", numberOfCoolDowns "_s, static_cast<unsigned>(state.numberOfCoolDowns), '}');
}

static String describe(const ICs::RestoredCallLink& state)
{
    return makeString("{isVirtual "_s, bit(state.isVirtual), ", seenOnce "_s, bit(state.seenOnce), ", hasSeenClosure "_s, bit(state.hasSeenClosure),
        ", clearedByGC "_s, bit(state.clearedByGC), ", clearedByVirtual "_s, bit(state.clearedByVirtual),
        ", maxArgumentCountIncludingThisForVarargs "_s, static_cast<unsigned>(state.maxArgumentCountIncludingThisForVarargs), '}');
}

static bool sameRecord(const ICs::PropertyICRecord& a, const ICs::PropertyICRecord& b)
{
    return equalSpans(asByteSpan(a), asByteSpan(b));
}

static bool sameRecord(const ICs::CallLinkRecord& a, const ICs::CallLinkRecord& b)
{
    return equalSpans(asByteSpan(a), asByteSpan(b));
}

// Records that the parse or summary read failed exactly the given check at the given site.
template<typename T>
static void expectInvalid(TestContext& context, ASCIILiteral what, const std::expected<T, ICs::Invalid>& result, ICs::Check check, uint32_t siteIndex = 0)
{
    if (result) {
        JITCACHE_FAIL(makeString(what, ": accepted, expected "_s, checkName(check), " at site "_s, siteIndex));
        return;
    }
    if (result.error().check != check || result.error().siteIndex != siteIndex) {
        JITCACHE_FAIL(makeString(what, ": failed "_s, checkName(result.error().check), " at site "_s, result.error().siteIndex,
            ", expected "_s, checkName(check), " at site "_s, siteIndex));
    }
}

static std::expected<ICs::SectionView, ICs::Invalid> parseStrict(std::span<const uint8_t> bytes)
{
    return ICs::parseSection(bytes, ICs::StrictChecks::Yes);
}

// Applies change to the valid section and records that the strict parse of the result fails exactly the given check at
// the given site.
template<typename Functor>
static void expectChangeInvalid(TestContext& context, ASCIILiteral what, ICs::Check check, uint32_t siteIndex, const Functor& change)
{
    Vector<uint8_t> bytes = encodeValidSectionWith(change);
    expectInvalid(context, what, parseStrict(bytes.span()), check, siteIndex);
}

// Records that one of prepare's check functions (A7, A8, S1) failed exactly the given check at the given site.
static void expectCheckFails(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& result, ICs::Check check, uint32_t siteIndex)
{
    if (!result) {
        JITCACHE_FAIL(makeString(what, ": passed, expected "_s, checkName(check), " at site "_s, siteIndex));
        return;
    }
    if (result->check != check || result->siteIndex != siteIndex) {
        JITCACHE_FAIL(makeString(what, ": failed "_s, checkName(result->check), " at site "_s, result->siteIndex,
            ", expected "_s, checkName(check), " at site "_s, siteIndex));
    }
}

static void expectCheckPasses(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& result)
{
    if (result)
        JITCACHE_FAIL(makeString(what, ": failed "_s, checkName(result->check), " at site "_s, result->siteIndex));
}

// A mold as baseline emission leaves it for these checks: only its access type and fold bit matter to A7 and S1.
static BaselineUnlinkedPropertyInlineCache mold(AccessType accessType, bool canBeMegamorphic = false)
{
    BaselineUnlinkedPropertyInlineCache result;
    result.accessType = accessType;
    result.canBeMegamorphic = canBeMegamorphic;
    return result;
}

// The molds the valid section's three property-IC records stand for, in mold order.
static Vector<BaselineUnlinkedPropertyInlineCache> validSectionMolds()
{
    Vector<BaselineUnlinkedPropertyInlineCache> molds;
    molds.append(mold(AccessType::GetById));
    molds.append(mold(AccessType::PutByIdStrict));
    molds.append(mold(AccessType::InstanceOf));
    return molds;
}

static unsigned callLinkPosition(OpcodeID opcodeID)
{
    auto position = ICs::canonicalCallLinkPosition(opcodeID);
    RELEASE_ASSERT(position);
    return *position;
}

// The call-link site counts of a CB whose metadata layout matches the valid section: two call sites, one construct.
static ICs::CallLinkSiteCounts validSectionSiteCounts()
{
    ICs::CallLinkSiteCounts counts { };
    counts[callLinkPosition(OpCall::opcodeID)] = 2;
    counts[callLinkPosition(OpConstruct::opcodeID)] = 1;
    return counts;
}

// The table of SPEC-ics.md section 6.1 for a property-IC record, restated from the SPEC's text.
static ICs::RestoredPropertyIC expectedRestoredPropertyIC(const ICs::PropertyICRecord& record)
{
    bool megamorphicCaseListed = record.stateBits & ICs::StateBit::megamorphicCaseListed;
    bool canBeMegamorphic = record.stateBits & ICs::StateBit::canBeMegamorphic;
    bool holdsGaveUp = record.stateBits & ICs::StateBit::holdsGaveUp;
    bool folded = megamorphicCaseListed || canBeMegamorphic;
    bool givenUp = holdsGaveUp && !record.caseCount;
    unsigned rewound = record.repatchCount > record.caseCount ? record.repatchCount - record.caseCount : 0;
    return ICs::RestoredPropertyIC {
        .everConsidered = static_cast<bool>(record.learningBits & ICs::LearningBit::everConsidered),
        .sawNonCell = static_cast<bool>(record.learningBits & ICs::LearningBit::sawNonCell),
        .tookSlowPath = (record.learningBits & ICs::LearningBit::tookSlowPath) || givenUp,
        .resetByGC = static_cast<bool>(record.learningBits & ICs::LearningBit::resetByGC),
        .foldsAtFirstCase = folded && !givenUp,
        .givenUp = givenUp,
        .countdown = 0,
        .repatchCount = static_cast<uint8_t>(rewound),
        .numberOfCoolDowns = record.numberOfCoolDowns,
    };
}

// The table of SPEC-ics.md section 6.1 for a call-link record, restated from the SPEC's text.
static ICs::RestoredCallLink expectedRestoredCallLink(const ICs::CallLinkRecord& record)
{
    bool isVirtual = (record.bits & ICs::CallLinkBit::modeMask) == static_cast<uint8_t>(ICs::CallLinkModeCode::Virtual);
    return ICs::RestoredCallLink {
        .isVirtual = isVirtual,
        .seenOnce = !isVirtual,
        .hasSeenClosure = static_cast<bool>(record.bits & ICs::CallLinkBit::hasSeenClosure),
        .clearedByGC = static_cast<bool>(record.bits & ICs::CallLinkBit::clearedByGC),
        .clearedByVirtual = (record.bits & ICs::CallLinkBit::clearedByVirtual) || isVirtual,
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
    };
}

static constexpr std::array<uint8_t, 8> sweptCaseCounts { 0, 1, 2, 3, 7, 8, 9, 255 };
static constexpr std::array<uint8_t, 8> sweptRepatchCounts { 0, 1, 2, 7, 8, 9, 254, 255 };
static constexpr std::array<uint8_t, 3> sweptCoolDowns { 0, 4, 255 };

// Calls functor(record) for every property-IC record the derivation tests sweep: every learning and state bit, case and
// repatch counts on both sides of each other, and the extreme access types.
template<typename Functor>
static void forEachSweptPropertyIC(const Functor& functor)
{
    for (unsigned accessType : { 0u, numberOfAccessTypes - 1 }) {
        for (unsigned learningBits = 0; learningBits <= static_cast<unsigned>(ICs::LearningBit::mask); ++learningBits) {
            for (unsigned stateBits = 0; stateBits <= static_cast<unsigned>(ICs::StateBit::mask); ++stateBits) {
                for (uint8_t caseCount : sweptCaseCounts) {
                    for (uint8_t repatchCount : sweptRepatchCounts) {
                        for (uint8_t numberOfCoolDowns : sweptCoolDowns) {
                            ICs::PropertyICRecord record {
                                .accessType = static_cast<uint8_t>(accessType),
                                .learningBits = static_cast<uint8_t>(learningBits),
                                .repatchCount = repatchCount,
                                .numberOfCoolDowns = numberOfCoolDowns,
                                .caseCount = caseCount,
                                .stateBits = static_cast<uint8_t>(stateBits),
                                .reserved = { 0, 0 },
                            };
                            if (!functor(record))
                                return;
                        }
                    }
                }
            }
        }
    }
}

// Calls functor(record) for every well-formed call-link record: every mode and history bit, every varargs maximum.
template<typename Functor>
static void forEachCallLinkRecord(const Functor& functor)
{
    for (unsigned bits = 0; bits <= static_cast<unsigned>(ICs::CallLinkBit::mask); ++bits) {
        for (unsigned maximum = 0; maximum <= static_cast<unsigned>(std::numeric_limits<uint8_t>::max()); ++maximum) {
            ICs::CallLinkRecord record {
                .bits = static_cast<uint8_t>(bits),
                .maxArgumentCountIncludingThisForVarargs = static_cast<uint8_t>(maximum),
            };
            if (!functor(record))
                return;
        }
    }
}

} // namespace ICSectionTestsInternal

using namespace ICSectionTestsInternal;

JITCACHE_TEST(icsSectionSizeFormula, No)
{
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 0) == 16u);
    JITCACHE_CHECK(ICs::sectionSize(0, 1, 0) == 24u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 1) == 24u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 4) == 24u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 5) == 32u);
    JITCACHE_CHECK(ICs::sectionSize(1, 0, 1) == 32u);
    JITCACHE_CHECK(ICs::sectionSize(2, 3, 3) == validSectionSize);
    // SPEC-ics.md section 4.1: 12 property ICs, five call sites and one construct site take 144 bytes.
    JITCACHE_CHECK(ICs::sectionSize(2, 12, 6) == 144u);
    JITCACHE_CHECK(ICs::sectionSize(ICs::numberOfCallLinkOpcodes, 0, ICs::numberOfCallLinkOpcodes) == 160u);
    JITCACHE_CHECK(!ICs::sectionSize(ICs::numberOfCallLinkOpcodes + 1, 0, ICs::numberOfCallLinkOpcodes + 1));
    JITCACHE_CHECK(!ICs::sectionSize(std::numeric_limits<size_t>::max(), 0, 0));

    constexpr size_t maximum = std::numeric_limits<size_t>::max();
    JITCACHE_CHECK(!ICs::sectionSize(0, maximum / 8 + 1, 0));
    JITCACHE_CHECK(!ICs::sectionSize(0, 0, maximum / 2 + 1));
    JITCACHE_CHECK(!ICs::sectionSize(0, maximum / 16, maximum / 4));
    // The largest representable section: 16 + 8 * count is the largest multiple of 8 that fits. One or three call-link
    // records more still fit unrounded but not rounded up.
    constexpr size_t largestPropertyICCount = (maximum - 23) / 8;
    JITCACHE_CHECK(ICs::sectionSize(0, largestPropertyICCount, 0) == maximum - 7);
    JITCACHE_CHECK(!ICs::sectionSize(0, largestPropertyICCount, 1));
    JITCACHE_CHECK(!ICs::sectionSize(0, largestPropertyICCount, 3));
}

JITCACHE_TEST(icsCanonicalCallLinkPositions, No)
{
    for (unsigned position = 0; position < ICs::numberOfCallLinkOpcodes; ++position) {
        if (ICs::canonicalCallLinkPosition(canonicalCallLinkOrder[position]) != position)
            JITCACHE_FAIL(makeString("canonicalCallLinkPosition("_s, opcodeNames[canonicalCallLinkOrder[position]], ") is not "_s, position));
    }
    for (OpcodeID opcodeID : { OpEnter::opcodeID, OpRet::opcodeID, OpGetById::opcodeID, OpAdd::opcodeID, OpInById::opcodeID })
        JITCACHE_CHECK(!ICs::canonicalCallLinkPosition(opcodeID));
    JITCACHE_CHECK(!ICs::canonicalCallLinkPosition(static_cast<OpcodeID>(std::numeric_limits<uint32_t>::max())));

    for (OpcodeID opcodeID : canonicalCallLinkOrder) {
        bool isVarargs = opcodeID == OpCallVarargs::opcodeID || opcodeID == OpTailCallVarargs::opcodeID
            || opcodeID == OpConstructVarargs::opcodeID || opcodeID == OpSuperConstructVarargs::opcodeID;
        if (ICs::isVarargsCallLinkOpcode(opcodeID) != isVarargs)
            JITCACHE_FAIL(makeString("isVarargsCallLinkOpcode("_s, opcodeNames[opcodeID], ") is not "_s, bit(isVarargs)));
    }
    JITCACHE_CHECK(!ICs::isVarargsCallLinkOpcode(OpEnter::opcodeID));
}

JITCACHE_TEST(icsRecordByteLayout, No)
{
    static_assert(!offsetof(ICs::PropertyICRecord, accessType));
    static_assert(offsetof(ICs::PropertyICRecord, learningBits) == 1);
    static_assert(offsetof(ICs::PropertyICRecord, repatchCount) == 2);
    static_assert(offsetof(ICs::PropertyICRecord, numberOfCoolDowns) == 3);
    static_assert(offsetof(ICs::PropertyICRecord, caseCount) == 4);
    static_assert(offsetof(ICs::PropertyICRecord, stateBits) == 5);
    static_assert(offsetof(ICs::PropertyICRecord, reserved) == 6);
    static_assert(!offsetof(ICs::CallLinkRecord, bits));
    static_assert(offsetof(ICs::CallLinkRecord, maxArgumentCountIncludingThisForVarargs) == 1);
    static_assert(ICs::LearningBit::mask == (ICs::LearningBit::everConsidered | ICs::LearningBit::sawNonCell | ICs::LearningBit::tookSlowPath | ICs::LearningBit::resetByGC));
    static_assert(ICs::StateBit::mask == (ICs::StateBit::megamorphicCaseListed | ICs::StateBit::canBeMegamorphic | ICs::StateBit::holdsGaveUp));
    static_assert(ICs::CallLinkBit::mask == (ICs::CallLinkBit::modeMask | ICs::CallLinkBit::seenOnce | ICs::CallLinkBit::hasSeenClosure | ICs::CallLinkBit::clearedByGC | ICs::CallLinkBit::clearedByVirtual));

    // Four property ICs, three of them with cases, and five call-link sites in two groups, with distinct values in every
    // header word and record field, so a field read at the wrong offset shows. 16 + 16 + 32 + 10 bytes, padded to 80.
    TestSection section;
    section.propertyICs.append(propertyIC(AccessType::GetByVal, 0x0f, 0x21, 0x32, 0x43, 0x07));
    section.propertyICs.append(propertyIC(AccessType::PutByIdSloppy, 0x01, 0x02, 0x03, 0x00, 0x04));
    section.propertyICs.append(propertyIC(AccessType::InstanceOf, 0x0a, 0x5b, 0x6c, 0x01, 0x01));
    section.propertyICs.append(propertyIC(AccessType::DeleteByValStrict, 0x05, 0x7d, 0x0e, 0x02, 0x02));
    section.groups.append(ICs::CallLinkGroup { OpCall::opcodeID, 3 });
    section.groups.append(ICs::CallLinkGroup { OpCallVarargs::opcodeID, 2 });
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Monomorphic, ICs::CallLinkBit::seenOnce, 0x11));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Polymorphic, ICs::CallLinkBit::hasSeenClosure | ICs::CallLinkBit::clearedByGC, 0x5a));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, ICs::CallLinkBit::clearedByVirtual, 0x22));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Virtual, ICs::CallLinkBit::seenOnce | ICs::CallLinkBit::clearedByVirtual, 0x33));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0x44));
    section.propertyICCount = 4;
    section.icSitesWithCases = 3;
    section.callLinkGroupCount = 2;
    section.callLinkSiteCount = 5;
    Vector<uint8_t> bytes = encode(section);
    JITCACHE_CHECK(bytes.size() == 80);
    JITCACHE_CHECK(ICs::sectionSize(2, 4, 5) == bytes.size());

    // The section start needs no alignment: parse it at every offset modulo 8.
    for (size_t offset = 0; offset < 8; ++offset) {
        Vector<uint8_t> buffer(FillWith { }, offset, 0);
        buffer.appendVector(bytes);
        std::span<const uint8_t> sectionBytes = buffer.span().subspan(offset);
        auto parsed = parseStrict(sectionBytes);
        if (!parsed) {
            JITCACHE_FAIL(makeString("the layout section at offset "_s, offset, " failed "_s, checkName(parsed.error().check)));
            continue;
        }
        const ICs::SectionView& view = *parsed;
        JITCACHE_CHECK(view.header.propertyICCount == 4);
        JITCACHE_CHECK(view.header.icSitesWithCases == 3);
        JITCACHE_CHECK(view.header.callLinkGroupCount == 2);
        JITCACHE_CHECK(view.header.callLinkSiteCount == 5);
        JITCACHE_CHECK(view.groupBytes.data() == sectionBytes.subspan(16).data() && view.groupBytes.size() == 16);
        JITCACHE_CHECK(static_cast<const void*>(view.propertyICs.data()) == sectionBytes.subspan(32).data() && view.propertyICs.size() == 4);
        JITCACHE_CHECK(static_cast<const void*>(view.callLinks.data()) == sectionBytes.subspan(64).data() && view.callLinks.size() == 5);
        JITCACHE_CHECK(view.group(0).opcodeID == OpCall::opcodeID && view.group(0).siteCount == 3);
        JITCACHE_CHECK(view.group(1).opcodeID == OpCallVarargs::opcodeID && view.group(1).siteCount == 2);
        for (size_t index = 0; index < section.propertyICs.size(); ++index) {
            if (!sameRecord(view.propertyICs[index], section.propertyICs[index]))
                JITCACHE_FAIL(makeString("property-IC record "_s, index, " reads "_s, describe(view.propertyICs[index]), " instead of "_s, describe(section.propertyICs[index])));
        }
        for (size_t index = 0; index < section.callLinks.size(); ++index) {
            if (!sameRecord(view.callLinks[index], section.callLinks[index]))
                JITCACHE_FAIL(makeString("call-link record "_s, index, " reads "_s, describe(view.callLinks[index]), " instead of "_s, describe(section.callLinks[index])));
        }
    }

    // Header and group words are little-endian: counts above 255 read back from their low byte first.
    TestSection wide;
    wide.propertyICs.appendVector(Vector<ICs::PropertyICRecord>(FillWith { }, 0x102, propertyIC(AccessType::GetById, 0, 0, 0, 0, 0)));
    wide.groups.append(ICs::CallLinkGroup { OpCall::opcodeID, 0x103 });
    wide.callLinks.appendVector(Vector<ICs::CallLinkRecord>(FillWith { }, 0x103, callLink(ICs::CallLinkModeCode::Init, 0, 0)));
    wide.propertyICCount = 0x102;
    wide.callLinkGroupCount = 1;
    wide.callLinkSiteCount = 0x103;
    Vector<uint8_t> wideBytes = encode(wide);
    auto wideParsed = parseStrict(wideBytes.span());
    JITCACHE_CHECK(wideParsed && wideParsed->header.propertyICCount == 0x102 && wideParsed->header.callLinkSiteCount == 0x103);
    JITCACHE_CHECK(wideParsed && wideParsed->group(0).siteCount == 0x103 && wideParsed->propertyICs.size() == 0x102 && wideParsed->callLinks.size() == 0x103);

    // SectionView::group reads its eight bytes as two little-endian words.
    const std::array<uint8_t, ICs::callLinkGroupSize> groupWords { 0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05 };
    ICs::SectionView handmade {
        .header = ICs::SectionHeader { 0, 0, 1, 0 },
        .groupBytes = std::span<const uint8_t> { groupWords },
        .propertyICs = { },
        .callLinks = { },
    };
    JITCACHE_CHECK(handmade.group(0).opcodeID == 0x01020304 && handmade.group(0).siteCount == 0x05060708);
}

JITCACHE_TEST(icsParseSectionStrictAndNormalAgree, No)
{
    TestSection withoutCallLinks;
    withoutCallLinks.propertyICs.append(propertyIC(AccessType::InById, ICs::LearningBit::everConsidered, 2, 0, 2, 0));
    withoutCallLinks.propertyICCount = 1;
    withoutCallLinks.icSitesWithCases = 1;

    Vector<Vector<uint8_t>> sections;
    sections.append(encode(validSection()));
    sections.append(encode(withoutCallLinks));
    sections.append(encode(TestSection { }));
    for (auto& bytes : sections) {
        auto strict = ICs::parseSection(bytes.span(), ICs::StrictChecks::Yes);
        auto normal = ICs::parseSection(bytes.span(), ICs::StrictChecks::No);
        if (!strict || !normal) {
            JITCACHE_FAIL(makeString("a valid section of "_s, bytes.size(), " bytes was rejected"_s));
            continue;
        }
        JITCACHE_CHECK(equalSpans(asByteSpan(strict->header), asByteSpan(normal->header)));
        JITCACHE_CHECK(strict->groupBytes.data() == normal->groupBytes.data() && strict->groupBytes.size() == normal->groupBytes.size());
        JITCACHE_CHECK(strict->propertyICs.data() == normal->propertyICs.data() && strict->propertyICs.size() == normal->propertyICs.size());
        JITCACHE_CHECK(strict->callLinks.data() == normal->callLinks.data() && strict->callLinks.size() == normal->callLinks.size());
    }

    auto empty = parseStrict(sections[2].span());
    JITCACHE_CHECK(empty && empty->propertyICs.empty() && empty->callLinks.empty() && empty->groupBytes.empty());
    JITCACHE_CHECK(sections[2].size() == ICs::sectionHeaderSize);
}

// A1. Each input below fails A1 and nothing before it.
JITCACHE_TEST(icsParseSectionSectionSize, No)
{
    Vector<uint8_t> valid = encode(validSection());
    JITCACHE_CHECK(parseStrict(valid.span()).has_value());

    std::span<const uint8_t> none;
    expectInvalid(context, "an empty span"_s, parseStrict(none), ICs::Check::SectionSize);
    expectInvalid(context, "a 15-byte span"_s, parseStrict(valid.span().first(15)), ICs::Check::SectionSize);
    expectInvalid(context, "a section without its padding"_s, parseStrict(valid.span().first(validSectionPaddingOffset)), ICs::Check::SectionSize);

    Vector<uint8_t> trailing = valid;
    trailing.appendVector(Vector<uint8_t>(FillWith { }, 8, 0));
    expectInvalid(context, "eight trailing zero bytes"_s, parseStrict(trailing.span()), ICs::Check::SectionSize);

    expectChangeInvalid(context, "15 call-link groups"_s, ICs::Check::SectionSize, 0, [](TestSection& section) {
        section.callLinkGroupCount = ICs::numberOfCallLinkOpcodes + 1;
    });
    expectChangeInvalid(context, "a header that counts one more property IC"_s, ICs::Check::SectionSize, 0, [](TestSection& section) {
        ++section.propertyICCount;
    });
    expectChangeInvalid(context, "a header that counts no call-link site"_s, ICs::Check::SectionSize, 0, [](TestSection& section) {
        section.callLinkSiteCount = 0;
    });
}

// A2.
JITCACHE_TEST(icsParseSectionSummaryBound, No)
{
    expectChangeInvalid(context, "more IC sites with cases than property ICs"_s, ICs::Check::SummaryBound, 0, [](TestSection& section) {
        section.icSitesWithCases = section.propertyICCount + 1;
    });

    TestSection withoutPropertyICs;
    withoutPropertyICs.icSitesWithCases = 1;
    Vector<uint8_t> bytes = encode(withoutPropertyICs);
    expectInvalid(context, "an IC site with cases and no property IC"_s, parseStrict(bytes.span()), ICs::Check::SummaryBound);
}

// A3.
JITCACHE_TEST(icsParseSectionCallLinkGroups, No)
{
    expectChangeInvalid(context, "a group of an opcode without a CallLinkInfo"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[0].opcodeID = OpEnter::opcodeID;
    });
    expectChangeInvalid(context, "a group of an opcode ID out of range"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[1].opcodeID = std::numeric_limits<uint32_t>::max();
    });
    expectChangeInvalid(context, "groups in decreasing canonical order"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[0] = ICs::CallLinkGroup { OpConstruct::opcodeID, 1 };
        section.groups[1] = ICs::CallLinkGroup { OpCall::opcodeID, 2 };
    });
    expectChangeInvalid(context, "two groups of one opcode"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[1].opcodeID = OpCall::opcodeID;
    });
    expectChangeInvalid(context, "a group without sites"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[0].siteCount = 3;
        section.groups[1].siteCount = 0;
    });
    expectChangeInvalid(context, "group counts that sum past the header's"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[1].siteCount = 2;
    });
    expectChangeInvalid(context, "group counts that sum short of the header's"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[0].siteCount = 1;
    });
    // 0xffffffff + 4 wraps to the header's 3 in 32 bits; the sum must not overflow.
    expectChangeInvalid(context, "group counts whose 32-bit sum wraps to the header's"_s, ICs::Check::CallLinkGroups, 0, [](TestSection& section) {
        section.groups[0].siteCount = std::numeric_limits<uint32_t>::max();
        section.groups[1].siteCount = 4;
    });

    // A group for every canonical opcode, in canonical order, passes.
    TestSection everyOpcode;
    for (OpcodeID opcodeID : canonicalCallLinkOrder) {
        everyOpcode.groups.append(ICs::CallLinkGroup { opcodeID, 1 });
        everyOpcode.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0));
    }
    everyOpcode.callLinkGroupCount = ICs::numberOfCallLinkOpcodes;
    everyOpcode.callLinkSiteCount = ICs::numberOfCallLinkOpcodes;
    JITCACHE_CHECK(parseStrict(encode(everyOpcode).span()).has_value());
}

// A4.
JITCACHE_TEST(icsParseSectionReservedBits, No)
{
    Vector<uint8_t> padding = encode(validSection());
    padding[validSectionPaddingOffset + 1] = 1;
    expectInvalid(context, "a nonzero padding byte"_s, parseStrict(padding.span()), ICs::Check::ReservedBits, 0);

    expectChangeInvalid(context, "a nonzero reserved byte"_s, ICs::Check::ReservedBits, 1, [](TestSection& section) {
        section.propertyICs[1].reserved[1] = 1;
    });
    expectChangeInvalid(context, "a nonzero first reserved byte"_s, ICs::Check::ReservedBits, 2, [](TestSection& section) {
        section.propertyICs[2].reserved[0] = 0x80;
    });
    for (unsigned unusedBit = 4; unusedBit < 8; ++unusedBit) {
        expectChangeInvalid(context, "an unused learning bit"_s, ICs::Check::ReservedBits, 2, [&](TestSection& section) {
            section.propertyICs[2].learningBits |= 1 << unusedBit;
        });
    }
    for (unsigned unusedBit = 3; unusedBit < 8; ++unusedBit) {
        expectChangeInvalid(context, "an unused state bit"_s, ICs::Check::ReservedBits, 0, [&](TestSection& section) {
            section.propertyICs[0].stateBits |= 1 << unusedBit;
        });
    }
    for (unsigned unusedBit = 6; unusedBit < 8; ++unusedBit) {
        expectChangeInvalid(context, "an unused call-link bit"_s, ICs::Check::ReservedBits, 2, [&](TestSection& section) {
            section.callLinks[2].bits |= 1 << unusedBit;
        });
    }
}

// A5.
JITCACHE_TEST(icsParseSectionEnumRange, No)
{
    expectChangeInvalid(context, "the first access type past the last"_s, ICs::Check::EnumRange, 1, [](TestSection& section) {
        section.propertyICs[1].accessType = numberOfAccessTypes;
    });
    expectChangeInvalid(context, "access type 255"_s, ICs::Check::EnumRange, 2, [](TestSection& section) {
        section.propertyICs[2].accessType = std::numeric_limits<uint8_t>::max();
    });

    Vector<uint8_t> lastAccessType = encodeValidSectionWith([](TestSection& section) {
        section.propertyICs[1].accessType = numberOfAccessTypes - 1;
    });
    JITCACHE_CHECK(parseStrict(lastAccessType.span()).has_value());
}

// A6.
JITCACHE_TEST(icsParseSectionSummaryCount, No)
{
    expectChangeInvalid(context, "a summary below the sites with cases"_s, ICs::Check::SummaryCount, 0, [](TestSection& section) {
        section.icSitesWithCases = 1;
    });
    expectChangeInvalid(context, "a summary above the sites with cases"_s, ICs::Check::SummaryCount, 0, [](TestSection& section) {
        section.icSitesWithCases = 3;
    });
    expectChangeInvalid(context, "a site that gained cases"_s, ICs::Check::SummaryCount, 0, [](TestSection& section) {
        section.propertyICs[1].caseCount = 4;
    });
}

JITCACHE_TEST(icsReadBaselineICsSummary, No)
{
    Vector<uint8_t> valid = encode(validSection());
    auto summary = ICs::readBaselineICsSummary(valid.span(), ICs::StrictChecks::Yes);
    JITCACHE_CHECK(summary && summary->icSitesWithCases == 2);
    auto trusted = ICs::readBaselineICsSummary(valid.span(), ICs::StrictChecks::No);
    JITCACHE_CHECK(trusted && trusted->icSitesWithCases == 2);

    auto read = [](std::span<const uint8_t> bytes) {
        return ICs::readBaselineICsSummary(bytes, ICs::StrictChecks::Yes);
    };
    std::span<const uint8_t> none;
    expectInvalid(context, "an empty section"_s, read(none), ICs::Check::SectionSize);
    expectInvalid(context, "a truncated header of 8 bytes"_s, read(valid.span().first(8)), ICs::Check::SectionSize);
    expectInvalid(context, "a truncated header of 15 bytes"_s, read(valid.span().first(15)), ICs::Check::SectionSize);
    expectInvalid(context, "a header alone"_s, read(valid.span().first(ICs::sectionHeaderSize)), ICs::Check::SectionSize);

    Vector<uint8_t> overcounted = encodeValidSectionWith([](TestSection& section) {
        section.propertyICCount += 2;
    });
    expectInvalid(context, "a header that counts more records than the section holds"_s, read(overcounted.span()), ICs::Check::SectionSize);
    Vector<uint8_t> tooManyGroups = encodeValidSectionWith([](TestSection& section) {
        section.callLinkGroupCount = ICs::numberOfCallLinkOpcodes + 1;
    });
    expectInvalid(context, "15 call-link groups"_s, read(tooManyGroups.span()), ICs::Check::SectionSize);
    Vector<uint8_t> unbounded = encodeValidSectionWith([](TestSection& section) {
        section.icSitesWithCases = 4;
    });
    expectInvalid(context, "more IC sites with cases than property ICs"_s, read(unbounded.span()), ICs::Check::SummaryBound);

    // The summary reads the header alone: A3 to A6 are prepare's.
    Vector<uint8_t> reservedByte = encodeValidSectionWith([](TestSection& section) {
        section.propertyICs[0].reserved[0] = 1;
    });
    auto reserved = read(reservedByte.span());
    JITCACHE_CHECK(reserved && reserved->icSitesWithCases == 2);
    Vector<uint8_t> miscountedBytes = encodeValidSectionWith([](TestSection& section) {
        section.icSitesWithCases = 1;
    });
    auto miscounted = read(miscountedBytes.span());
    JITCACHE_CHECK(miscounted && miscounted->icSitesWithCases == 1);
    Vector<uint8_t> bareHeaderBytes = encode(TestSection());
    auto bareHeader = read(bareHeaderBytes.span());
    JITCACHE_CHECK(bareHeader && !bareHeader->icSitesWithCases);
}

JITCACHE_TEST(icsIsPolymorphicPropertyIC, No)
{
    for (unsigned caseCount = 0; caseCount <= 3; ++caseCount) {
        for (unsigned stateBits = 0; stateBits <= ICs::StateBit::mask; ++stateBits) {
            ICs::PropertyICRecord record = propertyIC(AccessType::GetById, ICs::LearningBit::everConsidered, 4, 0, caseCount, stateBits);
            bool megamorphicCaseListed = stateBits & ICs::StateBit::megamorphicCaseListed;
            bool expected = caseCount >= 2 && !megamorphicCaseListed;
            if (ICs::isPolymorphicPropertyIC(record) != expected)
                JITCACHE_FAIL(makeString("isPolymorphicPropertyIC of "_s, describe(record), " is not "_s, bit(expected)));
        }
    }
}

JITCACHE_TEST(icsRestoredPropertyICStates, No)
{
    // The native states of SPEC-ics.md section 5.2 and the states around them, each with its consumer state written out.
    struct NamedState {
        ASCIILiteral name;
        ICs::PropertyICRecord record;
        ICs::RestoredPropertyIC expected;
    };
    using namespace ICs::LearningBit;
    using namespace ICs::StateBit;
    const NamedState states[] = {
        { "a cold IC"_s, propertyIC(AccessType::GetById, 0, 0, 0, 0, 0),
            { .everConsidered = false, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 0, .numberOfCoolDowns = 0 } },
        { "a considered IC with no case"_s, propertyIC(AccessType::PutByIdStrict, everConsidered, 1, 0, 0, 0),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 1, .numberOfCoolDowns = 0 } },
        { "three cases"_s, propertyIC(AccessType::GetById, everConsidered, 3, 0, 3, 0),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 0, .numberOfCoolDowns = 0 } },
        { "(a) a give-up on the eighth case"_s, propertyIC(AccessType::PutByIdDirectStrict, everConsidered, 8, 0, 8, holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 0, .numberOfCoolDowns = 0 } },
        { "(a) a give-up beside two cases, slow path taken"_s, propertyIC(AccessType::GetById, everConsidered | tookSlowPath, 5, 1, 2, holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = true, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 3, .numberOfCoolDowns = 1 } },
        { "(b) a folded instanceof"_s, propertyIC(AccessType::InstanceOf, everConsidered, 9, 1, 1, megamorphicCaseListed | holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = true, .givenUp = false, .countdown = 0, .repatchCount = 8, .numberOfCoolDowns = 1 } },
        { "a folded get"_s, propertyIC(AccessType::GetById, everConsidered, 8, 0, 1, megamorphicCaseListed),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = true, .givenUp = false, .countdown = 0, .repatchCount = 7, .numberOfCoolDowns = 0 } },
        { "(c) a folded get that gave up"_s, propertyIC(AccessType::GetById, everConsidered, 2, 0, 0, holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = true, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = true, .countdown = 0, .repatchCount = 2, .numberOfCoolDowns = 0 } },
        { "(c) a replayed fold that gave up"_s, propertyIC(AccessType::InById, everConsidered, 2, 0, 0, canBeMegamorphic | holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = true, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = true, .countdown = 0, .repatchCount = 2, .numberOfCoolDowns = 0 } },
        { "(d) a fold an import restored"_s, propertyIC(AccessType::PutByValStrict, everConsidered, 4, 0, 0, canBeMegamorphic),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = true, .givenUp = false, .countdown = 0, .repatchCount = 4, .numberOfCoolDowns = 0 } },
        { "an immediate give-up"_s, propertyIC(AccessType::DeleteByIdStrict, everConsidered, 1, 0, 0, holdsGaveUp),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = true, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = true, .countdown = 0, .repatchCount = 1, .numberOfCoolDowns = 0 } },
        { "a non-cell base"_s, propertyIC(AccessType::GetById, sawNonCell, 0, 0, 0, 0),
            { .everConsidered = false, .sawNonCell = true, .tookSlowPath = false, .resetByGC = false, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 0, .numberOfCoolDowns = 0 } },
        { "a reset by GC past a cool-down"_s, propertyIC(AccessType::GetByVal, everConsidered | resetByGC, 3, 2, 0, 0),
            { .everConsidered = true, .sawNonCell = false, .tookSlowPath = false, .resetByGC = true, .foldsAtFirstCase = false, .givenUp = false, .countdown = 0, .repatchCount = 3, .numberOfCoolDowns = 2 } },
    };
    for (auto& state : states) {
        ICs::RestoredPropertyIC restored = ICs::restoredPropertyIC(state.record);
        if (!(restored == state.expected))
            JITCACHE_FAIL(makeString(state.name, ": restores "_s, describe(restored), " instead of "_s, describe(state.expected)));
    }
}

JITCACHE_TEST(icsRestoredPropertyICTable, No)
{
    // Every combination of holdsGaveUp, megamorphicCaseListed, canBeMegamorphic, tookSlowPath and a zero or nonzero
    // caseCount, against the table of section 6.1.
    for (unsigned combination = 0; combination < 1u << 5; ++combination) {
        bool holdsGaveUp = combination & 1;
        bool megamorphicCaseListed = combination & 2;
        bool canBeMegamorphic = combination & 4;
        bool tookSlowPath = combination & 8;
        uint8_t caseCount = (combination & 16) ? 2 : 0;
        uint8_t stateBits = static_cast<uint8_t>((holdsGaveUp ? ICs::StateBit::holdsGaveUp : 0)
            | (megamorphicCaseListed ? ICs::StateBit::megamorphicCaseListed : 0)
            | (canBeMegamorphic ? ICs::StateBit::canBeMegamorphic : 0));
        uint8_t learningBits = static_cast<uint8_t>(ICs::LearningBit::everConsidered | (tookSlowPath ? ICs::LearningBit::tookSlowPath : 0));
        ICs::PropertyICRecord record = propertyIC(AccessType::GetById, learningBits, 6, 3, caseCount, stateBits);
        ICs::RestoredPropertyIC restored = ICs::restoredPropertyIC(record);

        bool givenUp = holdsGaveUp && !caseCount;
        JITCACHE_CHECK(restored.givenUp == givenUp);
        JITCACHE_CHECK(restored.foldsAtFirstCase == ((megamorphicCaseListed || canBeMegamorphic) && !givenUp));
        JITCACHE_CHECK(restored.tookSlowPath == (tookSlowPath || givenUp));
        JITCACHE_CHECK(restored.everConsidered && !restored.sawNonCell && !restored.resetByGC);
        JITCACHE_CHECK(!restored.countdown);
        JITCACHE_CHECK(restored.repatchCount == (caseCount ? 4 : 6));
        JITCACHE_CHECK(restored.numberOfCoolDowns == 3);
        // A restored IC is in exactly one of three states: cold or considered, folded, or given up.
        JITCACHE_CHECK(!(restored.givenUp && restored.foldsAtFirstCase));
        if (context.failed()) {
            JITCACHE_FAIL(makeString("the failing record is "_s, describe(record)));
            return;
        }
    }

    // The whole sweep against the table restated from the SPEC.
    forEachSweptPropertyIC([&](const ICs::PropertyICRecord& record) {
        ICs::RestoredPropertyIC restored = ICs::restoredPropertyIC(record);
        ICs::RestoredPropertyIC expected = expectedRestoredPropertyIC(record);
        if (restored == expected)
            return true;
        JITCACHE_FAIL(makeString(describe(record), " restores "_s, describe(restored), " instead of "_s, describe(expected)));
        return false;
    });
}

JITCACHE_TEST(icsRestoredPropertyICRepatchCount, No)
{
    // repatchCount - min(repatchCount, caseCount), including caseCount > repatchCount.
    for (uint8_t repatchCount : sweptRepatchCounts) {
        for (uint8_t caseCount : sweptCaseCounts) {
            ICs::PropertyICRecord record = propertyIC(AccessType::GetByVal, ICs::LearningBit::everConsidered, repatchCount, 0, caseCount, 0);
            unsigned expected = caseCount > repatchCount ? 0 : repatchCount - caseCount;
            if (static_cast<unsigned>(ICs::restoredPropertyIC(record).repatchCount) != expected)
                JITCACHE_FAIL(makeString("repatchCount "_s, static_cast<unsigned>(repatchCount), " with "_s, static_cast<unsigned>(caseCount), " cases does not rewind to "_s, expected));
        }
    }
    JITCACHE_CHECK(!ICs::restoredPropertyIC(propertyIC(AccessType::GetById, ICs::LearningBit::everConsidered, 2, 0, 8, ICs::StateBit::holdsGaveUp)).repatchCount);
}

JITCACHE_TEST(icsRestoredCallLinkTable, No)
{
    using namespace ICs::CallLinkBit;
    struct NamedState {
        ASCIILiteral name;
        ICs::CallLinkRecord record;
        ICs::RestoredCallLink expected;
    };
    const NamedState states[] = {
        { "a site never reached"_s, callLink(ICs::CallLinkModeCode::Init, 0, 0),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a site called once"_s, callLink(ICs::CallLinkModeCode::Init, seenOnce, 0),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a monomorphic site"_s, callLink(ICs::CallLinkModeCode::Monomorphic, seenOnce, 0),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a polymorphic site of closures"_s, callLink(ICs::CallLinkModeCode::Polymorphic, seenOnce | hasSeenClosure, 0),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = true, .clearedByGC = false, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a site whose callee died"_s, callLink(ICs::CallLinkModeCode::Init, clearedByGC, 0),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = false, .clearedByGC = true, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a virtual construct site"_s, callLink(ICs::CallLinkModeCode::Virtual, clearedByVirtual, 0),
            { .isVirtual = true, .seenOnce = false, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = true, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a virtual site without its history bit"_s, callLink(ICs::CallLinkModeCode::Virtual, seenOnce, 0),
            { .isVirtual = true, .seenOnce = false, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = true, .maxArgumentCountIncludingThisForVarargs = 0 } },
        { "a varargs site"_s, callLink(ICs::CallLinkModeCode::Monomorphic, seenOnce, 7),
            { .isVirtual = false, .seenOnce = true, .hasSeenClosure = false, .clearedByGC = false, .clearedByVirtual = false, .maxArgumentCountIncludingThisForVarargs = 7 } },
    };
    for (auto& state : states) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(state.record);
        if (!(restored == state.expected))
            JITCACHE_FAIL(makeString(state.name, ": restores "_s, describe(restored), " instead of "_s, describe(state.expected)));
    }

    forEachCallLinkRecord([&](const ICs::CallLinkRecord& record) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(record);
        ICs::RestoredCallLink expected = expectedRestoredCallLink(record);
        if (restored == expected)
            return true;
        JITCACHE_FAIL(makeString(describe(record), " restores "_s, describe(restored), " instead of "_s, describe(expected)));
        return false;
    });
}

JITCACHE_TEST(icsPropertyICRoundTrip, No)
{
    forEachSweptPropertyIC([&](const ICs::PropertyICRecord& record) {
        ICs::RestoredPropertyIC restored = ICs::restoredPropertyIC(record);
        ICs::PropertyICRecord recaptured = ICs::recapturedPropertyIC(record);
        ICs::RestoredPropertyIC restoredAgain = ICs::restoredPropertyIC(recaptured);
        if (!(restoredAgain == restored)) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured), ", which restores "_s, describe(restoredAgain), " instead of "_s, describe(restored)));
            return false;
        }

        // A capture of the restored state lists no case, keeps the identity and cool-down count, and holds exactly the
        // fold and give-up facts the restored state carries.
        uint8_t expectedStateBits = static_cast<uint8_t>((restored.givenUp ? ICs::StateBit::holdsGaveUp : 0) | (restored.foldsAtFirstCase ? ICs::StateBit::canBeMegamorphic : 0));
        bool shapeHolds = recaptured.accessType == record.accessType
            && recaptured.numberOfCoolDowns == record.numberOfCoolDowns
            && !recaptured.caseCount
            && recaptured.stateBits == expectedStateBits
            && recaptured.repatchCount == restored.repatchCount
            && !(recaptured.learningBits & ~ICs::LearningBit::mask)
            && !recaptured.reserved[0] && !recaptured.reserved[1];
        if (!shapeHolds) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured)));
            return false;
        }

        // The round trip is stable across generations of recaptures.
        ICs::PropertyICRecord recapturedAgain = ICs::recapturedPropertyIC(recaptured);
        if (!sameRecord(recapturedAgain, recaptured)) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured), " and then as "_s, describe(recapturedAgain)));
            return false;
        }
        return true;
    });

    // A section of recaptured records is well formed, with no IC site with cases.
    TestSection recapturedSection = validSection();
    for (auto& record : recapturedSection.propertyICs)
        record = ICs::recapturedPropertyIC(record);
    for (auto& record : recapturedSection.callLinks)
        record = ICs::recapturedCallLink(record);
    recapturedSection.icSitesWithCases = 0;
    JITCACHE_CHECK(parseStrict(encode(recapturedSection).span()).has_value());
}

JITCACHE_TEST(icsCallLinkRoundTrip, No)
{
    forEachCallLinkRecord([&](const ICs::CallLinkRecord& record) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(record);
        ICs::CallLinkRecord recaptured = ICs::recapturedCallLink(record);
        ICs::RestoredCallLink restoredAgain = ICs::restoredCallLink(recaptured);
        if (!(restoredAgain == restored)) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured), ", which restores "_s, describe(restoredAgain), " instead of "_s, describe(restored)));
            return false;
        }

        // Seeding leaves a site in Init or, through setVirtualCall, Virtual.
        auto mode = static_cast<ICs::CallLinkModeCode>(recaptured.bits & ICs::CallLinkBit::modeMask);
        bool shapeHolds = mode == (restored.isVirtual ? ICs::CallLinkModeCode::Virtual : ICs::CallLinkModeCode::Init)
            && !(recaptured.bits & ~ICs::CallLinkBit::mask)
            && recaptured.maxArgumentCountIncludingThisForVarargs == record.maxArgumentCountIncludingThisForVarargs;
        if (!shapeHolds) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured)));
            return false;
        }

        ICs::CallLinkRecord recapturedAgain = ICs::recapturedCallLink(recaptured);
        if (!sameRecord(recapturedAgain, recaptured)) {
            JITCACHE_FAIL(makeString(describe(record), " recaptures as "_s, describe(recaptured), " and then as "_s, describe(recapturedAgain)));
            return false;
        }
        return true;
    });
}

// A7. The valid section pairs with its own molds. Each mold list below fails A7 and only A7: the section passes A1 to
// A6, its layout matches the site counts (A8) and no mold carries the fold bit (S1).
JITCACHE_TEST(icsRestoreCheckMoldPairing, No)
{
    Vector<uint8_t> bytes = encode(validSection());
    auto view = parseStrict(bytes.span());
    if (!view) {
        JITCACHE_FAIL(makeString("the valid section failed "_s, checkName(view.error().check)));
        return;
    }
    Vector<BaselineUnlinkedPropertyInlineCache> molds = validSectionMolds();
    expectCheckPasses(context, "the valid section with its molds"_s, ICs::checkMoldPairing(*view, molds.span()));
    expectCheckPasses(context, "the valid section with its site counts"_s, ICs::checkMetadataLayout(*view, validSectionSiteCounts()));

    auto expectOnlyMoldPairingFails = [&](ASCIILiteral what, const Vector<BaselineUnlinkedPropertyInlineCache>& changed, uint32_t siteIndex) {
        expectCheckFails(context, what, ICs::checkMoldPairing(*view, changed.span()), ICs::Check::MoldPairing, siteIndex);
        expectCheckPasses(context, what, ICs::checkMolds(changed.span()));
    };

    Vector<BaselineUnlinkedPropertyInlineCache> fewer = molds;
    fewer.removeLast();
    expectOnlyMoldPairingFails("one mold fewer than records"_s, fewer, 0);

    Vector<BaselineUnlinkedPropertyInlineCache> more = molds;
    more.append(mold(AccessType::GetById));
    expectOnlyMoldPairingFails("one mold more than records"_s, more, 0);

    expectOnlyMoldPairingFails("no mold for three records"_s, { }, 0);

    // A record stands for the access type of its own mold, so a mold list in another order fails at the first index
    // whose access type differs.
    Vector<BaselineUnlinkedPropertyInlineCache> sloppy = molds;
    sloppy[1].accessType = AccessType::PutByIdSloppy;
    expectOnlyMoldPairingFails("the second mold of the sloppy put"_s, sloppy, 1);

    Vector<BaselineUnlinkedPropertyInlineCache> lastDiffers = molds;
    lastDiffers[2].accessType = AccessType::InById;
    expectOnlyMoldPairingFails("the third mold of another access type"_s, lastDiffers, 2);

    Vector<BaselineUnlinkedPropertyInlineCache> rotated;
    rotated.append(mold(AccessType::PutByIdStrict));
    rotated.append(mold(AccessType::InstanceOf));
    rotated.append(mold(AccessType::GetById));
    expectOnlyMoldPairingFails("the molds rotated by one"_s, rotated, 0);

    // A section without property ICs pairs only with code without molds.
    Vector<uint8_t> emptyBytes = encode(TestSection { });
    auto emptyView = parseStrict(emptyBytes.span());
    if (!emptyView) {
        JITCACHE_FAIL(makeString("the empty section failed "_s, checkName(emptyView.error().check)));
        return;
    }
    expectCheckPasses(context, "no record and no mold"_s, ICs::checkMoldPairing(*emptyView, { }));
    Vector<BaselineUnlinkedPropertyInlineCache> oneMold;
    oneMold.append(mold(AccessType::InById));
    expectCheckFails(context, "no record and one mold"_s, ICs::checkMoldPairing(*emptyView, oneMold.span()), ICs::Check::MoldPairing, 0);

    // Both ends of the access-type range pair with themselves.
    TestSection extremes;
    extremes.propertyICs.append(propertyIC(static_cast<AccessType>(0), 0, 0, 0, 0, 0));
    extremes.propertyICs.append(propertyIC(static_cast<AccessType>(numberOfAccessTypes - 1), 0, 0, 0, 0, 0));
    extremes.propertyICCount = 2;
    Vector<uint8_t> extremesBytes = encode(extremes);
    auto extremesView = parseStrict(extremesBytes.span());
    if (!extremesView) {
        JITCACHE_FAIL(makeString("the extreme access types failed "_s, checkName(extremesView.error().check)));
        return;
    }
    Vector<BaselineUnlinkedPropertyInlineCache> extremeMolds;
    extremeMolds.append(mold(static_cast<AccessType>(0)));
    extremeMolds.append(mold(static_cast<AccessType>(numberOfAccessTypes - 1)));
    expectCheckPasses(context, "the extreme access types"_s, ICs::checkMoldPairing(*extremesView, extremeMolds.span()));
    std::swap(extremeMolds[0], extremeMolds[1]);
    expectCheckFails(context, "the extreme access types swapped"_s, ICs::checkMoldPairing(*extremesView, extremeMolds.span()), ICs::Check::MoldPairing, 0);
}

// A8. The valid section matches a CB with two call sites and one construct site. Each count list below fails A8 and
// only A8: the section passes A1 to A6 and pairs with its molds (A7), which carry no fold bit (S1).
JITCACHE_TEST(icsRestoreCheckMetadataLayout, No)
{
    Vector<uint8_t> bytes = encode(validSection());
    auto view = parseStrict(bytes.span());
    if (!view) {
        JITCACHE_FAIL(makeString("the valid section failed "_s, checkName(view.error().check)));
        return;
    }
    Vector<BaselineUnlinkedPropertyInlineCache> molds = validSectionMolds();
    expectCheckPasses(context, "the valid section with its molds"_s, ICs::checkMoldPairing(*view, molds.span()));
    expectCheckPasses(context, "the valid section's molds"_s, ICs::checkMolds(molds.span()));
    expectCheckPasses(context, "the valid section with its site counts"_s, ICs::checkMetadataLayout(*view, validSectionSiteCounts()));

    const unsigned callPosition = callLinkPosition(OpCall::opcodeID);
    const unsigned constructPosition = callLinkPosition(OpConstruct::opcodeID);
    auto expectLayoutFails = [&](ASCIILiteral what, const auto& change) {
        ICs::CallLinkSiteCounts counts = validSectionSiteCounts();
        change(counts);
        expectCheckFails(context, what, ICs::checkMetadataLayout(*view, counts), ICs::Check::MetadataLayout, 0);
    };
    expectLayoutFails("one call site fewer"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[callPosition] = 1;
    });
    expectLayoutFails("one call site more"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[callPosition] = 3;
    });
    expectLayoutFails("no construct site"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[constructPosition] = 0;
    });
    expectLayoutFails("a tail call the section has no group for"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[callLinkPosition(OpTailCall::opcodeID)] = 1;
    });
    // The same three sites under other opcodes: the totals agree, the opcodes do not.
    expectLayoutFails("the counts of the two opcodes swapped"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[callPosition] = 1;
        counts[constructPosition] = 2;
    });
    expectLayoutFails("the call sites counted as call_ignore_result"_s, [&](ICs::CallLinkSiteCounts& counts) {
        counts[callPosition] = 0;
        counts[callLinkPosition(OpCallIgnoreResult::opcodeID)] = 2;
    });
    expectLayoutFails("a CB without a metadata table"_s, [](ICs::CallLinkSiteCounts& counts) {
        counts = { };
    });

    // A section without groups matches only a CB without call-link sites.
    Vector<uint8_t> emptyBytes = encode(TestSection { });
    auto emptyView = parseStrict(emptyBytes.span());
    if (!emptyView) {
        JITCACHE_FAIL(makeString("the empty section failed "_s, checkName(emptyView.error().check)));
        return;
    }
    expectCheckPasses(context, "no group and no site"_s, ICs::checkMetadataLayout(*emptyView, ICs::CallLinkSiteCounts { }));
    ICs::CallLinkSiteCounts oneSite { };
    oneSite[callLinkPosition(OpCallVarargs::opcodeID)] = 1;
    expectCheckFails(context, "no group and one varargs site"_s, ICs::checkMetadataLayout(*emptyView, oneSite), ICs::Check::MetadataLayout, 0);

    // A group for every canonical opcode matches one site of each.
    TestSection everyOpcode;
    ICs::CallLinkSiteCounts everyCount { };
    for (OpcodeID opcodeID : canonicalCallLinkOrder) {
        everyOpcode.groups.append(ICs::CallLinkGroup { opcodeID, 1 });
        everyOpcode.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0));
        everyCount[callLinkPosition(opcodeID)] = 1;
    }
    everyOpcode.callLinkGroupCount = ICs::numberOfCallLinkOpcodes;
    everyOpcode.callLinkSiteCount = ICs::numberOfCallLinkOpcodes;
    Vector<uint8_t> everyBytes = encode(everyOpcode);
    auto everyView = parseStrict(everyBytes.span());
    if (!everyView) {
        JITCACHE_FAIL(makeString("the section with every opcode failed "_s, checkName(everyView.error().check)));
        return;
    }
    expectCheckPasses(context, "one site of every opcode"_s, ICs::checkMetadataLayout(*everyView, everyCount));
    everyCount[callLinkPosition(OpCallIgnoreResult::opcodeID)] = 2;
    expectCheckFails(context, "one call_ignore_result site more"_s, ICs::checkMetadataLayout(*everyView, everyCount), ICs::Check::MetadataLayout, 0);
}

// S1. Molds as baseline emission leaves them carry no fold bit. Each mold list below fails S1 and only S1: it still
// pairs with the valid section (A7), whose layout matches the site counts (A8).
JITCACHE_TEST(icsRestoreCheckMolds, No)
{
    expectCheckPasses(context, "no mold"_s, ICs::checkMolds({ }));
    Vector<BaselineUnlinkedPropertyInlineCache> molds = validSectionMolds();
    expectCheckPasses(context, "the valid section's molds"_s, ICs::checkMolds(molds.span()));

    Vector<uint8_t> bytes = encode(validSection());
    auto view = parseStrict(bytes.span());
    if (!view) {
        JITCACHE_FAIL(makeString("the valid section failed "_s, checkName(view.error().check)));
        return;
    }
    expectCheckPasses(context, "the valid section with its site counts"_s, ICs::checkMetadataLayout(*view, validSectionSiteCounts()));

    auto expectOnlyMoldsFail = [&](ASCIILiteral what, const Vector<BaselineUnlinkedPropertyInlineCache>& changed, uint32_t siteIndex) {
        expectCheckFails(context, what, ICs::checkMolds(changed.span()), ICs::Check::MoldMegamorphicBit, siteIndex);
        expectCheckPasses(context, what, ICs::checkMoldPairing(*view, changed.span()));
    };

    Vector<BaselineUnlinkedPropertyInlineCache> second = molds;
    second[1].canBeMegamorphic = true;
    expectOnlyMoldsFail("the second mold with the fold bit"_s, second, 1);

    Vector<BaselineUnlinkedPropertyInlineCache> last = molds;
    last[2].canBeMegamorphic = true;
    expectOnlyMoldsFail("the last mold with the fold bit"_s, last, 2);

    // The first failure is the one reported.
    Vector<BaselineUnlinkedPropertyInlineCache> firstAndLast = molds;
    firstAndLast[0].canBeMegamorphic = true;
    firstAndLast[2].canBeMegamorphic = true;
    expectOnlyMoldsFail("the first and last molds with the fold bit"_s, firstAndLast, 0);

    // Only the fold bit matters: the other identity bits are the molds' own.
    Vector<BaselineUnlinkedPropertyInlineCache> otherBits = molds;
    for (auto& changed : otherBits) {
        changed.propertyIsInt32 = true;
        changed.propertyIsString = true;
        changed.propertyIsSymbol = true;
        changed.prototypeIsKnownObject = true;
    }
    expectCheckPasses(context, "molds with every other identity bit"_s, ICs::checkMolds(otherBits.span()));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
