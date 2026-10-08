/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

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

// The codec's charger over the producer budget (SPEC-ucb.md section 8.2, step 3; SPEC-ucb.codec.md, E8). The codec
// releases every charge it makes except a returned payload's size, which buildSections takes over.
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

// ceil(N / 8): the size of one constant map of ucb.identity (SPEC-ucb.md section 4.2).
static size_t ucbConstantMapSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

// SC1: every child UFE of the UCB's two tables has the identity (key, table, index) its parent's record gave it (I8).
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

// SC2: the UCB's value profiles are its argument profiles followed by its metadata's, the positional pairing the CB
// lane's summary relies on.
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
    // thresholdForJIT scales the option by the UCB's didOptimize history; options.md fixes the option at its positive
    // default, so the clamp only keeps a negative answer from wrapping.
    return static_cast<uint32_t>(std::max<int32_t>(ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()), 0));
}

std::expected<UCBSections, UCBCaptureFailure> buildSections(VM& vm, const CodeBlock& codeBlock, ProducerBudget& budget)
{
    using namespace UCBCaptureInternal;

    ASSERT(vm.currentThreadIsHoldingAPILock());
    VMState* state = vm.jitCacheState();
    RELEASE_ASSERT(state);
    UCBRegistry& registry = state->registry();
    UnlinkedCodeBlock& ucb = *codeBlock.unlinkedCodeBlock();

    // The glue calls this only for a candidate whose captureRecord answered in the same pause, and a record lives as long as
    // its UCB, which the CodeBlock keeps alive (I1, I3).
    std::optional<UCBRegistry::RecordView> record = captureRecord(vm, ucb);
    RELEASE_ASSERT(record);
    IdentityKind identityKind = record->key.identityKind();
    UnlinkedCodeBlockCoreKind coreKind = coreKindFor(identityKind);

    // Function code: the UFE whose slot holds the UCB, whose request produced it. It is the core's holder (codec E14) and
    // gives the identity section its holder digest and parse fields.
    const UnlinkedFunctionExecutable* holder = nullptr;
    if (codeBlock.codeType() == FunctionCode)
        holder = uncheckedDowncast<FunctionExecutable>(codeBlock.ownerExecutable())->unlinkedExecutable();
    ASSERT(!!holder == (coreKind == UnlinkedCodeBlockCoreKind::Function));

    // Step 1: SC1 and SC2, which check what the capture builds from the producer's own state, so a failure is a recording
    // fault (THREAD Session).
    if (state->strict()) {
        if (!ucbChildrenHaveIdentities(registry, ucb, record->key) || !ucbValueProfileCountsAgree(ucb))
            return std::unexpected(UCBCaptureFailure::StrictCheckFailed);
    }

    UCBCaptureCharges charges(budget);

    // Step 2: the feedback section. The exit sites cannot change between the count and the write (F17).
    FeedbackCounts counts = liveFeedbackCounts(ucb);
    size_t feedbackSize = feedbackSectionSize(counts);
    if (!charges.tryCharge(feedbackSize))
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    Vector<uint8_t> feedback(feedbackSize);
    writeFeedbackSection(feedback.mutableSpan(), ucb, counts);

    // Step 3: the core. The codec charges as it allocates and, on success, leaves exactly the payload's size charged.
    UCBCaptureEncodingBudget encodingBudget(budget);
    CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, ucb, holder, &encodingBudget, encodeFailure);
    if (!core) {
        ASSERT(encodeFailure == CoreEncodeFailure::BudgetRefused);
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    }
    charges.adopt(core->size());

    // Step 4.
    Digest256 coreDigest = SHA256::hash(core->span());

    // Step 5, digests, before the identity section's charge. The holder digest charges what it allocates through the same
    // adapter and releases it once hashed; the environment digests it keeps serve every role and stay uncharged (section 3.4).
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
    // A root UFE body's context covers its UFE's holder digest; a child's context does not (section 3.3).
    std::optional<Digest256> rootHolderDigest;
    if (identityKind == IdentityKind::FunctionConstructor || identityKind == IdentityKind::Builtin)
        rootHolderDigest = holderDigestValue;
    std::optional<Digest256> contextDigest = contextDigestOf(identityKind, record->context, rootHolderDigest);
    // captureRecord turned away a direct eval without a digest, and a root body has its holder digest.
    RELEASE_ASSERT(contextDigest);
    // A direct eval's digest is the one its record kept; every other context is computed here.
    if (identityKind != IdentityKind::DirectEval)
        ++statistics.contextDigests;

    // Step 5, the identity section.
    uint32_t constantCount = static_cast<uint32_t>(ucb.constantRegisters().size());
    size_t identitySize = identitySectionSize(constantCount);
    if (!charges.tryCharge(identitySize))
        return std::unexpected(UCBCaptureFailure::BudgetExceeded);
    Vector<uint8_t> identity(identitySize);

    // Provenance. A generated or imported UCB has generation's form, since an import takes only a body of provenance
    // Generated and rebuilds its butterflies. A decoded one is the embedder's, unless its record keeps the butterfly map of
    // the Generated body it was last seeded or attached from: its core matched that body's coreDigest and a core never
    // changes, so it is still that body's UCB, written with the kept map in place of its own plain butterflies (F23).
    // Only a Decoded record keeps a map (section 6.1).
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
        writeIdentitySection(identity.mutableSpan(), vm, record->key, *contextDigest, provenance, coreDigest, holderDigestValue, coreKind, parseFields, ucb, keptMap);
    }
    // The kept map's copy is freed with the scope above; it does not travel.
    if (keptMapSize)
        charges.release(keptMapSize);

    size_t chargedBytes = charges.handOff();
    return UCBSections { WTF::move(identity), core.releaseNonNull(), WTF::move(feedback), Ref<ProducerBudget> { budget }, chargedBytes };
}

} // namespace JSC::JITCache
