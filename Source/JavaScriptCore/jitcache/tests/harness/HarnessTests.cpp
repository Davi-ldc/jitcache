#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "Options.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <stdlib.h>
#include <wtf/text/StringView.h>

// Tests of testjitcache's own harness: the option groups its tests run under and how it reports a failing check.

namespace JSC::JITCache::Tests {

// A group's options hold their values once JSC::initialize returns. The runner runs this test as
// testjitcache --group="--useConcurrentJIT=false --numberOfGCMarkers=1", and the runner's self-test checks that --list
// names it with exactly that string.
JITCACHE_TEST_WITH_OPTIONS(integratorHarnessReadsGroupOptions, No, "--useConcurrentJIT=false --numberOfGCMarkers=1")
{
    JITCACHE_CHECK(!Options::useConcurrentJIT());
    JITCACHE_CHECK(Options::numberOfGCMarkers() == 1);
}

// testjitcache sets a group's options before notifyOptionsChanged derives the dependent ones, so a group that turns the
// JIT off also has the baseline JIT off.
JITCACHE_TEST_WITH_OPTIONS(integratorHarnessDerivesDependentOptions, No, "--useJIT=false")
{
    JITCACHE_CHECK(!Options::useJIT());
    JITCACHE_CHECK(!Options::useBaselineJIT());
}

// A failing check prints its file, line and message, and the process exits with 1. The check fails only when the
// runner's self-test names this test in JITCACHE_TEST_FAIL_ON_PURPOSE, so every other run passes it.
JITCACHE_TEST(integratorHarnessReportsAFailingCheck, No)
{
    const char* purpose = getenv("JITCACHE_TEST_FAIL_ON_PURPOSE");
    bool failsOnPurpose = purpose && StringView::fromLatin1(purpose) == "integratorHarnessReportsAFailingCheck"_s;
    JITCACHE_CHECK(!failsOnPurpose);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
