#include "config.h"
#include "ArtifactWriter.h"

#include "ArtifactStore.h"
#include "JITCacheBench.h"
#include "JITCacheContainer.h"
#include "JITCacheParameters.h"
#include "JITCachePlatform.h"
#include "ProducerBudget.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <utility>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/Locker.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if ENABLE(JITCACHE_TWINS)
#include <signal.h>
#endif

// The writer: it appends records and index blocks to the pack, stores the root, and compacts. Every section is opaque
// bytes to it.
//
// Its file work sits behind OS(LINUX). Elsewhere start rejects at start.platform, so no producing VM, and no writer,
// exists; a commit there fails at writer.open with ENOSYS, which only keeps the writer's interface compiling and linking.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ArtifactWriter);

namespace ArtifactWriterInternal {

using KillPoint = ArtifactWriter::KillPoint;

#if ENABLE(JITCACHE_TWINS)
static constexpr std::array<ASCIILiteral, 9> killPointNames {
    "before-append"_s, "mid-stream"_s, "after-stream"_s, "after-envelope"_s, "after-reread"_s, "after-index"_s, "after-root"_s,
    "compaction-before-rename"_s, "compaction-after-rename"_s,
};
static constexpr std::array<ASCIILiteral, 3> injectableChecks { WriterChecks::openPack, WriterChecks::write, WriterChecks::reread };
static constexpr std::array<ASCIILiteral, 4> injectableCompactionChecks { CompactionChecks::create, CompactionChecks::write, CompactionChecks::reread, CompactionChecks::publish };

static bool isCompactionCheck(ASCIILiteral check)
{
    return std::ranges::find(injectableCompactionChecks, check) != injectableCompactionChecks.end();
}

static bool isCompactionPoint(KillPoint point)
{
    return point == KillPoint::CompactionBeforeRename || point == KillPoint::CompactionAfterRename;
}
#endif

// What the twins builds' hooks do to one commit: at most one injected fault and one kill point. Other builds carry
// nothing.
struct CommitHooks {
#if ENABLE(JITCACHE_TWINS)
    std::optional<ASCIILiteral> fault; // the step that fails as if its call had failed with EIO
    std::optional<KillPoint> killPoint;
#endif
};

// raise(SIGKILL) when the armed kill point is this one, so no handler, destructor or exit hook runs and the process leaves
// exactly the bytes this point names.
static void reach(const CommitHooks& hooks, KillPoint point)
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

// Twins builds: whether check is the armed step, which then fails once as if its system call had failed with EIO.
static bool injectsFault(CommitHooks& hooks, ASCIILiteral check)
{
#if ENABLE(JITCACHE_TWINS)
    if (hooks.fault != check)
        return false;
    hooks.fault = std::nullopt;
    return true;
#else
    UNUSED_PARAM(hooks);
    UNUSED_PARAM(check);
    return false;
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

static uint64_t freshNonzeroValue()
{
    uint64_t value = 0;
    while (!value)
        cryptographicallyRandomValues(asMutableByteSpan(value));
    return value;
}

static constexpr std::array<uint8_t, packAlignment> zeroPadding { };

#if OS(LINUX)

// Writes every byte at offset; a short write is retried from where it stopped. Returns 0 or the errno.
static int writeAll(int fd, std::span<const uint8_t> bytes, uint64_t offset)
{
    while (!bytes.empty()) {
        ssize_t count = ::pwrite(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!count)
            return EIO;
        bytes = bytes.subspan(static_cast<size_t>(count));
        offset += static_cast<uint64_t>(count);
    }
    return 0;
}

// Reads exactly buffer.size() bytes at offset. Returns 0, the errno, or EIO at an early end of file.
static int readAll(int fd, std::span<uint8_t> buffer, uint64_t offset)
{
    while (!buffer.empty()) {
        ssize_t count = ::pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (!count)
            return EIO;
        buffer = buffer.subspan(static_cast<size_t>(count));
        offset += static_cast<uint64_t>(count);
    }
    return 0;
}

// The pack as one commit writes it: every write can carry the writer.write fault, and the first write of the stream
// reaches the mid-stream kill point.
class PackFile {
    WTF_MAKE_NONCOPYABLE(PackFile);
public:
    PackFile(int fd, CommitHooks& hooks)
        : m_fd(fd)
        , m_hooks(hooks)
    {
    }

    int writeAt(std::span<const uint8_t> bytes, uint64_t offset)
    {
        if (injectsFault(m_hooks, WriterChecks::write))
            return EIO;
        if (int error = writeAll(m_fd, bytes, offset))
            return error;
        if (m_streaming && !std::exchange(m_wroteInStream, true))
            reach(m_hooks, KillPoint::MidStream);
        return 0;
    }

    int readAt(std::span<uint8_t> buffer, uint64_t offset)
    {
        if (injectsFault(m_hooks, WriterChecks::reread))
            return EIO;
        return readAll(m_fd, buffer, offset);
    }

    void startStreaming() { m_streaming = true; }
    // A stream that wrote nothing, which only empty sections give, leaves the same state as its end, where the
    // mid-stream point then falls.
    void didFinishStreaming()
    {
        if (m_streaming && !std::exchange(m_wroteInStream, true))
            reach(m_hooks, KillPoint::MidStream);
        m_streaming = false;
    }

private:
    const int m_fd;
    CommitHooks& m_hooks;
    bool m_streaming { false };
    bool m_wroteInStream { false };
};

// The record's body bytes from the first section on, through the staging buffer: append copies into the buffer and
// flushes it whenever it fills, and writeDirect flushes and then writes its bytes straight to the file.
class SectionStream {
    WTF_MAKE_NONCOPYABLE(SectionStream);
public:
    SectionStream(PackFile& file, std::span<uint8_t> staging, uint64_t offset)
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

    uint64_t offset() const { return m_bufferOffset + m_buffered; }
    int error() const { return m_error; }

private:
    PackFile& m_file;
    const std::span<uint8_t> m_staging;
    uint64_t m_bufferOffset;
    size_t m_buffered { 0 };
    int m_error { 0 };
};

static CommitFailure sectionFailure(SectionKind kind, uint64_t declared, uint64_t streamed, bool reportedFailure)
{
    ASCIILiteral sectionName = sectionKindDescription(kind).name;
    if (reportedFailure)
        return { WriterChecks::section, makeString(sectionName, ": the source declared "_s, declared, " bytes, streamed "_s, streamed, " and failed"_s) };
    return { WriterChecks::section, makeString(sectionName, ": the source declared "_s, declared, " bytes and streamed "_s, streamed) };
}

// Streams the sources in order to their offsets from bodyOffset, with zeros between sections and after the last one up
// to recordEnd, and sets each entry's CRC from the bytes as they pass. Given a timing, each source's stream time runs from
// the end of the previous one's to the end of its own bytes, its padding and the flushes its bytes filled included.
static std::optional<CommitFailure> streamSections(PackFile& file, std::span<uint8_t> staging, std::span<const SectionSource> sources,
    std::span<BodyDirectoryEntry> entries, uint64_t bodyOffset, uint64_t recordEnd, CommitTiming* timing)
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
    SectionStream stream(file, staging, bodyOffset + entries[0].offset);
    auto writeFailure = [&] {
        return CommitFailure { WriterChecks::write, errorText(stream.error()) };
    };

    for (size_t index = 0; index < sources.size(); ++index) {
        const SectionSource& source = sources[index];
        BodyDirectoryEntry& entry = entries[index];
        uint64_t sectionOffset = bodyOffset + entry.offset;
        ASSERT(sectionOffset >= stream.offset() && sectionOffset - stream.offset() < zeroPadding.size());
        if (!stream.append(std::span { zeroPadding }.first(static_cast<size_t>(sectionOffset - stream.offset()))))
            return writeFailure();

        uint32_t crcState = ~0u;
        if (!source.stream) {
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

        uint64_t offered = 0;
        bool passedSize = false;
        auto sinkFunction = [&](std::span<const uint8_t> bytes) -> bool {
            if (stream.error() || passedSize)
                return false;
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
        if (passedSize || offered != source.size || !succeeded)
            return sectionFailure(source.kind, source.size, offered, !succeeded && !passedSize && offered == source.size);
        entry.crc = ~crcState;
        timeUntilNow(source.kind);
    }

    // The record's padding, so the pack holds zeros between a record and the block after it.
    ASSERT(recordEnd >= stream.offset() && recordEnd - stream.offset() < zeroPadding.size());
    if (!stream.append(std::span { zeroPadding }.first(static_cast<size_t>(recordEnd - stream.offset()))) || !stream.flush())
        return writeFailure();
    timeUntilNow(sources.back().kind);
    return std::nullopt;
}

// Rereads the body from the pack in chunks of the staging buffer into a Full-mode validation stream, whatever strict says,
// and requires exactly the planned layout.
static std::optional<CommitFailure> rereadBody(PackFile& file, std::span<uint8_t> staging, const BodyKey& key,
    std::span<const uint8_t, 16> headerDigest, uint64_t bodyOffset, uint64_t bodySize, const BodyLayout& plannedLayout)
{
    BodyValidationStream validation(key, headerDigest, ValidationMode::Full, bodySize);
    uint64_t done = 0;
    while (done < bodySize) {
        size_t count = static_cast<size_t>(std::min<uint64_t>(staging.size(), bodySize - done));
        if (int error = file.readAt(staging.first(count), bodyOffset + done))
            return CommitFailure { WriterChecks::reread, errorText(error) };
        validation.append(staging.first(count));
        done += count;
    }
    auto layout = validation.finish();
    if (!layout)
        return CommitFailure { WriterChecks::reread, makeString("the record's body fails "_s, layout.error()) };
    if (*layout != plannedLayout)
        return CommitFailure { WriterChecks::reread, "the record's envelope or directory differs from the planned one"_s };
    return std::nullopt;
}

#endif // OS(LINUX)

#if ENABLE(JITCACHE_TWINS)
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

static bool keyPrecedes(const PackIndexEntry& a, const PackIndexEntry& b)
{
    return std::ranges::lexicographical_compare(a.key.bytes(), b.key.bytes());
}

} // namespace ArtifactWriterInternal

// The sources.

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

// The writer.

ArtifactWriter::ArtifactWriter(OpenedArtifact& artifact, ProducerLock& producerLock, ProducerBudget& budget, size_t stagingBytes)
    : m_artifact(artifact)
    , m_producerLock(producerLock)
    , m_budget(budget)
    , m_stagingBytes(stagingBytes)
{
    RELEASE_ASSERT(m_stagingBytes);
    UNUSED_VARIABLE(m_producerLock);
}

ArtifactWriter::~ArtifactWriter()
{
    releaseStagingBuffer();
    closePack();
}

void ArtifactWriter::closePack()
{
#if OS(LINUX)
    if (!m_writablePrefix.empty())
        munmap(m_writablePrefix.data(), m_writablePrefix.size());
    m_writablePrefix = { };
    if (m_packFd >= 0)
        ::close(std::exchange(m_packFd, -1));
#endif
}

void ArtifactWriter::releaseStagingBuffer()
{
    if (!m_staging)
        return;
    m_staging = MallocSpan<uint8_t> { };
    m_budget.release(m_stagingBytes);
}

bool ArtifactWriter::ensureStagingBuffer()
{
    if (m_staging)
        return true;
    if (!m_budget.tryCharge(m_stagingBytes))
        return false;
    m_staging = MallocSpan<uint8_t>::malloc(m_stagingBytes);
    return true;
}

int ArtifactWriter::ensurePackOpen()
{
#if !OS(LINUX)
    return ENOSYS;
#else
    using namespace ArtifactWriterInternal;
    {
        Locker locker { m_artifact.m_indexLock };
        m_artifact.follow();
        if (m_packFd >= 0 && m_artifact.m_pack && m_artifact.m_pack->inode() == m_packInode)
            return 0;
    }
    closePack();
    int fd;
    do {
        fd = ::openat(m_artifact.cacheFd(), ArtifactNames::pack.characters(), O_RDWR | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return errno;
    struct stat status;
    if (fstat(fd, &status)) {
        int error = errno;
        ::close(fd);
        return error;
    }
    uint64_t inode = static_cast<uint64_t>(status.st_ino);
    bool retired = false;
    {
        Locker locker { m_artifact.m_indexLock };
        if (!m_artifact.m_pack || m_artifact.m_pack->inode() != inode) {
            // Only this producer replaces the pack while it holds the lock, so the object maps an older file: map this one.
            if (auto readFailure = m_artifact.reopen(0)) {
                ::close(fd);
                return readFailure->error ? readFailure->error : EIO;
            }
        }
        retired = m_artifact.m_pack->isRetired();
    }
    if (retired) {
        // The path still names the retired file, so a compaction died between its mark and its rename: the file is the
        // current pack, and the mark goes before anything is appended.
        uint64_t zero = 0;
        if (int error = writeAll(fd, asByteSpan(zero), packRetiredOffset)) {
            ::close(fd);
            return error;
        }
    }
    // A pwrite need not store the root as one 8-byte word, so the root goes through a mapping, where an aligned store
    // does. The mapping starts at offset 0, so the root's offset keeps it aligned.
    void* prefix = mmap(nullptr, packPrefixBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (prefix == MAP_FAILED) {
        int error = errno;
        ::close(fd);
        return error;
    }
    m_writablePrefix = unsafeMakeSpan(static_cast<uint8_t*>(prefix), packPrefixBytes);
    m_packFd = fd;
    m_packInode = inode;
    return 0;
#endif
}

std::expected<CommitResult, CommitFailure> ArtifactWriter::commit(const CommitStamp& stamp, const CommitSections& sections, CommitTiming* timing)
{
    if (timing)
        *timing = { };
#if !OS(LINUX)
    UNUSED_PARAM(stamp);
    UNUSED_PARAM(sections);
    return std::unexpected(CommitFailure { WriterChecks::openPack, String::fromUTF8(safeStrerror(ENOSYS).data()) });
#else
    using namespace ArtifactWriterInternal;

    CommitHooks hooks;
#if ENABLE(JITCACHE_TWINS)
    ++m_commits;
    if (m_faultForTesting && m_faultForTesting->n == m_commits && !isCompactionCheck(m_faultForTesting->check))
        hooks.fault = m_faultForTesting->check;
    if (m_killForTesting && m_killForTesting->n == m_commits && !isCompactionPoint(m_killForTesting->point))
        hooks.killPoint = m_killForTesting->point;
    m_commitFault = hooks.fault;
    auto clearCommitFault = makeScopeExit([&] {
        m_commitFault = std::nullopt;
    });
#endif

    if (!ensureStagingBuffer())
        return failure(WriterChecks::budgetLimit, makeString("the writer's staging buffer of "_s, m_stagingBytes, " bytes"_s));
    std::span<uint8_t> staging = m_staging.mutableSpan();

    // The layout's arithmetic is overflow-checked, and every offset must fit the off_t the system calls take.
    auto sources = sections.sources();
    std::array<BodyDirectoryEntry, numberOfSectionKinds> entryStorage { };
    auto entries = std::span { entryStorage }.first(sources.size());
    for (size_t index = 0; index < sources.size(); ++index) {
        entries[index].kind = sources[index].kind;
        entries[index].size = sources[index].size;
    }
    auto bodySize = layOutBodySections(entries);
    auto extent = bodySize ? packRecordExtent(*bodySize) : std::nullopt;
    if (!extent)
        return failure(WriterChecks::section, makeString("the sizes of the "_s, sources.size(), " sections give no body layout"_s));

    if (injectsFault(hooks, WriterChecks::openPack))
        return failure(WriterChecks::openPack, errorText(EIO));
    if (int error = ensurePackOpen())
        return failure(WriterChecks::openPack, errorText(error));
    uint64_t recordOffset;
    {
        Locker locker { m_artifact.m_indexLock };
        recordOffset = m_artifact.m_validEnd;
    }
    uint64_t limit = static_cast<uint64_t>(std::numeric_limits<off_t>::max()) / 2;
    if (*extent > limit || recordOffset > limit - *extent)
        return failure(WriterChecks::write, makeString("a record of "_s, *extent, " bytes at "_s, recordOffset, " passes the largest file"_s));
    reach(hooks, KillPoint::BeforeAppend);

    // The body's bytes past the record header, then its directory and envelope, then the record header: a record that
    // stops short of its envelope never validates, and none is published before the root moves anyway.
    PackFile file(m_packFd, hooks);
    uint64_t bodyOffset = recordOffset + packRecordHeaderBytes;
    file.startStreaming();
    if (auto streamFailure = streamSections(file, staging, sources, entries, bodyOffset, recordOffset + *extent, timing))
        return std::unexpected(WTF::move(*streamFailure));
    file.didFinishStreaming();
    reach(hooks, KillPoint::AfterStream);

    std::array<uint8_t, numberOfSectionKinds * bodyDirectoryEntryBytes> directoryStorage { };
    auto directory = std::span { directoryStorage }.first(entries.size() * bodyDirectoryEntryBytes);
    BodyEnvelope envelope;
    envelope.sectionCount = static_cast<uint16_t>(entries.size());
    envelope.highestTier = stamp.highestTier;
    envelope.key = stamp.key;
    envelope.version = freshNonzeroValue();
    memcpySpan(std::span { envelope.headerDigest }, m_artifact.headerDigest());
    envelope.fileSize = *bodySize;
    envelope.llintThreshold = stamp.llintThreshold;
    envelope.counterProgress = stamp.counterProgress;
    envelope.directoryCRC = encodeBodyDirectory(entries, directory);
    auto envelopeBytes = encodeBodyEnvelope(envelope);
    auto recordHeader = encodePackRecordHeader({ stamp.key, *bodySize, envelope.version });
    if (int error = file.writeAt(directory, bodyOffset + bodyEnvelopeBytes))
        return failure(WriterChecks::write, errorText(error));
    if (int error = file.writeAt(envelopeBytes, bodyOffset))
        return failure(WriterChecks::write, errorText(error));
    if (int error = file.writeAt(recordHeader, recordOffset))
        return failure(WriterChecks::write, errorText(error));
    reach(hooks, KillPoint::AfterEnvelope);

    uint64_t rereadStart = timing ? benchThreadCPUNanoseconds() : 0;
    BodyLayout plannedLayout { envelope.version, envelope.highestTier, envelope.llintThreshold, envelope.counterProgress, { } };
    for (auto& entry : entries)
        plannedLayout.sections[static_cast<size_t>(entry.kind)] = SectionExtent { entry.offset, entry.size };
    if (auto rereadFailure = rereadBody(file, staging, stamp.key, m_artifact.headerDigest(), bodyOffset, *bodySize, plannedLayout))
        return std::unexpected(WTF::move(*rereadFailure));
    if (timing)
        timing->rereadNanoseconds = benchThreadCPUNanoseconds() - rereadStart;
    reach(hooks, KillPoint::AfterReread);

    uint64_t publishStart = timing ? benchThreadCPUNanoseconds() : 0;
    PackIndexEntry indexEntry { stamp.key, recordOffset, *bodySize, envelope.version, stamp.llintThreshold, stamp.counterProgress, stamp.highestTier };
#if ENABLE(JITCACHE_TWINS)
    m_commitFault = hooks.fault;
#endif
    if (auto publishFailure = publish(indexEntry, recordOffset + *extent))
        return std::unexpected(WTF::move(*publishFailure));
    if (timing)
        timing->publishNanoseconds = benchThreadCPUNanoseconds() - publishStart;
    return CommitResult { envelope.version, *bodySize };
#endif
}

std::optional<CommitFailure> ArtifactWriter::publish(const PackIndexEntry& entry, uint64_t end)
{
#if !OS(LINUX)
    UNUSED_PARAM(entry);
    UNUSED_PARAM(end);
    return CommitFailure { WriterChecks::publish, String::fromUTF8(safeStrerror(ENOSYS).data()) };
#else
    using namespace ArtifactWriterInternal;
    CommitHooks hooks;
#if ENABLE(JITCACHE_TWINS)
    hooks.fault = m_commitFault;
    if (m_killForTesting && m_killForTesting->n == m_commits && !isCompactionPoint(m_killForTesting->point))
        hooks.killPoint = m_killForTesting->point;
#endif
    PackFile file(m_packFd, hooks);

    uint64_t latestBlock;
    Vector<PackIndexEntry> fullIndex;
    size_t fullIndexCharge = 0;
    auto releaseFullIndexCharge = makeScopeExit([&] {
        if (fullIndexCharge)
            m_budget.release(fullIndexCharge);
    });
    {
        Locker locker { m_artifact.m_indexLock };
        latestBlock = m_artifact.m_latestBlock;
        // A full index once the chain is long, so a reader that opens the pack walks at most that many deltas.
        if (!entry.isTombstone() && m_artifact.m_deltasSinceFull + 1 >= packDeltaChainBound) {
            size_t count = m_artifact.m_index.size() + 1;
            size_t charge = count * (sizeof(PackIndexEntry) + packIndexEntryBytes);
            if (m_budget.tryCharge(charge)) {
                fullIndexCharge = charge;
                fullIndex.reserveInitialCapacity(count);
                for (auto& listed : m_artifact.m_index.values()) {
                    if (listed.key != entry.key)
                        fullIndex.append(listed);
                }
                fullIndex.append(entry);
            }
        }
    }

    std::ranges::sort(fullIndex, keyPrecedes);
    bool isFull = !fullIndex.isEmpty();
    std::span<const PackIndexEntry> listed = isFull ? fullIndex.span() : singleElementSpan(entry);
    PackIndexBlock block { isFull ? PackIndexKind::Full : PackIndexKind::Delta, isFull ? 0 : latestBlock, static_cast<uint32_t>(listed.size()) };
    Vector<uint8_t> blockBytes;
    blockBytes.grow(packIndexBlockBytes(block.count));
    for (size_t ordinal = 0; ordinal < listed.size(); ++ordinal)
        encodePackIndexEntry(listed[ordinal], blockBytes.mutableSpan().subspan(packIndexBlockHeaderBytes + ordinal * packIndexEntryBytes, packIndexEntryBytes));
    sealPackIndexBlock(block, end, blockBytes.mutableSpan());
    if (int error = file.writeAt(blockBytes.span(), end))
        return CommitFailure { WriterChecks::write, errorText(error) };
    reach(hooks, KillPoint::AfterIndex);

    // The commit point. Every byte below the new valid end is in the file before the store, and the release order makes
    // a reader that loads the root with acquire see them.
    uint64_t validEnd = end + blockBytes.size();
    __atomic_store_n(reinterpret_cast<uint64_t*>(m_writablePrefix.subspan(packRootOffset, sizeof(uint64_t)).data()), validEnd, __ATOMIC_RELEASE);
    reach(hooks, KillPoint::AfterRoot);

    Locker locker { m_artifact.m_indexLock };
    if (auto damage = m_artifact.follow())
        return CommitFailure { WriterChecks::publish, makeString("the stored root fails "_s, damage->check, ": "_s, damage->packRejection) };
    if (m_artifact.m_validEnd != validEnd)
        return CommitFailure { WriterChecks::publish, "the opened artifact does not follow the stored root"_s };
    return std::nullopt;
#endif
}

std::optional<CommitFailure> ArtifactWriter::remove(const BodyKey& key)
{
    using namespace ArtifactWriterInternal;
    if (int error = ensurePackOpen())
        return CommitFailure { WriterChecks::openPack, errorText(error) };
    uint64_t end;
    {
        Locker locker { m_artifact.m_indexLock };
        if (!m_artifact.m_index.contains(key))
            return std::nullopt;
        end = m_artifact.m_validEnd;
    }
    PackIndexEntry tombstone;
    tombstone.key = key;
    return publish(tombstone, end);
}

std::expected<std::optional<CompactionResult>, CompactionFailure> ArtifactWriter::compactIfNeeded(bool timed)
{
    using namespace ArtifactWriterInternal;
    using Kind = CompactionFailure::Kind;
    RefPtr<MappedPack> pack;
    uint64_t validEnd;
    Vector<PackIndexEntry> entries;
    size_t charge = 0;
    {
        Locker locker { m_artifact.m_indexLock };
        if (auto damage = m_artifact.follow())
            return std::unexpected(CompactionFailure { Kind::Damaged, damage->check, String { damage->packRejection } });
        validEnd = m_artifact.m_validEnd;
        // A compacted pack holds its live records and one full index. Any other byte (a superseded version, a tombstone, an
        // index block appended since the last compaction) is dead, so a delta that committed anything compacts and one
        // that committed nothing leaves a dense pack alone.
        uint64_t denseBytes = m_artifact.m_liveBytes + packIndexBlockBytes(m_artifact.m_index.size());
        if (validEnd <= denseBytes)
            return std::optional<CompactionResult> { };
        charge = m_artifact.m_index.size() * sizeof(PackIndexEntry);
        if (!m_budget.tryCharge(charge))
            return std::unexpected(CompactionFailure { Kind::Budget, WriterChecks::budgetLimit, makeString("the compaction's plan of "_s, m_artifact.m_index.size(), " bodies"_s) });
        entries.reserveInitialCapacity(m_artifact.m_index.size());
        for (auto& entry : m_artifact.m_index.values())
            entries.append(entry);
        pack = m_artifact.m_pack;
    }
    auto releaseCharge = makeScopeExit([&] {
        m_budget.release(charge);
    });
    std::ranges::sort(entries, keyPrecedes);

    CompactionTarget target { m_artifact.cacheFd(), m_artifact.headerDigest() };
    target.budget = &m_budget;
#if ENABLE(JITCACHE_TWINS)
    ++m_compactions;
    if (m_faultForTesting && m_faultForTesting->n == m_compactions && isCompactionCheck(m_faultForTesting->check))
        target.faultForTesting = m_faultForTesting->check;
    if (m_killForTesting && m_killForTesting->n == m_compactions) {
        target.killsBeforeRenameForTesting = m_killForTesting->point == KillPoint::CompactionBeforeRename;
        target.killsAfterRenameForTesting = m_killForTesting->point == KillPoint::CompactionAfterRename;
    }
#endif
    if (int error = ensurePackOpen())
        return std::unexpected(CompactionFailure { Kind::Storage, CompactionChecks::create, makeString("opening cache/pack for writing: "_s, errorText(error)) });
    CompactionSource source { *pack, validEnd, entries.span(), m_packFd };
    auto result = compactPack(target, source, [](const BodyKey&) { return false; }, timed);
    pack = nullptr;

    // The writer's descriptor and mapping name the retired file now; the next commit opens the new one.
    closePack();
    {
        Locker locker { m_artifact.m_indexLock };
        m_artifact.m_nextRetiredReopen = MonotonicTime { };
        m_artifact.follow();
    }
    if (!result)
        return std::unexpected(WTF::move(result.error()));
    return std::optional<CompactionResult> { WTF::move(*result) };
}

std::expected<CompactionResult, CompactionFailure> ArtifactWriter::compactPack(const CompactionTarget& target, const CompactionSource& source,
    const ScopedLambda<bool(const BodyKey&)>& excludes, bool timed)
{
    using Kind = CompactionFailure::Kind;
#if !OS(LINUX)
    UNUSED_PARAM(target);
    UNUSED_PARAM(source);
    UNUSED_PARAM(excludes);
    UNUSED_PARAM(timed);
    return std::unexpected(CompactionFailure { Kind::Storage, CompactionChecks::create, String::fromUTF8(safeStrerror(ENOSYS).data()) });
#else
    using namespace ArtifactWriterInternal;
    uint64_t start = timed ? benchThreadCPUNanoseconds() : 0;
    std::optional<ASCIILiteral> fault;
#if ENABLE(JITCACHE_TWINS)
    fault = target.faultForTesting;
#endif
    auto storage = [&](ASCIILiteral check, String&& detail) {
        return std::unexpected(CompactionFailure { Kind::Storage, check, WTF::move(detail) });
    };
    auto injects = [&](ASCIILiteral check) {
        if (fault != check)
            return false;
        fault = std::nullopt;
        return true;
    };

    TemporaryFileName temporary = temporaryFileName(TemporaryKind::Pack);
    if (injects(CompactionChecks::create))
        return storage(CompactionChecks::create, errorText(EIO));
    int fd;
    do {
        fd = ::openat(target.cacheFd, temporary.data(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return storage(CompactionChecks::create, errorText(errno));
    bool renamed = false;
    auto cleanUp = makeScopeExit([&] {
        ::close(fd);
        if (!renamed)
            ::unlinkat(target.cacheFd, temporary.data(), 0);
    });
    auto write = [&](std::span<const uint8_t> bytes, uint64_t offset) {
        if (injects(CompactionChecks::write))
            return EIO;
        return writeAll(fd, bytes, offset);
    };

    // The new index's buffer is production memory of a producing VM.
    size_t indexCharge = source.entries.size() * sizeof(PackIndexEntry) + packIndexBlockBytes(static_cast<uint32_t>(source.entries.size()));
    if (target.budget && !target.budget->tryCharge(indexCharge))
        return std::unexpected(CompactionFailure { Kind::Budget, WriterChecks::budgetLimit, makeString("the compaction's index of "_s, source.entries.size(), " bodies"_s) });
    auto releaseIndexCharge = makeScopeExit([&] {
        if (target.budget)
            target.budget->release(indexCharge);
    });

    CompactionResult result;
    result.replacedPackBytes = source.validEnd;
    Vector<PackIndexEntry> written;
    written.reserveInitialCapacity(source.entries.size());
    uint64_t offset = packPrefixBytes;
    for (auto& entry : source.entries) {
        if (excludes(entry.key))
            continue;
        // Each body passes the full checks before it moves, so the new pack holds only what a strict reader accepts.
        auto header = decodePackRecordHeader(source.pack.bytes(entry.offset, packRecordHeaderBytes).first<packRecordHeaderBytes>());
        ASCIILiteral check = ContainerChecks::pack;
        bool sound = header && header->key == entry.key && header->size == entry.size && header->version == entry.version;
        if (sound) {
            auto layout = validateBody(source.pack.bytes(entry.offset + packRecordHeaderBytes, entry.size), entry.key, target.headerDigest, ValidationMode::Full);
            if (!layout) {
                sound = false;
                check = layout.error();
            } else
                sound = packEntryRepeatsEnvelope(entry, *layout);
        }
        if (!sound) {
            if (target.dropsDamagedBodies) {
                result.droppedBodies.append({ entry.key, check });
                continue;
            }
            return std::unexpected(CompactionFailure { Kind::Damaged, check, makeString("body "_s, String { std::span<const char> { bodyKeyHexDigits(entry.key) } }, " in cache/pack"_s) });
        }
        uint64_t recordBytes = packRecordHeaderBytes + entry.size;
        uint64_t extent = *packRecordExtent(entry.size);
        if (int error = write(source.pack.bytes(entry.offset, recordBytes), offset))
            return storage(CompactionChecks::write, errorText(error));
        if (extent > recordBytes) {
            if (int error = write(std::span { zeroPadding }.first(static_cast<size_t>(extent - recordBytes)), offset + recordBytes))
                return storage(CompactionChecks::write, errorText(error));
        }
        PackIndexEntry moved = entry;
        moved.offset = offset;
        written.append(moved);
        offset += extent;
    }

    PackIndexBlock block { PackIndexKind::Full, 0, static_cast<uint32_t>(written.size()) };
    Vector<uint8_t> blockBytes;
    blockBytes.grow(packIndexBlockBytes(block.count));
    for (size_t ordinal = 0; ordinal < written.size(); ++ordinal)
        encodePackIndexEntry(written[ordinal], blockBytes.mutableSpan().subspan(packIndexBlockHeaderBytes + ordinal * packIndexEntryBytes, packIndexEntryBytes));
    sealPackIndexBlock(block, offset, blockBytes.mutableSpan());
    if (int error = write(blockBytes.span(), offset))
        return storage(CompactionChecks::write, errorText(error));
    // No reader sees the temporary before the rename, so its root goes in with the prefix, by pwrite.
    uint64_t validEnd = offset + blockBytes.size();
    PackPrefix prefix;
    memcpySpan(std::span { prefix.headerDigest }, target.headerDigest);
    prefix.identifier = freshNonzeroValue();
    auto prefixBytes = encodePackPrefix(prefix, validEnd);
    if (int error = write(prefixBytes, 0))
        return storage(CompactionChecks::write, errorText(error));

    // The reread: the prefix and the index a reader of the new pack starts from.
    {
        Vector<uint8_t> reread;
        reread.grow(std::max<size_t>(blockBytes.size(), packPrefixBytes));
        int error = injects(CompactionChecks::reread) ? EIO : readAll(fd, reread.mutableSpan().first(packPrefixBytes), 0);
        if (error)
            return storage(CompactionChecks::reread, errorText(error));
        if (!equalSpans(reread.span().first(packPrefixBytes), std::span<const uint8_t> { prefixBytes }))
            return storage(CompactionChecks::reread, "the new pack's prefix reads back differently"_s);
        if (int error = readAll(fd, reread.mutableSpan().first(blockBytes.size()), offset))
            return storage(CompactionChecks::reread, errorText(error));
        if (!equalSpans(reread.span().first(blockBytes.size()), blockBytes.span()))
            return storage(CompactionChecks::reread, "the new pack's index reads back differently"_s);
    }

    // The old pack is retired before the rename, so a reader that meets the mark finds the path renamed or about to be.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint64_t retired = 1;
    if (injects(CompactionChecks::publish))
        return storage(CompactionChecks::publish, errorText(EIO));
    if (int error = writeAll(source.writableFd, asByteSpan(retired), packRetiredOffset))
        return storage(CompactionChecks::publish, makeString("marking the old pack retired: "_s, errorText(error)));
#if ENABLE(JITCACHE_TWINS)
    if (target.killsBeforeRenameForTesting) {
        raise(SIGKILL);
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif
    if (::renameat(target.cacheFd, temporary.data(), target.cacheFd, ArtifactNames::pack.characters()))
        return storage(CompactionChecks::publish, errorText(errno));
    renamed = true;
#if ENABLE(JITCACHE_TWINS)
    if (target.killsAfterRenameForTesting) {
        raise(SIGKILL);
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif

    result.bodies = written.size();
    result.packBytes = validEnd;
    if (timed)
        result.nanoseconds = benchThreadCPUNanoseconds() - start;
    return result;
#endif
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
    for (ASCIILiteral check : ArtifactWriterInternal::injectableCompactionChecks) {
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

    BodyOpen opened = m_artifact.open(key, ValidationMode::Full);
    if (opened.outcome != StoreOutcome::Found)
        return failure(WriterChecks::rewrite, describeUnopenedBody(opened));
    RefPtr<ValidatedBody> body = WTF::move(opened.body);

    CommitStamp stamp { body->key(), body->llintThreshold(), body->counterProgress(), body->highestTier() };
    uint8_t highestTier = stamp.highestTier;
    ASCIILiteral sectionName = sectionKindDescription(kind).name;
    if (!isSectionRequired(kind, highestTier))
        return failure(WriterChecks::rewrite, makeString("the body holds no "_s, sectionName, " section"_s));
    uint64_t sectionSize = body->section(kind).size();
    if (offset > sectionSize || bytes.size() > sectionSize - offset)
        return failure(WriterChecks::rewrite, makeString(bytes.size(), " bytes at offset "_s, offset, " lie outside the "_s, sectionSize, " bytes of "_s, sectionName));

    size_t copyBytes = 0;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto sectionKind = static_cast<SectionKind>(index);
        if (isSectionRequired(sectionKind, highestTier))
            copyBytes += body->section(sectionKind).size();
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
        auto destination = copy.mutableSpan().subspan(copied, section.size());
        if (!section.empty())
            memcpySpan(destination, section);
        if (sectionKind == kind && !bytes.empty())
            memcpySpan(destination.subspan(static_cast<size_t>(offset), bytes.size()), bytes);
        sources[count++] = SectionSource::inMemory(sectionKind, destination);
        copied += section.size();
    }
    body = nullptr;
    return commit(stamp, CommitSections { std::span { sources }.first(count) });
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC::JITCache
