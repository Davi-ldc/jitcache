#pragma once

#include "CodeSpecializationKind.h"
#include "ExecutableInfo.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/ucb/DirectEvalSite.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC {

class DirectEvalExecutable;
class GlobalExecutable;
class SourceCode;
class UnlinkedCodeBlock;
class UnlinkedEvalCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedGlobalCodeBlock;
class VM;
struct BuiltinSourceMetadata;

enum class SourceCodeType;

} // namespace JSC

namespace JSC::JITCache {

// The request objects and hooks native call sites use. Native call sites create one request object per request and tell
// it what happened; each member is a thin call into the engine of UCBImport.h. Every member is
// a no-op unless the request is active (the VM had JITCache state that tracks keys when the request began) and not
// settled. A request settles once it has recorded, imported or attached the UCB it asked for. No hook runs where the
// native path decodes a bytecode payload: what a native decode makes gets no record and no identity, so nothing in it
// imports or is captured.
//
// The constructors of GlobalRequest and DirectEvalRequest run before any native step of their request and snapshot the
// executable's lexicallyScopedFeatures() into RequestState::requestFeatures; keys and contexts read only that snapshot.

enum class RequestKind : uint8_t { Program, Module, IndirectEval, FunctionBody, DirectEval };
enum class MissReason : uint8_t; // UCBRegistry.h

// One per request, inside the request object on the native call site's stack. The constructors below fill the
// inputs; only the engine reads them and writes the rest.
struct RequestState {
    WTF_MAKE_NONCOPYABLE(RequestState);
    RequestState(VM&, RequestKind, const SourceCode&, OptionSet<CodeGenerationMode> requestMode, LexicallyScopedFeatures requestFeatures);

    VM& vm;
    const RequestKind kind;
    const SourceCode& source;
    const OptionSet<CodeGenerationMode> requestMode;
    const LexicallyScopedFeatures requestFeatures; // every kind but FunctionBody: the snapshot above; FunctionBody: none
    GlobalExecutable* globalExecutable { nullptr }; // every kind but FunctionBody
    UnlinkedFunctionExecutable* functionExecutable { nullptr }; // FunctionBody
    CodeSpecializationKind specialization { CodeSpecializationKind::CodeForCall };
    JSParserScriptMode scriptMode { JSParserScriptMode::Classic };
    EvalContextType evalContextType { EvalContextType::None };
    const DirectEvalSite* site { nullptr }; // DirectEval; null means no key
    const TDZEnvironment* variablesUnderTDZ { nullptr }; // DirectEval
    const PrivateNameEnvironment* privateNameEnvironment { nullptr }; // DirectEval

    bool active { false };
    bool settled { false };
    std::optional<uint64_t> missedBodyVersion; // the index token tryImport missed at; didGenerate records it
    std::optional<Digest256> sourceDigest; // roots: the root source digest; direct eval: of the evaluated text; taken once
    std::optional<std::optional<BodyKey>> key; // the request's key, computed once: outer empty, not yet; inner empty, no key
    std::optional<Digest256> context; // computed at most once, only when read
    std::optional<Digest256> holderDigest; // FunctionBody: holderDigest of functionExecutable, computed at most once (the holder check, a root's context)
    std::optional<Digest256> vouchedHolderDigest; // set with a child's key: the digest its imported parent's body lists for it, if any
    std::optional<MissReason> missReason; // why the import found no UCB to return, once it got that far; the generate event reports it
    std::optional<uint64_t> missNanoseconds; // with a bench report open: the thread CPU time when tryImport missed, for the generate event
};

// Program, module and indirect eval: SourceCodeType::ProgramType, ModuleType and EvalType map to Program, Module and
// IndirectEval. The request takes its source digest and computes its key once, on first use, so a request that ends in a
// map hit does neither.
class GlobalRequest {
    WTF_MAKE_NONCOPYABLE(GlobalRequest);

public:
    GlobalRequest(VM&, GlobalExecutable&, const SourceCode&, SourceCodeType, JSParserScriptMode, OptionSet<CodeGenerationMode>, EvalContextType);
    bool isActive() const { return m_state.active; }
    // A UCB the CodeCache returned: a map hit, or one it just decoded from the provider's cached bytecode, which has no
    // record and so gets nothing.
    void didServeLive(UnlinkedGlobalCodeBlock&);
    // The native path would generate. Returns an imported UCB with parse fields restored, feedback seeded and its record and
    // pending import in place, or null for a miss or after invalid material. Its class is the request's.
    UnlinkedGlobalCodeBlock* tryImport();
    void didGenerate(UnlinkedGlobalCodeBlock&);

private:
    RequestState m_state;
};

class FunctionBodyRequest { // the body of any UFE: child, Function constructor, builtin
    WTF_MAKE_NONCOPYABLE(FunctionBodyRequest);

public:
    FunctionBodyRequest(VM&, UnlinkedFunctionExecutable&, const SourceCode&, CodeSpecializationKind, OptionSet<CodeGenerationMode>);
    bool isActive() const { return m_state.active; }
    void didServeLive(UnlinkedFunctionCodeBlock&); // the requested slot was filled, by an earlier request or by a native decode
    UnlinkedFunctionCodeBlock* tryImport(); // as GlobalRequest::tryImport; restores the UFE's parse fields
    void didGenerate(UnlinkedFunctionCodeBlock&);

private:
    RequestState m_state;
};

class DirectEvalRequest {
    WTF_MAKE_NONCOPYABLE(DirectEvalRequest);

public:
    // Inactive when site is null.
    DirectEvalRequest(VM&, DirectEvalExecutable&, const SourceCode&, const DirectEvalSite*, OptionSet<CodeGenerationMode>, EvalContextType,
        const TDZEnvironment* variablesUnderTDZ, const PrivateNameEnvironment*);
    bool isActive() const { return m_state.active; }
    UnlinkedEvalCodeBlock* tryImport();
    void didGenerate(UnlinkedEvalCodeBlock&);

private:
    RequestState m_state;
};

// Root hooks, each before link reads the UFE and before any holder publishes it.
void didCreateFunctionConstructorExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, LexicallyScopedFeatures, std::optional<int> parametersEndPosition);
void didCreateBuiltinExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, const BuiltinSourceMetadata&);
// Destructor hooks, first in ~UnlinkedCodeBlock and ~UnlinkedFunctionExecutable, on the sweeping thread.
void unlinkedCodeBlockWillBeDestroyed(UnlinkedCodeBlock&);
void unlinkedFunctionExecutableWillBeDestroyed(UnlinkedFunctionExecutable&);

} // namespace JSC::JITCache
