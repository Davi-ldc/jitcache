#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/base/JITCacheSHA256.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/store/JITCachePlatform.h"
#include "jitcache/tests/store/ContainerTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <array>
#include <limits>
#include <optional>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of the artifact header in JITCacheContainer (store/JITCacheContainer.h and .cpp). None needs a VM. The tests write
// the header byte by byte at the offsets its layout defines, independently of the container's encoder, so every parse
// also checks the layout. The checks run over buffers of exactly the input's size from the system allocator, so ASan
// reports any read past the end.

namespace JSC::JITCache::Tests {

using namespace ContainerTestSupport;

namespace ContainerHeaderTestsInternal {

// The header, every field settable, so a test can break one rule at a time.

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
    uint16_t version { 2 };
    uint8_t architecture { 1 };
    std::optional<uint8_t> buildIDCount; // B; the number of records when absent
    std::optional<uint32_t> size; // H; the file's size when absent
    uint64_t cpuFeatures { 0 };
    std::optional<uint32_t> optionCount; // M; the number of option records when absent
    uint8_t appFlag { 0 }; // byte 28
    std::array<uint8_t, 3> appZero { }; // bytes 29 to 31
    uint64_t appDigest { 0 }; // bytes 32 to 39
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
    bytes.append(header.appFlag);
    for (uint8_t byte : header.appZero)
        bytes.append(byte);
    appendField(bytes, header.appDigest, 8);
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
    header.appFlag = contents.app ? 1 : 0;
    header.appDigest = contents.app.value_or(0);
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
    if (a.architecture != b.architecture || a.facts.cpuFeatures != b.facts.cpuFeatures || a.facts.mustMatch != b.facts.mustMatch || a.app != b.app)
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
    case HeaderVerdict::AnotherApp:
        return "AnotherApp"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace ContainerHeaderTestsInternal

using namespace ContainerHeaderTestsInternal;

// A header built from crafted facts parses back to them, with no app and with an app, and its bytes are the header
// layout's.
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
    // Any 64-bit value names an app, 0 and the largest included.
    for (uint64_t app : { uint64_t { 0x0123456789abcdef }, uint64_t { 0 }, std::numeric_limits<uint64_t>::max() }) {
        ArtifactHeader header = craftedHeader(headerX86_64, !!(app & 1));
        header.app = app;
        headers.append(header);
    }

    for (size_t index = 0; index < headers.size(); ++index) {
        auto& contents = headers[index];
        auto encoded = encodeArtifactHeader(contents);
        if (!encoded) {
            JITCACHE_FAIL(makeString("header "_s, index, " was not encoded"_s));
            continue;
        }
        JITCACHE_CHECK(encoded->size == (contents.facts.engineObject ? 216u : 144u));
        if (!equalSpans(encoded->span(), encodeRawHeader(rawHeaderFor(contents)).span()))
            JITCACHE_FAIL(makeString("header "_s, index, ": the encoded bytes differ from the header layout"_s));
        auto parsed = readArtifactHeader(encoded->span());
        if (!parsed)
            JITCACHE_FAIL(makeString("header "_s, index, " does not parse: "_s, parsed.error().reason));
        else if (!equalContents(*parsed, contents))
            JITCACHE_FAIL(makeString("header "_s, index, " parses to other contents"_s));
        auto check = checkArtifactHeader(encoded->span(), encoded->span(), contents.app);
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

// Each corrupt case of the header is classified corrupt; another layout version is incompatible without the rest being
// read.
JITCACHE_TEST(containerHeaderCorrupt, No)
{
    ArtifactHeader processContents = craftedHeader(headerX86_64, false);
    Vector<uint8_t> processHeader = encodedHeader(processContents);
    RawHeader base = rawHeaderFor(processContents);
    JITCACHE_CHECK(equalSpans(encodeRawHeader(base).span(), processHeader.span()));

    auto expectVerdict = [&](ASCIILiteral label, std::span<const uint8_t> file, HeaderVerdict expected) {
        auto copy = exactCopy(file);
        auto check = checkArtifactHeader(copy.span(), processHeader.span(), std::nullopt);
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
    expectVerdict("47 bytes"_s, processHeader.span().first(47), HeaderVerdict::Corrupt);
    {
        Vector<uint8_t> large = processHeader;
        large.grow(maximumArtifactHeaderFileBytes + 1);
        for (size_t index = processHeader.size(); index < large.size(); ++index)
            large[index] = 0;
        expectVerdict("a file larger than 4 KiB"_s, large.span(), HeaderVerdict::Corrupt);
    }
    expectCorrupt("another tag"_s, mutated([](RawHeader& header) { header.tag[3] = 'X'; }));
    expectCorrupt("H larger than the file"_s, mutated([](RawHeader& header) { header.size = 144 + 8; }));
    expectCorrupt("H smaller than the file"_s, mutated([](RawHeader& header) { header.size = 144 - 8; }));
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
    expectCorrupt("an app record's flag of 2"_s, mutated([](RawHeader& header) {
        header.appFlag = 2;
        header.appDigest = 5;
    }));
    expectCorrupt("a flag of 0 beside a digest other than 0"_s, mutated([](RawHeader& header) { header.appDigest = 5; }));
    for (size_t index = 0; index < 3; ++index) {
        expectCorrupt("a nonzero byte among 29 to 31"_s, mutated([&](RawHeader& header) {
            header.appFlag = 1;
            header.appZero[index] = 1;
        }));
    }
    expectCorrupt("the last four bytes not zero"_s, mutated([](RawHeader& header) { header.trailer = 1; }));
    expectCorrupt("another CRC"_s, mutated([&](RawHeader& header) { header.crc = static_cast<uint32_t>(getField(processHeader.span(), 136, 4)) ^ 1; }));
    {
        Vector<uint8_t> flipped = processHeader;
        flipped[48] ^= 0x01; // a byte of the main executable's ID, covered by the CRC alone
        expectVerdict("a flipped ID byte"_s, flipped.span(), HeaderVerdict::Corrupt);
        ArtifactHeader namingApp = processContents;
        namingApp.app = 0x0123456789abcdef;
        Vector<uint8_t> flippedApp = encodedHeader(namingApp);
        flippedApp[35] ^= 0x01; // a byte of the app record's digest, covered by the CRC alone
        expectVerdict("a flipped app digest byte"_s, flippedApp.span(), HeaderVerdict::Corrupt);
    }

    expectVerdict("layout version 1"_s, encodeRawHeader(mutated([](RawHeader& header) { header.version = 1; })).span(), HeaderVerdict::Incompatible);
    expectVerdict("layout version 3"_s, encodeRawHeader(mutated([](RawHeader& header) { header.version = 3; })).span(), HeaderVerdict::Incompatible);
    expectVerdict("layout version 0"_s, encodeRawHeader(mutated([](RawHeader& header) { header.version = 0; })).span(), HeaderVerdict::Incompatible);
    RawHeader otherVersionWithBadFields = mutated([](RawHeader& header) {
        header.version = 1;
        header.buildIDs.clear();
        header.optionCount = 9;
        header.appFlag = 7;
        header.crc = 0;
    });
    expectVerdict("layout version 1 with fields this version rejects"_s, encodeRawHeader(otherVersionWithBadFields).span(), HeaderVerdict::Incompatible);
    RawHeader otherVersionWithoutTag = mutated([](RawHeader& header) {
        header.version = 1;
        header.tag[0] = 'X';
    });
    expectVerdict("layout version 1 without the tag"_s, encodeRawHeader(otherVersionWithoutTag).span(), HeaderVerdict::Corrupt);
    auto otherVersion = checkArtifactHeader(encodeRawHeader(mutated([](RawHeader& header) { header.version = 1; })).span(), processHeader.span(), std::nullopt);
    JITCACHE_CHECK(otherVersion.detail == "layout version"_s);

    // A header as layout version 1 laid it out, without the app record, records from offset 32 and H = 32 + 72 B + 8 M + 8,
    // is another layout, not damage, whether or not its config names an app.
    {
        Vector<uint8_t> older = processHeader;
        older.removeAt(32, 8);
        putField(older, 8, 1, 2);
        putField(older, 12, older.size(), 4);
        putField(older, older.size() - 8, crc32cOf(older.span().first(older.size() - 8)), 4);
        expectVerdict("a header of layout version 1"_s, older.span(), HeaderVerdict::Incompatible);
        auto copy = exactCopy(older.span());
        JITCACHE_CHECK(checkArtifactHeader(copy.span(), processHeader.span(), 0x0123456789abcdef).verdict == HeaderVerdict::Incompatible);
    }
}

// Each differing field makes the header incompatible, with the detail naming that field.
JITCACHE_TEST(containerHeaderIncompatible, No)
{
    auto expectField = [&](ASCIILiteral label, const ArtifactHeader& artifact, const ArtifactHeader& process, const String& field) {
        auto artifactBytes = encodedHeader(artifact);
        auto copy = exactCopy(artifactBytes.span());
        auto check = checkArtifactHeader(copy.span(), encodedHeader(process).span(), std::nullopt);
        if (check.verdict != HeaderVerdict::Incompatible || check.detail != field) {
            JITCACHE_FAIL(makeString(label, ": "_s, verdictName(check.verdict), " naming \""_s, check.detail, "\", expected Incompatible naming \""_s,
                field, '"'));
        }
    };

    ArtifactHeader process = craftedHeader(headerX86_64, false);
    {
        auto check = checkArtifactHeader(encodedHeader(process).span(), encodedHeader(process).span(), std::nullopt);
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

// The app record gates only when both the header and the config name an app. A header is compatible whatever its app
// record when the config names none, and when the config names the header's app or the header names none; it is made for
// another app, with both digests in the detail, when both name apps that differ; and a header that also differs in a field
// is incompatible. headerNamingApp gives the process's header with an app record and a CRC computed again.
JITCACHE_TEST(containerHeaderApps, No)
{
    ArtifactHeader process = craftedHeader(headerX86_64, true);
    Vector<uint8_t> processHeader = encodedHeader(process);
    constexpr uint64_t appX = 0x0123456789abcdef;
    constexpr uint64_t appY = 0xfedcba9876543210;
    auto namingApp = [&](ArtifactHeader contents, std::optional<uint64_t> app) {
        contents.app = app;
        return encodedHeader(contents);
    };
    auto checkWith = [&](const Vector<uint8_t>& header, std::optional<uint64_t> config) {
        auto copy = exactCopy(header.span());
        return checkArtifactHeader(copy.span(), processHeader.span(), config);
    };
    auto appText = [](std::optional<uint64_t> app) -> String {
        if (!app)
            return "none"_s;
        return makeString(hex(*app, 16, Lowercase));
    };
    auto expectCompatible = [&](std::optional<uint64_t> header, std::optional<uint64_t> config) {
        auto check = checkWith(namingApp(process, header), config);
        if (check.verdict != HeaderVerdict::Compatible || !check.detail.isEmpty())
            JITCACHE_FAIL(makeString("a header naming "_s, appText(header), " for a config naming "_s, appText(config), ": "_s, verdictName(check.verdict), " ("_s, check.detail, ')'));
    };
    auto expectAnotherApp = [&](std::optional<uint64_t> header, std::optional<uint64_t> config, const String& detail) {
        auto check = checkWith(namingApp(process, header), config);
        if (check.verdict != HeaderVerdict::AnotherApp || check.detail != detail)
            JITCACHE_FAIL(makeString("a header naming "_s, appText(header), " for a config naming "_s, appText(config), ": "_s, verdictName(check.verdict), " ("_s, check.detail, "), expected AnotherApp ("_s, detail, ')'));
    };

    constexpr uint64_t appZero = 0; // an app like any other
    for (std::optional<uint64_t> header : { std::optional<uint64_t> { }, std::optional<uint64_t> { appX }, std::optional<uint64_t> { appZero } })
        expectCompatible(header, std::nullopt);
    expectCompatible(appX, appX);
    expectCompatible(appZero, appZero);
    expectCompatible(std::nullopt, appX);
    expectAnotherApp(appX, appY, "app 0123456789abcdef in the cache, fedcba9876543210 in this run"_s);
    expectAnotherApp(appY, appX, "app fedcba9876543210 in the cache, 0123456789abcdef in this run"_s);
    expectAnotherApp(appZero, appX, "app 0000000000000000 in the cache, 0123456789abcdef in this run"_s);

    // A field that differs makes the header incompatible whatever the apps.
    ArtifactHeader otherOption = process;
    otherOption.facts.mustMatch[2] = !otherOption.facts.mustMatch[2];
    for (std::optional<uint64_t> config : { std::optional<uint64_t> { }, std::optional<uint64_t> { appX }, std::optional<uint64_t> { appY } }) {
        auto check = checkWith(namingApp(otherOption, appX), config);
        JITCACHE_CHECK(check.verdict == HeaderVerdict::Incompatible && check.detail == "useImportDefer"_s);
    }

    // integratorBuildIDOfTestExecutable checks that the test executable has a build ID; without one, start never
    // computes the process's header.
    ProcessFacts facts = processFacts();
    if (!facts.mainExecutable.size || (facts.engineObject && !facts.engineObject->size))
        return;
    auto& expected = expectedHeader();
    JITCACHE_CHECK(equalSpans(headerNamingApp(std::nullopt).span(), expected.span()));
    ArtifactHeaderBytes named = headerNamingApp(appX);
    JITCACHE_CHECK(named.size == expected.size);
    if (named.size != expected.size)
        return;
    for (size_t index = 0; index < named.size; ++index) {
        bool appRecord = index >= 28 && index < 40;
        bool crc = index >= named.size - 8 && index < named.size - 4;
        if (!appRecord && !crc && named.bytes[index] != expected.bytes[index])
            JITCACHE_FAIL(makeString("headerNamingApp changes byte "_s, index, " outside the app record and the CRC"_s));
    }
    auto parsed = readArtifactHeader(named.span());
    JITCACHE_CHECK(parsed && parsed->app == appX);
    JITCACHE_CHECK(named.bytes[28] == 1 && getField(named.span(), 32, 8) == appX);
    JITCACHE_CHECK(checkArtifactHeader(named.span(), expected.span(), std::nullopt).verdict == HeaderVerdict::Compatible);
    JITCACHE_CHECK(checkArtifactHeader(named.span(), expected.span(), appX).verdict == HeaderVerdict::Compatible);
    JITCACHE_CHECK(checkArtifactHeader(named.span(), expected.span(), appY).verdict == HeaderVerdict::AnotherApp);
    JITCACHE_CHECK(checkArtifactHeader(expected.span(), named.span(), appY).verdict == HeaderVerdict::Compatible);
}

// The header digest, and the process's own header.
JITCACHE_TEST(containerHeaderDigest, No)
{
    Vector<uint8_t> header = encodedHeader(craftedHeader(headerX86_64, true));
    HeaderDigest digest = artifactHeaderDigest(header.span());
    Digest256 sha = SHA256::hash(header.span());
    JITCACHE_CHECK(equalSpans(std::span { digest }, std::span { sha }.first(digest.size())));

    Vector<uint8_t> other = encodedHeader(craftedHeader(headerX86_64, false));
    JITCACHE_CHECK(artifactHeaderDigest(other.span()) != digest);
    JITCACHE_CHECK(artifactHeaderDigest(encodedHeader(craftedHeader(headerX86_64, true)).span()) == digest);

    // Headers that differ only in their apps have different digests, so a pack moves only between artifacts of one app.
    ArtifactHeader namingX = craftedHeader(headerX86_64, true);
    namingX.app = 0x0123456789abcdef;
    ArtifactHeader namingY = namingX;
    namingY.app = 0xfedcba9876543210;
    ArtifactHeader namingZero = namingX;
    namingZero.app = 0;
    HeaderDigest digestX = artifactHeaderDigest(encodedHeader(namingX).span());
    JITCACHE_CHECK(digestX != digest);
    JITCACHE_CHECK(artifactHeaderDigest(encodedHeader(namingY).span()) != digestX);
    JITCACHE_CHECK(artifactHeaderDigest(encodedHeader(namingZero).span()) != digest);
    JITCACHE_CHECK(artifactHeaderDigest(encodedHeader(namingZero).span()) != digestX);

    // integratorBuildIDOfTestExecutable checks that the test executable has a build ID; without one, start never
    // computes the process's header.
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
    // The process's own header names no app: the app is a config's.
    JITCACHE_CHECK(!parsed->app);
    JITCACHE_CHECK(equalContents(*parsed, ArtifactHeader { thisHeaderArchitecture(), facts, std::nullopt }));
    JITCACHE_CHECK(checkArtifactHeader(expected.span(), expected.span(), std::nullopt).verdict == HeaderVerdict::Compatible);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
