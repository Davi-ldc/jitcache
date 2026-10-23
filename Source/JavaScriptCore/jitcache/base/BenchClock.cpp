#include "config.h"
#include "jitcache/base/BenchClock.h"

#if OS(LINUX)
#include <time.h>
#else
#include <wtf/CPUTime.h>
#include <wtf/MonotonicTime.h>
#endif

namespace JSC::JITCache {

#if OS(LINUX)
static uint64_t readClock(clockid_t clock)
{
    struct timespec now { };
    clock_gettime(clock, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000 + static_cast<uint64_t>(now.tv_nsec);
}
#endif

// Off Linux, start rejects before any bench report opens, so nothing measures; WTF's clocks keep the definitions
// meaningful.

uint64_t benchThreadCPUNanoseconds()
{
#if OS(LINUX)
    return readClock(CLOCK_THREAD_CPUTIME_ID);
#else
    return static_cast<uint64_t>(CPUTime::forCurrentThread().nanoseconds());
#endif
}

uint64_t benchMonotonicNanoseconds()
{
#if OS(LINUX)
    return readClock(CLOCK_MONOTONIC);
#else
    return MonotonicTime::now().secondsSinceEpoch().nanosecondsAs<uint64_t>();
#endif
}

} // namespace JSC::JITCache
