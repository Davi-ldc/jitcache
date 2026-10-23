#pragma once

#include "jitcache/base/BenchClock.h"
#include "jitcache/base/ProducerBudget.h"
#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdint.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/Platform.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Variant.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The bench report and the timers that feed it. When Config::benchReportPath is set, the VMState keeps a report, which
// appends JSON lines to that path, buffered and flushed when the buffer passes 1 MiB, after each delta, at each host's
// exit (finishHostRun) and in willDestroyVM. Every line holds event, pid and the VM's ordinal in the process, then the
// event's fields. Times are nanoseconds on the thread that did the work. The spans the installation bound reads, a
// request's counted total, an install's total and a native compilation's two spans, are CLOCK_MONOTONIC time capped at
// the thread CPU time read outside them (BoundSpanTimer, RequestTimer), so the CPU clock's system calls stay out of
// them; a request's apart total is the rest of its CPU time. The parts and steps that explain a bound span read
// CLOCK_MONOTONIC inside it, so they are timed only under jitCacheBenchBreakdowns, and runs that set the bound leave it
// off; a request's apart parts, the relinks a finalize span leaves out and the index line's times read CLOCK_MONOTONIC
// too. Every other time is CLOCK_THREAD_CPUTIME_ID, read where its span begins and ends. Sizes are bytes, and keys are
// lowercase hex. The report is off by default and is not charged to the producer budget. Any part may call record for
// the measurements it reports, at the cost of one load and one test when the report is off.

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

class OpenedArtifact;

struct BenchField {
    ASCIILiteral name; // a nested field joins its parts with '.', as "restore.carried"
    Variant<std::nullptr_t, bool, int64_t, uint64_t, double, String> value; // WTF's Variant, which WTF::switchOn visits; a key as a lowercase hex String
};

class BenchReport final {
    WTF_MAKE_NONCOPYABLE(BenchReport);
    WTF_MAKE_TZONE_ALLOCATED(BenchReport);
public:
    // start opens it once it has validated the configuration: appends to path; null when the file cannot be opened. The
    // report takes the VM's ordinal in the process, which every line carries, from a process-wide counter.
    static std::unique_ptr<BenchReport> open(const String& path);
    ~BenchReport(); // writes the lines still buffered, without the summary lines, and closes the file

    void record(ASCIILiteral event, std::initializer_list<BenchField>); // VM thread; flushes past 1 MiB
    // VM thread: the buffered lines, then the lookup line, the index line when the report has an artifact, and the budget
    // and process lines.
    void flush();
    // Whether the timers also time the parts and steps inside the bound's spans: jitCacheBenchBreakdowns when it opened.
    bool timesBreakdowns() const { return m_timesBreakdowns; }
    void addRelinkNanoseconds(uint64_t); // VM thread: RelinkTimer adds each relink of incoming calls it measured
    uint64_t takeRelinkNanoseconds(); // VM thread: returns the relinks added so far and resets the total

    // VM thread. The budget whose limit, charged and peak bytes each flush's budget line writes; the state sets its
    // producer budget once it exists, and a report without one writes zeros.
    void setBudget(RefPtr<ProducerBudget>&&);

    // VM thread. The VM's own artifact, whose IndexStatistics each flush's index line writes; start hands it the
    // OpenedArtifact the state holds, and a report without one writes no index line. The VMs of a process that open one
    // artifact share its index, so the line holds the process's totals for it, which each of their reports repeats.
    void setArtifact(RefPtr<OpenedArtifact>&&);

    // VM thread. The lookup line each flush writes: the count and CPU time of bodyVersion for absent and for present keys,
    // and of openBody by result, which VMState::bodyVersion and VMState::openBody add to.
    enum class Lookup : uint8_t { BodyVersionAbsent, BodyVersionPresent, OpenBodyFound, OpenBodyMissing, OpenBodyUnusable };
    static constexpr unsigned numberOfLookups = 5;
    void countLookup(Lookup, uint64_t nanoseconds);

private:
    BenchReport(int fd, unsigned vmOrdinal);

    struct LookupTally {
        uint64_t count { 0 };
        uint64_t nanoseconds { 0 };
    };

    const int m_fd; // opened for appending
    const int m_pid;
    const unsigned m_vmOrdinal;
    const bool m_timesBreakdowns;
    StringBuilder m_buffer; // the lines not yet written
    uint64_t m_relinkNanoseconds { 0 };
    RefPtr<ProducerBudget> m_budget;
    RefPtr<OpenedArtifact> m_artifact; // destroyed with the report, whose .cpp sees the complete type
    std::array<LookupTally, numberOfLookups> m_lookups { };
};

// Null when no report is open.
BenchReport* benchReport(VM&);

// A scope around ScriptExecutable::installCode's relink of incoming calls. It reads CLOCK_MONOTONIC, the clock of the
// finalize span, which leaves the relink out, when benchReport(vm) is non-null and !vm.heap.currentThreadIsDoingGCWork(),
// and otherwise measures nothing, so it touches the report only on the VM thread outside GC work.
class RelinkTimer {
    WTF_MAKE_NONCOPYABLE(RelinkTimer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit RelinkTimer(VM&);
    ~RelinkTimer(); // when it measured, adds the elapsed time to that report's relink total

private:
    BenchReport* m_report { nullptr }; // null when it measures nothing
    uint64_t m_startNanoseconds { 0 };
};

#if ENABLE(JIT)
// BaselineJITPlan's two measured spans, which the compile event reports as a native compilation's cost for the bench to
// set against installing an image of the same body.
enum class BaselineSpan : bool {
    Compile, // compileInThreadImpl after its profile drain, on whichever thread compiles
    Finalize, // finalize up to the finalize capture hook, on the VM thread, without relinking incoming calls
};

// One of those spans: its time as BoundSpanTimer reads it, and whether the VM generated per-VM support meanwhile, on this
// thread or another, which sets the body aside in the bench's comparison. A compile span touches no report: the plan's
// constructor decides on the VM thread whether it measures. A finalize span takes the VM's report, which lives as long as
// the VM's state and so outlives every plan; it discards what relinks before it left in the report's relink total, and
// at its end leaves out the relinks RelinkTimer added during it.
class BaselineSpanTimer {
    WTF_MAKE_NONCOPYABLE(BaselineSpanTimer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    BaselineSpanTimer(VM&, BaselineSpan, bool measures); // measures nothing unless measures is true

    struct Measured {
        uint64_t nanoseconds;
        bool supportGenerated;
    };
    // Where the span ends, once; nothing when the timer measures nothing.
    std::optional<Measured> stop();

private:
    VM& m_vm;
    BenchReport* m_report { nullptr }; // a measured finalize span's report
    bool m_measures { false };
    uint64_t m_supportGenerationsAtStart { 0 };
    BoundSpanTimer m_span;
};
#endif // ENABLE(JIT)

} // namespace JSC::JITCache
