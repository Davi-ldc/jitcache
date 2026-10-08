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

#include <stdint.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// The fault entry points native code and the lanes call (SPEC-integrator.md section 4.5). Each caller passes its site,
// which names the fault's step: exec-alloc.baseline-plan, exec-alloc.dfg-plan, exec-alloc.ftl-plan,
// exec-alloc.ic-handler, exec-alloc.mathic-snippet or exec-alloc.jitcache-image. The list of callers is exhaustive
// (THREAD Failures): the integrator's plan sites, the three didFailToAllocate() branches of InlineCacheCompiler, MathIC
// regeneration in every tier and the image's own allocation at install. Every call precedes the effects of the failure
// it reports.
enum class ExecutableAllocationSite : uint8_t {
    BaselinePlan, DFGPlan, FTLPlan, InlineCacheHandler, MathICSnippet, JITCacheImage,
};

// VM thread. Callable with any JSC lock held, CodeBlock::m_lock included. Allocates nothing, frees nothing, takes no
// lock and waits for no thread. A no-op for a VM start never configured.
void didFailExecutableAllocation(VM&, ExecutableAllocationSite);

// Any thread. Atomic stores only.
void didAttachDebugger(VM&);

} // namespace JSC::JITCache
