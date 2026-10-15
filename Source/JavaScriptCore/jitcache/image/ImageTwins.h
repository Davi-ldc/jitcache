#pragma once

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include <stddef.h>
#include <stdint.h>

// The image twin check: after an import installs restored code, test builds compile the body again in the consumer
// from the producer's twin data (ImageTwinsSection.h) and compare the two. This header also holds the registry that
// lets CodeBlock::~CodeBlock tell a twin apart, and the test hooks the harness sets. Only ENABLE(JITCACHE_TWINS) builds
// have any of it.

namespace JSC {

class BaselineJITCode;
class CodeBlock;
class JSScope;
class VM;

namespace JITCache {

class ImageSectionsView;
class TwinReport;

// The image twin check. The integrator owns one per VM and calls checkImage at the end of
// ScriptExecutable::prepareForExecutionImpl, after installCode and before JS runs, for each installed import. It holds no
// state: each twin dies with its check.
class Twins {
public:
    // Reports every difference, every skip and each equal relocation pair to the report. On the VM thread with the API
    // lock; the payload the view reads stays alive until it returns. The view carries the parsed twins section.
    void checkImage(VM&, CodeBlock& installed, JSScope*, const BaselineJITCode& restored, const ImageSectionsView&, TwinReport&);
};

// The process-wide registry of live twin CBs, under its own lock, so that CodeBlock::~CodeBlock can tell a twin apart on
// whichever thread sweeps; the set and its lock are never destroyed, since that destructor runs during process exit too.
void rememberImageTwin(CodeBlock&);
bool forgetImageTwin(CodeBlock&); // Erases the CB and returns whether it was a twin; CodeBlock::~CodeBlock calls it first.
size_t imageTwinCountForTesting(); // How many twins are alive, so tests can check that each check frees its twin.

// The test hooks, one per process: set before the process's first VM, read-only afterward.
enum class ImageTestHook : uint8_t {
    None,
    RelocationPairs, // checkImage forces one equal relocation pair in each domain the body's fixups reach
    OperationPair, // checkImage forces one for the body's first Operation fixup, which the relocation check skips
    ChangeRecordedTarget, // capture changes the target of the record's first fixup before its reach check runs
    SkipPatch, // prepareImage skips its first patch
};
void setImageTestHook(ImageTestHook);
ImageTestHook imageTestHook();

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
