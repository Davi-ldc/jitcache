#pragma once

#include "JITCacheContainer.h"
#include <array>
#include <expected>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <wtf/Platform.h>
#include <wtf/text/WTFString.h>

#if OS(LINUX)
#include <sys/types.h>
#endif

// The file helpers the store, start and maintenance share: openat with close-on-exec, the loops that read and write a
// whole span at an offset, an errno's text, and the one read of cache/header. Each is a plain system call, or a loop of
// them, on the calling thread; none takes a lock. Private to the store, start and maintenance: no header includes it.

namespace JSC::JITCache {

// The text strerror gives for an errno.
String errorText(int error);

#if OS(LINUX)

// openat with O_CLOEXEC added to flags, so every descriptor JITCache opens carries it, retried while it fails with
// EINTR. Returns the descriptor, or -1 with errno set.
int openAt(int directoryFd, const char* name, int flags, mode_t = 0);

// Reads exactly bytes.size() bytes at offset with pread. Returns 0, the errno of a failed read, or EIO at an early end of
// file.
int readExactly(int fd, std::span<uint8_t> bytes, uint64_t offset);

// Writes exactly bytes.size() bytes at offset with pwrite, retrying a short write from where it stopped. Returns 0, the
// errno of a failed write, or EIO for a write that wrote nothing.
int writeExactly(int fd, std::span<const uint8_t> bytes, uint64_t offset);

// The bytes of cache/header, read whole up to one byte more than the largest header file a reader accepts, so a larger
// file reads as too large and fails the header's checks. Anything at the name but a regular file, such as a FIFO or a
// directory, reads as empty and fails those checks too. The name is opened with O_NONBLOCK, so a FIFO there cannot block
// the caller while it waits for a writer.
struct HeaderFile {
    std::array<uint8_t, maximumArtifactHeaderFileBytes + 1> bytes { };
    size_t size { 0 };
    std::span<const uint8_t> span() const LIFETIME_BOUND { return std::span { bytes }.first(size); }
};
struct HeaderFileFailure {
    enum class Step : uint8_t { Open, Read };
    Step step;
    int error; // ENOENT at Open: the directory holds no header
};
std::expected<HeaderFile, HeaderFileFailure> readHeaderFile(int cacheFd);

#endif // OS(LINUX)

} // namespace JSC::JITCache
