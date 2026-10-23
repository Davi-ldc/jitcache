#pragma once

#include <wtf/Platform.h>

namespace JSC::JITCache {

// What a twin generation records in place of the executable's parse fields. The engine's generators take a pointer to it
// in every build; only test builds define it and pass one.
struct TwinParseResults;

} // namespace JSC::JITCache

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

// The UCB lane's twins-only functions that reach private engine code, each defined in a file that one engine file includes
// at its end: the twin generation helpers verifyImport uses, generateGlobalTwin in generation/UCBGlobalTwin.cpp, which
// runtime/CodeCache.cpp includes and which calls generateUnlinkedCodeBlockImpl, and generateFunctionBodyTwin in
// generation/UCBFunctionBodyTwin.cpp, which bytecode/UnlinkedFunctionExecutable.cpp includes and which calls
// generateUnlinkedFunctionCodeBlock; and the core codec's test of altered records in codec/UCBCoreCodecSelfTest.cpp, which
// runtime/CachedTypes.cpp includes.

struct RequestState;

struct TwinParseResults {
    CodeFeatures features { 0 };
    LexicallyScopedFeatures lexicallyScopedFeatures { NoLexicallyScopedFeatures };
    bool hasCapturedVariables { false };
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
