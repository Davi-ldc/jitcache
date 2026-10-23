#include "config.h"
#include "jitcache/session/ProducerContext.h"

#include "VM.h"
#include "jitcache/session/JITCacheVMState.h"

namespace JSC::JITCache {

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
