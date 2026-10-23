#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

// The VMs' retired body-event totals. A UCB counts what this process did with its body, and a dying UCB adds its counts to
// its VM's totals, so the body-event dump's total keeps the counts of UCBs that died before it. A UCB dies on whichever
// thread sweeps its VM's heap, so the totals live in a process-wide map from VM to counts under a leaf lock.

namespace JSC {

class UnlinkedCodeBlock;
class VM;

namespace JITCache {

struct BodyEventCounts; // bytecode/UnlinkedCodeBlock.h

} // namespace JITCache

} // namespace JSC

namespace JSC::JITCache {

void retireBodyEventCounts(const UnlinkedCodeBlock&); // the thread sweeping the UCB's VM: adds its counts to the VM's totals
BodyEventCounts retiredBodyEvents(VM&); // VM thread: the totals so far
void forgetRetiredBodyEvents(VM&); // didFinalizeHeap

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
