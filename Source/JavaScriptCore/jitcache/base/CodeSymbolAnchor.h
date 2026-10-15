#pragma once

// The anchor of the engine's code: a function that does nothing and is never called, of which only the address matters.
// The Image lane measures every CodeSymbol from it, and the store finds the object that holds the engine by it, so that
// the artifact header's build ID covers the code those offsets point into. ImageSupport.cpp defines it. Every part of
// JITCache may include this header.

namespace JSC::JITCache {

#if ENABLE(JIT)
void codeSymbolAnchor();
#endif

} // namespace JSC::JITCache
