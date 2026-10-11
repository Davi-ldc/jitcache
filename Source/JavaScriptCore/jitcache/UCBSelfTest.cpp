#include "config.h"
#include "UCBSelfTest.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "ArrayPrototype.h"
#include "BaselineJITCode.h"
#include "BuiltinExecutables.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "CodeCache.h"
#include "CommonSlowPaths.h"
#include "DeferGC.h"
#include "DirectEvalExecutable.h"
#include "DirectEvalSite.h"
#include "FunctionExecutable.h"
#include "IndirectEvalExecutable.h"
#include "JITCacheAPI.h"
#include "JITCacheSHA256.h"
#include "JITCacheVMState.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "LLIntData.h"
#include "ModuleProgramExecutable.h"
#include "Options.h"
#include "ParserError.h"
#include "ProducerBudget.h"
#include "ProgramExecutable.h"
#include "RegExp.h"
#include "RegExpCache.h"
#include "SourceCode.h"
#include "SourceCodeKey.h"
#include "SourceProvider.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "TwinReport.h"
#include "UCBCapture.h"
#include "UCBFeedback.h"
#include "UCBImport.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UCBRequests.h"
#include "UCBSections.h"
#include "UCBTwinGeneration.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "VMEntryScope.h"
#include "ValidatedBody.h"
#include "VariableEnvironmentInlines.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <sys/mman.h>
#include <tuple>
#include <type_traits>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/Function.h>
#include <wtf/InlineMap.h>
#include <wtf/MathExtras.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/PageBlock.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

namespace UCBSelfTestInternal {

// One part of the self-test: the first check that fails writes the part's name and what broke into `failure`.
class SelfTestPart {
public:
    SelfTestPart(ASCIILiteral name, String& failure)
        : m_name(name)
        , m_failure(failure)
    {
    }

    bool check(bool condition, ASCIILiteral description)
    {
        if (!condition)
            m_failure = makeString(m_name, ": "_s, description);
        return condition;
    }

    bool check(bool condition, const String& description)
    {
        if (!condition)
            m_failure = makeString(m_name, ": "_s, description);
        return condition;
    }

private:
    ASCIILiteral m_name;
    String& m_failure;
};

// What a stub's destructor saw: whether it ran, and whether it ran inside a registry's critical section, where section
// 6.3 forbids destroying a pending import and dropping a reference to a provider or a ParentIdentity.
struct DestructionLog {
    void noteDestruction()
    {
        destroyed = true;
        insideRegistryLock = isUCBRegistryLockHeldByCurrentThread();
    }

    bool destroyed { false };
    bool insideRegistryLock { false };
};

// Stands for the provider whose supplied digest a record, a ParentIdentity or a root's identity keeps (section 6.1).
class LoggingProvider final : public StringSourceProvider {
public:
    static Ref<LoggingProvider> create(DestructionLog& log)
    {
        return adoptRef(*new LoggingProvider(log));
    }

    ~LoggingProvider() final
    {
        m_log.noteDestruction();
    }

private:
    explicit LoggingProvider(DestructionLog& log)
        : StringSourceProvider("0"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Program)
        , m_log(log)
    {
    }

    DestructionLog& m_log;
};

static BodyKey testKey(uint8_t fill)
{
    Digest256 identityDigest;
    identityDigest.fill(fill);
    return BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, identityDigest);
}

static UCBRecord testRecord(const BodyKey& key, UCBOrigin origin, RefPtr<SourceProvider>&& suppliedDigestProvider = nullptr)
{
    return UCBRecord {
        key,
        GlobalContextInputs { 0, 1, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false },
        origin,
        NoLexicallyScopedFeatures,
        std::nullopt,
        std::nullopt,
        nullptr,
        WTF::move(suppliedDigestProvider),
        std::nullopt,
    };
}

// A pending import whose body is a test body with no sections; onBodyDestroyed runs first in the body's destructor.
static Ref<PendingImport> testImport(const BodyKey& key, uint64_t commitIdentifier, uint64_t indexToken, Function<void()>&& onBodyDestroyed = { })
{
    Ref<ValidatedBody> body = ValidatedBody::createForTesting(key, commitIdentifier, { }, WTF::move(onBodyDestroyed));
    return PendingImport::create(WTF::move(body), key, indexToken, PendingImport::Origin::Reused);
}

// Generates a program's UCB with its child UFEs and none of their bodies. Depth 0 reaches no request point, so the VM's own
// registry records nothing, and the Strong keeps the cells, whose addresses the tests' registries key, alive.
static Strong<UnlinkedProgramCodeBlock> generateProgram(VM& vm, ASCIILiteral text)
{
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    UnlinkedProgramCodeBlock* codeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0);
    return Strong<UnlinkedProgramCodeBlock> { vm, codeBlock };
}

// Calls functor(child, table, index) for each child UFE in table order until it returns false.
template<typename Functor>
static bool allChildren(UnlinkedCodeBlock& codeBlock, const Functor& functor)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i) {
        if (!functor(*codeBlock.functionDecl(i), ChildTable::Declarations, i))
            return false;
    }
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i) {
        if (!functor(*codeBlock.functionExpr(i), ChildTable::Expressions, i))
            return false;
    }
    return true;
}

// Whether the registry gives `child` the identity (key, table, index) and the provider through the node every child of one
// recorded UCB shares, which `sharedParent` keeps from the first child checked.
static bool hasChildIdentity(const UCBRegistry& registry, const UnlinkedFunctionExecutable& child, ChildTable table, uint32_t index, const BodyKey& key, const SourceProvider* provider, const ParentIdentity*& sharedParent)
{
    auto identity = registry.identityOf(child);
    auto* childIdentity = identity ? std::get_if<ChildIdentity>(&*identity) : nullptr;
    if (!childIdentity || !childIdentity->parent || childIdentity->table != table || childIdentity->index != index)
        return false;
    if (childIdentity->parent->key() != key || childIdentity->parent->suppliedDigestProvider() != provider)
        return false;
    if (!sharedParent)
        sharedParent = childIdentity->parent.get();
    return childIdentity->parent.get() == sharedParent;
}

// U4: record, look up and remove in the three maps; a second recordCodeBlock keeps the first record; the children of one
// record share one ParentIdentity, whose key and provider each child's identityOf returns, and which outlives the record
// while a child lives and goes with the last child's destructor callback, outside the lock.
static bool testIdentitiesAndRecords(VM& vm, SelfTestPart& u4)
{
    // Declared before the registry, which still holds the providers when a check fails and returns.
    DestructionLog parentProviderLog;
    DestructionLog rootProviderLog;

    auto parent = generateProgram(vm, "function a() { } function b() { } (function c() { }); (function d() { });"_s);
    auto other = generateProgram(vm, "function r() { }"_s);
    if (!u4.check(parent && other, "a test program failed to generate"_s))
        return false;
    UnlinkedProgramCodeBlock& parentCodeBlock = *parent.get();
    size_t childCount = parentCodeBlock.numberOfFunctionDecls() + parentCodeBlock.numberOfFunctionExprs();
    bool hasChildUFEs = parentCodeBlock.numberOfFunctionDecls() && parentCodeBlock.numberOfFunctionExprs() && other.get()->numberOfFunctionDecls() == 1;
    if (!u4.check(hasChildUFEs, "a test program lacks the child UFEs the test reads"_s))
        return false;

    UCBRegistry registry;
    BodyKey parentKey = testKey(1);
    SourceProvider* parentProvider = nullptr;
    {
        Ref<LoggingProvider> provider = LoggingProvider::create(parentProviderLog);
        parentProvider = provider.ptr();
        if (!u4.check(registry.recordCodeBlock(parentCodeBlock, testRecord(parentKey, UCBOrigin::Generated, WTF::move(provider))), "the first recordCodeBlock of a UCB was refused"_s))
            return false;
    }
    UCBRegistry::Counts counts = registry.counts();
    if (!u4.check(counts.children == childCount && !counts.roots && counts.codeBlocks == 1 && !counts.pendingImports, "recordCodeBlock did not record the UCB and one identity per child"_s))
        return false;
    {
        auto view = registry.recordOf(parentCodeBlock);
        bool isRecorded = view && view->key == parentKey && view->origin == UCBOrigin::Generated && !view->hasPendingImport && view->suppliedDigestProvider == parentProvider;
        if (!u4.check(isRecorded && !view->missedBodyVersion && !view->matchedBodyVersion, "recordOf does not return the record recordCodeBlock made"_s))
            return false;
    }
    if (!u4.check(registry.keyOf(parentCodeBlock) == parentKey, "keyOf does not return the recorded key"_s))
        return false;

    const ParentIdentity* sharedParent = nullptr;
    auto childrenHaveParentIdentity = [&] {
        return allChildren(parentCodeBlock, [&](const UnlinkedFunctionExecutable& child, ChildTable table, uint32_t index) {
            return hasChildIdentity(registry, child, table, index, parentKey, parentProvider, sharedParent);
        });
    };
    if (!u4.check(childrenHaveParentIdentity(), "the children of one record do not share one ParentIdentity with its key and provider"_s))
        return false;

    // A UCB keeps its first record, and its children the identities that record gave them.
    if (!u4.check(!registry.recordCodeBlock(parentCodeBlock, testRecord(testKey(2), UCBOrigin::Imported)), "a second recordCodeBlock of a UCB was accepted"_s))
        return false;
    {
        auto view = registry.recordOf(parentCodeBlock);
        bool keptFirst = view && view->key == parentKey && view->origin == UCBOrigin::Generated && registry.counts().codeBlocks == 1;
        if (!u4.check(keptFirst && childrenHaveParentIdentity(), "a second recordCodeBlock replaced the first record or its children's identities"_s))
            return false;
    }

    // The node outlives the record while a child lives.
    registry.unlinkedCodeBlockDestroyed(&parentCodeBlock);
    if (!u4.check(!registry.recordOf(parentCodeBlock) && !registry.keyOf(parentCodeBlock) && !registry.counts().codeBlocks, "the UCB destructor callback left its record"_s))
        return false;
    if (!u4.check(registry.counts().children == childCount && childrenHaveParentIdentity() && !parentProviderLog.destroyed, "the ParentIdentity did not outlive its parent's record"_s))
        return false;

    // It goes with the last child's destructor callback, after the lock is released.
    size_t remaining = childCount;
    bool removedOneByOne = allChildren(parentCodeBlock, [&](const UnlinkedFunctionExecutable& child, ChildTable, uint32_t) {
        registry.unlinkedFunctionExecutableDestroyed(&child);
        --remaining;
        return !registry.identityOf(child) && registry.counts().children == remaining && parentProviderLog.destroyed == !remaining;
    });
    if (!u4.check(removedOneByOne, "a child destructor callback left its identity, or the ParentIdentity did not go with the last child"_s))
        return false;
    if (!u4.check(!parentProviderLog.insideRegistryLock, "the last child destructor callback dropped the ParentIdentity under the registry lock"_s))
        return false;

    // A root: record, look up and remove; its destructor callback drops the provider after the lock is released.
    const UnlinkedFunctionExecutable& root = *other.get()->functionDecl(0);
    Digest256 rootDigest;
    rootDigest.fill(3);
    SourceProvider* rootProvider = nullptr;
    {
        Ref<LoggingProvider> provider = LoggingProvider::create(rootProviderLog);
        rootProvider = provider.ptr();
        registry.recordRootExecutable(root, RootIdentity { IdentityKind::FunctionConstructor, rootDigest, WTF::move(provider) });
    }
    {
        auto identity = registry.identityOf(root);
        auto* rootIdentity = identity ? std::get_if<RootIdentity>(&*identity) : nullptr;
        bool isRecorded = rootIdentity && rootIdentity->kind == IdentityKind::FunctionConstructor && rootIdentity->identityDigest == rootDigest && rootIdentity->suppliedDigestProvider == rootProvider;
        if (!u4.check(isRecorded && registry.counts().roots == 1, "recordRootExecutable did not record the root's identity"_s))
            return false;
    }
    registry.unlinkedFunctionExecutableDestroyed(&root);
    if (!u4.check(!registry.identityOf(root) && !registry.counts().roots && rootProviderLog.destroyed, "the root destructor callback left its identity or its provider"_s))
        return false;
    return u4.check(!rootProviderLog.insideRegistryLock, "the root destructor callback dropped its provider under the registry lock"_s);
}

// U4: stamping a missed index token; an attach, and an AtomMap miss, which stamps nothing, set the matched commit
// identifier; an attach replaces the kept butterfly map with its argument, an empty one included, and
// copyGeneratedButterflyMap copies a kept map or nothing; resolvePendingImport with Installed detaches only, and with
// DroppedByGate also stamps the import's index token and counts the drop; a destructor callback drops a pending import,
// whose test body checks in its destructor that the registry lock is not held.
static bool testStampsAndPendingImports(VM& vm, SelfTestPart& u4)
{
    // Declared before the registry, which still holds the import when a check fails and returns.
    DestructionLog bodyLog;

    auto program = generateProgram(vm, "0;"_s);
    auto unrecorded = generateProgram(vm, "1;"_s);
    if (!u4.check(program && unrecorded, "a test program failed to generate"_s))
        return false;
    UnlinkedProgramCodeBlock& codeBlock = *program.get();

    UCBRegistry registry;
    BodyKey key = testKey(4);
    bool recorded = registry.recordCodeBlock(codeBlock, testRecord(key, UCBOrigin::Decoded));
    if (!u4.check(recorded && !registry.counts().children, "recordCodeBlock refused a UCB or gave a UCB without children child identities"_s))
        return false;
    auto recordIs = [&](std::optional<uint64_t> missedBodyVersion, std::optional<uint64_t> matchedBodyVersion, bool hasPendingImport) {
        auto view = registry.recordOf(codeBlock);
        return view && view->missedBodyVersion == missedBodyVersion && view->matchedBodyVersion == matchedBodyVersion && view->hasPendingImport == hasPendingImport;
    };

    // An AtomMap miss sets the matched commit identifier and stamps nothing; a miss stamps its index token.
    registry.setMatchedBodyVersion(codeBlock, 9);
    if (!u4.check(recordIs(std::nullopt, 9, false), "setMatchedBodyVersion did not set the matched commit identifier alone"_s))
        return false;
    registry.setMissedBodyVersion(codeBlock, 5);
    if (!u4.check(recordIs(5, 9, false), "setMissedBodyVersion did not stamp the missed index token alone"_s))
        return false;

    // An attach needs a record.
    bool attachedWithoutRecord = registry.attachPendingImport(*unrecorded.get(), testImport(key, 40, 1), std::nullopt);
    if (!u4.check(!attachedWithoutRecord && !registry.recordOf(*unrecorded.get()) && !registry.counts().pendingImports, "attachPendingImport accepted a UCB without a record"_s))
        return false;

    // An attach sets the matched commit identifier to its body's and keeps its butterfly map.
    const Vector<uint8_t> firstMap { 0xa5, 0x01 };
    std::array<uint8_t, 2> copied { };
    auto keptMapIs = [&](const Vector<uint8_t>& map) {
        return registry.copyGeneratedButterflyMap(codeBlock, copied) && equalSpans(std::span { copied }, map.span());
    };
    Ref<PendingImport> first = testImport(key, 41, 3);
    bool attachedFirst = registry.attachPendingImport(codeBlock, first.copyRef(), Vector<uint8_t>(firstMap));
    bool firstIsAttached = recordIs(5, 41, true) && registry.pendingImport(codeBlock) == first.ptr() && registry.counts().pendingImports == 1;
    if (!u4.check(attachedFirst && firstIsAttached, "attachPendingImport did not attach the import and set the matched commit identifier"_s))
        return false;
    if (!u4.check(keptMapIs(firstMap), "copyGeneratedButterflyMap did not copy the kept map"_s))
        return false;

    // A UCB has at most one pending import (I6).
    bool attachedSecond = registry.attachPendingImport(codeBlock, testImport(key, 50, 4), Vector<uint8_t> { 0xff, 0xff });
    bool firstStillAttached = recordIs(5, 41, true) && registry.pendingImport(codeBlock) == first.ptr();
    if (!u4.check(!attachedSecond && firstStillAttached && keptMapIs(firstMap), "a second attachPendingImport changed the record"_s))
        return false;

    // Installed detaches only.
    uint64_t gateDrops = registry.statistics().gateDrops;
    registry.resolvePendingImport(codeBlock, first, ImportResolution::Installed);
    bool detachedOnly = recordIs(5, 41, false) && !registry.pendingImport(codeBlock) && !registry.counts().pendingImports && registry.statistics().gateDrops == gateDrops;
    if (!u4.check(detachedOnly, "resolvePendingImport with Installed did more than detach the import"_s))
        return false;

    // An attach replaces the kept map with its argument, here none.
    Ref<PendingImport> second = testImport(key, 42, 6);
    bool attachedAfterResolution = registry.attachPendingImport(codeBlock, second.copyRef(), std::nullopt);
    if (!u4.check(attachedAfterResolution && recordIs(5, 42, true), "attachPendingImport refused a UCB whose import was resolved"_s))
        return false;
    copied.fill(0x5a);
    bool copiedWithoutMap = registry.copyGeneratedButterflyMap(codeBlock, copied);
    if (!u4.check(!copiedWithoutMap && copied[0] == 0x5a && copied[1] == 0x5a, "an attach without a map kept the earlier one, or copyGeneratedButterflyMap wrote without one"_s))
        return false;

    // A resolution acts only on the import still attached.
    registry.resolvePendingImport(codeBlock, first, ImportResolution::DroppedByGate);
    bool unchanged = recordIs(5, 42, true) && registry.pendingImport(codeBlock) == second.ptr() && registry.statistics().gateDrops == gateDrops;
    if (!u4.check(unchanged, "resolvePendingImport acted on an import that was no longer attached"_s))
        return false;

    // DroppedByGate also stamps the import's index token as missed and counts the drop.
    registry.resolvePendingImport(codeBlock, second, ImportResolution::DroppedByGate);
    bool dropped = recordIs(6, 42, false) && !registry.pendingImport(codeBlock) && registry.statistics().gateDrops == gateDrops + 1;
    if (!u4.check(dropped, "resolvePendingImport with DroppedByGate did not detach, stamp the import's index token and count the drop"_s))
        return false;

    // An attach replaces an empty kept map with its argument, and the record pins the import's body until the UCB's
    // destructor callback drops both, after the lock is released.
    const Vector<uint8_t> lastMap { 0x3c, 0x00 };
    Ref<PendingImport> last = testImport(key, 43, 7, [&] {
        bodyLog.noteDestruction();
    });
    // A successful attach moves the import into the record, which then holds the only reference.
    bool attachedAfterDrop = registry.attachPendingImport(codeBlock, WTF::move(last), Vector<uint8_t>(lastMap));
    if (!u4.check(attachedAfterDrop, "attachPendingImport refused a UCB whose import the gate dropped"_s))
        return false;
    if (!u4.check(keptMapIs(lastMap) && !bodyLog.destroyed, "an attach did not replace an empty kept map, or the record did not pin its import's body"_s))
        return false;
    registry.unlinkedCodeBlockDestroyed(&codeBlock);
    bool droppedWithRecord = !registry.recordOf(codeBlock) && !registry.counts().codeBlocks && !registry.counts().pendingImports && bodyLog.destroyed;
    if (!u4.check(droppedWithRecord, "the UCB destructor callback did not drop its record and pending import"_s))
        return false;
    return u4.check(!bodyLog.insideRegistryLock, "the UCB destructor callback dropped its pending import under the registry lock"_s);
}

// U4: the verified set holds a provider's SourceID once an import has checked its supplied digest (section 7.3.1, step 4e).
static bool testSuppliedDigests(SelfTestPart& u4)
{
    UCBRegistry registry;
    Ref<StringSourceProvider> verified = StringSourceProvider::create("0"_s, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    Ref<StringSourceProvider> unverified = StringSourceProvider::create("0"_s, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    if (!u4.check(!registry.suppliedDigestVerified(verified->asID()), "a provider was verified before markSuppliedDigestVerified"_s))
        return false;
    registry.markSuppliedDigestVerified(verified->asID());
    bool verifiedExactly = registry.suppliedDigestVerified(verified->asID()) && !registry.suppliedDigestVerified(unverified->asID());
    return u4.check(verifiedExactly, "markSuppliedDigestVerified did not verify exactly its provider"_s);
}

// U4 (section 13.1). Each part uses a registry of its own, so the VM's registry and statistics stay untouched, and calls
// the destructor callbacks itself on cells it keeps alive.
static bool testRegistry(VM& vm, String& failure)
{
    SelfTestPart u4 { "U4"_s, failure };
    return testIdentitiesAndRecords(vm, u4) && testStampsAndPendingImports(vm, u4) && testSuppliedDigests(u4);
}

// U5 and U6 read the layouts of sections 4.2 and 5.2 from the offsets below, which repeat the SPEC's tables rather than
// the lane's own constants, so that the writers and parsers are held to the SPEC.
static constexpr uint32_t identitySectionMagic = 0x49424355;
static constexpr size_t identityVersionOffset = 4;
static constexpr size_t identityCoreKindOffset = 6;
static constexpr size_t identityProvenanceOffset = 7;
static constexpr size_t identityKeyOffset = 8;
static constexpr size_t identityContextDigestOffset = 48;
static constexpr size_t identityCoreDigestOffset = 80;
static constexpr size_t identityHolderDigestOffset = 112;
static constexpr size_t identityFeaturesOffset = 144;
static constexpr size_t identityLexicallyScopedFeaturesOffset = 146;
static constexpr size_t identityHasCapturedVariablesOffset = 147;
static constexpr size_t identityConstantCountOffset = 148;
static constexpr size_t identityMapsOffset = 152;

static constexpr uint32_t feedbackSectionMagic = 0x46424355;
static constexpr size_t feedbackVersionOffset = 4;
static constexpr size_t feedbackReservedOffset = 6;
// V, A, B, U, E, D, X and C, in FeedbackCounts order.
static constexpr std::array<size_t, 8> feedbackCountOffsets { 8, 12, 16, 20, 24, 28, 32, 52 };
static constexpr size_t feedbackDidOptimizeOffset = 36;
static constexpr size_t feedbackQuickDFGTierUpOffset = 37;
static constexpr size_t feedbackQuickFTLTierUpOffset = 38;
static constexpr size_t feedbackReservedByteOffset = 39;
static constexpr size_t feedbackThresholdOffset = 40;
static constexpr size_t feedbackTotalCountOffset = 44;
static constexpr size_t feedbackCounterOffset = 48;
static constexpr size_t feedbackHeaderSize = 56;
static constexpr size_t exitSiteRecordSize = 8;
static constexpr uint32_t sectionCountLimit = 1u << 28;

// Where each array of ucb.feedback starts for the given counts, and where the section ends (section 5.2).
struct FeedbackLayout {
    explicit FeedbackLayout(const FeedbackCounts& counts)
        : arrayProfiles(feedbackHeaderSize + sizeof(SpeculatedType) * counts.valueProfiles)
        , binaryArithProfiles(arrayProfiles + 8 * static_cast<size_t>(counts.arrayProfiles))
        , unaryArithProfiles(binaryArithProfiles + 2 * static_cast<size_t>(counts.binaryArithProfiles))
        , unaryEnd(unaryArithProfiles + 2 * static_cast<size_t>(counts.unaryArithProfiles))
        , exitSites(roundUpToMultipleOf<4>(unaryEnd))
        , children(exitSites + exitSiteRecordSize * counts.exitSites)
        , constants(children + counts.functionDecls + counts.functionExprs)
        , end(constants + (static_cast<size_t>(counts.constants) + 7) / 8)
        , size(roundUpToMultipleOf<8>(end))
    {
    }

    size_t arrayProfiles;
    size_t binaryArithProfiles;
    size_t unaryArithProfiles;
    size_t unaryEnd;
    size_t exitSites;
    size_t children;
    size_t constants;
    size_t end;
    size_t size;
};

template<typename T>
static T loadField(std::span<const uint8_t> bytes, size_t offset)
{
    std::array<uint8_t, sizeof(T)> raw;
    memcpySpan(std::span { raw }, bytes.subspan(offset, sizeof(T)));
    return std::bit_cast<T>(raw);
}

template<typename T>
static void storeField(Vector<uint8_t>& bytes, size_t offset, T value)
{
    memcpySpan(bytes.mutableSpan().subspan(offset, sizeof(T)), asByteSpan(value));
}

static void setBit(std::span<uint8_t> map, uint32_t index)
{
    map[index / 8] |= 1u << (index % 8);
}

static bool bitIsSet(std::span<const uint8_t> map, uint32_t index)
{
    return (map[index / 8] >> (index % 8)) & 1;
}

static bool isZeroBytes(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

static Digest256 filledDigest(uint8_t fill)
{
    Digest256 digest;
    digest.fill(fill);
    return digest;
}

static VirtualRegister constantRegisterFor(uint32_t index)
{
    return VirtualRegister(VirtualRegister::firstConstantRegisterIndex + static_cast<int>(index));
}

// A constant register as a cell of class T, or null. An empty constant, which generation emits for TDZ checks, is no cell.
template<typename T>
static T* constantCell(UnlinkedCodeBlock& codeBlock, uint32_t index)
{
    JSValue value = codeBlock.constantRegisters()[index].get();
    return value ? dynamicDowncast<T>(value) : nullptr;
}

static bool isStringWithContent(JSValue value, ASCIILiteral content)
{
    auto* string = value ? dynamicDowncast<JSString>(value) : nullptr;
    StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
    return impl && String { impl } == content;
}

static std::optional<uint32_t> stringConstantIndex(UnlinkedCodeBlock& codeBlock, ASCIILiteral content)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (isStringWithContent(codeBlock.constantRegisters()[i].get(), content))
            return i;
    }
    return std::nullopt;
}

// The JSCellButterfly constant of `length` elements whose first element is the string `first`.
static std::optional<uint32_t> butterflyConstantIndex(UnlinkedCodeBlock& codeBlock, ASCIILiteral first, unsigned length)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        auto* butterfly = constantCell<JSCellButterfly>(codeBlock, i);
        if (butterfly && butterfly->length() == length && !hasDouble(butterfly->indexingMode()) && isStringWithContent(butterfly->get(0), first))
            return i;
    }
    return std::nullopt;
}

static std::optional<uint32_t> symbolTableConstantIndex(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<SymbolTable>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

static Vector<DFG::FrequentExitSite> exitSitesOf(UnlinkedCodeBlock& codeBlock)
{
    Vector<DFG::FrequentExitSite> sites;
    ConcurrentJSLocker locker(codeBlock.m_lock);
    codeBlock.exitProfile().forEachFrequentExitSite(locker, [&](const DFG::FrequentExitSite& site) {
        sites.append(site);
    });
    return sites;
}

// The UCB's tier-up history: didOptimize and both quick tier-up bits, read under the cell lock as capture and twins read
// them (section 5.3, step 5). The UCBs the tests compare are Strong-held, so a marker can visit one while the test runs,
// and its first visit in a cycle rewrites m_age, which shares a memory location with the quick bits, under that lock
// (UnlinkedCodeBlock::visitChildrenImpl).
struct TierUpHistory {
    TriState didOptimize;
    TriState quickDFGTierUp;
    bool quickFTLTierUp;
    friend bool operator==(const TierUpHistory&, const TierUpHistory&) = default;
};

static TierUpHistory tierUpHistoryOf(UnlinkedCodeBlock& codeBlock)
{
    Locker locker { codeBlock.cellLock() };
    return { codeBlock.didOptimize(), codeBlock.quickDFGTierUp(), codeBlock.isQuickFTLTierUp() };
}

// The precision a counter's float m_totalCount keeps around `magnitude` (section 5.5), with a factor of two to spare.
static bool equalUpToFloatRounding(double a, double b, double magnitude)
{
    return std::abs(a - b) <= std::max(std::abs(magnitude), 1.0) * 0x1p-23;
}

// A program whose UCB holds what the section tests read: string constants, an all-string literal in the atom form, a mixed
// literal and an all-string prefix in the plain form (F23), value, array and both kinds of arithmetic profiles, function
// declarations and expressions, and the SymbolTable of a block scope that a closure captures. ArrayNode::emitBytecode
// builds the prefix butterfly before a spread only when the literal also has an elision; a spread without one emits
// new_array_with_spread, element by element, and leaves no butterfly constant.
static constexpr ASCIILiteral sectionFixtureSource = "var words = [\"alpha\", \"beta\", \"gamma\"];\n"
    "var mixed = [\"alpha\", 1];\n"
    "var prefix = [\"delta\", \"epsilon\", , ...words];\n"
    "var plain = \"jitcache-u5-plain-constant\";\n"
    "var atom = \"jitcache-u5-atom-constant\";\n"
    "var unmarked = \"jitcache-u5-unmarked-constant\";\n"
    "var sum = words.length + mixed[1];\n"
    "var negated = -sum;\n"
    "function declared() { return 1; }\n"
    "(function expressed() { return 2; });\n"
    "{ let captured = 1; (function closure() { return captured; }); }\n"_s;

// A program generated as generateProgram does, with the source that a decode of its core needs as its provider.
struct ProgramFixture {
    String text;
    SourceCode source;
    Strong<UnlinkedProgramCodeBlock> codeBlock;
};

static ProgramFixture generateFixture(VM& vm, const String& text)
{
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    UnlinkedProgramCodeBlock* codeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0);
    return ProgramFixture { text, WTF::move(source), Strong<UnlinkedProgramCodeBlock> { vm, codeBlock } };
}

// The map checks set the bit at N, which lies inside the maps' last byte only when N % 8 != 0, so the fixture program takes
// one more string constant per try until its constant count is not a multiple of eight.
static std::optional<ProgramFixture> generateSectionFixture(VM& vm)
{
    StringBuilder text;
    text.append(sectionFixtureSource);
    for (unsigned padding = 0; padding < 8; ++padding) {
        ProgramFixture fixture = generateFixture(vm, text.toString());
        if (!fixture.codeBlock)
            return std::nullopt;
        if (fixture.codeBlock.get()->constantRegisters().size() % 8)
            return fixture;
        text.append("var padding"_s, padding, " = \"jitcache-u5-padding-"_s, padding, "\";\n"_s);
    }
    return std::nullopt;
}

// Copies `valid`, breaks one rule in the copy, and checks that strict parsing rejects it while parsing with strict off,
// which reads each field where the layout puts it and trusts it, still answers when the fixed layout stays readable.
template<typename Parse, typename Mutate>
static bool strictRejects(SelfTestPart& part, ASCIILiteral sectionName, const Parse& parse, const Vector<uint8_t>& valid, ASCIILiteral rule, bool keepsLayout, const Mutate& mutate)
{
    Vector<uint8_t> crafted = valid;
    mutate(crafted);
    if (!part.check(!parse(crafted.span(), true), makeString("strict "_s, sectionName, " parsing accepts "_s, rule)))
        return false;
    return part.check(!keepsLayout || !!parse(crafted.span(), false), makeString(sectionName, " parsing with strict off refuses "_s, rule));
}

static bool sameParseFields(const std::optional<FunctionParseFields>& a, const std::optional<FunctionParseFields>& b)
{
    if (!a || !b)
        return !a && !b;
    return a->features == b->features && a->lexicallyScopedFeatures == b->lexicallyScopedFeatures && a->hasCapturedVariables == b->hasCapturedVariables;
}

static bool sameIdentitySection(const IdentitySection& a, const IdentitySection& b)
{
    return a.coreKind == b.coreKind && a.provenance == b.provenance && a.key == b.key && a.contextDigest == b.contextDigest && a.coreDigest == b.coreDigest
        && a.holderDigest == b.holderDigest && sameParseFields(a.functionParseFields, b.functionParseFields) && a.constantCount == b.constantCount
        && equalSpans(a.atomMap, b.atomMap) && equalSpans(a.butterflyMap, b.butterflyMap);
}

// Whether `bytes` hold `expected` where section 4.2 puts each field, with zero padding.
static bool identityBytesMatch(std::span<const uint8_t> bytes, const IdentitySection& expected)
{
    size_t mapBytes = (static_cast<size_t>(expected.constantCount) + 7) / 8;
    size_t mapsEnd = identityMapsOffset + 2 * mapBytes;
    if (bytes.size() != roundUpToMultipleOf<8>(mapsEnd))
        return false;
    FunctionParseFields parseFields = expected.functionParseFields.value_or(FunctionParseFields { NoFeatures, NoLexicallyScopedFeatures, false });
    return loadField<uint32_t>(bytes, 0) == identitySectionMagic
        && loadField<uint16_t>(bytes, identityVersionOffset) == 1
        && bytes[identityCoreKindOffset] == static_cast<uint8_t>(expected.coreKind)
        && bytes[identityProvenanceOffset] == static_cast<uint8_t>(expected.provenance)
        && equalSpans(bytes.subspan(identityKeyOffset, BodyKey::byteSize), expected.key.bytes())
        && loadField<Digest256>(bytes, identityContextDigestOffset) == expected.contextDigest
        && loadField<Digest256>(bytes, identityCoreDigestOffset) == expected.coreDigest
        && loadField<Digest256>(bytes, identityHolderDigestOffset) == expected.holderDigest.value_or(Digest256 { })
        && loadField<CodeFeatures>(bytes, identityFeaturesOffset) == parseFields.features
        && bytes[identityLexicallyScopedFeaturesOffset] == parseFields.lexicallyScopedFeatures
        && bytes[identityHasCapturedVariablesOffset] == static_cast<uint8_t>(parseFields.hasCapturedVariables)
        && loadField<uint32_t>(bytes, identityConstantCountOffset) == expected.constantCount
        && equalSpans(bytes.subspan(identityMapsOffset, mapBytes), expected.atomMap)
        && equalSpans(bytes.subspan(identityMapsOffset + mapBytes, mapBytes), expected.butterflyMap)
        && isZeroBytes(bytes.subspan(mapsEnd));
}

// U5: ucb.identity. A program core, and a function core whose butterfly map a record kept, round-trip with strict on and
// off, laid out as section 4.2 writes them, with the maps generation's constants give; strict parsing rejects each rule
// of section 4.2 broken once, and parsing with strict off still answers wherever the layout stays readable.
static bool testIdentitySection(VM& vm, SelfTestPart& u5)
{
    auto fixture = generateSectionFixture(vm);
    if (!u5.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(codeBlock.constantRegisters().size());
    size_t mapBytes = (static_cast<size_t>(constantCount) + 7) / 8;

    auto words = butterflyConstantIndex(codeBlock, "alpha"_s, 3);
    auto mixed = butterflyConstantIndex(codeBlock, "alpha"_s, 2);
    auto prefix = butterflyConstantIndex(codeBlock, "delta"_s, 2);
    if (!u5.check(words && mixed && prefix, "the fixture program lacks one of its array literals"_s))
        return false;
    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    bool generatedForms = constantCell<JSCellButterfly>(codeBlock, *words)->structure() == atomStringsStructure
        && constantCell<JSCellButterfly>(codeBlock, *mixed)->structure() != atomStringsStructure
        && constantCell<JSCellButterfly>(codeBlock, *prefix)->structure() != atomStringsStructure;
    if (!u5.check(generatedForms, "generation did not give the atom form to the all-string literal alone (F23)"_s))
        return false;

    // The maps a capture of this UCB writes: every string constant, which generation makes an atom (F19), and the one
    // literal in the atom form. The other literals hold strings too but keep the plain form, so their bits stay clear.
    Vector<uint8_t> atomMap(FillWith { }, mapBytes, 0);
    Vector<uint8_t> butterflyMap(FillWith { }, mapBytes, 0);
    std::optional<uint32_t> someAtom;
    for (uint32_t i = 0; i < constantCount; ++i) {
        auto* string = constantCell<JSString>(codeBlock, i);
        if (!string)
            continue;
        StringImpl* impl = string->tryGetValueImpl();
        if (!u5.check(impl && impl->isAtom(), "generation left a string constant that is not an atom (F19)"_s))
            return false;
        setBit(atomMap.mutableSpan(), i);
        someAtom = i;
    }
    if (!u5.check(!!someAtom, "the fixture program has no string constant"_s))
        return false;
    setBit(butterflyMap.mutableSpan(), *words);
    Vector<uint8_t> keptMap(FillWith { }, mapBytes, 0);
    setBit(keptMap.mutableSpan(), *mixed);
    setBit(keptMap.mutableSpan(), *prefix);

    IdentitySection programExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Program,
        .provenance = CoreProvenance::Generated,
        .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x41)),
        .contextDigest = filledDigest(0x11),
        .coreDigest = filledDigest(0x22),
        .holderDigest = std::nullopt,
        .functionParseFields = std::nullopt,
        .constantCount = constantCount,
        .atomMap = atomMap.span(),
        .butterflyMap = butterflyMap.span(),
    };
    IdentitySection functionExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Function,
        .provenance = CoreProvenance::EmbedderDecoded,
        .key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, { }, filledDigest(0x42)),
        .contextDigest = filledDigest(0x33),
        .coreDigest = filledDigest(0x44),
        .holderDigest = filledDigest(0x55),
        .functionParseFields = FunctionParseFields { static_cast<CodeFeatures>(ArgumentsFeature | ThisFeature | AsyncFunctionWithoutAwaitFeature), AllLexicallyScopedFeatures, true },
        .constantCount = constantCount,
        .atomMap = atomMap.span(),
        .butterflyMap = keptMap.span(),
    };
    if (!u5.check(identitySectionSize(constantCount) == roundUpToMultipleOf<8>(identityMapsOffset + 2 * mapBytes), "identitySectionSize disagrees with section 4.2"_s))
        return false;

    auto write = [&](const IdentitySection& expected, std::optional<std::span<const uint8_t>> keptButterflyMap) {
        Vector<uint8_t> bytes(identitySectionSize(constantCount));
        writeIdentitySection(bytes.mutableSpan(), vm, expected.key, expected.contextDigest, expected.provenance, expected.coreDigest, expected.holderDigest,
            expected.coreKind, expected.functionParseFields, codeBlock, keptButterflyMap);
        return bytes;
    };
    Vector<uint8_t> programSection = write(programExpected, std::nullopt);
    Vector<uint8_t> functionSection = write(functionExpected, keptMap.span());
    if (!u5.check(identityBytesMatch(programSection.span(), programExpected), "writeIdentitySection did not lay out a program core as section 4.2 does"_s))
        return false;
    if (!u5.check(identityBytesMatch(functionSection.span(), functionExpected), "writeIdentitySection did not lay out a function core with a kept butterfly map as section 4.2 does"_s))
        return false;
    for (bool strict : { true, false }) {
        auto program = parseIdentitySection(programSection.span(), strict);
        auto function = parseIdentitySection(functionSection.span(), strict);
        bool roundTrips = program && sameIdentitySection(*program, programExpected) && function && sameIdentitySection(*function, functionExpected);
        if (!u5.check(roundTrips, strict ? "ucb.identity does not round-trip with strict on"_s : "ucb.identity does not round-trip with strict off"_s))
            return false;
    }

    auto parseIdentity = [](std::span<const uint8_t> bytes, bool strict) {
        return parseIdentitySection(bytes, strict);
    };
    auto rejects = [&](const Vector<uint8_t>& valid, ASCIILiteral rule, bool keepsLayout, const auto& mutate) {
        return strictRejects(u5, "ucb.identity"_s, parseIdentity, valid, rule, keepsLayout, mutate);
    };
    // N % 8 != 0, so the bit at N lies in each map's last byte.
    size_t lastMapByte = constantCount / 8;
    uint8_t bitAtN = 1u << (constantCount % 8);
    size_t mapsEnd = identityMapsOffset + 2 * mapBytes;
    return rejects(functionSection, "a wrong magic"_s, true, [](auto& bytes) { bytes[0] ^= 0xff; })
        && rejects(functionSection, "layout version 2"_s, true, [](auto& bytes) { storeField<uint16_t>(bytes, identityVersionOffset, 2); })
        && rejects(functionSection, "core kind 4"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = 4; })
        && rejects(functionSection, "a program core under a child key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Program); })
        && rejects(programSection, "an eval core under a program key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Eval); })
        && rejects(functionSection, "provenance 2"_s, true, [](auto& bytes) { bytes[identityProvenanceOffset] = 2; })
        && rejects(functionSection, "key version 2"_s, true, [](auto& bytes) { bytes[identityKeyOffset] = 2; })
        && rejects(functionSection, "key identity kind 0"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 1] = 0; })
        && rejects(functionSection, "key identity kind 8"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 1] = 8; })
        && rejects(functionSection, "key specialization 2"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 2] = 2; })
        && rejects(functionSection, "a key mode bit outside CodeGenerationMode"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 3] = 0x80; })
        && rejects(functionSection, "a nonzero reserved key byte"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 7] = 1; })
        && rejects(functionSection, "function features at 1 << bitWidthOfCodeFeatures"_s, true, [](auto& bytes) { storeField<CodeFeatures>(bytes, identityFeaturesOffset, 1u << bitWidthOfCodeFeatures); })
        && rejects(functionSection, "lexically scoped features above AllLexicallyScopedFeatures"_s, true, [](auto& bytes) { bytes[identityLexicallyScopedFeaturesOffset] = AllLexicallyScopedFeatures + 1; })
        && rejects(functionSection, "has-captured-variables 2"_s, true, [](auto& bytes) { bytes[identityHasCapturedVariablesOffset] = 2; })
        && rejects(programSection, "a nonzero holder digest on a program core"_s, true, [](auto& bytes) { bytes[identityHolderDigestOffset + 31] = 1; })
        && rejects(programSection, "function features on a program core"_s, true, [](auto& bytes) { storeField<CodeFeatures>(bytes, identityFeaturesOffset, ArgumentsFeature); })
        && rejects(programSection, "lexically scoped features on a program core"_s, true, [](auto& bytes) { bytes[identityLexicallyScopedFeaturesOffset] = StrictModeLexicallyScopedFeature; })
        && rejects(programSection, "has-captured-variables on a program core"_s, true, [](auto& bytes) { bytes[identityHasCapturedVariablesOffset] = 1; })
        && rejects(programSection, "an atom map bit at N"_s, true, [&](auto& bytes) { bytes[identityMapsOffset + lastMapByte] |= bitAtN; })
        && rejects(programSection, "a butterfly map bit at N"_s, true, [&](auto& bytes) { bytes[identityMapsOffset + mapBytes + lastMapByte] |= bitAtN; })
        && rejects(programSection, "a constant marked in both maps"_s, true, [&](auto& bytes) { setBit(bytes.mutableSpan().subspan(identityMapsOffset + mapBytes, mapBytes), *someAtom); })
        && (programSection.size() == mapsEnd || rejects(programSection, "nonzero padding"_s, true, [&](auto& bytes) { bytes[mapsEnd] = 1; }))
        && rejects(programSection, "a constant count whose maps need eight more bytes"_s, false, [&](auto& bytes) { storeField<uint32_t>(bytes, identityConstantCountOffset, constantCount + 32); })
        && rejects(programSection, "a constant count of 1 << 28"_s, false, [](auto& bytes) { storeField<uint32_t>(bytes, identityConstantCountOffset, sectionCountLimit); })
        && rejects(programSection, "one byte more"_s, true, [](auto& bytes) { bytes.append(static_cast<uint8_t>(0)); })
        && rejects(programSection, "one byte fewer"_s, false, [](auto& bytes) {
            bytes.removeLast();
        });
}

// The seven flags a UCB copy of an array profile can hold: every ArrayProfileFlag but the pruning mark (section 5.2).
static constexpr std::array<ArrayProfileFlag, 7> restorableArrayProfileFlags {
    ArrayProfileFlag::MayStoreHole,
    ArrayProfileFlag::OutOfBounds,
    ArrayProfileFlag::MayBeLargeTypedArray,
    ArrayProfileFlag::MayInterceptIndexedAccesses,
    ArrayProfileFlag::UsesNonOriginalArrayStructures,
    ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray,
    ArrayProfileFlag::MayBeRegExpMatchesArray,
};

// Distinct, well-spread bit patterns for the profile slots.
static uint64_t slotPattern(uint64_t index)
{
    return (index + 1) * 0x9E3779B97F4A7C15ull;
}

static constexpr int32_t fixtureThreshold = 500;
static constexpr double fixtureProgress = 123.25;
static constexpr unsigned fixtureExitSiteCount = 3;

// Gives a generated UCB that no holder has published every kind of feedback section 5.1 says travels, as a run would
// leave it. The UCB is Strong-held, so markers visit it while the test runs.
static void giveFeedback(VM& vm, UnlinkedCodeBlock& codeBlock, uint32_t symbolTableIndex)
{
    auto& valueProfiles = codeBlock.unlinkedValueProfiles();
    for (unsigned i = 0; i < valueProfiles.size(); ++i)
        valueProfiles[i].restorePrediction(slotPattern(i) & SpecBytecodeTop);
    auto& arrayProfiles = codeBlock.unlinkedArrayProfiles();
    for (unsigned i = 0; i < arrayProfiles.size(); ++i) {
        OptionSet<ArrayProfileFlag> flags;
        for (unsigned flag = 0; flag < restorableArrayProfileFlags.size(); ++flag) {
            if ((slotPattern(i) >> (32 + flag)) & 1)
                flags.add(restorableArrayProfileFlags[flag]);
        }
        arrayProfiles[i].restoreAccumulatedState(static_cast<ArrayModes>(slotPattern(i) & ALL_ARRAY_MODES), flags);
    }
    for (unsigned i = 0; i < codeBlock.numberOfBinaryArithProfiles(); ++i)
        codeBlock.binaryArithProfile(i).restoreBits(static_cast<uint16_t>(slotPattern(i) & ((1u << 14) - 1)));
    for (unsigned i = 0; i < codeBlock.numberOfUnaryArithProfiles(); ++i)
        codeBlock.unaryArithProfile(i).restoreBits(static_cast<uint16_t>(slotPattern(i) & ((1u << 10) - 1)));
    {
        Vector<DFG::FrequentExitSite> sites {
            DFG::FrequentExitSite(BytecodeIndex(0), BadType, ExitFromDFG, ExitFromNotInlined),
            DFG::FrequentExitSite(BytecodeIndex(1), Overflow, ExitFromFTL, ExitFromInlined),
            DFG::FrequentExitSite(ArgumentsEscaped, ExitFromDFG, ExitFromNotInlined),
        };
        ConcurrentJSLocker locker(codeBlock.m_lock);
        codeBlock.exitProfile().restoreFrequentExitSites(locker, WTF::move(sites));
    }
    {
        // Under the cell lock, as seedFeedback writes them: a marker's first visit in a cycle rewrites m_age, which shares a
        // memory location with the quick tier-up bits, under that lock (UnlinkedCodeBlock::visitChildrenImpl), and an
        // unlocked write could be lost to its read-modify-write.
        Locker locker { codeBlock.cellLock() };
        codeBlock.setDidOptimize(TriState::True);
        codeBlock.setQuickDFGTierUp(TriState::False);
        codeBlock.setQuickFTLTierUp(true);
    }
    armLLIntCounter(codeBlock.llintExecuteCounter(), fixtureThreshold, fixtureProgress);
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); i += 2)
        codeBlock.functionDecl(i)->setSingletonHasBeenInvalidated();
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); i += 2)
        codeBlock.functionExpr(i)->setSingletonHasBeenInvalidated();
    constantCell<SymbolTable>(codeBlock, symbolTableIndex)->singleton().invalidate(vm, StringFireDetail("JITCache self-test: a scope created twice"));
}

// Whether `bytes` hold the UCB's feedback where section 5.2 puts each field, with zero gaps and padding.
static bool feedbackBytesMatch(std::span<const uint8_t> bytes, UnlinkedCodeBlock& codeBlock, const FeedbackCounts& counts)
{
    FeedbackLayout layout { counts };
    if (bytes.size() != layout.size || loadField<uint32_t>(bytes, 0) != feedbackSectionMagic || loadField<uint16_t>(bytes, feedbackVersionOffset) != 1 || loadField<uint16_t>(bytes, feedbackReservedOffset))
        return false;
    std::array<uint32_t, 8> countValues { counts.valueProfiles, counts.arrayProfiles, counts.binaryArithProfiles, counts.unaryArithProfiles, counts.exitSites, counts.functionDecls, counts.functionExprs, counts.constants };
    for (size_t i = 0; i < countValues.size(); ++i) {
        if (loadField<uint32_t>(bytes, feedbackCountOffsets[i]) != countValues[i])
            return false;
    }
    TierUpHistory history = tierUpHistoryOf(codeBlock);
    if (bytes[feedbackDidOptimizeOffset] != static_cast<uint8_t>(history.didOptimize) || bytes[feedbackQuickDFGTierUpOffset] != static_cast<uint8_t>(history.quickDFGTierUp)
        || bytes[feedbackQuickFTLTierUpOffset] != static_cast<uint8_t>(history.quickFTLTierUp) || bytes[feedbackReservedByteOffset])
        return false;
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    if (loadField<int32_t>(bytes, feedbackThresholdOffset) != counter.m_activeThreshold || loadField<uint32_t>(bytes, feedbackTotalCountOffset) != std::bit_cast<uint32_t>(counter.m_totalCount)
        || loadField<int32_t>(bytes, feedbackCounterOffset) != counter.m_counter)
        return false;

    for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
        if (loadField<SpeculatedType>(bytes, feedbackHeaderSize + sizeof(SpeculatedType) * i) != codeBlock.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        auto& profile = codeBlock.unlinkedArrayProfiles()[i];
        size_t offset = layout.arrayProfiles + 8 * static_cast<size_t>(i);
        if (loadField<uint32_t>(bytes, offset) != profile.observedArrayModes() || loadField<uint32_t>(bytes, offset + 4) != profile.arrayProfileFlags().toRaw())
            return false;
    }
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i) {
        if (loadField<uint16_t>(bytes, layout.binaryArithProfiles + 2 * static_cast<size_t>(i)) != codeBlock.binaryArithProfile(i).bits())
            return false;
    }
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i) {
        if (loadField<uint16_t>(bytes, layout.unaryArithProfiles + 2 * static_cast<size_t>(i)) != codeBlock.unaryArithProfile(i).bits())
            return false;
    }
    if (!isZeroBytes(bytes.subspan(layout.unaryEnd, layout.exitSites - layout.unaryEnd)))
        return false;
    Vector<DFG::FrequentExitSite> sites = exitSitesOf(codeBlock);
    if (sites.size() != counts.exitSites)
        return false;
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        size_t offset = layout.exitSites + exitSiteRecordSize * i;
        const DFG::FrequentExitSite& site = sites[i];
        if (loadField<uint32_t>(bytes, offset) != site.bytecodeIndex().asBits() || bytes[offset + 4] != site.kind() || bytes[offset + 5] != site.jitType()
            || bytes[offset + 6] != site.inlineKind() || bytes[offset + 7])
            return false;
    }
    for (uint32_t i = 0; i < counts.functionDecls; ++i) {
        if (bytes[layout.children + i] != static_cast<uint8_t>(codeBlock.functionDecl(i)->singletonHasBeenInvalidated()))
            return false;
    }
    for (uint32_t i = 0; i < counts.functionExprs; ++i) {
        if (bytes[layout.children + counts.functionDecls + i] != static_cast<uint8_t>(codeBlock.functionExpr(i)->singletonHasBeenInvalidated()))
            return false;
    }
    for (uint32_t i = 0; i < counts.constants; ++i) {
        auto* symbolTable = constantCell<SymbolTable>(codeBlock, i);
        if (bitIsSet(bytes.subspan(layout.constants), i) != (symbolTable && symbolTable->singleton().hasBeenInvalidated()))
            return false;
    }
    if (counts.constants % 8 && (bytes[layout.end - 1] >> (counts.constants % 8)))
        return false;
    return isZeroBytes(bytes.subspan(layout.end));
}

// Whether a parsed section reads back the UCB's feedback through every accessor of section 5.7.
static bool feedbackSectionDescribes(const FeedbackSection& section, UnlinkedCodeBlock& codeBlock)
{
    const FeedbackCounts& counts = section.counts();
    if (counts != liveFeedbackCounts(codeBlock))
        return false;
    for (uint32_t i = 0; i < counts.valueProfiles; ++i) {
        if (section.prediction(i) != codeBlock.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (uint32_t i = 0; i < counts.arrayProfiles; ++i) {
        auto& profile = codeBlock.unlinkedArrayProfiles()[i];
        if (section.observedArrayModes(i) != profile.observedArrayModes() || section.arrayProfileFlags(i) != profile.arrayProfileFlags())
            return false;
    }
    for (uint32_t i = 0; i < counts.binaryArithProfiles; ++i) {
        if (section.binaryArithBits(i) != codeBlock.binaryArithProfile(i).bits())
            return false;
    }
    for (uint32_t i = 0; i < counts.unaryArithProfiles; ++i) {
        if (section.unaryArithBits(i) != codeBlock.unaryArithProfile(i).bits())
            return false;
    }
    Vector<DFG::FrequentExitSite> sites = exitSitesOf(codeBlock);
    for (uint32_t i = 0; i < counts.exitSites; ++i) {
        if (!(section.exitSite(i) == sites[i]))
            return false;
    }
    for (uint32_t i = 0; i < counts.functionDecls; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Declarations, i) != codeBlock.functionDecl(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < counts.functionExprs; ++i) {
        if (section.childSingletonInvalidated(ChildTable::Expressions, i) != codeBlock.functionExpr(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < counts.constants; ++i) {
        auto* symbolTable = constantCell<SymbolTable>(codeBlock, i);
        if (section.constantSingletonInvalidated(i) != (symbolTable && symbolTable->singleton().hasBeenInvalidated()))
            return false;
    }
    TierUpHistory history = tierUpHistoryOf(codeBlock);
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    return section.didOptimize() == history.didOptimize && section.quickDFGTierUp() == history.quickDFGTierUp && section.quickFTLTierUp() == history.quickFTLTierUp
        && section.llintActiveThreshold() == counter.m_activeThreshold && section.llintProgress() == counter.count();
}

// Whether two UCBs of one program hold the same feedback, the list T4 compares: what seeding must leave in the UCB it seeds.
// Exit sites compare as a set, and the counter's progress up to the float rounding of m_totalCount.
static bool sameFeedback(UnlinkedCodeBlock& expected, UnlinkedCodeBlock& actual)
{
    if (liveFeedbackCounts(expected) != liveFeedbackCounts(actual))
        return false;
    for (unsigned i = 0; i < expected.numberOfValueProfiles(); ++i) {
        if (expected.unlinkedValueProfiles()[i].prediction() != actual.unlinkedValueProfiles()[i].prediction())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfArrayProfiles(); ++i) {
        auto& expectedProfile = expected.unlinkedArrayProfiles()[i];
        auto& actualProfile = actual.unlinkedArrayProfiles()[i];
        if (expectedProfile.observedArrayModes() != actualProfile.observedArrayModes() || expectedProfile.arrayProfileFlags() != actualProfile.arrayProfileFlags())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfBinaryArithProfiles(); ++i) {
        if (expected.binaryArithProfile(i).bits() != actual.binaryArithProfile(i).bits())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfUnaryArithProfiles(); ++i) {
        if (expected.unaryArithProfile(i).bits() != actual.unaryArithProfile(i).bits())
            return false;
    }
    Vector<DFG::FrequentExitSite> expectedSites = exitSitesOf(expected);
    Vector<DFG::FrequentExitSite> actualSites = exitSitesOf(actual);
    bool sameSites = expectedSites.size() == actualSites.size() && std::ranges::all_of(expectedSites, [&](const DFG::FrequentExitSite& site) {
        return actualSites.contains(site);
    });
    if (!sameSites)
        return false;
    if (tierUpHistoryOf(expected) != tierUpHistoryOf(actual))
        return false;
    BaselineExecutionCounter& expectedCounter = expected.llintExecuteCounter();
    BaselineExecutionCounter& actualCounter = actual.llintExecuteCounter();
    if (expectedCounter.m_activeThreshold != actualCounter.m_activeThreshold || !equalUpToFloatRounding(expectedCounter.count(), actualCounter.count(), expectedCounter.m_totalCount))
        return false;
    for (unsigned i = 0; i < expected.numberOfFunctionDecls(); ++i) {
        if (expected.functionDecl(i)->singletonHasBeenInvalidated() != actual.functionDecl(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (unsigned i = 0; i < expected.numberOfFunctionExprs(); ++i) {
        if (expected.functionExpr(i)->singletonHasBeenInvalidated() != actual.functionExpr(i)->singletonHasBeenInvalidated())
            return false;
    }
    for (uint32_t i = 0; i < expected.constantRegisters().size(); ++i) {
        auto* expectedTable = constantCell<SymbolTable>(expected, i);
        auto* actualTable = constantCell<SymbolTable>(actual, i);
        if (!!expectedTable != !!actualTable || (expectedTable && expectedTable->singleton().hasBeenInvalidated() != actualTable->singleton().hasBeenInvalidated()))
            return false;
    }
    return true;
}

// U5: ucb.feedback. A UCB given every kind of feedback writes a section laid out as section 5.2 says, which round-trips
// with strict on and off and seeds a second generation of the same program to the same feedback (section 5.3); the
// richness of the live UCB and of the saved section agree (section 5.6); C3, C7 and C9 refuse a UCB the section does not
// fit; and strict parsing rejects each rule of section 5.2 broken once.
static bool testFeedbackSection(VM& vm, SelfTestPart& u5)
{
    auto producer = generateSectionFixture(vm);
    if (!u5.check(!!producer, "the section fixture program failed to generate"_s))
        return false;
    ProgramFixture consumer = generateFixture(vm, producer->text);
    ProgramFixture unrelated = generateFixture(vm, "0;"_s);
    if (!u5.check(consumer.codeBlock && unrelated.codeBlock, "a test program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& source = *producer->codeBlock.get();
    UnlinkedCodeBlock& target = *consumer.codeBlock.get();
    auto symbolTable = symbolTableConstantIndex(source);
    bool coversEveryArray = source.numberOfValueProfiles() && source.numberOfArrayProfiles() && source.numberOfBinaryArithProfiles() && source.numberOfUnaryArithProfiles()
        && source.numberOfFunctionDecls() && source.numberOfFunctionExprs() && symbolTable;
    if (!u5.check(coversEveryArray, "the fixture program lacks a profile, a child table or a SymbolTable constant"_s))
        return false;
    if (!u5.check(liveFeedbackCounts(source) == liveFeedbackCounts(target), "two generations of one program differ in their feedback counts"_s))
        return false;

    giveFeedback(vm, source, *symbolTable);
    FeedbackCounts counts = liveFeedbackCounts(source);
    FeedbackLayout layout { counts };
    if (!u5.check(counts.exitSites == fixtureExitSiteCount && feedbackSectionSize(counts) == layout.size, "feedbackSectionSize disagrees with section 5.2"_s))
        return false;
    Vector<uint8_t> section(feedbackSectionSize(counts));
    writeFeedbackSection(section.mutableSpan(), source, counts);
    if (!u5.check(feedbackBytesMatch(section.span(), source, counts), "writeFeedbackSection did not lay out the feedback as section 5.2 does"_s))
        return false;
    for (bool strict : { true, false }) {
        auto parsed = FeedbackSection::parse(section.span(), strict);
        if (!u5.check(parsed && feedbackSectionDescribes(*parsed, source), strict ? "ucb.feedback does not round-trip with strict on"_s : "ucb.feedback does not round-trip with strict off"_s))
            return false;
    }

    // Richness (section 5.6): a binary profile's 13 low bits, a unary profile's 10, and one unit per exit site.
    UCBRichness expectedRichness;
    for (unsigned i = 0; i < source.numberOfBinaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.binaryArithProfile(i).bits() & ((1u << 13) - 1)));
    for (unsigned i = 0; i < source.numberOfUnaryArithProfiles(); ++i)
        expectedRichness.arithmeticUnits += static_cast<uint64_t>(std::popcount(source.unaryArithProfile(i).bits() & ((1u << 10) - 1)));
    expectedRichness.exitSiteUnits = fixtureExitSiteCount;
    UCBRichness live = liveRichness(source);
    auto saved = savedRichness(section.span(), true);
    bool richnessAgrees = live.arithmeticUnits == expectedRichness.arithmeticUnits && live.exitSiteUnits == expectedRichness.exitSiteUnits
        && saved && saved->arithmeticUnits == expectedRichness.arithmeticUnits && saved->exitSiteUnits == expectedRichness.exitSiteUnits;
    if (!u5.check(richnessAgrees, "liveRichness or savedRichness does not count section 5.6's units"_s))
        return false;
    Vector<uint8_t> brokenMagic = section;
    brokenMagic[0] ^= 0xff;
    if (!u5.check(!savedRichness(brokenMagic.span(), true) && savedRichness(brokenMagic.span(), false), "savedRichness does not answer as FeedbackSection::parse does"_s))
        return false;

    // Seeding (section 5.3) a second generation of the program, which no holder has published.
    auto parsed = FeedbackSection::parse(section.span(), true);
    if (!u5.check(parsed && feedbackFits(target, *parsed) && constantBitsFit(target, *parsed), "C3, C7 or C9 refuses a UCB its section fits"_s))
        return false;
    if (!u5.check(!feedbackFits(*unrelated.codeBlock.get(), *parsed), "C3 accepts a UCB with other counts"_s))
        return false;
    seedFeedback(vm, target, *parsed);
    if (!u5.check(sameFeedback(source, target), "seedFeedback did not leave the captured feedback in the seeded UCB"_s))
        return false;
    BaselineExecutionCounter& seededCounter = target.llintExecuteCounter();
    if (!u5.check(seededCounter.m_activeThreshold == fixtureThreshold && equalUpToFloatRounding(seededCounter.count(), fixtureProgress, seededCounter.m_totalCount), "seedFeedback did not arm the LLInt counter with the captured threshold and progress"_s))
        return false;

    // C7: an exit site at or past the end of the instruction stream.
    Vector<uint8_t> pastTheStream = section;
    storeField<uint32_t>(pastTheStream, layout.exitSites, BytecodeIndex(source.instructionsSize()).asBits());
    auto pastTheStreamSection = FeedbackSection::parse(pastTheStream.span(), true);
    if (!u5.check(pastTheStreamSection && !feedbackFits(source, *pastTheStreamSection), "C7 accepts an exit site past the instruction stream"_s))
        return false;
    // C9: a singleton bit on a constant that is not a SymbolTable.
    auto stringIndex = stringConstantIndex(source, "jitcache-u5-atom-constant"_s);
    if (!u5.check(!!stringIndex, "the fixture program lacks its string constants"_s))
        return false;
    Vector<uint8_t> notATable = section;
    setBit(notATable.mutableSpan().subspan(layout.constants), *stringIndex);
    auto notATableSection = FeedbackSection::parse(notATable.span(), true);
    if (!u5.check(notATableSection && !constantBitsFit(source, *notATableSection), "C9 accepts a singleton bit on a string constant"_s))
        return false;

    // The two counter states section 5.2 admits besides the fixture's.
    auto setCounter = [](Vector<uint8_t>& bytes, int32_t threshold, float totalCount, int32_t counter) {
        storeField<int32_t>(bytes, feedbackThresholdOffset, threshold);
        storeField<float>(bytes, feedbackTotalCountOffset, totalCount);
        storeField<int32_t>(bytes, feedbackCounterOffset, counter);
    };
    auto acceptsCounter = [&](int32_t threshold, float totalCount, int32_t counter) {
        Vector<uint8_t> crafted = section;
        setCounter(crafted, threshold, totalCount, counter);
        return !!FeedbackSection::parse(crafted.span(), true);
    };
    if (!u5.check(acceptsCounter(std::numeric_limits<int32_t>::max(), 0, std::numeric_limits<int32_t>::min()) && acceptsCounter(0, 0, 0), "strict ucb.feedback parsing refuses a deferred or a zero counter"_s))
        return false;

    auto parseFeedback = [](std::span<const uint8_t> bytes, bool strict) {
        return FeedbackSection::parse(bytes, strict);
    };
    // Parsing with strict off reads only the header, so every crafted section below keeps its layout readable.
    auto rejects = [&](ASCIILiteral rule, const auto& mutate) {
        return strictRejects(u5, "ucb.feedback"_s, parseFeedback, section, rule, true, mutate);
    };
    for (size_t offset : feedbackCountOffsets) {
        if (!rejects("a count of 1 << 28"_s, [&](auto& bytes) { storeField<uint32_t>(bytes, offset, sectionCountLimit); }))
            return false;
    }
    ArrayModes modesOutside = ~static_cast<ArrayModes>(ALL_ARRAY_MODES);
    size_t firstSite = layout.exitSites;
    size_t secondSite = layout.exitSites + exitSiteRecordSize;
    return rejects("a wrong magic"_s, [](auto& bytes) { bytes[0] ^= 0xff; })
        && rejects("layout version 2"_s, [](auto& bytes) { storeField<uint16_t>(bytes, feedbackVersionOffset, 2); })
        && rejects("a nonzero reserved half-word"_s, [](auto& bytes) { bytes[feedbackReservedOffset + 1] = 1; })
        && rejects("one byte more"_s, [](auto& bytes) { bytes.append(static_cast<uint8_t>(0)); })
        && rejects("one byte fewer"_s, [](auto& bytes) { bytes.removeLast(); })
        && rejects("a prediction with SpecInt52Any"_s, [](auto& bytes) { storeField<SpeculatedType>(bytes, feedbackHeaderSize, loadField<SpeculatedType>(bytes.span(), feedbackHeaderSize) | SpecInt52Any); })
        && rejects("a prediction with SpecDoubleImpureNaN"_s, [](auto& bytes) { storeField<SpeculatedType>(bytes, feedbackHeaderSize, loadField<SpeculatedType>(bytes.span(), feedbackHeaderSize) | SpecDoubleImpureNaN); })
        && (!modesOutside || rejects("array modes outside ALL_ARRAY_MODES"_s, [&](auto& bytes) { storeField<uint32_t>(bytes, layout.arrayProfiles, loadField<uint32_t>(bytes.span(), layout.arrayProfiles) | (1u << std::countr_zero(modesOutside))); }))
        && rejects("the pruning mark among array flags"_s, [&](auto& bytes) { storeField<uint32_t>(bytes, layout.arrayProfiles + 4, loadField<uint32_t>(bytes.span(), layout.arrayProfiles + 4) | static_cast<uint32_t>(ArrayProfileFlag::DidPerformFirstRunPruning)); })
        && rejects("an array flag past the eight"_s, [&](auto& bytes) { storeField<uint32_t>(bytes, layout.arrayProfiles + 4, loadField<uint32_t>(bytes.span(), layout.arrayProfiles + 4) | (1u << 8)); })
        && rejects("binary arithmetic bits at 1 << 14"_s, [&](auto& bytes) { storeField<uint16_t>(bytes, layout.binaryArithProfiles, 1u << 14); })
        && rejects("unary arithmetic bits at 1 << 10"_s, [&](auto& bytes) { storeField<uint16_t>(bytes, layout.unaryArithProfiles, 1u << 10); })
        && rejects("exit kind ExitKindUnset"_s, [&](auto& bytes) { bytes[firstSite + 4] = ExitKindUnset; })
        && rejects("an exit kind past the last"_s, [&](auto& bytes) { bytes[firstSite + 4] = static_cast<uint8_t>(UnexpectedResizableArrayBufferView) + 1; })
        && rejects("exiting JIT type ExitFromAnything"_s, [&](auto& bytes) { bytes[firstSite + 5] = ExitFromAnything; })
        && rejects("an exiting JIT type past ExitFromFTL"_s, [&](auto& bytes) { bytes[firstSite + 5] = static_cast<uint8_t>(ExitFromFTL) + 1; })
        && rejects("inline kind ExitFromAnyInlineKind"_s, [&](auto& bytes) { bytes[firstSite + 6] = ExitFromAnyInlineKind; })
        && rejects("an inline kind past ExitFromInlined"_s, [&](auto& bytes) { bytes[firstSite + 6] = static_cast<uint8_t>(ExitFromInlined) + 1; })
        && rejects("a nonzero exit-site pad byte"_s, [&](auto& bytes) { bytes[firstSite + 7] = 1; })
        && rejects("two equal exit sites"_s, [&](auto& bytes) { memcpySpan(bytes.mutableSpan().subspan(secondSite, exitSiteRecordSize), bytes.span().subspan(firstSite, exitSiteRecordSize)); })
        && (layout.exitSites == layout.unaryEnd || rejects("a nonzero alignment gap"_s, [&](auto& bytes) { bytes[layout.unaryEnd] = 1; }))
        && rejects("didOptimize 3"_s, [](auto& bytes) { bytes[feedbackDidOptimizeOffset] = 3; })
        && rejects("quick DFG tier-up 3"_s, [](auto& bytes) { bytes[feedbackQuickDFGTierUpOffset] = 3; })
        && rejects("quick FTL tier-up 2"_s, [](auto& bytes) { bytes[feedbackQuickFTLTierUpOffset] = 2; })
        && rejects("a nonzero byte 39"_s, [](auto& bytes) { bytes[feedbackReservedByteOffset] = 1; })
        && rejects("a negative counter threshold"_s, [&](auto& bytes) { setCounter(bytes, -1, 499.25f, -376); })
        && rejects("a NaN counter total"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, std::numeric_limits<float>::quiet_NaN(), 0); })
        && rejects("an infinite counter total"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, std::numeric_limits<float>::infinity(), 0); })
        && rejects("a negative counter progress"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, 1, -5); })
        && rejects("a counter progress of 2^31"_s, [&](auto& bytes) { setCounter(bytes, fixtureThreshold, 0x1p31f, 0); })
        && rejects("a deferred threshold with a nonzero total"_s, [&](auto& bytes) { setCounter(bytes, std::numeric_limits<int32_t>::max(), 1, std::numeric_limits<int32_t>::min()); })
        && rejects("a child bit of 2"_s, [&](auto& bytes) { bytes[layout.children] = 2; })
        && rejects("a constant bit at C"_s, [&](auto& bytes) { setBit(bytes.mutableSpan().subspan(layout.constants), counts.constants); })
        && (layout.size == layout.end || rejects("nonzero final padding"_s, [&](auto& bytes) {
            bytes[layout.end] = 1;
        }));
}

// U5: atomizeMarkedConstants makes a plain-string constant an atom marked as one, leaves an atom and the unmarked
// constants as they are, and allocates no cell (section 7.3.3, step 5). C8 and C10 answer before and after.
static bool testAtomization(VM& vm, SelfTestPart& u5)
{
    auto fixture = generateSectionFixture(vm);
    if (!u5.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(codeBlock.constantRegisters().size());
    size_t mapBytes = (static_cast<size_t>(constantCount) + 7) / 8;
    auto plainIndex = stringConstantIndex(codeBlock, "jitcache-u5-plain-constant"_s);
    auto atomIndex = stringConstantIndex(codeBlock, "jitcache-u5-atom-constant"_s);
    auto unmarkedIndex = stringConstantIndex(codeBlock, "jitcache-u5-unmarked-constant"_s);
    auto symbolTable = symbolTableConstantIndex(codeBlock);
    auto mixed = butterflyConstantIndex(codeBlock, "alpha"_s, 2);
    if (!u5.check(plainIndex && atomIndex && unmarkedIndex && symbolTable && mixed, "the fixture program lacks a constant the atomization test reads"_s))
        return false;

    // A natively decoded UCB can hold plain-string constants (F19); two such strings take the place of generation's atoms.
    JSString* plain = jsString(vm, makeString("jitcache-u5-plain-"_s, "runtime-string"_s));
    JSString* unmarked = jsString(vm, makeString("jitcache-u5-unmarked-"_s, "runtime-string"_s));
    codeBlock.constantRegister(constantRegisterFor(*plainIndex)).set(vm, &codeBlock, plain);
    codeBlock.constantRegister(constantRegisterFor(*unmarkedIndex)).set(vm, &codeBlock, unmarked);
    JSString* atom = constantCell<JSString>(codeBlock, *atomIndex);
    StringImpl* atomImpl = atom->tryGetValueImpl();
    bool preconditions = !plain->tryGetValueImpl()->isAtom() && !plain->isDefinitelyAtom() && !unmarked->tryGetValueImpl()->isAtom() && atomImpl->isAtom();
    if (!u5.check(preconditions, "the runtime strings of the atomization test are atoms already"_s))
        return false;

    Vector<uint8_t> atomMap(FillWith { }, mapBytes, 0);
    Vector<uint8_t> butterflyMap(FillWith { }, mapBytes, 0);
    setBit(atomMap.mutableSpan(), *plainIndex);
    setBit(atomMap.mutableSpan(), *atomIndex);
    auto identityWith = [&](std::span<const uint8_t> atoms, std::span<const uint8_t> butterflies, uint32_t count) {
        return IdentitySection {
            .coreKind = UnlinkedCodeBlockCoreKind::Program,
            .provenance = CoreProvenance::Generated,
            .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x61)),
            .contextDigest = { },
            .coreDigest = { },
            .holderDigest = std::nullopt,
            .functionParseFields = std::nullopt,
            .constantCount = count,
            .atomMap = atoms,
            .butterflyMap = butterflies,
        };
    };
    IdentitySection identity = identityWith(atomMap.span(), butterflyMap.span(), constantCount);

    // C8 holds; C10 fails until the plain string is an atom.
    if (!u5.check(constantMapsFit(codeBlock, identity) && !markedConstantsAreAtoms(codeBlock, identity), "C8 refuses, or C10 accepts, a marked plain-string constant"_s))
        return false;
    // C8 refuses a map that marks the wrong kind of constant, and a constant count that differs.
    Vector<uint8_t> tableAsAtom(FillWith { }, mapBytes, 0);
    setBit(tableAsAtom.mutableSpan(), *symbolTable);
    Vector<uint8_t> stringAsButterfly(FillWith { }, mapBytes, 0);
    setBit(stringAsButterfly.mutableSpan(), *atomIndex);
    Vector<uint8_t> mixedAsButterfly(FillWith { }, mapBytes, 0);
    setBit(mixedAsButterfly.mutableSpan(), *mixed);
    bool refusesMisfits = !constantMapsFit(codeBlock, identityWith(tableAsAtom.span(), butterflyMap.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(butterflyMap.span(), stringAsButterfly.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(butterflyMap.span(), mixedAsButterfly.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(atomMap.span(), butterflyMap.span(), constantCount + 1));
    if (!u5.check(refusesMisfits, "C8 accepts a map that marks a constant of the wrong kind, or another constant count"_s))
        return false;

    Vector<EncodedJSValue> before;
    for (auto& constant : codeBlock.constantRegisters())
        before.append(JSValue::encode(constant.get()));
    // Allocating a cell outside a GC deferral asserts under AssertNoGC in the debug builds the self-test runs in
    // (tryAllocateCellHelper), so the call below proves it allocates none.
    if (!u5.check(!vm.heap.isDeferred(), "the self-test runs with GC deferred, where AssertNoGC cannot catch an allocation"_s))
        return false;
    {
        AssertNoGC assertNoGC;
        atomizeMarkedConstants(vm, codeBlock, identity);
    }
    bool sameCells = true;
    for (uint32_t i = 0; i < constantCount; ++i)
        sameCells &= JSValue::encode(codeBlock.constantRegisters()[i].get()) == before[i];
    if (!u5.check(sameCells, "atomizeMarkedConstants replaced a constant instead of atomizing it in place"_s))
        return false;
    if (!u5.check(plain->tryGetValueImpl()->isAtom() && plain->isDefinitelyAtom(), "atomizeMarkedConstants left a marked plain string plain or unmarked as an atom"_s))
        return false;
    if (!u5.check(atom->tryGetValueImpl() == atomImpl && atom->isDefinitelyAtom(), "atomizeMarkedConstants changed a marked atom's value or left it unmarked"_s))
        return false;
    if (!u5.check(!unmarked->tryGetValueImpl()->isAtom() && !unmarked->isDefinitelyAtom(), "atomizeMarkedConstants atomized an unmarked constant"_s))
        return false;
    return u5.check(markedConstantsAreAtoms(codeBlock, identity), "C10 refuses constants that atomizeMarkedConstants made atoms"_s);
}

// Calls Array.prototype[name] on `array` with one argument through the native call path; empty if it throws.
static std::optional<JSValue> callArrayMethod(JSGlobalObject* globalObject, JSArray* array, ASCIILiteral name, JSValue argument)
{
    VM& vm = globalObject->vm();
    JSValue function = globalObject->arrayPrototype()->getDirect(vm, Identifier::fromString(vm, name));
    if (!function)
        return std::nullopt;
    auto callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None)
        return std::nullopt;
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(globalObject, function, callData, array, arguments, exception);
    if (exception)
        return std::nullopt;
    return result;
}

// U5: on a UCB decoded from a generated one's core, rebuildAtomStringButterflies gives each marked butterfly the atom form
// with the VM's canonical JSString for every element, as generation does, leaves the unmarked butterflies plain and the
// core encoding unchanged (section 7.3.1, step 8); indexOf and includes over an array made from a rebuilt butterfly, as
// new_array_buffer makes it, find every element and miss a string the map does not hold.
static bool testButterflyRebuild(VM& vm, SelfTestPart& u5)
{
    auto fixture = generateSectionFixture(vm);
    if (!u5.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& generated = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(generated.constantRegisters().size());
    auto words = butterflyConstantIndex(generated, "alpha"_s, 3);
    auto mixed = butterflyConstantIndex(generated, "alpha"_s, 2);
    auto prefix = butterflyConstantIndex(generated, "delta"_s, 2);
    if (!u5.check(words && mixed && prefix, "the fixture program lacks one of its array literals"_s))
        return false;

    // The identity section a capture of the generated UCB writes, whose butterfly map marks the literal in the atom form.
    Vector<uint8_t> identityBytes(identitySectionSize(constantCount));
    writeIdentitySection(identityBytes.mutableSpan(), vm, BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x71)), filledDigest(0x72),
        CoreProvenance::Generated, filledDigest(0x73), std::nullopt, UnlinkedCodeBlockCoreKind::Program, std::nullopt, generated);
    auto identity = parseIdentitySection(identityBytes.span(), true);
    bool marksAtomForm = identity && bitIsSet(identity->butterflyMap, *words) && !bitIsSet(identity->butterflyMap, *mixed) && !bitIsSet(identity->butterflyMap, *prefix);
    if (!u5.check(marksAtomForm, "the butterfly map of a generated UCB does not mark exactly its literal in the atom form"_s))
        return false;

    CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, generated, nullptr, nullptr, encodeFailure);
    if (!u5.check(!!core, "encodeUnlinkedCodeBlockCore failed on the fixture program"_s))
        return false;
    CoreDecodeFailure decodeFailure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* decodedCodeBlock = decodeUnlinkedCodeBlockCore(vm, Ref<CachedBytecode> { *core }, *fixture->source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, false, decodeFailure);
    Strong<UnlinkedCodeBlock> decoded { vm, decodedCodeBlock };
    if (!u5.check(decodedCodeBlock && decodeFailure == CoreDecodeFailure::None, "decodeUnlinkedCodeBlockCore failed on the fixture program's core"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *decodedCodeBlock;
    if (!u5.check(codeBlock.constantRegisters().size() == constantCount, "the decoded fixture program has another constant count"_s))
        return false;

    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    auto* decodedWords = constantCell<JSCellButterfly>(codeBlock, *words);
    auto* decodedMixed = constantCell<JSCellButterfly>(codeBlock, *mixed);
    auto* decodedPrefix = constantCell<JSCellButterfly>(codeBlock, *prefix);
    // The decode gives every butterfly the plain form (SPEC-ucb.codec.md, E10); otherwise the rebuild would be untested.
    if (!u5.check(decodedWords && decodedMixed && decodedPrefix && decodedWords->structure() != atomStringsStructure, "the decode did not leave the all-string literal in the plain form"_s))
        return false;
    if (!u5.check(constantMapsFit(codeBlock, *identity), "C8 refuses the UCB decoded from the generated one's core"_s))
        return false;

    Digest256 encodingBefore = coreDigestOf(vm, codeBlock, nullptr);
    {
        DeferGC deferGC(vm);
        rebuildAtomStringButterflies(vm, codeBlock, *identity);
    }
    if (!u5.check(coreDigestOf(vm, codeBlock, nullptr) == encodingBefore, "rebuildAtomStringButterflies changed the core encoding"_s))
        return false;

    auto* rebuilt = constantCell<JSCellButterfly>(codeBlock, *words);
    auto* original = constantCell<JSCellButterfly>(generated, *words);
    bool atomForm = rebuilt && rebuilt != decodedWords && rebuilt->structure() == atomStringsStructure && rebuilt->indexingMode() == original->indexingMode() && rebuilt->length() == original->length();
    if (!u5.check(atomForm, "rebuildAtomStringButterflies did not give the marked butterfly the atom form"_s))
        return false;
    for (unsigned i = 0; i < rebuilt->length(); ++i) {
        JSValue element = rebuilt->get(i);
        auto* string = element.isString() ? asString(element) : nullptr;
        StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
        bool canonical = impl && impl->isAtom() && string->isDefinitelyAtom() && vm.atomStringToJSStringMap.get(impl) == string && element == original->get(i);
        if (!u5.check(canonical, "a rebuilt element is not the JSString atomStringToJSStringMap holds for its atom, as generation's is"_s))
            return false;
    }
    bool unmarkedStayPlain = constantCell<JSCellButterfly>(codeBlock, *mixed) == decodedMixed && decodedMixed->structure() == vm.cellButterflyStructure(decodedMixed->indexingMode())
        && constantCell<JSCellButterfly>(codeBlock, *prefix) == decodedPrefix && decodedPrefix->structure() == vm.cellButterflyStructure(decodedPrefix->indexingMode());
    if (!u5.check(unmarkedStayPlain, "rebuildAtomStringButterflies changed an unmarked butterfly"_s))
        return false;
    if (!u5.check(constantMapsFit(codeBlock, *identity), "C8 refuses the rebuilt UCB"_s))
        return false;

    // The array new_array_buffer makes from the rebuilt butterfly, searched through the atom fast paths of indexOf and
    // includes (F23).
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    if (!u5.check(!!globalObject, "the self-test runs outside a VM entry, so it has no global object to search arrays in"_s))
        return false;
    JSArray* array = CommonSlowPaths::allocateNewArrayBuffer(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(rebuilt->indexingMode()), rebuilt);
    if (!u5.check(isCopyOnWrite(array->indexingMode()) && JSCellButterfly::isOnlyAtomStringsStructure(vm, array->butterfly()), "the array made from the rebuilt butterfly does not share it copy-on-write"_s))
        return false;
    for (unsigned i = 0; i < rebuilt->length(); ++i) {
        JSValue element = rebuilt->get(i);
        // The element itself, and a plain string with its characters, as a run-time search builds one.
        JSValue copy = jsString(vm, makeString(StringView { *asString(element)->tryGetValueImpl() }));
        for (JSValue search : { element, copy }) {
            auto index = callArrayMethod(globalObject, array, "indexOf"_s, search);
            auto included = callArrayMethod(globalObject, array, "includes"_s, search);
            bool found = index && index->isNumber() && index->asNumber() == i && included && included->isTrue();
            if (!u5.check(found, "indexOf or includes misses an element of a rebuilt butterfly"_s))
                return false;
        }
    }
    JSValue absent = jsString(vm, makeString("jitcache-u5-"_s, "absent-from-every-literal"_s));
    auto absentIndex = callArrayMethod(globalObject, array, "indexOf"_s, absent);
    auto absentIncluded = callArrayMethod(globalObject, array, "includes"_s, absent);
    bool missed = absentIndex && absentIndex->isNumber() && absentIndex->asNumber() == -1 && absentIncluded && absentIncluded->isFalse();
    return u5.check(missed, "indexOf or includes finds, in a rebuilt butterfly, a string the map does not hold"_s);
}

// U5 (section 13.1).
static bool testSections(VM& vm, String& failure)
{
    SelfTestPart u5 { "U5"_s, failure };
    return testIdentitySection(vm, u5) && testFeedbackSection(vm, u5) && testAtomization(vm, u5) && testButterflyRebuild(vm, u5);
}

// U6 (section 13.1): armLLIntCounter keeps the captured threshold and progress, arms the slice native setThreshold arms
// from them (section 5.5), and arms a deferred record as deferIndefinitely leaves it.
static bool testLLIntCounter(String& failure)
{
    SelfTestPart u6 { "U6"_s, failure };
    unsigned nativeArmings = 0;
    for (int32_t threshold : { 0, 100, 500 }) {
        double t = threshold;
        for (double progress : { 0.0, 0.37, 1.0, 99.63, t - 0.5, t - 1, t, t + 5, 1e6 + 0.25 }) {
            // Only the progress section 5.2 accepts.
            if (progress < 0 || progress >= 0x1p31)
                continue;
            BaselineExecutionCounter counter;
            armLLIntCounter(counter, threshold, progress);
            double remaining = t - progress;
            int32_t slice = remaining > 0 ? static_cast<int32_t>(BaselineExecutionCounter::clippedThreshold(nullptr, remaining)) : 0;
            bool armed = counter.m_activeThreshold == threshold && counter.m_counter == -slice && equalUpToFloatRounding(counter.count(), progress, progress + slice);
            if (!u6.check(armed, makeString("armLLIntCounter with threshold "_s, threshold, " did not keep the progress or arm the slice of section 5.5"_s)))
                return false;
            // Native setThreshold from the same threshold and progress, wherever the public path reaches it: a counter
            // that has not crossed re-arms there, and then crosses after the same number of points.
            BaselineExecutionCounter native;
            native.m_activeThreshold = threshold;
            native.m_totalCount = static_cast<float>(progress);
            native.m_counter = 0;
            if (!native.checkIfThresholdCrossedAndSet(nullptr)) {
                ++nativeArmings;
                if (!u6.check(native.m_counter == counter.m_counter, makeString("armLLIntCounter with threshold "_s, threshold, " crosses after another number of points than native setThreshold"_s)))
                    return false;
            }
        }
    }
    if (!u6.check(!!nativeArmings, "no case reached native setThreshold, so the slice went unchecked against it"_s))
        return false;

    BaselineExecutionCounter deferred;
    armLLIntCounter(deferred, std::numeric_limits<int32_t>::max(), -0x1p31);
    BaselineExecutionCounter reference;
    reference.deferIndefinitely();
    bool deferredExactly = deferred.m_activeThreshold == std::numeric_limits<int32_t>::max() && deferred.m_counter == std::numeric_limits<int32_t>::min()
        && std::bit_cast<uint32_t>(deferred.m_totalCount) == std::bit_cast<uint32_t>(0.0f) && deferred.count() == -0x1p31
        && reference.m_activeThreshold == deferred.m_activeThreshold && reference.m_counter == deferred.m_counter && std::bit_cast<uint32_t>(reference.m_totalCount) == std::bit_cast<uint32_t>(deferred.m_totalCount);
    return u6.check(deferredExactly, "armLLIntCounter did not leave a deferred record as deferIndefinitely does"_s);
}

// Helpers shared by U2, U3, U7, U8 and U9.

static Digest256 digestFromHex(ASCIILiteral hex)
{
    Digest256 digest { };
    auto characters = hex.span8();
    RELEASE_ASSERT(characters.size() == 2 * digest.size());
    for (size_t i = 0; i < digest.size(); ++i)
        digest[i] = toASCIIHexValue(characters[2 * i], characters[2 * i + 1]);
    return digest;
}

// The bytes a hex record spells, skipping the spaces that separate its fields.
static Vector<uint8_t> bytesFromHex(ASCIILiteral hex)
{
    Vector<uint8_t> bytes;
    std::optional<uint8_t> highNibble;
    for (auto character : hex.span8()) {
        if (character == ' ')
            continue;
        RELEASE_ASSERT(isASCIIHexDigit(character));
        uint8_t nibble = toASCIIHexValue(character);
        if (!highNibble) {
            highNibble = nibble;
            continue;
        }
        bytes.append(static_cast<uint8_t>(*highNibble << 4 | nibble));
        highNibble = std::nullopt;
    }
    RELEASE_ASSERT(!highNibble);
    return bytes;
}

// Section 3.5's encoding written out byte by byte, apart from sourceDigest: u8 1 then the Latin-1 bytes when every unit is
// at most 0xFF, u8 2 then UTF-16LE otherwise.
static Digest256 referenceSourceDigest(StringView text)
{
    bool isLatin1 = true;
    for (unsigned i = 0; i < text.length(); ++i)
        isLatin1 &= text[i] <= 0xFF;
    Vector<uint8_t> bytes;
    bytes.append(isLatin1 ? 1 : 2);
    for (unsigned i = 0; i < text.length(); ++i) {
        char16_t unit = text[i];
        bytes.append(static_cast<uint8_t>(unit));
        if (!isLatin1)
            bytes.append(static_cast<uint8_t>(unit >> 8));
    }
    return SHA256::hash(bytes.span());
}

// `length` 8-bit units mixing printable ASCII with the upper half of Latin-1.
static String latin1TestText(unsigned length)
{
    Vector<Latin1Character> characters;
    characters.reserveInitialCapacity(length);
    for (unsigned i = 0; i < length; ++i)
        characters.append(static_cast<Latin1Character>(i % 3 ? 0x20 + (i * 7) % 0x5F : 0xA0 + (i * 13) % 0x60));
    return String { characters.span() };
}

// A 16-bit string with the characters of `text`, its last unit replaced by `lastUnit` when given.
static String sixteenBitCopy(StringView text, std::optional<char16_t> lastUnit = std::nullopt)
{
    Vector<char16_t> units;
    units.reserveInitialCapacity(text.length());
    for (unsigned i = 0; i < text.length(); ++i)
        units.append(text[i]);
    if (lastUnit && !units.isEmpty())
        units.last() = *lastUnit;
    return String { units.span() };
}

// A provider that supplies a digest for its text (form 2 of section 3.5), as Bun's providers for compiled modules do. The
// digest is whatever the test gives, so a test can supply one that differs from the text.
class SuppliedDigestProvider final : public StringSourceProvider {
public:
    static Ref<SuppliedDigestProvider> create(const String& text, const std::optional<Digest256>& digest)
    {
        return adoptRef(*new SuppliedDigestProvider(text, digest));
    }

    std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const final { return m_digest; }

private:
    SuppliedDigestProvider(const String& text, const std::optional<Digest256>& digest)
        : StringSourceProvider(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Program)
        , m_digest(digest)
    {
    }

    const std::optional<Digest256> m_digest;
};

// A provider that supplies a digest and whose text lies on a page that cannot be read: reading the length touches no
// character (F25), and any read of a character faults, so a digest taken from it proves that no character was read.
class UnreadableTextProvider final : public SourceProvider {
public:
    static RefPtr<UnreadableTextProvider> create(unsigned length, const Digest256& digest)
    {
        size_t size = roundUpToMultipleOf(pageSize(), std::max<size_t>(length, 1));
        void* page = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return nullptr;
        return adoptRef(*new UnreadableTextProvider(page, size, length, digest));
    }

    ~UnreadableTextProvider() final
    {
        munmap(m_page, m_size);
    }

    unsigned hash() const final { return m_length; }
    StringView source() const final { return StringView { unsafeMakeSpan(static_cast<const Latin1Character*>(m_page), m_length) }; }
    std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const final { return m_digest; }

private:
    UnreadableTextProvider(void* page, size_t size, unsigned length, const Digest256& digest)
        : SourceProvider(SourceOrigin { }, String { }, String { }, SourceTaintedOrigin::Untainted, TextPosition { }, SourceProviderSourceType::Program)
        , m_page(page)
        , m_size(size)
        , m_length(length)
        , m_digest(digest)
    {
    }

    void* const m_page;
    const size_t m_size;
    const unsigned m_length;
    const Digest256 m_digest;
};

static TDZEnvironment tdzNames(VM& vm, std::initializer_list<ASCIILiteral> names)
{
    TDZEnvironment environment;
    for (ASCIILiteral name : names)
        environment.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() });
    return environment;
}

static TDZEnvironment tdzNames(VM& vm, const Vector<String>& names)
{
    TDZEnvironment environment;
    for (const String& name : names)
        environment.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() });
    return environment;
}

static void addPrivateName(VM& vm, PrivateNameEnvironment& environment, const String& name, uint16_t traits)
{
    environment.add(PackedRefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() }, PrivateNameEntry(traits));
}

// A program's generation with its children generated to `depth` levels (0: none), as generateFixture makes one.
static ProgramFixture generateProgramFixture(VM& vm, const String& text, unsigned depth, LexicallyScopedFeatures features = NoLexicallyScopedFeatures, OptionSet<CodeGenerationMode> mode = { })
{
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    UnlinkedProgramCodeBlock* codeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, features, JSParserScriptMode::Classic, mode, error, EvalContextType::None, depth);
    return ProgramFixture { text, WTF::move(source), Strong<UnlinkedProgramCodeBlock> { vm, codeBlock } };
}

// What a UFE body's native generation makes, without touching the UFE: its slots stay as they are and its parse results go
// to `results` (section 13.2). The request object is only the twin generator's input.
static Strong<UnlinkedFunctionCodeBlock> generateBodyOf(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& source, CodeSpecializationKind kind, TwinParseResults* parseResults = nullptr)
{
    RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
    request.functionExecutable = &executable;
    request.specialization = kind;
    TwinParseResults results;
    ParserError error;
    Strong<UnlinkedFunctionCodeBlock> codeBlock { vm, generateFunctionBodyTwin(request, results, error) };
    if (parseResults)
        *parseResults = results;
    return codeBlock;
}

static void prepareProgramRequest(RequestState& request, ProgramExecutable& executable)
{
    request.globalExecutable = &executable;
    request.scriptMode = JSParserScriptMode::Classic;
    request.evalContextType = EvalContextType::None;
}

static std::optional<Vector<uint8_t>> encodeCoreBytes(VM& vm, const UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder)
{
    CoreEncodeFailure failure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, codeBlock, holder, nullptr, failure);
    if (!core)
        return std::nullopt;
    return Vector<uint8_t> { core->span() };
}

static std::optional<Vector<uint8_t>> encodeDescriptorBytes(VM& vm, const UnlinkedFunctionExecutable& executable)
{
    CoreEncodeFailure failure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, executable, nullptr, failure);
    if (!descriptor)
        return std::nullopt;
    return Vector<uint8_t> { descriptor->span() };
}

// Decodes a copy of `bytes`, which starts 8-byte aligned in a buffer the payload owns, as the import's body does.
static UnlinkedCodeBlock* decodeCoreBytes(VM& vm, std::span<const uint8_t> bytes, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, bool validate, CoreDecodeFailure& failure)
{
    Vector<uint8_t> copy { bytes };
    std::span<uint8_t> data = copy.mutableSpan();
    Ref<CachedBytecode> payload = CachedBytecode::create(data, [copy = WTF::move(copy)](const void*) { }, { });
    failure = CoreDecodeFailure::None;
    return decodeUnlinkedCodeBlockCore(vm, WTF::move(payload), provider, kind, holder, validate, failure);
}

// U2 (section 3.5): sourceDigest's two encodings and its chunks, and the three forms of rootSourceDigest.
static bool testSourceDigests(String& failure)
{
    SelfTestPart u2 { "U2"_s, failure };

    // Pinned from section 3.5's encoding alone: "abc" as 0x01 then Latin-1, U+0100 U+007A as 0x02 then UTF-16LE.
    if (!u2.check(sourceDigest("abc"_s) == digestFromHex("1e18834c426d00e57788444cb3ccd62c771b420c095bb0c4e040a8c122c4570d"_s), "sourceDigest of \"abc\" is not SHA-256 of 0x01 and its Latin-1 bytes"_s))
        return false;
    std::array<char16_t, 2> wideUnits { 0x100, 0x7A };
    if (!u2.check(sourceDigest(StringView { std::span<const char16_t> { wideUnits } }) == digestFromHex("e7dd58da0cdf47c2cc0339b6eaf83a8369b7a14a9d050d9a2a88948034744f20"_s), "sourceDigest of U+0100 U+007A is not SHA-256 of 0x02 and its UTF-16LE units"_s))
        return false;

    // 16-bit views are narrowed in chunks of 4 KiB, so lengths around the boundary cross it.
    for (unsigned length : { 0u, 1u, 4095u, 4096u, 4097u, 8193u }) {
        String narrow = latin1TestText(length);
        String wide = sixteenBitCopy(narrow);
        Digest256 expected = referenceSourceDigest(narrow);
        bool sameEncoding = (!length || (narrow.is8Bit() && !wide.is8Bit())) && sourceDigest(narrow) == expected && sourceDigest(wide) == expected;
        if (!u2.check(sameEncoding, makeString("an 8-bit and a 16-bit string of "_s, length, " Latin-1 characters do not both digest as 0x01 and their bytes"_s)))
            return false;
        if (!length)
            continue;
        String beyondLatin1 = sixteenBitCopy(narrow, 0x100);
        Digest256 beyondDigest = sourceDigest(beyondLatin1);
        if (!u2.check(beyondDigest == referenceSourceDigest(beyondLatin1) && beyondDigest != expected, makeString("a unit above 0xFF in a string of "_s, length, " units does not switch it to UTF-16LE"_s)))
            return false;
    }

    // rootSourceDigest takes a supplied digest as given, so the one supplied here is deliberately not the text's.
    String text = "var jitcacheU2Root = 1;\n"_s;
    Digest256 supplied = filledDigest(0x5d);
    Digest256 builtin = filledDigest(0xb1);
    Ref<SuppliedDigestProvider> provider = SuppliedDigestProvider::create(text, supplied);
    SourceCode whole { Ref<SourceProvider> { provider.get() } };
    auto fromMetadata = rootSourceDigest(whole, &builtin);
    if (!u2.check(fromMetadata.digest == builtin && fromMetadata.origin == SourceDigestOrigin::BuiltinMetadata, "a builtin metadata digest does not win over the provider's"_s))
        return false;
    auto fromProvider = rootSourceDigest(whole);
    if (!u2.check(fromProvider.digest == supplied && fromProvider.origin == SourceDigestOrigin::Provider, "a source that spans its provider does not take the provider's digest"_s))
        return false;
    SourceCode part { RefPtr<SourceProvider> { provider.ptr() }, 1, static_cast<int>(text.length()), 1, 1 };
    auto fromPart = rootSourceDigest(part);
    if (!u2.check(fromPart.digest == sourceDigest(part.view()) && fromPart.origin == SourceDigestOrigin::Computed, "a source over part of a supplying provider does not compute its digest"_s))
        return false;
    Ref<StringSourceProvider> plain = StringSourceProvider::create(text, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    auto fromText = rootSourceDigest(SourceCode { Ref<SourceProvider> { plain.get() } });
    if (!u2.check(fromText.digest == sourceDigest(text) && fromText.origin == SourceDigestOrigin::Computed, "a provider that supplies no digest does not have its digest computed"_s))
        return false;

    // Form 2 reads no character: any read of this provider's text faults.
    RefPtr<UnreadableTextProvider> unreadable = UnreadableTextProvider::create(64, supplied);
    if (!u2.check(!!unreadable, "the page of the unreadable provider could not be mapped"_s))
        return false;
    auto unread = rootSourceDigest(SourceCode { Ref<SourceProvider> { *unreadable } });
    return u2.check(unread.digest == supplied && unread.origin == SourceDigestOrigin::Provider, "a source that spans an unreadable supplying provider does not take its digest"_s);
}

// U3: identity, child, direct-eval, context and TDZ chain digests for fixed inputs equal literal vectors. Each vector is
// pinned beside the record it is the SHA-256 of, as sections 3.2 to 3.4 lay that record out: its 16-byte label, then its
// fields in hex, integers little-endian, booleans and enumerations one byte each, and a canonical string as its encoding
// byte, its u32 length and its units. The test also hashes every record, so a vector that is not its record's digest fails
// with a message of its own, and sha256sum over the label's ASCII followed by the record's bytes recomputes any vector
// outside the engine.
static bool testPinnedDigests(VM& vm, SelfTestPart& u3)
{
    // Every child and direct-eval record below holds this key's 40 bytes: version 1, kind Program, call, no mode, four
    // reserved bytes, then 0x11 32 times.
    BodyKey parent = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x11));
    TDZEnvironment tdz = tdzNames(vm, { "b"_s, "a"_s });
    PrivateNameEnvironment privateNames;
    addPrivateName(vm, privateNames, "#y"_s, PrivateNameEntry::IsMethod | PrivateNameEntry::IsStatic);
    addPrivateName(vm, privateNames, "#x"_s, PrivateNameEntry::IsMethod);
    // One link over the environment {beta, alpha}, whose names section 3.4 writes sorted; one link over {gamma}; and a chain
    // of two links, a link over {beta, alpha} parented on the {gamma} link. The two-link chain's record holds its parent
    // link's digest where the one-link chain's holds zeros, so a chain digest that left parents out would give it the
    // one-link vector.
    RefPtr<TDZEnvironmentLink> link = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "beta"_s, "alpha"_s })), nullptr);
    RefPtr<TDZEnvironmentLink> gammaLink = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "gamma"_s })), nullptr);
    RefPtr<TDZEnvironmentLink> twoLinks = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "beta"_s, "alpha"_s })), gammaLink);
    unsigned environmentsDigested = 0;
    auto chainDigest = tdzChainDigest(link.get(), nullptr, environmentsDigested);
    auto gammaChainDigest = tdzChainDigest(gammaLink.get(), nullptr, environmentsDigested);
    auto twoLinkDigest = tdzChainDigest(twoLinks.get(), nullptr, environmentsDigested);
    if (!u3.check(chainDigest && gammaChainDigest && twoLinkDigest, "tdzChainDigest refused a chain with no budget to refuse it"_s))
        return false;

    LexicallyScopedFeatures strictWithScope = StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature;
    struct Pinned {
        std::optional<Digest256> actual; // empty for a record no function digests on its own
        ASCIILiteral label;
        ASCIILiteral record;
        ASCIILiteral expected;
        ASCIILiteral what;
    };
    std::array<Pinned, 15> pinned { {
        { rootIdentityDigest(IdentityKind::Program, filledDigest(0x5a), strictWithScope, std::nullopt), "JITCache.root.v1"_s,
            "01 01 01 00 00000000 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"_s,
            "ddedb7e3c271a41211e9e1fcad9ae83d16e11ecdf5f2bff9df39b97397d45647"_s, "a program's root identity digest"_s },
        { rootIdentityDigest(IdentityKind::FunctionConstructor, filledDigest(0x5a), NoLexicallyScopedFeatures, 17), "JITCache.root.v1"_s,
            "04 00 00 01 11000000 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"_s,
            "8922be7f3986be8b237051c463e64d3d639951b82761f48ff4be2452bc6882c3"_s, "a Function-constructor root's identity digest"_s },
        { childIdentityDigest(parent, ChildTable::Expressions, 7), "JITCache.chld.v1"_s,
            "01 01 00 00 00000000 1111111111111111111111111111111111111111111111111111111111111111 01 07000000"_s,
            "eb438a2767b839118e59e6edfefe15c8fe053090eb88c897546e30236b3085d6"_s, "a child identity digest"_s },
        { directEvalIdentityDigest(parent, BytecodeIndex(42), filledDigest(0x22)), "JITCache.deva.v1"_s,
            "01 01 00 00 00000000 1111111111111111111111111111111111111111111111111111111111111111 2a000000 2222222222222222222222222222222222222222222222222222222222222222"_s,
            "030199b882b969d4c9f3fd1d3d96cfd9934f10c11311967ea198290dc49473c4"_s, "a direct-eval identity digest"_s },
        { globalContextDigest(IdentityKind::Program, 5, 3, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false), "JITCache.gctx.v1"_s,
            "01 05000000 03000000 00 00 00 00"_s,
            "7d96201a066ba937eb38cbd5ba55258aecd942a7b19739d62687f4e0b0dea236"_s, "a program's context digest"_s },
        { globalContextDigest(IdentityKind::Module, 0, 1, JSParserScriptMode::Module, DerivedContextType::None, EvalContextType::None, false), "JITCache.gctx.v1"_s,
            "02 00000000 01000000 01 00 00 00"_s,
            "eca8c3dabf80cc2b1d839fdccba789300b09d63ec88e232a1ce2c67240b77635"_s, "a module's context digest"_s },
        { globalContextDigest(IdentityKind::IndirectEval, 9, 2, JSParserScriptMode::Classic, DerivedContextType::DerivedConstructorContext, EvalContextType::FunctionEvalContext, true), "JITCache.gctx.v1"_s,
            "03 09000000 02000000 00 01 01 01"_s,
            "770fe23f8e01df3022d88aa841082ad5fe65c0031121e50815c9e9c2a60cdc3a"_s, "an indirect eval's context digest"_s },
        { executableBodyContextDigest(IdentityKind::Child, 12, 2, std::nullopt), "JITCache.fctx.v1"_s,
            "06 0c000000 02000000 00 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "c464b213b36640d59f748ad047fbbcf66de3e5734e9a56b40763489c20826132"_s, "a child body's context digest"_s },
        { executableBodyContextDigest(IdentityKind::FunctionConstructor, 0, 1, filledDigest(0x33)), "JITCache.fctx.v1"_s,
            "04 00000000 01000000 01 3333333333333333333333333333333333333333333333333333333333333333"_s,
            "bd53125d8233578ffa3095a5f64c5fec677a4e17c84b88d84f0f5fa72aef3881"_s, "a root body's context digest"_s },
        // Offset 7, first line 4, strict, DerivedMethodContext, a class field initializer, a private brand, an arrow
        // context outside an ordinary function, FunctionEvalContext; TDZ names "a" and "b" sorted; private names "#x"
        // (IsMethod) and "#y" (IsMethod | IsStatic) sorted, each followed by its u16 bits.
        { directEvalContextDigest(7, 4, StrictModeLexicallyScopedFeature, DerivedContextType::DerivedMethodContext, NeedsClassFieldInitializer::Yes, PrivateBrandRequirement::Needed, true, false, EvalContextType::FunctionEvalContext, tdz, privateNames), "JITCache.dctx.v1"_s,
            "07000000 04000000 01 02 01 01 01 00 01 02000000 01 01000000 61 01 01000000 62 02000000 01 02000000 2378 0100 01 02000000 2379 0900"_s,
            "e28cbf9cf7a652e3d18376d7aead89bb92bcc4c48e17858c80b0310207880c81"_s, "a direct eval's context digest"_s },
        // The environment {alpha, beta}: its name count, then each name's EncodingOrder kind (0, a string) and canonical
        // string. Its digest is the first field of the link record below.
        { std::nullopt, "JITCache.tdze.v1"_s,
            "02000000 00 01 05000000 616c706861 00 01 04000000 62657461"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c"_s, "an environment's digest"_s },
        { *chainDigest, "JITCache.tdzl.v1"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "561249017536183aa860f028aab9a0dc08033ea2dcec8ab0f3201a9add063bd7"_s, "a one-link TDZ chain digest"_s },
        // The environment {gamma}, and the link over it with no parent, which is the second link of the chain below.
        { std::nullopt, "JITCache.tdze.v1"_s,
            "01000000 00 01 05000000 67616d6d61"_s,
            "51deeb97086c9a61737f4fa69e4a37666e378e2083d977b90a78de5e9bf97940"_s, "a one-name environment's digest"_s },
        { *gammaChainDigest, "JITCache.tdzl.v1"_s,
            "51deeb97086c9a61737f4fa69e4a37666e378e2083d977b90a78de5e9bf97940 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "49e5849659bb4c9079e410c2b8b6a6a10e03b672637cf42365578bfbadc126f1"_s, "the digest of a link over {gamma}"_s },
        // The {alpha, beta} link's record again, now with the {gamma} link's digest as its parent's.
        { *twoLinkDigest, "JITCache.tdzl.v1"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c 49e5849659bb4c9079e410c2b8b6a6a10e03b672637cf42365578bfbadc126f1"_s,
            "d445e4b48abe6c15246c231e61019f1be1dd01a78db2e64a47190a5dec23ed5d"_s, "a two-link TDZ chain digest"_s },
    } };
    for (const Pinned& entry : pinned) {
        Digest256 expected = digestFromHex(entry.expected);
        Vector<uint8_t> fields = bytesFromHex(entry.record);
        Vector<uint8_t> record;
        record.append(asBytes(entry.label.span8()));
        record.append(fields.span());
        if (!u3.check(SHA256::hash(record.span()) == expected, makeString("the vector pinned for "_s, entry.what, " is not the SHA-256 of the record beside it"_s)))
            return false;
        if (entry.actual && !u3.check(*entry.actual == expected, makeString(entry.what, " differs from the vector pinned from its layout"_s)))
            return false;
    }
    return true;
}

// U3: BodyKey::fromBytes rejects each field out of range, and a key round-trips through its 40 canonical bytes.
static bool testBodyKeyBytes(SelfTestPart& u3)
{
    BodyKey key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, CodeGenerationMode::Debugger, filledDigest(0x42));
    std::array<uint8_t, BodyKey::byteSize> bytes;
    memcpySpan(std::span { bytes }, key.bytes());
    bool laidOut = bytes[0] == 1 && bytes[1] == static_cast<uint8_t>(IdentityKind::Child) && bytes[2] == 1 && bytes[3] == OptionSet<CodeGenerationMode> { CodeGenerationMode::Debugger }.toRaw()
        && !bytes[4] && !bytes[5] && !bytes[6] && !bytes[7] && key.identityDigest() == filledDigest(0x42);
    if (!u3.check(laidOut, "BodyKey::make does not lay out the 40 bytes of section 3.1"_s))
        return false;
    auto parse = [](const std::array<uint8_t, BodyKey::byteSize>& candidate) {
        return BodyKey::fromBytes(std::span<const uint8_t, BodyKey::byteSize> { candidate });
    };
    if (!u3.check(parse(bytes) == key, "BodyKey::fromBytes does not read back a key's bytes"_s))
        return false;
    struct Mutation {
        size_t offset;
        uint8_t value;
        ASCIILiteral what;
    };
    static constexpr std::array<Mutation, 11> mutations { {
        { 0, 0, "version 0"_s }, { 0, 2, "version 2"_s }, { 1, 0, "identity kind 0"_s }, { 1, 8, "identity kind 8"_s }, { 2, 2, "specialization 2"_s },
        { 3, 0x08, "a mode bit outside CodeGenerationMode"_s }, { 3, 0x80, "the top mode bit"_s },
        { 4, 1, "reserved byte 4"_s }, { 5, 1, "reserved byte 5"_s }, { 6, 1, "reserved byte 6"_s }, { 7, 1, "reserved byte 7"_s },
    } };
    for (const Mutation& mutation : mutations) {
        auto crafted = bytes;
        crafted[mutation.offset] = mutation.value;
        if (!u3.check(!parse(crafted), makeString("BodyKey::fromBytes accepts "_s, mutation.what)))
            return false;
    }
    return true;
}

// U3: a direct eval's context digest does not depend on the order its TDZ and private-name sets were built in, and changes
// with each name added to either; each context digest changes with the first line.
static bool testContextInputs(VM& vm, SelfTestPart& u3)
{
    Vector<String> names;
    for (unsigned i = 0; i < 24; ++i)
        names.append(makeString("jitcacheU3Name"_s, i));
    Vector<String> reversed = names;
    reversed.reverse();
    auto privateSet = [&](const Vector<String>& order) {
        PrivateNameEnvironment environment;
        for (const String& name : order)
            addPrivateName(vm, environment, makeString('#', name), name.length() % 2 ? PrivateNameEntry::IsMethod : PrivateNameEntry::IsGetter);
        return environment;
    };
    auto digest = [&](unsigned firstLine, const TDZEnvironment& tdz, const PrivateNameEnvironment& privateNames) {
        return directEvalContextDigest(3, firstLine, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, true, EvalContextType::None, tdz, privateNames);
    };
    TDZEnvironment forwardTDZ = tdzNames(vm, names);
    TDZEnvironment backwardTDZ = tdzNames(vm, reversed);
    PrivateNameEnvironment forwardNames = privateSet(names);
    PrivateNameEnvironment backwardNames = privateSet(reversed);
    Digest256 base = digest(1, forwardTDZ, forwardNames);
    if (!u3.check(base == digest(1, backwardTDZ, backwardNames), "a direct eval's context digest depends on the insertion order of its sets"_s))
        return false;
    TDZEnvironment widerTDZ = forwardTDZ;
    widerTDZ.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, "jitcacheU3Added"_s).impl() });
    PrivateNameEnvironment widerNames = forwardNames;
    addPrivateName(vm, widerNames, "#jitcacheU3Added"_s, PrivateNameEntry::IsMethod);
    if (!u3.check(digest(1, widerTDZ, forwardNames) != base && digest(1, forwardTDZ, widerNames) != base, "a direct eval's context digest does not change with a name added to its TDZ or private-name set"_s))
        return false;

    bool changesWithLine = digest(2, forwardTDZ, forwardNames) != base
        && globalContextDigest(IdentityKind::Program, 0, 1, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false) != globalContextDigest(IdentityKind::Program, 0, 2, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false)
        && executableBodyContextDigest(IdentityKind::Child, 0, 1, std::nullopt) != executableBodyContextDigest(IdentityKind::Child, 0, 2, std::nullopt);
    return u3.check(changesWithLine, "a context digest does not change with the source's first line"_s);
}

// The first child UFE, declarations before expressions, that `select` accepts.
static UnlinkedFunctionExecutable* selectChild(UnlinkedCodeBlock& codeBlock, const Function<bool(UnlinkedFunctionExecutable&)>& select)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i) {
        if (select(*codeBlock.functionDecl(i)))
            return codeBlock.functionDecl(i);
    }
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i) {
        if (select(*codeBlock.functionExpr(i)))
            return codeBlock.functionExpr(i);
    }
    return nullptr;
}

// One step down to a variant's UFE: the first child UFE that the step accepts, of the UCB reached so far.
enum class HolderStep : uint8_t {
    End,
    Any,
    FieldInitializer, // the class field initializer, which holds the class element definitions
    DefaultConstructor, // the default constructor, which holds the class source range
    ClassConstructor, // a constructor the class declares
    MethodNamedM,
};

static bool acceptsStep(HolderStep step, UnlinkedFunctionExecutable& executable)
{
    switch (step) {
    case HolderStep::End:
        return false;
    case HolderStep::Any:
        return true;
    case HolderStep::FieldInitializer:
        return executable.parseMode() == SourceParseMode::ClassFieldInitializerMode;
    case HolderStep::DefaultConstructor:
        return executable.isBuiltinDefaultClassConstructor();
    case HolderStep::ClassConstructor:
        return executable.isClassConstructorFunction() && !executable.isBuiltinDefaultClassConstructor();
    case HolderStep::MethodNamedM:
        return executable.name().string() == "m"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

// Where a side of a variant starts. A program or a module is generated from the text, and the path starts at its UCB. A
// builtin root is made from the text, which reads "(function (...) { ... })", by BuiltinExecutables::createExecutable with
// the side's four builtin flags and no name, and the path starts at that root. A direct eval of the text is generated
// under the side's eval context type, with empty TDZ and private-name sets, and the path starts at its UCB. Each step
// after a UFE has been reached generates that UFE's body and looks among its children.
enum class HolderOrigin : uint8_t { Program, Module, Builtin, DirectEval };

struct HolderSide {
    ASCIILiteral text;
    HolderOrigin origin { HolderOrigin::Program };
    std::array<HolderStep, 2> path { HolderStep::Any, HolderStep::End };
    ImplementationVisibility implementationVisibility { ImplementationVisibility::Public }; // Builtin
    ConstructAbility constructAbility { ConstructAbility::CannotConstruct }; // Builtin
    InlineAttribute inlineAttribute { InlineAttribute::None }; // Builtin
    PrivateBrandRequirement privateBrandRequirement { PrivateBrandRequirement::None }; // Builtin
    EvalContextType evalContextType { EvalContextType::FunctionEvalContext }; // DirectEval
    // A TDZ variant's chain, one letter per link, the UFE's own link first: the one name among a, b and c that the link's
    // environment holds. Empty for the variants that leave the chain as generation makes it.
    ASCIILiteral tdzLinks { };
};

// The scalar flags the descriptor holds for a UFE (section 3.4's "flags", which CachedFunctionExecutable::packScalars
// writes), in the order descriptorFlags reads them through the UFE's accessors.
enum class DescriptorFlag : uint8_t {
    ScriptMode,
    SuperBinding,
    ConstructAbility,
    HasName,
    ConstructorKind,
    FunctionMode,
    ImplementationVisibility,
    DerivedContextType,
    EvalContextType,
    PrivateBrandRequirement,
    InlineAttribute,
    NeedsClassFieldInitializer,
    IsBuiltinFunction,
    IsBuiltinDefaultClassConstructor,
};
static constexpr size_t descriptorFlagCount = static_cast<size_t>(DescriptorFlag::IsBuiltinDefaultClassConstructor) + 1;

// Two UFEs that differ in what `field` names. `descriptorDiffers` and `chainDiffers` say whether their descriptors and
// their TDZ chains differ; the holder digest covers both, so it differs exactly when one of them does (section 3.4). A flag
// variant names the one flag it changes, and generateHolderPair checks that its two UFEs agree on every other flag, every
// position and every rare field the descriptor holds, and, unless chainDiffers, on the chain digest. Only then can a
// difference in the digest or the descriptor come from the flag alone.
//
// Every flag has a variant but IsBuiltinDefaultClassConstructor: only BuiltinExecutables makes a default constructor, from a
// constructor kind other than None, which also makes it a normal function rather than a builtin's, so that flag never
// moves alone. The constructor kind and super binding variants keep the other one fixed: a constructor that extends
// another always needs a super binding, so the base class's constructor uses super too.
//
// The TDZ variants create their function before the blocks' `let`s run. A block's link is made, and cached, when the first
// function inside the block is created (BytecodeGenerator::getVariablesUnderTDZ), and holds only the names still under TDZ
// then; a `let` that has run is lifted out (EmptyLetExpression::emitBytecode, liftTDZCheckIfPossible), so
// `{ let a; (function f() { }); }` gives f a link over no name at all. A program pushes no TDZ entry of its own, so each
// block that declares a name is one link. testHolderDigests checks each side's chain against its tdzLinks before it
// compares the digests.
struct HolderVariant {
    ASCIILiteral field;
    HolderSide base;
    HolderSide variant;
    bool descriptorDiffers;
    bool chainDiffers;
    std::optional<DescriptorFlag> flag;
};

static constexpr ASCIILiteral builtinHolderText = "(function (a) { return a; })"_s;
static constexpr ASCIILiteral nestingHolderText = "(function (a) { return function () { return a; }; })"_s;

static constexpr std::array<HolderVariant, 26> holderVariants { {
    { "the name"_s, { "function f(a) { return a; }"_s }, { "function g(a) { return a; }"_s }, true, false, std::nullopt },
    { "the parameter count"_s, { "function f(a,b){return a;}"_s }, { "function f(ab) {return a;}"_s }, true, false, std::nullopt },
    { "the parse mode"_s, { "function f(a) { return a; }"_s }, { "function*f(a) { return a; }"_s }, true, false, std::nullopt },
    { "the lexically scoped features"_s, { "function f(a) { 'use strict';  }"_s }, { "function f(a) { 'use strict!'; }"_s }, true, false, std::nullopt },
    { "the source position"_s, { "function f(a) { return a; }"_s }, { " function f(a) { return a; }"_s }, true, false, std::nullopt },
    { "the class element definitions"_s, { "class C { x = 1; }"_s, HolderOrigin::Program, { HolderStep::FieldInitializer } },
        { "class C { y = 1; }"_s, HolderOrigin::Program, { HolderStep::FieldInitializer } }, true, false, std::nullopt },
    { "the class source range"_s, { "class C { }"_s, HolderOrigin::Program, { HolderStep::DefaultConstructor } },
        { "class C {  }"_s, HolderOrigin::Program, { HolderStep::DefaultConstructor } }, true, false, std::nullopt },
    { "the parent's private names"_s, { "class C { #x; m() { return this.#x; } }"_s, HolderOrigin::Program, { HolderStep::MethodNamedM } },
        { "class C { #y; m() { return this.#y; } }"_s, HolderOrigin::Program, { HolderStep::MethodNamedM } }, true, false, std::nullopt },
    // The first function of the async function's body, which holds the wrapper's parameter names.
    { "the async wrapper's parameter names"_s, { "async function f(a) { await a; }"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } },
        { "async function f(b) { await b; }"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } }, true, false, std::nullopt },
    { "a TDZ name"_s, { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s },
        { .text = "{ (function f() { }); let b; }"_s, .tdzLinks = "b"_s }, false, true, std::nullopt },
    // The first links are over one environment, so only the second link's digest can set the two chains apart.
    { "a TDZ name in a parent link"_s, { .text = "{ { (function f() { }); let c; } let a; }"_s, .tdzLinks = "ca"_s },
        { .text = "{ { (function f() { }); let c; } let b; }"_s, .tdzLinks = "cb"_s }, false, true, std::nullopt },
    { "the order of the TDZ links"_s, { .text = "{ { (function f() { }); let b; } let a; }"_s, .tdzLinks = "ba"_s },
        { .text = "{ { (function f() { }); let a; } let b; }"_s, .tdzLinks = "ab"_s }, false, true, std::nullopt },
    { "nothing: distinct links over the same environments"_s, { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s },
        { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s }, false, false, std::nullopt },
    // The flags. Each pair of texts has one length and puts its UFE at one offset, so positions agree.
    { "the function mode"_s, { "  function f(a) { return a; }"_s }, { "x=function f(a) { return a; }"_s }, true, false, DescriptorFlag::FunctionMode },
    // Anonymous, the function still takes the binding's name as its ecmaName, as the named one has it.
    { "whether the function names itself"_s, { "var f = function  (a) { return a; };"_s }, { "var f = function f(a) { return a; };"_s }, true, false, DescriptorFlag::HasName },
    { "the constructor kind"_s, { "class A           { constructor(a) { super.x; } }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } },
        { "class A extends B { constructor(a) { super.x; } }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } }, true, false, DescriptorFlag::ConstructorKind },
    { "the super binding"_s, { "({ m() { return super.x; } });"_s }, { "({ m() { return this.x;  } });"_s }, true, false, DescriptorFlag::SuperBinding },
    // The arrow inside the method. Generation gives it DerivedMethodContext when the method's own super binding makes the
    // method's body a class context (generateUnlinkedFunctionCodeBlock, BytecodeGenerator::makeFunction), so the method's
    // use of super sets it. An extends clause alone changes neither a method that never uses super nor its arrow.
    { "the derived context type"_s, { "({ m() { super.x; return () => 1; } });"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } },
        { "({ m() { this.x;  return () => 1; } });"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } }, true, false, DescriptorFlag::DerivedContextType },
    // An arrow takes the eval context type of the code it is generated in, here the eval's own.
    { "the eval context type"_s, { .text = "() => 1;"_s, .origin = HolderOrigin::DirectEval, .evalContextType = EvalContextType::FunctionEvalContext },
        { .text = "() => 1;"_s, .origin = HolderOrigin::DirectEval, .evalContextType = EvalContextType::InstanceFieldEvalContext }, true, false, DescriptorFlag::EvalContextType },
    { "the class field initializer flag"_s, { "class A { constructor() { } x;    }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } },
        { "class A { constructor() { } x(){} }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } }, true, false, DescriptorFlag::NeedsClassFieldInitializer },
    // A strict program's block function against a module's: both strict, and both chains one link over an empty environment,
    // since a block that declares only a function pushes a TDZ entry whose names need none.
    { "the script mode"_s, { "'use strict';{ function f(a) { return a; } }"_s },
        { "             { function f(a) { return a; } }"_s, HolderOrigin::Module }, true, false, DescriptorFlag::ScriptMode },
    { "the implementation visibility"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .implementationVisibility = ImplementationVisibility::Private }, true, false, DescriptorFlag::ImplementationVisibility },
    { "the construct ability"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .constructAbility = ConstructAbility::CanConstruct }, true, false, DescriptorFlag::ConstructAbility },
    { "the inline attribute"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .inlineAttribute = InlineAttribute::Always }, true, false, DescriptorFlag::InlineAttribute },
    { "the private brand requirement"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .privateBrandRequirement = PrivateBrandRequirement::Needed }, true, false, DescriptorFlag::PrivateBrandRequirement },
    // The inner function of one text, generated inside a builtin root's body and inside a program's function. Debug builds
    // check that a builtin root's metadata is what the parser gives the same text (BuiltinExecutables::createExecutable), so
    // the two bodies parse the same source and place the inner function alike. Both outer functions are anonymous:
    // createExecutable makes every builtin root a function expression, and a named one's body pushes a scope for its own
    // name (BytecodeGenerator::emitPushFunctionNameScope) whose TDZ entry gives the inner function a chain link over an
    // empty environment, which the program side lacks.
    { "whether the function is a builtin's"_s, { .text = nestingHolderText, .origin = HolderOrigin::Builtin, .path = { HolderStep::Any } },
        { .text = nestingHolderText, .origin = HolderOrigin::Program, .path = { HolderStep::Any, HolderStep::Any } }, true, false, DescriptorFlag::IsBuiltinFunction },
} };

static std::array<unsigned, descriptorFlagCount> descriptorFlags(const UnlinkedFunctionExecutable& executable)
{
    return { {
        static_cast<unsigned>(executable.scriptMode()),
        static_cast<unsigned>(executable.superBinding()),
        static_cast<unsigned>(executable.constructAbility()),
        static_cast<unsigned>(!executable.name().isNull()), // name() is the ecmaName exactly when the UFE has its own name
        static_cast<unsigned>(executable.constructorKind()),
        static_cast<unsigned>(executable.functionMode()),
        static_cast<unsigned>(executable.implementationVisibility()),
        static_cast<unsigned>(executable.derivedContextType()),
        static_cast<unsigned>(executable.evalContextType()),
        static_cast<unsigned>(executable.privateBrandRequirement()),
        static_cast<unsigned>(executable.inlineAttribute()),
        static_cast<unsigned>(executable.needsClassFieldInitializer()),
        static_cast<unsigned>(executable.isBuiltinFunction()),
        static_cast<unsigned>(executable.isBuiltinDefaultClassConstructor()),
    } };
}

static Vector<std::pair<String, uint16_t>> parentPrivateNamesOf(const UnlinkedFunctionExecutable& executable)
{
    Vector<std::pair<String, uint16_t>> names;
    if (auto* environment = executable.parentPrivateNameEnvironment()) {
        for (const auto& entry : *environment)
            names.append({ String { entry.key.get() }, entry.value.bits() });
    }
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    return names;
}

// Whether two UFEs differ, among the flags, positions and rare fields the descriptor holds (CachedFunctionExecutable's
// header, scalars and rare data), in `flag` alone, and, when `sameChain`, have chains with one digest.
static bool differsOnlyInFlag(UnlinkedFunctionExecutable& a, UnlinkedFunctionExecutable& b, DescriptorFlag flag, bool sameChain)
{
    auto flagsOfA = descriptorFlags(a);
    auto flagsOfB = descriptorFlags(b);
    for (size_t i = 0; i < descriptorFlagCount; ++i) {
        if ((flagsOfA[i] != flagsOfB[i]) != (i == static_cast<size_t>(flag)))
            return false;
    }
    bool samePositions = a.startOffset() == b.startOffset() && a.sourceLength() == b.sourceLength() && a.unlinkedFunctionStart() == b.unlinkedFunctionStart()
        && a.unlinkedFunctionEnd() == b.unlinkedFunctionEnd() && a.parametersStartOffset() == b.parametersStartOffset()
        && a.unlinkedBodyStartColumn() == b.unlinkedBodyStartColumn() && a.unlinkedBodyEndColumn() == b.unlinkedBodyEndColumn() && a.lineCount() == b.lineCount();
    bool sameScalars = a.ecmaName() == b.ecmaName() && a.parameterCount() == b.parameterCount() && a.parseMode() == b.parseMode()
        && a.lexicallyScopedFeatures() == b.lexicallyScopedFeatures();
    auto sizeOf = [](const auto* vector) -> size_t {
        return vector ? vector->size() : 0;
    };
    SourceCode classOfA = a.classSource();
    SourceCode classOfB = b.classSource();
    bool sameRareFields = classOfA.startOffset() == classOfB.startOffset() && classOfA.endOffset() == classOfB.endOffset()
        && parentPrivateNamesOf(a) == parentPrivateNamesOf(b)
        && sizeOf(a.generatorOrAsyncWrapperFunctionParameterNames()) == sizeOf(b.generatorOrAsyncWrapperFunctionParameterNames())
        && sizeOf(a.classElementDefinitions()) == sizeOf(b.classElementDefinitions());
    if (!samePositions || !sameScalars || !sameRareFields)
        return false;
    if (!sameChain)
        return true;
    unsigned digested = 0;
    auto chainOfA = tdzChainDigest(a.parentScopeTDZVariables().get(), nullptr, digested);
    auto chainOfB = tdzChainDigest(b.parentScopeTDZVariables().get(), nullptr, digested);
    return chainOfA && chainOfA == chainOfB;
}

// A side of a variant, generated, with every cell on the way to its UFE held.
struct HolderFixture {
    SourceCode source;
    Strong<UnlinkedCodeBlock> codeBlock;
    Strong<UnlinkedFunctionExecutable> root;
    Vector<Strong<UnlinkedFunctionCodeBlock>> bodies;
    UnlinkedFunctionExecutable* executable { nullptr }; // null when the side failed to generate or its path found nothing
};

static HolderFixture generateHolderSide(VM& vm, JSGlobalObject* globalObject, const HolderSide& side)
{
    HolderFixture fixture;
    String text { side.text };
    ParserError error;
    switch (side.origin) {
    case HolderOrigin::Program:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, fixture.source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
        break;
    case HolderOrigin::Module:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, fixture.source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
        break;
    case HolderOrigin::Builtin:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        fixture.root = Strong<UnlinkedFunctionExecutable> { vm, BuiltinExecutables::createExecutable(vm, fixture.source, Identifier { }, side.implementationVisibility,
            ConstructorKind::None, side.constructAbility, side.inlineAttribute, NeedsClassFieldInitializer::No, side.privateBrandRequirement) };
        break;
    case HolderOrigin::DirectEval: {
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        TDZEnvironment variablesUnderTDZ;
        PrivateNameEnvironment privateNames;
        DirectEvalExecutable* executable = DirectEvalExecutable::create(globalObject, fixture.source, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
            PrivateBrandRequirement::None, false, false, side.evalContextType, &variablesUnderTDZ, &privateNames);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, executable ? executable->unlinkedCodeBlock() : nullptr };
        break;
    }
    }

    SourceCode source = fixture.source;
    UnlinkedCodeBlock* codeBlock = fixture.codeBlock.get();
    UnlinkedFunctionExecutable* executable = fixture.root.get();
    if (!codeBlock && !executable)
        return fixture;
    for (HolderStep step : side.path) {
        if (step == HolderStep::End)
            break;
        if (executable) {
            source = executable->linkedSourceCode(source);
            Strong<UnlinkedFunctionCodeBlock> body = generateBodyOf(vm, *executable, source, CodeSpecializationKind::CodeForCall);
            if (!body)
                return fixture;
            codeBlock = body.get();
            fixture.bodies.append(body);
        }
        executable = selectChild(*codeBlock, [step](UnlinkedFunctionExecutable& candidate) {
            return acceptsStep(step, candidate);
        });
        if (!executable)
            return fixture;
    }
    fixture.executable = executable;
    return fixture;
}

// Generates both sides of a variant; for a flag variant, also checks that the flag alone sets their UFEs apart.
static bool generateHolderPair(VM& vm, JSGlobalObject* globalObject, const HolderVariant& variant, SelfTestPart& part, HolderFixture& base, HolderFixture& other)
{
    base = generateHolderSide(vm, globalObject, variant.base);
    other = generateHolderSide(vm, globalObject, variant.variant);
    if (!part.check(base.executable && other.executable, makeString("a holder variant for "_s, variant.field, " lacks its UFE"_s)))
        return false;
    if (!variant.flag)
        return true;
    bool alone = differsOnlyInFlag(*base.executable, *other.executable, *variant.flag, !variant.chainDiffers);
    return part.check(alone, makeString("the two UFEs of the variant for "_s, variant.field, " differ in more than that flag, so they cannot show that it is covered"_s));
}

// Whether a UFE's TDZ chain is the one a TDZ variant's side names (HolderSide::tdzLinks): one link per letter, the UFE's own
// link first, each holding that letter and neither other letter of a, b and c, and no link after the last letter. A side
// that names no chain passes. The check reads the environments without the digest under test: TDZEnvironmentLink::contains
// reads only its own link's environment, and inflates it.
static bool hasTDZLinks(VM& vm, const UnlinkedFunctionExecutable& executable, ASCIILiteral links)
{
    if (links.isNull())
        return true;
    static constexpr std::array<ASCIILiteral, 3> letters { "a"_s, "b"_s, "c"_s };
    TDZEnvironmentLink* link = executable.parentScopeTDZVariables().get();
    for (char expected : links.span()) {
        if (!link)
            return false;
        for (ASCIILiteral letter : letters) {
            if (link->contains(Identifier::fromString(vm, letter).impl()) != (letter[0] == expected))
                return false;
        }
        link = link->parent();
    }
    return !link;
}

// U3: holder digests (section 3.4). Stable across the UFE's own parse and the filling of its slots; changed by each field
// the descriptor holds, its flags one by one included, by each TDZ name of any link and by the order of the links; the same
// over distinct links on the same environments.
static bool testHolderDigests(VM& vm, JSGlobalObject* globalObject, SelfTestPart& u3)
{
    ProgramFixture fixture = generateFixture(vm, "function jitcacheU3Holder(a) { return a; }"_s);
    if (!u3.check(!!fixture.codeBlock && fixture.codeBlock.get()->numberOfFunctionDecls() == 1, "the holder-digest program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& executable = *fixture.codeBlock.get()->functionDecl(0);
    auto before = holderDigest(vm, executable);
    executable.recordParse(ArgumentsFeature | ThisFeature, executable.lexicallyScopedFeatures(), true);
    auto afterParse = holderDigest(vm, executable);
    ParserError error;
    UnlinkedFunctionCodeBlock* body = executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(fixture.source), CodeSpecializationKind::CodeForCall, { }, error, executable.parseMode());
    auto afterBody = holderDigest(vm, executable);
    if (!u3.check(body && before && before == afterParse && before == afterBody, "a holder digest changes with the UFE's own parse or the filling of its slot"_s))
        return false;

    for (const HolderVariant& variant : holderVariants) {
        HolderFixture base;
        HolderFixture other;
        if (!generateHolderPair(vm, globalObject, variant, u3, base, other))
            return false;
        auto baseDigest = holderDigest(vm, *base.executable);
        auto otherDigest = holderDigest(vm, *other.executable);
        // Checked after the digests because contains() inflates an environment, so an environment first made here is digested
        // in the compact form generation gives it (testEnvironmentDigests covers both forms). The two sides come from two
        // generations, so their chains are distinct link objects even where their environments are one.
        bool chainsAsNamed = hasTDZLinks(vm, *base.executable, variant.base.tdzLinks) && hasTDZLinks(vm, *other.executable, variant.variant.tdzLinks)
            && (variant.base.tdzLinks.isNull() || base.executable->parentScopeTDZVariables() != other.executable->parentScopeTDZVariables());
        if (!u3.check(chainsAsNamed, makeString("the UFEs of the variant for "_s, variant.field, " do not hold the TDZ chains it names"_s)))
            return false;
        bool shouldDiffer = variant.descriptorDiffers || variant.chainDiffers;
        if (!u3.check(baseDigest && otherDigest && ((*baseDigest != *otherDigest) == shouldDiffer), makeString("the holder digest does not follow "_s, variant.field)))
            return false;
    }
    return true;
}

// The environment digest of section 3.4, written out apart from the codec: the names sorted by their characters, each as
// u8 kind (0 for a string) and a canonical string; and the digest of a one-link chain over that environment.
static Digest256 referenceOneLinkChainDigest(Vector<String> names)
{
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) {
        return codePointCompareLessThan(a, b);
    });
    Vector<uint8_t> environment;
    environment.append(asBytes("JITCache.tdze.v1"_s.span8()));
    auto appendU32 = [](Vector<uint8_t>& bytes, uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.append(static_cast<uint8_t>(value >> shift));
    };
    appendU32(environment, static_cast<uint32_t>(names.size()));
    for (const String& name : names) {
        RELEASE_ASSERT(name.is8Bit());
        environment.append(0); // EncodingOrder::kind of a string
        environment.append(1); // Latin-1
        appendU32(environment, name.length());
        environment.append(asBytes(name.span8()));
    }
    Digest256 environmentDigest = SHA256::hash(environment.span());
    std::array<uint8_t, 32> noParent { };
    Vector<uint8_t> link;
    link.append(asBytes("JITCache.tdzl.v1"_s.span8()));
    link.append(std::span<const uint8_t> { environmentDigest });
    link.append(std::span<const uint8_t> { noParent });
    return SHA256::hash(link.span());
}

// U3: an environment's digest equals the reference from its names, in its compact form and inflated; each environment is
// digested once while it lives, and again once it was freed and made again; a request's holder digest over environments
// already digested digests none.
static bool testEnvironmentDigests(VM& vm, SelfTestPart& u3)
{
    auto oneLink = [&](const Vector<String>& names) {
        return TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, names)), nullptr);
    };
    Vector<String> compactNames { "jitcacheU3EnvZeta"_s, "jitcacheU3EnvAlpha"_s, "jitcacheU3EnvMu"_s };
    Vector<String> inflatedNames { "jitcacheU3InflatedB"_s, "jitcacheU3InflatedA"_s };
    Vector<String> remadeNames { "jitcacheU3RemadeOne"_s, "jitcacheU3RemadeTwo"_s };

    RefPtr<TDZEnvironmentLink> compact = oneLink(compactNames);
    unsigned first = 0;
    auto compactDigest = tdzChainDigest(compact.get(), nullptr, first);
    unsigned second = 0;
    auto again = tdzChainDigest(compact.get(), nullptr, second);
    bool compactOK = compactDigest && *compactDigest == referenceOneLinkChainDigest(compactNames) && first == 1 && again == compactDigest && !second;
    if (!u3.check(compactOK, "a compact environment's digest differs from its reference, or is computed more than once"_s))
        return false;

    RefPtr<TDZEnvironmentLink> inflated = oneLink(inflatedNames);
    bool contains = inflated->contains(Identifier::fromString(vm, "jitcacheU3InflatedA"_s).impl()); // inflates the environment
    unsigned inflatedCount = 0;
    auto inflatedDigest = tdzChainDigest(inflated.get(), nullptr, inflatedCount);
    if (!u3.check(contains && inflatedDigest && *inflatedDigest == referenceOneLinkChainDigest(inflatedNames) && inflatedCount == 1, "an inflated environment's digest differs from its reference"_s))
        return false;

    RefPtr<TDZEnvironmentLink> remade = oneLink(remadeNames);
    unsigned remadeFirst = 0;
    auto remadeDigest = tdzChainDigest(remade.get(), nullptr, remadeFirst);
    remade = nullptr; // the last handle on the environment, which frees it with its kept digest
    remade = oneLink(remadeNames);
    unsigned remadeSecond = 0;
    auto remadeAgain = tdzChainDigest(remade.get(), nullptr, remadeSecond);
    if (!u3.check(remadeDigest && remadeAgain == remadeDigest && remadeFirst == 1 && remadeSecond == 1, "an environment freed and made again is not digested again"_s))
        return false;

    // Two sibling UFEs share their chain: the second request's holder digest finds every environment digested. The functions
    // come before the `let`, so their link holds both names, an environment no earlier part made (see the TDZ variants of
    // holderVariants); after the `let` it would be the empty environment, which earlier parts may have digested already.
    ProgramFixture fixture = generateFixture(vm, "{ (function f() { }); (function g() { }); let jitcacheU3SharedA, jitcacheU3SharedB; }"_s);
    bool shared = !!fixture.codeBlock && fixture.codeBlock.get()->numberOfFunctionExprs() == 2 && fixture.codeBlock.get()->functionExpr(0)->parentScopeTDZVariables()
        && fixture.codeBlock.get()->functionExpr(0)->parentScopeTDZVariables() == fixture.codeBlock.get()->functionExpr(1)->parentScopeTDZVariables();
    if (!u3.check(shared, "the shared-chain program failed to generate, or its two functions do not share one link"_s))
        return false;
    UCBStatistics& statistics = vm.jitCacheState()->registry().statistics();
    auto requestHolder = [&](UnlinkedFunctionExecutable& executable) {
        SourceCode source = executable.linkedSourceCode(fixture.source);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = &executable;
        return requestHolderDigest(request);
    };
    uint64_t environmentsBefore = statistics.tdzEnvironmentDigests;
    uint64_t holdersBefore = statistics.holderDigests;
    Digest256 f = requestHolder(*fixture.codeBlock.get()->functionExpr(0));
    uint64_t environmentsAfterFirst = statistics.tdzEnvironmentDigests;
    Digest256 g = requestHolder(*fixture.codeBlock.get()->functionExpr(1));
    bool counted = environmentsAfterFirst == environmentsBefore + 1 && statistics.tdzEnvironmentDigests == environmentsAfterFirst && statistics.holderDigests == holdersBefore + 2;
    return u3.check(counted && f != g, "the first holder digest over a one-link chain did not digest its one fresh environment, the second digested one again, or a holder digest is not counted"_s);
}

// U3: what each kind of request computes (section 3.7). contextDigestOf the inputs recordedContext keeps equals
// requestContext before and after the body is generated, for a root UFE body too; a request's key and context read only
// the snapshot its request object took; a program at another offset of its provider has another context, and no request
// context changes with the start column; a decoded program takes the request's key with its own mode only when its
// with-scope bit is the snapshot's.
static bool testRequestKeysAndContexts(VM& vm, JSGlobalObject* globalObject, SelfTestPart& u3)
{
    UCBRegistry& registry = vm.jitCacheState()->registry();
    String programText = "var jitcacheU3Request = 1; function jitcacheU3RequestChild(a) { return a + jitcacheU3Request; }\n"_s;
    Ref<StringSourceProvider> provider = StringSourceProvider::create(programText, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    SourceCode programSource { Ref<SourceProvider> { provider.get() } };
    Strong<ProgramExecutable> executable { vm, ProgramExecutable::create(globalObject, programSource) };

    // Program, before and after the CodeCache generates it, which overwrites the executable's features (F18).
    auto programRequestContext = [&]() -> std::optional<Digest256> {
        RequestState request(vm, RequestKind::Program, programSource, { }, executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *executable.get());
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::Program, recordedContext(request), std::nullopt) != context)
            return std::nullopt;
        return context;
    };
    auto programBefore = programRequestContext();
    ParserError error;
    Strong<UnlinkedProgramCodeBlock> program { vm, vm.codeCache()->getUnlinkedProgramCodeBlock(vm, executable.get(), programSource, { }, error) };
    auto programAfter = programRequestContext();
    if (!u3.check(program && programBefore && programBefore == programAfter, "a program request's context differs from contextDigestOf its recorded inputs, or changes with its generation"_s))
        return false;
    if (!u3.check(!!registry.recordOf(*program.get()) && program.get()->numberOfFunctionDecls() == 1, "the CodeCache's generation of a program did not record it"_s))
        return false;

    // Another offset of the provider, another context; another start column, the same.
    SourceCode shifted { RefPtr<SourceProvider> { provider.ptr() }, 1, static_cast<int>(programText.length()), 1, 1 };
    SourceCode otherColumn { RefPtr<SourceProvider> { provider.ptr() }, 0, static_cast<int>(programText.length()), 1, 9 };
    auto contextOf = [&](const SourceCode& source) -> std::optional<Digest256> {
        Strong<ProgramExecutable> sourceExecutable { vm, ProgramExecutable::create(globalObject, source) };
        RequestState request(vm, RequestKind::Program, source, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *sourceExecutable.get());
        if (!requestKey(request, { }))
            return std::nullopt;
        return requestContext(request);
    };
    auto shiftedContext = contextOf(shifted);
    auto otherColumnContext = contextOf(otherColumn);
    if (!u3.check(shiftedContext && shiftedContext != programBefore && otherColumnContext == programBefore, "a program's context does not follow its offset in the provider, or follows its start column"_s))
        return false;

    // Child body, before and after its generation, and under another start column of its parent.
    UnlinkedFunctionExecutable& child = *program.get()->functionDecl(0);
    auto childContext = [&](const SourceCode& parentSource) -> std::optional<Digest256> {
        SourceCode source = child.linkedSourceCode(parentSource);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = &child;
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::Child, recordedContext(request), std::nullopt) != context)
            return std::nullopt;
        return context;
    };
    auto childBefore = childContext(programSource);
    UnlinkedFunctionCodeBlock* childBody = child.unlinkedCodeBlockFor(vm, child.linkedSourceCode(programSource), CodeSpecializationKind::CodeForCall, { }, error, child.parseMode());
    auto childAfter = childContext(programSource);
    auto childOtherColumn = childContext(otherColumn);
    if (!u3.check(childBody && childBefore && childBefore == childAfter && childOtherColumn == childBefore, "a child body's context differs from its recorded inputs, changes with its generation or with the start column"_s))
        return false;

    // Function-constructor root body: its context covers the holder digest of its UFE.
    String rootText = "function anonymous(a\n) {\nreturn a + 'jitcache-u3-root';\n}"_s;
    SourceCode rootSource = makeSource(rootText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> root { vm, vm.codeCache()->getUnlinkedGlobalFunctionExecutable(vm, Identifier::fromString(vm, "anonymous"_s), rootSource, NoLexicallyScopedFeatures, { }, 22, error) };
    if (!u3.check(root && registry.identityOf(*root.get()), "a Function-constructor root has no identity"_s))
        return false;
    auto rootContext = [&]() -> std::optional<Digest256> {
        SourceCode source = root.get()->linkedSourceCode(rootSource);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = root.get();
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::FunctionConstructor, recordedContext(request), requestHolderDigest(request)) != context)
            return std::nullopt;
        return context;
    };
    auto rootBefore = rootContext();
    UnlinkedFunctionCodeBlock* rootBody = root.get()->unlinkedCodeBlockFor(vm, root.get()->linkedSourceCode(rootSource), CodeSpecializationKind::CodeForCall, { }, error, root.get()->parseMode());
    auto rootAfter = rootContext();
    if (!u3.check(rootBody && rootBefore && rootBefore == rootAfter, "a root body's context differs from its recorded inputs or changes with its generation"_s))
        return false;

    // Module and indirect eval: their executables generate at creation, so their contexts are compared with the inputs.
    SourceCode moduleSource = makeSource("export const jitcacheU3Module = 1;\n"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    Strong<ModuleProgramExecutable> module { vm, ModuleProgramExecutable::tryCreate(globalObject, moduleSource) };
    SourceCode evalSource = makeSource("var jitcacheU3IndirectEval = 2;"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<IndirectEvalExecutable> indirectEval { vm, IndirectEvalExecutable::tryCreate(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, false, EvalContextType::None) };
    if (!u3.check(module && indirectEval, "a module or an indirect eval failed to generate"_s))
        return false;
    auto globalContext = [&](RequestKind kind, GlobalExecutable& global, const SourceCode& source, JSParserScriptMode scriptMode, IdentityKind identityKind) {
        RequestState request(vm, kind, source, { }, global.lexicallyScopedFeatures());
        request.globalExecutable = &global;
        request.scriptMode = scriptMode;
        if (!requestKey(request, { }))
            return false;
        Digest256 expected = globalContextDigest(identityKind, source.startOffset(), source.firstLine().oneBasedInt(), scriptMode, global.derivedContextType(), EvalContextType::None, global.isArrowFunctionContext());
        return requestContext(request) == expected && contextDigestOf(identityKind, recordedContext(request), std::nullopt) == expected;
    };
    if (!u3.check(globalContext(RequestKind::Module, *module.get(), moduleSource, JSParserScriptMode::Module, IdentityKind::Module), "a module request's context differs from its inputs' digest"_s))
        return false;
    if (!u3.check(globalContext(RequestKind::IndirectEval, *indirectEval.get(), evalSource, JSParserScriptMode::Classic, IdentityKind::IndirectEval), "an indirect eval request's context differs from its inputs' digest"_s))
        return false;

    // Direct eval inside the recorded program: its record keeps the digest its request computed.
    TDZEnvironment tdz = tdzNames(vm, { "jitcacheU3EvalTDZ"_s });
    PrivateNameEnvironment privateNames;
    SourceCode directSource = makeSource("jitcacheU3Request + 1"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<DirectEvalExecutable> directEval { vm, DirectEvalExecutable::create(globalObject, directSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, false, EvalContextType::None, &tdz, &privateNames) };
    if (!u3.check(!!directEval, "a direct eval failed to generate"_s))
        return false;
    DirectEvalSite site { program.get(), BytecodeIndex(0) };
    {
        RequestState request(vm, RequestKind::DirectEval, directSource, { }, NoLexicallyScopedFeatures);
        request.globalExecutable = directEval.get();
        request.site = &site;
        request.variablesUnderTDZ = &tdz;
        request.privateNameEnvironment = &privateNames;
        if (!u3.check(!!requestKey(request, { }), "a direct eval whose caller is recorded has no key"_s))
            return false;
        Digest256 context = requestContext(request);
        if (!u3.check(contextDigestOf(IdentityKind::DirectEval, recordedContext(request), std::nullopt) == context, "a direct eval's recorded context differs from its request's"_s))
            return false;
    }

    // The snapshot: a GlobalRequest constructed before its executable's features are overwritten records under the key
    // and key features the snapshot gives.
    String snapshotText = "var jitcacheU3Snapshot = 3;\n"_s;
    SourceCode snapshotSource = makeSource(snapshotText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<ProgramExecutable> snapshotExecutable { vm, ProgramExecutable::create(globalObject, snapshotSource) };
    std::optional<BodyKey> expectedKey;
    std::optional<Digest256> expectedContext;
    {
        RequestState request(vm, RequestKind::Program, snapshotSource, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *snapshotExecutable.get());
        expectedKey = requestKey(request, { });
        if (expectedKey)
            expectedContext = requestContext(request);
    }
    ProgramFixture snapshotUCB = generateFixture(vm, snapshotText);
    {
        GlobalRequest request(vm, *snapshotExecutable.get(), snapshotSource, SourceCodeType::ProgramType, JSParserScriptMode::Classic, { }, EvalContextType::None);
        snapshotExecutable.get()->recordParse(0, StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature, false, 1, 1);
        request.didGenerate(*snapshotUCB.codeBlock.get());
    }
    auto snapshotRecord = registry.recordOf(*snapshotUCB.codeBlock.get());
    if (!u3.check(expectedKey && snapshotRecord && snapshotRecord->key == *expectedKey && snapshotRecord->keyFeatures == NoLexicallyScopedFeatures, "a request's key or key features follow features overwritten after its request object was constructed"_s))
        return false;
    {
        RequestState request(vm, RequestKind::Program, snapshotSource, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *snapshotExecutable.get());
        auto key = requestKey(request, { });
        if (!u3.check(key == expectedKey && requestContext(request) == *expectedContext, "a request's key or context reads the executable's overwritten features instead of the snapshot"_s))
            return false;
    }

    // A decoded program: its own with-scope bit decides, and its own mode enters the key.
    String decodedText = "var jitcacheU3Decoded = 4;\n"_s;
    SourceCode decodedSource = makeSource(decodedText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<ProgramExecutable> decodedExecutable { vm, ProgramExecutable::create(globalObject, decodedSource) };
    ProgramFixture plainUCB = generateProgramFixture(vm, decodedText, 0);
    ProgramFixture debuggerUCB = generateProgramFixture(vm, decodedText, 0, NoLexicallyScopedFeatures, CodeGenerationMode::Debugger);
    ProgramFixture withScopeUCB = generateProgramFixture(vm, decodedText, 0, TaintedByWithScopeLexicallyScopedFeature);
    if (!u3.check(plainUCB.codeBlock && debuggerUCB.codeBlock && withScopeUCB.codeBlock, "a decoded-program fixture failed to generate"_s))
        return false;
    auto decodedKey = [&](UnlinkedGlobalCodeBlock& codeBlock) {
        RequestState request(vm, RequestKind::Program, decodedSource, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *decodedExecutable.get());
        return decodedRootKey(request, codeBlock);
    };
    auto requestKeyWithMode = [&](OptionSet<CodeGenerationMode> mode) {
        RequestState request(vm, RequestKind::Program, decodedSource, mode, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *decodedExecutable.get());
        return requestKey(request, mode);
    };
    auto plainKey = decodedKey(*plainUCB.codeBlock.get());
    auto debuggerKey = decodedKey(*debuggerUCB.codeBlock.get());
    bool decodedKeys = plainKey && plainKey == requestKeyWithMode({ }) && debuggerKey && debuggerKey == requestKeyWithMode(CodeGenerationMode::Debugger)
        && debuggerKey->codeGenerationMode() == OptionSet<CodeGenerationMode> { CodeGenerationMode::Debugger };
    if (!u3.check(decodedKeys, "a decoded program whose with-scope bit is the snapshot's does not take the request's key with its own mode"_s))
        return false;
    return u3.check(!decodedKey(*withScopeUCB.codeBlock.get()), "a decoded program whose with-scope bit differs from the snapshot's has a key"_s);
}

// U3 (section 13.1).
static bool testKeys(VM& vm, String& failure)
{
    SelfTestPart u3 { "U3"_s, failure };
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    if (!u3.check(!!globalObject && vm.jitCacheState() && vm.jitCacheState()->tracksKeys(), "the self-test runs outside a VM entry or in a VM that does not track keys"_s))
        return false;
    return testPinnedDigests(vm, u3) && testBodyKeyBytes(u3) && testContextInputs(vm, u3) && testHolderDigests(vm, globalObject, u3) && testEnvironmentDigests(vm, u3)
        && testRequestKeysAndContexts(vm, globalObject, u3);
}

// U7 (SPEC-ucb.codec.md section 7). First the InlineMap transport of its section 5, over two string-keyed maps.

// The map a VariableEnvironment keeps its declarations in, which the codec transports (codec E4). VariableEnvironment names
// it Map privately, for CachedVariableEnvironment alone, so the test spells it from its public parts, and the assertion
// holds the spelling to the map VariableEnvironment iterates. Its key traits are RefPtr's while its key is a PackedRefPtr,
// which cannot store the deleted value those traits make, so this map takes no removal (codec E4).
using DeclarationMap = InlineMap<PackedRefPtr<UniquedStringImpl>, VariableEnvironmentEntry, VariableEnvironment::inlineMapCapacity, IdentifierRepHash,
    HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits>;
static_assert(std::is_same_v<DeclarationMap::iterator, decltype(std::declval<VariableEnvironment&>().begin())>);

// A map under the same hash whose key traits match its key, so that removals leave the deleted buckets a layout carries.
using RemovableMap = InlineMap<RefPtr<UniquedStringImpl>, VariableEnvironmentEntry, VariableEnvironment::inlineMapCapacity, IdentifierRepHash,
    HashTraits<RefPtr<UniquedStringImpl>>, VariableEnvironmentEntryHashTraits>;

static VariableEnvironmentEntry declarationEntryFor(unsigned keyIndex)
{
    VariableEnvironmentEntry entry;
    if (keyIndex % 2)
        entry.setIsVar();
    else
        entry.setIsLet();
    return entry;
}

template<typename Map>
static Vector<const UniquedStringImpl*> iterationOrder(const Map& map)
{
    Vector<const UniquedStringImpl*> order;
    for (const auto& entry : map)
        order.append(entry.key.get());
    return order;
}

// Whether two maps iterate alike and have the same storage, capacity and deleted count.
template<typename Map>
static bool sameLayout(Map& a, Map& b)
{
    using Access = WTF::InlineMapAccessForTesting;
    return iterationOrder(a) == iterationOrder(b) && Access::isInline(a) == Access::isInline(b) && Access::capacity(a) == Access::capacity(b)
        && Access::deletedCount(a) == Access::deletedCount(b);
}

// What a serializer records of a hashed map: its capacity, each occupied bucket as (index << 1) | isDeleted, and the
// entries of the live buckets in index order.
template<typename Map>
struct InlineMapLayout {
    unsigned capacity { 0 };
    Vector<uint32_t> slots;
    Vector<typename Map::Entry> entries;
};

template<typename Map>
static std::optional<InlineMapLayout<Map>> layoutOf(const Map& map)
{
    if (!map.usesHashedStorage())
        return std::nullopt;
    InlineMapLayout<Map> layout;
    layout.capacity = map.hashedCapacity();
    map.forEachOccupiedBucket([&](unsigned index, bool isDeleted, const typename Map::Entry* entry) {
        layout.slots.append((index << 1) | (isDeleted ? 1 : 0));
        if (!isDeleted)
            layout.entries.append(*entry);
    });
    return layout;
}

// A layout of `count` keys, taken in order from keys[firstKey] on, each in the bucket where its probe sequence starts in a
// table of `capacity` buckets, with no deleted bucket. Every lookup finds its key at once, so the layout meets each
// condition of restoreHashedLayout that does not weigh the capacity against the occupancy.
static std::optional<InlineMapLayout<RemovableMap>> directLayout(const Vector<Identifier>& keys, unsigned firstKey, unsigned capacity, unsigned count)
{
    Vector<std::pair<unsigned, unsigned>> placed; // bucket, key index
    Vector<bool> taken(FillWith { }, capacity, false);
    for (unsigned i = firstKey; i < keys.size() && placed.size() < count; ++i) {
        unsigned bucket = IdentifierRepHash::hash(keys[i].impl()) & (capacity - 1);
        if (taken[bucket])
            continue;
        taken[bucket] = true;
        placed.append({ bucket, i });
    }
    if (placed.size() < count)
        return std::nullopt;
    std::sort(placed.begin(), placed.end());
    InlineMapLayout<RemovableMap> layout;
    layout.capacity = capacity;
    for (auto [bucket, keyIndex] : placed) {
        layout.slots.append(bucket << 1);
        layout.entries.append(RemovableMap::Entry { keys[keyIndex].impl(), declarationEntryFor(keyIndex) });
    }
    return layout;
}

template<typename Map>
static bool refusedAndEmpty(Map& map, unsigned capacity, std::span<const uint32_t> slots, Vector<typename Map::Entry>&& entries)
{
    bool restored = map.restoreHashedLayout(capacity, slots, WTF::move(entries));
    return !restored && map.isEmpty() && !map.usesHashedStorage();
}

// Restores the layout of `map`, a hashed map holding the keys that `present` marks, into a fresh map, which must equal the
// original in iteration order, capacity and deleted count, find exactly the original's keys, and evolve as the original
// does under the same later additions and, for a map that takes them, removals.
template<typename Map>
static bool restoresExactly(SelfTestPart& u7, Map& map, Vector<bool>& present, const Vector<Identifier>& keys, WeakRandom& random, ASCIILiteral mapName)
{
    static constexpr bool takesRemovals = std::is_same_v<Map, RemovableMap>;
    unsigned size = map.size();
    auto layout = layoutOf(map);
    if (!u7.check(!!layout, makeString("a "_s, mapName, " of "_s, size, " entries does not use hashed storage"_s)))
        return false;
    Map restored;
    bool accepted = restored.restoreHashedLayout(layout->capacity, layout->slots.span(), Vector<typename Map::Entry> { layout->entries });
    if (!u7.check(accepted && sameLayout(restored, map), makeString("a hashed "_s, mapName, " of "_s, size, " entries does not restore to the same iteration order, capacity and deleted count"_s)))
        return false;
    bool lookups = true;
    for (unsigned i = 0; i < keys.size(); ++i)
        lookups &= restored.contains(keys[i].impl()) == present[i];
    if (!u7.check(lookups, makeString("a restored "_s, mapName, " of "_s, size, " entries does not find exactly its keys"_s)))
        return false;
    for (unsigned step = 0; step < 60; ++step) {
        unsigned i = random.getUint32(static_cast<unsigned>(keys.size()));
        if (present[i]) {
            if constexpr (takesRemovals) {
                map.remove(keys[i].impl());
                restored.remove(keys[i].impl());
            } else
                continue;
        } else {
            map.add(keys[i].impl(), declarationEntryFor(i));
            restored.add(keys[i].impl(), declarationEntryFor(i));
        }
        present[i] = !present[i];
    }
    return u7.check(sameLayout(restored, map), makeString("a restored "_s, mapName, " of "_s, size, " entries evolves unlike the original under the same additions and removals"_s));
}

// U7: string-keyed maps of 10 to 500 entries restore exactly and then evolve alike: a map whose key traits match its key,
// with random removals, and VariableEnvironment's own map, which takes none (codec E4). Each condition of
// restoreHashedLayout refuses once and leaves the map empty and inline.
static bool testInlineMapTransport(VM& vm, SelfTestPart& u7)
{
    Vector<Identifier> keys;
    for (unsigned i = 0; i < 1100; ++i)
        keys.append(Identifier::fromString(vm, makeString("jitcacheU7Key"_s, i)));
    static constexpr std::array<unsigned, 6> sizes { 10, 13, 40, 97, 256, 500 };

    unsigned hashedLayouts = 0;
    std::optional<InlineMapLayout<RemovableMap>> refusalBase;
    for (unsigned size : sizes) {
        WeakRandom random(size);
        RemovableMap map;
        Vector<bool> present(FillWith { }, keys.size(), false);
        for (unsigned i = 0; i < size; ++i) {
            map.add(keys[i].impl(), declarationEntryFor(i));
            present[i] = true;
        }
        for (unsigned i = 0; i < size; ++i) {
            if (!random.getUint32(3)) {
                map.remove(keys[i].impl());
                present[i] = false;
            }
        }
        auto layout = layoutOf(map);
        if (!layout)
            continue; // removals shrank it back to inline storage, which re-adding reproduces
        ++hashedLayouts;
        if (!refusalBase && layout->slots.size() > layout->entries.size() && layout->entries.size() > 4)
            refusalBase = WTF::move(layout);
        if (!restoresExactly(u7, map, present, keys, random, "map with removals"_s))
            return false;
    }
    if (!u7.check(hashedLayouts >= 4 && !!refusalBase, "too few maps with removals kept hashed storage with deleted buckets"_s))
        return false;

    // VariableEnvironment's map, every size past its inline capacity, so every one is hashed.
    for (unsigned size : sizes) {
        WeakRandom random(size + 1);
        DeclarationMap map;
        Vector<bool> present(FillWith { }, keys.size(), false);
        for (unsigned i = 0; i < size; ++i) {
            map.add(keys[i].impl(), declarationEntryFor(i));
            present[i] = true;
        }
        if (!restoresExactly(u7, map, present, keys, random, "declaration map"_s))
            return false;
    }

    // Each condition broken once, from a valid layout with deleted buckets.
    const InlineMapLayout<RemovableMap>& base = *refusalBase;
    auto refuses = [&](ASCIILiteral condition, unsigned capacity, const Vector<uint32_t>& slots, Vector<RemovableMap::Entry> entries) {
        RemovableMap map;
        return u7.check(refusedAndEmpty(map, capacity, slots.span(), WTF::move(entries)), makeString("restoreHashedLayout accepts "_s, condition, " or leaves the map non-empty"_s));
    };
    {
        RemovableMap map;
        if (!u7.check(map.restoreHashedLayout(base.capacity, base.slots.span(), Vector<RemovableMap::Entry> { base.entries }), "restoreHashedLayout refuses the layout the refusal cases start from"_s))
            return false;
    }
    Vector<uint32_t> swapped = base.slots;
    std::swap(swapped[0], swapped[1]);
    Vector<uint32_t> pastCapacity = base.slots;
    pastCapacity.last() = (base.capacity << 1) | (pastCapacity.last() & 1);
    Vector<RemovableMap::Entry> fewer = base.entries;
    fewer.removeLast();
    Vector<RemovableMap::Entry> duplicated = base.entries;
    duplicated[1] = duplicated[0];
    Vector<RemovableMap::Entry> nullKey = base.entries;
    nullKey[0].key = nullptr;
    // A live entry moved to an empty bucket. In the valid layout every empty bucket lies past the entry's own on its probe
    // sequence, or the probe would have stopped there, so the entry's own bucket, now empty, stops the probe first.
    Vector<uint32_t> movedSlots;
    Vector<RemovableMap::Entry> movedEntries;
    {
        Vector<std::pair<uint32_t, std::optional<size_t>>> buckets; // slot, and the entry a live slot holds
        size_t nextEntry = 0;
        for (uint32_t slot : base.slots)
            buckets.append({ slot, (slot & 1) ? std::nullopt : std::optional<size_t> { nextEntry++ } });
        Vector<bool> occupied(FillWith { }, base.capacity, false);
        for (uint32_t slot : base.slots)
            occupied[slot >> 1] = true;
        unsigned target = 0;
        while (occupied[target])
            ++target;
        for (auto& bucket : buckets) {
            if (bucket.second) {
                bucket.first = target << 1;
                break;
            }
        }
        std::sort(buckets.begin(), buckets.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (auto& bucket : buckets) {
            movedSlots.append(bucket.first);
            if (bucket.second)
                movedEntries.append(base.entries[*bucket.second]);
        }
    }
    // Load and minimum load, from layouts whose keys each sit where their probe starts, so that nothing else is wrong with
    // them: one with the same occupancy restores, while thirteen live buckets in sixteen pass the most a map built by add()
    // holds, and 128 buckets for eleven entries pass minLoadInverse (6) times the entries, which a removal would shrink.
    auto loadControl = directLayout(keys, 1000, 64, 11);
    auto crowded = directLayout(keys, 1000, 16, 13);
    auto sparse = directLayout(keys, 1000, 128, 11);
    if (!u7.check(loadControl && crowded && sparse, "the load test layouts could not be built from the test keys"_s))
        return false;
    {
        RemovableMap map;
        if (!u7.check(map.restoreHashedLayout(loadControl->capacity, loadControl->slots.span(), Vector<RemovableMap::Entry> { loadControl->entries }), "restoreHashedLayout refuses a layout whose keys sit where their probes start"_s))
            return false;
    }

    // The deleted value as a key. A RefPtr holding it must be neither copied nor destroyed, which would ref or deref it, so
    // the entry is made in place, and its key is leaked once the refusal has left the vector untouched.
    bool refusesDeletedKey = false;
    {
        Vector<RemovableMap::Entry> deletedKey = base.entries;
        deletedKey[0].key = RefPtr<UniquedStringImpl> { WTF::HashTableDeletedValue };
        RemovableMap map;
        bool restored = map.restoreHashedLayout(base.capacity, base.slots.span(), WTF::move(deletedKey));
        refusesDeletedKey = !restored && map.isEmpty() && !map.usesHashedStorage() && deletedKey.size() == base.entries.size();
        std::ignore = deletedKey[0].key.leakRef();
    }
    if (!u7.check(refusesDeletedKey, "restoreHashedLayout accepts the deleted value as a key or leaves the map non-empty"_s))
        return false;

    return refuses("a capacity that is not a power of two"_s, base.capacity + 1, base.slots, base.entries)
        && refuses("a capacity no larger than the inline capacity"_s, 8, base.slots, base.entries)
        && refuses("slot indices out of order"_s, base.capacity, swapped, base.entries)
        && refuses("a slot index at the capacity"_s, base.capacity, pastCapacity, base.entries)
        && refuses("fewer entries than live slots"_s, base.capacity, base.slots, fewer)
        && refuses("a duplicated key"_s, base.capacity, base.slots, duplicated)
        && refuses("an empty key"_s, base.capacity, base.slots, nullKey)
        && refuses("an entry its probe sequence cannot reach"_s, base.capacity, movedSlots, movedEntries)
        && refuses("more live buckets than the load factor allows"_s, crowded->capacity, crowded->slots, crowded->entries)
        && refuses("a capacity above minLoadInverse times the entries"_s, sparse->capacity, sparse->slots, sparse->entries);
}

// Programs in the manner of JSTests/stress, together covering the forms a core encodes: closures and loops, integer,
// character and string switches, exception handlers, classes with fields, private names, static blocks and super, generators
// and async functions, destructuring and spread, regular expressions, big integers, doubles, templates, butterflies, a literal
// whose elements the parser folds to NaNs of both signs (codec section 7), more than nine global declarations (F10), with and
// eval, accessors and computed keys, and nested block scopes.
static constexpr std::array<ASCIILiteral, 7> codecCorpus { {
    "function jitcacheU7Switch(x, s) {\n"
    "  var total = 0;\n"
    "  outer: for (var i = 0; i < x; i++) {\n"
    "    switch (i % 5) { case 0: total += 1; break; case 1: total -= 2; break; case 2: case 3: total *= 2; break; default: continue outer; }\n"
    "    switch (s[i % s.length]) { case 'a': total++; break; case 'b': total--; break; }\n"
    "    switch (s) { case 'alpha': total += 3; break; case 'beta': total += 4; break; case 'gamma': total += 5; break; }\n"
    "  }\n"
    "  try { if (total > 100) throw new Error('jitcache-u7-big'); } catch (e) { total = -1; } finally { total += arguments.length; }\n"
    "  return function inner(y) { return total + y; };\n"
    "}\n"
    "var jitcacheU7SwitchResult = jitcacheU7Switch(10, 'abba');\n"_s,
    "class JitcacheU7Base { #secret = 1; static count = 0; static { JitcacheU7Base.count++; } get secret() { return this.#secret; } #hidden() { return 2; } reveal() { return this.#hidden() + this.#secret; } }\n"
    "class JitcacheU7Derived extends JitcacheU7Base { extra = [\"alpha\", \"beta\"]; constructor() { super(); this.y = 3; } static make() { return new JitcacheU7Derived(); } }\n"
    "let jitcacheU7Instance = JitcacheU7Derived.make();\n"_s,
    "function* jitcacheU7Gen(n) { for (let i = 0; i < n; i++) yield i; }\n"
    "async function jitcacheU7Async(a, { b, c = 2 } = {}, ...rest) { const [d, ...e] = await Promise.resolve([a, b, c]); return d?.toString() ?? e.length + rest.length; }\n"
    "async function* jitcacheU7AsyncGen() { yield* jitcacheU7Gen(3); }\n"
    "const jitcacheU7Spread = [...jitcacheU7Gen(4), ...\"xyz\"];\n"_s,
    "var jitcacheU7Literals = { re: /jit(cache)+\\d*/gi, big: 12345678901234567890123n, d: 1.5, n: NaN, z: -0, inf: Infinity, words: [\"one\", \"two\", \"three\"], mixed: [\"one\", 2], hole: [1, , 3],\n"
    "  nans: [1e400 * 0, -(1e400 * 0)] };\n"
    "function jitcacheU7Tag(strings, ...values) { return strings.raw.join('|') + values.join(','); }\n"
    "var jitcacheU7Template = jitcacheU7Tag`a${1}b${2}c` + `plain ${jitcacheU7Literals.d}`;\n"_s,
    "var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11, v12 = 12;\n"
    "let l0 = 0, l1 = 1, l2 = 2, l3 = 3, l4 = 4, l5 = 5, l6 = 6, l7 = 7, l8 = 8, l9 = 9, l10 = 10;\n"
    "function jitcacheU7Sloppy(o, code) { with (o) { var r = x; } return eval(code) + r; }\n"_s,
    "var jitcacheU7Object = { get g() { return 1; }, set s(v) { this._s = v; }, ['computed' + 1]: 2, [Symbol.iterator]: function* () { yield 1; } };\n"
    "function jitcacheU7Walk(o) { var keys = []; for (var k in o) keys.push(k); for (var v of [1, 2]) keys.push(v); delete o.g; return ('s' in o) && (o instanceof Object) && typeof o; }\n"_s,
    // Two sibling functions of one block, whose chains of two links share both, and an arrow in each.
    "'use strict';\n"
    "{ let jitcacheU7Outer = 1; { const jitcacheU7Inner = 2;\n"
    "  function jitcacheU7Nested() { return () => jitcacheU7Outer + jitcacheU7Inner; }\n"
    "  function jitcacheU7Sibling() { return () => jitcacheU7Inner; }\n"
    "  jitcacheU7Nested(); jitcacheU7Sibling(); } }\n"_s,
} };
static constexpr size_t nestedScopesCorpusIndex = 6;
static_assert(nestedScopesCorpusIndex + 1 == codecCorpus.size());

// The codec's other root records (codec section 3): a module, whose var declarations, its import among them, outnumber an
// InlineMap's inline capacity, so its core crosses E4's hashed layout; an indirect eval with var and function declarations
// and a sloppy block function, which the eval keeps as a hoisting candidate; and a direct eval under a TDZ set, whose
// function's chain starts at the link over that set and goes into the core whole, since an eval core has no holder.
static constexpr ASCIILiteral moduleCorpusText = "import { jitcacheU7Imported } from 'jitcache-u7-module-dependency';\n"
    "var jitcacheU7M0 = 0, jitcacheU7M1 = 1, jitcacheU7M2 = 2, jitcacheU7M3 = 3, jitcacheU7M4 = 4, jitcacheU7M5 = 5;\n"
    "var jitcacheU7M6 = 6, jitcacheU7M7 = 7, jitcacheU7M8 = 8, jitcacheU7M9 = 9, jitcacheU7M10 = 10, jitcacheU7M11 = 11;\n"
    "let jitcacheU7ModuleLet = 'jitcache-u7-module-let';\n"
    "const jitcacheU7ModuleConst = [jitcacheU7M0, jitcacheU7Imported];\n"
    "export function jitcacheU7ModuleExported(a) { return a + jitcacheU7M1 + jitcacheU7ModuleLet.length; }\n"
    "export default class JitcacheU7ModuleDefault { static m() { return jitcacheU7ModuleConst; } }\n"_s;
static constexpr ASCIILiteral indirectEvalCorpusText = "var jitcacheU7EvalA = 1, jitcacheU7EvalB = 'jitcache-u7-eval-string';\n"
    "function jitcacheU7EvalFunction(a) { return a + jitcacheU7EvalA; }\n"
    "if (jitcacheU7EvalA) { function jitcacheU7EvalHoisted() { return jitcacheU7EvalB; } }\n"
    "let jitcacheU7EvalLet = [jitcacheU7EvalA, jitcacheU7EvalB];\n"
    "jitcacheU7EvalFunction(jitcacheU7EvalLet.length);\n"_s;
static constexpr ASCIILiteral directEvalCorpusText = "let jitcacheU7DirectLet = 2;\n"
    "function jitcacheU7DirectFunction() { return jitcacheU7DirectOuter + jitcacheU7DirectLet; }\n"
    "var jitcacheU7DirectVar = jitcacheU7DirectFunction;\n"_s;

// A program whose core spans several pages.
static String bigProgramText()
{
    StringBuilder text;
    for (unsigned i = 0; i < 400; ++i)
        text.append("function jitcacheU7Big"_s, i, "(a) { return a + \"jitcache-u7-big-constant-"_s, i, "\"; }\n"_s);
    return text.toString();
}

// A class whose field initializer's descriptor spans several pages.
static String manyFieldsText()
{
    StringBuilder text;
    text.append("class JitcacheU7Fields { "_s);
    for (unsigned i = 0; i < 1500; ++i)
        text.append("jitcacheU7Field"_s, i, " = 0; "_s);
    text.append("}\n"_s);
    return text.toString();
}

// The chain shape T5 compares: the number of links, and where the chain reaches `target`.
struct ChainShape {
    unsigned length { 0 };
    std::optional<unsigned> targetPosition;
    friend bool operator==(const ChainShape&, const ChainShape&) = default;
};

static ChainShape chainShape(TDZEnvironmentLink* link, const TDZEnvironmentLink* target)
{
    ChainShape shape;
    for (; link; link = link->parent()) {
        if (target && link == target && !shape.targetPosition)
            shape.targetPosition = shape.length;
        ++shape.length;
    }
    return shape;
}

// Whether a declaration map holds a symbol key. A symbol's hash is per process, so a hashed map holding one would lay
// out differently in another process; codec section 6 has the self-test assert that no corpus program declares one.
static bool holdsSymbolKey(const VariableEnvironment& environment)
{
    for (const auto& entry : environment) {
        if (entry.key->isSymbol())
            return true;
    }
    return false;
}

static bool sameDeclarationOrder(const VariableEnvironment& a, const VariableEnvironment& b)
{
    if (a.mapSize() != b.mapSize())
        return false;
    auto other = b.begin();
    for (const auto& entry : a) {
        if (other == b.end() || entry.key.get() != other->key.get() || !(entry.value == other->value))
            return false;
        ++other;
    }
    return true;
}

static bool isAtomString(JSValue value)
{
    auto* string = value ? dynamicDowncast<JSString>(value) : nullptr;
    StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
    return impl && impl->isAtom();
}

static bool allStringConstantsAreAtoms(UnlinkedCodeBlock& codeBlock)
{
    for (auto& constant : codeBlock.constantRegisters()) {
        JSValue value = constant.get();
        if (value && value.isString() && !isAtomString(value))
            return false;
    }
    return true;
}

// The comparison of twins T1 to T5 between a UCB and the UCB its core decodes to (codec section 7): the same encoding, every
// string constant register an atom, the same global parse fields and declaration orders, the same feedback layout, and
// children with the same fields, unparsed, whose chains have the same shape over the holder's.
static bool sameAsDecoded(VM& vm, SelfTestPart& u7, UnlinkedCodeBlock& original, UnlinkedCodeBlock& decoded, const UnlinkedFunctionExecutable* holder, std::span<const uint8_t> core, ASCIILiteral what)
{
    auto encoding = encodeCoreBytes(vm, decoded, holder);
    if (!u7.check(encoding && equalSpans(encoding->span(), core), makeString("decoding and re-encoding "_s, what, " does not give its core back"_s)))
        return false;
    if (!u7.check(allStringConstantsAreAtoms(decoded), makeString("a string constant register of decoded "_s, what, " is not an atom"_s)))
        return false;
    if (!u7.check(liveFeedbackCounts(original) == liveFeedbackCounts(decoded), makeString("decoded "_s, what, " has other profile, child or constant counts"_s)))
        return false;
    // UnlinkedGlobalCodeBlock declares no ClassInfo, so a dynamic cast to it accepts a function UCB, whose cell lacks these
    // fields. A function body's parse fields are its UFE's (T2); they travel in the identity section, and both sides share the holder.
    if (!is<UnlinkedFunctionCodeBlock>(original)) {
        auto& originalGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(original);
        auto& decodedGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(decoded);
        bool sameParse = originalGlobal.codeFeatures() == decodedGlobal.codeFeatures() && originalGlobal.lexicallyScopedFeatures() == decodedGlobal.lexicallyScopedFeatures()
            && originalGlobal.hasCapturedVariables() == decodedGlobal.hasCapturedVariables() && originalGlobal.lineCount() == decodedGlobal.lineCount()
            && originalGlobal.endColumn() == decodedGlobal.endColumn() && WTF::equal(originalGlobal.sourceURLDirective(), decodedGlobal.sourceURLDirective())
            && WTF::equal(originalGlobal.sourceMappingURLDirective(), decodedGlobal.sourceMappingURLDirective());
        if (!u7.check(sameParse, makeString("decoded "_s, what, " has other global parse fields"_s)))
            return false;
    }
    if (auto* originalProgram = dynamicDowncast<UnlinkedProgramCodeBlock>(original)) {
        auto& decodedProgram = uncheckedDowncast<UnlinkedProgramCodeBlock>(decoded);
        bool sameOrder = sameDeclarationOrder(originalProgram->variableDeclarations(), decodedProgram.variableDeclarations())
            && sameDeclarationOrder(originalProgram->lexicalDeclarations(), decodedProgram.lexicalDeclarations());
        if (!u7.check(sameOrder, makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalModule = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(original)) {
        auto& decodedModule = uncheckedDowncast<UnlinkedModuleProgramCodeBlock>(decoded);
        if (!u7.check(sameDeclarationOrder(originalModule->variableDeclarations(), decodedModule.variableDeclarations()), makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalEval = dynamicDowncast<UnlinkedEvalCodeBlock>(original)) {
        auto& decodedEval = uncheckedDowncast<UnlinkedEvalCodeBlock>(decoded);
        bool sameNames = std::ranges::equal(originalEval->variables(), decodedEval.variables())
            && std::ranges::equal(originalEval->functionHoistingCandidates(), decodedEval.functionHoistingCandidates());
        if (!u7.check(sameNames, makeString("decoded "_s, what, " has other variables or function hoisting candidates"_s)))
            return false;
    }
    const TDZEnvironmentLink* holderChain = holder ? holder->parentScopeTDZVariables().get() : nullptr;
    auto sameChild = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!child || !decodedChild)
            return false;
        auto descriptor = encodeDescriptorBytes(vm, *child);
        auto decodedDescriptor = encodeDescriptorBytes(vm, *decodedChild);
        return descriptor && decodedDescriptor && *descriptor == *decodedDescriptor && !decodedChild->features() && !decodedChild->hasCapturedVariables()
            && decodedChild->lexicallyScopedFeatures() == child->lexicallyScopedFeatures()
            && chainShape(decodedChild->parentScopeTDZVariables().get(), holderChain) == chainShape(child->parentScopeTDZVariables().get(), holderChain);
    };
    for (unsigned i = 0; i < original.numberOfFunctionDecls(); ++i) {
        if (!u7.check(sameChild(original.functionDecl(i), decoded.functionDecl(i)), makeString("function declaration "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    for (unsigned i = 0; i < original.numberOfFunctionExprs(); ++i) {
        if (!u7.check(sameChild(original.functionExpr(i), decoded.functionExpr(i)), makeString("function expression "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    return true;
}

// Encodes twice, decodes without and with validation, and compares each decode with the original.
static bool roundTripsCore(VM& vm, SelfTestPart& u7, UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, ASCIILiteral what)
{
    auto core = encodeCoreBytes(vm, codeBlock, holder);
    auto again = encodeCoreBytes(vm, codeBlock, holder);
    if (!u7.check(core && again && *core == *again, makeString("encoding "_s, what, " twice does not give equal bytes"_s)))
        return false;
    for (bool validate : { false, true }) {
        CoreDecodeFailure failure = CoreDecodeFailure::None;
        Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, core->span(), provider, kind, holder, validate, failure) };
        if (!u7.check(decoded && failure == CoreDecodeFailure::None, makeString("the core of "_s, what, validate ? " does not decode with validation"_s : " does not decode"_s)))
            return false;
        if (!sameAsDecoded(vm, u7, codeBlock, *decoded.get(), holder, core->span(), what))
            return false;
    }
    return true;
}

// U7: the corpus module, both corpus evals, every corpus program and the functions the programs declare encode
// deterministically, decode without and with validation to UCBs equal field by field, and re-encode to their cores.
static bool testCodecCorpus(VM& vm, JSGlobalObject* globalObject, SelfTestPart& u7)
{
    // An import decodes every core kind (section 7.3.1, step 6), so the round trip covers each root record the codec has.
    {
        SourceCode moduleSource = makeSource(String { moduleCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
        ParserError error;
        Strong<UnlinkedModuleProgramCodeBlock> module { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, moduleSource, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
        bool hashedDeclarations = module && module.get()->variableDeclarations().mapSize() > VariableEnvironment::inlineMapCapacity;
        if (!u7.check(hashedDeclarations, "the corpus module failed to generate, or its declarations fit inline storage, so E4's layout goes untested"_s))
            return false;
        if (!u7.check(!holdsSymbolKey(module.get()->variableDeclarations()), "the corpus module declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        if (!roundTripsCore(vm, u7, *module.get(), nullptr, *moduleSource.provider(), UnlinkedCodeBlockCoreKind::Module, "the corpus module"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { indirectEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<IndirectEvalExecutable> indirectEval { vm, IndirectEvalExecutable::tryCreate(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, false, EvalContextType::None) };
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, indirectEval ? indirectEval.get()->unlinkedCodeBlock() : nullptr };
        bool namesDeclarations = codeBlock && codeBlock.get()->numVariables() && codeBlock.get()->numFunctionHoistingCandidates();
        if (!u7.check(namesDeclarations, "the corpus indirect eval failed to generate, or lacks its variables or its hoisting candidate"_s))
            return false;
        if (!roundTripsCore(vm, u7, *codeBlock.get(), nullptr, *evalSource.provider(), UnlinkedCodeBlockCoreKind::Eval, "the corpus indirect eval"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { directEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        TDZEnvironment variablesUnderTDZ = tdzNames(vm, { "jitcacheU7DirectOuter"_s });
        PrivateNameEnvironment privateNames;
        DirectEvalExecutable* directEval = DirectEvalExecutable::create(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
            PrivateBrandRequirement::None, false, true, EvalContextType::FunctionEvalContext, &variablesUnderTDZ, &privateNames);
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, directEval ? directEval->unlinkedCodeBlock() : nullptr };
        UnlinkedFunctionExecutable* function = nullptr;
        if (codeBlock) {
            function = selectChild(*codeBlock.get(), [](UnlinkedFunctionExecutable&) {
                return true;
            });
        }
        if (!u7.check(function && function->parentScopeTDZVariables(), "the corpus direct eval failed to generate, or its function holds no TDZ chain"_s))
            return false;
        if (!roundTripsCore(vm, u7, *codeBlock.get(), nullptr, *evalSource.provider(), UnlinkedCodeBlockCoreKind::Eval, "the corpus direct eval"_s))
            return false;
    }

    Vector<String> texts;
    for (ASCIILiteral text : codecCorpus)
        texts.append(String { text });
    texts.append(bigProgramText());
    for (const String& text : texts) {
        // Only roundTripsChild generates children, never a class field initializer's (see there).
        ProgramFixture fixture = generateProgramFixture(vm, text, 0);
        if (!u7.check(!!fixture.codeBlock, "a corpus program failed to generate"_s))
            return false;
        UnlinkedProgramCodeBlock& program = *fixture.codeBlock.get();
        SourceProvider& provider = *fixture.source.provider();
        bool declaresSymbol = holdsSymbolKey(program.variableDeclarations()) || holdsSymbolKey(program.lexicalDeclarations());
        if (!u7.check(!declaresSymbol, "a corpus program declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        auto roundTripsChild = [&](UnlinkedFunctionExecutable* child) {
            if (!child)
                return false;
            // A class field initializer's body is never generated here. Its parse makes one DefineFieldNode per field, a
            // parser-arena node whose destructor never runs but which holds the field's Identifier, so each field name's atom
            // leaks with JITCache off too; the self-test has no oracle run, so it avoids input the engine leaks on
            // (SPEC-integrator.harness.md). The initializer's descriptor, class element definitions included, goes through
            // the program's core.
            if (child->parseMode() == SourceParseMode::ClassFieldInitializerMode)
                return true;
            CodeSpecializationKind kind = child->isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
            ParserError error;
            Strong<UnlinkedFunctionCodeBlock> body { vm, child->unlinkedCodeBlockFor(vm, child->linkedSourceCode(fixture.source), kind, { }, error, child->parseMode()) };
            if (!body)
                return true; // a body that does not generate on its own
            return roundTripsCore(vm, u7, *body.get(), child, provider, UnlinkedCodeBlockCoreKind::Function, "a corpus function"_s);
        };
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionDecls(), 8); ++i) {
            if (!roundTripsChild(program.functionDecl(i)))
                return false;
        }
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionExprs(), 8); ++i) {
            if (!roundTripsChild(program.functionExpr(i)))
                return false;
        }
        // After the children's round trips, whose generation parsed them and filled their slots: the program's decoded
        // children must still come back unparsed, with the lexically scoped features generation gave them (codec E1, E2).
        if (!roundTripsCore(vm, u7, program, nullptr, provider, UnlinkedCodeBlockCoreKind::Program, "a corpus program"_s))
            return false;
    }
    return true;
}

// Counts what an encode keeps charged (signed, so a release of something never charged shows), and refuses, for good,
// from the `refuseAt`-th charge of at least a first page's size; zero never refuses.
class TestEncodingBudget final : public CoreEncodingBudget {
public:
    explicit TestEncodingBudget(unsigned refuseAt = 0)
        : m_refuseAt(refuseAt)
    {
    }

    bool charge(size_t bytes) final
    {
        if (m_refused)
            return false;
        if (!m_firstCharge)
            m_firstCharge = bytes;
        if (bytes >= firstPageSize && m_refuseAt && ++m_pageSizedCharges == m_refuseAt) {
            m_refused = true;
            return false;
        }
        m_charged += static_cast<int64_t>(bytes);
        return true;
    }

    void release(size_t bytes) final { m_charged -= static_cast<int64_t>(bytes); }

    int64_t charged() const { return m_charged; }
    bool refused() const { return m_refused; }
    size_t firstCharge() const { return m_firstCharge; }

    static constexpr size_t firstPageSize = 4096;

private:
    const unsigned m_refuseAt;
    unsigned m_pageSizedCharges { 0 };
    bool m_refused { false };
    size_t m_firstCharge { 0 };
    int64_t m_charged { 0 };
};

// U7: the chunks of both entry points concatenate to the payloads the releasing encoders return, and the first page is
// 4 KiB (E12, E13); with a counting budget a payload's size stays charged; under a budget that refuses the third page every
// entry point returns BudgetRefused, the sinks see nothing and the budget is back at zero (E8).
static bool testCodecChunksAndBudgets(VM& vm, SelfTestPart& u7)
{
    ProgramFixture big = generateProgramFixture(vm, bigProgramText(), 0);
    ProgramFixture small = generateProgramFixture(vm, "var jitcacheU7Small = 1;\n"_s, 0);
    ProgramFixture fields = generateProgramFixture(vm, manyFieldsText(), 0);
    ProgramFixture tiny = generateProgramFixture(vm, "function jitcacheU7Tiny() { }\n"_s, 0);
    if (!u7.check(big.codeBlock && small.codeBlock && fields.codeBlock && tiny.codeBlock, "a chunking fixture failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable* initializer = selectChild(*fields.codeBlock.get(), [](UnlinkedFunctionExecutable& executable) {
        return executable.parseMode() == SourceParseMode::ClassFieldInitializerMode;
    });
    UnlinkedFunctionExecutable* tinyFunction = tiny.codeBlock.get()->functionDecl(0);
    if (!u7.check(initializer && tinyFunction, "a chunking fixture lacks its UFE"_s))
        return false;

    auto coreChunks = [&](UnlinkedCodeBlock& codeBlock, CoreEncodingBudget* budget, Vector<size_t>& sizes, CoreEncodeFailure& failure) {
        Vector<uint8_t> bytes;
        failure = forEachUnlinkedCodeBlockCoreChunk(vm, codeBlock, nullptr, budget, [&](std::span<const uint8_t> chunk) {
            sizes.append(chunk.size());
            bytes.append(chunk);
        });
        return bytes;
    };
    auto descriptorChunks = [&](UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, Vector<size_t>& sizes, CoreEncodeFailure& failure) {
        Vector<uint8_t> bytes;
        failure = forEachUnlinkedFunctionExecutableDescriptorChunk(vm, executable, budget, [&](std::span<const uint8_t> chunk) {
            sizes.append(chunk.size());
            bytes.append(chunk);
        });
        return bytes;
    };

    for (auto* fixture : { &big, &small }) {
        UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
        auto payload = encodeCoreBytes(vm, codeBlock, nullptr);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        Vector<uint8_t> concatenated = coreChunks(codeBlock, nullptr, sizes, failure);
        bool chunked = payload && failure == CoreEncodeFailure::None && concatenated == *payload && !sizes.isEmpty() && sizes[0] <= TestEncodingBudget::firstPageSize;
        if (!u7.check(chunked, "a core's chunks do not concatenate to its payload, or its first page is larger than 4 KiB"_s))
            return false;
        if (fixture == &big && !u7.check(sizes.size() > 2, "the large program's core does not span three pages"_s))
            return false;
    }
    for (auto* executable : { initializer, tinyFunction }) {
        auto payload = encodeDescriptorBytes(vm, *executable);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        Vector<uint8_t> concatenated = descriptorChunks(*executable, nullptr, sizes, failure);
        bool chunked = payload && failure == CoreEncodeFailure::None && concatenated == *payload && !sizes.isEmpty() && sizes[0] <= TestEncodingBudget::firstPageSize;
        if (!u7.check(chunked, "a descriptor's chunks do not concatenate to its payload, or its first page is larger than 4 KiB"_s))
            return false;
        if (executable == initializer && !u7.check(sizes.size() > 2, "the large descriptor does not span three pages"_s))
            return false;
    }

    // A counting budget: the first charge is the first page, and exactly the payload's size stays charged.
    {
        TestEncodingBudget budget;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!u7.check(core && budget.firstCharge() == TestEncodingBudget::firstPageSize && budget.charged() == static_cast<int64_t>(core->size()), "a core encode does not start with a 4 KiB page or does not leave its payload's size charged"_s))
            return false;
        TestEncodingBudget descriptorBudget;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &descriptorBudget, failure);
        if (!u7.check(descriptor && descriptorBudget.charged() == static_cast<int64_t>(descriptor->size()), "a descriptor encode does not leave its payload's size charged"_s))
            return false;
    }

    // A budget that refuses the third page.
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!u7.check(!core && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a core encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &budget, failure);
        if (!u7.check(!descriptor && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a descriptor encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        coreChunks(*big.codeBlock.get(), &budget, sizes, failure);
        if (!u7.check(failure == CoreEncodeFailure::BudgetRefused && sizes.isEmpty() && !budget.charged(), "the core chunks under a refusing budget reach the sink or leave a charge"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        descriptorChunks(*initializer, &budget, sizes, failure);
        if (!u7.check(failure == CoreEncodeFailure::BudgetRefused && sizes.isEmpty() && !budget.charged(), "the descriptor chunks under a refusing budget reach the sink or leave a charge"_s))
            return false;
    }
    return true;
}

static SourceCodeKey programCacheKey(const SourceCode& source)
{
    return SourceCodeKey { source, String { }, SourceCodeType::ProgramType, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false, { }, std::nullopt };
}

// Bytecode-cache payloads a test marks persistent. The blocks decoded from them borrow their bytes, so the bytes live for
// the process, as an embedder's mapped executable section does (bunpatches.md, Startup).
static Vector<Vector<uint8_t>>& persistentPayloads()
{
    static NeverDestroyed<Vector<Vector<uint8_t>>> payloads;
    return payloads.get();
}

// The function body a natively decoded program's lazily cached slot decodes to, at its first request.
static UnlinkedFunctionCodeBlock* decodedBody(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& parentSource)
{
    ParserError error;
    CodeSpecializationKind kind = executable.isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
    return executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(parentSource), kind, { }, error, executable.parseMode());
}

static std::optional<uint32_t> butterflyConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<JSCellButterfly>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// U7: what the native bytecode cache produces encodes as generation's twin would (codec section 6): a decoded function with
// plain string constants and a plain butterfly encodes to its generated twin's bytes and decodes with atoms; two decodes
// of one payload whose functions run in different orders encode alike; a decoded parent whose children are still cached
// encodes twice alike and leaves their decoders usable; a borrowed stream encodes as its copy.
static bool testCodecNativeDecodes(VM& vm, SelfTestPart& u7)
{
    String text = "function jitcacheU7Atoms(s) { var long = 'jitcache-u7-a-long-string-constant-that-is-no-inline-string'; return ['jitcache-u7-word-one', 'jitcache-u7-word-two'].indexOf(s) + long.length; }\n"
        "function jitcacheU7Second(s) { return 'jitcache-u7-a-long-string-constant-that-is-no-inline-string' + s + 'jitcache-u7-second-only-string'; }\n"_s;
    ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
    if (!u7.check(!!generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 2, "the native-decode program failed to generate"_s))
        return false;
    SourceCodeKey key = programCacheKey(generated.source);
    RefPtr<CachedBytecode> payload = encodeCodeBlock(vm, key, generated.codeBlock.get());
    if (!u7.check(!!payload, "the native bytecode cache did not encode the program"_s))
        return false;
    auto decode = [&](Ref<CachedBytecode>&& bytes) {
        return Strong<UnlinkedProgramCodeBlock> { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, WTF::move(bytes)) };
    };

    // A decoded function against its generated twin.
    auto decoded = decode(Ref<CachedBytecode> { *payload });
    if (!u7.check(!!decoded, "the native bytecode cache did not decode the program"_s))
        return false;
    UnlinkedFunctionExecutable& generatedFunction = *generated.codeBlock.get()->functionDecl(0);
    UnlinkedFunctionExecutable& decodedFunction = *decoded.get()->functionDecl(0);
    Strong<UnlinkedFunctionCodeBlock> generatedBody { vm, decodedBody(vm, generatedFunction, generated.source) };
    Strong<UnlinkedFunctionCodeBlock> nativeBody { vm, decodedBody(vm, decodedFunction, generated.source) };
    if (!u7.check(generatedBody && nativeBody, "a function body of the native-decode program is missing"_s))
        return false;
    auto literal = butterflyConstant(*nativeBody.get());
    bool nativeForms = !allStringConstantsAreAtoms(*nativeBody.get()) && literal && constantCell<JSCellButterfly>(*nativeBody.get(), *literal)->structure() != vm.cellButterflyOnlyAtomStringsStructure.get();
    if (!u7.check(nativeForms, "the native decode made every string constant an atom or gave the literal the atom form, so the case is untested"_s))
        return false;
    auto generatedCore = encodeCoreBytes(vm, *generatedBody.get(), &generatedFunction);
    auto nativeCore = encodeCoreBytes(vm, *nativeBody.get(), &decodedFunction);
    if (!u7.check(generatedCore && nativeCore && *generatedCore == *nativeCore, "a natively decoded function does not encode to its generated twin's bytes"_s))
        return false;
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> fromCore { vm, decodeCoreBytes(vm, nativeCore->span(), *generated.source.provider(), UnlinkedCodeBlockCoreKind::Function, &decodedFunction, false, failure) };
    if (!u7.check(fromCore && allStringConstantsAreAtoms(*fromCore.get()), "a core decoded from a natively decoded function does not make every string constant register an atom"_s))
        return false;
    auto* nativeLiteral = constantCell<JSCellButterfly>(*nativeBody.get(), *literal);
    auto* coreLiteral = constantCell<JSCellButterfly>(*fromCore.get(), *literal);
    bool literalKeepsDecode = coreLiteral && coreLiteral->structure() == vm.cellButterflyStructure(coreLiteral->indexingMode()) && coreLiteral->length() == nativeLiteral->length();
    for (unsigned i = 0; literalKeepsDecode && i < coreLiteral->length(); ++i) {
        JSValue element = coreLiteral->get(i);
        literalKeepsDecode = element.isString() && asString(element)->tryGetValueImpl() && String { asString(element)->tryGetValueImpl() } == String { asString(nativeLiteral->get(i))->tryGetValueImpl() };
    }
    if (!u7.check(literalKeepsDecode, "a decoded core's butterfly does not keep the plain structure and the string elements its decode gives"_s))
        return false;

    // Two decodes of one payload whose functions are first requested in different orders.
    auto first = decode(Ref<CachedBytecode> { *payload });
    auto second = decode(Ref<CachedBytecode> { *payload });
    if (!u7.check(first && second, "a second native decode of the payload failed"_s))
        return false;
    Strong<UnlinkedFunctionCodeBlock> firstA { vm, decodedBody(vm, *first.get()->functionDecl(0), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> firstB { vm, decodedBody(vm, *first.get()->functionDecl(1), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> secondB { vm, decodedBody(vm, *second.get()->functionDecl(1), generated.source) };
    Strong<UnlinkedFunctionCodeBlock> secondA { vm, decodedBody(vm, *second.get()->functionDecl(0), generated.source) };
    bool sameAcrossOrders = firstA && firstB && secondA && secondB
        && encodeCoreBytes(vm, *first.get(), nullptr) == encodeCoreBytes(vm, *second.get(), nullptr)
        && encodeCoreBytes(vm, *firstA.get(), first.get()->functionDecl(0)) == encodeCoreBytes(vm, *secondA.get(), second.get()->functionDecl(0))
        && encodeCoreBytes(vm, *firstB.get(), first.get()->functionDecl(1)) == encodeCoreBytes(vm, *secondB.get(), second.get()->functionDecl(1));
    if (!u7.check(sameAcrossOrders, "two native decodes of one payload encode differently after their functions ran in different orders"_s))
        return false;

    // A decoded parent whose children still hold their lazily cached slots.
    auto lazy = decode(Ref<CachedBytecode> { *payload });
    auto lazyOnce = lazy ? encodeCoreBytes(vm, *lazy.get(), nullptr) : std::nullopt;
    auto lazyTwice = lazy ? encodeCoreBytes(vm, *lazy.get(), nullptr) : std::nullopt;
    if (!u7.check(lazyOnce && lazyOnce == lazyTwice && lazyOnce == encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr), "a decoded parent with lazily cached children does not encode twice to its generated twin's bytes"_s))
        return false;
    Strong<UnlinkedFunctionCodeBlock> lazyChild { vm, decodedBody(vm, *lazy.get()->functionDecl(1), generated.source) };
    if (!u7.check(!!lazyChild, "encoding a decoded parent spoiled a child's lazily cached slot"_s))
        return false;

    // A persistent payload: the decoded stream borrows the payload's bytes.
    Vector<Vector<uint8_t>>& payloads = persistentPayloads();
    payloads.append(Vector<uint8_t> { payload->span() });
    Ref<CachedBytecode> persistent = CachedBytecode::create(payloads.last().mutableSpan(), [](const void*) { }, { });
    persistent->setPayloadIsPersistent();
    auto borrowed = decode(WTF::move(persistent));
    if (!u7.check(!!borrowed, "the native bytecode cache did not decode a persistent payload"_s))
        return false;
    if (!u7.check(!Options::useBorrowedBytecodeFromCache() || borrowed.get()->instructions().isBorrowed(), "a block decoded from a persistent payload does not borrow its stream"_s))
        return false;
    return u7.check(encodeCoreBytes(vm, *borrowed.get(), nullptr) == encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr), "a borrowed-stream UCB does not encode to its copied twin's bytes"_s);
}

static std::optional<uint32_t> regExpConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<RegExp>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// U7 (E11): a RegExp constant encodes the same whatever its shared cell's compiled state, and a core decodes to the cell the
// VM holds for its pattern and flags.
static bool testCodecRegExps(VM& vm, SelfTestPart& u7)
{
    JSGlobalObject* globalObject = vm.entryScope->globalObject();
    String text = "var jitcacheU7Pattern = /jitcache-u7-(a+)b/gi;\n"_s;
    ProgramFixture generated = generateFixture(vm, text);
    if (!u7.check(!!generated.codeBlock, "the RegExp program failed to generate"_s))
        return false;
    auto index = regExpConstant(*generated.codeBlock.get());
    if (!u7.check(!!index, "the RegExp program holds no RegExp constant"_s))
        return false;
    RegExp* regExp = constantCell<RegExp>(*generated.codeBlock.get(), *index);
    auto encoded = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-aab"_s, 0);
    auto compiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    vm.regExpCache()->deleteAllCode();
    auto cleared = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-ab"_s, 0);
    auto recompiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    if (!u7.check(encoded && encoded == compiled && encoded == cleared && encoded == recompiled, "a RegExp constant's encoding follows its cell's compiled state"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    ProgramFixture regenerated = generateFixture(vm, text);
    if (!u7.check(regenerated.codeBlock && encodeCoreBytes(vm, *regenerated.codeBlock.get(), nullptr) == encoded, "a generation made while the shared cell's code was cleared encodes differently"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, encoded->span(), *generated.source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, true, failure) };
    bool sameCell = decoded && constantCell<RegExp>(*decoded.get(), *index) == regExp && encodeCoreBytes(vm, *decoded.get(), nullptr) == encoded;
    return u7.check(sameCell, "a core's RegExp constant does not decode to the cell the VM holds with its code cleared, or does not re-encode to the core"_s);
}

static String moduleWithBindings(unsigned bindings)
{
    StringBuilder text;
    // The functions come first, so their own positions do not depend on how many bindings follow.
    text.append("export function jitcacheU7ModuleOuter() { return function jitcacheU7ModuleInner() { return binding0000; }; }\n"_s);
    for (unsigned i = 0; i < bindings; ++i) {
        String number = String::number(i);
        text.append("const binding"_s);
        for (unsigned digits = number.length(); digits < 4; ++digits)
            text.append('0');
        text.append(number, " = "_s, i, ";\n"_s);
    }
    return text.toString();
}

struct ModuleChainFixture {
    SourceCode source;
    Strong<UnlinkedModuleProgramCodeBlock> module;
    UnlinkedFunctionExecutable* outer { nullptr };
    Strong<UnlinkedFunctionCodeBlock> outerBody;
    UnlinkedFunctionExecutable* inner { nullptr };
    Strong<UnlinkedFunctionCodeBlock> innerBody;
};

static ModuleChainFixture generateModuleChain(VM& vm, unsigned bindings)
{
    ModuleChainFixture fixture;
    fixture.source = makeSource(moduleWithBindings(bindings), SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    ParserError error;
    fixture.module = Strong<UnlinkedModuleProgramCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, fixture.source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
    if (!fixture.module || !fixture.module.get()->numberOfFunctionDecls())
        return fixture;
    fixture.outer = fixture.module.get()->functionDecl(0);
    SourceCode outerSource = fixture.outer->linkedSourceCode(fixture.source);
    fixture.outerBody = generateBodyOf(vm, *fixture.outer, outerSource, CodeSpecializationKind::CodeForCall);
    if (!fixture.outerBody || !fixture.outerBody.get()->numberOfFunctionExprs())
        return fixture;
    fixture.inner = fixture.outerBody.get()->functionExpr(0);
    fixture.innerBody = generateBodyOf(vm, *fixture.inner, fixture.inner->linkedSourceCode(outerSource), CodeSpecializationKind::CodeForCall);
    return fixture;
}

// The names "binding" followed by four digits that a payload holds, each with its number of occurrences.
static std::optional<unsigned> countEachBindingOnce(std::span<const uint8_t> payload, unsigned bindings)
{
    static constexpr std::array<uint8_t, 7> prefix { 'b', 'i', 'n', 'd', 'i', 'n', 'g' };
    Vector<unsigned> occurrences(FillWith { }, bindings, 0);
    for (size_t i = 0; i + prefix.size() + 4 <= payload.size(); ++i) {
        if (!equalSpans(payload.subspan(i, prefix.size()), std::span<const uint8_t> { prefix }))
            continue;
        unsigned number = 0;
        bool digits = true;
        for (size_t d = 0; d < 4; ++d) {
            uint8_t character = payload[i + prefix.size() + d];
            digits &= isASCIIDigit(character);
            number = number * 10 + (character - '0');
        }
        if (!digits || number >= bindings)
            return std::nullopt;
        ++occurrences[number];
    }
    for (unsigned count : occurrences) {
        if (count != 1)
            return std::nullopt;
    }
    return bindings;
}

// A Function-constructor root, whose UFE has no TDZ chain (CodeCache::getUnlinkedGlobalFunctionExecutable creates it with none).
static Strong<UnlinkedFunctionExecutable> functionConstructorRoot(VM& vm, const String& body, SourceCode& source)
{
    String text = makeString("function anonymous(a\n) {\n"_s, body, "\n}"_s);
    source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    return Strong<UnlinkedFunctionExecutable> { vm, vm.codeCache()->getUnlinkedGlobalFunctionExecutable(vm, Identifier::fromString(vm, "anonymous"_s), source, NoLexicallyScopedFeatures, { }, 22, error) };
}

// The links of the chains of a UCB's children, children in table order and each chain from its child's own link, each link
// numbered where it first appears. Two UCBs share links among their children alike exactly when their numberings are equal.
// `sharedLinks` gains one for each link met again.
static Vector<unsigned> childLinkNumbering(UnlinkedCodeBlock& codeBlock, unsigned& sharedLinks)
{
    Vector<const TDZEnvironmentLink*> seen;
    Vector<unsigned> numbering;
    allChildren(codeBlock, [&](UnlinkedFunctionExecutable& child, ChildTable, uint32_t) {
        for (TDZEnvironmentLink* link = child.parentScopeTDZVariables().get(); link; link = link->parent()) {
            size_t number = seen.find(link);
            if (number == notFound) {
                number = seen.size();
                seen.append(link);
            } else
                ++sharedLinks;
            numbering.append(static_cast<unsigned>(number));
        }
        numbering.append(std::numeric_limits<unsigned>::max()); // ends the child's chain
        return true;
    });
    return numbering;
}

// Walks a generated tree and the native bytecode cache's decode of it in step, one function at a time. Each decoded child
// must hold its generated twin's TDZ chain whole, link by link (the same number of links and the same chain digest), the
// decoded children must share their links where the generated ones do, and each decoded body must encode, with its own UFE
// as the holder, to its generated twin's core (codec section 6). Along the way the generated child takes every JITCacheCore
// operation E14 adds: its holder digest, which keeps its environments' digests, its descriptor, whose chain becomes a start
// record, and its body's core with it as the holder, whose children's chains end in a start record too. `multiLinkChains`
// counts the chains of more than one link the walk compared, and `sharedLinks` the links siblings share.
static bool nativeDecodeKeepsChains(VM& vm, SelfTestPart& u7, UnlinkedCodeBlock& generated, UnlinkedCodeBlock& decoded, const SourceCode& source, unsigned& multiLinkChains, unsigned& sharedLinks)
{
    bool sameTables = generated.numberOfFunctionDecls() == decoded.numberOfFunctionDecls() && generated.numberOfFunctionExprs() == decoded.numberOfFunctionExprs();
    if (!u7.check(sameTables, "a UCB the native bytecode cache decoded has other child tables than its generated twin"_s))
        return false;
    unsigned decodedSharedLinks = 0;
    if (!u7.check(childLinkNumbering(generated, sharedLinks) == childLinkNumbering(decoded, decodedSharedLinks), "the children of a native decode do not share their links where generation shared them"_s))
        return false;
    auto compare = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!u7.check(child && decodedChild, "a child UFE is missing from a native decode or from its generated twin"_s))
            return false;
        unsigned digested = 0;
        TDZEnvironmentLink* chain = child->parentScopeTDZVariables().get();
        TDZEnvironmentLink* decodedChain = decodedChild->parentScopeTDZVariables().get();
        auto chainDigest = tdzChainDigest(chain, nullptr, digested);
        unsigned length = chainShape(chain, nullptr).length;
        bool sameChain = chainDigest && tdzChainDigest(decodedChain, nullptr, digested) == chainDigest && chainShape(decodedChain, nullptr).length == length;
        if (!u7.check(sameChain, "a child the native bytecode cache decoded does not hold its generated twin's TDZ chain, link by link"_s))
            return false;
        if (length > 1)
            ++multiLinkChains;
        if (!u7.check(holderDigest(vm, *child) && encodeDescriptorBytes(vm, *child), "a generated child's holder digest or descriptor failed"_s))
            return false;
        Strong<UnlinkedFunctionCodeBlock> body { vm, decodedBody(vm, *child, source) };
        Strong<UnlinkedFunctionCodeBlock> decodedChildBody { vm, decodedBody(vm, *decodedChild, source) };
        if (!u7.check(body && decodedChildBody, "a body of the native round trip is missing"_s))
            return false;
        auto core = encodeCoreBytes(vm, *body.get(), child);
        if (!u7.check(core && core == encodeCoreBytes(vm, *decodedChildBody.get(), decodedChild), "a function the native bytecode cache decoded does not encode, with its holder, to its generated twin's core"_s))
            return false;
        return nativeDecodeKeepsChains(vm, u7, *body.get(), *decodedChildBody.get(), child->linkedSourceCode(source), multiLinkChains, sharedLinks);
    };
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i) {
        if (!compare(generated.functionDecl(i), decoded.functionDecl(i)))
            return false;
    }
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i) {
        if (!compare(generated.functionExpr(i), decoded.functionExpr(i)))
            return false;
    }
    return true;
}

// U7 (E14): function cores and descriptors stay the size they are whatever the enclosing module declares, and the module's own
// core holds each binding once; a chain that does not end in the holder's is written whole; decoding with the holder ends
// the children's chains in its chain object; a start record decoded without a start chain is Malformed; and the native
// bytecode cache writes and reads chains as it did before E14.
static bool testCodecTDZChains(VM& vm, SelfTestPart& u7)
{
    ModuleChainFixture large = generateModuleChain(vm, 1000);
    ModuleChainFixture small = generateModuleChain(vm, 10);
    if (!u7.check(large.innerBody && small.innerBody, "a module of the chain test failed to generate"_s))
        return false;
    auto size = [&](const std::optional<Vector<uint8_t>>& bytes) {
        return bytes ? bytes->size() : 0;
    };
    auto largeOuterCore = encodeCoreBytes(vm, *large.outerBody.get(), large.outer);
    auto largeInnerCore = encodeCoreBytes(vm, *large.innerBody.get(), large.inner);
    bool sameSizes = largeOuterCore && size(largeOuterCore) == size(encodeCoreBytes(vm, *small.outerBody.get(), small.outer))
        && size(largeInnerCore) && size(largeInnerCore) == size(encodeCoreBytes(vm, *small.innerBody.get(), small.inner))
        && size(encodeDescriptorBytes(vm, *large.outer)) && size(encodeDescriptorBytes(vm, *large.outer)) == size(encodeDescriptorBytes(vm, *small.outer))
        && size(encodeDescriptorBytes(vm, *large.inner)) && size(encodeDescriptorBytes(vm, *large.inner)) == size(encodeDescriptorBytes(vm, *small.inner));
    if (!u7.check(sameSizes, "a function core or descriptor grows with the bindings its module declares"_s))
        return false;
    auto moduleCore = encodeCoreBytes(vm, *large.module.get(), nullptr);
    if (!u7.check(moduleCore && countEachBindingOnce(moduleCore->span(), 1000), "the module's own core does not hold each of its bindings exactly once"_s))
        return false;

    // Encoded with a holder whose chain is another, the children's chains are written link by link, the module's 1,000 names
    // included.
    auto foreignHolder = encodeCoreBytes(vm, *large.outerBody.get(), small.outer);
    if (!u7.check(foreignHolder && size(foreignHolder) >= size(largeOuterCore) + 1000 * 11, "a chain that does not end in the holder's chain is not written whole"_s))
        return false;

    // Decoded with the holder, the children's chains end in its chain object and equal the generated children's.
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, largeOuterCore->span(), *large.source.provider(), UnlinkedCodeBlockCoreKind::Function, large.outer, true, failure) };
    if (!u7.check(decoded && decoded.get()->numberOfFunctionExprs() == 1, "a function core of the chain test does not decode with its holder"_s))
        return false;
    UnlinkedFunctionExecutable& decodedInner = *decoded.get()->functionExpr(0);
    TDZEnvironmentLink* holderChain = large.outer->parentScopeTDZVariables().get();
    ChainShape decodedShape = chainShape(decodedInner.parentScopeTDZVariables().get(), holderChain);
    ChainShape generatedShape = chainShape(large.inner->parentScopeTDZVariables().get(), holderChain);
    unsigned digested = 0;
    auto decodedChain = tdzChainDigest(decodedInner.parentScopeTDZVariables().get(), nullptr, digested);
    auto generatedChain = tdzChainDigest(large.inner->parentScopeTDZVariables().get(), nullptr, digested);
    bool chainsMatch = holderChain && decodedShape.targetPosition && decodedShape == generatedShape && decodedChain && decodedChain == generatedChain;
    if (!u7.check(chainsMatch, "a decoded child's chain does not end in the holder's chain object or differs from the generated child's"_s))
        return false;
    if (!u7.check(encodeCoreBytes(vm, *decoded.get(), large.outer) == largeOuterCore, "decoding and re-encoding a function core with its holder does not give the core back"_s))
        return false;

    // The core's start record, decoded by a holder without a chain.
    SourceCode rootSource;
    Strong<UnlinkedFunctionExecutable> chainless = functionConstructorRoot(vm, "return 'jitcache-u7-chainless';"_s, rootSource);
    if (!u7.check(chainless && !chainless.get()->parentScopeTDZVariables(), "the chainless holder has a TDZ chain"_s))
        return false;
    Strong<UnlinkedCodeBlock> withoutStartChain { vm, decodeCoreBytes(vm, largeOuterCore->span(), *large.source.provider(), UnlinkedCodeBlockCoreKind::Function, chainless.get(), false, failure) };
    if (!u7.check(!withoutStartChain && failure == CoreDecodeFailure::Malformed, "a start record decoded without a start chain is not Malformed"_s))
        return false;

    // The native bytecode cache has no start chain, so it never writes a start record and its decode is unchanged (codec
    // E14): it must write and read chains as it did before E14. No encoder from before E14 exists in the process to compare
    // bytes with, so the test checks what E14 could have changed. A tree whose sibling functions share chains of two links,
    // and whose arrows' chains end in their functions' own chain objects, goes through the native cache and back with every
    // chain whole and every link shared where generation shared it; and the native bytes for the tree stay the same after
    // the walk has run E14's JITCacheCore operations on it, which turn those chains into start records and keep each
    // environment's digest.
    ProgramFixture program = generateProgramFixture(vm, String { codecCorpus[nestedScopesCorpusIndex] }, std::numeric_limits<unsigned>::max());
    if (!u7.check(!!program.codeBlock, "the native round-trip program failed to generate"_s))
        return false;
    SourceCodeKey key = programCacheKey(program.source);
    RefPtr<CachedBytecode> native = encodeCodeBlock(vm, key, program.codeBlock.get());
    if (!u7.check(!!native, "the native bytecode cache did not encode the round-trip program"_s))
        return false;
    Strong<UnlinkedProgramCodeBlock> nativeDecode { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, Ref<CachedBytecode> { *native }) };
    if (!u7.check(!!nativeDecode, "the native bytecode cache did not decode the round-trip program"_s))
        return false;
    unsigned multiLinkChains = 0;
    unsigned sharedLinks = 0;
    if (!nativeDecodeKeepsChains(vm, u7, *program.codeBlock.get(), *nativeDecode.get(), program.source, multiLinkChains, sharedLinks))
        return false;
    if (!u7.check(multiLinkChains >= 2 && sharedLinks, "the native round trip compared fewer than two chains of several links, or no link that siblings share, so it cannot show the cache keeps them"_s))
        return false;
    RefPtr<CachedBytecode> nativeAgain = encodeCodeBlock(vm, key, program.codeBlock.get());
    return u7.check(nativeAgain && equalSpans(native->span(), nativeAgain->span()), "the native bytecode cache writes other bytes for a tree after E14's JITCacheCore operations on it"_s);
}

// U7: a UFE's descriptor is the same before and after its body is parsed and its slots filled, differs with each field
// section 3.4 of the main file puts in it, its flags one by one included, and is the same for UFEs that differ only in
// their TDZ chains (the variants U3 shares).
static bool testCodecDescriptors(VM& vm, JSGlobalObject* globalObject, SelfTestPart& u7)
{
    ProgramFixture fixture = generateFixture(vm, "function jitcacheU7Descriptor(a, b) { return a + b; }"_s);
    if (!u7.check(!!fixture.codeBlock, "the descriptor program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& executable = *fixture.codeBlock.get()->functionDecl(0);
    auto before = encodeDescriptorBytes(vm, executable);
    executable.recordParse(ArgumentsFeature, executable.lexicallyScopedFeatures(), true);
    ParserError error;
    UnlinkedFunctionCodeBlock* body = executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(fixture.source), CodeSpecializationKind::CodeForCall, { }, error, executable.parseMode());
    auto after = encodeDescriptorBytes(vm, executable);
    if (!u7.check(body && before && before == after, "a descriptor changes with its UFE's own parse or the filling of its slot"_s))
        return false;
    for (const HolderVariant& variant : holderVariants) {
        HolderFixture base;
        HolderFixture other;
        if (!generateHolderPair(vm, globalObject, variant, u7, base, other))
            return false;
        auto baseDescriptor = encodeDescriptorBytes(vm, *base.executable);
        auto otherDescriptor = encodeDescriptorBytes(vm, *other.executable);
        if (!u7.check(baseDescriptor && otherDescriptor && ((*baseDescriptor != *otherDescriptor) == variant.descriptorDiffers), makeString("the descriptor does not follow "_s, variant.field)))
            return false;
    }
    return true;
}

static bool failsToDecodeWith(VM& vm, std::span<const uint8_t> bytes, SourceProvider& provider, UnlinkedCodeBlockCoreKind kind, bool validate, CoreDecodeFailure expected)
{
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* codeBlock = decodeCoreBytes(vm, bytes, provider, kind, nullptr, validate, failure);
    return !codeBlock && failure == expected;
}

// U7: each Malformed and KindMismatch condition of codec section 3, all of which the payload's header holds. The cases that
// alter a core's records are runCoreCodecSelfTest's, in runtime/CachedTypes.cpp beside the record types (codec section 7).
static bool testCodecHeader(VM& vm, SelfTestPart& u7)
{
    ProgramFixture fixture = generateFixture(vm, "var jitcacheU7Header = 'jitcache-u7-header';\n"_s);
    if (!u7.check(!!fixture.codeBlock, "the malformed-payload program failed to generate"_s))
        return false;
    auto core = encodeCoreBytes(vm, *fixture.codeBlock.get(), nullptr);
    if (!u7.check(core && core->size() > 16, "the malformed-payload program does not encode"_s))
        return false;
    SourceProvider& provider = *fixture.source.provider();
    const Vector<uint8_t>& valid = *core;
    auto altered = [&](const auto& mutate) {
        Vector<uint8_t> bytes = valid;
        mutate(bytes);
        return bytes;
    };

    // Section 3.
    uint32_t rootOffset = loadField<uint32_t>(valid.span(), 8);
    Vector<uint8_t> misaligned(FillWith { }, valid.size() + 8, 0);
    memcpySpan(misaligned.mutableSpan().subspan(1, valid.size()), valid.span());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    bool misalignedFails = !decodeUnlinkedCodeBlockCore(vm, CachedBytecode::create(misaligned.mutableSpan().subspan(1, valid.size()), [](const void*) { }, { }), provider, UnlinkedCodeBlockCoreKind::Program, nullptr, false, failure)
        && failure == CoreDecodeFailure::Malformed;
    bool sectionThree = failsToDecodeWith(vm, valid.span().first(15), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && misalignedFails
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[0] ^= 0xff; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[5] = 1; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[12] = 1; }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { storeField<uint32_t>(bytes, 8, rootOffset + 1); }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { storeField<uint32_t>(bytes, 8, static_cast<uint32_t>(bytes.size())); }).span(), provider, UnlinkedCodeBlockCoreKind::Program, false, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, valid.span(), provider, UnlinkedCodeBlockCoreKind::Module, false, CoreDecodeFailure::KindMismatch);
    return u7.check(sectionThree, "a payload breaking a condition of codec section 3 does not decode to Malformed or KindMismatch"_s);
}

// U7 (E16): Map.prototype.forEach holds the VM's ordered-hash-table sentinel among its constants (F27). Its core writes the
// sentinel's own kind and decodes to this VM's sentinel, re-encoding to the core's bytes, while the native builtin cache
// writes the cell as an immutable butterfly and decodes a new butterfly in its place.
static bool testCodecSentinel(VM& vm, SelfTestPart& u7)
{
    // A builtin root of its own over forEach's text alone, as Bun creates its internal modules: the native builtin cache
    // compares its payload with the provider's whole length (decodeBuiltinFunction).
    String text = vm.builtinExecutables()->mapPrototypeForEachCodeSource().view().toString();
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> executable { vm, BuiltinExecutables::createExecutable(vm, source, Identifier::fromString(vm, "forEach"_s), ImplementationVisibility::Public,
        ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No) };
    if (!u7.check(!!executable, "Map.prototype.forEach's text does not make a builtin root"_s))
        return false;
    ParserError error;
    Strong<UnlinkedFunctionCodeBlock> body { vm, executable.get()->unlinkedCodeBlockFor(vm, executable.get()->linkedSourceCode(source), CodeSpecializationKind::CodeForCall, { }, error, executable.get()->parseMode()) };
    JSCell* sentinel = vm.orderedHashTableSentinel();
    std::optional<uint32_t> index;
    for (uint32_t i = 0; body && !index && i < body.get()->constantRegisters().size(); ++i) {
        if (body.get()->constantRegisters()[i].get() == JSValue(sentinel))
            index = i;
    }
    if (!u7.check(!!index, "Map.prototype.forEach's body holds no ordered-hash-table sentinel"_s))
        return false;

    auto core = encodeCoreBytes(vm, *body.get(), executable.get());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), *source.provider(), UnlinkedCodeBlockCoreKind::Function, executable.get(), true, failure) : nullptr };
    bool keepsSentinel = decoded && decoded.get()->constantRegisters()[*index].get() == JSValue(sentinel) && encodeCoreBytes(vm, *decoded.get(), executable.get()) == core;
    if (!u7.check(keepsSentinel, "Map.prototype.forEach's core does not decode to this VM's sentinel, or does not re-encode to its bytes"_s))
        return false;

    // The native builtin cache: the encode holds the body the call slot keeps, and the decoded root decodes it at its first
    // request.
    RefPtr<CachedBytecode> native = encodeBuiltinFunction(vm, executable.get(), text.length(), 0);
    Strong<UnlinkedFunctionExecutable> nativeExecutable { vm, native ? decodeBuiltinFunction(vm, Ref<CachedBytecode> { *native }, *source.provider(), 0) : nullptr };
    Strong<UnlinkedFunctionCodeBlock> nativeBody { vm, nativeExecutable ? decodedBody(vm, *nativeExecutable.get(), source) : nullptr };
    JSValue nativeConstant = nativeBody && *index < nativeBody.get()->constantRegisters().size() ? nativeBody.get()->constantRegisters()[*index].get() : JSValue();
    auto* nativeButterfly = nativeConstant ? dynamicDowncast<JSCellButterfly>(nativeConstant) : nullptr;
    return u7.check(nativeButterfly && nativeButterfly != sentinel && !nativeButterfly->length(), "the native builtin cache does not write the sentinel as an immutable butterfly"_s);
}

// U7: a default class constructor, which only BuiltinExecutables::createDefaultConstructor makes, keeps
// isBuiltinDefaultClassConstructor and its constructor kind through a core, as generation left them. No pair of U3's can
// vary that flag alone, so the round trip checks it.
static bool testCodecDefaultConstructors(VM& vm, SelfTestPart& u7)
{
    ProgramFixture fixture = generateFixture(vm, "class JitcacheU7DefaultBase { }\nclass JitcacheU7DefaultDerived extends JitcacheU7DefaultBase { }\n"_s);
    if (!u7.check(!!fixture.codeBlock, "the default-constructor program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& generated = *fixture.codeBlock.get();
    auto core = encodeCoreBytes(vm, generated, nullptr);
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), *fixture.source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, false, failure) : nullptr };
    bool sameTables = decoded && decoded.get()->numberOfFunctionDecls() == generated.numberOfFunctionDecls() && decoded.get()->numberOfFunctionExprs() == generated.numberOfFunctionExprs();
    if (!u7.check(sameTables, "the default-constructor program's core does not decode to its child tables"_s))
        return false;
    unsigned defaultConstructors = 0;
    auto sameKind = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        defaultConstructors += child->isBuiltinDefaultClassConstructor();
        return child->isBuiltinDefaultClassConstructor() == decodedChild->isBuiltinDefaultClassConstructor() && child->constructorKind() == decodedChild->constructorKind();
    };
    bool kept = true;
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i)
        kept &= sameKind(generated.functionDecl(i), decoded.get()->functionDecl(i));
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i)
        kept &= sameKind(generated.functionExpr(i), decoded.get()->functionExpr(i));
    if (!u7.check(defaultConstructors == 2, "the default-constructor program does not hold a base and a derived default constructor"_s))
        return false;
    return u7.check(kept, "a default constructor decoded from a core lost isBuiltinDefaultClassConstructor or its constructor kind"_s);
}

// U7 (section 13.1, SPEC-ucb.codec.md section 7).
static bool testCodec(VM& vm, String& failure)
{
    SelfTestPart u7 { "U7"_s, failure };
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    if (!u7.check(!!globalObject, "the self-test runs outside a VM entry"_s))
        return false;
    return testInlineMapTransport(vm, u7) && testCodecCorpus(vm, globalObject, u7) && testCodecChunksAndBudgets(vm, u7) && testCodecNativeDecodes(vm, u7)
        && testCodecRegExps(vm, u7) && testCodecSentinel(vm, u7) && testCodecTDZChains(vm, u7) && testCodecDescriptors(vm, globalObject, u7)
        && testCodecDefaultConstructors(vm, u7) && testCodecHeader(vm, u7) && runCoreCodecSelfTest(vm, failure);
}

// U8: the engine (sections 7.3 and 7.7) with the integrator's body lookup stubbed (SPEC-integrator.md section 6.2). The
// parts act on the VM's own registry and statistics, since the engine does; their UCBs are unpublished or test-held, so
// no CodeBlock installs what they attach.

// The stubbed index and store: each listed key has the index token bodyVersion answers and the body openBody returns, or
// no body for one the index lists but the store no longer holds.
class TestArtifact {
    WTF_MAKE_NONCOPYABLE(TestArtifact);

public:
    TestArtifact() = default;

    void list(const BodyKey& key, uint64_t token, RefPtr<ValidatedBody>&& body)
    {
        if (Entry* entry = find(key)) {
            entry->token = token;
            entry->body = WTF::move(body);
            return;
        }
        m_entries.append(Entry { key, token, WTF::move(body), 0 });
    }

    void unlist(const BodyKey& key)
    {
        m_entries.removeFirstMatching([&](const Entry& entry) {
            return entry.key == key;
        });
    }

    uint64_t token(const BodyKey& key) const
    {
        const Entry* entry = find(key);
        return entry ? entry->token : 0;
    }

    BodyLookup open(const BodyKey& key)
    {
        Entry* entry = find(key);
        if (!entry)
            return BodyLookup::missing();
        ++entry->opens;
        if (!entry->body)
            return BodyLookup::missing();
        return BodyLookup::found(Ref<ValidatedBody> { *entry->body });
    }

    unsigned opens(const BodyKey& key) const
    {
        const Entry* entry = find(key);
        return entry ? entry->opens : 0;
    }

private:
    struct Entry {
        BodyKey key;
        uint64_t token;
        RefPtr<ValidatedBody> body;
        unsigned opens;
    };

    const Entry* find(const BodyKey& key) const
    {
        for (const Entry& entry : m_entries) {
            if (entry.key == key)
                return &entry;
        }
        return nullptr;
    }

    Entry* find(const BodyKey& key) { return const_cast<Entry*>(std::as_const(*this).find(key)); }

    Vector<Entry> m_entries;
};

// Serves a TestArtifact through the VMState's lookup override while it lives.
class StubbedBodyLookup {
    WTF_MAKE_NONCOPYABLE(StubbedBodyLookup);

public:
    StubbedBodyLookup(VMState& state, TestArtifact& artifact)
        : m_state(state)
    {
        m_state.setBodyLookupForTesting([&artifact](const BodyKey& key) {
            return artifact.token(key);
        }, [&artifact](const BodyKey& key) {
            return artifact.open(key);
        });
    }

    ~StubbedBodyLookup() { m_state.clearBodyLookupForTesting(); }

private:
    VMState& m_state;
};

// The three sections a capture of a UCB writes (sections 4.2 and 5.2).
struct TestBody {
    Vector<uint8_t> identity;
    Vector<uint8_t> core;
    Vector<uint8_t> feedback;

    Ref<ValidatedBody> validated(const BodyKey& key, uint64_t version) const
    {
        std::array<ValidatedBody::TestSection, 3> sections { {
            { SectionKind::UCBIdentity, identity.span() },
            { SectionKind::UCBCore, core.span() },
            { SectionKind::UCBFeedback, feedback.span() },
        } };
        return ValidatedBody::createForTesting(key, version, sections);
    }
};

// The sections a capture of `producer` would write under the given key, context and provenance, with a function body's
// parse fields and holder, as buildSections writes them (section 8.2).
static std::optional<TestBody> buildTestBody(VM& vm, UnlinkedCodeBlock& producer, const UnlinkedFunctionExecutable* holder, const BodyKey& key, const Digest256& context, CoreProvenance provenance,
    const std::optional<FunctionParseFields>& parseFields = std::nullopt)
{
    auto core = encodeCoreBytes(vm, producer, holder);
    if (!core)
        return std::nullopt;
    TestBody body;
    body.core = WTF::move(*core);
    FeedbackCounts counts = liveFeedbackCounts(producer);
    body.feedback = Vector<uint8_t>(feedbackSectionSize(counts));
    writeFeedbackSection(body.feedback.mutableSpan(), producer, counts);
    std::optional<Digest256> holderDigestValue = holder ? holderDigest(vm, *holder) : std::nullopt;
    auto constantCount = static_cast<uint32_t>(producer.constantRegisters().size());
    body.identity = Vector<uint8_t>(identitySectionSize(constantCount));
    writeIdentitySection(body.identity.mutableSpan(), vm, key, context, provenance, SHA256::hash(body.core.span()), holderDigestValue, coreKindFor(key.identityKind()), parseFields, producer);
    return body;
}

static FunctionParseFields parseFieldsOf(const TwinParseResults& results)
{
    return FunctionParseFields { results.features, results.lexicallyScopedFeatures, results.hasCapturedVariables };
}

static std::span<const uint8_t> butterflyMapOf(const TestBody& body)
{
    auto identity = parseIdentitySection(body.identity.span(), false);
    return identity ? identity->butterflyMap : std::span<const uint8_t> { };
}

struct ProgramRequestFixture {
    SourceCode source;
    Strong<ProgramExecutable> executable;
};

static ProgramRequestFixture programRequestFixture(JSGlobalObject* globalObject, Ref<SourceProvider>&& provider)
{
    VM& vm = globalObject->vm();
    SourceCode source { WTF::move(provider) };
    Strong<ProgramExecutable> executable { vm, ProgramExecutable::create(globalObject, source) };
    return ProgramRequestFixture { WTF::move(source), WTF::move(executable) };
}

static Ref<SourceProvider> plainProvider(const String& text)
{
    return StringSourceProvider::create(text, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
}

// The key and context a program request computes (section 3.7), from the inputs alone.
static BodyKey programKey(const SourceCode& source, LexicallyScopedFeatures snapshot)
{
    return BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, rootIdentityDigest(IdentityKind::Program, rootSourceDigest(source).digest, snapshot, std::nullopt));
}

static Digest256 programContext(const SourceCode& source, ProgramExecutable& executable)
{
    return globalContextDigest(IdentityKind::Program, source.startOffset(), source.firstLine().oneBasedInt(), JSParserScriptMode::Classic, executable.derivedContextType(), EvalContextType::None, executable.isArrowFunctionContext());
}

static Strong<UnlinkedCodeBlock> generateProgramFor(VM& vm, ProgramRequestFixture& fixture)
{
    RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
    prepareProgramRequest(request, *fixture.executable.get());
    TwinParseResults results;
    ParserError error;
    return Strong<UnlinkedCodeBlock> { vm, generateGlobalTwin(request, results, error) };
}

// A program whose UCB the registry records under a test key, so its children have identities (section 6.1).
struct RecordedParent {
    ProgramFixture program;
    BodyKey key;
};

static std::optional<RecordedParent> recordedParent(VM& vm, UCBRegistry& registry, const String& text, uint8_t keyFill)
{
    ProgramFixture program = generateFixture(vm, text);
    if (!program.codeBlock)
        return std::nullopt;
    BodyKey key = testKey(keyFill);
    if (!registry.recordCodeBlock(*program.codeBlock.get(), testRecord(key, UCBOrigin::Generated)))
        return std::nullopt;
    return RecordedParent { WTF::move(program), key };
}

// What a child body's request computes (section 3.7), from the inputs alone.
struct ChildInputs {
    SourceCode source;
    BodyKey key;
    Digest256 context;
};

static ChildInputs childInputs(const UnlinkedFunctionExecutable& child, const SourceCode& parentSource, const BodyKey& parentKey, ChildTable table, uint32_t index,
    CodeSpecializationKind kind = CodeSpecializationKind::CodeForCall)
{
    SourceCode source = child.linkedSourceCode(parentSource);
    BodyKey key = BodyKey::make(IdentityKind::Child, kind, { }, childIdentityDigest(parentKey, table, index));
    Digest256 context = executableBodyContextDigest(IdentityKind::Child, source.startOffset(), source.firstLine().oneBasedInt(), std::nullopt);
    return ChildInputs { WTF::move(source), key, context };
}

static void prepareBodyRequest(RequestState& request, UnlinkedFunctionExecutable& executable, CodeSpecializationKind kind = CodeSpecializationKind::CodeForCall)
{
    request.functionExecutable = &executable;
    request.specialization = kind;
}

static uint64_t missesOf(const UCBStatistics& statistics, MissReason reason)
{
    return statistics.misses[static_cast<size_t>(reason)];
}

static bool computedNoDigest(const UCBStatistics& before, const UCBStatistics& after)
{
    return before.sourceDigests == after.sourceDigests && before.suppliedSourceDigests == after.suppliedSourceDigests && before.contextDigests == after.contextDigests
        && before.holderDigests == after.holderDigests;
}

// A copy of an identity section under another key, structurally valid, which the engine refuses wherever it parses one.
static Vector<uint8_t> identityUnderAnotherKey(const Vector<uint8_t>& identity)
{
    Vector<uint8_t> bytes = identity;
    bytes[identityKeyOffset + BodyKey::byteSize - 1] ^= 0xff;
    return bytes;
}

static Vector<uint8_t> brokenMagic(const Vector<uint8_t>& section)
{
    Vector<uint8_t> bytes = section;
    bytes[0] ^= 0xff;
    return bytes;
}

// U8: attachLive on a generated program (section 7.3.4). With no body, with code parked in its sharing slot, or with a miss
// stamped at the current token it computes no digest; a request whose snapshot differs from the record's key features misses
// RequestKey unstamped; a miss is stamped with the index token, never the file's commit identifier, and a match records the
// commit identifier, never the token; at a matched commit identifier it attaches without parsing a section; an index that
// lists a body whose file is gone costs one open, stamped, and nothing at the next request.
static bool testAttachToGeneratedProgram(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    String text = "var jitcacheU8Attach = [\"jitcache-u8-attach-alpha\", \"jitcache-u8-attach-beta\"]; function jitcacheU8AttachChild() { return 1; }\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider(text));
    ProgramExecutable& executable = *fixture.executable.get();
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Digest256 context = programContext(fixture.source, executable);

    // The live UCB, generated at its request and recorded there, as the CodeCache's generation records it.
    Strong<UnlinkedCodeBlock> live = generateProgramFor(vm, fixture);
    if (!u8.check(!!live, "the attach program failed to generate"_s))
        return false;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        recordGenerated(request, *live.get());
    }
    auto record = [&] {
        return registry.recordOf(*live.get());
    };
    if (!u8.check(record() && record()->key == key && record()->origin == UCBOrigin::Generated, "recordGenerated did not record the program under its request's key"_s))
        return false;
    auto body = buildTestBody(vm, *live.get(), nullptr, key, context, CoreProvenance::Generated);
    auto wrongContext = buildTestBody(vm, *live.get(), nullptr, key, filledDigest(0x99), CoreProvenance::Generated);
    if (!u8.check(body && wrongContext, "the attach program's body could not be built"_s))
        return false;
    auto attach = [&](UnlinkedCodeBlock& codeBlock) {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        attachLive(request, codeBlock);
    };

    // No body.
    UCBStatistics before = statistics;
    attach(*live.get());
    if (!u8.check(missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics) && !record()->missedBodyVersion, "an attach for a key with no body computed a digest, stamped or missed otherwise than NoBody"_s))
        return false;

#if ENABLE(JIT)
    // Code parked in the sharing slot wins, so the request reads nothing.
    artifact.list(key, 5, body->validated(key, 1000));
    live.get()->m_unlinkedBaselineCode = adoptRef(*new BaselineJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_function_for_call_prologue), LLInt::getCodePtr<JSEntryPtrTag>(llint_function_for_call_arity_check)));
    before = statistics;
    attach(*live.get());
    live.get()->m_unlinkedBaselineCode = nullptr;
    bool parkedReadNothing = computedNoDigest(before, statistics) && statistics.misses == before.misses && statistics.attaches == before.attaches && !artifact.opens(key) && !record()->hasPendingImport;
    if (!u8.check(parkedReadNothing, "an attach to a UCB with parked code read the artifact or computed a digest"_s))
        return false;
#endif

    // A request whose snapshot differs from the record's key features (THREAD Identity, F4).
    Strong<UnlinkedCodeBlock> other = generateProgramFor(vm, fixture);
    BodyKey otherKey = testKey(0x81);
    UCBRecord otherRecord = testRecord(otherKey, UCBOrigin::Generated);
    otherRecord.keyFeatures = TaintedByWithScopeLexicallyScopedFeature;
    if (!u8.check(other && registry.recordCodeBlock(*other.get(), WTF::move(otherRecord)), "the RequestKey fixture could not be recorded"_s))
        return false;
    artifact.list(otherKey, 3, body->validated(otherKey, 1001));
    before = statistics;
    attach(*other.get());
    auto otherView = registry.recordOf(*other.get());
    bool requestKeyMiss = missesOf(statistics, MissReason::RequestKey) == missesOf(before, MissReason::RequestKey) + 1 && otherView && !otherView->missedBodyVersion && !artifact.opens(otherKey);
    if (!u8.check(requestKeyMiss, "an attach whose snapshot differs from the record's key features did not miss RequestKey, or stamped or opened"_s))
        return false;

    // A miss is stamped with the index token, here behind the file's commit identifier.
    artifact.list(key, 5, wrongContext->validated(key, 1000));
    before = statistics;
    attach(*live.get());
    bool stampedWithToken = missesOf(statistics, MissReason::Context) == missesOf(before, MissReason::Context) + 1 && statistics.contextDigests == before.contextDigests + 1
        && record()->missedBodyVersion == 5 && !record()->matchedBodyVersion && artifact.opens(key) == 1;
    if (!u8.check(stampedWithToken, "a Context miss was not stamped with the index token"_s))
        return false;
    before = statistics;
    attach(*live.get());
    bool skipped = missesOf(statistics, MissReason::BodyUnchanged) == missesOf(before, MissReason::BodyUnchanged) + 1 && computedNoDigest(before, statistics) && artifact.opens(key) == 1;
    if (!u8.check(skipped, "a request at the stamped index token read the artifact or computed a digest"_s))
        return false;

    // Another body at the key: the match records the commit identifier.
    artifact.list(key, 6, body->validated(key, 1000));
    before = statistics;
    attach(*live.get());
    if (!u8.check(statistics.attaches == before.attaches + 1 && record()->hasPendingImport && record()->matchedBodyVersion == 1000, "an attach to a matching generated program did not attach at the file's commit identifier"_s))
        return false;

    // At the matched commit identifier the attach parses nothing: a parse of this identity section would find another key,
    // and of this feedback section a wrong magic, both invalid material.
    RefPtr<PendingImport> attached = registry.pendingImport(*live.get());
    registry.resolvePendingImport(*live.get(), *attached, ImportResolution::Installed);
    TestBody unparsable { identityUnderAnotherKey(body->identity), body->core, brokenMagic(body->feedback) };
    artifact.list(key, 7, unparsable.validated(key, 1000));
    before = statistics;
    attach(*live.get());
    bool withoutParsing = state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && statistics.contextDigests == before.contextDigests;
    if (!u8.check(withoutParsing, "an attach at a matched commit identifier parsed a section or computed a context"_s))
        return false;

    // The index lists a body whose file is gone.
    attached = registry.pendingImport(*live.get());
    registry.resolvePendingImport(*live.get(), *attached, ImportResolution::Installed);
    artifact.list(key, 9, nullptr);
    unsigned opensBefore = artifact.opens(key);
    before = statistics;
    attach(*live.get());
    bool goneOnce = missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && record()->missedBodyVersion == 9 && artifact.opens(key) == opensBefore + 1;
    attach(*live.get());
    bool thenNothing = missesOf(statistics, MissReason::BodyUnchanged) == missesOf(before, MissReason::BodyUnchanged) + 1 && artifact.opens(key) == opensBefore + 1;
    return u8.check(goneOnce && thenNothing, "a listed body whose file is gone did not cost one stamped open and then nothing"_s);
}

// U8: a decoded UCB that misses AtomMap is not stamped, computes no digest at its next request, and attaches once its
// marked constants are atoms, without parsing more than the identity section; in a VM whose production is active the attach
// keeps the Generated body's butterfly map (sections 7.3.2, 7.3.4 and 8.2).
static bool testAtomMapAttach(VM& vm, VMState& state, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    auto parent = recordedParent(vm, registry, "function jitcacheU8AtomMap() { return 'jitcache-u8-atom-map-constant-0123456789abcdef'; }\n"_s, 0x82);
    if (!u8.check(!!parent, "the AtomMap parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    ChildInputs inputs = childInputs(child, parent->program.source, parent->key, ChildTable::Declarations, 0);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
    Strong<UnlinkedFunctionCodeBlock> decoded = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
    if (!u8.check(producer && decoded, "the AtomMap function failed to generate"_s))
        return false;

    // The long constant as a native decode leaves it: a plain string with the same characters (F19).
    auto index = stringConstantIndex(*decoded.get(), "jitcache-u8-atom-map-constant-0123456789abcdef"_s);
    JSString* plain = jsString(vm, makeString("jitcache-u8-atom-map-constant-"_s, "0123456789abcdef"_s));
    if (!u8.check(index && !plain->tryGetValueImpl()->isAtom(), "the AtomMap function lacks its constant, or the runtime string is an atom already"_s))
        return false;
    decoded.get()->constantRegister(constantRegisterFor(*index)).set(vm, decoded.get(), plain);
    BodyContextInputs contextInputs { static_cast<unsigned>(inputs.source.startOffset()), static_cast<unsigned>(inputs.source.firstLine().oneBasedInt()) };
    UCBRecord decodedRecord { inputs.key, contextInputs, UCBOrigin::Decoded, NoLexicallyScopedFeatures, std::nullopt, std::nullopt, nullptr, nullptr, std::nullopt };
    if (!u8.check(registry.recordCodeBlock(*decoded.get(), WTF::move(decodedRecord)), "the decoded function could not be recorded"_s))
        return false;
    auto body = buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results));
    if (!u8.check(!!body, "the AtomMap function's body could not be built"_s))
        return false;
    artifact.list(inputs.key, 11, body->validated(inputs.key, 2000));
    auto attach = [&] {
        RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, child);
        attachLive(request, *decoded.get());
    };
    auto record = [&] {
        return registry.recordOf(*decoded.get());
    };

    UCBStatistics before = statistics;
    attach();
    bool firstMiss = missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && record() && !record()->missedBodyVersion && record()->matchedBodyVersion == 2000
        && !record()->hasPendingImport && statistics.holderDigests == before.holderDigests;
    if (!u8.check(firstMiss, "a decoded UCB whose marked constant is not an atom did not miss AtomMap unstamped at the file's commit identifier"_s))
        return false;
    before = statistics;
    attach();
    if (!u8.check(missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && computedNoDigest(before, statistics), "the request after an AtomMap miss computed a digest"_s))
        return false;

    // Once the constant is an atom, the next request attaches; the feedback section, here broken, is never parsed.
    atomizeStringConstant(vm, *plain);
    TestBody identityOnly { body->identity, body->core, brokenMagic(body->feedback) };
    artifact.list(inputs.key, 11, identityOnly.validated(inputs.key, 2000));
    before = statistics;
    attach();
    if (!u8.check(state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && computedNoDigest(before, statistics), "a decoded UCB whose marked constants became atoms did not attach at the matched commit identifier"_s))
        return false;
    std::span<const uint8_t> bodyMap = butterflyMapOf(*body);
    Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
    bool keeps = registry.copyGeneratedButterflyMap(*decoded.get(), kept.mutableSpan());
    return u8.check(keeps == state.productionActive() && (!keeps || equalSpans(kept.span(), bodyMap)), "an attach from a Generated body did not keep its butterfly map exactly when production is active"_s);
}

// U8: a decoded program in the case above computes only its request key at its next requests (section 7.3.4 step 4, I21):
// sourceDigests grows by one per request while contextDigests and holderDigests stay as they are, and no core digest runs,
// which the last attach shows against a body whose identity section names another core digest and whose feedback section
// is broken.
static bool testAtomMapAttachToProgram(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider("var jitcacheU8AtomMapProgram = 'jitcache-u8-atom-map-program-0123456789';\n"_s));
    ProgramExecutable& executable = *fixture.executable.get();
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    Strong<UnlinkedCodeBlock> decoded = generateProgramFor(vm, fixture);
    if (!u8.check(producer && decoded, "the AtomMap program failed to generate"_s))
        return false;

    // The long constant as a native decode leaves it: a plain string with the same characters (F19).
    auto index = stringConstantIndex(*decoded.get(), "jitcache-u8-atom-map-program-0123456789"_s);
    JSString* plain = jsString(vm, makeString("jitcache-u8-atom-map-program-"_s, "0123456789"_s));
    if (!u8.check(index && !plain->tryGetValueImpl()->isAtom(), "the AtomMap program lacks its constant, or the runtime string is an atom already"_s))
        return false;
    decoded.get()->constantRegister(constantRegisterFor(*index)).set(vm, decoded.get(), plain);
    GlobalContextInputs contextInputs { static_cast<unsigned>(fixture.source.startOffset()), static_cast<unsigned>(fixture.source.firstLine().oneBasedInt()), JSParserScriptMode::Classic,
        executable.derivedContextType(), EvalContextType::None, executable.isArrowFunctionContext() };
    UCBRecord decodedRecord { key, contextInputs, UCBOrigin::Decoded, NoLexicallyScopedFeatures, std::nullopt, std::nullopt, nullptr, nullptr, std::nullopt };
    if (!u8.check(registry.recordCodeBlock(*decoded.get(), WTF::move(decodedRecord)), "the decoded program could not be recorded"_s))
        return false;
    auto body = buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, executable), CoreProvenance::Generated);
    if (!u8.check(!!body, "the AtomMap program's body could not be built"_s))
        return false;
    artifact.list(key, 12, body->validated(key, 2100));
    auto attach = [&] {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        attachLive(request, *decoded.get());
    };
    auto record = [&] {
        return registry.recordOf(*decoded.get());
    };
    // What a request at the matched commit identifier computes: its request key, whose root source digest is computed
    // from the text, and nothing else.
    auto computedOnlyItsKey = [&](const UCBStatistics& before) {
        return statistics.sourceDigests == before.sourceDigests + 1 && statistics.suppliedSourceDigests == before.suppliedSourceDigests
            && statistics.contextDigests == before.contextDigests && statistics.holderDigests == before.holderDigests;
    };

    UCBStatistics before = statistics;
    attach();
    bool firstMiss = missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && record() && !record()->missedBodyVersion && record()->matchedBodyVersion == 2100
        && !record()->hasPendingImport;
    if (!u8.check(firstMiss, "a decoded program whose marked constant is not an atom did not miss AtomMap unstamped at the file's commit identifier"_s))
        return false;
    before = statistics;
    attach();
    if (!u8.check(missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && computedOnlyItsKey(before), "the request after a decoded program's AtomMap miss computed more than its request key"_s))
        return false;

    // Once the constant is an atom, the next request attaches without comparing a core digest or parsing the feedback.
    atomizeStringConstant(vm, *plain);
    Vector<uint8_t> otherCoreDigest = body->identity;
    otherCoreDigest[identityCoreDigestOffset] ^= 0xff;
    TestBody unmatchable { WTF::move(otherCoreDigest), body->core, brokenMagic(body->feedback) };
    artifact.list(key, 12, unmatchable.validated(key, 2100));
    before = statistics;
    attach();
    bool attached = state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && computedOnlyItsKey(before);
    return u8.check(attached, "a decoded program whose marked constants became atoms did not attach at the matched commit identifier with its request key alone"_s);
}

// U8: a root body's import computes no context and no holder digest while its key has no body, or a body of provenance
// EmbedderDecoded, and neither does seedDecoded for a key with no body; an import that reaches C11 computes the holder digest
// once (section 7.3.1).
static bool testRootImport(VM& vm, VMState& state, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    SourceCode rootSource;
    Strong<UnlinkedFunctionExecutable> root = functionConstructorRoot(vm, "return a + 'jitcache-u8-root';"_s, rootSource);
    std::optional<ExecutableIdentity> identity = root ? registry.identityOf(*root.get()) : std::nullopt;
    auto* rootIdentity = identity ? std::get_if<RootIdentity>(&*identity) : nullptr;
    if (!u8.check(!!rootIdentity, "the Function-constructor root has no identity"_s))
        return false;
    BodyKey key = BodyKey::make(rootIdentity->kind, CodeSpecializationKind::CodeForCall, { }, rootIdentity->identityDigest);
    SourceCode bodySource = root.get()->linkedSourceCode(rootSource);
    auto rootHolder = holderDigest(vm, *root.get());
    Digest256 context = executableBodyContextDigest(IdentityKind::FunctionConstructor, bodySource.startOffset(), bodySource.firstLine().oneBasedInt(), rootHolder);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, *root.get(), bodySource, CodeSpecializationKind::CodeForCall, &results);
    Strong<UnlinkedFunctionCodeBlock> unpublished = generateBodyOf(vm, *root.get(), bodySource, CodeSpecializationKind::CodeForCall);
    if (!u8.check(producer && unpublished, "the root's body failed to generate"_s))
        return false;
    auto generatedBody = buildTestBody(vm, *producer.get(), root.get(), key, context, CoreProvenance::Generated, parseFieldsOf(results));
    auto embedderBody = buildTestBody(vm, *producer.get(), root.get(), key, context, CoreProvenance::EmbedderDecoded, parseFieldsOf(results));
    if (!u8.check(generatedBody && embedderBody, "the root's bodies could not be built"_s))
        return false;
    auto importRootBody = [&] {
        RequestState request(vm, RequestKind::FunctionBody, bodySource, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, *root.get());
        return Strong<UnlinkedCodeBlock> { vm, importBody(request) };
    };

    UCBStatistics before = statistics;
    bool noBody = !importRootBody() && missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics);
    if (!u8.check(noBody, "a root body's import with no body computed a context or holder digest"_s))
        return false;
    before = statistics;
    {
        RequestState request(vm, RequestKind::FunctionBody, bodySource, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, *root.get());
        seedDecoded(request, *unpublished.get());
    }
    auto seededRecord = registry.recordOf(*unpublished.get());
    bool seedNoBody = missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics)
        && seededRecord && seededRecord->origin == UCBOrigin::Decoded && !seededRecord->hasPendingImport && !seededRecord->missedBodyVersion;
    if (!u8.check(seedNoBody, "seedDecoded for a key with no body computed a digest, stamped, or did not record the UCB as decoded"_s))
        return false;
    artifact.list(key, 21, embedderBody->validated(key, 3000));
    before = statistics;
    bool provenanceMiss = !importRootBody() && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && computedNoDigest(before, statistics);
    if (!u8.check(provenanceMiss, "a root body's import of an EmbedderDecoded body computed a context or holder digest, or missed otherwise than Provenance"_s))
        return false;
    artifact.list(key, 22, generatedBody->validated(key, 3001));
    before = statistics;
    Strong<UnlinkedCodeBlock> imported = importRootBody();
    bool importedOnce = imported && statistics.imports == before.imports + 1 && statistics.holderDigests == before.holderDigests + 1 && statistics.contextDigests == before.contextDigests + 1;
    return u8.check(importedOnce, "a root body's import did not compute its holder digest exactly once"_s);
}

// U8: a body of provenance EmbedderDecoded misses Provenance for a program import, a function import and an attach to a
// generated function UCB, and seeds a decoded function whose core matches; so does a body of provenance Generated, and only a
// seed from that one keeps its butterfly map, in a VM whose production is active (sections 7.3.1 to 7.3.3).
static bool testProvenance(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    // A program import.
    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider("var jitcacheU8Provenance = 1;\n"_s));
    BodyKey programBodyKey = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> programProducer = generateProgramFor(vm, fixture);
    auto programBody = programProducer ? buildTestBody(vm, *programProducer.get(), nullptr, programBodyKey, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::EmbedderDecoded) : std::nullopt;
    if (!u8.check(!!programBody, "the provenance program's body could not be built"_s))
        return false;
    artifact.list(programBodyKey, 31, programBody->validated(programBodyKey, 4000));
    UCBStatistics before = statistics;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        Strong<UnlinkedCodeBlock> imported { vm, importBody(request) };
        if (!u8.check(!imported && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && statistics.contextDigests == before.contextDigests, "a program import of an EmbedderDecoded body did not miss Provenance before its context"_s))
            return false;
    }

    // A function import, an attach to a generated function UCB, and seeds of decoded functions.
    auto parent = recordedParent(vm, registry, "function jitcacheU8Provenance(a) { return [\"jitcache-u8-p-one\", \"jitcache-u8-p-two\"].indexOf(a); }\n"_s, 0x83);
    if (!u8.check(!!parent, "the provenance parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    ChildInputs inputs = childInputs(child, parent->program.source, parent->key, ChildTable::Declarations, 0);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
    auto embedderBody = producer ? buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::EmbedderDecoded, parseFieldsOf(results)) : std::nullopt;
    auto generatedBody = producer ? buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results)) : std::nullopt;
    if (!u8.check(embedderBody && generatedBody, "the provenance function's bodies could not be built"_s))
        return false;
    auto withChildRequest = [&](const auto& act) {
        RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, child);
        return act(request);
    };
    artifact.list(inputs.key, 32, embedderBody->validated(inputs.key, 4001));
    before = statistics;
    Strong<UnlinkedCodeBlock> importedFunction { vm, withChildRequest([&](RequestState& request) {
        return importBody(request);
    }) };
    if (!u8.check(!importedFunction && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1, "a function import of an EmbedderDecoded body did not miss Provenance"_s))
        return false;

    Strong<UnlinkedFunctionCodeBlock> generatedFunction = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
    if (!u8.check(!!generatedFunction, "the provenance function failed to generate"_s))
        return false;
    withChildRequest([&](RequestState& request) {
        recordGenerated(request, *generatedFunction.get());
        return true;
    });
    artifact.list(inputs.key, 33, embedderBody->validated(inputs.key, 4002));
    before = statistics;
    withChildRequest([&](RequestState& request) {
        attachLive(request, *generatedFunction.get());
        return true;
    });
    auto generatedRecord = registry.recordOf(*generatedFunction.get());
    bool attachMiss = missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && generatedRecord && generatedRecord->missedBodyVersion == 33 && !generatedRecord->hasPendingImport;
    if (!u8.check(attachMiss, "an attach of an EmbedderDecoded body to a generated function UCB did not miss Provenance, stamped"_s))
        return false;

    auto seed = [&](const TestBody& body, uint64_t token, CoreProvenance provenance) {
        Strong<UnlinkedFunctionCodeBlock> decoded = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
        if (!decoded)
            return false;
        artifact.list(inputs.key, token, body.validated(inputs.key, 4000 + token));
        UCBStatistics seedBefore = statistics;
        withChildRequest([&](RequestState& request) {
            seedDecoded(request, *decoded.get());
            return true;
        });
        auto view = registry.recordOf(*decoded.get());
        std::span<const uint8_t> bodyMap = butterflyMapOf(body);
        Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
        bool keeps = registry.copyGeneratedButterflyMap(*decoded.get(), kept.mutableSpan());
        bool expectsMap = provenance == CoreProvenance::Generated && state.productionActive();
        return statistics.seededDecodes == seedBefore.seededDecodes + 1 && view && view->origin == UCBOrigin::Decoded && view->hasPendingImport
            && keeps == expectsMap && (!keeps || equalSpans(kept.span(), bodyMap));
    };
    if (!u8.check(seed(*embedderBody, 34, CoreProvenance::EmbedderDecoded), "an EmbedderDecoded body did not seed a decoded function whose core matches, or left it a butterfly map"_s))
        return false;
    return u8.check(seed(*generatedBody, 35, CoreProvenance::Generated), "a Generated body did not seed a decoded function whose core matches, or kept its butterfly map otherwise than while production is active"_s);
}

// U8: didDecodeCachedSlots with the requested slot empty and the other decoded records the other slot and leaves the request
// unsettled, so didGenerate records the generated slot (sections 7.1 and 7.3.5). Any role that tracks keys.
static bool testDecodedSlotRecords(VM& vm, VMState& state, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };
    auto parent = recordedParent(vm, registry, "function jitcacheU8Slots(a) { this.a = a; }\n"_s, 0x84);
    if (!u8.check(!!parent, "the slots parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    SourceCode source = child.linkedSourceCode(parent->program.source);
    Strong<UnlinkedFunctionCodeBlock> forCall = generateBodyOf(vm, child, source, CodeSpecializationKind::CodeForCall);
    Strong<UnlinkedFunctionCodeBlock> forConstruct = generateBodyOf(vm, child, source, CodeSpecializationKind::CodeForConstruct);
    if (!u8.check(forCall && forConstruct, "the slots function failed to generate"_s))
        return false;
    {
        FunctionBodyRequest request(vm, child, source, CodeSpecializationKind::CodeForConstruct, { });
        request.didDecodeCachedSlots(forCall.get(), nullptr);
        request.didGenerate(*forConstruct.get());
    }
    Digest256 identityDigest = childIdentityDigest(parent->key, ChildTable::Declarations, 0);
    auto callRecord = registry.recordOf(*forCall.get());
    auto constructRecord = registry.recordOf(*forConstruct.get());
    bool recorded = callRecord && callRecord->origin == UCBOrigin::Decoded && callRecord->key == BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForCall, { }, identityDigest)
        && constructRecord && constructRecord->origin == UCBOrigin::Generated && constructRecord->key == BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, { }, identityDigest);
    return u8.check(recorded, "didDecodeCachedSlots did not record the other slot as decoded while leaving the request to record its generated slot"_s);
}

// U8: a decoded function, seeded or attached through the native decode of a bytecode-cache payload, keeps the butterfly map
// of a Generated body in a VM whose production is active, and buildSections writes it with provenance Generated; seeded
// from an EmbedderDecoded body, or in a Consumer, it keeps none and is written EmbedderDecoded (sections 7.3.3, 7.3.4, 8.2).
static bool testDecodedButterflyProvenance(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    String text = "function jitcacheU8Literal(s) { return [\"jitcache-u8-literal-alpha\", \"jitcache-u8-literal-beta\"].indexOf(s); }\n"_s;
    ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
    if (!u8.check(generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 1, "the butterfly-provenance program failed to generate"_s))
        return false;
    SourceCodeKey cacheKey = programCacheKey(generated.source);
    RefPtr<CachedBytecode> payload = encodeCodeBlock(vm, cacheKey, generated.codeBlock.get());
    UnlinkedFunctionExecutable& generatedFunction = *generated.codeBlock.get()->functionDecl(0);
    Strong<UnlinkedFunctionCodeBlock> producer { vm, decodedBody(vm, generatedFunction, generated.source) };
    if (!u8.check(payload && producer, "the butterfly-provenance program did not encode to a bytecode cache"_s))
        return false;
    FunctionParseFields parseFields { generatedFunction.features(), generatedFunction.lexicallyScopedFeatures(), generatedFunction.hasCapturedVariables() };

    auto scenario = [&](uint8_t parentFill, CoreProvenance provenance, bool attachInstead, bool expectsMap) {
        Strong<UnlinkedProgramCodeBlock> decoded { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, cacheKey, Ref<CachedBytecode> { *payload }) };
        BodyKey parentKey = testKey(parentFill);
        if (!decoded || !registry.recordCodeBlock(*decoded.get(), testRecord(parentKey, UCBOrigin::Decoded)))
            return false;
        UnlinkedFunctionExecutable& function = *decoded.get()->functionDecl(0);
        ChildInputs inputs = childInputs(function, generated.source, parentKey, ChildTable::Declarations, 0);
        auto body = buildTestBody(vm, *producer.get(), &generatedFunction, inputs.key, inputs.context, provenance, parseFields);
        if (!body)
            return false;
        TestArtifact artifact;
        StubbedBodyLookup lookup { state, artifact };
        UnlinkedFunctionCodeBlock* slot = nullptr;
        if (attachInstead) {
            slot = decodedBody(vm, function, generated.source); // no body: the decode is recorded as Decoded
            if (!slot)
                return false;
            // A live decoded UCB attaches only once the constants its atom map marks are atoms (C10, section 7.3.2 step 9),
            // which the native decode leaves plain (F19); a property-key use of each would atomize it in place.
            for (auto& constant : slot->constantRegisters()) {
                if (JSValue value = constant.get(); value && value.isString())
                    atomizeStringConstant(vm, *asString(value));
            }
            artifact.list(inputs.key, 41, body->validated(inputs.key, 5000));
            if (decodedBody(vm, function, generated.source) != slot) // the filled slot: an attach
                return false;
        } else {
            artifact.list(inputs.key, 41, body->validated(inputs.key, 5000));
            slot = decodedBody(vm, function, generated.source); // the decode: a seed
        }
        auto view = slot ? registry.recordOf(*slot) : std::nullopt;
        if (!view || view->origin != UCBOrigin::Decoded || !view->hasPendingImport || view->matchedBodyVersion != 5000)
            return false;
        std::span<const uint8_t> bodyMap = butterflyMapOf(*body);
        Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
        if (registry.copyGeneratedButterflyMap(*slot, kept.mutableSpan()) != expectsMap)
            return false;

        // A CodeBlock of the decoded function, which buildSections captures from (section 8.2).
        FunctionExecutable* functionExecutable = function.link(vm, nullptr, generated.source);
        JSFunction* jsFunction = JSFunction::create(vm, globalObject, functionExecutable, globalObject->globalScope());
        DeferGC deferGC(vm);
        CodeBlock* codeBlock = functionExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, jsFunction, globalObject->globalScope());
        if (!codeBlock || codeBlock->unlinkedCodeBlock() != slot)
            return false;
        auto sections = buildSections(vm, *codeBlock, state.twinBudget().get());
        if (!sections)
            return false;
        auto identity = parseIdentitySection(sections->identity.span(), true);
        if (!identity)
            return false;
        bool anyMarked = std::ranges::any_of(identity->butterflyMap, [](uint8_t byte) {
            return !!byte;
        });
        if (expectsMap)
            return identity->provenance == CoreProvenance::Generated && equalSpans(identity->butterflyMap, bodyMap) && anyMarked;
        return identity->provenance == CoreProvenance::EmbedderDecoded && !anyMarked;
    };

    if (state.productionActive()) {
        return u8.check(scenario(0x91, CoreProvenance::Generated, false, true), "a function seeded from a Generated body did not keep its butterfly map or was not written Generated with it"_s)
            && u8.check(scenario(0x92, CoreProvenance::Generated, true, true), "a function attached from a Generated body did not keep its butterfly map or was not written Generated with it"_s)
            && u8.check(scenario(0x93, CoreProvenance::EmbedderDecoded, false, false), "a function seeded from an EmbedderDecoded body kept a map or was not written EmbedderDecoded"_s);
    }
    return u8.check(scenario(0x94, CoreProvenance::Generated, false, false), "a function seeded in a Consumer kept a map or was not written EmbedderDecoded"_s);
}

// A direct eval inside `caller`, whose record has `callerKey`, imported from a body the test serves: it relies on no supplied
// digest, so it verifies none, and its record keeps no provider (section 3.5).
static bool importDirectEval(VM& vm, JSGlobalObject* globalObject, VMState& state, TestArtifact& artifact, UnlinkedCodeBlock& caller, const BodyKey& callerKey, uint8_t salt)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    String evalText = makeString("jitcacheU8Eval"_s, salt, " + 1"_s);
    SourceCode evalSource = makeSource(evalText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    TDZEnvironment tdz = tdzNames(vm, { "jitcacheU8EvalTDZ"_s });
    PrivateNameEnvironment privateNames;
    Strong<DirectEvalExecutable> executable { vm, DirectEvalExecutable::create(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, true, EvalContextType::None, &tdz, &privateNames) };
    if (!executable || !executable.get()->unlinkedCodeBlock())
        return false;
    BytecodeIndex siteIndex(7);
    DirectEvalSite site { &caller, siteIndex };
    BodyKey key = BodyKey::make(IdentityKind::DirectEval, CodeSpecializationKind::CodeForCall, { }, directEvalIdentityDigest(callerKey, siteIndex, sourceDigest(evalSource.view())));
    Digest256 context = directEvalContextDigest(evalSource.startOffset(), evalSource.firstLine().oneBasedInt(), NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
        PrivateBrandRequirement::None, false, true, EvalContextType::None, tdz, privateNames);
    auto body = buildTestBody(vm, *executable.get()->unlinkedCodeBlock(), nullptr, key, context, CoreProvenance::Generated);
    if (!body)
        return false;
    artifact.list(key, 51, body->validated(key, 6000 + salt));
    UCBStatistics before = statistics;
    RequestState request(vm, RequestKind::DirectEval, evalSource, { }, NoLexicallyScopedFeatures);
    request.globalExecutable = executable.get();
    request.site = &site;
    request.variablesUnderTDZ = &tdz;
    request.privateNameEnvironment = &privateNames;
    Strong<UnlinkedCodeBlock> imported { vm, importBody(request) };
    auto view = imported ? registry.recordOf(*imported.get()) : std::nullopt;
    return view && !view->suppliedDigestProvider && statistics.imports == before.imports + 1 && statistics.suppliedDigestVerifications == before.suppliedDigestVerifications;
}

// U8: supplied digests (sections 3.5 and 7.3.1, step 4e). With strict on, an import of a program whose provider supplies its
// digest and an import of one of its functions verify the provider once in all, and their records keep it; with strict off
// they verify nothing and keep none; a direct eval inside the function imports without a verification, also when the
// program and the function were seeded, and keeps no provider. Every digest here is its text's: the cases of a digest that
// differs from its text, which T7 reports wherever a request takes one, are the InvalidMaterial scope's.
static bool testSuppliedDigestImports(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    bool strict = state.strict();
    TestArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    // Each chain makes a provider of its own over this text, so both chains compute the same keys. S2 runs once per provider
    // (section 7.3.1, step 4e): had the seeded chain reused the provider the import chain verified, a direct eval import that
    // wrongly relied on it would find it verified and count nothing. Seeding verifies nothing (section 3.5), so the seeded
    // chain's provider is still unverified when its direct eval imports, and such an import would verify it there.
    String text = "function jitcacheU8Supplied(a) { return eval('a + 1'); }\n"_s;

    auto importChain = [&](bool seed) {
        Ref<SuppliedDigestProvider> provider = SuppliedDigestProvider::create(text, sourceDigest(text));
        SourceProvider* expectedProvider = strict ? provider.ptr() : nullptr;
        ProgramRequestFixture fixture = programRequestFixture(globalObject, Ref<SourceProvider> { provider.get() });
        BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
        Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
        auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
        if (!body)
            return false;
        artifact.list(key, seed ? 61 : 62, body->validated(key, seed ? 7000 : 7001));
        Strong<UnlinkedCodeBlock> program;
        {
            RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
            prepareProgramRequest(request, *fixture.executable.get());
            if (seed) {
                program = generateProgramFor(vm, fixture);
                if (program)
                    seedDecoded(request, *program.get());
            } else
                program = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto programView = program ? registry.recordOf(*program.get()) : std::nullopt;
        if (!programView || !programView->hasPendingImport || programView->suppliedDigestProvider != expectedProvider)
            return false;

        UnlinkedFunctionExecutable& child = *program.get()->functionDecl(0);
        ChildInputs inputs = childInputs(child, fixture.source, key, ChildTable::Declarations, 0);
        TwinParseResults results;
        Strong<UnlinkedFunctionCodeBlock> childProducer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
        auto childBody = childProducer ? buildTestBody(vm, *childProducer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results)) : std::nullopt;
        if (!childBody)
            return false;
        artifact.list(inputs.key, seed ? 63 : 64, childBody->validated(inputs.key, seed ? 7002 : 7003));
        Strong<UnlinkedCodeBlock> function;
        {
            RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
            prepareBodyRequest(request, child);
            if (seed) {
                function = Strong<UnlinkedCodeBlock> { vm, generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall).get() };
                if (function)
                    seedDecoded(request, *function.get());
            } else
                function = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto functionView = function ? registry.recordOf(*function.get()) : std::nullopt;
        if (!functionView || !functionView->hasPendingImport || functionView->suppliedDigestProvider != expectedProvider)
            return false;
        // Verified now exactly when the program was imported with strict on; the direct eval's import leaves that as it is.
        bool verified = registry.suppliedDigestVerified(provider->asID());
        if (verified != (strict && !seed))
            return false;
        if (!importDirectEval(vm, globalObject, state, artifact, *function.get(), functionView->key, seed ? 1 : 2))
            return false;
        return registry.suppliedDigestVerified(provider->asID()) == verified;
    };

    UCBStatistics before = statistics;
    if (!u8.check(importChain(false), "the import of a program, of its function and of a direct eval inside it did not succeed with the records section 6.1 gives, or did not leave the provider verified exactly with strict on"_s))
        return false;
    if (!u8.check(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications + (strict ? 1 : 0), "the imports of a program and its function did not verify its provider exactly once with strict on, and never with it off"_s))
        return false;
    before = statistics;
    if (!u8.check(importChain(true), "the seeds of a program and its function, then the import of a direct eval inside it, did not succeed, or the direct eval verified the provider the seeds left unverified"_s))
        return false;
    return u8.check(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications, "seeding or a direct eval's import verified a supplied digest"_s);
}

// U8 in a Producer: no record keeps a provider, since only a VM that imports with strict on keeps one (section 6.1).
static bool testProducerRecordsKeepNoProvider(VM& vm, VMState& state, JSGlobalObject* globalObject, SelfTestPart& u8)
{
    UCBRegistry& registry = state.registry();
    String text = "function jitcacheU8ProducerSupplied() { return 1; }\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, SuppliedDigestProvider::create(text, sourceDigest(text)));
    Strong<UnlinkedCodeBlock> program = generateProgramFor(vm, fixture);
    if (!u8.check(!!program, "the Producer's program failed to generate"_s))
        return false;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        recordGenerated(request, *program.get());
    }
    auto view = registry.recordOf(*program.get());
    std::optional<ExecutableIdentity> childIdentity = registry.identityOf(*program.get()->functionDecl(0));
    auto* child = childIdentity ? std::get_if<ChildIdentity>(&*childIdentity) : nullptr;
    bool keepsNone = view && !view->suppliedDigestProvider && child && child->parent && !child->parent->suppliedDigestProvider();
    return u8.check(keepsNone, "a Producer's record or its children's identities keep a supplied digest's provider"_s);
}

// U8 (section 13.1), the parts this VM's configuration allows. Each part's failure names the configuration it ran in.
static bool testEngine(VM& vm, String& failure)
{
    VMState* state = vm.jitCacheState();
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    SelfTestPart u8 { "U8"_s, failure };
    if (!u8.check(state && state->tracksKeys() && globalObject, "the self-test runs outside a VM entry or in a VM that does not track keys"_s))
        return false;
    ASCIILiteral roleName = state->role() == Role::Producer ? "Producer"_s : state->role() == Role::Consumer ? "Consumer"_s : "ConsumerProducer"_s;
    auto passes = [&](bool result) {
        if (!result)
            failure = makeString(failure, " (in a "_s, roleName, state->strict() ? " with strict on)"_s : " with strict off)"_s);
        return result;
    };
    if (!passes(testDecodedSlotRecords(vm, *state, u8)))
        return false;
    if (state->role() == Role::Producer)
        return passes(testProducerRecordsKeepNoProvider(vm, *state, globalObject, u8));
    if (!u8.check(state->importsEnabled(), "a Consumer or ConsumerProducer does not import"_s))
        return passes(false);
    return passes(testAttachToGeneratedProgram(vm, *state, globalObject, u8))
        && passes(testAtomMapAttach(vm, *state, u8))
        && passes(testAtomMapAttachToProgram(vm, *state, globalObject, u8))
        && passes(testRootImport(vm, *state, u8))
        && passes(testProvenance(vm, *state, globalObject, u8))
        && passes(testDecodedButterflyProvenance(vm, *state, globalObject, u8))
        && passes(testSuppliedDigestImports(vm, *state, globalObject, u8));
}

// U8's two cases of a supplied digest that differs from its text, alone in their scope (section 13.1): T7 reports such a
// digest wherever a request takes it, and the second case turns cache activity off for good. In a Consumer with strict on,
// a decoded program under a wrong supplied digest is first matched by its core, which no digest enters, and seeded with no
// verification, since S2 runs only at an import. Then a program whose import relies on a wrong supplied digest is invalid
// material at ucb.supplied-digest before anything is decoded: its body's core and feedback sections are broken, so a decode
// or a feedback parse would raise another step. With a twin report open, each case adds exactly one difference, T7's.
static bool testSuppliedDigestInvalidMaterial(VM& vm, String& failure)
{
    SelfTestPart u8 { "U8 (invalid material)"_s, failure };
    VMState* state = vm.jitCacheState();
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    if (!u8.check(state && globalObject && state->importsEnabled() && state->strict(), "the invalid-material cases need a VM that imports with strict on"_s))
        return false;
    UCBRegistry& registry = state->registry();
    UCBStatistics& statistics = registry.statistics();
    TwinReportSink* sink = state->twinReportSink();
    auto differences = [&] {
        return sink ? sink->differences() : 0;
    };
    TestArtifact artifact;
    StubbedBodyLookup lookup { *state, artifact };

    // The decoded program.
    Ref<SuppliedDigestProvider> decodedProvider = SuppliedDigestProvider::create("var jitcacheU8WrongDecoded = 'jitcache-u8-wrong-decoded';\n"_s, filledDigest(0xde));
    ProgramRequestFixture decodedFixture = programRequestFixture(globalObject, Ref<SourceProvider> { decodedProvider.get() });
    BodyKey decodedKey = programKey(decodedFixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> decodedProducer = generateProgramFor(vm, decodedFixture);
    Strong<UnlinkedCodeBlock> decoded = generateProgramFor(vm, decodedFixture);
    auto seededBody = decodedProducer ? buildTestBody(vm, *decodedProducer.get(), nullptr, decodedKey, programContext(decodedFixture.source, *decodedFixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
    if (!u8.check(decoded && seededBody, "the wrong-digest decoded program could not be built"_s))
        return false;
    artifact.list(decodedKey, 65, seededBody->validated(decodedKey, 7004));
    UCBStatistics beforeSeed = statistics;
    unsigned differencesBeforeSeed = differences();
    {
        RequestState request(vm, RequestKind::Program, decodedFixture.source, { }, decodedFixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *decodedFixture.executable.get());
        seedDecoded(request, *decoded.get());
    }
    bool matchedByCore = state->tracksKeys() && statistics.seededDecodes == beforeSeed.seededDecodes + 1 && statistics.suppliedDigestVerifications == beforeSeed.suppliedDigestVerifications
        && !registry.suppliedDigestVerified(decodedProvider->asID());
    if (!u8.check(matchedByCore, "a decoded program under a wrong supplied digest was verified, or not matched by its core"_s))
        return false;
    if (!u8.check(!sink || differences() == differencesBeforeSeed + 1, "the seed under a wrong supplied digest did not add exactly T7's difference to the twin report"_s))
        return false;

    // The import.
    String text = "var jitcacheU8WrongSupplied = 'jitcache-u8-wrong-supplied-digest';\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, SuppliedDigestProvider::create(text, filledDigest(0xee)));
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
    if (!u8.check(!!body, "the wrong-digest program's body could not be built"_s))
        return false;
    TestBody undecodable { body->identity, Vector<uint8_t>(FillWith { }, body->core.size(), 0), brokenMagic(body->feedback) };
    artifact.list(key, 71, undecodable.validated(key, 8000));
    UCBStatistics before = statistics;
    unsigned differencesBeforeImport = differences();
    Strong<UnlinkedCodeBlock> imported;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        imported = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
    }
    auto step = [&](InvalidMaterialStep invalidStep) {
        return statistics.invalidMaterial[static_cast<size_t>(invalidStep)] - before.invalidMaterial[static_cast<size_t>(invalidStep)];
    };
    bool raised = !imported && !state->tracksKeys() && step(InvalidMaterialStep::SuppliedDigest) == 1 && !step(InvalidMaterialStep::Decode) && !step(InvalidMaterialStep::Feedback)
        && statistics.imports == before.imports;
    if (!u8.check(raised, "an import relying on a supplied digest that differs from its text did not raise invalid material at ucb.supplied-digest before decoding"_s))
        return false;
    return u8.check(!sink || differences() == differencesBeforeImport + 1, "the import under a wrong supplied digest did not add exactly T7's difference to the twin report"_s);
}

// U9 (section 7.2.6): every JSC builtin's generated metadata carries the digest of the text name##Source() spans.
static bool testBuiltinMetadata(VM& vm, String& failure)
{
    SelfTestPart u9 { "U9"_s, failure };
    BuiltinExecutables& builtins = *vm.builtinExecutables();
    auto check = [&](const BuiltinSourceMetadata& metadata, const SourceCode& source, ASCIILiteral name) {
        if (!u9.check(metadata.hasSourceDigest, makeString("the metadata of "_s, name, " carries no source digest"_s)))
            return false;
        Digest256 recorded = metadata.sourceDigest;
        return u9.check(recorded == sourceDigest(source.view()), makeString("the metadata digest of "_s, name, " differs from the digest of its source"_s));
    };
#define JITCACHE_U9_CHECK_BUILTIN(name, functionName, overriddenName, length) \
    if (!check(s_JSCBuiltinSourceMetadata[static_cast<unsigned>(BuiltinCodeIndex::name)], builtins.name##Source(), #name ""_s)) \
        return false;
    JSC_FOREACH_BUILTIN_CODE(JITCACHE_U9_CHECK_BUILTIN)
#undef JITCACHE_U9_CHECK_BUILTIN
    return true;
}

} // namespace UCBSelfTestInternal

bool runUCBSelfTest(VM& vm, UCBSelfTestScope scope, String& failure)
{
    using namespace UCBSelfTestInternal;

    JSLockHolder locker(vm);
    switch (scope) {
    case UCBSelfTestScope::Configured:
        return runSHA256SelfTest(failure) && testSourceDigests(failure) && testKeys(vm, failure) && testRegistry(vm, failure) && testSections(vm, failure)
            && testLLIntCounter(failure) && testCodec(vm, failure) && testEngine(vm, failure) && testBuiltinMetadata(vm, failure);
    case UCBSelfTestScope::InvalidMaterial:
        return testSuppliedDigestInvalidMaterial(vm, failure);
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
