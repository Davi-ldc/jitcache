#pragma once

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// Teardown, called from VM::~VM on the VM thread. willDestroyVM runs right after the cancelAllPlansForVM block, with GC
// deferred for good and no compilation of the VM running: it destroys the state's parts that live until then
// (VMPart.h), closes the twin report, flushes the bench report, ends production without recording a fault and frees the
// production memory, the production parts included.
// didFinalizeHeap runs right after heap.shutDown(), whose lastChanceToFinalize() has run every UCB and UFE destructor: it
// stores null into the VM's pointer and destroys the state.
void willDestroyVM(VM&);
void didFinalizeHeap(VM&);

} // namespace JSC::JITCache
