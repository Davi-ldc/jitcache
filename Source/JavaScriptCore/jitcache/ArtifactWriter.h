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
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

class OpenedArtifact;
class ProducerBudget;
class ProducerLock;

// The checks a commit fails at (container sub-SPEC section 8.2): the budget's own fault for a refused charge, and the
// writer's steps. The capture glue raises each as a recording fault with the failure's detail.
namespace WriterChecks {
inline constexpr ASCIILiteral budgetLimit = "budget.limit"_s; // step 1's staging charge, or rewriteSection's copy
inline constexpr ASCIILiteral create = "writer.create"_s; // step 3
inline constexpr ASCIILiteral write = "writer.write"_s; // steps 4 and 5
// Step 2, when the sources' sizes give no layout, and step 4, when a source broke its size or failed on its own.
inline constexpr ASCIILiteral section = "writer.section"_s;
inline constexpr ASCIILiteral reread = "writer.reread"_s; // step 6
inline constexpr ASCIILiteral publish = "writer.publish"_s; // step 7
#if ENABLE(JITCACHE_TWINS)
// rewriteSection had nothing it could rewrite (section 8.3): open did not answer Found, the kind names no section the
// body's highest tier requires, or the range does not lie inside that section. Only rewriteSection returns it, and
// nothing is written when it does.
inline constexpr ASCIILiteral rewrite = "writer.rewrite"_s;
#endif
} // namespace WriterChecks

// The capture glue hands an accepted capture to the writer as an ordered list of section sources (SPEC-integrator.md
// section 6.3). Each source is a section kind, a size, and either the section's bytes in memory or a function through
// which an object streams them, so the writer sees no lane type and reads no lane format.

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
    explicit CommitSections(std::span<const SectionSource>); // at most numberOfSectionKinds, kinds strictly increasing; copied without allocating
    std::span<const SectionSource> sources() const LIFETIME_BOUND;

private:
    std::array<SectionSource, numberOfSectionKinds> m_sources;
    unsigned m_count { 0 };
};

struct CommitStamp {
    BodyKey key;
    uint32_t llintThreshold; // L: UCB envelopeLLIntThreshold (THREAD Maintenance)
    uint32_t counterProgress; // P: the CB lane's counterProgress (SPEC-cb.md section 4.3), 0 when the counter does not travel
    uint8_t highestTier; // 1
};
struct CommitResult {
    uint64_t version;
    uint64_t fileSize;
};
struct CommitFailure {
    ASCIILiteral check; // budget.limit, a writer.* step of container sub-SPEC section 8.2, or writer.rewrite (its section 8.3)
    String detail;
};

// The writer (container sub-SPEC section 8). Each producing VMState owns one, over the shared OpenedArtifact, the
// ProducerLock and the budget it holds; the state destroys the writer before all three. A commit runs on the VM thread
// inside the capture: it lays the sources out, streams, checksums and counts their bytes through the staging buffer into
// a temporary in cache/, rereads and validates the file in Full mode, publishes it with renameat, bumps the epoch and
// updates the index. Any failure after the temporary exists unlinks it. Apart from the staging buffer and a failure's
// detail, a commit allocates only the index entries step 8 adds: its key's, which the capture glue charged, and those of
// the queued events it applies, which are not production memory (SPEC-integrator.md section 4.4). It reads no lane format.
class ArtifactWriter {
    WTF_MAKE_NONCOPYABLE(ArtifactWriter);
    WTF_MAKE_TZONE_ALLOCATED(ArtifactWriter);
public:
    ArtifactWriter(OpenedArtifact&, ProducerLock&, ProducerBudget&, size_t stagingBytes); // stagingBytes is at least 1
    ~ArtifactWriter(); // frees the staging buffer and releases its charge
    std::expected<CommitResult, CommitFailure> commit(const CommitStamp&, const CommitSections&);
    void releaseStagingBuffer(); // the end of production

#if ENABLE(JITCACHE_TWINS)
    // Container sub-SPEC section 8.3. n counts this writer's commits from 1.
    struct FaultForTesting {
        ASCIILiteral check; // writer.create, writer.write, writer.reread or writer.publish
        uint64_t n;
    };
    void setFaultForTesting(std::optional<FaultForTesting>);
    // The fault injection's check by its name, for the shell's --jitcache-test-writer-fault; nothing for any other name.
    static std::optional<ASCIILiteral> faultCheckNamed(StringView);
    // Harness sub-SPEC section 12: raise(SIGKILL) at the point of the n-th commit.
    enum class KillPoint : uint8_t { BeforeCreate, AfterCreate, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterRename };
    struct KillForTesting {
        KillPoint point;
        uint64_t n;
    };
    void setKillForTesting(std::optional<KillForTesting>);
    // The names harness sub-SPEC section 12 gives the points, which the shell's --jitcache-test-kill takes: before-create,
    // after-create, mid-stream, after-stream, after-envelope, after-reread and after-rename.
    static ASCIILiteral name(KillPoint);
    static std::optional<KillPoint> killPointNamed(StringView);
    // Opens the key's current body in Full mode through OpenedArtifact::open (container sub-SPEC section 7.2), copies its
    // sections into a buffer charged to the budget, overwrites bytes.size() bytes of the kind's section at offset, and
    // commits the copy's sections under the opened body's key, highestTier(), llintThreshold() and counterProgress(),
    // which recomputes every checksum and draws a fresh commit identifier. An open that answers Absent, Unavailable or
    // Invalid, a section the body's tier does not require and a range outside the section fail at writer.rewrite, and a
    // refused charge for the copy at budget.limit, with nothing written; the commit itself fails as commit does.
    std::expected<CommitResult, CommitFailure> rewriteSection(const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
#endif

private:
    // Step 8 of container sub-SPEC section 8.2, under the opened artifact's m_indexLock: applies the queued events, bumps
    // the epoch, records the bump as seen when the object was current, and learns the published file's inode.
    void didPublish(const BodyKey&, uint64_t inode);

    OpenedArtifact& m_artifact;
    ProducerLock& m_producerLock;
    ProducerBudget& m_budget;
    const size_t m_stagingBytes; // at least one byte
    MallocSpan<uint8_t> m_staging; // allocated and charged at the first commit; empty before it and once released
#if ENABLE(JITCACHE_TWINS)
    uint64_t m_commits { 0 }; // commits started, which the fault injection's and the kill point's n count
    std::optional<FaultForTesting> m_faultForTesting;
    std::optional<KillForTesting> m_killForTesting;
#endif
};

} // namespace JSC::JITCache
