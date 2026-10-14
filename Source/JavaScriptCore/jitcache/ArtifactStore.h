#pragma once

#include "JITCacheContainer.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <bit>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/HashTraits.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

// The artifact in a process: its names, the producer lock, the registry of opened artifacts, the pack's mapping, and an
// opened artifact that follows the pack's root and serves its bodies.

// The artifact's names, relative to <parent> (Config::artifactPath):
//   <parent>/.cache.producer.lock
//   <parent>/cache/header
//   <parent>/cache/pack                    every body, in one log-structured file (JITCacheContainer.h)
//   <parent>/cache/.header.<32 hex>.tmp    a header being written
//   <parent>/cache/.pack.<32 hex>.tmp      a pack being written: a new artifact's, or a compaction's
//   <parent>/.cache.replaced               an incompatible cache/ a ConsumerProducer moved aside
namespace ArtifactNames {
inline constexpr ASCIILiteral lockFile = ".cache.producer.lock"_s;
inline constexpr ASCIILiteral cacheDirectory = "cache"_s;
inline constexpr ASCIILiteral header = "header"_s;
inline constexpr ASCIILiteral pack = "pack"_s;
inline constexpr ASCIILiteral replacedCacheDirectory = ".cache.replaced"_s;
} // namespace ArtifactNames

// A body key as reports and the shell spell it: the lowercase hex of its 40 canonical bytes.
constexpr size_t bodyKeyHexLength = 2 * BodyKey::byteSize;
std::array<char, bodyKeyHexLength> bodyKeyHexDigits(const BodyKey&);
// The key 80 lowercase hex digits spell, or nothing when they are not that or BodyKey::fromBytes rejects the bytes.
std::optional<BodyKey> bodyKeyFromHex(std::span<const char>);

// A temporary's name: ".header." or ".pack.", 32 lowercase hex digits of a fresh 128-bit value from
// cryptographicallyRandomValues, and ".tmp". Both are created with O_CREAT | O_EXCL in cache/, so a header or a pack
// takes its name only through a rename.
enum class TemporaryKind : uint8_t { Header, Pack };
constexpr size_t maximumTemporaryFileNameLength = 8 + 32 + 4; // ".header.", the hex digits, ".tmp"
struct TemporaryFileName {
    std::array<char, maximumTemporaryFileNameLength + 1> characters { }; // NUL-terminated
    const char* data() const LIFETIME_BOUND { return characters.data(); }
};
TemporaryFileName temporaryFileName(TemporaryKind);
std::optional<TemporaryKind> temporaryKindOfFileName(std::span<const char> name);

// Reads the directory directoryFd names: it opens "." relative to directoryFd, so it reads an open file description of its
// own from offset zero, reads it with readdir on fdopendir of that descriptor, and calls visit with each entry's name, "."
// and ".." aside, and its inode. Returns 0, or the errno of the call that failed, after which visit may have seen part of
// the directory. Any thread; it takes no lock.
int listDirectory(int directoryFd, const ScopedLambda<void(std::span<const char> name, uint64_t inode)>& visit);

// Removes <parent>/.cache.replaced under the producer lock: its temporaries, its pack and its header, then the directory.
// A name that is none of these stays, and with it the directory. Adds each removed file's size, taken with fstatat before
// its unlinkat, to bytesRemoved. Returns 0, also when there is no such directory, or the errno of the first call that
// failed.
int removeReplacedArtifact(int parentFd, uint64_t& bytesRemoved);

// Writes a new artifact's empty pack, under the producer lock: a pack temporary holding the prefix, an empty full index
// and the root of generation 1, renamed to cache/pack. Returns 0 or the errno of the call that failed, after which no
// temporary is left.
int createEmptyPack(int cacheFd, std::span<const uint8_t, 16> headerDigest);

// Whether a header-less cache/'s pack may hold a committed body: false only for a pack whose current root is the one a new
// artifact's pack starts with, an empty full index right after the prefix. A pack that cannot be read counts as holding
// bodies.
bool packHoldsBodies(int cacheFd);

// Why the artifact could not be read: the errno of the call that failed, or, with error 0, the reason its pack fails
// container.pack.
struct ArtifactReadFailure {
    int error { 0 };
    PackRejection packRejection;
};

// One shared, read-only mapping of a pack file whose prefix passed its checks. It maps more than the file holds, so the
// pack grows into it without a new mapping; nothing reads past a valid end a root of this file named, which the file
// always holds. The opened artifact that mapped it and every body or saved summary that borrows from it hold references,
// and the last one unmaps it. A pack replaced or deleted meanwhile stays readable through it.
class MappedPack final : public ThreadSafeRefCounted<MappedPack> {
    WTF_MAKE_NONCOPYABLE(MappedPack);
    WTF_MAKE_TZONE_ALLOCATED(MappedPack);
public:
    // cacheFd's pack, mapped with room for at least minimumCapacity bytes. ENOENT when cacheFd holds no pack.
    static std::expected<Ref<MappedPack>, ArtifactReadFailure> map(int cacheFd, std::span<const uint8_t, 16> headerDigest, uint64_t minimumCapacity);
    ~MappedPack();

    uint64_t inode() const { return m_inode; }
    uint64_t identifier() const { return m_identifier; }
    uint64_t capacity() const { return m_mapping.size(); }
    // The bytes [offset, offset + size), which the caller has bounded by a valid end this file's root named.
    std::span<const uint8_t> bytes(uint64_t offset, uint64_t size) const LIFETIME_BOUND;
    // The valid root with the highest generation, read with acquire order, so the bytes below its valid end are visible.
    std::optional<PackRoot> currentRoot() const;
    // The generations both slots hold now, read without checks: the cheap test of whether the root moved.
    uint64_t highestSlotGeneration() const;
    bool isRetired() const;

private:
    MappedPack(std::span<const uint8_t> mapping, uint64_t inode, uint64_t identifier);
    uint64_t loadWord(size_t offset) const;

    const std::span<const uint8_t> m_mapping; // MAP_SHARED, PROT_READ, larger than the file
    const uint64_t m_inode;
    const uint64_t m_identifier;
};

// The producer lock: a non-blocking flock on <parent>/.cache.producer.lock, a 64-byte file
//   0   8  the bytes JITCLOCK
//   8   4  layout version, 1
//   12  52 0
// which a producing VM, clean and compact hold through one type. The lock lives on the open file description, so
// destroying the object, or the process exiting, releases it; two descriptors of one process conflict, so a second
// producing VM in a process is busy too. The file is never deleted or replaced.
class ProducerLock final {
    WTF_MAKE_NONCOPYABLE(ProducerLock);
    WTF_MAKE_TZONE_ALLOCATED(ProducerLock);
public:
    struct Failure {
        bool busy; // another holder has it
        int error; // otherwise the errno of the call that failed
    };
    static std::expected<std::unique_ptr<ProducerLock>, Failure> tryAcquire(int parentFd);
    ~ProducerLock(); // closes, which releases the lock

private:
    explicit ProducerLock(int fd);

    const int m_fd; // the lock file, O_RDWR | O_CLOEXEC, holding LOCK_EX
};

// The hash and traits of every integrator map keyed by a body key: the index, the kept summaries and delta's candidate
// table. The hash reads the first four bytes of the key's identity digest, which SHA-256 makes uniform, mixed with its
// kind, specialization and mode bytes. An all-zero key, which BodyKey::fromBytes never accepts, is the empty value, and an
// all-0xFF key the deleted value.
struct BodyKeyHash {
    static unsigned hash(const BodyKey&);
    static bool equal(const BodyKey& a, const BodyKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};
struct BodyKeyHashTraits : WTF::GenericHashTraits<BodyKey> {
    static constexpr bool emptyValueIsZero = true;
    static BodyKey emptyValue();
    static void constructDeletedValue(BodyKey&);
    static bool isDeletedValue(const BodyKey&);
};
static_assert(std::is_trivially_copyable_v<BodyKey>);
static_assert(sizeof(BodyKey) == BodyKey::byteSize);

// A pack's live entries by key, as its full index and deltas leave them.
using PackIndex = HashMap<BodyKey, PackIndexEntry, BodyKeyHash, BodyKeyHashTraits>;

// Reads the index the root names from a pack's bytes: walks the blocks back from the root's latest one to the full index,
// or to stopAt when it meets it, and applies them in order to index. Every block and every entry must lie below the root's
// valid end, and every body entry's record extent too, and a record header must repeat its entry's key, size and commit
// identifier. Returns the number of blocks applied, or why the pack fails container.pack, after which index is
// unspecified. readAt returns the bytes [offset, offset + size) of the pack, which the walk bounds first.
// liveBytes, packLiveBytes of index, follows the blocks applied.
struct PackIndexRead {
    uint64_t blocks { 0 };
    uint64_t deltasSinceFull { 0 }; // the delta blocks applied; with reachedFull, all of those since the full index
    bool reachedFull { false }; // the walk reached a full index, so index holds only what the applied blocks list
};
std::expected<PackIndexRead, PackRejection> readPackIndex(const PackRoot&, std::optional<uint64_t> stopAt, PackIndex&, uint64_t& liveBytes,
    const ScopedLambda<std::span<const uint8_t>(uint64_t offset, uint64_t size)>& readAt);

// The bytes a pack's live bodies take: its prefix and the extents of the records its index lists. The rest of its valid
// bytes, superseded records, tombstoned ones and index blocks, are dead.
uint64_t packLiveBytes(const PackIndex&);

// The outcomes of the store's two reads of a body:
//   Absent       the index lists no body for the key
//   Unavailable  the fault hook of test builds failed the read with EMFILE, ENFILE or ENOMEM
//   Invalid      the pack failed its own checks (container.pack), or the body failed the first check of B1 to B8 that the
//                read's mode runs, or, in Full mode, its index entry does not repeat its envelope (container.pack)
//   Found        the read succeeded
enum class StoreOutcome : uint8_t { Absent, Unavailable, Invalid, Found };
struct StoreFailure { // Invalid: container.io with its errno, a check of B1 to B8, or container.pack
    ASCIILiteral check;
    int error { 0 };
    PackRejection packRejection { };
};

struct BodyOpen {
    StoreOutcome outcome;
    RefPtr<ValidatedBody> body; // Found only
    StoreFailure failure;
    // The size of the body the read reached: set for Found and for an Invalid at a body check, 0 otherwise. The open
    // bench event and Progress::bodyOpens count the opens that reached one.
    uint64_t bodyBytes { 0 };
};

// What one opened artifact's index has cost so far, which every flush of a bench report writes as an index line. The VMs
// of a process that open one artifact share its index, so these are the process's totals for it. Times are wall time
// from MonotonicTime, which the vDSO serves.
struct IndexStatistics {
    uint64_t bodies { 0 }; // the keys the index lists now
    uint64_t generation { 0 }; // the root the index follows now
    uint64_t packBytes { 0 }; // its valid end
    uint64_t liveBytes { 0 }; // packLiveBytes of the index now
    uint64_t tableBytes { 0 }; // the index table's storage now: its buckets times their size
    uint64_t buildNanoseconds { 0 }; // the first reading of the pack: its mapping and its whole index
    uint64_t refreshes { 0 }; // reads that found the root moved and applied it, reopens included
    uint64_t refreshNanoseconds { 0 };
    uint64_t appliedBlocks { 0 }; // index blocks those refreshes and the build applied
    uint64_t reopens { 0 }; // new mappings: of a retired pack's successor, or of a pack that outgrew its mapping
    uint64_t failedReopens { 0 }; // attempts that kept the mapping they had
    uint64_t opens { 0 }; // reads that reached a body, the scoring reads included
};

class OpenedArtifact;

class SavedSummaries { // one body's bytes, with its three summary sections checked; holds the pack's mapping
    WTF_MAKE_NONCOPYABLE(SavedSummaries);
    WTF_MAKE_TZONE_ALLOCATED(SavedSummaries);
public:
    uint64_t version() const { return m_version; } // the envelope's commit identifier
    uint8_t highestTier() const { return m_highestTier; }
    std::span<const uint8_t> ucbFeedback() const { return m_ucbFeedback; }
    std::span<const uint8_t> cbSummary() const { return m_cbSummary; }
    std::span<const uint8_t> ics() const { return m_ics; } // the whole ICsBaseline section
    ~SavedSummaries();

private:
    friend class OpenedArtifact; // readSavedSummaries builds it from bytes whose envelope, directory and summaries passed
    SavedSummaries(Ref<MappedPack>&&, uint64_t version, uint8_t highestTier, std::span<const uint8_t> ucbFeedback,
        std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics);

    const Ref<MappedPack> m_pack;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const std::span<const uint8_t> m_ucbFeedback;
    const std::span<const uint8_t> m_cbSummary;
    const std::span<const uint8_t> m_ics;
};
struct SavedSummaryRead {
    StoreOutcome outcome;
    std::unique_ptr<SavedSummaries> summaries; // Found only
    StoreFailure failure;
};

// An opened artifact, which the VMs of a process that open one artifact share. It maps the pack once and follows its root:
// every read first loads the two slots' generations from the mapping, and only when they moved does it read the new root
// and apply the index blocks appended since, or, when the mapped pack was retired or outgrew its mapping, map the path
// again and read the whole index. Everything outside m_indexLock is immutable once the object is built. m_indexLock is a
// leaf: nothing under it takes another lock, the only system calls made under it are a new mapping's, and it is never held
// while a body is validated or while any lane, JSC or the collector is called.
class OpenedArtifact final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<OpenedArtifact> {
    WTF_MAKE_NONCOPYABLE(OpenedArtifact);
    WTF_MAKE_TZONE_ALLOCATED(OpenedArtifact);
public:
    ~OpenedArtifact(); // drops the pack's mapping and closes the descriptors

    // Any thread that holds no JITCache lock.
    uint64_t token(const BodyKey&); // the commit identifier of the key's newest body, 0 when it has none; no system call
    BodyOpen open(const BodyKey&, ValidationMode); // borrows the body from the mapping and validates it
    SavedSummaryRead readSavedSummaries(const BodyKey&, ValidationMode); // the key's body through the current root
    bool containsKey(const BodyKey&);
    uint64_t indexedBodies(); // without following the root: status and the start bench event
    IndexStatistics statistics(); // without following the root

    std::span<const uint8_t, 16> headerDigest() const LIFETIME_BOUND;
    int cacheFd() const;

private:
    friend class ArtifactRegistry; // builds and registers objects
    friend class ArtifactWriter; // reads the current root and pack under m_indexLock and follows its own commits
    static std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);
    OpenedArtifact(int parentFd, int cacheFd, const std::array<uint8_t, 16>& headerDigest);

    // Follows the root: returns the failure that makes every read Invalid, which stays once set.
    std::optional<StoreFailure> follow() WTF_REQUIRES_LOCK(m_indexLock);
    // Maps cache/pack afresh and reads its whole index; on failure keeps what the object had.
    std::optional<ArtifactReadFailure> reopen(uint64_t minimumCapacity) WTF_REQUIRES_LOCK(m_indexLock);
    std::optional<StoreFailure> applyRoot(const PackRoot&) WTF_REQUIRES_LOCK(m_indexLock);

    const int m_parentFd; // O_PATH | O_DIRECTORY
    const int m_cacheFd;
    const std::array<uint8_t, 16> m_headerDigest;
    Lock m_indexLock;
    PackIndex m_index WTF_GUARDED_BY_LOCK(m_indexLock);
    RefPtr<MappedPack> m_pack WTF_GUARDED_BY_LOCK(m_indexLock);
    PackRoot m_root WTF_GUARDED_BY_LOCK(m_indexLock);
    uint64_t m_liveBytes WTF_GUARDED_BY_LOCK(m_indexLock) { packPrefixBytes };
    uint64_t m_deltasSinceFull WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };
    std::optional<StoreFailure> m_damage WTF_GUARDED_BY_LOCK(m_indexLock);
    bool m_gone WTF_GUARDED_BY_LOCK(m_indexLock) { false }; // the retired pack's path holds no pack: every key misses
    MonotonicTime m_nextRetiredReopen WTF_GUARDED_BY_LOCK(m_indexLock);
    MonotonicTime m_retiredSince WTF_GUARDED_BY_LOCK(m_indexLock); // when a read first met the mapped pack's retired mark with the path unchanged

    IndexStatistics m_statistics WTF_GUARDED_BY_LOCK(m_indexLock);
};

// The process-wide registry of opened artifacts, keyed by the cache/ directory's (st_dev, st_ino). take holds m_lock only
// to look the key up and to register a new object, and builds an object outside it, so the lock is a leaf; when two VMs
// build an object for one directory at once, the second to register takes the first's and destroys its own.
class ArtifactRegistry {
    WTF_MAKE_NONCOPYABLE(ArtifactRegistry);
public:
    static std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);

private:
    friend class LazyNeverDestroyed<ArtifactRegistry>;
    ArtifactRegistry() = default;
    static ArtifactRegistry& singleton();

    using DirectoryIdentity = std::pair<uint64_t, uint64_t>;
    Lock m_lock;
    HashMap<DirectoryIdentity, ThreadSafeWeakPtr<OpenedArtifact>> m_artifacts WTF_GUARDED_BY_LOCK(m_lock);
};

#if ENABLE(JITCACHE_TWINS)
// The store's test interface. Every hook is process-wide and made of atomics, so it is read without a lock.
namespace StoreTesting {
// Open is open's read of a body, ReadSavedSummaries the scoring read's, PackMap the openat of cache/pack that maps a pack.
// A failed read answers as a failed system call would have: EMFILE, ENFILE and ENOMEM Unavailable, any other errno Invalid.
enum class Call : uint8_t { Open, ReadSavedSummaries, PackMap };
struct Fault {
    Call call;
    int error;
    std::optional<uint64_t> n; // counts calls of that kind from 1, from the setFault call; absent: every call
};
void setFault(std::optional<Fault>);
void setRegistrySharing(bool); // false: ArtifactRegistry::take builds a new OpenedArtifact at every call
} // namespace StoreTesting
#endif

} // namespace JSC::JITCache
