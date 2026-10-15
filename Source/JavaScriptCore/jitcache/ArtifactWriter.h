#pragma once

#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

class MappedPack;
class OpenedArtifact;
class ProducerBudget;
class ProducerLock;
struct PackIndexEntry;

// The checks a commit fails at: the budget's own fault for a refused charge, and the writer's steps. The capture glue
// raises each as a recording fault with the failure's detail.
namespace WriterChecks {
inline constexpr ASCIILiteral budgetLimit = "budget.limit"_s; // the staging charge, or rewriteSection's copy
inline constexpr ASCIILiteral openPack = "writer.open"_s; // opening cache/pack for writing, and mapping its prefix
inline constexpr ASCIILiteral section = "writer.section"_s; // sources whose sizes give no layout, or a source that broke its size or failed
inline constexpr ASCIILiteral write = "writer.write"_s; // the record or the index block
inline constexpr ASCIILiteral reread = "writer.reread"_s; // the record read back fails its checks
// The opened artifact cannot follow the root the commit stored. The store itself cannot fail, so no fault hook names it.
inline constexpr ASCIILiteral publish = "writer.publish"_s;
#if ENABLE(JITCACHE_TWINS)
// rewriteSection had nothing it could rewrite: open did not answer Found, the kind names no section the body's highest
// tier requires, or the range does not lie inside that section. Nothing is written when it fails.
inline constexpr ASCIILiteral rewrite = "writer.rewrite"_s;
#endif
} // namespace WriterChecks

// The steps a compaction fails at, each a storage failure, which a delta raises as a recording fault; a body that fails
// its checks while it is copied is invalid material instead, unless the compaction drops damaged bodies.
namespace CompactionChecks {
inline constexpr ASCIILiteral create = "compaction.create"_s;
inline constexpr ASCIILiteral write = "compaction.write"_s;
inline constexpr ASCIILiteral reread = "compaction.reread"_s;
inline constexpr ASCIILiteral publish = "compaction.publish"_s; // the retired mark and the rename
} // namespace CompactionChecks

// The capture glue hands an accepted capture to the writer as an ordered list of section sources. Each source is a
// section kind, a size, and either the section's bytes in memory or a function through which an object streams them, so
// the writer sees no lane type and reads no lane format.
using SectionSink = ScopedLambda<bool(std::span<const uint8_t>)>; // false once the commit has failed; the source then stops

struct SectionSource {
    SectionKind kind;
    uint64_t size; // the bytes the source promises
    std::span<const uint8_t> bytes; // a section held in memory: size == bytes.size()
    bool (*stream)(const void* object, const SectionSink&) { nullptr }; // or a section its object streams
    const void* object { nullptr };

    static SectionSource inMemory(SectionKind, std::span<const uint8_t>);
    static SectionSource streamed(SectionKind, uint64_t size, bool (*)(const void*, const SectionSink&), const void* object);
};

class CommitSections { // copies its sources; borrows the bytes and objects they name, which outlive the commit
public:
    explicit CommitSections(std::span<const SectionSource>); // at most numberOfSectionKinds, kinds strictly increasing
    std::span<const SectionSource> sources() const LIFETIME_BOUND;

private:
    std::array<SectionSource, numberOfSectionKinds> m_sources;
    unsigned m_count { 0 };
};

struct CommitStamp {
    BodyKey key;
    uint32_t llintThreshold; // L
    uint32_t counterProgress; // P, 0 when the counter does not travel
    uint8_t highestTier;
};
struct CommitResult {
    uint64_t version;
    uint64_t fileSize; // the body's size
};
// Thread CPU nanoseconds of a commit's parts, which the capture glue passes while a bench report is open and writes in the
// capture event. A part the commit did not reach stays 0.
struct CommitTiming {
    std::array<uint64_t, numberOfSectionKinds> streamNanoseconds { }; // by SectionKind; 0 for a kind the commit lacks
    uint64_t rereadNanoseconds { 0 };
    uint64_t publishNanoseconds { 0 }; // the index block, the root's store and the index update
};
struct CommitFailure {
    ASCIILiteral check; // budget.limit or a writer.* step
    String detail;
};

struct CompactionResult {
    uint64_t bodies { 0 }; // in the new pack
    uint64_t packBytes { 0 }; // the new pack's valid end
    uint64_t replacedPackBytes { 0 }; // the valid end of the pack it replaced
    uint64_t nanoseconds { 0 }; // thread CPU time, when timed
    Vector<std::pair<BodyKey, ASCIILiteral>> droppedBodies; // dropsDamagedBodies: each body left out and the check it failed
};
struct CompactionFailure {
    // Storage: a compaction.* step. Budget: a refused charge. Damaged: a body failed the check named.
    enum class Kind : uint8_t { Storage, Budget, Damaged };
    Kind kind;
    ASCIILiteral check;
    String detail;
};
// A pack to compact: its mapping, its valid end and its index as a reader built them, and a descriptor of the same file
// opened for writing, which receives the retired mark.
struct CompactionSource {
    const MappedPack& pack;
    uint64_t validEnd;
    std::span<const PackIndexEntry> entries; // the live entries, in strictly increasing key order
    int writableFd;
};
struct CompactionTarget {
    CompactionTarget(int cacheFd, std::span<const uint8_t, 16> headerDigest)
        : cacheFd(cacheFd)
        , headerDigest(headerDigest)
    {
    }

    int cacheFd;
    std::span<const uint8_t, 16> headerDigest;
    ProducerBudget* budget { nullptr }; // a producing VM charges the new index's buffer; maintenance passes none
    bool dropsDamagedBodies { false }; // maintenance: a body that fails its checks is left out instead of failing
#if ENABLE(JITCACHE_TWINS)
    std::optional<ASCIILiteral> faultForTesting; // the compaction.* step that fails as if its call had failed with EIO
    bool killsBeforeRenameForTesting { false }; // raise(SIGKILL) with the new pack complete and the old one retired
    bool killsAfterRenameForTesting { false }; // raise(SIGKILL) right after the rename
#endif
};

// The writer. Each producing VMState owns one, over the shared OpenedArtifact, the ProducerLock and the budget it holds;
// the state destroys the writer before all three. A commit runs on the VM thread inside the capture: it appends a record
// at the pack's valid end, streaming, checksumming and counting the sources through the staging buffer, rereads and
// validates the body in Full mode, appends an index block naming the record, and stores the new valid end in the root,
// which is the commit point; it then has the opened artifact follow that root. A failure before the store leaves bytes
// past the valid end, which no reader sees and the next commit overwrites. Apart from the staging buffer, a failure's
// detail and an occasional full index, a commit allocates nothing. It reads no lane format.
class ArtifactWriter {
    WTF_MAKE_NONCOPYABLE(ArtifactWriter);
    WTF_MAKE_TZONE_ALLOCATED(ArtifactWriter);
public:
    ArtifactWriter(OpenedArtifact&, ProducerLock&, ProducerBudget&, size_t stagingBytes); // stagingBytes is at least 1
    ~ArtifactWriter(); // frees the staging buffer, releases its charge and closes the pack
    // Given a timing, the commit reads the thread CPU clock at the bounds of the parts it fills; given none, it reads no
    // clock.
    std::expected<CommitResult, CommitFailure> commit(const CommitStamp&, const CommitSections&, CommitTiming* = nullptr);
    // Appends a tombstone for a key the index lists and publishes it; a key without a body is left alone.
    std::optional<CommitFailure> remove(const BodyKey&);
    // The end of a delta: compacts the pack unless it already holds only its live records and one full index. Returns the
    // compaction, or nothing when the pack stays.
    std::expected<std::optional<CompactionResult>, CompactionFailure> compactIfNeeded(bool timed);
    void releaseStagingBuffer(); // the end of production

    // Writes the source's live bodies, except the excluded keys, into a new pack: a temporary holding the records in the
    // entries' order, a full index and then a fresh prefix whose root names the index's end; rereads its prefix and index,
    // marks the source retired and renames the temporary over cache/pack. The caller holds the producer lock. A failure
    // before the mark leaves the artifact as it was and no temporary.
    static std::expected<CompactionResult, CompactionFailure> compactPack(const CompactionTarget&, const CompactionSource&,
        const ScopedLambda<bool(const BodyKey&)>& excludes, bool timed);

    // The points of a commit or compaction where a test build can kill the process. Every build names them, so the
    // commit path marks each point once; only test builds act on the mark.
    enum class KillPoint : uint8_t { BeforeAppend, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterIndex, AfterRoot, CompactionBeforeRename, CompactionAfterRename };

#if ENABLE(JITCACHE_TWINS)
    // n counts this writer's commits from 1, or its compactions for a compaction.* check or point.
    struct FaultForTesting {
        ASCIILiteral check; // writer.open, writer.write, writer.reread, or a compaction.* step
        uint64_t n;
    };
    void setFaultForTesting(std::optional<FaultForTesting>);
    static std::optional<ASCIILiteral> faultCheckNamed(StringView);
    // raise(SIGKILL) at the point of the n-th commit, or of the n-th compaction for a compaction point.
    struct KillForTesting {
        KillPoint point;
        uint64_t n;
    };
    void setKillForTesting(std::optional<KillForTesting>);
    // before-append, mid-stream, after-stream, after-envelope, after-reread, after-index, after-root,
    // compaction-before-rename and compaction-after-rename.
    static ASCIILiteral name(KillPoint);
    static std::optional<KillPoint> killPointNamed(StringView);
    // Opens the key's current body in Full mode through OpenedArtifact::open, copies its sections into a buffer charged
    // to the budget, overwrites bytes.size() bytes of the kind's section at offset, and commits the copy under the opened
    // body's key, tier, L and P, which recomputes every checksum and draws a fresh commit identifier.
    std::expected<CommitResult, CommitFailure> rewriteSection(const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
#endif

private:
    // The staging buffer, charged before it is allocated and kept until production ends.
    bool ensureStagingBuffer();
    // A descriptor of the file the opened artifact maps, opened for writing, and its prefix mapped shared and writable;
    // clears a retired mark an abandoned compaction left on it. Returns 0 or the errno of the call that failed.
    int ensurePackOpen();
    void closePack();
    // Appends one index block after the bytes up to end and stores its end in the root. With a record entry, the block is
    // a full index once the chain since the last one reaches packDeltaChainBound and the budget accepts its buffer.
    std::optional<CommitFailure> publish(const PackIndexEntry&, uint64_t end);

    OpenedArtifact& m_artifact;
    ProducerLock& m_producerLock;
    ProducerBudget& m_budget;
    const size_t m_stagingBytes;
    MallocSpan<uint8_t> m_staging;
    int m_packFd { -1 };
    uint64_t m_packInode { 0 };
    std::span<uint8_t> m_writablePrefix; // the pack's prefix, MAP_SHARED and writable: the root is stored through it
#if ENABLE(JITCACHE_TWINS)
    uint64_t m_commits { 0 };
    uint64_t m_compactions { 0 };
    std::optional<FaultForTesting> m_faultForTesting;
    std::optional<KillForTesting> m_killForTesting;
    std::optional<ASCIILiteral> m_commitFault; // the armed fault of the commit in progress
#endif
};

} // namespace JSC::JITCache
