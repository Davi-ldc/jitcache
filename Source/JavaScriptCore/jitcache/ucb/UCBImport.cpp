#include "config.h"
#include "jitcache/ucb/UCBImport.h"

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "GlobalExecutable.h"
#include "JSCInlines.h"
#include "JSModuleRecord.h"
#include "ModuleProgramExecutable.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedGlobalCodeBlock.h"
#include "VM.h"
#include "WeakInlines.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/ucb/UCBFeedback.h"
#include "jitcache/ucb/UCBMatch.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBRequestCost.h"
#include "jitcache/ucb/UCBSections.h"
#include "jitcache/ucb/UCBTwins.h"
#include <wtf/StdLibExtras.h>

// The UCB lane's engine. Where the native path would generate, it imports a matching body; where the holder already has
// a UCB that a request recorded, it attaches a pending import; and it records what every request generated or imported
// and every root's identity. What a native decode makes has no record, so nothing here acts on it. Each entry point runs
// on the VM thread holding the API lock and heap access, re-reads the VM's JITCache state first, and counts each outcome
// in the registry's statistics. UCBMatch.cpp decides whether a body fits a request, and UCBRequestCost.h measures each
// request for the bench report.

namespace JSC::JITCache {

namespace UCBImportInternal {

// The holder a record of the request's UCB keeps: the requesting UFE, for a function body in a VM that
// captures bodies without a CodeBlock (VMState::capturesUncompiledBodies), and none otherwise.
static UnlinkedFunctionExecutable* recordedHolder(VMState& vmState, const RequestState& state)
{
    if (state.kind != RequestKind::FunctionBody || !vmState.capturesUncompiledBodies())
        return nullptr;
    return state.functionExecutable;
}

// Whether the body holds an image, as its envelope's highest tier says.
static BodyImage imageOf(const ValidatedBody& body)
{
    return body.highestTier() ? BodyImage::Present : BodyImage::Absent;
}

// Recording stops for good when cache activity turns off; existing records stay. A record the registry
// refuses stays in the caller's temporary and goes after the call, outside the registry's lock.
static bool addRecord(VMState& vmState, UnlinkedCodeBlock& codeBlock, UCBRecord&& record, std::span<const uint8_t> childHolderDigests = { })
{
    if (!vmState.tracksKeys())
        return false;
    UCBOrigin origin = record.origin;
    if (!vmState.registry().recordCodeBlock(codeBlock, WTF::move(record), childHolderDigests))
        return false;
    ++statisticsOf(vmState).records[static_cast<size_t>(origin)];
    return true;
}

static FeedbackSection parseFeedback(const ValidatedBody& body, RequestTimer& timer)
{
    PartScope scope(&timer, RequestPart::FeedbackParse, CostAccount::Counted);
    return FeedbackSection::parse(body.section(SectionKind::UCBFeedback));
}

static ASCIILiteral decodeFailureDescription(CoreDecodeFailure failure)
{
    switch (failure) {
    case CoreDecodeFailure::None:
        return "the core decoded to no code block"_s;
    case CoreDecodeFailure::Malformed:
        return "the core is malformed"_s;
    case CoreDecodeFailure::KindMismatch:
        return "the core's kind is not the one its key implies"_s;
    case CoreDecodeFailure::UnresolvedSymbol:
        return "the core names a symbol this VM lacks"_s;
    case CoreDecodeFailure::InconsistentMapLayout:
        return "the core holds an inconsistent declaration map layout"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "the core is malformed"_s;
}

// The decode checks the kind the key implies, takes the requesting UFE as the holder of a function core and the request's
// source, whose provider the UCB is bound to, and runs under its own DeferGC. The payload pins the body while the decoder
// reads it, and the decoder lets the payload go when the decode returns, since no decoded UFE keeps it.
static UnlinkedCodeBlock* decodeCore(VMState& vmState, RequestState& state, ValidatedBody& body, const BodyKey& key, RequestTimer& timer)
{
    std::span<const uint8_t> core = body.section(SectionKind::UCBCore);
    timer.noteCoreBytes(core.size());
    CachePayload::Destructor releaseBody = [pinned = RefPtr<ValidatedBody> { &body }](const void*) mutable {
        pinned = nullptr;
    };
    Ref<CachedBytecode> payload = CachedBytecode::create(spanConstCast<uint8_t>(core), WTF::move(releaseBody), { });
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* codeBlock = nullptr;
    {
        PartScope scope(&timer, RequestPart::Decode, CostAccount::Apart);
        codeBlock = decodeUnlinkedCodeBlockCore(state.vm, WTF::move(payload), state.source, coreKindFor(key.identityKind()), state.functionExecutable, failure);
    }
    if (!codeBlock || failure != CoreDecodeFailure::None) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Decode, decodeFailureDescription(failure), key);
        return nullptr;
    }
    return codeBlock;
}

static BodyLookup openBodyMeasured(VMState& vmState, const BodyKey& key, RequestTimer& timer)
{
    PartScope scope(&timer, RequestPart::Lookup, CostAccount::Apart);
    return vmState.openBody(key);
}

// The parse fields generation would have recorded. Both kinds run before tryImport returns, so before publication and
// before newCodeBlockFor reads them.
static void restoreParseFields(RequestState& state, UnlinkedCodeBlock& codeBlock, const IdentitySection& identity)
{
    if (state.kind == RequestKind::FunctionBody) {
        // The values generateUnlinkedFunctionCodeBlock records, which newCodeBlockFor copies to the FunctionExecutable.
        // The holder check passed only for a function core, whose identity section carries them.
        ASSERT(identity.functionParseFields);
        const FunctionParseFields& fields = *identity.functionParseFields;
        state.functionExecutable->recordParse(fields.features, fields.lexicallyScopedFeatures, fields.hasCapturedVariables);
        return;
    }
    // As a CodeCache hit does: the executable's parse fields, and the directives back to the provider. The core carries no
    // line starts, so the provider's stay as they are.
    recordParseFromUnlinkedCodeBlock(state.globalExecutable, state.source, uncheckedDowncast<UnlinkedGlobalCodeBlock>(&codeBlock));
}

} // namespace UCBImportInternal

UnlinkedCodeBlock* importBody(RequestState& state)
{
    using namespace UCBImportInternal;

    VM& vm = state.vm;
    VMState* vmState = vm.jitCacheState();
    if (!vmState || !vmState->importsEnabled())
        return nullptr;
    // The parser tests this limit before it recurses, so the native generation that follows throws the stack-overflow
    // RangeError it throws with JITCache off, and a later request at a shallower depth imports. Nothing is computed.
    if (!vm.isSafeToRecurse()) {
        countMiss(*vmState, MissReason::Stack);
        state.missReason = MissReason::Stack;
        return nullptr;
    }
    RequestTimer timer(*vmState);

    // The key. The context waits for the context check, the first step that reads it.
    std::optional<BodyKey> key = requestKeyMeasured(state, state.requestMode, &timer);
    if (!key) {
        countMiss(*vmState, MissReason::NoKey);
        state.missReason = MissReason::NoKey;
        return nullptr;
    }

    // The body at the key.
    uint64_t indexToken = 0;
    std::optional<BodyLookup> lookup;
    {
        PartScope scope(&timer, RequestPart::Lookup, CostAccount::Apart);
        indexToken = vmState->bodyVersion(*key);
        if (indexToken)
            lookup = vmState->openBody(*key);
    }
    if (!indexToken) {
        countMiss(*vmState, MissReason::NoBody);
        state.missReason = MissReason::NoBody;
        return nullptr;
    }
    // Unusable: the integrator has raised the fault.
    if (lookup->kind() == BodyLookup::Kind::Unusable)
        return nullptr;
    // Missing: the index lists a body the artifact no longer holds, as after compact.
    if (lookup->kind() == BodyLookup::Kind::Missing) {
        countMiss(*vmState, MissReason::NoBody);
        state.missReason = MissReason::NoBody;
        state.missedBodyVersion = indexToken;
        return nullptr;
    }
    Ref<ValidatedBody> body = lookup->body().releaseNonNull();
    // The container's checks matched the file's envelope key to the request's.
    ASSERT(body->key() == *key);

    // A miss from here on remembers the index token, which didGenerate's record keeps.
    auto miss = [&](MissReason reason) -> UnlinkedCodeBlock* {
        countMiss(*vmState, reason);
        state.missReason = reason;
        state.missedBodyVersion = indexToken;
        return nullptr;
    };

    // The identity section.
    IdentitySection identity = parseIdentity(body.get(), timer);

    // Its key: a body file is named by its whole key, so a stored key other than its own contradicts the body's envelope.
    if (identity.key != body->key()) {
        reportInvalidMaterial(*vmState, InvalidMaterialStep::Identity, "ucb.identity holds another body's key"_s, body->key());
        return nullptr;
    }
    // Its context.
    if (identity.contextDigest != requestContextMeasured(state, &timer))
        return miss(MissReason::Context);
    // The holder check. A root body's context has already computed the holder digest.
    if (state.kind == RequestKind::FunctionBody && !holderFits(*vmState, state, identity, timer))
        return miss(MissReason::Holder);

    // The feedback section.
    FeedbackSection feedback = parseFeedback(body.get(), timer);

    // The core.
    UnlinkedCodeBlock* codeBlock = decodeCore(*vmState, state, body.get(), *key, timer);
    if (!codeBlock)
        return nullptr;

    // Whether the body holds an image, which decides whether its feedback carries the UCB's value and array copies.
    BodyImage image = imageOf(body.get());

    // The decode gives every butterfly the plain structure and long string elements as plain strings, so each butterfly
    // generation built from atom strings is built again in that form. The part encloses its own DeferGC, so a collection
    // that the deferral's end runs for these allocations is measured with them.
    {
        PartScope scope(&timer, RequestPart::ButterflyRebuild, CostAccount::Counted);
        DeferGC deferGC(vm);
        rebuildAtomStringButterflies(vm, *codeBlock, identity);
    }

    // The feedback. A body with an image allocates the UCB's value and array copies, which its install would otherwise
    // skip, and seeds them; a body without one allocates none and keeps its LLInt counter's progress only up to one
    // invocation short of crossing.
    {
        PartScope scope(&timer, RequestPart::Seeding, CostAccount::Counted);
        seedFeedback(vm, *codeBlock, feedback, image);
    }

    // The parse fields.
    {
        PartScope scope(&timer, RequestPart::ParseFields, CostAccount::Counted);
        restoreParseFields(state, *codeBlock, identity);
    }

    // The amount UnlinkedCodeBlockGenerator::finalize reports for a generated UCB. It stands in for generation, so it
    // counts apart from the bound with the decode, and so does a collection it may start.
    {
        PartScope scope(&timer, RequestPart::ExtraMemory, CostAccount::Apart);
        vm.heap.reportExtraMemoryAllocated(codeBlock, codeBlock->instructions().sizeInBytes() + codeBlock->metadataSizeInBytes());
    }

    // The record. A body without an image leaves no pending import, since its CodeBlocks start in the LLInt as natively;
    // its record is stamped with the index token instead, so a request for the published UCB skips the body that has
    // nothing to attach until the index lists another at the key.
    {
        PartScope scope(&timer, RequestPart::Records, CostAccount::Counted);
        bool hasImage = image == BodyImage::Present;
        UCBRecord record {
            .key = *key,
            .context = recordedContextMeasured(state, &timer),
            .origin = UCBOrigin::Imported,
            .keyFeatures = keyFeaturesOf(state),
            .missedBodyVersion = hasImage ? std::nullopt : std::optional<uint64_t> { indexToken },
            .matchedBodyVersion = body->version(),
            .pendingImport = hasImage ? RefPtr<PendingImport> { PendingImport::create(body.copyRef(), *key, indexToken, PendingImport::Origin::Imported) } : nullptr,
            .holder = recordedHolder(*vmState, state),
        };
        // The children's holder digests the body lists, which the holder check then compares for the child UFEs this decode
        // made. The list is kept only when it has one digest per child.
        std::span<const uint8_t> childHolderDigests;
        if (Options::jitCacheVouchedHolderDigests() && identity.childHolderDigestCount() == codeBlock->numberOfFunctionDecls() + codeBlock->numberOfFunctionExprs())
            childHolderDigests = identity.childHolderDigests;
        bool recorded = addRecord(*vmState, *codeBlock, WTF::move(record), childHolderDigests);
        // The UCB was just decoded, so nothing has recorded it, and nothing since the key has turned cache activity off.
        ASSERT_UNUSED(recorded, recorded);
    }
    // A ConsumerProducer keeps the module's analysis source, which a later capture of the imported UCB encodes.
    keepModuleAnalysisSource(state, *codeBlock);
    ++statisticsOf(*vmState).imports;
    state.settled = true;
    timer.record("import"_s, *key, state.source.length());

#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState->twinReportSink())
        verifyImport(state, *codeBlock, body.get(), *sink);
#endif

    // The native holder publishes the UCB as it publishes a generated one.
    return codeBlock;
}

// The UCB was published before, so nothing here writes to it, its UFE or its executable, apart from the value and array
// copies the attach allocates, as the native baseline plan does on a published UCB.
void attachLive(RequestState& state, UnlinkedCodeBlock& codeBlock)
{
    using namespace UCBImportInternal;

    // Before every check, since the first turns away each VM that does not import, a Producer included, and a Producer
    // must still replace an analysis source that died.
    keepModuleAnalysisSource(state, codeBlock);

    VMState* vmState = state.vm.jitCacheState();
    if (!vmState || !vmState->importsEnabled())
        return;

    // A parked BaselineJITCode wins anyway, and a UCB holds at most one pending import.
#if ENABLE(JIT)
    if (codeBlock.m_unlinkedBaselineCode)
        return;
#endif
    UCBRegistry& registry = vmState->registry();
    std::optional<UCBRegistry::RecordView> record = registry.recordOf(codeBlock);
    if (!record || record->hasPendingImport)
        return;

    // The index token and the stamp, with no digest and no file read.
    uint64_t indexToken = vmState->bodyVersion(record->key);
    if (!indexToken) {
        countMiss(*vmState, MissReason::NoBody);
        return;
    }
    if (record->missedBodyVersion == indexToken) {
        countMiss(*vmState, MissReason::BodyUnchanged);
        return;
    }
    RequestTimer timer(*vmState);

    // The request's key, checked without computing it. A UFE body's record was made for this UFE's identity, this slot's
    // specialization and the UCB's own mode, so its key equals the request's by construction. A global request's strict and
    // with-scope bits must be the record's, which is where a CodeCache hit whose request differs in the with-scope bit
    // misses; that miss belongs to the request rather than to the UCB, so it is not stamped. The rest of a global key follows
    // from the hit: the CodeCache serves the UCB only to a source with its text, code type, script mode, strict bit and
    // code-generation mode, so no source digest is computed.
    if (state.kind != RequestKind::FunctionBody && keyFeaturesOf(state) != record->keyFeatures) {
        countMiss(*vmState, MissReason::RequestKey);
        return;
    }

    // The body, which the pending import needs anyway.
    BodyLookup lookup = openBodyMeasured(*vmState, record->key, timer);
    if (lookup.kind() == BodyLookup::Kind::Unusable)
        return;
    if (lookup.kind() == BodyLookup::Kind::Missing) {
        countMiss(*vmState, MissReason::NoBody);
        registry.setMissedBodyVersion(codeBlock, indexToken);
        return;
    }
    Ref<ValidatedBody> body = lookup.body().releaseNonNull();
    // A body without an image has nothing to attach: what it carries seeds a UCB only before publication. Requests for the
    // UCB skip it until the index lists another body at the key.
    if (imageOf(body.get()) == BodyImage::Absent) {
        registry.setMissedBodyVersion(codeBlock, indexToken);
        return;
    }
    const UnlinkedFunctionExecutable* holder = state.functionExecutable;

    // At the commit identifier the UCB was matched at, it passed every check of the match against this very body, and
    // nothing the match read can have changed since: the record's context and the holder UFE are immutable, and the body's
    // bytes never change.
    if (record->matchedBodyVersion != body->version()) {
        switch (matchLive(*vmState, state, timer, holder, *record, body.get())) {
        case LiveMatch::Matched:
            break;
        case LiveMatch::Missed:
            registry.setMissedBodyVersion(codeBlock, indexToken);
            return;
        case LiveMatch::Stopped:
            return;
        }
    }

    // The body has an image, whose install skips the baseline plan that would allocate the UCB's value and array copies, so
    // the attach allocates them, as the plan does on a published UCB. They stay as the native path leaves them, empty or
    // already folded into; the body's feedback seeds a UCB only before publication.
    {
        PartScope scope(&timer, RequestPart::Seeding, CostAccount::Counted);
        codeBlock.ensureValueAndArrayProfiles();
    }

    // The pending import.
    bool attached = [&] {
        PartScope scope(&timer, RequestPart::Records, CostAccount::Counted);
        return registry.attachPendingImport(codeBlock, PendingImport::create(body.copyRef(), record->key, indexToken, PendingImport::Origin::Reused));
    }();
    // The record had no pending import above, and nothing since has attached one.
    if (!attached)
        return;
    ++statisticsOf(*vmState).attaches;
    state.settled = true;
    timer.record("attach"_s, record->key, state.source.length());

#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState->twinReportSink())
        verifyMatched(codeBlock, holder, body.get(), *sink);
#endif
}

// What the request generated, under the request's key.
void recordGenerated(RequestState& state, UnlinkedCodeBlock& codeBlock)
{
    using namespace UCBImportInternal;

    VMState* vmState = state.vm.jitCacheState();
    if (!vmState || !vmState->tracksKeys())
        return;
    // A request generates in its own mode, so the key its import attempt computed is this UCB's.
    ASSERT(codeBlock.codeGenerationMode() == state.requestMode);
    std::optional<BodyKey> key = requestKeyMeasured(state, state.requestMode, nullptr);
    if (!key)
        return;
    UCBRecord record {
        .key = *key,
        .context = recordedContextMeasured(state, nullptr),
        .origin = UCBOrigin::Generated,
        .keyFeatures = keyFeaturesOf(state),
        .missedBodyVersion = state.missedBodyVersion,
        .matchedBodyVersion = std::nullopt,
        .pendingImport = nullptr,
        .holder = recordedHolder(*vmState, state),
    };
    if (addRecord(*vmState, codeBlock, WTF::move(record)))
        keepModuleAnalysisSource(state, codeBlock);
    state.settled = true;
}

// The record the module's analysis parse built stays the UCB's analysis source only through a weak handle, which goes
// with the UCB's record. Only that parse's record is a source: a record Bun built from module info, the printer's or this
// cache's, has a provider of type BunTranspiledModule, and an executable that vm.SourceTextModule or a test creates has no
// linker.
void keepModuleAnalysisSource(RequestState& state, UnlinkedCodeBlock& codeBlock)
{
    if (state.kind != RequestKind::Module)
        return;
    VMState* vmState = state.vm.jitCacheState();
    if (!vmState || !vmState->productionActive())
        return;
    SourceProvider* provider = state.source.provider();
    if (!provider || provider->sourceType() != SourceProviderSourceType::Module)
        return;
    auto* executable = dynamicDowncast<ModuleProgramExecutable>(state.globalExecutable);
    JSModuleRecord* linker = executable ? executable->linker() : nullptr;
    if (!linker)
        return;
    UCBRegistry& registry = vmState->registry();
    if (!registry.needsModuleAnalysisSource(codeBlock))
        return;
    // Making a Weak counts as a heap allocation, which can run incremental marking, so it is made with the registry's lock
    // released; setModuleAnalysisSource destroys a handle it does not keep after unlocking.
    registry.setModuleAnalysisSource(codeBlock, Weak<JSModuleRecord> { linker });
}

// The UFE does not keep the root source its identity covers, so the identity digest runs here, at the root's creation.
// No body is looked up or opened: global object setup creates many builtins eagerly, and each body is looked up at its
// own first request. The root UFE's own singleton bit stays native.
void recordRoot(VM& vm, UnlinkedFunctionExecutable& executable, IdentityKind kind, const SourceCode& rootSource, LexicallyScopedFeatures lexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest)
{
    VMState* vmState = vm.jitCacheState();
    if (!vmState || !vmState->tracksKeys())
        return;
    ASSERT(kind == IdentityKind::FunctionConstructor || kind == IdentityKind::Builtin);
    RootSourceDigest digest = takeRootSourceDigest(*vmState, rootSource, builtinMetadataDigest);
    // The generator's digest belongs to the build, which debug builds check here.
    ASSERT(!builtinMetadataDigest || *builtinMetadataDigest == sourceDigest(rootSource.view()));
    RootIdentity identity {
        .kind = kind,
        .identityDigest = rootIdentityDigest(kind, digest.digest, lexicallyScopedFeatures, parameterEnd),
    };
    vmState->registry().recordRootExecutable(executable, identity);
}

} // namespace JSC::JITCache
