#pragma once

#include "jitcache/base/BodyKey.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/base/VMPart.h"
#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/store/ArtifactStore.h"
#include <optional>
#include <stdint.h>
#include <utility>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/Platform.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/WTFString.h>

// What the capture glue charges the producer budget for the production tables it owns, the faults it raises for a capture,
// and the kept summaries: the score of each key's saved body, which the scoring read of a saved body fills and each
// commit replaces. KeptSummaries.cpp defines them.

#if ENABLE(JIT)

namespace JSC::JITCache {

class CaptureClock;
class VMState;

// A hash table the glue owns for production is charged eight bucket sizes with its first entry and six more per entry,
// which bounds WTF's storage for a table that only grows, rehashes included.
constexpr size_t tableChargeBuckets = 8;
constexpr size_t entryChargeBuckets = 6;
static_assert(BodyKeyHashTraits::minimumTableSize == tableChargeBuckets);

// What a new entry costs a table whose buckets take bucketBytes: the entry's buckets, and the table's while the table holds
// no charge yet.
inline size_t newEntryCharge(size_t bucketBytes, bool tableCharged)
{
    return (tableCharged ? 0 : tableChargeBuckets * bucketBytes) + entryChargeBuckets * bucketBytes;
}

// A charge against the producer budget that the scope releases unless something takes it over. A buffer the charge pays
// for is declared after it, so the buffer is freed before the charge is released.
class ScopedCharge {
    WTF_MAKE_NONCOPYABLE(ScopedCharge);
public:
    explicit ScopedCharge(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~ScopedCharge()
    {
        if (m_bytes)
            m_budget.release(m_bytes);
    }

    [[nodiscard]] bool tryCharge(size_t bytes)
    {
        if (!m_budget.tryCharge(bytes))
            return false;
        m_bytes += bytes;
        return true;
    }

    size_t take() { return std::exchange(m_bytes, 0); }

private:
    ProducerBudget& m_budget;
    size_t m_bytes { 0 };
};

// A capture's fault detail: the body by its key.
String bodyDetail(const BodyKey&);

// Raises budget.limit, the budget's own fault, with the detail.
void raiseBudgetLimit(VMState&, String detail);

// The kept summaries: the score of each key's saved body, read once at the key's first scoring and replaced at each
// commit, so a kept summary always describes the file its key names. The table is production memory, charged by the
// rule above before each entry is added, and destroying it releases every charge it holds. It is a production part of
// the VM's state (VMPart.h), which the VM's first kept entry creates and the release of ended production memory
// destroys.
struct KeptSummaries final : VMPart {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(KeptSummaries);
    WTF_MAKE_NONCOPYABLE(KeptSummaries);

    static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilProductionEnds;

    using Map = HashMap<BodyKey, SavedScore, BodyKeyHash, BodyKeyHashTraits>;
    static constexpr size_t bucketBytes = sizeof(Map::KeyValuePairType);
    static constexpr size_t entryBytes = entryChargeBuckets * bucketBytes;

    explicit KeptSummaries(ProducerBudget& producerBudget)
        : budget(producerBudget)
    {
    }

    ~KeptSummaries() final
    {
        // The table is freed before its charge is released.
        scores.clear();
        if (chargedBytes)
            budget->release(chargedBytes);
    }

    // An entry's charge goes with the entry; the table's stays until the table goes.
    void erase(const BodyKey& key)
    {
        if (!scores.remove(key))
            return;
        chargedBytes -= entryBytes;
        budget->release(entryBytes);
    }

    const Ref<ProducerBudget> budget;
    Map scores;
    size_t chargedBytes { 0 }; // the table's eight buckets once it held an entry, and six per entry it holds
};

// Adds a key's first kept entry, whose charge (newEntryCharge) the caller made and the entry now holds, and creates the
// kept summaries at the VM's first kept entry.
void addKeptEntry(VMState&, ProducerBudget&, const BodyKey&, const SavedScore&, size_t chargedBytes);

// What a key's first kept entry would charge now.
size_t keptEntryCharge(VMState&);

// A saved body's score at one scoring.
struct SavedScoring {
    enum class Outcome : uint8_t { Scored, Absent, Deferred, Faulted };
    Outcome outcome;
    std::optional<SavedScore> saved; // Scored only
    std::optional<uint64_t> readNanoseconds; // when the scoring read ran
};

// The key's kept entry when it has one. Otherwise the store's scoring read opens the body by its name, whatever the
// index holds, and tells four cases apart: no body (Absent), a transient error that defers the key's capture
// (Unavailable), invalid material (Invalid), and a body whose summaries score it (Found), which is kept. An unreadable
// body never scores as absent, since any candidate beats an absent body.
SavedScoring scoreSavedBody(VMState&, ProducerBudget&, const BodyKey&, const CaptureClock&);

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
