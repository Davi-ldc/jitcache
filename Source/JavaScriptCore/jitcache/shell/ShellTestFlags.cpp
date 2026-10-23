#include "config.h"
#include "jitcache/shell/ShellTestFlags.h"

#if ENABLE(JITCACHE_TWINS)

#include "MacroAssembler.h"
#include "VM.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/twins/TwinReport.h"
#include <array>
#include <errno.h>
#include <optional>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

namespace JSC::JITCache {

namespace ShellTestFlagsInternal {

// Process-wide test state. The shell writes it on its main thread while it parses its command line, before any other
// thread exists; applyTestFlags reads it right after start, and the threads that emit code read forcedBlinding, so
// thread creation orders every read after the writes.
static bool forcedBlinding { false };
static std::optional<ArtifactWriter::FaultForTesting> writerFault;
static std::optional<ArtifactWriter::KillForTesting> writerKill;
struct TwinEntry {
    enum class Kind : uint8_t { Difference, Skip, Coincidence };
    Kind kind;
    RelocationDomain domain;
};
static std::optional<TwinEntry> twinEntry;

// <value>@<n> splits at its last '@'; n counts from 1.
static std::optional<uint64_t> parseCount(StringView text)
{
    if (text.isEmpty())
        return std::nullopt;
    for (auto character : text.codeUnits()) {
        if (!isASCIIDigit(character))
            return std::nullopt;
    }
    auto count = parseInteger<uint64_t>(text);
    if (!count || !*count)
        return std::nullopt;
    return count;
}

} // namespace ShellTestFlagsInternal

void setForcesBlindingForTesting(bool forces)
{
    ShellTestFlagsInternal::forcedBlinding = forces;
}

bool forcesBlindingForTesting()
{
    return ShellTestFlagsInternal::forcedBlinding;
}

bool setStoreFaultFlag(const char* value)
{
    // <call>:<error>[@<n>]; bodyVersion makes no system call, so openBody and scoring are the calls a hook can fail.
    StringView text = StringView::fromLatin1(value);
    size_t colon = text.find(':');
    if (colon == notFound)
        return false;
    StringView callName = text.left(colon);
    StringView rest = text.substring(colon + 1);
    size_t at = rest.find('@');
    StringView errorName = at == notFound ? rest : rest.left(at);

    StoreTesting::Call call;
    if (callName == "openBody"_s)
        call = StoreTesting::Call::Open;
    else if (callName == "scoring"_s)
        call = StoreTesting::Call::ReadSavedSummaries;
    else
        return false;

    int error;
    if (errorName == "EMFILE"_s)
        error = EMFILE;
    else if (errorName == "ENFILE"_s)
        error = ENFILE;
    else if (errorName == "ENOMEM"_s)
        error = ENOMEM;
    else if (errorName == "EIO"_s)
        error = EIO;
    else
        return false;

    std::optional<uint64_t> count;
    if (at != notFound) {
        count = ShellTestFlagsInternal::parseCount(rest.substring(at + 1));
        if (!count)
            return false;
    }
    StoreTesting::setFault(StoreTesting::Fault { call, error, count });
    return true;
}

bool setWriterFaultFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto check = ArtifactWriter::faultCheckNamed(text.left(at));
    auto count = ShellTestFlagsInternal::parseCount(text.substring(at + 1));
    if (!check || !count)
        return false;
    ShellTestFlagsInternal::writerFault = ArtifactWriter::FaultForTesting { *check, *count };
    return true;
}

bool setKillFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto point = ArtifactWriter::killPointNamed(text.left(at));
    auto count = ShellTestFlagsInternal::parseCount(text.substring(at + 1));
    if (!point || !count)
        return false;
    ShellTestFlagsInternal::writerKill = ArtifactWriter::KillForTesting { *point, *count };
    return true;
}

bool setTwinEntryFlag(const char* value)
{
    using TwinEntry = ShellTestFlagsInternal::TwinEntry;
    StringView kind = StringView::fromLatin1(value);
    if (kind == "difference"_s) {
        ShellTestFlagsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Difference, RelocationDomain::Heap };
        return true;
    }
    if (kind == "skip"_s) {
        ShellTestFlagsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Skip, RelocationDomain::Heap };
        return true;
    }
    static constexpr ASCIILiteral coincidencePrefix = "coincidence:"_s;
    if (!kind.startsWith(StringView { coincidencePrefix }))
        return false;
    // The domains as the twin report spells them.
    static constexpr std::array<std::pair<ASCIILiteral, RelocationDomain>, 4> domains { {
        { "engine-image"_s, RelocationDomain::EngineImage },
        { "executable-pool"_s, RelocationDomain::ExecutablePool },
        { "structure-reservation"_s, RelocationDomain::StructureReservation },
        { "heap"_s, RelocationDomain::Heap },
    } };
    StringView domainName = kind.substring(coincidencePrefix.length());
    for (auto& [name, domain] : domains) {
        if (domainName == name) {
            ShellTestFlagsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Coincidence, domain };
            return true;
        }
    }
    return false;
}

void applyTestFlags(VM& vm)
{
    using namespace ShellTestFlagsInternal;
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // The hooks count this writer's commits from 1.
    if (ArtifactWriter* writer = state->writer()) {
        writer->setFaultForTesting(writerFault);
        writer->setKillForTesting(writerKill);
    }
    // The test entry, of part Integrator and check test-entry, through which the runner's self-tests check how the runner
    // judges a difference, a skip and a coincidence.
    TwinReportSink* report = state->twinReportSink();
    if (!twinEntry || !report)
        return;
    static constexpr ASCIILiteral detail = "--jitcache-test-twin-entry"_s;
    switch (twinEntry->kind) {
    case TwinEntry::Kind::Difference:
        report->difference(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Skip:
        report->skip(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Coincidence:
        report->relocationCoincidence(twinEntry->domain, detail);
        return;
    }
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
