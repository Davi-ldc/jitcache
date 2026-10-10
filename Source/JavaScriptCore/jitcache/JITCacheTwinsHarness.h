#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheAPI.h"
#include "JITCacheTwinsHost.h"
#include <span>
#include <wtf/Forward.h>

namespace JSC {

class JSGlobalObject;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// What only the jsc shell calls in twins builds (SPEC-integrator.harness.md sections 5, 6 and 11.5), beside the placement
// calls, host functions and body-event dump that JITCacheTwinsHost.h declares for both hosts. JITCacheTwinsHarness.cpp
// defines everything both headers declare.

// Section 11.5: forced blinding, which MacroAssembler::shouldConsiderBlinding reads to blind every immediate the assembler
// considers for blinding. The shell sets it from --jitcache-test-force-blinding while it parses its command line, before
// JSC::initialize and before any thread emits code, so the threads that emit read it without a lock.
JS_EXPORT_PRIVATE void setForcesBlindingForTesting(bool);
JS_EXPORT_PRIVATE bool forcesBlindingForTesting();

// SPEC-image.md section 11.3: sets the Image lane's process-wide test hook from its --jitcache-test-image-hook name,
// relocation-pairs, operation-pair, change-recorded-target or skip-patch, and returns true; any other name leaves the hook
// as it was and returns false. The shell calls it while it parses its command line, before JSC::initialize.
JS_EXPORT_PRIVATE bool setImageTestHookNamed(const char*);

// The shell's flags of section 5.1 that set the store's and the writer's hooks and H1's test entry. Each parses its flag's
// value and returns false when it is malformed, which the shell reports as it does a bad JSC option; a later call replaces
// what an earlier one set, so the last occurrence of a flag wins.
JS_EXPORT_PRIVATE bool setStoreFaultFlag(const char*); // <call>:<error>[@<n>]; sets the store's process-wide hook at once
JS_EXPORT_PRIVATE bool setWriterFaultFlag(const char*); // <check>@<n>, kept for didStartShellVM
JS_EXPORT_PRIVATE bool setKillFlag(const char*); // <point>@<n>, a point of section 12's table, kept for didStartShellVM
JS_EXPORT_PRIVATE bool setTwinEntryFlag(const char*); // difference, skip or coincidence:<domain>, kept for didStartShellVM

// runJSC calls it right after start returns for the shell's main VM, whatever the outcome. It keeps the outcome that
// jitcacheStartOutcome reports and whether jitcacheDelta logs its results to stderr, hands the kept writer fault and kill
// point to the VM's writer when the VM has one, and writes H1's test entry when the VM's twin report is open.
JS_EXPORT_PRIVATE void didStartShellVM(VM&, StartOutcome, bool logsResults);

// Section 6: the description of the heap JavaScript can reach. VM thread, API lock held. Runs a full collection, then
// describes; calls no getter, trap or host accessor (section 6.2). It walks, under a GC deferral, the graph reachable from
// roots in order, or from the default roots of section 6.1 when roots is empty. Given roots, it writes the global object,
// its JSGlobalProxy and its global lexical environment, unless they are among the roots, with their line and no edges. A
// lazy property is reified the way its host reifies it, which in Bun can run JavaScript (section 6.1). Called with no
// exception pending, it returns a null string, leaving the exception on the VM, when reifying or reading a property
// throws or a termination stops the walk.
JS_EXPORT_PRIVATE String describeReachableHeap(JSGlobalObject&, std::span<const JSValue> roots);
// The shell's end-of-run description of the default roots (SPEC-integrator.md section 11.1), written to path. It first
// withdraws for good what is left of a termination that ended the run, which the shell has already recorded. A
// description that throws or a file that cannot be written prints the path and the error to stderr and calls exit(1), as
// the functions of section 4 do.
JS_EXPORT_PRIVATE void writeReachableHeapDescription(JSGlobalObject&, const String& path);

// Section 5.2's shell functions that JITCacheTwinsHost.h does not declare. jitcacheICsSnapshot is the ICs lane's
// ICs::functionSnapshotBaselineICs, which the shell registers itself.
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheDelta);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheStartOutcome);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheBodyKey);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheReadSection);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheRewriteSection);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
