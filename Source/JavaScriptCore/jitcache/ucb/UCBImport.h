#pragma once

#include "ParserModes.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/ucb/UCBRequests.h"
#include <optional>

namespace JSC {

class SourceCode;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The engine's entry points, which the request objects call. All run on the VM thread holding the API lock and heap
// access. The request's own digests are in UCBMatch.h.

// Where the native path would generate: imports a body at the request's key, decoding its core into a new UCB, seeding its
// feedback and, for a body with an image, attaching a pending import, or returns null.
UnlinkedCodeBlock* importBody(RequestState&);
// The holder already has the UCB: attaches a pending import when the UCB fits the body at its key, so that the body's
// image can still install. It allocates the UCB's value and array copies, as the baseline plan the install skips would,
// and writes nothing else to the published UCB.
void attachLive(RequestState&, UnlinkedCodeBlock&);
void recordGenerated(RequestState&, UnlinkedCodeBlock&);
// A module request in a producing VM: keeps the record JSC's analysis parse built as the UCB's analysis source, unless
// its record holds a live one. importBody, attachLive and recordGenerated call it themselves.
void keepModuleAnalysisSource(RequestState&, UnlinkedCodeBlock&);
// Records a root UFE's identity: a Function constructor's or a builtin's.
void recordRoot(VM&, UnlinkedFunctionExecutable&, IdentityKind, const SourceCode& rootSource, LexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest = nullptr);

} // namespace JSC::JITCache
