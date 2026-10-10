#include "config.h"
#include "JITCacheMaintenance.h"

#include "ArtifactStore.h"
#include "JITCacheAPI.h"
#include "JITCacheContainer.h"
#include "JITCacheMaintenanceTesting.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string.h>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/FastFloat.h>
#include <wtf/Noncopyable.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// The maintenance backend and its command line (SPEC-integrator.maintenance.md). clean and compact need no VM and no
// JSC::initialize: they run on the calling thread, use the filesystem, SHA-256 and CRC32C, and hold the producer lock of
// <parent> for all their work. They judge bodies by their envelopes alone and never compare the header with the running
// process, since the command line may run under another binary than the one that produced the artifact.

namespace JSC::JITCache::Maintenance {

namespace MaintenanceInternal {

// The diagnostic codes of section 6.
namespace Codes {
inline constexpr ASCIILiteral badHeader = "bad-header"_s;
inline constexpr ASCIILiteral badRatio = "bad-ratio"_s;
inline constexpr ASCIILiteral damagedBody = "damaged-body"_s;
inline constexpr ASCIILiteral foreignBody = "foreign-body"_s;
inline constexpr ASCIILiteral misnamedBody = "misnamed-body"_s;
inline constexpr ASCIILiteral remnantWithBodies = "remnant-with-bodies"_s;
inline constexpr ASCIILiteral unknownFile = "unknown-file"_s;
inline constexpr ASCIILiteral unlinkFailed = "unlink-failed"_s;
inline constexpr ASCIILiteral rmdirFailed = "rmdir-failed"_s;
inline constexpr ASCIILiteral io = "io"_s;
inline constexpr ASCIILiteral platform = "platform"_s;
} // namespace Codes

static void addDiagnostic(Report& report, ASCIILiteral code, String&& detail)
{
    report.diagnostics.append(Diagnostic { code, WTF::move(detail) });
}

static void fail(Report& report, ASCIILiteral code, String&& detail)
{
    report.outcome = Outcome::Failed;
    addDiagnostic(report, code, WTF::move(detail));
}

#if ENABLE(JITCACHE_TWINS)
// Section 7's hooks; 0 leaves every apply alone.
static std::atomic<uint64_t> failingBodyUnlinkForTesting { 0 };
static std::atomic<uint64_t> stopAfterUnlinkForTesting { 0 };
#endif

#if OS(LINUX)

// Names as diagnostics print them, relative to <parent>.
static constexpr ASCIILiteral cachePrefix = "cache/"_s;
static constexpr ASCIILiteral bodiesPrefix = "cache/bodies/"_s;

static constexpr int parentFlags = O_PATH | O_DIRECTORY | O_CLOEXEC;
static constexpr int directoryFlags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;

// A descriptor this file opened, closed when the object goes.
class OwnedDescriptor {
    WTF_MAKE_NONCOPYABLE(OwnedDescriptor);
public:
    OwnedDescriptor() = default;
    explicit OwnedDescriptor(int fd)
        : m_fd(fd)
    {
    }
    OwnedDescriptor(OwnedDescriptor&& other)
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }
    OwnedDescriptor& operator=(OwnedDescriptor&& other)
    {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    ~OwnedDescriptor() { reset(); }

    explicit operator bool() const { return m_fd >= 0; }
    int get() const { return m_fd; }

private:
    void reset()
    {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = -1;
    }

    int m_fd { -1 };
};

static int openAt(int directoryFd, const char* name, int flags)
{
    int fd;
    do {
        fd = ::openat(directoryFd, name, flags);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static String errorText(int error)
{
    return String::fromUTF8(safeStrerror(error).span());
}

// What a name from a listing prints as: the directory's prefix and the name, with any byte that is not UTF-8 replaced.
static String displayName(ASCIILiteral directory, std::span<const char> name)
{
    return makeString(directory, String::fromUTF8ReplacingInvalidSequences(byteCast<Latin1Character>(name)));
}

// ENOENT or ENOTDIR: no directory is at the name.
static bool isAbsence(int error)
{
    return error == ENOENT || error == ENOTDIR;
}

static std::expected<OwnedDescriptor, int> openDirectory(int directoryFd, ASCIILiteral name)
{
    OwnedDescriptor fd { openAt(directoryFd, name.characters(), directoryFlags) };
    if (!fd)
        return std::unexpected(errno);
    return fd;
}

// bodies/, or an empty descriptor when the directory has none, as a remnant may not (container sub-SPEC section 1.3).
static std::expected<OwnedDescriptor, int> openBodiesDirectory(int cacheFd)
{
    auto bodies = openDirectory(cacheFd, ArtifactNames::bodiesDirectory);
    if (!bodies && bodies.error() == ENOENT)
        return OwnedDescriptor { };
    return bodies;
}

static void failToOpenCache(Report& report, int error)
{
    if (isAbsence(error)) {
        report.outcome = Outcome::NoArtifact;
        return;
    }
    fail(report, Codes::io, makeString("opening cache/: "_s, errorText(error)));
}

// What a call holds while it works: the pinned <parent>, the producer lock and cache/, opened under the lock. Destroying
// it releases the lock.
struct Session {
    OwnedDescriptor parent;
    OwnedDescriptor cache;
    std::unique_ptr<ProducerLock> lock;
};

// Section 2: opens <parent> and looks for cache/ without the lock, so a path without an artifact gets no lock file; then
// takes the lock without blocking and opens cache/ again under it. Without a session, report holds the outcome:
// NoArtifact, Busy, or Failed at io.
static std::optional<Session> beginSession(const String& parentPath, Report& report)
{
    std::optional<Session> session { std::in_place };
    CString path = parentPath.utf8();
    session->parent = OwnedDescriptor { openAt(AT_FDCWD, path.data() ? path.data() : "", parentFlags) };
    if (!session->parent) {
        int error = errno;
        if (isAbsence(error))
            report.outcome = Outcome::NoArtifact;
        else
            fail(report, Codes::io, makeString("opening "_s, parentPath, ": "_s, errorText(error)));
        return std::nullopt;
    }
    if (auto cache = openDirectory(session->parent.get(), ArtifactNames::cacheDirectory); !cache) {
        failToOpenCache(report, cache.error());
        return std::nullopt;
    }

    auto lock = ProducerLock::tryAcquire(session->parent.get());
    if (!lock) {
        if (lock.error().busy)
            report.outcome = Outcome::Busy;
        else
            fail(report, Codes::io, makeString(ArtifactNames::lockFile, ": "_s, errorText(lock.error().error)));
        return std::nullopt;
    }
    session->lock = WTF::move(*lock);

    // A cache/ that went away before the lock was taken is no artifact.
    auto cache = openDirectory(session->parent.get(), ArtifactNames::cacheDirectory);
    if (!cache) {
        failToOpenCache(report, cache.error());
        return std::nullopt;
    }
    session->cache = WTF::move(*cache);
    return session;
}

// The header as maintenance reads it: its own checks and its digest, without the comparison with the running process
// (section 2; container sub-SPEC section 3.2).
enum class HeaderState : uint8_t { Absent, Valid, Bad };
struct HeaderRead {
    HeaderState state { HeaderState::Absent };
    HeaderDigest digest { };
    uint64_t bytes { 0 }; // the file's size, H
    String problem; // Bad: why the header fails its checks
};

static HeaderRead readHeader(int cacheFd)
{
    HeaderRead result;
    // O_NONBLOCK: a FIFO at the name reads as empty instead of stopping the call, and then fails the header's checks.
    OwnedDescriptor fd { openAt(cacheFd, ArtifactNames::header.characters(), O_RDONLY | O_NONBLOCK | O_CLOEXEC) };
    if (!fd) {
        int error = errno;
        if (error == ENOENT)
            return result;
        result.state = HeaderState::Bad;
        result.problem = makeString("cache/header cannot be opened: "_s, errorText(error));
        return result;
    }

    // One byte more than the largest header file a reader accepts, so a larger file reads as too large and fails.
    std::array<uint8_t, maximumArtifactHeaderFileBytes + 1> buffer { };
    size_t length = 0;
    while (length < buffer.size()) {
        auto rest = std::span { buffer }.subspan(length);
        ssize_t count = ::read(fd.get(), rest.data(), rest.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            int error = errno;
            result.state = HeaderState::Bad;
            result.problem = makeString("cache/header cannot be read: "_s, errorText(error));
            return result;
        }
        if (!count)
            break;
        length += static_cast<size_t>(count);
    }

    auto file = std::span<const uint8_t> { buffer }.first(length);
    if (auto header = readArtifactHeader(file); !header) {
        result.state = HeaderState::Bad;
        auto verdict = header.error().verdict == HeaderVerdict::Incompatible ? "incompatible"_s : "corrupt"_s;
        result.problem = makeString("cache/header is "_s, verdict, ": "_s, header.error().reason);
        return result;
    }
    result.state = HeaderState::Valid;
    result.digest = artifactHeaderDigest(file);
    result.bytes = length;
    return result;
}

// The names of cache/ beside header and bodies/: the temporaries clean removes (container sub-SPEC section 1.1) and the
// names it only reports.
struct CacheListing {
    Vector<CString> temporaries;
    Vector<String> unknownNames;
};

static std::expected<CacheListing, int> listCache(int cacheFd)
{
    CacheListing listing;
    int error = listDirectory(cacheFd, [&](std::span<const char> name, uint64_t) {
        if (equalSpans(name, ArtifactNames::header.span()) || equalSpans(name, ArtifactNames::bodiesDirectory.span()))
            return;
        if (temporaryKindOfFileName(name)) {
            listing.temporaries.append(CString { name });
            return;
        }
        listing.unknownNames.append(displayName(cachePrefix, name));
    });
    if (error)
        return std::unexpected(error);
    return listing;
}

struct EnvelopeRead {
    uint64_t fileSize { 0 }; // B
    std::expected<BodyEnvelope, ContainerCheck> envelope; // B1 to B5 in Full mode
};

// A body name that is not a regular file fails B1, with 0 bytes (section 6).
static EnvelopeRead notARegularFile()
{
    return { 0, std::unexpected(ContainerChecks::size) };
}

// Reads one body's envelope: B1 to B5 in Full mode, with the file's own name as the key and the header's digest
// (section 2). An error is the errno of the call that failed.
static std::expected<EnvelopeRead, int> readEnvelope(int bodiesFd, const BodyFileName& name, const BodyKey& key, const HeaderDigest& headerDigest)
{
    // O_NONBLOCK keeps a FIFO planted under a body's name from stopping the call, and O_NOFOLLOW keeps a symbolic link from
    // being judged by the file it names. Two open errors mean the name is not a regular file: ELOOP, a link, and ENXIO, a
    // socket or a device without a driver. Any other file that is not regular opens, and the fstat test below rejects it.
    OwnedDescriptor fd { openAt(bodiesFd, name.data(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC) };
    if (!fd) {
        int error = errno;
        if (error == ELOOP || error == ENXIO)
            return notARegularFile();
        return std::unexpected(error);
    }
    struct stat status;
    if (fstat(fd.get(), &status))
        return std::unexpected(errno);
    if (!S_ISREG(status.st_mode))
        return notARegularFile();

    uint64_t fileSize = static_cast<uint64_t>(status.st_size);
    std::array<uint8_t, bodyEnvelopeBytes> start { };
    size_t wanted = static_cast<size_t>(std::min<uint64_t>(start.size(), fileSize));
    size_t length = 0;
    while (length < wanted) {
        auto rest = std::span { start }.subspan(length, wanted - length);
        ssize_t count = ::pread(fd.get(), rest.data(), rest.size(), static_cast<off_t>(length));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errno);
        }
        if (!count)
            break; // the file shrank since fstat; B1 rejects the short start
        length += static_cast<size_t>(count);
    }
    return EnvelopeRead { fileSize, validateBodyEnvelope(std::span<const uint8_t> { start }.first(length), fileSize, key, headerDigest, ValidationMode::Full) };
}

// The code a failed envelope check is reported under (section 4.2, step 4).
static ASCIILiteral codeForEnvelopeCheck(ContainerCheck check)
{
    if (check == ContainerChecks::headerDigest)
        return Codes::foreignBody;
    if (check == ContainerChecks::key)
        return Codes::misnamedBody;
    return Codes::damagedBody;
}

// The names of bodies/. With a header digest, every body's envelope is read into one Eviction-sized record, so planning's
// memory grows by one record per body and never with a body's size (section 2), and each envelope that fails B1 to B5 adds
// its diagnostic; without one, as for clean, the body names are only counted.
struct BodiesListing {
    uint64_t bodyNames { 0 };
    Vector<Eviction> bodies;
    Vector<String> unknownNames;
};

static std::expected<BodiesListing, String> listBodies(int bodiesFd, const HeaderDigest* headerDigest, Report& report)
{
    BodiesListing listing;
    String readFailure;
    int error = listDirectory(bodiesFd, [&](std::span<const char> name, uint64_t) {
        auto key = bodyKeyFromFileName(name);
        if (!key) {
            listing.unknownNames.append(displayName(bodiesPrefix, name));
            return;
        }
        ++listing.bodyNames;
        if (!headerDigest || !readFailure.isNull())
            return;

        // The name passed bodyKeyFromFileName, so it is exactly the name the key encodes.
        BodyFileName fileName = bodyFileName(*key);
        auto read = readEnvelope(bodiesFd, fileName, *key, *headerDigest);
        if (!read) {
            // A body gone since the listing needs no plan; any other failure leaves the call unable to judge the artifact.
            if (read.error() != ENOENT)
                readFailure = makeString("reading "_s, bodiesPrefix, String { fileName.span() }, ": "_s, errorText(read.error()));
            return;
        }

        Eviction eviction { };
        std::ranges::copy(key->bytes(), eviction.key.begin());
        eviction.bytes = read->fileSize;
        if (read->envelope) {
            // B1 passed, so the file holds at least the envelope's 128 bytes.
            eviction.version = read->envelope->version;
            eviction.score = (static_cast<double>(read->envelope->llintThreshold) + static_cast<double>(read->envelope->counterProgress)) / static_cast<double>(read->fileSize);
        } else {
            ContainerCheck check = read->envelope.error();
            eviction.version = 0;
            eviction.score = -std::numeric_limits<double>::infinity();
            addDiagnostic(report, codeForEnvelopeCheck(check), makeString(bodiesPrefix, String { fileName.span() }, ": "_s, check));
        }
        listing.bodies.append(eviction);
    });
    if (error)
        return std::unexpected(makeString("listing cache/bodies/: "_s, errorText(error)));
    if (!readFailure.isNull())
        return std::unexpected(WTF::move(readFailure));
    return listing;
}

// targetBytes, ceil(ratio * postCleanBytes) for a ratio in [0, 1], never above postCleanBytes.
static uint64_t targetBytesFor(double ratio, uint64_t postCleanBytes)
{
    double target = std::ceil(ratio * static_cast<double>(postCleanBytes));
    if (target >= static_cast<double>(postCleanBytes))
        return postCleanBytes;
    return static_cast<uint64_t>(target);
}

// Section 4.2, step 6: by score, then by key bytes, both ascending.
static bool isEvictedBefore(const Eviction& a, const Eviction& b)
{
    if (a.score != b.score)
        return a.score < b.score;
    return a.key < b.key;
}

// Section 4.2, steps 5 to 7, over every body's record; the plan keeps the records of the bodies it evicts.
static Plan makePlan(const HeaderDigest& headerDigest, uint64_t headerBytes, double ratio, Vector<Eviction>&& bodies)
{
    Plan plan { };
    plan.headerDigest = headerDigest;
    plan.postCleanBytes = headerBytes;
    for (auto& body : bodies)
        plan.postCleanBytes += body.bytes;
    plan.targetBytes = targetBytesFor(ratio, plan.postCleanBytes);

    std::ranges::sort(bodies, isEvictedBefore);
    size_t taken = 0;
    while (taken < bodies.size() && plan.evictedBytes < plan.targetBytes)
        plan.evictedBytes += bodies[taken++].bytes;
    // The header's bytes count in postCleanBytes and no body removes them, so a target only they could reach takes every
    // body; a ratio of 0 has a target of 0, which never deletes the artifact.
    plan.deletesArtifact = ratio == 1 || (plan.targetBytes && taken == bodies.size());
    bodies.shrink(taken);
    plan.evictions = WTF::move(bodies);
    return plan;
}

static bool bodyUnlinkFailsForTesting(uint64_t bodyUnlink)
{
#if ENABLE(JITCACHE_TWINS)
    uint64_t n = failingBodyUnlinkForTesting.load();
    return n && n == bodyUnlink;
#else
    UNUSED_PARAM(bodyUnlink);
    return false;
#endif
}

static bool stopsAfterUnlinkForTesting(uint64_t unlink)
{
#if ENABLE(JITCACHE_TWINS)
    uint64_t n = stopAfterUnlinkForTesting.load();
    return n && n == unlink;
#else
    UNUSED_PARAM(unlink);
    return false;
#endif
}

// What one call removed, and the unlinks the test hooks count within it.
struct Removals {
    bool removedAnything { false }; // clean bumps the epoch only when it removed something (section 3, step 6)
    bool bodyStayed { false }; // an evicted body's unlink failed
    uint64_t bodyUnlinks { 0 };
    uint64_t unlinks { 0 }; // of a body or of header
};

// Clean's step 3: each temporary's size, taken with fstatat before its unlinkat, counts once it is gone.
static void removeTemporaries(int cacheFd, const Vector<CString>& temporaries, Report& report, Removals& removals)
{
    for (auto& name : temporaries) {
        struct stat status;
        if (fstatat(cacheFd, name.data(), &status, AT_SYMLINK_NOFOLLOW)) {
            int error = errno;
            if (error != ENOENT)
                addDiagnostic(report, Codes::unlinkFailed, makeString(cachePrefix, String { name.span() }, ": "_s, errorText(error)));
            continue;
        }
        if (unlinkat(cacheFd, name.data(), 0)) {
            int error = errno;
            if (error != ENOENT)
                addDiagnostic(report, Codes::unlinkFailed, makeString(cachePrefix, String { name.span() }, ": "_s, errorText(error)));
            continue;
        }
        ++report.temporariesRemoved;
        report.bytesReclaimed += static_cast<uint64_t>(status.st_size);
        removals.removedAnything = true;
    }
}

// Clean's step 3 also removes a .cache.replaced that a ConsumerProducer's replacement left when it stopped midway
// (container sub-SPEC section 1.4), counting its files' sizes; a name in it that is no artifact file stays, and with it
// the directory.
static void removeReplacedLeftover(int parentFd, Report& report, Removals& removals)
{
    uint64_t bytes = 0;
    int error = removeReplacedArtifact(parentFd, bytes);
    report.bytesReclaimed += bytes;
    if (bytes)
        removals.removedAnything = true;
    if (error)
        addDiagnostic(report, Codes::unlinkFailed, makeString(ArtifactNames::replacedCacheDirectory, "/: "_s, errorText(error)));
}

// Clean's step 5.
static void reportUnknownFiles(Vector<String>&& names, Report& report)
{
    for (auto& name : names)
        addDiagnostic(report, Codes::unknownFile, WTF::move(name));
}

// The last steps of a whole-artifact deletion and of clean's bodiless remnant: bodies/, when there is one, then cache/.
// A failure leaves a remnant without a body, which the next Producer reuses or clean finishes.
static void removeArtifactDirectories(const Session& session, bool hasBodiesDirectory, Report& report, Removals& removals)
{
    if (hasBodiesDirectory) {
        if (unlinkat(session.cache.get(), ArtifactNames::bodiesDirectory.characters(), AT_REMOVEDIR)) {
            int error = errno;
            // cache/ still holds bodies/, so its removal cannot succeed either.
            addDiagnostic(report, Codes::rmdirFailed, makeString("cache/bodies/: "_s, errorText(error)));
            return;
        }
        removals.removedAnything = true;
    }
    if (unlinkat(session.parent.get(), ArtifactNames::cacheDirectory.characters(), AT_REMOVEDIR)) {
        int error = errno;
        addDiagnostic(report, Codes::rmdirFailed, makeString("cache/: "_s, errorText(error)));
        return;
    }
    removals.removedAnything = true;
}

// Section 4.5: one unlinkat per evicted body, so every surviving body stays whole. Returns false when the stop hook ended
// the apply.
static bool evictBodies(int bodiesFd, const Plan& plan, Report& report, Removals& removals)
{
    for (auto& eviction : plan.evictions) {
        // The plan was made from body names, whose bytes BodyKey::fromBytes accepted.
        auto key = BodyKey::fromBytes(std::span<const uint8_t, BodyKey::byteSize> { eviction.key });
        RELEASE_ASSERT(key);
        BodyFileName name = bodyFileName(*key);

        int error = 0;
        if (bodyUnlinkFailsForTesting(++removals.bodyUnlinks))
            error = EIO;
        else if (unlinkat(bodiesFd, name.data(), 0))
            error = errno;
        ++removals.unlinks;

        if (!error) {
            ++report.bodiesEvicted;
            report.bytesReclaimed += eviction.bytes;
            removals.removedAnything = true;
        } else if (error != ENOENT) {
            // A body gone meanwhile is no body that stays.
            removals.bodyStayed = true;
            addDiagnostic(report, Codes::unlinkFailed, makeString(bodiesPrefix, String { name.span() }, ": "_s, errorText(error)));
        }
        if (stopsAfterUnlinkForTesting(removals.unlinks))
            return false;
    }
    return true;
}

#endif // OS(LINUX)

static ASCIILiteral outcomeName(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Done:
        return "done"_s;
    case Outcome::NoArtifact:
        return "no-artifact"_s;
    case Outcome::Busy:
        return "busy"_s;
    case Outcome::NeedsConfirmation:
        return "needs-confirmation"_s;
    case Outcome::Declined:
        return "declined"_s;
    case Outcome::PlanChanged:
        return "plan-changed"_s;
    case Outcome::Failed:
        return "failed"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Section 5's exit codes; a usage error is 2.
static constexpr int usageExitCode = 2;

static int exitCodeOf(Outcome outcome)
{
    switch (outcome) {
    case Outcome::Done:
    case Outcome::NoArtifact:
        return 0;
    case Outcome::Failed:
        return 1;
    case Outcome::Busy:
        return 3;
    case Outcome::NeedsConfirmation:
    case Outcome::Declined:
        return 4;
    case Outcome::PlanChanged:
        return 5;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr ASCIILiteral confirmationPrompt = "This operation will delete the entire cache. Ok? (y/n)"_s;

struct CommandLine {
    enum class Command : uint8_t { Clean, Compact };
    Command command { Command::Clean };
    String path;
    double ratio { 0 };
    Answer answer { Answer::Ask };
};

static String displayArgument(const CString& argument)
{
    return String::fromUTF8ReplacingInvalidSequences(byteCast<Latin1Character>(argument.span()));
}

// A path is UTF-8, as the String the backend takes is.
static std::optional<String> pathArgument(const CString& argument)
{
    if (!argument.length())
        return emptyString();
    String path = String::fromUTF8(argument.span());
    if (path.isNull())
        return std::nullopt;
    return path;
}

// A decimal number, read without the locale; the backend rejects one outside [0, 1] as bad-ratio.
static std::optional<double> ratioArgument(const CString& argument)
{
    auto characters = byteCast<Latin1Character>(argument.span());
    if (characters.empty())
        return std::nullopt;
    size_t parsedLength = 0;
    double ratio = WTF::parseDouble(characters, parsedLength);
    if (parsedLength != characters.size())
        return std::nullopt;
    return ratio;
}

// "clean [<path>]" or "compact <ratio> [<path>] [--yes | --no]" (section 5), the path defaulting to defaultArtifactPath; a
// usage error says why.
static std::expected<CommandLine, String> parseCommandLine(std::span<const CString> arguments)
{
    if (arguments.empty())
        return std::unexpected(String { "no command"_s });

    CommandLine commandLine;
    auto command = arguments[0].span();
    size_t trailing; // the index of the first argument that is a path or an option
    if (equalSpans(command, "clean"_span)) {
        commandLine.command = CommandLine::Command::Clean;
        trailing = 1;
    } else if (equalSpans(command, "compact"_span)) {
        if (arguments.size() < 2)
            return std::unexpected(String { "compact takes a ratio"_s });
        commandLine.command = CommandLine::Command::Compact;
        auto ratio = ratioArgument(arguments[1]);
        if (!ratio)
            return std::unexpected(makeString("the ratio "_s, displayArgument(arguments[1]), " is not a number"_s));
        commandLine.ratio = *ratio;
        trailing = 2;
    } else
        return std::unexpected(makeString("unknown command "_s, displayArgument(arguments[0])));

    // Every argument that starts with "--" is an option, so a mistyped one is never taken for a path.
    std::optional<String> path;
    bool answered = false;
    for (const CString& argument : arguments.subspan(trailing)) {
        auto text = argument.span();
        if (!spanHasPrefix(text, "--"_span)) {
            if (path)
                return std::unexpected(String { "at most one path"_s });
            path = pathArgument(argument);
            if (!path)
                return std::unexpected(makeString("the path "_s, displayArgument(argument), " is not UTF-8"_s));
            continue;
        }
        bool yes = equalSpans(text, "--yes"_span);
        if (commandLine.command != CommandLine::Command::Compact || (!yes && !equalSpans(text, "--no"_span)))
            return std::unexpected(makeString("unknown option "_s, displayArgument(argument)));
        if (answered)
            return std::unexpected(String { "at most one of --yes and --no"_s });
        commandLine.answer = yes ? Answer::Yes : Answer::No;
        answered = true;
    }
    commandLine.path = path ? WTF::move(*path) : String { defaultArtifactPath };
    return commandLine;
}

static void printLine(FILE* file, const String& line)
{
    if (!file)
        return;
    CString text = line.utf8();
    fwrite(text.data(), 1, text.length(), file);
    fputc('\n', file);
}

static void printDiagnostics(FILE* err, const Report& report)
{
    for (auto& diagnostic : report.diagnostics) {
        if (diagnostic.detail.isEmpty())
            printLine(err, String { diagnostic.code });
        else
            printLine(err, makeString(diagnostic.code, ": "_s, diagnostic.detail));
    }
}

// The plan's summary: its bodies, its bytes, and whether the whole artifact goes.
static String planSummary(const Plan& plan)
{
    return makeString("plan: evicts "_s, plan.evictions.size(), " bodies ("_s, plan.evictedBytes, " bytes; target "_s, plan.targetBytes,
        " of "_s, plan.postCleanBytes, " bytes)"_s, plan.deletesArtifact ? " and deletes the whole artifact"_s : ""_s);
}

static String cleanSummary(const Report& report)
{
    return makeString("clean: "_s, outcomeName(report.outcome), ", "_s, report.temporariesRemoved, " temporaries removed, "_s,
        report.bytesReclaimed, " bytes reclaimed"_s);
}

static String compactSummary(const Report& report)
{
    return makeString("compact: "_s, outcomeName(report.outcome), ", "_s, report.temporariesRemoved, " temporaries removed, "_s,
        report.bodiesEvicted, " bodies evicted, "_s, report.bytesReclaimed, " bytes reclaimed"_s);
}

static bool isTerminal(FILE* file)
{
#if OS(LINUX)
    if (!file)
        return false;
    int fd = fileno(file);
    return fd >= 0 && isatty(fd);
#else
    UNUSED_PARAM(file);
    return false;
#endif
}

// One line from in: true when it holds y or Y alone, around ASCII whitespace.
static bool readsYes(FILE* in)
{
    std::array<char, 64> buffer { };
    if (!fgets(buffer.data(), static_cast<int>(buffer.size()), in))
        return false;
    auto line = std::span<const char> { buffer }.first(strnlen(buffer.data(), buffer.size()));
    if (line.empty() || line.back() != '\n') {
        // The rest of a longer line is not left for whoever reads in next.
        int character;
        do {
            character = fgetc(in);
        } while (character != EOF && character != '\n');
    }
    while (!line.empty() && isASCIIWhitespace(line.back()))
        line = line.first(line.size() - 1);
    while (!line.empty() && isASCIIWhitespace(line.front()))
        line = line.subspan(1);
    return line.size() == 1 && (line[0] == 'y' || line[0] == 'Y');
}

static int runCompact(const CommandLine& commandLine, FILE* in, FILE* out, FILE* err)
{
    CompactOptions options;
    options.answer = commandLine.answer;
    Report report = compact(commandLine.path, commandLine.ratio, options);
    printDiagnostics(err, report);
    if (report.plan)
        printLine(out, planSummary(*report.plan));

    // Only a plan that deletes the whole artifact waits, and only a terminal is asked; the lock is free meanwhile, and the
    // second call applies only an unchanged plan (section 4.4).
    if (report.outcome == Outcome::NeedsConfirmation && report.plan && isTerminal(in)) {
        if (out) {
            fputs(confirmationPrompt.characters(), out);
            fputc(' ', out);
            fflush(out);
        }
        if (readsYes(in)) {
            CompactOptions confirmed;
            confirmed.answer = Answer::Yes;
            confirmed.confirmedPlan = WTF::move(report.plan);
            report = compact(commandLine.path, commandLine.ratio, confirmed);
            printDiagnostics(err, report);
            if (report.outcome == Outcome::PlanChanged && report.plan)
                printLine(out, planSummary(*report.plan));
        } else
            report.outcome = Outcome::Declined;
    }
    printLine(out, compactSummary(report));
    return exitCodeOf(report.outcome);
}

} // namespace MaintenanceInternal

Report clean(const String& parentPath)
{
    using namespace MaintenanceInternal;
    Report report;
#if OS(LINUX)
    auto session = beginSession(parentPath, report); // steps 1 and 2
    if (!session)
        return report;

    // A header that fails its checks does not stop clean, which needs only names.
    HeaderRead header = readHeader(session->cache.get());
    if (header.state == HeaderState::Bad)
        addDiagnostic(report, Codes::badHeader, WTF::move(header.problem));

    // Both directories are listed before anything is removed, so a listing that fails leaves the artifact as it was.
    auto bodiesDirectory = openBodiesDirectory(session->cache.get());
    if (!bodiesDirectory) {
        fail(report, Codes::io, makeString("opening cache/bodies/: "_s, errorText(bodiesDirectory.error())));
        return report;
    }
    BodiesListing bodies;
    if (*bodiesDirectory) {
        auto listed = listBodies(bodiesDirectory->get(), nullptr, report);
        if (!listed) {
            fail(report, Codes::io, WTF::move(listed.error()));
            return report;
        }
        bodies = WTF::move(*listed);
    }
    auto cache = listCache(session->cache.get());
    if (!cache) {
        fail(report, Codes::io, makeString("listing cache/: "_s, errorText(cache.error())));
        return report;
    }

    Removals removals;
    removeTemporaries(session->cache.get(), cache->temporaries, report, removals); // step 3
    removeReplacedLeftover(session->parent.get(), report, removals);

    // Step 4. A remnant without a body and without unknown names goes; one with unknown names is what the next Producer
    // reuses; one with bodies keeps them, since clean never removes a committed body.
    if (header.state == HeaderState::Absent) {
        if (bodies.bodyNames)
            addDiagnostic(report, Codes::remnantWithBodies, makeString("cache/ has no header and cache/bodies/ holds "_s, bodies.bodyNames, " bodies"_s));
        else if (cache->unknownNames.isEmpty() && bodies.unknownNames.isEmpty())
            removeArtifactDirectories(*session, static_cast<bool>(*bodiesDirectory), report, removals);
    }

    reportUnknownFiles(WTF::move(cache->unknownNames), report); // step 5
    reportUnknownFiles(WTF::move(bodies.unknownNames), report);

    if (removals.removedAnything) // step 6
        session->lock->bumpEpoch();
    report.outcome = Outcome::Done;
    return report;
#else
    UNUSED_PARAM(parentPath);
    fail(report, Codes::platform, "maintenance runs only on Linux"_s);
    return report;
#endif
}

Report compact(const String& parentPath, double ratio, const CompactOptions& options)
{
    using namespace MaintenanceInternal;
    Report report;
#if OS(LINUX)
    // Section 4.2. The plan is made on the artifact as the clean would leave it, temporaries left out of every count, and
    // the clean itself runs only when the plan is applied, so a plan that is never applied changes nothing.
    auto session = beginSession(parentPath, report); // step 1
    if (!session)
        return report;

    HeaderRead header = readHeader(session->cache.get()); // step 2
    if (header.state == HeaderState::Absent) {
        report.outcome = Outcome::NoArtifact;
        return report;
    }
    if (header.state == HeaderState::Bad) {
        fail(report, Codes::badHeader, WTF::move(header.problem));
        return report;
    }

    // Step 3; NaN fails both tests.
    if (!(ratio >= 0 && ratio <= 1)) {
        fail(report, Codes::badRatio, makeString("the ratio "_s, ratio, " is not in [0, 1]"_s));
        return report;
    }

    auto bodiesDirectory = openBodiesDirectory(session->cache.get()); // step 4
    if (!bodiesDirectory) {
        fail(report, Codes::io, makeString("opening cache/bodies/: "_s, errorText(bodiesDirectory.error())));
        return report;
    }
    BodiesListing bodies;
    if (*bodiesDirectory) {
        auto listed = listBodies(bodiesDirectory->get(), &header.digest, report);
        if (!listed) {
            fail(report, Codes::io, WTF::move(listed.error()));
            return report;
        }
        bodies = WTF::move(*listed);
    }
    report.plan = makePlan(header.digest, header.bytes, ratio, WTF::move(bodies.bodies)); // steps 5 to 7
    const Plan& plan = *report.plan;

    // Section 4.4: a confirmed plan is applied only when planning again under this lock gives it back field for field,
    // header digest included; a plan that deletes the artifact waits for an answer.
    if (options.confirmedPlan && *options.confirmedPlan != plan) {
        report.outcome = Outcome::PlanChanged;
        return report;
    }
    if (plan.deletesArtifact) {
        if (options.answer == Answer::Ask) {
            report.outcome = Outcome::NeedsConfirmation;
            return report;
        }
        if (options.answer == Answer::No) {
            report.outcome = Outcome::Declined;
            return report;
        }
    }

    // Sections 4.3 and 4.5: under the lock the plan was made under, the clean of clean's steps 3 to 5, then the evictions.
    // compact returned NoArtifact for a remnant, so clean's step 4 has nothing to do here.
    auto cache = listCache(session->cache.get());
    if (!cache) {
        fail(report, Codes::io, makeString("listing cache/: "_s, errorText(cache.error())));
        return report;
    }
    Removals removals;
    removeTemporaries(session->cache.get(), cache->temporaries, report, removals);
    removeReplacedLeftover(session->parent.get(), report, removals);
    reportUnknownFiles(WTF::move(cache->unknownNames), report);
    reportUnknownFiles(WTF::move(bodies.unknownNames), report);

    if (!evictBodies(bodiesDirectory->get(), plan, report, removals)) {
        report.outcome = Outcome::Failed;
        return report;
    }

    // A whole-artifact deletion removes header only once every body is gone: until then the artifact is valid with fewer
    // bodies, while a header-less cache/ that still held a body would be rejected by start and kept by clean.
    bool keptHeader = false;
    if (plan.deletesArtifact) {
        if (removals.bodyStayed)
            keptHeader = true;
        else {
            int error = 0;
            if (unlinkat(session->cache.get(), ArtifactNames::header.characters(), 0))
                error = errno;
            ++removals.unlinks;
            if (!error) {
                report.bytesReclaimed += header.bytes;
                removals.removedAnything = true;
            } else if (error != ENOENT) {
                keptHeader = true;
                addDiagnostic(report, Codes::unlinkFailed, makeString("cache/header: "_s, errorText(error)));
            }
            if (stopsAfterUnlinkForTesting(removals.unlinks)) {
                report.outcome = Outcome::Failed;
                return report;
            }
            if (!keptHeader)
                removeArtifactDirectories(*session, static_cast<bool>(*bodiesDirectory), report, removals);
        }
    }

    // Section 4.5: once after applying, a deletion that kept header included.
    session->lock->bumpEpoch();
    report.outcome = keptHeader ? Outcome::Failed : Outcome::Done;
    return report;
#else
    UNUSED_PARAM(parentPath);
    UNUSED_PARAM(ratio);
    UNUSED_PARAM(options);
    fail(report, Codes::platform, "maintenance runs only on Linux"_s);
    return report;
#endif
}

int runCommandLine(std::span<const CString> arguments, FILE* in, FILE* out, FILE* err)
{
    using namespace MaintenanceInternal;
    auto commandLine = parseCommandLine(arguments);
    int exitCode;
    if (!commandLine) {
        printLine(err, makeString("jitcache maintenance: "_s, commandLine.error()));
        printLine(err, "usage: clean [<path>]"_s);
        printLine(err, "       compact <ratio> [<path>] [--yes | --no]"_s);
        exitCode = usageExitCode;
    } else if (commandLine->command == CommandLine::Command::Clean) {
        Report report = clean(commandLine->path);
        printDiagnostics(err, report);
        printLine(out, cleanSummary(report));
        exitCode = exitCodeOf(report.outcome);
    } else
        exitCode = runCompact(*commandLine, in, out, err);

    if (out)
        fflush(out);
    if (err)
        fflush(err);
    return exitCode;
}

#if ENABLE(JITCACHE_TWINS)
namespace Testing {

void setFailingBodyUnlink(std::optional<uint64_t> n)
{
    MaintenanceInternal::failingBodyUnlinkForTesting.store(n.value_or(0));
}

void setStopAfterUnlink(std::optional<uint64_t> n)
{
    MaintenanceInternal::stopAfterUnlinkForTesting.store(n.value_or(0));
}

} // namespace Testing
#endif

} // namespace JSC::JITCache::Maintenance
