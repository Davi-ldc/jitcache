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

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "ArrayPrototype.h"
#include "CachedBytecode.h"
#include "CallData.h"
#include "CodeCache.h"
#include "CommonSlowPaths.h"
#include "DeferGC.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "ParserError.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "UCBFeedback.h"
#include "UCBRegistry.h"
#include "UCBSections.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "VMEntryScope.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <wtf/Function.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

// The scopes of $vm.jitCacheUCBSelfTest() and $vm.jitCacheUCBSelfTest({ invalidMaterial: true }) (SPEC-ucb.md section
// 13.1, M4). Configured runs U1 to U7, U9 and the parts of U8 the VM's configuration allows; InvalidMaterial runs only
// U8's case that makes an import invalid material, which turns cache activity off for good, so nothing runs after it.
enum class UCBSelfTestScope : uint8_t { Configured, InvalidMaterial };

// Defined beside the code they test, whose internals stay private to its file.
bool runSHA256SelfTest(String& failure); // U1, JITCacheSHA256.cpp
bool isUCBRegistryLockHeldByCurrentThread(); // UCBRegistry.cpp

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
        && rejects(programSection, "one byte fewer"_s, false, [](auto& bytes) { bytes.removeLast(); });
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
    if (bytes[feedbackDidOptimizeOffset] != static_cast<uint8_t>(codeBlock.didOptimize()) || bytes[feedbackQuickDFGTierUpOffset] != static_cast<uint8_t>(codeBlock.quickDFGTierUp())
        || bytes[feedbackQuickFTLTierUpOffset] != static_cast<uint8_t>(codeBlock.isQuickFTLTierUp()) || bytes[feedbackReservedByteOffset])
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
    BaselineExecutionCounter& counter = codeBlock.llintExecuteCounter();
    return section.didOptimize() == codeBlock.didOptimize() && section.quickDFGTierUp() == codeBlock.quickDFGTierUp() && section.quickFTLTierUp() == codeBlock.isQuickFTLTierUp()
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
    if (expected.didOptimize() != actual.didOptimize() || expected.quickDFGTierUp() != actual.quickDFGTierUp() || expected.isQuickFTLTierUp() != actual.isQuickFTLTierUp())
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
        && (layout.size == layout.end || rejects("nonzero final padding"_s, [&](auto& bytes) { bytes[layout.end] = 1; }));
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

} // namespace UCBSelfTestInternal

bool runUCBSelfTest(VM& vm, UCBSelfTestScope scope, String& failure)
{
    JSLockHolder locker(vm);
    switch (scope) {
    case UCBSelfTestScope::Configured:
        // FIXME: U2, U3 and U7 to U9 join this sequence, in their order, with the tasks that implement them (SPEC-ucb.md
        // section 15.3).
        return runSHA256SelfTest(failure) && UCBSelfTestInternal::testRegistry(vm, failure) && UCBSelfTestInternal::testSections(vm, failure)
            && UCBSelfTestInternal::testLLIntCounter(failure);
    case UCBSelfTestScope::InvalidMaterial:
        // FIXME: U8's supplied-digest case belongs here, with the engine's part of U8 (SPEC-ucb.md section 15.3, task 7).
        failure = "U8: this build's self-test has no invalid-material case"_s;
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
