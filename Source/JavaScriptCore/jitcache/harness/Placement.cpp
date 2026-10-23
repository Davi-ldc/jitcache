#include "config.h"
#include "jitcache/harness/JITCacheTwinsHarness.h"

#if ENABLE(JITCACHE_TWINS)

#include "CommonIdentifiers.h"
#include "JSCConfig.h"
#include "JSCInlines.h"
#include "VM.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/posix/POSIXExtras.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// Address-space placement, and writeTestFile for the harness's other files. Each process of a twins sequence records its
// executable pool, its structure reservation and three heap probes in a layout file, and the runner hands a later process
// the layout files of the processes before it. Before JSC::initialize that process maps PROT_NONE placeholders over every
// free gap of the recorded pool and structure ranges, so neither of its reservations can land there, and it unmaps them
// once both exist; the twin checks then see an imported image's targets at other addresses than the capturing process's.
// Everything here runs only in test processes, so a failure prints the path and the error to stderr and calls exit(1),
// and the runner fails the run on its exit code. The layout files write every value in lowercase hex without a prefix,
// so the fixed probes read "heap 1 2 3"; readLayoutRanges also accepts a 0x prefix.

// A kernel older than 4.17 ignores the flag and treats the address as a hint, which placePlaceholder detects; the value is
// the same on x86_64 and ARM64.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace JSC::JITCache {

namespace PlacementInternal {

[[noreturn]] static void exitAfterError(const String& path, int error)
{
    SAFE_FPRINTF(stderr, "JITCache: %s: %s\n", path.utf8(), safeStrerror(error));
    exit(1);
}

[[noreturn]] static void exitAfterMalformedLine(const String& path, std::span<const char> line)
{
    SAFE_FPRINTF(stderr, "JITCache: %s: %s in line \"%s\"\n", path.utf8(), safeStrerror(EINVAL), UTF8CString::fromUTF8(line));
    exit(1);
}

// The whole file, or the errno of the call that failed.
static std::expected<Vector<char>, int> readWholeFile(const UTF8CString& path)
{
    int fd = posixOpen(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(errno);
    Vector<char> contents;
    std::array<char, 4096> chunk;
    while (true) {
        ssize_t count = ::read(fd, chunk.data(), chunk.size());
        if (count < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            ::close(fd);
            return std::unexpected(error);
        }
        if (!count)
            break;
        contents.append(std::span<const char> { chunk }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return contents;
}

static Vector<std::span<const char>> splitText(std::span<const char> text, char separator)
{
    Vector<std::span<const char>> parts;
    size_t start = 0;
    for (size_t index = 0; index <= text.size(); ++index) {
        if (index < text.size() && text[index] != separator)
            continue;
        if (index > start)
            parts.append(text.subspan(start, index - start));
        start = index + 1;
    }
    return parts;
}

// A hexadecimal value as the layout lines and /proc/self/maps write it, with or without a 0x prefix.
static std::optional<uint64_t> parseHexValue(std::span<const char> token)
{
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        token = token.subspan(2);
    if (token.empty() || token.size() > 16)
        return std::nullopt;
    uint64_t value = 0;
    for (char character : token) {
        if (!isASCIIHexDigit(character))
            return std::nullopt;
        value = (value << 4) | toASCIIHexValue(character);
    }
    return value;
}

struct AddressRange {
    uintptr_t start;
    uintptr_t end;
};

// The pool and structures ranges of a layout file; heap lines are the runner's alone.
static Vector<AddressRange> readLayoutRanges(const String& path)
{
    auto contents = readWholeFile(path.utf8());
    if (!contents)
        exitAfterError(path, contents.error());
    Vector<AddressRange> ranges;
    for (auto line : splitText(contents->span(), '\n')) {
        auto tokens = splitText(line, ' ');
        if (tokens.isEmpty())
            continue;
        StringView name { byteCast<Latin1Character>(tokens[0]) };
        if (name == "heap"_s) {
            if (tokens.size() != 4)
                exitAfterMalformedLine(path, line);
            continue;
        }
        bool isPool = name == "pool"_s;
        if (tokens.size() != 3 || (!isPool && name != "structures"_s))
            exitAfterMalformedLine(path, line);
        auto start = parseHexValue(tokens[1]);
        auto second = parseHexValue(tokens[2]);
        if (!start || !second)
            exitAfterMalformedLine(path, line);
        // pool <start> <end>; structures <start> <size>.
        uint64_t end = isPool ? *second : *start + *second;
        if (end <= *start || end > std::numeric_limits<uintptr_t>::max())
            exitAfterMalformedLine(path, line);
        ranges.append({ static_cast<uintptr_t>(*start), static_cast<uintptr_t>(end) });
    }
    return ranges;
}

static constexpr ASCIILiteral mapsPath = "/proc/self/maps"_s;

// The process's mappings in address order, as /proc/self/maps lists them.
static Vector<AddressRange> readMappings()
{
    auto contents = readWholeFile(mapsPath);
    if (!contents)
        exitAfterError(String { mapsPath }, contents.error());
    Vector<AddressRange> mappings;
    for (auto line : splitText(contents->span(), '\n')) {
        auto fields = splitText(line, ' ');
        auto bounds = fields.isEmpty() ? Vector<std::span<const char>> { } : splitText(fields[0], '-');
        std::optional<uint64_t> start = bounds.size() == 2 ? parseHexValue(bounds[0]) : std::nullopt;
        std::optional<uint64_t> end = bounds.size() == 2 ? parseHexValue(bounds[1]) : std::nullopt;
        if (!start || !end)
            exitAfterMalformedLine(String { mapsPath }, line);
        mappings.append({ static_cast<uintptr_t>(*start), static_cast<uintptr_t>(*end) });
    }
    return mappings;
}

static Vector<AddressRange>& placeholders()
{
    static NeverDestroyed<Vector<AddressRange>> list;
    return list.get();
}

enum class GapPlacement : uint8_t { Placed, Taken };

// One PROT_NONE placeholder over a gap /proc/self/maps listed as free. Taken: something mapped into the gap after the
// listing, which only this process's own allocations can do before JSC::initialize.
static GapPlacement placePlaceholder(const String& path, AddressRange gap)
{
    size_t size = gap.end - gap.start;
    void* requested = reinterpret_cast<void*>(gap.start);
    void* placed = mmap(requested, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (placed == MAP_FAILED) {
        if (errno == EEXIST)
            return GapPlacement::Taken;
        exitAfterError(path, errno);
    }
    if (placed != requested) {
        munmap(placed, size);
        exitAfterError(path, EEXIST);
    }
    placeholders().append(gap);
    return GapPlacement::Placed;
}

// Blocks every gap of a recorded range, so neither reservation JSC::initialize makes can land in it.
static void placePlaceholders(const String& path, AddressRange range)
{
    uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    uintptr_t start = WTF::roundUpToMultipleOf(pageSize, range.start);
    uintptr_t end = range.end & ~(pageSize - 1);
    if (start >= end)
        return;

    // Each attempt lists the mappings again, so a gap an attempt filled is a mapping in the next one.
    static constexpr unsigned maximumAttempts = 8;
    for (unsigned attempt = 1;; ++attempt) {
        bool taken = false;
        uintptr_t cursor = start;
        for (auto& mapping : readMappings()) {
            if (mapping.end <= cursor)
                continue;
            if (mapping.start >= end)
                break;
            if (mapping.start > cursor && placePlaceholder(path, { cursor, mapping.start }) == GapPlacement::Taken) {
                taken = true;
                break;
            }
            cursor = std::max(cursor, mapping.end);
            if (cursor >= end)
                break;
        }
        if (!taken && cursor < end && placePlaceholder(path, { cursor, end }) == GapPlacement::Taken)
            taken = true;
        if (!taken)
            return;
        if (attempt == maximumAttempts)
            exitAfterError(path, EEXIST);
    }
}

} // namespace PlacementInternal

void writeTestFile(const String& path, const UTF8CString& bytes, TestFileWrite mode)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (mode == TestFileWrite::Append ? O_APPEND : O_TRUNC);
    int fd = posixOpen(path.utf8(), flags, 0644);
    if (fd < 0)
        PlacementInternal::exitAfterError(path, errno);
    auto remaining = bytes.span();
    while (!remaining.empty()) {
        ssize_t written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            PlacementInternal::exitAfterError(path, error);
        }
        remaining = remaining.subspan(static_cast<size_t>(written));
    }
    if (::close(fd) < 0)
        PlacementInternal::exitAfterError(path, errno);
}

void placeholdersBeforeInitialize(const Vector<String>& layoutPaths)
{
    using namespace PlacementInternal;
    for (auto& path : layoutPaths) {
        for (auto range : readLayoutRanges(path))
            placePlaceholders(path, range);
    }
}

void releasePlaceholdersAfterInitialize()
{
    using namespace PlacementInternal;
    for (auto placeholder : placeholders()) {
        if (munmap(reinterpret_cast<void*>(placeholder.start), placeholder.end - placeholder.start) < 0)
            exitAfterError(String { mapsPath }, errno);
    }
    placeholders().clear();
}

void recordLayout(const String& path)
{
    uintptr_t poolStart = reinterpret_cast<uintptr_t>(g_jscConfig.startExecutableMemory);
    uintptr_t poolEnd = reinterpret_cast<uintptr_t>(g_jscConfig.endExecutableMemory);
    String lines = makeString(
        "pool "_s, hex(poolStart, Lowercase), ' ', hex(poolEnd, Lowercase), '\n',
        "structures "_s, hex(g_jscConfig.startOfStructureHeap, Lowercase), ' ', hex(g_jscConfig.sizeOfStructureHeap, Lowercase), '\n');
    writeTestFile(path, lines.utf8(), TestFileWrite::Replace);
}

void recordVMLayout(VM& vm, const String& path, HeapProbes probes)
{
    // Three kinds of heap target an image names: the VM's fields, its cells and the atoms. The fixed probes repeat the
    // same values in every process, as a heap that never moved would, so the runner's self-tests can check that the
    // runner refuses such a heap.
    uintptr_t vmProbe = 1;
    uintptr_t cellProbe = 2;
    uintptr_t atomProbe = 3;
    if (probes == HeapProbes::Live) {
        vmProbe = reinterpret_cast<uintptr_t>(&vm);
        cellProbe = reinterpret_cast<uintptr_t>(jsEmptyString(vm));
        atomProbe = reinterpret_cast<uintptr_t>(vm.propertyNames->length.impl());
    }
    String line = makeString("heap "_s, hex(vmProbe, Lowercase), ' ', hex(cellProbe, Lowercase), ' ', hex(atomProbe, Lowercase), '\n');
    writeTestFile(path, line.utf8(), TestFileWrite::Append);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
