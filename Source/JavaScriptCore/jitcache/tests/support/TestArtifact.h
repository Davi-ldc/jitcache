#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "BodyKey.h"
#include "JITCacheAPI.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCachePlatform.h"
#include "JITCacheTest.h"
#include "ProducerBudget.h"
#include "TestFiles.h"
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What the tests of an artifact share: body keys and bytes built from a seed; a temporary artifact holding an empty pack,
// with the producer lock, the shared opened artifact and a writer over it; the scope that puts the process-wide test
// hooks back; and what a test needs to start JITCache on a VM: its strict configuration, the checks of start's outcome
// and of a fault status reports, and the comparison and text of capture scores.

namespace JSC::JITCache::Tests {

// size bytes drawn from the seed, for any test data whose content matters only through its seed.
inline Vector<uint8_t> patternBytes(size_t size, uint8_t seed)
{
    Vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(seed * 31 + i * 7);
    return bytes;
}

// The store's hooks and the build-ID override are process-wide, and the tests of a group share a process, so a test that
// may set one restores the defaults when it ends.
class ProcessHooksScope {
    WTF_MAKE_NONCOPYABLE(ProcessHooksScope);
public:
    ProcessHooksScope() = default;
    ~ProcessHooksScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
        removeMainBuildIDForTesting(false);
    }
};

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

// The key of a body of the given kind, a program by default, called as a function, built from its canonical bytes as the
// store reads them, so a test needs none of the UCB lane's key derivation: version 1, no mode bits, and an identity digest
// drawn from both bytes of the seed, so distinct seeds give distinct keys of a kind, and seeds below 256 give keys in the
// seeds' order.
inline BodyKey testKey(uint16_t seed, IdentityKind kind = IdentityKind::Program)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(kind);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>((seed >> ((index & 1) * 8)) ^ (29 * (index - 8)));
    auto parsed = BodyKey::fromBytes(bytes);
    RELEASE_ASSERT(parsed);
    return *parsed;
}

inline constexpr size_t testProducerLimitBytes = 64 * MB;

// The configuration a test starts JITCache with: strict, as every test that starts it runs, with testProducerLimitBytes
// for a producing role. The delta capture defaults to Compiled, so delta commits only bodies that compiled and a test
// counts exactly what its compilations commit; a test of the bodies that ran without compiling passes All.
inline Config strictConfig(const String& path, Role role, DeltaCapture deltaCapture = DeltaCapture::Compiled)
{
    Config config;
    config.artifactPath = path;
    config.role = role;
    if (role != Role::Consumer)
        config.producerLimitBytes = testProducerLimitBytes;
    config.strict = true;
    config.deltaCapture = deltaCapture;
    return config;
}

// Whether start returned the outcome, and the step for an outcome that names one. Otherwise the test fails with the label
// and the whole result.
inline bool startsAs(TestContext& context, ASCIILiteral label, const StartResult& result, StartOutcome outcome, ASCIILiteral step = { })
{
    if (result.outcome == outcome && result.step == step)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, name(outcome), step.isNull() ? ""_s : " at "_s, step, ", got "_s, toJSON(result)));
    return false;
}

// That the fault is there, with the class and the step. Otherwise the test fails with the label and what it got.
inline void checkFault(TestContext& context, ASCIILiteral label, const std::optional<FaultReport>& fault, FaultClass faultClass, ASCIILiteral step)
{
    if (!fault) {
        JITCACHE_FAIL(makeString(label, ": no fault, expected "_s, step));
        return;
    }
    if (fault->faultClass != faultClass || fault->stepName() != step)
        JITCACHE_FAIL(makeString(label, ": "_s, name(fault->faultClass), " at "_s, fault->stepName(), ", expected "_s, name(faultClass), " at "_s, step));
}

inline bool sameScore(const CaptureScore& a, const CaptureScore& b)
{
    return a.tier == b.tier && a.richness == b.richness && a.icSitesWithCases == b.icSitesWithCases
        && a.counterWithheld == b.counterWithheld && a.counterProgress == b.counterProgress;
}

// The score as a failure message prints it.
inline String scoreText(const CaptureScore& score)
{
    return makeString("{ tier "_s, static_cast<unsigned>(score.tier), ", richness "_s, score.richness, ", IC sites with cases "_s,
        score.icSitesWithCases, ", counter "_s, score.counterWithheld ? "withheld"_s : "carried"_s, ", progress "_s, score.counterProgress, " }"_s);
}

// The sections of a body of highest tier 1 in this build, each filled from the seed; scale multiplies their sizes. With
// analysisBytes, the body also holds a ucb.analysis section of that many bytes, which only a module's body may hold.
struct BodySections {

    std::array<Vector<uint8_t>, numberOfSectionKinds> bytes;
    Vector<SectionSource> sources;

    CommitSections sections() const { return CommitSections { sources.span() }; }
};

inline std::unique_ptr<BodySections> bodySections(uint8_t seed, size_t scale = 1, size_t analysisBytes = 0)
{
    // By kind; ucb.analysis takes analysisBytes.
    static constexpr std::array<size_t, numberOfSectionKinds> sizes { 24, 40, 17, 0, 64, 8, 12, 33, 9, 20 };
    auto body = std::make_unique<BodySections>(); // a plain struct of the tests, which WTF's allocators need not know
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        bool isAnalysis = kind == SectionKind::UCBAnalysis;
        if (isAnalysis ? !analysisBytes : !isSectionRequired(kind, 1))
            continue;
        size_t size = isAnalysis ? analysisBytes : sizes[index] * scale;
        for (size_t offset = 0; offset < size; ++offset)
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

// Whether the opened body holds exactly the sections bodySections(seed, scale, analysisBytes) builds.
inline bool holdsSections(const ValidatedBody& body, uint8_t seed, size_t scale = 1, size_t analysisBytes = 0)
{
    auto expected = bodySections(seed, scale, analysisBytes);
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

    // The pack's root, its valid end, read from the file.
    std::optional<uint64_t> validEnd() const
    {
        auto bytes = readPack();
        if (!bytes || bytes->size() < packPrefixBytes)
            return std::nullopt;
        uint64_t validEnd = 0;
        memcpySpan(asMutableByteSpan(validEnd), bytes->span().subspan(packRootOffset, sizeof(validEnd)));
        return validEnd;
    }

    // The latest index block's offset: the trailer that ends the valid bytes.
    std::optional<uint64_t> latestBlock() const
    {
        auto bytes = readPack();
        auto end = validEnd();
        if (!bytes || !end || *end < packPrefixBytes + packIndexTrailerBytes || *end > bytes->size())
            return std::nullopt;
        return decodePackIndexTrailer(bytes->span().subspan(*end - packIndexTrailerBytes).first<packIndexTrailerBytes>());
    }

    // Overwrites the root, as damage or another process would.
    bool writeValidEnd(uint64_t validEnd)
    {
        return writePackAt(packRootOffset, asByteSpan(validEnd));
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

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
