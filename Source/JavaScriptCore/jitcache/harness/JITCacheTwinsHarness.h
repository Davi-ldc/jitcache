#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheAPI.h"
#include "JITCacheTwinsHost.h"
#include <span>
#include <wtf/Forward.h>

// What only the jsc shell calls in twins builds, beside the placement calls, host functions and body-event dump that
// JITCacheTwinsHost.h declares for both hosts, and writeTestFile, which the harness's own files share. Placement.cpp
// defines the placement calls and writeTestFile, HeapDescription.cpp the heap description, and ShellFunctions.cpp the
// flags, the shell's start, the body-event dump and the host functions.

namespace JSC {

class JSGlobalObject;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// Forced blinding, which MacroAssembler::shouldConsiderBlinding reads through forcesBlindingForTesting (MacroAssembler.h)
// to blind every immediate the assembler considers for blinding. The shell sets it from --jitcache-test-force-blinding
// while it parses its command line, before JSC::initialize and before any thread emits code, so the threads that emit
// read it without a lock. Only runs with JITCache off take it: in a producer it would blind literals that a consumer's
// image twin, which replays the producer's draws without the flag, emits plain.
JS_EXPORT_PRIVATE void setForcesBlindingForTesting(bool);

// Sets the Image lane's process-wide test hook from its --jitcache-test-image-hook name, relocation-pairs, operation-pair,
// change-recorded-target or skip-patch, and returns true; any other name leaves the hook as it was and returns false. The
// shell calls it while it parses its command line, before JSC::initialize.
JS_EXPORT_PRIVATE bool setImageTestHookNamed(const char*);

// The shell's test flags that set the store's and the writer's hooks and the twin report's test entry. Each parses its
// flag's value and returns false when it is malformed, which the shell reports as it does a bad JSC option; a later call
// replaces what an earlier one set, so the last occurrence of a flag wins.
JS_EXPORT_PRIVATE bool setStoreFaultFlag(const char*); // <call>:<error>[@<n>]; sets the store's process-wide hook at once
JS_EXPORT_PRIVATE bool setWriterFaultFlag(const char*); // <check>@<n>, kept for didStartShellVM
JS_EXPORT_PRIVATE bool setKillFlag(const char*); // <point>@<n>, a kill point the writer names, kept for didStartShellVM
JS_EXPORT_PRIVATE bool setTwinEntryFlag(const char*); // difference, skip or coincidence:<domain>, kept for didStartShellVM

// runJSC calls it right after start returns for the shell's main VM, whatever the outcome. It keeps the outcome that
// jitcacheStartOutcome reports and whether jitcacheDelta logs its results to stderr, hands the kept writer fault and kill
// point to the VM's writer when the VM has one, and writes the test entry when the VM's twin report is open.
JS_EXPORT_PRIVATE void didStartShellVM(VM&, StartOutcome, bool logsResults);

// The description of the heap JavaScript can reach, which the runner compares between runs. VM thread, API lock held.
// Runs a full collection, then describes; calls no getter, trap or host accessor. It walks, under a GC deferral, the
// graph reachable from roots in order, or from the default roots when roots is empty: the global object, its global
// lexical environment and the module environment of each module record its loader holds. Given roots, it writes the
// global object, its JSGlobalProxy and its global lexical environment, unless they are among the roots, with their line
// and no edges. A lazy property is reified the way its host reifies it, which in Bun can run JavaScript. Called with no
// exception pending, it returns a null string, leaving the exception on the VM, when reifying or reading a property
// throws or a termination stops the walk.
JS_EXPORT_PRIVATE String describeReachableHeap(JSGlobalObject&, std::span<const JSValue> roots);
// The shell's end-of-run description of the default roots, written to path. It first withdraws for good what is left of
// a termination that ended the run, which the shell has already recorded. A description that throws or a file that
// cannot be written prints the path and the error to stderr and calls exit(1), as the placement calls do.
JS_EXPORT_PRIVATE void writeReachableHeapDescription(JSGlobalObject&, const String& path);

// The shell functions JITCacheTwinsHost.h does not declare, which only the jsc shell registers.
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheDelta);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheStartOutcome);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheBodyKey);
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheReadSection);
#if ENABLE(JIT)
// jitcacheICsSnapshot(fn, kind), the ICs lane's ICs::functionSnapshotBaselineICs, which the shell reaches through the
// harness so that it includes no lane header.
JS_EXPORT_PRIVATE JSC_DECLARE_HOST_FUNCTION(functionJITCacheICsSnapshot);
#endif

// Writes bytes to path, replacing the file or appending to it, for the layout files, the body-event dump and the heap
// description. These run only in test processes, so a file that cannot be opened, written or closed prints the path and
// the error to stderr and calls exit(1), and the runner fails the run on its exit code.
enum class TestFileWrite : bool { Replace, Append };
void writeTestFile(const String& path, const UTF8CString& bytes, TestFileWrite);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
