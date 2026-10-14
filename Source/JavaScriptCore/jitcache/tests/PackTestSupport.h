#pragma once

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "JITCacheContainer.h"
#include "JITCacheTest.h"
#include "ProducerBudget.h"
#include "UCBKeys.h"
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What the store's and the writer's tests share: a temporary artifact holding an empty pack, the producer lock, the
// shared opened artifact and a writer over it, and bodies built from a seed.

namespace JSC::JITCache::Tests::PackTestSupport {

inline String errnoText(int error)
{
    return String::fromUTF8(strerror(error));
}

// The store reads no header file: start checks the header and hands its bytes to ArtifactRegistry::take, which digests
// them. Any bytes stand for a header here.
inline std::span<const uint8_t> headerBytes()
{
    static const std::array<uint8_t, 48> bytes = [] {
        std::array<uint8_t, 48> result { };
        for (size_t index = 0; index < result.size(); ++index)
            result[index] = static_cast<uint8_t>(5 * index + 3);
        return result;
    }();
    return bytes;
}

inline HeaderDigest headerDigest()
{
    return artifactHeaderDigest(headerBytes());
}

// A program body's key whose identity digest starts from the seed, so distinct seeds give distinct keys.
inline BodyKey key(uint16_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>((seed >> ((index & 1) * 8)) ^ (29 * (index - 8)));
    auto parsed = BodyKey::fromBytes(bytes);
    RELEASE_ASSERT(parsed);
    return *parsed;
}

// The sections of a body of highest tier 1 in this build, each filled from the seed; scale multiplies their sizes.
struct BodySections {

    std::array<Vector<uint8_t>, numberOfSectionKinds> bytes;
    Vector<SectionSource> sources;

    CommitSections sections() const { return CommitSections { sources.span() }; }
};

inline std::unique_ptr<BodySections> bodySections(uint8_t seed, size_t scale = 1)
{
    static constexpr std::array<size_t, numberOfSectionKinds> sizes { 24, 40, 17, 64, 8, 12, 33, 9, 20 };
    auto body = std::make_unique<BodySections>(); // a plain struct of the tests, which WTF's allocators need not know
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (!isSectionRequired(kind, 1))
            continue;
        for (size_t offset = 0; offset < sizes[index] * scale; ++offset)
            body->bytes[index].append(static_cast<uint8_t>(seed + 13 * index + offset));
        body->sources.append(SectionSource::inMemory(kind, body->bytes[index].span()));
    }
    return body;
}

inline std::expected<CommitResult, CommitFailure> commitBody(ArtifactWriter& writer, const BodyKey& key, uint8_t seed, size_t scale = 1)
{
    auto body = bodySections(seed, scale);
    return writer.commit({ key, 500, seed, 1 }, body->sections());
}

// Whether the opened body holds exactly the sections bodySections(seed) builds.
inline bool holdsSections(const ValidatedBody& body, uint8_t seed, size_t scale = 1)
{
    auto expected = bodySections(seed, scale);
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        if (!equalSpans(body.section(static_cast<SectionKind>(index)), expected->bytes[index].span()))
            return false;
    }
    return true;
}

// A temporary <parent> with cache/, an empty pack, the producer lock and the process's shared opened artifact, removed
// with everything in it when the object goes.
class PackArtifact {
    WTF_MAKE_NONCOPYABLE(PackArtifact);
public:
    static std::unique_ptr<PackArtifact> create(TestContext& context, size_t limitBytes = 64 << 20, size_t stagingBytes = 4096)
    {
        const char* temporaryDirectory = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(temporaryDirectory && *temporaryDirectory ? temporaryDirectory : "/tmp"), "/jitcache-pack-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        std::unique_ptr<PackArtifact> artifact(new PackArtifact(String::fromUTF8(path.span().data())));
        artifact->m_parentFd = ::open(path.span().data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (artifact->m_parentFd < 0
            || mkdirat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), 0755)
            || (artifact->m_cacheFd = openat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot lay out a test artifact: "_s, errnoText(errno)));
            return nullptr;
        }
        if (int error = createEmptyPack(artifact->m_cacheFd, std::span<const uint8_t, 16> { headerDigest() })) {
            JITCACHE_FAIL(makeString("createEmptyPack failed: "_s, errnoText(error)));
            return nullptr;
        }
        auto lock = ProducerLock::tryAcquire(artifact->m_parentFd);
        if (!lock) {
            JITCACHE_FAIL(makeString("cannot take the producer lock: "_s, errnoText(lock.error().error)));
            return nullptr;
        }
        artifact->m_lock = WTF::move(*lock);
        artifact->m_artifact = artifact->take(context);
        if (!artifact->m_artifact)
            return nullptr;
        artifact->m_budget = ProducerBudget::create(limitBytes);
        artifact->m_writer = makeUnique<ArtifactWriter>(*artifact->m_artifact, *artifact->m_lock, *artifact->m_budget, stagingBytes);
        return artifact;
    }

    ~PackArtifact()
    {
        m_writer = nullptr;
        m_artifact = nullptr;
        m_lock = nullptr;
        for (int fd : { m_cacheFd, m_parentFd }) {
            if (fd >= 0)
                ::close(fd);
        }
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    OpenedArtifact& artifact() { return *m_artifact; }
    ArtifactWriter& writer() { return *m_writer; }
    ProducerBudget& budget() { return *m_budget; }
    int cacheFd() const { return m_cacheFd; }
    int parentFd() const { return m_parentFd; }
    const String& path() const { return m_path; }

    // The registry's object for cache/, or, with sharing off, a new one standing for another process's view.
    RefPtr<OpenedArtifact> take(TestContext& context)
    {
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, headerBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, taken.error().error ? errnoText(taken.error().error) : String { taken.error().packRejection }));
            return nullptr;
        }
        return WTF::move(*taken);
    }

    RefPtr<OpenedArtifact> takeAnother(TestContext& context)
    {
        StoreTesting::setRegistrySharing(false);
        auto other = take(context);
        StoreTesting::setRegistrySharing(true);
        return other;
    }

    std::optional<Vector<uint8_t>> readPack() const
    {
        int fd = openat(m_cacheFd, ArtifactNames::pack.characters(), O_RDONLY | O_CLOEXEC);
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

    // Overwrites bytes of cache/pack in place, as damage or another process would.
    bool writePackAt(uint64_t offset, std::span<const uint8_t> bytes)
    {
        int fd = openat(m_cacheFd, ArtifactNames::pack.characters(), O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        bool written = pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset)) == static_cast<ssize_t>(bytes.size());
        ::close(fd);
        return written;
    }

    // The pack's current root, read from the file.
    std::optional<PackRoot> root() const
    {
        auto bytes = readPack();
        if (!bytes || bytes->size() < packPrefixBytes)
            return std::nullopt;
        std::optional<PackRoot> best;
        for (uint64_t slot = 0; slot < 2; ++slot) {
            auto root = decodePackRoot(bytes->span().subspan(packRootSlotOffset(slot)).first<packRootSlotBytes>());
            if (root && (!best || root->generation > best->generation))
                best = root;
        }
        return best;
    }

    uint64_t packFileSize() const
    {
        struct stat status;
        if (fstatat(m_cacheFd, ArtifactNames::pack.characters(), &status, 0))
            return 0;
        return static_cast<uint64_t>(status.st_size);
    }

private:
    explicit PackArtifact(String&& path)
        : m_path(WTF::move(path))
    {
    }

    String m_path;
    int m_parentFd { -1 };
    int m_cacheFd { -1 };
    std::unique_ptr<ProducerLock> m_lock;
    RefPtr<OpenedArtifact> m_artifact;
    RefPtr<ProducerBudget> m_budget;
    std::unique_ptr<ArtifactWriter> m_writer;
};

} // namespace JSC::JITCache::Tests::PackTestSupport

#endif // ENABLE(JITCACHE_TWINS)
