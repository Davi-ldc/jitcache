/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BaselineJITPlan.h"
#include "LOLJIT.h"

#include "JITCacheBench.h"
#include "JITCacheFaults.h"
#include "JITCacheGlue.h"
#include "JITCacheVMState.h"
#include "JITSafepoint.h"
#include "JITThunks.h"
#include "ProducerBudget.h"
#include "VM.h"

#if ENABLE(JIT)

namespace JSC {

BaselineJITPlan::BaselineJITPlan(CodeBlock* codeBlock)
    : JITPlan(JITCompilationMode::Baseline, codeBlock)
{
    VM& vm = codeBlock->vm();
    JIT::doMainThreadPreparationBeforeCompile(vm);

    // JITCache: the compilation records its image when the VM's production is active and the parent-key registry holds
    // a key for the UCB (SPEC-integrator.md section 4.4, SPEC-image.md R-INT-12). Both callers, jitCompileAndSetHeuristics
    // and JIT::compileSync, construct the plan on the VM thread, and the lookup takes only the registry's leaf lock.
    if (JITCache::producerContext(vm))
        m_jitCacheRecordsImage = vm.jitCacheState()->registry().keyOf(*codeBlock->unlinkedCodeBlock()).has_value();

    // JITCache: the plan measures its native cost, and passes it to the finalize capture hook, exactly when the VM has an
    // open bench report now (harness sub-SPEC section 9.3).
    m_jitCacheMeasure = !!JITCache::benchReport(vm);
}

auto BaselineJITPlan::compileInThreadImpl(JITCompilationEffort effort) -> CompilationPath
{
    m_codeBlock->updateAllNonLazyValueProfilePredictions();
    m_codeBlock->updateAllLazyValueProfilePredictions();

    // JITCache: the native cost counts the compilation after its profile drain, and notes per-VM support generated
    // meanwhile, on this thread or another (harness sub-SPEC section 9.3).
    uint64_t jitCacheSupportGenerations = 0;
    uint64_t jitCacheCompileStart = 0;
    if (m_jitCacheMeasure) {
        jitCacheSupportGenerations = m_vm->jitStubs->supportGenerations();
        jitCacheCompileStart = JITCache::benchThreadCPUNanoseconds();
    }

    // BaselineJITPlan can keep underlying CodeBlock alive while running.
    // So we do not need to suspend this compilation thread while running GC.
    Safepoint::Result result;
    {
        Safepoint safepoint(*this, result);
        safepoint.begin(false);

        if (Options::useLOLJIT()) {
            LOL::LOLJIT jit(*m_vm, *this, m_codeBlock);
            auto jitCode = jit.compileAndLinkWithoutFinalizing(effort);
            m_jitCode = WTF::move(jitCode);
        } else {
            JIT jit(*m_vm, *this, m_codeBlock);
            auto jitCode = jit.compileAndLinkWithoutFinalizing(effort);
            m_jitCode = WTF::move(jitCode);
        }
    }
    // JITCache: a cancelled plan has dropped its VM (JITPlan::cancel) and is never finalized, so the measured span closes
    // only after the cancellation test.
    if (result.didGetCancelled())
        return CancelPath;
    if (m_jitCacheMeasure) {
        m_jitCacheCompileNanoseconds = JITCache::benchThreadCPUNanoseconds() - jitCacheCompileStart;
        m_jitCacheSupportGenerated = m_vm->jitStubs->supportGenerations() != jitCacheSupportGenerations;
    }
    return BaselinePath;
}

auto BaselineJITPlan::compileInThreadImpl() -> CompilationPath
{
    return compileInThreadImpl(JITCompilationCanFail);
}

auto BaselineJITPlan::compileSync(JITCompilationEffort effort) -> CompilationPath
{
    return compileInThreadImpl(effort);
}

size_t BaselineJITPlan::codeSize() const
{
    if (m_jitCode)
        return m_jitCode->size();
    return 0;
}

bool BaselineJITPlan::isKnownToBeLiveAfterGC()
{
    // If stage is not JITPlanStage::Canceled, we should keep this alive and mark underlying CodeBlock anyway.
    // Regardless of whether the owner ScriptExecutable / CodeBlock dies, compiled code would be still usable
    // since Baseline JIT is *unlinked*. So, let's not stop compilation.
    return m_stage != JITPlanStage::Canceled;
}

bool BaselineJITPlan::isKnownToBeLiveDuringGC(AbstractSlotVisitor&)
{
    // Ditto to isKnownToBeLiveAfterGC. Unless plan gets completely cancelled before running, we should keep compilation running.
    return m_stage != JITPlanStage::Canceled;
}

CompilationResult BaselineJITPlan::finalize()
{
    // JITCache: when measuring, the finalization's own span runs from here to the finalize capture hook, without the
    // relinks of incoming calls that installCode's relink timer adds to the report meanwhile (harness sub-SPEC section
    // 9.3). What relinks outside this span left in the accumulator is discarded first.
    JITCache::BenchReport* jitCacheReport = m_jitCacheMeasure ? JITCache::benchReport(*m_vm) : nullptr;
    ASSERT(!m_jitCacheMeasure || jitCacheReport); // the report lives as long as the state, which outlives every plan
    uint64_t jitCacheSupportGenerations = 0;
    uint64_t jitCacheFinalizeStart = 0;
    if (jitCacheReport) {
        jitCacheReport->takeRelinkNanoseconds();
        jitCacheSupportGenerations = m_vm->jitStubs->supportGenerations();
        jitCacheFinalizeStart = JITCache::benchThreadCPUNanoseconds();
    }

    CompilationResult result = JIT::finalizeOnMainThread(m_codeBlock, *this, m_jitCode);
    switch (result) {
    case CompilationResult::CompilationFailed:
        // JITCache: a baseline plan fails only for lack of executable memory (JIT::link returns null only when its
        // LinkBuffer failed to allocate), so the fault comes before the effects below (SPEC-integrator.md section 9).
        JITCache::didFailExecutableAllocation(*m_vm, JITCache::ExecutableAllocationSite::BaselinePlan);
        CODEBLOCK_LOG_EVENT(m_codeBlock, "delayJITCompile", ("compilation failed"));
        dataLogLnIf(Options::verboseOSR(), "    JIT compilation failed.");
        m_codeBlock->dontJITAnytimeSoon();
        m_codeBlock->m_didFailJITCompilation = true;
        break;
    case CompilationResult::CompilationSuccessful: {
        WTF::crossModifyingCodeFence();
        dataLogLnIf(Options::verboseOSR(), "    JIT compilation successful.");
        m_codeBlock->ownerExecutable()->installCode(m_codeBlock);
        m_codeBlock->jitSoon();
        JITCache::BaselineCompileTiming jitCacheTiming { };
        if (jitCacheReport) {
            uint64_t finalizeNanoseconds = JITCache::benchThreadCPUNanoseconds() - jitCacheFinalizeStart;
            // installCode's relink ran inside the span, timed by the same thread's clock.
            uint64_t relinkNanoseconds = jitCacheReport->takeRelinkNanoseconds();
            ASSERT(relinkNanoseconds <= finalizeNanoseconds);
            bool supportGenerated = m_jitCacheSupportGenerated || m_vm->jitStubs->supportGenerations() != jitCacheSupportGenerations;
            jitCacheTiming = { m_jitCacheCompileNanoseconds, finalizeNanoseconds - relinkNanoseconds, supportGenerated };
        }
        // JITCache: the finalize capture (SPEC-integrator.md section 8.5), now that the CB is its executable's replacement.
        JITCache::didFinalizeBaselineCompilation(*m_vm, *m_codeBlock, jitCacheReport ? &jitCacheTiming : nullptr);
        break;
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }

    return result;
}

} // namespace JSC

#endif // ENABLE(JIT)
