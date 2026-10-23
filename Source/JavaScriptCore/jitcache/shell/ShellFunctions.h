#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JSCJSValue.h"
#include "jitcache/session/JITCacheAPI.h"

// The shell functions only the jsc shell registers, beside the five JITCacheTwinsHost.h declares for both hosts, and
// the record of the main VM's start that two of them read.

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// Right after start returns for the shell's main VM, whatever the outcome: keeps the outcome jitcacheStartOutcome
// reports and whether jitcacheDelta logs its results to stderr.
void didStartShellVM(VM&, StartOutcome, bool logsResults);

JSC_DECLARE_HOST_FUNCTION(functionJITCacheDelta);
JSC_DECLARE_HOST_FUNCTION(functionJITCacheStartOutcome);
JSC_DECLARE_HOST_FUNCTION(functionJITCacheBodyKey);
JSC_DECLARE_HOST_FUNCTION(functionJITCacheReadSection);
JSC_DECLARE_HOST_FUNCTION(functionJITCacheRewriteSection);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
