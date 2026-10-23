#pragma once

#include "JSExportMacros.h"
#include "NativeFunction.h"
#include <optional>
#include <span>
#include <wtf/Forward.h>
#include <wtf/text/ASCIILiteral.h>

// The jsc shell's JITCache, which jsc.cpp calls at the points of its run: JITCache's flags and their usage lines,
// maintenance before JSC::initialize, the main VM's start and the end of its run, the shell functions every realm
// registers and, in twins builds, address-space placement around JSC::initialize. Every build has these calls; the work
// only twins builds do is empty elsewhere. jscmain parses the flags on its main thread before JSC::initialize, before
// any other thread exists, and everything after that only reads them.

namespace JSC {

class JSGlobalObject;
class VM;

} // namespace JSC

namespace JSC::JITCache::Shell {

// Whether arg is one of JITCache's flags. A malformed value prints the error the shell prints for an invalid option and
// sets hasBadOptions, as a bad JSC option does. The last occurrence of a flag wins.
JS_EXPORT_PRIVATE bool parseArgument(const char* arg, bool& hasBadOptions);

// The flags' lines of the shell's usage statement, and the blank line that ends them.
JS_EXPORT_PRIVATE void printUsage();

// Under --jitcache-maintenance, runs JITCache maintenance on the arguments after -- and returns its exit code, which
// the shell exits with; nothing otherwise. Maintenance needs no VM, so the shell calls this before JSC::initialize.
JS_EXPORT_PRIVATE std::optional<int> runMaintenanceIfRequested(const Vector<String>& arguments);

// Right before and right after JSC::initialize.
JS_EXPORT_PRIVATE void willInitialize();
JS_EXPORT_PRIVATE void didInitialize();

// Right after VM::create for the main VM, before its first GlobalObject: starts its JITCache under --jitcache. The
// $262.agent workers' VMs are not configured.
JS_EXPORT_PRIVATE void didCreateMainVM(VM&);

// The end of the main VM's iteration, once its run loop has returned or it ended on a termination, with the iteration's
// GlobalObject. A script that calls quit() exits before this.
JS_EXPORT_PRIVATE void didFinishMainIteration(VM&, JSGlobalObject&);

// A shell function: the global property every realm of the shell registers it as, and its length.
struct ShellFunction {
    ASCIILiteral name;
    NativeFunction::Ptr function;
    unsigned length;
};

// The shell functions, in the order every realm registers them. Only twins builds have any, and a shell given
// --jitcache-test-no-shell-functions registers none.
JS_EXPORT_PRIVATE std::span<const ShellFunction> functions();

} // namespace JSC::JITCache::Shell
