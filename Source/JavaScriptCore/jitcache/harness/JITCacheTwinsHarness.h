#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/harness/JITCacheTwinsHost.h"
#include <span>
#include <wtf/Forward.h>

// The harness's declarations that JITCacheTwinsHost.h does not export to Bun, in twins builds: the heap description,
// which jitcacheDescribeHeap returns (HostFunctions.cpp) and the jsc shell's adapter (shell/) writes at the end of a
// run, and writeTestFile, which only the harness's own files call. HeapDescription.cpp defines the heap description and
// Placement.cpp writeTestFile.

namespace JSC {

class JSGlobalObject;

} // namespace JSC

namespace JSC::JITCache {

// The description of the heap JavaScript can reach, which the runner compares between runs. VM thread, API lock held.
// Runs a full collection, then describes; calls no getter, trap or host accessor. It walks, under a GC deferral, the
// graph reachable from roots in order, or from the default roots when roots is empty: the global object, its global
// lexical environment and the module environment of each module record its loader holds. Given roots, it writes the
// global object, its JSGlobalProxy and its global lexical environment, unless they are among the roots, with their line
// and no edges. A lazy property is reified the way its host reifies it, which in Bun can run JavaScript. Called with no
// exception pending, it returns a null string, leaving the exception on the VM, when reifying or reading a property
// throws or a termination stops the walk.
String describeReachableHeap(JSGlobalObject&, std::span<const JSValue> roots);
// The shell's end-of-run description of the default roots, written to path. It first withdraws for good what is left of
// a termination that ended the run, which the shell has already recorded. A description that throws or a file that
// cannot be written prints the path and the error to stderr and calls exit(1), as the placement calls do.
void writeReachableHeapDescription(JSGlobalObject&, const String& path);

// Writes bytes to path, replacing the file or appending to it, for the layout files, the body-event dump and the heap
// description. These run only in test processes, so a file that cannot be opened, written or closed prints the path and
// the error to stderr and calls exit(1), and the runner fails the run on its exit code.
enum class TestFileWrite : bool { Replace, Append };
void writeTestFile(const String& path, const UTF8CString& bytes, TestFileWrite);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
