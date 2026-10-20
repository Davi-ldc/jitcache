# SPEC-integrator.container: the artifact on disk

Part of [SPEC-integrator.md](SPEC-integrator.md), which indexes it and whose facts, interfaces and invariants apply here; its section 1 lists what this file covers. [SPEC-integrator-history.md](SPEC-integrator-history.md) records the path to its non-obvious decisions. All integers are little-endian, as every supported target is (`JITCacheContainer.h` asserts `std::endian::native == std::endian::little`), and fields are read with `memcpy`-based loads.

## 1. Layout

### 1.1 Paths and names

THREAD Storage fixes the shape; this section fixes the names, which `ArtifactNames` in `ArtifactStore.h` holds:

```text
<parent>/
  .cache.producer.lock
  cache/
    header
    pack
    .header.<32 hex>.tmp       a header being written
    .pack.<32 hex>.tmp         a pack being written: a new artifact's or a compaction's
  .cache.replaced/             an incompatible cache/ being removed
```

- `<parent>` is `Config::artifactPath`.
- A temporary's name is `.header.` or `.pack.`, 32 lowercase hex digits of a fresh 128-bit value from `cryptographicallyRandomValues`, and `.tmp` (`temporaryFileName(TemporaryKind)`, which `temporaryKindOfFileName` recognizes). A temporary is created with `O_CREAT | O_EXCL` in `cache/`, so a header or a pack takes its name only through `renameat`.
- No file is named after a body. Diagnostics, reports and the shell spell a body key as the lowercase hex of its 40 canonical bytes (SPEC-ucb.md section 3.1): `bodyKeyHexDigits` writes it, and `bodyKeyFromHex` accepts exactly 80 lowercase hex digits whose bytes `BodyKey::fromBytes` accepts.
- Directories are created with mode 0755 and files with mode 0644, before the umask. Every descriptor is opened through `openAt`, which adds `O_CLOEXEC`, so no child process inherits the producer lock or the pack.

`JITCacheFiles.h` holds the file helpers the store, `start` and maintenance share, each a system call or a loop of them on the calling thread: `openAt`, which also retries `EINTR`; `readExactly` and `writeExactly`, which `pread` or `pwrite` a whole span at an offset, retry `EINTR` and short transfers, and return `EIO` for an early end of file or a write that wrote nothing; `errorText`, an errno's `strerror` text; and `readHeaderFile` (section 3.2). `listDirectory(fd, visit)` in `ArtifactStore.h` reads a directory through an open file description of its own, `openat(fd, ".")` then `fdopendir` and `readdir`, because glibc's `fdopendir` does not reset an offset it shares. Creation, replacement and maintenance list `cache/` and `.cache.replaced`; no reader lists a directory.

### 1.2 What an artifact is

An artifact exists at `<parent>` exactly when `<parent>/cache/header` exists. A `cache/` directory without a header is a remnant, left by an interrupted creation (section 1.3) or by the last steps of a whole-artifact deletion (maintenance sub-SPEC section 4.5). Neither leaves a committed body in the remnant's pack unless someone else put one there: creation writes an empty pack before the header, and a whole deletion empties the pack before it removes the header.

`packHoldsBodies(cacheFd)` answers whether a remnant's pack may hold a body. It opens `cache/pack` with `O_RDONLY | O_NONBLOCK | O_NOFOLLOW`, reads the root (section 4.6), and answers false only when there is no pack or its root is 104, a new pack's valid end, which every record pushes past. A pack it cannot read counts as holding bodies. Its callers hold the producer lock, so no writer moves the root during the read.

`retirePack(cacheFd)` writes 1 to the retired word of the file at `cache/pack`, which it opens with `O_WRONLY | O_NONBLOCK | O_NOFOLLOW`, and returns 0 when there is no pack or the errno of the call that failed. Its callers hold the producer lock and call it before they unlink that file or rename another over it: the creation that reuses a remnant (section 1.3), and the compact that finishes a remnant or deletes the whole artifact (maintenance sub-SPEC sections 3 and 4.5). Every object that maps the file then meets the mark and looks at the path again (section 6.3).

### 1.3 Creating an artifact

A Producer creates the artifact while it holds the producer lock (`createArtifact` in `ArtifactAccess.cpp`; SPEC-integrator.md section 3.2, step 7, names the outcomes):

1. When `cache/` is absent: `mkdirat(parent, "cache", 0755)`. When `cache/` holds a header, the creation stops at `Exists`. When `cache/` exists without a header: if `packHoldsBodies` says its pack may hold a body, the creation stops at `NotAnArtifact` and changes nothing; otherwise its pack, if it has one, is marked retired with `retirePack`, every temporary in `cache/` is removed, any other name stays, and the directory is reused. An object that read the remnant's empty pack, as one does between a whole deletion's emptying and its stop, follows the mark to the new pack at its next read. A failed mark is `IO`.
2. `createEmptyPack` writes a new pack (section 4.6) to a pack temporary, the prefix with a fresh identifier followed by an empty full index, and `renameat` moves it to `pack`, over the pack a remnant held. A failure unlinks the temporary.
3. The header's bytes, `headerNamingApp(appDigest)` for the config's app (section 3.1), go to a header temporary, which `renameat` moves to `header`. The pack goes first, so a reader that finds a header finds a pack.
4. The VM takes the artifact's `OpenedArtifact` from the registry, as every role does (section 5.1).

A failed system call in any step is `IO`; whatever the step created stays, and the next Producer or `compact` finishes or removes it (maintenance sub-SPEC section 3).

### 1.4 Replacing an artifact

A ConsumerProducer whose artifact has an incompatible header, or one made for another app (section 3.2), replaces it while it holds the producer lock (`replaceArtifact`; SPEC-integrator.md section 3.2, step 7):

1. `removeReplacedArtifact` removes a `.cache.replaced` that an earlier replacement left, as in step 4, since step 2 needs the name.
2. `renameat(parent, "cache", parent, ".cache.replaced")` moves the old artifact aside. A process that opened the old artifact keeps its pinned descriptors and its mapping of the old pack (section 5.2), so its imports go on reading that pack, which stays mapped after its unlink.
3. A new artifact is created as a Producer creates one (section 1.3, steps 1 to 3), so its header names the config's app, if any.
4. `removeReplacedArtifact` removes `.cache.replaced`: its temporaries, its `pack` and its `header`, then the directory. A name that is none of these stays, and with it the directory.

The registry keys objects by the `cache/` directory's inode (section 5.1), so the new artifact gets an object of its own. A failure in steps 1 to 3 is `IO`. A failure in step 4 leaves what it could not remove, and the start succeeds. The next replacement's step 1 or `compact` (maintenance sub-SPEC section 3) removes the leftover, except a name step 4 does not know, which stays until someone removes it by hand and until then fails step 1 of every replacement with `ENOTEMPTY`. ([history](SPEC-integrator-history.md#a-consumerproducer-replaces-an-incompatible-artifact))

### 1.5 Reaching an artifact at start

`start` reaches the artifact through `openArtifact(path, mode, appDigest)`, which `ArtifactAccess.h` declares and SPEC-integrator.md section 3.2, step 7, walks through: it opens `<parent>`, takes the producer lock when the mode needs it (section 2), opens or creates `cache/` and checks its header against the process's and the config's app (section 3.2), and takes the opened artifact from the registry (section 5.1), which maps the pack and reads its index (section 5.2). The lock stays a local until success, so every failure releases it, and the descriptors it opens close before it returns, since the opened artifact pins its own. `start` is its only caller and maps each failure kind to its own result.

```cpp
enum class ArtifactAccessMode : uint8_t {
    Open,            // a Consumer's: no producer lock; <parent>, cache/ and a compatible header not made for another app must exist
    Create,          // a Producer's: the producer lock; creates <parent>, one level deep, and an artifact where none exists
    OpenOrReplace,   // a ConsumerProducer's: the producer lock; opens a compatible artifact and replaces an incompatible one
                     // or one made for another app
};

struct ArtifactAccess {
    std::unique_ptr<ProducerLock> producerLock;   // Create and OpenOrReplace
    RefPtr<OpenedArtifact> artifact;
    String replaced;                              // OpenOrReplace: why the old artifact went, its differing field or both apps;
                                                  // null otherwise
};

struct ArtifactAccessFailure {   // the detail names the path and the cause
    enum class Kind : uint8_t {
        Missing,         // <parent>, cache/ or cache/header does not exist, in a mode that does not create it
        Exists,          // Create: cache/header exists
        NotAnArtifact,   // Create: a header-less cache/ whose pack may hold bodies
        Incompatible,    // Open: the header passes its own checks but differs from the process's
        AnotherApp,      // Open: the header is compatible but made for another app than the config's
        Busy,            // another producer or maintenance holds the producer lock
        IO,              // a system call failed, the pack's mapping included
        CorruptHeader,   // the header fails its own checks
        DamagedPack,     // cache/pack fails container.pack while the registry builds the object
    };
    Kind kind;
    String detail;
};

std::expected<ArtifactAccess, ArtifactAccessFailure> openArtifact(const String& path, ArtifactAccessMode, std::optional<uint64_t> appDigest);
```

## 2. The lock file

`<parent>/.cache.producer.lock`, 64 bytes:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCLOCK` |
| 8 | 4 | layout version, 1 |
| 12 | 52 | 0 |

The file carries nothing a reader needs; its tag and version only let `tryAcquire` recognize it. A producing VM and `compact` hold the lock through one type, which `ArtifactStore.h` declares:

```cpp
class ProducerLock final {
    WTF_MAKE_NONCOPYABLE(ProducerLock);
    WTF_MAKE_TZONE_ALLOCATED(ProducerLock);
public:
    struct Failure { bool busy; int error; };   // busy: another holder has it; otherwise the errno of the call that failed
    static std::expected<std::unique_ptr<ProducerLock>, Failure> tryAcquire(int parentFd);
    ~ProducerLock();                            // closes, which releases the lock

private:
    explicit ProducerLock(int fd);

    const int m_fd;                             // the lock file, O_RDWR | O_CLOEXEC, holding LOCK_EX
};
```

`tryAcquire`:

1. `openAt(parentFd, ".cache.producer.lock", O_RDWR | O_CREAT, 0644)`.
2. `flock(fd, LOCK_EX | LOCK_NB)`, retried on `EINTR`. `EWOULDBLOCK` means another producer or maintenance holds it: close the descriptor and return `busy`. Any other error closes it and returns the error.
3. With the lock held, a file shorter than 64 bytes, or with another tag or version, is set up in place: `ftruncate(fd, 64)`, then `pwrite` of the 64 bytes above. A failure closes the descriptor, which releases the lock. The file is never deleted or replaced (THREAD Storage), so every process locks the same inode.

The lock lives on the open file description: destroying the `ProducerLock`, when the `VMState` that owns it is destroyed or maintenance ends, or the process exiting, releases it. Two descriptors of one process conflict, so a second producing VM in a process is busy too. Readers take no lock and never open the lock file (THREAD Storage); a commit reaches them through the pack's root (section 6.3).

## 3. The header

### 3.1 Layout

`cache/header`, with B build IDs and M must-match options:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCHEAD` |
| 8 | 2 | layout version, 2 ([history](SPEC-integrator-history.md#the-headers-layout-version-2)) |
| 10 | 1 | architecture: 1 x86_64, 2 ARM64 |
| 11 | 1 | B, 1 or 2 |
| 12 | 4 | H, the header's size: 40 + 72·B + 8·M + 8 |
| 16 | 8 | the CPU feature vector (SPEC-integrator.md section 5.2) |
| 24 | 4 | M, 3 in this version |
| 28 | 1 | the app record's flag: 0 when the header names no app, 1 when it names the app at offset 32 |
| 29 | 3 | 0 |
| 32 | 8 | the app record's digest, `Config::appDigest` of the config that created the artifact (SPEC-integrator.md section 3.2); 0 when the flag is 0 |
| 40 | 72·B | build-ID records: `u8` role (1 main executable, 2 engine object), `u8` length n (1 to 64), `u16` 0, `u32` 0, then 64 bytes holding the ID in the first n and zero after; the main executable's first, the engine object's second when it is a separate object |
| 40 + 72·B | 8·M | option records: `u16` index (0 `evalMode`, 1 `useExplicitResourceManagement`, 2 `useImportDefer`), `u8` type (1 `Bool`), `u8` value (0 or 1), `u32` 0; in index order |
| H - 8 | 4 | CRC32C of bytes [0, H - 8) |
| H - 4 | 4 | 0 |

The app record is bytes [28, 40). The process computes its own header bytes once, from the facts of SPEC-integrator.md section 5.2, with an app record that names no app, since the app is a config's and not the process's (`expectedHeader()` in `JITCacheContainer.cpp`). `headerNamingApp(appDigest)` returns those bytes for an empty `appDigest`, and otherwise a copy whose app record names it, with the CRC computed again; creation writes what it returns (section 1.3).

### 3.2 Reading a header

A Consumer or ConsumerProducer reads the whole file with `readHeaderFile(cacheFd)` (`JITCacheFiles.h`). It opens `header` with `O_RDONLY | O_NONBLOCK`, so a FIFO at the name opens without waiting for a writer, and `fstat`s it: a file that is not a regular file reads as empty and fails the checks below as a corrupt header. It reads at most 4 KiB and one byte, so a larger file reads as too large. A failed `openat`, or a failed `fstat` or `read`, is the failure's step (open or read) and errno, `ENOENT` at the open meaning that the directory holds no header. The checks:

- A file shorter than 48 bytes, larger than 4 KiB, or without the tag `JITCHEAD` is corrupt.
- A layout version other than 2 is incompatible: another format, so the rest is not read.
- Otherwise it is corrupt when H differs from the file's size or from the formula, B is not 1 or 2, M is not 3, a record has an out-of-range role, length, index, type or value, the app record's flag is neither 0 nor 1 or is 0 beside a digest other than 0, a padding or reserved byte is not zero, or the CRC differs.
- A header that is not corrupt is compatible exactly when its bytes [0, H - 8) equal the process's outside the app record.
- A compatible header is made for another app exactly when it names an app, the config names one, and the two digests differ (THREAD Session). A header or a config that names none gates nothing.

The comparison names the first field that differs: the architecture, a build ID, a CPU feature bit by its predicate's name, or an option by its name. The app check runs only on a compatible header, so an artifact both incompatible and made for another app is incompatible. Its detail gives both apps as 16 lowercase hex digits of the 64-bit value, `app <header's digest> in the cache, <config's digest> in this run`.

`JITCacheContainer.h` declares `checkArtifactHeader(file, processHeader, appDigest)`, which `start` calls with `expectedHeader().span()` and the config's `appDigest` and which answers corrupt, incompatible, made for another app or compatible, each with its detail, and `readArtifactHeader(file)`, the header's own checks without the comparison with the running process or an app, which maintenance calls (maintenance sub-SPEC section 2).

### 3.3 The header digest

The header digest is the first 16 bytes of the SHA-256 (SPEC-ucb.md section 3.8) of the header's H bytes (`artifactHeaderDigest`), app record included, of the bytes read from the file, never of bytes a VM computes, since a VM whose config names no app opens an artifact whose header may name one. The pack's prefix (section 4.6) and every body's envelope (section 4.1) stamp the digest of the header they were written under. Mapping a pack requires the prefix's digest to equal the opened artifact's, and every read of a body requires the envelope's to (check B4). Two artifacts made by the same binary, options and CPU features for the same app, or both for none, have equal headers, so a pack moves between them freely (packaging is external, THREAD Storage), while a pack from an incompatible artifact, or one of another app, is rejected.

## 4. Bodies and the pack

A body is the run of bytes THREAD Storage names: an envelope, a section directory and the sections. Each version of a body lies in one record of the pack (section 4.6). Sections 4.1 to 4.3 give offsets from the body's first byte.

### 4.1 Envelope

128 bytes at offset 0:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCBODY` |
| 8 | 2 | layout version, 1 |
| 10 | 2 | envelope size, 128 |
| 12 | 2 | N, the number of sections, 1 to 64 |
| 14 | 1 | the highest tier the body holds: 0 for a body without an image, which holds the UCB lane's sections alone, 1 for a baseline capture |
| 15 | 1 | 0 |
| 16 | 40 | the body key |
| 56 | 8 | the commit identifier: nonzero, drawn with `cryptographicallyRandomValues` for every commit |
| 64 | 16 | the header digest (section 3.3) |
| 80 | 8 | S, the body's size |
| 88 | 4 | L (THREAD Maintenance): for a body with an image, the LLInt threshold the import skips, `envelopeLLIntThreshold`; for one without, the LLInt progress it carries, `carriedLLIntPoints` |
| 92 | 4 | P (THREAD Maintenance): the baseline counter's progress, the CB lane's `counterProgress`; 0 for a body without an image |
| 96 | 4 | CRC32C of the directory |
| 100 | 20 | 0 |
| 120 | 4 | CRC32C of bytes [0, 120) |
| 124 | 4 | 0 |

The commit identifier is the body version THREAD Maintenance names, which compact compares and `ValidatedBody::version()` returns (SPEC-integrator.md section 6.2). The record header and the index entry repeat it, and the index answers it as the key's token (section 6.1). ([history](SPEC-integrator-history.md#bodies-are-checksummed-with-crc32c-and-versioned-by-random-identifiers))

### 4.2 Section directory

N entries of 24 bytes at offset 128:

| offset | size | field |
|---|---|---|
| 0 | 2 | type id (SPEC-integrator.md section 6.1) |
| 2 | 1 | tier |
| 3 | 1 | 0 |
| 4 | 4 | CRC32C of the section's bytes |
| 8 | 8 | the section's offset from the body's start |
| 16 | 8 | the section's size |

### 4.3 Sections

The entries are in strictly increasing order of (type id, tier), which is the order of `SectionKind`. The first section starts at the first multiple of 8 at or after 128 + 24·N, and each next one at the first multiple of 8 at or after the end of the one before. The bytes between sections are zero, and the body ends where the last section ends. A section may be empty; the lanes' own validation decides whether theirs may. `layOutBodySections(entries)` sets each entry's offset from the kinds and sizes in directory order and returns S, or nothing when there are no entries or more than 64 or the arithmetic overflows; `encodeBodyDirectory` and `encodeBodyEnvelope` write the directory and the envelope with their CRCs.

### 4.4 CRC32C

The checksum is the CRC-32C of RFC 3720: the Castagnoli polynomial `0x1EDC6F41`, reflected (`0x82F63B78`), initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`; `crc32c("123456789")` is `0xE3069283`. `JITCachePlatform.h` declares `uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>)`, which takes and returns the running state without the initial value or the final XOR, so the writer can checksum a stream; every whole checksum is `~crc32cExtend(~0u, bytes)`. `JITCachePlatform.cpp` defines it as the container's own CRC-32C, on any thread, by one of three paths that give the same state: SSE4.2's `crc32` instruction, eight bytes at a time, on an x86_64 CPU that has it, which `__get_cpuid` tells once per process (`__builtin_cpu_supports` needs compiler-rt's `__cpu_model`, which not every link provides); the CRC extension's `crc32c` instructions on an ARM64 build whose target defines `__ARM_FEATURE_CRC32`, as Bun's `-march=armv8-a+crc` does (SPEC-integrator.md N15); and otherwise a `constexpr` table of 256 entries built from the reflected polynomial, one byte at a time. In twins builds `JITCachePlatform.h` also declares `uint32_t crc32cExtendWithTableForTesting(uint32_t state, std::span<const uint8_t>)`, the table path alone, so the test compares it with the instructions on a CPU that has them. ([history](SPEC-integrator-history.md#bodies-are-checksummed-with-crc32c-and-versioned-by-random-identifiers))

### 4.5 Validation

`validateBody(bytes, expectedKey, headerDigest, ValidationMode) -> std::expected<BodyLayout, ContainerCheck>` runs these checks over a body's S bytes in this order and names the first that fails. `JITCacheContainer.h` declares `enum class ValidationMode : uint8_t { Integrity, Full }` and the checks' names in `ContainerChecks`. Each check has an integrity part, which both modes run, and some have a structure part, which only `Full` adds. Integrity is what THREAD Session has normal mode check: the header digest (B4) ties the body to the header `start` checked, B3 checks the key and B2, B6 and B8 the checksums, with the sizes and bounds that keep those checksums inside the body (B1 and the framing parts of B2 and B6). The pack's own checks, which come before and after these in a read, are `container.pack` (sections 4.6 and 7.2). ([history](SPEC-integrator-history.md#strict-integrity-in-both-modes-structure-under-strict))

| check | name | integrity, both modes | structure, `Full` only |
|---|---|---|---|
| B1 | `container.size` | the body is at least 128 bytes and its size, the S its index entry gives, equals the envelope's | |
| B2 | `container.envelope` | the tag, layout version 1, envelope size 128, N from 1 to 64, and the envelope's CRC | the zero bytes and a highest tier of 0 or 1 |
| B3 | `container.key` | the envelope's key equals `expectedKey`, the key of the index entry that names the record | |
| B4 | `container.header-digest` | the envelope's header digest equals the opened artifact's | |
| B5 | `container.version` | | the commit identifier is not zero |
| B6 | `container.directory` | the directory's CRC; every entry's offset and size lie inside the body without overflow | every entry's zero byte; every (type id, tier) is a section kind of SPEC-integrator.md section 6.1; the entries strictly increase; every offset and size follows section 4.3, and the padding is zero |
| B7 | `container.required` | | the sections present are those `isSectionRequired` names for the highest tier, every kind whose tier is at most it, with `image-twins.baseline` present exactly in `ENABLE(JITCACHE_TWINS)` builds, plus at most `ucb.analysis`, which `isSectionRequired` never names and which only a body whose `expectedKey.identityKind()` is `Module` may hold (SPEC-ucb.analysis.md R-INT-A1) |
| B8 | `container.checksum` | each section's CRC | |

`JITCacheContainer.h` declares the checks' entry points. The writer rereads a record through the staging buffer, in chunks of any size down to one byte (section 8.2, step 6), so the checks also run over a stream; `validateBody` feeds its whole span to one stream, and both forms run the same checks in the same order:

```cpp
using ContainerCheck = ASCIILiteral;    // the failing check's name, such as "container.key"

struct SectionExtent { uint64_t offset; uint64_t size; };   // both with a defaulted operator==
struct BodyLayout {
    uint64_t version;                   // the commit identifier
    uint8_t highestTier;
    uint32_t llintThreshold;            // L
    uint32_t counterProgress;           // P
    std::array<std::optional<SectionExtent>, numberOfSectionKinds> sections;   // by SectionKind; nullopt when absent
};

std::expected<BodyLayout, ContainerCheck> validateBody(std::span<const uint8_t> bytes, const BodyKey& expectedKey,
    std::span<const uint8_t, 16> headerDigest, ValidationMode);

class BodyValidationStream {   // keeps the envelope and directory (at most 128 + 64 * 24 bytes) and a CRC per section; allocates nothing
    WTF_MAKE_NONCOPYABLE(BodyValidationStream);
public:
    BodyValidationStream(const BodyKey& expectedKey, std::span<const uint8_t, 16> headerDigest, ValidationMode, uint64_t fileSize);   // fileSize: S
    void append(std::span<const uint8_t>);           // the body's bytes in order; container.size fails when their count differs from S
    std::expected<BodyLayout, ContainerCheck> finish();   // after the last byte; names the first check that failed

private:
    enum class Stage : uint8_t { Envelope, Directory, Sections };   // keeps the envelope, then the directory, then CRCs only
    void didReceiveEnvelope();
    void didReceiveDirectory();
    void appendSectionBytes(std::span<const uint8_t>);
    std::span<const uint8_t> directoryBytes() const LIFETIME_BOUND;

    const BodyKey m_expectedKey;
    const HeaderDigest m_headerDigest;
    const ValidationMode m_mode;
    const uint64_t m_fileSize;
    uint64_t m_received { 0 };                       // the bytes appended so far, those past fileSize included
    uint64_t m_framingEnd { bodyEnvelopeBytes };     // where the bytes the stage keeps end: 128, then 128 + 24 * N
    Stage m_stage { Stage::Envelope };
    std::optional<ContainerCheck> m_failure;         // a failed check no later byte can precede; only container.size can still come first
    bool m_requiredSectionsDiffer { false };         // container.required failed (Full); only the directory's padding can still fail before it
    bool m_paddingIsZero { true };                   // every padding byte so far was zero (Full)
    BodyEnvelope m_envelope;                         // decoded once the size, envelope, key, header-digest and version checks passed
    std::array<uint8_t, maximumBodyFramingBytes> m_framing { };         // the envelope and the directory, as they arrived
    std::array<uint32_t, maximumBodySections> m_sectionCRCStates { };   // each directory entry's running crc32cExtend state
};
```

With `Integrity`, the layout records the first directory entry of each section kind and ignores an entry whose (type id, tier) names no kind; the writer produces neither case, and normal mode trusts it not to (section 7.4). B8 still checks every entry's CRC in both modes. `BodyLayout` records each present section's offset and size, so `ValidatedBody::section` returns a span without searching.

One read checks part of a body. `validateBodyFraming(bytes, expectedKey, headerDigest, mode, checksummedKinds)` runs B1 to B7 on the envelope and the directory and B8 on the given kinds alone, and reads no other byte, so B6's padding check is left to a full validation; the scoring read uses it (section 7.3).

### 4.6 The pack

`cache/pack` holds every body in one log-structured file. It is only ever appended to, apart from two words of its prefix, the retired word and the root, and a reader reads only bytes below a valid end that a root of the same file gave it. `JITCacheContainer.h` holds every offset, as comments and constants, and the encoders and decoders named below.

The prefix, 64 bytes (`packPrefixBytes`) at offset 0:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCPACK` |
| 8 | 2 | layout version, 2 |
| 10 | 2 | prefix size, 64 |
| 12 | 4 | 0 |
| 16 | 16 | the header digest (section 3.3) |
| 32 | 8 | the pack identifier: nonzero, drawn with `cryptographicallyRandomValues` when the file is created |
| 40 | 4 | CRC32C of bytes [0, 40) |
| 44 | 4 | 0 |
| 48 | 8 | retired: 0, or 1 once a compaction or a whole deletion is about to put another file at the pack's name (section 8.4) |
| 56 | 8 | the root: the valid end, the first byte past the committed data |

The CRC covers neither the retired word nor the root, since both change after the file is created. Each is an aligned 8-byte word that readers load whole (`static_assert`s in `JITCacheContainer.h`). The producer stores the root with one atomic release store after every byte below it is written (section 8.2, step 8), and readers load it with acquire (section 5.2), so an aligned word is never seen half-written, by a concurrent reader or after a crash. Within a file the root only grows, so it also tells a reader that something was committed. ([history](SPEC-integrator-history.md#the-root-is-one-word-without-a-checksum))

`encodePackPrefix(prefix, validEnd)` writes a new file's prefix, retired 0 and `validEnd` in the root. `decodePackPrefix(prefix, headerDigest)` checks, in this order: at least the first 48 bytes; the tag; the layout version; the CRC; the prefix size; the zero bytes; a nonzero identifier; and a header digest equal to the opened artifact's. The version comes right after the tag because every layout keeps the tag and the version in its first 10 bytes while the CRC's place depends on the layout. A pack whose tag matches and whose version differs is of another layout, which `isOtherPackLayout(prefix)` names: a reader rejects it like any other prefix, and maintenance reports it as incompatible rather than damaged (maintenance sub-SPEC section 6). ([history](SPEC-integrator-history.md#the-packs-layout-version-is-read-before-its-crc))

Records and index blocks follow from offset 64, each at a multiple of 8 (`packAlignment`), with zeros between. A record holds one version of one body:

| offset | size | field |
|---|---|---|
| 0 | 40 | the body key |
| 40 | 8 | S, the body's size |
| 48 | 8 | the body's commit identifier |
| 56 | 4 | CRC32C of bytes [0, 56) |
| 60 | 4 | 0 |
| 64 | S | the body (sections 4.1 to 4.3), then zeros to the next multiple of 8 |

A record takes `packRecordExtent(S)` bytes, 64 + S rounded up to 8. The body starts 64 bytes past an 8-aligned offset, so in a page-aligned mapping every section starts at an 8-aligned address, which the lanes rely on (`ValidatedBody::section`). `decodePackRecordHeader` checks the CRC, the zero bytes and that `BodyKey::fromBytes` accepts the key.

An index block is a 32-byte header, N entries of 80 bytes and an 8-byte trailer, `packIndexBlockBytes(N)` = 32 + 80·N + 8 bytes:

| offset | size | field |
|---|---|---|
| 0 | 8 | the bytes `JITCINDX` |
| 8 | 8 | the previous block's offset: 0 for a full index; for a delta, the block that ended the valid bytes before this one, a full index or a delta |
| 16 | 4 | kind: 0 full, 1 delta |
| 20 | 4 | N |
| 24 | 4 | CRC32C of bytes [0, 24) followed by the N entries |
| 28 | 4 | 0 |
| 32 | 80·N | the entries |
| 32 + 80·N | 8 | the trailer: the block's own offset |

An index entry:

| offset | size | field |
|---|---|---|
| 0 | 40 | the body key |
| 40 | 8 | the record's offset, or 0 for a tombstone: the key has no body from this entry on |
| 48 | 8 | S, 0 for a tombstone |
| 56 | 8 | the commit identifier, 0 for a tombstone |
| 64 | 4 | L, as in the envelope |
| 68 | 4 | P, as in the envelope |
| 72 | 1 | the body's highest tier, 0 for a tombstone |
| 73 | 7 | 0 |

A full index lists every live key once, in strictly increasing byte order of the key, and no tombstone. A delta lists changes in the order they apply, the last entry for a key winning. The index a root names is the last full index followed by the deltas appended since (section 6.2). The index repeats L, P and the tier so that compact plans without reading a body (THREAD Maintenance), and in `Full` mode a read requires an entry to repeat its body's envelope: commit identifier, tier, L and P (`packEntryRepeatsEnvelope`).

The valid bytes always end with an index block, and the trailer in the 8 bytes before the valid end gives that block's start, so the root alone leads a reader to the latest index. The block CRC does not cover the trailer, which section 6.2's structural checks verify instead. `sealPackIndexBlock(block, offset, bytes)` writes the header and the trailer of a block whose entries are in place. `decodePackIndexBlockHeader` checks the tag, the kind, the zero bytes, that a block names a previous block exactly when it is a delta, and that the previous offset is a multiple of 8; `checkPackIndexBlockCRC` checks the CRC over the entries; `decodePackIndexEntry` checks the key and the zero bytes, and then for a tombstone a zero size, identifier, L, P and tier, and for a body a record offset of at least 64 that is a multiple of 8 and a nonzero identifier.

A new pack is the prefix followed by an empty full index at 64, so its valid end is 104 and its trailer holds 64 (`createEmptyPack`, section 1.3).

Every check of this section, and of the walk in section 6.2, fails `container.pack` (`ContainerChecks::pack`) with a `PackRejection`, the rule the bytes break, as its detail.

## 5. Artifacts in a process

### 5.1 The registry

`ArtifactRegistry` is a process-wide object (`LazyNeverDestroyed`) with a `Lock m_lock` and a map from the `cache/` directory's `(st_dev, st_ino)` to `ThreadSafeWeakPtr<OpenedArtifact>`. `openArtifact` goes through it once the role's own steps have passed (section 1.5): a producing role holds the producer lock, a Producer has created the artifact (section 1.3), and every role has opened `cache/` and read and checked `header` through that descriptor. The registry is keyed by that descriptor's `fstat`, so an object the VM finds pinned the directory whose header the VM checked. `take` returns the live object registered under the key or, when there is none, builds one (section 5.2) and registers it; a dead weak entry is replaced the same way. `take` digests the header bytes the VM read or wrote, makes the live object follow the root (section 6.3), and returns it only when its header digest equals that digest and it is not gone. The follow tells an object that has not read since its pack was retired whether the path still holds a pack before it is shared. An object whose `cache/` was removed finds none and goes gone, so a new `cache/` that receives the old inode number gets a new object. Otherwise `take` builds a new object and registers it in place of the old one, whose holders keep it and read the pack they pinned (section 5.2). `cache/` keeps its inode when its header changes under a live object, as when a Producer reuses the remnant a whole deletion left (section 1.3) or a host renames another artifact's files into it, and the app record makes the header depend on the config ([history](SPEC-integrator-history.md#the-registry-compares-headers-in-every-build)). `ArtifactStore.h` declares:

```cpp
class ArtifactRegistry {
    WTF_MAKE_NONCOPYABLE(ArtifactRegistry);
public:
    // The object for the directory cacheFd names; builds and registers one when no VM of the process holds it. A failure
    // is the build's: an errno, or the reason the pack fails container.pack.
    static std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);

private:
    friend class LazyNeverDestroyed<ArtifactRegistry>;
    ArtifactRegistry() = default;
    static ArtifactRegistry& singleton();

    using DirectoryIdentity = std::pair<uint64_t, uint64_t>;   // the cache/ directory's (st_dev, st_ino)
    Lock m_lock;
    HashMap<DirectoryIdentity, ThreadSafeWeakPtr<OpenedArtifact>> m_artifacts WTF_GUARDED_BY_LOCK(m_lock);
};
```

`take` holds `m_lock` only to look the key up and to register a new object, and builds an object outside it, so the registry lock is a leaf. When two VMs of the process build an object for one directory at once, the second to register takes the first's and destroys its own.

An object is built and follows the root the same way whatever the roles of the VMs that hold it, so no VM's role decides what another VM of the process sees (section 6.3). ([history](SPEC-integrator-history.md#one-opened-artifact-per-process))

The registry holds no producer lock: each producing `VMState` owns its `ProducerLock` (section 2), so the lock follows the VM and the object follows the VMs that hold it.

### 5.2 An opened artifact and its mapping

`OpenedArtifact` (`ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr`) holds:

- two directory descriptors of its own, `parentFd` (`O_PATH | O_DIRECTORY`) and `cacheFd` (`O_RDONLY | O_DIRECTORY`), each opened as `openAt(fd, ".")` from the caller's, so the object outlives the descriptors of the VM that built it. Every later open of `cache/pack` goes through `cacheFd`, so a VM keeps using the directory whose header it validated and never reads a pack under a header it did not check (THREAD Storage). A `cache/` that a replacement (section 1.4) or anything else moves or deletes leaves the object reading the pack it mapped, which stays valid; maintenance retires each pack before it unlinks or replaces it, so the object then misses every key or follows the new pack (section 6.3);
- the header digest;
- under `Lock m_indexLock`: the index (section 6.1); the current mapping; the valid end the index follows and the block that ends it; the live bytes; the deltas applied since the last full index; the damage that makes every read `Invalid` once set; whether the path holds no pack any more (gone); when a read first met the mapped pack's retired mark and when it may look again (section 6.3); and the statistics.

`ArtifactStore.h` declares it with the members sections 6 to 8 use:

```cpp
// Why the artifact could not be read: the errno of the call that failed, or, with error 0, the reason its pack fails
// container.pack, and whether that pack is of another layout (isOtherPackLayout).
struct ArtifactReadFailure {
    int error { 0 };
    PackRejection packRejection;
    bool otherLayout { false };
};

// The process's totals for one object, which every flush of a bench report writes as an index line. The times are
// MonotonicTime wall time.
struct IndexStatistics {
    uint64_t bodies { 0 };              // the keys the index lists now
    uint64_t packBytes { 0 };           // the valid end the index follows now
    uint64_t liveBytes { 0 };           // packLiveBytes of the index now
    uint64_t tableBytes { 0 };          // the index table's buckets times their size
    uint64_t buildNanoseconds { 0 };    // the first reading of the pack: its mapping and its whole index
    uint64_t refreshes { 0 };           // reads that found the root moved and applied it, reopens of a retired pack included
    uint64_t refreshNanoseconds { 0 };
    uint64_t appliedBlocks { 0 };       // index blocks the build and the refreshes applied
    uint64_t reopens { 0 };             // new mappings: of a retired pack's successor, or of a pack that outgrew its mapping
    uint64_t failedReopens { 0 };       // attempts that kept the mapping they had
    uint64_t opens { 0 };               // reads that reached a body, the scoring reads included
};

class OpenedArtifact final : public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<OpenedArtifact> {
    WTF_MAKE_NONCOPYABLE(OpenedArtifact);
    WTF_MAKE_TZONE_ALLOCATED(OpenedArtifact);
public:
    ~OpenedArtifact();   // drops the mapping and closes the descriptors

    // The reads; any thread that holds no JITCache lock.
    uint64_t token(const BodyKey&);   // the commit identifier of the key's newest body, 0 when it has none; no system call
    BodyOpen open(const BodyKey&, ValidationMode);   // borrows the body from the mapping and validates it
    SavedSummaryRead readSavedSummaries(const BodyKey&, ValidationMode);
    SectionRead readSection(const BodyKey&, SectionKind, ValidationMode);   // one section, checksummed without the others
    bool containsKey(const BodyKey&);   // token(key) != 0
    uint64_t indexedBodies();   // without following the root: Progress::indexedBodies and the start event
    IndexStatistics statistics();   // without following the root: the bench report's index line

    std::span<const uint8_t, 16> headerDigest() const LIFETIME_BOUND;
    int cacheFd() const;   // the writer's temporaries and its descriptor of the pack

private:
    friend class ArtifactRegistry;   // builds and registers objects
    friend class ArtifactWriter;     // reads the valid end, the latest block and the index under m_indexLock, and follows its own commits
    static std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes);
    OpenedArtifact(int parentFd, int cacheFd, const std::array<uint8_t, 16>& headerDigest);

    std::optional<StoreFailure> follow() WTF_REQUIRES_LOCK(m_indexLock);   // follows the root; the damage that makes every read Invalid, which stays once set
    std::optional<ArtifactReadFailure> reopen(uint64_t minimumCapacity) WTF_REQUIRES_LOCK(m_indexLock);   // maps cache/pack afresh and reads its whole index; on failure keeps what the object had
    std::optional<StoreFailure> applyValidEnd(uint64_t) WTF_REQUIRES_LOCK(m_indexLock);   // applies the index blocks up to a larger valid end

    const int m_parentFd;   // O_PATH | O_DIRECTORY
    const int m_cacheFd;
    const std::array<uint8_t, 16> m_headerDigest;
    Lock m_indexLock;
    PackIndex m_index WTF_GUARDED_BY_LOCK(m_indexLock);
    RefPtr<MappedPack> m_pack WTF_GUARDED_BY_LOCK(m_indexLock);
    uint64_t m_validEnd WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };      // the root the index follows
    uint64_t m_latestBlock WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };   // the index block that ends those valid bytes
    uint64_t m_liveBytes WTF_GUARDED_BY_LOCK(m_indexLock) { packPrefixBytes };
    uint64_t m_deltasSinceFull WTF_GUARDED_BY_LOCK(m_indexLock) { 0 };
    std::optional<StoreFailure> m_damage WTF_GUARDED_BY_LOCK(m_indexLock);
    bool m_gone WTF_GUARDED_BY_LOCK(m_indexLock) { false };           // the retired pack's path holds no pack: every key misses
    MonotonicTime m_nextRetiredReopen WTF_GUARDED_BY_LOCK(m_indexLock);
    MonotonicTime m_retiredSince WTF_GUARDED_BY_LOCK(m_indexLock);   // when a read first met the retired mark with the path unchanged
    IndexStatistics m_statistics WTF_GUARDED_BY_LOCK(m_indexLock);
};
```

Everything outside `m_indexLock` is immutable once the object is built. `create` pins the descriptors and then, under `m_indexLock`, calls `reopen(0)`, which maps the pack and reads its whole index (section 6.2); `buildNanoseconds` times it. This happens at `start`, the demand a role creates, so no mapping and no whole-index read lands inside a request point. A failure returns the `ArtifactReadFailure`, and the object is destroyed unregistered; `openArtifact` turns an errno into `IO` and a pack rejection into `DamagedPack` (section 1.5). The object is destroyed with the last `VMState` that holds it.

`MappedPack` (`ThreadSafeRefCounted`) is one `MAP_SHARED`, `PROT_READ` mapping of one pack file whose prefix passed its checks. `MappedPack::map(cacheFd, headerDigest, minimumCapacity) -> std::expected<PackMapping, ArtifactReadFailure>`, where `PackMapping` is `{ Ref<MappedPack> pack; uint64_t validEnd; }`:

1. `openAt(cacheFd, "pack", O_RDONLY | O_NONBLOCK)`, which fails with `ENOENT` when the directory holds no pack and opens a FIFO without waiting for a writer, and `fstat` for its type, size and inode. A file that is not a regular file is rejected as damage, "the name holds no regular file", which `start` reports at `start.pack` and maintenance as `damaged-pack`.
2. A file shorter than 64 bytes is rejected. `pread` the first 48 bytes and run `decodePackPrefix` (section 4.6); a rejection sets `otherLayout` from `isOtherPackLayout`.
3. Map `capacity` bytes, the page-rounded sum of `wanted` and the larger of `wanted` and `packMappingSlackBytes`, where `wanted` is the larger of the file's size and `minimumCapacity`: `mmap(nullptr, capacity, PROT_READ, MAP_SHARED, fd, 0)`. A `wanted` above a quarter of 2^64, or a capacity a `size_t` cannot hold, is `EFBIG`. The file grows into the room past its end without a new mapping, and nothing reads the mapped pages past the file's end, which fault.
4. Load the root (`validEnd()`). When it passes the size step 1 took, `fstat` the same descriptor again, since the producer extends the file before it stores a root. A root still past the file is the rejection "the root's valid end passes the file", so no read runs past the file's end.
5. Close the descriptor and return the mapping with that root.

`validEnd()` and `isRetired()` load the root and the retired word with `__atomic_load_n(..., __ATOMIC_ACQUIRE)`, so the bytes below a loaded valid end are visible to the loading thread. `bytes(offset, size)` returns a span of the mapping, which a release assertion bounds by the capacity; every caller bounds it by a valid end first. `inode()` and `identifier()` return what step 1 and the prefix gave.

The opened artifact holds its current mapping, and every `ValidatedBody` and `SavedSummaries` taken from a mapping holds a reference to it, so the last of them unmaps it. A pack that a compaction replaced or a deletion unlinked stays readable through every mapping of it: imports already made keep reading their bytes, which never change in place, while new reads follow the current pack (THREAD Storage). A mapping holds page-cache pages rather than process memory, and is not charged to the producer budget (SPEC-integrator.md section 4.4).

The object's reads (section 7) return results and raise nothing, and know no VM: the `VMState` turns their outcomes into faults and counters (SPEC-integrator.md section 6.2), the capture glue turns the scoring read's (SPEC-integrator.md section 8.3), and the store's tests read them directly (section 9).

## 6. The index

### 6.1 Entries and tokens

The index is `PackIndex`, a `HashMap<BodyKey, PackIndexEntry, BodyKeyHash, BodyKeyHashTraits>` that holds each live key's newest entry (section 4.6). Its hash reads the first four bytes of the key's identity digest, which SHA-256 makes uniform, mixed with its kind, specialization and mode bytes. An all-zero key, which `fromBytes` never accepts, is the empty value, and an all-`0xFF` key the deleted value. `ArtifactStore.h` declares the two, which every integrator map keyed by a body key uses (the index, the kept summaries and `delta`'s candidate table):

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

`BodyKey` keeps its bytes private and its `make` and `fromBytes` reject both values, but it is trivially copyable and exactly `BodyKey::byteSize` bytes, which a `static_assert` beside the traits checks, so both values are byte fills that need nothing from the UCB lane. Keying by the whole key makes the index exact and needs no second hash: a key names one record, and a record whose header or envelope names another key is damage (section 7.2), never a collision.

The token `bodyVersion` returns (SPEC-integrator.md section 6.2) is the commit identifier of the key's entry, 0 when the index lists no body for the key. A token changes exactly when the index applies another version of the key. A compaction moves records but keeps their commit identifiers, so it changes no token and does not send the UCB lane back to bodies it already found not to fit (SPEC-ucb.md R-INT-3 and section 7.3.2). ([history](SPEC-integrator-history.md#tokens-are-commit-identifiers))

`packLiveBytes(index)` is the prefix's 64 bytes plus the extent of every record the index lists; the rest of the valid bytes, superseded and tombstoned records and every index block, are dead. The object keeps its live bytes current as it applies blocks.

### 6.2 Reading the index

`readPackIndex(validEnd, stopAt, index, liveBytes, readAt) -> std::expected<PackIndexRead, PackRejection>` (`ArtifactStore.h`) reads the index a root names and applies it to `index`. `readAt(offset, size)` returns the pack's bytes, which the walk bounds by `validEnd` before it asks. It rejects:

1. a valid end that is not a multiple of 8 or is below 104, which no pack has;
2. in the walk back from the block that the trailer at `validEnd - 8` gives, through each delta's previous block, until a full index or the block at `stopAt`: a block below 64, misaligned, or not below the block after it; a latest block that does not end exactly at the valid end, or an earlier one that ends past the start of the block after it; a block whose header fails its checks or whose trailer does not give its own start; and a delta whose previous block does not precede it. The block at `stopAt` is checked before the walk stops there, so a valid end that moved without a block of its own fails. Offsets strictly decrease along the chain, which bounds the walk, and no trailer sends a read outside [64, valid end);
3. then, from the oldest walked block to the latest: a block's CRC; an entry's own checks; in a full index, a tombstone or keys that do not strictly increase; and a body entry whose record does not end at or before its block's start.

A walk that reached a full index first clears `index` and resets the live bytes. Each entry then replaces the key's earlier one, and a tombstone removes it. `PackIndexRead` gives `latestBlock`, where the next read of a longer valid end stops, the blocks applied, the deltas among them, and whether the walk reached a full index. A rejection leaves `index` unspecified, which is why a rejection makes the object damaged for good (section 6.3).

The writer bounds the walk: once `packDeltaChainBound` deltas would follow the last full index, a commit writes a full index instead of a delta (section 8.2, step 7), so a reader that opens the pack applies at most `packDeltaChainBound` blocks, the full index included. Only a tombstone, which is always a delta, can lengthen the chain past that, and production writes none. SPEC-integrator.md section 16 lists the values of `packDeltaChainBound` and the other parameters of `JITCacheParameters.h`.

### 6.3 Following the root

Every read of the object (`token`, `open`, `readSavedSummaries` and `readSection`) first calls `follow()` under `m_indexLock`, and so do the writer's open of the pack, its publish and its compaction (section 8). Every object follows the root whatever the roles of the VMs that hold it ([history](SPEC-integrator-history.md#readers-follow-the-packs-root)):

1. A damaged object returns its damage, which every later `open`, `readSavedSummaries` and `readSection` answers as `Invalid`. A gone object does nothing.
2. When the mapped pack carries the retired mark, a compaction, a whole deletion, the compact that finishes a remnant or the creation that reuses one is putting another file at its name, or none. `follow` takes `fstatat(cacheFd, "pack")`. `ENOENT` marks the object gone and clears its index, so every key misses from then on. Another inode makes it `reopen(0)`, which maps the new file and reads its whole index; a pack rejection there is damage, except `packOfAnotherHeader`, the rejection of a prefix that passes every other check, the CRC over the digest included, and names another header's digest: that file belongs to an artifact created at the path after the one the object checked was deleted, so it marks the object gone as `ENOENT` does ([history](SPEC-integrator-history.md#every-pack-is-retired-before-it-goes)). The same inode, a failed `fstatat` or a reopen whose system call failed means that the rename has not happened yet, or that a compaction died between its mark and its rename: the mapped pack stays current, every read looks again during the first second after a read first met the mark, and after that at most once per `retiredPackReopenIntervalMilliseconds`. The next producer clears an abandoned mark before it appends (section 8.1), and so does `compact` (maintenance sub-SPEC section 3).
3. Load the root. An equal valid end does nothing, which is the whole cost of a read while nothing changes: one acquire load from a shared page. A smaller one is damage ("the root's valid end moved back"), since nothing lowers the root within a file; compactions and deletions write a new file.
4. A larger valid end goes to `applyValidEnd`:
   - Past the mapping's capacity, `reopen(validEnd)` maps the file again with room for that root and reads the whole index. `reopen` maps again while the loaded root passes the new mapping's capacity; each mapping has room for twice the root that asked for it, so only a pack that doubles in the meantime makes it go round again. Bodies borrowed from the old mapping keep it. A reopen whose system call fails keeps the object as it was, counts a failed reopen and is tried again at the next read; a pack rejection is damage.
   - Otherwise `fstatat(cacheFd, "pack")`. While the path names the mapped inode, a root past the file's size is damage ("the root's valid end passes the file"): the producer extends the file before it stores a root, so only a file shrunk under the mapping, such as a pack rewritten in place, gives one, and reading it would fault (section 7.4). ([history](SPEC-integrator-history.md#a-moved-root-is-checked-against-the-files-size))
   - Then `readPackIndex(validEnd, latestBlock, ...)` applies the blocks appended since, in place, and stops at the block the object applied last. A rejection is damage, so the half-applied index is never read again.

Damage is `StoreFailure { "container.pack", 0, rejection }` and stays for the object's life; a VM that starts once the object's last holder is gone builds a new object, which reads the pack afresh. `reopen` succeeds only as a whole: it replaces the mapping, the valid end, the latest block, the index and the live bytes together, and clears the damage, the gone mark and the retired timing.

A VM therefore sees a commit by another process at its first read after that commit's root store, and a commit by a VM of its own process from the store on, since the commit follows the root under the same lock (section 8.2, step 9). That is the cadence SPEC-integrator.md II16 states and SPEC-ucb.md section 7.3.2 counts on.

### 6.4 Lookup and the lock

`m_indexLock` guards everything the object changes (section 5.2). It is a leaf: nothing under it takes a JSC or JITCache lock. Plain memory work and `fastMalloc` run under it, since `reopen` builds a whole index there. The only system calls made under it are section 6.3's `fstatat`s and a reopen's calls: `openat`, `fstat`, `pread`, `mmap`, `close`, and the `munmap` of a mapping whose last reference the object dropped. It is never held while a body is validated, or while any lane, JSC or the collector is called: `open` and `readSavedSummaries` copy the entry and a reference to the mapping under it and validate outside it. `indexedBodies()` and `statistics()` take it without following the root. The writer takes it to read the valid end, the latest block and the index, and to follow its own commits (section 8).

## 7. Opening bodies

`ArtifactStore.h` declares the opened artifact's reads with the class (section 5.2): `token` (section 7.1), `open` (section 7.2), `readSavedSummaries` (section 7.3), `readSection` (section 7.6) and `containsKey`, which is `token(key) != 0`. `token` reads the index alone. The three others read a body through the current root, check it in the mode their caller gives, `ValidationMode::Full` exactly when the caller's VM runs strict (section 4.5), and return one of four outcomes:

```cpp
enum class StoreOutcome : uint8_t { Absent, Unavailable, Invalid, Found };
struct StoreFailure {                  // Invalid: container.io with its errno, a body check, or container.pack
    ASCIILiteral check;
    int error { 0 };
    PackRejection packRejection { };   // container.pack: the rule the bytes break
};

struct BodyOpen {
    StoreOutcome outcome;
    RefPtr<ValidatedBody> body;        // Found only
    StoreFailure failure;
    uint64_t bodyBytes { 0 };          // S of the record the read reached: Found, or Invalid at the record or a body check; 0 otherwise
};

class SavedSummaries {   // one body's three summary sections, checked; holds the pack's mapping
    WTF_MAKE_NONCOPYABLE(SavedSummaries);
    WTF_MAKE_TZONE_ALLOCATED(SavedSummaries);
public:
    uint64_t version() const;                    // the envelope's commit identifier
    uint8_t highestTier() const;
    std::span<const uint8_t> ucbFeedback() const;
    std::span<const uint8_t> cbSummary() const;
    std::span<const uint8_t> ics() const;        // the whole ICsBaseline section
    ~SavedSummaries();                           // drops its reference to the mapping

private:
    friend class OpenedArtifact;                 // readSavedSummaries builds it from bytes whose checks passed
    SavedSummaries(Ref<MappedPack>&&, uint64_t version, uint8_t highestTier, std::span<const uint8_t> ucbFeedback,
        std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics);

    const Ref<MappedPack> m_pack;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const std::span<const uint8_t> m_ucbFeedback;
    const std::span<const uint8_t> m_cbSummary;
    const std::span<const uint8_t> m_ics;
};
struct SavedSummaryRead { StoreOutcome outcome; std::unique_ptr<SavedSummaries> summaries; StoreFailure failure; };   // summaries: Found only

struct SectionRead {
    StoreOutcome outcome;
    RefPtr<MappedPack> pack;           // Found: keeps the section's bytes mapped
    std::span<const uint8_t> section;  // Found: the section, empty when the body lacks it
    StoreFailure failure;
};
```

| outcome | `open`, `readSavedSummaries` and `readSection` |
|---|---|
| `Absent` | the index lists no body for the key, or the object is gone (section 6.3); no system call |
| `Unavailable` | only the fault hook of test builds, with `EMFILE`, `ENFILE` or `ENOMEM` (section 7.5) |
| `Invalid` | the object is damaged (section 6.3); the record fails its header checks or differs from its entry; the body fails the first check of section 4.5 that the read runs; in `Full` mode, the entry does not repeat the envelope; or the fault hook's other errno, at `container.io` |
| `Found` | the read succeeded |

The `VMState`'s mapping of each outcome is in SPEC-integrator.md section 6.2.

### 7.1 Reading a token

`token` follows the root (section 6.3) and returns the commit identifier of the key's entry, or 0 when the index lists none or the object is gone. It ignores damage, which the next `open` reports. It opens no file and reads no record (SPEC-ucb.md R-INT-3), and while the root stays it costs one lock and one atomic load.

### 7.2 Borrowing and checking a body

`open(key, mode)`:

1. Under `m_indexLock`, follow the root; damage answers `Invalid` with it. A key the index lacks, or a gone object, answers `Absent`. Otherwise copy the entry, take a reference to the current mapping and count the open (`IndexStatistics::opens`).
2. Outside the lock, read the record. `decodePackRecordHeader` checks the 64 bytes at the entry's offset, which must repeat the entry's key, S and commit identifier ("a record differs from the index entry that names it"); both failures are `container.pack`. `validateBody` then runs over the S bytes after the header, in the read's mode, with the entry's key as `expectedKey` and the opened artifact's header digest. In `Full` mode the entry must also repeat the envelope (`packEntryRepeatsEnvelope`, `container.pack`).
3. `Found` builds the `ValidatedBody` through its private constructor, from the reference to the mapping, the body's span and a span per section that the `BodyLayout` gives, with the envelope's version, tier, L and P. `bodyBytes` is S.

The body borrows its bytes from the mapping and copies nothing. Its reference keeps the mapping alive, so the bytes stay readable and unchanged after the pack is compacted, replaced or unlinked (section 5.2), and a pending import pins the body (SPEC-ucb.md section 6.4). ([history](SPEC-integrator-history.md#bodies-are-borrowed-from-the-packs-mapping))

### 7.3 Reading a saved score

The capture glue reads a saved body's score through `readSavedSummaries(key, mode)` (SPEC-integrator.md section 8.3), which only a producing VM calls. Its `Unavailable` defers the key instead of counting as no saved body, and it checks only what a score reads. ([history](SPEC-integrator-history.md#the-scoring-read-is-the-glues-own)) It finds the body through the current root, as `open` does: a body the root does not cover is not committed, so `Absent` means that no body exists for the key. ([history](SPEC-integrator-history.md#the-scoring-read-follows-the-root))

1. As `open`'s step 1.
2. As `open`'s step 2, with `validateBodyFraming` in place of `validateBody`: B1 to B7 on the envelope and the directory, and B8 on `ucb.feedback`, `cb.summary` and `ICsBaseline`, the three sections a score reads, in the caller's mode. The other sections and the padding are left unread; an import of the body checks them all (section 7.2).
3. `Found` carries the reference to the mapping, the envelope's tier and commit identifier, and the three spans.

The glue's handling of each outcome is in SPEC-integrator.md section 8.3, and the mapping's exemption from the producer budget in its section 4.4.

### 7.4 Trust

The checksums reject damaged material; neither mode defends against a writer of the artifact directory, who could as well plant valid code. Only the producer, maintenance and the human write it. JITCache changes a pack in place only at its root and its retired word, truncates it only past its valid end and under the producer lock (maintenance sub-SPEC section 3), and replaces it only by renaming a new file over it. A host that writes a pack over the old one in place, as `cp` or `rsync --inplace` do, shrinks or rewrites the file under running consumers. A root that then lies past the shrunk file is damage (section 6.3), but a body or summary borrowed before the shrink faults with `SIGBUS` when it reads a page past the new end, as with any mapped file.

### 7.5 Twins-build fault hook

Only `ENABLE(JITCACHE_TWINS)` builds compile it, in `ArtifactStore.cpp` behind the `StoreTesting` interface `ArtifactStore.h` declares, for the tests of SPEC-integrator.md section 15 and test C6. A hook `{ call, error, n }` makes the n-th call, or every call when n is absent, of `open` (`Open`), `readSavedSummaries` (`ReadSavedSummaries`) or `MappedPack::map` (`PackMap`) fail at its start with that error, as a failed system call would. A read answers `ENOENT` as `Absent`, `EMFILE`, `ENFILE` and `ENOMEM` as `Unavailable`, and any other errno as `Invalid` at `container.io`; a failed map fails the build or the reopen that called it (sections 5.2 and 6.3). `token` makes no such call, so the hook never reaches it. The shell sets the first two with `--jitcache-test-store-fault=openBody|scoring:<errno>[@<n>]` (harness sub-SPEC section 5.1), and the tests through the interface, which also offers the registry hook that builds a new object at every `take` instead of sharing one, so a test holds two views of one artifact as two processes would:

```cpp
#if ENABLE(JITCACHE_TWINS)
namespace JSC::JITCache::StoreTesting {
enum class Call : uint8_t { Open, ReadSavedSummaries, PackMap };
struct Fault { Call call; int error; std::optional<uint64_t> n; };   // n counts calls of that kind from 1, from setFault; absent: every call
void setFault(std::optional<Fault>);   // process-wide atomics, any thread
void setRegistrySharing(bool);         // false: ArtifactRegistry::take builds a new OpenedArtifact at every call
}
#endif
```

### 7.6 Reading one section

`readSection(key, kind, mode)` serves SPEC-ucb.analysis.md R-INT-A2 through `VMState::readBodySection` (SPEC-integrator.md section 6.2): the module's request opens the whole body moments after Bun's provider reads its `ucb.analysis`, and a large bundle's core is megabytes, so this read checksums the one section it returns and leaves the rest to that open. It reads as `readSavedSummaries` does (section 7.3), with the one kind in place of the three:

1. As `open`'s step 1, counting the open in `IndexStatistics::opens`.
2. As `open`'s step 2, with `validateBodyFraming` in place of `validateBody`: B1 to B7 on the envelope and the directory, and B8 on `kind` alone, in the caller's mode; in `Full` mode the entry must also repeat the envelope.
3. `Found` carries the reference to the mapping and the section's span, empty when the body has no section of that kind.

It makes no call the fault hook of section 7.5 reaches, so it never answers `Unavailable`.

## 8. The writer

### 8.1 Resources

Each producing `VMState` owns one `ArtifactWriter` (SPEC-integrator.md section 6.3), which `start` creates over the shared `OpenedArtifact`, the `ProducerLock` the state holds and the state's budget, with a staging buffer of `writerStagingBytes` (SPEC-integrator.md section 16); the state destroys the writer before all three. The writer holds:

- the staging buffer, charged to the producer budget and allocated at the first commit and kept until production ends, when `releaseStagingBuffer` frees it and releases the charge;
- a descriptor of the file the opened artifact maps, opened `O_RDWR`, with that file's inode, and the file's first 64 bytes mapped `PROT_READ | PROT_WRITE`, `MAP_SHARED`, through which it stores the root. `closePack` unmaps and closes both, after every compaction and in the destructor.

`ensurePackOpen` gives a commit or a compaction that descriptor and mapping, and returns 0 or the errno of the call that failed:

1. Under `m_indexLock`, follow the root (section 6.3). A descriptor the writer holds for the inode the object maps is kept, and nothing else happens.
2. Otherwise `closePack`, `openAt(cacheFd, "pack", O_RDWR)` and `fstat`. When the object maps another file, which only an earlier compaction by this writer leaves, since only the lock's holder replaces the pack, the object reopens (section 6.3); a failed reopen is its errno, or `EIO` for a pack rejection.
3. A mapped pack that still carries the retired mark is one a compaction marked and died before renaming over: it is the current pack, so `pwrite` of 0 into the retired word clears the mark before anything is appended.
4. `mmap(nullptr, 64, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)`. The mapping starts at offset 0, so the root's offset keeps it 8-aligned.

Besides the lanes' own buffers, which they charged, a commit uses the staging buffer, the reread's `BodyValidationStream`, a fixed-size object on the stack that holds at most the envelope, the directory and a CRC per section and allocates nothing (section 4.5), and its index block's bytes. A delta's block takes 120 bytes; a full index's buffer is charged first (section 8.2, step 7). None of these grows with the body's size (THREAD Capture). A test constructs a writer with any staging size of at least one byte (test C7).

### 8.2 Commit

`ArtifactWriter::commit(stamp, sections, timing) -> std::expected<CommitResult, CommitFailure>` runs on the VM thread inside the capture (SPEC-integrator.md section 8.4), under the producer lock its VM holds. Every section is opaque bytes to it, as THREAD Storage has the container treat them: it lays the sources out, streams, checksums and counts their bytes, rereads the record, validates the container and publishes it, and parses no lane format. ([history](SPEC-integrator-history.md#the-writer-takes-section-sources-and-reads-no-lane-format))

1. Staging. At the first commit, charge `stagingBytes` and allocate the buffer. A refused charge is `budget.limit`, with nothing written.
2. Layout. Take each source's size; `layOutBodySections` computes N, the offsets of section 4.3 and S, and `packRecordExtent` the record's extent, with overflow-checked arithmetic. A layout that cannot be computed, because there is no source or the sizes overflow, is `writer.section`, with nothing written.
3. Open. `ensurePackOpen` (section 8.1); a failure is `writer.open`. The record goes at the valid end the object follows. A record whose end would pass half the largest `off_t` is `writer.write`.
4. Stream the sources in order to their offsets in the record, through the staging buffer, writing zeros between sections and after the last one up to the record's end, and computing each section's CRC as its bytes pass. A source in memory goes through the buffer, or straight to `pwrite` when larger than it. A streamed source's function receives a sink that appends to the buffer, flushing it to the file when full; the sink returns false once a write has failed or once the bytes would pass the source's size, and the source then stops and returns false (SPEC-image.md section 9). A write error is `writer.write`. A source whose count ends other than its size, or that returns false while its sink refused nothing, is `writer.section`, with the section's name (SPEC-integrator.md section 6.1) and both counts, the size a lane declared and the bytes it streamed, in the detail.
5. `pwrite` the directory at the body's offset 128 and the envelope at its offset 0, with a fresh commit identifier, the header digest, the stamp's key, tier, L and P, S and both CRCs, then the record header (section 4.6) at the record's offset. Failure: `writer.write`. A record that stops short of its envelope never validates, and none is read before the root covers it.
6. Reread. `pread` the body in chunks of the staging buffer and feed them to a `BodyValidationStream` in `Full` mode, whatever strict says, with the stamp's key and the artifact's header digest, and require exactly the planned `BodyLayout`. Any difference or read error is `writer.reread`. Each section's checksum in the directory was computed from the bytes step 4 streamed, so only now is the record known to hold the lanes' bytes.
7. Index block. Right after the record, `pwrite` one block naming it, whose previous block is the object's latest block. It is a delta of one entry, unless `packDeltaChainBound` deltas would then follow the last full index and the budget accepts the full index's buffer, (index size + 1) · (`sizeof(PackIndexEntry)` + 80) bytes, released when the commit ends: then it is a full index of every live key, the new entry in place of the key's old one, in key order. A refused charge writes a delta. Failure: `writer.write`.
8. Commit point. `__atomic_store_n(root, validEnd, __ATOMIC_RELEASE)` through the writable prefix mapping, where `validEnd` is the block's end. A `pwrite` need not store the word in one piece, while an aligned 8-byte store through a mapping does, so neither a concurrent reader nor a crash ever leaves half a root, and every byte below the new valid end is in the file before the store.
9. Under `m_indexLock`, the object follows the root (section 6.3), so every VM of the process sees the body from now on. Damage, or an object that does not reach the stored valid end, is `writer.publish`, a check that names only an object that cannot follow the root, since the store itself cannot fail. Return `CommitResult { version, fileSize }`, the commit identifier and S.

A failure before step 8 leaves bytes past the valid end, which no reader sees and the next commit overwrites, since it writes at the same valid end; `compact` truncates any it leaves (maintenance sub-SPEC section 3). A failure returns `CommitFailure { check, detail }`, whose detail is `errorText` of the error for a failed system call; the capture glue raises it as a recording fault (SPEC-integrator.md section 8.4). Harness sub-SPEC section 12 says what a producer killed during a commit leaves. No `fsync` is issued, since THREAD Capture excludes power-loss durability; a commit relies on the order of its writes in the page cache.

A commit given a `CommitTiming` (SPEC-integrator.md section 6.3) reads the thread CPU clock of harness sub-SPEC section 9.1 at the bounds of the parts it fills, and a commit given none reads no clock. A source's stream time runs from the end of the previous source's, or from the start of step 4 for the first source, to the end of its own bytes, its padding and the flushes it fills included, and the last source's also covers step 4's final padding and flush; the reread time covers step 6, and the publish time steps 7 to 9.

`remove(key)` appends a tombstone for a key the index lists and publishes it as steps 3 and 7 to 9 do; a tombstone is always a delta, and a key without a body is left alone. Production never removes a key; test C7 does.

### 8.3 Twins-build hooks

Only `ENABLE(JITCACHE_TWINS)` builds act on these, in `ArtifactWriter.cpp` behind a test interface `ArtifactWriter.h` declares, for test C7 and the tests of SPEC-integrator.md section 15.2:

- A fault injection, `{ check, n }` (`setFaultForTesting`): `writer.open`, `writer.write` or `writer.reread` fails the writer's n-th commit, and `compaction.create`, `compaction.write`, `compaction.reread` or `compaction.publish` its n-th compaction (section 8.4), as if the step's call had failed with `EIO`, after the real work of the earlier steps. `faultCheckNamed` accepts exactly these seven names. `writer.section` and `writer.publish` are not injectable: the first is a lane's broken size contract, and the root's store makes no system call.
- A kill point, `{ point, n }` (`setKillForTesting`): `raise(SIGKILL)` at the point of the n-th commit, or of the n-th compaction for the two compaction points, so no handler, destructor or exit hook runs and the process leaves exactly the bytes the point names. Every build marks the points; only test builds act. `name(point)` and `killPointNamed` give their names, and harness sub-SPEC section 12 says what each leaves:

| point | where |
|---|---|
| `before-append` | after section 8.2's step 3, before any byte of the record |
| `mid-stream` | after step 4's first write, or at its end when it wrote nothing |
| `after-stream` | after step 4 |
| `after-envelope` | after step 5 |
| `after-reread` | after step 6 |
| `after-index` | after step 7, the last point before the root's store |
| `after-root` | after step 8, the first point after the store, before the object follows it |
| `compaction-before-rename` | with the new pack complete and the old one marked retired (section 8.4, `compactPack`'s step 7) |
| `compaction-after-rename` | right after the rename |

- `rewriteSection(key, kind, offset, bytes) -> std::expected<CommitResult, CommitFailure>`: it opens the key's current body with the store's `open` in `Full` mode (section 7.2), copies every section the body holds, those its highest tier requires and `ucb.analysis` when a module's body has one, into one buffer, charged before it is allocated (SPEC-integrator.md section 4.4), overwrites `bytes.size()` bytes of `kind`'s section at `offset`, releases the body, and commits the copy's sections, each an in-memory source, under a stamp of the body's key, `highestTier()`, `llintThreshold()` and `counterProgress()` (SPEC-integrator.md section 6.2). The commit recomputes every checksum and draws a fresh commit identifier, so the lanes' validation sees the damage while the container sees a sound body. An open that does not answer `Found`, a `kind` the body does not hold, and a range that does not lie inside the section fail at `writer.rewrite`, a check only this function returns, with the store's outcome, and for `Invalid` its check, in the detail; a refused charge for the copy fails at `budget.limit`. Neither writes anything. The commit itself fails as `commit` does. It runs only in a producing VM, whose writer and lock it uses; the capture glue's `rewriteSectionForTesting` calls it (SPEC-integrator.md section 8.3).

### 8.4 Compaction

A compaction writes the live records and one full index into a new pack and renames it over `cache/pack`, which removes every dead byte (THREAD Storage). Two callers run it, both under the producer lock: the writer's `compactIfNeeded` at the end of every `delta` (SPEC-integrator.md section 8.6), and maintenance's `compact` (maintenance sub-SPEC section 4.5). Both go through `ArtifactWriter::compactPack`. `ArtifactWriter.h` declares:

```cpp
namespace CompactionChecks {   // each a storage failure
inline constexpr ASCIILiteral create = "compaction.create"_s;
inline constexpr ASCIILiteral write = "compaction.write"_s;
inline constexpr ASCIILiteral reread = "compaction.reread"_s;
inline constexpr ASCIILiteral publish = "compaction.publish"_s;   // the retired mark and the rename
}

struct CompactionResult {
    uint64_t bodies { 0 };              // in the new pack
    uint64_t packBytes { 0 };           // the new pack's valid end
    uint64_t replacedPackBytes { 0 };   // the valid end of the pack it replaced
    uint64_t nanoseconds { 0 };         // thread CPU time, when timed
    Vector<std::pair<BodyKey, ASCIILiteral>> droppedBodies;   // dropsDamagedBodies: each body left out and the check it failed
};
struct CompactionFailure {
    enum class Kind : uint8_t { Storage, Budget, Damaged };   // a compaction.* step; a refused charge; a body or the pack failed the check named
    Kind kind;
    ASCIILiteral check;
    String detail;
};
struct CompactionSource {               // as a reader built it
    const MappedPack& pack;
    uint64_t validEnd;
    std::span<const PackIndexEntry> entries;   // the live entries, in strictly increasing key order
    int writableFd;                     // the same file, opened for writing: it receives the retired mark
};
struct CompactionTarget {
    CompactionTarget(int cacheFd, std::span<const uint8_t, 16> headerDigest);
    int cacheFd;
    std::span<const uint8_t, 16> headerDigest;
    ProducerBudget* budget { nullptr };   // a producing VM charges the new index's buffer; maintenance passes none
    bool dropsDamagedBodies { false };    // maintenance: a body that fails its checks is left out instead of failing
    // Twins builds: faultForTesting, killsBeforeRenameForTesting, killsAfterRenameForTesting.
};

std::expected<std::optional<CompactionResult>, CompactionFailure> compactIfNeeded(bool timed);   // nothing when the pack stays
static std::expected<CompactionResult, CompactionFailure> compactPack(const CompactionTarget&, const CompactionSource&,
    const ScopedLambda<bool(const BodyKey&)>& excludes, bool timed);

// What a live body must pass before a compaction keeps it: its record header repeats its index entry, the body passes
// validateBody in Full mode under headerDigest, and the entry repeats the envelope. Nothing when it passes, otherwise
// the first check it failed. Reads the body through the mapping and allocates nothing.
static std::optional<ContainerCheck> checkPackedBody(const MappedPack&, const PackIndexEntry&, std::span<const uint8_t, 16> headerDigest);
```

`compactIfNeeded(timed)`:

1. Under `m_indexLock`, follow the root; damage fails with kind `Damaged` at the damage's check. A pack whose valid end is at most its live bytes plus a full index of its live keys, `packIndexBlockBytes(N)`, is dense, and the call returns nothing. Any other byte, a superseded version, a tombstone or a delta appended since the last compaction, makes it compact, so a delta that committed anything compacts and one that committed nothing leaves a dense pack alone. ([history](SPEC-integrator-history.md#every-delta-ends-with-a-dense-pack))
2. Charge the plan, N · `sizeof(PackIndexEntry)`; a refusal fails with kind `Budget` at `budget.limit`. Copy the live entries and sort them by key bytes.
3. `ensurePackOpen` (section 8.1); a failure is kind `Storage` at `compaction.create`. Then `compactPack` with no exclusions and the writer's budget.
4. `closePack`, since the writer's descriptor and mapping name the retired file; the next commit opens the new one. Clear the object's retired timing and follow the root, so the object maps the new pack at once. Return the result or the failure; the delta raises a `Damaged` failure as invalid material and any other as a recording fault (SPEC-integrator.md section 8.6).

`compactPack(target, source, excludes, timed)` writes the new pack in a pack temporary (section 1.1):

1. `openAt(cacheFd, temporary, O_RDWR | O_CREAT | O_EXCL, 0644)`. Failure: `compaction.create`.
2. With a budget, charge the new index's buffer, entries · `sizeof(PackIndexEntry)` + `packIndexBlockBytes(entries)`; a refusal is kind `Budget` at `budget.limit`.
3. For each entry `excludes` does not name, in the source's order: `checkPackedBody(source.pack, entry, target.headerDigest)`, so the new pack holds only what a strict reader accepts. A body that fails is left out with its check when the target drops damaged bodies, and otherwise fails the compaction with kind `Damaged` at that check, the key's hex in the detail. Copy the record, header and body, from the source mapping to the next offset from 64, with its padding. Failure: `compaction.write`.
4. `pwrite` a full index of the records written, with its trailer, after the last record.
5. `pwrite` the prefix, with a fresh identifier and the root holding the index's end, at offset 0. Every write here is a `pwrite`, since no reader sees the temporary before the rename. Failure of step 4 or 5: `compaction.write`.
6. Reread the prefix and the index and compare them byte for byte with what was written. Failure: `compaction.reread`.
7. A sequentially consistent fence, then `pwrite` of 1 into the source's retired word through `writableFd`. Only one byte of the word changes, so a partial write cannot be misread. Failure: `compaction.publish`.
8. `renameat(cacheFd, temporary, cacheFd, "pack")`. Failure: `compaction.publish`.

The temporary is unlinked whenever the rename did not happen. A failure before step 7 leaves the artifact as it was. A failure at step 8 leaves the old pack marked retired under its own name, which readers treat as a compaction that died before its rename (section 6.3) and the next commit or `compact` clears. The result counts the bodies written, the new and the replaced valid ends, the thread CPU time when `timed`, and the bodies dropped. A dense pack's size is its live records plus `packIndexBlockBytes` of its live count, the formula compact's rewrite test also uses (maintenance sub-SPEC section 4). Maintenance also runs `checkPackedBody` over every live body of a dense pack it would keep, so a damaged body never survives a `compact` (maintenance sub-SPEC section 4.3).

## 9. Tests

In `jitcache/tests/store/ContainerTests.cpp`, `StoreTests.cpp` and `WriterTests.cpp`, run by `testjitcache`, except C1 and C8, whose items name their files; each item names its tests:

- C1. CRC-32C, in `tests/store/PlatformTests.cpp` beside the other tests of `JITCachePlatform.cpp`: the RFC 3720 vectors (`"123456789"`, 32 zero bytes, 32 `0xFF` bytes, the bytes 0 to 31 and 31 down to 0), whole and byte by byte, and equal results with a bitwise reference the test computes on 4096 random buffers of random lengths and alignments, fed whole and in random pieces, each through `crc32cExtend` and through `crc32cExtendWithTableForTesting`; an empty span leaves the state unchanged (`integratorCRC32C`).
- C2. Header: a header built from crafted facts parses back to them, with no app and with an app; each corrupt case of section 3.2 is classified corrupt, the app record's flag 2, a flag 0 beside a nonzero digest and a nonzero byte at 29 to 31 among them, and each differing field incompatible, with the detail naming that field; another layout version, 1 among them, is incompatible without the rest being read; `checkArtifactHeader` finds a header compatible whatever its app record when the config names no app, and when the config names the header's app or the header names none, and made for another app, with both digests in its detail, when both name apps that differ, while a header that also differs in a field is incompatible; `headerNamingApp` with no app returns `expectedHeader()` and with one differs from it only in the app record and the CRC; the digest is the first 16 bytes of the header's SHA-256 and differs for headers that differ only in their apps (`containerHeaderRoundTrip`, `containerHeaderCorrupt`, `containerHeaderIncompatible`, `containerHeaderApps`, `containerHeaderDigest`).
- C3. Body: the section kinds' table gives each kind's type id, tier and name, and `sectionKindFor` and `isSectionRequired` agree with it (`containerSectionKinds`); a body built from synthetic sections of every kind encodes as sections 4.1 to 4.3 lay out and validates in both modes (`containerBodyEncoding`). In `Full` mode each check B1 to B8 fails exactly when its own condition is broken, with its own name, for each way section 4.5 lists. In `Integrity` mode each integrity part fails the same way, and a body broken only in a structure part (a nonzero zero byte, a tier of 2, a zero commit identifier, an unknown section kind, entries out of order, nonzero padding, a missing required section) validates (`containerBodyChecks`); `Integrity` checks every entry's CRC and records the first entry of each kind (`containerBodyIntegrityEntries`). Truncation at every region boundary fails in both modes (`containerBodyTruncation`); the scoring read's framing read runs its checks in order (`containerBodyPartialReads`); no malformed input reads out of bounds under ASan in either mode (`containerBodyFuzz`); a tier-0 body holding exactly the UCB lane's three sections validates in `Full` mode, and another highest tier fails (`runOnceContainerAcceptsTierZero`); a `ucb.analysis` section validates in bodies of tier 0 and 1 whose key's identity kind is `Module`, with and without it, while in `Full` mode a body of any other identity kind that holds one fails B7 (`containerAnalysisSectionOnlyInModules`).
- C4. The pack and its readers. Temporary names and key hex round trip (`storeNames`). The prefix, record header, index block and entry encodings round trip; the prefix's checks ignore the root and the retired word and reject another header digest and a damaged digest byte; a sealed block's trailer gives its offset; a tombstone with a commit identifier and a full index naming a previous block fail; `packRecordExtent` rounds to 8 (`storePackLayoutsRoundTrip`). A new pack lists nothing, answers every read `Absent`, has a valid end of 104 and a latest block at 64, and `packHoldsBodies` is false for it (`storeEmptyPackListsNothing`). A second object for the directory (the registry hook of section 7.5), standing for another process, sees a commit at its first read after the root's store, with the commit identifier as the token; a recapture's version wins while a body opened before keeps its bytes, and the live bytes fall below the valid end (`storeOtherViewFollowsTheRoot`). Six damaged trailers, each in its own pack (an earlier sound block, a record, 0, misaligned, the trailer's own offset, the largest aligned value), make every read of a following object `Invalid` at `container.pack` and a fresh object's build fail (`storeDamagedTrailerFailsWithoutMisreading`). Roots past the file by one word, by 64 MiB and at the largest aligned value, a misaligned root, one below 104 and one inside a record all fail a fresh object's build; a root moved back to an earlier valid end reads as that earlier pack to a fresh object and is damage to an object that applied the later one (`storeDamagedRootFailsWithoutMisreading`). When the file is truncated below a root that an object has not applied yet, that object's next read finds the root past the file, which is damage at `container.pack` for every key (`storeRootPastAShrunkFileIsDamage`). Bytes a commit that failed at its reread left past the valid end are invisible, and the next commit overwrites them, leaving the file ending at its valid end (`storeBytesPastTheValidEndAreInvisible`). A damaged last section is `Invalid` at `container.checksum` in both modes (`storeOpenRejectsDamagedBodies`). A damaged delta makes every read of an object that applies it `Invalid` at `container.pack` and a fresh object's build fail (`storeDamagedIndexMakesReadsInvalid`). A fresh object over `packDeltaChainBound` - 1 commits applies that many deltas and the empty full index, and one over `packDeltaChainBound` + 10 commits applies a full index and 10 deltas (`storeDeltaChainIsBounded`).
- C5. Lock: a second `ProducerLock::tryAcquire` on the same parent in the process is busy while the first lives (`storeProducerLockIsExclusive`).
- C6. Reads on the store's own results. The scoring read of a second object finds a committed body's three summaries, commit identifier and tier; the fault hook's `EMFILE` makes it `Unavailable`; an `EIO` on the first `open` gives `Invalid` with that errno, and the next `open` finds the body (`storeScoringReadFollowsTheRoot`). After a compaction, a second object reopens the path at its next read, counts one reopen, keeps every key's token and opens every newest version, while a body it opened before keeps reading the old mapping; the compacted pack holds its live records and one full index (`storeRetiredPackReopensAndBodiesOutliveIt`). A retired mark followed by the pack's unlink makes a second object answer every key `Absent` with a zero token and no indexed body (`storeDeletedPackMissesEveryKey`). `readSection` returns a committed module body's `ucb.analysis` bytes, an empty span for a body without one, and `Absent` for a key without a body; damage in another section passes it unseen, which `open` then reports, while damage in that section is `Invalid` at `container.checksum` (`storeReadSectionChecksOneSection`). `ValidatedBody::createForTesting` places each section at an 8-aligned address and keeps the key, version and tier, and the last reference's thread runs `onDestroy` once (`integratorValidatedBodyForTesting`, `integratorValidatedBodyDestroyedByLastReference`). Under registry sharing, an object held while `cache/` gets a header naming another app and a pack under its digest stays as it was, the next `take` with that header builds a new object with the new digest, which later takes share, and a gone object is replaced the same way under an equal header (`storeRegistryRebuildsWhenTheHeaderChanges`). A retired pack replaced by a sound pack made under another header makes the object gone, so its view answers `Absent` and lists no body, while the same pack with one digest byte flipped is still `Invalid` (`storeRetiredPackFollowedByAnotherHeadersPackIsGone`).
- C7. Writer, on `CommitSections` built from crafted spans and streamed sources. A commit appends its record at the old valid end and a one-entry delta after it, whose end is the new valid end and the file's size; the token is the commit identifier, and `open` returns the sources' bytes with the stamp's tier, L and P (`writerCommitPublishesTheSources`). Staging buffers of 1, 7, 64 and 4096 bytes, with sources streamed in 3-byte chunks, write the right bytes (`writerStreamsThroughSmallStagingBuffers`). A source that declares one byte more than it streams fails at `writer.section` and publishes nothing (`writerRejectsMiscountedSections`). Each injectable commit fault fails at its check, keeps the valid end and the key's token, and leaves the next commit to succeed (`writerFaultsPublishNothing`). A refused staging charge fails at `budget.limit` with the valid end unchanged (`writerRefusedStagingChargeWritesNothing`). A tombstone removes the key from a second object and from a fresh one, and removing a key without a body does nothing (`writerTombstoneRemovesTheKey`). `compactIfNeeded` leaves a new pack and a dense one alone, compacts once index blocks or superseded versions add a dead byte, keeps every token, and the next commit appends to the new pack (`writerCompactsUnlessThePackIsDense`). Each compaction fault fails at its check with kind `Storage`, keeps the valid end and leaves no temporary (`writerCompactionFaultsKeepTheOldPack`). Eight generations of recaptures each end dense after the compaction (`writerChainGrowsDeadBytesAndCompacts`). A consumer thread opening bodies while the producer commits and compacts finds every body whole and ends seeing the last versions (`writerConsumerReadsWhileProducerCommitsAndCompacts`). A thread loading the root through its own mapping during 2 · `packDeltaChainBound` + 8 commits reads every valid end it loads back through the trailer to a full index whose records repeat their entries (`writerReaderNeverSeesARootBeforeItsBlock`). `rewriteSection` reseals a body under a fresh commit identifier and fails at `writer.rewrite` for a key without a body (`writerRewriteSectionResealsTheBody`). On a module's body at tier 0 and tier 1, a rewrite of another section keeps `ucb.analysis`, a rewrite reaches `ucb.analysis` itself, and a kind the body does not hold fails at `writer.rewrite` with nothing written (`writerRewriteSectionCopiesEverySectionTheBodyHolds`). Hook names round trip, and `writer.create` and `writer.publish` name no injectable check (`writerHookNames`). A kill point ends its process, so the producers of harness sub-SPEC H8 and SPEC-integrator.md's `producer-kill.js` test each point rather than C7.
- C8. Creation and replacement, through `start`, in `jitcache/tests/session/SessionTests.cpp` (SPEC-integrator.md section 15.1, T-START). A header-less `cache/` holding temporaries and another name is reused: the temporaries go, the other name stays, a pack and the process's header are written, and a Consumer then starts; a remnant whose pack may hold a body is `start.not-an-artifact` and keeps everything; an artifact with a header is `start.artifact-exists` (`integratorStartCreatesOverARemnant`). A ConsumerProducer over an artifact whose header differs in one must-match option is `Started` at `start.replaced`, naming the option; the new `cache/` holds a header and a new pack, a leftover `.cache.replaced` from an earlier replacement is gone, and a Consumer then starts (`integratorStartRejections`); a ConsumerProducer over an artifact made for another app replaces it the same way, and the new header names the ConsumerProducer's app (T-START's `integratorStartApps`). A failed pack mapping is a `Fault` at `start.io`, and a file at `cache/pack` that is no pack a `Fault` at `start.pack` (`integratorStartFaults`). A FIFO at `cache/header`, which `replaceWithFIFO` (`tests/support/TestFiles.h`) puts in place of the file, makes a Consumer's `start` a `Fault` at `start.header`, and one at `cache/pack` makes a Consumer's and a ConsumerProducer's a `Fault` at `start.pack`, each without blocking (`integratorStartFaultsOnAFIFO`).
