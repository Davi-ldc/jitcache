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

#include "JITSafepoint.h"
#include "jitcache/glue/JITCacheGlue.h"
#include "jitcache/session/JITCacheBench.h"
#include "jitcache/session/JITCacheFaults.h"

#if ENABLE(JIT)

namespace JSC {

BaselineJITPlan::BaselineJITPlan(CodeBlock* codeBlock)
    : JITPlan(JITCompilationMode::Baseline, codeBlock)
{
    VM& vm = codeBlock->vm();
    JIT::doMainThreadPreparationBeforeCompile(vm);
    codeBlock->unlinkedCodeBlock()->ensureValueAndArrayProfiles();

    // JITCache: whether the compilation records its image. Both callers, jitCompileAndSetHeuristics and JIT::compileSync,
    // construct the plan on the VM thread, where the glue's test runs.
    m_jitCacheRecordsImage = JITCache::shouldRecordBaselineImage(vm, *codeBlock);

    // JITCache: the plan measures its native cost, and passes it to the finalize capture hook, exactly when the VM has an
    // open bench report now.
    m_jitCacheMeasure = !!JITCache::benchReport(vm);
}

auto BaselineJITPlan::compileInThreadImpl(JITCompilationEffort effort) -> CompilationPath
{
    m_codeBlock->updatePredictionsConcurrently();

    // JITCache: the native cost counts the compilation after its profile drain.
    JITCache::BaselineSpanTimer jitCacheTimer(*m_vm, JITCache::BaselineSpan::Compile, m_jitCacheMeasure);

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
    if (auto span = jitCacheTimer.stop()) {
        m_jitCacheCompileNanoseconds = span->nanoseconds;
        m_jitCacheSupportGenerated = span->supportGenerated;
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
    // relinks of incoming calls that installCode makes meanwhile.
    JITCache::BaselineSpanTimer jitCacheTimer(*m_vm, JITCache::BaselineSpan::Finalize, m_jitCacheMeasure);

    CompilationResult result = JIT::finalizeOnMainThread(m_codeBlock, *this, m_jitCode);
    switch (result) {
    case CompilationResult::CompilationFailed:
        // JITCache: a baseline plan fails only when it found no executable memory, so report that before the lines
        // below write the failure's effects.
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
        // JITCache: the finalize capture, now that the CB is its executable's replacement, with both spans' native cost
        // when the plan measured it.
        std::optional<JITCache::BaselineCompileTiming> jitCacheTiming;
        if (auto span = jitCacheTimer.stop())
            jitCacheTiming = JITCache::BaselineCompileTiming { m_jitCacheCompileNanoseconds, span->nanoseconds, m_jitCacheSupportGenerated || span->supportGenerated };
        JITCache::didFinalizeBaselineCompilation(*m_vm, *m_codeBlock, jitCacheTiming ? &*jitCacheTiming : nullptr);
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
