#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "VM.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include "jitcache/tests/support/TestStart.h"
#include "jitcache/tests/support/TestVM.h"
#include "jitcache/ucb/UCBTwins.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What a UCB test that starts JITCache does around its body: it starts JITCache on its VM in the role it needs, and it
// ends with the registry check.

namespace JSC::JITCache::Tests {

// Starts JITCache on the test's VM in `role`, with the delta capture All, on an artifact in `directory`, with a twin report
// at `twinReportPath` and a bench report at `benchReportPath` unless each is null. A Consumer and a ConsumerProducer open
// an artifact that exists, so for them a Producer on a VM of its own creates one first, and that VM goes before the test's
// VM starts, releasing the producer lock a ConsumerProducer takes. Fails the test unless every start returns Started.
inline bool startJITCache(TestContext& context, VM& vm, const TemporaryDirectory& directory, Role role, const String& twinReportPath = { }, const String& benchReportPath = { })
{
    String artifact = directory.path("artifact"_s);
    if (role != Role::Producer) {
        ExtraVM creator;
        if (!startsAs(context, "the Producer that creates the artifact"_s, JITCache::start(creator.vm(), testConfig(artifact, Role::Producer, DeltaCapture::All)), StartOutcome::Started))
            return false;
    }
    Config config = testConfig(artifact, role, DeltaCapture::All);
    config.twinReportPath = twinReportPath;
    config.benchReportPath = benchReportPath;
    return startsAs(context, "the test's VM"_s, JITCache::start(vm, config), StartOutcome::Started);
}

// How a test that started JITCache ends: after a full collection, the registry holds no entry of a dead cell, and each
// child UFE of a recorded UCB has the identity its parent's record gives it. verifyRegistry also reports each violation to
// the VM's twin report when one is open.
inline void checkRegistry(TestContext& context, VM& vm)
{
    VMState* state = vm.jitCacheState();
    unsigned violations = verifyRegistry(vm, state ? state->twinReportSink() : nullptr);
    if (violations)
        JITCACHE_FAIL(makeString("verifyRegistry found "_s, violations, " violations"_s));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
