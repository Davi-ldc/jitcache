#include "config.h"
#include "UCBCapture.h"

#include "CachedTypes.h"
#include "CodeBlock.h"
#include "FunctionExecutable.h"
#include "JITCacheSHA256.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "Options.h"
#include "ProducerBudget.h"
#include "UCBFeedback.h"
#include "UCBSections.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedMetadataTable.h"
#include "VM.h"
#include <algorithm>
#include <utility>
#include <wtf/Noncopyable.h>

namespace JSC::JITCache {

namespace UCBCaptureInternal {

// The codec's charger over the producer budget. The codec releases every charge it makes except a returned payload's size,
// which buildSections takes over.
class UCBCaptureEncodingBudget final : public CoreEncodingBudget {
    WTF_MAKE_NONCOPYABLE(UCBCaptureEncodingBudget);

public:
    explicit UCBCaptureEncodingBudget(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    bool charge(size_t bytes) final { return m_budget.tryCharge(bytes); }
    void release(size_t bytes) final { m_budget.release(bytes); }

private:
    ProducerBudget& m_budget;
};

// The bytes one buildSections call holds charged. Whatever the call has not handed to the UCBSections it returns goes
// back to the budget when the call returns. Declared before the buffers it pays for, so they are freed before it releases.
class UCBCaptureCharges {
    WTF_MAKE_NONCOPYABLE(UCBCaptureCharges);

public:
    explicit UCBCaptureCharges(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~UCBCaptureCharges()
    {
        if (m_bytes)
            m_budget.release(m_bytes);
    }

    // A refusal charges nothing; what the call charged before stays held until the call returns.
    [[nodiscard]] bool tryCharge(size_t bytes)
    {
        if (!m_budget.tryCharge(bytes))
            return false;
        m_bytes += bytes;
        return true;
    }

    // Bytes someone else charged that this call now owns: the size of a payload the codec returned.
    void adopt(size_t bytes) { m_bytes += bytes; }

    // After freeing a buffer that does not travel in the sections.
    void release(size_t bytes)
    {
        ASSERT(bytes <= m_bytes);
        m_budget.release(bytes);
        m_bytes -= bytes;
    }

    size_t handOff() { return std::exchange(m_bytes, 0); }

private:
    ProducerBudget& m_budget;
    size_t m_bytes { 0 };
};

// ceil(N / 8): the size of one constant map of ucb.identity.
static size_t ucbConstantMapSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

// A strict check of what the capture builds from: every child UFE of the UCB's two tables has the identity (key, table,
// index) its parent's record gave it.
static bool ucbChildrenHaveIdentities(UCBRegistry& registry, UnlinkedCodeBlock& ucb, const BodyKey& key)
{
    auto hasIdentity = [&](const UnlinkedFunctionExecutable* child, ChildTable table, size_t index) {
        if (!child)
            return false;
        // The copy's references drop here, outside the registry lock, and never the last one: the child's entry holds one.
        std::optional<ExecutableIdentity> identity = registry.identityOf(*child);
        if (!identity)
            return false;
        auto* childIdentity = std::get_if<ChildIdentity>(&*identity);
        return childIdentity && childIdentity->parent && childIdentity->parent->key() == key && childIdentity->table == table && childIdentity->index == index;
    };

    size_t declarationCount = ucb.numberOfFunctionDecls();
    for (size_t index = 0; index < declarationCount; ++index) {
        if (!hasIdentity(ucb.functionDecl(static_cast<int>(index)), ChildTable::Declarations, index))
            return false;
    }
    size_t expressionCount = ucb.numberOfFunctionExprs();
    for (size_t index = 0; index < expressionCount; ++index) {
        if (!hasIdentity(ucb.functionExpr(static_cast<int>(index)), ChildTable::Expressions, index))
            return false;
    }
    return true;
}

// The other strict check: the UCB's value profiles are its argument profiles followed by its metadata's, the positional
// pairing the CB lane's summary relies on.
static bool ucbValueProfileCountsAgree(UnlinkedCodeBlock& ucb)
{
    return static_cast<uint64_t>(ucb.numberOfValueProfiles()) == static_cast<uint64_t>(ucb.numParameters()) + ucb.metadata().numValueProfiles();
}

} // namespace UCBCaptureInternal

UCBSections::UCBSections(Vector<uint8_t>&& identity, Ref<CachedBytecode>&& core, Vector<uint8_t>&& feedback, Ref<ProducerBudget>&& budget, size_t chargedBytes)
    : identity(WTF::move(identity))
    , core(WTF::move(core))
    , feedback(WTF::move(feedback))
    , budget(WTF::move(budget))
    , chargedBytes(chargedBytes)
{
}

UCBSections::UCBSections(UCBSections&& other)
    : identity(WTF::move(other.identity))
    , core(WTF::move(other.core))
    , feedback(WTF::move(other.feedback))
    , budget(WTF::move(other.budget))
    , chargedBytes(std::exchange(other.chargedBytes, 0))
{
}

UCBSections::~UCBSections()
{
    if (budget)
        budget->release(chargedBytes);
}

std::optional<UCBRegistry::RecordView> captureRecord(VM& vm, const UnlinkedCodeBlock& ucb)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return std::nullopt;
    auto record = state->registry().recordOf(ucb);
    if (!record)
        return std::nullopt;
    if (record->key.identityKind() == IdentityKind::DirectEval) {
        // A direct eval's sets lived only while its request ran, so a record that kept no digest cannot write a context.
        auto* context = std::get_if<DirectEvalContext>(&record->context);
        if (!context || !context->digest)
            return std::nullopt;
    }
    return record;
}

uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock& ucb)
{
    // thresholdForJIT scales the option by the UCB's didOptimize history. JITCache fixes the option at its positive default,
    // so the clamp only keeps a negative answer from wrapping.
    return static_cast<uint32_t>(std::max<int32_t>(ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()), 0));
}

const UnlinkedFunctionExecutable* holderOf(const CodeBlock& codeBlock)
{
    if (codeBlock.codeType() != FunctionCode)
        return nullptr;
    return uncheckedDowncast<FunctionExecutable>(codeBlock.ownerExecutable())->unlinkedExecutable();
}

std::expected<UCBSections, UCBCaptureFailure> buildSections(VM& vm, UnlinkedCodeBlock& ucb, const UnlinkedFunctionExecutable* holder, ProducerBudget& budget)
{
    using namespace UCBCaptureInternal;

    ASSERT(vm.currentThreadIsHoldingAPILock());
    VMState* state = vm.jitCacheState();
    RELEASE_ASSERT(state);
    UCBRegistry& registry = state->registry();

    // The glue calls this only for a candidate whose captureRecord answered in the same pause, and a record lives as long as
    // its UCB, which a CodeBlock or, at delta's walk of the registry, the last collection's marks keep alive.
    std::optional<UCBRegistry::RecordView> record = captureRecord(vm, ucb);
    RELEASE_ASSERT(record);
    IdentityKind identityKind = record->key.identityKind();
    UnlinkedCodeBlockCoreKind coreKind = coreKindFor(identityKind);

    // Function code: the UFE whose request produced the UCB. It is the core's holder, against whose chain the core writes
    // its children's TDZ chains, and gives the identity section its holder digest and parse fields.
    ASSERT(!!holder == (coreKind == UnlinkedCodeBlockCoreKind::Function));

    // The strict checks. They check what the capture builds from the producer's own state, so a failure is a recording
    // fault.
    if (state->strict()) {
        if (!ucbChildrenHaveIdentities(registry, ucb, record->key) || !ucbValueProfileCountsAgree(ucb))
            return std::unexpected(UCBCaptureFailure::StrictCheckFailed);
    }

    UCBCaptureCharges charges(budget);

    // The feedback section. The exit sites cannot change between the count and the write.
    FeedbackCounts counts = liveFeedbackCounts(ucb);
    size_t feedbackSize = feedbackSectionSize(counts);
    if (!charges.tryCharge(feedbackSize))
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    Vector<uint8_t> feedback(feedbackSize);
    writeFeedbackSection(feedback.mutableSpan(), ucb, counts);

    // The core. The codec charges as it allocates and, on success, leaves exactly the payload's size charged.
    UCBCaptureEncodingBudget encodingBudget(budget);
    CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, ucb, holder, &encodingBudget, encodeFailure);
    if (!core) {
        ASSERT(encodeFailure == CoreEncodeFailure::BudgetRefused);
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    }
    charges.adopt(core->size());

    Digest256 coreDigest = SHA256::hash(core->span());

    // The digests, before the identity section's charge. The holder digest charges what it allocates through the same
    // adapter and releases it once hashed; the environment digests it keeps serve every role and stay uncharged.
    UCBStatistics& statistics = registry.statistics();
    std::optional<Digest256> holderDigestValue;
    std::optional<FunctionParseFields> parseFields;
    if (holder) {
        unsigned environmentsDigested = 0;
        holderDigestValue = holderDigest(vm, *holder, &encodingBudget, &environmentsDigested);
        // An environment digested before a refusal keeps its digest, so it counts either way.
        statistics.tdzEnvironmentDigests += environmentsDigested;
        if (!holderDigestValue)
            return std::unexpected(UCBCaptureFailure::BudgetExceeded);
        ++statistics.holderDigests;
        parseFields = FunctionParseFields { holder->features(), holder->lexicallyScopedFeatures(), holder->hasCapturedVariables() };
    }
    // A root UFE body's context covers its UFE's holder digest; a child's context does not.
    std::optional<Digest256> rootHolderDigest;
    if (identityKind == IdentityKind::FunctionConstructor || identityKind == IdentityKind::Builtin)
        rootHolderDigest = holderDigestValue;
    std::optional<Digest256> contextDigest = contextDigestOf(identityKind, record->context, rootHolderDigest);
    // captureRecord turned away a direct eval without a digest, and a root body has its holder digest.
    RELEASE_ASSERT(contextDigest);
    // A direct eval's digest is the one its record kept; every other context is computed here.
    if (identityKind != IdentityKind::DirectEval)
        ++statistics.contextDigests;

    // The child holder digests: each child UFE's holder digest, from this producer's own UFE, so that a consumer
    // that imports this core can answer its children's holder checks without encoding the UFEs its decode made. A refused charge
    // leaves that entry zero, which a consumer reads as no digest.
    size_t declarationCount = ucb.numberOfFunctionDecls();
    size_t childCount = declarationCount + ucb.numberOfFunctionExprs();
    size_t childDigestsBytes = childCount * sizeof(Digest256);
    if (childDigestsBytes && !charges.tryCharge(childDigestsBytes))
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    Vector<Digest256> childHolderDigests(childCount);
    for (size_t i = 0; i < childCount; ++i) {
        UnlinkedFunctionExecutable* child = i < declarationCount ? ucb.functionDecl(i) : ucb.functionExpr(i - declarationCount);
        unsigned environmentsDigested = 0;
        std::optional<Digest256> digest = holderDigest(vm, *child, &encodingBudget, &environmentsDigested);
        statistics.tdzEnvironmentDigests += environmentsDigested;
        childHolderDigests[i] = digest.value_or(Digest256 { });
        if (digest)
            ++statistics.childHolderDigests;
    }

    // The identity section.
    uint32_t constantCount = static_cast<uint32_t>(ucb.constantRegisters().size());
    size_t identitySize = identitySectionSize(constantCount, childCount);
    if (!charges.tryCharge(identitySize))
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    Vector<uint8_t> identity(identitySize);

    // Provenance. A generated or imported UCB has generation's form, since an import takes only a body of provenance
    // Generated and rebuilds its butterflies. A decoded one is the embedder's, unless its record keeps the butterfly map of
    // the Generated body it was last seeded or attached from: its core matched that body's coreDigest and a core never
    // changes, so it is still that body's UCB, written with the kept map in place of its own plain butterflies. Only a
    // Decoded record keeps a map.
    CoreProvenance provenance = record->origin == UCBOrigin::Decoded ? CoreProvenance::EmbedderDecoded : CoreProvenance::Generated;
    size_t keptMapSize = 0;
    {
        Vector<uint8_t> keptButterflyMap;
        bool keepsButterflyMap = false;
        if (record->origin == UCBOrigin::Decoded) {
            keptMapSize = ucbConstantMapSize(constantCount);
            if (!charges.tryCharge(keptMapSize))
                return std::unexpected(UCBCaptureFailure::BudgetExceeded);
            keptButterflyMap = Vector<uint8_t>(keptMapSize);
            keepsButterflyMap = registry.copyGeneratedButterflyMap(ucb, keptButterflyMap.mutableSpan());
            if (keepsButterflyMap)
                provenance = CoreProvenance::Generated;
        }
        std::optional<std::span<const uint8_t>> keptMap;
        if (keepsButterflyMap)
            keptMap = keptButterflyMap.span();
        writeIdentitySection(identity.mutableSpan(), vm, record->key, *contextDigest, provenance, coreDigest, holderDigestValue, coreKind, parseFields, ucb, keptMap, childHolderDigests.span());
    }
    // The kept map's copy is freed with the scope above, and the digests now live in the section; neither copy travels.
    if (keptMapSize)
        charges.release(keptMapSize);
    childHolderDigests.clear();
    if (childDigestsBytes)
        charges.release(childDigestsBytes);

    size_t chargedBytes = charges.handOff();
    return UCBSections { WTF::move(identity), core.releaseNonNull(), WTF::move(feedback), Ref<ProducerBudget> { budget }, chargedBytes };
}

} // namespace JSC::JITCache
