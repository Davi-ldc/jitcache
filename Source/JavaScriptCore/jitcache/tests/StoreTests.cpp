#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "JITCacheContainer.h"
#include "JITCacheTest.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <bit>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/WTFString.h>

// Tests C4 to C6 of SPEC-integrator.container.md section 9, and C5's lock: the index with its listings and refreshes, the
// producer lock, and the store's three reads. They read the store's own results, so none needs a VM. Each test works in
// a fresh temporary <parent>; a ProducerLock of the test process stands for the producer or maintenance run of another
// process that bumps the epoch, and the registry hook builds a second OpenedArtifact for a directory where a test needs
// two processes' views of it.

namespace JSC::JITCache::Tests {

namespace StoreTestsInternal {

// The store's hooks are process-wide, and the tests of a group share a process, so every test restores the defaults when
// it ends.
class HookScope {
    WTF_MAKE_NONCOPYABLE(HookScope);
public:
    HookScope() = default;
    ~HookScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
        StoreTesting::setInotify(true);
        StoreTesting::setFallbackListingInterval(std::nullopt);
    }
};

// A body key from its canonical bytes (SPEC-ucb.md section 3.1): version 1, a program body, call specialization, no mode
// bits, and an identity digest whose first byte is the seed, so distinct seeds give distinct keys.
static std::array<uint8_t, BodyKey::byteSize> storeKeyBytes(uint8_t seed, uint8_t version = 1)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = version;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(seed ^ (29 * (index - 8)));
    return bytes;
}

static BodyKey storeKey(uint8_t seed)
{
    auto key = BodyKey::fromBytes(storeKeyBytes(seed));
    RELEASE_ASSERT(key);
    return *key;
}

// The store reads no header file: start checks the header and hands its bytes to ArtifactRegistry::take, which digests
// them. Any bytes stand for a header here.
static std::span<const uint8_t> storeHeaderBytes()
{
    static const std::array<uint8_t, 48> bytes = [] {
        std::array<uint8_t, 48> result { };
        for (size_t index = 0; index < result.size(); ++index)
            result[index] = static_cast<uint8_t>(3 * index + 1);
        return result;
    }();
    return bytes;
}

static HeaderDigest storeHeaderDigest()
{
    return artifactHeaderDigest(storeHeaderBytes());
}

static String describeOutcome(StoreOutcome outcome)
{
    switch (outcome) {
    case StoreOutcome::Absent:
        return "Absent"_s;
    case StoreOutcome::Unavailable:
        return "Unavailable"_s;
    case StoreOutcome::Invalid:
        return "Invalid"_s;
    case StoreOutcome::Found:
        return "Found"_s;
    }
    return "?"_s;
}

static String describeMode(ValidationMode mode)
{
    return mode == ValidationMode::Full ? "Full"_s : "Integrity"_s;
}

// A body as the writer lays it out, holding every section a body of highest tier 1 holds in this build, each non-empty
// and filled from the seed and its kind.
static constexpr std::array<size_t, numberOfSectionKinds> sectionSizes { 24, 40, 17, 64, 8, 12, 33, 9, 20 };

struct SyntheticBody {
    Vector<uint8_t> file;
    std::array<Vector<uint8_t>, numberOfSectionKinds> sections; // by SectionKind; empty for a kind the build's bodies lack
};

static SyntheticBody makeBody(const BodyKey& key, uint64_t version, uint8_t seed, const HeaderDigest& headerDigest)
{
    SyntheticBody body;
    Vector<ContainerTesting::Section> sections;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (!isSectionRequired(kind, 1))
            continue;
        for (size_t offset = 0; offset < sectionSizes[index]; ++offset)
            body.sections[index].append(static_cast<uint8_t>(seed + 13 * index + offset));
        sections.append({ kind, body.sections[index].span() });
    }
    BodyEnvelope stamp;
    stamp.highestTier = 1;
    stamp.key = key;
    stamp.version = version;
    stamp.headerDigest = headerDigest;
    stamp.llintThreshold = 500;
    stamp.counterProgress = seed;
    body.file = ContainerTesting::buildBody(stamp, sections.span());
    return body;
}

static SyntheticBody makeBody(const BodyKey& key, uint64_t version, uint8_t seed)
{
    return makeBody(key, version, seed, storeHeaderDigest());
}

// Where a section of a well-formed synthetic body lies in its file.
static SectionExtent extentOf(const SyntheticBody& body, const BodyKey& key, SectionKind kind)
{
    auto layout = validateBody(body.file.span(), key, storeHeaderDigest(), ValidationMode::Full);
    RELEASE_ASSERT(layout && layout->sections[static_cast<size_t>(kind)]);
    return *layout->sections[static_cast<size_t>(kind)];
}

static bool writeFileAt(TestContext& context, int directoryFd, const char* name, std::span<const uint8_t> bytes)
{
    int fd = openat(directoryFd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot create "_s, String::fromUTF8(name), ": "_s, String::fromUTF8(strerror(errno))));
        return false;
    }
    while (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            JITCACHE_FAIL(makeString("cannot write "_s, String::fromUTF8(name), ": "_s, String::fromUTF8(strerror(errno))));
            ::close(fd);
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    ::close(fd);
    return true;
}

static std::optional<Vector<uint8_t>> readFileAt(int directoryFd, const char* name)
{
    int fd = openat(directoryFd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    Vector<uint8_t> bytes;
    std::array<uint8_t, 4096> buffer;
    while (true) {
        ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        bytes.append(std::span { buffer }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return bytes;
}

enum class LockFile : bool { Absent, Present };

// A temporary <parent> with cache/ and cache/bodies/, removed with everything in it when the object goes. With
// LockFile::Present it also holds the producer lock, through which the test bumps the epoch as another process's producer
// or maintenance run would.
class TestArtifact {
    WTF_MAKE_NONCOPYABLE(TestArtifact);
public:
    static std::unique_ptr<TestArtifact> create(TestContext& context, LockFile lockFile)
    {
        const char* temporaryDirectory = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(temporaryDirectory && *temporaryDirectory ? temporaryDirectory : "/tmp"), "/jitcache-store-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, String::fromUTF8(strerror(errno))));
            return nullptr;
        }
        std::unique_ptr<TestArtifact> artifact(new TestArtifact(String::fromUTF8(path.span().data())));
        artifact->m_parentFd = ::open(path.span().data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (artifact->m_parentFd < 0
            || mkdirat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), 0755)
            || (artifact->m_cacheFd = openat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0
            || mkdirat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), 0755)
            || (artifact->m_bodiesFd = openat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot lay out a test artifact: "_s, String::fromUTF8(strerror(errno))));
            return nullptr;
        }
        if (lockFile == LockFile::Present) {
            auto lock = ProducerLock::tryAcquire(artifact->m_parentFd);
            if (!lock) {
                JITCACHE_FAIL(makeString("cannot take the producer lock: "_s, String::fromUTF8(strerror(lock.error().error))));
                return nullptr;
            }
            artifact->m_lock = WTF::move(*lock);
        }
        return artifact;
    }

    ~TestArtifact()
    {
        m_lock = nullptr;
        for (int fd : { m_bodiesFd, m_cacheFd, m_parentFd }) {
            if (fd >= 0)
                ::close(fd);
        }
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    int parentFd() const { return m_parentFd; }
    int cacheFd() const { return m_cacheFd; }
    int bodiesFd() const { return m_bodiesFd; }

    RefPtr<OpenedArtifact> take(TestContext& context)
    {
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, storeHeaderBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, String::fromUTF8(strerror(taken.error()))));
            return nullptr;
        }
        return WTF::move(*taken);
    }

    // Writes a temporary in cache/ and renames it into bodies/ under name, as the writer publishes.
    bool publish(TestContext& context, const char* name, std::span<const uint8_t> bytes)
    {
        TemporaryFileName temporary = temporaryFileName(TemporaryKind::Body);
        if (!writeFileAt(context, m_cacheFd, temporary.data(), bytes))
            return false;
        if (renameat(m_cacheFd, temporary.data(), m_bodiesFd, name)) {
            JITCACHE_FAIL(makeString("cannot rename a body into bodies/: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
        return true;
    }

    bool publishBody(TestContext& context, const BodyKey& key, std::span<const uint8_t> bytes)
    {
        return publish(context, bodyFileName(key).data(), bytes);
    }

    bool unlinkBody(TestContext& context, const BodyKey& key)
    {
        if (unlinkat(m_bodiesFd, bodyFileName(key).data(), 0)) {
            JITCACHE_FAIL(makeString("cannot unlink a body: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
        return true;
    }

    bool renameBody(TestContext& context, const BodyKey& from, const BodyKey& to)
    {
        if (renameat(m_bodiesFd, bodyFileName(from).data(), m_bodiesFd, bodyFileName(to).data())) {
            JITCACHE_FAIL(makeString("cannot rename a body: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
        return true;
    }

    // Removes bodies/, which must be empty, and with removeCache cache/ too.
    bool removeBodies(TestContext& context, bool removeCache)
    {
        if (unlinkat(m_cacheFd, ArtifactNames::bodiesDirectory.characters(), AT_REMOVEDIR)
            || (removeCache && unlinkat(m_parentFd, ArtifactNames::cacheDirectory.characters(), AT_REMOVEDIR))) {
            JITCACHE_FAIL(makeString("cannot remove a directory: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
        return true;
    }

    // Creates bodies/ anew in cache/ after removeBodies, and points bodiesFd() at the new directory.
    bool recreateBodies(TestContext& context)
    {
        ::close(m_bodiesFd);
        m_bodiesFd = -1;
        if (mkdirat(m_cacheFd, ArtifactNames::bodiesDirectory.characters(), 0755)
            || (m_bodiesFd = openat(m_cacheFd, ArtifactNames::bodiesDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot recreate bodies/: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
        return true;
    }

    // Stands for another process's commit or maintenance run.
    void bump()
    {
        RELEASE_ASSERT(m_lock);
        m_lock->bumpEpoch();
    }

private:
    explicit TestArtifact(String&& path)
        : m_path(WTF::move(path))
    {
    }

    const String m_path;
    int m_parentFd { -1 };
    int m_cacheFd { -1 };
    int m_bodiesFd { -1 };
    std::unique_ptr<ProducerLock> m_lock;
};

// The lock file of container sub-SPEC section 2, written byte by byte: the tag, layout version 1, four zero bytes, the
// epoch and 40 zero bytes.
static Vector<uint8_t> lockFileImage(uint64_t epoch)
{
    Vector<uint8_t> bytes(FillWith { }, 64, 0);
    static constexpr std::array<uint8_t, 8> tag { 'J', 'I', 'T', 'C', 'L', 'O', 'C', 'K' };
    memcpySpan(bytes.mutableSpan().first(tag.size()), std::span { tag });
    bytes[8] = 1;
    for (unsigned index = 0; index < 8; ++index)
        bytes[16 + index] = static_cast<uint8_t>(epoch >> (8 * index));
    return bytes;
}

static std::optional<uint64_t> epochInLockFile(int parentFd)
{
    auto bytes = readFileAt(parentFd, ArtifactNames::lockFile.characters());
    if (!bytes || bytes->size() < 24)
        return std::nullopt;
    uint64_t epoch = 0;
    for (unsigned index = 0; index < 8; ++index)
        epoch |= static_cast<uint64_t>((*bytes)[16 + index]) << (8 * index);
    return epoch;
}

// The kernel's limit on queued inotify events, past which a queue overflows.
static uint64_t inotifyQueueLimit()
{
    constexpr uint64_t defaultLimit = 16384;
    int fd = ::open("/proc/sys/fs/inotify/max_queued_events", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return defaultLimit;
    std::array<char, 32> text { };
    ssize_t count = ::read(fd, text.data(), text.size() - 1);
    ::close(fd);
    uint64_t limit = 0;
    for (ssize_t index = 0; index < count && isASCIIDigit(text[index]); ++index)
        limit = 10 * limit + static_cast<uint64_t>(text[index] - '0');
    return limit ? limit : defaultLimit;
}

// Overflows an object's event queue: a file renamed into bodies/ and back raises two events per round.
static bool overflowEventQueue(TestContext& context, TestArtifact& artifact)
{
    uint64_t limit = inotifyQueueLimit();
    if (limit > (static_cast<uint64_t>(1) << 22)) {
        JITCACHE_FAIL(makeString("the inotify queue limit "_s, limit, " is too large to overflow in a test"_s));
        return false;
    }
    static constexpr const char* churn = "churn";
    if (!writeFileAt(context, artifact.cacheFd(), churn, { }))
        return false;
    for (uint64_t round = 0; round < limit / 2 + 2; ++round) {
        if (renameat(artifact.cacheFd(), churn, artifact.bodiesFd(), churn) || renameat(artifact.bodiesFd(), churn, artifact.cacheFd(), churn)) {
            JITCACHE_FAIL(makeString("cannot rename the churn file: "_s, String::fromUTF8(strerror(errno))));
            return false;
        }
    }
    return true;
}

static void checkSections(TestContext& context, const String& label, const ValidatedBody& body, const SyntheticBody& expected)
{
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto section = body.section(static_cast<SectionKind>(index));
        if (!equalSpans(section, expected.sections[index].span()))
            JITCACHE_FAIL(makeString(label, ": section "_s, sectionKindDescription(static_cast<SectionKind>(index)).name, " differs from the bytes written"_s));
        if (!section.empty() && reinterpret_cast<uintptr_t>(section.data()) % 8)
            JITCACHE_FAIL(makeString(label, ": section "_s, sectionKindDescription(static_cast<SectionKind>(index)).name, " does not start 8-byte aligned"_s));
    }
}

static void checkOpenOutcome(TestContext& context, const String& label, const BodyOpen& opened, StoreOutcome expected, ASCIILiteral expectedCheck = { }, int expectedError = 0)
{
    if (opened.outcome != expected) {
        JITCACHE_FAIL(makeString(label, ": "_s, describeOutcome(expected), " expected, "_s, describeOutcome(opened.outcome), " found"_s));
        return;
    }
    if ((expected == StoreOutcome::Found) != !!opened.body)
        JITCACHE_FAIL(makeString(label, ": the body is present exactly for Found"_s));
    if (expected == StoreOutcome::Invalid && (opened.failure.check != expectedCheck || opened.failure.error != expectedError)) {
        JITCACHE_FAIL(makeString(label, ": the failure is "_s, opened.failure.check, " with errno "_s, opened.failure.error, ", expected "_s,
            expectedCheck, " with errno "_s, expectedError));
    }
}

} // namespace StoreTestsInternal

using namespace StoreTestsInternal;

// The names of section 1.1, on which every listing, open and commit relies.
JITCACHE_TEST(storeNames, No)
{
    BodyKey key = storeKey(0x31);
    BodyFileName name = bodyFileName(key);
    JITCACHE_CHECK(name.span().size() == 84 && !name.characters[84]);
    StringBuilder expected;
    for (uint8_t byte : key.bytes()) {
        expected.append(upperNibbleToLowercaseASCIIHexDigit(byte));
        expected.append(lowerNibbleToLowercaseASCIIHexDigit(byte));
    }
    expected.append(".bin"_s);
    JITCACHE_CHECK(String { name.span() } == expected.toString());
    auto decoded = bodyKeyFromFileName(name.span());
    JITCACHE_CHECK(decoded && *decoded == key);

    // An uppercase digit, another suffix, another length and bytes BodyKey::fromBytes rejects make no body name.
    std::array<char, bodyFileNameLength> uppercase { };
    memcpySpan(std::span { uppercase }, name.span());
    uppercase[0] = 'A';
    JITCACHE_CHECK(!bodyKeyFromFileName(std::span { uppercase }));
    std::array<char, bodyFileNameLength> otherSuffix { };
    memcpySpan(std::span { otherSuffix }, name.span());
    otherSuffix[bodyFileNameLength - 1] = 'x';
    JITCACHE_CHECK(!bodyKeyFromFileName(std::span { otherSuffix }));
    JITCACHE_CHECK(!bodyKeyFromFileName(name.span().first(bodyFileNameLength - 1)));
    BodyFileName rejected = bodyFileName(std::bit_cast<BodyKey>(storeKeyBytes(0x31, 2)));
    JITCACHE_CHECK(!bodyKeyFromFileName(rejected.span()));

    TemporaryFileName bodyTemporary = temporaryFileName(TemporaryKind::Body);
    TemporaryFileName otherBodyTemporary = temporaryFileName(TemporaryKind::Body);
    TemporaryFileName headerTemporary = temporaryFileName(TemporaryKind::Header);
    auto bodyTemporaryName = unsafeSpan(bodyTemporary.data());
    auto headerTemporaryName = unsafeSpan(headerTemporary.data());
    JITCACHE_CHECK(bodyTemporaryName.size() == 37 && bodyTemporaryName[0] == '.');
    JITCACHE_CHECK(headerTemporaryName.size() == 44 && equalSpans(headerTemporaryName.first(8), ".header."_span));
    JITCACHE_CHECK(temporaryKindOfFileName(bodyTemporaryName) == TemporaryKind::Body);
    JITCACHE_CHECK(temporaryKindOfFileName(headerTemporaryName) == TemporaryKind::Header);
    JITCACHE_CHECK(!equalSpans(bodyTemporaryName, unsafeSpan(otherBodyTemporary.data())));
    JITCACHE_CHECK(!temporaryKindOfFileName(name.span()));
    JITCACHE_CHECK(!temporaryKindOfFileName("header"_span));
    JITCACHE_CHECK(!bodyKeyFromFileName(bodyTemporaryName));
}

// C5. A second tryAcquire in the process is busy while the first lock lives and succeeds once it is destroyed.
JITCACHE_TEST(storeProducerLockIsExclusive, No)
{
    auto artifact = TestArtifact::create(context, LockFile::Absent);
    if (!artifact)
        return;
    auto first = ProducerLock::tryAcquire(artifact->parentFd());
    JITCACHE_CHECK(first.has_value());
    auto second = ProducerLock::tryAcquire(artifact->parentFd());
    JITCACHE_CHECK(!second.has_value() && second.error().busy);
    if (first)
        first->reset();
    auto third = ProducerLock::tryAcquire(artifact->parentFd());
    JITCACHE_CHECK(third.has_value());
}

// C5. A lock file shorter than 64 bytes, or with another tag, is set up in place with a zero epoch; a valid one keeps its
// epoch, and bumpEpoch returns the value it replaced, one less than the value a reader then loads.
JITCACHE_TEST(storeProducerLockSetsUpItsFile, No)
{
    auto artifact = TestArtifact::create(context, LockFile::Absent);
    if (!artifact)
        return;
    const char* lockFile = ArtifactNames::lockFile.characters();

    auto expectFreshLockFile = [&](ASCIILiteral label) {
        auto lock = ProducerLock::tryAcquire(artifact->parentFd());
        if (!lock) {
            JITCACHE_FAIL(makeString(label, ": tryAcquire failed"_s));
            return;
        }
        auto bytes = readFileAt(artifact->parentFd(), lockFile);
        if (!bytes || *bytes != lockFileImage(0))
            JITCACHE_FAIL(makeString(label, ": the lock file was not set up in place"_s));
    };

    std::array<uint8_t, 10> shortFile;
    shortFile.fill(0xAB);
    if (!writeFileAt(context, artifact->parentFd(), lockFile, shortFile))
        return;
    expectFreshLockFile("a 10-byte lock file"_s);

    std::array<uint8_t, 64> otherTag;
    otherTag.fill('X');
    if (!writeFileAt(context, artifact->parentFd(), lockFile, otherTag))
        return;
    expectFreshLockFile("a lock file with another tag"_s);

    if (!writeFileAt(context, artifact->parentFd(), lockFile, lockFileImage(41).span()))
        return;
    auto lock = ProducerLock::tryAcquire(artifact->parentFd());
    if (!lock) {
        JITCACHE_FAIL("tryAcquire on a valid lock file failed"_s);
        return;
    }
    JITCACHE_CHECK(epochInLockFile(artifact->parentFd()) == uint64_t { 41 });
    JITCACHE_CHECK((*lock)->bumpEpoch() == 41);
    JITCACHE_CHECK(epochInLockFile(artifact->parentFd()) == uint64_t { 42 });
    JITCACHE_CHECK((*lock)->bumpEpoch() == 42);
    JITCACHE_CHECK(epochInLockFile(artifact->parentFd()) == uint64_t { 43 });
}

// C5. The lock file survives deleting cache/, epoch included.
JITCACHE_TEST(storeLockFileSurvivesCacheDeletion, No)
{
    auto artifact = TestArtifact::create(context, LockFile::Absent);
    if (!artifact)
        return;
    {
        auto lock = ProducerLock::tryAcquire(artifact->parentFd());
        if (!lock) {
            JITCACHE_FAIL("tryAcquire failed"_s);
            return;
        }
        (*lock)->bumpEpoch();
        (*lock)->bumpEpoch();
    }
    if (!artifact->removeBodies(context, true))
        return;
    struct stat status;
    JITCACHE_CHECK(!fstatat(artifact->parentFd(), ArtifactNames::lockFile.characters(), &status, 0) && status.st_size == 64);
    auto lock = ProducerLock::tryAcquire(artifact->parentFd());
    JITCACHE_CHECK(lock.has_value() && (*lock)->bumpEpoch() == 2);
}

// Section 5.1: the registry hands every taker of one directory the same object, and the registry hook builds another.
JITCACHE_TEST(storeRegistrySharesObjects, No)
{
    HookScope hooks;
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    RefPtr first = artifact->take(context);
    RefPtr second = artifact->take(context);
    JITCACHE_CHECK(first && first == second);
    StoreTesting::setRegistrySharing(false);
    RefPtr third = artifact->take(context);
    JITCACHE_CHECK(third && third != first);
}

// C4. A listing finds the body names and ignores every other name; every later listing of the object
// finds the same set with the same tokens; a body replaced by a rename gets a fresh token at the next listing, and an
// untouched one keeps its token. Inotify is forced off with a zero listing interval, so each epoch change lists.
JITCACHE_TEST(storeListingKeepsTokens, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(false);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(1);
    BodyKey b = storeKey(2);
    BodyKey c = storeKey(3);
    BodyKey absent = storeKey(4);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 1).file.span())
        || !artifact->publishBody(context, b, makeBody(b, 2, 2).file.span())
        || !artifact->publishBody(context, c, makeBody(c, 3, 3).file.span()))
        return;

    // A listing ignores every name that is not a body (container sub-SPEC section 6.2): temporaries, a body name with an
    // uppercase digit, a name whose key BodyKey::fromBytes rejects, and any other name.
    BodyFileName uppercase = bodyFileName(absent);
    uppercase.characters[0] = 'A';
    BodyFileName rejected = bodyFileName(std::bit_cast<BodyKey>(storeKeyBytes(5, 2)));
    std::array<uint8_t, 1> oneByte { 0 };
    if (!writeFileAt(context, artifact->bodiesFd(), temporaryFileName(TemporaryKind::Body).data(), oneByte)
        || !writeFileAt(context, artifact->bodiesFd(), temporaryFileName(TemporaryKind::Header).data(), oneByte)
        || !writeFileAt(context, artifact->bodiesFd(), uppercase.data(), oneByte)
        || !writeFileAt(context, artifact->bodiesFd(), rejected.data(), oneByte)
        || !writeFileAt(context, artifact->bodiesFd(), "notes.txt", oneByte))
        return;

    RefPtr object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->indexedBodies() == 3);
    uint64_t tokenA = object->token(a);
    uint64_t tokenB = object->token(b);
    uint64_t tokenC = object->token(c);
    JITCACHE_CHECK(tokenA && tokenB && tokenC && tokenA != tokenB && tokenB != tokenC && tokenA != tokenC);
    JITCACHE_CHECK(!object->token(absent));

    for (unsigned listing = 0; listing < 2; ++listing) {
        artifact->bump();
        JITCACHE_CHECK(object->token(a) == tokenA && object->token(b) == tokenB && object->token(c) == tokenC);
        JITCACHE_CHECK(object->indexedBodies() == 3);
    }

    if (!artifact->publishBody(context, a, makeBody(a, 9, 9).file.span()))
        return;
    artifact->bump();
    uint64_t replacedA = object->token(a);
    JITCACHE_CHECK(replacedA && replacedA != tokenA);
    JITCACHE_CHECK(object->token(b) == tokenB && object->token(c) == tokenC);
}

// C4. Two objects for one directory stand for two processes. An addition, a replacement, a deletion and a rename are
// found by no lookup before the epoch moves, and are visible to both objects at their first lookup after it.
static void checkChangesAcrossObjects(TestContext& context, bool withInotify)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(withInotify);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(11);
    BodyKey b = storeKey(12);
    BodyKey c = storeKey(13);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 11).file.span()))
        return;
    RefPtr first = artifact->take(context);
    RefPtr second = artifact->take(context);
    if (!first || !second)
        return;
    JITCACHE_CHECK(first != second);
    std::array<RefPtr<OpenedArtifact>, 2> objects { first, second };
    String mode = withInotify ? "with inotify"_s : "with inotify forced off"_s;

    auto expectTokens = [&](ASCIILiteral label, auto&& expectation) {
        for (auto& object : objects) {
            if (!expectation(*object))
                JITCACHE_FAIL(makeString(label, ' ', mode, ": an object's tokens are not as expected"_s));
        }
    };

    // An addition.
    if (!artifact->publishBody(context, b, makeBody(b, 2, 12).file.span()))
        return;
    expectTokens("before the bump that announces an addition"_s, [&](OpenedArtifact& object) { return !object.token(b); });
    artifact->bump();
    expectTokens("after an addition"_s, [&](OpenedArtifact& object) { return !!object.token(b); });

    // A replacement.
    std::array<uint64_t, 2> previousA { first->token(a), second->token(a) };
    if (!artifact->publishBody(context, a, makeBody(a, 3, 13).file.span()))
        return;
    for (size_t index = 0; index < objects.size(); ++index)
        JITCACHE_CHECK(objects[index]->token(a) == previousA[index]);
    artifact->bump();
    for (size_t index = 0; index < objects.size(); ++index) {
        uint64_t token = objects[index]->token(a);
        if (!token || token == previousA[index])
            JITCACHE_FAIL(makeString("after a replacement "_s, mode, ": the body has no fresh token"_s));
    }

    // A deletion.
    if (!artifact->unlinkBody(context, b))
        return;
    expectTokens("before the bump that announces a deletion"_s, [&](OpenedArtifact& object) { return !!object.token(b); });
    artifact->bump();
    expectTokens("after a deletion"_s, [&](OpenedArtifact& object) { return !object.token(b); });

    // A rename.
    if (!artifact->renameBody(context, a, c))
        return;
    expectTokens("before the bump that announces a rename"_s, [&](OpenedArtifact& object) { return object.token(a) && !object.token(c); });
    artifact->bump();
    expectTokens("after a rename"_s, [&](OpenedArtifact& object) { return !object.token(a) && object.token(c); });
}

JITCACHE_TEST(storeObjectsSeeChangesWithInotify, No)
{
    checkChangesAcrossObjects(context, true);
}

JITCACHE_TEST(storeObjectsSeeChangesWithoutInotify, No)
{
    checkChangesAcrossObjects(context, false);
}

// C4. An overflowed event queue makes a listing, and a second overflow another, each finding every body.
JITCACHE_TEST(storeOverflowedQueueLists, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(21);
    BodyKey b = storeKey(22);
    BodyKey c = storeKey(23);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 21).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    // Events past the limit are dropped, the addition's among them, so only a listing finds it.
    if (!overflowEventQueue(context, *artifact) || !artifact->publishBody(context, b, makeBody(b, 2, 22).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(object->token(a) && object->token(b));
    JITCACHE_CHECK(object->indexedBodies() == 2);

    if (!overflowEventQueue(context, *artifact) || !artifact->publishBody(context, c, makeBody(c, 3, 23).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(object->token(a) && object->token(b) && object->token(c));
    JITCACHE_CHECK(object->indexedBodies() == 3);
}

// C4. A listing the store's fault hook fails leaves the index as it was, and the next epoch change lists and finds every
// body.
JITCACHE_TEST(storeFailedListingKeepsIndex, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(false);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(31);
    BodyKey b = storeKey(32);
    BodyKey c = storeKey(33);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 31).file.span()) || !artifact->publishBody(context, b, makeBody(b, 2, 32).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    uint64_t tokenA = object->token(a);
    uint64_t tokenB = object->token(b);

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Listing, EIO, std::nullopt });
    if (!artifact->publishBody(context, c, makeBody(c, 3, 33).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(!object->token(c));
    JITCACHE_CHECK(object->token(a) == tokenA && object->token(b) == tokenB && object->indexedBodies() == 2);

    // The failed listing recorded the epoch it read, so nothing lists until the epoch moves again.
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(!object->token(c));
    artifact->bump();
    JITCACHE_CHECK(object->token(c));
    JITCACHE_CHECK(object->token(a) == tokenA && object->token(b) == tokenB && object->indexedBodies() == 3);
}

// C4. Deleting the directory empties the index, and a directory recreated at its path is another directory, which the
// object never sees.
JITCACHE_TEST(storeDeletedDirectoryEmptiesIndex, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(41);
    BodyKey b = storeKey(42);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 41).file.span()) || !artifact->publishBody(context, b, makeBody(b, 2, 42).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->indexedBodies() == 2);

    if (!artifact->unlinkBody(context, a) || !artifact->unlinkBody(context, b) || !artifact->removeBodies(context, false))
        return;
    artifact->bump();
    JITCACHE_CHECK(!object->token(a) && !object->token(b));
    JITCACHE_CHECK(!object->indexedBodies());

    if (!artifact->recreateBodies(context) || !artifact->publishBody(context, a, makeBody(a, 3, 43).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(!object->token(a));
    JITCACHE_CHECK(object->open(a, ValidationMode::Full).outcome == StoreOutcome::Absent);
}

// C4. Without the lock file at its build, an object never refreshes, even once the file exists and its epoch moves.
JITCACHE_TEST(storeWithoutLockFileNeverRefreshes, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Absent);
    if (!artifact)
        return;
    BodyKey a = storeKey(51);
    BodyKey b = storeKey(52);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 51).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->token(a));

    if (!artifact->publishBody(context, b, makeBody(b, 2, 52).file.span()))
        return;
    JITCACHE_CHECK(!object->token(b));
    auto lock = ProducerLock::tryAcquire(artifact->parentFd());
    if (!lock) {
        JITCACHE_FAIL("tryAcquire failed"_s);
        return;
    }
    (*lock)->bumpEpoch();
    JITCACHE_CHECK(!object->token(b));
}

// C4. With inotify forced off and a one-second interval, an epoch change within the second of the last listing makes no
// listing, so the body it brought is absent until the first lookup after the second has passed, which lists without
// another epoch change.
JITCACHE_TEST(storeListingIntervalDefersListings, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(false);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(61);
    BodyKey b = storeKey(62);
    BodyKey c = storeKey(63);
    if (!artifact->publishBody(context, a, makeBody(a, 1, 61).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    if (!artifact->publishBody(context, b, makeBody(b, 2, 62).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(object->token(b)); // lists now

    StoreTesting::setFallbackListingInterval(Seconds(1));
    if (!artifact->publishBody(context, c, makeBody(c, 3, 63).file.span()))
        return;
    artifact->bump();
    JITCACHE_CHECK(!object->token(c));
    JITCACHE_CHECK(!object->token(c));

    WTF::sleep(Seconds(1.1));
    JITCACHE_CHECK(object->token(c));
    JITCACHE_CHECK(object->token(a) && object->token(b));
}

// C6. token reaches no openat, and neither does open of a key the index lacks: the fault hook, set to fail every call,
// never fires for them.
JITCACHE_TEST(storeTokenAndAbsentOpenMakeNoOpenat, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey present = storeKey(71);
    BodyKey absent = storeKey(72);
    if (!artifact->publishBody(context, present, makeBody(present, 1, 71).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    JITCACHE_CHECK(object->token(present));
    JITCACHE_CHECK(!object->token(absent));
    checkOpenOutcome(context, "open of a key the index lacks"_s, object->open(absent, ValidationMode::Full), StoreOutcome::Absent);
    // The hook is armed: an open of a listed key reaches its openat and fails there.
    BodyOpen failed = object->open(present, ValidationMode::Full);
    checkOpenOutcome(context, "open with the hook set"_s, failed, StoreOutcome::Invalid, "container.io"_s, EIO);
    JITCACHE_CHECK(!failed.mappedBytes);

    // While the epoch stands still, token refreshes nothing, so a failing listing never runs.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Listing, EIO, std::nullopt });
    JITCACHE_CHECK(object->token(present));
    JITCACHE_CHECK(object->indexedBodies() == 1);
}

// C6. open's outcomes on the store's own results: Found in both modes with the file's bytes and commit identifier;
// ENOENT is Absent and erases the entry; a forced EMFILE, ENFILE or ENOMEM is Unavailable and keeps it, counting calls
// from 1 when the hook names one; any other error is Invalid at container.io.
JITCACHE_TEST(storeOpenOutcomes, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(81);
    BodyKey b = storeKey(82);
    SyntheticBody bodyA = makeBody(a, 11, 81);
    if (!artifact->publishBody(context, a, bodyA.file.span()) || !artifact->publishBody(context, b, makeBody(b, 12, 82).file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    for (ValidationMode mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        String label = makeString("open in "_s, describeMode(mode), " mode"_s);
        BodyOpen opened = object->open(a, mode);
        checkOpenOutcome(context, label, opened, StoreOutcome::Found);
        if (!opened.body)
            continue;
        JITCACHE_CHECK(opened.mappedBytes == bodyA.file.size());
        JITCACHE_CHECK(opened.body->key() == a && opened.body->version() == 11 && opened.body->highestTier() == 1);
        JITCACHE_CHECK(opened.body->fileSize() == bodyA.file.size());
        checkSections(context, label, *opened.body, bodyA);
    }

    if (!artifact->unlinkBody(context, b))
        return;
    BodyOpen vanished = object->open(b, ValidationMode::Full);
    checkOpenOutcome(context, "open of an unlinked body"_s, vanished, StoreOutcome::Absent);
    JITCACHE_CHECK(!vanished.mappedBytes);
    JITCACHE_CHECK(!object->token(b));

    for (int error : { EMFILE, ENFILE, ENOMEM }) {
        StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, error, std::nullopt });
        checkOpenOutcome(context, makeString("open with errno "_s, error), object->open(a, ValidationMode::Full), StoreOutcome::Unavailable);
        JITCACHE_CHECK(object->token(a));
    }

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EMFILE, 2 });
    checkOpenOutcome(context, "the first open after the hook"_s, object->open(a, ValidationMode::Full), StoreOutcome::Found);
    checkOpenOutcome(context, "the second open after the hook"_s, object->open(a, ValidationMode::Full), StoreOutcome::Unavailable);
    checkOpenOutcome(context, "the third open after the hook"_s, object->open(a, ValidationMode::Full), StoreOutcome::Found);

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    checkOpenOutcome(context, "open with EIO"_s, object->open(a, ValidationMode::Integrity), StoreOutcome::Invalid, "container.io"_s, EIO);
    JITCACHE_CHECK(object->token(a));
}

// C6. A damaged body gives Invalid at the check of C3, in the modes C3 names it in.
JITCACHE_TEST(storeOpenRejectsDamagedBodies, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;

    struct DamageCase {
        ASCIILiteral label;
        BodyKey key; // the key the file's name encodes
        Vector<uint8_t> file;
        std::optional<ASCIILiteral> integrity; // the failing check in Integrity mode; empty: Found
        std::optional<ASCIILiteral> full;
        uint64_t mappedBytes;
    };
    Vector<DamageCase> cases;

    {
        BodyKey key = storeKey(91);
        SyntheticBody body = makeBody(key, 1, 91);
        SectionExtent image = extentOf(body, key, SectionKind::ImageBaseline);
        body.file[static_cast<size_t>(image.offset)] ^= 0xFF;
        uint64_t size = body.file.size();
        cases.append({ "a flipped image byte"_s, key, WTF::move(body.file), ContainerChecks::checksum, ContainerChecks::checksum, size });
    }
    {
        BodyKey key = storeKey(92);
        Vector<uint8_t> file = makeBody(storeKey(93), 1, 92).file;
        uint64_t size = file.size();
        cases.append({ "another key's body under this key's name"_s, key, WTF::move(file), ContainerChecks::key, ContainerChecks::key, size });
    }
    {
        BodyKey key = storeKey(94);
        HeaderDigest otherDigest;
        otherDigest.fill(0x5A);
        Vector<uint8_t> file = makeBody(key, 1, 94, otherDigest).file;
        uint64_t size = file.size();
        cases.append({ "a body written under another header"_s, key, WTF::move(file), ContainerChecks::headerDigest, ContainerChecks::headerDigest, size });
    }
    {
        BodyKey key = storeKey(95);
        Vector<uint8_t> file = makeBody(key, 1, 95).file;
        file.shrink(100);
        cases.append({ "a file shorter than an envelope"_s, key, WTF::move(file), ContainerChecks::size, ContainerChecks::size, 0 });
    }
    {
        BodyKey key = storeKey(96);
        Vector<uint8_t> file = makeBody(key, 1, 96).file;
        file.shrink(200);
        cases.append({ "a truncated file"_s, key, WTF::move(file), ContainerChecks::size, ContainerChecks::size, 200 });
    }
    {
        BodyKey key = storeKey(97);
        Vector<uint8_t> file = makeBody(key, 0, 97).file;
        uint64_t size = file.size();
        cases.append({ "a zero commit identifier"_s, key, WTF::move(file), std::nullopt, ContainerChecks::version, size });
    }

    for (auto& damage : cases) {
        if (!artifact->publishBody(context, damage.key, damage.file.span()))
            return;
    }
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    for (auto& damage : cases) {
        for (ValidationMode mode : { ValidationMode::Integrity, ValidationMode::Full }) {
            auto expected = mode == ValidationMode::Full ? damage.full : damage.integrity;
            String label = makeString(damage.label, " in "_s, describeMode(mode), " mode"_s);
            BodyOpen opened = object->open(damage.key, mode);
            if (!expected) {
                checkOpenOutcome(context, label, opened, StoreOutcome::Found);
                continue;
            }
            checkOpenOutcome(context, label, opened, StoreOutcome::Invalid, *expected, 0);
            if (opened.mappedBytes != damage.mappedBytes)
                JITCACHE_FAIL(makeString(label, ": mapped "_s, opened.mappedBytes, " bytes, expected "_s, damage.mappedBytes));
            // A damaged body stays in the index: only ENOENT takes a key out.
            JITCACHE_CHECK(object->token(damage.key));
        }
    }
}

// C6. A ValidatedBody keeps returning the same bytes after another version is renamed over its file and after the file
// is unlinked, and its version() is the commit identifier of the file it mapped; an open after the replacement maps the
// new file.
JITCACHE_TEST(storeValidatedBodyOutlivesItsFile, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey key = storeKey(101);
    SyntheticBody first = makeBody(key, 31, 101);
    SyntheticBody second = makeBody(key, 32, 102);
    if (!artifact->publishBody(context, key, first.file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    BodyOpen opened = object->open(key, ValidationMode::Full);
    checkOpenOutcome(context, "the first open"_s, opened, StoreOutcome::Found);
    RefPtr<ValidatedBody> body = opened.body;
    if (!body)
        return;

    if (!artifact->publishBody(context, key, second.file.span()))
        return;
    JITCACHE_CHECK(body->version() == 31);
    checkSections(context, "after a replacement"_s, *body, first);
    BodyOpen reopened = object->open(key, ValidationMode::Full);
    checkOpenOutcome(context, "an open after the replacement"_s, reopened, StoreOutcome::Found);
    if (reopened.body) {
        JITCACHE_CHECK(reopened.body->version() == 32);
        checkSections(context, "the replacement"_s, *reopened.body, second);
    }

    if (!artifact->unlinkBody(context, key))
        return;
    JITCACHE_CHECK(body->version() == 31 && body->fileSize() == first.file.size());
    checkSections(context, "after an unlink"_s, *body, first);
}

// C6. The scoring read opens a body by its name whatever the index holds: a body renamed in without a bump is Found with
// its three summary spans and leaves the index as it is; a key with no file is Absent; a forced EMFILE, ENFILE or ENOMEM
// is Unavailable and keeps the entry; an unlinked file is Absent and erases the entry; a damaged image leaves the scoring
// read Found while open is Invalid at container.checksum; a damaged cb.summary is Invalid at container.checksum.
JITCACHE_TEST(storeScoringRead, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = TestArtifact::create(context, LockFile::Present);
    if (!artifact)
        return;
    BodyKey a = storeKey(111);
    BodyKey damagedImage = storeKey(112);
    BodyKey damagedSummary = storeKey(113);
    BodyKey unindexed = storeKey(114);
    BodyKey absent = storeKey(115);

    SyntheticBody bodyA = makeBody(a, 21, 111);
    SyntheticBody imageBody = makeBody(damagedImage, 22, 112);
    imageBody.file[static_cast<size_t>(extentOf(imageBody, damagedImage, SectionKind::ImageBaseline).offset)] ^= 0xFF;
    SyntheticBody summaryBody = makeBody(damagedSummary, 23, 113);
    summaryBody.file[static_cast<size_t>(extentOf(summaryBody, damagedSummary, SectionKind::CBSummaryBaseline).offset)] ^= 0xFF;
    if (!artifact->publishBody(context, a, bodyA.file.span())
        || !artifact->publishBody(context, damagedImage, imageBody.file.span())
        || !artifact->publishBody(context, damagedSummary, summaryBody.file.span()))
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;

    auto checkSummaries = [&](const String& label, const SavedSummaryRead& read, const SyntheticBody& expected, uint64_t version) {
        if (read.outcome != StoreOutcome::Found || !read.summaries) {
            JITCACHE_FAIL(makeString(label, ": "_s, describeOutcome(read.outcome), " where Found was expected"_s));
            return;
        }
        auto& summaries = *read.summaries;
        JITCACHE_CHECK(summaries.version() == version && summaries.highestTier() == 1);
        JITCACHE_CHECK(equalSpans(summaries.ucbFeedback(), expected.sections[static_cast<size_t>(SectionKind::UCBFeedback)].span()));
        JITCACHE_CHECK(equalSpans(summaries.cbSummary(), expected.sections[static_cast<size_t>(SectionKind::CBSummaryBaseline)].span()));
        JITCACHE_CHECK(equalSpans(summaries.ics(), expected.sections[static_cast<size_t>(SectionKind::ICsBaseline)].span()));
    };

    // Renamed in without a bump: the index lacks it, and the read leaves the index as it is.
    SyntheticBody unindexedBody = makeBody(unindexed, 24, 114);
    if (!artifact->publishBody(context, unindexed, unindexedBody.file.span()))
        return;
    JITCACHE_CHECK(!object->token(unindexed));
    for (ValidationMode mode : { ValidationMode::Integrity, ValidationMode::Full })
        checkSummaries(makeString("a body the index lacks in "_s, describeMode(mode), " mode"_s), object->readSavedSummaries(unindexed, mode), unindexedBody, 24);
    JITCACHE_CHECK(!object->token(unindexed));

    JITCACHE_CHECK(object->readSavedSummaries(absent, ValidationMode::Full).outcome == StoreOutcome::Absent);

    for (int error : { EMFILE, ENFILE, ENOMEM }) {
        StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::ReadSavedSummaries, error, std::nullopt });
        SavedSummaryRead read = object->readSavedSummaries(a, ValidationMode::Full);
        if (read.outcome != StoreOutcome::Unavailable || read.summaries)
            JITCACHE_FAIL(makeString("the scoring read with errno "_s, error, " is "_s, describeOutcome(read.outcome)));
        JITCACHE_CHECK(object->token(a));
    }
    StoreTesting::setFault(std::nullopt);
    checkSummaries("a listed body"_s, object->readSavedSummaries(a, ValidationMode::Full), bodyA, 21);

    for (ValidationMode mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        checkSummaries(makeString("a damaged image in "_s, describeMode(mode), " mode"_s), object->readSavedSummaries(damagedImage, mode), imageBody, 22);
        checkOpenOutcome(context, makeString("open of a damaged image in "_s, describeMode(mode), " mode"_s), object->open(damagedImage, mode),
            StoreOutcome::Invalid, ContainerChecks::checksum, 0);

        SavedSummaryRead read = object->readSavedSummaries(damagedSummary, mode);
        if (read.outcome != StoreOutcome::Invalid || read.failure.check != ContainerChecks::checksum)
            JITCACHE_FAIL(makeString("the scoring read of a damaged cb.summary in "_s, describeMode(mode), " mode is "_s, describeOutcome(read.outcome)));
    }

    if (!artifact->unlinkBody(context, a))
        return;
    JITCACHE_CHECK(object->readSavedSummaries(a, ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(!object->token(a));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
