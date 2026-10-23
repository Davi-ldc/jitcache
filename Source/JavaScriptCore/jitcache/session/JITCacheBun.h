#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <array>
#include <optional>
#include <stdint.h>
#include <wtf/Vector.h>
#include <wtf/text/StringView.h>

// The entry points only Bun's integration calls, so that JITCacheAPI.h stays the interface of any host that embeds
// JavaScriptCore. Exported to Bun as <JavaScriptCore/JITCacheBun.h>: it includes only WTF headers and JSExportMacros.h, and
// no member exists only under ENABLE(JITCACHE_TWINS), so every translation unit sees one layout. ucb/UCBAnalysis.cpp
// defines importModuleAnalysis and ucb/UCBKeys.cpp providerSourceDigest.

namespace JSC {

class JSGlobalObject;

} // namespace JSC

namespace JSC::JITCache {

struct ModuleAnalysisImport {
    std::array<uint8_t, 32> sourceDigest; // the digest the module's key was built from; the provider supplies it afterwards
    Vector<uint8_t> moduleInfo; // the body's analysis in Bun's self-contained module-info format; empty when none
};

// For Bun's provider of an ES module whose analysis Bun would otherwise parse from the text. The bytes it returns are in
// Bun's module-info format, which only Bun's loader reads. VM thread, API lock held; source is the provider's whole text,
// and suppliedDigest the source digest the provider carries, or null when it carries none. Empty when the VM does not
// import or a fault stopped the import; Bun then creates the provider as it would without JITCache.
JS_EXPORT_PRIVATE std::optional<ModuleAnalysisImport> importModuleAnalysis(JSGlobalObject&, StringView source,
    const std::array<uint8_t, 32>* suppliedDigest);

// The source digest JITCache keys a root body's text with, which a provider returns from jitCacheSourceDigest(): SHA-256
// of one byte, 1 when every code unit is at most 0xFF and 2 otherwise, then the code units, one Latin-1 byte each after a
// 1 and two UTF-16LE bytes each after a 2. Bun computes it where it produces a text, in `bun build --compile` and in the
// runtime transpiler cache, so that text's provider supplies it and JITCache keys the text without reading it. Pure: any
// thread, no lock.
JS_EXPORT_PRIVATE std::array<uint8_t, 32> providerSourceDigest(StringView source);

} // namespace JSC::JITCache
