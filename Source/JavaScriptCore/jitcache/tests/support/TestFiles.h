#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/tests/support/JITCacheTest.h"
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <span>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Vector.h>
#include <wtf/posix/POSIXExtras.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// What the tests of every part need around files: a temporary directory that goes with the test, the plain reads and
// writes a test lays files out with or reads a report back with, and the text of an errno for a failure message.

namespace JSC::JITCache::Tests {

// strerror's text for the error, read through safeStrerror, which keeps no buffer shared between threads.
inline String errnoText(int error)
{
    return String::fromUTF8(safeStrerror(error).span());
}

// A fresh directory that mkdtemp makes as <name>-XXXXXX under $TMPDIR, or under /tmp when TMPDIR is unset or empty, and
// that goes with everything in it when the object goes.
class TemporaryDirectory {
    WTF_MAKE_NONCOPYABLE(TemporaryDirectory);
public:
    static std::unique_ptr<TemporaryDirectory> create(TestContext& context, ASCIILiteral name)
    {
        const char* base = getenv("TMPDIR");
        UTF8CString pattern = makeString(String::fromUTF8(base && *base ? base : "/tmp"), '/', name, "-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        return std::unique_ptr<TemporaryDirectory>(new TemporaryDirectory(String::fromUTF8(path.span().data())));
    }

    ~TemporaryDirectory()
    {
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    // A path inside the directory.
    String path(StringView relative) const
    {
        return makeString(m_path, '/', relative);
    }

private:
    explicit TemporaryDirectory(String&& path)
        : m_path(WTF::move(path))
    {
    }

    const String m_path;
};

// Whether anything has the name, a dangling symbolic link included.
inline bool pathExists(const String& path)
{
    struct stat fileStatus;
    return !posixLstat(path.utf8(), &fileStatus);
}

inline bool makeDirectory(TestContext& context, const String& path)
{
    if (!posixMkdir(path.utf8(), 0755))
        return true;
    JITCACHE_FAIL(makeString("cannot create "_s, path, ": "_s, errnoText(errno)));
    return false;
}

// Creates or truncates the file and writes the bytes, retrying an interrupted write.
inline bool writeFile(TestContext& context, const String& path, std::span<const uint8_t> bytes)
{
    int fd = posixOpen(path.utf8(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot create "_s, path, ": "_s, errnoText(errno)));
        return false;
    }
    while (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            JITCACHE_FAIL(makeString("cannot write "_s, path, ": "_s, errnoText(written ? errno : EIO)));
            ::close(fd);
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    ::close(fd);
    return true;
}

// Replaces whatever has the name with a FIFO, on which an open without O_NONBLOCK waits until a writer opens it.
inline bool replaceWithFIFO(TestContext& context, const String& path)
{
    UTF8CString bytes = path.utf8();
    // WTF has no wrapper for mkfifo, so it takes the path through legacyCStringPointer().
    if ((posixUnlink(bytes) && errno != ENOENT) || ::mkfifo(bytes.legacyCStringPointer(), 0644)) {
        JITCACHE_FAIL(makeString("cannot make a FIFO at "_s, path, ": "_s, errnoText(errno)));
        return false;
    }
    return true;
}

// The whole file, or nothing when it cannot be read.
inline std::optional<Vector<uint8_t>> readFileBytes(const String& path)
{
    int fd = posixOpen(path.utf8(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    Vector<uint8_t> bytes;
    std::array<uint8_t, 4096> buffer;
    while (true) {
        ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ::close(fd);
            return std::nullopt;
        }
        if (!count)
            break;
        bytes.append(std::span { buffer }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return bytes;
}

// The whole file as UTF-8 text, or a null String when it cannot be read.
inline String readTextFile(const String& path)
{
    auto bytes = readFileBytes(path);
    return bytes ? String::fromUTF8(bytes->span()) : String();
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
