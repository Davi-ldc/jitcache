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

#include "CodeCache.h"
#include "JSLock.h"
#include "ParserError.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "StrongInlines.h"
#include "UCBRegistry.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <array>
#include <span>
#include <wtf/Function.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
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

} // namespace UCBSelfTestInternal

bool runUCBSelfTest(VM& vm, UCBSelfTestScope scope, String& failure)
{
    JSLockHolder locker(vm);
    switch (scope) {
    case UCBSelfTestScope::Configured:
        // FIXME: U2, U3 and U5 to U9 join this sequence, in their order, with the tasks that implement them (SPEC-ucb.md
        // section 15.3).
        return runSHA256SelfTest(failure) && UCBSelfTestInternal::testRegistry(vm, failure);
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
