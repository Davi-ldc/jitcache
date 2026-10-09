#pragma once

// The entry points native code calls: install, capture, teardown and the bench hooks (JITCacheBench.h: benchReport and
// RelinkTimer), and in twins builds the image twin check's hook. Native edit sites include this header, and
// JITCacheFaults.h for the executable-allocation fault.

#include "JITCacheBench.h"
#include <stdint.h>
#include <wtf/Platform.h>

namespace JSC {

class CodeBlock;
class JSScope;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The install glue (SPEC-integrator.md section 7). THREAD Restoration's two install points: before setupLLInt in
// ScriptExecutable::prepareForExecutionImpl, and at the top of JIT::compileSync for LLInt-off creation. The function does
// nothing unless the VM imports, the UCB's sharing slot is empty, the CB is newborn (JIT type None, no BaselineJITData)
// and its UCB holds a pending import; it then prepares every lane's part privately under its own GC deferral, writes to
// the CB only once nothing can fail, and calls installCode itself.
//
//   NotInstalled          the CB and the pending import untouched; native setupLLInt or compilation follows
//   Installed             the CB is baseline and installed, the import resolved; prepareForExecutionImpl skips its trailing
//                         installCode (LLInt on) or calls it a second time (LLInt off)
//   KeptForNextCodeBlock  a baked-fact mismatch: the CB untouched, the import kept for the next newborn CB of the UCB
//   DroppedByGate         shouldJIT dropped the import, which stamps its index token as missed; the CB untouched
//   Abandoned             invalid material or the image's executable-memory fault turned cache activity off; the CB
//                         untouched, and the import dies with its UCB
enum class InstallPoint : uint8_t { BeforeSetupLLInt, CompileSync };
enum class InstallOutcome : uint8_t { NotInstalled, Installed, KeptForNextCodeBlock, DroppedByGate, Abandoned };
InstallOutcome installAtNewbornCodeBlock(VM&, CodeBlock& newborn, InstallPoint);

// The finalize capture (section 8.5): BaselineJITPlan::finalize calls it at the end of its CompilationSuccessful case,
// after installCode and jitSoon. It records the compile event from the timing when one is given, whatever the VM's role,
// and then captures the CB when its VM's production is active and the CB is a candidate.
struct BaselineCompileTiming {
    uint64_t compileNanoseconds; // compileInThreadImpl after its profile drain (harness sub-SPEC section 9.3)
    uint64_t finalizeNanoseconds; // finalize up to this call, without relinking incoming calls
    bool supportGenerated; // per-VM support generated during either span
};
void didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*); // null when the plan did not measure

// Teardown (section 4.6), called from VM::~VM. willDestroyVM runs right after the cancelAllPlansForVM block, with GC
// deferred for good and no compilation of the VM running: it destroys the image twin-check state, closes the twin
// report, flushes the bench report, ends production without recording a fault and frees the production memory.
// didFinalizeHeap runs right after heap.lastChanceToFinalize(), once every UCB and UFE destructor has run: it stores
// null into the VM's pointer and destroys the state.
void willDestroyVM(VM&);
void didFinalizeHeap(VM&);

#if ENABLE(JITCACHE_TWINS)
// Harness sub-SPEC section 3: prepareForExecutionImpl calls it last. When the image twin-check state holds this CB's
// pending check and a twin report is open, it runs the Image lane's Twins::checkImage with the scope; when it holds
// another CB's, it reports skip(Image, "no-scope", ...). Either way it clears the pending check.
void didFinishPrepareForExecution(VM&, CodeBlock&, JSScope*);
#endif

} // namespace JSC::JITCache
