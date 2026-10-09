#include "config.h"
#include "ArtifactStore.h"

#include "JITCacheParameters.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <stddef.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringCommon.h>

#if OS(LINUX)
#include <limits.h>
#include <sys/inotify.h>
#endif

// The artifact in a process (container sub-SPEC sections 1.1, 2 and 5 to 7): the names, the producer lock, the registry,
// the opened artifact with its index, listings and refresh, and the store's three reads.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProducerLock);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SavedSummaries);
WTF_MAKE_TZONE_ALLOCATED_IMPL(OpenedArtifact);

namespace ArtifactStoreInternal {

// The lock file (container sub-SPEC section 2): the tag, the layout version, four zero bytes, the commit epoch and 40
// zero bytes.
static constexpr size_t lockFileBytes = 64;
static constexpr std::array<uint8_t, 8> lockFileTag { 'J', 'I', 'T', 'C', 'L', 'O', 'C', 'K' };
static constexpr uint32_t lockFileLayoutVersion = 1;
static constexpr size_t lockFileVersionOffset = 8;
static constexpr size_t lockFileEpochOffset = 16;
static_assert(!(lockFileEpochOffset % alignof(uint64_t)), "the epoch is loaded and bumped atomically through the page-aligned mapping");

// The names of section 1.1.
static constexpr std::array<char, 4> bodyNameSuffix { '.', 'b', 'i', 'n' };
static constexpr std::array<char, 4> temporaryNameSuffix { '.', 't', 'm', 'p' };
static constexpr std::array<char, 1> bodyTemporaryPrefix { '.' };
static constexpr std::array<char, 8> headerTemporaryPrefix { '.', 'h', 'e', 'a', 'd', 'e', 'r', '.' };
static constexpr size_t temporaryRandomBytes = 16; // 128 bits, 32 hex digits
static_assert(headerTemporaryPrefix.size() + 2 * temporaryRandomBytes + temporaryNameSuffix.size() == maximumTemporaryFileNameLength);
static_assert(bodyFileNameLength == 2 * BodyKey::byteSize + bodyNameSuffix.size());

// The bytes of a body key the index's hash reads (SPEC-ucb.md section 3.1): kind, specialization and mode, then the
// identity digest from offset 8.
static constexpr size_t keyKindOffset = 1;
static constexpr size_t keySpecializationOffset = 2;
static constexpr size_t keyModeOffset = 3;
static constexpr size_t keyIdentityDigestOffset = 8;

// The summary sections a score reads (section 7.3), which the scoring read checksums.
static constexpr std::array<SectionKind, 3> summarySectionKinds { SectionKind::UCBFeedback, SectionKind::CBSummaryBaseline, SectionKind::ICsBaseline };

// The pinned descriptors (section 5.2). O_PATH and MAP_POPULATE are Linux's; elsewhere start rejects at its build-ID
// step before any artifact is opened, so the fallbacks only keep the file compiling.
static constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#if OS(LINUX)
static constexpr int pinnedParentFlags = O_PATH | O_DIRECTORY | O_CLOEXEC;
static constexpr int populateMapping = MAP_POPULATE;
#else
static constexpr int pinnedParentFlags = directoryFlags;
static constexpr int populateMapping = 0;
#endif

#if OS(LINUX)
// One read of the inotify queue holds at least one event with the longest name.
static constexpr size_t inotifyBufferBytes = 4096;
static_assert(inotifyBufferBytes >= sizeof(struct inotify_event) + NAME_MAX + 1);
static constexpr uint32_t watchedEvents = IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
#endif

static int openAt(int directoryFd, const char* name, int flags, mode_t mode = 0)
{
    int fd;
    do {
        fd = ::openat(directoryFd, name, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

// Closes a descriptor and keeps errno, so a caller can close before it reports the error of an earlier call.
static void closeKeepingErrno(int fd)
{
    int error = errno;
    ::close(fd);
    errno = error;
}

static bool isTransientError(int error)
{
    return error == EMFILE || error == ENFILE || error == ENOMEM;
}

static StoreOutcome outcomeOfError(int error)
{
    if (error == ENOENT)
        return StoreOutcome::Absent;
    if (isTransientError(error))
        return StoreOutcome::Unavailable;
    return StoreOutcome::Invalid;
}

static StoreFailure failureOfError(int error)
{
    if (outcomeOfError(error) != StoreOutcome::Invalid)
        return { };
    return { "container.io"_s, error };
}

// Reads exactly bytes.size() bytes at offset. Returns 0, the errno of a failed read, or EIO at an early end of file.
static int readExactly(int fd, std::span<uint8_t> bytes, off_t offset)
{
    while (!bytes.empty()) {
        ssize_t count = ::pread(fd, bytes.data(), bytes.size(), offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!count)
            return EIO;
        bytes = bytes.subspan(static_cast<size_t>(count));
        offset += count;
    }
    return 0;
}

// Writes exactly bytes.size() bytes at offset, retrying short writes. Returns 0 or the errno of a failed write.
static int writeExactly(int fd, std::span<const uint8_t> bytes, off_t offset)
{
    while (!bytes.empty()) {
        ssize_t count = ::pwrite(fd, bytes.data(), bytes.size(), offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        bytes = bytes.subspan(static_cast<size_t>(count));
        offset += count;
    }
    return 0;
}

static bool holdsLockFileHeader(std::span<const uint8_t> bytes)
{
    ASSERT(bytes.size() >= lockFileEpochOffset);
    uint32_t version = 0;
    memcpySpan(asMutableByteSpan(version), bytes.subspan(lockFileVersionOffset, sizeof(version)));
    return equalSpans(bytes.first(lockFileTag.size()), std::span { lockFileTag }) && version == lockFileLayoutVersion;
}

// With the lock held, a lock file shorter than 64 bytes, or with another tag or version, is set up in place: ftruncate to
// 64 bytes, then the whole layout with a zero epoch, so its reserved bytes are zero too. A valid file keeps its epoch.
// Returns 0 or the errno of the call that failed.
static int setUpLockFile(int fd)
{
    struct stat status;
    if (fstat(fd, &status))
        return errno;
    if (status.st_size >= static_cast<off_t>(lockFileBytes)) {
        std::array<uint8_t, lockFileEpochOffset> head { };
        if (int error = readExactly(fd, head, 0))
            return error;
        if (holdsLockFileHeader(head))
            return 0;
    }
    if (ftruncate(fd, static_cast<off_t>(lockFileBytes)))
        return errno;
    std::array<uint8_t, lockFileBytes> layout { };
    memcpySpan(std::span { layout }.first(lockFileTag.size()), std::span { lockFileTag });
    uint32_t version = lockFileLayoutVersion;
    memcpySpan(std::span { layout }.subspan(lockFileVersionOffset, sizeof(version)), asByteSpan(version));
    return writeExactly(fd, layout, 0);
}

// The epoch of the lock file, mapped PROT_READ and MAP_SHARED, or null when the object gets none (section 2). A missing
// file, one shorter than 64 bytes or with another tag or version, or one that cannot be opened or mapped, only changes
// how the index refreshes: it never does.
static const uint64_t* mapEpochForReading(int parentFd)
{
    int fd = openAt(parentFd, ArtifactNames::lockFile.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    void* page = MAP_FAILED;
    struct stat status;
    if (!fstat(fd, &status) && status.st_size >= static_cast<off_t>(lockFileBytes))
        page = mmap(nullptr, lockFileBytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (page == MAP_FAILED)
        return nullptr;
    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(page), lockFileBytes);
    if (!holdsLockFileHeader(bytes)) {
        munmap(page, lockFileBytes);
        return nullptr;
    }
    return reinterpret_cast<const uint64_t*>(bytes.subspan(lockFileEpochOffset).data());
}

static void unmapEpoch(const uint64_t* epoch)
{
    auto* page = reinterpret_cast<const uint8_t*>(epoch) - lockFileEpochOffset;
    munmap(const_cast<uint8_t*>(page), lockFileBytes);
}

static uint64_t loadEpoch(const uint64_t* epoch)
{
    return __atomic_load_n(epoch, __ATOMIC_ACQUIRE);
}

// Hex digits as section 1.1 spells them: lowercase only.
static std::optional<uint8_t> lowercaseHexDigitValue(char character)
{
    if (isASCIIDigit(character))
        return static_cast<uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<uint8_t>(character - 'a' + 10);
    return std::nullopt;
}

static void writeLowercaseHex(std::span<char> destination, std::span<const uint8_t> bytes)
{
    RELEASE_ASSERT(destination.size() == 2 * bytes.size());
    for (size_t index = 0; index < bytes.size(); ++index) {
        destination[2 * index] = upperNibbleToLowercaseASCIIHexDigit(bytes[index]);
        destination[2 * index + 1] = lowerNibbleToLowercaseASCIIHexDigit(bytes[index]);
    }
}

static bool readLowercaseHex(std::span<const char> digits, std::span<uint8_t> bytes)
{
    RELEASE_ASSERT(digits.size() == 2 * bytes.size());
    for (size_t index = 0; index < bytes.size(); ++index) {
        auto high = lowercaseHexDigitValue(digits[2 * index]);
        auto low = lowercaseHexDigitValue(digits[2 * index + 1]);
        if (!high || !low)
            return false;
        bytes[index] = static_cast<uint8_t>(*high << 4 | *low);
    }
    return true;
}

static bool isLowercaseHex(std::span<const char> digits)
{
    return std::ranges::all_of(digits, [](char character) {
        return !!lowercaseHexDigitValue(character);
    });
}

static bool isDotOrDotDot(std::span<const char> name)
{
    return (name.size() == 1 && name[0] == '.') || (name.size() == 2 && name[0] == '.' && name[1] == '.');
}

// The store's three calls that open a file (section 7.5), which the twins builds' fault hook can fail.
enum class StoreCall : uint8_t { Open, ReadSavedSummaries, Listing };

#if ENABLE(JITCACHE_TWINS)
static_assert(static_cast<uint8_t>(StoreTesting::Call::Open) == static_cast<uint8_t>(StoreCall::Open));
static_assert(static_cast<uint8_t>(StoreTesting::Call::ReadSavedSummaries) == static_cast<uint8_t>(StoreCall::ReadSavedSummaries));
static_assert(static_cast<uint8_t>(StoreTesting::Call::Listing) == static_cast<uint8_t>(StoreCall::Listing));

// Every hook is made of atomics: the listing's hook is read under m_indexLock, which is a leaf, and the shell sets the
// hooks from its own thread. A test sets a hook while the store is idle, so the fields need no joint update; armed is
// stored last and loaded first.
struct StoreFaultHook {
    std::atomic<bool> armed { false };
    std::atomic<uint8_t> call { 0 };
    std::atomic<int> error { 0 };
    std::atomic<uint64_t> n { 0 }; // 0: every call
    std::atomic<uint64_t> calls { 0 }; // the calls of that kind since the hook was set
};
static StoreFaultHook storeFaultHook;
static std::atomic<bool> storeRegistrySharing { true };
static std::atomic<bool> storeInotifyEnabled { true };
static std::atomic<double> storeListingIntervalSeconds { std::numeric_limits<double>::quiet_NaN() }; // NaN: the parameter
#endif

// The error the fault hook makes the call's openat fail with, or 0.
static int injectedOpenError(StoreCall call)
{
#if ENABLE(JITCACHE_TWINS)
    if (!storeFaultHook.armed.load())
        return 0;
    if (storeFaultHook.call.load() != static_cast<uint8_t>(call))
        return 0;
    uint64_t ordinal = storeFaultHook.calls.fetch_add(1) + 1;
    uint64_t n = storeFaultHook.n.load();
    if (n && ordinal != n)
        return 0;
    return storeFaultHook.error.load();
#else
    UNUSED_PARAM(call);
    return 0;
#endif
}

static bool registryShares()
{
#if ENABLE(JITCACHE_TWINS)
    return storeRegistrySharing.load();
#else
    return true;
#endif
}

static Seconds fallbackListingInterval()
{
#if ENABLE(JITCACHE_TWINS)
    double seconds = storeListingIntervalSeconds.load();
    if (!std::isnan(seconds))
        return Seconds(seconds);
#endif
    return Seconds::fromMilliseconds(static_cast<double>(fallbackListingIntervalMilliseconds));
}

// The inotify watch on the pinned bodies/ directory (section 6.3), or -1, in which case the object refreshes by listing.
static int watchBodies(int bodiesFd)
{
#if OS(LINUX)
#if ENABLE(JITCACHE_TWINS)
    if (!storeInotifyEnabled.load())
        return -1;
#endif
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0)
        return -1;
    // The watch names the descriptor, so it follows the directory whose header the VM checked, whatever its path does.
    CString path = makeString("/proc/self/fd/"_s, bodiesFd).utf8();
    if (inotify_add_watch(fd, path.data(), watchedEvents) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
#else
    UNUSED_PARAM(bodiesFd);
    return -1;
#endif
}

// Reads the inotify queue until EAGAIN and drops what it read: a listing reads the directory those events describe.
static void discardEvents(int inotifyFd)
{
#if OS(LINUX)
    if (inotifyFd < 0)
        return;
    std::array<uint8_t, inotifyBufferBytes> buffer;
    while (true) {
        ssize_t count = ::read(inotifyFd, buffer.data(), buffer.size());
        if (count > 0)
            continue;
        if (count < 0 && errno == EINTR)
            continue;
        return;
    }
#else
    UNUSED_PARAM(inotifyFd);
#endif
}

struct MapFailure {
    int error { 0 }; // the errno of openat, fstat or mmap; 0 when check names the failure
    ContainerCheck check; // B1, for a file shorter than an envelope, which is not mapped
};

// Opens the key's body name relative to bodiesFd, takes its size with fstat, maps the whole file privately and read-only,
// and closes the descriptor. The caller owns the mapping.
static std::expected<std::span<const uint8_t>, MapFailure> mapBodyFile(StoreCall call, int bodiesFd, const BodyKey& key, int extraMapFlags)
{
    if (int injected = injectedOpenError(call))
        return std::unexpected(MapFailure { injected, { } });
    BodyFileName name = bodyFileName(key);
    int fd = openAt(bodiesFd, name.data(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(MapFailure { errno, { } });

    struct stat status;
    if (fstat(fd, &status)) {
        closeKeepingErrno(fd);
        return std::unexpected(MapFailure { errno, { } });
    }
    // B1: a file shorter than the envelope holds no envelope to read, and an empty one cannot be mapped.
    if (status.st_size < static_cast<off_t>(bodyEnvelopeBytes)) {
        ::close(fd);
        return std::unexpected(MapFailure { 0, ContainerChecks::size });
    }
    size_t size = static_cast<size_t>(status.st_size);
    void* address = mmap(nullptr, size, PROT_READ, MAP_PRIVATE | extraMapFlags, fd, 0);
    closeKeepingErrno(fd);
    if (address == MAP_FAILED)
        return std::unexpected(MapFailure { errno, { } });
    return unsafeMakeSpan(static_cast<const uint8_t*>(address), size);
}

static void unmap(std::span<const uint8_t> mapping)
{
    int result = munmap(const_cast<uint8_t*>(mapping.data()), mapping.size());
    ASSERT_UNUSED(result, !result);
}

static std::span<const uint8_t> sectionIn(std::span<const uint8_t> file, const BodyLayout& layout, SectionKind kind)
{
    auto& extent = layout.sections[static_cast<size_t>(kind)];
    if (!extent)
        return { };
    return file.subspan(static_cast<size_t>(extent->offset), static_cast<size_t>(extent->size));
}

} // namespace ArtifactStoreInternal

// Names (container sub-SPEC section 1.1).

BodyFileName bodyFileName(const BodyKey& key)
{
    using namespace ArtifactStoreInternal;
    BodyFileName name;
    auto characters = std::span { name.characters };
    writeLowercaseHex(characters.first(2 * BodyKey::byteSize), key.bytes());
    memcpySpan(characters.subspan(2 * BodyKey::byteSize, bodyNameSuffix.size()), std::span { bodyNameSuffix });
    return name;
}

std::optional<BodyKey> bodyKeyFromFileName(std::span<const char> name)
{
    using namespace ArtifactStoreInternal;
    if (name.size() != bodyFileNameLength || !equalSpans(name.last(bodyNameSuffix.size()), std::span { bodyNameSuffix }))
        return std::nullopt;
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    if (!readLowercaseHex(name.first(2 * BodyKey::byteSize), bytes))
        return std::nullopt;
    return BodyKey::fromBytes(bytes);
}

TemporaryFileName temporaryFileName(TemporaryKind kind)
{
    using namespace ArtifactStoreInternal;
    std::array<uint8_t, temporaryRandomBytes> random { };
    cryptographicallyRandomValues(random);
    std::span<const char> prefix = kind == TemporaryKind::Header ? std::span<const char> { headerTemporaryPrefix } : std::span<const char> { bodyTemporaryPrefix };

    TemporaryFileName name;
    auto characters = std::span { name.characters };
    memcpySpan(characters.first(prefix.size()), prefix);
    writeLowercaseHex(characters.subspan(prefix.size(), 2 * random.size()), random);
    memcpySpan(characters.subspan(prefix.size() + 2 * random.size(), temporaryNameSuffix.size()), std::span { temporaryNameSuffix });
    return name;
}

std::optional<TemporaryKind> temporaryKindOfFileName(std::span<const char> name)
{
    using namespace ArtifactStoreInternal;
    auto matches = [&](std::span<const char> prefix) {
        size_t digits = 2 * temporaryRandomBytes;
        return name.size() == prefix.size() + digits + temporaryNameSuffix.size()
            && equalSpans(name.first(prefix.size()), prefix)
            && isLowercaseHex(name.subspan(prefix.size(), digits))
            && equalSpans(name.last(temporaryNameSuffix.size()), std::span { temporaryNameSuffix });
    };
    if (matches(headerTemporaryPrefix))
        return TemporaryKind::Header;
    if (matches(bodyTemporaryPrefix))
        return TemporaryKind::Body;
    return std::nullopt;
}

int listDirectory(int directoryFd, const ScopedLambda<void(std::span<const char> name, uint64_t inode)>& visit)
{
    using namespace ArtifactStoreInternal;
    // "." relative to the descriptor gives an open file description of the listing's own: fdopendir of a dup would share
    // the descriptor's offset, which glibc's fdopendir does not reset, so a second listing would read nothing.
    int fd = openAt(directoryFd, ".", directoryFlags);
    if (fd < 0)
        return errno;
    DIR* directory = fdopendir(fd);
    if (!directory) {
        int error = errno;
        ::close(fd);
        return error;
    }
    int error = 0;
    while (true) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (!entry) {
            error = errno;
            break;
        }
        auto name = unsafeSpan(entry->d_name);
        if (isDotOrDotDot(name))
            continue;
        visit(name, static_cast<uint64_t>(entry->d_ino));
    }
    closedir(directory);
    return error;
}

// The producer lock (container sub-SPEC section 2).

ProducerLock::ProducerLock(int fd, uint64_t* epoch)
    : m_fd(fd)
    , m_epoch(epoch)
{
}

std::expected<std::unique_ptr<ProducerLock>, ProducerLock::Failure> ProducerLock::tryAcquire(int parentFd)
{
    using namespace ArtifactStoreInternal;
    int fd = openAt(parentFd, ArtifactNames::lockFile.characters(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(Failure { false, errno });

    int result;
    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result && errno == EINTR);
    if (result) {
        int error = errno;
        ::close(fd);
        if (error == EWOULDBLOCK)
            return std::unexpected(Failure { true, error });
        return std::unexpected(Failure { false, error });
    }

    // Closing the descriptor releases the lock, so every failure from here on leaves the lock free.
    if (int error = setUpLockFile(fd)) {
        ::close(fd);
        return std::unexpected(Failure { false, error });
    }
    void* page = mmap(nullptr, lockFileBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (page == MAP_FAILED) {
        int error = errno;
        ::close(fd);
        return std::unexpected(Failure { false, error });
    }
    auto* epoch = reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(page) + lockFileEpochOffset);
    return std::unique_ptr<ProducerLock>(new ProducerLock(fd, epoch));
}

uint64_t ProducerLock::bumpEpoch()
{
    // Release order: whoever loads the new value with acquire order also sees the rename or the removals before it.
    return __atomic_fetch_add(m_epoch, 1, __ATOMIC_RELEASE);
}

ProducerLock::~ProducerLock()
{
    using namespace ArtifactStoreInternal;
    munmap(reinterpret_cast<uint8_t*>(m_epoch) - lockFileEpochOffset, lockFileBytes);
    ::close(m_fd);
}

// The index's hash and traits (container sub-SPEC section 6.1).

unsigned BodyKeyHash::hash(const BodyKey& key)
{
    using namespace ArtifactStoreInternal;
    auto bytes = key.bytes();
    auto digest = bytes.subspan(keyIdentityDigestOffset);
    uint32_t digestPrefix = static_cast<uint32_t>(digest[0]) | static_cast<uint32_t>(digest[1]) << 8
        | static_cast<uint32_t>(digest[2]) << 16 | static_cast<uint32_t>(digest[3]) << 24;
    uint32_t kindBytes = static_cast<uint32_t>(bytes[keyKindOffset]) | static_cast<uint32_t>(bytes[keySpecializationOffset]) << 8
        | static_cast<uint32_t>(bytes[keyModeOffset]) << 16;
    return WTF::pairIntHash(digestPrefix, kindBytes);
}

BodyKey BodyKeyHashTraits::emptyValue()
{
    return std::bit_cast<BodyKey>(std::array<uint8_t, BodyKey::byteSize> { });
}

void BodyKeyHashTraits::constructDeletedValue(BodyKey& slot)
{
    std::array<uint8_t, BodyKey::byteSize> bytes;
    bytes.fill(0xFF);
    slot = std::bit_cast<BodyKey>(bytes);
}

bool BodyKeyHashTraits::isDeletedValue(const BodyKey& key)
{
    return std::ranges::all_of(key.bytes(), [](uint8_t byte) {
        return byte == 0xFF;
    });
}

// Saved summaries (container sub-SPEC section 7.3).

SavedSummaries::SavedSummaries(std::span<const uint8_t> mapping, uint64_t version, uint8_t highestTier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics)
    : m_mapping(mapping)
    , m_version(version)
    , m_highestTier(highestTier)
    , m_ucbFeedback(ucbFeedback)
    , m_cbSummary(cbSummary)
    , m_ics(ics)
{
}

SavedSummaries::~SavedSummaries()
{
    ArtifactStoreInternal::unmap(m_mapping);
}

uint64_t SavedSummaries::version() const
{
    return m_version;
}

uint8_t SavedSummaries::highestTier() const
{
    return m_highestTier;
}

std::span<const uint8_t> SavedSummaries::ucbFeedback() const
{
    return m_ucbFeedback;
}

std::span<const uint8_t> SavedSummaries::cbSummary() const
{
    return m_cbSummary;
}

std::span<const uint8_t> SavedSummaries::ics() const
{
    return m_ics;
}

// An opened artifact (container sub-SPEC section 5.2).

OpenedArtifact::OpenedArtifact(int parentFd, int cacheFd, int bodiesFd, const std::array<uint8_t, 16>& headerDigest, const uint64_t* epoch, int inotifyFd)
    : m_parentFd(parentFd)
    , m_cacheFd(cacheFd)
    , m_bodiesFd(bodiesFd)
    , m_headerDigest(headerDigest)
    , m_epoch(epoch)
    , m_inotifyFd(inotifyFd)
{
}

std::expected<Ref<OpenedArtifact>, int> OpenedArtifact::create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes)
{
    using namespace ArtifactStoreInternal;
    // The object pins descriptors of its own, so it outlives the descriptors of the VM that built it, and every later
    // lookup, open and write goes through the directories whose header that VM checked.
    int pinnedParentFd = openAt(parentFd, ".", pinnedParentFlags);
    if (pinnedParentFd < 0)
        return std::unexpected(errno);
    int pinnedCacheFd = openAt(cacheFd, ".", directoryFlags);
    if (pinnedCacheFd < 0) {
        closeKeepingErrno(pinnedParentFd);
        return std::unexpected(errno);
    }
    int pinnedBodiesFd = openAt(cacheFd, ArtifactNames::bodiesDirectory.characters(), directoryFlags);
    if (pinnedBodiesFd < 0) {
        closeKeepingErrno(pinnedCacheFd);
        closeKeepingErrno(pinnedParentFd);
        return std::unexpected(errno);
    }

    // A missing epoch or inotify only changes how the index refreshes.
    const uint64_t* epoch = mapEpochForReading(pinnedParentFd);
    int inotifyFd = watchBodies(pinnedBodiesFd);
    Ref<OpenedArtifact> artifact = adoptRef(*new OpenedArtifact(pinnedParentFd, pinnedCacheFd, pinnedBodiesFd, artifactHeaderDigest(headerBytes), epoch, inotifyFd));

    // The index is listed here, at the demand a role creates when it starts, so no directory scan lands inside a request
    // point. The watch exists before the listing, so a change made during it is queued for the next refresh. A failure
    // destroys the object unregistered, and start reports the errno at start.io.
    int error;
    {
        OpenedArtifact& object = artifact.get();
        Locker locker { object.m_indexLock };
        error = object.list();
    }
    if (error)
        return std::unexpected(error);
    return artifact;
}

OpenedArtifact::~OpenedArtifact()
{
    // The last reference is gone, so no other thread reads the guarded fields.
    if (m_epoch)
        ArtifactStoreInternal::unmapEpoch(m_epoch);
    if (m_inotifyFd >= 0)
        ::close(m_inotifyFd);
    ::close(m_bodiesFd);
    ::close(m_cacheFd);
    ::close(m_parentFd);
}

uint64_t OpenedArtifact::token(const BodyKey& key)
{
    Locker locker { m_indexLock };
    refreshIfStale();
    return currentToken(key);
}

BodyOpen OpenedArtifact::open(const BodyKey& key, ValidationMode mode)
{
    using namespace ArtifactStoreInternal;
    uint64_t tokenBeforeOpen;
    {
        Locker locker { m_indexLock };
        refreshIfStale();
        tokenBeforeOpen = currentToken(key);
    }
    // A key the index lacks costs no system call (SPEC-ucb.md R-INT-3).
    if (!tokenBeforeOpen)
        return { StoreOutcome::Absent, nullptr, { } };

    // The file work runs outside m_indexLock (section 6.4).
    auto mapped = mapBodyFile(StoreCall::Open, m_bodiesFd, key, populateMapping);
    if (!mapped) {
        if (!mapped.error().error)
            return { StoreOutcome::Invalid, nullptr, { mapped.error().check, 0 } };
        int error = mapped.error().error;
        if (error == ENOENT)
            eraseIfTokenIs(key, tokenBeforeOpen);
        return { outcomeOfError(error), nullptr, failureOfError(error) };
    }

    std::span<const uint8_t> file = *mapped;
    auto layout = validateBody(file, key, headerDigest(), mode);
    if (!layout) {
        unmap(file);
        return { StoreOutcome::Invalid, nullptr, { layout.error(), 0 }, file.size() };
    }

    ValidatedBody::SectionSpans sections { };
    for (size_t index = 0; index < numberOfSectionKinds; ++index)
        sections[index] = sectionIn(file, *layout, static_cast<SectionKind>(index));
    // The body owns the mapping from here and unmaps it in its destructor.
    Ref<ValidatedBody> body = adoptRef(*new ValidatedBody(key, layout->version, layout->highestTier, file, sections));
    return { StoreOutcome::Found, WTF::move(body), { }, file.size() };
}

SavedSummaryRead OpenedArtifact::readSavedSummaries(const BodyKey& key, ValidationMode mode)
{
    using namespace ArtifactStoreInternal;
    // The read opens the body by its name whatever the index holds, so a body the index lacks, such as one whose producer
    // died between its rename and its epoch bump, is found; ENOENT alone answers that no body exists. The token read
    // here only decides whether an ENOENT may erase the key.
    uint64_t tokenBeforeOpen;
    {
        Locker locker { m_indexLock };
        tokenBeforeOpen = currentToken(key);
    }

    // Without MAP_POPULATE: the read faults in only the envelope, the directory and the three summary sections.
    auto mapped = mapBodyFile(StoreCall::ReadSavedSummaries, m_bodiesFd, key, 0);
    if (!mapped) {
        if (!mapped.error().error)
            return { StoreOutcome::Invalid, nullptr, { mapped.error().check, 0 } };
        int error = mapped.error().error;
        if (error == ENOENT)
            eraseIfTokenIs(key, tokenBeforeOpen);
        return { outcomeOfError(error), nullptr, failureOfError(error) };
    }

    std::span<const uint8_t> file = *mapped;
    auto layout = validateBodyFraming(file, key, headerDigest(), mode, summarySectionKinds);
    if (!layout) {
        unmap(file);
        return { StoreOutcome::Invalid, nullptr, { layout.error(), 0 } };
    }
    std::unique_ptr<SavedSummaries> summaries(new SavedSummaries(file, layout->version, layout->highestTier,
        sectionIn(file, *layout, SectionKind::UCBFeedback), sectionIn(file, *layout, SectionKind::CBSummaryBaseline),
        sectionIn(file, *layout, SectionKind::ICsBaseline)));
    return { StoreOutcome::Found, WTF::move(summaries), { } };
}

bool OpenedArtifact::containsKey(const BodyKey& key)
{
    Locker locker { m_indexLock };
    refreshIfStale();
    return m_index.contains(key);
}

uint64_t OpenedArtifact::indexedBodies()
{
    Locker locker { m_indexLock };
    return m_index.size();
}

uint64_t OpenedArtifact::foreignNames()
{
    Locker locker { m_indexLock };
    return m_foreignNames;
}

std::span<const uint8_t, 16> OpenedArtifact::headerDigest() const
{
    return std::span { m_headerDigest };
}

int OpenedArtifact::cacheFd() const
{
    return m_cacheFd;
}

int OpenedArtifact::bodiesFd() const
{
    return m_bodiesFd;
}

void OpenedArtifact::refreshIfStale()
{
    using namespace ArtifactStoreInternal;
    if (!m_epoch || m_gone)
        return;
    // While nothing changes, this load from a shared page is the whole cost of a lookup.
    uint64_t epoch = loadEpoch(m_epoch);
    if (epoch == m_lastEpochSeen)
        return;

    if (m_inotifyFd >= 0 && !m_listingPending) {
        // The kernel queues a rename's events during the rename, before the writer bumps the epoch, so the queue holds
        // every change the new epoch covers.
        m_lastEpochSeen = epoch;
        drainEvents(true);
        return;
    }

    // Without inotify, or with a listing pending, at most one listing starts per interval. Inside it the epoch is not
    // recorded: the index answers as it stands, and the first lookup after the interval lists.
    if (MonotonicTime::now() - m_lastListingStart < fallbackListingInterval())
        return;
    list();
}

int OpenedArtifact::list()
{
    using namespace ArtifactStoreInternal;
    // One order for every listing: drain the queue, read the epoch, list, and swap the new map in only on success. The map
    // is built under the lock, so nothing the writer's commit or a drained event records meanwhile is dropped.
    discardEvents(m_inotifyFd);
    std::optional<uint64_t> epoch;
    if (m_epoch)
        epoch = loadEpoch(m_epoch);
    m_lastListingStart = MonotonicTime::now();

    HashMap<BodyKey, IndexEntry, BodyKeyHash, BodyKeyHashTraits> index;
    uint64_t foreignNames = 0;
    // A key whose entry in the old index holds the same nonzero inode keeps its token; every other key takes a fresh one.
    // A body replaced by renameat carries its temporary's inode, so it gets a fresh token.
    const auto& previousIndex = m_index;
    uint64_t& nextToken = m_nextToken;
    int error = injectedOpenError(StoreCall::Listing);
    if (!error) {
        error = listDirectory(m_bodiesFd, [&](std::span<const char> name, uint64_t inode) {
            if (auto key = bodyKeyFromFileName(name)) {
                auto previous = previousIndex.find(*key);
                bool keepsToken = previous != previousIndex.end() && previous->value.inode && previous->value.inode == inode;
                index.set(*key, IndexEntry { keepsToken ? previous->value.token : nextToken++, inode });
                return;
            }
            if (!temporaryKindOfFileName(name))
                ++foreignNames;
        });
    }

    if (error) {
        // The index stays as it was. The events drained above are gone, so the next epoch change lists again; until
        // then, the bodies the index lacks are misses.
        if (epoch)
            m_lastEpochSeen = *epoch;
        m_listingPending = true;
        return error;
    }
    m_index = WTF::move(index);
    m_foreignNames = foreignNames;
    if (epoch)
        m_lastEpochSeen = *epoch;
    m_listingPending = false;
    return 0;
}

void OpenedArtifact::drainEvents(bool listOnOverflow)
{
#if OS(LINUX)
    using namespace ArtifactStoreInternal;
    if (m_inotifyFd < 0)
        return;
    bool mustList = false;
    std::array<uint8_t, inotifyBufferBytes> buffer;
    while (true) {
        ssize_t count = ::read(m_inotifyFd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            // A read that fails other than with EAGAIN may have lost events, which only a listing recovers.
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                mustList = true;
            break;
        }
        if (!count)
            break;

        auto events = std::span { buffer }.first(static_cast<size_t>(count));
        while (events.size() >= sizeof(struct inotify_event)) {
            // The fixed fields of struct inotify_event, read without forming an object of a type that ends in a
            // flexible array.
            uint32_t mask = 0;
            uint32_t length = 0;
            memcpySpan(asMutableByteSpan(mask), events.subspan(offsetof(struct inotify_event, mask), sizeof(mask)));
            memcpySpan(asMutableByteSpan(length), events.subspan(offsetof(struct inotify_event, len), sizeof(length)));
            size_t eventBytes = sizeof(struct inotify_event) + length;
            if (eventBytes > events.size())
                break; // the kernel returns whole events; a torn one would be a kernel defect
            auto nameBytes = events.subspan(sizeof(struct inotify_event), length);
            events = events.subspan(eventBytes);

            if (mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                // The pinned directory is gone: every later lookup misses without refreshing. A recreated artifact is
                // another directory, whose header this object never checked.
                m_index.clear();
                ::close(m_inotifyFd);
                m_inotifyFd = -1;
                m_gone = true;
                return;
            }
            if (mask & IN_Q_OVERFLOW) {
                mustList = true;
                continue;
            }
            // The kernel pads the name with NUL bytes.
            auto nameEnd = std::ranges::find(nameBytes, static_cast<uint8_t>(0));
            auto name = spanReinterpretCast<const char>(nameBytes.first(static_cast<size_t>(nameEnd - nameBytes.begin())));
            auto key = bodyKeyFromFileName(name);
            if (!key)
                continue;
            if (mask & IN_MOVED_TO)
                learn(*key, 0); // present, inode unknown
            else if (mask & (IN_MOVED_FROM | IN_DELETE))
                erase(*key);
        }
    }

    if (!mustList)
        return;
    // The descriptor stays. A listing that fails records the epoch it read and marks a listing pending; the writer's
    // index update only marks one, so no listing runs inside a capture pause.
    if (listOnOverflow)
        list();
    else
        m_listingPending = true;
#else
    UNUSED_PARAM(listOnOverflow);
#endif
}

void OpenedArtifact::learn(const BodyKey& key, uint64_t inode)
{
    m_index.set(key, IndexEntry { m_nextToken++, inode });
}

void OpenedArtifact::erase(const BodyKey& key)
{
    m_index.remove(key);
}

uint64_t OpenedArtifact::currentToken(const BodyKey& key)
{
    auto iterator = m_index.find(key);
    return iterator == m_index.end() ? 0 : iterator->value.token;
}

void OpenedArtifact::eraseIfTokenIs(const BodyKey& key, uint64_t token)
{
    // A key absent before the openat has nothing to erase, and a token learned since, from the writer's own commit or an
    // event, names a body that arrived after that openat: erasing it would hide that body from bodyVersion (II8).
    if (!token)
        return;
    Locker locker { m_indexLock };
    auto iterator = m_index.find(key);
    if (iterator != m_index.end() && iterator->value.token == token)
        m_index.remove(iterator);
}

// The registry (container sub-SPEC section 5.1).

ArtifactRegistry& ArtifactRegistry::singleton()
{
    static LazyNeverDestroyed<ArtifactRegistry> registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        registry.construct();
    });
    return registry.get();
}

std::expected<Ref<OpenedArtifact>, int> ArtifactRegistry::take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes)
{
    // The key is the directory start opened and read the header through, so an object found under it pinned the very
    // directory whose header the VM checked.
    struct stat status;
    if (fstat(cacheFd, &status))
        return std::unexpected(errno);
    DirectoryIdentity identity { static_cast<uint64_t>(status.st_dev), static_cast<uint64_t>(status.st_ino) };
    ArtifactRegistry& registry = singleton();
    bool shares = ArtifactStoreInternal::registryShares();

    if (shares) {
        RefPtr<OpenedArtifact> existing;
        {
            Locker locker { registry.m_lock };
            auto iterator = registry.m_artifacts.find(identity);
            if (iterator != registry.m_artifacts.end())
                existing = iterator->value.get();
        }
        if (existing) {
#if ASSERT_ENABLED
            // Every input of the header is process-wide, so the bytes the object was built from equal this VM's.
            HeaderDigest digest = artifactHeaderDigest(headerBytes);
            ASSERT(equalSpans(existing->headerDigest(), std::span { digest }));
#endif
            return existing.releaseNonNull();
        }
    }

    // Built outside the lock, which stays a leaf.
    auto created = OpenedArtifact::create(parentFd, cacheFd, headerBytes);
    if (!created || !shares)
        return created;

    RefPtr<OpenedArtifact> registered;
    {
        Locker locker { registry.m_lock };
        auto& entry = registry.m_artifacts.add(identity, nullptr).iterator->value;
        registered = entry.get();
        if (!registered) {
            // No live object for the directory, a dead entry included: this one is registered.
            entry = created->get();
            registered = created->ptr();
        }
    }
    // When another VM registered its object first, that object wins, and this one is destroyed with created, outside
    // the lock.
    return registered.releaseNonNull();
}

#if ENABLE(JITCACHE_TWINS)
namespace StoreTesting {

void setFault(std::optional<Fault> fault)
{
    auto& hook = ArtifactStoreInternal::storeFaultHook;
    hook.armed.store(false);
    if (!fault)
        return;
    ASSERT(!fault->n || *fault->n);
    hook.call.store(static_cast<uint8_t>(fault->call));
    hook.error.store(fault->error);
    hook.n.store(fault->n.value_or(0));
    hook.calls.store(0);
    hook.armed.store(true);
}

void setRegistrySharing(bool sharing)
{
    ArtifactStoreInternal::storeRegistrySharing.store(sharing);
}

void setInotify(bool enabled)
{
    ArtifactStoreInternal::storeInotifyEnabled.store(enabled);
}

void setFallbackListingInterval(std::optional<Seconds> interval)
{
    ArtifactStoreInternal::storeListingIntervalSeconds.store(interval ? interval->seconds() : std::numeric_limits<double>::quiet_NaN());
}

} // namespace StoreTesting
#endif

} // namespace JSC::JITCache
