#include "config.h"
#include "ArtifactAccess.h"

#include "JITCacheContainer.h"
#include "JITCacheFiles.h"
#include <errno.h>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/posix/POSIXExtras.h>
#include <wtf/unix/UnixFileDescriptor.h>
#endif

// The system calls sit behind OS(LINUX), as the rest of the store's do. Elsewhere start rejects before it gets here, and
// openArtifact fails with ENOSYS.

namespace JSC::JITCache {

namespace ArtifactAccessInternal {

using Kind = ArtifactAccessFailure::Kind;

static std::unexpected<ArtifactAccessFailure> failure(Kind kind, String&& detail)
{
    return std::unexpected(ArtifactAccessFailure { kind, WTF::move(detail) });
}

#if OS(LINUX)

static int openDirectoryAt(int directoryFd, const char* name)
{
    return openAt(directoryFd, name, O_RDONLY | O_DIRECTORY);
}

static ArtifactAccessFailure ioFailure(const String& path, ASCIILiteral what, int error)
{
    return { Kind::IO, makeString(what, " in "_s, path, ": "_s, errorText(error)) };
}

// cache/'s descriptor and the header the VM checked or wrote there, whose bytes the opened artifact digests: those of the
// file, which may name an app the VM's config does not.
struct ReachedArtifact {
    UnixFileDescriptor cache;
    ArtifactHeaderBytes header;
};

// A header-less cache/ is reused only when its pack, if it has one, holds no body: its temporaries are removed, its pack
// is marked retired, and the caller writes a new pack and the header. Returns the failure that stops the creation, or
// nothing.
static std::optional<ArtifactAccessFailure> resetRemnant(int cacheFd, const String& path)
{
    if (packHoldsBodies(cacheFd))
        return ArtifactAccessFailure { Kind::NotAnArtifact, makeString(path, "/cache holds a pack with bodies but no header"_s) };

    // An object may still map the remnant's pack, such as the empty pack of a whole deletion that stopped midway; without
    // the mark it would keep reading that pack after the new one replaces it.
    if (int error = retirePack(cacheFd))
        return ioFailure(path, "cannot mark the pack in cache/ retired"_s, error);

    // Collected first and removed after the listing, so no entry goes while the directory is being read.
    Vector<TemporaryFileName> temporaries;
    int error = listDirectory(cacheFd, [&](std::span<const char> name, uint64_t) {
        if (!temporaryKindOfFileName(name) || name.size() > maximumTemporaryFileNameLength)
            return;
        TemporaryFileName temporary;
        memcpySpan(std::span { temporary.characters }, name);
        temporaries.append(temporary);
    });
    if (error)
        return ioFailure(path, "cannot list cache/"_s, error);
    for (auto& temporary : temporaries) {
        if (::unlinkat(cacheFd, temporary.data(), 0) && errno != ENOENT)
            return ioFailure(path, "cannot remove a temporary from cache/"_s, errno);
    }
    return std::nullopt;
}

// The header's bytes go to a header temporary, which a rename publishes as cache/header.
static std::optional<ArtifactAccessFailure> writeHeader(int cacheFd, const String& path, std::span<const uint8_t> header)
{
    TemporaryFileName temporary = temporaryFileName(TemporaryKind::Header);
    int fd = openAt(cacheFd, temporary.data(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return ioFailure(path, "cannot create a header temporary in cache/"_s, errno);
    UnixFileDescriptor file { fd, UnixFileDescriptor::Adopt };
    if (int error = writeExactly(file.value(), header, 0))
        return ioFailure(path, "cannot write the header temporary in cache/"_s, error);
    if (::close(file.release()))
        return ioFailure(path, "cannot close the header temporary in cache/"_s, errno);
    if (::renameat(cacheFd, temporary.data(), cacheFd, ArtifactNames::header.characters()))
        return ioFailure(path, "cannot publish cache/header"_s, errno);
    return std::nullopt;
}

// A Producer's creation, under the producer lock: cache/, or a remnant reset, then an empty pack, then the header, which
// names the config's app when the config names one, so a Consumer that finds the header finds the pack. A failure leaves
// whatever the step created, which the next Producer or compact finishes or removes. Returns cache/'s descriptor and the
// header it wrote.
static std::expected<ReachedArtifact, ArtifactAccessFailure> createArtifact(int parentFd, const String& path, std::optional<uint64_t> appDigest)
{
    UnixFileDescriptor cache { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()), UnixFileDescriptor::Adopt };
    if (!cache) {
        int error = errno;
        if (error != ENOENT)
            return std::unexpected(ioFailure(path, "cannot open cache/"_s, error));
        if (::mkdirat(parentFd, ArtifactNames::cacheDirectory.characters(), 0755))
            return std::unexpected(ioFailure(path, "cannot create cache/"_s, errno));
        cache = UnixFileDescriptor { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()), UnixFileDescriptor::Adopt };
        if (!cache)
            return std::unexpected(ioFailure(path, "cannot open cache/"_s, errno));
    } else {
        // An artifact exists exactly when cache/header does.
        struct stat headerStatus;
        if (!::fstatat(cache.value(), ArtifactNames::header.characters(), &headerStatus, AT_SYMLINK_NOFOLLOW))
            return failure(Kind::Exists, makeString(path, " already holds an artifact"_s));
        if (int error = errno; error != ENOENT)
            return std::unexpected(ioFailure(path, "cannot look for cache/header"_s, error));
        if (auto stopped = resetRemnant(cache.value(), path))
            return std::unexpected(WTF::move(*stopped));
    }
    ArtifactHeaderBytes header = headerNamingApp(appDigest);
    HeaderDigest digest = artifactHeaderDigest(header.span());
    if (int error = createEmptyPack(cache.value(), std::span<const uint8_t, 16> { digest }))
        return std::unexpected(ioFailure(path, "cannot create cache/pack"_s, error));
    if (auto stopped = writeHeader(cache.value(), path, header.span()))
        return std::unexpected(WTF::move(*stopped));
    return ReachedArtifact { WTF::move(cache), header };
}

// A Consumer's or ConsumerProducer's artifact: cache/ and a header compatible with the process's and not made for another
// app than the config's. Returns cache/'s descriptor and the header's bytes as the file holds them.
static std::expected<ReachedArtifact, ArtifactAccessFailure> openCompatibleArtifact(int parentFd, const String& path, std::optional<uint64_t> appDigest)
{
    UnixFileDescriptor cache { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()), UnixFileDescriptor::Adopt };
    if (!cache) {
        int error = errno;
        if (error == ENOENT)
            return failure(Kind::Missing, makeString(path, " holds no cache/ directory"_s));
        return std::unexpected(ioFailure(path, "cannot open cache/"_s, error));
    }
    auto header = readHeaderFile(cache.value());
    if (!header) {
        int error = header.error().error;
        if (header.error().step == HeaderFileFailure::Step::Read)
            return std::unexpected(ioFailure(path, "cannot read cache/header"_s, error));
        if (error == ENOENT)
            return failure(Kind::Missing, makeString(path, "/cache holds no header"_s));
        return std::unexpected(ioFailure(path, "cannot open cache/header"_s, error));
    }

    HeaderCheck check = checkArtifactHeader(header->span(), expectedHeader().span(), appDigest);
    switch (check.verdict) {
    case HeaderVerdict::Compatible: {
        // A compatible header is as long as the process's own, so its bytes fit.
        ReachedArtifact reached { WTF::move(cache), { } };
        memcpySpan(std::span { reached.header.bytes }, header->span());
        reached.header.size = static_cast<uint32_t>(header->size);
        return reached;
    }
    case HeaderVerdict::Incompatible:
        return failure(Kind::Incompatible, WTF::move(check.detail));
    case HeaderVerdict::AnotherApp:
        return failure(Kind::AnotherApp, WTF::move(check.detail));
    case HeaderVerdict::Corrupt:
        return failure(Kind::CorruptHeader, WTF::move(check.detail));
    }
    RELEASE_ASSERT_NOT_REACHED();
    return failure(Kind::CorruptHeader, { });
}

// A ConsumerProducer's replacement of an incompatible artifact, or of one made for another app, under the producer lock:
// cache/ moves aside to .cache.replaced, so a consumer that pinned it keeps a directory that only loses entries, a new
// artifact is created as a Producer creates one, for the config's app, and the old directory is then removed; what of it
// stays, compact removes. A .cache.replaced an earlier replacement left goes first, since the move needs its name. Returns
// the new cache/'s descriptor and the header it wrote.
static std::expected<ReachedArtifact, ArtifactAccessFailure> replaceArtifact(int parentFd, const String& path, std::optional<uint64_t> appDigest)
{
    uint64_t bytesRemoved = 0;
    if (int error = removeReplacedArtifact(parentFd, bytesRemoved))
        return std::unexpected(ioFailure(path, "cannot remove an earlier .cache.replaced"_s, error));
    if (::renameat(parentFd, ArtifactNames::cacheDirectory.characters(), parentFd, ArtifactNames::replacedCacheDirectory.characters()))
        return std::unexpected(ioFailure(path, "cannot move the old cache/ aside"_s, errno));
    auto reached = createArtifact(parentFd, path, appDigest);
    if (reached)
        removeReplacedArtifact(parentFd, bytesRemoved);
    return reached;
}

#endif // OS(LINUX)

} // namespace ArtifactAccessInternal

std::expected<ArtifactAccess, ArtifactAccessFailure> openArtifact(const String& path, ArtifactAccessMode mode, std::optional<uint64_t> appDigest)
{
    using namespace ArtifactAccessInternal;
#if !OS(LINUX)
    UNUSED_PARAM(path);
    UNUSED_PARAM(mode);
    UNUSED_PARAM(appDigest);
    return failure(Kind::IO, errorText(ENOSYS));
#else
    UTF8CString pathBytes = path.utf8();
    UnixFileDescriptor parent { openDirectoryAt(AT_FDCWD, pathBytes.legacyCStringPointer()), UnixFileDescriptor::Adopt };
    if (!parent) {
        int error = errno;
        if (error != ENOENT)
            return std::unexpected(ioFailure(path, "cannot open the artifact's directory"_s, error));
        if (mode != ArtifactAccessMode::Create)
            return failure(Kind::Missing, makeString(path, " does not exist"_s));
        // One level only: a missing grandparent is an I/O error.
        if (posixMkdir(pathBytes, 0755) && errno != EEXIST)
            return std::unexpected(ioFailure(path, "cannot create the artifact's directory"_s, errno));
        parent = UnixFileDescriptor { openDirectoryAt(AT_FDCWD, pathBytes.legacyCStringPointer()), UnixFileDescriptor::Adopt };
        if (!parent)
            return std::unexpected(ioFailure(path, "cannot open the artifact's directory"_s, errno));
    }

    // The lock is a local until success, so every failure after it was taken releases it.
    std::unique_ptr<ProducerLock> producerLock;
    if (mode != ArtifactAccessMode::Open) {
        auto acquired = ProducerLock::tryAcquire(parent.value());
        if (!acquired) {
            if (acquired.error().busy)
                return failure(Kind::Busy, makeString("another producer or maintenance holds the producer lock of "_s, path));
            return std::unexpected(ioFailure(path, "cannot take the producer lock"_s, acquired.error().error));
        }
        producerLock = WTF::move(*acquired);
    }

    auto reached = mode == ArtifactAccessMode::Create ? createArtifact(parent.value(), path, appDigest) : openCompatibleArtifact(parent.value(), path, appDigest);
    // A ConsumerProducer replaces an incompatible artifact, or one made for another app, under the producer lock it holds,
    // where a Consumer fails.
    String replaced;
    if (!reached && mode == ArtifactAccessMode::OpenOrReplace && (reached.error().kind == Kind::Incompatible || reached.error().kind == Kind::AnotherApp)) {
        replaced = WTF::move(reached.error().detail);
        reached = replaceArtifact(parent.value(), path, appDigest);
    }
    if (!reached)
        return std::unexpected(WTF::move(reached.error()));

    // The VMs of the process that open this directory share one object and its index, which digests the header this VM
    // checked or wrote. Every VM that opens the directory reads the same header, whatever app its config names.
    auto taken = ArtifactRegistry::take(parent.value(), reached->cache.value(), reached->header.span());
    if (!taken) {
        // A pack that fails its checks is the artifact's own damage, which start reports as it reports a corrupt header.
        if (!taken.error().error)
            return failure(Kind::DamagedPack, makeString("cache/pack fails container.pack: "_s, taken.error().packRejection));
        return std::unexpected(ioFailure(path, "cannot map cache/pack"_s, taken.error().error));
    }
    return ArtifactAccess { WTF::move(producerLock), WTF::move(*taken), WTF::move(replaced) };
#endif
}

} // namespace JSC::JITCache
