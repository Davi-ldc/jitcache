#pragma once

#include "ParserModes.h"
#include "jitcache/ucb/UCBKeys.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBRequests.h"
#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class SourceCode;
class UnlinkedFunctionExecutable;

} // namespace JSC

namespace JSC::JITCache {

class RequestTimer;
class VMState;
class ValidatedBody;
struct IdentitySection;

// Matching: whether a body at a request's key is the one the request, or a live UCB's record, describes. All run on the VM
// thread holding the API lock and heap access. A request's key mode is fixed by the UCB it acts on, so requestKey is asked
// for one mode per request.

// The request's key with the given mode and the snapshot's bits, computed once per request and cached in the state with the
// source digest it rests on; empty when the request has no key.
std::optional<BodyKey> requestKey(RequestState&, OptionSet<CodeGenerationMode> keyMode);
// The request's context digest and a UFE body's holder digest, each computed at most once, by the first step that reads
// it. requestContext requires a key; a root body's context reads requestHolderDigest.
const Digest256& requestContext(RequestState&);
const Digest256& requestHolderDigest(RequestState&);
// What a record made by the request keeps of its context: the inputs its context digest is computed from, or for a direct
// eval the digest requestContext computed, which it computes now if it has not and production is active.
RecordedContext recordedContext(RequestState&);

// What UCBImport.cpp builds an import or an attach from. A null timer, or one with no bench report open, measures nothing.
std::optional<BodyKey> requestKeyMeasured(RequestState&, OptionSet<CodeGenerationMode> keyMode, RequestTimer*);
const Digest256& requestContextMeasured(RequestState&, RequestTimer*);
RecordedContext recordedContextMeasured(RequestState&, RequestTimer*);
LexicallyScopedFeatures keyFeaturesOf(const RequestState&);
RootSourceDigest takeRootSourceDigest(VMState&, const SourceCode& rootSource, const Digest256* builtinMetadataDigest);
IdentitySection parseIdentity(const ValidatedBody&, RequestTimer&);
bool holderFits(VMState&, RequestState&, const IdentitySection&, RequestTimer&);

enum class LiveMatch : uint8_t {
    Matched,
    Missed, // stamped with the index token
    Stopped, // invalid material
};
LiveMatch matchLive(VMState&, RequestState&, RequestTimer&, const UnlinkedFunctionExecutable* holder, const UCBRegistry::RecordView&, ValidatedBody&);

// The registry's statistics, and the outcomes that matching and the import count in them.
UCBStatistics& statisticsOf(VMState&);
void countMiss(VMState&, MissReason);
void reportInvalidMaterial(VMState&, InvalidMaterialStep, ASCIILiteral what, const BodyKey&);

} // namespace JSC::JITCache
