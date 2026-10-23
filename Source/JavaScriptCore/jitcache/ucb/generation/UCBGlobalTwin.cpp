// The twin generation of a program, module or eval body, for test builds. runtime/CodeCache.cpp includes this file at its
// end, where generateUnlinkedCodeBlockImpl, local to that file, is defined. It compiles only there, so no Sources.txt lists
// it.

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/ucb/UCBRequests.h"
#include "jitcache/ucb/UCBTwinGeneration.h"

namespace JSC::JITCache {

// The twin of a program, module or eval body: what the request's native generation makes, with the request's
// snapshot of its features in place of the executable's, which an import has overwritten with the parse's. Every
// other input comes from the executable, as native generation reads it, since no import changes those fields.
UnlinkedCodeBlock* generateGlobalTwin(const RequestState& state, TwinParseResults& results, ParserError& error)
{
    VM& vm = state.vm;
    GlobalExecutable* globalExecutable = state.globalExecutable;
    RELEASE_ASSERT(globalExecutable);

    switch (state.kind) {
    case RequestKind::Program: {
        auto* executable = uncheckedDowncast<ProgramExecutable>(globalExecutable);
        return generateUnlinkedCodeBlockImpl<UnlinkedProgramCodeBlock, ProgramExecutable>(vm, state.source, state.requestFeatures, state.scriptMode, state.requestMode, error,
            state.evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), nullptr, nullptr, executable, OptimizeBytecode::No, &results);
    }
    case RequestKind::Module: {
        auto* executable = uncheckedDowncast<ModuleProgramExecutable>(globalExecutable);
        return generateUnlinkedCodeBlockImpl<UnlinkedModuleProgramCodeBlock, ModuleProgramExecutable>(vm, state.source, state.requestFeatures, state.scriptMode, state.requestMode, error,
            state.evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), nullptr, nullptr, executable, OptimizeBytecode::No, &results);
    }
    case RequestKind::IndirectEval: {
        auto* executable = uncheckedDowncast<IndirectEvalExecutable>(globalExecutable);
        return generateUnlinkedCodeBlockImpl<UnlinkedEvalCodeBlock, IndirectEvalExecutable>(vm, state.source, state.requestFeatures, state.scriptMode, state.requestMode, error,
            state.evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), nullptr, nullptr, executable, OptimizeBytecode::No, &results);
    }
    case RequestKind::DirectEval: {
        auto* executable = uncheckedDowncast<DirectEvalExecutable>(globalExecutable);
        return generateUnlinkedCodeBlockImpl<UnlinkedEvalCodeBlock, DirectEvalExecutable>(vm, state.source, state.requestFeatures, state.scriptMode, state.requestMode, error,
            state.evalContextType, executable->derivedContextType(), executable->isArrowFunctionContext(), state.variablesUnderTDZ, state.privateNameEnvironment, executable, OptimizeBytecode::No, &results);
    }
    case RequestKind::FunctionBody:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
