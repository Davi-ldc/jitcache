#include "config.h"
#include "JITCacheContainer.h"

#include "JITCacheSHA256.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
#include <tuple>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC::JITCache {

// Unified sources compile this file with other parts' files, so its helpers live in this per-file namespace
// (SPEC-integrator.md R-ALL-8), and the functions outside it name them qualified.
namespace ContainerInternal {

// Fields are little-endian and read and written with memcpy (the container sub-SPEC's preamble); every caller has checked
// that the field lies inside bytes.
template<typename T>
static T loadField(std::span<const uint8_t> bytes, size_t offset)
{
    T value { };
    memcpySpan(asMutableByteSpan(value), bytes.subspan(offset, sizeof(T)));
    return value;
}

template<typename T>
static void storeField(std::span<uint8_t> bytes, size_t offset, T value)
{
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

// CRC-32C of RFC 3720 (container sub-SPEC section 4.4).
static uint32_t crc32cOf(std::span<const uint8_t> bytes)
{
    return ~crc32cExtend(~0u, bytes);
}

static bool isAllZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

// The first multiple of 8 at or after value, or nothing when that does not fit in 64 bits.
static std::optional<uint64_t> roundUpToMultipleOfEight(uint64_t value)
{
    if (value > std::numeric_limits<uint64_t>::max() - 7)
        return std::nullopt;
    return (value + 7) & ~static_cast<uint64_t>(7);
}

static HeaderDigest digestFromSpan(std::span<const uint8_t, 16> bytes)
{
    HeaderDigest digest;
    memcpySpan(std::span { digest }, bytes);
    return digest;
}

// The section kinds' table.

// The directory sorts its entries by (type id, tier); this is that order as one integer.
static constexpr uint32_t directoryOrder(uint16_t typeId, uint8_t tier)
{
    return (static_cast<uint32_t>(typeId) << 8) | tier;
}

static constexpr bool sectionKindsFollowDirectoryOrder()
{
    for (size_t index = 1; index < numberOfSectionKinds; ++index) {
        auto& previous = sectionKindDescriptions[index - 1];
        auto& next = sectionKindDescriptions[index];
        if (directoryOrder(previous.typeId, previous.tier) >= directoryOrder(next.typeId, next.tier))
            return false;
    }
    return true;
}
static_assert(sectionKindsFollowDirectoryOrder(), "SectionKind's order is the directory's order of (type id, tier)");

// The header (container sub-SPEC section 3).

static constexpr std::array<uint8_t, 8> headerTag { 'J', 'I', 'T', 'C', 'H', 'E', 'A', 'D' };
static constexpr uint16_t headerLayoutVersion = 1;
static constexpr size_t headerVersionOffset = 8;
static constexpr size_t headerArchitectureOffset = 10;
static constexpr size_t headerBuildIDCountOffset = 11;
static constexpr size_t headerSizeOffset = 12;
static constexpr size_t headerCPUFeaturesOffset = 16;
static constexpr size_t headerOptionCountOffset = 24;
static constexpr size_t headerReservedOffset = 28;
static constexpr size_t headerRecordsOffset = 32;
static constexpr size_t buildIDRecordBytes = 72;
static constexpr size_t buildIDRecordIDOffset = 8;
static constexpr size_t maximumBuildIDBytes = 64;
static constexpr size_t optionRecordBytes = 8;
static constexpr size_t headerTrailerBytes = 8; // the CRC and four zero bytes
static constexpr size_t headerTrailerZeroBytes = 4;
static constexpr uint8_t optionTypeBool = 1;
static constexpr unsigned headerOptionCount = 3; // M

static_assert(std::tuple_size_v<decltype(ProcessFacts::mustMatch)> == headerOptionCount);
static_assert(std::tuple_size_v<decltype(BuildID::bytes)> == maximumBuildIDBytes);

static constexpr uint64_t headerSizeFor(uint64_t buildIDCount, uint64_t optionCount)
{
    return headerRecordsOffset + buildIDRecordBytes * buildIDCount + optionRecordBytes * optionCount + headerTrailerBytes;
}
static_assert(headerSizeFor(2, headerOptionCount) == maximumArtifactHeaderBytes);
static_assert(headerSizeFor(0, 0) == minimumArtifactHeaderFileBytes);

// By the option index of the option records, which is ProcessFacts::mustMatch's (SPEC-integrator.md section 5.2).
static constexpr std::array<ASCIILiteral, headerOptionCount> mustMatchOptionNames { "evalMode"_s, "useExplicitResourceManagement"_s, "useImportDefer"_s };

// The predicate of each bit of the CPU feature vector (SPEC-integrator.md section 5.2).
static constexpr std::array<ASCIILiteral, 10> x86FeatureNames {
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
static constexpr std::array<ASCIILiteral, 8> arm64FeatureNames {
    "supportsFloatingPointRounding"_s,
    "supportsCountPopulation"_s,
    "supportsFloat16"_s,
    "supportsDotProd"_s,
    "supportsLSE"_s,
    "supportsDoubleToInt32ConversionUsingJavaScriptSemantics"_s,
    "supportsRoundFloatToIntegerFloat"_s,
    "supportsSHA3"_s,
};

static ASCIILiteral cpuFeatureName(uint8_t architecture, unsigned bit)
{
    if (architecture == static_cast<uint8_t>(HeaderArchitecture::X86_64) && bit < x86FeatureNames.size())
        return x86FeatureNames[bit];
    if (architecture == static_cast<uint8_t>(HeaderArchitecture::ARM64) && bit < arm64FeatureNames.size())
        return arm64FeatureNames[bit];
    return { };
}

static bool isEncodableBuildID(const BuildID& buildID)
{
    return buildID.size >= 1 && buildID.size <= maximumBuildIDBytes;
}

// Both IDs come from parsed headers, so their sizes are 1 to 64.
static bool equalBuildIDs(const BuildID& a, const BuildID& b)
{
    return a.size == b.size && equalSpans(std::span { a.bytes }.first(a.size), std::span { b.bytes }.first(b.size));
}

static void writeBuildIDRecord(std::span<uint8_t> header, unsigned index, const BuildID& buildID)
{
    auto record = header.subspan(headerRecordsOffset + buildIDRecordBytes * index, buildIDRecordBytes);
    storeField<uint8_t>(record, 0, static_cast<uint8_t>(index + 1)); // the role: 1 main executable, 2 engine object
    storeField<uint8_t>(record, 1, buildID.size);
    memcpySpan(record.subspan(buildIDRecordIDOffset, buildID.size), std::span { buildID.bytes }.first(buildID.size));
}

// The first field in which two well-formed headers differ, in the order of HeaderCheck::detail.
static String firstDifferingField(const ArtifactHeader& artifact, const ArtifactHeader& process)
{
    if (artifact.architecture != process.architecture)
        return "architecture"_s;
    if (!equalBuildIDs(artifact.facts.mainExecutable, process.facts.mainExecutable))
        return "main executable build ID"_s;
    auto& artifactEngine = artifact.facts.engineObject;
    auto& processEngine = process.facts.engineObject;
    if (artifactEngine.has_value() != processEngine.has_value() || (artifactEngine && !equalBuildIDs(*artifactEngine, *processEngine)))
        return "engine object build ID"_s;
    if (uint64_t differingFeatures = artifact.facts.cpuFeatures ^ process.facts.cpuFeatures) {
        unsigned bit = std::countr_zero(differingFeatures);
        if (ASCIILiteral name = cpuFeatureName(artifact.architecture, bit); !name.isNull())
            return name;
        return makeString("CPU feature bit "_s, bit);
    }
    for (unsigned index = 0; index < headerOptionCount; ++index) {
        if (artifact.facts.mustMatch[index] != process.facts.mustMatch[index])
            return mustMatchOptionNames[index];
    }
    // Two well-formed headers with equal contents have equal bytes, so this is not reached.
    return "header"_s;
}

// Body files (container sub-SPEC section 4).

static constexpr std::array<uint8_t, 8> bodyTag { 'J', 'I', 'T', 'C', 'B', 'O', 'D', 'Y' };
static constexpr uint16_t bodyLayoutVersion = 1;
static constexpr uint8_t bodyHighestTier = 1; // the only highest tier of this version
static constexpr size_t envelopeVersionOffset = 8;
static constexpr size_t envelopeSizeOffset = 10;
static constexpr size_t envelopeSectionCountOffset = 12;
static constexpr size_t envelopeTierOffset = 14;
static constexpr size_t envelopeZeroOffset = 15;
static constexpr size_t envelopeKeyOffset = 16;
static constexpr size_t envelopeCommitOffset = 56;
static constexpr size_t envelopeDigestOffset = 64;
static constexpr size_t envelopeFileSizeOffset = 80;
static constexpr size_t envelopeLLIntThresholdOffset = 88;
static constexpr size_t envelopeCounterProgressOffset = 92;
static constexpr size_t envelopeDirectoryCRCOffset = 96;
static constexpr size_t envelopeReservedOffset = 100;
static constexpr size_t envelopeReservedBytes = 20;
static constexpr size_t envelopeCRCOffset = 120;
static constexpr size_t envelopeTrailerOffset = 124;
static constexpr size_t entryTypeIdOffset = 0;
static constexpr size_t entryTierOffset = 2;
static constexpr size_t entryZeroOffset = 3;
static constexpr size_t entryCRCOffset = 4;
static constexpr size_t entryOffsetOffset = 8;
static constexpr size_t entrySizeOffset = 16;

static_assert(!(bodyEnvelopeBytes % 8) && !(bodyDirectoryEntryBytes % 8), "the first section starts right after the directory");
static_assert(envelopeTrailerOffset + 4 == bodyEnvelopeBytes);

static constexpr uint64_t framingBytesFor(uint64_t sectionCount)
{
    return bodyEnvelopeBytes + bodyDirectoryEntryBytes * sectionCount;
}

// A key is exactly its 40 canonical bytes (UCBKeys.h asserts it), so the envelope holds a byte copy of it.
static std::array<uint8_t, BodyKey::byteSize> keyBytes(const BodyKey& key)
{
    return std::bit_cast<std::array<uint8_t, BodyKey::byteSize>>(key);
}

static BodyEnvelope decodeEnvelope(std::span<const uint8_t> envelope)
{
    std::array<uint8_t, BodyKey::byteSize> key;
    memcpySpan(std::span { key }, envelope.subspan(envelopeKeyOffset, BodyKey::byteSize));
    BodyEnvelope result;
    result.sectionCount = loadField<uint16_t>(envelope, envelopeSectionCountOffset);
    result.highestTier = loadField<uint8_t>(envelope, envelopeTierOffset);
    result.key = std::bit_cast<BodyKey>(key);
    result.version = loadField<uint64_t>(envelope, envelopeCommitOffset);
    memcpySpan(std::span { result.headerDigest }, envelope.subspan(envelopeDigestOffset, result.headerDigest.size()));
    result.fileSize = loadField<uint64_t>(envelope, envelopeFileSizeOffset);
    result.llintThreshold = loadField<uint32_t>(envelope, envelopeLLIntThresholdOffset);
    result.counterProgress = loadField<uint32_t>(envelope, envelopeCounterProgressOffset);
    result.directoryCRC = loadField<uint32_t>(envelope, envelopeDirectoryCRCOffset);
    return result;
}

// B1 to B5. fileStart holds the file's first bytes, all 128 of the envelope when the file is that long.
static std::expected<BodyEnvelope, ContainerCheck> checkEnvelope(std::span<const uint8_t> fileStart, uint64_t fileSize,
    const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode mode)
{
    // B1. A file start shorter than the envelope while the file is not means the file changed under its reader.
    if (fileSize < bodyEnvelopeBytes || fileStart.size() < bodyEnvelopeBytes)
        return std::unexpected(ContainerChecks::size);
    auto envelope = fileStart.first(bodyEnvelopeBytes);
    if (loadField<uint64_t>(envelope, envelopeFileSizeOffset) != fileSize)
        return std::unexpected(ContainerChecks::size);

    // B2.
    uint16_t sectionCount = loadField<uint16_t>(envelope, envelopeSectionCountOffset);
    if (!equalSpans(envelope.first(bodyTag.size()), std::span { bodyTag })
        || loadField<uint16_t>(envelope, envelopeVersionOffset) != bodyLayoutVersion
        || loadField<uint16_t>(envelope, envelopeSizeOffset) != bodyEnvelopeBytes
        || !sectionCount || sectionCount > maximumBodySections
        || loadField<uint32_t>(envelope, envelopeCRCOffset) != crc32cOf(envelope.first(envelopeCRCOffset)))
        return std::unexpected(ContainerChecks::envelope);
    if (mode == ValidationMode::Full
        && (envelope[envelopeZeroOffset]
            || !isAllZero(envelope.subspan(envelopeReservedOffset, envelopeReservedBytes))
            || !isAllZero(envelope.subspan(envelopeTrailerOffset))
            || envelope[envelopeTierOffset] != bodyHighestTier))
        return std::unexpected(ContainerChecks::envelope);

    // B3.
    auto expectedKeyBytes = keyBytes(expectedKey);
    if (!equalSpans(envelope.subspan(envelopeKeyOffset, BodyKey::byteSize), std::span { expectedKeyBytes }))
        return std::unexpected(ContainerChecks::key);

    // B4.
    if (!equalSpans(envelope.subspan(envelopeDigestOffset, headerDigest.size()), headerDigest))
        return std::unexpected(ContainerChecks::headerDigest);

    // B5.
    if (mode == ValidationMode::Full && !loadField<uint64_t>(envelope, envelopeCommitOffset))
        return std::unexpected(ContainerChecks::version);

    return decodeEnvelope(envelope);
}

struct DirectoryRecord {
    uint16_t typeId;
    uint8_t tier;
    uint8_t zero;
    uint32_t crc;
    uint64_t offset;
    uint64_t size;
};

static DirectoryRecord directoryRecord(std::span<const uint8_t> directory, size_t index)
{
    auto entry = directory.subspan(bodyDirectoryEntryBytes * index, bodyDirectoryEntryBytes);
    return {
        loadField<uint16_t>(entry, entryTypeIdOffset),
        loadField<uint8_t>(entry, entryTierOffset),
        loadField<uint8_t>(entry, entryZeroOffset),
        loadField<uint32_t>(entry, entryCRCOffset),
        loadField<uint64_t>(entry, entryOffsetOffset),
        loadField<uint64_t>(entry, entrySizeOffset),
    };
}

static size_t directoryRecordCount(std::span<const uint8_t> directory)
{
    return directory.size() / bodyDirectoryEntryBytes;
}

// B6 apart from the padding, over a directory that lies inside the file, whose size B1 established is the envelope's.
static std::optional<ContainerCheck> checkDirectory(std::span<const uint8_t> directory, const BodyEnvelope& envelope, ValidationMode mode)
{
    uint64_t fileSize = envelope.fileSize;
    size_t count = directoryRecordCount(directory);
    if (crc32cOf(directory) != envelope.directoryCRC)
        return ContainerChecks::directory;
    for (size_t index = 0; index < count; ++index) {
        DirectoryRecord record = directoryRecord(directory, index);
        if (record.offset > fileSize || record.size > fileSize - record.offset)
            return ContainerChecks::directory;
    }
    if (mode == ValidationMode::Integrity)
        return std::nullopt;

    uint64_t expectedOffset = framingBytesFor(count);
    uint64_t end = expectedOffset;
    for (size_t index = 0; index < count; ++index) {
        DirectoryRecord record = directoryRecord(directory, index);
        if (record.zero || !sectionKindFor(record.typeId, record.tier))
            return ContainerChecks::directory;
        if (index) {
            DirectoryRecord previous = directoryRecord(directory, index - 1);
            if (directoryOrder(previous.typeId, previous.tier) >= directoryOrder(record.typeId, record.tier))
                return ContainerChecks::directory;
            auto aligned = roundUpToMultipleOfEight(end);
            if (!aligned)
                return ContainerChecks::directory;
            expectedOffset = *aligned;
        }
        if (record.offset != expectedOffset)
            return ContainerChecks::directory;
        end = record.offset + record.size; // inside the file, checked above
    }
    if (end != fileSize)
        return ContainerChecks::directory;
    return std::nullopt;
}

// B7, over a directory that passed B6 in Full mode, so its entries name distinct kinds.
static bool holdsRequiredSections(std::span<const uint8_t> directory, uint8_t highestTier)
{
    std::array<bool, numberOfSectionKinds> present { };
    for (size_t index = 0; index < directoryRecordCount(directory); ++index) {
        DirectoryRecord record = directoryRecord(directory, index);
        if (auto kind = sectionKindFor(record.typeId, record.tier))
            present[static_cast<size_t>(*kind)] = true;
    }
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        if (present[index] != isSectionRequired(static_cast<SectionKind>(index), highestTier))
            return false;
    }
    return true;
}

// The index of the first entry of the kind, which is the one the layout records.
static std::optional<size_t> firstRecordOf(std::span<const uint8_t> directory, SectionKind kind)
{
    auto& description = sectionKindDescription(kind);
    for (size_t index = 0; index < directoryRecordCount(directory); ++index) {
        DirectoryRecord record = directoryRecord(directory, index);
        if (record.typeId == description.typeId && record.tier == description.tier)
            return index;
    }
    return std::nullopt;
}

static BodyLayout layoutOf(const BodyEnvelope& envelope, std::span<const uint8_t> directory)
{
    BodyLayout layout { envelope.version, envelope.highestTier, envelope.llintThreshold, envelope.counterProgress, { } };
    for (size_t index = 0; index < directoryRecordCount(directory); ++index) {
        DirectoryRecord record = directoryRecord(directory, index);
        auto kind = sectionKindFor(record.typeId, record.tier);
        if (!kind)
            continue;
        auto& extent = layout.sections[static_cast<size_t>(*kind)];
        if (!extent)
            extent = SectionExtent { record.offset, record.size };
    }
    return layout;
}

} // namespace ContainerInternal

std::optional<ArtifactHeaderBytes> encodeArtifactHeader(const ArtifactHeader& header)
{
    using ContainerInternal::storeField;
    auto& facts = header.facts;
    if (!ContainerInternal::isEncodableBuildID(facts.mainExecutable) || (facts.engineObject && !ContainerInternal::isEncodableBuildID(*facts.engineObject)))
        return std::nullopt;

    unsigned buildIDCount = facts.engineObject ? 2 : 1;
    ArtifactHeaderBytes result;
    result.size = static_cast<uint32_t>(ContainerInternal::headerSizeFor(buildIDCount, ContainerInternal::headerOptionCount));
    auto bytes = std::span { result.bytes }.first(result.size);
    memcpySpan(bytes, std::span { ContainerInternal::headerTag });
    storeField<uint16_t>(bytes, ContainerInternal::headerVersionOffset, ContainerInternal::headerLayoutVersion);
    storeField<uint8_t>(bytes, ContainerInternal::headerArchitectureOffset, header.architecture);
    storeField<uint8_t>(bytes, ContainerInternal::headerBuildIDCountOffset, static_cast<uint8_t>(buildIDCount));
    storeField<uint32_t>(bytes, ContainerInternal::headerSizeOffset, result.size);
    storeField<uint64_t>(bytes, ContainerInternal::headerCPUFeaturesOffset, facts.cpuFeatures);
    storeField<uint32_t>(bytes, ContainerInternal::headerOptionCountOffset, ContainerInternal::headerOptionCount);
    ContainerInternal::writeBuildIDRecord(bytes, 0, facts.mainExecutable);
    if (facts.engineObject)
        ContainerInternal::writeBuildIDRecord(bytes, 1, *facts.engineObject);
    size_t options = ContainerInternal::headerRecordsOffset + ContainerInternal::buildIDRecordBytes * buildIDCount;
    for (unsigned index = 0; index < ContainerInternal::headerOptionCount; ++index) {
        size_t record = options + ContainerInternal::optionRecordBytes * index;
        storeField<uint16_t>(bytes, record, static_cast<uint16_t>(index));
        storeField<uint8_t>(bytes, record + 2, ContainerInternal::optionTypeBool);
        storeField<uint8_t>(bytes, record + 3, facts.mustMatch[index] ? 1 : 0);
    }
    size_t checksummed = result.size - ContainerInternal::headerTrailerBytes;
    storeField<uint32_t>(bytes, checksummed, ContainerInternal::crc32cOf(bytes.first(checksummed)));
    return result;
}

const ArtifactHeaderBytes& expectedHeader()
{
    static LazyNeverDestroyed<ArtifactHeaderBytes> header;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        auto bytes = encodeArtifactHeader({ thisHeaderArchitecture(), processFacts() });
        RELEASE_ASSERT(bytes);
        header.construct(*bytes);
    });
    return header.get();
}

std::expected<ArtifactHeader, HeaderRejection> readArtifactHeader(std::span<const uint8_t> file)
{
    using ContainerInternal::loadField;
    auto corrupt = [](ASCIILiteral reason) {
        return std::unexpected(HeaderRejection { HeaderVerdict::Corrupt, reason });
    };

    if (file.size() < minimumArtifactHeaderFileBytes || file.size() > maximumArtifactHeaderFileBytes)
        return corrupt("the file is shorter than 40 bytes or larger than 4 KiB"_s);
    if (!equalSpans(file.first(ContainerInternal::headerTag.size()), std::span { ContainerInternal::headerTag }))
        return corrupt("the tag is not JITCHEAD"_s);
    if (loadField<uint16_t>(file, ContainerInternal::headerVersionOffset) != ContainerInternal::headerLayoutVersion)
        return std::unexpected(HeaderRejection { HeaderVerdict::Incompatible, "layout version"_s });

    uint8_t buildIDCount = loadField<uint8_t>(file, ContainerInternal::headerBuildIDCountOffset);
    uint32_t size = loadField<uint32_t>(file, ContainerInternal::headerSizeOffset);
    uint32_t optionCount = loadField<uint32_t>(file, ContainerInternal::headerOptionCountOffset);
    if (size != file.size())
        return corrupt("H differs from the file's size"_s);
    if (size != ContainerInternal::headerSizeFor(buildIDCount, optionCount))
        return corrupt("H differs from 32 + 72 B + 8 M + 8"_s);
    if (buildIDCount < 1 || buildIDCount > 2)
        return corrupt("B is not 1 or 2"_s);
    if (optionCount != ContainerInternal::headerOptionCount)
        return corrupt("M is not 3"_s);
    if (loadField<uint32_t>(file, ContainerInternal::headerReservedOffset))
        return corrupt("a reserved byte is not zero"_s);

    ArtifactHeader header;
    header.architecture = loadField<uint8_t>(file, ContainerInternal::headerArchitectureOffset);
    header.facts.cpuFeatures = loadField<uint64_t>(file, ContainerInternal::headerCPUFeaturesOffset);
    for (unsigned index = 0; index < buildIDCount; ++index) {
        auto record = file.subspan(ContainerInternal::headerRecordsOffset + ContainerInternal::buildIDRecordBytes * index, ContainerInternal::buildIDRecordBytes);
        uint8_t length = record[1];
        if (record[0] != static_cast<uint8_t>(index + 1))
            return corrupt("a build-ID record has an out-of-range role"_s);
        if (!length || length > ContainerInternal::maximumBuildIDBytes)
            return corrupt("a build-ID record has an out-of-range length"_s);
        if (!ContainerInternal::isAllZero(record.subspan(2, ContainerInternal::buildIDRecordIDOffset - 2))
            || !ContainerInternal::isAllZero(record.subspan(ContainerInternal::buildIDRecordIDOffset + length)))
            return corrupt("a build-ID record's padding is not zero"_s);
        BuildID buildID;
        buildID.size = length;
        memcpySpan(std::span { buildID.bytes }, record.subspan(ContainerInternal::buildIDRecordIDOffset, length));
        if (!index)
            header.facts.mainExecutable = buildID;
        else
            header.facts.engineObject = buildID;
    }
    size_t options = ContainerInternal::headerRecordsOffset + ContainerInternal::buildIDRecordBytes * buildIDCount;
    for (unsigned index = 0; index < ContainerInternal::headerOptionCount; ++index) {
        auto record = file.subspan(options + ContainerInternal::optionRecordBytes * index, ContainerInternal::optionRecordBytes);
        if (loadField<uint16_t>(record, 0) != static_cast<uint16_t>(index))
            return corrupt("an option record has an out-of-range index"_s);
        if (record[2] != ContainerInternal::optionTypeBool)
            return corrupt("an option record has an out-of-range type"_s);
        if (record[3] > 1)
            return corrupt("an option record has an out-of-range value"_s);
        if (!ContainerInternal::isAllZero(record.subspan(4)))
            return corrupt("an option record's padding is not zero"_s);
        header.facts.mustMatch[index] = record[3];
    }
    if (!ContainerInternal::isAllZero(file.last(ContainerInternal::headerTrailerZeroBytes)))
        return corrupt("a reserved byte is not zero"_s);
    size_t checksummed = size - ContainerInternal::headerTrailerBytes;
    if (loadField<uint32_t>(file, checksummed) != ContainerInternal::crc32cOf(file.first(checksummed)))
        return corrupt("the CRC differs"_s);
    return header;
}

HeaderCheck checkArtifactHeader(std::span<const uint8_t> file, std::span<const uint8_t> processHeader)
{
    auto artifact = readArtifactHeader(file);
    if (!artifact)
        return { artifact.error().verdict, String { artifact.error().reason } };
    size_t compared = file.size() - ContainerInternal::headerTrailerBytes;
    if (processHeader.size() == file.size() && equalSpans(file.first(compared), processHeader.first(compared)))
        return { HeaderVerdict::Compatible, { } };
    auto process = readArtifactHeader(processHeader);
    ASSERT(process); // the process's own header, which encodeArtifactHeader wrote
    if (!process)
        return { HeaderVerdict::Incompatible, "header"_s };
    return { HeaderVerdict::Incompatible, ContainerInternal::firstDifferingField(*artifact, *process) };
}

HeaderDigest artifactHeaderDigest(std::span<const uint8_t> header)
{
    Digest256 digest = SHA256::hash(header);
    HeaderDigest result;
    memcpySpan(std::span { result }, std::span { digest }.first(result.size()));
    return result;
}

std::array<uint8_t, bodyEnvelopeBytes> encodeBodyEnvelope(const BodyEnvelope& envelope)
{
    using ContainerInternal::storeField;
    std::array<uint8_t, bodyEnvelopeBytes> result { };
    auto bytes = std::span { result };
    memcpySpan(bytes, std::span { ContainerInternal::bodyTag });
    storeField<uint16_t>(bytes, ContainerInternal::envelopeVersionOffset, ContainerInternal::bodyLayoutVersion);
    storeField<uint16_t>(bytes, ContainerInternal::envelopeSizeOffset, static_cast<uint16_t>(bodyEnvelopeBytes));
    storeField<uint16_t>(bytes, ContainerInternal::envelopeSectionCountOffset, envelope.sectionCount);
    storeField<uint8_t>(bytes, ContainerInternal::envelopeTierOffset, envelope.highestTier);
    auto key = ContainerInternal::keyBytes(envelope.key);
    memcpySpan(bytes.subspan(ContainerInternal::envelopeKeyOffset, key.size()), std::span { key });
    storeField<uint64_t>(bytes, ContainerInternal::envelopeCommitOffset, envelope.version);
    memcpySpan(bytes.subspan(ContainerInternal::envelopeDigestOffset, envelope.headerDigest.size()), std::span { envelope.headerDigest });
    storeField<uint64_t>(bytes, ContainerInternal::envelopeFileSizeOffset, envelope.fileSize);
    storeField<uint32_t>(bytes, ContainerInternal::envelopeLLIntThresholdOffset, envelope.llintThreshold);
    storeField<uint32_t>(bytes, ContainerInternal::envelopeCounterProgressOffset, envelope.counterProgress);
    storeField<uint32_t>(bytes, ContainerInternal::envelopeDirectoryCRCOffset, envelope.directoryCRC);
    storeField<uint32_t>(bytes, ContainerInternal::envelopeCRCOffset, ContainerInternal::crc32cOf(bytes.first(ContainerInternal::envelopeCRCOffset)));
    return result;
}

std::optional<uint64_t> layOutBodySections(std::span<BodyDirectoryEntry> entries)
{
    if (entries.empty() || entries.size() > maximumBodySections)
        return std::nullopt;
    uint64_t end = ContainerInternal::framingBytesFor(entries.size());
    for (size_t index = 0; index < entries.size(); ++index) {
        uint64_t offset = end;
        if (index) {
            auto aligned = ContainerInternal::roundUpToMultipleOfEight(end);
            if (!aligned)
                return std::nullopt;
            offset = *aligned;
        }
        if (entries[index].size > std::numeric_limits<uint64_t>::max() - offset)
            return std::nullopt;
        entries[index].offset = offset;
        end = offset + entries[index].size;
    }
    return end;
}

uint32_t encodeBodyDirectory(std::span<const BodyDirectoryEntry> entries, std::span<uint8_t> directory)
{
    using ContainerInternal::storeField;
    RELEASE_ASSERT(directory.size() == entries.size() * bodyDirectoryEntryBytes);
    for (size_t index = 0; index < entries.size(); ++index) {
        auto& description = sectionKindDescription(entries[index].kind);
        auto entry = directory.subspan(bodyDirectoryEntryBytes * index, bodyDirectoryEntryBytes);
        storeField<uint16_t>(entry, ContainerInternal::entryTypeIdOffset, description.typeId);
        storeField<uint8_t>(entry, ContainerInternal::entryTierOffset, description.tier);
        storeField<uint8_t>(entry, ContainerInternal::entryZeroOffset, 0);
        storeField<uint32_t>(entry, ContainerInternal::entryCRCOffset, entries[index].crc);
        storeField<uint64_t>(entry, ContainerInternal::entryOffsetOffset, entries[index].offset);
        storeField<uint64_t>(entry, ContainerInternal::entrySizeOffset, entries[index].size);
    }
    return ContainerInternal::crc32cOf(directory);
}

std::expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode mode)
{
    BodyValidationStream stream(expectedKey, headerDigest, mode, bytes.size());
    stream.append(bytes);
    return stream.finish();
}

BodyValidationStream::BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode mode, uint64_t fileSize)
    : m_expectedKey(expectedKey)
    , m_headerDigest(ContainerInternal::digestFromSpan(headerDigest))
    , m_mode(mode)
    , m_fileSize(fileSize)
{
    // B1: a file shorter than the envelope has no envelope to read.
    if (fileSize < bodyEnvelopeBytes)
        m_failure = ContainerChecks::size;
}

std::span<const uint8_t> BodyValidationStream::directoryBytes() const
{
    return std::span { m_framing }.subspan(bodyEnvelopeBytes, bodyDirectoryEntryBytes * m_envelope.sectionCount);
}

void BodyValidationStream::append(std::span<const uint8_t> bytes)
{
    while (!bytes.empty()) {
        // After a failure, and past the file's size, the bytes only count toward B1.
        if (m_failure || m_received >= m_fileSize) {
            m_received += bytes.size();
            return;
        }
        if (m_stage != Stage::Sections) {
            // m_framingEnd is at most the file's size, so these bytes are the file's and fit the framing buffer.
            size_t count = static_cast<size_t>(std::min<uint64_t>(bytes.size(), m_framingEnd - m_received));
            memcpySpan(std::span { m_framing }.subspan(static_cast<size_t>(m_received), count), bytes.first(count));
            m_received += count;
            bytes = bytes.subspan(count);
            if (m_received == m_framingEnd) {
                if (m_stage == Stage::Envelope)
                    didReceiveEnvelope();
                else
                    didReceiveDirectory();
            }
            continue;
        }
        size_t count = static_cast<size_t>(std::min<uint64_t>(bytes.size(), m_fileSize - m_received));
        appendSectionBytes(bytes.first(count));
        m_received += count;
        bytes = bytes.subspan(count);
    }
}

void BodyValidationStream::didReceiveEnvelope()
{
    auto envelope = ContainerInternal::checkEnvelope(std::span { m_framing }.first(bodyEnvelopeBytes), m_fileSize, m_expectedKey, m_headerDigest, m_mode);
    if (!envelope) {
        m_failure = envelope.error();
        return;
    }
    m_envelope = *envelope;
    m_framingEnd = ContainerInternal::framingBytesFor(m_envelope.sectionCount);
    // B6: the directory must lie inside the file for its CRC and its entries to be read.
    if (m_framingEnd > m_fileSize) {
        m_failure = ContainerChecks::directory;
        return;
    }
    m_stage = Stage::Directory;
}

void BodyValidationStream::didReceiveDirectory()
{
    auto directory = directoryBytes();
    if (auto failure = ContainerInternal::checkDirectory(directory, m_envelope, m_mode)) {
        m_failure = *failure;
        return;
    }
    if (m_mode == ValidationMode::Full)
        m_requiredSectionsDiffer = !ContainerInternal::holdsRequiredSections(directory, m_envelope.highestTier);
    for (size_t index = 0; index < m_envelope.sectionCount; ++index) {
        auto record = ContainerInternal::directoryRecord(directory, index);
        uint32_t state = ~0u;
        // Only Integrity lets an entry cover framing bytes, which have arrived already; B6 kept the entry inside the file.
        if (record.offset < m_framingEnd) {
            uint64_t end = std::min(record.offset + record.size, m_framingEnd);
            state = crc32cExtend(state, std::span { m_framing }.subspan(static_cast<size_t>(record.offset), static_cast<size_t>(end - record.offset)));
        }
        m_sectionCRCStates[index] = state;
    }
    m_stage = Stage::Sections;
}

// bytes start at file offset m_received, past the framing and inside the file.
void BodyValidationStream::appendSectionBytes(std::span<const uint8_t> bytes)
{
    uint64_t begin = m_received;
    uint64_t end = begin + bytes.size();
    auto directory = directoryBytes();
    for (size_t index = 0; index < m_envelope.sectionCount; ++index) {
        auto record = ContainerInternal::directoryRecord(directory, index);
        uint64_t from = std::max(begin, record.offset);
        uint64_t to = std::min(end, record.offset + record.size);
        if (from < to)
            m_sectionCRCStates[index] = crc32cExtend(m_sectionCRCStates[index], bytes.subspan(static_cast<size_t>(from - begin), static_cast<size_t>(to - from)));
    }
    if (m_mode != ValidationMode::Full || !m_paddingIsZero)
        return;
    // B6 in Full mode laid the sections out by section 4.3, so the only bytes past the framing that no section covers are
    // the gaps between consecutive sections.
    for (size_t index = 1; index < m_envelope.sectionCount; ++index) {
        auto previous = ContainerInternal::directoryRecord(directory, index - 1);
        uint64_t gapBegin = std::max(begin, previous.offset + previous.size);
        uint64_t gapEnd = std::min(end, ContainerInternal::directoryRecord(directory, index).offset);
        if (gapBegin < gapEnd && !ContainerInternal::isAllZero(bytes.subspan(static_cast<size_t>(gapBegin - begin), static_cast<size_t>(gapEnd - gapBegin)))) {
            m_paddingIsZero = false;
            return;
        }
    }
}

std::expected<BodyLayout, ContainerCheck> BodyValidationStream::finish()
{
    if (m_received != m_fileSize)
        return std::unexpected(ContainerChecks::size);
    if (m_failure)
        return std::unexpected(*m_failure);
    // A stream that received the whole file without a failure has checked its envelope and its directory.
    ASSERT(m_stage == Stage::Sections);
    if (!m_paddingIsZero)
        return std::unexpected(ContainerChecks::directory);
    if (m_requiredSectionsDiffer)
        return std::unexpected(ContainerChecks::required);
    auto directory = directoryBytes();
    for (size_t index = 0; index < m_envelope.sectionCount; ++index) {
        if (~m_sectionCRCStates[index] != ContainerInternal::directoryRecord(directory, index).crc)
            return std::unexpected(ContainerChecks::checksum);
    }
    return ContainerInternal::layoutOf(m_envelope, directory);
}

std::expected<BodyEnvelope, ContainerCheck> validateBodyEnvelope(std::span<const uint8_t> fileStart, uint64_t fileSize,
    const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode mode)
{
    return ContainerInternal::checkEnvelope(fileStart, fileSize, expectedKey, headerDigest, mode);
}

std::expected<BodyLayout, ContainerCheck> validateBodyFraming(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode mode, std::span<const SectionKind> checksummedKinds)
{
    auto envelope = ContainerInternal::checkEnvelope(bytes, bytes.size(), expectedKey, headerDigest, mode);
    if (!envelope)
        return std::unexpected(envelope.error());
    uint64_t framingEnd = ContainerInternal::framingBytesFor(envelope->sectionCount);
    if (framingEnd > bytes.size())
        return std::unexpected(ContainerChecks::directory);
    auto directory = bytes.subspan(bodyEnvelopeBytes, static_cast<size_t>(framingEnd - bodyEnvelopeBytes));
    if (auto failure = ContainerInternal::checkDirectory(directory, *envelope, mode))
        return std::unexpected(*failure);
    if (mode == ValidationMode::Full && !ContainerInternal::holdsRequiredSections(directory, envelope->highestTier))
        return std::unexpected(ContainerChecks::required);
    for (SectionKind kind : checksummedKinds) {
        auto index = ContainerInternal::firstRecordOf(directory, kind);
        if (!index)
            continue;
        auto record = ContainerInternal::directoryRecord(directory, *index);
        if (ContainerInternal::crc32cOf(bytes.subspan(static_cast<size_t>(record.offset), static_cast<size_t>(record.size))) != record.crc)
            return std::unexpected(ContainerChecks::checksum);
    }
    return ContainerInternal::layoutOf(*envelope, directory);
}

#if ENABLE(JITCACHE_TWINS)
namespace ContainerTesting {

Vector<uint8_t> buildBody(const BodyEnvelope& stamp, std::span<const Section> sections)
{
    RELEASE_ASSERT(!sections.empty() && sections.size() <= maximumBodySections);
    std::array<BodyDirectoryEntry, maximumBodySections> storage { };
    auto entries = std::span { storage }.first(sections.size());
    for (size_t index = 0; index < sections.size(); ++index) {
        entries[index].kind = sections[index].kind;
        entries[index].size = sections[index].bytes.size();
    }
    auto fileSize = layOutBodySections(entries);
    RELEASE_ASSERT(fileSize);

    Vector<uint8_t> file(FillWith { }, static_cast<size_t>(*fileSize), 0);
    auto bytes = file.mutableSpan();
    for (size_t index = 0; index < sections.size(); ++index) {
        if (!sections[index].bytes.empty())
            memcpySpan(bytes.subspan(static_cast<size_t>(entries[index].offset), sections[index].bytes.size()), sections[index].bytes);
        entries[index].crc = ContainerInternal::crc32cOf(sections[index].bytes);
    }
    BodyEnvelope envelope = stamp;
    envelope.sectionCount = static_cast<uint16_t>(sections.size());
    envelope.fileSize = *fileSize;
    envelope.directoryCRC = encodeBodyDirectory(entries, bytes.subspan(bodyEnvelopeBytes, bodyDirectoryEntryBytes * sections.size()));
    auto envelopeBytes = encodeBodyEnvelope(envelope);
    memcpySpan(bytes, std::span { envelopeBytes });
    return file;
}

} // namespace ContainerTesting
#endif

} // namespace JSC::JITCache
