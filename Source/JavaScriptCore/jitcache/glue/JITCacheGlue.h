#pragma once

#include "jitcache/session/JITCacheBench.h"
#include "jitcache/session/JITCacheTeardown.h"
#include <stdint.h>
#include <wtf/Platform.h>

// The entry points native code calls: install, capture, teardown (JITCacheTeardown.h: willDestroyVM and
// didFinalizeHeap) and the bench hooks (JITCacheBench.h: benchReport, RelinkTimer and BaselineSpanTimer), and in twins
// builds the image twin check's hook. Native edit sites include this header, and JITCacheFaults.h for the
// executable-allocation fault. Every function this header declares runs on the VM thread. BaselineInstall.cpp defines
// the install, BaselineCapture.cpp the two capture hooks and InstallTwins.cpp the image twin check's hook.

namespace JSC {

class CodeBlock;
class JSScope;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The install glue, at the two points where a newborn CB receives its first code: before setupLLInt in
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

// Whether a baseline compilation of the CB records its image: the VM's production is active and the UCB lane recorded a
// key for the CB's UCB. BaselineJITPlan's constructor calls it on the VM thread; it takes only the UCB registry's lock, a
// leaf.
bool shouldRecordBaselineImage(VM&, CodeBlock&);

// The finalize capture: BaselineJITPlan::finalize calls it at the end of its CompilationSuccessful case, after
// installCode and jitSoon. It records the compile event from the timing when one is given, whatever the VM's role, and
// then captures the CB when its VM's production is active and the CB is a candidate. The plan's two BaselineSpanTimers
// measure the timing, as the installation bound's spans are read (BoundSpanTimer).
struct BaselineCompileTiming {
    uint64_t compileNanoseconds; // compileInThreadImpl after its profile drain
    uint64_t finalizeNanoseconds; // finalize up to this call, without relinking incoming calls
    bool supportGenerated; // per-VM support generated during either span
};
void didFinalizeBaselineCompilation(VM&, CodeBlock&, const BaselineCompileTiming*); // null when the plan did not measure

#if ENABLE(JITCACHE_TWINS)
// prepareForExecutionImpl calls it last. When the image twin-check state holds this CB's pending check and a twin report
// is open, it runs the Image lane's Twins::checkImage with the scope; when it holds another CB's, it reports skip(Image,
// "no-scope", ...). Either way it clears the pending check.
void didFinishPrepareForExecution(VM&, CodeBlock&, JSScope*);
#endif

} // namespace JSC::JITCache
