#include "config.h"
#include "ArtifactStore.h"

#include "JITCacheFiles.h"
#include "JITCacheParameters.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <wtf/ASCIICType.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringCommon.h>

#if OS(LINUX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// The artifact in a process: the names, the producer lock, the registry, the pack's mapping, and the opened artifact
// that follows the pack's root.
//
// Every system call sits behind OS(LINUX). Elsewhere start rejects at start.platform and maintenance fails with platform
// before either reaches the store, so the calls that would reach the operating system fail with ENOSYS and the other
// definitions only keep the store's interface compiling and linking.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProducerLock);
WTF_MAKE_TZONE_ALLOCATED_IMPL(MappedPack);
WTF_MAKE_TZONE_ALLOCATED_IMPL(SavedSummaries);
WTF_MAKE_TZONE_ALLOCATED_IMPL(OpenedArtifact);

namespace ArtifactStoreInternal {

static constexpr std::array<char, 4> temporaryNameSuffix { '.', 't', 'm', 'p' };
static constexpr std::array<char, 8> headerTemporaryPrefix { '.', 'h', 'e', 'a', 'd', 'e', 'r', '.' };
static constexpr std::array<char, 6> packTemporaryPrefix { '.', 'p', 'a', 'c', 'k', '.' };
static constexpr size_t temporaryRandomBytes = 16; // 128 bits, 32 hex digits
static_assert(headerTemporaryPrefix.size() + 2 * temporaryRandomBytes + temporaryNameSuffix.size() == maximumTemporaryFileNameLength);

// The bytes of a body key the index's hash reads: kind, specialization and mode, then the identity digest from offset 8.
static constexpr size_t keyKindOffset = 1;
static constexpr size_t keySpecializationOffset = 2;
static constexpr size_t keyModeOffset = 3;
static constexpr size_t keyIdentityDigestOffset = 8;

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

static bool isLowercaseHex(std::span<const char> digits)
{
    return std::ranges::all_of(digits, [](char character) {
        return !!lowercaseHexDigitValue(character);
    });
}

enum class StoreCall : uint8_t { Open, ReadSavedSummaries, PackMap };

#if ENABLE(JITCACHE_TWINS)
static_assert(static_cast<uint8_t>(StoreTesting::Call::Open) == static_cast<uint8_t>(StoreCall::Open));
static_assert(static_cast<uint8_t>(StoreTesting::Call::ReadSavedSummaries) == static_cast<uint8_t>(StoreCall::ReadSavedSummaries));
static_assert(static_cast<uint8_t>(StoreTesting::Call::PackMap) == static_cast<uint8_t>(StoreCall::PackMap));

// A test sets a hook while the store is idle, so the fields need no joint update; armed is stored last and loaded first.
struct StoreFaultHook {
    std::atomic<bool> armed { false };
    std::atomic<uint8_t> call { 0 };
    std::atomic<int> error { 0 };
    std::atomic<uint64_t> n { 0 }; // 0: every call
    std::atomic<uint64_t> calls { 0 };
};
static StoreFaultHook storeFaultHook;
static std::atomic<bool> storeRegistrySharing { true };
#endif

// The error the fault hook makes the call fail with, or 0.
static int injectedError(StoreCall call)
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

static StoreFailure failureOfError(int error)
{
    return { "container.io"_s, error, { } };
}

static StoreOutcome outcomeOfError(int error)
{
    if (error == ENOENT)
        return StoreOutcome::Absent;
    if (error == EMFILE || error == ENFILE || error == ENOMEM)
        return StoreOutcome::Unavailable;
    return StoreOutcome::Invalid;
}

// A new artifact's pack: the prefix and an empty full index.
static constexpr uint64_t emptyPackBytes = packPrefixBytes + packIndexBlockBytes(0);

static uint64_t freshPackIdentifier()
{
    uint64_t identifier = 0;
    while (!identifier)
        cryptographicallyRandomValues(asMutableByteSpan(identifier));
    return identifier;
}

#if OS(LINUX)

static constexpr size_t lockFileBytes = 64;
static constexpr std::array<uint8_t, 8> lockFileTag { 'J', 'I', 'T', 'C', 'L', 'O', 'C', 'K' };
static constexpr uint32_t lockFileLayoutVersion = 1;
static constexpr size_t lockFileVersionOffset = 8;
static constexpr size_t lockFileHeaderBytes = 12;

// The summary sections a score reads, which the scoring read checksums.
static constexpr std::array<SectionKind, 3> summarySectionKinds { SectionKind::UCBFeedback, SectionKind::CBSummaryBaseline, SectionKind::ICsBaseline };

static constexpr int directoryFlags = O_RDONLY | O_DIRECTORY;
static constexpr int pinnedParentFlags = O_PATH | O_DIRECTORY;

static void closeKeepingErrno(int fd)
{
    int error = errno;
    ::close(fd);
    errno = error;
}

// With the lock held, a lock file shorter than 64 bytes, or with another tag or version, is set up in place.
static int setUpLockFile(int fd)
{
    struct stat status;
    if (fstat(fd, &status))
        return errno;
    if (status.st_size >= static_cast<off_t>(lockFileBytes)) {
        std::array<uint8_t, lockFileHeaderBytes> head { };
        if (int error = readExactly(fd, head, 0))
            return error;
        uint32_t version = 0;
        memcpySpan(asMutableByteSpan(version), std::span { head }.subspan(lockFileVersionOffset, sizeof(version)));
        if (equalSpans(std::span { head }.first(lockFileTag.size()), std::span { lockFileTag }) && version == lockFileLayoutVersion)
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

static bool isDotOrDotDot(std::span<const char> name)
{
    return (name.size() == 1 && name[0] == '.') || (name.size() == 2 && name[0] == '.' && name[1] == '.');
}

static uint64_t pageRounded(uint64_t bytes)
{
    uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return (bytes + page - 1) / page * page;
}

#endif // OS(LINUX)

// A body's record in a mapped pack, checked against the entry that names it: the record header repeats the entry's key,
// size and commit identifier, then the body's own checks run in the read's mode with the entry's key as the expected key.
struct ReadBody {
    std::span<const uint8_t> bytes;
    BodyLayout layout;
};
static std::expected<ReadBody, StoreFailure> readBody(const MappedPack& pack, const PackIndexEntry& entry, std::span<const uint8_t, 16> headerDigest,
    ValidationMode mode, std::optional<std::span<const SectionKind>> checksummedKinds)
{
    auto header = decodePackRecordHeader(pack.bytes(entry.offset, packRecordHeaderBytes).first<packRecordHeaderBytes>());
    if (!header)
        return std::unexpected(StoreFailure { ContainerChecks::pack, 0, header.error() });
    if (header->key != entry.key || header->size != entry.size || header->version != entry.version)
        return std::unexpected(StoreFailure { ContainerChecks::pack, 0, "a record differs from the index entry that names it"_s });
    auto bytes = pack.bytes(entry.offset + packRecordHeaderBytes, entry.size);
    auto layout = checksummedKinds ? validateBodyFraming(bytes, entry.key, headerDigest, mode, *checksummedKinds) : validateBody(bytes, entry.key, headerDigest, mode);
    if (!layout)
        return std::unexpected(StoreFailure { layout.error(), 0, { } });
    if (mode == ValidationMode::Full && !packEntryRepeatsEnvelope(entry, *layout))
        return std::unexpected(StoreFailure { ContainerChecks::pack, 0, "an index entry does not repeat its body's envelope"_s });
    return ReadBody { bytes, *layout };
}

static std::span<const uint8_t> sectionIn(std::span<const uint8_t> body, const BodyLayout& layout, SectionKind kind)
{
    auto& extent = layout.sections[static_cast<size_t>(kind)];
    if (!extent)
        return { };
    return body.subspan(static_cast<size_t>(extent->offset), static_cast<size_t>(extent->size));
}

} // namespace ArtifactStoreInternal

// Names.

std::array<char, bodyKeyHexLength> bodyKeyHexDigits(const BodyKey& key)
{
    std::array<char, bodyKeyHexLength> digits { };
    ArtifactStoreInternal::writeLowercaseHex(std::span { digits }, key.bytes());
    return digits;
}

std::optional<BodyKey> bodyKeyFromHex(std::span<const char> digits)
{
    using namespace ArtifactStoreInternal;
    if (digits.size() != bodyKeyHexLength)
        return std::nullopt;
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    for (size_t index = 0; index < bytes.size(); ++index) {
        auto high = lowercaseHexDigitValue(digits[2 * index]);
        auto low = lowercaseHexDigitValue(digits[2 * index + 1]);
        if (!high || !low)
            return std::nullopt;
        bytes[index] = static_cast<uint8_t>(*high << 4 | *low);
    }
    return BodyKey::fromBytes(bytes);
}

TemporaryFileName temporaryFileName(TemporaryKind kind)
{
    using namespace ArtifactStoreInternal;
    std::array<uint8_t, temporaryRandomBytes> random { };
    cryptographicallyRandomValues(random);
    std::span<const char> prefix = kind == TemporaryKind::Header ? std::span<const char> { headerTemporaryPrefix } : std::span<const char> { packTemporaryPrefix };
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
    if (matches(packTemporaryPrefix))
        return TemporaryKind::Pack;
    return std::nullopt;
}

int listDirectory(int directoryFd, const ScopedLambda<void(std::span<const char> name, uint64_t inode)>& visit)
{
#if !OS(LINUX)
    UNUSED_PARAM(directoryFd);
    UNUSED_PARAM(visit);
    return ENOSYS;
#else
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
#endif
}

int removeReplacedArtifact(int parentFd, uint64_t& bytesRemoved)
{
#if !OS(LINUX)
    UNUSED_PARAM(parentFd);
    UNUSED_PARAM(bytesRemoved);
    return ENOSYS;
#else
    using namespace ArtifactStoreInternal;
    int replacedFd = openAt(parentFd, ArtifactNames::replacedCacheDirectory.characters(), directoryFlags);
    if (replacedFd < 0)
        return errno == ENOENT ? 0 : errno;

    auto removeFile = [&](const char* name) -> int {
        struct stat status;
        bool sized = !::fstatat(replacedFd, name, &status, AT_SYMLINK_NOFOLLOW);
        if (::unlinkat(replacedFd, name, 0))
            return errno == ENOENT ? 0 : errno;
        if (sized)
            bytesRemoved += static_cast<uint64_t>(status.st_size);
        return 0;
    };
    // Names are collected first and removed after the listing, so no entry goes while the directory is read.
    Vector<CString> temporaries;
    int error = listDirectory(replacedFd, [&](std::span<const char> name, uint64_t) {
        if (temporaryKindOfFileName(name))
            temporaries.append(CString { name });
    });
    for (auto& name : temporaries) {
        if (error)
            break;
        error = removeFile(name.data());
    }
    if (!error)
        error = removeFile(ArtifactNames::pack.characters());
    if (!error)
        error = removeFile(ArtifactNames::header.characters());
    ::close(replacedFd);
    if (!error && ::unlinkat(parentFd, ArtifactNames::replacedCacheDirectory.characters(), AT_REMOVEDIR))
        error = errno;
    return error;
#endif
}

int createEmptyPack(int cacheFd, std::span<const uint8_t, 16> headerDigest)
{
#if !OS(LINUX)
    UNUSED_PARAM(cacheFd);
    UNUSED_PARAM(headerDigest);
    return ENOSYS;
#else
    using namespace ArtifactStoreInternal;
    PackPrefix prefix;
    memcpySpan(std::span { prefix.headerDigest }, headerDigest);
    prefix.identifier = freshPackIdentifier();
    std::array<uint8_t, emptyPackBytes> bytes { };
    auto span = std::span { bytes };
    auto prefixBytes = encodePackPrefix(prefix, emptyPackBytes);
    memcpySpan(span.first(packPrefixBytes), std::span<const uint8_t> { prefixBytes });
    sealPackIndexBlock({ PackIndexKind::Full, 0, 0 }, packPrefixBytes, span.subspan(packPrefixBytes));

    TemporaryFileName temporary = temporaryFileName(TemporaryKind::Pack);
    int fd = openAt(cacheFd, temporary.data(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return errno;
    int error = writeExactly(fd, bytes, 0);
    if (::close(fd) && !error && errno != EINTR)
        error = errno;
    if (!error && ::renameat(cacheFd, temporary.data(), cacheFd, ArtifactNames::pack.characters()))
        error = errno;
    if (error)
        ::unlinkat(cacheFd, temporary.data(), 0);
    return error;
#endif
}

bool packHoldsBodies(int cacheFd)
{
#if !OS(LINUX)
    UNUSED_PARAM(cacheFd);
    return true;
#else
    using namespace ArtifactStoreInternal;
    int fd = openAt(cacheFd, ArtifactNames::pack.characters(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return errno != ENOENT;
    // The caller holds the producer lock, so no writer moves the root during the read. Every record pushes the valid end
    // past the empty pack's, so a root equal to it names only the empty full index.
    uint64_t validEnd = 0;
    int error = readExactly(fd, asMutableByteSpan(validEnd), packRootOffset);
    ::close(fd);
    return error || validEnd != emptyPackBytes;
#endif
}

// The producer lock.

ProducerLock::ProducerLock(int fd)
    : m_fd(fd)
{
}

std::expected<std::unique_ptr<ProducerLock>, ProducerLock::Failure> ProducerLock::tryAcquire(int parentFd)
{
#if !OS(LINUX)
    UNUSED_PARAM(parentFd);
    return std::unexpected(Failure { false, ENOSYS });
#else
    using namespace ArtifactStoreInternal;
    int fd = openAt(parentFd, ArtifactNames::lockFile.characters(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return std::unexpected(Failure { false, errno });

    int result;
    do {
        result = flock(fd, LOCK_EX | LOCK_NB);
    } while (result && errno == EINTR);
    if (result) {
        int error = errno;
        ::close(fd);
        return std::unexpected(Failure { error == EWOULDBLOCK, error });
    }
    // Closing the descriptor releases the lock, so every failure from here on leaves the lock free.
    if (int error = setUpLockFile(fd)) {
        ::close(fd);
        return std::unexpected(Failure { false, error });
    }
    return std::unique_ptr<ProducerLock>(new ProducerLock(fd));
#endif
}

ProducerLock::~ProducerLock()
{
#if OS(LINUX)
    ::close(m_fd);
#else
    UNUSED_VARIABLE(m_fd);
#endif
}

// The index's hash and traits.

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

// Reading a pack's index.

static uint64_t recordExtentOf(const PackIndexEntry& entry)
{
    // Every applied entry's extent was bounded by a valid end, so it cannot overflow.
    return *packRecordExtent(entry.size);
}

uint64_t packLiveBytes(const PackIndex& index)
{
    uint64_t bytes = packPrefixBytes;
    for (auto& entry : index.values())
        bytes += recordExtentOf(entry);
    return bytes;
}

std::expected<PackIndexRead, PackRejection> readPackIndex(uint64_t validEnd, std::optional<uint64_t> stopAt, PackIndex& index,
    uint64_t& liveBytes, const ScopedLambda<std::span<const uint8_t>(uint64_t offset, uint64_t size)>& readAt)
{
    auto trailerAt = [&](uint64_t offset) {
        return decodePackIndexTrailer(readAt(offset, packIndexTrailerBytes).first<packIndexTrailerBytes>());
    };
    // The valid bytes end with an index block, so the shortest pack is the prefix and an empty full index.
    if (validEnd % packAlignment || validEnd < packPrefixBytes + packIndexBlockBytes(0))
        return std::unexpected("the root's valid end cannot end a pack"_s);
    PackIndexRead result;
    result.latestBlock = trailerAt(validEnd - packIndexTrailerBytes);

    // Back from the latest block to the full index, or to the block the caller applied last. The latest block ends exactly
    // at the valid end and every earlier one at or before the start of the block after it, so offsets strictly decrease
    // along the chain, which bounds the walk. The block the caller applied last is checked too, so a valid end that moved
    // without a block of its own fails.
    Vector<std::pair<uint64_t, PackIndexBlock>, 16> chain;
    uint64_t at = result.latestBlock;
    uint64_t limit = validEnd; // where the block at `at` must end: exactly for the latest one, at the latest for the others
    for (bool latest = true;; latest = false) {
        if (at < packPrefixBytes || at % packAlignment || at >= limit || limit - at < packIndexBlockBytes(0))
            return std::unexpected("an index block lies outside the valid bytes"_s);
        auto block = decodePackIndexBlockHeader(readAt(at, packIndexBlockHeaderBytes).first<packIndexBlockHeaderBytes>());
        if (!block)
            return std::unexpected(block.error());
        uint64_t extent = packIndexBlockBytes(block->count);
        if (extent > limit - at)
            return std::unexpected("an index block passes the valid end or the block after it"_s);
        if (latest && extent != limit - at)
            return std::unexpected("the valid end's trailer gives a block that does not end there"_s);
        if (trailerAt(at + extent - packIndexTrailerBytes) != at)
            return std::unexpected("an index block's trailer does not give its start"_s);
        if (stopAt && at == *stopAt)
            break;
        chain.append({ at, *block });
        if (block->kind == PackIndexKind::Full) {
            result.reachedFull = true;
            break;
        }
        if (block->previous >= at)
            return std::unexpected("an index block's previous block does not precede it"_s);
        limit = at;
        at = block->previous;
    }

    if (result.reachedFull) {
        index.clear();
        liveBytes = packPrefixBytes;
    }
    for (size_t position = chain.size(); position--;) {
        auto [offset, block] = chain[position];
        auto header = readAt(offset, packIndexBlockHeaderBytes).first<packIndexBlockHeaderBytes>();
        auto entries = readAt(offset + packIndexBlockHeaderBytes, static_cast<uint64_t>(block.count) * packIndexEntryBytes);
        if (auto rejection = checkPackIndexBlockCRC(header, entries))
            return std::unexpected(*rejection);
        std::optional<BodyKey> previousKey;
        for (size_t ordinal = 0; ordinal < block.count; ++ordinal) {
            auto entry = decodePackIndexEntry(entries.subspan(ordinal * packIndexEntryBytes, packIndexEntryBytes));
            if (!entry)
                return std::unexpected(entry.error());
            if (block.kind == PackIndexKind::Full) {
                if (entry->isTombstone())
                    return std::unexpected("a full index lists a tombstone"_s);
                if (previousKey && !std::ranges::lexicographical_compare(previousKey->bytes(), entry->key.bytes()))
                    return std::unexpected("a full index's keys do not strictly increase"_s);
                previousKey = entry->key;
            }
            auto replaced = index.find(entry->key);
            if (replaced != index.end()) {
                liveBytes -= recordExtentOf(replaced->value);
                index.remove(replaced);
            }
            if (entry->isTombstone())
                continue;
            auto extent = packRecordExtent(entry->size);
            if (!extent || entry->offset >= offset || *extent > offset - entry->offset)
                return std::unexpected("an index entry's record does not precede its block"_s);
            index.add(entry->key, *entry);
            liveBytes += *extent;
        }
        ++result.blocks;
        if (block.kind == PackIndexKind::Delta)
            ++result.deltasSinceFull;
    }
    return result;
}

// The pack's mapping.

MappedPack::MappedPack(std::span<const uint8_t> mapping, uint64_t inode, uint64_t identifier)
    : m_mapping(mapping)
    , m_inode(inode)
    , m_identifier(identifier)
{
}

std::expected<PackMapping, ArtifactReadFailure> MappedPack::map(int cacheFd, std::span<const uint8_t, 16> headerDigest, uint64_t minimumCapacity)
{
#if !OS(LINUX)
    UNUSED_PARAM(cacheFd);
    UNUSED_PARAM(headerDigest);
    UNUSED_PARAM(minimumCapacity);
    return std::unexpected(ArtifactReadFailure { ENOSYS, { } });
#else
    using namespace ArtifactStoreInternal;
    if (int error = injectedError(StoreCall::PackMap))
        return std::unexpected(ArtifactReadFailure { error, { } });
    int fd = openAt(cacheFd, ArtifactNames::pack.characters(), O_RDONLY);
    if (fd < 0)
        return std::unexpected(ArtifactReadFailure { errno, { } });
    struct stat status;
    if (fstat(fd, &status)) {
        closeKeepingErrno(fd);
        return std::unexpected(ArtifactReadFailure { errno, { } });
    }
    uint64_t fileSize = static_cast<uint64_t>(status.st_size);
    if (fileSize < packPrefixBytes) {
        ::close(fd);
        return std::unexpected(ArtifactReadFailure { 0, "the file is shorter than the pack's prefix"_s });
    }
    std::array<uint8_t, packRetiredOffset> prefixBytes { };
    if (int error = readExactly(fd, prefixBytes, 0)) {
        ::close(fd);
        return std::unexpected(ArtifactReadFailure { error, { } });
    }
    auto prefix = decodePackPrefix(prefixBytes, headerDigest);
    if (!prefix) {
        ::close(fd);
        return std::unexpected(ArtifactReadFailure { 0, prefix.error(), isOtherPackLayout(prefixBytes) });
    }
    // Room for the file and for growth past it; the file grows into the mapping, and the pages past its end are never read.
    uint64_t wanted = std::max(fileSize, minimumCapacity);
    if (wanted > std::numeric_limits<uint64_t>::max() / 4) {
        ::close(fd);
        return std::unexpected(ArtifactReadFailure { EFBIG, { } });
    }
    uint64_t capacity = pageRounded(wanted + std::max(wanted, packMappingSlackBytes));
    if (capacity > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        ::close(fd);
        return std::unexpected(ArtifactReadFailure { EFBIG, { } });
    }
    void* address = mmap(nullptr, static_cast<size_t>(capacity), PROT_READ, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        closeKeepingErrno(fd);
        return std::unexpected(ArtifactReadFailure { errno, { } });
    }
    auto mapping = unsafeMakeSpan(static_cast<const uint8_t*>(address), static_cast<size_t>(capacity));
    Ref<MappedPack> pack = adoptRef(*new MappedPack(mapping, static_cast<uint64_t>(status.st_ino), prefix->identifier));

    // The producer extends the file before it stores a valid end, so the file's size taken after loading the root holds
    // it. A root past that size is damage, and rejecting it keeps every read inside the file, since the mapped pages past
    // the file's end fault.
    uint64_t validEnd = pack->validEnd();
    if (validEnd > fileSize) {
        if (fstat(fd, &status)) {
            closeKeepingErrno(fd);
            return std::unexpected(ArtifactReadFailure { errno, { } });
        }
        fileSize = static_cast<uint64_t>(status.st_size);
    }
    ::close(fd);
    if (validEnd > fileSize)
        return std::unexpected(ArtifactReadFailure { 0, "the root's valid end passes the file"_s });
    return PackMapping { WTF::move(pack), validEnd };
#endif
}

MappedPack::~MappedPack()
{
#if OS(LINUX)
    munmap(const_cast<uint8_t*>(m_mapping.data()), m_mapping.size());
#endif
}

std::span<const uint8_t> MappedPack::bytes(uint64_t offset, uint64_t size) const
{
    RELEASE_ASSERT(offset <= m_mapping.size() && size <= m_mapping.size() - offset);
    return m_mapping.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}

uint64_t MappedPack::loadWord(size_t offset) const
{
    // A producer in this process or another may be storing the word: an aligned 8-byte load sees it whole.
    return __atomic_load_n(reinterpret_cast<const uint64_t*>(m_mapping.subspan(offset, sizeof(uint64_t)).data()), __ATOMIC_ACQUIRE);
}

uint64_t MappedPack::validEnd() const
{
    return loadWord(packRootOffset);
}

bool MappedPack::isRetired() const
{
    return !!loadWord(packRetiredOffset);
}

// Saved summaries.

SavedSummaries::SavedSummaries(Ref<MappedPack>&& pack, uint64_t version, uint8_t highestTier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics)
    : m_pack(WTF::move(pack))
    , m_version(version)
    , m_highestTier(highestTier)
    , m_ucbFeedback(ucbFeedback)
    , m_cbSummary(cbSummary)
    , m_ics(ics)
{
}

SavedSummaries::~SavedSummaries() = default;

// The opened artifact.

OpenedArtifact::OpenedArtifact(int parentFd, int cacheFd, const std::array<uint8_t, 16>& headerDigest)
    : m_parentFd(parentFd)
    , m_cacheFd(cacheFd)
    , m_headerDigest(headerDigest)
{
}

std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> OpenedArtifact::create(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes)
{
#if !OS(LINUX)
    UNUSED_PARAM(parentFd);
    UNUSED_PARAM(cacheFd);
    UNUSED_PARAM(headerBytes);
    return std::unexpected(ArtifactReadFailure { ENOSYS, { } });
#else
    using namespace ArtifactStoreInternal;
    // The object pins descriptors of its own, so it outlives the descriptors of the VM that built it.
    int pinnedParentFd = openAt(parentFd, ".", pinnedParentFlags);
    if (pinnedParentFd < 0)
        return std::unexpected(ArtifactReadFailure { errno, { } });
    int pinnedCacheFd = openAt(cacheFd, ".", directoryFlags);
    if (pinnedCacheFd < 0) {
        closeKeepingErrno(pinnedParentFd);
        return std::unexpected(ArtifactReadFailure { errno, { } });
    }
    Ref<OpenedArtifact> artifact = adoptRef(*new OpenedArtifact(pinnedParentFd, pinnedCacheFd, artifactHeaderDigest(headerBytes)));

    // The pack is mapped and its index read here, at the demand a role creates when it starts, so no mapping and no full
    // index read land inside a request point.
    std::optional<ArtifactReadFailure> failure;
    {
        OpenedArtifact& object = artifact.get();
        Locker locker { object.m_indexLock };
        MonotonicTime start = MonotonicTime::now();
        failure = object.reopen(0);
        object.m_statistics.buildNanoseconds = (MonotonicTime::now() - start).nanosecondsAs<uint64_t>();
        object.m_statistics.reopens = 0;
    }
    if (failure)
        return std::unexpected(*failure);
    return artifact;
#endif
}

OpenedArtifact::~OpenedArtifact()
{
#if OS(LINUX)
    ::close(m_cacheFd);
    ::close(m_parentFd);
#else
    UNUSED_VARIABLE(m_parentFd);
#endif
}

std::optional<ArtifactReadFailure> OpenedArtifact::reopen(uint64_t minimumCapacity)
{
    auto mapped = MappedPack::map(m_cacheFd, headerDigest(), minimumCapacity);
    // The file grew past the mapping between the size that sized it and the root: map it again with room for that root.
    // Each mapping has room for twice the root that asked for it, so only a pack that doubles meanwhile asks again.
    while (mapped && mapped->validEnd > mapped->pack->capacity())
        mapped = MappedPack::map(m_cacheFd, headerDigest(), mapped->validEnd);
    if (!mapped)
        return mapped.error();
    Ref<MappedPack> pack = WTF::move(mapped->pack);
    uint64_t validEnd = mapped->validEnd;
    PackIndex index;
    uint64_t liveBytes = packPrefixBytes;
    auto read = readPackIndex(validEnd, std::nullopt, index, liveBytes, [&](uint64_t offset, uint64_t size) {
        return pack->bytes(offset, size);
    });
    if (!read)
        return ArtifactReadFailure { 0, read.error() };

    m_pack = WTF::move(pack);
    m_validEnd = validEnd;
    m_latestBlock = read->latestBlock;
    m_index = WTF::move(index);
    m_liveBytes = liveBytes;
    m_deltasSinceFull = read->deltasSinceFull;
    m_damage = std::nullopt;
    m_gone = false;
    m_retiredSince = MonotonicTime { };
    m_nextRetiredReopen = MonotonicTime { };
    ++m_statistics.reopens;
    m_statistics.appliedBlocks += read->blocks;
    return std::nullopt;
}

std::optional<StoreFailure> OpenedArtifact::applyValidEnd(uint64_t validEnd)
{
    if (validEnd > m_pack->capacity()) {
        // The pack outgrew the mapping: a new, larger one; bodies borrowed from the old one keep it.
        if (auto failure = reopen(validEnd)) {
            if (failure->error) {
                ++m_statistics.failedReopens;
                return std::nullopt;
            }
            m_damage = StoreFailure { ContainerChecks::pack, 0, failure->packRejection };
            return m_damage;
        }
        return std::nullopt;
    }
#if OS(LINUX)
    // A host that rewrites the pack in place, instead of renaming a new file over it, can leave the file shorter than the
    // root it holds, and the mapped pages past the file's end fault. The producer extends the file before it stores a valid
    // end, so a root past the file's current size is damage. A path that names another file now leaves the mapped one as
    // it was.
    struct stat status;
    if (!fstatat(m_cacheFd, ArtifactNames::pack.characters(), &status, 0) && static_cast<uint64_t>(status.st_ino) == m_pack->inode()
        && validEnd > static_cast<uint64_t>(status.st_size)) {
        m_damage = StoreFailure { ContainerChecks::pack, 0, "the root's valid end passes the file"_s };
        return m_damage;
    }
#endif
    RefPtr<MappedPack> pack = m_pack;
    // Blocks are applied in place: a failure marks the object damaged for good, so the index is never read again.
    auto read = readPackIndex(validEnd, m_latestBlock, m_index, m_liveBytes, [&](uint64_t offset, uint64_t size) {
        return pack->bytes(offset, size);
    });
    if (!read) {
        m_damage = StoreFailure { ContainerChecks::pack, 0, read.error() };
        return m_damage;
    }
    m_deltasSinceFull = read->reachedFull ? read->deltasSinceFull : m_deltasSinceFull + read->deltasSinceFull;
    m_validEnd = validEnd;
    m_latestBlock = read->latestBlock;
    m_statistics.appliedBlocks += read->blocks;
    return std::nullopt;
}

std::optional<StoreFailure> OpenedArtifact::follow()
{
#if !OS(LINUX)
    return std::nullopt;
#else
    using namespace ArtifactStoreInternal;
    if (m_damage)
        return m_damage;
    if (m_gone || !m_pack)
        return std::nullopt;

    if (m_pack->isRetired()) {
        // A compaction or a whole deletion replaced the file: the path names its successor, or nothing.
        MonotonicTime now = MonotonicTime::now();
        if (now < m_nextRetiredReopen)
            return std::nullopt;
        MonotonicTime start = now;
        struct stat status;
        if (fstatat(m_cacheFd, ArtifactNames::pack.characters(), &status, 0)) {
            if (errno == ENOENT) {
                m_gone = true;
                m_index.clear();
                m_liveBytes = packPrefixBytes;
                return std::nullopt;
            }
        } else if (static_cast<uint64_t>(status.st_ino) != m_pack->inode()) {
            auto failure = reopen(0);
            ++m_statistics.refreshes;
            m_statistics.refreshNanoseconds += (MonotonicTime::now() - start).nanosecondsAs<uint64_t>();
            if (!failure)
                return std::nullopt;
            if (!failure->error) {
                m_damage = StoreFailure { ContainerChecks::pack, 0, failure->packRejection };
                return m_damage;
            }
        }
        // The rename has not happened yet, or a compaction died between its mark and its rename, which the next producer
        // or compact repairs: the mapped pack stays current until then. A rename follows its mark within microseconds, so
        // every read looks again during the first second, and a mark older than that is looked at once per interval.
        ++m_statistics.failedReopens;
        if (!m_retiredSince)
            m_retiredSince = now;
        else if (now - m_retiredSince > 1_s)
            m_nextRetiredReopen = now + Seconds::fromMilliseconds(static_cast<double>(retiredPackReopenIntervalMilliseconds));
    } else
        m_retiredSince = MonotonicTime { };

    uint64_t validEnd = m_pack->validEnd();
    if (validEnd == m_validEnd)
        return std::nullopt;
    if (validEnd < m_validEnd) {
        // Within a file the valid end only grows.
        m_damage = StoreFailure { ContainerChecks::pack, 0, "the root's valid end moved back"_s };
        return m_damage;
    }
    MonotonicTime start = MonotonicTime::now();
    auto failure = applyValidEnd(validEnd);
    ++m_statistics.refreshes;
    m_statistics.refreshNanoseconds += (MonotonicTime::now() - start).nanosecondsAs<uint64_t>();
    return failure;
#endif
}

uint64_t OpenedArtifact::token(const BodyKey& key)
{
    Locker locker { m_indexLock };
    follow();
    if (m_gone)
        return 0;
    auto iterator = m_index.find(key);
    return iterator == m_index.end() ? 0 : iterator->value.version;
}

bool OpenedArtifact::containsKey(const BodyKey& key)
{
    return !!token(key);
}

uint64_t OpenedArtifact::indexedBodies()
{
    Locker locker { m_indexLock };
    return m_gone ? 0 : m_index.size();
}

IndexStatistics OpenedArtifact::statistics()
{
    Locker locker { m_indexLock };
    IndexStatistics statistics = m_statistics;
    statistics.bodies = m_gone ? 0 : m_index.size();
    statistics.packBytes = m_validEnd;
    statistics.liveBytes = m_liveBytes;
    statistics.tableBytes = static_cast<uint64_t>(m_index.capacity()) * sizeof(PackIndex::KeyValuePairType);
    return statistics;
}

std::span<const uint8_t, 16> OpenedArtifact::headerDigest() const
{
    return std::span<const uint8_t, 16> { m_headerDigest };
}

int OpenedArtifact::cacheFd() const
{
    return m_cacheFd;
}

BodyOpen OpenedArtifact::open(const BodyKey& key, ValidationMode mode)
{
    using namespace ArtifactStoreInternal;
    if (int error = injectedError(StoreCall::Open))
        return { outcomeOfError(error), nullptr, outcomeOfError(error) == StoreOutcome::Invalid ? failureOfError(error) : StoreFailure { }, 0 };

    RefPtr<MappedPack> pack;
    PackIndexEntry entry;
    {
        Locker locker { m_indexLock };
        if (auto damage = follow())
            return { StoreOutcome::Invalid, nullptr, *damage, 0 };
        auto iterator = m_index.find(key);
        if (m_gone || iterator == m_index.end())
            return { StoreOutcome::Absent, nullptr, { }, 0 };
        entry = iterator->value;
        pack = m_pack;
        ++m_statistics.opens;
    }

    auto read = readBody(*pack, entry, headerDigest(), mode, std::nullopt);
    if (!read)
        return { StoreOutcome::Invalid, nullptr, read.error(), entry.size };
    ValidatedBody::SectionSpans spans;
    for (size_t index = 0; index < numberOfSectionKinds; ++index)
        spans[index] = sectionIn(read->bytes, read->layout, static_cast<SectionKind>(index));
    Ref<ValidatedBody> body = adoptRef(*new ValidatedBody(key, read->layout.version, read->layout.highestTier, read->layout.llintThreshold,
        read->layout.counterProgress, pack.releaseNonNull(), read->bytes, spans));
    return { StoreOutcome::Found, WTF::move(body), { }, entry.size };
}

SavedSummaryRead OpenedArtifact::readSavedSummaries(const BodyKey& key, ValidationMode mode)
{
#if !OS(LINUX)
    UNUSED_PARAM(key);
    UNUSED_PARAM(mode);
    return { StoreOutcome::Invalid, nullptr, ArtifactStoreInternal::failureOfError(ENOSYS) };
#else
    using namespace ArtifactStoreInternal;
    if (int error = injectedError(StoreCall::ReadSavedSummaries))
        return { outcomeOfError(error), nullptr, outcomeOfError(error) == StoreOutcome::Invalid ? failureOfError(error) : StoreFailure { } };

    RefPtr<MappedPack> pack;
    PackIndexEntry entry;
    {
        Locker locker { m_indexLock };
        if (auto damage = follow())
            return { StoreOutcome::Invalid, nullptr, *damage };
        auto iterator = m_index.find(key);
        if (m_gone || iterator == m_index.end())
            return { StoreOutcome::Absent, nullptr, { } };
        entry = iterator->value;
        pack = m_pack;
        ++m_statistics.opens;
    }

    auto read = readBody(*pack, entry, headerDigest(), mode, std::span<const SectionKind> { summarySectionKinds });
    if (!read)
        return { StoreOutcome::Invalid, nullptr, read.error() };
    auto& layout = read->layout;
    auto summaries = std::unique_ptr<SavedSummaries>(new SavedSummaries(pack.releaseNonNull(), layout.version, layout.highestTier,
        sectionIn(read->bytes, layout, SectionKind::UCBFeedback), sectionIn(read->bytes, layout, SectionKind::CBSummaryBaseline),
        sectionIn(read->bytes, layout, SectionKind::ICsBaseline)));
    return { StoreOutcome::Found, WTF::move(summaries), { } };
#endif
}

// The registry.

ArtifactRegistry& ArtifactRegistry::singleton()
{
    static LazyNeverDestroyed<ArtifactRegistry> registry;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        registry.construct();
    });
    return registry.get();
}

std::expected<Ref<OpenedArtifact>, ArtifactReadFailure> ArtifactRegistry::take(int parentFd, int cacheFd, std::span<const uint8_t> headerBytes)
{
#if !OS(LINUX)
    UNUSED_PARAM(parentFd);
    UNUSED_PARAM(cacheFd);
    UNUSED_PARAM(headerBytes);
    return std::unexpected(ArtifactReadFailure { ENOSYS, { } });
#else
    struct stat status;
    if (fstat(cacheFd, &status))
        return std::unexpected(ArtifactReadFailure { errno, { } });
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
            HeaderDigest digest = artifactHeaderDigest(headerBytes);
            ASSERT(equalSpans(existing->headerDigest(), std::span { digest }));
#endif
            return existing.releaseNonNull();
        }
    }

    auto created = OpenedArtifact::create(parentFd, cacheFd, headerBytes);
    if (!created || !shares)
        return created;

    RefPtr<OpenedArtifact> registered;
    {
        Locker locker { registry.m_lock };
        auto& entry = registry.m_artifacts.add(identity, nullptr).iterator->value;
        registered = entry.get();
        if (!registered) {
            entry = created->get();
            registered = created->ptr();
        }
    }
    return registered.releaseNonNull();
#endif
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

} // namespace StoreTesting
#endif

} // namespace JSC::JITCache
