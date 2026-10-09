/*
 * Copyright (C) 2016-2024 Apple Inc. All rights reserved.
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
#include "JITMathIC.h"

#if ENABLE(JIT)

#include "CodeBlock.h"
#include "ImageEmission.h"
#include "ImageRecord.h"
#include "LinkBuffer.h"
#include "Repatch.h"

namespace JSC {

template<typename GeneratorType, typename ArithProfileType>
void JITMathIC<GeneratorType, ArithProfileType>::generateOutOfLine(CodeBlock* codeBlock, CodePtr<CFunctionPtrTag> callReplacement)
{
    JITCache::MathICRegeneration regeneration(codeBlock, this, callReplacement, m_arithProfile ? static_cast<uint16_t>(m_arithProfile->bits()) : 0);
    generateOutOfLine(codeBlock, callReplacement, regeneration);
}

// The regeneration learns of every change this makes to the code. Unless it records, no recorder is attached, and each
// JITCache helper below emits exactly the native link it replaces.
template<typename GeneratorType, typename ArithProfileType>
void JITMathIC<GeneratorType, ArithProfileType>::generateOutOfLine(CodeBlock* codeBlock, CodePtr<CFunctionPtrTag> callReplacement, JITCache::MathICRegeneration& regeneration)
{
    // The rewrite of the inline start stays native and attaches no recorder: it emits one jump and never draws a random
    // number. The regeneration updates the record once the jump is written.
    auto linkJumpToOutOfLineSnippet = [&] {
        CCallHelpers jit(codeBlock);
        jit.jumpThunk(CodeLocationLabel<JITStubRoutinePtrTag>(m_code.code()));
        RELEASE_ASSERT(jit.m_assembler.buffer().codeSize() <= static_cast<size_t>(MacroAssembler::differenceBetweenCodePtr(m_inlineStart, m_inlineEnd)));
        LinkBuffer linkBuffer(jit, m_inlineStart, jit.m_assembler.buffer().codeSize(), LinkBuffer::Profile::InlineCache, JITCompilationMustSucceed);
        RELEASE_ASSERT(linkBuffer.isValid());
        FINALIZE_CODE(linkBuffer, NoPtrTag, nullptr, "JITMathIC: linking constant jump to out of line stub");
        regeneration.didRewriteInlineStart();
    };

    auto replaceCall = [&] {
        ftlThunkAwareRepatchCall(codeBlock, slowPathCallLocation().template retagged<JSInternalPtrTag>(), callReplacement);
        regeneration.didReplaceSlowCall(callReplacement);
    };

    bool shouldEmitProfiling = !JSC::JITCode::isOptimizingJIT(codeBlock->jitType());

    if (m_generateFastPathOnRepatch) {
        CCallHelpers jit(codeBlock);
        regeneration.attach(jit);
        MathICGenerationState generationState;
        bool generatedInline = generateInline(jit, generationState, shouldEmitProfiling);

        // We no longer want to try to regenerate the fast path.
        m_generateFastPathOnRepatch = false;

        if (generatedInline) {
            JITCache::jumpToImage(jit, regeneration.imageOffset(doneLocation()), doneLocation());
            JITCache::linkJumpsToImage(jit, generationState.slowPathJumps, regeneration.imageOffset(slowPathStartLocation()), slowPathStartLocation());
            regeneration.emitVeneers(jit);

            LinkBuffer linkBuffer(jit, codeBlock, LinkBuffer::Profile::InlineCache, JITCompilationCanFail);
            if (!linkBuffer.didFailToAllocate()) {
                m_code = FINALIZE_CODE_FOR(codeBlock, linkBuffer, JITStubRoutinePtrTag, nullptr, "JITMathIC: generating out of line fast IC snippet");
                regeneration.didLinkSnippet(linkBuffer, m_code);

                if (!generationState.shouldSlowPathRepatch) {
                    // We won't need to regenerate, so we can wire the slow path call
                    // to a non repatching variant.
                    replaceCall();
                }

                linkJumpToOutOfLineSnippet();

                return;
            }
            regeneration.didFailToAllocate(codeBlock->vm());
        }

        // We weren't able to generate an out of line fast path.
        // We just generate the snippet in its full generality.
    }

    // We rewire to the alternate regardless of whether or not we can allocate the out of line path
    // because if we fail allocating the out of line path, we don't want to waste time trying to
    // allocate it in the future.
    replaceCall();

    {
        CCallHelpers jit(codeBlock);
        regeneration.attach(jit);

        MacroAssembler::JumpList endJumpList;
        MacroAssembler::JumpList slowPathJumpList;

        bool emittedFastPath = m_generator.generateFastPath(jit, endJumpList, slowPathJumpList, m_arithProfile, shouldEmitProfiling);
        if (!emittedFastPath)
            return;
        endJumpList.append(jit.jump());
        JITCache::linkJumpsToImage(jit, endJumpList, regeneration.imageOffset(doneLocation()), doneLocation());
        JITCache::linkJumpsToImage(jit, slowPathJumpList, regeneration.imageOffset(slowPathStartLocation()), slowPathStartLocation());
        regeneration.emitVeneers(jit);

        LinkBuffer linkBuffer(jit, codeBlock, LinkBuffer::Profile::InlineCache, JITCompilationCanFail);
        if (linkBuffer.didFailToAllocate()) {
            regeneration.didFailToAllocate(codeBlock->vm());
            return;
        }

        m_code = FINALIZE_CODE_FOR(codeBlock, linkBuffer, JITStubRoutinePtrTag, nullptr, "JITMathIC: generating out of line IC snippet");
        regeneration.didLinkSnippet(linkBuffer, m_code);
    }

    linkJumpToOutOfLineSnippet();
}

template void JITMathIC<JITAddGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>);
template void JITMathIC<JITAddGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>, JITCache::MathICRegeneration&);
template void JITMathIC<JITMulGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>);
template void JITMathIC<JITMulGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>, JITCache::MathICRegeneration&);
template void JITMathIC<JITSubGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>);
template void JITMathIC<JITSubGenerator, BinaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>, JITCache::MathICRegeneration&);
template void JITMathIC<JITNegGenerator, UnaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>);
template void JITMathIC<JITNegGenerator, UnaryArithProfile>::generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag>, JITCache::MathICRegeneration&);

} // namespace JSC

#endif // ENABLE(JIT)
