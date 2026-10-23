#pragma once

#include <stdint.h>
#include <wtf/Platform.h>
#include <wtf/Vector.h>

// The object that holds the engine: the loaded object whose segments hold codeSymbolAnchor. The artifact header's build
// ID must cover it, and the Image lane measures every CodeSymbol within its text. One dl_iterate_phdr walk at the first
// call finds it with the main executable and, in twins builds, every object loaded at its link-time address. Only Linux
// lists loaded objects; elsewhere both objects are null and every segment is empty. Every part of JITCache may include
// this header.

// The loader's record of one loaded object. <link.h> defines it at global scope, so it is declared there, outside the
// namespace below.
struct dl_phdr_info;

namespace JSC::JITCache {

// The anchor of the engine's code: a function that does nothing and is never called, of which only the address matters.
void codeSymbolAnchor();

// The addresses of one loaded segment, from start up to end.
struct LoadedSegment {
    uintptr_t start { 0 };
    uintptr_t end { 0 };

    bool contains(uintptr_t address) const { return address >= start && address < end; }
};

struct LoadedObjects {
    // Neither object is ever unloaded, so the program headers each record points to stay mapped.
    const dl_phdr_info* mainExecutable { nullptr }; // the first object the walk lists
    const dl_phdr_info* engine { nullptr }; // mainExecutable itself when it holds the anchor; null when no object does
    LoadedSegment engineText; // the engine's executable PT_LOAD segment that holds the anchor
#if ENABLE(JITCACHE_TWINS)
    Vector<LoadedSegment> engineSegments; // every PT_LOAD segment of the engine
    Vector<LoadedSegment> atLinkTimeAddress; // the PT_LOAD segments of every object whose load bias is zero
#endif
};

// Any thread. Read once per process, at the first call.
const LoadedObjects& loadedObjects();

} // namespace JSC::JITCache
