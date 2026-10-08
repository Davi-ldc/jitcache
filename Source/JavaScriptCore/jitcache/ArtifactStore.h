/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JITCacheContainer.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <bit>
#include <expected>
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
#include <wtf/Seconds.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC::JITCache {

// The artifact in a process (container sub-SPEC sections 2 and 5 to 7): the producer lock, the registry of opened
// artifacts, an opened artifact with its index, and the store's reads.

// The producer lock (container sub-SPEC section 2): a non-blocking flock on <parent>/.cache.producer.lock, a 64-byte file
//   0   8  the bytes JITCLOCK
//   8   4  layout version, 1
//   12  4  0
//   16  8  commit epoch
//   24  40 0
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
    uint64_t bumpEpoch(); // returns the value the bump replaced
    ~ProducerLock(); // unmaps and closes, which releases the lock

private:
    ProducerLock(int fd, uint64_t* epoch);

    const int m_fd; // the lock file, O_RDWR | O_CLOEXEC, holding LOCK_EX
    uint64_t* const m_epoch; // the commit epoch, mapped PROT_READ | PROT_WRITE and MAP_SHARED; bumped with release order
};

// The index's hash and traits (container sub-SPEC section 6.1), which every integrator map keyed by a body key uses: the
// index, the kept summaries and delta's candidate table. The hash reads the first four bytes of the key's identity
// digest, which SHA-256 makes uniform, mixed with its kind, specialization and mode bytes. An all-zero key, which
// BodyKey::fromBytes never accepts, is the empty value, and an all-0xFF key the deleted value.
struct BodyKeyHash {
    static unsigned hash(const BodyKey&);
    static bool equal(const BodyKey& a, const BodyKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};
struct BodyKeyHashTraits : WTF::GenericHashTraits<BodyKey> {
    static constexpr bool emptyValueIsZero = true;
    static BodyKey emptyValue(); // std::bit_cast of BodyKey::byteSize zero bytes
    static void constructDeletedValue(BodyKey&); // std::bit_cast of BodyKey::byteSize 0xFF bytes
    static bool isDeletedValue(const BodyKey&);
};
// Both values are byte fills, which needs a BodyKey that is trivially copyable and exactly its canonical bytes.
static_assert(std::is_trivially_copyable_v<BodyKey>);
static_assert(sizeof(BodyKey) == BodyKey::byteSize);

struct IndexEntry {
    uint64_t token; // nonzero; a fresh one whenever the index learns of another body at the key
    uint64_t inode; // the body file's inode as a listing or the writer saw it; 0 when an event made the entry
};

// The outcomes of the store's two reads that open a body by its name (container sub-SPEC section 7):
//   Absent       open: a key the index lacks, without a system call, or ENOENT, which erases the entry unless its
//                token changed meanwhile; readSavedSummaries: ENOENT, which erases an entry for the key the same way
//   Unavailable  EMFILE, ENFILE or ENOMEM from openat, fstat or mmap; the entry stays
//   Invalid      any other error (container.io), or the first failed check of section 4.5 that the read's mode runs
//   Found        the read succeeded; the index is left as it is
enum class StoreOutcome : uint8_t { Absent, Unavailable, Invalid, Found };
struct StoreFailure { // Invalid: container.io with its errno, or a check of section 4.5
    ASCIILiteral check;
    int error { 0 };
};

struct BodyOpen {
    StoreOutcome outcome;
    RefPtr<ValidatedBody> body; // Found only
    StoreFailure failure;
};

class OpenedArtifact;

class SavedSummaries { // one body's mapping, with its three summary sections checked; unmaps when destroyed
    WTF_MAKE_NONCOPYABLE(SavedSummaries);
    WTF_MAKE_TZONE_ALLOCATED(SavedSummaries);
public:
    uint64_t version() const; // the envelope's commit identifier
    uint8_t highestTier() const;
    std::span<const uint8_t> ucbFeedback() const;
    std::span<const uint8_t> cbSummary() const;
    std::span<const uint8_t> ics() const; // the whole ICsBaseline section
    ~SavedSummaries(); // unmaps the body

private:
    friend class OpenedArtifact; // readSavedSummaries builds it from a mapping whose envelope, directory and summaries passed
    SavedSummaries(std::span<const uint8_t> mapping, uint64_t version, uint8_t highestTier, std::span<const uint8_t> ucbFeedback,
        std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics);

    const std::span<const uint8_t> m_mapping; // the private mapping the destructor unmaps
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

// An opened artifact (container sub-SPEC section 5.2), which the VMs of a process that open one artifact share (II21).
// Everything outside m_indexLock is immutable once the object is built. m_indexLock is a leaf: nothing under it takes
// another lock, the only system calls made under it are the reads of the inotify descriptor and the listings' calls,
// and it is never held while a body is mapped or validated, while an envelope is read, or while any lane, JSC or the
// collector is called. The object's reads return results, raise nothing and know no VM.
class OpenedArtifact final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<OpenedArtifact> {
    WTF_MAKE_NONCOPYABLE(OpenedArtifact);
    WTF_MAKE_TZONE_ALLOCATED(OpenedArtifact);
public:
    ~OpenedArtifact(); // unmaps the epoch page and closes every descriptor

    // Section 7; any thread that holds no JITCache lock.
    uint64_t token(const BodyKey&); // refreshes the index and returns the key's token, 0 when it lists no body; opens no file
    BodyOpen open(const BodyKey&, ValidationMode); // maps the body name with MAP_POPULATE and validates it (section 7.2)
    SavedSummaryRead readSavedSummaries(const BodyKey&, ValidationMode); // opens the body name whatever the index holds (section 7.3)
    bool containsKey(const BodyKey&); // section 6.4
    // The number of keys the index lists, read under m_indexLock without a refresh: Progress::indexedBodies in status,
    // which opens no file and changes no state, and the start bench event (SPEC-integrator.md section 3.3; harness
    // sub-SPEC section 9.2).
    uint64_t indexedBodies();

    std::span<const uint8_t, 16> headerDigest() const LIFETIME_BOUND;
    int cacheFd() const; // the writer's temporaries (section 8.2)
    int bodiesFd() const; // the writer's renames (section 8.2)

private:
    friend class ArtifactRegistry; // builds and registers objects (section 5.1)
    friend class ArtifactWriter; // runs step 8 of section 8.2 under m_indexLock
    // Opens the descriptors, the epoch mapping and the inotify watch, then lists; an error is the failing call's errno.
    static std::expected<Ref<OpenedArtifact>, int> create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);
    OpenedArtifact(int parentFd, int cacheFd, int bodiesFd, const std::array<uint8_t, 16>& headerDigest, const uint64_t* epoch, int inotifyFd);

    void refreshIfStale() WTF_REQUIRES_LOCK(m_indexLock); // section 6.3
    bool list() WTF_REQUIRES_LOCK(m_indexLock); // section 6.2; false leaves the index as it was
    void drainEvents(bool listOnOverflow) WTF_REQUIRES_LOCK(m_indexLock); // section 6.3; the writer's step 8 passes false
    void learn(const BodyKey&, uint64_t inode) WTF_REQUIRES_LOCK(m_indexLock); // section 6.4
    void erase(const BodyKey&) WTF_REQUIRES_LOCK(m_indexLock);

    const int m_parentFd; // O_PATH | O_DIRECTORY
    const int m_cacheFd;
    const int m_bodiesFd;
    const std::array<uint8_t, 16> m_headerDigest;
    const uint64_t* const m_epoch; // the lock file's epoch, mapped PROT_READ; null when the object has none (section 2)
    Lock m_indexLock;
    int m_inotifyFd WTF_GUARDED_BY_LOCK(m_indexLock) { -1 }; // -1 without inotify, and once the directory is gone
    HashMap<BodyKey, IndexEntry, BodyKeyHash, BodyKeyHashTraits> m_index WTF_GUARDED_BY_LOCK(m_indexLock);
    uint64_t m_nextToken WTF_GUARDED_BY_LOCK(m_indexLock) { 1 };
    uint64_t m_lastEpochSeen WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };
    // When the last listing started, so an object that refreshes by listing lists at most once per
    // fallbackListingIntervalMilliseconds (section 6.3).
    MonotonicTime m_lastListingStart WTF_GUARDED_BY_LOCK(m_indexLock);
    bool m_listingPending WTF_GUARDED_BY_LOCK(m_indexLock) { false };
    bool m_gone WTF_GUARDED_BY_LOCK(m_indexLock) { false };
};

// The process-wide registry of opened artifacts (container sub-SPEC section 5.1), keyed by the cache/ directory's
// (st_dev, st_ino). take holds m_lock only to look the key up and to register a new object, and builds an object outside
// it, so the lock is a leaf; when two VMs build an object for one directory at once, the second to register takes the
// first's and destroys its own. A dead weak entry is replaced the same way.
class ArtifactRegistry {
    WTF_MAKE_NONCOPYABLE(ArtifactRegistry);
public:
    // The object for the directory cacheFd names; builds and registers one when no VM of the process holds it.
    // An error is the errno of the failing call, which start reports at start.io.
    static std::expected<Ref<OpenedArtifact>, int> take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);

private:
    friend class LazyNeverDestroyed<ArtifactRegistry>;
    ArtifactRegistry() = default;
    static ArtifactRegistry& singleton();

    using DirectoryIdentity = std::pair<uint64_t, uint64_t>; // the cache/ directory's (st_dev, st_ino)
    Lock m_lock;
    HashMap<DirectoryIdentity, ThreadSafeWeakPtr<OpenedArtifact>> m_artifacts WTF_GUARDED_BY_LOCK(m_lock);
};

#if ENABLE(JITCACHE_TWINS)
// The store's test interface (container sub-SPEC section 7.5).
namespace StoreTesting {
enum class Call : uint8_t { Open, ReadSavedSummaries, Listing };
struct Fault {
    Call call;
    int error;
    std::optional<uint64_t> n; // counts calls of that kind from 1; absent: every call
};
void setFault(std::optional<Fault>); // process-wide, any thread
void setRegistrySharing(bool); // false: ArtifactRegistry::take builds a new OpenedArtifact at every call (test C4)
void setInotify(bool); // false: an object built afterward creates no inotify descriptor and lists instead (test C4)
void setFallbackListingInterval(std::optional<Seconds>); // replaces fallbackListingIntervalMilliseconds for every object; absent: the parameter (test C4)
} // namespace StoreTesting
#endif

} // namespace JSC::JITCache
