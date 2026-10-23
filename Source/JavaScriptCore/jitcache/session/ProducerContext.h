#pragma once

#include "jitcache/base/ProducerBudget.h"
#include <wtf/Ref.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// What a recording compilation charges against: the state's one context, which lives until the state is destroyed.
class ProducerContext {
public:
    explicit ProducerContext(Ref<ProducerBudget>&&);
    Ref<ProducerBudget> budget() const;

private:
    const Ref<ProducerBudget> m_budget;
};

// Any thread, no lock. Loads vm.jitCacheState() with acquire order and returns its producerContextIfActive(): non-null
// exactly while the role produces, cache activity is on and production has not ended, the same context each time, and
// null for good from then on. The object lives until the state is destroyed, after cancelAllPlansForVM has stopped every
// compilation of the VM, so a JIT worker that obtained it may use it for its whole compilation.
ProducerContext* producerContext(VM&);

} // namespace JSC::JITCache
