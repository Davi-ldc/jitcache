# SPEC-integrator.container: the artifact on disk

Part of [SPEC-integrator.md](SPEC-integrator.md), which indexes it and whose facts, interfaces and invariants apply here; its section 1 lists what this file covers. All integers are little-endian, as every supported target is (`JITCacheContainer.h` asserts `std::endian::native == std::endian::little`), and fields are read with `memcpy`-based loads.

## 1. Layout

### 1.1 Paths and names

THREAD Storage fixes the shape; this section fixes the names:

```text
<parent>/
  .cache.producer.lock
  cache/
    header
    .header.<32 hex>.tmp       a header being written
    .<32 hex>.tmp              a body being written
    bodies/
      <80 hex>.bin             a body
```

- `<parent>` is `Config::artifactPath`.
- A body's name is the lowercase hex of its 40 canonical key bytes (SPEC-ucb.md section 4.1), followed by `.bin`. This hex is THREAD Storage's body-key hash, and a lookup computes no hash for it (section 6.1). A name is a body name exactly when it has 80 lowercase hex digits before `.bin` and `BodyKey::fromBytes` accepts the decoded bytes.
- A temporary's name is a dot, 32 lowercase hex digits of a fresh 128-bit value from `cryptographicallyRandomValues`, and `.tmp`; a header temporary's is `.header.`, 32 hex digits and `.tmp`. Both are created with `O_CREAT | O_EXCL`, in `cache/`, so that a body enters `bodies/` only through the writer's rename (II5) and each commit raises one event in the directory the index watches (section 6.3).
- Directories are created with mode 0755 and files with mode 0644, before the umask. Every descriptor is opened with `O_CLOEXEC`, so no child process inherits the producer lock or a body.

### 1.2 What an artifact is

An artifact exists at `<parent>` exactly when `<parent>/cache/header` exists. A `cache/` directory without a header is a remnant, left by an interrupted creation (section 1.3) or by the last steps of a whole-artifact deletion (maintenance sub-SPEC section 4.5); neither leaves a committed body behind unless someone else put it there.

### 1.3 Creating an artifact

A Producer creates the artifact while it holds the producer lock (SPEC-integrator.md section 4.2, step 7):

1. When `cache/` is absent: `mkdirat(parent, "cache")` and `mkdirat(cache, "bodies")`. When `cache/` exists without a header: if a listing of `bodies/` (section 6.2) finds any body name, `start` rejects at `start.not-an-artifact` and changes nothing; otherwise every temporary in `cache/` is removed, `bodies/` is created when missing, and the directory is reused.
2. The header's bytes (section 3) are written to a header temporary, which `renameat` moves to `header`.
3. The VM takes the artifact's `OpenedArtifact` from the registry, as every role does (section 5.1).

A failure in any step is a `Fault` at `start.io`; whatever the step created stays, and the next Producer or `clean` finishes or removes it.

## 2. The lock file

`<parent>/.cache.producer.lock`, 64 bytes:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCLOCK` |
| 8 | 4 | layout version, 1 |
| 12 | 4 | 0 |
| 16 | 8 | commit epoch |
| 24 | 40 | 0 |

A producing VM, `clean` and `compact` hold the lock through one type, which `ArtifactStore.h` declares:

```cpp
class ProducerLock final {
    WTF_MAKE_NONCOPYABLE(ProducerLock);
    WTF_MAKE_TZONE_ALLOCATED(ProducerLock);
public:
    struct Failure { bool busy; int error; };   // busy: another holder has it; otherwise the errno of the call that failed
    static Expected<std::unique_ptr<ProducerLock>, Failure> tryAcquire(int parentFd);
    uint64_t bumpEpoch();                       // returns the value the bump replaced
    ~ProducerLock();                            // unmaps and closes, which releases the lock
};
```

`tryAcquire`:

1. `openat(parentFd, ".cache.producer.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644)`.
2. `flock(fd, LOCK_EX | LOCK_NB)`. `EWOULDBLOCK` means another producer or maintenance holds it: close the descriptor and return `busy`. Any other error closes it and returns the error, which `start` turns into a `start.io` fault and maintenance into a failure.
3. With the lock held, a file shorter than 64 bytes, or with another tag or version, is set up in place: `ftruncate(fd, 64)`, then `pwrite` of the tag, the version and a zero epoch. The file is never deleted or replaced (THREAD Storage), so every process maps the same inode.
4. `mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`.

`bumpEpoch` is `__atomic_fetch_add(&epoch, 1, __ATOMIC_RELEASE)` through the mapping. The writer calls it after every publish (section 8.2, step 8), and maintenance after every batch of removals.

The lock lives on the open file description: destroying the `ProducerLock`, when the `VMState` that owns it is destroyed or maintenance ends, or the process exiting, releases it. Two descriptors of one process conflict, so a second producing VM in a process is busy too.

Every `OpenedArtifact` follows the epoch, whatever the roles of the VMs that hold it (section 6.3). At its build it opens the file `O_RDONLY | O_CLOEXEC`, without creating it, and maps its 64 bytes `PROT_READ`, `MAP_SHARED`; it reads the epoch with `__atomic_load_n(&epoch, __ATOMIC_ACQUIRE)`. A missing file, one shorter than 64 bytes, or one with another tag or version gives the object no epoch, and its index then never refreshes. Consumers take no lock (THREAD Storage).

## 3. The header

### 3.1 Layout

`cache/header`, with B build IDs and M must-match options:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCHEAD` |
| 8 | 2 | layout version, 1 |
| 10 | 1 | architecture: 1 x86_64, 2 ARM64 |
| 11 | 1 | B, 1 or 2 |
| 12 | 4 | H, the header's size: 32 + 72·B + 8·M + 8 |
| 16 | 8 | the CPU feature vector (SPEC-integrator.md section 6.4) |
| 24 | 4 | M, 3 in this version |
| 28 | 4 | 0 |
| 32 | 72·B | build-ID records: `u8` role (1 main executable, 2 engine object), `u8` length n (1 to 64), `u16` 0, `u32` 0, then 64 bytes holding the ID in the first n and zero after; the main executable's first, the engine object's second when it is a separate object |
| 32 + 72·B | 8·M | option records: `u16` index (0 `evalMode`, 1 `useExplicitResourceManagement`, 2 `useImportDefer`), `u8` type (1 `Bool`), `u8` value (0 or 1), `u32` 0; in index order |
| H - 8 | 4 | CRC32C of bytes [0, H - 8) |
| H - 4 | 4 | 0 |

The process computes its own header bytes once, from the facts of SPEC-integrator.md section 6.4 (`expectedHeader()` in `JITCacheContainer.cpp`).

### 3.2 Reading a header

A Consumer or ConsumerProducer reads the whole file, which must be at most 4 KiB:

- A file shorter than 40 bytes, larger than 4 KiB, or without the tag `JITCHEAD` is corrupt.
- A layout version other than 1 is incompatible: another format, so the rest is not read.
- Otherwise it is corrupt when H differs from the file's size or from the formula, B is not 1 or 2, M is not 3, a record has an out-of-range role, length, index, type or value, a padding or reserved byte is not zero, or the CRC differs.
- A header that is not corrupt is compatible exactly when its bytes [0, H - 8) equal the process's.

`start` turns corrupt into a `Fault` at `start.header` and incompatible into `Rejected` at `start.incompatible`, whose detail names the first field that differs: the architecture, a build ID, a CPU feature bit by its predicate's name, or an option by its name. Maintenance reads the header only to check it and digest it (maintenance sub-SPEC section 2).

### 3.3 The header digest

The header digest is the first 16 bytes of the SHA-256 (SPEC-ucb.md section 4.6) of the header's H bytes. Every body stamps the digest of the header it was written under (section 4.1), and every read of a body requires it to equal the opened artifact's (check B4). Two artifacts made by the same binary, options and CPU features have equal headers, so their bodies move between them freely (packaging is external, THREAD Storage), while a body copied from an incompatible artifact is rejected.

## 4. Body files

A body file holds the three parts THREAD Storage names.

### 4.1 Envelope

128 bytes at offset 0:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCBODY` |
| 8 | 2 | layout version, 1 |
| 10 | 2 | envelope size, 128 |
| 12 | 2 | N, the number of sections, 1 to 64 |
| 14 | 1 | the highest tier the body holds, 1 in this version |
| 15 | 1 | 0 |
| 16 | 40 | the body key |
| 56 | 8 | the commit identifier: nonzero, drawn with `cryptographicallyRandomValues` for every write of the file |
| 64 | 16 | the header digest (section 3.3) |
| 80 | 8 | the file's size |
| 88 | 4 | L: the LLInt threshold the import skips, `envelopeLLIntThreshold` (THREAD Maintenance) |
| 92 | 4 | P: the baseline counter's progress, the CB lane's `counterProgress` (THREAD Maintenance) |
| 96 | 4 | CRC32C of the directory |
| 100 | 20 | 0 |
| 120 | 4 | CRC32C of bytes [0, 120) |
| 124 | 4 | 0 |

The commit identifier is the body version THREAD Maintenance names, which compact compares and `ValidatedBody::version()` returns (SPEC-integrator.md section 7.2); the index holds tokens instead (section 6.1).

### 4.2 Section directory

N entries of 24 bytes at offset 128:

| offset | size | field |
|---|---|---|
| 0 | 2 | type id (SPEC-integrator.md section 7.1) |
| 2 | 1 | tier |
| 3 | 1 | 0 |
| 4 | 4 | CRC32C of the section's bytes |
| 8 | 8 | the section's offset from the file's start |
| 16 | 8 | the section's size |

### 4.3 Sections

The entries are in strictly increasing order of (type id, tier), which is the order of `SectionKind`. The first section starts at the first multiple of 8 at or after 128 + 24·N, and each next one at the first multiple of 8 at or after the end of the one before. The bytes between sections are zero, and the file ends where the last section ends. A section may be empty; the lanes' own validation decides whether theirs may.

### 4.4 CRC32C

The checksum is the CRC-32C of RFC 3720: the Castagnoli polynomial `0x1EDC6F41`, reflected (`0x82F63B78`), initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`; `crc32c("123456789")` is `0xE3069283`. `JITCachePlatform.h` declares `uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>)`, with `crc32c(bytes) = ~crc32cExtend(~0u, bytes)`, so the writer can checksum a stream. `crc32cExtend` calls the engine's own `crc32c` of `runtime/CachedTypes.cpp`, the function the bytecode cache checksums its payloads with, which takes the same running state: the SSE4.2 `crc32` instructions on x86_64 when CPUID reports SSE4.2, the CRC32 instructions on ARM64 when the build defines `__ARM_FEATURE_CRC32`, as Bun's `-march=armv8-a+crc` does, and a byte table otherwise. That function loses `static` (SPEC-integrator.md section 3.2) and `JITCachePlatform.h` declares it in `namespace JSC`, so one implementation serves both formats.

### 4.5 Validation

`validateBody(bytes, expectedKey, headerDigest, ValidationMode) -> Expected<BodyLayout, ContainerCheck>` runs these checks in this order and names the first that fails. `JITCacheContainer.h` declares `enum class ValidationMode : uint8_t { Integrity, Full }`. Each check has an integrity part, which both modes run, and some have a structure part, which only `Full` adds. Integrity is what THREAD Session has normal mode check: the header digest (B4) ties the body to the header `start` checked, B3 checks the key and B2, B6 and B8 the checksums, with the sizes and bounds that keep those checksums inside the file (B1 and the framing parts of B2 and B6).

| check | name | integrity, both modes | structure, `Full` only |
|---|---|---|---|
| B1 | `container.size` | the file is at least 128 bytes and its size equals the envelope's file size | |
| B2 | `container.envelope` | the tag, layout version 1, envelope size 128, N from 1 to 64, and the envelope's CRC | the zero bytes and a highest tier of 1 |
| B3 | `container.key` | the envelope's key equals `expectedKey`, the key the file's name encodes | |
| B4 | `container.header-digest` | the envelope's header digest equals the opened artifact's | |
| B5 | `container.version` | | the commit identifier is not zero |
| B6 | `container.directory` | the directory's CRC; every entry's offset and size lie inside the file without overflow | every entry's zero byte; every (type id, tier) is a section kind of SPEC-integrator.md section 7.1; the entries strictly increase; every offset and size follows section 4.3, and the padding is zero |
| B7 | `container.required` | | every section required for the highest tier is present, and `image-twins.baseline` is present exactly in `ENABLE(JITCACHE_TWINS)` builds |
| B8 | `container.checksum` | each section's CRC | |

`JITCacheContainer.h` declares the checks' entry points. The writer rereads its file through the staging buffer, in chunks of any size down to one byte (section 8.2, step 6), so the checks also run over a stream; `validateBody` feeds its whole span to one stream, and both forms run the same checks in the same order:

```cpp
using ContainerCheck = ASCIILiteral;    // the failing check's name in the table above, such as "container.key"

struct SectionExtent { uint64_t offset; uint64_t size; };
struct BodyLayout {
    uint64_t version;                   // the commit identifier
    uint8_t highestTier;
    uint32_t llintThreshold;            // L
    uint32_t counterProgress;           // P
    std::array<std::optional<SectionExtent>, numberOfSectionKinds> sections;   // by SectionKind; nullopt when absent
};

Expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode);

class BodyValidationStream {   // keeps the envelope and directory (at most 128 + 64 * 24 bytes) and a CRC per section; allocates nothing
public:
    BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode, uint64_t fileSize);
    void append(std::span<const uint8_t>);           // the file's bytes in order
    Expected<BodyLayout, ContainerCheck> finish();   // after the last byte; names the first check that failed
};
```

A VM's `open` and scoring read use `Full` exactly when strict is on (section 7), and the scoring read runs B8 on three sections only (section 7.3). The writer's reread always uses `Full` (section 8.2), and maintenance runs B1 to B5 in full on each envelope (maintenance sub-SPEC section 2).

With `Integrity`, the layout records the first directory entry of each section kind and ignores an entry whose (type id, tier) names no kind; the writer produces neither case, and normal mode trusts it not to (section 7.4). A failure in an opened body is invalid material at the check's name (SPEC-integrator.md section 7.2). `BodyLayout` records each present section's offset and size, so `ValidatedBody::section` returns a span without searching.

## 5. Artifacts in a process

### 5.1 The registry

`ArtifactRegistry` is a process-wide object (`LazyNeverDestroyed`) with a `Lock m_lock` and a map from the `cache/` directory's `(st_dev, st_ino)` to `ThreadSafeWeakPtr<OpenedArtifact>`. `start` goes through it once the role's own steps have passed (SPEC-integrator.md section 4.2, step 7): a producing role holds the producer lock, a Producer has created the artifact (section 1.3), and every role has opened `cache/` with `openat` and read and checked `header` through that descriptor. The VM keys the registry by that descriptor's `fstat`, so an object it finds pinned the directory whose header the VM checked. It takes the live object registered under the key or, when there is none, builds one (section 5.2) and registers it; a dead weak entry is replaced the same way. The header bytes an object validated equal every VM's own, since every input of the header is process-wide (SPEC-integrator.md section 6.4); a debug assertion checks it. `ArtifactStore.h` declares:

```cpp
class ArtifactRegistry {
public:
    // The object for the directory cacheFd names; builds and registers one when no VM of the process holds it.
    // An error is the errno of the failing call, which start reports at start.io.
    static Expected<Ref<OpenedArtifact>, int> take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);
};
```

`take` holds `m_lock` only to look the key up and to register a new object, and builds an object outside it, so the registry lock is a leaf. When two VMs of the process build an object for one directory at once, the second to register takes the first's and destroys its own.

An object is built and refreshed the same way whatever the roles of the VMs that hold it, so no VM's role decides what another VM of the process sees (sections 6.2 and 6.3). Nothing asks the index to hold every body on disk: a key it lacks is a miss for an import, which THREAD Failures calls normal, and the scoring read finds a saved body by its name (section 7.3).

The registry holds no producer lock: each producing `VMState` owns its `ProducerLock` (section 2), so the lock follows the VM and the object follows the VMs that hold it.

### 5.2 An opened artifact

`OpenedArtifact` (`ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr`) holds:

- three pinned directory descriptors: `parentFd` (`O_PATH | O_DIRECTORY`), `cacheFd` and `bodiesFd` (`O_RDONLY | O_DIRECTORY`). Every later lookup, open and write goes through them with `openat`, `renameat` and `unlinkat`, so a VM keeps using the directory whose header it validated. A deleted and recreated artifact is another directory; the pinned one only loses entries, and new imports miss instead of opening bodies whose header was never checked (THREAD Storage);
- the header digest;
- the refresh sources: the epoch mapping (section 2), when the lock file gave one, and the inotify descriptor (section 6.3), when the kernel gave one;
- under `Lock m_indexLock`: the index and its token counter (section 6), the last epoch seen, whether the next refresh must list, and whether the pinned directory is gone.

`ArtifactStore.h` declares it with the members sections 6 to 8 use:

```cpp
class OpenedArtifact final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<OpenedArtifact> {
    WTF_MAKE_NONCOPYABLE(OpenedArtifact);
    WTF_MAKE_TZONE_ALLOCATED(OpenedArtifact);
public:
    ~OpenedArtifact();   // unmaps the epoch page and closes every descriptor

    // Section 7; any thread that holds no JITCache lock.
    uint64_t token(const BodyKey&);
    BodyOpen open(const BodyKey&, ValidationMode);
    SavedSummaryRead readSavedSummaries(const BodyKey&, ValidationMode);
    bool containsKey(const BodyKey&);

    std::span<const uint8_t, 16> headerDigest() const LIFETIME_BOUND;
    int cacheFd() const;    // the writer's temporaries (section 8.2)
    int bodiesFd() const;   // the writer's renames (section 8.2)

private:
    friend class ArtifactRegistry;   // builds and registers objects (section 5.1)
    friend class ArtifactWriter;     // runs step 8 of section 8.2 under m_indexLock
    // Opens the descriptors, the epoch mapping and the inotify watch, then lists; an error is the failing call's errno.
    static Expected<Ref<OpenedArtifact>, int> create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);

    void refreshIfStale() WTF_REQUIRES_LOCK(m_indexLock);                       // section 6.3
    bool list() WTF_REQUIRES_LOCK(m_indexLock);                                // section 6.2; false leaves the index as it was
    void drainEvents(bool listOnOverflow) WTF_REQUIRES_LOCK(m_indexLock);       // section 6.3; the writer's step 8 passes false
    void learn(const BodyKey&, uint64_t inode) WTF_REQUIRES_LOCK(m_indexLock);  // section 6.4
    void erase(const BodyKey&) WTF_REQUIRES_LOCK(m_indexLock);

    const int m_parentFd;   // O_PATH | O_DIRECTORY
    const int m_cacheFd;
    const int m_bodiesFd;
    const std::array<uint8_t, 16> m_headerDigest;
    const uint64_t* const m_epoch;   // the lock file's epoch, mapped PROT_READ; null when the object has none (section 2)
    Lock m_indexLock;
    int m_inotifyFd WTF_GUARDED_BY_LOCK(m_indexLock) { -1 };   // -1 without inotify, and once the directory is gone
    HashMap<BodyKey, IndexEntry, BodyKeyHash, BodyKeyHashTraits> m_index WTF_GUARDED_BY_LOCK(m_indexLock);
    uint64_t m_nextToken WTF_GUARDED_BY_LOCK(m_indexLock) { 1 };
    uint64_t m_lastEpochSeen WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };
    bool m_listingPending WTF_GUARDED_BY_LOCK(m_indexLock) { false };
    bool m_gone WTF_GUARDED_BY_LOCK(m_indexLock) { false };
};
```

Everything outside `m_indexLock` is immutable once the object is built. A failure of the descriptors or of the listing at the build is a `Fault` at `start.io`; a missing epoch or inotify only changes how the index refreshes. The object is destroyed with the last `VMState` that holds it.

The object's reads (section 7) return results and raise nothing, and know no VM: the `VMState` turns their outcomes into faults and counters (SPEC-integrator.md section 7.2), the capture glue turns the scoring read's (SPEC-integrator.md section 9.3), and the store's tests read them directly (section 10).

## 6. The index

### 6.1 Entries

The index is a `HashMap<BodyKey, IndexEntry, BodyKeyHash, BodyKeyHashTraits>` whose hash reads the first four bytes of the key's identity digest, which SHA-256 makes uniform, mixed with its kind, specialization and mode bytes. An all-zero key, which `fromBytes` never accepts, is the empty value, and an all-`0xFF` key the deleted value. `ArtifactStore.h` declares the two, which every integrator map keyed by a body key uses (the index, the kept summaries and `delta`'s candidate table):

```cpp
struct BodyKeyHash {
    static unsigned hash(const BodyKey&);
    static bool equal(const BodyKey& a, const BodyKey& b) { return a == b; }
    static constexpr bool safeToCompareToEmptyOrDeleted = true;
};
struct BodyKeyHashTraits : GenericHashTraits<BodyKey> {
    static constexpr bool emptyValueIsZero = true;
    static BodyKey emptyValue();                  // std::bit_cast of BodyKey::byteSize zero bytes
    static void constructDeletedValue(BodyKey&);  // std::bit_cast of BodyKey::byteSize 0xFF bytes
    static bool isDeletedValue(const BodyKey&);
};
```

`BodyKey` keeps its bytes private and its `make` and `fromBytes` reject both values, but it is trivially copyable and exactly `BodyKey::byteSize` bytes, which a `static_assert` beside the traits checks, so both values are byte fills that need nothing from the UCB lane. Keying by the whole key makes the index exact: a key the index holds names exactly one file, so a mismatch between a file's envelope and its name is damage (B3), never a collision.

```cpp
struct IndexEntry {
    uint64_t token;   // nonzero; a fresh one whenever the index learns of another body at the key
    uint64_t inode;   // the body file's inode as a listing or the writer saw it; 0 when an event made the entry
};
```

The token is what `bodyVersion` returns (SPEC-integrator.md section 7.2). The object draws tokens from a counter of its own, starting at 1 and advanced under `m_indexLock`, so every VM of the process sees one token per key. The index learns of another body at a key in three ways, and each gives the key a fresh token: an event reports the key's file moved in (section 6.3); a listing finds the key absent from the index or its file under another inode than the entry holds (section 6.2); or the writer publishes the key (section 8.2, step 8). The index holds no commit identifier. Those live in the envelopes, which no lookup reads, so the UCB lane compares tokens only with tokens (SPEC-ucb.md R-INT-3).

### 6.2 Listing

A listing opens `bodies/` anew with `openat(bodiesFd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)`, reads it with `readdir` on `fdopendir` of that descriptor, which `closedir` closes, and builds a new map: every body name's key with the inode `readdir` reports for it (`d_ino`), temporaries, `.` and `..` ignored, and any other name counted for `status`. A key whose entry in the old index holds the same nonzero inode keeps its token, and every other key takes a fresh one. A body replaced by `renameat` carries its temporary's inode, so a listing gives a replaced body a fresh token and keeps the token of a file it saw before (SPEC-ucb.md section 6.3.2). Each listing reads an open file description of its own from offset zero, never one it shares with `bodiesFd` through `dup`. The new map replaces the index only when the whole listing succeeded; a listing that fails leaves the index as it was.

Every listing of an object's index runs under `m_indexLock` in one order: drain the inotify queue (read until `EAGAIN` and discard, when the object has a descriptor), read the epoch, list, and on success swap the map in and record that epoch as the last seen. A commit after the epoch read moves the epoch past the recorded value, and the next refresh catches it; an event for an entry the listing already found is applied again, which at worst gives the key one more fresh token and costs the UCB lane one more read of that body. Listings of an object's index run in two places (a Producer's check of a remnant, section 1.3, and maintenance list `bodies/` the same way without an index):

1. Building an object (section 5.2). A failure is a `Fault` at `start.io`.
2. A refresh that must list (section 6.3).

### 6.3 Refresh

Every object refreshes, whatever the roles of the VMs that hold it (section 5.1). At its build it creates `inotify_init1(IN_NONBLOCK | IN_CLOEXEC)` and watches `/proc/self/fd/<bodiesFd>`, the pinned directory, for `IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR`. A body enters `bodies/` only by the writer's rename from `cache/` (section 1.1, II5), so a commit raises one event in the watched directory, and a consumer's queue overflows only after about 16,384 commits since its last refresh. When either call fails, the object refreshes by listing instead.

`refreshIfStale()` runs under `m_indexLock` at the start of every `token` and `open` (section 7). It returns at once when the object has no epoch or its pinned directory is gone. Otherwise it loads the epoch and returns when it equals the last epoch seen; this is the whole cost of a lookup while nothing changes, one acquire load from a shared page. When the epoch moved:

- with inotify and no listing pending, it records the new epoch and reads events until `EAGAIN`. For a body name, `IN_MOVED_TO` gives the key a fresh token and inode 0 (present, inode unknown), and `IN_MOVED_FROM` and `IN_DELETE` erase it. `IN_Q_OVERFLOW`, or a `read` that fails with an error other than `EAGAIN`, makes it list (section 6.2). `IN_DELETE_SELF`, `IN_MOVE_SELF` and `IN_IGNORED` mean the pinned directory is gone: the index is cleared, the inotify descriptor closed and the object marked gone, so every later lookup misses without refreshing;
- without inotify, or with a listing pending, it lists (section 6.2), unless the object's last listing started less than `fallbackListingIntervalMilliseconds` ago (SPEC-integrator.md section 17). Then it returns without recording the epoch: the index answers as it stood, a body it lacks is a miss (THREAD Failures), and the first lookup after the interval lists. The object records when each listing starts, so a process whose objects have no inotify descriptor lists at most once per interval.

A listing that fails during a refresh leaves the index as it was, records the epoch it read and marks a listing pending, so the next epoch change lists again, since the events drained before the failure are gone; until then, the bodies the index lacks are misses (THREAD Failures).

A VM's own commits do not make its process refresh: the writer updates the index under `m_indexLock` and records its own bump as seen when it can (section 8.2, step 8). Commits by other processes and maintenance runs, the same process's maintenance included, move the epoch past what the object has seen and are refreshed as above.

The kernel queues a rename's events during the rename itself, before the writer bumps the epoch, so a lookup that sees the new epoch also reads the event. A crash between the rename and the bump delays a refresh until the next bump, the next commit through the object's writer included, which applies the queued event. This gives the cadence of SPEC-integrator.md II16, which SPEC-ucb.md section 6.3.2 counts on.

### 6.4 Lookup and update

`token(key) -> uint64_t`, 0 for a key the index lacks, `learn(key, inode)`, which gives the key a fresh token and that inode and inserts it when absent, `erase(key)` and `containsKey(key)` run under `m_indexLock`, as do the refresh, the listings and the writer's index update. The lock is a leaf: nothing under it takes another lock, the only system calls made under it are the `read`s of the inotify descriptor and the listings' calls, and the epoch is read and bumped with atomic operations on its shared page. It is never held while a body is mapped or validated, while an envelope is read, or while any lane, JSC or the collector is called. `open` and `readSavedSummaries` release it around their file work and take it again only to erase a key whose file is gone, and only while the key still holds the token they read before their `openat` (0 when absent): a token learned meanwhile, from the writer's own commit or an event, names a body that arrived after that `openat`, and erasing it would hide that body from `bodyVersion` (II8).

## 7. Opening bodies

`ArtifactStore.h` declares the opened artifact's three reads. `token` reads the index alone. The two others open a body by its name, `openat(bodiesFd, name, O_RDONLY | O_CLOEXEC)`, check it in the mode their caller gives, `ValidationMode::Full` exactly when the caller's VM runs strict (section 4.5), and return one of four outcomes:

```cpp
enum class StoreOutcome : uint8_t { Absent, Unavailable, Invalid, Found };
struct StoreFailure { ASCIILiteral check; int error { 0 }; };   // Invalid: container.io with its errno, or a check of section 4.5

struct BodyOpen { StoreOutcome outcome; RefPtr<ValidatedBody> body; StoreFailure failure; };     // body: Found only

class SavedSummaries {   // one body's mapping, with its three summary sections checked; unmaps when destroyed
public:
    uint64_t version() const;                    // the envelope's commit identifier
    uint8_t highestTier() const;
    std::span<const uint8_t> ucbFeedback() const;
    std::span<const uint8_t> cbSummary() const;
    std::span<const uint8_t> ics() const;        // the whole ICsBaseline section
};
struct SavedSummaryRead { StoreOutcome outcome; std::unique_ptr<SavedSummaries> summaries; StoreFailure failure; };   // summaries: Found only

// OpenedArtifact's reads, declared with the class in section 5.2: token (section 7.1, 0 when the index lists
// no body), open (section 7.2), readSavedSummaries (section 7.3) and containsKey (section 6.4).
```

| outcome | `open` | `readSavedSummaries` |
|---|---|---|
| `Absent` | a key the index lacks, without a system call; or `ENOENT`, which erases the entry unless its token changed meanwhile (section 6.4) | `ENOENT`, which erases an entry for the key unless its token changed meanwhile |
| `Unavailable` | `EMFILE`, `ENFILE` or `ENOMEM` from `openat`, `fstat` or `mmap`; the entry stays | the same |
| `Invalid` | any other error (`container.io`), or the first failed check of section 4.5 that the read's mode runs | the same |
| `Found` | the read succeeded; the index is left as it is | the same |

### 7.1 Reading a token

`token` refreshes the index (section 6.3) and returns the key's token, or 0 when the index lacks the key. It opens no file and reads no envelope (SPEC-ucb.md R-INT-3), and its refresh makes system calls only when section 6.3 says, once for every VM of the process.

### 7.2 Mapping and checking

`open` refreshes the index and looks the key up the same way. For a present key it opens the body name, takes its size with `fstat`, maps it with `mmap(nullptr, size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0)` and closes the descriptor; the populated mapping costs one call where page-by-page faults would cost one per page, and the checksums read every byte anyway. It then runs `validateBody` over the mapping in the caller's mode. Success creates the `ValidatedBody` through its private constructor, and the body owns the mapping and unmaps it in its destructor; a failure unmaps it first. The `VMState`'s mapping of each outcome is in SPEC-integrator.md section 7.2, and every case is in section 7's table.

A private mapping of a file that JITCache never rewrites keeps the validated bytes readable after the file is unlinked or replaced by a rename (THREAD Storage).

### 7.3 Reading a saved score

The capture glue reads a saved body's score through `readSavedSummaries(key, mode)` (SPEC-integrator.md section 9.3), which only a producing VM calls. The read opens the body by its name whatever the index holds, so it needs nothing from the index: `ENOENT` is the only way it answers that no body exists, and a body the index lacks, such as one whose producer died between its rename and its epoch bump, is found.

1. Open the body name, take its size with `fstat`, map the whole file with `mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)`, without `MAP_POPULATE`, so only the pages the read touches are faulted in, and close the descriptor.
2. Run B1 to B7 on the envelope and the directory, and B8 on `ucb.feedback`, `cb.summary` and `ICsBaseline`, the three sections a score reads, in the caller's mode. The other sections are left unread: the scoring reads nothing else, and an import of the body checks them all (section 7.2).
3. `Found` carries the mapping, the envelope's tier and commit identifier, and the three spans.

The glue's handling of each outcome, and the mapping's lifetime and exemption from the producer budget, are in SPEC-integrator.md sections 9.3 and 5.3.

### 7.4 Trust

The artifact is trusted input (THREAD Session): normal mode checks only its integrity, the header, the keys and the checksums, and trusts the structure the writer gave the file; strict also validates that structure and the lanes' material. The checksums reject damaged material; neither mode defends against a writer of the artifact directory, who could as well plant valid code. Only the producer, maintenance and the human write it, and JITCache never truncates or rewrites a published body. A process that truncates a body file a consumer has mapped can make that consumer fault when it reads the lost pages, as with any mapped file.

### 7.5 Twins-build fault hook

Only `ENABLE(JITCACHE_TWINS)` builds compile it, in `ArtifactStore.cpp` behind a test interface `ArtifactStore.h` declares, for the tests of SPEC-integrator.md section 16 and tests C4 and C6. A hook `{ call, error, n }` makes the n-th call, or every call when n is absent, of `open` (`openBody` on the shell's flag), `readSavedSummaries` (`scoring`) or a listing (section 6.2) fail at its `openat` with that error, as if the system call had returned it. `token` makes no such call, so the hook never reaches it. The shell sets it with `--jitcache-test-store-fault` (harness sub-SPEC section 5.1), and C4 and C6 through the test interface, which also offers the registry hook C4 uses to build a second object for a directory instead of sharing the first, and the two refresh hooks C4 uses to force inotify off and to set the listing interval (section 6.3). `ArtifactStore.h` declares it, and task 5 of SPEC-integrator.md section 18 implements it:

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache::StoreTesting {
enum class Call : uint8_t { Open, ReadSavedSummaries, Listing };
struct Fault { Call call; int error; std::optional<uint64_t> n; };   // n counts calls of that kind from 1; absent: every call
void setFault(std::optional<Fault>);   // process-wide, any thread
void setRegistrySharing(bool);         // false: ArtifactRegistry::take builds a new OpenedArtifact at every call (test C4)
void setInotify(bool);                 // false: an object built afterward creates no inotify descriptor and lists instead (test C4)
void setFallbackListingInterval(std::optional<Seconds>);   // replaces fallbackListingIntervalMilliseconds for every object; absent: the parameter (test C4)
}
#endif
```

## 8. The writer

### 8.1 Resources

Each producing `VMState` owns one `ArtifactWriter` (SPEC-integrator.md section 7.3), which `start` creates over the shared `OpenedArtifact`, the `ProducerLock` the state holds and the state's budget, with a staging buffer of `writerStagingBytes` (SPEC-integrator.md section 17). The buffer is allocated and charged to the producer budget at the first commit and kept until production ends, when `releaseStagingBuffer` frees it and releases the charge. Besides the lanes' own buffers, which they charged, a commit uses the staging buffer and the reread's `BodyValidationStream`, a fixed-size object on the stack that holds at most the envelope, the directory and a CRC per section and allocates nothing (section 4.5). Neither grows with the body's size (THREAD Capture). A test constructs a writer with any staging size of at least one byte (test C7).

### 8.2 Commit

`ArtifactWriter::commit(stamp, sections)` runs on the VM thread inside the capture (SPEC-integrator.md section 9.4, step 9). Every section is opaque bytes to it, as THREAD Storage has the container treat them: it lays the sources out, streams, checksums and counts their bytes, rereads the file, validates the container and publishes it, and parses no lane format.

1. Staging. At the first commit, charge `stagingBytes` and allocate the buffer. A refused charge returns `budget.limit`, with nothing written.
2. Layout. Take each source's size; compute N, the offsets of section 4.3 and the file's size with overflow-checked arithmetic.
3. Create a temporary in `cache/` through `cacheFd` (section 1.1). Failure: `writer.create`.
4. Stream the sources in order to their offsets, through the staging buffer, writing zeros between sections and computing each section's CRC as its bytes pass. A source in memory goes through the buffer in chunks, or straight to `write` when larger than it. A streamed source's function receives a sink that appends to the buffer, flushing it to the file when full, and adds to the source's count; the sink returns false once a write has failed or once the bytes would pass the source's size, and the source then stops and returns false (SPEC-image.md section 10). A short write is retried; any other write error is `writer.write`. A source whose count ends other than its size, or that returns false while its sink refused nothing, is `writer.section`, with the section's name (SPEC-integrator.md section 7.1) and both counts, the size a lane declared and the bytes it streamed, in the detail.
5. `pwrite` the directory at offset 128 and the envelope at offset 0: a fresh commit identifier, the header digest, the stamp's key, tier, L and P, the file's size and both CRCs. Failure: `writer.write`.
6. Reread. `pread` the file from offset 0 in chunks of the staging buffer and feed them to a `BodyValidationStream` (section 4.5) in `Full` mode, whatever strict says, with the stamp's key and the artifact's header digest; check that the file has exactly the planned size. Any difference or read error is `writer.reread`. Each section's checksum in the directory was computed from the bytes step 4 streamed, so only now is the temporary known to hold the lanes' bytes.
7. Take the temporary's inode with `fstat`, close its descriptor, then `renameat(cacheFd, temporary, bodiesFd, name)`, which replaces any earlier version atomically, since `cache/` and `bodies/` share a file system, and keeps the temporary's inode. Failure: `writer.publish`.
8. Under `m_indexLock`: read the object's inotify queue until `EAGAIN` and apply its events as a refresh does (section 6.3), except that an overflow or a read error marks a listing pending instead of listing; `bumpEpoch()` (section 2); when the object has an epoch, no listing is pending and the value the bump replaced equals the last epoch the object saw, record the new value as seen; and `learn(key, inode)` with the inode of step 7, which gives the key a fresh token and inserts it when the index lacks it (section 6.4). Return the commit identifier and the file's size.

Step 8 keeps the object's index current without a refresh, so every VM of the process sees the body from its publication on. The queue holds this VM's commits' events and, until its first commit drains them, the events of changes made before the VM took the lock that no lookup has drained yet; step 8 applies them all, and the producer lock keeps every other process from adding more. When the object had seen every earlier bump, neither the events nor the bumps of this VM's commits make any VM of the process refresh or list. Otherwise its last epoch seen stays behind, and its next lookup refreshes, reading an empty queue, or listing when the object has no inotify descriptor or a listing is pending. The index entry step 8 inserts is charged to the producing VM's budget before the commit begins (SPEC-integrator.md section 9.4, step 8).

Any failure after step 3 unlinks the temporary, best effort, and returns `CommitFailure { check, detail }`, whose detail is `strerror` of the error for a failed system call; the capture glue raises it as a recording fault (SPEC-integrator.md section 9.4). A process killed between steps 3 and 7 leaves a temporary, which no reader sees and `clean` reports and removes; one killed before step 3 leaves nothing, and one killed after step 7 has published the body (harness sub-SPEC section 12). Commits are independent and process-crash-consistent, and no `fsync` is issued, since power-loss durability is excluded (THREAD Capture).

### 8.3 Twins-build hooks

Only `ENABLE(JITCACHE_TWINS)` builds compile these, in `ArtifactWriter.cpp` behind a test interface `ArtifactWriter.h` declares, for test C7 and the tests of SPEC-integrator.md section 16.2:

- A fault injection, `{ check, n }`: the writer's n-th commit fails at `check` (one of `writer.create`, `writer.write`, `writer.reread`, `writer.publish`) as if the call had failed with `EIO`, after doing the real work of the earlier steps.
- A kill point, `{ point, n }`: the n-th commit calls `raise(SIGKILL)` right after the step the point names, so the process dies leaving the files that step left. The seven points and what each leaves are in harness sub-SPEC section 12: after step 2, after step 3, after step 4's first `write`, after steps 4, 5 and 6, and after step 7's rename.
- `rewriteSection(key, kind, offset, bytes) -> Expected<CommitResult, CommitFailure>`: it opens the key's current body in `Full` mode (section 7.2), copies it, overwrites `bytes.size()` bytes of the section at `offset`, which must lie inside the section, and commits the copy's sections, each an in-memory source, under a stamp holding the envelope's key, tier, L and P. The commit recomputes every checksum and draws a fresh commit identifier, so the lanes' validation sees the damage while the container sees a sound file. It runs only in a producing VM, whose writer and lock it uses; the capture glue's `rewriteSectionForTesting` calls it and erases the key's kept summary (SPEC-integrator.md section 9.3).

## 9. Filesystem errors

| operation | error | outcome |
|---|---|---|
| `start`: open or create the parent, `cache/`, `bodies/`, the header or the lock file | any but those listed | `Fault` at `start.io` |
| `start`: `flock` | `EWOULDBLOCK` | `Busy` |
| `start`: read the header | missing | `Rejected` at `start.artifact-missing` |
| `token` | none: it reads the index, and a refresh's errors are the rows below | the key's token or 0 |
| `open`, `readSavedSummaries` | any | section 7's table; SPEC-integrator.md sections 7.2 and 9.3 say what the VM and the glue make of each outcome |
| writer | any | a recording fault at the step's `writer.*` name |
| `start`: the listing that builds an object (section 6.2) | any | `Fault` at `start.io`; the object is destroyed unregistered |
| refresh: `read` on the inotify descriptor | any but `EAGAIN` | the refresh lists (section 6.2); the descriptor stays |
| refresh: listing | any | the index stays as it was; the next epoch change lists again |

## 10. Tests

In `tests/ContainerTests.cpp`, `tests/StoreTests.cpp` and `tests/WriterTests.cpp`, run by `testjitcache`:

- C1. CRC32C: the RFC 3720 vectors (`"123456789"`, 32 zero bytes, 32 `0xFF` bytes, the 0 to 31 ascending pattern) through `crc32cExtend`, and equal results with a bitwise reference the test computes on 4096 random buffers of random lengths and alignments, fed whole and in random pieces.
- C2. Header: a header built from crafted facts parses back to them; each corrupt case of section 3.2 is classified corrupt and each differing field incompatible, with the detail naming that field; a layout version of 2 is incompatible.
- C3. Body: a body built from synthetic sections of every kind validates in both modes. In `Full` mode each check B1 to B8 fails exactly when its own condition is broken, with its own name, for each way section 4.5 lists. In `Integrity` mode each integrity part fails the same way, and a body broken only in a structure part (a nonzero zero byte, a tier of 2, a zero commit identifier, an unknown section kind, entries out of order, nonzero padding, a missing required section) validates. Truncation at every region boundary fails in both modes; no malformed input reads out of bounds under ASan in either.
- C4. Index: a listing finds body names and ignores temporaries and foreign names, and every later listing of the same object finds the same set with the same tokens; a body replaced by a rename while events are off (inotify forced off) gets a fresh token at the next listing, and an untouched one keeps its token. Two `OpenedArtifact`s for one directory (the registry hook of section 7.5) stand for two processes; a test helper renames a synthetic body (C3's builder) into `bodies/`, or unlinks one, and then bumps the epoch through a `ProducerLock`. An addition, a replacement, a deletion and a rename are each visible to both objects at their first lookup after the epoch moves: an added or replaced body's key has a token it did not have before, and a deleted one's token is 0. An overflowed event queue makes a listing, and a second overflow another, each finding every body. A listing the store's fault hook fails leaves the index as it was, and the next epoch change lists and finds every body. A body renamed in without a bump is found by no lookup before the epoch next moves and by the first lookup after it, with inotify on and forced off. Deleting the directory empties the index. Without the lock file no refresh happens. With inotify forced off (`setInotify(false)`) and the listing interval set to zero, each of two epoch changes lists and finds every body, and every case above that forces inotify off runs with that interval; with a one-second interval, a second epoch change within the second makes no listing, so a body it brought is absent until the first lookup after the second has passed, which lists and finds it.
- C5. Lock: a second `ProducerLock::tryAcquire` on the same parent in the process is busy while the first lives and succeeds once it is destroyed; a lock file shorter than 64 bytes, or with another tag, is set up in place; the lock file survives deleting `cache/`; `bumpEpoch` returns the value it replaced, one less than the value a reader then loads.
- C6. Opening, on the store's own results: `token` of any key reaches no `openat`, and of a key the index lacks is 0; `open` of a key the index lacks is `Absent` and reaches no `openat` either (the store's fault hook, set to fail every call, never fires); `ENOENT` gives `Absent` and erases the entry, after which `token` is 0; a forced `ENOMEM` (section 7.5) gives `Unavailable` and keeps the entry; a damaged body gives `Invalid` at the check of C3, in the mode C3 names; a `ValidatedBody` keeps returning the same bytes after its file is unlinked and after another version is renamed over it, and its `version()` is the commit identifier of the file it mapped. The scoring read: a body renamed in without a bump, which the index lacks, is `Found`; a key with no file is `Absent`; a forced `EMFILE`, `ENFILE` or `ENOMEM` gives `Unavailable` and keeps the index entry; an unlinked file gives `Absent` and erases the entry; a body whose image section is damaged gives `Found` with the three summary spans, while `open` gives `Invalid` at `container.checksum`; a damaged `cb.summary` gives `Invalid` at `container.checksum`.
- C7. Writer, on `CommitSections` built from crafted spans and streamed sources: a commit publishes a body that `open` validates and whose sections are byte for byte the sources' bytes; its temporary is created in `cache/`, and a second object's queue receives one event for the commit, the rename's `IN_MOVED_TO`; each publish bumps the epoch once, and afterwards the committing object's index holds a fresh token for the key and the published file's inode, its last epoch seen equals the epoch and its inotify queue is empty, so its next lookup neither refreshes nor lists, while a second object for the directory sees the commit at its first lookup after the bump; a commit through an object whose queue holds an earlier change's events, or that has not seen an earlier bump, leaves an index holding both changes, and in the second case the object's next lookup refreshes; each fault injection of section 8.3 leaves no temporary and no new file, and leaves the earlier version readable; a streamed source that emits one byte fewer or one more than its size fails at `writer.section` with no temporary left; a staging buffer of one byte, and one smaller than every section, still writes the right bytes; a refused staging charge fails at `budget.limit` before any file exists; sections whose bytes no lane would accept are published unchanged, since the writer reads no lane format. A kill point ends its process, so the producers of harness sub-SPEC H8 and SPEC-integrator.md's `producer-kill.js` test each point rather than C7.
- C8. Creation, through `start` (SPEC-integrator.md section 16.1, T-START): a header-less `cache/` with only temporaries is reset and reused; one with a body name is `start.not-an-artifact`; an artifact with a header is `start.artifact-exists`.
