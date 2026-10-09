#pragma once

#include "JITCachePlatform.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <bit>
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/Platform.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

#if ENABLE(JITCACHE_TWINS)
#include <wtf/Vector.h>
#endif

// Every integer of the artifact is little-endian, as every supported target is, and is read with memcpy-based loads
// (container sub-SPEC).
static_assert(std::endian::native == std::endian::little);

namespace JSC::JITCache {

// The artifact's formats: the section kinds' table (SPEC-integrator.md section 6.1), the header (container sub-SPEC
// section 3) and the body files with their validation (container sub-SPEC section 4). Everything here works on bytes in
// memory: it opens no file, takes no lock and allocates nothing, apart from the test builder at the end.

// The section kinds (SPEC-integrator.md section 6.1). The high byte of a type id names the lane (1 UCB, 2 Image, 3 CB,
// 4 ICs) and the low byte the section family; the directory entry's tier byte separates tiers, 0 for the sections every
// tier shares. A later tier adds entries such as (0x0201, 2) under the same layout version, which is how the format admits
// more than one tier per body. The table is in SectionKind's order, which is the directory's order of (type id, tier).
struct SectionKindDescription {
    uint16_t typeId;
    uint8_t tier;
    ASCIILiteral name; // the lane's name, which the writer's diagnostics use (container sub-SPEC section 8.2)
};

inline constexpr std::array<SectionKindDescription, numberOfSectionKinds> sectionKindDescriptions { {
    { 0x0101, 0, "ucb.identity"_s },
    { 0x0102, 0, "ucb.core"_s },
    { 0x0103, 0, "ucb.feedback"_s },
    { 0x0201, 1, "image.baseline"_s },
    { 0x0202, 1, "baked-facts.baseline"_s },
    { 0x0203, 1, "image-twins.baseline"_s },
    { 0x0301, 1, "cb.state"_s },
    { 0x0302, 1, "cb.summary"_s },
    { 0x0401, 1, "ICsBaseline"_s },
} };

constexpr const SectionKindDescription& sectionKindDescription(SectionKind kind)
{
    return sectionKindDescriptions[static_cast<size_t>(kind)];
}

// The kind a directory entry's (type id, tier) names, or nothing.
constexpr std::optional<SectionKind> sectionKindFor(uint16_t typeId, uint8_t tier)
{
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        if (sectionKindDescriptions[index].typeId == typeId && sectionKindDescriptions[index].tier == tier)
            return static_cast<SectionKind>(index);
    }
    return std::nullopt;
}

// Whether a body whose highest tier is highestTier holds the kind (check B7): a body holds exactly the kinds this returns
// true for, every kind whose tier is at most its highest tier, except image-twins.baseline, which ENABLE(JITCACHE_TWINS)
// builds require and every other build forbids. The writer writes them all, and strict validation checks it.
constexpr bool isSectionRequired(SectionKind kind, uint8_t highestTier)
{
#if !ENABLE(JITCACHE_TWINS)
    if (kind == SectionKind::ImageTwinsBaseline)
        return false;
#endif
    return sectionKindDescription(kind).tier <= highestTier;
}

// The header, cache/header (container sub-SPEC section 3.1), with B build IDs and M = 3 must-match options:
//   0         8     the bytes JITCHEAD
//   8         2     layout version, 1
//   10        1     architecture: 1 x86_64, 2 ARM64
//   11        1     B, 1 or 2
//   12        4     H, the header's size: 32 + 72 * B + 8 * M + 8
//   16        8     the CPU feature vector (SPEC-integrator.md section 5.2)
//   24        4     M, 3 in this version
//   28        4     0
//   32        72*B  build-ID records: u8 role (1 main executable, 2 engine object), u8 length n (1 to 64), u16 0, u32 0,
//                   then 64 bytes holding the ID in the first n and zero after; the main executable's first
//   32+72*B   8*M   option records: u16 index (0 evalMode, 1 useExplicitResourceManagement, 2 useImportDefer), u8 type
//                   (1 Bool), u8 value (0 or 1), u32 0; in index order
//   H-8       4     CRC32C of bytes [0, H - 8)
//   H-4       4     0
enum class HeaderArchitecture : uint8_t { X86_64 = 1, ARM64 = 2 };

constexpr uint8_t thisHeaderArchitecture()
{
#if CPU(X86_64)
    return static_cast<uint8_t>(HeaderArchitecture::X86_64);
#elif CPU(ARM64)
    return static_cast<uint8_t>(HeaderArchitecture::ARM64);
#else
    return 0; // not a JITCache target: no artifact header matches it
#endif
}

constexpr size_t maximumArtifactHeaderBytes = 208; // 32 + 72 * 2 + 8 * 3 + 8
constexpr size_t minimumArtifactHeaderFileBytes = 40; // a shorter file is corrupt (section 3.2)
constexpr size_t maximumArtifactHeaderFileBytes = 4096; // a larger file is corrupt; a reader reads at most one byte more

// What a header records.
struct ArtifactHeader {
    uint8_t architecture { 0 }; // the byte at offset 10, HeaderArchitecture's value in a header this build writes
    ProcessFacts facts; // the build IDs, the must-match options' values and the CPU feature vector
};

struct ArtifactHeaderBytes {
    std::array<uint8_t, maximumArtifactHeaderBytes> bytes { };
    uint32_t size { 0 }; // H
    std::span<const uint8_t> span() const LIFETIME_BOUND { return std::span { bytes }.first(size); }
};

// The header's H bytes for these contents; empty when a build ID it records is not 1 to 64 bytes long. The engine object's
// record is written exactly when facts.engineObject is set.
std::optional<ArtifactHeaderBytes> encodeArtifactHeader(const ArtifactHeader&);

// The process's own header (section 3.1), computed once, from processFacts() and the build's architecture, at the first
// call. start makes that call only once the process facts have passed its build-ID step (SPEC-integrator.md section 3.2,
// step 6), so they hold every build ID; facts without one fail a release assertion. Any thread.
const ArtifactHeaderBytes& expectedHeader();

// Reading a header (section 3.2). A header file is corrupt when it is shorter than 40 bytes, larger than 4 KiB or lacks the
// tag; incompatible when its layout version is not 1, without reading the rest; otherwise corrupt when H differs from the
// file's size or from the formula, B is not 1 or 2, M is not 3, a record has an out-of-range role (the first record's is 1
// and the second's 2), length, index, type or value, a padding or reserved byte is not zero, or the CRC differs.
enum class HeaderVerdict : uint8_t { Compatible, Incompatible, Corrupt };

struct HeaderRejection {
    HeaderVerdict verdict; // Corrupt, or Incompatible for another layout version
    ASCIILiteral reason; // the rule the bytes break
};

// The header's own checks, without the comparison with the running process: maintenance, which may run under another
// binary, reads a header this way (maintenance sub-SPEC section 2). file is the whole header file.
std::expected<ArtifactHeader, HeaderRejection> readArtifactHeader(std::span<const uint8_t> file);

struct HeaderCheck {
    HeaderVerdict verdict;
    // Empty for Compatible. For Corrupt, the rule the bytes break. For Incompatible, "layout version" or the first field
    // that differs, in this order: "architecture", "main executable build ID", "engine object build ID", a CPU feature
    // bit by its predicate's name (SPEC-integrator.md section 5.2), or "CPU feature bit <i>" for a bit no predicate
    // names, and an option by its name.
    String detail;
};

// start's reading of a Consumer's or ConsumerProducer's header: a header that is not corrupt is compatible exactly when
// its bytes [0, H - 8) equal those of processHeader, the process's own header (expectedHeader()).
HeaderCheck checkArtifactHeader(std::span<const uint8_t> file, std::span<const uint8_t> processHeader);

// The header digest (section 3.3): the first 16 bytes of the SHA-256 of the header's H bytes. Every body stamps the
// digest of the header it was written under, and every read of a body requires it to equal the opened artifact's (B4).
using HeaderDigest = std::array<uint8_t, 16>;
HeaderDigest artifactHeaderDigest(std::span<const uint8_t> header);

// Body files (container sub-SPEC section 4): an envelope, a section directory and the sections.
//
// The envelope, 128 bytes at offset 0:
//   0    8   the bytes JITCBODY
//   8    2   layout version, 1
//   10   2   envelope size, 128
//   12   2   N, the number of sections, 1 to 64
//   14   1   the highest tier the body holds, 1 in this version
//   15   1   0
//   16   40  the body key
//   56   8   the commit identifier: nonzero, drawn with cryptographicallyRandomValues for every write of the file
//   64   16  the header digest
//   80   8   the file's size
//   88   4   L: the LLInt threshold the import skips (THREAD Maintenance)
//   92   4   P: the baseline counter's progress (THREAD Maintenance)
//   96   4   CRC32C of the directory
//   100  20  0
//   120  4   CRC32C of bytes [0, 120)
//   124  4   0
// The directory, N entries of 24 bytes at offset 128:
//   0    2   type id
//   2    1   tier
//   3    1   0
//   4    4   CRC32C of the section's bytes
//   8    8   the section's offset from the file's start
//   16   8   the section's size
// The entries strictly increase in (type id, tier). The first section starts at 128 + 24 * N, which is a multiple of 8,
// each next one at the first multiple of 8 at or after the end of the one before, the bytes between sections are zero,
// and the file ends where the last section ends.
constexpr size_t bodyEnvelopeBytes = 128;
constexpr size_t bodyDirectoryEntryBytes = 24;
constexpr unsigned maximumBodySections = 64;
constexpr size_t maximumBodyFramingBytes = bodyEnvelopeBytes + maximumBodySections * bodyDirectoryEntryBytes;

struct BodyEnvelope {
    uint16_t sectionCount { 0 }; // N
    uint8_t highestTier { 0 };
    BodyKey key { };
    uint64_t version { 0 }; // the commit identifier
    HeaderDigest headerDigest { };
    uint64_t fileSize { 0 };
    uint32_t llintThreshold { 0 }; // L
    uint32_t counterProgress { 0 }; // P
    uint32_t directoryCRC { 0 };
};

// The envelope's 128 bytes, its zero bytes and its own CRC included.
std::array<uint8_t, bodyEnvelopeBytes> encodeBodyEnvelope(const BodyEnvelope&);

struct BodyDirectoryEntry {
    SectionKind kind { SectionKind::UCBIdentity };
    uint64_t size { 0 };
    uint64_t offset { 0 }; // set by layOutBodySections
    uint32_t crc { 0 }; // the CRC32C of the section's bytes
};

// Lays out sections whose kinds and sizes are given in directory order (section 4.3): sets each entry's offset and returns
// the file's size; empty when there are no entries or more than 64, or when the arithmetic overflows.
std::optional<uint64_t> layOutBodySections(std::span<BodyDirectoryEntry>);

// Writes the directory's 24 * N bytes into directory, which holds exactly that many, and returns the directory's CRC32C,
// the envelope's directoryCRC.
uint32_t encodeBodyDirectory(std::span<const BodyDirectoryEntry>, std::span<uint8_t> directory);

// Body validation (section 4.5). Each check has an integrity part, which both modes run, and some have a structure part,
// which only Full adds. The checks run in this order, and validation names the first that fails:
//
//   B1 container.size           integrity: the file is at least 128 bytes and its size equals the envelope's file size
//   B2 container.envelope       integrity: the tag, layout version 1, envelope size 128, N from 1 to 64, the envelope's
//                               CRC; structure: the zero bytes and a highest tier of 1
//   B3 container.key            integrity: the envelope's key equals the key the file's name encodes
//   B4 container.header-digest  integrity: the envelope's header digest equals the opened artifact's
//   B5 container.version        structure: the commit identifier is not zero
//   B6 container.directory      integrity: the directory lies inside the file and its CRC holds; every entry's offset and
//                               size lie inside the file without overflow; structure: every entry's zero byte, every
//                               (type id, tier) a section kind, entries strictly increasing, offsets and sizes as
//                               section 4.3 lays them out, zero padding
//   B7 container.required       structure: the sections present are exactly those isSectionRequired names for the
//                               highest tier
//   B8 container.checksum       integrity: each section's CRC
//
// Integrity is what normal mode checks (THREAD Session): B4 ties the body to the header start checked, B3 checks the key
// and B2, B6 and B8 the checksums, with the sizes and bounds that keep those checksums inside the file.
enum class ValidationMode : uint8_t { Integrity, Full };

using ContainerCheck = ASCIILiteral; // the failing check's name in the table above, such as "container.key"

namespace ContainerChecks {
inline constexpr ContainerCheck size = "container.size"_s; // B1
inline constexpr ContainerCheck envelope = "container.envelope"_s; // B2
inline constexpr ContainerCheck key = "container.key"_s; // B3
inline constexpr ContainerCheck headerDigest = "container.header-digest"_s; // B4
inline constexpr ContainerCheck version = "container.version"_s; // B5
inline constexpr ContainerCheck directory = "container.directory"_s; // B6
inline constexpr ContainerCheck required = "container.required"_s; // B7
inline constexpr ContainerCheck checksum = "container.checksum"_s; // B8
} // namespace ContainerChecks

struct SectionExtent {
    uint64_t offset;
    uint64_t size;
    friend bool operator==(const SectionExtent&, const SectionExtent&) = default;
};
struct BodyLayout {
    uint64_t version; // the commit identifier
    uint8_t highestTier;
    uint32_t llintThreshold; // L
    uint32_t counterProgress; // P
    std::array<std::optional<SectionExtent>, numberOfSectionKinds> sections; // by SectionKind; nullopt when absent
    friend bool operator==(const BodyLayout&, const BodyLayout&) = default;
};

// With Integrity, the layout records the first directory entry of each section kind and ignores an entry whose
// (type id, tier) names no kind; the writer produces neither case, and normal mode trusts it not to (section 7.4). B8
// checks every entry's CRC in both modes. bytes is the whole file.
std::expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode);

// The same checks, in the same order, over the file's bytes fed in order in chunks of any size down to one byte: the
// writer's reread (section 8.2, step 6) feeds it through the staging buffer, and validateBody feeds its whole span to one
// stream. It keeps the envelope and directory (at most 128 + 64 * 24 bytes) and a CRC per section; allocates nothing.
// The file is the bytes appended: when their count differs from fileSize, B1 fails.
class BodyValidationStream {
    WTF_MAKE_NONCOPYABLE(BodyValidationStream);
public:
    BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode, uint64_t fileSize);
    void append(std::span<const uint8_t>); // the file's bytes in order
    std::expected<BodyLayout, ContainerCheck> finish(); // after the last byte; names the first check that failed

private:
    // The stream keeps the envelope until it has arrived, then the directory, then checksums the sections.
    enum class Stage : uint8_t { Envelope, Directory, Sections };
    void didReceiveEnvelope();
    void didReceiveDirectory();
    void appendSectionBytes(std::span<const uint8_t>);
    std::span<const uint8_t> directoryBytes() const LIFETIME_BOUND;

    const BodyKey m_expectedKey;
    const HeaderDigest m_headerDigest;
    const ValidationMode m_mode;
    const uint64_t m_fileSize;
    uint64_t m_received { 0 }; // the bytes appended so far, those past fileSize included
    uint64_t m_framingEnd { bodyEnvelopeBytes }; // where the bytes the stage keeps end: 128, then 128 + 24 * N
    Stage m_stage { Stage::Envelope };
    // A failed check that no later byte can precede in the order of checks; the stream stops reading bytes once it is set.
    // Only B1, for the count of bytes appended, can still come first.
    std::optional<ContainerCheck> m_failure;
    bool m_requiredSectionsDiffer { false }; // B7 failed (Full); only the padding (B6) can still fail before it
    bool m_paddingIsZero { true }; // every padding byte so far was zero (B6, Full)
    BodyEnvelope m_envelope; // decoded once B1 to B5 passed
    std::array<uint8_t, maximumBodyFramingBytes> m_framing { }; // the envelope and the directory, as they arrived
    std::array<uint32_t, maximumBodySections> m_sectionCRCStates { }; // each directory entry's running crc32cExtend state
};

// B1 to B5 over the envelope alone, for a reader that reads no more of the body: maintenance judges bodies this way, in
// Full mode (maintenance sub-SPEC section 2). fileStart holds the file's first bytes, at least 128 of them when the file
// has that many, and fileSize is the file's size.
std::expected<BodyEnvelope, ContainerCheck> validateBodyEnvelope(std::span<const uint8_t> fileStart, uint64_t fileSize,
    const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode);

// The scoring read's checks (container sub-SPEC section 7.3): B1 to B7 on the envelope and the directory, and B8 on the
// sections of checksummedKinds alone. It reads the envelope, the directory and those sections of bytes, the whole file,
// and nothing else, so the padding, which lies outside them, is left to the full validation an import runs.
std::expected<BodyLayout, ContainerCheck> validateBodyFraming(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode, std::span<const SectionKind> checksummedKinds);

#if ENABLE(JITCACHE_TWINS)
namespace ContainerTesting {

struct Section {
    SectionKind kind;
    std::span<const uint8_t> bytes;
};

// A body file as the writer lays it out (sections 4.1 to 4.3), from sections in the order given: the envelope takes its
// key, commit identifier, header digest, highest tier, L and P from stamp, and N, the file's size and both directory and
// envelope checksums are computed. Test C3 builds its bodies with it, and so do the store's, the writer's and maintenance's
// tests for synthetic bodies.
Vector<uint8_t> buildBody(const BodyEnvelope& stamp, std::span<const Section>);

} // namespace ContainerTesting
#endif

} // namespace JSC::JITCache
