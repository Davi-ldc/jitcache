#pragma once

#include "jitcache/base/BenchClock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/image/ImagePrepare.h"
#include <array>
#include <optional>
#include <stdint.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Noncopyable.h>
#include <wtf/Platform.h>

// The install event, the bench's account of one install: the measurement the install takes while a bench report is open
// and what the event reports beside it. InstallEvents.cpp writes the event.

#if ENABLE(JIT)

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

class BenchReport;

// The install's steps whose wall time the install event reports, each lane's call apart.
enum class InstallStep : uint8_t {
    ImageParse,
    BakedFacts,
    Gate,
    PrepareImage,
    CBPrepare,
    PrepareBaselineICs,
    SeedLinkedState,
    SeedCallLinkHistory,
    Commit,
    Setup,
    AttachPropertyICState,
    FinishCounter,
    InstallCode,
};
constexpr unsigned numberOfInstallSteps = static_cast<unsigned>(InstallStep::InstallCode) + 1;

// The install event's measurements, taken only while a bench report is open.
// The total is the whole function as the installation bound reads it (BoundSpanTimer). The steps only explain it, and
// their CLOCK_MONOTONIC reads would land inside it, so they are timed only under jitCacheBenchBreakdowns, as are the
// stages of prepareImage. Each step's time runs from the previous mark to the step's end. The support generations
// counted at the start tell, at the return, whether the install generated per-VM support, which sets the body aside from
// the installation bound, whose two sides must both run with support already generated.
class InstallMeasurement {
    WTF_MAKE_NONCOPYABLE(InstallMeasurement);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    InstallMeasurement(VM&, BenchReport*);

    BenchReport* report() const { return m_report; }
    bool timesSteps() const { return m_timesSteps; }
    bool supportGenerated() const;

    // Marks the start of the next step, or skips work that is no step of the install, such as a twin check.
    void mark()
    {
        if (m_timesSteps)
            m_mark = MonotonicTime::now();
    }

    void endStep(InstallStep step)
    {
        if (!m_timesSteps)
            return;
        MonotonicTime now = MonotonicTime::now();
        m_stepNanoseconds[static_cast<unsigned>(step)] = (now - m_mark).nanosecondsAs<uint64_t>();
        m_mark = now;
    }

    // Closes the total, once and only with a report open.
    BoundSpanTimer::Times stopTotal() const { return m_total.stop(); }
    std::optional<uint64_t> stepNanoseconds(InstallStep step) const { return m_stepNanoseconds[static_cast<unsigned>(step)]; }

private:
    VM& m_vm;
    BenchReport* const m_report;
    const bool m_timesSteps;
    const uint64_t m_supportGenerationsAtStart;
    BoundSpanTimer m_total;
    MonotonicTime m_mark;
    std::array<std::optional<uint64_t>, numberOfInstallSteps> m_stepNanoseconds { }; // empty for a step that did not run
};

// What the install event reports beside the times. codeSize is the installed
// BaselineJITCode's JITCode::size(), its executable allocation, which the compile event of a native compilation reports
// the same way, although native code and the image can differ in size. imageCodeSize, the image's linked size, is what
// installationBoundSmallestBody reads.
struct InstalledBody {
    BodyKey key;
    CBCounterRestore restore;
    uint64_t codeSize;
    uint32_t imageCodeSize;
    uint32_t imageFixupCount;
    PrepareTimings prepareTimings; // the stages of prepareImage, timed with the steps
};

void recordInstallEvent(const InstallMeasurement&, const InstalledBody&, uint64_t totalNanoseconds, uint64_t bodyReleaseNanoseconds, bool supportGenerated);

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
