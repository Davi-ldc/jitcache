#include "config.h"
#include "UCBRegistry.h"

#include "UnlinkedCodeBlock.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <wtf/Locker.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC::JITCache {

namespace UCBRegistryInternal {

#if ASSERT_ENABLED || ENABLE(JITCACHE_TWINS)
// Whether the calling thread is inside a critical section of a registry's m_lock. The lock is a leaf, so a thread is inside
// at most one at a time, and nothing it runs there may destroy a pending import or drop a reference to a provider or a
// ParentIdentity. ~PendingImport asserts it, and the registry tests' stubs check it.
static thread_local bool insideCriticalSection { false };
#endif

// Brackets one critical section: declared right before the Locker, it is set before m_lock is taken and cleared after
// m_lock is released. A nested acquisition, which would deadlock on the same registry and break the leaf rule on another,
// asserts here instead.
class CriticalSectionMark {
    WTF_MAKE_NONCOPYABLE(CriticalSectionMark);

public:
    CriticalSectionMark()
    {
#if ASSERT_ENABLED || ENABLE(JITCACHE_TWINS)
        ASSERT(!insideCriticalSection);
        insideCriticalSection = true;
#endif
    }

    ~CriticalSectionMark()
    {
#if ASSERT_ENABLED || ENABLE(JITCACHE_TWINS)
        insideCriticalSection = false;
#endif
    }
};

} // namespace UCBRegistryInternal

#if ENABLE(JITCACHE_TWINS)
bool isUCBRegistryLockHeldByCurrentThread()
{
    return UCBRegistryInternal::insideCriticalSection;
}
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(UCBRegistry);

Ref<ParentIdentity> ParentIdentity::create(const BodyKey& key, RefPtr<SourceProvider>&& suppliedDigestProvider, unsigned declarationCount, std::span<const uint8_t> childHolderDigests)
{
    return adoptRef(*new ParentIdentity(key, WTF::move(suppliedDigestProvider), declarationCount, childHolderDigests));
}

ParentIdentity::ParentIdentity(const BodyKey& key, RefPtr<SourceProvider>&& suppliedDigestProvider, unsigned declarationCount, std::span<const uint8_t> childHolderDigests)
    : m_key(key)
    , m_suppliedDigestProvider(WTF::move(suppliedDigestProvider))
    , m_declarationCount(declarationCount)
    , m_childHolderDigests(childHolderDigests.size() / sizeof(Digest256))
{
    ASSERT(!(childHolderDigests.size() % sizeof(Digest256)));
    for (size_t i = 0; i < m_childHolderDigests.size(); ++i)
        memcpySpan(std::span { m_childHolderDigests[i] }, childHolderDigests.subspan(i * sizeof(Digest256), sizeof(Digest256)));
}

std::optional<Digest256> ParentIdentity::childHolderDigest(ChildTable table, uint32_t index) const
{
    size_t position = table == ChildTable::Declarations ? index : static_cast<size_t>(m_declarationCount) + index;
    if (position >= m_childHolderDigests.size())
        return std::nullopt;
    const Digest256& digest = m_childHolderDigests[position];
    if (std::ranges::all_of(digest, [](uint8_t byte) { return !byte; }))
        return std::nullopt;
    return digest;
}

const BodyKey& ParentIdentity::key() const
{
    return m_key;
}

SourceProvider* ParentIdentity::suppliedDigestProvider() const
{
    return m_suppliedDigestProvider.get();
}

Ref<PendingImport> PendingImport::create(Ref<ValidatedBody>&& body, const BodyKey& key, uint64_t indexToken, Origin origin)
{
    return adoptRef(*new PendingImport(WTF::move(body), key, indexToken, origin));
}

PendingImport::PendingImport(Ref<ValidatedBody>&& body, const BodyKey& key, uint64_t indexToken, Origin origin)
    : m_body(WTF::move(body))
    , m_key(key)
    , m_indexToken(indexToken)
    , m_origin(origin)
{
    // openBody found the body at this key, whose file the container's checks matched to it, and the index listed a body
    // there, so its token is nonzero.
    ASSERT(m_body->key() == m_key);
    ASSERT(m_indexToken);
}

PendingImport::~PendingImport()
{
    // The registry lets a pending import go only after releasing its lock.
    ASSERT(!UCBRegistryInternal::insideCriticalSection);
}

ValidatedBody& PendingImport::body() const
{
    return m_body.get();
}

const BodyKey& PendingImport::key() const
{
    return m_key;
}

uint64_t PendingImport::indexToken() const
{
    return m_indexToken;
}

auto PendingImport::origin() const -> Origin
{
    return m_origin;
}

void UCBRegistry::recordRootExecutable(const UnlinkedFunctionExecutable& executable, const RootIdentity& identity)
{
    ASSERT(identity.kind == IdentityKind::FunctionConstructor || identity.kind == IdentityKind::Builtin);
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    ASSERT(!m_children.contains(&executable));
    // A UFE keeps its first identity. The map copies the identity, taking a provider reference, only when it inserts.
    auto result = m_roots.add(&executable, identity);
    ASSERT_UNUSED(result, result.isNewEntry || (result.iterator->value.kind == identity.kind && result.iterator->value.identityDigest == identity.identityDigest));
}

std::optional<ExecutableIdentity> UCBRegistry::identityOf(const UnlinkedFunctionExecutable& executable) const
{
    // The copy takes its references under the lock, and the caller drops them after it is released.
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    if (auto iterator = m_children.find(&executable); iterator != m_children.end())
        return ExecutableIdentity { iterator->value };
    if (auto iterator = m_roots.find(&executable); iterator != m_roots.end())
        return ExecutableIdentity { iterator->value };
    return std::nullopt;
}

bool UCBRegistry::recordCodeBlock(UnlinkedCodeBlock& codeBlock, UCBRecord&& record, std::span<const uint8_t> childHolderDigests)
{
    // The tables are read, and the node the children share is made, before the lock is taken, so the critical section only
    // inserts. Both are declared before the lock, so they go after it is released when nothing takes them.
    unsigned declarationCount = codeBlock.numberOfFunctionDecls();
    unsigned expressionCount = codeBlock.numberOfFunctionExprs();
    Vector<const UnlinkedFunctionExecutable*, 16> children;
    RefPtr<ParentIdentity> parent;
    if (declarationCount || expressionCount) {
        children.reserveInitialCapacity(declarationCount + expressionCount);
        for (unsigned i = 0; i < declarationCount; ++i)
            children.append(codeBlock.functionDecl(i));
        for (unsigned i = 0; i < expressionCount; ++i)
            children.append(codeBlock.functionExpr(i));
        parent = ParentIdentity::create(record.key, RefPtr { record.suppliedDigestProvider }, declarationCount, childHolderDigests);
    }

    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    // A UCB keeps the key it was recorded under. The functor runs only when the map inserts, so a refused record stays with
    // the caller, which drops it after this returns. Boxing it mallocs under the lock, which the leaf rule allows; UCBRecord
    // is a plain struct, without the TZone allocation makeUnique requires.
    auto added = m_codeBlocks.ensure(&codeBlock, [&] { return makeUniqueWithoutFastMallocCheck<UCBRecord>(WTF::move(record)); });
    if (!added.isNewEntry)
        return false;
    if (UnlinkedFunctionExecutable* holder = added.iterator->value->holder)
        m_heldCodeBlocks.ensure(holder, [] { return Vector<const UnlinkedCodeBlock*, 1> { }; }).iterator->value.append(&codeBlock);
    for (unsigned i = 0; i < children.size(); ++i) {
        const UnlinkedFunctionExecutable* child = children[i];
        ChildTable table = i < declarationCount ? ChildTable::Declarations : ChildTable::Expressions;
        uint32_t index = i < declarationCount ? i : i - declarationCount;
        ASSERT(child);
        ASSERT(!m_roots.contains(child));
        // A child that already has an identity keeps it, and it is the identity this record derives.
        auto result = m_children.ensure(child, [&] {
            return ChildIdentity { parent, table, index };
        });
        ASSERT_UNUSED(result, result.isNewEntry || (result.iterator->value.parent->key() == parent->key() && result.iterator->value.table == table && result.iterator->value.index == index));
    }
    return true;
}

auto UCBRegistry::recordOf(const UnlinkedCodeBlock& codeBlock) const -> std::optional<RecordView>
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    auto iterator = m_codeBlocks.find(&codeBlock);
    if (iterator == m_codeBlocks.end())
        return std::nullopt;
    const UCBRecord& record = *iterator->value;
    return RecordView {
        record.key,
        record.context,
        record.origin,
        record.keyFeatures,
        record.missedBodyVersion,
        record.matchedBodyVersion,
        !!record.pendingImport,
        record.suppliedDigestProvider,
    };
}

std::optional<BodyKey> UCBRegistry::keyOf(const UnlinkedCodeBlock& codeBlock) const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    auto iterator = m_codeBlocks.find(&codeBlock);
    if (iterator == m_codeBlocks.end())
        return std::nullopt;
    return iterator->value->key;
}

void UCBRegistry::setMissedBodyVersion(const UnlinkedCodeBlock& codeBlock, uint64_t indexToken)
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    if (auto iterator = m_codeBlocks.find(&codeBlock); iterator != m_codeBlocks.end())
        iterator->value->missedBodyVersion = indexToken;
}

void UCBRegistry::setMatchedBodyVersion(const UnlinkedCodeBlock& codeBlock, uint64_t commitIdentifier)
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    if (auto iterator = m_codeBlocks.find(&codeBlock); iterator != m_codeBlocks.end())
        iterator->value->matchedBodyVersion = commitIdentifier;
}

bool UCBRegistry::attachPendingImport(const UnlinkedCodeBlock& codeBlock, Ref<PendingImport>&& import, std::optional<Vector<uint8_t>>&& generatedButterflyMap)
{
    // Read before the lock, under which no integrator code runs.
    uint64_t commitIdentifier = import->body().version();

    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    auto iterator = m_codeBlocks.find(&codeBlock);
    // A refused import stays with the caller, which drops it after this returns.
    if (iterator == m_codeBlocks.end() || iterator->value->pendingImport)
        return false;
    UCBRecord& record = *iterator->value;
    record.pendingImport = WTF::move(import);
    record.matchedBodyVersion = commitIdentifier;
    // Replaces the kept map, an empty argument included; freeing the old one is allowed here.
    record.generatedButterflyMap = WTF::move(generatedButterflyMap);
    return true;
}

bool UCBRegistry::copyGeneratedButterflyMap(const UnlinkedCodeBlock& codeBlock, std::span<uint8_t> out) const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    auto iterator = m_codeBlocks.find(&codeBlock);
    if (iterator == m_codeBlocks.end() || !iterator->value->generatedButterflyMap)
        return false;
    const Vector<uint8_t>& map = *iterator->value->generatedButterflyMap;
    // Both are ceil(N / 8) bytes for the UCB's N constants. A map of another size would not describe these
    // constants, so the capture writes the maps its own constants give instead.
    ASSERT(map.size() == out.size());
    if (map.size() != out.size())
        return false;
    memcpySpan(out, map.span());
    return true;
}

RefPtr<PendingImport> UCBRegistry::pendingImport(const UnlinkedCodeBlock& codeBlock) const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    auto iterator = m_codeBlocks.find(&codeBlock);
    if (iterator == m_codeBlocks.end())
        return nullptr;
    return iterator->value->pendingImport;
}

void UCBRegistry::resolvePendingImport(const UnlinkedCodeBlock& codeBlock, const PendingImport& import, ImportResolution resolution)
{
    // Declared before the lock, so the detached import goes after it is released.
    RefPtr<PendingImport> detached;
    {
        UCBRegistryInternal::CriticalSectionMark mark;
        Locker locker { m_lock };
        auto iterator = m_codeBlocks.find(&codeBlock);
        if (iterator == m_codeBlocks.end() || iterator->value->pendingImport.get() != &import)
            return;
        detached = WTF::move(iterator->value->pendingImport);
        // The shouldJIT gate would drop the import again, so the record skips this body until the index lists another at
        // its key.
        if (resolution == ImportResolution::DroppedByGate)
            iterator->value->missedBodyVersion = import.indexToken();
    }
    if (resolution == ImportResolution::DroppedByGate)
        ++m_statistics.gateDrops;
}

bool UCBRegistry::suppliedDigestVerified(SourceID provider) const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    return m_verifiedSuppliedDigests.contains(provider);
}

void UCBRegistry::markSuppliedDigestVerified(SourceID provider)
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    m_verifiedSuppliedDigests.add(provider);
}

size_t UCBRegistry::recordedCodeBlockCount() const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    return m_codeBlocks.size();
}

size_t UCBRegistry::copyRecordedCodeBlocks(std::span<RecordedCodeBlock> out) const
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    size_t copied = 0;
    for (auto& entry : m_codeBlocks) {
        if (copied == out.size())
            break;
        // The keys are the recorded cells themselves, which the registry only observes; delta reads them through the UCB's
        // accessors, which are non-const natively.
        out[copied++] = { const_cast<UnlinkedCodeBlock*>(entry.key), entry.value->holder };
    }
    return copied;
}

void UCBRegistry::unlinkedCodeBlockDestroyed(const UnlinkedCodeBlock* codeBlock)
{
    // Declared before the lock, so the record's pending import and provider go after it is released.
    std::unique_ptr<UCBRecord> removed;
    {
        UCBRegistryInternal::CriticalSectionMark mark;
        Locker locker { m_lock };
        removed = m_codeBlocks.take(codeBlock);
        if (removed && removed->holder) {
            auto iterator = m_heldCodeBlocks.find(removed->holder);
            ASSERT(iterator != m_heldCodeBlocks.end());
            if (iterator != m_heldCodeBlocks.end()) {
                iterator->value.removeFirst(codeBlock);
                if (iterator->value.isEmpty())
                    m_heldCodeBlocks.remove(iterator);
            }
        }
    }
}

void UCBRegistry::unlinkedFunctionExecutableDestroyed(const UnlinkedFunctionExecutable* executable)
{
    // Declared before the lock, so the identity's ParentIdentity and provider go after it is released; the
    // last child of a parent destroys the node the children shared.
    std::optional<ChildIdentity> child;
    std::optional<RootIdentity> root;
    {
        UCBRegistryInternal::CriticalSectionMark mark;
        Locker locker { m_lock };
        child = m_children.takeOptional(executable);
        root = m_roots.takeOptional(executable);
        // A UCB this UFE produced may outlive it, kept by a conservative root alone, so its record forgets the holder
        // (UCBRecord::holder). Freeing the list is allowed under the lock.
        for (const UnlinkedCodeBlock* codeBlock : m_heldCodeBlocks.take(executable)) {
            auto iterator = m_codeBlocks.find(codeBlock);
            if (iterator != m_codeBlocks.end() && iterator->value->holder == executable)
                iterator->value->holder = nullptr;
        }
    }
}

auto UCBRegistry::counts() const -> Counts
{
    UCBRegistryInternal::CriticalSectionMark mark;
    Locker locker { m_lock };
    size_t pendingImports = 0;
    for (auto& record : m_codeBlocks.values()) {
        if (record->pendingImport)
            ++pendingImports;
    }
    return Counts { m_children.size(), m_roots.size(), m_codeBlocks.size(), pendingImports };
}

UCBStatistics& UCBRegistry::statistics()
{
    return m_statistics;
}

} // namespace JSC::JITCache
