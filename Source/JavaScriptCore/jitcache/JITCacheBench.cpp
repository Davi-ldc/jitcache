#include "config.h"
#include "JITCacheBench.h"

#include "ArtifactStore.h"
#include "ExecutableAllocator.h"
#include "JITCacheVMState.h"
#include "UCBKeys.h"
#include "VM.h"
#include <atomic>
#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(BenchReport);

namespace JITCacheBenchInternal {

// The buffer goes out once it passes 1 MiB (harness sub-SPEC section 9.1).
static constexpr size_t flushThresholdCharacters = 1 * MB;

// Each report takes the next ordinal, which every line carries as "vm".
static std::atomic<unsigned> nextVMOrdinal { 0 };

static void appendValue(StringBuilder& line, const decltype(BenchField::value)& value)
{
    WTF::switchOn(value,
        [&](std::nullptr_t) {
            line.append("null"_s);
        },
        [&](bool boolean) {
            line.append(boolean ? "true"_s : "false"_s);
        },
        [&](int64_t number) {
            line.append(number);
        },
        [&](uint64_t number) {
            line.append(number);
        },
        [&](double number) {
            // JSON has no infinities and no NaN.
            if (std::isfinite(number))
                line.append(number);
            else
                line.append("null"_s);
        },
        [&](const String& string) {
            line.appendQuotedJSONString(string);
        });
}

// One JSON line: event, pid and the VM's ordinal, then the fields in order. Event and field names are identifiers the
// code spells, so they are written as they are; a nested field keeps its dotted name as one key.
static void appendLine(StringBuilder& buffer, int pid, unsigned vmOrdinal, ASCIILiteral event, std::initializer_list<BenchField> fields)
{
    buffer.append("{\"event\":\""_s, event, "\",\"pid\":"_s, pid, ",\"vm\":"_s, vmOrdinal);
    for (auto& field : fields) {
        buffer.append(",\""_s, field.name, "\":"_s);
        appendValue(buffer, field.value);
    }
    buffer.append("}\n"_s);
}

// Writes the buffered lines and empties the buffer. The report only carries measurements, so a failed write costs the
// lines it held and is reported on stderr; the run goes on.
static void writeOut(int fd, StringBuilder& buffer)
{
    if (buffer.isEmpty())
        return;
    CString bytes = buffer.toString().utf8();
    buffer.clear();
    auto remaining = bytes.span();
    while (!remaining.empty()) {
        ssize_t written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            SAFE_FPRINTF(stderr, "JITCache: cannot write the bench report: %s\n", safeStrerror(error));
            return;
        }
        remaining = remaining.subspan(static_cast<size_t>(written));
    }
}

} // namespace JITCacheBenchInternal

std::unique_ptr<BenchReport> BenchReport::open(const String& path)
{
    int fd = ::open(path.utf8().data(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return nullptr;
    unsigned vmOrdinal = JITCacheBenchInternal::nextVMOrdinal.fetch_add(1, std::memory_order_relaxed);
    return std::unique_ptr<BenchReport>(new BenchReport(fd, vmOrdinal));
}

BenchReport::BenchReport(int fd, unsigned vmOrdinal)
    : m_fd(fd)
    , m_pid(getpid())
    , m_vmOrdinal(vmOrdinal)
{
}

BenchReport::~BenchReport()
{
    // The owner flushes the report first (section 9.1). Lines recorded since still reach the file, without another
    // round of the summary lines, which a report closed before its VM was configured must not write.
    JITCacheBenchInternal::writeOut(m_fd, m_buffer);
    ::close(m_fd);
}

void BenchReport::record(ASCIILiteral event, std::initializer_list<BenchField> fields)
{
    JITCacheBenchInternal::appendLine(m_buffer, m_pid, m_vmOrdinal, event, fields);
    if (m_buffer.length() > JITCacheBenchInternal::flushThresholdCharacters)
        flush();
}

void BenchReport::flush()
{
    // The summary lines carry totals so far, so the last one of a VM holds its whole run.
    auto tally = [&](Lookup lookup) -> const LookupTally& {
        return m_lookups[static_cast<unsigned>(lookup)];
    };
    JITCacheBenchInternal::appendLine(m_buffer, m_pid, m_vmOrdinal, "lookup"_s, {
        { "bodyVersion.absent.count"_s, tally(Lookup::BodyVersionAbsent).count },
        { "bodyVersion.absent.nanoseconds"_s, tally(Lookup::BodyVersionAbsent).nanoseconds },
        { "bodyVersion.present.count"_s, tally(Lookup::BodyVersionPresent).count },
        { "bodyVersion.present.nanoseconds"_s, tally(Lookup::BodyVersionPresent).nanoseconds },
        { "openBody.found.count"_s, tally(Lookup::OpenBodyFound).count },
        { "openBody.found.nanoseconds"_s, tally(Lookup::OpenBodyFound).nanoseconds },
        { "openBody.missing.count"_s, tally(Lookup::OpenBodyMissing).count },
        { "openBody.missing.nanoseconds"_s, tally(Lookup::OpenBodyMissing).nanoseconds },
        { "openBody.unusable.count"_s, tally(Lookup::OpenBodyUnusable).count },
        { "openBody.unusable.nanoseconds"_s, tally(Lookup::OpenBodyUnusable).nanoseconds },
    });

    // A report without a producer budget, as a Consumer's, writes zeros.
    JITCacheBenchInternal::appendLine(m_buffer, m_pid, m_vmOrdinal, "budget"_s, {
        { "limit"_s, static_cast<uint64_t>(m_budget ? m_budget->limitBytes() : 0) },
        { "charged"_s, static_cast<uint64_t>(m_budget ? m_budget->chargedBytes() : 0) },
        { "peak"_s, static_cast<uint64_t>(m_budget ? m_budget->peakBytes() : 0) },
    });

    // Linux reports ru_maxrss in kilobytes.
    struct rusage usage { };
    uint64_t maxResidentBytes = 0;
    if (!getrusage(RUSAGE_SELF, &usage))
        maxResidentBytes = static_cast<uint64_t>(usage.ru_maxrss) * KB;
    JITCacheBenchInternal::appendLine(m_buffer, m_pid, m_vmOrdinal, "process"_s, {
        { "maxResidentBytes"_s, maxResidentBytes },
        { "executablePoolCommittedBytes"_s, static_cast<uint64_t>(ExecutableAllocator::committedByteCount()) },
    });

    JITCacheBenchInternal::writeOut(m_fd, m_buffer);
}

void BenchReport::addRelinkNanoseconds(uint64_t nanoseconds)
{
    m_relinkNanoseconds += nanoseconds;
}

uint64_t BenchReport::takeRelinkNanoseconds()
{
    return std::exchange(m_relinkNanoseconds, 0);
}

void BenchReport::setBudget(RefPtr<ProducerBudget>&& budget)
{
    m_budget = WTF::move(budget);
}

void BenchReport::countLookup(Lookup lookup, uint64_t nanoseconds)
{
    auto& tally = m_lookups[static_cast<unsigned>(lookup)];
    ++tally.count;
    tally.nanoseconds += nanoseconds;
}

BenchReport* benchReport(VM& vm)
{
    VMState* state = vm.jitCacheState();
    return state ? state->benchReport() : nullptr;
}

uint64_t benchThreadCPUNanoseconds()
{
    struct timespec now { };
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000 + static_cast<uint64_t>(now.tv_nsec);
}

String bodyKeyHex(const BodyKey& key)
{
    // One encoding serves the file names and the reports, so a report's key names its body file.
    BodyFileName name = bodyFileName(key);
    return String { name.span().first(2 * BodyKey::byteSize) };
}

RelinkTimer::RelinkTimer(VM& vm)
{
    // A collection's End phase can relink too (harness sub-SPEC section 9.3); only the VM thread outside GC work
    // touches the report.
    BenchReport* report = benchReport(vm);
    if (!report || vm.heap.currentThreadIsDoingGCWork())
        return;
    m_report = report;
    m_startNanoseconds = benchThreadCPUNanoseconds();
}

RelinkTimer::~RelinkTimer()
{
    if (m_report)
        m_report->addRelinkNanoseconds(benchThreadCPUNanoseconds() - m_startNanoseconds);
}

} // namespace JSC::JITCache
