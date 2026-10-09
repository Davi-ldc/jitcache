#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "ParserModes.h"

namespace JSC {

class ParserError;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;

} // namespace JSC

namespace JSC::JITCache {

// The twin generation helpers verifyImport uses (SPEC-ucb.md section 13.2). generateGlobalTwin is defined in
// runtime/CodeCache.cpp on generateUnlinkedCodeBlockImpl, and generateFunctionBodyTwin in
// bytecode/UnlinkedFunctionExecutable.cpp on generateUnlinkedFunctionCodeBlock.

struct RequestState;

struct TwinParseResults {
    CodeFeatures features { 0 };
    LexicallyScopedFeatures lexicallyScopedFeatures { NoLexicallyScopedFeatures };
    bool hasCapturedVariables { false };
    int lastLine { 0 }; // globals and direct eval: what recordParse would give the executable
    unsigned endColumn { 0 }; // likewise
};

// Each generates what the request's native generation would, but writes its parse results only into `results`: no
// executable, UFE, CodeCache, provider cache hook or registry changes. The parse itself still writes the provider's
// directives and interns TDZ environments, as section 13.2 describes. Null, with `error` set, on a parse or generation error.
UnlinkedCodeBlock* generateGlobalTwin(const RequestState&, TwinParseResults&, ParserError&); // programs, modules, both evals
UnlinkedFunctionCodeBlock* generateFunctionBodyTwin(const RequestState&, TwinParseResults&, ParserError&); // UFE bodies

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
