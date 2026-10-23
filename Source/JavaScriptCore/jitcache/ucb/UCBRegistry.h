#pragma once

#include "ParserModes.h"
#include "Weak.h"
#include "jitcache/ucb/UCBKeys.h"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <wtf/FixedVector.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>

namespace JSC {

class JSModuleRecord;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;

} // namespace JSC

namespace JSC::JITCache {

// The parent-key registry, pending imports and statistics.

class ValidatedBody; // the integrator's handle on a validated body file

// Statistics.
enum class MissReason : uint8_t { NoKey, NoBody, BodyUnchanged, RequestKey, Context, Holder, Stack };
// The steps ucb.identity and ucb.decode.
enum class InvalidMaterialStep : uint8_t { Identity, Decode };

enum class UCBOrigin : uint8_t { Generated, Imported }; // what produced the UCB in this VM
enum class ImportResolution : uint8_t { Installed, DroppedByGate };

struct UCBStatistics {
    uint64_t imports { 0 }; // imports that returned a UCB
    uint64_t attaches { 0 }; // pending imports attached to a live UCB
    uint64_t gateDrops { 0 }; // pending imports the shouldJIT gate dropped
    uint64_t sourceDigests { 0 }; // root source digests hashed from text, at requests, root creations and analysis imports; a direct eval's is not counted
    uint64_t suppliedSourceDigests { 0 }; // root digests requests and root creations took from builtin metadata or a provider
    uint64_t contextDigests { 0 }; // context digests computed, by requests and captures
    uint64_t holderDigests { 0 }; // holder digests computed, by requests and captures
    uint64_t tdzEnvironmentDigests { 0 }; // TDZ environments digested, each once while it lives
    uint64_t childHolderDigests { 0 }; // holder digests a capture computed for the child UFEs of the UCB it captured
    uint64_t vouchedHolderChecks { 0 }; // holder checks that used the digest an imported parent's body lists for the child UFE
    // A module's analysis: imported; a key with no body; a body without the section; serialized by a capture; copied from
    // the saved body by a capture; left out by a capture as unrepresentable.
    uint64_t analysisImports { 0 };
    uint64_t analysisMisses { 0 };
    uint64_t analysisAbsent { 0 };
    uint64_t analysisCaptured { 0 };
    uint64_t analysisCopied { 0 };
    uint64_t analysisUnrepresentable { 0 };
    std::array<uint64_t, 7> misses { }; // indexed by MissReason
    std::array<uint64_t, 2> records { }; // indexed by UCBOrigin
    std::array<uint64_t, 2> invalidMaterial { }; // indexed by InvalidMaterialStep
};
static_assert(static_cast<size_t>(MissReason::Stack) + 1 == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(static_cast<size_t>(UCBOrigin::Imported) + 1 == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(static_cast<size_t>(InvalidMaterialStep::Decode) + 1 == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

// Identities.
// What the children of one recorded UCB share: its key, kept once per parent and alive while any of its child UFEs has an
// entry. A UCB imported from a body that lists its children's holder digests also keeps them, declarations then
// expressions, which the holder check compares for the child UFEs its decode made.
class ParentIdentity final : public ThreadSafeRefCounted<ParentIdentity> {
public:
    static Ref<ParentIdentity> create(const BodyKey&, unsigned declarationCount = 0, std::span<const uint8_t> childHolderDigests = { });
    const BodyKey& key() const LIFETIME_BOUND;
    // The digest the parent's body lists for that child, or none when it lists none or a zero digest.
    std::optional<Digest256> childHolderDigest(ChildTable, uint32_t index) const;

private:
    ParentIdentity(const BodyKey&, unsigned declarationCount, std::span<const uint8_t> childHolderDigests);
    BodyKey m_key;
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
};

using ExecutableIdentity = Variant<ChildIdentity, RootIdentity>;

// Pending imports. One lives in its UCB's record from the moment the request point records it until the
// install glue resolves it, and otherwise goes with the UCB. It never enters UnlinkedCodeBlock::m_unlinkedBaselineCode.
class PendingImport final : public ThreadSafeRefCounted<PendingImport> {
public:
    enum class Origin : uint8_t { Imported, Reused }; // Imported: the UCB and its feedback came from this body before publication. Reused: a live UCB, not seeded.
    static Ref<PendingImport> create(Ref<ValidatedBody>&&, const BodyKey&, uint64_t indexToken, Origin);
    ValidatedBody& body() const; // pins the body's bytes, and with them the pack's mapping
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

struct UCBRecord { // move-only, for its Weak
    BodyKey key;
    RecordedContext context; // the context's inputs, or a direct eval's digest
    UCBOrigin origin;
    LexicallyScopedFeatures keyFeatures { NoLexicallyScopedFeatures }; // a program, module or indirect eval: the strict and with-scope bits of its key
    std::optional<uint64_t> missedBodyVersion; // the index token at which no body fitted the UCB
    std::optional<uint64_t> matchedBodyVersion; // the commit identifier of the body the UCB was imported or attached from
    RefPtr<PendingImport> pendingImport; // at most one
    // Function code, in a VM whose production is active with the delta capture All (VMState::capturesUncompiledBodies):
    // the requesting UFE, which a capture without a CodeBlock encodes through; null otherwise, and once the UFE's
    // destructor hook ran. A conservative root can keep the UCB alive after the UFE dies, which is why the hook clears it.
    UnlinkedFunctionExecutable* holder { nullptr };
    // Module code, in a VM whose production is active: the record JSC's analysis parse built for the module, which a
    // capture encodes; empty otherwise, and reads as null once that record dies.
    Weak<JSModuleRecord> analysisSource;
};

// One per VM, owned by the integrator's per-VM state, created when start configures the VM and destroyed after
// Heap::lastChanceToFinalize. It serves every role and is not charged to the production limit. Its maps hold raw cell
// pointers, none of which is a GC root or holds a cell.
//
// m_lock is a leaf: no other lock is taken while it is held, and inside its critical sections no code
// allocates a cell, enters or leaves a DeferGC or DeferGCForAWhile scope, runs a write barrier, calls into the collector or
// the integrator, destroys a PendingImport, makes or destroys a module's analysis source, whose making counts as a heap
// allocation, or drops a reference to a ParentIdentity. A destructor hook moves such references and handles out under the
// lock and lets them go after unlocking.
class UCBRegistry {
    WTF_MAKE_NONCOPYABLE(UCBRegistry);
    WTF_MAKE_TZONE_ALLOCATED(UCBRegistry);

public:
    // WTF_MAKE_NONCOPYABLE suppresses the implicit default constructor, which the integrator's VM state uses to create its
    // registry.
    UCBRegistry() = default;
    // Out of line, beside the definitions the records' analysis sources need to go.
    ~UCBRegistry();

    // UFE identities. VM thread.
    void recordRootExecutable(const UnlinkedFunctionExecutable&, const RootIdentity&);
    std::optional<ExecutableIdentity> identityOf(const UnlinkedFunctionExecutable&) const;

    // UCB records. VM thread.
    // Records the UCB and gives each child UFE of its two tables (functionDeclIfDecoded, functionExpr) a ChildIdentity, in
    // table order, all referencing one ParentIdentity made from the record's key (none for a UCB without children). A UCB
    // that already has a record keeps it, and with it the key it was recorded under; returns false then.
    // childHolderDigests: an import's list (IdentitySection::childHolderDigests), which the ParentIdentity keeps; empty for
    // every other record.
    bool recordCodeBlock(UnlinkedCodeBlock&, UCBRecord&&, std::span<const uint8_t> childHolderDigests = { });
    struct RecordView {
        BodyKey key;
        RecordedContext context;
        UCBOrigin origin;
        LexicallyScopedFeatures keyFeatures;
        std::optional<uint64_t> missedBodyVersion;
        std::optional<uint64_t> matchedBodyVersion;
        bool hasPendingImport;
    };
    std::optional<RecordView> recordOf(const UnlinkedCodeBlock&) const;
    std::optional<BodyKey> keyOf(const UnlinkedCodeBlock&) const;
    void setMissedBodyVersion(const UnlinkedCodeBlock&, uint64_t);
    // False when the UCB has no record or already has a pending import. On success the record's matchedBodyVersion becomes
    // the commit identifier of the import's body.
    bool attachPendingImport(const UnlinkedCodeBlock&, Ref<PendingImport>&&);
    RefPtr<PendingImport> pendingImport(const UnlinkedCodeBlock&) const;
    // Detaches the import if it is still the attached one. DroppedByGate also sets missedBodyVersion to the import's index
    // token and counts statistics().gateDrops.
    void resolvePendingImport(const UnlinkedCodeBlock&, const PendingImport&, ImportResolution);

    // A module UCB's analysis source, the record JSC's analysis parse built, which a capture encodes. VM thread.
    // True when the UCB has a record whose source is empty or has died.
    bool needsModuleAnalysisSource(const UnlinkedCodeBlock&) const;
    // Stores the handle in the UCB's record when the record's source is empty or dead. The caller makes the handle with
    // this lock released, since making a Weak counts as a heap allocation. A handle this call does not keep, and one it
    // replaces, is destroyed after unlocking.
    void setModuleAnalysisSource(const UnlinkedCodeBlock&, Weak<JSModuleRecord>&&);
    JSModuleRecord* moduleAnalysisSource(const UnlinkedCodeBlock&) const; // the live source, or null

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
    UCBStatistics m_statistics; // not under m_lock: only the VM thread's engine and readers touch it
};

#if ENABLE(JITCACHE_TWINS)
// Whether the calling thread is inside a critical section of a registry's lock. The registry tests' stubs read it in
// their destructors, which the registry's destructor hooks run, to show that the registry lets pending imports go only
// after it unlocks.
bool isUCBRegistryLockHeldByCurrentThread();
#endif

} // namespace JSC::JITCache
