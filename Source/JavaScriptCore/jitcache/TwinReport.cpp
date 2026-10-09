#include "config.h"
#include "TwinReport.h"

#if ENABLE(JITCACHE_TWINS)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(TwinReport);

namespace TwinReportInternal {

static ASCIILiteral partName(TwinPart part)
{
    switch (part) {
    case TwinPart::UCB:
        return "ucb"_s;
    case TwinPart::Image:
        return "image"_s;
    case TwinPart::CB:
        return "cb"_s;
    case TwinPart::ICs:
        return "ics"_s;
    case TwinPart::Integrator:
        return "integrator"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral domainName(RelocationDomain domain)
{
    switch (domain) {
    case RelocationDomain::EngineImage:
        return "engine-image"_s;
    case RelocationDomain::ExecutablePool:
        return "executable-pool"_s;
    case RelocationDomain::StructureReservation:
        return "structure-reservation"_s;
    case RelocationDomain::Heap:
        return "heap"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// One JSON object per line, its fields in the order given, every value a JSON string.
static void appendField(StringBuilder& line, ASCIILiteral name, const String& value)
{
    line.append(line.isEmpty() ? "{\""_s : ",\""_s, name, "\":"_s);
    line.appendQuotedJSONString(value);
}

static void writeLine(int fd, StringBuilder&& line)
{
    line.append("}\n"_s);
    CString bytes = line.toString().utf8();

    // The line goes straight to the file, with no buffer of its own, so a later crash keeps it. The report runs only in
    // test processes, and a line it cannot write could hide a difference, so a failure ends the process, as the
    // harness's other test-only writers do (harness sub-SPEC sections 4 and 10.3).
    auto remaining = bytes.span();
    while (!remaining.empty()) {
        ssize_t written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            SAFE_FPRINTF(stderr, "JITCache: cannot write the twin report: %s\n", safeStrerror(error));
            exit(1);
        }
        remaining = remaining.subspan(static_cast<size_t>(written));
    }
}

} // namespace TwinReportInternal

std::unique_ptr<TwinReport> TwinReport::open(const String& path)
{
    int fd = ::open(path.utf8().data(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return nullptr;
    return std::unique_ptr<TwinReport>(new TwinReport(fd));
}

TwinReport::TwinReport(int fd)
    : m_fd(fd)
{
}

TwinReport::~TwinReport()
{
    ::close(m_fd);
}

void TwinReport::difference(TwinPart part, ASCIILiteral check, String detail)
{
    ++m_differences;
    StringBuilder line;
    TwinReportInternal::appendField(line, "kind"_s, String { "difference"_s });
    TwinReportInternal::appendField(line, "part"_s, String { TwinReportInternal::partName(part) });
    TwinReportInternal::appendField(line, "check"_s, String { check });
    TwinReportInternal::appendField(line, "detail"_s, detail);
    TwinReportInternal::writeLine(m_fd, WTF::move(line));
}

void TwinReport::skip(TwinPart part, ASCIILiteral check, String reason)
{
    ++m_skips;
    StringBuilder line;
    TwinReportInternal::appendField(line, "kind"_s, String { "skip"_s });
    TwinReportInternal::appendField(line, "part"_s, String { TwinReportInternal::partName(part) });
    TwinReportInternal::appendField(line, "check"_s, String { check });
    TwinReportInternal::appendField(line, "reason"_s, reason);
    TwinReportInternal::writeLine(m_fd, WTF::move(line));
}

void TwinReport::relocationCoincidence(RelocationDomain domain, String detail)
{
    ++m_coincidences;
    StringBuilder line;
    TwinReportInternal::appendField(line, "kind"_s, String { "coincidence"_s });
    TwinReportInternal::appendField(line, "domain"_s, String { TwinReportInternal::domainName(domain) });
    TwinReportInternal::appendField(line, "detail"_s, detail);
    TwinReportInternal::writeLine(m_fd, WTF::move(line));
}

unsigned TwinReport::differences() const
{
    return m_differences;
}

unsigned TwinReport::skips() const
{
    return m_skips;
}

unsigned TwinReport::coincidences() const
{
    return m_coincidences;
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
