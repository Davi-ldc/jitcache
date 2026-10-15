#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "ParserModes.h"
#include <wtf/Forward.h>

namespace JSC {

class ParserError;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The UCB lane's twins-only functions that engine files define beside the private code they reach: the twin generation
// helpers verifyImport uses, generateGlobalTwin in runtime/CodeCache.cpp on generateUnlinkedCodeBlockImpl and
// generateFunctionBodyTwin in bytecode/UnlinkedFunctionExecutable.cpp on generateUnlinkedFunctionCodeBlock; and the core
// codec's test of altered records in runtime/CachedTypes.cpp.

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
// directives and interns TDZ environments. Null, with `error` set, on a parse or generation error.
UnlinkedCodeBlock* generateGlobalTwin(const RequestState&, TwinParseResults&, ParserError&); // programs, modules, both evals
UnlinkedFunctionCodeBlock* generateFunctionBodyTwin(const RequestState&, TwinParseResults&, ParserError&); // UFE bodies

// The core codec's cases that alter a core's records, which testjitcache runs: each encodes a core, changes one record of a
// copy through the record's type, and decodes the copy, which must fail as the case names. It takes the VM's API lock,
// writes the first case that fails to `failure` and returns false.
bool runCoreCodecSelfTest(VM&, String& failure);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
