#pragma once

#include "jitcache/base/BodyKey.h"
#include "jitcache/store/JITCachePlatform.h"
#include "jitcache/store/ValidatedBody.h"
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

// The artifact's formats: the section kinds' table, the header, the body format with its validation, and the pack that
// holds the bodies. Everything here works on bytes in memory: it opens no file, takes no lock and allocates nothing,
// apart from the test builder at the end. No lane includes it: a lane sees a body through ValidatedBody.h.

// Every integer of the artifact is little-endian, as every supported target is, and is read with memcpy-based loads.
static_assert(std::endian::native == std::endian::little);

namespace JSC::JITCache {

// When a body holds a section kind, which container.required checks:
//   Required    every body whose highest tier is at least the kind's tier
//   TwinsOnly   as Required in ENABLE(JITCACHE_TWINS) builds; every other build forbids it
//   ModuleOnly  no tier requires it, and only a module's body may hold it
enum class SectionPresence : uint8_t { Required, TwinsOnly, ModuleOnly };

// The section kinds' table, from the rows of JITCACHE_FOR_EACH_SECTION_KIND (ValidatedBody.h), in SectionKind's order,
// which is the directory's order of (type id, tier). A later tier adds rows such as (0x0201, 2) under the same layout
// version, which is how the format admits more than one tier per body.
struct SectionKindDescription {
    uint16_t typeId;
    uint8_t tier;
    ASCIILiteral name; // the lane's name, which the writer's diagnostics use
    SectionPresence presence;
};

#define JITCACHE_SECTION_KIND_DESCRIPTION(kind, typeId, tier, name, presence) { typeId, tier, name, SectionPresence::presence },
inline constexpr std::array<SectionKindDescription, numberOfSectionKinds> sectionKindDescriptions { {
    JITCACHE_FOR_EACH_SECTION_KIND(JITCACHE_SECTION_KIND_DESCRIPTION)
} };
#undef JITCACHE_SECTION_KIND_DESCRIPTION

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

// Whether a body whose highest tier is highestTier must hold the kind, which container.required checks: every kind whose
// tier is at most its highest tier, by its presence. A body holds exactly the kinds this returns true for, plus at most
// the ModuleOnly ones, which only a module's body may hold. The writer writes every required kind, and Full validation
// checks it.
constexpr bool isSectionRequired(SectionKind kind, uint8_t highestTier)
{
    auto& description = sectionKindDescription(kind);
    switch (description.presence) {
    case SectionPresence::Required:
        return description.tier <= highestTier;
    case SectionPresence::TwinsOnly:
#if ENABLE(JITCACHE_TWINS)
        return description.tier <= highestTier;
#else
        return false;
#endif
    case SectionPresence::ModuleOnly:
        return false;
    }
    return false;
}

// The header, cache/header, with B build IDs and M = 3 must-match options:
//   0         8     the bytes JITCHEAD
//   8         2     layout version, 2
//   10        1     architecture: 1 x86_64, 2 ARM64
//   11        1     B, 1 or 2
//   12        4     H, the header's size: 40 + 72 * B + 8 * M + 8
//   16        8     the CPU feature vector (JITCachePlatform.h)
//   24        4     M, 3 in this version
//   28        1     the app record's flag: 0 when the header names no app, 1 when it names the app at offset 32
//   29        3     0
//   32        8     the app record's digest: the app the config that created the artifact named; 0 when the flag is 0
//   40        72*B  build-ID records: u8 role (1 main executable, 2 engine object), u8 length n (1 to 64), u16 0, u32 0,
//                   then 64 bytes holding the ID in the first n and zero after; the main executable's first
//   40+72*B   8*M   option records: u16 index (0 evalMode, 1 useExplicitResourceManagement, 2 useImportDefer), u8 type
//                   (1 Bool), u8 value (0 or 1), u32 0; in index order
//   H-8       4     CRC32C of bytes [0, H - 8)
//   H-4       4     0
// The app record is bytes [28, 40). The app is a config's, not the process's, so the headers of one process differ at
// most there, and any 64-bit value is an app: only the flag says that a header names none.
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

constexpr size_t maximumArtifactHeaderBytes = 216; // 40 + 72 * 2 + 8 * 3 + 8
constexpr size_t minimumArtifactHeaderFileBytes = 48; // a shorter file is corrupt
constexpr size_t maximumArtifactHeaderFileBytes = 4096; // a larger file is corrupt; a reader reads at most one byte more

// What a header records.
struct ArtifactHeader {
    uint8_t architecture { 0 }; // the byte at offset 10, HeaderArchitecture's value in a header this build writes
    ProcessFacts facts; // the build IDs, the must-match options' values and the CPU feature vector
    std::optional<uint64_t> app; // the app record: the app the header names, or none
};

struct ArtifactHeaderBytes {
    std::array<uint8_t, maximumArtifactHeaderBytes> bytes { };
    uint32_t size { 0 }; // H
    std::span<const uint8_t> span() const LIFETIME_BOUND { return std::span { bytes }.first(size); }
};

// The header's H bytes for these contents; empty when a build ID it records is not 1 to 64 bytes long. The engine object's
// record is written exactly when facts.engineObject is set.
std::optional<ArtifactHeaderBytes> encodeArtifactHeader(const ArtifactHeader&);

// The process's own header, computed once, from processFacts() and the build's architecture, at the first call, with an
// app record that names no app. start makes that call only once the process facts have passed its build-ID check, so they
// hold every build ID; facts without one fail a release assertion. Any thread.
const ArtifactHeaderBytes& expectedHeader();

// The header a creation writes for a config's app: expectedHeader() when appDigest is empty, and otherwise a copy whose
// app record names it, with the CRC computed again. Any thread, under the same condition as expectedHeader().
ArtifactHeaderBytes headerNamingApp(std::optional<uint64_t> appDigest);

// Reading a header. A header file is corrupt when it is shorter than 48 bytes, larger than 4 KiB or lacks the tag;
// incompatible when its layout version is not 2, without reading the rest; otherwise corrupt when H differs from the file's
// size or from the formula, B is not 1 or 2, M is not 3, a record has an out-of-range role (the first record's is 1 and
// the second's 2), length, index, type or value, the app record's flag is neither 0 nor 1 or is 0 beside a digest other
// than 0, a padding or reserved byte is not zero, or the CRC differs.
enum class HeaderVerdict : uint8_t { Compatible, Incompatible, Corrupt, AnotherApp };

struct HeaderRejection {
    HeaderVerdict verdict; // Corrupt, or Incompatible for another layout version
    ASCIILiteral reason; // the rule the bytes break
};

// The header's own checks, without the comparison with the running process or with an app: maintenance, which may run
// under another binary and has no config, reads a header this way. file is the whole header file.
std::expected<ArtifactHeader, HeaderRejection> readArtifactHeader(std::span<const uint8_t> file);

struct HeaderCheck {
    HeaderVerdict verdict;
    // Empty for Compatible. For Corrupt, the rule the bytes break. For Incompatible, "layout version" or the first field
    // that differs, in this order: "architecture", "main executable build ID", "engine object build ID", a CPU feature
    // bit by its predicate's name, or "CPU feature bit <i>" for a bit no predicate names, and an option by its name. For
    // AnotherApp, both apps as 16 lowercase hex digits: "app <the header's> in the cache, <the config's> in this run".
    String detail;
};

// start's reading of a Consumer's or ConsumerProducer's header. A header that is not corrupt is compatible exactly when its
// bytes [0, H - 8) equal those of processHeader, the process's own header (expectedHeader()), outside the app record. A
// compatible header is made for another app (AnotherApp) exactly when it names an app, the config names one too in
// appDigest, and the two differ; a header or a config that names no app gates nothing. The app is compared only on a
// compatible header, so a header both incompatible and made for another app is Incompatible.
HeaderCheck checkArtifactHeader(std::span<const uint8_t> file, std::span<const uint8_t> processHeader, std::optional<uint64_t> appDigest);

// The header digest: the first 16 bytes of the SHA-256 of the header's H bytes, its app record included. Every body stamps
// the digest of the header it was written under, and every read of a body requires it to equal the opened artifact's
// (container.header-digest). start digests the bytes it read from cache/header or wrote there, never bytes a VM computes,
// since a VM whose config names no app opens an artifact whose header may name one.
using HeaderDigest = std::array<uint8_t, 16>;
HeaderDigest artifactHeaderDigest(std::span<const uint8_t> header);

// A body: an envelope, a section directory and the sections. A body lives in the pack, as the bytes of a record that
// follow the record's header (the pack's description below), and every offset here counts from the body's first byte.
//
// The envelope, 128 bytes at offset 0:
//   0    8   the bytes JITCBODY
//   8    2   layout version, 1
//   10   2   envelope size, 128
//   12   2   N, the number of sections, 1 to 64
//   14   1   the highest tier the body holds: 0 without an image, 1 for a baseline capture
//   15   1   0
//   16   40  the body key
//   56   8   the commit identifier: nonzero, drawn with cryptographicallyRandomValues for every commit of the body
//   64   16  the header digest
//   80   8   the body's size, which equals S in the header of the record that holds it
//   88   4   L: the LLInt threshold the import skips
//   92   4   P: the baseline counter's progress
//   96   4   CRC32C of the directory
//   100  20  0
//   120  4   CRC32C of bytes [0, 120)
//   124  4   0
// The directory, N entries of 24 bytes at offset 128:
//   0    2   type id
//   2    1   tier
//   3    1   0
//   4    4   CRC32C of the section's bytes
//   8    8   the section's offset from the body's start
//   16   8   the section's size
// The entries strictly increase in (type id, tier). The first section starts at 128 + 24 * N, which is a multiple of 8,
// each next one at the first multiple of 8 at or after the end of the one before, the bytes between sections are zero,
// and the body ends where the last section ends.
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
    uint64_t fileSize { 0 }; // the body's size
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

// Lays out sections whose kinds and sizes are given in directory order, as the directory's description above places them:
// sets each entry's offset and returns the body's size; empty when there are no entries or more than 64, or when the
// arithmetic overflows.
std::optional<uint64_t> layOutBodySections(std::span<BodyDirectoryEntry>);

// Writes the directory's 24 * N bytes into directory, which holds exactly that many, and returns the directory's CRC32C,
// the envelope's directoryCRC.
uint32_t encodeBodyDirectory(std::span<const BodyDirectoryEntry>, std::span<uint8_t> directory);

// Body validation. Each check has an integrity part, which both modes run, and some have a structure part, which only
// Full adds. The checks run in this order, and validation names the first that fails:
//
//   container.size           integrity: the body is at least 128 bytes and its size equals the envelope's
//   container.envelope       integrity: the tag, layout version 1, envelope size 128, N from 1 to 64, the envelope's CRC;
//                            structure: the zero bytes and a highest tier of 0 or 1
//   container.key            integrity: the envelope's key equals the key the reader asked for
//   container.header-digest  integrity: the envelope's header digest equals the opened artifact's
//   container.version        structure: the commit identifier is not zero
//   container.directory      integrity: the directory lies inside the body and its CRC holds; every entry's offset and
//                            size lie inside the body without overflow; structure: every entry's zero byte, every
//                            (type id, tier) a section kind, entries strictly increasing, offsets and sizes as
//                            layOutBodySections places them, zero padding
//   container.required       structure: the sections present are those isSectionRequired names for the highest tier,
//                            plus at most ucb.analysis, which only a body whose key's identity kind is Module may hold
//   container.checksum       integrity: each section's CRC
//
// Integrity is what OpenedArtifact::open checks: the header digest ties the body to the header start checked, the key
// check ties it to its key, and the envelope, directory and section checksums cover its bytes, with the sizes and bounds
// that keep those checksums inside the body. A read that needs only some sections checks less (validateBodyFraming).
// Full adds the structure parts, which the writer's reread of each body it commits and compaction run, so the pack
// holds only bodies laid out as the writer lays them out.
enum class ValidationMode : uint8_t { Integrity, Full };

using ContainerCheck = ASCIILiteral; // the failing check's name in the table above, such as "container.key"

namespace ContainerChecks {
inline constexpr ContainerCheck size = "container.size"_s;
inline constexpr ContainerCheck envelope = "container.envelope"_s;
inline constexpr ContainerCheck key = "container.key"_s;
inline constexpr ContainerCheck headerDigest = "container.header-digest"_s;
inline constexpr ContainerCheck version = "container.version"_s;
inline constexpr ContainerCheck directory = "container.directory"_s;
inline constexpr ContainerCheck required = "container.required"_s;
inline constexpr ContainerCheck checksum = "container.checksum"_s;
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
// (type id, tier) names no kind; the writer produces neither case, which its Full reread checks, so a reader trusts the
// pack to hold neither. container.checksum checks every entry's CRC in both modes. bytes is the whole body.
std::expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode);

// The same checks, in the same order, over the body's bytes fed in order in chunks of any size down to one byte: the
// writer's reread of a record it wrote feeds it through the staging buffer, and validateBody feeds its whole span to one
// stream. It keeps the envelope and directory (at most 128 + 64 * 24 bytes) and a CRC per section; allocates nothing.
// The body is the bytes appended: when their count differs from fileSize, container.size fails.
class BodyValidationStream {
    WTF_MAKE_NONCOPYABLE(BodyValidationStream);
public:
    BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode, uint64_t fileSize);
    void append(std::span<const uint8_t>); // the body's bytes in order
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
    // Only container.size, for the count of bytes appended, can still come first.
    std::optional<ContainerCheck> m_failure;
    bool m_requiredSectionsDiffer { false }; // container.required failed (Full); only the padding can still fail before it
    bool m_paddingIsZero { true }; // every padding byte so far was zero (container.directory, Full)
    BodyEnvelope m_envelope; // decoded once the checks up to container.version passed
    std::array<uint8_t, maximumBodyFramingBytes> m_framing { }; // the envelope and the directory, as they arrived
    std::array<uint32_t, maximumBodySections> m_sectionCRCStates { }; // each directory entry's running crc32cExtend state
};

// The checks of the reads that need only some of a body's sections, the scoring read and readSection: the integrity
// parts of the checks from container.size to container.directory on the envelope and the directory, and
// container.checksum on the sections of checksummedKinds alone. It reads the envelope, the directory and those sections
// of bytes, the whole body, and nothing else.
std::expected<BodyLayout, ContainerCheck> validateBodyFraming(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, std::span<const SectionKind> checksummedKinds);

// The pack, cache/pack: one log-structured file holding every body. It is only ever appended to, except for its root
// and its retired word, and a reader sees exactly the bytes below the valid end it loaded from the root.
//
// The prefix, 64 bytes at offset 0:
//   0    8   the bytes JITCPACK
//   8    2   layout version, 2
//   10   2   prefix size, 64
//   12   4   0
//   16   16  the artifact header digest (artifactHeaderDigest)
//   32   8   the pack identifier: nonzero, drawn with cryptographicallyRandomValues when the file is created
//   40   4   CRC32C of bytes [0, 40)
//   44   4   0
//   48   8   retired: 0, or 1 once a compaction is about to rename another pack over this file
//   56   8   the root: the valid end, the first byte past the committed data. Within a file it only grows, so it also
//            tells a reader that something was committed. The producer stores it with one atomic release store
//            through a shared writable mapping, after every byte below it, and readers load it with acquire from
//            their own mappings; an aligned 8-byte word is never seen half-written.
// Past the prefix lie records and index blocks, each at a multiple of 8, with zeros between. The valid bytes always end
// with an index block, whose trailer leads from the root to the latest index.
// A record, a 64-byte header followed by the body's bytes exactly as the envelope and directory above lay out a body:
//   0    40  the body key
//   40   8   S, the body's size
//   48   8   the body's commit identifier
//   56   4   CRC32C of bytes [0, 56)
//   60   4   0
// An index block, a 32-byte header, N entries of 80 bytes and an 8-byte trailer:
//   0    8   the bytes JITCINDX
//   8    8   the previous block's offset: 0 for a full index, which lists every live key, else the block that ended the
//            valid bytes before this one, a full index or a delta
//   16   4   kind: 0 full, 1 delta
//   20   4   N
//   24   4   CRC32C of bytes [0, 24) followed by the entries
//   28   4   0
//   32 + 80 * N   8   the trailer: the block's own offset
// An index entry; a full index lists live keys in strictly increasing byte order, and a delta lists changes in the order
// they apply, the last entry for a key winning:
//   0    40  the body key
//   40   8   the record's offset
//   48   8   S
//   56   8   the commit identifier
//   64   4   L
//   68   4   P
//   72   1   the body's highest tier
//   73   7   0
constexpr size_t packPrefixBytes = 64;
constexpr size_t packRetiredOffset = 48;
constexpr size_t packRootOffset = 56;
constexpr size_t packRecordHeaderBytes = 64;
constexpr size_t packIndexBlockHeaderBytes = 32;
constexpr size_t packIndexEntryBytes = 80;
constexpr size_t packIndexTrailerBytes = 8;
constexpr uint64_t packAlignment = 8;
static_assert(!(packRetiredOffset % alignof(uint64_t)), "the retired word is stored and loaded whole through a page-aligned mapping");
static_assert(!(packRootOffset % alignof(uint64_t)), "the root is stored and loaded whole through page-aligned mappings");

namespace ContainerChecks {
inline constexpr ContainerCheck pack = "container.pack"_s; // the pack's own structure: prefix, root, records, index blocks
} // namespace ContainerChecks
using PackRejection = ASCIILiteral; // why the pack fails container.pack

// The bytes an index block of count entries takes: its header, its entries and its trailer.
constexpr uint64_t packIndexBlockBytes(uint32_t count) { return packIndexBlockHeaderBytes + packIndexEntryBytes * count + packIndexTrailerBytes; }
// The first multiple of 8 at or after offset; empty on overflow.
std::optional<uint64_t> packAligned(uint64_t offset);
// The bytes a record holding a body of bodySize takes, its header and padding included; empty on overflow.
std::optional<uint64_t> packRecordExtent(uint64_t bodySize);

struct PackPrefix {
    HeaderDigest headerDigest { };
    uint64_t identifier { 0 };
};
// A new file's prefix: retired 0, and validEnd in the root.
std::array<uint8_t, packPrefixBytes> encodePackPrefix(const PackPrefix&, uint64_t validEnd);
// The prefix's own checks: tag, version, size, zero bytes, CRC, and the artifact header digest. prefix holds at least the
// first 48 bytes of the file; the retired word and the root, which change, are loaded on their own.
std::expected<PackPrefix, PackRejection> decodePackPrefix(std::span<const uint8_t> prefix, std::span<const uint8_t, 16> headerDigest);
// decodePackPrefix's last check, so a prefix it rejects with this passed every other check, the CRC over the digest
// included: a sound pack made under another artifact header.
inline constexpr PackRejection packOfAnotherHeader = "the pack belongs to another artifact header"_s;
// Whether prefix carries the pack's tag and a layout version other than this build's: a pack another binary wrote, which is
// incompatible rather than damaged. Every layout keeps the tag and the version in its first 10 bytes, while the CRC's place
// depends on the layout.
bool isOtherPackLayout(std::span<const uint8_t> prefix);

struct PackRecordHeader {
    BodyKey key { };
    uint64_t size { 0 };
    uint64_t version { 0 };
};
std::array<uint8_t, packRecordHeaderBytes> encodePackRecordHeader(const PackRecordHeader&);
std::expected<PackRecordHeader, PackRejection> decodePackRecordHeader(std::span<const uint8_t, packRecordHeaderBytes>);

enum class PackIndexKind : uint32_t { Full = 0, Delta = 1 };
struct PackIndexBlock {
    PackIndexKind kind { PackIndexKind::Delta };
    uint64_t previous { 0 };
    uint32_t count { 0 };
};

struct PackIndexEntry {
    BodyKey key { };
    uint64_t offset { 0 };
    uint64_t size { 0 };
    uint64_t version { 0 };
    uint32_t llintThreshold { 0 };
    uint32_t counterProgress { 0 };
    uint8_t highestTier { 0 };
    friend bool operator==(const PackIndexEntry&, const PackIndexEntry&) = default;
};

// The block's header, its CRC computed over its first 24 bytes and entries, the N * 80 bytes that follow it.
std::array<uint8_t, packIndexBlockHeaderBytes> encodePackIndexBlockHeader(const PackIndexBlock&, std::span<const uint8_t> entries);
// Completes a block whose entries are in place: bytes holds exactly packIndexBlockBytes(block.count) bytes and receives
// the header and the trailer, which gives offset, the block's own start in the pack.
void sealPackIndexBlock(const PackIndexBlock&, uint64_t offset, std::span<uint8_t> bytes);
// The header's checks: tag, kind, zero bytes, and a full index naming no previous block. The CRC needs the entries, which
// checkPackIndexBlockCRC checks.
std::expected<PackIndexBlock, PackRejection> decodePackIndexBlockHeader(std::span<const uint8_t, packIndexBlockHeaderBytes>);
std::optional<PackRejection> checkPackIndexBlockCRC(std::span<const uint8_t, packIndexBlockHeaderBytes>, std::span<const uint8_t> entries);
uint64_t decodePackIndexTrailer(std::span<const uint8_t, packIndexTrailerBytes>); // the offset the trailer gives
void encodePackIndexEntry(const PackIndexEntry&, std::span<uint8_t> entry); // entry holds exactly 80 bytes
// An entry's own checks: its key, its zero bytes, an offset that is a multiple of 8 outside the prefix, and a commit
// identifier that is not zero.
std::expected<PackIndexEntry, PackRejection> decodePackIndexEntry(std::span<const uint8_t> entry);

// Whether an index entry repeats its body's envelope: commit identifier, tier, L and P. Compaction requires it of every
// body it keeps.
bool packEntryRepeatsEnvelope(const PackIndexEntry&, const BodyLayout&);

#if ENABLE(JITCACHE_TWINS)
namespace ContainerTesting {

struct Section {
    SectionKind kind;
    std::span<const uint8_t> bytes;
};

// A body as the writer lays it out, from sections in the order given: the envelope takes its key, commit identifier,
// header digest, highest tier, L and P from stamp, and N, the body's size and both directory and envelope checksums are
// computed. The container's, the store's, the writer's and maintenance's tests build their synthetic bodies with it.
Vector<uint8_t> buildBody(const BodyEnvelope& stamp, std::span<const Section>);

} // namespace ContainerTesting
#endif

} // namespace JSC::JITCache
