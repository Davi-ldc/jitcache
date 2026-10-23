#include "config.h"
#include "jitcache/store/JITCacheFiles.h"

#include "jitcache/store/ArtifactStore.h"
#include <wtf/SafeStrerror.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/unix/UnixFileDescriptor.h>
#endif

namespace JSC::JITCache {

String errorText(int error)
{
    return String::fromUTF8(safeStrerror(error).span());
}

#if OS(LINUX)

int openAt(int directoryFd, const char* name, int flags, mode_t mode)
{
    int fd;
    do {
        fd = ::openat(directoryFd, name, flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int readExactly(int fd, std::span<uint8_t> bytes, uint64_t offset)
{
    while (!bytes.empty()) {
        ssize_t count = ::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(offset));
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

int writeExactly(int fd, std::span<const uint8_t> bytes, uint64_t offset)
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

std::expected<HeaderFile, HeaderFileFailure> readHeaderFile(int cacheFd)
{
    int opened = openAt(cacheFd, ArtifactNames::header.characters(), O_RDONLY | O_NONBLOCK);
    if (opened < 0)
        return std::unexpected(HeaderFileFailure { HeaderFileFailure::Step::Open, errno });
    UnixFileDescriptor fd { opened, UnixFileDescriptor::Adopt };

    HeaderFile file;
    struct stat status;
    if (fstat(fd.value(), &status))
        return std::unexpected(HeaderFileFailure { HeaderFileFailure::Step::Read, errno });
    if (!S_ISREG(status.st_mode))
        return file;
    while (file.size < file.bytes.size()) {
        auto rest = std::span { file.bytes }.subspan(file.size);
        ssize_t count = ::read(fd.value(), rest.data(), rest.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(HeaderFileFailure { HeaderFileFailure::Step::Read, errno });
        }
        if (!count)
            break;
        file.size += static_cast<size_t>(count);
    }
    return file;
}

#endif // OS(LINUX)

} // namespace JSC::JITCache
