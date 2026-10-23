#pragma once

#include "jitcache/base/BenchClock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/session/JITCacheBench.h"
#include <array>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC::JITCache {

class VMState;

// The request event of the bench report. While a bench report is open, a request's time from its first measured part
// to its record passes between two accounts: what counts toward the installation bound, and what is measured apart
// from it. The window reads CLOCK_MONOTONIC, which the vDSO serves, once at each move from one account to the other,
// and the thread CPU clock, a system call, only before it opens and after it closes, so neither CPU read lands in the
// counted account. Counted is the counted account's wall time, capped at the request's thread CPU time, and apart is
// the rest of that CPU time. Losing the CPU during apart work changes neither total; losing it during counted work
// lengthens counted, which the bench's minimum over copies then passes over, and shortens apart by the same time.
//
// Each part reports its wall time. An apart part's time runs between the reads that move the window into its account
// and back out, so it costs no read of its own. A counted part's time needs two reads inside the counted account, so a
// counted part is timed only under jitCacheBenchBreakdowns, whose runs therefore do not set the bound. Whatever a timer
// does after a read that reopens the counted account is counted, so it only subtracts and adds integers there. With no
// report open, each part costs one test.
//
// Counted holds what the bound's accounting puts inside it: the section parses, matching, the butterfly rebuild, seeding,
// parse fields and records. Apart holds the key and context digests, the holder digest a root's context covers, each TDZ
// environment's first digest, reading the artifact, the decode and the extra-memory report that stands in for
// generation's.
enum class CostAccount : uint8_t { Counted, Apart };

enum class RequestPart : uint8_t {
    KeyDigest,
    Lookup, // bodyVersion and openBody; an attach, whose bodyVersion runs before anything is measured, only openBody
    IdentityParse,
    ContextDigest,
    RootHolderDigest,
    EnvironmentDigests,
    HolderDigest,
    FeedbackParse,
    Decode,
    ButterflyRebuild,
    Seeding,
    ParseFields,
    ExtraMemory,
    Records,
};
constexpr unsigned numberOfRequestParts = static_cast<unsigned>(RequestPart::Records) + 1;

class RequestTimer {
    WTF_MAKE_NONCOPYABLE(RequestTimer);

public:
    // Measures only while the VM has a bench report open.
    explicit RequestTimer(VMState&);

    bool isMeasuring() const { return !!m_measurements; }

    // Where a part began: the account the window goes back to when the part ends, and the part's start when it is timed.
    struct PartStart {
        CostAccount previous { CostAccount::Counted };
        std::optional<uint64_t> nanoseconds;
    };

    // A part begins. The first one opens the window in its own account; every later one moves the window to its account.
    PartStart enter(CostAccount account)
    {
        Measurements& measurements = *m_measurements;
        bool timed = account == CostAccount::Apart || measurements.timesBreakdowns;
        if (!measurements.started) {
            // The window goes back to counted after the first part: the request's work outside its parts counts.
            measurements.started = true;
            measurements.account = account;
            measurements.cpuStart = benchThreadCPUNanoseconds();
            measurements.wallMark = benchMonotonicNanoseconds();
            return { CostAccount::Counted, timed ? std::optional { measurements.wallMark } : std::nullopt };
        }
        CostAccount previous = measurements.account;
        if (account != previous) {
            uint64_t now = switchTo(account);
            return { previous, timed ? std::optional { now } : std::nullopt };
        }
        return { previous, timed ? std::optional { benchMonotonicNanoseconds() } : std::nullopt };
    }

    void leave(const PartStart& start, RequestPart part)
    {
        Measurements& measurements = *m_measurements;
        uint64_t end = 0;
        if (start.previous != measurements.account)
            end = switchTo(start.previous);
        else if (start.nanoseconds)
            end = benchMonotonicNanoseconds();
        if (!start.nanoseconds)
            return;
        auto index = static_cast<unsigned>(part);
        measurements.partNanoseconds[index] += end - *start.nanoseconds;
        measurements.partRan[index] = true;
    }

    // The size of the core the decode reads, which the request event reports beside the request's source.
    void noteCoreBytes(size_t bytes)
    {
        if (m_measurements)
            m_measurements->coreBytes = bytes;
    }

    // Closes the window, the wall clock before the CPU clock, then writes the request event; nothing it does after the CPU
    // clock's read is measured.
    void record(ASCIILiteral action, const BodyKey&, uint64_t sourceCharacters);

private:
    using BenchValue = decltype(BenchField::value);

    struct Measurements {
        explicit Measurements(BenchReport& report)
            : report(report)
            , timesBreakdowns(report.timesBreakdowns())
        {
        }

        BenchReport& report;
        const bool timesBreakdowns;
        bool started { false };
        CostAccount account { CostAccount::Counted };
        uint64_t cpuStart { 0 }; // the thread CPU clock before the window opened
        uint64_t wallMark { 0 }; // where the window's time in its current account began
        uint64_t countedNanoseconds { 0 }; // the counted account's wall time
        std::array<uint64_t, numberOfRequestParts> partNanoseconds { };
        std::array<bool, numberOfRequestParts> partRan { };
        std::optional<uint64_t> coreBytes; // set when the request decodes a core
    };

    // Moves the window to another account with one clock read, which also marks where the part that begins or ends there
    // starts or stops. Returns the read.
    uint64_t switchTo(CostAccount account)
    {
        Measurements& measurements = *m_measurements;
        uint64_t now = benchMonotonicNanoseconds();
        if (measurements.account == CostAccount::Counted)
            measurements.countedNanoseconds += now - measurements.wallMark;
        measurements.wallMark = now;
        measurements.account = account;
        return now;
    }

    uint64_t partNanoseconds(RequestPart part) const { return m_measurements->partNanoseconds[static_cast<unsigned>(part)]; }
    bool partRan(RequestPart part) const { return m_measurements->partRan[static_cast<unsigned>(part)]; }

    BenchValue partValue(RequestPart part) const
    {
        if (!partRan(part))
            return nullptr;
        return partNanoseconds(part);
    }

    std::optional<Measurements> m_measurements;
};

// One measured part. A null timer, or one with no report open, measures nothing.
class PartScope {
    WTF_MAKE_NONCOPYABLE(PartScope);

public:
    PartScope(RequestTimer* timer, RequestPart part, CostAccount account)
        : m_timer(timer && timer->isMeasuring() ? timer : nullptr)
        , m_part(part)
    {
        if (m_timer)
            m_start = m_timer->enter(account);
    }

    ~PartScope()
    {
        if (m_timer)
            m_timer->leave(m_start, m_part);
    }

private:
    RequestTimer* const m_timer;
    const RequestPart m_part;
    RequestTimer::PartStart m_start;
};

} // namespace JSC::JITCache
