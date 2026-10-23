#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/base/BodyKey.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/maintenance/JITCacheMaintenanceTesting.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What the tests of JITCacheMaintenance (maintenance/JITCacheMaintenance.h and .cpp) share: the scope that turns
// maintenance's hooks off, a temporary directory whose bodies the writer committed to the pack, with the states a test lays
// out there by hand, and the checks and text of a report. The names live in a namespace of their own, since names such as
// Layout and describe are generic.

namespace JSC::JITCache::Tests::MaintenanceTestSupport {

// Maintenance's hooks are process-wide, and the tests of a group share a process, so every test turns them off when it ends.
class HookScope {
    WTF_MAKE_NONCOPYABLE(HookScope);
public:
    HookScope() = default;
    ~HookScope()
    {
        Maintenance::Testing::setFailingBodyUnlink(std::nullopt);
        Maintenance::Testing::setStopAfterUnlink(std::nullopt);
        Maintenance::Testing::setFailingCompaction(false);
    }
};

// A body key from its canonical bytes: version 1, a program body, call specialization, no mode bits, and an identity
// digest drawn from the seed, so distinct seeds give distinct keys.
inline BodyKey maintenanceKey(uint8_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(11 * seed + 29 * (index - 8) + 3);
    auto key = BodyKey::fromBytes(bytes);
    RELEASE_ASSERT(key);
    return *key;
}

// A header that passes its own checks. Maintenance never compares it with the running process or with an app, so crafted
// facts serve; another CPU feature vector or another app gives another header and digest.
inline ArtifactHeaderBytes maintenanceHeader(uint64_t cpuFeatures = 0x15, std::optional<uint64_t> app = std::nullopt)
{
    ArtifactHeader header;
    header.architecture = static_cast<uint8_t>(HeaderArchitecture::X86_64);
    header.facts.mainExecutable.size = 20;
    for (size_t index = 0; index < header.facts.mainExecutable.size; ++index)
        header.facts.mainExecutable.bytes[index] = static_cast<uint8_t>(0xA0 + index);
    header.facts.mustMatch = { false, true, false };
    header.facts.cpuFeatures = cpuFeatures;
    header.app = app;
    auto bytes = encodeArtifactHeader(header);
    RELEASE_ASSERT(bytes);
    return *bytes;
}

inline Vector<uint8_t> filler(size_t size, uint8_t seed)
{
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(size);
    for (size_t offset = 0; offset < size; ++offset)
        bytes.append(static_cast<uint8_t>(seed + 7 * offset + (offset >> 8)));
    return bytes;
}

inline bool writeFileAt(TestContext& context, int directoryFd, const char* name, std::span<const uint8_t> bytes)
{
    int fd = openat(directoryFd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot create "_s, String::fromUTF8(name), ": "_s, errnoText(errno)));
        return false;
    }
    while (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            JITCACHE_FAIL(makeString("cannot write "_s, String::fromUTF8(name), ": "_s, errnoText(errno)));
            ::close(fd);
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    ::close(fd);
    return true;
}

inline std::optional<Vector<uint8_t>> readFileAt(int directoryFd, const char* name)
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

// A body the writer committed, with the terms compact orders it by.
struct CommittedBody {
    BodyKey key;
    uint64_t bytes; // B, the file's size
    uint64_t version; // the envelope's commit identifier; 0 for a body whose envelope fails
    double score; // (L + P) / B; -infinity for a body whose envelope fails
};

// The order compact evicts in: by score, then by key bytes, both ascending.
inline Vector<CommittedBody> inEvictionOrder(Vector<CommittedBody> bodies)
{
    std::ranges::sort(bodies, [](const CommittedBody& a, const CommittedBody& b) {
        if (a.score != b.score)
            return a.score < b.score;
        return std::ranges::lexicographical_compare(a.key.bytes(), b.key.bytes());
    });
    return bodies;
}

inline uint64_t totalBytes(std::span<const CommittedBody> bodies)
{
    uint64_t total = 0;
    for (auto& body : bodies)
        total += body.bytes;
    return total;
}

enum class Layout : uint8_t {
    Parent, // <parent> alone
    Remnant, // cache/ without a header or a pack
    Artifact, // cache/, an empty pack and a header
};

// A temporary <parent>, removed with everything in it when the object goes. Bodies are committed through the writer,
// under the producer lock it takes and releases around each commit, so a maintenance call can take the lock in between.
class MaintenanceArtifact {
    WTF_MAKE_NONCOPYABLE(MaintenanceArtifact);
public:
    static std::unique_ptr<MaintenanceArtifact> create(TestContext& context, Layout layout = Layout::Artifact, const ArtifactHeaderBytes& header = maintenanceHeader())
    {
        const char* temporaryDirectory = getenv("TMPDIR");
        UTF8CString pattern = makeString(String::fromUTF8(temporaryDirectory && *temporaryDirectory ? temporaryDirectory : "/tmp"), "/jitcache-maintenance-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        std::unique_ptr<MaintenanceArtifact> artifact(new MaintenanceArtifact(String::fromUTF8(path.span().data()), header));
        artifact->m_parentFd = ::open(path.span().data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (artifact->m_parentFd < 0) {
            JITCACHE_FAIL(makeString("cannot open the test parent: "_s, errnoText(errno)));
            return nullptr;
        }
        if (layout == Layout::Parent)
            return artifact;
        if (mkdirat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), 0755)
            || (artifact->m_cacheFd = openat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot lay out a test artifact: "_s, errnoText(errno)));
            return nullptr;
        }
        if (layout == Layout::Artifact) {
            auto digest = artifact->headerDigest();
            if (int error = createEmptyPack(artifact->m_cacheFd, std::span<const uint8_t, 16> { digest })) {
                JITCACHE_FAIL(makeString("createEmptyPack failed: "_s, errnoText(error)));
                return nullptr;
            }
            if (!artifact->writeHeader(context, artifact->headerBytes()))
                return nullptr;
        }
        return artifact;
    }

    ~MaintenanceArtifact()
    {
        for (int fd : { m_cacheFd, m_parentFd }) {
            if (fd >= 0)
                ::close(fd);
        }
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    const String& path() const { return m_path; }
    UTF8CString pathArgument() const { return m_path.utf8(); }
    int parentFd() const { return m_parentFd; }
    int cacheFd() const { return m_cacheFd; }
    std::span<const uint8_t> headerBytes() const LIFETIME_BOUND { return m_header.span(); }
    HeaderDigest headerDigest() const { return artifactHeaderDigest(headerBytes()); }

    bool writeHeader(TestContext& context, std::span<const uint8_t> bytes)
    {
        return writeFileAt(context, m_cacheFd, ArtifactNames::header.characters(), bytes);
    }

    // Another header, which the bodies committed under the first no longer match.
    bool replaceHeader(TestContext& context, const ArtifactHeaderBytes& header)
    {
        m_header = header;
        return writeHeader(context, headerBytes());
    }

    // A commit through the writer, as a producer of this artifact makes it: every section of a tier-1 body holds
    // sectionBytes bytes, and the envelope holds L and P.
    std::optional<CommittedBody> commit(TestContext& context, const BodyKey& key, uint32_t llintThreshold, uint32_t counterProgress, size_t sectionBytes)
    {
        auto lock = ProducerLock::tryAcquire(m_parentFd);
        if (!lock) {
            JITCACHE_FAIL(makeString("cannot take the producer lock to commit: "_s, lock.error().busy ? "busy"_s : errnoText(lock.error().error)));
            return std::nullopt;
        }
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, headerBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, taken.error().error ? errnoText(taken.error().error) : String { taken.error().packRejection }));
            return std::nullopt;
        }
        Ref<OpenedArtifact> object = WTF::move(*taken);
        Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();

        std::array<Vector<uint8_t>, numberOfSectionKinds> sections;
        Vector<SectionSource, numberOfSectionKinds> sources;
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            auto kind = static_cast<SectionKind>(index);
            if (!isSectionRequired(kind, 1))
                continue;
            sections[index] = filler(sectionBytes, static_cast<uint8_t>(key.bytes()[8] + 13 * index + llintThreshold + counterProgress));
            sources.append(SectionSource::inMemory(kind, sections[index].span()));
        }

        // The writer goes before the budget, the object and the lock it refers to.
        ArtifactWriter writer(object.get(), **lock, budget.get(), 4096);
        auto committed = writer.commit(CommitStamp { key, llintThreshold, counterProgress, 1 }, CommitSections { sources.span() });
        if (!committed) {
            JITCACHE_FAIL(makeString("the commit failed at "_s, committed.error().check, ": "_s, committed.error().detail));
            return std::nullopt;
        }
        double score = (static_cast<double>(llintThreshold) + static_cast<double>(counterProgress)) / static_cast<double>(committed->fileSize);
        return CommittedBody { key, committed->fileSize, committed->version, score };
    }

    // Flips a byte of the pack's last index block, which its CRC then rejects.
    bool damagePack(TestContext& context)
    {
        auto bytes = readFileAt(m_cacheFd, ArtifactNames::pack.characters());
        if (!bytes || bytes->size() < 100) {
            JITCACHE_FAIL("the artifact holds no pack to damage"_s);
            return false;
        }
        (*bytes)[bytes->size() - 30] ^= 0x01;
        return writeFileAt(context, m_cacheFd, ArtifactNames::pack.characters(), bytes->span());
    }

    // Writes another layout version into the pack's prefix, as a binary that writes that layout would leave it. The
    // prefix's CRC, which covers the version, then differs too, as it would where another layout keeps its CRC elsewhere.
    bool setPackLayoutVersion(TestContext& context, uint16_t version)
    {
        auto bytes = readFileAt(m_cacheFd, ArtifactNames::pack.characters());
        if (!bytes || bytes->size() < packPrefixBytes) {
            JITCACHE_FAIL("the artifact holds no pack to relabel"_s);
            return false;
        }
        memcpySpan(bytes->mutableSpan().subspan(8, sizeof(version)), asByteSpan(version));
        return writeFileAt(context, m_cacheFd, ArtifactNames::pack.characters(), bytes->span());
    }

    // The bytes of every file in cache/: the artifact's size on disk.
    uint64_t artifactBytes() const
    {
        uint64_t total = 0;
        for (const char* directory : { "cache" }) {
            auto listed = names(directory);
            if (!listed)
                continue;
            for (auto& name : *listed) {
                struct stat status;
                UTF8CString path = makeString(String::fromUTF8(directory), '/', name).utf8();
                if (!fstatat(m_parentFd, path.legacyCStringPointer(), &status, AT_SYMLINK_NOFOLLOW) && S_ISREG(status.st_mode))
                    total += static_cast<uint64_t>(status.st_size);
            }
        }
        return total;
    }

    // A temporary of that kind in cache/, as a producer killed mid-commit leaves one.
    bool placeTemporary(TestContext& context, TemporaryKind kind, size_t bytes)
    {
        return writeFileAt(context, m_cacheFd, temporaryFileName(kind).data(), filler(bytes, 0x33).span());
    }

    bool placeFile(TestContext& context, int directoryFd, const char* name, size_t bytes)
    {
        return writeFileAt(context, directoryFd, name, filler(bytes, 0x44).span());
    }

    // Whether a name exists, by its path from <parent>.
    bool exists(const char* path) const
    {
        struct stat status;
        return !fstatat(m_parentFd, path, &status, AT_SYMLINK_NOFOLLOW);
    }

    bool exists(const String& path) const { return exists(path.utf8().legacyCStringPointer()); }

    // A fresh reader's view of the pack, which the registry does not share, or null without a pack that passes its checks.
    RefPtr<OpenedArtifact> freshView() const
    {
        if (m_cacheFd < 0)
            return nullptr;
        StoreTesting::setRegistrySharing(false);
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, headerBytes());
        StoreTesting::setRegistrySharing(true);
        if (!taken)
            return nullptr;
        return WTF::move(*taken);
    }

    // Whether the pack's index lists a body for the key.
    bool holdsBody(const BodyKey& key) const
    {
        RefPtr view = freshView();
        return view && view->token(key);
    }

    // The bodies the pack's index lists, 0 without a pack or for one that fails its checks.
    uint64_t bodiesInPack() const
    {
        RefPtr view = freshView();
        return view ? view->indexedBodies() : 0;
    }

    size_t currentBodies() const { return bodiesInPack(); }

    // The pack's valid end and file size.
    uint64_t packValidEnd() const
    {
        RefPtr view = freshView();
        return view ? view->statistics().packBytes : 0;
    }

    uint64_t packFileSize() const
    {
        struct stat status;
        if (fstatat(m_cacheFd, ArtifactNames::pack.characters(), &status, AT_SYMLINK_NOFOLLOW))
            return 0;
        return static_cast<uint64_t>(status.st_size);
    }

    // Bytes a commit wrote past the valid end before it stopped.
    bool appendGarbage(TestContext& context, size_t bytes)
    {
        int fd = openat(m_cacheFd, ArtifactNames::pack.characters(), O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd < 0) {
            JITCACHE_FAIL(makeString("cannot open the pack: "_s, errnoText(errno)));
            return false;
        }
        auto garbage = filler(bytes, 0x5C);
        bool written = ::write(fd, garbage.span().data(), garbage.size()) == static_cast<ssize_t>(garbage.size());
        ::close(fd);
        return written;
    }

    // The names a fresh listing of a directory under <parent> finds, or nothing when it cannot be listed.
    std::optional<Vector<String>> names(const char* path) const
    {
        int fd = openat(m_parentFd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            return std::nullopt;
        Vector<String> result;
        int error = listDirectory(fd, [&](std::span<const char> name, uint64_t) {
            result.append(String::fromUTF8(name));
        });
        ::close(fd);
        if (error)
            return std::nullopt;
        return result;
    }

    size_t temporariesInCache() const
    {
        auto listed = names("cache");
        if (!listed)
            return SIZE_MAX;
        size_t count = 0;
        for (auto& name : *listed) {
            UTF8CString characters = name.utf8();
            if (temporaryKindOfFileName(byteCast<char>(characters.span())))
                ++count;
        }
        return count;
    }

    // The store's view of what remains, as a VM that opens the artifact afterwards builds it.
    RefPtr<OpenedArtifact> take(TestContext& context)
    {
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, headerBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, taken.error().error ? errnoText(taken.error().error) : String { taken.error().packRejection }));
            return nullptr;
        }
        return WTF::move(*taken);
    }

private:
    MaintenanceArtifact(String&& path, const ArtifactHeaderBytes& header)
        : m_path(WTF::move(path))
        , m_header(header)
    {
    }

    const String m_path;
    ArtifactHeaderBytes m_header;
    int m_parentFd { -1 };
    int m_cacheFd { -1 };
};

// Commits count bodies with distinct scores and returns them in eviction order.
inline std::optional<Vector<CommittedBody>> commitBodies(TestContext& context, MaintenanceArtifact& artifact, unsigned count, uint8_t firstSeed = 1)
{
    Vector<CommittedBody> bodies;
    for (unsigned index = 0; index < count; ++index) {
        auto body = artifact.commit(context, maintenanceKey(static_cast<uint8_t>(firstSeed + index)), 500, 40 * index, 64 + 8 * index);
        if (!body)
            return std::nullopt;
        bodies.append(*body);
    }
    return inEvictionOrder(WTF::move(bodies));
}

inline ASCIILiteral outcomeText(Maintenance::Outcome outcome)
{
    switch (outcome) {
    case Maintenance::Outcome::Done:
        return "Done"_s;
    case Maintenance::Outcome::NoArtifact:
        return "NoArtifact"_s;
    case Maintenance::Outcome::Busy:
        return "Busy"_s;
    case Maintenance::Outcome::NeedsConfirmation:
        return "NeedsConfirmation"_s;
    case Maintenance::Outcome::Declined:
        return "Declined"_s;
    case Maintenance::Outcome::PlanChanged:
        return "PlanChanged"_s;
    case Maintenance::Outcome::Failed:
        return "Failed"_s;
    }
    return "unknown"_s;
}

inline String describe(const Maintenance::Report& report)
{
    String text = makeString(outcomeText(report.outcome), " with "_s, report.temporariesRemoved, " temporaries removed, "_s, report.bodiesEvicted,
        " bodies evicted, "_s, report.bytesReclaimed, " bytes reclaimed"_s);
    for (auto& diagnostic : report.diagnostics)
        text = makeString(text, "; "_s, diagnostic.code, ": "_s, diagnostic.detail);
    return text;
}

inline bool checkOutcome(TestContext& context, const String& label, const Maintenance::Report& report, Maintenance::Outcome expected)
{
    if (report.outcome == expected)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, outcomeText(expected), ", got "_s, describe(report)));
    return false;
}

inline size_t countDiagnostics(const Maintenance::Report& report, ASCIILiteral code)
{
    return static_cast<size_t>(std::ranges::count_if(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code;
    }));
}

inline bool hasDiagnostic(const Maintenance::Report& report, ASCIILiteral code, const String& detail)
{
    return std::ranges::any_of(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.detail == detail;
    });
}

inline bool hasDiagnosticStartingWith(const Maintenance::Report& report, ASCIILiteral code, const String& prefix)
{
    return std::ranges::any_of(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.detail.startsWith(prefix);
    });
}

// The plan lists exactly these bodies, in this order, with their sizes, versions and scores.
inline void checkEvictions(TestContext& context, const String& label, const Maintenance::Plan& plan, std::span<const CommittedBody> expected)
{
    if (plan.evictions.size() != expected.size()) {
        JITCACHE_FAIL(makeString(label, ": the plan evicts "_s, plan.evictions.size(), " bodies where "_s, expected.size(), " were expected"_s));
        return;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        auto& eviction = plan.evictions[index];
        if (!equalSpans(std::span { eviction.key }, expected[index].key.bytes()))
            JITCACHE_FAIL(makeString(label, ": eviction "_s, index, " is another body"_s));
        else if (eviction.bytes != expected[index].bytes || eviction.version != expected[index].version || eviction.score != expected[index].score)
            JITCACHE_FAIL(makeString(label, ": eviction "_s, index, " holds "_s, eviction.bytes, " bytes, version "_s, eviction.version, " and score "_s, eviction.score));
    }
}

inline Maintenance::Report compactAnswering(MaintenanceArtifact& artifact, double ratio, Maintenance::Answer answer)
{
    Maintenance::CompactOptions options;
    options.answer = answer;
    return Maintenance::compact(artifact.path(), ratio, options);
}

} // namespace JSC::JITCache::Tests::MaintenanceTestSupport

#endif // ENABLE(JITCACHE_TWINS)
