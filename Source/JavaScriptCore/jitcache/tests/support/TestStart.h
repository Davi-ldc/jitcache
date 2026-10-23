#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/JITCachePlatform.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What a test needs to start JITCache on a VM: the scope that puts the process-wide test hooks back, the configuration it
// starts with, and the checks of start's outcome and of a fault status reports.

namespace JSC::JITCache::Tests {

// The store's hooks and the build-ID override are process-wide, and the tests of a group share a process, so a test that
// may set one restores the defaults when it ends.
class ProcessHooksScope {
    WTF_MAKE_NONCOPYABLE(ProcessHooksScope);
public:
    ProcessHooksScope() = default;
    ~ProcessHooksScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
        removeMainBuildIDForTesting(false);
    }
};

inline constexpr size_t testProducerLimitBytes = 64 * MB;

// The configuration a test starts JITCache with, with testProducerLimitBytes for a producing role. The delta capture
// defaults to Compiled, so delta commits only bodies that compiled and a test counts exactly what its compilations
// commit; a test of the bodies that ran without compiling passes All.
inline Config testConfig(const String& path, Role role, DeltaCapture deltaCapture = DeltaCapture::Compiled)
{
    Config config;
    config.artifactPath = path;
    config.role = role;
    if (role != Role::Consumer)
        config.producerLimitBytes = testProducerLimitBytes;
    config.deltaCapture = deltaCapture;
    return config;
}

// Whether start returned the outcome, and the step for an outcome that names one. Otherwise the test fails with the label
// and the whole result.
inline bool startsAs(TestContext& context, ASCIILiteral label, const StartResult& result, StartOutcome outcome, ASCIILiteral step = { })
{
    if (result.outcome == outcome && result.step == step)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, name(outcome), step.isNull() ? ""_s : " at "_s, step, ", got "_s, toJSON(result)));
    return false;
}

// That the fault is there, with the class and the step. Otherwise the test fails with the label and what it got.
inline void checkFault(TestContext& context, ASCIILiteral label, const std::optional<FaultReport>& fault, FaultClass faultClass, ASCIILiteral step)
{
    if (!fault) {
        JITCACHE_FAIL(makeString(label, ": no fault, expected "_s, step));
        return;
    }
    if (fault->faultClass != faultClass || fault->stepName() != step)
        JITCACHE_FAIL(makeString(label, ": "_s, name(fault->faultClass), " at "_s, fault->stepName(), ", expected "_s, name(faultClass), " at "_s, step));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
