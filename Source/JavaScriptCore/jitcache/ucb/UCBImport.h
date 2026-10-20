#pragma once

#include "CodeSpecializationKind.h"
#include "ParserModes.h"
#include "UCBKeys.h"
#include "UCBRequests.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC {

class SourceCode;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The engine's entry points, which the request objects call. All run on the VM thread holding the API lock and heap
// access. A request's key mode is fixed by the UCB it acts on, so requestKey is asked for one mode per request.

// The request's key with the given mode and the snapshot's bits, computed once per request and cached in the state with the
// source digest it rests on and the provider of a supplied digest; empty when the request has no key.
std::optional<BodyKey> requestKey(RequestState&, OptionSet<CodeGenerationMode> keyMode);
// The request's context digest and a UFE body's holder digest, each computed at most once, by the first step that reads
// it. requestContext requires a key; a root body's context reads requestHolderDigest.
const Digest256& requestContext(RequestState&);
const Digest256& requestHolderDigest(RequestState&);
// What a record made by the request keeps of its context: the inputs its context digest is computed from, or for a direct
// eval the digest requestContext computed, which it computes now if it has not and production is active.
RecordedContext recordedContext(RequestState&);

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
