/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

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
