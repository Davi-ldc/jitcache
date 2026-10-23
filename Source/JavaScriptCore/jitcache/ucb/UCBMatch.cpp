#include "config.h"
#include "jitcache/ucb/UCBMatch.h"

#include "EvalExecutable.h"
#include "GlobalExecutable.h"
#include "JSCInlines.h"
#include "SourceCode.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/ucb/UCBRequestCost.h"
#include "jitcache/ucb/UCBSections.h"
#include "jitcache/ucb/UCBTwins.h"
#include <array>
#include <wtf/text/MakeString.h>

// The request's key, context and holder digests, and the checks that hold a body's identity section to them, for an
// import or for a live UCB's record. Each check counts its outcome in the registry's statistics.

namespace JSC::JITCache {

namespace UCBMatchInternal {

static ASCIILiteral stepName(InvalidMaterialStep step)
{
    switch (step) {
    case InvalidMaterialStep::Identity:
        return "ucb.identity"_s;
    case InvalidMaterialStep::Decode:
        return "ucb.decode"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "ucb.identity"_s;
}

static bool isGlobalRequest(const RequestState& state)
{
    return state.kind == RequestKind::Program || state.kind == RequestKind::Module || state.kind == RequestKind::IndirectEval;
}

static IdentityKind globalIdentityKind(RequestKind kind)
{
    switch (kind) {
    case RequestKind::Program:
        return IdentityKind::Program;
    case RequestKind::Module:
        return IdentityKind::Module;
    case RequestKind::IndirectEval:
        return IdentityKind::IndirectEval;
    case RequestKind::FunctionBody:
    case RequestKind::DirectEval:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return IdentityKind::Program;
}

static bool isRootFunctionKind(IdentityKind kind)
{
    return kind == IdentityKind::FunctionConstructor || kind == IdentityKind::Builtin;
}

// The key the request's inputs give with `keyMode`, or none, with the source digest it rests on.
static std::optional<BodyKey> computeRequestKey(VMState& vmState, RequestState& state, OptionSet<CodeGenerationMode> keyMode)
{
    switch (state.kind) {
    case RequestKind::Program:
    case RequestKind::Module:
    case RequestKind::IndirectEval: {
        RootSourceDigest digest = takeRootSourceDigest(vmState, state.source, nullptr);
        state.sourceDigest = digest.digest;
        IdentityKind kind = globalIdentityKind(state.kind);
        return BodyKey::make(kind, CodeSpecializationKind::CodeForCall, keyMode, rootIdentityDigest(kind, digest.digest, state.requestFeatures, std::nullopt));
    }
    case RequestKind::FunctionBody: {
        // A UFE without an identity misses forever.
        std::optional<ExecutableIdentity> identity = vmState.registry().identityOf(*state.functionExecutable);
        if (!identity)
            return std::nullopt;
        return WTF::switchOn(*identity,
            [&](const ChildIdentity& child) {
                state.vouchedHolderDigest = child.parent->childHolderDigest(child.table, child.index);
                // Computed at the body's request rather than when its parent was recorded, since most children never run.
                return BodyKey::make(IdentityKind::Child, state.specialization, keyMode, childIdentityDigest(child.parent->key(), child.table, child.index));
            },
            [&](const RootIdentity& root) {
                return BodyKey::make(root.kind, state.specialization, keyMode, root.identityDigest);
            });
    }
    case RequestKind::DirectEval: {
        // A direct eval without a site, or whose caller UCB has no record, has no key. Its key ends in its own text's
        // digest, which rests on no provider and counts in neither digest statistic.
        const DirectEvalSite* site = state.site;
        if (!site || !site->callerUnlinkedCodeBlock)
            return std::nullopt;
        std::optional<BodyKey> callerKey = vmState.registry().keyOf(*site->callerUnlinkedCodeBlock);
        if (!callerKey)
            return std::nullopt;
        Digest256 textDigest = sourceDigest(state.source.view());
        state.sourceDigest = textDigest;
        return BodyKey::make(IdentityKind::DirectEval, CodeSpecializationKind::CodeForCall, keyMode, directEvalIdentityDigest(*callerKey, site->bytecodeIndex, textDigest));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
    return std::nullopt;
}

// The holder check and a root UFE body's context read the requesting UFE's holder digest, computed once per request.
// `account` is the caller's: a child's holder check counts toward the bound and a root's context does not, and the first
// caller is the one that computes.
static const Digest256& requestHolderDigestMeasured(RequestState& state, RequestTimer* timer, CostAccount account)
{
    if (state.holderDigest)
        return *state.holderDigest;
    RELEASE_ASSERT(state.kind == RequestKind::FunctionBody && state.functionExecutable);
    VMState* vmState = state.vm.jitCacheState();
    RELEASE_ASSERT(vmState);
    UnlinkedFunctionExecutable& executable = *state.functionExecutable;
    unsigned environmentsDigested = 0;
    // A child's holder check counts beyond its environments' first digests, which count apart. While a report is open,
    // the chain digest therefore runs alone first, so each environment's first digest lands in the apart total and the
    // holder digest below finds them all kept. Without a report this costs nothing.
    if (account == CostAccount::Counted && timer && timer->isMeasuring()) {
        if (RefPtr chain = executable.parentScopeTDZVariables()) {
            PartScope scope(timer, RequestPart::EnvironmentDigests, CostAccount::Apart);
            std::optional<std::array<uint8_t, 32>> chainDigest = tdzChainDigest(chain.get(), nullptr, environmentsDigested);
            ASSERT_UNUSED(chainDigest, chainDigest);
        }
    }
    std::optional<Digest256> digest;
    {
        PartScope scope(timer, account == CostAccount::Counted ? RequestPart::HolderDigest : RequestPart::RootHolderDigest, account);
        digest = holderDigest(state.vm, executable, nullptr, &environmentsDigested);
    }
    // Requests pass no budget, so nothing refuses.
    RELEASE_ASSERT(digest);
    UCBStatistics& statistics = statisticsOf(*vmState);
    ++statistics.holderDigests;
    statistics.tdzEnvironmentDigests += environmentsDigested;
    state.holderDigest = *digest;
    return *state.holderDigest;
}

// The inputs a global's or a UFE body's context digest is computed from, which its record keeps. No context holds a line
// or column: a UCB holds none, and deriving one would build the provider's line-start table from its whole text.
static RecordedContext contextInputsOf(const RequestState& state)
{
    auto providerOffset = static_cast<unsigned>(state.source.startOffset());
    if (state.kind == RequestKind::FunctionBody)
        return BodyContextInputs { .providerOffset = providerOffset };
    ASSERT(isGlobalRequest(state));
    return GlobalContextInputs {
        .providerOffset = providerOffset,
        .scriptMode = state.scriptMode,
        .derivedContextType = state.globalExecutable->derivedContextType(),
        .evalContextType = state.evalContextType,
        .isArrowFunctionContext = state.globalExecutable->isArrowFunctionContext(),
    };
}

// A direct eval's context: the snapshot is the call site's features, and the TDZ and private-name sets are the ones
// JSC::eval passed, which live until DirectEvalExecutable::create returns.
static Digest256 directEvalContextOf(const RequestState& state)
{
    auto* executable = uncheckedDowncast<EvalExecutable>(state.globalExecutable);
    TDZEnvironment noVariables;
    PrivateNameEnvironment noPrivateNames;
    return directEvalContextDigest(static_cast<unsigned>(state.source.startOffset()), state.requestFeatures, executable->derivedContextType(),
        executable->needsClassFieldInitializer(), executable->privateBrandRequirement(), executable->isArrowFunctionContext(), executable->isInsideOrdinaryFunction(),
        state.evalContextType, state.variablesUnderTDZ ? *state.variablesUnderTDZ : noVariables, state.privateNameEnvironment ? *state.privateNameEnvironment : noPrivateNames);
}

// A live UCB's context, from the inputs its record keeps; the request's own context does not enter.
static std::optional<Digest256> recordContextDigest(VMState& vmState, RequestState& state, const BodyKey& key, const RecordedContext& context, RequestTimer& timer)
{
    IdentityKind kind = key.identityKind();
    std::optional<Digest256> rootHolderDigest;
    if (isRootFunctionKind(kind))
        rootHolderDigest = requestHolderDigestMeasured(state, &timer, CostAccount::Apart);
    std::optional<Digest256> digest;
    {
        PartScope scope(&timer, RequestPart::ContextDigest, CostAccount::Apart);
        digest = contextDigestOf(kind, context, rootHolderDigest);
    }
    // A direct eval's record keeps its digest, so only the other kinds compute one here.
    if (kind != IdentityKind::DirectEval)
        ++statisticsOf(vmState).contextDigests;
    return digest;
}

} // namespace UCBMatchInternal

UCBStatistics& statisticsOf(VMState& vmState)
{
    return vmState.registry().statistics();
}

void countMiss(VMState& vmState, MissReason reason)
{
    ++statisticsOf(vmState).misses[static_cast<size_t>(reason)];
}

// Turns cache activity off for good and names the step in status. The caller then returns, and the native path
// goes on as if JITCache were off; a UCB the import decoded is unreachable and the collector frees it.
void reportInvalidMaterial(VMState& vmState, InvalidMaterialStep step, ASCIILiteral what, const BodyKey& key)
{
    using namespace UCBMatchInternal;

    ++statisticsOf(vmState).invalidMaterial[static_cast<size_t>(step)];
    vmState.raiseInvalidMaterial(stepName(step), makeString(what, "; body "_s, bodyKeyHex(key)));
}

// The strict and with-scope bits a global root's key carries, which its record keeps.
LexicallyScopedFeatures keyFeaturesOf(const RequestState& state)
{
    using namespace UCBMatchInternal;

    if (!isGlobalRequest(state))
        return NoLexicallyScopedFeatures;
    return static_cast<LexicallyScopedFeatures>(state.requestFeatures & (StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature));
}

// A root's source digest, counted in the statistics. In twins builds with a report open, the twin check holds a supplied
// one to its text.
RootSourceDigest takeRootSourceDigest(VMState& vmState, const SourceCode& rootSource, const Digest256* builtinMetadataDigest)
{
    RootSourceDigest digest = rootSourceDigest(rootSource, builtinMetadataDigest);
    UCBStatistics& statistics = statisticsOf(vmState);
    if (digest.origin == SourceDigestOrigin::Computed) {
        ++statistics.sourceDigests;
        return digest;
    }
    ++statistics.suppliedSourceDigests;
#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState.twinReportSink())
        verifySuppliedDigest(rootSource, digest, *sink);
#endif
    return digest;
}

std::optional<BodyKey> requestKeyMeasured(RequestState& state, OptionSet<CodeGenerationMode> keyMode, RequestTimer* timer)
{
    using namespace UCBMatchInternal;

    if (state.key) {
        // The UCB a request acts on fixes its key mode, so a request asks for one mode only.
        ASSERT(!*state.key || (*state.key)->codeGenerationMode() == keyMode);
        return *state.key;
    }
    VMState* vmState = state.vm.jitCacheState();
    RELEASE_ASSERT(vmState);
    std::optional<BodyKey> key;
    {
        PartScope scope(timer, RequestPart::KeyDigest, CostAccount::Apart);
        key = computeRequestKey(*vmState, state, keyMode);
    }
    state.key.emplace(key);
    return key;
}

// The request's context, computed at most once and only once a body at its key has passed the key comparison.
const Digest256& requestContextMeasured(RequestState& state, RequestTimer* timer)
{
    using namespace UCBMatchInternal;

    if (state.context)
        return *state.context;
    RELEASE_ASSERT(state.key && *state.key);
    VMState* vmState = state.vm.jitCacheState();
    RELEASE_ASSERT(vmState);
    IdentityKind kind = (*state.key)->identityKind();
    if (kind == IdentityKind::DirectEval) {
        PartScope scope(timer, RequestPart::ContextDigest, CostAccount::Apart);
        state.context = directEvalContextOf(state);
    } else {
        // A root UFE body's context covers its UFE's holder digest, which the holder check then reuses.
        std::optional<Digest256> rootHolderDigest;
        if (isRootFunctionKind(kind))
            rootHolderDigest = requestHolderDigestMeasured(state, timer, CostAccount::Apart);
        PartScope scope(timer, RequestPart::ContextDigest, CostAccount::Apart);
        state.context = contextDigestOf(kind, contextInputsOf(state), rootHolderDigest);
    }
    RELEASE_ASSERT(state.context);
    ++statisticsOf(*vmState).contextDigests;
    return *state.context;
}

// What a record keeps of the request's context. A direct eval's sets exist only while its request runs, so
// its record keeps the digest: the one an import computed or, in a VM whose production is active, one computed now.
RecordedContext recordedContextMeasured(RequestState& state, RequestTimer* timer)
{
    using namespace UCBMatchInternal;

    if (state.kind != RequestKind::DirectEval)
        return contextInputsOf(state);
    if (!state.context) {
        VMState* vmState = state.vm.jitCacheState();
        if (vmState && vmState->productionActive())
            requestContextMeasured(state, timer);
    }
    return DirectEvalContext { .digest = state.context };
}

IdentitySection parseIdentity(const ValidatedBody& body, RequestTimer& timer)
{
    PartScope scope(&timer, RequestPart::IdentityParse, CostAccount::Counted);
    return parseIdentitySection(body.section(SectionKind::UCBIdentity));
}

// The holder check: the requesting UFE's holder digest equals the identity section's. A section with none, whose core kind
// is not a function's, fails it as well.
//
// A UFE that an imported parent's decode made has the digest the parent's body lists for it, when the body lists one: the
// parent's capture computed it from the producer's UFE that the core encodes, the decode reproduces that UFE's descriptor,
// and its TDZ chain extends a start chain whose content the parent's own holder check or context matched. The check then
// compares that listed digest and encodes nothing.
bool holderFits(VMState& vmState, RequestState& state, const IdentitySection& identity, RequestTimer& timer)
{
    using namespace UCBMatchInternal;

    if (!identity.holderDigest)
        return false;
    std::optional<Digest256> vouched = Options::jitCacheVouchedHolderDigests() ? state.vouchedHolderDigest : std::nullopt;
    if (vouched) {
        ++statisticsOf(vmState).vouchedHolderChecks;
        PartScope scope(&timer, RequestPart::HolderDigest, CostAccount::Counted);
        return *identity.holderDigest == *vouched;
    }
    return *identity.holderDigest == requestHolderDigestMeasured(state, &timer, CostAccount::Counted);
}

// Whether a live UCB, which the native path generated or the lane imported, is the UCB the body opened at its record's
// key describes. It matches by its generation inputs: the key, the context its record's inputs give and, for a function
// body, its holder UFE; from equal inputs the engine's generation makes the producer's UCB. holder is the UFE whose slot
// holds a function UCB, and null for any other. Counts each miss; the caller stamps a Missed result.
LiveMatch matchLive(VMState& vmState, RequestState& state, RequestTimer& timer, const UnlinkedFunctionExecutable* holder, const UCBRegistry::RecordView& record,
    ValidatedBody& body)
{
    using namespace UCBMatchInternal;

    // The identity section: its key, then the context.
    IdentitySection identity = parseIdentity(body, timer);
    if (identity.key != body.key()) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Identity, "ucb.identity holds another body's key"_s, body.key());
        return LiveMatch::Stopped;
    }
    std::optional<Digest256> context = recordContextDigest(vmState, state, record.key, record.context, timer);
    if (!context || *context != identity.contextDigest) {
        countMiss(vmState, MissReason::Context);
        return LiveMatch::Missed;
    }

    // The holder check.
    if (holder && !holderFits(vmState, state, identity, timer)) {
        countMiss(vmState, MissReason::Holder);
        return LiveMatch::Missed;
    }

    return LiveMatch::Matched;
}

std::optional<BodyKey> requestKey(RequestState& state, OptionSet<CodeGenerationMode> keyMode)
{
    return requestKeyMeasured(state, keyMode, nullptr);
}

const Digest256& requestContext(RequestState& state)
{
    return requestContextMeasured(state, nullptr);
}

const Digest256& requestHolderDigest(RequestState& state)
{
    return UCBMatchInternal::requestHolderDigestMeasured(state, nullptr, CostAccount::Counted);
}

RecordedContext recordedContext(RequestState& state)
{
    return recordedContextMeasured(state, nullptr);
}

} // namespace JSC::JITCache
