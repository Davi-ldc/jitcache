#include "config.h"
#include "jitcache/base/EngineObject.h"

#include <mutex>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>

#if OS(LINUX)
#include <link.h>
#endif

// Besides the declarations of its header, the walk over the loaded objects, on Linux.

namespace JSC::JITCache {

namespace EngineObjectInternal {

// What loadedObjects returns, with the records its two object pointers point to.
struct LoadedObjectsStorage {
    LoadedObjects objects;
#if OS(LINUX)
    dl_phdr_info mainExecutable { };
    dl_phdr_info engine { };
#endif
};

#if OS(LINUX)

// The load bias and the program headers, which stay valid while the object stays loaded. Nothing reads the rest of the
// record.
static dl_phdr_info recordOf(const dl_phdr_info& object)
{
    dl_phdr_info record { };
    record.dlpi_addr = object.dlpi_addr;
    record.dlpi_phdr = object.dlpi_phdr;
    record.dlpi_phnum = object.dlpi_phnum;
    return record;
}

using ProgramHeader = ElfW(Phdr);

static LoadedSegment segmentOf(const dl_phdr_info& object, const ProgramHeader& header)
{
    uintptr_t start = static_cast<uintptr_t>(object.dlpi_addr + header.p_vaddr);
    return LoadedSegment { start, start + static_cast<uintptr_t>(header.p_memsz) };
}

static bool loadedSegmentsContain(const dl_phdr_info& object, uintptr_t address)
{
    for (auto& header : unsafeMakeSpan(object.dlpi_phdr, object.dlpi_phnum)) {
        if (header.p_type == PT_LOAD && segmentOf(object, header).contains(address))
            return true;
    }
    return false;
}

#if ENABLE(JITCACHE_TWINS)
static void appendLoadedSegments(const dl_phdr_info& object, Vector<LoadedSegment>& segments)
{
    for (auto& header : unsafeMakeSpan(object.dlpi_phdr, object.dlpi_phnum)) {
        if (header.p_type == PT_LOAD)
            segments.append(segmentOf(object, header));
    }
}
#endif

// dl_iterate_phdr lists the main executable first. Outside twins builds the walk stops at the engine, and its callback
// allocates nothing; twins builds walk every object, for the ones loaded at their link-time address.
static void readLoadedObjects(LoadedObjectsStorage& storage)
{
    struct Walk {
        uintptr_t anchor;
        LoadedObjectsStorage& storage;
        bool sawMainExecutable;
    } walk { reinterpret_cast<uintptr_t>(&codeSymbolAnchor), storage, false };
    dl_iterate_phdr([](struct dl_phdr_info* object, size_t, void* data) -> int {
        auto& walk = *static_cast<Walk*>(data);
        LoadedObjects& objects = walk.storage.objects;
        bool isMainExecutable = !walk.sawMainExecutable;
        walk.sawMainExecutable = true;
        if (isMainExecutable) {
            walk.storage.mainExecutable = recordOf(*object);
            objects.mainExecutable = &walk.storage.mainExecutable;
        }
        if (!objects.engine && loadedSegmentsContain(*object, walk.anchor)) {
            if (isMainExecutable)
                objects.engine = objects.mainExecutable;
            else {
                walk.storage.engine = recordOf(*object);
                objects.engine = &walk.storage.engine;
            }
        }
#if ENABLE(JITCACHE_TWINS)
        if (!object->dlpi_addr)
            appendLoadedSegments(*object, objects.atLinkTimeAddress);
        return 0;
#else
        return !!objects.engine;
#endif
    }, &walk);

    // The engine's text, and in twins builds every segment of it, from the record the walk kept.
    LoadedObjects& objects = storage.objects;
    if (!objects.engine)
        return;
    const dl_phdr_info& engine = *objects.engine;
    for (auto& header : unsafeMakeSpan(engine.dlpi_phdr, engine.dlpi_phnum)) {
        if (header.p_type != PT_LOAD)
            continue;
        LoadedSegment segment = segmentOf(engine, header);
        if ((header.p_flags & PF_X) && segment.contains(walk.anchor))
            objects.engineText = segment;
#if ENABLE(JITCACHE_TWINS)
        objects.engineSegments.append(segment);
#endif
    }
}

#endif // OS(LINUX)

} // namespace EngineObjectInternal

NEVER_INLINE void codeSymbolAnchor()
{
    // Never called: only its address matters.
}

const LoadedObjects& loadedObjects()
{
    static LazyNeverDestroyed<EngineObjectInternal::LoadedObjectsStorage> storage;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        storage.construct();
#if OS(LINUX)
        EngineObjectInternal::readLoadedObjects(storage.get());
#endif
    });
    return storage->objects;
}

} // namespace JSC::JITCache
