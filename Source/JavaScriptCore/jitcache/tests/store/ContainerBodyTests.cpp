#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/tests/store/ContainerTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <optional>
#include <utility>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of the body format in JITCacheContainer (store/JITCacheContainer.h and .cpp), with its validation, and of the
// section-kind table the format reads. None needs a VM. The tests write bodies byte by byte at the offsets the format
// defines, independently of the container's encoders, with each section's type id and tier from the section table, so
// every parse also checks the layout. Validation runs over buffers of exactly the input's size from the system allocator,
// so ASan reports any read past the end.

namespace JSC::JITCache::Tests {

using namespace ContainerTestSupport;

namespace ContainerBodyTestsInternal {

// A fixed seed, so a failure reproduces.
static constexpr unsigned randomSeed = 0x4a495443;

// Body files.

struct TestSection {
    SectionKind kind;
    Vector<uint8_t> bytes;
};

// Sizes that leave padding after ucb.core, ucb.analysis, baked-facts.baseline, image-twins.baseline and cb.summary, and an
// empty ucb.feedback. A body of every kind holds ucb.analysis, so it is a module's body.
static constexpr std::array<size_t, numberOfSectionKinds> sectionSizes { 40, 13, 0, 11, 64, 7, 9, 24, 3, 17 };

static Vector<TestSection> sectionsWithout(std::optional<SectionKind> omitted = std::nullopt)
{
    Vector<TestSection> sections;
    for (size_t kindIndex = 0; kindIndex < numberOfSectionKinds; ++kindIndex) {
        SectionKind kind = static_cast<SectionKind>(kindIndex);
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

// The body the writer would write: the body layout for directoryEntries entries, the sections first.
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
        auto& description = sectionKindDescription(sections[index].kind);
        body.entries.append(RawEntry { description.typeId, description.tier, 0, offset, sections[index].bytes.size() });
        body.placements.append(Placement { offset, sections[index].bytes });
        end = offset + sections[index].bytes.size();
    }
    body.fileSize = end;
    return body;
}

static BodyKey testKey(uint8_t seed, IdentityKind kind = IdentityKind::Program)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1; // version
    bytes[1] = static_cast<uint8_t>(kind);
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

// A module's body, which may hold every kind.
static Fixture makeFixture(const Vector<TestSection>& sections)
{
    Fixture fixture { testKey(0x20, IdentityKind::Module), testDigest(0xa0), { }, { } };
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

} // namespace ContainerBodyTestsInternal

using namespace ContainerBodyTestsInternal;

// The section-kind table, which the container, the writer and Full validation read: each kind's type id and tier name it
// back, and in this twins build a body of a highest tier must hold every kind of that tier or a lower one but the
// ModuleOnly ones.
JITCACHE_TEST(containerSectionKinds, No)
{
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        auto& description = sectionKindDescription(kind);
        if (sectionKindFor(description.typeId, description.tier) != kind)
            JITCACHE_FAIL(makeString(description.name, ": type id "_s, hex(description.typeId), " at tier "_s, static_cast<unsigned>(description.tier), " names another kind"_s));
        bool required = description.presence != SectionPresence::ModuleOnly;
        JITCACHE_CHECK(isSectionRequired(kind, 0) == (required && !description.tier));
        JITCACHE_CHECK(isSectionRequired(kind, 1) == (required && description.tier <= 1));
    }
    JITCACHE_CHECK(!sectionKindFor(0x0101, 1));
    JITCACHE_CHECK(!sectionKindFor(0x0104, 1));
    JITCACHE_CHECK(!sectionKindFor(0x0105, 0));
    JITCACHE_CHECK(!sectionKindFor(0x0201, 0));
    JITCACHE_CHECK(!sectionKindFor(0x0201, 2));
    JITCACHE_CHECK(!sectionKindFor(0x0501, 1));
    JITCACHE_CHECK(!sectionKindFor(0, 0));
}

// The body's encoders lay a body out as the body format does: envelope, directory and sections.
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

    // The sections' offsets.
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

    // The envelope round-trips: the encoder writes the file's first 128 bytes, and validation reads its fields back.
    BodyEnvelope envelope = stamp;
    envelope.sectionCount = numberOfSectionKinds;
    envelope.fileSize = fixture.body.fileSize;
    envelope.directoryCRC = directoryCRC;
    auto envelopeBytes = encodeBodyEnvelope(envelope);
    JITCACHE_CHECK(equalSpans(std::span { envelopeBytes }, fixture.file.span().first(128)));
    auto decoded = validateBody(fixture.file.span(), fixture.key, fixture.headerDigest, ValidationMode::Full);
    if (!decoded)
        JITCACHE_FAIL(makeString("the encoded body fails "_s, decoded.error()));
    else {
        JITCACHE_CHECK(decoded->version == fixture.body.version);
        JITCACHE_CHECK(decoded->highestTier == 1);
        JITCACHE_CHECK(decoded->llintThreshold == fixture.body.llintThreshold);
        JITCACHE_CHECK(decoded->counterProgress == fixture.body.counterProgress);
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

// A body of every kind validates in both modes, and each of the eight checks, from container.size to container.checksum,
// fails exactly when its own condition is broken, with its own name; a body broken only in a structure part validates in
// Integrity mode. Bodies that break two checks name the first.
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

    // container.size.
    expect("an empty file"_s, { }, sizeCheck, sizeCheck);
    expect("127 bytes"_s, file.span().first(127), sizeCheck, sizeCheck);
    expectChanged("an envelope file size one too large"_s, sizeCheck, sizeCheck, [&](auto& bytes) {
        putField(bytes, 80, fileSize + 1, 8);
        resealEnvelope(bytes);
    });
    expectChanged("one byte past the envelope's file size"_s, sizeCheck, sizeCheck, [](auto& bytes) {
        bytes.append(0);
    });

    // container.envelope, integrity.
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

    // container.envelope, structure.
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
    // Tier 0, a body without an image, is a highest tier container.envelope accepts; one that holds tier-1 sections fails
    // container.required.
    expectChanged("a highest tier of 0 over tier-1 sections"_s, requiredCheck, accepted, [](auto& bytes) {
        bytes[14] = 0;
        resealEnvelope(bytes);
    });

    // container.key.
    expectOutcomes(context, "another expected key"_s, file.span(), testKey(0x21), digest, keyCheck, keyCheck, random);
    expectChanged("another key in the envelope"_s, keyCheck, keyCheck, [](auto& bytes) {
        bytes[16 + 30] ^= 1;
        resealEnvelope(bytes);
    });

    // container.header-digest.
    expectOutcomes(context, "another header digest"_s, file.span(), key, testDigest(0xa1), headerDigestCheck, headerDigestCheck, random);

    // container.version.
    expectChanged("a zero commit identifier"_s, versionCheck, accepted, [](auto& bytes) {
        putField(bytes, 56, 0, 8);
        resealEnvelope(bytes);
    });

    // container.directory, integrity.
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

    // container.directory, structure.
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
        // image-twins.baseline's entry names baked-facts.baseline, the entry before it.
        putField(bytes, entryAt(static_cast<size_t>(SectionKind::ImageTwinsBaseline)), 0x0202, 2);
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

    // container.required.
    for (SectionKind omitted : { SectionKind::UCBCore, SectionKind::ImageTwinsBaseline, SectionKind::ICsBaseline }) {
        Fixture without = makeFixture(sectionsWithout(omitted));
        String label = makeString("a body without "_s, sectionKindDescription(omitted).name);
        expectOutcomes(context, label, without.file.span(), without.key, without.headerDigest, requiredCheck, accepted, random);
    }

    // container.checksum.
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

// Integrity mode checks every entry's CRC wherever the entry points, envelope and directory included, and the layout
// records the first entry of each kind, ignoring the kinds it does not know.
JITCACHE_TEST(containerBodyIntegrityEntries, No)
{
    WeakRandom random(randomSeed);
    Vector<TestSection> sections = sectionsWithout();
    BodyKey key = testKey(0x30, IdentityKind::Module);
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

// Truncation at every region boundary fails in both modes, as does a stream that receives fewer or more bytes than its
// file's size.
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

// The scoring read, which checks part of a body: the envelope, the directory and three sections' checksums, and no
// other byte.
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

    auto framingOutcome = [&](std::span<const uint8_t> bytes, std::span<const SectionKind> kinds) -> Outcome {
        auto copy = exactCopy(bytes);
        auto result = validateBodyFraming(copy.span(), key, digest, kinds);
        if (result)
            return std::nullopt;
        return result.error();
    };

    auto layout = validateBodyFraming(file.span(), key, digest, std::span { summaries });
    auto whole = validateBody(file.span(), key, digest, ValidationMode::Integrity);
    JITCACHE_CHECK(layout && whole && layout->version == whole->version && layout->sections == whole->sections);

    auto damagedImage = mutated([&](auto& bytes) { bytes[sectionByte(SectionKind::ImageBaseline, 0)] ^= 1; });
    JITCACHE_CHECK(framingOutcome(damagedImage.span(), std::span { summaries }) == accepted);
    auto damagedSummary = mutated([&](auto& bytes) { bytes[sectionByte(SectionKind::CBSummaryBaseline, 2)] ^= 1; });
    JITCACHE_CHECK(framingOutcome(damagedSummary.span(), std::span { summaries }) == checksumCheck);
    auto damagedICs = mutated([&](auto& bytes) { bytes.last() ^= 1; });
    JITCACHE_CHECK(framingOutcome(damagedICs.span(), std::span { summaries }) == checksumCheck);
    JITCACHE_CHECK(framingOutcome(damagedICs.span(), { }) == accepted);
    auto padded = mutated([&](auto& bytes) { bytes[firstPaddingByte(body)] = 1; });
    JITCACHE_CHECK(framingOutcome(padded.span(), std::span { summaries }) == accepted);
    auto beyond = mutated([](auto& bytes) {
        putField(bytes, 12, maximumBodySections, 2);
        resealEnvelope(bytes);
    });
    JITCACHE_CHECK(framingOutcome(beyond.span(), std::span { summaries }) == directoryCheck);
    auto outOfOrder = mutated([](auto& bytes) {
        for (size_t index = 0; index < 24; ++index)
            std::swap(bytes[entryAt(3) + index], bytes[entryAt(4) + index]);
        resealDirectory(bytes);
    });
    JITCACHE_CHECK(framingOutcome(outOfOrder.span(), std::span { summaries }) == accepted);
    JITCACHE_CHECK(framingOutcome(file.span().first(file.size() - 1), std::span { summaries }) == sizeCheck);

    Fixture without = makeFixture(sectionsWithout(SectionKind::UCBCore));
    auto copy = exactCopy(without.file.span());
    auto missing = validateBodyFraming(copy.span(), without.key, without.headerDigest, std::span { summaries });
    JITCACHE_CHECK(outcomeOf(missing) == accepted);
}

// No malformed input reads out of bounds, in any form or mode, and the stream agrees with validateBody on all of them.
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
                putField(bytes, entryAt(entry), sectionKindDescriptions[random.getUint32(numberOfSectionKinds)].typeId, 2);
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
        for (ValidationMode mode : { ValidationMode::Full, ValidationMode::Integrity })
            validateEveryWay(context, label, bytes.span(), fixture.key, fixture.headerDigest, mode, random);
        auto copy = exactCopy(bytes.span());
        (void)validateBodyFraming(copy.span(), fixture.key, fixture.headerDigest, std::span { summaries });
    }
}

// The envelope's highest tier: Full mode accepts 0 or 1 and rejects any other, and a body of tier 0 must then hold exactly
// the UCB lane's three sections. Integrity mode leaves the tier to the structure checks.
JITCACHE_TEST(runOnceContainerAcceptsTierZero, No)
{
    BodyKey key = testKey(7);
    HeaderDigest headerDigest;
    headerDigest.fill(0x5a);
    Vector<uint8_t> identity(FillWith { }, 24, 1);
    Vector<uint8_t> core(FillWith { }, 40, 2);
    Vector<uint8_t> feedback(FillWith { }, 56, 3);
    Vector<uint8_t> image(FillWith { }, 64, 4);
    std::array ucbOnly {
        ContainerTesting::Section { SectionKind::UCBIdentity, identity.span() },
        ContainerTesting::Section { SectionKind::UCBCore, core.span() },
        ContainerTesting::Section { SectionKind::UCBFeedback, feedback.span() },
    };
    std::array withImage {
        ContainerTesting::Section { SectionKind::UCBIdentity, identity.span() },
        ContainerTesting::Section { SectionKind::UCBCore, core.span() },
        ContainerTesting::Section { SectionKind::UCBFeedback, feedback.span() },
        ContainerTesting::Section { SectionKind::ImageBaseline, image.span() },
    };
    auto stamp = [&](uint8_t tier) {
        BodyEnvelope envelope;
        envelope.highestTier = tier;
        envelope.key = key;
        envelope.version = 0x1234;
        envelope.headerDigest = headerDigest;
        envelope.llintThreshold = 30;
        return envelope;
    };
    auto validate = [&](uint8_t tier, std::span<const ContainerTesting::Section> sections, ValidationMode mode) {
        Vector<uint8_t> file = ContainerTesting::buildBody(stamp(tier), sections);
        return validateBody(file.span(), key, std::span<const uint8_t, 16> { headerDigest }, mode);
    };

    auto tierZero = validate(0, ucbOnly, ValidationMode::Full);
    if (!tierZero)
        JITCACHE_FAIL(makeString("Full mode rejected a body of tier 0 at "_s, tierZero.error()));
    else {
        JITCACHE_CHECK(!tierZero->highestTier);
        JITCACHE_CHECK(tierZero->llintThreshold == 30);
        JITCACHE_CHECK(!tierZero->sections[static_cast<size_t>(SectionKind::ImageBaseline)]);
    }
    JITCACHE_CHECK(validate(0, ucbOnly, ValidationMode::Integrity).has_value());

    auto tierTwo = validate(2, ucbOnly, ValidationMode::Full);
    JITCACHE_CHECK(!tierTwo && tierTwo.error() == ContainerChecks::envelope);
    JITCACHE_CHECK(validate(2, ucbOnly, ValidationMode::Integrity).has_value());

    auto imageAtTierZero = validate(0, withImage, ValidationMode::Full);
    JITCACHE_CHECK(!imageAtTierZero && imageAtTierZero.error() == ContainerChecks::required);
}

// ucb.analysis is a section no tier requires: a module's body validates with it and without it, at tier 0 and at tier 1,
// while in Full mode a body of any other identity kind that holds one fails container.required. Integrity mode leaves
// the question to the structure checks.
JITCACHE_TEST(containerAnalysisSectionOnlyInModules, No)
{
    WeakRandom random(randomSeed);
    HeaderDigest digest = testDigest(0xc0);
    // A body of the highest tier with every kind that tier holds, ucb.analysis when asked.
    auto bodyOf = [&](const BodyKey& key, uint8_t tier, bool withAnalysis) {
        Vector<TestSection> sections;
        for (auto& section : sectionsWithout()) {
            if (section.kind == SectionKind::UCBAnalysis ? !withAnalysis : sectionKindDescription(section.kind).tier > tier)
                continue;
            sections.append(section);
        }
        RawBody body = canonicalBody(key, digest, sections);
        body.highestTier = tier;
        return encodeRawBody(body);
    };
    static constexpr std::array<std::pair<IdentityKind, ASCIILiteral>, 6> otherKinds { {
        { IdentityKind::Program, "program"_s },
        { IdentityKind::IndirectEval, "indirect eval"_s },
        { IdentityKind::FunctionConstructor, "Function-constructor"_s },
        { IdentityKind::Builtin, "builtin"_s },
        { IdentityKind::Child, "child"_s },
        { IdentityKind::DirectEval, "direct eval"_s },
    } };

    for (uint8_t tier : { 0, 1 }) {
        BodyKey module = testKey(static_cast<uint8_t>(0x40 + tier), IdentityKind::Module);
        unsigned tierNumber = tier;
        for (bool withAnalysis : { false, true }) {
            String label = makeString("a module's body of tier "_s, tierNumber, withAnalysis ? " with"_s : " without"_s, " ucb.analysis"_s);
            expectOutcomes(context, label, bodyOf(module, tier, withAnalysis).span(), module, digest, accepted, accepted, random);
        }
        for (size_t index = 0; index < otherKinds.size(); ++index) {
            auto& [kind, kindName] = otherKinds[index];
            BodyKey key = testKey(static_cast<uint8_t>(0x50 + 2 * index + tier), kind);
            expectOutcomes(context, makeString("a "_s, kindName, " body of tier "_s, tierNumber, " with ucb.analysis"_s), bodyOf(key, tier, true).span(),
                key, digest, requiredCheck, accepted, random);
            expectOutcomes(context, makeString("a "_s, kindName, " body of tier "_s, tierNumber, " without ucb.analysis"_s), bodyOf(key, tier, false).span(),
                key, digest, accepted, accepted, random);
        }
    }

    // The reads that need only some sections run the integrity checks alone, so they accept such a body too.
    BodyKey program = testKey(0x60);
    Vector<uint8_t> programWithAnalysis = bodyOf(program, 1, true);
    auto copy = exactCopy(programWithAnalysis.span());
    static constexpr std::array<SectionKind, 1> analysis { SectionKind::UCBAnalysis };
    JITCACHE_CHECK(validateBodyFraming(copy.span(), program, digest, std::span { analysis }).has_value());
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
