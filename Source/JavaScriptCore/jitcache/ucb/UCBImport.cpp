#include "config.h"
#include "UCBImport.h"

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "EvalExecutable.h"
#include "GlobalExecutable.h"
#include "JITCacheBench.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "LinkTimeConstant.h"
#include "RegExp.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "UCBFeedback.h"
#include "UCBRegistry.h"
#include "UCBSections.h"
#include "UCBTwins.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedGlobalCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <array>
#include <utility>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

// The UCB lane's engine. Where the native path would generate, it imports a matching body; where the native path decoded
// an embedder's bytecode cache, it seeds the decoded UCB before its holder publishes it; where the holder already has a
// UCB, it attaches a pending import; and it records what every request produced and every root's identity. Each entry
// point runs on the VM thread holding the API lock and heap access, re-reads the VM's JITCache state first, and counts
// each outcome in the registry's statistics.

namespace JSC::JITCache {

namespace UCBImportInternal {

// The request event of the bench report. While a bench report is open, each part of a request point records its wall
// time from CLOCK_MONOTONIC, and the thread CPU time from the first measured part to the close of the counted span is
// split into two totals: what counts toward the installation bound, and what is measured apart from it. The thread CPU
// clock is read only where the time passes from one account to another, so every read sits where a counted span begins or
// ends. Strict's checks go to neither total, since the bound is measured with strict off. With no report open, each part
// costs one test.
//
// Counted holds what the bound's accounting puts inside it: the section parses, matching, the butterfly rebuild,
// atomization, seeding, parse fields and records. Apart holds the key and context digests, the holder digest a root's
// context covers, each TDZ environment's first digest, reading the artifact, the decode and the extra-memory report that
// stands in for generation's. Excluded holds strict's checks.
enum class CostAccount : uint8_t { Counted, Apart, Excluded };
constexpr unsigned numberOfCostAccounts = 3;

enum class RequestPart : uint8_t {
    KeyDigest,
    Lookup, // bodyVersion and openBody; an attach, whose bodyVersion runs before anything is measured, only openBody
    IdentityParse,
    ContextDigest,
    RootHolderDigest,
    EnvironmentDigests,
    HolderDigest,
    SuppliedDigest,
    FeedbackParse,
    Decode,
    ClosureChecks,
    CoreDigest,
    AtomMap,
    StrictCore,
    ButterflyRebuild,
    Atomization,
    Seeding,
    ParseFields,
    ExtraMemory,
    Records,
};
constexpr unsigned numberOfRequestParts = static_cast<unsigned>(RequestPart::Records) + 1;

using BenchValue = decltype(BenchField::value);

class RequestTimer {
    WTF_MAKE_NONCOPYABLE(RequestTimer);

public:
    explicit RequestTimer(VMState& vmState)
    {
        if (BenchReport* report = vmState.benchReport())
            m_measurements.emplace(*report);
    }

    bool isMeasuring() const { return !!m_measurements; }

    // A part begins. The first one opens the CPU window in its own account; every later one moves the window to its
    // account. Returns the account the window goes back to when the part ends.
    CostAccount enter(CostAccount account)
    {
        Measurements& measurements = *m_measurements;
        if (!measurements.started) {
            measurements.started = true;
            measurements.account = account;
            measurements.cpuMark = benchThreadCPUNanoseconds();
            return CostAccount::Counted;
        }
        return switchTo(account);
    }

    void leave(CostAccount previous, RequestPart part, Seconds wallTime)
    {
        Measurements& measurements = *m_measurements;
        auto index = static_cast<unsigned>(part);
        measurements.partNanoseconds[index] += wallTime.nanosecondsAs<uint64_t>();
        measurements.partRan[index] = true;
        switchTo(previous);
    }

    // The size of the core the decode reads, which the request event reports beside the request's source.
    void noteCoreBytes(size_t bytes)
    {
        if (m_measurements)
            m_measurements->coreBytes = bytes;
    }

    // Closes the counted span, then writes the request event; nothing it does after that clock read is measured.
    void record(ASCIILiteral action, const BodyKey&, bool strict, uint64_t sourceCharacters);

private:
    struct Measurements {
        explicit Measurements(BenchReport& report)
            : report(report)
        {
        }

        BenchReport& report;
        bool started { false };
        CostAccount account { CostAccount::Counted };
        uint64_t cpuMark { 0 };
        std::array<uint64_t, numberOfCostAccounts> accountNanoseconds { };
        std::array<uint64_t, numberOfRequestParts> partNanoseconds { };
        std::array<bool, numberOfRequestParts> partRan { };
        std::optional<uint64_t> coreBytes; // set when the request decodes a core
    };

    CostAccount switchTo(CostAccount account)
    {
        Measurements& measurements = *m_measurements;
        if (account == measurements.account)
            return account;
        uint64_t now = benchThreadCPUNanoseconds();
        measurements.accountNanoseconds[static_cast<unsigned>(measurements.account)] += now - measurements.cpuMark;
        measurements.cpuMark = now;
        return std::exchange(measurements.account, account);
    }

    uint64_t partNanoseconds(RequestPart part) const { return m_measurements->partNanoseconds[static_cast<unsigned>(part)]; }
    bool partRan(RequestPart part) const { return m_measurements->partRan[static_cast<unsigned>(part)]; }

    BenchValue partValue(RequestPart part) const
    {
        if (!partRan(part))
            return nullptr;
        return partNanoseconds(part);
    }

    std::optional<Measurements> m_measurements;
};

void RequestTimer::record(ASCIILiteral action, const BodyKey& key, bool strict, uint64_t sourceCharacters)
{
    if (!m_measurements)
        return;
    Measurements& measurements = *m_measurements;
    if (measurements.started) {
        uint64_t now = benchThreadCPUNanoseconds();
        measurements.accountNanoseconds[static_cast<unsigned>(measurements.account)] += now - measurements.cpuMark;
        measurements.started = false;
    }

    // The event's matching is the counted comparisons that choose a body: a decoded UCB's core digest, the check that its
    // marked constants are atoms, and a child's holder digest beyond its environments' first digests. The section parses
    // have their own fields.
    BenchValue matching = nullptr;
    if (partRan(RequestPart::CoreDigest) || partRan(RequestPart::AtomMap) || partRan(RequestPart::HolderDigest))
        matching = partNanoseconds(RequestPart::CoreDigest) + partNanoseconds(RequestPart::AtomMap) + partNanoseconds(RequestPart::HolderDigest);

    measurements.report.record("request"_s, {
        { "key"_s, bodyKeyHex(key) },
        { "action"_s, String { action } },
        { "strict"_s, strict },
        { "counted"_s, measurements.accountNanoseconds[static_cast<unsigned>(CostAccount::Counted)] },
        { "apart"_s, measurements.accountNanoseconds[static_cast<unsigned>(CostAccount::Apart)] },
        { "matching"_s, matching },
        { "keyDigest"_s, partValue(RequestPart::KeyDigest) },
        { "lookup"_s, partValue(RequestPart::Lookup) },
        { "identityParse"_s, partValue(RequestPart::IdentityParse) },
        { "contextDigest"_s, partValue(RequestPart::ContextDigest) },
        { "rootHolderDigest"_s, partValue(RequestPart::RootHolderDigest) },
        { "environmentDigests"_s, partValue(RequestPart::EnvironmentDigests) },
        { "holderDigest"_s, partValue(RequestPart::HolderDigest) },
        { "suppliedDigest"_s, partValue(RequestPart::SuppliedDigest) },
        { "feedbackParse"_s, partValue(RequestPart::FeedbackParse) },
        { "decode"_s, partValue(RequestPart::Decode) },
        { "closureChecks"_s, partValue(RequestPart::ClosureChecks) },
        { "coreDigest"_s, partValue(RequestPart::CoreDigest) },
        { "atomMap"_s, partValue(RequestPart::AtomMap) },
        { "strictCore"_s, partValue(RequestPart::StrictCore) },
        { "butterflyRebuild"_s, partValue(RequestPart::ButterflyRebuild) },
        { "atomization"_s, partValue(RequestPart::Atomization) },
        { "seeding"_s, partValue(RequestPart::Seeding) },
        { "parseFields"_s, partValue(RequestPart::ParseFields) },
        { "extraMemory"_s, partValue(RequestPart::ExtraMemory) },
        { "records"_s, partValue(RequestPart::Records) },
        { "sourceCharacters"_s, sourceCharacters },
        { "coreBytes"_s, measurements.coreBytes ? BenchValue { *measurements.coreBytes } : BenchValue { nullptr } },
    });
}

// One measured part. A null timer, or one with no report open, measures nothing.
class PartScope {
    WTF_MAKE_NONCOPYABLE(PartScope);

public:
    PartScope(RequestTimer* timer, RequestPart part, CostAccount account)
        : m_timer(timer && timer->isMeasuring() ? timer : nullptr)
        , m_part(part)
    {
        if (!m_timer)
            return;
        m_previous = m_timer->enter(account);
        m_start = MonotonicTime::now();
    }

    ~PartScope()
    {
        if (m_timer)
            m_timer->leave(m_previous, m_part, MonotonicTime::now() - m_start);
    }

private:
    RequestTimer* const m_timer;
    const RequestPart m_part;
    CostAccount m_previous { CostAccount::Counted };
    MonotonicTime m_start;
};

static UCBStatistics& statisticsOf(VMState& vmState)
{
    return vmState.registry().statistics();
}

static void countMiss(VMState& vmState, MissReason reason)
{
    ++statisticsOf(vmState).misses[static_cast<size_t>(reason)];
}

static ASCIILiteral stepName(InvalidMaterialStep step)
{
    switch (step) {
    case InvalidMaterialStep::Identity:
        return "ucb.identity"_s;
    case InvalidMaterialStep::Feedback:
        return "ucb.feedback"_s;
    case InvalidMaterialStep::Decode:
        return "ucb.decode"_s;
    case InvalidMaterialStep::Closure:
        return "ucb.closure"_s;
    case InvalidMaterialStep::StrictCore:
        return "ucb.strict-core"_s;
    case InvalidMaterialStep::SuppliedDigest:
        return "ucb.supplied-digest"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return "ucb.identity"_s;
}

// Turns cache activity off for good and names the step in status. The caller then returns, and the native path
// goes on as if JITCache were off; a UCB the import decoded is unreachable and the collector frees it.
static void reportInvalidMaterial(VMState& vmState, InvalidMaterialStep step, ASCIILiteral what, const BodyKey& key)
{
    ++statisticsOf(vmState).invalidMaterial[static_cast<size_t>(step)];
    vmState.raiseInvalidMaterial(stepName(step), makeString(what, "; body "_s, bodyKeyHex(key)));
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

// The strict and with-scope bits a global root's key carries, which its record keeps.
static LexicallyScopedFeatures keyFeaturesOf(const RequestState& state)
{
    if (!isGlobalRequest(state))
        return NoLexicallyScopedFeatures;
    return static_cast<LexicallyScopedFeatures>(state.requestFeatures & (StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature));
}

// The decode's SourceCodeKey check ignores the with-scope bit, so a decoded root can carry the other bit.
static bool withScopeBitDiffers(const RequestState& state, const UnlinkedGlobalCodeBlock& codeBlock)
{
    return !!((state.requestFeatures ^ codeBlock.lexicallyScopedFeatures()) & TaintedByWithScopeLexicallyScopedFeature);
}

// A root's source digest, counted in the statistics. In twins builds with a report open, the twin check holds a supplied
// one to its text, which leaves the registry's verified set alone.
static RootSourceDigest takeRootSourceDigest(VMState& vmState, const SourceCode& rootSource, const Digest256* builtinMetadataDigest)
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

// The provider a key rests on when its root's digest came from it, kept only in a VM that imports with strict on, where
// an import verifies it.
static RefPtr<SourceProvider> suppliedDigestProviderFor(VMState& vmState, const SourceCode& rootSource, const RootSourceDigest& digest)
{
    if (digest.origin != SourceDigestOrigin::Provider || !vmState.importsEnabled() || !vmState.strict())
        return nullptr;
    return rootSource.provider();
}

// The key the request's inputs give with `keyMode`, or none, with the source digest and the supplied-digest provider it
// rests on.
static std::optional<BodyKey> computeRequestKey(VMState& vmState, RequestState& state, OptionSet<CodeGenerationMode> keyMode)
{
    switch (state.kind) {
    case RequestKind::Program:
    case RequestKind::Module:
    case RequestKind::IndirectEval: {
        RootSourceDigest digest = takeRootSourceDigest(vmState, state.source, nullptr);
        state.sourceDigest = digest.digest;
        state.suppliedDigestProvider = suppliedDigestProviderFor(vmState, state.source, digest);
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
                state.suppliedDigestProvider = child.parent->suppliedDigestProvider();
                state.vouchedHolderDigest = child.parent->childHolderDigest(child.table, child.index);
                // Computed at the body's request rather than when its parent was recorded, since most children never run.
                return BodyKey::make(IdentityKind::Child, state.specialization, keyMode, childIdentityDigest(child.parent->key(), child.table, child.index));
            },
            [&](const RootIdentity& root) {
                state.suppliedDigestProvider = root.suppliedDigestProvider;
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

static std::optional<BodyKey> requestKeyMeasured(RequestState& state, OptionSet<CodeGenerationMode> keyMode, RequestTimer* timer)
{
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

// The inputs a global's or a UFE body's context digest is computed from, which its record keeps.
static RecordedContext contextInputsOf(const RequestState& state)
{
    auto providerOffset = static_cast<unsigned>(state.source.startOffset());
    auto firstLine = static_cast<unsigned>(state.source.firstLine().oneBasedInt());
    if (state.kind == RequestKind::FunctionBody)
        return BodyContextInputs { .providerOffset = providerOffset, .firstLine = firstLine };
    ASSERT(isGlobalRequest(state));
    return GlobalContextInputs {
        .providerOffset = providerOffset,
        .firstLine = firstLine,
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
    return directEvalContextDigest(static_cast<unsigned>(state.source.startOffset()), static_cast<unsigned>(state.source.firstLine().oneBasedInt()), state.requestFeatures,
        executable->derivedContextType(), executable->needsClassFieldInitializer(), executable->privateBrandRequirement(), executable->isArrowFunctionContext(),
        executable->isInsideOrdinaryFunction(), state.evalContextType, state.variablesUnderTDZ ? *state.variablesUnderTDZ : noVariables,
        state.privateNameEnvironment ? *state.privateNameEnvironment : noPrivateNames);
}

// The request's context, computed at most once and only once a body at its key has passed the key comparison.
static const Digest256& requestContextMeasured(RequestState& state, RequestTimer* timer)
{
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
static RecordedContext recordedContextMeasured(RequestState& state, RequestTimer* timer)
{
    if (state.kind != RequestKind::DirectEval)
        return contextInputsOf(state);
    if (!state.context) {
        VMState* vmState = state.vm.jitCacheState();
        if (vmState && vmState->productionActive())
            requestContextMeasured(state, timer);
    }
    return DirectEvalContext { .digest = state.context };
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

// The holder a record of the request's UCB keeps: the requesting UFE, for a function body in a VM that
// captures bodies without a CodeBlock (VMState::capturesUncompiledBodies), and none otherwise.
static UnlinkedFunctionExecutable* recordedHolder(VMState& vmState, const RequestState& state)
{
    if (state.kind != RequestKind::FunctionBody || !vmState.capturesUncompiledBodies())
        return nullptr;
    return state.functionExecutable;
}

// Whether the body holds an image. With strict on, the container's checks have matched its sections to the envelope's
// highest tier; with strict off the envelope is trusted.
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

// A copy of a Generated body's butterfly map for the record of a decoded UCB, so that a later capture of it keeps the
// body's provenance; none for a body of the other provenance or where nothing will capture.
static std::optional<Vector<uint8_t>> generatedButterflyMapFor(VMState& vmState, const IdentitySection& identity)
{
    if (identity.provenance != CoreProvenance::Generated || !vmState.productionActive())
        return std::nullopt;
    return Vector<uint8_t>(identity.butterflyMap);
}

// With strict on, an identity section that breaks a rule of its layout is invalid material.
static std::optional<IdentitySection> parseIdentity(VMState& vmState, const ValidatedBody& body, RequestTimer& timer)
{
    std::optional<IdentitySection> identity;
    {
        PartScope scope(&timer, RequestPart::IdentityParse, CostAccount::Counted);
        identity = parseIdentitySection(body.section(SectionKind::UCBIdentity), vmState.strict());
    }
    if (!identity)
        reportInvalidMaterial(vmState, InvalidMaterialStep::Identity, "ucb.identity breaks a rule of its layout"_s, body.key());
    return identity;
}

// With strict on, a feedback section must keep the rules of its layout and the identity section's constant count.
static std::optional<FeedbackSection> parseFeedback(VMState& vmState, const ValidatedBody& body, const IdentitySection& identity, RequestTimer& timer)
{
    std::optional<FeedbackSection> feedback;
    {
        PartScope scope(&timer, RequestPart::FeedbackParse, CostAccount::Counted);
        feedback = FeedbackSection::parse(body.section(SectionKind::UCBFeedback), vmState.strict());
    }
    if (!feedback) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Feedback, "ucb.feedback breaks a rule of its layout"_s, body.key());
        return std::nullopt;
    }
    if (vmState.strict() && feedback->counts().constants != identity.constantCount) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Feedback, "ucb.feedback and ucb.identity disagree on the constant count"_s, body.key());
        return std::nullopt;
    }
    return feedback;
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

// The decode checks the kind the key implies, takes the requesting UFE as the holder of a function core, validates the
// core's structure with strict on and runs under its own DeferGC. The payload pins the body while the decoder reads it,
// and the decoder lets the payload go when the decode returns, since no decoded UFE keeps it.
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
        SourceProvider* provider = state.source.provider();
        RELEASE_ASSERT(provider);
        codeBlock = decodeUnlinkedCodeBlockCore(state.vm, WTF::move(payload), *provider, coreKindFor(key.identityKind()), state.functionExecutable, vmState.strict(), failure);
    }
    if (!codeBlock || failure != CoreDecodeFailure::None) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Decode, decodeFailureDescription(failure), key);
        return nullptr;
    }
    return codeBlock;
}

static CodeType codeTypeFor(UnlinkedCodeBlockCoreKind kind)
{
    switch (kind) {
    case UnlinkedCodeBlockCoreKind::Program:
        return GlobalCode;
    case UnlinkedCodeBlockCoreKind::Module:
        return ModuleCode;
    case UnlinkedCodeBlockCoreKind::Eval:
        return EvalCode;
    case UnlinkedCodeBlockCoreKind::Function:
        return FunctionCode;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return GlobalCode;
}

// The decoded UCB's class is the core kind's.
static bool classFits(const UnlinkedCodeBlock& codeBlock, UnlinkedCodeBlockCoreKind kind)
{
    switch (kind) {
    case UnlinkedCodeBlockCoreKind::Program:
        return codeBlock.inherits<UnlinkedProgramCodeBlock>();
    case UnlinkedCodeBlockCoreKind::Module:
        return codeBlock.inherits<UnlinkedModuleProgramCodeBlock>();
    case UnlinkedCodeBlockCoreKind::Eval:
        return codeBlock.inherits<UnlinkedEvalCodeBlock>();
    case UnlinkedCodeBlockCoreKind::Function:
        return codeBlock.inherits<UnlinkedFunctionCodeBlock>();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

// The decoded UCB's mode, constructor bit and code type are the key's.
static bool shapeFits(const UnlinkedCodeBlock& codeBlock, const BodyKey& key, UnlinkedCodeBlockCoreKind kind)
{
    return codeBlock.codeGenerationMode() == key.codeGenerationMode()
        && codeBlock.isConstructor() == (key.specialization() == CodeSpecializationKind::CodeForConstruct)
        && codeBlock.codeType() == codeTypeFor(kind);
}

// A function UCB's builtin bit, parse mode, constructor kind and default-constructor bit are its UFE's.
static bool functionKindFits(const UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable& executable)
{
    return codeBlock.isBuiltinFunction() == executable.isBuiltinFunction()
        && codeBlock.parseMode() == executable.parseMode()
        && codeBlock.constructorKind() == executable.constructorKind()
        && codeBlock.isBuiltinDefaultClassConstructor() == executable.isBuiltinDefaultClassConstructor();
}

// Every link-time constant indexes JSGlobalObject::linkTimeConstant's array, which casts it to unsigned, so a negative
// one would wrap; and every RegExp constant is valid, as generation emits only valid ones.
static bool constantsResolve(UnlinkedCodeBlock& codeBlock)
{
    const auto& constants = codeBlock.constantRegisters();
    for (size_t index = 0; index < constants.size(); ++index) {
        JSValue value = constants[index].get();
        if (codeBlock.constantSourceCodeRepresentation(static_cast<unsigned>(index)) == SourceCodeRepresentation::LinkTimeConstant) {
            if (!value.isInt32() || value.asInt32() < 0 || static_cast<unsigned>(value.asInt32()) >= numberOfLinkTimeConstants)
                return false;
            continue;
        }
        // An empty constant, which generation emits for TDZ checks, is no cell.
        if (!value)
            continue;
        if (auto* regExp = dynamicDowncast<RegExp>(value); regExp && !regExp->isValid())
            return false;
    }
    return true;
}

// With strict on, the closure checks of a UCB decoded from the body's core: its class, its shape, the feedback's counts
// and exit sites, its function kind, its constants, the constant maps, the singleton bits and the child holder digests.
// What the first check that fails found, or null.
static ASCIILiteral importedClosureFailure(const RequestState& state, UnlinkedCodeBlock& codeBlock, const BodyKey& key, const IdentitySection& identity, const FeedbackSection& feedback)
{
    UnlinkedCodeBlockCoreKind kind = coreKindFor(key.identityKind());
    if (!classFits(codeBlock, kind))
        return "the decoded UCB's class is not its core kind's"_s;
    if (!shapeFits(codeBlock, key, kind))
        return "the decoded UCB's mode, constructor bit or code type is not its key's"_s;
    if (!feedbackFits(codeBlock, feedback))
        return "ucb.feedback's counts or exit sites do not fit the decoded UCB"_s;
    if (state.functionExecutable && !functionKindFits(codeBlock, *state.functionExecutable))
        return "the decoded UCB's function kind is not its UFE's"_s;
    if (!constantsResolve(codeBlock))
        return "the decoded UCB holds a link-time constant out of range or an invalid RegExp"_s;
    if (!constantMapsFit(codeBlock, identity))
        return "the constant maps do not fit the decoded UCB's constants"_s;
    if (!constantBitsFit(codeBlock, feedback))
        return "ucb.feedback marks a constant that is no SymbolTable"_s;
    if (!childHolderDigestsFit(codeBlock, identity))
        return "the child holder digests do not fit the child UFEs"_s;
    return { };
}

// With strict on, the closure checks that apply to a UCB the native path produced: the feedback's counts and exit sites,
// its function kind and the constant maps.
static ASCIILiteral matchedClosureFailure(UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, const IdentitySection& identity, const FeedbackSection& feedback)
{
    if (!feedbackFits(codeBlock, feedback))
        return "ucb.feedback's counts or exit sites do not fit the UCB"_s;
    if (holder && !functionKindFits(codeBlock, *holder))
        return "the UCB's function kind is not its UFE's"_s;
    if (!constantMapsFit(codeBlock, identity))
        return "the constant maps do not fit the UCB's constants"_s;
    return { };
}

enum class HolderFit : uint8_t { Fits, Differs, Invalid };

// The holder check: the requesting UFE's holder digest equals the identity section's. A section with none, whose core kind is not a
// function's, differs as well.
//
// A UFE that an imported parent's decode made has the digest the parent's body lists for it, when the body lists one: the
// parent's capture computed it from the producer's UFE that the core encodes, the decode reproduces that UFE's descriptor,
// and its TDZ chain extends a start chain whose content the parent's own holder check or context matched. Normal mode
// compares that listed digest and encodes nothing. Strict computes the UFE's own digest, decides by it, and holds the
// listed one to it, so that every strict import checks the reasoning above; a listed digest that differs is invalid
// material.
static HolderFit holderFits(VMState& vmState, RequestState& state, const IdentitySection& identity, RequestTimer& timer, const BodyKey& key)
{
    if (!identity.holderDigest)
        return HolderFit::Differs;
    std::optional<Digest256> vouched = Options::jitCacheVouchedHolderDigests() ? state.vouchedHolderDigest : std::nullopt;
    if (vouched)
        ++statisticsOf(vmState).vouchedHolderChecks;
    if (vouched && !vmState.strict()) {
        PartScope scope(&timer, RequestPart::HolderDigest, CostAccount::Counted);
        return *identity.holderDigest == *vouched ? HolderFit::Fits : HolderFit::Differs;
    }
    const Digest256& own = requestHolderDigestMeasured(state, &timer, CostAccount::Counted);
    if (vouched && *vouched != own) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Identity, "a parent's body lists a holder digest that differs from its child UFE's own"_s, key);
        return HolderFit::Invalid;
    }
    return *identity.holderDigest == own ? HolderFit::Fits : HolderFit::Differs;
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
    // As a CodeCache hit does: the executable's features, last line and end column, and the directives back to the provider.
    recordParseFromUnlinkedCodeBlock(state.globalExecutable, state.source, uncheckedDowncast<UnlinkedGlobalCodeBlock>(&codeBlock));
}

// With strict on, the supplied digest the key rests on equals its text's, verified once per provider in the VM and
// before anything is decoded. The hash runs outside the registry's lock.
static bool suppliedDigestHolds(VMState& vmState, RequestState& state, const BodyKey& key, RequestTimer& timer)
{
    RefPtr<SourceProvider> provider = state.suppliedDigestProvider;
    if (!provider)
        return true;
    UCBRegistry& registry = vmState.registry();
    SourceID sourceID = provider->asID();
    if (registry.suppliedDigestVerified(sourceID))
        return true;
    bool holds = [&] {
        PartScope scope(&timer, RequestPart::SuppliedDigest, CostAccount::Excluded);
        std::optional<Digest256> supplied = provider->jitCacheSourceDigest();
        return supplied && *supplied == sourceDigest(provider->source());
    }();
    if (!holds) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::SuppliedDigest, "a provider's supplied digest is not the digest of its text"_s, key);
        return false;
    }
    registry.markSuppliedDigestVerified(sourceID);
    ++statisticsOf(vmState).suppliedDigestVerifications;
    return true;
}

enum class MatchResult : uint8_t {
    Matched,
    Missed, // stamped with the index token
    MissedAtomMap, // only the marked constants are not all atoms: not stamped, and the record notes the file's commit identifier
    Stopped, // invalid material
};

struct MatchSubject {
    UnlinkedCodeBlock& codeBlock;
    UCBOrigin origin;
    const RecordedContext* recordedContext; // a live UCB's record; null for a UCB being seeded, whose context is the request's
    const UnlinkedFunctionExecutable* holder; // the UFE whose slot holds a function UCB; null for any other
};

// Matches the subject against the body opened at its key, once the body is found. Counts each miss; the caller stamps a
// Missed result.
static MatchResult matchBody(VMState& vmState, RequestState& state, RequestTimer& timer, const MatchSubject& subject, const BodyKey& key, ValidatedBody& body,
    std::optional<IdentitySection>& identity, std::optional<FeedbackSection>& feedback)
{
    bool strict = vmState.strict();
    bool live = !!subject.recordedContext;

    // The identity section: its key, then the context.
    identity = parseIdentity(vmState, body, timer);
    if (!identity)
        return MatchResult::Stopped;
    if (identity->key != body.key()) {
        reportInvalidMaterial(vmState, InvalidMaterialStep::Identity, "ucb.identity holds another body's key"_s, body.key());
        return MatchResult::Stopped;
    }
    std::optional<Digest256> context = live ? recordContextDigest(vmState, state, key, *subject.recordedContext, timer) : std::optional<Digest256> { requestContextMeasured(state, &timer) };
    if (!context || *context != identity->contextDigest) {
        countMiss(vmState, MissReason::Context);
        return MatchResult::Missed;
    }

    // The feedback section.
    feedback = parseFeedback(vmState, body, *identity, timer);
    if (!feedback)
        return MatchResult::Stopped;

    if (subject.origin == UCBOrigin::Decoded) {
        // An embedder's payload is pinned by no key, so a natively decoded UCB matches only by its content, in both modes.
        Digest256 digest = [&] {
            PartScope scope(&timer, RequestPart::CoreDigest, CostAccount::Counted);
            return coreDigestOf(state.vm, subject.codeBlock, subject.holder);
        }();
        if (digest != identity->coreDigest) {
            countMiss(vmState, MissReason::CoreDigest);
            return MatchResult::Missed;
        }
    } else {
        // A generated or imported UCB matches by its generation inputs, which an embedder's payload does not pin.
        if (identity->provenance != CoreProvenance::Generated) {
            countMiss(vmState, MissReason::Provenance);
            return MatchResult::Missed;
        }
        if (subject.holder) {
            HolderFit fit = holderFits(vmState, state, *identity, timer, key);
            if (fit == HolderFit::Invalid)
                return MatchResult::Stopped;
            if (fit == HolderFit::Differs) {
                countMiss(vmState, MissReason::Holder);
                return MatchResult::Missed;
            }
        }
    }

    // With strict on, the closure checks.
    if (strict) {
        ASCIILiteral failure = [&] {
            PartScope scope(&timer, RequestPart::ClosureChecks, CostAccount::Excluded);
            return matchedClosureFailure(subject.codeBlock, subject.holder, *identity, *feedback);
        }();
        if (!failure.isNull()) {
            reportInvalidMaterial(vmState, InvalidMaterialStep::Closure, failure, key);
            return MatchResult::Stopped;
        }
    }

    // Atom-ness only grows, so this miss can turn into a match with the same file. A UCB being seeded has its marked
    // constants atomized after the match instead.
    if (subject.origin == UCBOrigin::Decoded && live) {
        bool atoms = [&] {
            PartScope scope(&timer, RequestPart::AtomMap, CostAccount::Counted);
            return markedConstantsAreAtoms(subject.codeBlock, *identity);
        }();
        if (!atoms) {
            countMiss(vmState, MissReason::AtomMap);
            return MatchResult::MissedAtomMap;
        }
    }

    // From equal inputs the engine's generation makes the producer's UCB, which strict checks on the content.
    if (subject.origin != UCBOrigin::Decoded && strict) {
        Digest256 digest = [&] {
            PartScope scope(&timer, RequestPart::StrictCore, CostAccount::Excluded);
            return coreDigestOf(state.vm, subject.codeBlock, subject.holder);
        }();
        if (digest != identity->coreDigest) {
            reportInvalidMaterial(vmState, InvalidMaterialStep::StrictCore, "a reused UCB's core encoding is not the body's"_s, key);
            return MatchResult::Stopped;
        }
    }

    return MatchResult::Matched;
}

} // namespace UCBImportInternal

std::optional<BodyKey> requestKey(RequestState& state, OptionSet<CodeGenerationMode> keyMode)
{
    return UCBImportInternal::requestKeyMeasured(state, keyMode, nullptr);
}

const Digest256& requestContext(RequestState& state)
{
    return UCBImportInternal::requestContextMeasured(state, nullptr);
}

const Digest256& requestHolderDigest(RequestState& state)
{
    return UCBImportInternal::requestHolderDigestMeasured(state, nullptr, UCBImportInternal::CostAccount::Counted);
}

RecordedContext recordedContext(RequestState& state)
{
    return UCBImportInternal::recordedContextMeasured(state, nullptr);
}

std::optional<BodyKey> decodedRootKey(RequestState& state, const UnlinkedGlobalCodeBlock& codeBlock)
{
    // A decoded root whose with-scope bit differs from its request's has no key.
    if (UCBImportInternal::withScopeBitDiffers(state, codeBlock))
        return std::nullopt;
    return UCBImportInternal::requestKeyMeasured(state, codeBlock.codeGenerationMode(), nullptr);
}

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
    bool strict = vmState->strict();
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
    std::optional<IdentitySection> identity = parseIdentity(*vmState, body.get(), timer);
    if (!identity)
        return nullptr;

    // Its key, in both modes: a body file is named by its whole key, so a stored key other than its own contradicts the
    // body's envelope, and keys are among what strict off checks too.
    if (identity->key != body->key()) {
        reportInvalidMaterial(*vmState, InvalidMaterialStep::Identity, "ucb.identity holds another body's key"_s, body->key());
        return nullptr;
    }
    // Its provenance: an embedder's core was generated under option values no key records, with plain constant butterflies
    // and, for a program or module, a decoder's declaration-map layout.
    if (identity->provenance == CoreProvenance::EmbedderDecoded)
        return miss(MissReason::Provenance);
    // Its context.
    if (identity->contextDigest != requestContextMeasured(state, &timer))
        return miss(MissReason::Context);
    // The holder check. A root body's context has already computed the holder digest.
    if (state.kind == RequestKind::FunctionBody) {
        HolderFit fit = holderFits(*vmState, state, *identity, timer, *key);
        if (fit == HolderFit::Invalid)
            return nullptr;
        if (fit == HolderFit::Differs)
            return miss(MissReason::Holder);
    }
    // With strict on, the supplied digest the key rests on.
    if (strict && !suppliedDigestHolds(*vmState, state, *key, timer))
        return nullptr;

    // The feedback section.
    std::optional<FeedbackSection> feedback = parseFeedback(*vmState, body.get(), *identity, timer);
    if (!feedback)
        return nullptr;

    // The core.
    UnlinkedCodeBlock* codeBlock = decodeCore(*vmState, state, body.get(), *key, timer);
    if (!codeBlock)
        return nullptr;

    // With strict on, the closure checks. With strict off the lane trusts the decoded core, and in neither mode does it
    // re-encode it.
    if (strict) {
        ASCIILiteral failure = [&] {
            PartScope scope(&timer, RequestPart::ClosureChecks, CostAccount::Excluded);
            return importedClosureFailure(state, *codeBlock, *key, *identity, *feedback);
        }();
        if (!failure.isNull()) {
            reportInvalidMaterial(*vmState, InvalidMaterialStep::Closure, failure, *key);
            return nullptr;
        }
    }

    // The decode gives every butterfly the plain structure and long string elements as plain strings, so each butterfly
    // generation built from atom strings is built again in that form. The part encloses its own DeferGC, so a collection
    // that the deferral's end runs for these allocations is measured with them.
    {
        PartScope scope(&timer, RequestPart::ButterflyRebuild, CostAccount::Counted);
        DeferGC deferGC(vm);
        rebuildAtomStringButterflies(vm, *codeBlock, *identity);
    }

    // The feedback. A body without an image keeps its LLInt counter's progress only up to one invocation short of crossing.
    BodyImage image = imageOf(body.get());
    {
        PartScope scope(&timer, RequestPart::Seeding, CostAccount::Counted);
        seedFeedback(vm, *codeBlock, *feedback, image);
    }

    // The parse fields.
    {
        PartScope scope(&timer, RequestPart::ParseFields, CostAccount::Counted);
        restoreParseFields(state, *codeBlock, *identity);
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
            .pendingImport = hasImage ? RefPtr<PendingImport> { PendingImport::create(body.copyRef(), *key, indexToken, PendingImport::Origin::Seeded) } : nullptr,
            .suppliedDigestProvider = state.suppliedDigestProvider,
            .generatedButterflyMap = std::nullopt,
            .holder = recordedHolder(*vmState, state),
        };
        // The children's holder digests the body lists, which the holder check then compares for the child UFEs this decode
        // made. Strict's closure checks have matched the list to the UCB's children; normal mode keeps it only when it has
        // one digest per child.
        std::span<const uint8_t> childHolderDigests;
        if (Options::jitCacheVouchedHolderDigests() && identity->childHolderDigestCount() == codeBlock->numberOfFunctionDecls() + codeBlock->numberOfFunctionExprs())
            childHolderDigests = identity->childHolderDigests;
        bool recorded = addRecord(*vmState, *codeBlock, WTF::move(record), childHolderDigests);
        // The UCB was just decoded, so nothing has recorded it, and nothing since the key has turned cache activity off.
        ASSERT_UNUSED(recorded, recorded);
    }
    ++statisticsOf(*vmState).imports;
    state.settled = true;
    timer.record("import"_s, *key, strict, state.source.length());

#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState->twinReportSink())
        verifyImport(state, *codeBlock, body.get(), *sink);
#endif

    // The native holder publishes the UCB as it publishes a generated one.
    return codeBlock;
}

// No holder has published the UCB and no CodeBlock references it.
void seedDecoded(RequestState& state, UnlinkedCodeBlock& codeBlock)
{
    using namespace UCBImportInternal;

    VM& vm = state.vm;
    VMState* vmState = vm.jitCacheState();
    if (!vmState || !vmState->tracksKeys())
        return;
    RequestTimer timer(*vmState);

    // The UCB's key, with its own mode; the request's context waits for the match's context check.
    if (isGlobalRequest(state) && withScopeBitDiffers(state, uncheckedDowncast<UnlinkedGlobalCodeBlock>(codeBlock))) {
        // No key, so no record, no identity for its children, and nothing in it imports or is captured.
        countMiss(*vmState, MissReason::RequestKey);
        state.settled = true;
        return;
    }
    std::optional<BodyKey> key = requestKeyMeasured(state, codeBlock.codeGenerationMode(), &timer);
    if (!key) {
        countMiss(*vmState, MissReason::NoKey);
        return;
    }
    const UnlinkedFunctionExecutable* holder = state.functionExecutable;

    // The UCB's record when it is not seeded: origin Decoded, stamped with the index token of a body that did not fit. The
    // request settles.
    auto recordUnseeded = [&](std::optional<uint64_t> missedBodyVersion) {
        UCBRecord record {
            .key = *key,
            .context = recordedContextMeasured(state, &timer),
            .origin = UCBOrigin::Decoded,
            .keyFeatures = keyFeaturesOf(state),
            .missedBodyVersion = missedBodyVersion,
            .matchedBodyVersion = std::nullopt,
            .pendingImport = nullptr,
            .suppliedDigestProvider = state.suppliedDigestProvider,
            .generatedButterflyMap = std::nullopt,
            .holder = recordedHolder(*vmState, state),
        };
        addRecord(*vmState, codeBlock, WTF::move(record));
        state.settled = true;
    };

    // A VM that does not import records the UCB unseeded.
    if (!vmState->importsEnabled()) {
        recordUnseeded(std::nullopt);
        return;
    }

    // The body at the key. The UCB has no record yet, so no stamp is compared.
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
        recordUnseeded(std::nullopt);
        return;
    }
    // Unusable: cache activity is off, nothing is recorded, and the native path publishes the UCB without seeds.
    if (lookup->kind() == BodyLookup::Kind::Unusable)
        return;
    if (lookup->kind() == BodyLookup::Kind::Missing) {
        countMiss(*vmState, MissReason::NoBody);
        recordUnseeded(indexToken);
        return;
    }
    Ref<ValidatedBody> body = lookup->body().releaseNonNull();

    std::optional<IdentitySection> identity;
    std::optional<FeedbackSection> feedback;
    switch (matchBody(*vmState, state, timer, MatchSubject { codeBlock, UCBOrigin::Decoded, nullptr, holder }, *key, body.get(), identity, feedback)) {
    case MatchResult::Matched:
        break;
    case MatchResult::Missed:
        recordUnseeded(indexToken);
        return;
    case MatchResult::MissedAtomMap:
        // Only a live UCB checks that its marked constants are atoms.
        ASSERT_NOT_REACHED();
        return;
    case MatchResult::Stopped:
        // Invalid material: cache activity is off and nothing is recorded.
        return;
    }

    bool strict = vmState->strict();
    // With strict on, the singleton bits.
    if (strict) {
        bool fits = [&] {
            PartScope scope(&timer, RequestPart::ClosureChecks, CostAccount::Excluded);
            return constantBitsFit(codeBlock, *feedback);
        }();
        if (!fits) {
            reportInvalidMaterial(*vmState, InvalidMaterialStep::Closure, "ucb.feedback marks a constant that is no SymbolTable"_s, *key);
            return;
        }
    }

    // The image compares these constants by pointer as atoms. One decoded from an embedder's string table is the VM-wide
    // JSString that published UCBs share; swapping in its atom is the change a property-key use makes, and keeps the old
    // StringImpl alive for concurrent readers.
    {
        PartScope scope(&timer, RequestPart::Atomization, CostAccount::Counted);
        atomizeMarkedConstants(vm, codeBlock, *identity);
    }

    // The feedback. The parse fields stay the decode's own.
    BodyImage image = imageOf(body.get());
    {
        PartScope scope(&timer, RequestPart::Seeding, CostAccount::Counted);
        seedFeedback(vm, codeBlock, *feedback, image);
    }

    // The record. As at an import, a body without an image leaves no pending import and stamps the
    // record with the index token.
    {
        PartScope scope(&timer, RequestPart::Records, CostAccount::Counted);
        bool hasImage = image == BodyImage::Present;
        UCBRecord record {
            .key = *key,
            .context = recordedContextMeasured(state, &timer),
            .origin = UCBOrigin::Decoded,
            .keyFeatures = keyFeaturesOf(state),
            .missedBodyVersion = hasImage ? std::nullopt : std::optional<uint64_t> { indexToken },
            .matchedBodyVersion = body->version(),
            .pendingImport = hasImage ? RefPtr<PendingImport> { PendingImport::create(body.copyRef(), *key, indexToken, PendingImport::Origin::Seeded) } : nullptr,
            .suppliedDigestProvider = state.suppliedDigestProvider,
            .generatedButterflyMap = generatedButterflyMapFor(*vmState, *identity),
            .holder = recordedHolder(*vmState, state),
        };
        bool recorded = addRecord(*vmState, codeBlock, WTF::move(record));
        // The UCB is unpublished, so nothing has recorded it.
        ASSERT_UNUSED(recorded, recorded);
    }
    ++statisticsOf(*vmState).seededDecodes;
    state.settled = true;
    timer.record("seed"_s, *key, strict, state.source.length());

#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState->twinReportSink())
        verifyMatched(codeBlock, holder, true, body.get(), *sink);
#endif
}

// The UCB was published before, so nothing here writes to it, its UFE or its executable.
void attachLive(RequestState& state, UnlinkedCodeBlock& codeBlock)
{
    using namespace UCBImportInternal;

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

    // The request's key. A UFE body's record was made for this UFE's identity, this slot's specialization and the UCB's own
    // mode, so its key equals the request's by construction. A global request compares the bits first, which is where a
    // CodeCache hit whose request differs in the with-scope bit misses, and only then computes its key, which differs only
    // when the CodeCache served a block whose SourceCodeKey merely collided. Either miss belongs to the request rather than
    // to the UCB, so neither is stamped.
    if (state.kind != RequestKind::FunctionBody) {
        if (keyFeaturesOf(state) != record->keyFeatures) {
            countMiss(*vmState, MissReason::RequestKey);
            return;
        }
        if (requestKeyMeasured(state, codeBlock.codeGenerationMode(), &timer) != record->key) {
            countMiss(*vmState, MissReason::RequestKey);
            return;
        }
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

    std::optional<IdentitySection> identity;
    if (record->matchedBodyVersion == body->version()) {
        // The UCB passed every check of matchBody against this very file, but perhaps the atom check of its marked
        // constants. Nothing else the match read can have changed: the UCB's core encoding and its holder UFE are
        // immutable, and a constant can only have become an atom.
        if (record->origin == UCBOrigin::Decoded) {
            identity = parseIdentity(*vmState, body.get(), timer);
            if (!identity)
                return;
            bool atoms = [&] {
                PartScope scope(&timer, RequestPart::AtomMap, CostAccount::Counted);
                return markedConstantsAreAtoms(codeBlock, *identity);
            }();
            if (!atoms) {
                countMiss(*vmState, MissReason::AtomMap);
                return;
            }
        }
    } else {
        std::optional<FeedbackSection> feedback;
        switch (matchBody(*vmState, state, timer, MatchSubject { codeBlock, record->origin, &record->context, holder }, record->key, body.get(), identity, feedback)) {
        case MatchResult::Matched:
            break;
        case MatchResult::Missed:
            registry.setMissedBodyVersion(codeBlock, indexToken);
            return;
        case MatchResult::MissedAtomMap:
            // The next request repeats only the atom check against this file.
            registry.setMatchedBodyVersion(codeBlock, body->version());
            return;
        case MatchResult::Stopped:
            return;
        }
    }

    // The pending import. A generated or imported UCB's own butterflies have generation's form, so only a decoded one keeps
    // a map, and an attach to a body of the other provenance clears an earlier one.
    std::optional<Vector<uint8_t>> generatedButterflyMap;
    if (record->origin == UCBOrigin::Decoded)
        generatedButterflyMap = generatedButterflyMapFor(*vmState, *identity);
    bool attached = [&] {
        PartScope scope(&timer, RequestPart::Records, CostAccount::Counted);
        return registry.attachPendingImport(codeBlock, PendingImport::create(body.copyRef(), record->key, indexToken, PendingImport::Origin::Reused), WTF::move(generatedButterflyMap));
    }();
    // The record had no pending import above, and nothing since has attached one.
    if (!attached)
        return;
    ++statisticsOf(*vmState).attaches;
    state.settled = true;
    timer.record("attach"_s, record->key, vmState->strict(), state.source.length());

#if ENABLE(JITCACHE_TWINS)
    if (TwinReportSink* sink = vmState->twinReportSink())
        verifyMatched(codeBlock, holder, false, body.get(), *sink);
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
        .suppliedDigestProvider = state.suppliedDigestProvider,
        .generatedButterflyMap = std::nullopt,
        .holder = recordedHolder(*vmState, state),
    };
    addRecord(*vmState, codeBlock, WTF::move(record));
    state.settled = true;
}

// The slot of a lazily decoded UFE that the request did not ask for. It never settles the request, which goes on to
// import or generate its own slot when the decode left that one empty.
void recordDecodedSlot(RequestState& state, UnlinkedFunctionCodeBlock& codeBlock, CodeSpecializationKind specialization)
{
    using namespace UCBImportInternal;

    VMState* vmState = state.vm.jitCacheState();
    if (!vmState || !vmState->tracksKeys())
        return;
    ASSERT(state.kind == RequestKind::FunctionBody && state.functionExecutable);
    // The UFE's identity, this slot's specialization and the slot UCB's own mode; the request's key is the other slot's.
    std::optional<ExecutableIdentity> identity = vmState->registry().identityOf(*state.functionExecutable);
    if (!identity)
        return;
    IdentityKind kind = IdentityKind::Child;
    Digest256 identityDigest { };
    RefPtr<SourceProvider> suppliedDigestProvider;
    WTF::switchOn(*identity,
        [&](const ChildIdentity& child) {
            // The requested slot's key, when the request computed one, already holds this identity digest.
            identityDigest = state.key && *state.key ? (*state.key)->identityDigest() : childIdentityDigest(child.parent->key(), child.table, child.index);
            suppliedDigestProvider = child.parent->suppliedDigestProvider();
        },
        [&](const RootIdentity& root) {
            kind = root.kind;
            identityDigest = root.identityDigest;
            suppliedDigestProvider = root.suppliedDigestProvider;
        });
    // Both slots of one UFE share its source, so the record takes the request's offset and first line.
    UCBRecord record {
        .key = BodyKey::make(kind, specialization, codeBlock.codeGenerationMode(), identityDigest),
        .context = contextInputsOf(state),
        .origin = UCBOrigin::Decoded,
        .keyFeatures = NoLexicallyScopedFeatures,
        .missedBodyVersion = std::nullopt,
        .matchedBodyVersion = std::nullopt,
        .pendingImport = nullptr,
        .suppliedDigestProvider = WTF::move(suppliedDigestProvider),
        .generatedButterflyMap = std::nullopt,
        .holder = recordedHolder(*vmState, state),
    };
    addRecord(*vmState, codeBlock, WTF::move(record));
}

// The UFE does not keep the root source its identity covers, so the identity digest runs here, at the root's creation.
// No body is looked up or opened: global object setup creates many builtins eagerly, and each body is looked up at its
// own first request. The root UFE's own singleton bit stays native.
void recordRoot(VM& vm, UnlinkedFunctionExecutable& executable, IdentityKind kind, const SourceCode& rootSource, LexicallyScopedFeatures lexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest)
{
    using namespace UCBImportInternal;

    VMState* vmState = vm.jitCacheState();
    if (!vmState || !vmState->tracksKeys())
        return;
    ASSERT(isRootFunctionKind(kind));
    RootSourceDigest digest = takeRootSourceDigest(*vmState, rootSource, builtinMetadataDigest);
    // The generator's digest belongs to the build, which debug builds check here.
    ASSERT(!builtinMetadataDigest || *builtinMetadataDigest == sourceDigest(rootSource.view()));
    RootIdentity identity {
        .kind = kind,
        .identityDigest = rootIdentityDigest(kind, digest.digest, lexicallyScopedFeatures, parameterEnd),
        .suppliedDigestProvider = suppliedDigestProviderFor(*vmState, rootSource, digest),
    };
    vmState->registry().recordRootExecutable(executable, identity);
}

} // namespace JSC::JITCache
