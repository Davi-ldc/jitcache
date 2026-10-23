#pragma once

#include <atomic>
#include <stddef.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC::JITCache {

// The producer working-memory limit, against which production charges its working memory. tryCharge returns false at
// once once the budget has refused; otherwise it adds the bytes to the charged total with a compare-and-swap loop, and a
// sum that overflows or exceeds the limit sets the sticky refused flag and returns false without changing the total; a
// success raises the peak. release subtracts and asserts that the total stays at or above zero. Every member uses
// atomics, so a charge, a release and a read may run on any thread, and a record destroyed after its VM's session
// releases into a budget it still references.
class ProducerBudget final : public ThreadSafeRefCounted<ProducerBudget> {
    WTF_MAKE_TZONE_ALLOCATED(ProducerBudget);
public:
    static Ref<ProducerBudget> create(size_t limitBytes);
    static Ref<ProducerBudget> createUnlimited(); // twin compiles; never refuses
    [[nodiscard]] bool tryCharge(size_t bytes); // any thread
    void release(size_t bytes); // any thread
    bool hasRefused() const; // any thread
    void refuseFurtherCharges(); // VM thread, when production ends
    size_t limitBytes() const;
    size_t chargedBytes() const;
    size_t peakBytes() const;

private:
    ProducerBudget(size_t limitBytes, bool isUnlimited);

    const size_t m_limitBytes; // SIZE_MAX for an unlimited budget
    const bool m_isUnlimited; // createUnlimited: no charge is refused, an overflowing one included
    std::atomic<size_t> m_chargedBytes { 0 };
    std::atomic<size_t> m_peakBytes { 0 };
    std::atomic<bool> m_hasRefused { false };
};

} // namespace JSC::JITCache
