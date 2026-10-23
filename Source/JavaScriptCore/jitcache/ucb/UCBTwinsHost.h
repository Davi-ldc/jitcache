#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JSCJSValue.h"

// The UCB lane's part of what hosts call in twins builds: the UCB statistics both hosts register as
// jitcacheUCBStatistics, and the module-analysis twin Bun's loader calls. harness/HostFunctions.cpp exports them under
// the names JITCacheTwinsHost.h gives them, since a lane includes no harness header.

namespace JSC {

class JSModuleRecord;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// jitcacheUCBStatistics(options): every counter of UCBStatistics by name, the indexed ones as objects keyed by their
// enumerators' names, the registry's counts() as `counts`, and, for { verifyRegistry: true }, registryViolations, the
// count verifyRegistry returns after its full collection, reported through the twin report when one is open. A VM start
// never configured has no registry and counts zeros.
JSC_DECLARE_HOST_FUNCTION(functionUCBStatistics);

// For a module record built from the module info JITCache imported and the record the analysis parse built from the
// same text: while the VM's twin report is open, reports the first field in which the two differ, if any, as a UCB
// difference named analysis, with the record's module key; without a report it does nothing.
void verifyModuleAnalysis(VM&, JSModuleRecord& fromArtifact, JSModuleRecord& fromParse);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
