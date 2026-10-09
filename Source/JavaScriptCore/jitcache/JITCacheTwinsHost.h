#pragma once

// What a host calls in twins builds (SPEC-integrator.md section 3.5), exported to Bun as
// <JavaScriptCore/JITCacheTwinsHost.h> and included by the jsc shell, so both hosts call the same declarations. Every
// declaration is under ENABLE(JITCACHE_TWINS), so a build without twins sees none, and every signature uses only WTF and
// JSC types. JITCacheTwinsHarness.cpp defines them all.

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include <JavaScriptCore/JSCJSValue.h>
#include <JavaScriptCore/JSExportMacros.h>
#include <wtf/Forward.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// Address-space placement (harness sub-SPEC section 4). These run only in test processes, so a failure prints the path
// and strerror(errno) to stderr and calls exit(1).
enum class HeapProbes : bool { Live, Fixed };
// Before JSC::initialize: PROT_NONE placeholders over every free gap of the pool and structures ranges the layout files
// record, so neither reservation can land in them.
JS_EXPORT_PRIVATE void placeholdersBeforeInitialize(const Vector<String>& layoutPaths);
// Right after JSC::initialize, once both reservations exist.
JS_EXPORT_PRIVATE void releasePlaceholdersAfterInitialize();
// After JSC::initialize: writes "pool <start> <end>" and "structures <start> <size>", in hex.
JS_EXPORT_PRIVATE void recordLayout(const String& path);
// Once the host's main VM exists: appends "heap <vm> <cell> <atom>", or "heap 1 2 3" with HeapProbes::Fixed.
JS_EXPORT_PRIVATE void recordVMLayout(VM&, const String& path, HeapProbes = HeapProbes::Live);

// The host functions bun:jsc registers under the names the jsc shell gives them (harness sub-SPEC section 5.3):
// jitcacheStatus, jitcacheProgress, jitcacheUCBStatistics, jitcacheDescribeHeap and jitcacheBodyEvents.
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheStatus);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheProgress);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheUCBStatistics);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheDescribeHeap);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheBodyEvents);

// The body-event dump at the end of a run (harness sub-SPEC section 10.3); it needs no JITCache configuration.
JS_EXPORT_PRIVATE void writeBodyEvents(VM&, const String& path);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
