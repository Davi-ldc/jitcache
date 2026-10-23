#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/ModuleSources.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/NotFound.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// The module's layers: every source file and header of the module, tests aside, may include only the module headers the
// layer table allows, named by their directory.

namespace JSC::JITCache::Tests {

namespace LayerTestsInternal {

// The module's directories by layer, bottom up. A file may include a header of its own directory, of a lower layer, or
// of another directory of its own layer, except that the lanes never include one another. A directory the table does not
// list fails the test until someone places it.
enum class Layer : uint8_t { Base, Store, Session, Lanes, Glue, Hosts };

static constexpr std::array<std::pair<ASCIILiteral, Layer>, 12> moduleLayers { {
    { "base"_s, Layer::Base },
    { "twins"_s, Layer::Base },
    { "store"_s, Layer::Store },
    { "session"_s, Layer::Session },
    { "cb"_s, Layer::Lanes },
    { "ics"_s, Layer::Lanes },
    { "image"_s, Layer::Lanes },
    { "ucb"_s, Layer::Lanes },
    { "glue"_s, Layer::Glue },
    { "maintenance"_s, Layer::Glue },
    { "harness"_s, Layer::Hosts },
    { "shell"_s, Layer::Hosts },
} };

// The includes that break the rule on purpose, each with its reason. An exception that no longer applies fails the test,
// so the table holds only edges the module still has.
struct LayerException {
    ASCIILiteral file; // from jitcache/
    ASCIILiteral header; // from jitcache/
    ASCIILiteral reason;
};

static constexpr std::array layerExceptions {
    LayerException { "session/JITCacheVMState.cpp"_s, "ucb/UCBRegistry.h"_s, "the VM's state constructs the UCB lane's registry"_s },
};

static std::optional<Layer> layerOf(StringView directory)
{
    for (auto& [name, layer] : moduleLayers) {
        if (directory == name)
            return layer;
    }
    return std::nullopt;
}

// The module directory a path from jitcache/ sits in: "store" for "store/ArtifactStore.h", empty for a file at the top.
static StringView directoryOf(StringView path)
{
    size_t slash = path.find('/');
    return slash == notFound ? StringView { } : path.left(static_cast<unsigned>(slash));
}

// Every source file and header below directory, tests/ aside, as a path from jitcache/: "store/ArtifactStore.h".
static void addSourcesBelow(const String& directory, const String& prefix, Vector<String>& sources)
{
    for (auto& name : FileSystem::listDirectory(directory)) {
        String path = FileSystem::pathByAppendingComponent(directory, name);
        String fromModule = prefix.isEmpty() ? name : makeString(prefix, '/', name);
        if (FileSystem::fileType(path) == FileSystem::FileType::Directory) {
            if (fromModule != "tests"_s)
                addSourcesBelow(path, fromModule, sources);
        } else if (name.endsWith(".h"_s) || name.endsWith(".cpp"_s))
            sources.append(WTF::move(fromModule));
    }
}

} // namespace LayerTestsInternal

using namespace LayerTestsInternal;

// Each #include of a module header in a module file names the header as "jitcache/<directory>/<file>". An exported
// header names another as <JavaScriptCore/<file>> instead, the flat copy Bun compiles against, since Bun's copies
// resolve no jitcache/ path; no other file may use that spelling. Any other spelling of a module header's name fails,
// since a bare name still compiles from the header's own directory. The header must be a module file outside tests/,
// and the edge from the including file's directory to the header's must be one the layer table allows.
JITCACHE_TEST(integratorModuleIncludesKeepTheLayers, No)
{
    auto directories = moduleDirectories(context);
    if (!directories)
        return;

    Vector<String> sources;
    addSourcesBelow(directories->jitcache, { }, sources);
    if (sources.isEmpty()) {
        JITCACHE_FAIL(makeString(directories->jitcache, " lists no module sources"_s));
        return;
    }
    HashSet<String> moduleFiles;
    HashMap<String, String> headersByName; // no two module headers share a name; if two did, the first listed would win
    for (auto& source : sources) {
        moduleFiles.add(source);
        if (source.endsWith(".h"_s))
            headersByName.add(fileNameOfPath(source), source);
    }

    std::array<bool, layerExceptions.size()> exceptionApplies { };
    for (auto& source : sources) {
        StringView directory = directoryOf(source);
        auto layer = layerOf(directory);
        if (!layer) {
            JITCACHE_FAIL(makeString(source, " sits in a directory the layer table does not list"_s));
            continue;
        }
        auto text = readSourceText(FileSystem::pathByAppendingComponent(directories->jitcache, source));
        if (!text) {
            JITCACHE_FAIL(makeString(source, " cannot be read"_s));
            continue;
        }
        String sourceName = fileNameOfPath(source);
        bool exported = std::ranges::any_of(exportedJITCacheHeaders, [&](ASCIILiteral exportedName) { return sourceName == exportedName; });
        for (auto& path : includedPaths(*text)) {
            constexpr auto modulePrefix = "jitcache/"_s;
            String name = fileNameOfPath(path);
            String header; // from jitcache/, or null when the include names no module header
            if (path.startsWith(modulePrefix))
                header = path.substring(static_cast<unsigned>(modulePrefix.length()));
            else if (exported && headersByName.contains(name) && path == makeString("JavaScriptCore/"_s, name))
                header = headersByName.get(name);
            else if (headersByName.contains(name)) {
                String spelling = exported ? makeString("<JavaScriptCore/"_s, name, '>') : makeString('"', modulePrefix, headersByName.get(name), '"');
                JITCACHE_FAIL(makeString(source, " includes the module header "_s, name, " as "_s, path, " instead of "_s, spelling));
            }
            if (header.isNull())
                continue;
            if (!moduleFiles.contains(header) || !header.endsWith(".h"_s)) {
                JITCACHE_FAIL(makeString(source, " includes "_s, path, ", which names no module header outside tests/"_s));
                continue;
            }
            StringView headerDirectory = directoryOf(header);
            auto headerLayer = layerOf(headerDirectory);
            // A header in an unlisted directory has failed the test as a source already.
            if (headerDirectory == directory || !headerLayer)
                continue;
            if (*headerLayer < *layer || (*headerLayer == *layer && *layer != Layer::Lanes))
                continue;
            bool excepted = false;
            for (size_t index = 0; index < layerExceptions.size(); ++index) {
                if (source == layerExceptions[index].file && header == layerExceptions[index].header) {
                    exceptionApplies[index] = true;
                    excepted = true;
                }
            }
            if (!excepted)
                JITCACHE_FAIL(makeString(source, " includes "_s, header, ", but "_s, directory, " may include neither a higher layer nor another lane"_s));
        }
    }
    for (size_t index = 0; index < layerExceptions.size(); ++index) {
        if (!exceptionApplies[index]) {
            auto& exception = layerExceptions[index];
            JITCACHE_FAIL(makeString("the layer exception for "_s, exception.file, " including "_s, exception.header, " ("_s, exception.reason, ") matches no include of the module; update or remove it"_s));
        }
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
