#include "config.h"
#include "ProducerBudget.h"

#include "JITCacheVMState.h"
#include "VM.h"
#include <limits>
#include <wtf/CheckedArithmetic.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ProducerBudget);

ProducerBudget::ProducerBudget(size_t limitBytes, bool isUnlimited)
    : m_limitBytes(limitBytes)
    , m_isUnlimited(isUnlimited)
{
}

Ref<ProducerBudget> ProducerBudget::create(size_t limitBytes)
{
    return adoptRef(*new ProducerBudget(limitBytes, false));
}

Ref<ProducerBudget> ProducerBudget::createUnlimited()
{
    return adoptRef(*new ProducerBudget(std::numeric_limits<size_t>::max(), true));
}

bool ProducerBudget::tryCharge(size_t bytes)
{
    if (m_hasRefused.load())
        return false;

    size_t current = m_chargedBytes.load();
    size_t next;
    do {
        CheckedSize sum = current;
        sum += bytes;
        if (sum.hasOverflowed() || sum.value() > m_limitBytes) {
            if (!m_isUnlimited) {
                // The refusal is sticky (THREAD Failures): every later charge fails at the test above.
                m_hasRefused.store(true);
                return false;
            }
            // An unlimited budget never refuses; a total past SIZE_MAX, which no process can allocate, saturates.
            next = std::numeric_limits<size_t>::max();
        } else
            next = sum.value();
    } while (!m_chargedBytes.compare_exchange_weak(current, next));

    // Every total is reached by a charge, so raising the peak here makes it the maximum total.
    size_t peak = m_peakBytes.load();
    while (peak < next && !m_peakBytes.compare_exchange_weak(peak, next)) { }
    return true;
}

void ProducerBudget::release(size_t bytes)
{
    size_t previous = m_chargedBytes.fetch_sub(bytes);
    ASSERT_UNUSED(previous, previous >= bytes);
}

bool ProducerBudget::hasRefused() const
{
    return m_hasRefused.load();
}

void ProducerBudget::refuseFurtherCharges()
{
    // Production ending refuses the budget's later charges; the budget of twin compiles has no production to end.
    if (m_isUnlimited)
        return;
    m_hasRefused.store(true);
}

size_t ProducerBudget::limitBytes() const
{
    return m_limitBytes;
}

size_t ProducerBudget::chargedBytes() const
{
    return m_chargedBytes.load();
}

size_t ProducerBudget::peakBytes() const
{
    return m_peakBytes.load();
}

ProducerContext::ProducerContext(Ref<ProducerBudget>&& budget)
    : m_budget(WTF::move(budget))
{
}

Ref<ProducerBudget> ProducerContext::budget() const
{
    return m_budget.copyRef();
}

ProducerContext* producerContext(VM& vm)
{
    VMState* state = vm.jitCacheState();
    return state ? state->producerContextIfActive() : nullptr;
}

} // namespace JSC::JITCache
