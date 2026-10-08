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

#include "config.h"
#include "JITCacheFaults.h"

#include "JITCacheVMState.h"
#include "VM.h"

namespace JSC::JITCache {

namespace JITCacheFaultsInternal {

// The step each site names (SPEC-integrator.md section 4.5).
static ASCIILiteral executableAllocationStep(ExecutableAllocationSite site)
{
    switch (site) {
    case ExecutableAllocationSite::BaselinePlan:
        return "exec-alloc.baseline-plan"_s;
    case ExecutableAllocationSite::DFGPlan:
        return "exec-alloc.dfg-plan"_s;
    case ExecutableAllocationSite::FTLPlan:
        return "exec-alloc.ftl-plan"_s;
    case ExecutableAllocationSite::InlineCacheHandler:
        return "exec-alloc.ic-handler"_s;
    case ExecutableAllocationSite::MathICSnippet:
        return "exec-alloc.mathic-snippet"_s;
    case ExecutableAllocationSite::JITCacheImage:
        return "exec-alloc.jitcache-image"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

} // namespace JITCacheFaultsInternal

void didFailExecutableAllocation(VM& vm, ExecutableAllocationSite site)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    ASSERT(vm.currentThreadIsHoldingAPILock());
    // The report holds two literals and a null detail, so recording it allocates nothing, and the call is safe with any
    // JSC lock held, CodeBlock::m_lock under a GCSafeConcurrentJSLocker included. Production memory is released at the
    // next glue entry, not here (section 4.2).
    state->turnActivityOff(FaultReport { FaultClass::ExecutableMemory, { }, JITCacheFaultsInternal::executableAllocationStep(site), { } });
}

void didAttachDebugger(VM& vm)
{
    if (VMState* state = vm.jitCacheState())
        state->noteDebuggerAttached();
}

} // namespace JSC::JITCache
