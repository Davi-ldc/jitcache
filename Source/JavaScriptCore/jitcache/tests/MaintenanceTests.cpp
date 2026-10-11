#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "JITCacheAPI.h"
#include "JITCacheContainer.h"
#include "JITCacheMaintenance.h"
#include "JITCacheMaintenanceTesting.h"
#include "JITCacheTest.h"
#include "ProducerBudget.h"
#include "UCBKeys.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// Tests M1 to M6 of SPEC-integrator.maintenance.md section 7: clean, compact and their command line, on temporary
// directories whose bodies the writer committed. A test places the other files a run can meet by hand: temporaries a
// killed producer left, damaged, foreign and misnamed bodies, and names JITCache never writes. Only the test that checks
// that the next Producer reuses a deletion's remnant needs a VM, for start.

namespace JSC::JITCache::Tests {

namespace MaintenanceTestsInternal {

// Maintenance's hooks are process-wide, and the tests of a group share a process, so every test turns them off when it ends.
class HookScope {
    WTF_MAKE_NONCOPYABLE(HookScope);
public:
    HookScope() = default;
    ~HookScope()
    {
        Maintenance::Testing::setFailingBodyUnlink(std::nullopt);
        Maintenance::Testing::setStopAfterUnlink(std::nullopt);
    }
};

static String errnoText(int error)
{
    return String::fromUTF8(safeStrerror(error).span());
}

// A body key from its canonical bytes (SPEC-ucb.md section 3.1): version 1, a program body, call specialization, no mode
// bits, and an identity digest drawn from the seed, so distinct seeds give distinct keys.
static BodyKey maintenanceKey(uint8_t seed)
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

static String bodyName(const BodyKey& key)
{
    return String { bodyFileName(key).span() };
}

// A header that passes its own checks (container sub-SPEC section 3.2). Maintenance never compares it with the running
// process, so crafted facts serve; another CPU feature vector gives another header and digest.
static ArtifactHeaderBytes maintenanceHeader(uint64_t cpuFeatures = 0x15)
{
    ArtifactHeader header;
    header.architecture = static_cast<uint8_t>(HeaderArchitecture::X86_64);
    header.facts.mainExecutable.size = 20;
    for (size_t index = 0; index < header.facts.mainExecutable.size; ++index)
        header.facts.mainExecutable.bytes[index] = static_cast<uint8_t>(0xA0 + index);
    header.facts.mustMatch = { false, true, false };
    header.facts.cpuFeatures = cpuFeatures;
    auto bytes = encodeArtifactHeader(header);
    RELEASE_ASSERT(bytes);
    return *bytes;
}

static Vector<uint8_t> filler(size_t size, uint8_t seed)
{
    Vector<uint8_t> bytes;
    bytes.reserveInitialCapacity(size);
    for (size_t offset = 0; offset < size; ++offset)
        bytes.append(static_cast<uint8_t>(seed + 7 * offset + (offset >> 8)));
    return bytes;
}

// A body file laid out as the writer lays it out, every section of a tier-1 body in this build filled with sectionBytes
// bytes, under any key, commit identifier and header digest; a test damages or misplaces it to reach a check.
static Vector<uint8_t> syntheticBody(const BodyKey& key, const HeaderDigest& headerDigest, uint64_t version, size_t sectionBytes)
{
    std::array<Vector<uint8_t>, numberOfSectionKinds> sections;
    Vector<ContainerTesting::Section> list;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (!isSectionRequired(kind, 1))
            continue;
        sections[index] = filler(sectionBytes, static_cast<uint8_t>(key.bytes()[8] + index));
        list.append({ kind, sections[index].span() });
    }
    BodyEnvelope stamp;
    stamp.highestTier = 1;
    stamp.key = key;
    stamp.version = version;
    stamp.headerDigest = headerDigest;
    stamp.llintThreshold = 300;
    return ContainerTesting::buildBody(stamp, list.span());
}

static bool writeFileAt(TestContext& context, int directoryFd, const char* name, std::span<const uint8_t> bytes)
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

// A body the writer committed, with the terms compact orders it by.
struct CommittedBody {
    BodyKey key;
    uint64_t bytes; // B, the file's size
    uint64_t version; // the envelope's commit identifier; 0 for a body whose envelope fails
    double score; // (L + P) / B; -infinity for a body whose envelope fails
};

// The order of section 4.2, step 6: by score, then by key bytes, both ascending.
static Vector<CommittedBody> inEvictionOrder(Vector<CommittedBody> bodies)
{
    std::ranges::sort(bodies, [](const CommittedBody& a, const CommittedBody& b) {
        if (a.score != b.score)
            return a.score < b.score;
        return std::ranges::lexicographical_compare(a.key.bytes(), b.key.bytes());
    });
    return bodies;
}

static uint64_t totalBytes(std::span<const CommittedBody> bodies)
{
    uint64_t total = 0;
    for (auto& body : bodies)
        total += body.bytes;
    return total;
}

enum class Layout : uint8_t {
    Parent, // <parent> alone
    Remnant, // cache/ and cache/bodies/ without a header
    Artifact, // cache/, cache/bodies/ and a header
};

// A temporary <parent>, removed with everything in it when the object goes. Bodies are committed through the writer,
// under the producer lock it takes and releases around each commit, so a maintenance call can take the lock in between.
class MaintenanceArtifact {
    WTF_MAKE_NONCOPYABLE(MaintenanceArtifact);
public:
    static std::unique_ptr<MaintenanceArtifact> create(TestContext& context, Layout layout = Layout::Artifact)
    {
        const char* temporaryDirectory = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(temporaryDirectory && *temporaryDirectory ? temporaryDirectory : "/tmp"), "/jitcache-maintenance-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        std::unique_ptr<MaintenanceArtifact> artifact(new MaintenanceArtifact(String::fromUTF8(path.span().data())));
        artifact->m_parentFd = ::open(path.span().data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (artifact->m_parentFd < 0) {
            JITCACHE_FAIL(makeString("cannot open the test parent: "_s, errnoText(errno)));
            return nullptr;
        }
        if (layout == Layout::Parent)
            return artifact;
        if (mkdirat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), 0755)
            || (artifact->m_cacheFd = openat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0
            || mkdirat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), 0755)
            || (artifact->m_bodiesFd = openat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot lay out a test artifact: "_s, errnoText(errno)));
            return nullptr;
        }
        if (layout == Layout::Artifact && !artifact->writeHeader(context, artifact->headerBytes()))
            return nullptr;
        return artifact;
    }

    ~MaintenanceArtifact()
    {
        for (int fd : { m_bodiesFd, m_cacheFd, m_parentFd }) {
            if (fd >= 0)
                ::close(fd);
        }
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    const String& path() const { return m_path; }
    CString pathArgument() const { return m_path.utf8(); }
    int parentFd() const { return m_parentFd; }
    int cacheFd() const { return m_cacheFd; }
    int bodiesFd() const { return m_bodiesFd; }
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
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, errnoText(taken.error())));
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

    // A body file placed under key's name by hand, as no writer would.
    bool placeBody(TestContext& context, const BodyKey& key, std::span<const uint8_t> file)
    {
        return writeFileAt(context, m_bodiesFd, bodyFileName(key).data(), file);
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

    bool exists(const String& path) const { return exists(path.utf8().data()); }

    bool holdsBody(const BodyKey& key) const
    {
        return exists(makeString("cache/bodies/"_s, bodyName(key)));
    }

    // The names a fresh listing of a directory under <parent> finds, or nothing when it cannot be listed.
    std::optional<Vector<String>> names(const char* path) const
    {
        int fd = openat(m_parentFd, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            return std::nullopt;
        Vector<String> result;
        int error = listDirectory(fd, [&](std::span<const char> name, uint64_t) {
            result.append(String { name });
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
            CString characters = name.utf8();
            if (temporaryKindOfFileName(characters.span()))
                ++count;
        }
        return count;
    }

    size_t bodiesInBodies() const
    {
        auto listed = names("cache/bodies");
        if (!listed)
            return 0;
        size_t count = 0;
        for (auto& name : *listed) {
            CString characters = name.utf8();
            if (bodyKeyFromFileName(characters.span()))
                ++count;
        }
        return count;
    }

    // The commit epoch, read through the lock file (container sub-SPEC section 2).
    std::optional<uint64_t> epoch() const
    {
        int fd = openat(m_parentFd, ArtifactNames::lockFile.characters(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return std::nullopt;
        std::array<uint8_t, sizeof(uint64_t)> bytes { };
        ssize_t count = ::pread(fd, bytes.data(), bytes.size(), 16);
        ::close(fd);
        if (count != static_cast<ssize_t>(bytes.size()))
            return std::nullopt;
        uint64_t epoch = 0;
        memcpySpan(asMutableByteSpan(epoch), std::span { bytes });
        return epoch;
    }

    // The store's view of what remains, as a VM that opens the artifact afterwards builds it.
    RefPtr<OpenedArtifact> take(TestContext& context)
    {
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, headerBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, errnoText(taken.error())));
            return nullptr;
        }
        return WTF::move(*taken);
    }

private:
    explicit MaintenanceArtifact(String&& path)
        : m_path(WTF::move(path))
        , m_header(maintenanceHeader())
    {
    }

    const String m_path;
    ArtifactHeaderBytes m_header;
    int m_parentFd { -1 };
    int m_cacheFd { -1 };
    int m_bodiesFd { -1 };
};

// Commits count bodies with distinct scores and returns them in eviction order.
static std::optional<Vector<CommittedBody>> commitBodies(TestContext& context, MaintenanceArtifact& artifact, unsigned count, uint8_t firstSeed = 1)
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

static ASCIILiteral outcomeText(Maintenance::Outcome outcome)
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

static String describe(const Maintenance::Report& report)
{
    String text = makeString(outcomeText(report.outcome), " with "_s, report.temporariesRemoved, " temporaries removed, "_s, report.bodiesEvicted,
        " bodies evicted, "_s, report.bytesReclaimed, " bytes reclaimed"_s);
    for (auto& diagnostic : report.diagnostics)
        text = makeString(text, "; "_s, diagnostic.code, ": "_s, diagnostic.detail);
    return text;
}

static bool checkOutcome(TestContext& context, const String& label, const Maintenance::Report& report, Maintenance::Outcome expected)
{
    if (report.outcome == expected)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, outcomeText(expected), ", got "_s, describe(report)));
    return false;
}

static size_t countDiagnostics(const Maintenance::Report& report, ASCIILiteral code)
{
    return static_cast<size_t>(std::ranges::count_if(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code;
    }));
}

static bool hasDiagnostic(const Maintenance::Report& report, ASCIILiteral code, const String& detail)
{
    return std::ranges::any_of(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.detail == detail;
    });
}

static bool hasDiagnosticStartingWith(const Maintenance::Report& report, ASCIILiteral code, const String& prefix)
{
    return std::ranges::any_of(report.diagnostics, [&](const Maintenance::Diagnostic& diagnostic) {
        return diagnostic.code == code && diagnostic.detail.startsWith(prefix);
    });
}

// The plan lists exactly these bodies, in this order, with their sizes, versions and scores.
static void checkEvictions(TestContext& context, const String& label, const Maintenance::Plan& plan, std::span<const CommittedBody> expected)
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

// The first call of section 4.4 on a plan that deletes the artifact.
static std::optional<Maintenance::Plan> askForDeletion(TestContext& context, const String& label, MaintenanceArtifact& artifact)
{
    Maintenance::Report report = Maintenance::compact(artifact.path(), 1, { });
    if (!checkOutcome(context, label, report, Maintenance::Outcome::NeedsConfirmation) || !report.plan)
        return std::nullopt;
    return WTF::move(report.plan);
}

// The second call of section 4.4.
static Maintenance::Report confirm(MaintenanceArtifact& artifact, const Maintenance::Plan& plan)
{
    Maintenance::CompactOptions options;
    options.answer = Maintenance::Answer::Yes;
    options.confirmedPlan = plan;
    return Maintenance::compact(artifact.path(), 1, options);
}

static Maintenance::Report compactAnswering(MaintenanceArtifact& artifact, double ratio, Maintenance::Answer answer)
{
    Maintenance::CompactOptions options;
    options.answer = answer;
    return Maintenance::compact(artifact.path(), ratio, options);
}

// What a stream written through open_memstream holds.
class CapturedOutput {
    WTF_MAKE_NONCOPYABLE(CapturedOutput);
public:
    CapturedOutput()
        : m_file(open_memstream(&m_buffer, &m_size))
    {
    }

    ~CapturedOutput()
    {
        if (m_file)
            fclose(m_file);
        free(m_buffer);
    }

    FILE* file() const { return m_file; }

    String text()
    {
        if (!m_file)
            return { };
        fflush(m_file);
        if (!m_buffer)
            return emptyString();
        return String::fromUTF8(unsafeMakeSpan(static_cast<const char*>(m_buffer), m_size));
    }

private:
    char* m_buffer { nullptr };
    size_t m_size { 0 };
    FILE* m_file { nullptr };
};

struct CommandLineRun {
    int exitCode;
    String out;
    String err;
};

static CommandLineRun runMaintenanceCommand(std::initializer_list<CString> arguments, FILE* in = nullptr)
{
    Vector<CString> vector(arguments);
    CapturedOutput out;
    CapturedOutput err;
    int exitCode = Maintenance::runCommandLine(vector.span(), in, out.file(), err.file());
    return { exitCode, out.text(), err.text() };
}

static constexpr ASCIILiteral confirmationPrompt = "This operation will delete the entire cache. Ok? (y/n)"_s;

// A pseudo-terminal: a command line reads its terminal device as input, and the test types the answer on the controlling
// side.
class Pseudoterminal {
    WTF_MAKE_NONCOPYABLE(Pseudoterminal);
public:
    Pseudoterminal()
    {
        m_controller = posix_openpt(O_RDWR | O_NOCTTY);
        if (m_controller < 0)
            return;
        std::array<char, 128> name { };
        if (grantpt(m_controller) || unlockpt(m_controller) || ptsname_r(m_controller, name.data(), name.size()))
            return;
        int device = ::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (device < 0)
            return;
        m_input = fdopen(device, "r");
        if (!m_input)
            ::close(device);
    }

    ~Pseudoterminal()
    {
        if (m_input)
            fclose(m_input);
        if (m_controller >= 0)
            ::close(m_controller);
    }

    bool isValid() const { return m_controller >= 0 && m_input; }
    FILE* input() const { return m_input; }

    // The line discipline hands what the controlling side writes to the terminal device as typed input.
    bool type(const char* text) const
    {
        size_t length = strlen(text);
        return ::write(m_controller, text, length) == static_cast<ssize_t>(length);
    }

private:
    int m_controller { -1 };
    FILE* m_input { nullptr };
};

} // namespace MaintenanceTestsInternal

using namespace MaintenanceTestsInternal;

// M1. clean removes exactly the temporaries, of both kinds, and reports their bytes; it leaves the bodies, the header and
// the names it does not know, and reports each of those.
JITCACHE_TEST(maintenanceCleanRemovesOnlyTemporaries, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies)
        return;
    if (!artifact->placeTemporary(context, TemporaryKind::Body, 100) || !artifact->placeTemporary(context, TemporaryKind::Body, 200)
        || !artifact->placeTemporary(context, TemporaryKind::Header, 50)
        || !artifact->placeFile(context, artifact->cacheFd(), "notes.txt", 7) || !artifact->placeFile(context, artifact->bodiesFd(), "stray", 9))
        return;
    auto epochBefore = artifact->epoch();

    Maintenance::Report report = Maintenance::clean(artifact->path());
    if (!checkOutcome(context, "clean"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.temporariesRemoved == 3);
    JITCACHE_CHECK(report.bytesReclaimed == 350);
    JITCACHE_CHECK(!report.bodiesEvicted);
    JITCACHE_CHECK(!report.plan);
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->holdsBody((*bodies)[0].key) && artifact->holdsBody((*bodies)[1].key));
    JITCACHE_CHECK(artifact->exists("cache/header"));
    JITCACHE_CHECK(artifact->exists("cache/notes.txt") && artifact->exists("cache/bodies/stray"));
    JITCACHE_CHECK(report.diagnostics.size() == 2);
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/notes.txt"_s));
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/bodies/stray"_s));
    auto epochAfter = artifact->epoch();
    JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);

    // A clean with nothing to remove leaves the epoch alone.
    report = Maintenance::clean(artifact->path());
    JITCACHE_CHECK(report.outcome == Maintenance::Outcome::Done && !report.temporariesRemoved && !report.bytesReclaimed);
    JITCACHE_CHECK(artifact->epoch() == epochAfter);
}

// M1. clean removes the .cache.replaced a ConsumerProducer's replacement left when it stopped midway (container sub-SPEC
// section 1.4), with its header, bodies and temporaries, counts their bytes and bumps the epoch; a name in it that is no
// artifact file stays, with the directories that hold it, and is reported.
JITCACHE_TEST(maintenanceCleanRemovesAReplacedArtifact, No)
{
    HookScope hooks;
    for (bool withUnknownFile : { false, true }) {
        String label = withUnknownFile ? "with an unknown file"_s : "without one"_s;
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        int replacedFd = -1;
        int replacedBodiesFd = -1;
        if (mkdirat(artifact->parentFd(), ArtifactNames::replacedCacheDirectory.characters(), 0755)
            || (replacedFd = openat(artifact->parentFd(), ArtifactNames::replacedCacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0
            || mkdirat(replacedFd, ArtifactNames::bodiesDirectory.characters(), 0755)
            || (replacedBodiesFd = openat(replacedFd, ArtifactNames::bodiesDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString(label, ": cannot make .cache.replaced: "_s, errnoText(errno)));
            return;
        }
        bool placed = artifact->placeFile(context, replacedFd, ArtifactNames::header.characters(), 64)
            && artifact->placeFile(context, replacedBodiesFd, bodyFileName(maintenanceKey(7)).data(), 300)
            && (!withUnknownFile || artifact->placeFile(context, replacedBodiesFd, "stray", 5));
        ::close(replacedBodiesFd);
        ::close(replacedFd);
        if (!placed)
            return;
        // The lock file a replacing ConsumerProducer leaves, whose epoch clean bumps.
        if (!ProducerLock::tryAcquire(artifact->parentFd())) {
            JITCACHE_FAIL(makeString(label, ": cannot create the lock file"_s));
            return;
        }
        auto epochBefore = artifact->epoch();

        Maintenance::Report report = Maintenance::clean(artifact->path());
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done))
            continue;
        if (report.bytesReclaimed != 364)
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (artifact->exists(".cache.replaced") != withUnknownFile || artifact->exists(".cache.replaced/bodies/stray") != withUnknownFile)
            JITCACHE_FAIL(makeString(label, ": .cache.replaced is "_s, artifact->exists(".cache.replaced") ? "still there"_s : "gone"_s));
        if (withUnknownFile != hasDiagnosticStartingWith(report, "unlink-failed"_s, ".cache.replaced/"_s))
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        auto epochAfter = artifact->epoch();
        JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);
        JITCACHE_CHECK(artifact->exists("cache/header"));
    }
}

// M1. A header-less cache/ that holds a body is a remnant clean never finishes: it removes the temporaries only.
JITCACHE_TEST(maintenanceCleanKeepsTheBodiesOfARemnant, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Body, 40))
        return;
    if (unlinkat(artifact->cacheFd(), ArtifactNames::header.characters(), 0)) {
        JITCACHE_FAIL(makeString("cannot remove the header: "_s, errnoText(errno)));
        return;
    }

    Maintenance::Report report = Maintenance::clean(artifact->path());
    if (!checkOutcome(context, "clean"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.temporariesRemoved == 1 && report.bytesReclaimed == 40);
    JITCACHE_CHECK(artifact->holdsBody(body->key));
    JITCACHE_CHECK(artifact->exists("cache/bodies"));
    JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "remnant-with-bodies"_s) == 1);
}

// M1. clean finishes a header-less remnant with no body, with or without bodies/, and keeps the lock file.
JITCACHE_TEST(maintenanceCleanFinishesARemnantWithoutBodies, No)
{
    HookScope hooks;
    for (bool withBodiesDirectory : { true, false }) {
        String label = withBodiesDirectory ? "with bodies/"_s : "without bodies/"_s;
        auto artifact = MaintenanceArtifact::create(context, Layout::Remnant);
        if (!artifact)
            return;
        if (!artifact->placeTemporary(context, TemporaryKind::Body, 30) || !artifact->placeTemporary(context, TemporaryKind::Header, 20))
            return;
        if (!withBodiesDirectory && unlinkat(artifact->cacheFd(), ArtifactNames::bodiesDirectory.characters(), AT_REMOVEDIR)) {
            JITCACHE_FAIL(makeString("cannot remove bodies/: "_s, errnoText(errno)));
            return;
        }

        Maintenance::Report report = Maintenance::clean(artifact->path());
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done))
            continue;
        if (report.temporariesRemoved != 2 || report.bytesReclaimed != 50 || !report.diagnostics.isEmpty())
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (artifact->exists("cache"))
            JITCACHE_FAIL(makeString(label, ": cache/ is still there"_s));
        if (!artifact->exists(ArtifactNames::lockFile.characters()))
            JITCACHE_FAIL(makeString(label, ": the lock file is gone"_s));
    }
}

// M1. A remnant without a body that holds an unknown file stays in place, so the next Producer reuses it, and clean is Done.
JITCACHE_TEST(maintenanceCleanLeavesARemnantHoldingAnUnknownFile, No)
{
    HookScope hooks;
    for (bool inBodies : { false, true }) {
        String label = inBodies ? "an unknown file in bodies/"_s : "an unknown file in cache/"_s;
        auto artifact = MaintenanceArtifact::create(context, Layout::Remnant);
        if (!artifact)
            return;
        if (!artifact->placeTemporary(context, TemporaryKind::Body, 30)
            || !artifact->placeFile(context, inBodies ? artifact->bodiesFd() : artifact->cacheFd(), "keep-me", 5))
            return;

        Maintenance::Report report = Maintenance::clean(artifact->path());
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done))
            continue;
        String unknown = inBodies ? "cache/bodies/keep-me"_s : "cache/keep-me"_s;
        if (report.temporariesRemoved != 1 || report.diagnostics.size() != 1 || !hasDiagnostic(report, "unknown-file"_s, unknown))
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (!artifact->exists("cache/bodies") || !artifact->exists(unknown) || artifact->temporariesInCache())
            JITCACHE_FAIL(makeString(label, ": the remnant did not stay as it should"_s));
    }
}

// M1. On a path that does not exist, on a directory without cache/ and on one whose cache is not a directory, clean and
// compact return NoArtifact and create no lock file; compact gets there before it would read its ratio.
JITCACHE_TEST(maintenanceWithoutAnArtifactCreatesNoLockFile, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context, Layout::Parent);
    if (!artifact)
        return;
    String missing = makeString(artifact->path(), "/missing"_s);
    JITCACHE_CHECK(Maintenance::clean(missing).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(missing, 0.5, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists("missing"));

    JITCACHE_CHECK(Maintenance::clean(artifact->path()).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0.5, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), std::numeric_limits<double>::quiet_NaN(), { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists(ArtifactNames::lockFile.characters()));

    if (!artifact->placeFile(context, artifact->parentFd(), ArtifactNames::cacheDirectory.characters(), 3))
        return;
    JITCACHE_CHECK(Maintenance::clean(artifact->path()).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 1, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists(ArtifactNames::lockFile.characters()));
}

// M1. While a ProducerLock of the test process holds the lock, every call is Busy and changes nothing.
JITCACHE_TEST(maintenanceIsBusyWhileAProducerLockIsHeld, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
        return;

    {
        auto held = ProducerLock::tryAcquire(artifact->parentFd());
        if (!held) {
            JITCACHE_FAIL("cannot take the producer lock"_s);
            return;
        }
        JITCACHE_CHECK(Maintenance::clean(artifact->path()).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0.5, { }).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(compactAnswering(*artifact, 1, Maintenance::Answer::Yes).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(artifact->temporariesInCache() == 1);
        JITCACHE_CHECK(artifact->holdsBody(body->key) && artifact->exists("cache/header"));
    }

    Maintenance::Report report = Maintenance::clean(artifact->path());
    JITCACHE_CHECK(report.outcome == Maintenance::Outcome::Done && report.temporariesRemoved == 1);
}

// Sections 3 and 6: a header that fails its checks does not stop clean, which needs only names; compact returns Failed and
// removes nothing.
JITCACHE_TEST(maintenanceBadHeaderStopsCompactButNotClean, No)
{
    HookScope hooks;
    ArtifactHeaderBytes laterLayout = maintenanceHeader();
    laterLayout.bytes[8] = 2; // layout version 2: incompatible, the rest unread
    Vector<uint8_t> garbage = filler(64, 0x42);

    for (bool corrupt : { true, false }) {
        String label = corrupt ? "a corrupt header"_s : "an incompatible header"_s;
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
        if (!body || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
            return;
        if (!artifact->writeHeader(context, corrupt ? garbage.span() : laterLayout.span()))
            return;

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0.5, { });
        if (checkOutcome(context, makeString(label, ", compact"_s), report, Maintenance::Outcome::Failed)) {
            JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "bad-header"_s) == 1);
            JITCACHE_CHECK(!report.plan && !report.temporariesRemoved && !report.bodiesEvicted);
        }
        JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->holdsBody(body->key));

        report = Maintenance::clean(artifact->path());
        if (checkOutcome(context, makeString(label, ", clean"_s), report, Maintenance::Outcome::Done)) {
            JITCACHE_CHECK(report.temporariesRemoved == 1);
            JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "bad-header"_s) == 1);
        }
        JITCACHE_CHECK(!artifact->temporariesInCache() && artifact->holdsBody(body->key) && artifact->exists("cache/header"));
    }
}

// M2. Bodies with known L, P and sizes are planned in increasing (L + P) / B, ties by key bytes, and the plan counts the
// header's bytes beside the bodies'.
JITCACHE_TEST(maintenanceCompactOrdersByScoreThenKey, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto low = artifact->commit(context, maintenanceKey(3), 100, 0, 4096);
    auto first = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    auto tieA = artifact->commit(context, maintenanceKey(4), 500, 50, 64);
    auto tieB = artifact->commit(context, maintenanceKey(5), 500, 50, 64);
    auto high = artifact->commit(context, maintenanceKey(2), 500, 100, 64);
    if (!low || !first || !tieA || !tieB || !high)
        return;
    // The data gives the order it is meant to: low, first, the tie, high.
    JITCACHE_CHECK(low->score < first->score && first->score < tieA->score && tieA->score == tieB->score && tieB->score < high->score);

    Vector<CommittedBody> expected = inEvictionOrder({ *low, *first, *tieA, *tieB, *high });
    JITCACHE_CHECK(expected[0].key == low->key && expected[1].key == first->key && expected[4].key == high->key);

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::No);
    if (!checkOutcome(context, "compact 1 --no"_s, report, Maintenance::Outcome::Declined) || !report.plan)
        return;
    const Maintenance::Plan& plan = *report.plan;
    checkEvictions(context, "the whole plan"_s, plan, expected.span());
    uint64_t bodiesBytes = totalBytes(expected.span());
    JITCACHE_CHECK(plan.postCleanBytes == bodiesBytes + artifact->headerBytes().size());
    JITCACHE_CHECK(plan.targetBytes == plan.postCleanBytes);
    JITCACHE_CHECK(plan.evictedBytes == bodiesBytes);
    JITCACHE_CHECK(plan.deletesArtifact);
    JITCACHE_CHECK(plan.headerDigest == artifact->headerDigest());
    JITCACHE_CHECK(report.diagnostics.isEmpty());
    JITCACHE_CHECK(artifact->bodiesInBodies() == 5 && artifact->exists("cache/header"));
}

// M2. Ratio 0 evicts nothing and only cleans, on an artifact with bodies and on one with none, whose header stays.
JITCACHE_TEST(maintenanceCompactAtRatioZeroOnlyCleans, No)
{
    HookScope hooks;
    for (unsigned bodyCount : { 2u, 0u }) {
        String label = makeString("ratio 0 over "_s, bodyCount, " bodies"_s);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 40))
            return;

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done) || !report.plan)
            continue;
        const Maintenance::Plan& plan = *report.plan;
        if (!plan.evictions.isEmpty() || plan.targetBytes || plan.evictedBytes || plan.deletesArtifact)
            JITCACHE_FAIL(makeString(label, ": the plan evicts something"_s));
        if (plan.postCleanBytes != totalBytes(bodies->span()) + artifact->headerBytes().size())
            JITCACHE_FAIL(makeString(label, ": postCleanBytes is "_s, plan.postCleanBytes));
        if (report.temporariesRemoved != 1 || report.bytesReclaimed != 40 || report.bodiesEvicted || !report.diagnostics.isEmpty())
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (!artifact->exists("cache/header") || artifact->bodiesInBodies() != bodyCount || artifact->temporariesInCache())
            JITCACHE_FAIL(makeString(label, ": the artifact is not what a clean leaves"_s));
    }
}

// M2. A ratio whose target falls inside a body's bytes evicts that body, and every body before it, and nothing after.
JITCACHE_TEST(maintenanceCompactEvictsTheBodyItsTargetFallsIn, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    auto& order = *bodies;
    uint64_t postCleanBytes = totalBytes(order.span()) + artifact->headerBytes().size();
    double ratio = (static_cast<double>(order[0].bytes) + static_cast<double>(order[1].bytes) / 2) / static_cast<double>(postCleanBytes);
    auto epochBefore = artifact->epoch();

    Maintenance::Report report = Maintenance::compact(artifact->path(), ratio, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    const Maintenance::Plan& plan = *report.plan;
    JITCACHE_CHECK(plan.postCleanBytes == postCleanBytes);
    JITCACHE_CHECK(plan.targetBytes == static_cast<uint64_t>(std::ceil(ratio * static_cast<double>(postCleanBytes))));
    JITCACHE_CHECK(plan.targetBytes > order[0].bytes && plan.targetBytes <= order[0].bytes + order[1].bytes);
    checkEvictions(context, "the plan"_s, plan, order.span().first(2));
    JITCACHE_CHECK(plan.evictedBytes == order[0].bytes + order[1].bytes);
    JITCACHE_CHECK(!plan.deletesArtifact);
    JITCACHE_CHECK(report.bodiesEvicted == 2 && report.bytesReclaimed == order[0].bytes + order[1].bytes);
    JITCACHE_CHECK(!artifact->holdsBody(order[0].key) && !artifact->holdsBody(order[1].key) && artifact->holdsBody(order[2].key));
    JITCACHE_CHECK(artifact->exists("cache/header"));
    auto epochAfter = artifact->epoch();
    JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);
}

// M2. Bodies whose envelopes fail B1 to B5 are evicted first, with score -infinity, version 0 and a diagnostic each:
// damaged-body for B1, B2 and B5, foreign-body for B4, misnamed-body for B3. A body name that is not a regular file fails
// B1 with 0 bytes (section 6): a FIFO, which an open that could block would wait on for good, and a symbolic link to a
// sound body, whose target's envelope would otherwise be judged under the link's name.
JITCACHE_TEST(maintenanceCompactEvictsDamagedForeignAndMisnamedBodiesFirst, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto good = commitBodies(context, *artifact, 2);
    if (!good)
        return;

    HeaderDigest digest = artifact->headerDigest();
    HeaderDigest otherDigest = artifactHeaderDigest(maintenanceHeader(0x2A).span());
    enum class Placement : uint8_t { File, FIFO, Link };
    struct Damage {
        BodyKey name;
        Vector<uint8_t> file; // Placement::File only
        ASCIILiteral code;
        ContainerCheck check;
        Placement placement { Placement::File };
    };
    Vector<Damage> damages;
    {
        Vector<uint8_t> truncated = syntheticBody(maintenanceKey(20), digest, 7, 16);
        truncated.shrink(100);
        damages.append({ maintenanceKey(20), WTF::move(truncated), "damaged-body"_s, ContainerChecks::size });
        Vector<uint8_t> badEnvelope = syntheticBody(maintenanceKey(21), digest, 8, 16);
        badEnvelope[92] ^= 0x01; // P changes under the envelope's CRC
        damages.append({ maintenanceKey(21), WTF::move(badEnvelope), "damaged-body"_s, ContainerChecks::envelope });
        damages.append({ maintenanceKey(22), syntheticBody(maintenanceKey(22), digest, 0, 16), "damaged-body"_s, ContainerChecks::version });
        damages.append({ maintenanceKey(23), syntheticBody(maintenanceKey(23), otherDigest, 9, 16), "foreign-body"_s, ContainerChecks::headerDigest });
        damages.append({ maintenanceKey(24), syntheticBody(maintenanceKey(25), digest, 10, 16), "misnamed-body"_s, ContainerChecks::key });
        damages.append({ maintenanceKey(26), { }, "damaged-body"_s, ContainerChecks::size, Placement::FIFO });
        damages.append({ maintenanceKey(27), { }, "damaged-body"_s, ContainerChecks::size, Placement::Link });
    }
    Vector<CommittedBody> bad;
    for (auto& damage : damages) {
        switch (damage.placement) {
        case Placement::File:
            if (!artifact->placeBody(context, damage.name, damage.file.span()))
                return;
            break;
        case Placement::FIFO:
            if (mkfifoat(artifact->bodiesFd(), bodyFileName(damage.name).data(), 0644)) {
                JITCACHE_FAIL(makeString("cannot make a FIFO under a body name: "_s, errnoText(errno)));
                return;
            }
            break;
        case Placement::Link:
            // A relative target resolves in bodies/, the link's own directory.
            if (symlinkat(bodyFileName((*good)[0].key).data(), artifact->bodiesFd(), bodyFileName(damage.name).data())) {
                JITCACHE_FAIL(makeString("cannot make a symbolic link under a body name: "_s, errnoText(errno)));
                return;
            }
            break;
        }
        bad.append({ damage.name, damage.file.size(), 0, -std::numeric_limits<double>::infinity() });
    }
    Vector<CommittedBody> all = bad;
    all.appendVector(*good);
    Vector<CommittedBody> expected = inEvictionOrder(WTF::move(all));
    // The applied plan below aims at exactly the bad bodies' bytes, so it takes all of them only when the last of them in
    // eviction order, by key bytes, has bytes of its own: a body of 0 bytes after it would be left out.
    JITCACHE_CHECK(expected[damages.size() - 1].bytes);

    auto checkDiagnostics = [&](const String& label, const Maintenance::Report& report) {
        for (auto& damage : damages) {
            String detail = makeString("cache/bodies/"_s, bodyName(damage.name), ": "_s, damage.check);
            if (!hasDiagnostic(report, damage.code, detail))
                JITCACHE_FAIL(makeString(label, ": no "_s, damage.code, " diagnostic "_s, detail, " in "_s, describe(report)));
        }
    };

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::No);
    if (!checkOutcome(context, "compact 1 --no"_s, report, Maintenance::Outcome::Declined) || !report.plan)
        return;
    checkEvictions(context, "the whole plan"_s, *report.plan, expected.span());
    checkDiagnostics("the whole plan"_s, report);
    JITCACHE_CHECK(report.diagnostics.size() == damages.size());

    // A target of exactly the bad bodies' bytes takes them and stops.
    uint64_t badBytes = totalBytes(bad.span());
    double ratio = (static_cast<double>(badBytes) - 0.5) / static_cast<double>(report.plan->postCleanBytes);
    report = Maintenance::compact(artifact->path(), ratio, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    checkEvictions(context, "the plan"_s, *report.plan, expected.span().first(damages.size()));
    checkDiagnostics("the applied plan"_s, report);
    JITCACHE_CHECK(report.bodiesEvicted == damages.size() && report.bytesReclaimed == badBytes);
    for (auto& damage : damages)
        JITCACHE_CHECK(!artifact->holdsBody(damage.name));
    JITCACHE_CHECK(artifact->holdsBody((*good)[0].key) && artifact->holdsBody((*good)[1].key) && artifact->exists("cache/header"));
}

// Section 4.2, step 3: a ratio that is NaN or outside [0, 1] fails at bad-ratio and changes nothing.
JITCACHE_TEST(maintenanceCompactRejectsABadRatio, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
        return;
    for (double ratio : { std::numeric_limits<double>::quiet_NaN(), -0.25, 1.5, std::numeric_limits<double>::infinity() }) {
        Maintenance::Report report = compactAnswering(*artifact, ratio, Maintenance::Answer::Yes);
        if (!checkOutcome(context, makeString("ratio "_s, ratio), report, Maintenance::Outcome::Failed))
            continue;
        if (report.diagnostics.size() != 1 || countDiagnostics(report, "bad-ratio"_s) != 1 || report.plan)
            JITCACHE_FAIL(makeString("ratio "_s, ratio, ": "_s, describe(report)));
    }
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->holdsBody(body->key) && artifact->exists("cache/header"));
}

// M3. Ratio 1, a ratio whose target needs every body, and any ratio above 0 on an artifact with no body plan to delete the
// whole artifact: with Ask that is NeedsConfirmation and with No Declined, and neither changes anything, temporaries
// included.
JITCACHE_TEST(maintenanceWholeDeletionWaitsForConfirmation, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
        return;
    auto& order = *bodies;
    uint64_t bodiesBytes = totalBytes(order.span());
    double everyBody = (static_cast<double>(bodiesBytes) - static_cast<double>(order[2].bytes) / 2) / static_cast<double>(bodiesBytes + artifact->headerBytes().size());
    JITCACHE_CHECK(everyBody < 1);

    auto unchanged = [&](MaintenanceArtifact& checked, std::span<const CommittedBody> held) {
        if (checked.temporariesInCache() != 1 || !checked.exists("cache/header"))
            return false;
        return std::ranges::all_of(held, [&](const CommittedBody& body) {
            return checked.holdsBody(body.key);
        });
    };

    for (double ratio : { 1.0, everyBody }) {
        for (auto answer : { Maintenance::Answer::Ask, Maintenance::Answer::No }) {
            String label = makeString("ratio "_s, ratio, answer == Maintenance::Answer::Ask ? " with Ask"_s : " with No"_s);
            Maintenance::Report report = compactAnswering(*artifact, ratio, answer);
            auto expected = answer == Maintenance::Answer::Ask ? Maintenance::Outcome::NeedsConfirmation : Maintenance::Outcome::Declined;
            if (!checkOutcome(context, label, report, expected) || !report.plan)
                continue;
            if (!report.plan->deletesArtifact)
                JITCACHE_FAIL(makeString(label, ": the plan keeps the artifact"_s));
            checkEvictions(context, label, *report.plan, order.span());
            if (report.temporariesRemoved || report.bodiesEvicted || report.bytesReclaimed || !unchanged(*artifact, order.span()))
                JITCACHE_FAIL(makeString(label, ": the call changed the artifact"_s));
        }
    }

    auto empty = MaintenanceArtifact::create(context);
    if (!empty || !empty->placeTemporary(context, TemporaryKind::Body, 10))
        return;
    for (auto answer : { Maintenance::Answer::Ask, Maintenance::Answer::No }) {
        String label = answer == Maintenance::Answer::Ask ? "no body with Ask"_s : "no body with No"_s;
        Maintenance::Report report = compactAnswering(*empty, 0.01, answer);
        auto expected = answer == Maintenance::Answer::Ask ? Maintenance::Outcome::NeedsConfirmation : Maintenance::Outcome::Declined;
        if (!checkOutcome(context, label, report, expected) || !report.plan)
            continue;
        if (!report.plan->deletesArtifact || !report.plan->evictions.isEmpty() || report.plan->postCleanBytes != empty->headerBytes().size())
            JITCACHE_FAIL(makeString(label, ": the plan is not a deletion of an artifact without bodies"_s));
        if (!unchanged(*empty, { }))
            JITCACHE_FAIL(makeString(label, ": the call changed the artifact"_s));
    }
}

// M3. Yes deletes the temporaries, the bodies, the header and both directories, keeps the lock file, and bumps the epoch.
JITCACHE_TEST(maintenanceWholeDeletionRemovesEverythingButTheLockFile, No)
{
    HookScope hooks;
    for (unsigned bodyCount : { 3u, 0u }) {
        String label = makeString("a deletion of "_s, bodyCount, " bodies"_s);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 100) || !artifact->placeTemporary(context, TemporaryKind::Header, 30))
            return;
        // A path without commits has no lock file yet; the first maintenance call creates it with a zero epoch.
        uint64_t epochBefore = artifact->epoch().value_or(0);

        Maintenance::Report report = compactAnswering(*artifact, bodyCount ? 1 : 0.5, Maintenance::Answer::Yes);
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done) || !report.plan)
            continue;
        JITCACHE_CHECK(report.plan->deletesArtifact);
        JITCACHE_CHECK(report.diagnostics.isEmpty());
        JITCACHE_CHECK(report.temporariesRemoved == 2 && report.bodiesEvicted == bodyCount);
        uint64_t everything = 130 + totalBytes(bodies->span()) + artifact->headerBytes().size();
        if (report.bytesReclaimed != everything)
            JITCACHE_FAIL(makeString(label, ": "_s, report.bytesReclaimed, " bytes reclaimed where "_s, everything, " were expected"_s));
        JITCACHE_CHECK(!artifact->exists("cache"));
        JITCACHE_CHECK(artifact->exists(ArtifactNames::lockFile.characters()));
        JITCACHE_CHECK(artifact->epoch() == epochBefore + 1);
    }
}

// M3. With one body's unlink made to fail, Yes evicts the others, keeps that body and the header, returns Failed with
// unlink-failed, and the store's open accepts what remains.
JITCACHE_TEST(maintenanceWholeDeletionKeepsTheHeaderWhenABodyUnlinkFails, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    auto& order = *bodies;
    Maintenance::Testing::setFailingBodyUnlink(2);

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    Maintenance::Testing::setFailingBodyUnlink(std::nullopt);
    if (!checkOutcome(context, "compact 1 --yes"_s, report, Maintenance::Outcome::Failed) || !report.plan)
        return;
    checkEvictions(context, "the plan"_s, *report.plan, order.span());
    JITCACHE_CHECK(report.diagnostics.size() == 1);
    JITCACHE_CHECK(hasDiagnosticStartingWith(report, "unlink-failed"_s, makeString("cache/bodies/"_s, bodyName(order[1].key), ": "_s)));
    JITCACHE_CHECK(report.bodiesEvicted == 2 && report.bytesReclaimed == order[0].bytes + order[2].bytes);
    JITCACHE_CHECK(!artifact->holdsBody(order[0].key) && artifact->holdsBody(order[1].key) && !artifact->holdsBody(order[2].key));
    JITCACHE_CHECK(artifact->exists("cache/header") && artifact->exists("cache/bodies"));

    RefPtr<OpenedArtifact> object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->indexedBodies() == 1);
    JITCACHE_CHECK(object->token(order[1].key) && !object->token(order[0].key) && !object->token(order[2].key));
    BodyOpen opened = object->open(order[1].key, ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Found && opened.body && opened.body->version() == order[1].version);
}

// M3. With an unknown file in bodies/, Yes returns Done with rmdir-failed, and the next Producer reuses the remnant
// (container sub-SPEC section 1.3).
JITCACHE_TEST(maintenanceWholeDeletionLeavesARemnantTheNextProducerReuses, Yes)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeFile(context, artifact->bodiesFd(), "stray", 4))
        return;

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    if (!checkOutcome(context, "compact 1 --yes"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.bodiesEvicted == 2);
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/bodies/stray"_s));
    JITCACHE_CHECK(countDiagnostics(report, "rmdir-failed"_s) == 1 && report.diagnostics.size() == 2);
    JITCACHE_CHECK(!artifact->exists("cache/header") && artifact->exists("cache/bodies/stray") && !artifact->bodiesInBodies());

    JITCache::Config config;
    config.artifactPath = artifact->path();
    config.role = Role::Producer;
    config.producerLimitBytes = 1 << 20;
    config.strict = true;
    StartResult started = JITCache::start(*context.vm(), config);
    if (started.outcome != StartOutcome::Started)
        JITCACHE_FAIL(makeString("the Producer's start is "_s, JITCache::name(started.outcome), " at "_s, started.step, ": "_s, started.detail));
    JITCACHE_CHECK(artifact->exists("cache/header") && artifact->exists("cache/bodies/stray"));
}

// M4. A plan confirmed after a commit, a recapture of a key, an eviction or a header change in between gives PlanChanged
// with the new plan and evicts nothing; an unchanged one applies.
JITCACHE_TEST(maintenanceConfirmationRequiresAnUnchangedPlan, No)
{
    HookScope hooks;
    enum class Change : uint8_t { Commit, Recapture, Eviction, Header, None };
    for (Change change : { Change::Commit, Change::Recapture, Change::Eviction, Change::Header, Change::None }) {
        static constexpr std::array<ASCIILiteral, 5> labels { "a commit"_s, "a recapture"_s, "an eviction"_s, "a header change"_s, "no change"_s };
        String label = labels[static_cast<size_t>(change)];
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, 3);
        if (!bodies)
            return;
        auto plan = askForDeletion(context, label, *artifact);
        if (!plan)
            continue;

        size_t bodiesAfterChange = 3;
        switch (change) {
        case Change::Commit:
            if (!artifact->commit(context, maintenanceKey(9), 500, 5, 64))
                return;
            bodiesAfterChange = 4;
            break;
        case Change::Recapture:
            if (!artifact->commit(context, (*bodies)[0].key, 700, 30, 64))
                return;
            break;
        case Change::Eviction: {
            Maintenance::Report eviction = Maintenance::compact(artifact->path(), 1 / static_cast<double>(plan->postCleanBytes), { });
            if (!checkOutcome(context, makeString(label, ", the eviction"_s), eviction, Maintenance::Outcome::Done) || eviction.bodiesEvicted != 1)
                return;
            bodiesAfterChange = 2;
            break;
        }
        case Change::Header:
            if (!artifact->replaceHeader(context, maintenanceHeader(0x2A)))
                return;
            break;
        case Change::None:
            break;
        }

        Maintenance::Report report = confirm(*artifact, *plan);
        if (change == Change::None) {
            if (checkOutcome(context, label, report, Maintenance::Outcome::Done) && report.plan)
                JITCACHE_CHECK(*report.plan == *plan && report.bodiesEvicted == 3);
            JITCACHE_CHECK(!artifact->exists("cache"));
            continue;
        }
        if (!checkOutcome(context, label, report, Maintenance::Outcome::PlanChanged) || !report.plan)
            continue;
        if (*report.plan == *plan)
            JITCACHE_FAIL(makeString(label, ": PlanChanged returned the confirmed plan"_s));
        if (change == Change::Header && equalSpans(std::span { report.plan->headerDigest }, std::span { plan->headerDigest }))
            JITCACHE_FAIL(makeString(label, ": the new plan has the old header digest"_s));
        if (report.bodiesEvicted || report.temporariesRemoved || report.bytesReclaimed)
            JITCACHE_FAIL(makeString(label, ": PlanChanged removed something: "_s, describe(report)));
        if (artifact->bodiesInBodies() != bodiesAfterChange || !artifact->exists("cache/header"))
            JITCACHE_FAIL(makeString(label, ": the artifact changed after PlanChanged"_s));
    }
}

// M5. An apply stopped after each unlink of a body leaves an artifact that the store's open and its index accept, with only
// whole bodies and no epoch bump; one stopped after the header's unlink leaves a remnant that clean finishes.
JITCACHE_TEST(maintenanceInterruptedDeletionLeavesWholeBodies, No)
{
    HookScope hooks;
    constexpr unsigned bodyCount = 3;
    for (unsigned n = 1; n <= bodyCount + 1; ++n) {
        String label = makeString("stopped after unlink "_s, n);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies)
            return;
        auto& order = *bodies;
        auto epochBefore = artifact->epoch();

        Maintenance::Testing::setStopAfterUnlink(n);
        Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
        Maintenance::Testing::setStopAfterUnlink(std::nullopt);
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Failed) || !report.plan)
            continue;
        checkEvictions(context, label, *report.plan, order.span());
        if (artifact->epoch() != epochBefore)
            JITCACHE_FAIL(makeString(label, ": the stopped apply bumped the epoch"_s));

        if (n > bodyCount) {
            if (artifact->exists("cache/header") || artifact->bodiesInBodies() || !artifact->exists("cache"))
                JITCACHE_FAIL(makeString(label, ": the artifact is not a bodiless remnant"_s));
            Maintenance::Report cleaned = Maintenance::clean(artifact->path());
            if (checkOutcome(context, makeString(label, ", clean"_s), cleaned, Maintenance::Outcome::Done) && artifact->exists("cache"))
                JITCACHE_FAIL(makeString(label, ": clean did not finish the remnant"_s));
            continue;
        }

        if (report.bodiesEvicted != n || !artifact->exists("cache/header") || artifact->bodiesInBodies() != bodyCount - n)
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        RefPtr<OpenedArtifact> object = artifact->take(context);
        if (!object)
            continue;
        if (object->indexedBodies() != bodyCount - n)
            JITCACHE_FAIL(makeString(label, ": the index lists "_s, object->indexedBodies(), " bodies"_s));
        for (unsigned index = 0; index < bodyCount; ++index) {
            const BodyKey& key = order[index].key;
            if (index < n) {
                if (object->token(key) || artifact->holdsBody(key))
                    JITCACHE_FAIL(makeString(label, ": evicted body "_s, index, " is still there"_s));
                continue;
            }
            BodyOpen opened = object->open(key, ValidationMode::Full);
            if (!object->token(key) || opened.outcome != StoreOutcome::Found || !opened.body || opened.body->version() != order[index].version)
                JITCACHE_FAIL(makeString(label, ": surviving body "_s, index, " does not open whole"_s));
        }
    }
}

// M6. The command line's exit codes: 0 for Done and NoArtifact, 1 for Failed, 2 for a usage error, 3 for Busy, 4 for
// NeedsConfirmation and Declined; a summary goes to out and diagnostics to err. Without a terminal there is no prompt and
// no change, even with an answer waiting on in. PlanChanged's 5 needs a change while the user answers, the last test.
JITCACHE_TEST(maintenanceCommandLineExitCodes, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
        return;
    CString path = artifact->pathArgument();

    auto checkUsage = [&](const String& label, std::initializer_list<CString> arguments) {
        CommandLineRun run = runMaintenanceCommand(arguments);
        if (run.exitCode != 2 || !run.out.isEmpty() || !run.err.contains("usage: clean [<path>]"_s))
            JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, "\", err \""_s, run.err, '"'));
    };
    checkUsage("no arguments"_s, { });
    checkUsage("an unknown command"_s, { "prune"_s, path });
    checkUsage("clean with two paths"_s, { "clean"_s, path, path });
    checkUsage("an option of clean"_s, { "clean"_s, path, "--yes"_s });
    checkUsage("compact without a ratio"_s, { "compact"_s });
    checkUsage("a path before the ratio"_s, { "compact"_s, path, "0.5"_s });
    checkUsage("a ratio that is no number"_s, { "compact"_s, "half"_s, path });
    checkUsage("an empty ratio"_s, { "compact"_s, ""_s, path });
    checkUsage("compact with two paths"_s, { "compact"_s, "0.5"_s, path, path });
    checkUsage("an unknown option"_s, { "compact"_s, "0.5"_s, path, "--maybe"_s });
    checkUsage("a mistyped answer without a path"_s, { "compact"_s, "0.5"_s, "--yse"_s });
    checkUsage("both answers"_s, { "compact"_s, "0.5"_s, path, "--yes"_s, "--no"_s });
    JITCACHE_CHECK(artifact->temporariesInCache() == 1);

    CommandLineRun run = runMaintenanceCommand({ "clean"_s, makeString(artifact->path(), "/missing"_s).utf8() });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("clean: no-artifact"_s));

    // An answer waits on in, but in is no terminal.
    std::array<char, 3> answer { 'y', '\n', '\0' };
    FILE* input = fmemopen(answer.data(), 2, "r");
    if (!input) {
        JITCACHE_FAIL(makeString("fmemopen failed: "_s, errnoText(errno)));
        return;
    }
    run = runMaintenanceCommand({ "compact"_s, "1"_s, path }, input);
    fclose(input);
    JITCACHE_CHECK(run.exitCode == 4);
    JITCACHE_CHECK(run.out.contains("compact: needs-confirmation"_s) && run.out.contains("deletes the whole artifact"_s));
    JITCACHE_CHECK(!run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->bodiesInBodies() == 2 && artifact->exists("cache/header"));

    run = runMaintenanceCommand({ "compact"_s, "1"_s, path, "--no"_s });
    JITCACHE_CHECK(run.exitCode == 4 && run.out.contains("compact: declined"_s) && !run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->bodiesInBodies() == 2);

    run = runMaintenanceCommand({ "compact"_s, "2"_s, path });
    JITCACHE_CHECK(run.exitCode == 1 && run.out.contains("compact: failed"_s) && run.err.contains("bad-ratio"_s));

    {
        auto held = ProducerLock::tryAcquire(artifact->parentFd());
        if (!held) {
            JITCACHE_FAIL("cannot take the producer lock"_s);
            return;
        }
        run = runMaintenanceCommand({ "clean"_s, path });
        JITCACHE_CHECK(run.exitCode == 3 && run.out.contains("clean: busy"_s));
        run = runMaintenanceCommand({ "compact"_s, "0"_s, path });
        JITCACHE_CHECK(run.exitCode == 3 && run.out.contains("compact: busy"_s));
    }

    if (!artifact->placeFile(context, artifact->cacheFd(), "notes.txt", 3))
        return;
    run = runMaintenanceCommand({ "clean"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("clean: done, 1 temporaries removed, 10 bytes reclaimed"_s));
    JITCACHE_CHECK(run.err == "unknown-file: cache/notes.txt\n"_s);

    run = runMaintenanceCommand({ "compact"_s, "0"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: done, 0 temporaries removed, 0 bodies evicted"_s));
    JITCACHE_CHECK(artifact->bodiesInBodies() == 2);

    run = runMaintenanceCommand({ "compact"_s, "1"_s, "--yes"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: done"_s) && !run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(!artifact->exists("cache/header") && !artifact->bodiesInBodies());
}

// M6. Without a path, both commands work on ./.jitcache in the working directory, here a symbolic link to the artifact.
JITCACHE_TEST(maintenanceCommandLineDefaultsToTheJITCacheDirectory, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    auto workingDirectory = MaintenanceArtifact::create(context, Layout::Parent);
    if (!artifact || !workingDirectory)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
        return;
    if (symlinkat(artifact->pathArgument().data(), workingDirectory->parentFd(), ".jitcache")) {
        JITCACHE_FAIL(makeString("symlinkat failed: "_s, errnoText(errno)));
        return;
    }

    // The tests of a process run one at a time, so this one may move the working directory while it runs.
    int previous = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (previous < 0 || fchdir(workingDirectory->parentFd())) {
        JITCACHE_FAIL(makeString("cannot enter the working directory: "_s, errnoText(errno)));
        if (previous >= 0)
            ::close(previous);
        return;
    }
    CommandLineRun clean = runMaintenanceCommand({ "clean"_s });
    CommandLineRun compact = runMaintenanceCommand({ "compact"_s, "1"_s, "--yes"_s });
    bool restored = !fchdir(previous);
    ::close(previous);

    JITCACHE_CHECK(restored);
    JITCACHE_CHECK(!clean.exitCode && clean.out.contains("clean: done, 1 temporaries removed, 10 bytes reclaimed"_s));
    JITCACHE_CHECK(!compact.exitCode && compact.out.contains("compact: done"_s));
    JITCACHE_CHECK(!artifact->exists("cache/header") && !artifact->bodiesInBodies());
}

// M6. With a terminal on in (a pseudo-terminal here), compact prints the plan and the prompt, reads one line, and applies
// the plan on y or Y; any other answer is Declined and changes nothing.
JITCACHE_TEST(maintenanceCommandLinePromptsOnATerminal, No)
{
    HookScope hooks;
    Pseudoterminal terminal;
    if (!terminal.isValid()) {
        JITCACHE_FAIL(makeString("no pseudo-terminal: "_s, errnoText(errno)));
        return;
    }
    struct TypedAnswer {
        const char* typed;
        bool applies;
    };
    for (const TypedAnswer& answer : { TypedAnswer { "n\n", false }, TypedAnswer { "yes\n", false }, TypedAnswer { "y\n", true }, TypedAnswer { "Y\n", true } }) {
        const char* typed = answer.typed;
        bool applies = answer.applies;
        String label = makeString("typing "_s, String::fromUTF8(typed).trim(isASCIIWhitespace<char16_t>));
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, 2);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Body, 10))
            return;
        if (!terminal.type(typed)) {
            JITCACHE_FAIL(makeString(label, ": the terminal refused the answer"_s));
            return;
        }

        CommandLineRun run = runMaintenanceCommand({ "compact"_s, "1"_s, artifact->pathArgument() }, terminal.input());
        if (!run.out.contains(confirmationPrompt) || !run.out.contains("deletes the whole artifact"_s))
            JITCACHE_FAIL(makeString(label, ": no plan and prompt in \""_s, run.out, '"'));
        if (applies) {
            if (run.exitCode || !run.out.contains("compact: done"_s) || artifact->exists("cache"))
                JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, '"'));
        } else {
            if (run.exitCode != 4 || !run.out.contains("compact: declined"_s))
                JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, '"'));
            if (artifact->temporariesInCache() != 1 || artifact->bodiesInBodies() != 2 || !artifact->exists("cache/header"))
                JITCACHE_FAIL(makeString(label, ": the declined plan changed the artifact"_s));
        }
    }
}

// M6. A commit while the user reads the prompt, which the free lock allows, makes the confirmed call return PlanChanged,
// exit code 5, and evict nothing.
JITCACHE_TEST(maintenanceCommandLineReportsAPlanChangedAtThePrompt, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    Pseudoterminal terminal;
    if (!terminal.isValid()) {
        JITCACHE_FAIL(makeString("no pseudo-terminal: "_s, errnoText(errno)));
        return;
    }
    std::array<int, 2> pipeFds { -1, -1 };
    if (pipe2(pipeFds.data(), O_CLOEXEC)) {
        JITCACHE_FAIL(makeString("pipe2 failed: "_s, errnoText(errno)));
        return;
    }
    FILE* out = fdopen(pipeFds[1], "w");
    if (!out) {
        JITCACHE_FAIL(makeString("fdopen failed: "_s, errnoText(errno)));
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return;
    }

    // The command line runs on a thread of its own and writes to the pipe, which this thread reads until the prompt.
    Vector<CString> arguments { CString { "compact"_s }, CString { "1"_s }, artifact->pathArgument() };
    CapturedOutput err;
    std::atomic<int> exitCode { -1 };
    FILE* input = terminal.input();
    FILE* errFile = err.file();
    Ref<Thread> thread = Thread::create("jitcache maintenance"_s, [&] {
        exitCode.store(Maintenance::runCommandLine(arguments.span(), input, out, errFile));
        fclose(out);
    });

    Vector<char> received;
    auto readOutput = [&](bool untilPrompt) {
        std::array<char, 256> chunk;
        while (!untilPrompt || !StringView { std::span<const char> { received.span() } }.contains(confirmationPrompt)) {
            ssize_t count = ::read(pipeFds[0], chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return;
            received.append(std::span<const char> { chunk }.first(static_cast<size_t>(count)));
        }
    };
    readOutput(true);
    bool prompted = StringView { std::span<const char> { received.span() } }.contains(confirmationPrompt);
    JITCACHE_CHECK(prompted);
    if (prompted)
        JITCACHE_CHECK(artifact->commit(context, maintenanceKey(9), 500, 5, 64).has_value());
    // The answer goes in whatever happened, so the thread never waits on it.
    terminal.type("y\n");
    thread->waitForCompletion();
    readOutput(false);
    ::close(pipeFds[0]);

    String output = String::fromUTF8(received.span());
    JITCACHE_CHECK(exitCode.load() == 5);
    if (!output.contains("compact: plan-changed"_s))
        JITCACHE_FAIL(makeString("the output is \""_s, output, '"'));
    JITCACHE_CHECK(artifact->bodiesInBodies() == 4 && artifact->exists("cache/header"));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
