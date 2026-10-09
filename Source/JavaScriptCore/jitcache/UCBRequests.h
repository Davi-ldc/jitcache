#pragma once

#include "CodeSpecializationKind.h"
#include "DirectEvalSite.h"
#include "ExecutableInfo.h"
#include "ParserModes.h"
#include "SourceProvider.h"
#include "UCBKeys.h"
#include "VariableEnvironment.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/RefPtr.h>

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

// The request objects and hooks native call sites use (SPEC-ucb.md section 7.1). Native call sites create one request
// object per request and tell it what happened; each member is a thin call into the engine of section 7.7. Every member is
// a no-op unless the request is active (the VM had JITCache state that tracks keys when the request began) and not
// settled. A request settles once it has recorded, imported, seeded or attached the UCB it asked for.
//
// The constructors of GlobalRequest and DirectEvalRequest run before any native step of their request and snapshot the
// executable's lexicallyScopedFeatures() into RequestState::requestFeatures; keys and contexts read only that snapshot.

enum class RequestKind : uint8_t { Program, Module, IndirectEval, FunctionBody, DirectEval };

// One per request, inside the request object on the native call site's stack. The constructors below fill the
// inputs; only the engine (section 7.7) reads them and writes the rest.
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
    std::optional<Digest256> sourceDigest; // roots: the root source digest of section 3.5; direct eval: of the evaluated text; taken once
    std::optional<std::optional<BodyKey>> key; // the request's key, computed once (section 7.7): outer empty, not yet; inner empty, no key
    RefPtr<SourceProvider> suppliedDigestProvider; // set with the key: the provider an import at the key relies on (section 6.1); none for a direct eval
    std::optional<Digest256> context; // computed at most once, only when read (section 3.4)
    std::optional<Digest256> holderDigest; // FunctionBody: holderDigest of functionExecutable, computed at most once (C11, a root's context)
};

// Program, module and indirect eval: SourceCodeType::ProgramType, ModuleType and EvalType map to Program, Module and
// IndirectEval. The request takes its source digest and computes its key once, on first use, so a request that ends in a
// map hit does neither unless an attach is about to follow (section 7.3.4).
class GlobalRequest {
    WTF_MAKE_NONCOPYABLE(GlobalRequest);

public:
    GlobalRequest(VM&, GlobalExecutable&, const SourceCode&, SourceCodeType, JSParserScriptMode, OptionSet<CodeGenerationMode>, EvalContextType);
    bool isActive() const { return m_state.active; }
    void didServeLive(UnlinkedGlobalCodeBlock&); // a CodeCache map hit (section 7.3.4)
    void didDecode(UnlinkedGlobalCodeBlock&); // the provider's cached bytecode, before the map holds it (section 7.3.3)
    // The native path would generate. Returns an imported UCB with parse fields restored, feedback seeded and its record and
    // pending import in place, or null for a miss or after invalid material. Its class is the request's (check C1).
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
    // Inside decodeCachedCodeBlocks, after both cached slots are decoded and before m_isCached clears (F1). Seeds the requested
    // specialization's UCB (section 7.3.3) and records the other one as Decoded without settling the request.
    void didDecodeCachedSlots(UnlinkedFunctionCodeBlock* forCall, UnlinkedFunctionCodeBlock* forConstruct);
    void didServeLive(UnlinkedFunctionCodeBlock&); // the requested slot was filled
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

// Root hooks (section 7.2.4), each before link reads the UFE and before any holder publishes it.
void didCreateFunctionConstructorExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, LexicallyScopedFeatures, std::optional<int> parametersEndPosition);
void didCreateBuiltinExecutable(VM&, UnlinkedFunctionExecutable&, const SourceCode&, const BuiltinSourceMetadata&);
void didDecodeBuiltinExecutable(VM&, UnlinkedFunctionExecutable&, SourceProvider&);
// Destructor hooks (section 6.3), first in ~UnlinkedCodeBlock and ~UnlinkedFunctionExecutable, on the sweeping thread.
void unlinkedCodeBlockWillBeDestroyed(UnlinkedCodeBlock&);
void unlinkedFunctionExecutableWillBeDestroyed(UnlinkedFunctionExecutable&);

} // namespace JSC::JITCache
