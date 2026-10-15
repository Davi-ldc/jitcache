#include "config.h"
#include "JITCacheBodyEvents.h"

#if ENABLE(JITCACHE_TWINS)

#include "HeapCellInlines.h"
#include "UnlinkedCodeBlock.h"
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/Locker.h>
#include <wtf/NeverDestroyed.h>

namespace JSC::JITCache {

namespace JITCacheBodyEventsInternal {

// The retired totals of every VM of the process that has retired a count and has not reached didFinalizeHeap yet. The
// lock is a leaf: only the map's own work and malloc run under it, so a sweep on any thread may take it whatever heap
// locks that sweep holds.
class RetiredBodyEvents {
    WTF_MAKE_NONCOPYABLE(RetiredBodyEvents);
public:
    static RetiredBodyEvents& singleton()
    {
        static NeverDestroyed<RetiredBodyEvents> retired;
        return retired.get();
    }

    void add(VM& vm, const BodyEventCounts& counts)
    {
        Locker locker { m_lock };
        BodyEventCounts& totals = m_totals.add(&vm, BodyEventCounts { }).iterator->value;
        totals.llintInstructions += counts.llintInstructions;
        totals.baselineCompiles += counts.baselineCompiles;
        totals.dfgCompiles += counts.dfgCompiles;
        totals.ftlCompiles += counts.ftlCompiles;
        totals.osrExits += counts.osrExits;
        totals.jettisons += counts.jettisons;
        totals.reoptimizations += counts.reoptimizations;
    }

    BodyEventCounts totals(VM& vm)
    {
        Locker locker { m_lock };
        auto iterator = m_totals.find(&vm);
        if (iterator == m_totals.end())
            return { };
        return iterator->value;
    }

    void forget(VM& vm)
    {
        Locker locker { m_lock };
        m_totals.remove(&vm);
    }

private:
    friend class NeverDestroyed<RetiredBodyEvents>;
    RetiredBodyEvents() = default;

    Lock m_lock;
    HashMap<VM*, BodyEventCounts> m_totals WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace JITCacheBodyEventsInternal

void retireBodyEventCounts(const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    // vm() reads the cell's block header, which stays valid while the sweep that destroys the cell runs.
    JITCacheBodyEventsInternal::RetiredBodyEvents::singleton().add(unlinkedCodeBlock.vm(), unlinkedCodeBlock.jitCacheEventCounts());
}

BodyEventCounts retiredBodyEvents(VM& vm)
{
    return JITCacheBodyEventsInternal::RetiredBodyEvents::singleton().totals(vm);
}

void forgetRetiredBodyEvents(VM& vm)
{
    // Every UCB of the VM has retired its counts by now, so the entry goes before the VM's address can name another VM.
    JITCacheBodyEventsInternal::RetiredBodyEvents::singleton().forget(vm);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
