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
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedGlobalCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The engine's entry points, which the request objects call. All run on the VM thread holding the API lock and heap
// access. A request's key mode is fixed by the UCB it acts on, so requestKey is asked for one mode per request;
// recordDecodedSlot computes the other slot's key itself.

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
// The key of a program or module the native path decoded: the request's key with the decoded UCB's own mode, or empty when
// the decoded UCB's own with-scope bit differs from the snapshot's.
std::optional<BodyKey> decodedRootKey(RequestState&, const UnlinkedGlobalCodeBlock&);

// Imports the request's body: decodes its core into a new UCB, seeds its feedback and attaches a pending import, or misses.
UnlinkedCodeBlock* importBody(RequestState&);
// Seeds a UCB the native path decoded from the body whose core it matches, or records it when it does not seed.
void seedDecoded(RequestState&, UnlinkedCodeBlock&);
// Attaches the body to a live UCB that matches it, so that the body's image can still install.
void attachLive(RequestState&, UnlinkedCodeBlock&);
void recordGenerated(RequestState&, UnlinkedCodeBlock&);
void recordDecodedSlot(RequestState&, UnlinkedFunctionCodeBlock&, CodeSpecializationKind); // never settles the request
// Records a root UFE's identity: a Function constructor's or a builtin's.
void recordRoot(VM&, UnlinkedFunctionExecutable&, IdentityKind, const SourceCode& rootSource, LexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest = nullptr);

} // namespace JSC::JITCache
