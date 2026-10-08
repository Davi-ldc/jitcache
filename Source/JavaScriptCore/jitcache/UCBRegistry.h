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

#pragma once

#include "ParserModes.h"
#include "SourceID.h"
#include "SourceProvider.h"
#include "UCBKeys.h"
#include <array>
#include <optional>
#include <span>
#include <variant>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;

} // namespace JSC

namespace JSC::JITCache {

// The parent-key registry, pending imports and statistics (SPEC-ucb.md section 6).

class ValidatedBody; // the integrator's handle on a validated body file (R-INT-3)

// Statistics (section 6.5).
enum class MissReason : uint8_t { NoKey, NoBody, BodyUnchanged, RequestKey, Context, Provenance, Holder, AtomMap, CoreDigest, Stack };
enum class InvalidMaterialStep : uint8_t { Identity, Feedback, Decode, Closure, StrictCore, SuppliedDigest }; // ucb.identity ... ucb.supplied-digest

// What produced the UCB in this VM. A capture of a Decoded one writes provenance EmbedderDecoded unless its record keeps
// generatedButterflyMap (section 8.2).
enum class UCBOrigin : uint8_t { Generated, Decoded, Imported };
enum class ImportResolution : uint8_t { Installed, DroppedByGate };

struct UCBStatistics {
    uint64_t imports { 0 }; // section 7.3.1 succeeded
    uint64_t seededDecodes { 0 }; // section 7.3.3 succeeded
    uint64_t attaches { 0 }; // section 7.3.4 succeeded
    uint64_t gateDrops { 0 }; // pending imports the shouldJIT gate dropped (section 6.4)
    uint64_t sourceDigests { 0 }; // root source digests computed from text (form 3 of section 3.5); a direct eval's is not counted
    uint64_t suppliedSourceDigests { 0 }; // root digests taken from builtin metadata or a provider (forms 1 and 2)
    uint64_t suppliedDigestVerifications { 0 }; // supplied digests an import verified (section 7.3.1, step 4e)
    uint64_t contextDigests { 0 }; // context digests computed, by requests and captures (section 3.4)
    uint64_t holderDigests { 0 }; // holder digests computed, by requests (C11, a root's context) and captures
    uint64_t tdzEnvironmentDigests { 0 }; // TDZ environments digested, each once while it lives (section 3.4)
    std::array<uint64_t, 10> misses { }; // indexed by MissReason
    std::array<uint64_t, 3> records { }; // indexed by UCBOrigin
    std::array<uint64_t, 6> invalidMaterial { }; // indexed by InvalidMaterialStep
};
static_assert(static_cast<size_t>(MissReason::Stack) + 1 == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(static_cast<size_t>(UCBOrigin::Imported) + 1 == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(static_cast<size_t>(InvalidMaterialStep::SuppliedDigest) + 1 == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

// Identities (section 6.1).
// suppliedDigestProvider, in each type below: the provider whose supplied digest (form 2 of section 3.5) an import at the
// key relies on, kept only in a VM that imports with strict on, so that the import can verify it (section 7.3.1, step 4e);
// else null, and always null for a direct eval and everything nested in one (section 3.5).
// What the children of one recorded UCB share: its key and suppliedDigestProvider, kept once per parent and alive while
// any of its child UFEs has an entry.
class ParentIdentity final : public ThreadSafeRefCounted<ParentIdentity> {
public:
    static Ref<ParentIdentity> create(const BodyKey&, RefPtr<SourceProvider>&& suppliedDigestProvider);
    const BodyKey& key() const LIFETIME_BOUND;
    SourceProvider* suppliedDigestProvider() const;

private:
    ParentIdentity(const BodyKey&, RefPtr<SourceProvider>&&);
    BodyKey m_key;
    RefPtr<SourceProvider> m_suppliedDigestProvider;
};

struct ChildIdentity { // parent never null; 16 bytes
    RefPtr<ParentIdentity> parent;
    ChildTable table;
    uint32_t index;
};
static_assert(sizeof(ChildIdentity) == 16);

struct RootIdentity { // FunctionConstructor or Builtin
    IdentityKind kind;
    Digest256 identityDigest;
    RefPtr<SourceProvider> suppliedDigestProvider;
};

using ExecutableIdentity = std::variant<ChildIdentity, RootIdentity>;

// Pending imports (section 6.4). One lives in its UCB's record from the moment the request point records it until the
// install glue resolves it, and otherwise goes with the UCB. It never enters UnlinkedCodeBlock::m_unlinkedBaselineCode.
class PendingImport final : public ThreadSafeRefCounted<PendingImport> {
public:
    enum class Origin : uint8_t { Seeded, Reused }; // Seeded: the UCB's feedback came from this body before publication. Reused: a live UCB, not seeded.
    static Ref<PendingImport> create(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    ValidatedBody& body() const; // pins the validated payload (THREAD Storage)
    const BodyKey& key() const;
    uint64_t indexToken() const; // the bodyVersion token the import was made at (R-INT-3)
    Origin origin() const;
    ~PendingImport(); // out of line, in UCBRegistry.cpp, where ValidatedBody is complete

private:
    PendingImport(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    const Ref<ValidatedBody> m_body;
    const BodyKey m_key;
    const uint64_t m_indexToken;
    const Origin m_origin;
};

struct UCBRecord {
    BodyKey key;
    RecordedContext context; // the context's inputs, or a direct eval's digest (section 3.4)
    UCBOrigin origin;
    LexicallyScopedFeatures keyFeatures { NoLexicallyScopedFeatures }; // a program, module or indirect eval: the strict and with-scope bits of its key
    std::optional<uint64_t> missedBodyVersion; // the index token at which no body fitted the UCB (section 7.3.2, R-INT-3)
    std::optional<uint64_t> matchedBodyVersion; // the commit identifier of the body file the UCB was imported, seeded or attached from, or matched in every check but C10 (sections 7.3.2 and 7.3.4)
    RefPtr<PendingImport> pendingImport; // at most one
    RefPtr<SourceProvider> suppliedDigestProvider;
    // Origin Decoded, in a VM whose production is active: the butterfly map of the body of provenance Generated the UCB
    // was last seeded or attached from (sections 7.3.3, 7.3.4 and 8.2); ceil(N / 8) bytes.
    std::optional<Vector<uint8_t>> generatedButterflyMap;
};

// One per VM, owned by the integrator's per-VM state (R-INT-1), created when start configures the VM and destroyed after
// Heap::lastChanceToFinalize. It serves every role and is not charged to the production limit. Its maps hold raw cell
// pointers, none of which is a GC root or holds a cell.
//
// m_lock is a leaf (section 6.3): no other lock is taken while it is held, and inside its critical sections no code
// allocates a cell, enters or leaves a DeferGC or DeferGCForAWhile scope, runs a write barrier, calls into the collector or
// the integrator, destroys a PendingImport or drops a reference to a SourceProvider or a ParentIdentity. A destructor hook
// moves such references out under the lock and lets them go after unlocking.
class UCBRegistry {
    WTF_MAKE_NONCOPYABLE(UCBRegistry);
    WTF_MAKE_TZONE_ALLOCATED(UCBRegistry);

public:
    // WTF_MAKE_NONCOPYABLE suppresses the implicit default constructor; the integrator's VM state holds the registry by value
    // (SPEC-integrator.md section 4.1).
    UCBRegistry() = default;

    // UFE identities. VM thread.
    void recordRootExecutable(const UnlinkedFunctionExecutable&, const RootIdentity&);
    std::optional<ExecutableIdentity> identityOf(const UnlinkedFunctionExecutable&) const;

    // UCB records. VM thread.
    // Records the UCB and gives each child UFE of its two tables a ChildIdentity, in table order, all referencing one
    // ParentIdentity made from the record's key and suppliedDigestProvider (none for a UCB without children).
    // A UCB that already has a record keeps it (THREAD: a UCB keeps the key it was recorded under); returns false then.
    bool recordCodeBlock(UnlinkedCodeBlock&, UCBRecord&&);
    struct RecordView {
        BodyKey key;
        RecordedContext context;
        UCBOrigin origin;
        LexicallyScopedFeatures keyFeatures;
        std::optional<uint64_t> missedBodyVersion;
        std::optional<uint64_t> matchedBodyVersion;
        bool hasPendingImport;
        RefPtr<SourceProvider> suppliedDigestProvider;
    };
    std::optional<RecordView> recordOf(const UnlinkedCodeBlock&) const;
    std::optional<BodyKey> keyOf(const UnlinkedCodeBlock&) const;
    void setMissedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // A miss at C10 alone (section 7.3.2): the commit identifier of the file the UCB matched in every other check.
    void setMatchedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // False when the UCB has no record or already has a pending import. On success the record's matchedBodyVersion becomes
    // the commit identifier of the import's body file, and its generatedButterflyMap becomes the argument (section 7.3.4).
    bool attachPendingImport(const UnlinkedCodeBlock&, Ref<PendingImport>&&, std::optional<Vector<uint8_t>>&& generatedButterflyMap);
    // Section 8.2: copies the record's generatedButterflyMap into `out`, whose size is ceil(N / 8), and returns true;
    // returns false, copying nothing, when the record keeps none.
    bool copyGeneratedButterflyMap(const UnlinkedCodeBlock&, std::span<uint8_t> out) const;
    RefPtr<PendingImport> pendingImport(const UnlinkedCodeBlock&) const;
    // Detaches the import if it is still the attached one. DroppedByGate also sets missedBodyVersion to the import's index
    // token and counts statistics().gateDrops (section 6.4).
    void resolvePendingImport(const UnlinkedCodeBlock&, const PendingImport&, ImportResolution);

    // Supplied digests an import verified (section 7.3.1, step 4e), by the provider's SourceID, which no other provider
    // ever takes. VM thread.
    bool suppliedDigestVerified(SourceID) const;
    void markSuppliedDigestVerified(SourceID);

    // Destructors: the thread sweeping the VM's heap (F12).
    void unlinkedCodeBlockDestroyed(const UnlinkedCodeBlock*);
    void unlinkedFunctionExecutableDestroyed(const UnlinkedFunctionExecutable*);

    struct Counts {
        size_t children;
        size_t roots;
        size_t codeBlocks;
        size_t pendingImports;
    };
    Counts counts() const;

    UCBStatistics& statistics() LIFETIME_BOUND; // VM thread only (section 6.5)

private:
    mutable Lock m_lock;
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, ChildIdentity> m_children WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, RootIdentity> m_roots WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedCodeBlock*, UCBRecord> m_codeBlocks WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashSet<SourceID> m_verifiedSuppliedDigests WTF_GUARDED_BY_LOCK(m_lock);
    UCBStatistics m_statistics; // not under m_lock: only the VM thread's engine and readers touch it
};

} // namespace JSC::JITCache
