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

#pragma once

#include "JITPlan.h"

#if ENABLE(JIT)

#include "JIT.h"

namespace JSC {

class BaselineJITCode;

class BaselineJITPlan final : public JITPlan {
    using Base = JITPlan;

public:
    BaselineJITPlan(CodeBlock*);

    CompilationPath compileInThreadImpl() final;
    size_t codeSize() const final;
    CompilationResult finalize() override;

    CompilationPath compileSync(JITCompilationEffort);

    bool isKnownToBeLiveAfterGC() final;
    bool isKnownToBeLiveDuringGC(AbstractSlotVisitor&) final;

    // JITCache: whether this plan's compilation records its image (SPEC-integrator.md section 4.4, SPEC-image.md
    // R-INT-12). The constructor writes it on the VM thread before the plan is enqueued or compiled, and whichever thread
    // compiles only reads it, so it needs no lock.
    bool jitCacheRecordsImage() const { return m_jitCacheRecordsImage; }

private:
    CompilationPath compileInThreadImpl(JITCompilationEffort);

    RefPtr<BaselineJITCode> m_jitCode;
    bool m_jitCacheRecordsImage { false };
    // JITCache: the native cost of harness sub-SPEC section 9.3. The constructor sets m_jitCacheMeasure on the VM thread
    // when the VM has an open bench report. While it is set, compileInThreadImpl writes the other two on the thread that
    // compiles, and finalize reads them on the VM thread after the worklist has handed it the plan, as it reads m_jitCode.
    bool m_jitCacheMeasure { false };
    bool m_jitCacheSupportGenerated { false }; // per-VM support generated during the compilation's measured span
    uint64_t m_jitCacheCompileNanoseconds { 0 }; // thread CPU time of the compilation after its profile drain
};

} // namespace JSC

#endif // ENABLE(JIT)
