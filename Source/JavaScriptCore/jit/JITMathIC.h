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

#pragma once

#if ENABLE(JIT)

#include "ArithProfile.h"
#include "CCallHelpers.h"
#include "JITAddGenerator.h"
#include "JITMathICInlineResult.h"
#include "JITMulGenerator.h"
#include "JITNegGenerator.h"
#include "JITSubGenerator.h"
#include "LinkBuffer.h"
#include "Repatch.h"
#include <wtf/TZoneMalloc.h>

namespace JSC {

class LinkBuffer;

namespace JITCache {
class MathICRegeneration;
}

struct MathICGenerationState {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(MathICGenerationState);
    MacroAssembler::Label fastPathStart;
    MacroAssembler::Label fastPathEnd;
    MacroAssembler::Label slowPathStart;
    MacroAssembler::Call slowPathCall;
    MacroAssembler::JumpList slowPathJumps;
    bool shouldSlowPathRepatch;
};

#define ENABLE_MATH_IC_STATS 0

template <typename GeneratorType, typename ArithProfileType>
class JITMathIC {
    WTF_MAKE_TZONE_ALLOCATED_TEMPLATE(JITMathIC);
public:
    JITMathIC(ArithProfileType* arithProfile)
        : m_arithProfile(arithProfile)
    {
    }

    CodeLocationLabel<JSInternalPtrTag> doneLocation() { return m_inlineEnd; }
    CodeLocationCall<JSInternalPtrTag> slowPathCallLocation() { return m_slowPathCallLocation; }
    CodeLocationLabel<JSInternalPtrTag> slowPathStartLocation() { return m_slowPathStartLocation; }

    bool generateInline(CCallHelpers& jit, MathICGenerationState& state, bool shouldEmitProfiling = true)
    {
        jit.padBeforePatch(); // Make sure that the first patchable jump below is aligned, but don't count alignment in the inline size.
        state.fastPathStart = jit.label();
        size_t startSize = jit.m_assembler.buffer().codeSize();

        if (m_arithProfile && m_arithProfile->isObservedTypeEmpty()) {
            // It looks like the MathIC has yet to execute. We don't want to emit code in this
            // case for a couple reasons. First, the operation may never execute, so if we don't emit
            // code, it's a win. Second, if the operation does execute, we can emit better code
            // once we have an idea about the types.
            state.slowPathJumps.append(jit.patchableJump());
            size_t inlineSize = jit.m_assembler.buffer().codeSize() - startSize;
            ASSERT_UNUSED(inlineSize, static_cast<ptrdiff_t>(inlineSize) <= MacroAssembler::patchableJumpSize());
            state.shouldSlowPathRepatch = true;
            state.fastPathEnd = jit.label();
            ASSERT(!m_generateFastPathOnRepatch); // We should have gathered some observed type info about the types before trying to regenerate again.
            m_generateFastPathOnRepatch = true;
            return true;
        }

        JITMathICInlineResult result = m_generator.generateInline(jit, state, m_arithProfile);

        switch (result) {
        case JITMathICInlineResult::GeneratedFastPath: {
            size_t inlineSize = jit.m_assembler.buffer().codeSize() - startSize;
            if (static_cast<ptrdiff_t>(inlineSize) < MacroAssembler::patchableJumpSize()) {
                size_t nopsToEmitInBytes = MacroAssembler::patchableJumpSize() - inlineSize;
                jit.emitNops(nopsToEmitInBytes);
            }
            state.shouldSlowPathRepatch = true;
            state.fastPathEnd = jit.label();
            return true;
        }
        case JITMathICInlineResult::GenerateFullSnippet: {
            MacroAssembler::JumpList endJumpList;
            bool result = m_generator.generateFastPath(jit, endJumpList, state.slowPathJumps, m_arithProfile, shouldEmitProfiling);
            if (result) {
                state.fastPathEnd = jit.label();
                state.shouldSlowPathRepatch = false;
                endJumpList.link(&jit);
                return true;
            }
            return false;
        }
        case JITMathICInlineResult::DontGenerate: {
            return false;
        }
        default:
            ASSERT_NOT_REACHED();
        }

        return false;
    }

    // Both are defined, with their explicit instantiations for the four ICs below, in JITMathIC.cpp. Every native caller
    // uses the first, which builds the regeneration JITCache records through; only JITCache's twin replay passes its own.
    void generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag> callReplacement);
    void generateOutOfLine(CodeBlock*, CodePtr<CFunctionPtrTag> callReplacement, JITCache::MathICRegeneration&);

    void finalizeInlineCode(const MathICGenerationState& state, LinkBuffer& linkBuffer)
    {
        CodeLocationLabel<JSInternalPtrTag> start = linkBuffer.locationOf<JSInternalPtrTag>(state.fastPathStart);
        m_inlineStart = start;

        m_inlineEnd = linkBuffer.locationOf<JSInternalPtrTag>(state.fastPathEnd);
        ASSERT(m_inlineEnd.untaggedPtr() > m_inlineStart.untaggedPtr());

        m_slowPathCallLocation = linkBuffer.locationOf<JSInternalPtrTag>(state.slowPathCall);
        m_slowPathStartLocation = linkBuffer.locationOf<JSInternalPtrTag>(state.slowPathStart);
    }

    ArithProfileType* arithProfile() const { return m_arithProfile; }

#if ENABLE(MATH_IC_STATS)
    size_t m_generatedCodeSize { 0 };
    size_t codeSize() const
    {
        size_t result = m_generatedCodeSize;
        if (m_code)
            result += m_code.size();
        return result;
    }
#endif

    ArithProfileType* m_arithProfile;
    MacroAssemblerCodeRef<JITStubRoutinePtrTag> m_code;
    CodeLocationLabel<JSInternalPtrTag> m_inlineStart;
    CodeLocationLabel<JSInternalPtrTag> m_inlineEnd;
    CodeLocationLabel<JSInternalPtrTag> m_slowPathCallLocation;
    CodeLocationLabel<JSInternalPtrTag> m_slowPathStartLocation;
    bool m_generateFastPathOnRepatch { false };
    GeneratorType m_generator;
};

#define TZONE_TEMPLATE_PARAMS template<typename GeneratorType, typename ArithProfileType>
#define TZONE_TYPE JITMathIC<GeneratorType, ArithProfileType>

WTF_MAKE_TZONE_ALLOCATED_TEMPLATE_IMPL_WITH_MULTIPLE_OR_SPECIALIZED_PARAMETERS();

#undef TZONE_TEMPLATE_PARAMS
#undef TZONE_TYPE

template <typename GeneratorType>
class JITBinaryMathIC : public JITMathIC<GeneratorType, BinaryArithProfile> {
public:
    JITBinaryMathIC(BinaryArithProfile* arithProfile)
        : JITMathIC<GeneratorType, BinaryArithProfile>(arithProfile)
    {
    }
};

typedef JITBinaryMathIC<JITAddGenerator> JITAddIC;
typedef JITBinaryMathIC<JITMulGenerator> JITMulIC;
typedef JITBinaryMathIC<JITSubGenerator> JITSubIC;

template <typename GeneratorType>
class JITUnaryMathIC : public JITMathIC<GeneratorType, UnaryArithProfile> {
public:
    JITUnaryMathIC(UnaryArithProfile* arithProfile)
        : JITMathIC<GeneratorType, UnaryArithProfile>(arithProfile)
    {
    }
};

typedef JITUnaryMathIC<JITNegGenerator> JITNegIC;

} // namespace JSC

#endif // ENABLE(JIT)
