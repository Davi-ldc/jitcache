#pragma once

#include "ParserModes.h"
#include "SourceID.h"
#include "SourceProvider.h"
#include "UCBKeys.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC {

class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;

} // namespace JSC

namespace JSC::JITCache {

// The parent-key registry, pending imports and statistics.

class ValidatedBody; // the integrator's handle on a validated body file

// Statistics.
enum class MissReason : uint8_t { NoKey, NoBody, BodyUnchanged, RequestKey, Context, Provenance, Holder, AtomMap, CoreDigest, Stack };
enum class InvalidMaterialStep : uint8_t { Identity, Feedback, Decode, Closure, StrictCore, SuppliedDigest }; // ucb.identity ... ucb.supplied-digest

// What produced the UCB in this VM. A capture of a Decoded one writes provenance EmbedderDecoded unless its record keeps
// generatedButterflyMap.
enum class UCBOrigin : uint8_t { Generated, Decoded, Imported };
enum class ImportResolution : uint8_t { Installed, DroppedByGate };

struct UCBStatistics {
    uint64_t imports { 0 }; // importBody succeeded
    uint64_t seededDecodes { 0 }; // seedDecoded seeded
    uint64_t attaches { 0 }; // attachLive attached
    uint64_t gateDrops { 0 }; // pending imports the shouldJIT gate dropped
    uint64_t sourceDigests { 0 }; // root source digests computed from text; a direct eval's is not counted
    uint64_t suppliedSourceDigests { 0 }; // root digests taken from builtin metadata or a provider
    uint64_t suppliedDigestVerifications { 0 }; // supplied digests an import verified
    uint64_t contextDigests { 0 }; // context digests computed, by requests and captures
    uint64_t holderDigests { 0 }; // holder digests computed, by requests (the holder check, a root's context) and captures
    uint64_t tdzEnvironmentDigests { 0 }; // TDZ environments digested, each once while it lives
    uint64_t childHolderDigests { 0 }; // holder digests a capture computed for the child UFEs of the UCB it captured
    uint64_t vouchedHolderChecks { 0 }; // holder checks that had the digest an imported parent's body lists for the child UFE
    std::array<uint64_t, 10> misses { }; // indexed by MissReason
    std::array<uint64_t, 3> records { }; // indexed by UCBOrigin
    std::array<uint64_t, 6> invalidMaterial { }; // indexed by InvalidMaterialStep
};
static_assert(static_cast<size_t>(MissReason::Stack) + 1 == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(static_cast<size_t>(UCBOrigin::Imported) + 1 == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(static_cast<size_t>(InvalidMaterialStep::SuppliedDigest) + 1 == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

// Identities.
// suppliedDigestProvider, in each type below: the provider whose supplied digest an import at the key relies on, kept only
// in a VM that imports with strict on, so that the import can verify it; else null, and always null for a direct eval and
// everything nested in one.
// What the children of one recorded UCB share: its key and suppliedDigestProvider, kept once per parent and alive while
// any of its child UFEs has an entry. A UCB imported from a body that lists its children's holder digests also keeps them,
// declarations then expressions, which the holder check compares for the child UFEs its decode made.
class ParentIdentity final : public ThreadSafeRefCounted<ParentIdentity> {
public:
    static Ref<ParentIdentity> create(const BodyKey&, RefPtr<SourceProvider>&& suppliedDigestProvider, unsigned declarationCount = 0, std::span<const uint8_t> childHolderDigests = { });
    const BodyKey& key() const LIFETIME_BOUND;
    SourceProvider* suppliedDigestProvider() const;
    // The digest the parent's body lists for that child, or none when it lists none or a zero digest.
    std::optional<Digest256> childHolderDigest(ChildTable, uint32_t index) const;

private:
    ParentIdentity(const BodyKey&, RefPtr<SourceProvider>&&, unsigned declarationCount, std::span<const uint8_t> childHolderDigests);
    BodyKey m_key;
    RefPtr<SourceProvider> m_suppliedDigestProvider;
    unsigned m_declarationCount { 0 };
    FixedVector<Digest256> m_childHolderDigests;
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

using ExecutableIdentity = Variant<ChildIdentity, RootIdentity>;

// Pending imports. One lives in its UCB's record from the moment the request point records it until the
// install glue resolves it, and otherwise goes with the UCB. It never enters UnlinkedCodeBlock::m_unlinkedBaselineCode.
class PendingImport final : public ThreadSafeRefCounted<PendingImport> {
public:
    enum class Origin : uint8_t { Seeded, Reused }; // Seeded: the UCB's feedback came from this body before publication. Reused: a live UCB, not seeded.
    static Ref<PendingImport> create(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    ValidatedBody& body() const; // pins the validated payload
    const BodyKey& key() const;
    uint64_t indexToken() const; // the bodyVersion token the import was made at
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
    RecordedContext context; // the context's inputs, or a direct eval's digest
    UCBOrigin origin;
    LexicallyScopedFeatures keyFeatures { NoLexicallyScopedFeatures }; // a program, module or indirect eval: the strict and with-scope bits of its key
    std::optional<uint64_t> missedBodyVersion; // the index token at which no body fitted the UCB
    std::optional<uint64_t> matchedBodyVersion; // the commit identifier of the body file the UCB was imported, seeded or attached from, or matched in every check but the atom check of its marked constants
    RefPtr<PendingImport> pendingImport; // at most one
    RefPtr<SourceProvider> suppliedDigestProvider;
    // Origin Decoded, in a VM whose production is active: the butterfly map of the body of provenance Generated the UCB
    // was last seeded or attached from; ceil(N / 8) bytes.
    std::optional<Vector<uint8_t>> generatedButterflyMap;
    // Function code, in a VM whose production is active with the delta capture All: the UFE whose request produced the
    // UCB, which delta needs to capture a body that no CodeBlock holds. A conservative root can keep the UCB alive after the
    // UFE dies, so the UFE's destructor hook clears the field. Null otherwise.
    UnlinkedFunctionExecutable* holder { nullptr };
};

// One per VM, owned by the integrator's per-VM state, created when start configures the VM and destroyed after
// Heap::lastChanceToFinalize. It serves every role and is not charged to the production limit. Its maps hold raw cell
// pointers, none of which is a GC root or holds a cell.
//
// m_lock is a leaf: no other lock is taken while it is held, and inside its critical sections no code
// allocates a cell, enters or leaves a DeferGC or DeferGCForAWhile scope, runs a write barrier, calls into the collector or
// the integrator, destroys a PendingImport or drops a reference to a SourceProvider or a ParentIdentity. A destructor hook
// moves such references out under the lock and lets them go after unlocking.
class UCBRegistry {
    WTF_MAKE_NONCOPYABLE(UCBRegistry);
    WTF_MAKE_TZONE_ALLOCATED(UCBRegistry);

public:
    // WTF_MAKE_NONCOPYABLE suppresses the implicit default constructor, which the integrator's VM state uses to create its
    // registry.
    UCBRegistry() = default;

    // UFE identities. VM thread.
    void recordRootExecutable(const UnlinkedFunctionExecutable&, const RootIdentity&);
    std::optional<ExecutableIdentity> identityOf(const UnlinkedFunctionExecutable&) const;

    // UCB records. VM thread.
    // Records the UCB and gives each child UFE of its two tables a ChildIdentity, in table order, all referencing one
    // ParentIdentity made from the record's key and suppliedDigestProvider (none for a UCB without children).
    // A UCB that already has a record keeps it, so a UCB keeps the key it was recorded under; returns false then.
    // childHolderDigests: for a UCB imported from a body that lists them, that list (IdentitySection::childHolderDigests),
    // which the ParentIdentity keeps; empty otherwise.
    bool recordCodeBlock(UnlinkedCodeBlock&, UCBRecord&&, std::span<const uint8_t> childHolderDigests = { });
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
    // A miss at the atom check of the marked constants alone: the commit identifier of the file the UCB matched in every
    // other check.
    void setMatchedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // False when the UCB has no record or already has a pending import. On success the record's matchedBodyVersion becomes
    // the commit identifier of the import's body file, and its generatedButterflyMap becomes the argument.
    bool attachPendingImport(const UnlinkedCodeBlock&, Ref<PendingImport>&&, std::optional<Vector<uint8_t>>&& generatedButterflyMap);
    // For a capture: copies the record's generatedButterflyMap into `out`, whose size is ceil(N / 8), and returns true;
    // returns false, copying nothing, when the record keeps none.
    bool copyGeneratedButterflyMap(const UnlinkedCodeBlock&, std::span<uint8_t> out) const;
    RefPtr<PendingImport> pendingImport(const UnlinkedCodeBlock&) const;
    // Detaches the import if it is still the attached one. DroppedByGate also sets missedBodyVersion to the import's index
    // token and counts statistics().gateDrops.
    void resolvePendingImport(const UnlinkedCodeBlock&, const PendingImport&, ImportResolution);

    // Supplied digests an import verified, by the provider's SourceID, which no other provider ever takes. VM thread.
    bool suppliedDigestVerified(SourceID) const;
    void markSuppliedDigestVerified(SourceID);

    // Delta's walk of every recorded UCB. Under the lock the walk only copies, so the caller tests each cell
    // after it, outside the lock: a cell's liveness test takes its block's lock, which the leaf rule forbids here.
    struct RecordedCodeBlock {
        UnlinkedCodeBlock* codeBlock;
        UnlinkedFunctionExecutable* holder; // the record's, or null
    };
    // VM thread. Only the VM thread's requests and the destructor hooks change the count, and the hooks run only where the
    // VM thread sweeps, so between this call and copyRecordedCodeBlocks a caller that does neither sees the same records.
    size_t recordedCodeBlockCount() const;
    // VM thread. Copies each recorded UCB with its record's holder into `out`, at most out.size() of them, and returns how
    // many it copied.
    size_t copyRecordedCodeBlocks(std::span<RecordedCodeBlock> out) const;

    // Destructors: the thread sweeping the VM's heap.
    void unlinkedCodeBlockDestroyed(const UnlinkedCodeBlock*);
    void unlinkedFunctionExecutableDestroyed(const UnlinkedFunctionExecutable*);

    struct Counts {
        size_t children;
        size_t roots;
        size_t codeBlocks;
        size_t pendingImports;
    };
    Counts counts() const;

    UCBStatistics& statistics() LIFETIME_BOUND; // VM thread only

private:
    mutable Lock m_lock;
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, ChildIdentity> m_children WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, RootIdentity> m_roots WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashMap<const UnlinkedCodeBlock*, std::unique_ptr<UCBRecord>> m_codeBlocks WTF_GUARDED_BY_LOCK(m_lock); // boxed: WTF's HashTable refuses buckets above 150 bytes
    // The UCBs whose records name each holder (UCBRecord::holder), so that the holder's destructor hook clears those
    // records' field. A UFE holds one UCB per specialization, and an older one lives on only while a CodeBlock keeps it.
    UncheckedKeyHashMap<const UnlinkedFunctionExecutable*, Vector<const UnlinkedCodeBlock*, 1>> m_heldCodeBlocks WTF_GUARDED_BY_LOCK(m_lock);
    UncheckedKeyHashSet<SourceID> m_verifiedSuppliedDigests WTF_GUARDED_BY_LOCK(m_lock);
    UCBStatistics m_statistics; // not under m_lock: only the VM thread's engine and readers touch it
};

#if ENABLE(JITCACHE_TWINS)
// Whether the calling thread is inside a critical section of a registry's lock. The registry tests' stubs read it in their
// destructors, which the registry's destructor hooks run, to show that the registry lets pending imports, providers and
// ParentIdentity references go only after it unlocks.
bool isUCBRegistryLockHeldByCurrentThread();
#endif

} // namespace JSC::JITCache
