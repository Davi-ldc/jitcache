#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/tests/support/JITCacheTest.h"
#include <array>
#include <optional>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/NotFound.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// What the tests that read the module's own files need: where the build testjitcache runs from and the module's sources
// are, which headers the module exports, a file's text, and the path each of its #include directives names.

namespace JSC::JITCache::Tests {

// The module's exported headers, by the names their flat copies carry among the engine's private headers, the copies Bun
// compiles against. JITCache.cmake lists them.
inline constexpr std::array exportedJITCacheHeaders {
    "JITCacheAPI.h"_s,
    "JITCacheBun.h"_s,
    "JITCacheMaintenance.h"_s,
    "JITCacheTwinsHost.h"_s,
};

// The whole file as text, one character per byte, or nothing when it cannot be read.
inline std::optional<String> readSourceText(const String& path)
{
    auto bytes = FileSystem::readEntireFile(path);
    if (!bytes)
        return std::nullopt;
    return String(bytes->span());
}

// The path each #include directive names, as written between its quotes or angle brackets: "X.h" for "X.h",
// "jitcache/store/X.h" for "jitcache/store/X.h" and "JavaScriptCore/X.h" for <JavaScriptCore/X.h>. Every directive
// counts, whatever preprocessor condition it sits under, since another build's switches can turn it on.
inline Vector<String> includedPaths(StringView text)
{
    Vector<String> paths;
    for (StringView line : text.split('\n')) {
        unsigned index = 0;
        auto skipSpaces = [&] {
            while (index < line.length() && isASCIIWhitespace(line[index]))
                ++index;
        };
        skipSpaces();
        if (index >= line.length() || line[index] != '#')
            continue;
        ++index;
        skipSpaces();
        constexpr auto include = "include"_s;
        if (!line.substring(index).startsWith(include))
            continue;
        index += static_cast<unsigned>(include.length());
        skipSpaces();
        if (index >= line.length() || (line[index] != '"' && line[index] != '<'))
            continue;
        char16_t close = line[index] == '"' ? '"' : '>';
        unsigned start = index + 1;
        size_t end = line.find(close, start);
        if (end == notFound)
            continue;
        paths.append(line.substring(start, static_cast<unsigned>(end) - start).toString());
    }
    return paths;
}

// A path's last component: "X.h" for "X.h", "jitcache/store/X.h" and "JavaScriptCore/X.h".
inline String fileNameOfPath(StringView path)
{
    size_t slash = path.reverseFind('/');
    return (slash == notFound ? path : path.substring(static_cast<unsigned>(slash) + 1)).toString();
}

// The build testjitcache runs from, as <build>/bin, and the module's directory under the source root that build's CMake
// cache names, which Bun's recipe sets to this repository.
struct ModuleDirectories {
    String build;
    String jitcache;
};

// Nothing, after failing the test, when the CMake cache names no source root.
inline std::optional<ModuleDirectories> moduleDirectories(TestContext& context)
{
    String executable = FileSystem::realPath("/proc/self/exe"_s);
    String buildDirectory = FileSystem::parentPath(FileSystem::parentPath(executable));
    String sourceRoot;
    if (auto cache = readSourceText(FileSystem::pathByAppendingComponent(buildDirectory, "CMakeCache.txt"_s))) {
        constexpr auto prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="_s;
        for (StringView line : StringView(*cache).split('\n')) {
            if (line.startsWith(prefix)) {
                sourceRoot = line.substring(static_cast<unsigned>(prefix.length())).toString();
                break;
            }
        }
    }
    if (sourceRoot.isEmpty()) {
        JITCACHE_FAIL(makeString("no CMAKE_HOME_DIRECTORY in the CMake cache of "_s, buildDirectory, ", the build testjitcache ("_s, executable, ") runs from"_s));
        return std::nullopt;
    }
    std::array<StringView, 3> jitcacheComponents { "Source"_s, "JavaScriptCore"_s, "jitcache"_s };
    return ModuleDirectories { buildDirectory, FileSystem::pathByAppendingComponents(sourceRoot, jitcacheComponents) };
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
