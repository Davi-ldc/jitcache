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
