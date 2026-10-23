// The twin generation of a UFE body, for test builds. bytecode/UnlinkedFunctionExecutable.cpp includes this file at its
// end, where generateUnlinkedFunctionCodeBlock, local to that file, is defined. It compiles only there, so no Sources.txt
// lists it.

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/ucb/UCBRequests.h"
#include "jitcache/ucb/UCBTwinGeneration.h"

namespace JSC::JITCache {

// The twin of a UFE body: what the request's native generation makes, with its parse results written only into
// `results`, so the UFE, its slots and the provider's cache hook stay as the import left them.
UnlinkedFunctionCodeBlock* generateFunctionBodyTwin(const RequestState& state, TwinParseResults& results, ParserError& error)
{
    UnlinkedFunctionExecutable* executable = state.functionExecutable;
    RELEASE_ASSERT(state.kind == RequestKind::FunctionBody && executable);
    return generateUnlinkedFunctionCodeBlock(state.vm, executable, state.source, state.specialization, state.requestMode,
        executable->isBuiltinFunction() ? UnlinkedBuiltinFunction : UnlinkedNormalFunction, error, executable->parseMode(), OptimizeBytecode::No, &results);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
