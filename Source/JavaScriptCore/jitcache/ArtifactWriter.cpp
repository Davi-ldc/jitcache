#include "config.h"
#include "ArtifactWriter.h"

#include "ArtifactStore.h"
#include "JITCacheBench.h"
#include "JITCacheContainer.h"
#include "JITCachePlatform.h"
#include "ProducerBudget.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/Locker.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if ENABLE(JITCACHE_TWINS)
#include <signal.h>
#include <wtf/Scope.h>
#endif

// The writer (container sub-SPEC section 8): it streams a body to a temporary in cache/, rereads and validates it,
// publishes it with renameat and keeps the shared index current. Every section is opaque bytes to it.
//
// Its file work sits behind OS(LINUX) (SPEC-integrator.md section 11). Elsewhere start rejects at start.platform, so no
// producing VM, and no writer, exists; a commit there fails at writer.create with ENOSYS, which only keeps the writer's
// interface compiling and linking.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ArtifactWriter);

namespace ArtifactWriterInternal {

#if OS(LINUX)

// The states a commit's system calls leave, in commit order (harness sub-SPEC section 12). Twins builds can kill the
// producer at each of them; other builds arm none.
enum class CommitPoint : uint8_t { BeforeCreate, AfterCreate, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterRename };

#if ENABLE(JITCACHE_TWINS)
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::BeforeCreate) == static_cast<uint8_t>(CommitPoint::BeforeCreate));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::AfterCreate) == static_cast<uint8_t>(CommitPoint::AfterCreate));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::MidStream) == static_cast<uint8_t>(CommitPoint::MidStream));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::AfterStream) == static_cast<uint8_t>(CommitPoint::AfterStream));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::AfterEnvelope) == static_cast<uint8_t>(CommitPoint::AfterEnvelope));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::AfterReread) == static_cast<uint8_t>(CommitPoint::AfterReread));
static_assert(static_cast<uint8_t>(ArtifactWriter::KillPoint::AfterRename) == static_cast<uint8_t>(CommitPoint::AfterRename));

static constexpr std::array<ASCIILiteral, 7> killPointNames {
    "before-create"_s, "after-create"_s, "mid-stream"_s, "after-stream"_s, "after-envelope"_s, "after-reread"_s, "after-rename"_s,
};
static constexpr std::array<ASCIILiteral, 4> injectableChecks { WriterChecks::create, WriterChecks::write, WriterChecks::reread, WriterChecks::publish };
#endif

// What the twins builds' hooks do to one commit (container sub-SPEC section 8.3): at most one injected fault and one kill
// point, both armed by the commit's ordinal. Other builds carry nothing.
struct CommitHooks {
#if ENABLE(JITCACHE_TWINS)
    std::optional<ASCIILiteral> fault; // the step that fails as if its call had failed with EIO
    std::optional<CommitPoint> killPoint;
#endif
};

// raise(SIGKILL) when the commit's armed kill point is this one, so no handler, destructor or exit hook runs and the
// process leaves exactly the files this point names.
static void reach(const CommitHooks& hooks, CommitPoint point)
{
#if ENABLE(JITCACHE_TWINS)
    if (hooks.killPoint != point)
        return;
    raise(SIGKILL);
    RELEASE_ASSERT_NOT_REACHED();
#else
    UNUSED_PARAM(hooks);
    UNUSED_PARAM(point);
#endif
}

static String errorText(int error)
{
    return String::fromUTF8(safeStrerror(error).data());
}

static std::unexpected<CommitFailure> failure(ASCIILiteral check, String&& detail)
{
    return std::unexpected(CommitFailure { check, WTF::move(detail) });
}

// The commit identifier (container sub-SPEC section 4.1): nonzero, drawn afresh for every write of a file.
static uint64_t freshCommitIdentifier()
{
    uint64_t identifier = 0;
    while (!identifier)
        cryptographicallyRandomValues(asMutableByteSpan(identifier));
    return identifier;
}

// The commit's temporary in cache/ (container sub-SPEC section 8.2, steps 3 to 7), named by section 1.1 and created with
// O_CREAT | O_EXCL, so a body enters bodies/ only through the rename. Unless the rename published it, the destructor
// unlinks it, best effort, which covers every failure after step 3.
class TemporaryBody {
    WTF_MAKE_NONCOPYABLE(TemporaryBody);
public:
    TemporaryBody(int cacheFd, const CommitHooks& hooks)
        : m_cacheFd(cacheFd)
        , m_name(temporaryFileName(TemporaryKind::Body))
#if ENABLE(JITCACHE_TWINS)
        , m_hooks(hooks)
        , m_fault(hooks.fault)
        , m_killAfterFirstWrite(hooks.killPoint == CommitPoint::MidStream)
#endif
    {
#if !ENABLE(JITCACHE_TWINS)
        UNUSED_PARAM(hooks);
#endif
    }

    ~TemporaryBody()
    {
        if (m_fd >= 0)
            ::close(m_fd);
        if (m_exists)
            ::unlinkat(m_cacheFd, m_name.data(), 0);
    }

    // Returns 0 or the errno of the openat.
    int create()
    {
        if (injectsFault(WriterChecks::create))
            return EIO;
        int fd;
        do {
            fd = ::openat(m_cacheFd, m_name.data(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            return errno;
        m_fd = fd;
        m_exists = true;
        return 0;
    }

    // Writes every byte at offset; a short write is retried from where it stopped. Returns 0 or the errno of the write
    // that failed.
    int writeAt(std::span<const uint8_t> bytes, uint64_t offset)
    {
        if (injectsFault(WriterChecks::write))
            return EIO;
        while (!bytes.empty()) {
            ssize_t count = ::pwrite(m_fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                return errno;
            }
            // A regular file accepts at least one byte or reports why not, so no progress is an I/O error, not a loop.
            if (!count)
                return EIO;
            bytes = bytes.subspan(static_cast<size_t>(count));
            offset += static_cast<uint64_t>(count);
        }
#if ENABLE(JITCACHE_TWINS)
        if (std::exchange(m_killAfterFirstWrite, false))
            reach(m_hooks, CommitPoint::MidStream);
#endif
        return 0;
    }

    // The mid-stream point is step 4's first write; a step 4 that wrote nothing, which only empty sections give, leaves
    // the same state as its end, where the point then falls. Step 5's writes never reach it.
    void didFinishStreaming()
    {
#if ENABLE(JITCACHE_TWINS)
        if (std::exchange(m_killAfterFirstWrite, false))
            reach(m_hooks, CommitPoint::MidStream);
#endif
    }

    // One pread at offset: the count read, 0 at the end of the file, or the errno.
    std::expected<size_t, int> readAt(std::span<uint8_t> buffer, uint64_t offset)
    {
        if (injectsFault(WriterChecks::reread))
            return std::unexpected(EIO);
        while (true) {
            ssize_t count = ::pread(m_fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (count >= 0)
                return static_cast<size_t>(count);
            if (errno != EINTR)
                return std::unexpected(errno);
        }
    }

    // Step 7's first half: the temporary's inode, which the rename keeps, then the descriptor is closed.
    std::expected<uint64_t, int> closeForPublishing()
    {
        struct stat status;
        if (fstat(m_fd, &status))
            return std::unexpected(errno);
        // Linux releases the descriptor whatever close returns; EINTR loses nothing, while another error reports a
        // write the kernel could not complete.
        if (::close(std::exchange(m_fd, -1)) && errno != EINTR)
            return std::unexpected(errno);
        return static_cast<uint64_t>(status.st_ino);
    }

    // Step 7's rename, which replaces any earlier version atomically, since cache/ and bodies/ share a file system.
    // Returns 0 or the errno.
    int publish(int bodiesFd, const BodyFileName& bodyName)
    {
        if (injectsFault(WriterChecks::publish))
            return EIO;
        if (::renameat(m_cacheFd, m_name.data(), bodiesFd, bodyName.data()))
            return errno;
        m_exists = false;
        return 0;
    }

private:
    // Twins builds: whether this is the armed step's first call, which then fails as if its system call had failed with
    // EIO, after the real work of the earlier steps.
    bool injectsFault(ASCIILiteral check)
    {
#if ENABLE(JITCACHE_TWINS)
        if (m_fault != check)
            return false;
        m_fault = std::nullopt;
        return true;
#else
        UNUSED_PARAM(check);
        return false;
#endif
    }

    const int m_cacheFd;
    const TemporaryFileName m_name;
    int m_fd { -1 };
    bool m_exists { false }; // created and not renamed into bodies/
#if ENABLE(JITCACHE_TWINS)
    const CommitHooks& m_hooks;
    std::optional<ASCIILiteral> m_fault;
    bool m_killAfterFirstWrite;
#endif
};

// Step 4's bytes, from the first section's offset to the end of the file, through the staging buffer: append copies
// into the buffer and flushes it to the file whenever it fills, and writeDirect flushes and then writes its bytes
// straight to the file. After a failed write every call returns false.
class SectionStream {
    WTF_MAKE_NONCOPYABLE(SectionStream);
public:
    SectionStream(TemporaryBody& file, std::span<uint8_t> staging, uint64_t offset)
        : m_file(file)
        , m_staging(staging)
        , m_bufferOffset(offset)
    {
        ASSERT(!m_staging.empty());
    }

    bool append(std::span<const uint8_t> bytes)
    {
        while (!bytes.empty()) {
            if (m_error)
                return false;
            size_t count = std::min(bytes.size(), m_staging.size() - m_buffered);
            memcpySpan(m_staging.subspan(m_buffered, count), bytes.first(count));
            m_buffered += count;
            bytes = bytes.subspan(count);
            if (m_buffered == m_staging.size())
                flush();
        }
        return !m_error;
    }

    bool writeDirect(std::span<const uint8_t> bytes)
    {
        if (!flush())
            return false;
        if (int error = m_file.writeAt(bytes, m_bufferOffset)) {
            m_error = error;
            return false;
        }
        m_bufferOffset += bytes.size();
        return true;
    }

    bool flush()
    {
        if (m_error)
            return false;
        if (!m_buffered)
            return true;
        if (int error = m_file.writeAt(m_staging.first(m_buffered), m_bufferOffset)) {
            m_error = error;
            return false;
        }
        m_bufferOffset += m_buffered;
        m_buffered = 0;
        return true;
    }

    uint64_t offset() const { return m_bufferOffset + m_buffered; } // where the next byte lands in the file
    int error() const { return m_error; } // the errno of the write that failed, or 0

private:
    TemporaryBody& m_file;
    const std::span<uint8_t> m_staging;
    uint64_t m_bufferOffset; // the file offset of the buffer's first byte
    size_t m_buffered { 0 };
    int m_error { 0 };
};

static constexpr std::array<uint8_t, 8> zeroPadding { };

static CommitFailure sectionFailure(SectionKind kind, uint64_t declared, uint64_t streamed, bool reportedFailure)
{
    ASCIILiteral sectionName = sectionKindDescription(kind).name;
    if (reportedFailure)
        return { WriterChecks::section, makeString(sectionName, ": the source declared "_s, declared, " bytes, streamed "_s, streamed, " and failed"_s) };
    return { WriterChecks::section, makeString(sectionName, ": the source declared "_s, declared, " bytes and streamed "_s, streamed) };
}

// Step 4: streams the sources in order to their offsets, with zeros between sections, and sets each entry's CRC from the
// bytes as they pass. A source in memory goes through the buffer, or straight to the file when larger than it; a
// streamed source writes through a sink that counts its bytes and refuses any past its size or after a failed write.
// Given a timing, each source's stream time runs from the end of the previous one's, or from the start of the step, to
// the end of its own bytes, its padding and the flushes its bytes filled included; the final flush joins the last
// source's. A source the step did not finish keeps its 0.
static std::optional<CommitFailure> streamSections(TemporaryBody& file, std::span<uint8_t> staging, std::span<const SectionSource> sources,
    std::span<BodyDirectoryEntry> entries, CommitTiming* timing)
{
    ASSERT(sources.size() == entries.size() && !entries.empty());
    uint64_t mark = timing ? benchThreadCPUNanoseconds() : 0;
    auto timeUntilNow = [&](SectionKind kind) {
        if (!timing)
            return;
        uint64_t now = benchThreadCPUNanoseconds();
        timing->streamNanoseconds[static_cast<size_t>(kind)] += now - mark;
        mark = now;
    };
    SectionStream stream(file, staging, entries[0].offset);
    auto writeFailure = [&] {
        return CommitFailure { WriterChecks::write, errorText(stream.error()) };
    };

    for (size_t index = 0; index < sources.size(); ++index) {
        const SectionSource& source = sources[index];
        BodyDirectoryEntry& entry = entries[index];
        ASSERT(entry.offset >= stream.offset() && entry.offset - stream.offset() < zeroPadding.size());
        if (!stream.append(std::span { zeroPadding }.first(static_cast<size_t>(entry.offset - stream.offset()))))
            return writeFailure();

        uint32_t crcState = ~0u;
        if (!source.stream) {
            // A source in memory promises exactly the bytes it holds.
            if (source.bytes.size() != source.size)
                return sectionFailure(source.kind, source.size, source.bytes.size(), false);
            crcState = crc32cExtend(crcState, source.bytes);
            bool written = source.bytes.size() > staging.size() ? stream.writeDirect(source.bytes) : stream.append(source.bytes);
            if (!written)
                return writeFailure();
            entry.crc = ~crcState;
            timeUntilNow(source.kind);
            continue;
        }

        uint64_t offered = 0; // the bytes the source handed the sink, refused ones included
        bool passedSize = false;
        auto sinkFunction = [&](std::span<const uint8_t> bytes) -> bool {
            if (stream.error() || passedSize)
                return false;
            // Sizes are bounded by the file's layout, which fits an off_t, so the sum cannot wrap.
            offered += bytes.size();
            if (offered > source.size) {
                passedSize = true;
                return false;
            }
            crcState = crc32cExtend(crcState, bytes);
            return stream.append(bytes);
        };
        SectionSink sink = sinkFunction;
        bool succeeded = source.stream(source.object, sink);
        if (stream.error())
            return writeFailure();
        // A count other than the size breaks the lane's size contract, and a failure the sink did not cause is the
        // source's own: both are the section's, not an I/O error.
        if (passedSize || offered != source.size || !succeeded)
            return sectionFailure(source.kind, source.size, offered, !succeeded && !passedSize && offered == source.size);
        entry.crc = ~crcState;
        timeUntilNow(source.kind);
    }

    if (!stream.flush())
        return writeFailure();
    timeUntilNow(sources.back().kind);
    return std::nullopt;
}

// Step 6: rereads the temporary from offset 0 in chunks of the staging buffer into a Full-mode validation stream, whatever
// strict says, and requires exactly the planned size and layout. Each section's checksum in the directory came from the
// bytes step 4 streamed, so a file that passes holds the lanes' bytes.
static std::optional<CommitFailure> rereadTemporary(TemporaryBody& file, std::span<uint8_t> staging, const BodyKey& key,
    std::span<const uint8_t, 16> headerDigest, uint64_t fileSize, const BodyLayout& plannedLayout)
{
    BodyValidationStream validation(key, headerDigest, ValidationMode::Full, fileSize);
    uint64_t offset = 0;
    // One chunk past the planned size is enough to see a longer file.
    while (offset <= fileSize) {
        auto count = file.readAt(staging, offset);
        if (!count)
            return CommitFailure { WriterChecks::reread, errorText(count.error()) };
        if (!*count)
            break;
        validation.append(staging.first(*count));
        offset += *count;
    }
    if (offset != fileSize) {
        if (offset > fileSize)
            return CommitFailure { WriterChecks::reread, makeString("the temporary holds more than the "_s, fileSize, " planned bytes"_s) };
        return CommitFailure { WriterChecks::reread, makeString("the temporary holds "_s, offset, " bytes where "_s, fileSize, " were planned"_s) };
    }
    auto layout = validation.finish();
    if (!layout)
        return CommitFailure { WriterChecks::reread, makeString("the temporary fails "_s, layout.error()) };
    if (*layout != plannedLayout)
        return CommitFailure { WriterChecks::reread, "the temporary's envelope or directory differs from the planned one"_s };
    return std::nullopt;
}

#if ENABLE(JITCACHE_TWINS)
// rewriteSection's detail for an open that found no usable body, by the outcome of container sub-SPEC section 7's table.
static String describeUnopenedBody(const BodyOpen& opened)
{
    switch (opened.outcome) {
    case StoreOutcome::Absent:
        return "open finds no body for the key"_s;
    case StoreOutcome::Unavailable:
        return "open cannot read the body now (EMFILE, ENFILE or ENOMEM)"_s;
    case StoreOutcome::Invalid:
        if (opened.failure.error)
            return makeString("open finds the body invalid at "_s, opened.failure.check, ": "_s, errorText(opened.failure.error));
        return makeString("open finds the body invalid at "_s, opened.failure.check);
    case StoreOutcome::Found:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}
#endif

#endif // OS(LINUX)

} // namespace ArtifactWriterInternal

// The sources (SPEC-integrator.md section 6.3).

SectionSource SectionSource::inMemory(SectionKind kind, std::span<const uint8_t> bytes)
{
    return SectionSource { kind, bytes.size(), bytes, nullptr, nullptr };
}

SectionSource SectionSource::streamed(SectionKind kind, uint64_t size, bool (*stream)(const void*, const SectionSink&), const void* object)
{
    ASSERT(stream);
    return SectionSource { kind, size, { }, stream, object };
}

CommitSections::CommitSections(std::span<const SectionSource> sources)
    : m_sources { }
    , m_count(static_cast<unsigned>(sources.size()))
{
    // Kinds in strictly increasing order are the directory's order of (type id, tier), at most one entry per kind.
    RELEASE_ASSERT(sources.size() <= numberOfSectionKinds);
    for (size_t index = 0; index < sources.size(); ++index) {
        RELEASE_ASSERT(!index || sources[index - 1].kind < sources[index].kind);
        m_sources[index] = sources[index];
    }
}

std::span<const SectionSource> CommitSections::sources() const
{
    return std::span { m_sources }.first(m_count);
}

// The writer (container sub-SPEC section 8).

ArtifactWriter::ArtifactWriter(OpenedArtifact& artifact, ProducerLock& producerLock, ProducerBudget& budget, size_t stagingBytes)
    : m_artifact(artifact)
    , m_producerLock(producerLock)
    , m_budget(budget)
    , m_stagingBytes(stagingBytes)
{
    RELEASE_ASSERT(m_stagingBytes);
}

ArtifactWriter::~ArtifactWriter()
{
    releaseStagingBuffer();
}

void ArtifactWriter::releaseStagingBuffer()
{
    if (!m_staging)
        return;
    m_staging = MallocSpan<uint8_t> { };
    m_budget.release(m_stagingBytes);
}

std::expected<CommitResult, CommitFailure> ArtifactWriter::commit(const CommitStamp& stamp, const CommitSections& sections, CommitTiming* timing)
{
    if (timing)
        *timing = { };
#if !OS(LINUX)
    UNUSED_PARAM(stamp);
    UNUSED_PARAM(sections);
    return std::unexpected(CommitFailure { WriterChecks::create, String::fromUTF8(safeStrerror(ENOSYS).data()) });
#else
    using namespace ArtifactWriterInternal;

    CommitHooks hooks;
#if ENABLE(JITCACHE_TWINS)
    ++m_commits;
    if (m_faultForTesting && m_faultForTesting->n == m_commits)
        hooks.fault = m_faultForTesting->check;
    if (m_killForTesting && m_killForTesting->n == m_commits)
        hooks.killPoint = static_cast<CommitPoint>(m_killForTesting->point);
#endif

    // Step 1. The buffer is charged before it is allocated, and kept until production ends.
    if (!m_staging) {
        if (!m_budget.tryCharge(m_stagingBytes))
            return failure(WriterChecks::budgetLimit, makeString("the writer's staging buffer of "_s, m_stagingBytes, " bytes"_s));
        m_staging = MallocSpan<uint8_t>::malloc(m_stagingBytes);
    }
    std::span<uint8_t> staging = m_staging.mutableSpan();

    // Step 2. The layout's arithmetic is overflow-checked, and every offset must fit the off_t the system calls take.
    auto sources = sections.sources();
    std::array<BodyDirectoryEntry, numberOfSectionKinds> entryStorage { };
    auto entries = std::span { entryStorage }.first(sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        entries[index].kind = sources[index].kind;
        entries[index].size = sources[index].size;
    }
    auto fileSize = layOutBodySections(entries);
    if (!fileSize || *fileSize > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
        return failure(WriterChecks::section, makeString("the sizes of the "_s, sources.size(), " sections give no body layout"_s));
    reach(hooks, CommitPoint::BeforeCreate);

    // Step 3.
    TemporaryBody temporary(m_artifact.cacheFd(), hooks);
    if (int error = temporary.create())
        return failure(WriterChecks::create, errorText(error));
    reach(hooks, CommitPoint::AfterCreate);

    // Step 4.
    if (auto streamFailure = streamSections(temporary, staging, sources, entries, timing))
        return std::unexpected(WTF::move(*streamFailure));
    temporary.didFinishStreaming();
    reach(hooks, CommitPoint::AfterStream);

    // Step 5. The envelope goes last, so a temporary that stops short of it never validates.
    std::array<uint8_t, numberOfSectionKinds * bodyDirectoryEntryBytes> directoryStorage { };
    auto directory = std::span { directoryStorage }.first(entries.size() * bodyDirectoryEntryBytes);
    BodyEnvelope envelope;
    envelope.sectionCount = static_cast<uint16_t>(entries.size());
    envelope.highestTier = stamp.highestTier;
    envelope.key = stamp.key;
    envelope.version = freshCommitIdentifier();
    memcpySpan(std::span { envelope.headerDigest }, m_artifact.headerDigest());
    envelope.fileSize = *fileSize;
    envelope.llintThreshold = stamp.llintThreshold;
    envelope.counterProgress = stamp.counterProgress;
    envelope.directoryCRC = encodeBodyDirectory(entries, directory);
    auto envelopeBytes = encodeBodyEnvelope(envelope);
    if (int error = temporary.writeAt(directory, bodyEnvelopeBytes))
        return failure(WriterChecks::write, errorText(error));
    if (int error = temporary.writeAt(envelopeBytes, 0))
        return failure(WriterChecks::write, errorText(error));
    reach(hooks, CommitPoint::AfterEnvelope);

    // Step 6.
    uint64_t rereadStart = timing ? benchThreadCPUNanoseconds() : 0;
    BodyLayout plannedLayout { envelope.version, envelope.highestTier, envelope.llintThreshold, envelope.counterProgress, { } };
    for (auto& entry : entries)
        plannedLayout.sections[static_cast<size_t>(entry.kind)] = SectionExtent { entry.offset, entry.size };
    if (auto rereadFailure = rereadTemporary(temporary, staging, stamp.key, m_artifact.headerDigest(), *fileSize, plannedLayout))
        return std::unexpected(WTF::move(*rereadFailure));
    if (timing)
        timing->rereadNanoseconds = benchThreadCPUNanoseconds() - rereadStart;
    reach(hooks, CommitPoint::AfterReread);

    // Step 7.
    uint64_t publishStart = timing ? benchThreadCPUNanoseconds() : 0;
    auto inode = temporary.closeForPublishing();
    if (!inode)
        return failure(WriterChecks::publish, errorText(inode.error()));
    if (int error = temporary.publish(m_artifact.bodiesFd(), bodyFileName(stamp.key)))
        return failure(WriterChecks::publish, errorText(error));
    reach(hooks, CommitPoint::AfterRename);

    // Step 8.
    didPublish(stamp.key, *inode, !!timing);
    if (timing)
        timing->publishNanoseconds = benchThreadCPUNanoseconds() - publishStart;
    return CommitResult { envelope.version, *fileSize };
#endif
}

void ArtifactWriter::didPublish(const BodyKey& key, uint64_t inode, bool timed)
{
    Locker locker { m_artifact.m_indexLock };
    // The update's own time, apart from the publish part that holds it, for the index statistics (SPEC-integrator.md
    // IB2). Wall time from the vDSO, so it adds no system call to the timed commit.
    MonotonicTime start = timed ? MonotonicTime::now() : MonotonicTime { };
    // The queue holds this commit's own event and those of changes made before the VM took the lock, which are applied
    // as a refresh applies them. An overflow or a read error marks a listing pending instead of listing inside the pause.
    m_artifact.drainEvents(false);
    uint64_t replacedEpoch = m_producerLock.bumpEpoch();
    // The bump proves the object current only when it replaced the last epoch the object saw and no listing is pending;
    // an object that was behind keeps its last epoch seen and refreshes at its next lookup.
    if (m_artifact.m_epoch && !m_artifact.m_listingPending && replacedEpoch == m_artifact.m_lastEpochSeen)
        m_artifact.m_lastEpochSeen = replacedEpoch + 1;
    // Section 6.3 keeps a gone object's index empty, so every later lookup misses without refreshing. The rename into the
    // pinned bodies/ directory succeeds when that directory was moved away before or after it, or removed after it, and
    // the drain above then marks the object gone: step 8 learns nothing, since section 8.2's unconditional learn would
    // contradict section 6.3.
    if (!m_artifact.m_gone)
        m_artifact.learn(key, inode);

    IndexStatistics& statistics = m_artifact.m_statistics;
    ++statistics.writerUpdates;
    if (timed) {
        ++statistics.timedWriterUpdates;
        statistics.writerUpdateNanoseconds += (MonotonicTime::now() - start).nanosecondsAs<uint64_t>();
    }
}

#if ENABLE(JITCACHE_TWINS)

void ArtifactWriter::setFaultForTesting(std::optional<FaultForTesting> fault)
{
    ASSERT(!fault || (fault->n && faultCheckNamed(StringView { fault->check })));
    m_faultForTesting = fault;
}

std::optional<ASCIILiteral> ArtifactWriter::faultCheckNamed(StringView checkName)
{
    for (ASCIILiteral check : ArtifactWriterInternal::injectableChecks) {
        if (checkName == check)
            return check;
    }
    return std::nullopt;
}

void ArtifactWriter::setKillForTesting(std::optional<KillForTesting> kill)
{
    ASSERT(!kill || kill->n);
    m_killForTesting = kill;
}

ASCIILiteral ArtifactWriter::name(KillPoint point)
{
    return ArtifactWriterInternal::killPointNames[static_cast<size_t>(point)];
}

std::optional<ArtifactWriter::KillPoint> ArtifactWriter::killPointNamed(StringView pointName)
{
    for (size_t index = 0; index < ArtifactWriterInternal::killPointNames.size(); ++index) {
        if (pointName == ArtifactWriterInternal::killPointNames[index])
            return static_cast<KillPoint>(index);
    }
    return std::nullopt;
}

std::expected<CommitResult, CommitFailure> ArtifactWriter::rewriteSection(const BodyKey& key, SectionKind kind, uint64_t offset, std::span<const uint8_t> bytes)
{
    using namespace ArtifactWriterInternal;

    // The key's current body, opened in Full mode by the store's own open (container sub-SPEC section 7.2): a key the
    // index lacks is absent without a system call, the store's fault hook applies, and an ENOENT erases the key's entry.
    // open finds only a body the index lists, so the commit's index update replaces that entry and never adds one that
    // nothing charged (SPEC-integrator.md section 4.4).
    BodyOpen opened = m_artifact.open(key, ValidationMode::Full);
    if (opened.outcome != StoreOutcome::Found)
        return failure(WriterChecks::rewrite, describeUnopenedBody(opened));
    RefPtr<ValidatedBody> body = WTF::move(opened.body);

    // The stamp comes from the body open validated: its key, tier, L and P (SPEC-integrator.md section 6.2).
    CommitStamp stamp { body->key(), body->llintThreshold(), body->counterProgress(), body->highestTier() };

    // Full validation leaves exactly the sections the highest tier requires (check B7), so they are the ones to copy, and
    // body->section gives each one, empty ones included.
    uint8_t highestTier = stamp.highestTier;
    ASCIILiteral sectionName = sectionKindDescription(kind).name;
    if (!isSectionRequired(kind, highestTier))
        return failure(WriterChecks::rewrite, makeString("the body holds no "_s, sectionName, " section"_s));
    uint64_t sectionSize = body->section(kind).size();
    if (offset > sectionSize || bytes.size() > sectionSize - offset)
        return failure(WriterChecks::rewrite, makeString(bytes.size(), " bytes at offset "_s, offset, " lie outside the "_s, sectionSize, " bytes of "_s, sectionName));

    // The copy, every section back to back, is production memory of the producing VM: charged before it is allocated and
    // released once it is freed.
    size_t copyBytes = 0;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto sectionKind = static_cast<SectionKind>(index);
        if (isSectionRequired(sectionKind, highestTier))
            copyBytes += body->section(sectionKind).size(); // the sections lie inside one mapped file, so the sum fits
    }
    if (!m_budget.tryCharge(copyBytes))
        return failure(WriterChecks::budgetLimit, makeString("the rewrite's copy of "_s, copyBytes, " bytes"_s));
    auto releaseCopyCharge = makeScopeExit([&] {
        m_budget.release(copyBytes);
    });
    MallocSpan<uint8_t> copy = MallocSpan<uint8_t>::malloc(copyBytes);

    std::array<SectionSource, numberOfSectionKinds> sources { };
    size_t count = 0;
    size_t copied = 0;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto sectionKind = static_cast<SectionKind>(index);
        if (!isSectionRequired(sectionKind, highestTier))
            continue;
        auto section = body->section(sectionKind);
        auto target = copy.mutableSpan().subspan(copied, section.size());
        if (!section.empty())
            memcpySpan(target, section);
        if (sectionKind == kind && !bytes.empty())
            memcpySpan(target.subspan(static_cast<size_t>(offset), bytes.size()), bytes);
        sources[count++] = SectionSource::inMemory(sectionKind, target);
        copied += section.size();
    }
    // The copy holds every byte the commit reads, so the mapping goes now. Like the scoring read's (SPEC-integrator.md
    // section 4.4), that mapping borrows page-cache pages of the body file and is not charged.
    body = nullptr;

    return commit(stamp, CommitSections { std::span { sources }.first(count) });
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC::JITCache
