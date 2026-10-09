#include "config.h"
#include "JITCacheFaults.h"

#include "JITCacheVMState.h"
#include "VM.h"

namespace JSC::JITCache {

namespace JITCacheFaultsInternal {

// The step each site names (SPEC-integrator.md section 4.5).
static ASCIILiteral executableAllocationStep(ExecutableAllocationSite site)
{
    switch (site) {
    case ExecutableAllocationSite::BaselinePlan:
        return "exec-alloc.baseline-plan"_s;
    case ExecutableAllocationSite::DFGPlan:
        return "exec-alloc.dfg-plan"_s;
    case ExecutableAllocationSite::FTLPlan:
        return "exec-alloc.ftl-plan"_s;
    case ExecutableAllocationSite::InlineCacheHandler:
        return "exec-alloc.ic-handler"_s;
    case ExecutableAllocationSite::MathICSnippet:
        return "exec-alloc.mathic-snippet"_s;
    case ExecutableAllocationSite::JITCacheImage:
        return "exec-alloc.jitcache-image"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

} // namespace JITCacheFaultsInternal

void didFailExecutableAllocation(VM& vm, ExecutableAllocationSite site)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    ASSERT(vm.currentThreadIsHoldingAPILock());
    // The report holds two literals and a null detail, so recording it allocates nothing, and the call is safe with any
    // JSC lock held, CodeBlock::m_lock under a GCSafeConcurrentJSLocker included. Production memory is released at the
    // next glue entry, not here (section 4.2).
    state->turnActivityOff(FaultReport { FaultClass::ExecutableMemory, { }, JITCacheFaultsInternal::executableAllocationStep(site), { } });
}

void didAttachDebugger(VM& vm)
{
    if (VMState* state = vm.jitCacheState())
        state->noteDebuggerAttached();
}

} // namespace JSC::JITCache
