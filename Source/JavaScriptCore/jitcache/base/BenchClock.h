#pragma once

#include <algorithm>
#include <stdint.h>

namespace JSC::JITCache {

// Any thread. The thread CPU clock the bench events read: CLOCK_THREAD_CPUTIME_ID, in nanoseconds. The vDSO does not
// serve it on Linux, so each read is a system call, and about one read's cost lands inside any span two reads bound.
uint64_t benchThreadCPUNanoseconds();

// Any thread. CLOCK_MONOTONIC in nanoseconds, which the vDSO serves: the wall clock of the spans the installation bound
// reads, kept in integers so a timer does no conversion after a read.
uint64_t benchMonotonicNanoseconds();

// A span the installation bound reads, timed on the thread that runs it: the CLOCK_MONOTONIC time between its two ends,
// capped at the thread CPU time between reads just outside them. Neither system call lands inside the span, and a thread
// that loses its CPU inside it counts no more than the CPU time between those outer reads.
class BoundSpanTimer {
public:
    void start()
    {
        m_cpuStart = benchThreadCPUNanoseconds();
        m_wallStart = benchMonotonicNanoseconds();
    }

    struct Times {
        uint64_t wall; // between the span's ends
        uint64_t cpu; // between the reads just outside them
        uint64_t cpuEnd; // the last read, where a measure that follows the span starts

        // The span's time without excludedWall, time the same clock measured inside it that the span leaves out.
        uint64_t nanoseconds(uint64_t excludedWall = 0) const { return std::min(wall - excludedWall, cpu); }
    };

    // Closes the span: the wall clock, then the CPU clock.
    Times stop() const
    {
        uint64_t wallEnd = benchMonotonicNanoseconds();
        uint64_t cpuEnd = benchThreadCPUNanoseconds();
        return { wallEnd - m_wallStart, cpuEnd - m_cpuStart, cpuEnd };
    }

private:
    uint64_t m_cpuStart { 0 };
    uint64_t m_wallStart { 0 };
};

} // namespace JSC::JITCache
