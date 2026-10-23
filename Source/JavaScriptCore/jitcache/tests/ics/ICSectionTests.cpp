#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayProfile.h"
#include "BytecodeStructs.h"
#include "IndexingType.h"
#include "PropertyInlineCache.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <array>
#include <cstddef>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of ICSection (ics/ICSection.h and .cpp): the ICsBaseline format and its parsing, the opcode table, the state a
// consumer's IC and call site derive from each record, and the round trip. None of these tests needs a VM.

namespace JSC::JITCache::Tests {

namespace ICSectionTestsInternal {

// A section as the ICsBaseline format lays it out. encode() writes the header words as they stand, so a test can make
// them disagree with the arrays.
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
        for (uint8_t byte : record.thisArrayModes)
            bytes.append(byte);
        for (uint8_t byte : record.thisArrayProfileFlags)
            bytes.append(byte);
    }
    while (bytes.size() % 8)
        bytes.append(0);
    return bytes;
}

// A this word as the format stores it, least significant byte first, written without the lane's helpers.
static void storeLittleEndian(uint8_t (&bytes)[4], uint32_t value)
{
    unsigned shift = 0;
    for (uint8_t& byte : bytes) {
        byte = static_cast<uint8_t>(value >> shift);
        shift += 8;
    }
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

static ICs::CallLinkRecord callLink(ICs::CallLinkModeCode mode, uint8_t historyBits, uint8_t maxArgumentCountIncludingThisForVarargs, uint32_t thisArrayModes = 0, uint32_t thisArrayProfileFlags = 0)
{
    ICs::CallLinkRecord record {
        .bits = static_cast<uint8_t>(static_cast<uint8_t>(mode) | historyBits),
        .maxArgumentCountIncludingThisForVarargs = maxArgumentCountIncludingThisForVarargs,
        .thisArrayModes = { },
        .thisArrayProfileFlags = { },
    };
    storeLittleEndian(record.thisArrayModes, thisArrayModes);
    storeLittleEndian(record.thisArrayProfileFlags, thisArrayProfileFlags);
    return record;
}

// Modes and flags an array profile can hold.
static constexpr uint32_t int32ArrayModes = asArrayModesIgnoringTypedArrays(ArrayWithInt32);
static constexpr uint32_t outOfBoundsFlag = static_cast<uint32_t>(ArrayProfileFlag::OutOfBounds);
static constexpr uint32_t pruningFlag = static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

// Three property ICs (two with cases), and three call-link sites in two groups, the first of them with a this profile:
// 16 + 16 + 24 + 30 bytes, padded with two zero bytes to 88.
static constexpr size_t validSectionSize = 88;

static TestSection validSection()
{
    TestSection section;
    section.propertyICs.append(propertyIC(AccessType::GetById, ICs::LearningBit::everConsidered | ICs::LearningBit::tookSlowPath, 3, 1, 2, 0));
    section.propertyICs.append(propertyIC(AccessType::PutByIdStrict, 0, 0, 0, 0, 0));
    section.propertyICs.append(propertyIC(AccessType::InstanceOf, ICs::LearningBit::everConsidered, 9, 2, 1, ICs::StateBit::megamorphicCaseListed | ICs::StateBit::holdsGaveUp));
    section.groups.append(ICs::CallLinkGroup { OpCall::opcodeID, 2 });
    section.groups.append(ICs::CallLinkGroup { OpConstruct::opcodeID, 1 });
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Monomorphic, ICs::CallLinkBit::seenOnce, 0, int32ArrayModes, pruningFlag));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Virtual, ICs::CallLinkBit::clearedByVirtual, 0));
    section.propertyICCount = static_cast<uint32_t>(section.propertyICs.size());
    section.icSitesWithCases = 2;
    section.callLinkGroupCount = static_cast<uint32_t>(section.groups.size());
    section.callLinkSiteCount = static_cast<uint32_t>(section.callLinks.size());
    return section;
}

// The canonical call-link order, as the format lists the call opcodes, with each opcode's site shape and whether its
// site's CallSiteData holds a this profile, written out apart from callLinkOpcodes.
struct ExpectedCallLinkOpcode {
    OpcodeID opcodeID;
    ICs::CallLinkSiteKind kind;
    bool hasThisProfile;
};
static constexpr std::array<ExpectedCallLinkOpcode, ICs::numberOfCallLinkOpcodes> expectedCallLinkOpcodes { {
    { OpCall::opcodeID, ICs::CallLinkSiteKind::Lazy, true },
    { OpTailCall::opcodeID, ICs::CallLinkSiteKind::LazyTailCall, true },
    { OpCallDirectEval::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpConstruct::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpSuperConstruct::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpIteratorOpen::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpIteratorNext::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpAsyncIteratorOpen::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpAsyncIteratorNext::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpCallVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false },
    { OpTailCallVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false },
    { OpConstructVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false },
    { OpSuperConstructVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false },
    { OpCallIgnoreResult::opcodeID, ICs::CallLinkSiteKind::Lazy, true },
} };

// The opcodes the derivation tests take: one of each shape, with and without a this profile. An inline opcode with a this
// profile does not exist, but the derivation reads only the two properties, and the tests cover every pair.
static constexpr std::array<ICs::CallLinkOpcode, 6> derivationOpcodes { {
    { OpCallVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false },
    { OpCallVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, true },
    { OpConstruct::opcodeID, ICs::CallLinkSiteKind::Lazy, false },
    { OpCall::opcodeID, ICs::CallLinkSiteKind::Lazy, true },
    { OpTailCall::opcodeID, ICs::CallLinkSiteKind::LazyTailCall, false },
    { OpTailCall::opcodeID, ICs::CallLinkSiteKind::LazyTailCall, true },
} };

template<typename Functor>
static Vector<uint8_t> encodeValidSectionWith(const Functor& change)
{
    TestSection section = validSection();
    change(section);
    return encode(section);
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
    return makeString("{bits "_s, static_cast<unsigned>(record.bits), ", maxArgumentCountIncludingThisForVarargs "_s, static_cast<unsigned>(record.maxArgumentCountIncludingThisForVarargs),
        ", thisArrayModes "_s, ICs::readThisArrayModes(record), ", thisArrayProfileFlags "_s, ICs::readThisArrayProfileFlags(record), '}');
}

static ASCIILiteral kindName(ICs::CallLinkSiteKind kind)
{
    switch (kind) {
    case ICs::CallLinkSiteKind::Inline:
        return "Inline"_s;
    case ICs::CallLinkSiteKind::Lazy:
        return "Lazy"_s;
    case ICs::CallLinkSiteKind::LazyTailCall:
        return "LazyTailCall"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral dataName(ICs::CallSiteDataState data)
{
    switch (data) {
    case ICs::CallSiteDataState::Inline:
        return "Inline"_s;
    case ICs::CallSiteDataState::NotExecuted:
        return "NotExecuted"_s;
    case ICs::CallSiteDataState::ExecutedOnce:
        return "ExecutedOnce"_s;
    case ICs::CallSiteDataState::Own:
        return "Own"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String describe(const ICs::CallLinkOpcode& opcode)
{
    return makeString(opcodeNames[opcode.opcodeID], " ("_s, kindName(opcode.kind), opcode.hasThisProfile ? ", with a this profile)"_s : ")"_s);
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
    return makeString("{data "_s, dataName(state.data), ", isVirtual "_s, bit(state.isVirtual), ", seenOnce "_s, bit(state.seenOnce), ", hasSeenClosure "_s, bit(state.hasSeenClosure),
        ", clearedByGC "_s, bit(state.clearedByGC), ", clearedByVirtual "_s, bit(state.clearedByVirtual),
        ", maxArgumentCountIncludingThisForVarargs "_s, static_cast<unsigned>(state.maxArgumentCountIncludingThisForVarargs),
        ", thisArrayModes "_s, state.thisArrayModes, ", thisArrayProfileFlags "_s, state.thisArrayProfileFlags, '}');
}

static bool sameRecord(const ICs::PropertyICRecord& a, const ICs::PropertyICRecord& b)
{
    return equalSpans(asByteSpan(a), asByteSpan(b));
}

static bool sameRecord(const ICs::CallLinkRecord& a, const ICs::CallLinkRecord& b)
{
    return equalSpans(asByteSpan(a), asByteSpan(b));
}

// The state a consumer's property IC takes from a record, restated here independently of ICSection.
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

// A this word of a record, read byte by byte without the lane's helpers.
static uint32_t loadLittleEndian(const uint8_t (&bytes)[4])
{
    uint32_t value = 0;
    unsigned shift = 0;
    for (uint8_t byte : bytes) {
        value |= static_cast<uint32_t>(byte) << shift;
        shift += 8;
    }
    return value;
}

// The state a consumer's call site takes from a record at a site of the opcode, restated here independently of
// ICSection: a record holds something when it has what only a site's own data can hold, and is empty when it holds
// nothing and the site neither linked nor was seen.
static ICs::RestoredCallLink expectedRestoredCallLink(const ICs::CallLinkOpcode& opcode, const ICs::CallLinkRecord& record)
{
    uint8_t mode = record.bits & ICs::CallLinkBit::modeMask;
    bool isVirtual = mode == static_cast<uint8_t>(ICs::CallLinkModeCode::Virtual);
    uint32_t thisArrayModes = opcode.hasThisProfile ? loadLittleEndian(record.thisArrayModes) : 0;
    uint32_t thisArrayProfileFlags = opcode.hasThisProfile ? loadLittleEndian(record.thisArrayProfileFlags) : 0;
    uint8_t historyBits = ICs::CallLinkBit::hasSeenClosure | ICs::CallLinkBit::clearedByGC | ICs::CallLinkBit::clearedByVirtual;
    bool holding = isVirtual || (record.bits & historyBits) || record.maxArgumentCountIncludingThisForVarargs || thisArrayModes || thisArrayProfileFlags;
    bool empty = !holding && mode == static_cast<uint8_t>(ICs::CallLinkModeCode::Init) && !(record.bits & ICs::CallLinkBit::seenOnce);

    auto data = ICs::CallSiteDataState::Inline;
    switch (opcode.kind) {
    case ICs::CallLinkSiteKind::Inline:
        break;
    case ICs::CallLinkSiteKind::Lazy:
        data = empty ? ICs::CallSiteDataState::NotExecuted : holding ? ICs::CallSiteDataState::Own : ICs::CallSiteDataState::ExecutedOnce;
        break;
    case ICs::CallLinkSiteKind::LazyTailCall:
        data = empty ? ICs::CallSiteDataState::NotExecuted : ICs::CallSiteDataState::Own;
        break;
    }
    return ICs::RestoredCallLink {
        .data = data,
        .isVirtual = isVirtual,
        .seenOnce = !empty && !isVirtual,
        .hasSeenClosure = static_cast<bool>(record.bits & ICs::CallLinkBit::hasSeenClosure),
        .clearedByGC = static_cast<bool>(record.bits & ICs::CallLinkBit::clearedByGC),
        .clearedByVirtual = (record.bits & ICs::CallLinkBit::clearedByVirtual) || isVirtual,
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
        .thisArrayModes = thisArrayModes,
        .thisArrayProfileFlags = thisArrayProfileFlags,
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

// Calls functor(opcode, record) for each derivation opcode and every call-link record the derivation tests sweep: every
// value of the six call-link bits, a zero and a nonzero varargs maximum, and this fields that are zero, hold modes alone
// or hold flags alone. The this fields are swept at the opcodes without a this profile too, where the derivation
// ignores them.
template<typename Functor>
static void forEachCallLinkRecord(const Functor& functor)
{
    struct ThisProfile {
        uint32_t modes;
        uint32_t flags;
    };
    constexpr std::array<ThisProfile, 3> thisProfiles { { { 0, 0 }, { int32ArrayModes, 0 }, { 0, outOfBoundsFlag } } };
    constexpr std::array<uint8_t, 3> maxima { 0, 1, 255 };
    for (const ICs::CallLinkOpcode& opcode : derivationOpcodes) {
        for (unsigned bits = 0; bits <= static_cast<unsigned>(ICs::CallLinkBit::mask); ++bits) {
            for (uint8_t maximum : maxima) {
                for (const ThisProfile& thisProfile : thisProfiles) {
                    ICs::CallLinkRecord record = callLink(ICs::CallLinkModeCode::Init, static_cast<uint8_t>(bits), maximum, thisProfile.modes, thisProfile.flags);
                    if (!functor(opcode, record))
                        return;
                }
            }
        }
    }
}

} // namespace ICSectionTestsInternal

using namespace ICSectionTestsInternal;

JITCACHE_TEST(icsSectionSizeFormula, No)
{
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 0) == 16u);
    JITCACHE_CHECK(ICs::sectionSize(0, 1, 0) == 24u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 1) == 32u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 4) == 56u);
    JITCACHE_CHECK(ICs::sectionSize(0, 0, 5) == 72u);
    JITCACHE_CHECK(ICs::sectionSize(1, 0, 1) == 40u);
    JITCACHE_CHECK(ICs::sectionSize(2, 3, 3) == validSectionSize);
    // 12 property ICs, five call sites and one construct site take 192 bytes.
    JITCACHE_CHECK(ICs::sectionSize(2, 12, 6) == 192u);
    JITCACHE_CHECK(ICs::sectionSize(ICs::numberOfCallLinkOpcodes, 0, ICs::numberOfCallLinkOpcodes) == 272u);
    JITCACHE_CHECK(!ICs::sectionSize(ICs::numberOfCallLinkOpcodes + 1, 0, ICs::numberOfCallLinkOpcodes + 1));
    JITCACHE_CHECK(!ICs::sectionSize(std::numeric_limits<size_t>::max(), 0, 0));

    constexpr size_t maximum = std::numeric_limits<size_t>::max();
    JITCACHE_CHECK(!ICs::sectionSize(0, maximum / 8 + 1, 0));
    JITCACHE_CHECK(!ICs::sectionSize(0, 0, maximum / 10 + 1));
    JITCACHE_CHECK(!ICs::sectionSize(0, maximum / 16, maximum / 16));
    // The largest representable section: 16 + 8 * count is the largest multiple of 8 that fits.
    constexpr size_t largestPropertyICCount = (maximum - 23) / 8;
    JITCACHE_CHECK(ICs::sectionSize(0, largestPropertyICCount, 0) == maximum - 7);
    JITCACHE_CHECK(!ICs::sectionSize(0, largestPropertyICCount, 1));
    // One call-link record more than these property ICs, 16 + 8 * count + 10 = maximum - 5, fits unrounded but not
    // rounded up.
    constexpr size_t propertyICCountBelowTheLimit = (maximum - 31) / 8;
    JITCACHE_CHECK(ICs::sectionSize(0, propertyICCountBelowTheLimit, 0) == maximum - 15);
    JITCACHE_CHECK(!ICs::sectionSize(0, propertyICCountBelowTheLimit, 1));
}

JITCACHE_TEST(icsCanonicalCallLinkPositions, No)
{
    for (unsigned position = 0; position < ICs::numberOfCallLinkOpcodes; ++position) {
        const ExpectedCallLinkOpcode& expected = expectedCallLinkOpcodes[position];
        if (ICs::canonicalCallLinkPosition(expected.opcodeID) != position)
            JITCACHE_FAIL(makeString("canonicalCallLinkPosition("_s, opcodeNames[expected.opcodeID], ") is not "_s, position));
        const ICs::CallLinkOpcode& actual = ICs::callLinkOpcodes[position];
        if (actual.opcodeID != expected.opcodeID)
            JITCACHE_FAIL(makeString("callLinkOpcodes["_s, position, "] is "_s, opcodeNames[actual.opcodeID], " instead of "_s, opcodeNames[expected.opcodeID]));
        if (actual.kind != expected.kind)
            JITCACHE_FAIL(makeString(opcodeNames[expected.opcodeID], " has kind "_s, kindName(actual.kind), " instead of "_s, kindName(expected.kind)));
        if (actual.hasThisProfile != expected.hasThisProfile)
            JITCACHE_FAIL(makeString(opcodeNames[expected.opcodeID], " has hasThisProfile "_s, bit(actual.hasThisProfile), " instead of "_s, bit(expected.hasThisProfile)));
    }
    for (OpcodeID opcodeID : { OpEnter::opcodeID, OpRet::opcodeID, OpGetById::opcodeID, OpAdd::opcodeID, OpInById::opcodeID, OpGetByVal::opcodeID })
        JITCACHE_CHECK(!ICs::canonicalCallLinkPosition(opcodeID));
    JITCACHE_CHECK(!ICs::canonicalCallLinkPosition(static_cast<OpcodeID>(std::numeric_limits<uint32_t>::max())));
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
    static_assert(offsetof(ICs::CallLinkRecord, thisArrayModes) == 2);
    static_assert(offsetof(ICs::CallLinkRecord, thisArrayProfileFlags) == 6);
    static_assert(ICs::LearningBit::mask == (ICs::LearningBit::everConsidered | ICs::LearningBit::sawNonCell | ICs::LearningBit::tookSlowPath | ICs::LearningBit::resetByGC));
    static_assert(ICs::StateBit::mask == (ICs::StateBit::megamorphicCaseListed | ICs::StateBit::canBeMegamorphic | ICs::StateBit::holdsGaveUp));
    static_assert(ICs::CallLinkBit::mask == (ICs::CallLinkBit::modeMask | ICs::CallLinkBit::seenOnce | ICs::CallLinkBit::hasSeenClosure | ICs::CallLinkBit::clearedByGC | ICs::CallLinkBit::clearedByVirtual));

    // The lane's this-word helpers agree with the format's byte order.
    ICs::CallLinkRecord helperRecord = callLink(ICs::CallLinkModeCode::Init, 0, 0);
    ICs::writeThisProfile(helperRecord, 0x0a0b0c0d, 0x01020304);
    JITCACHE_CHECK(loadLittleEndian(helperRecord.thisArrayModes) == 0x0a0b0c0d && loadLittleEndian(helperRecord.thisArrayProfileFlags) == 0x01020304);
    JITCACHE_CHECK(helperRecord.thisArrayModes[0] == 0x0d && helperRecord.thisArrayProfileFlags[3] == 0x01);
    JITCACHE_CHECK(ICs::readThisArrayModes(helperRecord) == 0x0a0b0c0d && ICs::readThisArrayProfileFlags(helperRecord) == 0x01020304);

    // Four property ICs, three of them with cases, and five call-link sites in two groups, with distinct values in every
    // header word and record field, the this words of the three op_call sites included, so a field read at the wrong
    // offset shows. 16 + 16 + 32 + 50 bytes, padded to 120.
    constexpr uint32_t doubleArrayModes = asArrayModesIgnoringTypedArrays(ArrayWithDouble) | asArrayModesIgnoringTypedArrays(NonArrayWithContiguous);
    TestSection section;
    section.propertyICs.append(propertyIC(AccessType::GetByVal, 0x0f, 0x21, 0x32, 0x43, 0x07));
    section.propertyICs.append(propertyIC(AccessType::PutByIdSloppy, 0x01, 0x02, 0x03, 0x00, 0x04));
    section.propertyICs.append(propertyIC(AccessType::InstanceOf, 0x0a, 0x5b, 0x6c, 0x01, 0x01));
    section.propertyICs.append(propertyIC(AccessType::DeleteByValStrict, 0x05, 0x7d, 0x0e, 0x02, 0x02));
    section.groups.append(ICs::CallLinkGroup { OpCall::opcodeID, 3 });
    section.groups.append(ICs::CallLinkGroup { OpCallVarargs::opcodeID, 2 });
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Monomorphic, ICs::CallLinkBit::seenOnce, 0x11, int32ArrayModes, 0x01));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Polymorphic, ICs::CallLinkBit::hasSeenClosure | ICs::CallLinkBit::clearedByGC, 0x5a, doubleArrayModes, 0x42));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, ICs::CallLinkBit::clearedByVirtual, 0x22, Float64ArrayMode | BigUint64ArrayMode, 0x80));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Virtual, ICs::CallLinkBit::seenOnce | ICs::CallLinkBit::clearedByVirtual, 0x33));
    section.callLinks.append(callLink(ICs::CallLinkModeCode::Init, 0, 0x44));
    section.propertyICCount = 4;
    section.icSitesWithCases = 3;
    section.callLinkGroupCount = 2;
    section.callLinkSiteCount = 5;
    Vector<uint8_t> bytes = encode(section);
    JITCACHE_CHECK(bytes.size() == 120);
    JITCACHE_CHECK(ICs::sectionSize(2, 4, 5) == bytes.size());

    // The section start needs no alignment: parse it at every offset modulo 8.
    for (size_t offset = 0; offset < 8; ++offset) {
        Vector<uint8_t> buffer(FillWith { }, offset, 0);
        buffer.appendVector(bytes);
        std::span<const uint8_t> sectionBytes = buffer.span().subspan(offset);
        ICs::SectionView view = ICs::parseSection(sectionBytes);
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
        JITCACHE_CHECK(ICs::readThisArrayModes(view.callLinks[1]) == doubleArrayModes && ICs::readThisArrayProfileFlags(view.callLinks[1]) == 0x42);
        JITCACHE_CHECK(ICs::readThisArrayModes(view.callLinks[2]) == (Float64ArrayMode | BigUint64ArrayMode) && ICs::readThisArrayProfileFlags(view.callLinks[2]) == 0x80);
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
    ICs::SectionView wideParsed = ICs::parseSection(wideBytes.span());
    JITCACHE_CHECK(wideParsed.header.propertyICCount == 0x102 && wideParsed.header.callLinkSiteCount == 0x103);
    JITCACHE_CHECK(wideParsed.group(0).siteCount == 0x103 && wideParsed.propertyICs.size() == 0x102 && wideParsed.callLinks.size() == 0x103);

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

// A bare header, with no group and no record, parses into empty views.
JITCACHE_TEST(icsParseSectionHeaderOnly, No)
{
    Vector<uint8_t> bytes = encode(TestSection { });
    JITCACHE_CHECK(bytes.size() == ICs::sectionHeaderSize);
    ICs::SectionView empty = ICs::parseSection(bytes.span());
    JITCACHE_CHECK(empty.propertyICs.empty() && empty.callLinks.empty() && empty.groupBytes.empty());
}

// The summary is the header's count, even where the records disagree with it.
JITCACHE_TEST(icsReadBaselineICsSummary, No)
{
    Vector<uint8_t> valid = encode(validSection());
    JITCACHE_CHECK(ICs::readBaselineICsSummary(valid.span()).icSitesWithCases == 2);
    Vector<uint8_t> miscounted = encodeValidSectionWith([](TestSection& section) {
        section.icSitesWithCases = 1;
    });
    JITCACHE_CHECK(ICs::readBaselineICsSummary(miscounted.span()).icSitesWithCases == 1);
    Vector<uint8_t> bareHeader = encode(TestSection());
    JITCACHE_CHECK(!ICs::readBaselineICsSummary(bareHeader.span()).icSitesWithCases);
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
    // The native IC states capture tells apart and the states around them, each with its consumer state written out.
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
    // caseCount, against the table expectedRestoredPropertyIC restates.
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

    // The whole sweep against the table expectedRestoredPropertyIC restates.
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
    using Mode = ICs::CallLinkModeCode;
    using Data = ICs::CallSiteDataState;
    const ICs::CallLinkOpcode inlineOpcode { OpCallVarargs::opcodeID, ICs::CallLinkSiteKind::Inline, false };
    const ICs::CallLinkOpcode lazyOpcode { OpConstruct::opcodeID, ICs::CallLinkSiteKind::Lazy, false };
    const ICs::CallLinkOpcode thisOpcode { OpCall::opcodeID, ICs::CallLinkSiteKind::Lazy, true };
    const ICs::CallLinkOpcode tailCallOpcode { OpTailCall::opcodeID, ICs::CallLinkSiteKind::LazyTailCall, true };

    // The data state of each kind of record, with the fields that matter to it written out; the sweep below checks every
    // other field.
    struct NamedState {
        ASCIILiteral name;
        ICs::CallLinkOpcode opcode;
        ICs::CallLinkRecord record;
        Data data;
        bool seenOnce;
        uint32_t thisArrayModes;
    };
    const NamedState states[] = {
        { "an empty lazy site"_s, lazyOpcode, callLink(Mode::Init, 0, 0), Data::NotExecuted, false, 0 },
        { "an empty tail call"_s, tailCallOpcode, callLink(Mode::Init, 0, 0), Data::NotExecuted, false, 0 },
        { "an empty inline site"_s, inlineOpcode, callLink(Mode::Init, 0, 0), Data::Inline, false, 0 },
        { "a lazy site called once"_s, thisOpcode, callLink(Mode::Init, seenOnce, 0), Data::ExecutedOnce, true, 0 },
        { "a monomorphic lazy site that holds nothing"_s, thisOpcode, callLink(Mode::Monomorphic, seenOnce, 0), Data::ExecutedOnce, true, 0 },
        { "a tail call that ran"_s, tailCallOpcode, callLink(Mode::Init, seenOnce, 0), Data::Own, true, 0 },
        { "an inline site called once"_s, inlineOpcode, callLink(Mode::Init, seenOnce, 0), Data::Inline, true, 0 },
        { "a lazy site whose only fact is the virtual mode"_s, lazyOpcode, callLink(Mode::Virtual, 0, 0), Data::Own, false, 0 },
        { "a lazy site gone virtual"_s, lazyOpcode, callLink(Mode::Virtual, clearedByVirtual, 0), Data::Own, false, 0 },
        { "a lazy site whose only fact is hasSeenClosure"_s, thisOpcode, callLink(Mode::Init, hasSeenClosure, 0), Data::Own, true, 0 },
        { "a polymorphic lazy site with closures"_s, thisOpcode, callLink(Mode::Polymorphic, seenOnce | hasSeenClosure, 0), Data::Own, true, 0 },
        { "a lazy site whose callee died, unseen"_s, thisOpcode, callLink(Mode::Init, clearedByGC, 0), Data::Own, true, 0 },
        { "a lazy site once virtual"_s, lazyOpcode, callLink(Mode::Init, clearedByVirtual, 0), Data::Own, true, 0 },
        { "a lazy site with a varargs maximum"_s, lazyOpcode, callLink(Mode::Init, 0, 3), Data::Own, true, 0 },
        { "an unseen site whose only fact is its this modes"_s, thisOpcode, callLink(Mode::Init, 0, 0, int32ArrayModes, 0), Data::Own, true, int32ArrayModes },
        { "an unseen site whose only fact is its this flags"_s, thisOpcode, callLink(Mode::Init, 0, 0, 0, outOfBoundsFlag), Data::Own, true, 0 },
        { "a this profile at an opcode without one"_s, lazyOpcode, callLink(Mode::Init, 0, 0, int32ArrayModes, outOfBoundsFlag), Data::NotExecuted, false, 0 },
        { "a seen site with this modes at an opcode without one"_s, lazyOpcode, callLink(Mode::Monomorphic, seenOnce, 0, int32ArrayModes, 0), Data::ExecutedOnce, true, 0 },
        { "a tail call with a this profile"_s, tailCallOpcode, callLink(Mode::Monomorphic, seenOnce, 0, int32ArrayModes, pruningFlag), Data::Own, true, int32ArrayModes },
        { "an inline site with this modes"_s, inlineOpcode, callLink(Mode::Monomorphic, seenOnce, 7, int32ArrayModes, 0), Data::Inline, true, 0 },
    };
    for (auto& state : states) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(state.opcode, state.record);
        if (restored.data != state.data || restored.seenOnce != state.seenOnce || restored.thisArrayModes != state.thisArrayModes) {
            JITCACHE_FAIL(makeString(state.name, " at "_s, describe(state.opcode), ": restores "_s, describe(restored), " instead of data "_s, dataName(state.data),
                ", seenOnce "_s, bit(state.seenOnce), ", thisArrayModes "_s, state.thisArrayModes));
        }
    }

    // The derived this fields are the record's at an opcode with a this profile and zero elsewhere, whatever it holds.
    ICs::CallLinkRecord withThisProfile = callLink(Mode::Monomorphic, seenOnce, 0, int32ArrayModes, outOfBoundsFlag | pruningFlag);
    ICs::RestoredCallLink atThisOpcode = ICs::restoredCallLink(thisOpcode, withThisProfile);
    JITCACHE_CHECK(atThisOpcode.thisArrayModes == int32ArrayModes && atThisOpcode.thisArrayProfileFlags == (outOfBoundsFlag | pruningFlag));
    for (const ICs::CallLinkOpcode& opcode : { inlineOpcode, lazyOpcode }) {
        ICs::RestoredCallLink elsewhere = ICs::restoredCallLink(opcode, withThisProfile);
        JITCACHE_CHECK(!elsewhere.thisArrayModes && !elsewhere.thisArrayProfileFlags);
    }

    forEachCallLinkRecord([&](const ICs::CallLinkOpcode& opcode, const ICs::CallLinkRecord& record) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(opcode, record);
        ICs::RestoredCallLink expected = expectedRestoredCallLink(opcode, record);
        if (restored == expected)
            return true;
        JITCACHE_FAIL(makeString(describe(record), " at "_s, describe(opcode), " restores "_s, describe(restored), " instead of "_s, describe(expected)));
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
}

JITCACHE_TEST(icsCallLinkRoundTrip, No)
{
    forEachCallLinkRecord([&](const ICs::CallLinkOpcode& opcode, const ICs::CallLinkRecord& record) {
        ICs::RestoredCallLink restored = ICs::restoredCallLink(opcode, record);
        ICs::CallLinkRecord recaptured = ICs::recapturedCallLink(opcode, record);
        ICs::RestoredCallLink restoredAgain = ICs::restoredCallLink(opcode, recaptured);
        if (!(restoredAgain == restored)) {
            JITCACHE_FAIL(makeString(describe(record), " at "_s, describe(opcode), " recaptures as "_s, describe(recaptured), ", which restores "_s, describe(restoredAgain), " instead of "_s, describe(restored)));
            return false;
        }

        // A capture of a site on what linking gave it reads nothing, of a site on the executed-once data only the seen
        // bit, and of a site with a CallLinkInfo of its own the mode seeding leaves (Init or, through setVirtualCall,
        // Virtual), its bits, its varargs maximum and its this fields.
        auto mode = static_cast<ICs::CallLinkModeCode>(recaptured.bits & ICs::CallLinkBit::modeMask);
        bool shapeHolds = false;
        switch (restored.data) {
        case ICs::CallSiteDataState::NotExecuted:
            shapeHolds = sameRecord(recaptured, callLink(ICs::CallLinkModeCode::Init, 0, 0));
            break;
        case ICs::CallSiteDataState::ExecutedOnce:
            shapeHolds = sameRecord(recaptured, callLink(ICs::CallLinkModeCode::Init, ICs::CallLinkBit::seenOnce, 0));
            break;
        case ICs::CallSiteDataState::Inline:
        case ICs::CallSiteDataState::Own:
            shapeHolds = mode == (restored.isVirtual ? ICs::CallLinkModeCode::Virtual : ICs::CallLinkModeCode::Init)
                && !(recaptured.bits & ~ICs::CallLinkBit::mask)
                && recaptured.maxArgumentCountIncludingThisForVarargs == record.maxArgumentCountIncludingThisForVarargs
                && loadLittleEndian(recaptured.thisArrayModes) == restored.thisArrayModes
                && loadLittleEndian(recaptured.thisArrayProfileFlags) == restored.thisArrayProfileFlags;
            break;
        }
        if (!shapeHolds) {
            JITCACHE_FAIL(makeString(describe(record), " at "_s, describe(opcode), " recaptures as "_s, describe(recaptured)));
            return false;
        }

        // A recaptured record is empty exactly when the record is, and holds something exactly when it does.
        if ((restored.data == ICs::CallSiteDataState::NotExecuted) != (restoredAgain.data == ICs::CallSiteDataState::NotExecuted)) {
            JITCACHE_FAIL(makeString(describe(record), " at "_s, describe(opcode), " changes its emptiness when recaptured"_s));
            return false;
        }

        ICs::CallLinkRecord recapturedAgain = ICs::recapturedCallLink(opcode, recaptured);
        if (!sameRecord(recapturedAgain, recaptured)) {
            JITCACHE_FAIL(makeString(describe(record), " at "_s, describe(opcode), " recaptures as "_s, describe(recaptured), " and then as "_s, describe(recapturedAgain)));
            return false;
        }
        return true;
    });
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
