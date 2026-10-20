#include "config.h"
#include "JITCacheMaintenance.h"

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "BodyKey.h"
#include "JITCacheAPI.h"
#include "JITCacheContainer.h"
#include "JITCacheFiles.h"
#include "JITCacheMaintenanceTesting.h"
#include "JITCacheParameters.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string.h>
#include <wtf/ASCIICType.h>
#include <wtf/FastFloat.h>
#include <wtf/HashSet.h>
#include <wtf/MallocSpan.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/unix/UnixFileDescriptor.h>
#endif

// The maintenance backend and its command line. compact needs no VM and no JSC::initialize: it runs on the calling
// thread, uses the filesystem, SHA-256 and CRC32C, and holds the producer lock of <parent> for all its work. It first
// removes what interrupted writes left, then evicts by plan. It judges bodies by the pack's index, reads no section to
// plan, and never compares the header with the running process, since the command line may run under another binary than
// the one that produced the artifact. It applies a plan that keeps bodies as a compaction of the pack.

namespace JSC::JITCache::Maintenance {

namespace MaintenanceInternal {

// The diagnostic codes a report carries and the command line prints.
namespace Codes {
inline constexpr ASCIILiteral badHeader = "bad-header"_s;
inline constexpr ASCIILiteral badRatio = "bad-ratio"_s;
inline constexpr ASCIILiteral damagedBody = "damaged-body"_s;
inline constexpr ASCIILiteral damagedPack = "damaged-pack"_s;
inline constexpr ASCIILiteral incompatiblePack = "incompatible-pack"_s;
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
// The test hooks; 0 and false leave every apply alone.
static std::atomic<uint64_t> failingBodyUnlinkForTesting { 0 };
static std::atomic<uint64_t> stopAfterUnlinkForTesting { 0 };
static std::atomic<bool> failingCompactionForTesting { false };
#endif

#if OS(LINUX)

// Names as diagnostics print them, relative to <parent>.
static constexpr ASCIILiteral cachePrefix = "cache/"_s;

static constexpr int parentFlags = O_PATH | O_DIRECTORY;
static constexpr int directoryFlags = O_RDONLY | O_DIRECTORY;

// Takes ownership of a descriptor openAt returned, which then closes when the object goes; -1 gives an empty one.
static UnixFileDescriptor ownedDescriptor(int fd)
{
    return UnixFileDescriptor { fd, UnixFileDescriptor::Adopt };
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

static std::expected<UnixFileDescriptor, int> openDirectory(int directoryFd, ASCIILiteral name)
{
    UnixFileDescriptor fd = ownedDescriptor(openAt(directoryFd, name.characters(), directoryFlags));
    if (!fd)
        return std::unexpected(errno);
    return fd;
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
    UnixFileDescriptor parent;
    UnixFileDescriptor cache;
    std::unique_ptr<ProducerLock> lock;
};

// Opens <parent> and looks for cache/ without the lock, so a path without an artifact gets no lock file; then takes the
// lock without blocking and opens cache/ again under it. Without a session, report holds the outcome: NoArtifact, Busy,
// or Failed at io.
static std::optional<Session> beginSession(const String& parentPath, Report& report)
{
    std::optional<Session> session { std::in_place };
    UTF8CString path = parentPath.utf8();
    session->parent = ownedDescriptor(openAt(AT_FDCWD, path.isNull() ? "" : path.legacyCStringPointer(), parentFlags));
    if (!session->parent) {
        int error = errno;
        if (isAbsence(error))
            report.outcome = Outcome::NoArtifact;
        else
            fail(report, Codes::io, makeString("opening "_s, parentPath, ": "_s, errorText(error)));
        return std::nullopt;
    }
    if (auto cache = openDirectory(session->parent.value(), ArtifactNames::cacheDirectory); !cache) {
        failToOpenCache(report, cache.error());
        return std::nullopt;
    }

    auto lock = ProducerLock::tryAcquire(session->parent.value());
    if (!lock) {
        if (lock.error().busy)
            report.outcome = Outcome::Busy;
        else
            fail(report, Codes::io, makeString(ArtifactNames::lockFile, ": "_s, errorText(lock.error().error)));
        return std::nullopt;
    }
    session->lock = WTF::move(*lock);

    // A cache/ that went away before the lock was taken is no artifact.
    auto cache = openDirectory(session->parent.value(), ArtifactNames::cacheDirectory);
    if (!cache) {
        failToOpenCache(report, cache.error());
        return std::nullopt;
    }
    session->cache = WTF::move(*cache);
    return session;
}

// The header as maintenance reads it: its own checks and its digest, without the comparison with the running process.
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
    auto header = readHeaderFile(cacheFd);
    if (!header) {
        int error = header.error().error;
        bool opening = header.error().step == HeaderFileFailure::Step::Open;
        if (opening && error == ENOENT)
            return result;
        result.state = HeaderState::Bad;
        result.problem = makeString(opening ? "cache/header cannot be opened: "_s : "cache/header cannot be read: "_s, errorText(error));
        return result;
    }

    auto file = header->span();
    if (auto checked = readArtifactHeader(file); !checked) {
        result.state = HeaderState::Bad;
        auto verdict = checked.error().verdict == HeaderVerdict::Incompatible ? "incompatible"_s : "corrupt"_s;
        result.problem = makeString("cache/header is "_s, verdict, ": "_s, checked.error().reason);
        return result;
    }
    result.state = HeaderState::Valid;
    result.digest = artifactHeaderDigest(file);
    result.bytes = file.size();
    return result;
}

// The names of cache/ beside header and pack: the temporaries compact removes and the names it only reports.
struct CacheListing {
    Vector<ASCIICString> temporaries;
    Vector<String> unknownNames;
    bool hasPack { false };
};

static std::expected<CacheListing, int> listCache(int cacheFd)
{
    CacheListing listing;
    int error = listDirectory(cacheFd, [&](std::span<const char> name, uint64_t) {
        if (equalSpans(name, ArtifactNames::header.span()))
            return;
        if (equalSpans(name, ArtifactNames::pack.span())) {
            listing.hasPack = true;
            return;
        }
        if (temporaryKindOfFileName(name)) {
            listing.temporaries.append(ASCIICString { name });
            return;
        }
        listing.unknownNames.append(displayName(cachePrefix, name));
    });
    if (error)
        return std::unexpected(error);
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

// The order in which the plan evicts: by score, then by key bytes, both ascending.
static bool isEvictedBefore(const Eviction& a, const Eviction& b)
{
    if (a.score != b.score)
        return a.score < b.score;
    return a.key < b.key;
}

struct PackRead {
    RefPtr<MappedPack> pack; // null without a pack, or for a damaged one
    uint64_t validEnd { 0 }; // the root
    Vector<PackIndexEntry> entries; // the live entries, in strictly increasing key order
    uint64_t fileBytes { 0 }; // the file's size, the bytes past the valid end included
    uint64_t damagedBytes { 0 }; // a damaged pack's size
    PackRejection otherLayout; // set for a pack of another layout, which leaves no plan
};

static bool keyPrecedes(const PackIndexEntry& a, const PackIndexEntry& b)
{
    return std::ranges::lexicographical_compare(a.key.bytes(), b.key.bytes());
}

// The pack as planning reads it: its prefix, its root and its index, which repeats every body's L, P, size and commit
// identifier, so planning reads no body. A pack that fails its checks is evicted whole, while a pack of another layout,
// which another binary can still read, is incompatible and left alone.
static std::expected<PackRead, String> readPack(int cacheFd, const HeaderDigest& headerDigest, Report& report)
{
    PackRead result;
    struct stat status;
    if (fstatat(cacheFd, ArtifactNames::pack.characters(), &status, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT)
            return result;
        return std::unexpected(makeString("reading cache/pack: "_s, errorText(errno)));
    }
    result.fileBytes = static_cast<uint64_t>(status.st_size);
    auto damaged = [&](PackRejection rejection) -> std::expected<PackRead, String> {
        result.pack = nullptr;
        result.damagedBytes = result.fileBytes;
        addDiagnostic(report, Codes::damagedPack, makeString("cache/pack: "_s, rejection));
        return WTF::move(result);
    };
    auto mapped = MappedPack::map(cacheFd, std::span<const uint8_t, 16> { headerDigest }, 0);
    if (!mapped) {
        if (mapped.error().error)
            return std::unexpected(makeString("reading cache/pack: "_s, errorText(mapped.error().error)));
        if (mapped.error().otherLayout) {
            result.otherLayout = mapped.error().packRejection;
            return result;
        }
        return damaged(mapped.error().packRejection);
    }
    // The producer lock keeps the file as it was when its size was taken, which the root's valid end therefore stays in.
    if (mapped->validEnd > result.fileBytes)
        return damaged("the root's valid end passes the file"_s);
    PackIndex index;
    uint64_t liveBytes = packPrefixBytes;
    auto read = readPackIndex(mapped->validEnd, std::nullopt, index, liveBytes, [&](uint64_t offset, uint64_t size) {
        return mapped->pack->bytes(offset, size);
    });
    if (!read)
        return damaged(read.error());
    result.pack = mapped->pack.ptr();
    result.validEnd = mapped->validEnd;
    for (auto& entry : index.values())
        result.entries.append(entry);
    std::ranges::sort(result.entries, keyPrecedes);
    return result;
}

// Every body the pack's index lists.
static void appendPackedBodies(const PackRead& pack, Vector<Eviction>& bodies)
{
    for (auto& entry : pack.entries) {
        Eviction eviction { };
        std::ranges::copy(entry.key.bytes(), eviction.key.begin());
        eviction.bytes = entry.size;
        eviction.version = entry.version;
        eviction.score = (static_cast<double>(entry.llintThreshold) + static_cast<double>(entry.counterProgress)) / static_cast<double>(entry.size);
        bodies.append(eviction);
    }
}

// The plan over every body's record: the target the ratio gives, then the bodies taken in eviction order until their
// bytes reach it. The plan keeps the records of the bodies it evicts.
static Plan makePlan(const HeaderDigest& headerDigest, uint64_t headerBytes, uint64_t damagedPackBytes, double ratio, Vector<Eviction>&& bodies)
{
    Plan plan { };
    plan.headerDigest = headerDigest;
    plan.postCleanBytes = headerBytes + damagedPackBytes;
    for (auto& body : bodies)
        plan.postCleanBytes += body.bytes;
    plan.targetBytes = targetBytesFor(ratio, plan.postCleanBytes);

    // A damaged pack goes first, before any body, whenever the target is above zero; a ratio of 0 evicts nothing.
    if (plan.targetBytes && damagedPackBytes) {
        plan.damagedPackBytes = damagedPackBytes;
        plan.evictedBytes = damagedPackBytes;
    }
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

// The cleanup's temporaries: each one's size, taken with fstatat before its unlinkat, counts once it is gone.
static void removeTemporaries(int cacheFd, const Vector<ASCIICString>& temporaries, Report& report)
{
    for (auto& name : temporaries) {
        struct stat status;
        if (fstatat(cacheFd, name.data(), &status, AT_SYMLINK_NOFOLLOW)) {
            int error = errno;
            if (error != ENOENT)
                addDiagnostic(report, Codes::unlinkFailed, makeString(cachePrefix, String { name }, ": "_s, errorText(error)));
            continue;
        }
        if (unlinkat(cacheFd, name.data(), 0)) {
            int error = errno;
            if (error != ENOENT)
                addDiagnostic(report, Codes::unlinkFailed, makeString(cachePrefix, String { name }, ": "_s, errorText(error)));
            continue;
        }
        ++report.temporariesRemoved;
        report.bytesReclaimed += static_cast<uint64_t>(status.st_size);
    }
}

// The cleanup also removes a .cache.replaced that a ConsumerProducer's replacement left when it stopped midway,
// counting its files' sizes; a name in it that is no artifact file stays, and with it the directory.
static void removeReplacedLeftover(int parentFd, Report& report)
{
    uint64_t bytes = 0;
    int error = removeReplacedArtifact(parentFd, bytes);
    report.bytesReclaimed += bytes;
    if (error)
        addDiagnostic(report, Codes::unlinkFailed, makeString(ArtifactNames::replacedCacheDirectory, "/: "_s, errorText(error)));
}

static void reportUnknownFiles(Vector<String>&& names, Report& report)
{
    for (auto& name : names)
        addDiagnostic(report, Codes::unknownFile, WTF::move(name));
}

// The last step of a whole-artifact deletion and of a bodiless remnant's removal: cache/. A failure leaves a remnant
// without a body, which the next Producer reuses or the next compact finishes.
static void removeCacheDirectory(const Session& session, Report& report)
{
    if (unlinkat(session.parent.value(), ArtifactNames::cacheDirectory.characters(), AT_REMOVEDIR)) {
        int error = errno;
        addDiagnostic(report, Codes::rmdirFailed, makeString("cache/: "_s, errorText(error)));
    }
}

// A header-less cache/ is a remnant, which a creation or a whole-artifact deletion that stopped midway leaves. One whose
// pack may hold a body keeps it, since compact never removes a committed body, and one that holds an unknown name stays
// for the next Producer to reuse; any other goes, its pack first and then cache/.
static void finishRemnant(const Session& session, const CacheListing& cache, Report& report)
{
    if (cache.hasPack && packHoldsBodies(session.cache.value())) {
        addDiagnostic(report, Codes::remnantWithBodies, "cache/ has no header and its pack holds bodies"_s);
        return;
    }
    if (!cache.unknownNames.isEmpty())
        return;
    if (cache.hasPack) {
        struct stat status;
        bool sized = !fstatat(session.cache.value(), ArtifactNames::pack.characters(), &status, AT_SYMLINK_NOFOLLOW);
        // An object that maps the pack meets the mark at its next read and looks at the path again.
        if (int error = retirePack(session.cache.value())) {
            addDiagnostic(report, Codes::unlinkFailed, makeString("cache/pack: marking it retired: "_s, errorText(error)));
            return;
        }
        if (unlinkat(session.cache.value(), ArtifactNames::pack.characters(), 0) && errno != ENOENT) {
            addDiagnostic(report, Codes::unlinkFailed, makeString("cache/pack: "_s, errorText(errno)));
            return;
        }
        if (sized)
            report.bytesReclaimed += static_cast<uint64_t>(status.st_size);
    }
    removeCacheDirectory(session, report);
}

// The key of an eviction the plan made from a body name or a pack index entry, whose bytes BodyKey::fromBytes accepted.
static BodyKey evictedKey(const Eviction& eviction)
{
    auto key = BodyKey::fromBytes(std::span<const uint8_t, BodyKey::byteSize> { eviction.key });
    RELEASE_ASSERT(key);
    return *key;
}

// The pack opened for writing, when the path still names the file planning mapped.
static UnixFileDescriptor openPackForWriting(int cacheFd, const PackRead& pack, Report& report)
{
    UnixFileDescriptor fd = ownedDescriptor(openAt(cacheFd, ArtifactNames::pack.characters(), O_RDWR));
    if (!fd) {
        addDiagnostic(report, Codes::io, makeString("cache/pack: "_s, errorText(errno)));
        return fd;
    }
    struct stat status;
    if (fstat(fd.value(), &status) || static_cast<uint64_t>(status.st_ino) != pack.pack->inode())
        return UnixFileDescriptor { };
    return fd;
}

// The pack's cleanup: the bytes past the valid end go, which commits that never stored their root left, and a retired
// mark the path's file carries, which only a compaction that died between its mark and its rename leaves, is cleared. It
// runs before a rewrite too, so a rewrite that fails, as on a full disk, still leaves the pack cleaned. Returns the file's
// size after it.
static uint64_t cleanUpPack(int cacheFd, const PackRead& pack, Report& report)
{
    UnixFileDescriptor fd = openPackForWriting(cacheFd, pack, report);
    if (!fd)
        return pack.fileBytes;
    if (pack.pack->isRetired()) {
        uint64_t zero = 0;
        if (pwrite(fd.value(), &zero, sizeof(zero), static_cast<off_t>(packRetiredOffset)) != static_cast<ssize_t>(sizeof(zero)))
            addDiagnostic(report, Codes::io, makeString("cache/pack: clearing its retired mark: "_s, errorText(errno)));
    }
    if (pack.fileBytes <= pack.validEnd)
        return pack.fileBytes;
    if (ftruncate(fd.value(), static_cast<off_t>(pack.validEnd))) {
        addDiagnostic(report, Codes::io, makeString("cache/pack: truncating it to its valid end: "_s, errorText(errno)));
        return pack.fileBytes;
    }
    report.bytesReclaimed += pack.fileBytes - pack.validEnd;
    return pack.validEnd;
}

// A whole deletion's first step: an empty pack replaces the pack, which is retired first, so every reader that maps it
// drops its bodies. An interruption after it leaves a valid artifact without bodies, and one after the header's unlink a
// remnant whose pack holds no body, which the next compact finishes and the next Producer reuses.
static int emptyPack(int cacheFd, const PackRead& pack, const HeaderDigest& headerDigest, Report& report)
{
    if (pack.pack) {
        if (UnixFileDescriptor fd = openPackForWriting(cacheFd, pack, report)) {
            uint64_t retired = 1;
            if (pwrite(fd.value(), &retired, sizeof(retired), static_cast<off_t>(packRetiredOffset)) != static_cast<ssize_t>(sizeof(retired)))
                return errno ? errno : EIO;
        }
    }
    return createEmptyPack(cacheFd, std::span<const uint8_t, 16> { headerDigest });
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

// The command line's exit codes by outcome; a usage error is 2.
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
    String path;
    double ratio { 0 };
    Answer answer { Answer::Ask };
};

static String displayArgument(const UTF8CString& argument)
{
    return String::fromUTF8ReplacingInvalidSequences(byteCast<Latin1Character>(argument.span()));
}

// A path is UTF-8, as the String the backend takes is.
static std::optional<String> pathArgument(const UTF8CString& argument)
{
    if (!argument.length())
        return emptyString();
    String path = String::fromUTF8(argument.span());
    if (path.isNull())
        return std::nullopt;
    return path;
}

// A decimal number, read without the locale; the backend rejects one outside [0, 1] as bad-ratio.
static std::optional<double> ratioArgument(const UTF8CString& argument)
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

// "compact <ratio> [<path>] [--yes | --no]", the ratio optional and 0 without it under RatioArgument::Optional, the path
// defaulting to defaultArtifactPath; a usage error says why.
static std::expected<CommandLine, String> parseCommandLine(std::span<const UTF8CString> arguments, RatioArgument ratioRule)
{
    if (arguments.empty())
        return std::unexpected(String { "no command"_s });
    if (!equalSpans(arguments[0].span(), "compact"_span))
        return std::unexpected(makeString("unknown command "_s, displayArgument(arguments[0])));

    CommandLine commandLine;
    auto ratio = arguments.size() > 1 ? ratioArgument(arguments[1]) : std::nullopt;
    if (ratio)
        commandLine.ratio = *ratio;
    else if (ratioRule == RatioArgument::Required) {
        if (arguments.size() < 2)
            return std::unexpected(String { "compact takes a ratio"_s });
        return std::unexpected(makeString("the ratio "_s, displayArgument(arguments[1]), " is not a number"_s));
    }

    // Every argument that starts with "--" is an option, so a mistyped one is never taken for a path.
    std::optional<String> path;
    bool answered = false;
    for (const UTF8CString& argument : arguments.subspan(ratio ? 2 : 1)) {
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
        if (!yes && !equalSpans(text, "--no"_span))
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
    UTF8CString text = line.utf8();
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
    // second call applies only an unchanged plan.
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

Report compact(const String& parentPath, double ratio, const CompactOptions& options)
{
    using namespace MaintenanceInternal;
    Report report;
#if OS(LINUX)
    // The plan is made on the artifact as the cleanup would leave it, temporaries left out of every count, and the cleanup
    // itself runs only when the plan is applied, so a plan that is never applied changes nothing.
    auto session = beginSession(parentPath, report);
    if (!session)
        return report;

    // Before anything is removed; NaN fails both tests.
    if (!(ratio >= 0 && ratio <= 1)) {
        fail(report, Codes::badRatio, makeString("the ratio "_s, ratio, " is not in [0, 1]"_s));
        return report;
    }

    // Without a header that passes its checks, or with a pack of another layout, there is no plan, and the cleanup does
    // only what names allow: the pack is read against the header's digest.
    HeaderRead header = readHeader(session->cache.value());
    std::optional<PackRead> pack;
    if (header.state == HeaderState::Valid) {
        auto read = readPack(session->cache.value(), header.digest, report);
        if (!read) {
            fail(report, Codes::io, WTF::move(read.error()));
            return report;
        }
        pack = WTF::move(*read);
    }
    if (pack && pack->otherLayout.isNull()) {
        Vector<Eviction> everyBody;
        appendPackedBodies(*pack, everyBody);
        report.plan = makePlan(header.digest, header.bytes, pack->damagedBytes, ratio, WTF::move(everyBody));

        // A confirmed plan is applied only when planning again under this lock gives it back field for field, header
        // digest included; a plan that deletes the artifact waits for an answer.
        if (options.confirmedPlan && *options.confirmedPlan != *report.plan) {
            report.outcome = Outcome::PlanChanged;
            return report;
        }
        if (report.plan->deletesArtifact) {
            if (options.answer == Answer::Ask) {
                report.outcome = Outcome::NeedsConfirmation;
                return report;
            }
            if (options.answer == Answer::No) {
                report.outcome = Outcome::Declined;
                return report;
            }
        }
    }

    // Under the lock the plan was made under: first the cleanup of what interrupted writes left, then the plan.
    auto cache = listCache(session->cache.value());
    if (!cache) {
        fail(report, Codes::io, makeString("listing cache/: "_s, errorText(cache.error())));
        return report;
    }
    removeTemporaries(session->cache.value(), cache->temporaries, report);
    removeReplacedLeftover(session->parent.value(), report);
    if (header.state == HeaderState::Absent)
        finishRemnant(*session, *cache, report);
    reportUnknownFiles(WTF::move(cache->unknownNames), report);
    if (header.state == HeaderState::Absent) {
        report.outcome = Outcome::NoArtifact;
        return report;
    }
    if (header.state == HeaderState::Bad) {
        fail(report, Codes::badHeader, WTF::move(header.problem));
        return report;
    }
    if (!pack->otherLayout.isNull()) {
        fail(report, Codes::incompatiblePack, makeString("cache/pack is incompatible: "_s, pack->otherLayout));
        return report;
    }
    const Plan& plan = *report.plan;

    // A plan that keeps bodies is applied as a compaction: a new pack of every body the plan keeps, without dead bytes,
    // replaces the old one in one rename. A survivor that fails its checks on the way is left out as a damaged body. A pack
    // without dead bytes and evictions keeps its file after its cleanup only when every body in it passes the checks the
    // compaction would run, so a body that rotted in a dense pack, as every delta leaves one, still goes.
    if (!plan.deletesArtifact && pack->pack) {
        uint64_t packBytes = cleanUpPack(session->cache.value(), *pack, report);
        uint64_t liveBytes = packPrefixBytes;
        for (auto& entry : pack->entries)
            liveBytes += *packRecordExtent(entry.size);
        bool rewrites = !plan.evictions.isEmpty() || liveBytes + packIndexBlockBytes(static_cast<uint32_t>(pack->entries.size())) < pack->validEnd;
        if (!rewrites) {
            rewrites = std::ranges::any_of(pack->entries, [&](const PackIndexEntry& entry) {
                return !!ArtifactWriter::checkPackedBody(*pack->pack, entry, std::span<const uint8_t, 16> { header.digest });
            });
        }
        if (rewrites) {
            UnixFileDescriptor fd = openPackForWriting(session->cache.value(), *pack, report);
            if (!fd) {
                fail(report, Codes::io, "cache/pack cannot be opened for writing"_s);
                return report;
            }
            HashSet<BodyKey, BodyKeyHash, BodyKeyHashTraits> evicted;
            for (auto& eviction : plan.evictions)
                evicted.add(evictedKey(eviction));
            CompactionTarget target { session->cache.value(), std::span<const uint8_t, 16> { header.digest } };
            target.dropsDamagedBodies = true;
#if ENABLE(JITCACHE_TWINS)
            if (failingCompactionForTesting.load())
                target.faultForTesting = CompactionChecks::write;
#endif
            CompactionSource source { *pack->pack, pack->validEnd, pack->entries.span(), fd.value() };
            auto compacted = ArtifactWriter::compactPack(target, source, [&](const BodyKey& key) { return evicted.contains(key); }, false);
            if (!compacted) {
                fail(report, Codes::io, makeString(compacted.error().check, ": "_s, compacted.error().detail));
                return report;
            }
            report.bodiesEvicted += plan.evictions.size();
            for (auto& [key, check] : compacted->droppedBodies) {
                ++report.bodiesEvicted;
                addDiagnostic(report, Codes::damagedBody, makeString("cache/pack: "_s, String::fromLatin1(std::span<const char> { bodyKeyHexDigits(key) }), ": "_s, check));
            }
            if (packBytes > compacted->packBytes)
                report.bytesReclaimed += packBytes - compacted->packBytes;
        }
    }
    // A damaged pack leaves no body to keep, so any ratio above 0 deletes the artifact, and ratio 0 leaves the pack.

    // A whole-artifact deletion empties the pack, then removes the header, then the empty pack and cache/: until the
    // header goes the artifact is valid, and after it a remnant whose pack holds no body. The test hooks count its
    // unlinks, of which the pack's emptying is the one body unlink.
    bool keptHeader = false;
    if (plan.deletesArtifact) {
        uint64_t bodyUnlinks = 0;
        uint64_t unlinks = 0;
        int error = bodyUnlinkFailsForTesting(++bodyUnlinks) ? EIO : emptyPack(session->cache.value(), *pack, header.digest, report);
        ++unlinks;
        // The pack's bytes count once: what the empty pack does not hold now, the rest with the empty pack's unlink.
        uint64_t emptyPackBytes = std::min<uint64_t>(packPrefixBytes + packIndexBlockBytes(0), pack->fileBytes);
        if (!error) {
            report.bodiesEvicted += plan.evictions.size();
            report.bytesReclaimed += pack->fileBytes - emptyPackBytes;
        } else {
            keptHeader = true;
            addDiagnostic(report, Codes::unlinkFailed, makeString("cache/pack: "_s, errorText(error)));
        }
        if (stopsAfterUnlinkForTesting(unlinks)) {
            report.outcome = Outcome::Failed;
            return report;
        }
        if (!keptHeader) {
            if (unlinkat(session->cache.value(), ArtifactNames::header.characters(), 0) && errno != ENOENT) {
                keptHeader = true;
                addDiagnostic(report, Codes::unlinkFailed, makeString("cache/header: "_s, errorText(errno)));
            } else
                report.bytesReclaimed += header.bytes;
            ++unlinks;
            if (stopsAfterUnlinkForTesting(unlinks)) {
                report.outcome = Outcome::Failed;
                return report;
            }
        }
        if (!keptHeader) {
            // A reader that met the old pack's mark since the emptying may map the empty pack, so it is retired before it
            // goes too.
            if (int error = retirePack(session->cache.value()))
                addDiagnostic(report, Codes::unlinkFailed, makeString("cache/pack: marking it retired: "_s, errorText(error)));
            else if (unlinkat(session->cache.value(), ArtifactNames::pack.characters(), 0) && errno != ENOENT)
                addDiagnostic(report, Codes::unlinkFailed, makeString("cache/pack: "_s, errorText(errno)));
            else {
                report.bytesReclaimed += emptyPackBytes;
                removeCacheDirectory(*session, report);
            }
        }
    }

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

int runCommandLine(std::span<const UTF8CString> arguments, FILE* in, FILE* out, FILE* err, RatioArgument ratioRule)
{
    using namespace MaintenanceInternal;
    auto commandLine = parseCommandLine(arguments, ratioRule);
    int exitCode;
    if (!commandLine) {
        printLine(err, makeString("jitcache maintenance: "_s, commandLine.error()));
        printLine(err, ratioRule == RatioArgument::Optional ? "usage: compact [<ratio>] [<path>] [--yes | --no]"_s : "usage: compact <ratio> [<path>] [--yes | --no]"_s);
        exitCode = usageExitCode;
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

void setFailingCompaction(bool fails)
{
    MaintenanceInternal::failingCompactionForTesting.store(fails);
}

} // namespace Testing
#endif

} // namespace JSC::JITCache::Maintenance
