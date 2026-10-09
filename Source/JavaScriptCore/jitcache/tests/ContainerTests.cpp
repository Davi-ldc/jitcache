#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheContainer.h"
#include "JITCachePlatform.h"
#include "JITCacheSHA256.h"
#include "JITCacheTest.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <utility>
#include <wtf/HexNumber.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/SystemMalloc.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests C1 to C3 of SPEC-integrator.container.md section 9: CRC32C, the header, and the body format with its validation.
// None needs a VM. The tests write both formats byte by byte at the offsets sections 3 and 4 give, independently of the
// container's encoders, so every parse also checks the layout. Validation runs over buffers of exactly the input's size
// from the system allocator, so ASan reports any read past the end.

namespace JSC::JITCache::Tests {

namespace ContainerTestsInternal {

// A fixed seed, so a failure reproduces.
static constexpr unsigned randomSeed = 0x4a495443;

static uint32_t crc32cOf(std::span<const uint8_t> bytes)
{
    return ~crc32cExtend(~0u, bytes);
}

// CRC-32C of RFC 3720 bit by bit: the reflected Castagnoli polynomial, initial value and final XOR 0xFFFFFFFF.
static uint32_t bitwiseCRC32C(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78 : crc >> 1;
    }
    return ~crc;
}

// Little-endian fields, written and read byte by byte.
static void putField(Vector<uint8_t>& bytes, size_t offset, uint64_t value, unsigned width)
{
    for (unsigned index = 0; index < width; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

static void appendField(Vector<uint8_t>& bytes, uint64_t value, unsigned width)
{
    for (unsigned index = 0; index < width; ++index)
        bytes.append(static_cast<uint8_t>(value >> (8 * index)));
}

static uint64_t getField(std::span<const uint8_t> bytes, size_t offset, unsigned width)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < width; ++index)
        value |= static_cast<uint64_t>(bytes[offset + index]) << (8 * index);
    return value;
}

// A buffer of exactly the input's size from the system allocator.
using ExactBuffer = MallocSpan<uint8_t, SystemMallocBase<uint8_t>>;

static ExactBuffer exactCopy(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return { };
    auto copy = ExactBuffer::malloc(bytes.size());
    memcpySpan(copy.mutableSpan(), bytes);
    return copy;
}

// The table of SPEC-integrator.md section 6.1, as the test reads it.
struct KindRow {
    SectionKind kind;
    uint16_t typeId;
    uint8_t tier;
    ASCIILiteral name;
};

static constexpr std::array<KindRow, numberOfSectionKinds> kindRows { {
    { SectionKind::UCBIdentity, 0x0101, 0, "ucb.identity"_s },
    { SectionKind::UCBCore, 0x0102, 0, "ucb.core"_s },
    { SectionKind::UCBFeedback, 0x0103, 0, "ucb.feedback"_s },
    { SectionKind::ImageBaseline, 0x0201, 1, "image.baseline"_s },
    { SectionKind::BakedFactsBaseline, 0x0202, 1, "baked-facts.baseline"_s },
    { SectionKind::ImageTwinsBaseline, 0x0203, 1, "image-twins.baseline"_s },
    { SectionKind::CBStateBaseline, 0x0301, 1, "cb.state"_s },
    { SectionKind::CBSummaryBaseline, 0x0302, 1, "cb.summary"_s },
    { SectionKind::ICsBaseline, 0x0401, 1, "ICsBaseline"_s },
} };

static const KindRow& rowOf(SectionKind kind)
{
    for (auto& row : kindRows) {
        if (row.kind == kind)
            return row;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The header (container sub-SPEC section 3.1), every field settable, so a test can break one rule at a time.

struct RawBuildIDRecord {
    uint8_t role { 1 };
    uint8_t length { 0 };
    uint16_t reserved16 { 0 };
    uint32_t reserved32 { 0 };
    std::array<uint8_t, 64> id { };
};

struct RawOptionRecord {
    uint16_t index { 0 };
    uint8_t type { 1 };
    uint8_t value { 0 };
    uint32_t reserved { 0 };
};

struct RawHeader {
    std::array<uint8_t, 8> tag { 'J', 'I', 'T', 'C', 'H', 'E', 'A', 'D' };
    uint16_t version { 1 };
    uint8_t architecture { 1 };
    std::optional<uint8_t> buildIDCount; // B; the number of records when absent
    std::optional<uint32_t> size; // H; the file's size when absent
    uint64_t cpuFeatures { 0 };
    std::optional<uint32_t> optionCount; // M; the number of option records when absent
    uint32_t reserved { 0 }; // bytes 28 to 31
    Vector<RawBuildIDRecord> buildIDs;
    Vector<RawOptionRecord> options;
    size_t extraBytes { 0 }; // zero bytes between the option records and the CRC
    std::optional<uint32_t> crc; // the CRC32C of the bytes before it when absent
    uint32_t trailer { 0 }; // the last four bytes
};

static Vector<uint8_t> encodeRawHeader(const RawHeader& header)
{
    Vector<uint8_t> bytes;
    for (uint8_t byte : header.tag)
        bytes.append(byte);
    appendField(bytes, header.version, 2);
    bytes.append(header.architecture);
    bytes.append(header.buildIDCount.value_or(static_cast<uint8_t>(header.buildIDs.size())));
    appendField(bytes, 0, 4); // H, set below
    appendField(bytes, header.cpuFeatures, 8);
    appendField(bytes, header.optionCount.value_or(static_cast<uint32_t>(header.options.size())), 4);
    appendField(bytes, header.reserved, 4);
    for (auto& record : header.buildIDs) {
        bytes.append(record.role);
        bytes.append(record.length);
        appendField(bytes, record.reserved16, 2);
        appendField(bytes, record.reserved32, 4);
        for (uint8_t byte : record.id)
            bytes.append(byte);
    }
    for (auto& record : header.options) {
        appendField(bytes, record.index, 2);
        bytes.append(record.type);
        bytes.append(record.value);
        appendField(bytes, record.reserved, 4);
    }
    for (size_t index = 0; index < header.extraBytes; ++index)
        bytes.append(0);
    putField(bytes, 12, header.size.value_or(static_cast<uint32_t>(bytes.size() + 8)), 4);
    appendField(bytes, header.crc.value_or(crc32cOf(bytes.span())), 4);
    appendField(bytes, header.trailer, 4);
    return bytes;
}

static RawBuildIDRecord rawBuildIDRecord(uint8_t role, const BuildID& buildID)
{
    RawBuildIDRecord record;
    record.role = role;
    record.length = buildID.size;
    for (size_t index = 0; index < buildID.size; ++index)
        record.id[index] = buildID.bytes[index];
    return record;
}

static RawHeader rawHeaderFor(const ArtifactHeader& contents)
{
    RawHeader header;
    header.architecture = contents.architecture;
    header.cpuFeatures = contents.facts.cpuFeatures;
    header.buildIDs.append(rawBuildIDRecord(1, contents.facts.mainExecutable));
    if (contents.facts.engineObject)
        header.buildIDs.append(rawBuildIDRecord(2, *contents.facts.engineObject));
    for (uint16_t index = 0; index < 3; ++index)
        header.options.append(RawOptionRecord { index, 1, contents.facts.mustMatch[index] ? uint8_t { 1 } : uint8_t { 0 }, 0 });
    return header;
}

static BuildID craftedBuildID(uint8_t length, uint8_t seed)
{
    BuildID buildID;
    buildID.size = length;
    for (size_t index = 0; index < length; ++index)
        buildID.bytes[index] = static_cast<uint8_t>(seed + 3 * index + 1);
    return buildID;
}

static constexpr uint8_t headerX86_64 = 1;
static constexpr uint8_t headerARM64 = 2;

static ArtifactHeader craftedHeader(uint8_t architecture, bool withEngineObject)
{
    ArtifactHeader header;
    header.architecture = architecture;
    header.facts.mainExecutable = craftedBuildID(20, 0x10);
    if (withEngineObject)
        header.facts.engineObject = craftedBuildID(20, 0x70);
    header.facts.cpuFeatures = architecture == headerX86_64 ? 0b0101010101 : 0b01010101;
    header.facts.mustMatch = { false, true, false };
    return header;
}

static bool equalBuildIDs(const BuildID& a, const BuildID& b)
{
    return a.size == b.size && equalSpans(std::span { a.bytes }, std::span { b.bytes });
}

static bool equalContents(const ArtifactHeader& a, const ArtifactHeader& b)
{
    if (a.architecture != b.architecture || a.facts.cpuFeatures != b.facts.cpuFeatures || a.facts.mustMatch != b.facts.mustMatch)
        return false;
    if (!equalBuildIDs(a.facts.mainExecutable, b.facts.mainExecutable))
        return false;
    if (a.facts.engineObject.has_value() != b.facts.engineObject.has_value())
        return false;
    return !a.facts.engineObject || equalBuildIDs(*a.facts.engineObject, *b.facts.engineObject);
}

static Vector<uint8_t> encodedHeader(const ArtifactHeader& contents)
{
    auto bytes = encodeArtifactHeader(contents);
    RELEASE_ASSERT(bytes);
    Vector<uint8_t> result;
    result.append(bytes->span());
    return result;
}

static ASCIILiteral verdictName(HeaderVerdict verdict)
{
    switch (verdict) {
    case HeaderVerdict::Compatible:
        return "Compatible"_s;
    case HeaderVerdict::Incompatible:
        return "Incompatible"_s;
    case HeaderVerdict::Corrupt:
        return "Corrupt"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Body files (container sub-SPEC section 4).

struct TestSection {
    SectionKind kind;
    Vector<uint8_t> bytes;
};

// Sizes that leave padding after ucb.core, baked-facts.baseline, image-twins.baseline and cb.summary, and an empty
// ucb.feedback.
static constexpr std::array<size_t, numberOfSectionKinds> sectionSizes { 40, 13, 0, 64, 7, 9, 24, 3, 17 };

static Vector<TestSection> sectionsWithout(std::optional<SectionKind> omitted = std::nullopt)
{
    Vector<TestSection> sections;
    for (size_t kindIndex = 0; kindIndex < numberOfSectionKinds; ++kindIndex) {
        SectionKind kind = kindRows[kindIndex].kind;
        if (kind == omitted)
            continue;
        Vector<uint8_t> bytes;
        for (size_t index = 0; index < sectionSizes[kindIndex]; ++index)
            bytes.append(static_cast<uint8_t>(0x11 * (kindIndex + 1) + 7 * index + 1));
        sections.append(TestSection { kind, WTF::move(bytes) });
    }
    return sections;
}

struct RawEntry {
    uint16_t typeId { 0 };
    uint8_t tier { 0 };
    uint8_t zero { 0 };
    uint64_t offset { 0 };
    uint64_t size { 0 };
};

struct Placement {
    uint64_t offset;
    Vector<uint8_t> bytes;
};

// A body with every envelope field and directory entry given. The encoder places the bytes, writes the envelope's fields,
// then each entry with the CRC of the bytes its range covers at that point, then the directory's and the envelope's CRCs,
// so an entry may cover any bytes but the CRC fields that come after it.
struct RawBody {
    BodyKey key { };
    HeaderDigest headerDigest { };
    uint64_t version { 0 };
    uint8_t highestTier { 1 };
    uint32_t llintThreshold { 0 };
    uint32_t counterProgress { 0 };
    uint64_t fileSize { 0 };
    Vector<RawEntry> entries;
    Vector<Placement> placements;
};

static Vector<uint8_t> encodeRawBody(const RawBody& body)
{
    Vector<uint8_t> file(FillWith { }, static_cast<size_t>(body.fileSize), 0);
    for (auto& placement : body.placements) {
        for (size_t index = 0; index < placement.bytes.size(); ++index)
            file[placement.offset + index] = placement.bytes[index];
    }
    static constexpr std::array<uint8_t, 8> tag { 'J', 'I', 'T', 'C', 'B', 'O', 'D', 'Y' };
    for (size_t index = 0; index < tag.size(); ++index)
        file[index] = tag[index];
    putField(file, 8, 1, 2);
    putField(file, 10, 128, 2);
    putField(file, 12, body.entries.size(), 2);
    file[14] = body.highestTier;
    auto key = std::bit_cast<std::array<uint8_t, BodyKey::byteSize>>(body.key);
    for (size_t index = 0; index < key.size(); ++index)
        file[16 + index] = key[index];
    putField(file, 56, body.version, 8);
    for (size_t index = 0; index < body.headerDigest.size(); ++index)
        file[64 + index] = body.headerDigest[index];
    putField(file, 80, body.fileSize, 8);
    putField(file, 88, body.llintThreshold, 4);
    putField(file, 92, body.counterProgress, 4);
    for (size_t index = 0; index < body.entries.size(); ++index) {
        auto& entry = body.entries[index];
        size_t at = 128 + 24 * index;
        putField(file, at, entry.typeId, 2);
        file[at + 2] = entry.tier;
        file[at + 3] = entry.zero;
        putField(file, at + 4, crc32cOf(file.span().subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.size))), 4);
        putField(file, at + 8, entry.offset, 8);
        putField(file, at + 16, entry.size, 8);
    }
    putField(file, 96, crc32cOf(file.span().subspan(128, 24 * body.entries.size())), 4);
    putField(file, 120, crc32cOf(file.span().first(120)), 4);
    return file;
}

// The body the writer would write: section 4.3's layout for directoryEntries entries, the sections first.
static RawBody canonicalBody(const BodyKey& key, const HeaderDigest& headerDigest, const Vector<TestSection>& sections, size_t directoryEntries = 0)
{
    RawBody body;
    body.key = key;
    body.headerDigest = headerDigest;
    body.version = 0x0123456789abcdef;
    body.llintThreshold = 500;
    body.counterProgress = 77;
    uint64_t end = 128 + 24 * std::max(directoryEntries, sections.size());
    for (size_t index = 0; index < sections.size(); ++index) {
        uint64_t offset = index ? (end + 7) & ~static_cast<uint64_t>(7) : end;
        auto& row = rowOf(sections[index].kind);
        body.entries.append(RawEntry { row.typeId, row.tier, 0, offset, sections[index].bytes.size() });
        body.placements.append(Placement { offset, sections[index].bytes });
        end = offset + sections[index].bytes.size();
    }
    body.fileSize = end;
    return body;
}

static BodyKey testKey(uint8_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1; // version
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(seed + 5 * index);
    return std::bit_cast<BodyKey>(bytes);
}

static HeaderDigest testDigest(uint8_t seed)
{
    HeaderDigest digest;
    for (size_t index = 0; index < digest.size(); ++index)
        digest[index] = static_cast<uint8_t>(seed + 11 * index);
    return digest;
}

static void resealEnvelope(Vector<uint8_t>& file)
{
    putField(file, 120, crc32cOf(file.span().first(120)), 4);
}

// The directory's CRC for the envelope's N, which must leave the directory inside the file, then the envelope's.
static void resealDirectory(Vector<uint8_t>& file)
{
    size_t count = static_cast<size_t>(getField(file.span(), 12, 2));
    putField(file, 96, crc32cOf(file.span().subspan(128, 24 * count)), 4);
    resealEnvelope(file);
}

static size_t entryAt(size_t index)
{
    return 128 + 24 * index;
}

// The first padding byte: the end of the first section, followed by another, whose size is not a multiple of 8.
static size_t firstPaddingByte(const RawBody& body)
{
    for (size_t index = 0; index + 1 < body.entries.size(); ++index) {
        auto& entry = body.entries[index];
        if (entry.size % 8)
            return static_cast<size_t>(entry.offset + entry.size);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct Fixture {
    BodyKey key;
    HeaderDigest headerDigest;
    RawBody body;
    Vector<uint8_t> file;
};

static Fixture makeFixture(const Vector<TestSection>& sections)
{
    Fixture fixture { testKey(0x20), testDigest(0xa0), { }, { } };
    fixture.body = canonicalBody(fixture.key, fixture.headerDigest, sections);
    fixture.file = encodeRawBody(fixture.body);
    return fixture;
}

using Outcome = std::optional<ASCIILiteral>; // empty: the body validates; otherwise the failing check's name

static Outcome outcomeOf(const std::expected<BodyLayout, ContainerCheck>& result)
{
    if (result)
        return std::nullopt;
    return result.error();
}

static String describe(const Outcome& outcome)
{
    if (!outcome)
        return "acceptance"_s;
    return *outcome;
}

// Runs validateBody and the stream over an exact-size copy of the file, feeding the stream whole, byte by byte and in
// random pieces, and fails the test when they disagree. Returns validateBody's outcome.
static Outcome validateEveryWay(TestContext& context, const String& label, std::span<const uint8_t> file, const BodyKey& key,
    const HeaderDigest& headerDigest, ValidationMode mode, WeakRandom& random)
{
    auto copy = exactCopy(file);
    auto bytes = copy.span();
    Outcome expected = outcomeOf(validateBody(bytes, key, headerDigest, mode));
    auto compare = [&](ASCIILiteral form, Outcome outcome) {
        if (outcome != expected)
            JITCACHE_FAIL(makeString(label, ": the stream fed "_s, form, " gives "_s, describe(outcome), " where validateBody gives "_s, describe(expected)));
    };
    {
        BodyValidationStream stream(key, headerDigest, mode, bytes.size());
        stream.append(bytes);
        compare("whole"_s, outcomeOf(stream.finish()));
    }
    {
        BodyValidationStream stream(key, headerDigest, mode, bytes.size());
        for (size_t index = 0; index < bytes.size(); ++index)
            stream.append(bytes.subspan(index, 1));
        compare("byte by byte"_s, outcomeOf(stream.finish()));
    }
    {
        BodyValidationStream stream(key, headerDigest, mode, bytes.size());
        size_t offset = 0;
        while (offset < bytes.size()) {
            size_t piece = 1 + random.getUint32(static_cast<unsigned>(std::min<size_t>(bytes.size() - offset, 64)));
            stream.append(bytes.subspan(offset, piece));
            offset += piece;
        }
        compare("in random pieces"_s, outcomeOf(stream.finish()));
    }
    return expected;
}

static void expectOutcomes(TestContext& context, const String& label, std::span<const uint8_t> file, const BodyKey& key,
    const HeaderDigest& headerDigest, Outcome full, Outcome integrity, WeakRandom& random)
{
    for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
        Outcome expected = mode == ValidationMode::Full ? full : integrity;
        Outcome found = validateEveryWay(context, label, file, key, headerDigest, mode, random);
        if (found != expected) {
            JITCACHE_FAIL(makeString(label, mode == ValidationMode::Full ? " in Full mode: "_s : " in Integrity mode: "_s,
                describe(expected), " expected, "_s, describe(found), " found"_s));
        }
    }
}

static const Outcome accepted = std::nullopt;
static const Outcome sizeCheck { ContainerChecks::size };
static const Outcome envelopeCheck { ContainerChecks::envelope };
static const Outcome keyCheck { ContainerChecks::key };
static const Outcome headerDigestCheck { ContainerChecks::headerDigest };
static const Outcome versionCheck { ContainerChecks::version };
static const Outcome directoryCheck { ContainerChecks::directory };
static const Outcome requiredCheck { ContainerChecks::required };
static const Outcome checksumCheck { ContainerChecks::checksum };

} // namespace ContainerTestsInternal

using namespace ContainerTestsInternal;

// C1. The RFC 3720 vectors, and agreement with the bitwise reference over random buffers, lengths and alignments, fed
// whole and in random pieces.
JITCACHE_TEST(containerCRC32C, No)
{
    WeakRandom random(randomSeed);
    auto checkVector = [&](ASCIILiteral name, std::span<const uint8_t> bytes, uint32_t expected) {
        if (crc32cOf(bytes) != expected)
            JITCACHE_FAIL(makeString("crc32c of "_s, name, " is "_s, hex(crc32cOf(bytes)), ", expected "_s, hex(expected)));
        if (bitwiseCRC32C(bytes) != expected)
            JITCACHE_FAIL(makeString("the bitwise reference of "_s, name, " is "_s, hex(bitwiseCRC32C(bytes))));
        uint32_t state = ~0u;
        for (size_t index = 0; index < bytes.size(); ++index)
            state = crc32cExtend(state, bytes.subspan(index, 1));
        if (~state != expected)
            JITCACHE_FAIL(makeString("crc32c of "_s, name, " fed byte by byte is "_s, hex(~state)));
    };

    static constexpr std::array<uint8_t, 9> checkString { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    checkVector("\"123456789\""_s, std::span { checkString }, 0xE3069283);
    std::array<uint8_t, 32> pattern { };
    checkVector("32 zero bytes"_s, std::span { pattern }, 0x8A9136AA);
    pattern.fill(0xFF);
    checkVector("32 0xFF bytes"_s, std::span { pattern }, 0x62A8AB43);
    for (size_t index = 0; index < pattern.size(); ++index)
        pattern[index] = static_cast<uint8_t>(index);
    checkVector("the bytes 0 to 31"_s, std::span { pattern }, 0x46DD794E);
    JITCACHE_CHECK(!crc32cOf({ }));
    JITCACHE_CHECK(crc32cExtend(0x12345678, { }) == 0x12345678);

    static constexpr size_t maximumLength = 2048;
    static constexpr size_t maximumMisalignment = 16;
    Vector<uint8_t> storage(FillWith { }, maximumLength + maximumMisalignment, 0);
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration < 4096 && failures < 8; ++iteration) {
        size_t length = random.getUint32(maximumLength + 1);
        size_t misalignment = random.getUint32(maximumMisalignment);
        auto bytes = storage.mutableSpan().subspan(misalignment, length);
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random.getUint32(256));
        uint32_t expected = bitwiseCRC32C(bytes);
        uint32_t whole = crc32cOf(bytes);
        uint32_t state = ~0u;
        for (size_t offset = 0; offset < length;) {
            size_t piece = 1 + random.getUint32(static_cast<unsigned>(length - offset));
            state = crc32cExtend(state, bytes.subspan(offset, piece));
            offset += piece;
        }
        if (whole != expected || ~state != expected) {
            ++failures;
            JITCACHE_FAIL(makeString("buffer "_s, iteration, " of length "_s, length, " at misalignment "_s, misalignment, ": reference "_s,
                hex(expected), ", whole "_s, hex(whole), ", in pieces "_s, hex(~state)));
        }
    }
}

// SPEC-integrator.md section 6.1's table, which the container, the writer and strict validation read.
JITCACHE_TEST(containerSectionKinds, No)
{
    for (auto& row : kindRows) {
        auto& description = sectionKindDescription(row.kind);
        if (description.typeId != row.typeId || description.tier != row.tier || description.name != row.name)
            JITCACHE_FAIL(makeString(row.name, ": the container describes it as "_s, description.name, ", type id "_s, hex(description.typeId), ", tier "_s, static_cast<unsigned>(description.tier)));
        JITCACHE_CHECK(sectionKindFor(row.typeId, row.tier) == row.kind);
        // Every kind is required in a body of highest tier 1 in a twins build, image-twins.baseline included; a body of
        // highest tier 0 would hold the shared UCB sections alone.
        JITCACHE_CHECK(isSectionRequired(row.kind, 1));
        JITCACHE_CHECK(isSectionRequired(row.kind, 0) == !row.tier);
    }
    JITCACHE_CHECK(!sectionKindFor(0x0101, 1));
    JITCACHE_CHECK(!sectionKindFor(0x0104, 0));
    JITCACHE_CHECK(!sectionKindFor(0x0201, 0));
    JITCACHE_CHECK(!sectionKindFor(0x0201, 2));
    JITCACHE_CHECK(!sectionKindFor(0x0501, 1));
    JITCACHE_CHECK(!sectionKindFor(0, 0));
}

// C2. A header built from crafted facts parses back to them, and its bytes are section 3.1's.
JITCACHE_TEST(containerHeaderRoundTrip, No)
{
    Vector<ArtifactHeader> headers;
    headers.append(craftedHeader(headerX86_64, false));
    headers.append(craftedHeader(headerX86_64, true));
    headers.append(craftedHeader(headerARM64, false));
    headers.append(craftedHeader(headerARM64, true));
    {
        ArtifactHeader header = craftedHeader(headerX86_64, true);
        header.facts.mainExecutable = craftedBuildID(64, 0x30);
        header.facts.engineObject = craftedBuildID(1, 0x40);
        header.facts.cpuFeatures = std::numeric_limits<uint64_t>::max();
        header.facts.mustMatch = { true, true, true };
        headers.append(header);
    }
    {
        ArtifactHeader header = craftedHeader(headerARM64, false);
        header.facts.mainExecutable = craftedBuildID(1, 0xfe);
        header.facts.cpuFeatures = 0;
        header.facts.mustMatch = { true, false, true };
        headers.append(header);
    }

    for (size_t index = 0; index < headers.size(); ++index) {
        auto& contents = headers[index];
        auto encoded = encodeArtifactHeader(contents);
        if (!encoded) {
            JITCACHE_FAIL(makeString("header "_s, index, " was not encoded"_s));
            continue;
        }
        JITCACHE_CHECK(encoded->size == (contents.facts.engineObject ? 208u : 136u));
        if (!equalSpans(encoded->span(), encodeRawHeader(rawHeaderFor(contents)).span()))
            JITCACHE_FAIL(makeString("header "_s, index, ": the encoded bytes differ from section 3.1's layout"_s));
        auto parsed = readArtifactHeader(encoded->span());
        if (!parsed)
            JITCACHE_FAIL(makeString("header "_s, index, " does not parse: "_s, parsed.error().reason));
        else if (!equalContents(*parsed, contents))
            JITCACHE_FAIL(makeString("header "_s, index, " parses to other contents"_s));
        auto check = checkArtifactHeader(encoded->span(), encoded->span());
        JITCACHE_CHECK(check.verdict == HeaderVerdict::Compatible && check.detail.isEmpty());
    }

    ArtifactHeader withoutMainID = craftedHeader(headerX86_64, false);
    withoutMainID.facts.mainExecutable = BuildID { };
    JITCACHE_CHECK(!encodeArtifactHeader(withoutMainID));
    ArtifactHeader withLongMainID = craftedHeader(headerX86_64, false);
    withLongMainID.facts.mainExecutable.size = 65;
    JITCACHE_CHECK(!encodeArtifactHeader(withLongMainID));
    ArtifactHeader withoutEngineID = craftedHeader(headerX86_64, false);
    withoutEngineID.facts.engineObject = BuildID { };
    JITCACHE_CHECK(!encodeArtifactHeader(withoutEngineID));
}

// C2. Each corrupt case of section 3.2 is classified corrupt; another layout version is incompatible without the rest
// being read.
JITCACHE_TEST(containerHeaderCorrupt, No)
{
    ArtifactHeader processContents = craftedHeader(headerX86_64, false);
    Vector<uint8_t> processHeader = encodedHeader(processContents);
    RawHeader base = rawHeaderFor(processContents);
    JITCACHE_CHECK(equalSpans(encodeRawHeader(base).span(), processHeader.span()));

    auto expectVerdict = [&](ASCIILiteral label, std::span<const uint8_t> file, HeaderVerdict expected) {
        auto copy = exactCopy(file);
        auto check = checkArtifactHeader(copy.span(), processHeader.span());
        if (check.verdict != expected)
            JITCACHE_FAIL(makeString(label, ": "_s, verdictName(check.verdict), " ("_s, check.detail, "), expected "_s, verdictName(expected)));
        auto read = readArtifactHeader(copy.span());
        if (expected == HeaderVerdict::Compatible ? !read : (read || read.error().verdict != expected))
            JITCACHE_FAIL(makeString(label, ": readArtifactHeader disagrees with checkArtifactHeader"_s));
    };
    auto expectCorrupt = [&](ASCIILiteral label, const RawHeader& header) {
        expectVerdict(label, encodeRawHeader(header).span(), HeaderVerdict::Corrupt);
    };
    auto mutated = [&](auto&& mutate) {
        RawHeader header = base;
        mutate(header);
        return header;
    };

    expectVerdict("the base header"_s, processHeader.span(), HeaderVerdict::Compatible);
    expectVerdict("an empty file"_s, { }, HeaderVerdict::Corrupt);
    expectVerdict("39 bytes"_s, processHeader.span().first(39), HeaderVerdict::Corrupt);
    {
        Vector<uint8_t> large = processHeader;
        large.grow(maximumArtifactHeaderFileBytes + 1);
        for (size_t index = processHeader.size(); index < large.size(); ++index)
            large[index] = 0;
        expectVerdict("a file larger than 4 KiB"_s, large.span(), HeaderVerdict::Corrupt);
    }
    expectCorrupt("another tag"_s, mutated([](RawHeader& header) { header.tag[3] = 'X'; }));
    expectCorrupt("H larger than the file"_s, mutated([](RawHeader& header) { header.size = 136 + 8; }));
    expectCorrupt("H smaller than the file"_s, mutated([](RawHeader& header) { header.size = 136 - 8; }));
    expectCorrupt("H equal to the file's size but not to the formula"_s, mutated([](RawHeader& header) { header.extraBytes = 8; }));
    expectCorrupt("B of 0"_s, mutated([](RawHeader& header) { header.buildIDs.clear(); }));
    expectCorrupt("B of 3"_s, mutated([&](RawHeader& header) {
        header.buildIDs.append(rawBuildIDRecord(2, craftedBuildID(8, 0x50)));
        header.buildIDs.append(rawBuildIDRecord(3, craftedBuildID(8, 0x60)));
    }));
    expectCorrupt("M of 2"_s, mutated([](RawHeader& header) { header.options.removeLast(); }));
    expectCorrupt("M of 4"_s, mutated([](RawHeader& header) { header.options.append(RawOptionRecord { 3, 1, 0, 0 }); }));
    expectCorrupt("a role of 0"_s, mutated([](RawHeader& header) { header.buildIDs[0].role = 0; }));
    expectCorrupt("a role of 3"_s, mutated([](RawHeader& header) { header.buildIDs[0].role = 3; }));
    expectCorrupt("the engine object's role first"_s, mutated([](RawHeader& header) { header.buildIDs[0].role = 2; }));
    expectCorrupt("two main executable records"_s, mutated([&](RawHeader& header) { header.buildIDs.append(rawBuildIDRecord(1, craftedBuildID(8, 0x50))); }));
    expectCorrupt("an ID of length 0"_s, mutated([](RawHeader& header) { header.buildIDs[0].length = 0; }));
    expectCorrupt("an ID of length 65"_s, mutated([](RawHeader& header) { header.buildIDs[0].length = 65; }));
    expectCorrupt("a record's u16 not zero"_s, mutated([](RawHeader& header) { header.buildIDs[0].reserved16 = 1; }));
    expectCorrupt("a record's u32 not zero"_s, mutated([](RawHeader& header) { header.buildIDs[0].reserved32 = 0x100; }));
    expectCorrupt("a byte after the ID not zero"_s, mutated([](RawHeader& header) { header.buildIDs[0].id[20] = 1; }));
    expectCorrupt("the last byte of the ID field not zero"_s, mutated([](RawHeader& header) { header.buildIDs[0].id[63] = 1; }));
    expectCorrupt("option records out of order"_s, mutated([](RawHeader& header) {
        header.options[0].index = 1;
        header.options[1].index = 0;
    }));
    expectCorrupt("an option index of 3"_s, mutated([](RawHeader& header) { header.options[2].index = 3; }));
    expectCorrupt("an option type of 0"_s, mutated([](RawHeader& header) { header.options[0].type = 0; }));
    expectCorrupt("an option type of 2"_s, mutated([](RawHeader& header) { header.options[1].type = 2; }));
    expectCorrupt("an option value of 2"_s, mutated([](RawHeader& header) { header.options[2].value = 2; }));
    expectCorrupt("an option record's u32 not zero"_s, mutated([](RawHeader& header) { header.options[1].reserved = 1; }));
    expectCorrupt("bytes 28 to 31 not zero"_s, mutated([](RawHeader& header) { header.reserved = 0x1000000; }));
    expectCorrupt("the last four bytes not zero"_s, mutated([](RawHeader& header) { header.trailer = 1; }));
    expectCorrupt("another CRC"_s, mutated([&](RawHeader& header) { header.crc = static_cast<uint32_t>(getField(processHeader.span(), 128, 4)) ^ 1; }));
    {
        Vector<uint8_t> flipped = processHeader;
        flipped[40] ^= 0x01; // a byte of the main executable's ID, covered by the CRC alone
        expectVerdict("a flipped ID byte"_s, flipped.span(), HeaderVerdict::Corrupt);
    }

    expectVerdict("layout version 2"_s, encodeRawHeader(mutated([](RawHeader& header) { header.version = 2; })).span(), HeaderVerdict::Incompatible);
    expectVerdict("layout version 0"_s, encodeRawHeader(mutated([](RawHeader& header) { header.version = 0; })).span(), HeaderVerdict::Incompatible);
    RawHeader otherVersionWithBadFields = mutated([](RawHeader& header) {
        header.version = 2;
        header.buildIDs.clear();
        header.optionCount = 9;
        header.crc = 0;
    });
    expectVerdict("layout version 2 with fields this version rejects"_s, encodeRawHeader(otherVersionWithBadFields).span(), HeaderVerdict::Incompatible);
    RawHeader otherVersionWithoutTag = mutated([](RawHeader& header) {
        header.version = 2;
        header.tag[0] = 'X';
    });
    expectVerdict("layout version 2 without the tag"_s, encodeRawHeader(otherVersionWithoutTag).span(), HeaderVerdict::Corrupt);
    auto otherVersion = checkArtifactHeader(encodeRawHeader(mutated([](RawHeader& header) { header.version = 2; })).span(), processHeader.span());
    JITCACHE_CHECK(otherVersion.detail == "layout version"_s);
}

// C2. Each differing field makes the header incompatible, with the detail naming that field.
JITCACHE_TEST(containerHeaderIncompatible, No)
{
    auto expectField = [&](ASCIILiteral label, const ArtifactHeader& artifact, const ArtifactHeader& process, const String& field) {
        auto artifactBytes = encodedHeader(artifact);
        auto copy = exactCopy(artifactBytes.span());
        auto check = checkArtifactHeader(copy.span(), encodedHeader(process).span());
        if (check.verdict != HeaderVerdict::Incompatible || check.detail != field) {
            JITCACHE_FAIL(makeString(label, ": "_s, verdictName(check.verdict), " naming \""_s, check.detail, "\", expected Incompatible naming \""_s,
                field, '"'));
        }
    };

    ArtifactHeader process = craftedHeader(headerX86_64, false);
    {
        auto check = checkArtifactHeader(encodedHeader(process).span(), encodedHeader(process).span());
        JITCACHE_CHECK(check.verdict == HeaderVerdict::Compatible && check.detail.isEmpty());
    }

    ArtifactHeader artifact = process;
    artifact.architecture = headerARM64;
    expectField("the architecture"_s, artifact, process, "architecture"_s);

    artifact = process;
    artifact.facts.mainExecutable.bytes[5] ^= 0x80;
    expectField("a main executable ID byte"_s, artifact, process, "main executable build ID"_s);
    artifact = process;
    artifact.facts.mainExecutable.bytes[19] = 0;
    artifact.facts.mainExecutable.size = 19;
    expectField("a shorter main executable ID"_s, artifact, process, "main executable build ID"_s);

    artifact = process;
    artifact.facts.engineObject = craftedBuildID(20, 0x70);
    expectField("an engine object the process lacks"_s, artifact, process, "engine object build ID"_s);
    expectField("an engine object the artifact lacks"_s, process, artifact, "engine object build ID"_s);
    ArtifactHeader otherEngine = artifact;
    otherEngine.facts.engineObject->bytes[0] ^= 1;
    expectField("an engine object ID byte"_s, otherEngine, artifact, "engine object build ID"_s);

    static constexpr std::array<ASCIILiteral, 10> x86Names {
        "supportsSSE3"_s,
        "supportsSupplementalSSE3"_s,
        "supportsSSE4_1"_s,
        "supportsFloatingPointRounding"_s,
        "supportsCountPopulation"_s,
        "supportsAVX"_s,
        "supportsAVX2"_s,
        "supportsLZCNT"_s,
        "supportsBMI1"_s,
        "supportsFloat16"_s,
    };
    for (unsigned bit = 0; bit < x86Names.size(); ++bit) {
        artifact = process;
        artifact.facts.cpuFeatures ^= uint64_t { 1 } << bit;
        expectField("an x86_64 CPU feature bit"_s, artifact, process, x86Names[bit]);
    }
    for (unsigned bit : { 10u, 40u, 63u }) {
        artifact = process;
        artifact.facts.cpuFeatures ^= uint64_t { 1 } << bit;
        expectField("an unused x86_64 CPU feature bit"_s, artifact, process, makeString("CPU feature bit "_s, bit));
    }

    static constexpr std::array<ASCIILiteral, 8> arm64Names {
        "supportsFloatingPointRounding"_s,
        "supportsCountPopulation"_s,
        "supportsFloat16"_s,
        "supportsDotProd"_s,
        "supportsLSE"_s,
        "supportsDoubleToInt32ConversionUsingJavaScriptSemantics"_s,
        "supportsRoundFloatToIntegerFloat"_s,
        "supportsSHA3"_s,
    };
    ArtifactHeader armProcess = craftedHeader(headerARM64, false);
    for (unsigned bit = 0; bit < arm64Names.size(); ++bit) {
        artifact = armProcess;
        artifact.facts.cpuFeatures ^= uint64_t { 1 } << bit;
        expectField("an ARM64 CPU feature bit"_s, artifact, armProcess, arm64Names[bit]);
    }
    artifact = armProcess;
    artifact.facts.cpuFeatures ^= uint64_t { 1 } << 8;
    expectField("an unused ARM64 CPU feature bit"_s, artifact, armProcess, "CPU feature bit 8"_s);

    static constexpr std::array<ASCIILiteral, 3> optionNames { "evalMode"_s, "useExplicitResourceManagement"_s, "useImportDefer"_s };
    for (size_t index = 0; index < optionNames.size(); ++index) {
        artifact = process;
        artifact.facts.mustMatch[index] = !artifact.facts.mustMatch[index];
        expectField("a must-match option"_s, artifact, process, optionNames[index]);
    }

    // The fields are named in the order architecture, build IDs, CPU features, options.
    artifact = process;
    artifact.facts.cpuFeatures ^= 1 << 3;
    artifact.facts.mustMatch[0] = !artifact.facts.mustMatch[0];
    expectField("a CPU feature and an option"_s, artifact, process, "supportsFloatingPointRounding"_s);
    artifact.facts.mainExecutable.bytes[0] ^= 1;
    expectField("a build ID, a CPU feature and an option"_s, artifact, process, "main executable build ID"_s);
    artifact.architecture = headerARM64;
    expectField("every field"_s, artifact, process, "architecture"_s);
    artifact = process;
    artifact.facts.cpuFeatures ^= (1 << 6) | (1 << 2);
    expectField("two CPU features"_s, artifact, process, "supportsSSE4_1"_s);
}

// The header digest (section 3.3), and the process's own header.
JITCACHE_TEST(containerHeaderDigest, No)
{
    Vector<uint8_t> header = encodedHeader(craftedHeader(headerX86_64, true));
    HeaderDigest digest = artifactHeaderDigest(header.span());
    Digest256 sha = SHA256::hash(header.span());
    JITCACHE_CHECK(equalSpans(std::span { digest }, std::span { sha }.first(digest.size())));

    Vector<uint8_t> other = encodedHeader(craftedHeader(headerX86_64, false));
    JITCACHE_CHECK(artifactHeaderDigest(other.span()) != digest);
    JITCACHE_CHECK(artifactHeaderDigest(encodedHeader(craftedHeader(headerX86_64, true)).span()) == digest);

    // T-BUILDID checks that the test executable has a build ID; without one, start never computes the process's header.
    ProcessFacts facts = processFacts();
    if (!facts.mainExecutable.size || (facts.engineObject && !facts.engineObject->size))
        return;
    auto& expected = expectedHeader();
    JITCACHE_CHECK(&expected == &expectedHeader());
    auto parsed = readArtifactHeader(expected.span());
    if (!parsed) {
        JITCACHE_FAIL(makeString("the process's header does not parse: "_s, parsed.error().reason));
        return;
    }
    JITCACHE_CHECK(parsed->architecture == thisHeaderArchitecture());
    JITCACHE_CHECK(equalContents(*parsed, ArtifactHeader { thisHeaderArchitecture(), facts }));
    JITCACHE_CHECK(checkArtifactHeader(expected.span(), expected.span()).verdict == HeaderVerdict::Compatible);
}

// C3. The body's encoders lay a body out as sections 4.1 to 4.3 do.
JITCACHE_TEST(containerBodyEncoding, No)
{
    Fixture fixture = makeFixture(sectionsWithout());
    JITCACHE_CHECK(fixture.file.size() < maximumBodyFramingBytes);

    // The test builder, which the writer's encoders back, writes the bytes this file writes by hand.
    Vector<TestSection> sections = sectionsWithout();
    Vector<ContainerTesting::Section> sources;
    for (auto& section : sections)
        sources.append(ContainerTesting::Section { section.kind, section.bytes.span() });
    BodyEnvelope stamp;
    stamp.key = fixture.key;
    stamp.headerDigest = fixture.headerDigest;
    stamp.version = fixture.body.version;
    stamp.highestTier = 1;
    stamp.llintThreshold = fixture.body.llintThreshold;
    stamp.counterProgress = fixture.body.counterProgress;
    Vector<uint8_t> built = ContainerTesting::buildBody(stamp, sources.span());
    JITCACHE_CHECK(equalSpans(built.span(), fixture.file.span()));

    // Section 4.3's offsets.
    std::array<BodyDirectoryEntry, numberOfSectionKinds> entries { };
    for (size_t index = 0; index < entries.size(); ++index) {
        entries[index].kind = sections[index].kind;
        entries[index].size = sections[index].bytes.size();
    }
    auto fileSize = layOutBodySections(std::span { entries });
    JITCACHE_CHECK(fileSize == fixture.body.fileSize);
    for (size_t index = 0; index < entries.size(); ++index)
        JITCACHE_CHECK(entries[index].offset == fixture.body.entries[index].offset);
    JITCACHE_CHECK(entries[0].offset == 128 + 24 * numberOfSectionKinds);
    for (auto& entry : entries)
        JITCACHE_CHECK(!(entry.offset % 8));

    // The directory's bytes and CRC.
    for (size_t index = 0; index < entries.size(); ++index)
        entries[index].crc = crc32cOf(sections[index].bytes.span());
    std::array<uint8_t, 24 * numberOfSectionKinds> directory { };
    uint32_t directoryCRC = encodeBodyDirectory(std::span { entries }, std::span { directory });
    JITCACHE_CHECK(directoryCRC == crc32cOf(std::span { directory }));
    JITCACHE_CHECK(equalSpans(std::span { directory }, fixture.file.span().subspan(128, directory.size())));
    JITCACHE_CHECK(directoryCRC == getField(fixture.file.span(), 96, 4));

    // The envelope round-trips through its check.
    BodyEnvelope envelope = stamp;
    envelope.sectionCount = numberOfSectionKinds;
    envelope.fileSize = fixture.body.fileSize;
    envelope.directoryCRC = directoryCRC;
    auto envelopeBytes = encodeBodyEnvelope(envelope);
    JITCACHE_CHECK(equalSpans(std::span { envelopeBytes }, fixture.file.span().first(128)));
    auto decoded = validateBodyEnvelope(std::span { envelopeBytes }, fixture.body.fileSize, fixture.key, fixture.headerDigest, ValidationMode::Full);
    if (!decoded)
        JITCACHE_FAIL(makeString("the encoded envelope fails "_s, decoded.error()));
    else {
        JITCACHE_CHECK(decoded->sectionCount == numberOfSectionKinds);
        JITCACHE_CHECK(decoded->highestTier == 1);
        JITCACHE_CHECK(decoded->key == fixture.key);
        JITCACHE_CHECK(decoded->version == fixture.body.version);
        JITCACHE_CHECK(decoded->headerDigest == fixture.headerDigest);
        JITCACHE_CHECK(decoded->fileSize == fixture.body.fileSize);
        JITCACHE_CHECK(decoded->llintThreshold == fixture.body.llintThreshold);
        JITCACHE_CHECK(decoded->counterProgress == fixture.body.counterProgress);
        JITCACHE_CHECK(decoded->directoryCRC == directoryCRC);
    }

    // Layouts that cannot exist.
    JITCACHE_CHECK(!layOutBodySections({ }));
    std::array<BodyDirectoryEntry, maximumBodySections + 1> tooMany { };
    JITCACHE_CHECK(!layOutBodySections(std::span { tooMany }));
    JITCACHE_CHECK(layOutBodySections(std::span { tooMany }.first(maximumBodySections)) == maximumBodyFramingBytes);
    std::array<BodyDirectoryEntry, 1> huge { };
    huge[0].size = std::numeric_limits<uint64_t>::max() - 152;
    JITCACHE_CHECK(layOutBodySections(std::span { huge }) == std::numeric_limits<uint64_t>::max());
    huge[0].size = std::numeric_limits<uint64_t>::max() - 151;
    JITCACHE_CHECK(!layOutBodySections(std::span { huge }));
    std::array<BodyDirectoryEntry, 2> unaligned { };
    unaligned[0].size = std::numeric_limits<uint64_t>::max() - 176 - 3;
    unaligned[1].size = 0;
    JITCACHE_CHECK(!layOutBodySections(std::span { unaligned }));
}

// C3. A body of every kind validates in both modes, and each check B1 to B8 fails exactly when its own condition is broken,
// with its own name; a body broken only in a structure part validates in Integrity mode. Bodies that break two checks
// name the first.
JITCACHE_TEST(containerBodyChecks, No)
{
    WeakRandom random(randomSeed);
    Fixture fixture = makeFixture(sectionsWithout());
    auto& key = fixture.key;
    auto& digest = fixture.headerDigest;
    auto& body = fixture.body;
    auto& file = fixture.file;
    uint64_t fileSize = file.size();
    JITCACHE_CHECK(fileSize < maximumBodyFramingBytes);

    auto expect = [&](ASCIILiteral label, std::span<const uint8_t> bytes, Outcome full, Outcome integrity) {
        expectOutcomes(context, String { label }, bytes, key, digest, full, integrity, random);
    };
    auto mutated = [&](auto&& mutate) {
        Vector<uint8_t> copy = file;
        mutate(copy);
        return copy;
    };
    auto sectionByte = [&](SectionKind kind, size_t index) {
        return static_cast<size_t>(body.entries[static_cast<size_t>(kind)].offset) + index;
    };
    // Validates the file as mutate changes it.
    auto expectChanged = [&](ASCIILiteral label, Outcome full, Outcome integrity, auto&& mutate) {
        expect(label, mutated(mutate).span(), full, integrity);
    };

    // A body of every kind, and its layout.
    expect("a body of every kind"_s, file.span(), accepted, accepted);
    for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
        auto layout = validateBody(file.span(), key, digest, mode);
        if (!layout) {
            JITCACHE_FAIL(makeString("a body of every kind fails "_s, layout.error()));
            continue;
        }
        JITCACHE_CHECK(layout->version == body.version);
        JITCACHE_CHECK(layout->highestTier == 1);
        JITCACHE_CHECK(layout->llintThreshold == body.llintThreshold);
        JITCACHE_CHECK(layout->counterProgress == body.counterProgress);
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            auto& extent = layout->sections[index];
            JITCACHE_CHECK(extent && extent->offset == body.entries[index].offset && extent->size == body.entries[index].size);
        }
    }

    // B1.
    expect("an empty file"_s, { }, sizeCheck, sizeCheck);
    expect("127 bytes"_s, file.span().first(127), sizeCheck, sizeCheck);
    expectChanged("an envelope file size one too large"_s, sizeCheck, sizeCheck, [&](auto& bytes) {
        putField(bytes, 80, fileSize + 1, 8);
        resealEnvelope(bytes);
    });
    expectChanged("one byte past the envelope's file size"_s, sizeCheck, sizeCheck, [](auto& bytes) {
        bytes.append(0);
    });

    // B2, integrity.
    expectChanged("another tag"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        bytes[2] ^= 1;
        resealEnvelope(bytes);
    });
    expectChanged("layout version 2"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        putField(bytes, 8, 2, 2);
        resealEnvelope(bytes);
    });
    expectChanged("an envelope size of 127"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        putField(bytes, 10, 127, 2);
        resealEnvelope(bytes);
    });
    expectChanged("no sections"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        putField(bytes, 12, 0, 2);
        resealEnvelope(bytes);
    });
    expectChanged("65 sections"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        putField(bytes, 12, 65, 2);
        resealEnvelope(bytes);
    });
    expectChanged("another envelope CRC"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        bytes[121] ^= 1;
    });
    expectChanged("a damaged L"_s, envelopeCheck, envelopeCheck, [](auto& bytes) {
        bytes[88] ^= 1;
    });

    // B2, structure.
    expectChanged("a nonzero byte 15"_s, envelopeCheck, accepted, [](auto& bytes) {
        bytes[15] = 1;
        resealEnvelope(bytes);
    });
    expectChanged("a nonzero byte among 100 to 119"_s, envelopeCheck, accepted, [](auto& bytes) {
        bytes[110] = 1;
        resealEnvelope(bytes);
    });
    expectChanged("a nonzero byte among 124 to 127"_s, envelopeCheck, accepted, [](auto& bytes) {
        bytes[126] = 1;
    });
    expectChanged("a highest tier of 2"_s, envelopeCheck, accepted, [](auto& bytes) {
        bytes[14] = 2;
        resealEnvelope(bytes);
    });
    expectChanged("a highest tier of 0"_s, envelopeCheck, accepted, [](auto& bytes) {
        bytes[14] = 0;
        resealEnvelope(bytes);
    });

    // B3.
    expectOutcomes(context, "another expected key"_s, file.span(), testKey(0x21), digest, keyCheck, keyCheck, random);
    expectChanged("another key in the envelope"_s, keyCheck, keyCheck, [](auto& bytes) {
        bytes[16 + 30] ^= 1;
        resealEnvelope(bytes);
    });

    // B4.
    expectOutcomes(context, "another header digest"_s, file.span(), key, testDigest(0xa1), headerDigestCheck, headerDigestCheck, random);

    // B5.
    expectChanged("a zero commit identifier"_s, versionCheck, accepted, [](auto& bytes) {
        putField(bytes, 56, 0, 8);
        resealEnvelope(bytes);
    });

    // B6, integrity.
    expectChanged("another directory CRC"_s, directoryCheck, directoryCheck, [](auto& bytes) {
        bytes[97] ^= 1;
        resealEnvelope(bytes);
    });
    expectChanged("a damaged directory byte"_s, directoryCheck, directoryCheck, [](auto& bytes) {
        bytes[entryAt(0) + 4] ^= 1;
    });
    expectChanged("a directory past the end of the file"_s, directoryCheck, directoryCheck, [](auto& bytes) {
        putField(bytes, 12, maximumBodySections, 2);
        resealEnvelope(bytes);
    });
    expectChanged("a section offset past the end of the file"_s, directoryCheck, directoryCheck, [&](auto& bytes) {
        putField(bytes, entryAt(2) + 8, fileSize + 8, 8);
        resealDirectory(bytes);
    });
    expectChanged("a section past the end of the file"_s, directoryCheck, directoryCheck, [&](auto& bytes) {
        putField(bytes, entryAt(3) + 16, fileSize, 8);
        resealDirectory(bytes);
    });
    expectChanged("a section whose end overflows"_s, directoryCheck, directoryCheck, [](auto& bytes) {
        putField(bytes, entryAt(4) + 8, 8, 8);
        putField(bytes, entryAt(4) + 16, std::numeric_limits<uint64_t>::max(), 8);
        resealDirectory(bytes);
    });
    expectChanged("a section whose offset is near 2^64"_s, directoryCheck, directoryCheck, [](auto& bytes) {
        putField(bytes, entryAt(0) + 8, std::numeric_limits<uint64_t>::max() - 7, 8);
        putField(bytes, entryAt(0) + 16, 16, 8);
        resealDirectory(bytes);
    });

    // B6, structure.
    expectChanged("a nonzero entry byte"_s, directoryCheck, accepted, [](auto& bytes) {
        bytes[entryAt(1) + 3] = 1;
        resealDirectory(bytes);
    });
    expectChanged("an unknown section kind"_s, directoryCheck, accepted, [](auto& bytes) {
        putField(bytes, entryAt(8), 0x0501, 2);
        resealDirectory(bytes);
    });
    expectChanged("a kind's type id with another tier"_s, directoryCheck, accepted, [](auto& bytes) {
        bytes[entryAt(0) + 2] = 1;
        resealDirectory(bytes);
    });
    expectChanged("entries out of order"_s, directoryCheck, accepted, [](auto& bytes) {
        for (size_t index = 0; index < 24; ++index)
            std::swap(bytes[entryAt(3) + index], bytes[entryAt(4) + index]);
        resealDirectory(bytes);
    });
    expectChanged("a kind listed twice"_s, directoryCheck, accepted, [](auto& bytes) {
        putField(bytes, entryAt(5), 0x0202, 2);
        resealDirectory(bytes);
    });
    {
        RawBody shifted = body;
        for (size_t index = 3; index < shifted.entries.size(); ++index) {
            shifted.entries[index].offset += 8;
            shifted.placements[index].offset += 8;
        }
        shifted.fileSize += 8;
        expect("a section 8 bytes past its offset"_s, encodeRawBody(shifted).span(), directoryCheck, accepted);
    }
    {
        RawBody shifted = body;
        for (size_t index = 0; index < shifted.entries.size(); ++index) {
            shifted.entries[index].offset += 8;
            shifted.placements[index].offset += 8;
        }
        shifted.fileSize += 8;
        expect("a gap after the directory"_s, encodeRawBody(shifted).span(), directoryCheck, accepted);
    }
    {
        RawBody longer = body;
        longer.fileSize += 8;
        expect("bytes after the last section"_s, encodeRawBody(longer).span(), directoryCheck, accepted);
    }
    expectChanged("a nonzero padding byte"_s, directoryCheck, accepted, [&](auto& bytes) {
        bytes[firstPaddingByte(body)] = 1;
    });
    expectChanged("a nonzero last padding byte"_s, directoryCheck, accepted, [&](auto& bytes) {
        bytes[static_cast<size_t>(body.entries[static_cast<size_t>(SectionKind::ICsBaseline)].offset) - 1] = 0x80;
    });

    // B7.
    for (SectionKind omitted : { SectionKind::UCBCore, SectionKind::ImageTwinsBaseline, SectionKind::ICsBaseline }) {
        Fixture without = makeFixture(sectionsWithout(omitted));
        String label = makeString("a body without "_s, rowOf(omitted).name);
        expectOutcomes(context, label, without.file.span(), without.key, without.headerDigest, requiredCheck, accepted, random);
    }

    // B8.
    expectChanged("a damaged image.baseline byte"_s, checksumCheck, checksumCheck, [&](auto& bytes) {
        bytes[sectionByte(SectionKind::ImageBaseline, 33)] ^= 0x10;
    });
    expectChanged("a damaged last byte"_s, checksumCheck, checksumCheck, [](auto& bytes) {
        bytes.last() ^= 1;
    });
    expectChanged("another entry CRC"_s, checksumCheck, checksumCheck, [](auto& bytes) {
        bytes[entryAt(6) + 4] ^= 1;
        resealDirectory(bytes);
    });
    expectChanged("an empty section given a size"_s, directoryCheck, checksumCheck, [](auto& bytes) {
        putField(bytes, entryAt(2) + 16, 1, 8);
        resealDirectory(bytes);
    });

    // The first failing check is named.
    {
        Vector<uint8_t> damaged = mutated([&](auto& bytes) {
            bytes[sectionByte(SectionKind::UCBIdentity, 0)] ^= 1;
        });
        expectOutcomes(context, "another expected key and a damaged section"_s, damaged.span(), testKey(0x22), digest, keyCheck, keyCheck, random);
    }
    {
        Fixture without = makeFixture(sectionsWithout(SectionKind::UCBCore));
        Vector<uint8_t> damaged = without.file;
        damaged[static_cast<size_t>(without.body.entries[0].offset)] ^= 1;
        expectOutcomes(context, "a missing section and a damaged one"_s, damaged.span(), without.key, without.headerDigest, requiredCheck, checksumCheck, random);
        Vector<uint8_t> padded = without.file;
        padded[firstPaddingByte(without.body)] = 1;
        expectOutcomes(context, "a missing section and nonzero padding"_s, padded.span(), without.key, without.headerDigest, directoryCheck, accepted, random);
    }
    expectChanged("a highest tier of 2 and a damaged section"_s, envelopeCheck, checksumCheck, [&](auto& bytes) {
        bytes[14] = 2;
        resealEnvelope(bytes);
        bytes[sectionByte(SectionKind::CBStateBaseline, 0)] ^= 1;
    });
    expectChanged("a zero commit identifier and another directory CRC"_s, versionCheck, directoryCheck, [](auto& bytes) {
        putField(bytes, 56, 0, 8);
        bytes[96] ^= 1;
        resealEnvelope(bytes);
    });
    expectChanged("nonzero padding and a damaged section"_s, directoryCheck, checksumCheck, [&](auto& bytes) {
        bytes[firstPaddingByte(body)] = 1;
        bytes[sectionByte(SectionKind::ICsBaseline, 0)] ^= 1;
    });
    expectChanged("a short file with another tag"_s, sizeCheck, sizeCheck, [](auto& bytes) {
        bytes[0] ^= 1;
        bytes.removeLast();
    });
}

// C3. Integrity mode checks every entry's CRC wherever the entry points, envelope and directory included, and the layout
// records the first entry of each kind, ignoring the kinds it does not know.
JITCACHE_TEST(containerBodyIntegrityEntries, No)
{
    WeakRandom random(randomSeed);
    Vector<TestSection> sections = sectionsWithout();
    BodyKey key = testKey(0x30);
    HeaderDigest digest = testDigest(0xb0);
    static constexpr size_t extraEntries = 4;
    RawBody body = canonicalBody(key, digest, sections, sections.size() + extraEntries);
    RawEntry image = body.entries[static_cast<size_t>(SectionKind::ImageBaseline)];
    RawEntry state = body.entries[static_cast<size_t>(SectionKind::CBStateBaseline)];
    // ucb.core again, over image.baseline's bytes.
    body.entries.append(RawEntry { 0x0102, 0, 0, image.offset, image.size });
    // A kind no table lists, over the envelope's first 16 bytes.
    body.entries.append(RawEntry { 0x0999, 7, 0, 0, 16 });
    // cb.state again, from the middle of image.baseline into cb.state, across three sections and their padding.
    body.entries.append(RawEntry { 0x0301, 1, 0, image.offset + 32, state.offset + 8 - (image.offset + 32) });
    // A kind no table lists, over the first directory entry.
    body.entries.append(RawEntry { 0x0102, 9, 0, 128, 24 });
    Vector<uint8_t> file = encodeRawBody(body);

    expectOutcomes(context, "overlapping, repeated and unknown entries"_s, file.span(), key, digest, directoryCheck, accepted, random);
    auto layout = validateBody(file.span(), key, digest, ValidationMode::Integrity);
    if (!layout)
        JITCACHE_FAIL(makeString("overlapping, repeated and unknown entries fail "_s, layout.error()));
    else {
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            auto& extent = layout->sections[index];
            JITCACHE_CHECK(extent && extent->offset == body.entries[index].offset && extent->size == body.entries[index].size);
        }
    }

    for (size_t extra = 0; extra < extraEntries; ++extra) {
        Vector<uint8_t> damaged = file;
        damaged[entryAt(numberOfSectionKinds + extra) + 4] ^= 1;
        resealDirectory(damaged);
        expectOutcomes(context, makeString("an extra entry with another CRC, "_s, extra), damaged.span(), key, digest, directoryCheck, checksumCheck, random);
    }
    Vector<uint8_t> damagedEnvelope = file;
    damagedEnvelope[1] ^= 1; // inside the unknown entry's range and the envelope's CRC
    resealEnvelope(damagedEnvelope);
    expectOutcomes(context, "a resealed tag change under an entry"_s, damagedEnvelope.span(), key, digest, envelopeCheck, envelopeCheck, random);
    Vector<uint8_t> damagedImage = file;
    damagedImage[static_cast<size_t>(image.offset + 40)] ^= 1; // under image.baseline and both repeated entries
    expectOutcomes(context, "a damaged byte under three entries"_s, damagedImage.span(), key, digest, directoryCheck, checksumCheck, random);
}

// C3. Truncation at every region boundary fails in both modes, as does a stream that receives fewer or more bytes than
// its file's size.
JITCACHE_TEST(containerBodyTruncation, No)
{
    WeakRandom random(randomSeed);
    Fixture fixture = makeFixture(sectionsWithout());
    auto& file = fixture.file;

    Vector<size_t> boundaries { 0, 1, 8, 10, 12, 14, 16, 56, 64, 80, 88, 96, 100, 120, 124, 127, 128 };
    for (size_t index = 0; index <= numberOfSectionKinds; ++index)
        boundaries.append(entryAt(index));
    for (auto& entry : fixture.body.entries) {
        boundaries.append(static_cast<size_t>(entry.offset));
        boundaries.append(static_cast<size_t>(entry.offset + entry.size));
    }
    for (size_t boundary : boundaries) {
        for (size_t length : { boundary - 1, boundary, boundary + 1 }) {
            if (length >= file.size())
                continue;
            expectOutcomes(context, makeString("the first "_s, length, " bytes"_s), file.span().first(length), fixture.key, fixture.headerDigest, sizeCheck, sizeCheck, random);
        }
    }

    for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
        for (size_t boundary : boundaries) {
            if (boundary >= file.size())
                continue;
            BodyValidationStream stream(fixture.key, fixture.headerDigest, mode, file.size());
            stream.append(file.span().first(boundary));
            JITCACHE_CHECK(outcomeOf(stream.finish()) == sizeCheck);
        }
        BodyValidationStream longer(fixture.key, fixture.headerDigest, mode, file.size());
        longer.append(file.span());
        std::array<uint8_t, 1> extra { };
        longer.append(std::span { extra });
        JITCACHE_CHECK(outcomeOf(longer.finish()) == sizeCheck);
        BodyValidationStream shorter(fixture.key, fixture.headerDigest, mode, file.size() - 1);
        shorter.append(file.span());
        JITCACHE_CHECK(outcomeOf(shorter.finish()) == sizeCheck);
    }
}

// The reads that check part of a body: the envelope alone (maintenance) and the scoring read's framing with three
// sections' checksums.
JITCACHE_TEST(containerBodyPartialReads, No)
{
    Fixture fixture = makeFixture(sectionsWithout());
    auto& key = fixture.key;
    auto& digest = fixture.headerDigest;
    auto& body = fixture.body;
    auto& file = fixture.file;
    auto mutated = [&](auto&& mutate) {
        Vector<uint8_t> copy = file;
        mutate(copy);
        return copy;
    };
    auto sectionByte = [&](SectionKind kind, size_t index) {
        return static_cast<size_t>(body.entries[static_cast<size_t>(kind)].offset) + index;
    };
    static constexpr std::array<SectionKind, 3> summaries { SectionKind::UCBFeedback, SectionKind::CBSummaryBaseline, SectionKind::ICsBaseline };

    auto envelopeOutcome = [&](std::span<const uint8_t> fileStart, uint64_t fileSize, const BodyKey& expectedKey, const HeaderDigest& expectedDigest, ValidationMode mode) -> Outcome {
        auto copy = exactCopy(fileStart);
        auto result = validateBodyEnvelope(copy.span(), fileSize, expectedKey, expectedDigest, mode);
        if (result)
            return std::nullopt;
        return result.error();
    };
    auto framingOutcome = [&](std::span<const uint8_t> bytes, ValidationMode mode, std::span<const SectionKind> kinds) -> Outcome {
        auto copy = exactCopy(bytes);
        auto result = validateBodyFraming(copy.span(), key, digest, mode, kinds);
        if (result)
            return std::nullopt;
        return result.error();
    };

    // The envelope alone.
    for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
        auto envelope = validateBodyEnvelope(file.span().first(128), file.size(), key, digest, mode);
        JITCACHE_CHECK(envelope && envelope->version == body.version && envelope->llintThreshold == body.llintThreshold
            && envelope->counterProgress == body.counterProgress && envelope->fileSize == file.size() && envelope->sectionCount == numberOfSectionKinds);
        JITCACHE_CHECK(envelopeOutcome(file.span(), file.size(), key, digest, mode) == accepted);
        JITCACHE_CHECK(envelopeOutcome(file.span().first(100), file.size(), key, digest, mode) == sizeCheck);
        JITCACHE_CHECK(envelopeOutcome(file.span().first(100), 100, key, digest, mode) == sizeCheck);
        JITCACHE_CHECK(envelopeOutcome(file.span().first(128), file.size() + 1, key, digest, mode) == sizeCheck);
        JITCACHE_CHECK(envelopeOutcome(file.span().first(128), file.size(), testKey(0x23), digest, mode) == keyCheck);
        JITCACHE_CHECK(envelopeOutcome(file.span().first(128), file.size(), key, testDigest(0xa2), mode) == headerDigestCheck);
        auto badTag = mutated([](auto& bytes) {
            bytes[0] ^= 1;
            resealEnvelope(bytes);
        });
        JITCACHE_CHECK(envelopeOutcome(badTag.span().first(128), file.size(), key, digest, mode) == envelopeCheck);
        auto nonzero = mutated([](auto& bytes) {
            bytes[15] = 1;
            resealEnvelope(bytes);
        });
        JITCACHE_CHECK(envelopeOutcome(nonzero.span().first(128), file.size(), key, digest, mode) == (mode == ValidationMode::Full ? envelopeCheck : accepted));
        auto zeroVersion = mutated([](auto& bytes) {
            putField(bytes, 56, 0, 8);
            resealEnvelope(bytes);
        });
        JITCACHE_CHECK(envelopeOutcome(zeroVersion.span().first(128), file.size(), key, digest, mode) == (mode == ValidationMode::Full ? versionCheck : accepted));
        // The directory is not the envelope's to check.
        auto badDirectory = mutated([](auto& bytes) { bytes[entryAt(0) + 4] ^= 1; });
        JITCACHE_CHECK(envelopeOutcome(badDirectory.span().first(128), file.size(), key, digest, mode) == accepted);
    }

    // The framing and three sections' checksums.
    for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
        auto layout = validateBodyFraming(file.span(), key, digest, mode, std::span { summaries });
        auto full = validateBody(file.span(), key, digest, mode);
        JITCACHE_CHECK(layout && full && layout->version == full->version && layout->sections == full->sections);

        auto damagedImage = mutated([&](auto& bytes) { bytes[sectionByte(SectionKind::ImageBaseline, 0)] ^= 1; });
        JITCACHE_CHECK(framingOutcome(damagedImage.span(), mode, std::span { summaries }) == accepted);
        auto damagedSummary = mutated([&](auto& bytes) { bytes[sectionByte(SectionKind::CBSummaryBaseline, 2)] ^= 1; });
        JITCACHE_CHECK(framingOutcome(damagedSummary.span(), mode, std::span { summaries }) == checksumCheck);
        auto damagedICs = mutated([&](auto& bytes) { bytes.last() ^= 1; });
        JITCACHE_CHECK(framingOutcome(damagedICs.span(), mode, std::span { summaries }) == checksumCheck);
        JITCACHE_CHECK(framingOutcome(damagedICs.span(), mode, { }) == accepted);
        auto padded = mutated([&](auto& bytes) { bytes[firstPaddingByte(body)] = 1; });
        JITCACHE_CHECK(framingOutcome(padded.span(), mode, std::span { summaries }) == accepted);
        auto beyond = mutated([](auto& bytes) {
            putField(bytes, 12, maximumBodySections, 2);
            resealEnvelope(bytes);
        });
        JITCACHE_CHECK(framingOutcome(beyond.span(), mode, std::span { summaries }) == directoryCheck);
        auto outOfOrder = mutated([](auto& bytes) {
            for (size_t index = 0; index < 24; ++index)
                std::swap(bytes[entryAt(3) + index], bytes[entryAt(4) + index]);
            resealDirectory(bytes);
        });
        JITCACHE_CHECK(framingOutcome(outOfOrder.span(), mode, std::span { summaries }) == (mode == ValidationMode::Full ? directoryCheck : accepted));
        JITCACHE_CHECK(framingOutcome(file.span().first(file.size() - 1), mode, std::span { summaries }) == sizeCheck);

        Fixture without = makeFixture(sectionsWithout(SectionKind::UCBCore));
        auto copy = exactCopy(without.file.span());
        auto missing = validateBodyFraming(copy.span(), without.key, without.headerDigest, mode, std::span { summaries });
        JITCACHE_CHECK(outcomeOf(missing) == (mode == ValidationMode::Full ? requiredCheck : accepted));
    }
}

// C3. No malformed input reads out of bounds, in any form or mode, and the stream agrees with validateBody on all of them.
JITCACHE_TEST(containerBodyFuzz, No)
{
    WeakRandom random(randomSeed);
    Fixture fixture = makeFixture(sectionsWithout());
    auto& file = fixture.file;
    static constexpr std::array<SectionKind, 3> summaries { SectionKind::UCBFeedback, SectionKind::CBSummaryBaseline, SectionKind::ICsBaseline };
    auto interestingValue = [&]() -> uint64_t {
        switch (random.getUint32(6)) {
        case 0:
            return 0;
        case 1:
            return file.size();
        case 2:
            return file.size() - 1;
        case 3:
            return std::numeric_limits<uint64_t>::max();
        case 4:
            return std::numeric_limits<uint64_t>::max() - 7;
        default:
            return random.getUint32(static_cast<unsigned>(file.size() + 16));
        }
    };

    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        Vector<uint8_t> bytes = file;
        switch (random.getUint32(5)) {
        case 0:
            for (unsigned flips = 1 + random.getUint32(3); flips; --flips)
                bytes[random.getUint32(static_cast<unsigned>(bytes.size()))] ^= static_cast<uint8_t>(1 + random.getUint32(255));
            break;
        case 1:
            bytes.shrink(random.getUint32(static_cast<unsigned>(bytes.size())));
            break;
        case 2: {
            size_t count = 1 + random.getUint32(maximumBodySections);
            putField(bytes, 12, count, 2);
            if (128 + 24 * count <= bytes.size()) {
                for (size_t index = 128; index < 128 + 24 * count; ++index)
                    bytes[index] = static_cast<uint8_t>(random.getUint32(256));
                resealDirectory(bytes);
            } else
                resealEnvelope(bytes);
            break;
        }
        case 3: {
            size_t entry = random.getUint32(numberOfSectionKinds);
            putField(bytes, entryAt(entry) + 8, interestingValue(), 8);
            putField(bytes, entryAt(entry) + 16, interestingValue(), 8);
            if (random.getUint32(2))
                putField(bytes, entryAt(entry), kindRows[random.getUint32(numberOfSectionKinds)].typeId, 2);
            resealDirectory(bytes);
            break;
        }
        default:
            for (unsigned appended = 1 + random.getUint32(16); appended; --appended)
                bytes.append(static_cast<uint8_t>(random.getUint32(256)));
            putField(bytes, 80, bytes.size(), 8);
            resealEnvelope(bytes);
            break;
        }
        String label = makeString("fuzzed body "_s, iteration);
        for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity }) {
            validateEveryWay(context, label, bytes.span(), fixture.key, fixture.headerDigest, mode, random);
            auto copy = exactCopy(bytes.span());
            (void)validateBodyEnvelope(copy.span(), copy.span().size(), fixture.key, fixture.headerDigest, mode);
            (void)validateBodyFraming(copy.span(), fixture.key, fixture.headerDigest, mode, std::span { summaries });
        }
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
