#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

// The Image lane's part of what hosts call in twins builds: the names of its test hooks, which the jsc shell's
// --jitcache-test-image-hook takes.

namespace JSC::JITCache {

// Sets the Image lane's process-wide test hook from its --jitcache-test-image-hook name, relocation-pairs or
// operation-pair, and returns true; any other name leaves the hook as it was and returns false. The shell calls it
// while it parses its command line, before JSC::initialize.
bool setImageTestHookNamed(const char*);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
