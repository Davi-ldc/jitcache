#include "config.h"
#include "UCBRequests.h"

#include "BuiltinExecutables.h"
#include "DirectEvalExecutable.h"
#include "GlobalExecutable.h"
#include "JITCacheBench.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "SourceCode.h"
#include "SourceCodeKey.h"
#include "UCBImport.h"
#include "UCBRegistry.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedGlobalCodeBlock.h"
#include "VM.h"

// The request objects and hooks the native call sites use. Each member only decides whether the request still acts and
// then calls the engine of UCBImport.h, which re-checks the VM's state at every step and writes everything in RequestState
// past its inputs, `settled` included.

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

// The bench report's generate event times what the native path generates after an import missed: the
// thread CPU time from tryImport's miss to didGenerate's start, which holds the parse and the bytecode generation and
// nothing JITCache does. Without a bench report it costs one load and one test.
static void noteImportMissed(RequestState& state)
{
    VMState* vmState = state.vm.jitCacheState();
    if (vmState && vmState->benchReport())
        state.missNanoseconds = benchThreadCPUNanoseconds();
}

static std::optional<uint64_t> generationEnd(const RequestState& state)
{
    if (!state.missNanoseconds)
        return std::nullopt;
    return benchThreadCPUNanoseconds();
}

static ASCIILiteral generatedKindName(const RequestState& state)
{
    if (state.key && *state.key) {
        switch ((*state.key)->identityKind()) {
        case IdentityKind::Program:
            return "program"_s;
        case IdentityKind::Module:
            return "module"_s;
        case IdentityKind::IndirectEval:
            return "indirectEval"_s;
        case IdentityKind::FunctionConstructor:
            return "functionConstructor"_s;
        case IdentityKind::Builtin:
            return "builtin"_s;
        case IdentityKind::Child:
            return "child"_s;
        case IdentityKind::DirectEval:
            return "directEval"_s;
        }
    }
    // Without a key the request's own kind is all there is; a UFE body of unknown identity says "function".
    switch (state.kind) {
    case RequestKind::Program:
        return "program"_s;
    case RequestKind::Module:
        return "module"_s;
    case RequestKind::IndirectEval:
        return "indirectEval"_s;
    case RequestKind::FunctionBody:
        return "function"_s;
    case RequestKind::DirectEval:
        return "directEval"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "function"_s;
}

static ASCIILiteral missReasonName(std::optional<MissReason> reason)
{
    if (!reason)
        return "none"_s;
    switch (*reason) {
    case MissReason::NoKey:
        return "noKey"_s;
    case MissReason::NoBody:
        return "noBody"_s;
    case MissReason::BodyUnchanged:
        return "bodyUnchanged"_s;
    case MissReason::RequestKey:
        return "requestKey"_s;
    case MissReason::Context:
        return "context"_s;
    case MissReason::Provenance:
        return "provenance"_s;
    case MissReason::Holder:
        return "holder"_s;
    case MissReason::AtomMap:
        return "atomMap"_s;
    case MissReason::CoreDigest:
        return "coreDigest"_s;
    case MissReason::Stack:
        return "stack"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "none"_s;
}

static void recordGenerateEvent(const RequestState& state, const UnlinkedCodeBlock& codeBlock, std::optional<uint64_t> end)
{
    if (!end)
        return;
    VMState* vmState = state.vm.jitCacheState();
    BenchReport* report = vmState ? vmState->benchReport() : nullptr;
    if (!report)
        return;
    using BenchValue = decltype(BenchField::value);
    BenchValue key = nullptr;
    if (state.key && *state.key)
        key = bodyKeyHex(**state.key);
    BenchValue specialization = nullptr;
    if (state.kind == RequestKind::FunctionBody)
        specialization = String { state.specialization == CodeSpecializationKind::CodeForCall ? "call"_s : "construct"_s };
    SourceProvider* provider = state.source.provider();
    report->record("generate"_s, {
        { "key"_s, key },
        { "kind"_s, String { generatedKindName(state) } },
        { "specialization"_s, specialization },
        { "miss"_s, String { missReasonName(state.missReason) } },
        { "nanoseconds"_s, *end - *state.missNanoseconds },
        { "sourceCharacters"_s, static_cast<uint64_t>(state.source.length()) },
        { "instructionBytes"_s, static_cast<uint64_t>(codeBlock.instructions().sizeInBytes()) },
        { "url"_s, provider ? provider->sourceURL() : String { } },
    });
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
// was built from, before a CodeCache hit or a generation overwrites the executable's.
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
    // The core decodes as the kind the key implies, which is the request's class.
    UnlinkedCodeBlock* imported = importBody(m_state);
    if (!imported)
        noteImportMissed(m_state);
    return uncheckedDowncast<UnlinkedGlobalCodeBlock>(imported);
}

void GlobalRequest::didGenerate(UnlinkedGlobalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    std::optional<uint64_t> end = generationEnd(m_state);
    recordGenerated(m_state, codeBlock);
    recordGenerateEvent(m_state, codeBlock, end);
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
    UnlinkedCodeBlock* imported = importBody(m_state);
    if (!imported)
        noteImportMissed(m_state);
    return uncheckedDowncast<UnlinkedFunctionCodeBlock>(imported);
}

void FunctionBodyRequest::didGenerate(UnlinkedFunctionCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    std::optional<uint64_t> end = generationEnd(m_state);
    recordGenerated(m_state, codeBlock);
    recordGenerateEvent(m_state, codeBlock, end);
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
    // debugger has already turned cache activity off.
    if (!site)
        m_state.active = false;
}

UnlinkedEvalCodeBlock* DirectEvalRequest::tryImport()
{
    if (!ucbRequestActs(m_state))
        return nullptr;
    UnlinkedCodeBlock* imported = importBody(m_state);
    if (!imported)
        noteImportMissed(m_state);
    return uncheckedDowncast<UnlinkedEvalCodeBlock>(imported);
}

void DirectEvalRequest::didGenerate(UnlinkedEvalCodeBlock& codeBlock)
{
    if (!ucbRequestActs(m_state))
        return;
    std::optional<uint64_t> end = generationEnd(m_state);
    recordGenerated(m_state, codeBlock);
    recordGenerateEvent(m_state, codeBlock, end);
}

// Root hooks. Each runs before link reads the UFE and before any holder publishes it, looks up
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
    // The builtins generator's digest when it recorded one; the runtime scan records none.
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

// Destructor hooks, on the thread sweeping the VM's heap. vm() is valid during the sweep, and the state outlives every
// cell of its VM, so the entries go even after cache activity turned off.

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
