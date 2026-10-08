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
#include "UCBRequests.h"

#include "BuiltinExecutables.h"
#include "DirectEvalExecutable.h"
#include "GlobalExecutable.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "SourceCode.h"
#include "SourceCodeKey.h"
#include "UCBImport.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedGlobalCodeBlock.h"
#include "VM.h"

// The request objects and hooks the native call sites use (SPEC-ucb.md section 7.1). Each member only decides whether
// the request still acts and then calls the engine of section 7.7 (UCBImport.h), which re-checks the VM's state at every
// step and writes everything in RequestState past its inputs, `settled` included.

namespace JSC::JITCache {

// One load and one test while the VM has no JITCache state.
static bool ucbRequestTracksKeys(VM& vm)
{
    VMState* state = vm.jitCacheState();
    return state && state->tracksKeys();
}

// Every member is a no-op unless the request is active and not settled.
static bool ucbRequestActs(const RequestState& state)
{
    return state.active && !state.settled;
}

static RequestKind ucbRequestKindFor(SourceCodeType codeType)
{
    switch (codeType) {
    case SourceCodeType::ProgramType:
        return RequestKind::Program;
    case SourceCodeType::ModuleType:
        return RequestKind::Module;
    case SourceCodeType::EvalType:
        return RequestKind::IndirectEval;
    case SourceCodeType::FunctionType:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return RequestKind::Program;
}

RequestState::RequestState(VM& vm, RequestKind kind, const SourceCode& source, OptionSet<CodeGenerationMode> requestMode, LexicallyScopedFeatures requestFeatures)
    : vm(vm)
    , kind(kind)
    , source(source)
    , requestMode(requestMode)
    , requestFeatures(requestFeatures)
    , active(ucbRequestTracksKeys(vm))
{
}

// The constructor runs before any native step of the request, so the snapshot is the features the native SourceCodeKey
// was built from, before a CodeCache hit or a generation overwrites the executable's (F18).
GlobalRequest::GlobalRequest(VM& vm, GlobalExecutable& executable, const SourceCode& source, SourceCodeType codeType, JSParserScriptMode scriptMode, OptionSet<CodeGenerationMode> codeGenerationMode, EvalContextType evalContextType)
    : m_state(vm, ucbRequestKindFor(codeType), source, codeGenerationMode, executable.lexicallyScopedFeatures())
{
    m_state.globalExecutable = &executable;
    m_state.scriptMode = scriptMode;
    m_state.evalContextType = evalContextType;
}

void GlobalRequest::didServeLive(UnlinkedGlobalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    attachLive(m_state, codeBlock);
}

void GlobalRequest::didDecode(UnlinkedGlobalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    seedDecoded(m_state, codeBlock);
}

UnlinkedGlobalCodeBlock* GlobalRequest::tryImport()
{
    if (!ucbRequestActs(m_state))
        return nullptr;
    // The core decodes as the kind the key implies (codec section 3), which is the request's class.
    return uncheckedDowncast<UnlinkedGlobalCodeBlock>(importBody(m_state));
}

void GlobalRequest::didGenerate(UnlinkedGlobalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    recordGenerated(m_state, codeBlock);
}

FunctionBodyRequest::FunctionBodyRequest(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& source, CodeSpecializationKind specialization, OptionSet<CodeGenerationMode> codeGenerationMode)
    : m_state(vm, RequestKind::FunctionBody, source, codeGenerationMode, NoLexicallyScopedFeatures)
{
    m_state.functionExecutable = &executable;
    m_state.specialization = specialization;
}

void FunctionBodyRequest::didDecodeCachedSlots(UnlinkedFunctionCodeBlock* forCall, UnlinkedFunctionCodeBlock* forConstruct)
{
    if (!ucbRequestActs(m_state))
        return;
    bool requestsCall = m_state.specialization == CodeSpecializationKind::CodeForCall;
    UnlinkedFunctionCodeBlock* requested = requestsCall ? forCall : forConstruct;
    UnlinkedFunctionCodeBlock* other = requestsCall ? forConstruct : forCall;
    // A requested slot the decode left empty leaves the request unsettled, so it goes on to import or generate.
    if (requested)
        seedDecoded(m_state, *requested);
    // The slot nobody requested is recorded as decoded without settling the request; its own first request attaches.
    if (other)
        recordDecodedSlot(m_state, *other, requestsCall ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall);
}

void FunctionBodyRequest::didServeLive(UnlinkedFunctionCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    attachLive(m_state, codeBlock);
}

UnlinkedFunctionCodeBlock* FunctionBodyRequest::tryImport()
{
    if (!ucbRequestActs(m_state))
        return nullptr;
    return uncheckedDowncast<UnlinkedFunctionCodeBlock>(importBody(m_state));
}

void FunctionBodyRequest::didGenerate(UnlinkedFunctionCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    recordGenerated(m_state, codeBlock);
}

// DirectEvalExecutable::create constructs the request before it generates, so the snapshot is the call site's features.
DirectEvalRequest::DirectEvalRequest(VM& vm, DirectEvalExecutable& executable, const SourceCode& source, const DirectEvalSite* site, OptionSet<CodeGenerationMode> codeGenerationMode, EvalContextType evalContextType,
    const TDZEnvironment* variablesUnderTDZ, const PrivateNameEnvironment* privateNameEnvironment)
    : m_state(vm, RequestKind::DirectEval, source, codeGenerationMode, executable.lexicallyScopedFeatures())
{
    m_state.globalExecutable = &executable;
    m_state.evalContextType = evalContextType;
    m_state.site = site;
    m_state.variablesUnderTDZ = variablesUnderTDZ;
    m_state.privateNameEnvironment = privateNameEnvironment;
    // Without a site the eval has no key. Only DebuggerCallFrame::evaluateWithScopeExtension passes none, and an attached
    // debugger has already turned cache activity off (section 7.2.3).
    if (!site)
        m_state.active = false;
}

UnlinkedEvalCodeBlock* DirectEvalRequest::tryImport()
{
    if (!ucbRequestActs(m_state))
        return nullptr;
    return uncheckedDowncast<UnlinkedEvalCodeBlock>(importBody(m_state));
}

void DirectEvalRequest::didGenerate(UnlinkedEvalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    recordGenerated(m_state, codeBlock);
}

// Root hooks (sections 7.2.4 and 7.3.6). Each runs before link reads the UFE and before any holder publishes it, looks up
// no body and leaves the root's own singleton bit native.

void didCreateFunctionConstructorExecutable(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& source, LexicallyScopedFeatures lexicallyScopedFeatures, std::optional<int> parametersEndPosition)
{
    if (!ucbRequestTracksKeys(vm))
        return;
    recordRoot(vm, executable, IdentityKind::FunctionConstructor, source, lexicallyScopedFeatures, parametersEndPosition);
}

void didCreateBuiltinExecutable(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& source, const BuiltinSourceMetadata& metadata)
{
    if (!ucbRequestTracksKeys(vm))
        return;
    // Form 1 of section 3.5 when the builtins generator recorded the digest; the runtime scan records none.
    recordRoot(vm, executable, IdentityKind::Builtin, source, executable.lexicallyScopedFeatures(), std::nullopt, metadata.hasSourceDigest ? &metadata.sourceDigest : nullptr);
}

void didDecodeBuiltinExecutable(VM& vm, UnlinkedFunctionExecutable& executable, SourceProvider& provider)
{
    if (!ucbRequestTracksKeys(vm))
        return;
    // The root source is the provider's whole source, which spans the provider as the created form's does, so both forms
    // take the same digest. Building it reads only the provider's length.
    SourceCode rootSource { Ref<SourceProvider> { provider } };
    recordRoot(vm, executable, IdentityKind::Builtin, rootSource, executable.lexicallyScopedFeatures(), std::nullopt);
}

// Destructor hooks (section 6.3), on the thread sweeping the VM's heap (F12). vm() is valid during the sweep, and the
// state outlives every cell of its VM (R-INT-1), so the entries go even after cache activity turned off.

void unlinkedCodeBlockWillBeDestroyed(UnlinkedCodeBlock& codeBlock)
{
    if (VMState* state = codeBlock.vm().jitCacheState())
        state->registry().unlinkedCodeBlockDestroyed(&codeBlock);
}

void unlinkedFunctionExecutableWillBeDestroyed(UnlinkedFunctionExecutable& executable)
{
    if (VMState* state = executable.vm().jitCacheState())
        state->registry().unlinkedFunctionExecutableDestroyed(&executable);
}

} // namespace JSC::JITCache
