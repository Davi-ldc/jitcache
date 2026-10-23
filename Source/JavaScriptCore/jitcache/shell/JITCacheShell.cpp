#include "config.h"
#include "jitcache/shell/JITCacheShell.h"

#include "JSLock.h"
#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/session/JITCacheAPI.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wtf/ASCIICType.h>
#include <wtf/DataLog.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

#if ENABLE(JITCACHE_TWINS)
#include "jitcache/harness/JITCacheTwinsHarness.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/image/ImageTwinsHost.h"
#include "jitcache/shell/ShellFunctions.h"
#include "jitcache/shell/ShellTestFlags.h"
#endif

namespace JSC::JITCache::Shell {

namespace JITCacheShellInternal {

// The shell's JITCache flags. The flags that set process-wide hooks are not here: they hand their values to
// ShellTestFlags.cpp, or to the Image lane, while they are parsed.
struct ShellFlags {
    std::optional<String> artifactPath; // set when --jitcache configures the main VM
    Role role { Role::Consumer };
    size_t maxMemory { SIZE_MAX };
    DeltaCapture deltaCapture { DeltaCapture::All };
    bool deltaAtExit { false };
    bool log { false };
    bool maintenance { false };
    String benchReport;
#if ENABLE(JITCACHE_TWINS)
    String twinsReport;
    String recordLayout;
    Vector<String> avoidLayouts;
    bool fixedHeapProbes { false };
    bool noShellFunctions { false };
    String describeHeap;
    String bodyEvents;
#endif
};

static ShellFlags& shellFlags()
{
    static NeverDestroyed<ShellFlags> flags;
    return flags;
}

// --jitcache-log's lines: toJSON of a result, one line each on stderr.
static void printJITCacheLine(const String& line)
{
    SAFE_FPRINTF(stderr, "%s\n", line.utf8());
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

// A JITCache flag as the command line gives it: the flag alone, or the value after its '='.
struct JITCacheFlagValue {
    bool bare;
    const char* value;
};

static std::optional<JITCacheFlagValue> jitCacheFlagValue(const char* arg, const char* flag)
{
    size_t length = strlen(flag);
    if (strncmp(arg, flag, length))
        return std::nullopt;
    if (!arg[length])
        return JITCacheFlagValue { true, arg + length };
    if (arg[length] != '=')
        return std::nullopt;
    return JITCacheFlagValue { false, arg + length + 1 };
}

// Bare or =1 turns the flag on, =0 off.
static std::optional<bool> jitCacheBooleanFlag(const JITCacheFlagValue& flag)
{
    if (flag.bare || !strcmp(flag.value, "1"))
        return true;
    if (!strcmp(flag.value, "0"))
        return false;
    return std::nullopt;
}

// A path flag takes a non-empty path.
static std::optional<String> jitCachePathFlag(const JITCacheFlagValue& flag)
{
    if (flag.bare || !*flag.value)
        return std::nullopt;
    return String::fromUTF8WithLatin1Fallback(unsafeSpan(flag.value));
}

// Bytes with an optional K, M or G suffix in powers of 1024, or unlimited, which is SIZE_MAX.
static std::optional<size_t> jitCacheMemoryLimit(const JITCacheFlagValue& flag)
{
    if (flag.bare)
        return std::nullopt;
    StringView text = StringView::fromLatin1(flag.value);
    if (text == "unlimited"_s)
        return SIZE_MAX;
    size_t multiplier = 1;
    if (!text.isEmpty()) {
        switch (text[text.length() - 1]) {
        case 'K':
            multiplier = 1024;
            break;
        case 'M':
            multiplier = 1024 * 1024;
            break;
        case 'G':
            multiplier = 1024 * 1024 * 1024;
            break;
        default:
            break;
        }
        if (multiplier != 1)
            text = text.left(text.length() - 1);
    }
    if (text.isEmpty())
        return std::nullopt;
    for (auto character : text.codeUnits()) {
        if (!isASCIIDigit(character))
            return std::nullopt;
    }
    auto bytes = parseInteger<uint64_t>(text);
    if (!bytes || *bytes > SIZE_MAX / multiplier)
        return std::nullopt;
    return static_cast<size_t>(*bytes) * multiplier;
}

// Whether arg is one of JITCache's flags; malformed when its value is not one the flag takes.
static bool parseJITCacheArgument(const char* arg, bool& malformed)
{
    malformed = false;
    if (strncmp(arg, "--jitcache", strlen("--jitcache")))
        return false;

    ShellFlags& flags = shellFlags();
    // The last occurrence of a flag wins.
    if (auto flag = jitCacheFlagValue(arg, "--jitcache")) {
        flags.artifactPath = flag->bare || !*flag->value ? String { defaultArtifactPath } : String::fromUTF8WithLatin1Fallback(unsafeSpan(flag->value));
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-mode")) {
        std::optional<Role> role;
        if (!flag->bare && !strcmp(flag->value, "c"))
            role = Role::Consumer;
        else if (!flag->bare && !strcmp(flag->value, "p"))
            role = Role::Producer;
        else if (!flag->bare && !strcmp(flag->value, "p-c"))
            role = Role::ConsumerProducer;
        if (role)
            flags.role = *role;
        malformed = !role;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-max-memory")) {
        auto limit = jitCacheMemoryLimit(*flag);
        if (limit)
            flags.maxMemory = *limit;
        malformed = !limit;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-delta-capture")) {
        std::optional<DeltaCapture> deltaCapture;
        if (!flag->bare)
            deltaCapture = parseDeltaCapture(StringView::fromLatin1(flag->value));
        if (deltaCapture)
            flags.deltaCapture = *deltaCapture;
        malformed = !deltaCapture;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-delta-at-exit")) {
        if (flag->bare)
            flags.deltaAtExit = true;
        malformed = !flag->bare;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-log")) {
        auto logsResults = jitCacheBooleanFlag(*flag);
        if (logsResults)
            flags.log = *logsResults;
        malformed = !logsResults;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-bench-report")) {
        auto path = jitCachePathFlag(*flag);
        if (path)
            flags.benchReport = WTF::move(*path);
        malformed = !path;
        return true;
    }
    if (auto flag = jitCacheFlagValue(arg, "--jitcache-maintenance")) {
        if (flag->bare)
            flags.maintenance = true;
        malformed = !flag->bare;
        return true;
    }

#if ENABLE(JITCACHE_TWINS)
    // The twins builds' test flags. Parsing runs in jscmain before JSC::initialize, which is where the forced blinding and
    // the image hook must be set.
    auto pathFlag = [&](const char* name, String& field) -> bool {
        auto flag = jitCacheFlagValue(arg, name);
        if (!flag)
            return false;
        auto path = jitCachePathFlag(*flag);
        if (path)
            field = WTF::move(*path);
        malformed = !path;
        return true;
    };
    auto valueFlag = [&](const char* name, bool (*set)(const char*)) -> bool {
        auto flag = jitCacheFlagValue(arg, name);
        if (!flag)
            return false;
        malformed = flag->bare || !set(flag->value);
        return true;
    };
    auto bareFlag = [&](const char* name, const auto& set) -> bool {
        auto flag = jitCacheFlagValue(arg, name);
        if (!flag)
            return false;
        if (flag->bare)
            set();
        malformed = !flag->bare;
        return true;
    };

    if (pathFlag("--jitcache-twins-report", flags.twinsReport)
        || pathFlag("--jitcache-twins-record-layout", flags.recordLayout)
        || pathFlag("--jitcache-describe-heap", flags.describeHeap)
        || pathFlag("--jitcache-body-events", flags.bodyEvents)
        || valueFlag("--jitcache-test-writer-fault", setWriterFaultFlag)
        || valueFlag("--jitcache-test-kill", setKillFlag)
        || valueFlag("--jitcache-test-store-fault", setStoreFaultFlag)
        || valueFlag("--jitcache-test-image-hook", setImageTestHookNamed)
        || valueFlag("--jitcache-test-twin-entry", setTwinEntryFlag)
        || bareFlag("--jitcache-test-fixed-heap-probes", [&] { flags.fixedHeapProbes = true; })
        || bareFlag("--jitcache-test-force-blinding", [] { setForcesBlindingForTesting(true); })
        || bareFlag("--jitcache-test-no-shell-functions", [&] { flags.noShellFunctions = true; }))
        return true;

    if (auto flag = jitCacheFlagValue(arg, "--jitcache-twins-avoid-layout")) {
        // The runner passes the layouts of the sequence's earlier runs, none for its first run, so an empty value is an
        // empty list; an empty path inside a list is malformed.
        Vector<String> paths;
        if (!flag->bare && *flag->value)
            paths = String::fromUTF8WithLatin1Fallback(unsafeSpan(flag->value)).splitAllowingEmptyEntries(',');
        malformed = flag->bare || paths.containsIf([](const String& path) { return path.isEmpty(); });
        if (!malformed)
            flags.avoidLayouts = WTF::move(paths);
        return true;
    }
#endif

    return false;
}

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} // namespace JITCacheShellInternal

bool parseArgument(const char* arg, bool& hasBadOptions)
{
    bool malformed = false;
    if (!JITCacheShellInternal::parseJITCacheArgument(arg, malformed))
        return false;
    if (malformed) {
        hasBadOptions = true;
        dataLog("ERROR: invalid option: ", arg, "\n");
    }
    return true;
}

void printUsage()
{
    fprintf(stderr, "  --jitcache[=<path>]                Configures the main VM's JITCache over the artifact at path (./.jitcache by default)\n");
    fprintf(stderr, "  --jitcache-mode=c|p|p-c            JITCache's role: consumer (the default), producer or consumer-producer\n");
    fprintf(stderr, "  --jitcache-max-memory=<bytes>|unlimited  The producer's working-memory limit, with an optional K, M or G suffix (unlimited by default)\n");
    fprintf(stderr, "  --jitcache-delta-capture=all|compiled  What delta saves: every body that ran (the default) or only compiled ones\n");
    fprintf(stderr, "  --jitcache-delta-at-exit           Calls JITCache's delta once the scripts and the run loop have finished\n");
    fprintf(stderr, "  --jitcache-log[=0|1]               Writes JITCache's start result, delta results and final status to stderr as JSON lines\n");
    fprintf(stderr, "  --jitcache-bench-report=<path>     Appends JITCache's bench report to path\n");
    fprintf(stderr, "  --jitcache-maintenance -- <command>  Runs JITCache maintenance and exits: compact <ratio> [<path>] [--yes | --no]\n");
#if ENABLE(JITCACHE_TWINS)
    fprintf(stderr, "  --jitcache-twins-report=<path>     Appends JITCache's twin report to path\n");
    fprintf(stderr, "  --jitcache-twins-record-layout=<path>  Records the executable pool, the structure reservation and the heap probes in path\n");
    fprintf(stderr, "  --jitcache-twins-avoid-layout=<path>[,<path>...]  Keeps the executable pool and the structure reservation out of the recorded ranges\n");
    fprintf(stderr, "  --jitcache-test-fixed-heap-probes  Records fixed heap probes in place of the live ones\n");
    fprintf(stderr, "  --jitcache-describe-heap=<path>    Writes the description of the heap JavaScript can reach to path at the end of the run\n");
    fprintf(stderr, "  --jitcache-body-events=<path>      Writes the per-body event counts to path at the end of the run\n");
    fprintf(stderr, "  --jitcache-test-writer-fault=<check>@<n>  Fails the writer's n-th commit at check\n");
    fprintf(stderr, "  --jitcache-test-kill=<point>@<n>   Kills the process at point of the writer's n-th commit\n");
    fprintf(stderr, "  --jitcache-test-force-blinding     Blinds every immediate the assembler considers for blinding (JITCache off only)\n");
    fprintf(stderr, "  --jitcache-test-no-shell-functions  Registers none of JITCache's shell functions in any realm, as the pin's shell has none\n");
    fprintf(stderr, "  --jitcache-test-store-fault=openBody|scoring:EMFILE|ENFILE|ENOMEM|EIO[@<n>]  Fails the store's opens with that error\n");
    fprintf(stderr, "  --jitcache-test-image-hook=<name>  Sets the image test hook: relocation-pairs or operation-pair\n");
    fprintf(stderr, "  --jitcache-test-twin-entry=difference|skip|coincidence:<domain>  Writes a test entry to the twin report after start\n");
#endif
    fprintf(stderr, "\n");
}

std::optional<int> runMaintenanceIfRequested(const Vector<String>& arguments)
{
    if (!JITCacheShellInternal::shellFlags().maintenance)
        return std::nullopt;
    // The shell keeps argv's bytes as Latin-1 strings (CommandLine::parseArguments), which latin1() gives back unchanged;
    // the command line reads those bytes as UTF-8.
    Vector<UTF8CString> bytes = arguments.map([](const String& argument) {
        return UTF8CString::fromUTF8(byteCast<char>(argument.latin1().span()));
    });
    return Maintenance::runCommandLine(bytes.span(), stdin, stdout, stderr);
}

void willInitialize()
{
#if ENABLE(JITCACHE_TWINS)
    // Placeholders over the earlier processes' recorded ranges keep this process's executable pool and structure
    // reservation out of them.
    placeholdersBeforeInitialize(JITCacheShellInternal::shellFlags().avoidLayouts);
#endif
}

void didInitialize()
{
#if ENABLE(JITCACHE_TWINS)
    auto& flags = JITCacheShellInternal::shellFlags();
    releasePlaceholdersAfterInitialize();
    if (!flags.recordLayout.isNull())
        recordLayout(flags.recordLayout);
#endif
}

void didCreateMainVM(VM& vm)
{
    JSLockHolder locker(vm);
    auto& flags = JITCacheShellInternal::shellFlags();
#if ENABLE(JITCACHE_TWINS)
    // The heap probes of the run's layout file, recorded with or without --jitcache.
    if (!flags.recordLayout.isNull())
        recordVMLayout(vm, flags.recordLayout, flags.fixedHeapProbes ? HeapProbes::Fixed : HeapProbes::Live);
#endif
    if (!flags.artifactPath)
        return;

    JITCache::Config config;
    config.artifactPath = *flags.artifactPath;
    config.role = flags.role;
    config.producerLimitBytes = flags.maxMemory;
    config.deltaCapture = flags.deltaCapture;
    config.benchReportPath = flags.benchReport;
#if ENABLE(JITCACHE_TWINS)
    config.twinReportPath = flags.twinsReport;
#endif
    JITCache::StartResult result = JITCache::start(vm, config);
    if (flags.log)
        JITCacheShellInternal::printJITCacheLine(JITCache::toJSON(result));
#if ENABLE(JITCACHE_TWINS)
    didStartShellVM(vm, result.outcome, flags.log);
    applyTestFlags(vm);
#endif
}

void didFinishMainIteration(VM& vm, JSGlobalObject& globalObject)
{
#if !ENABLE(JITCACHE_TWINS)
    UNUSED_PARAM(globalObject);
#endif
    JSLockHolder locker(vm);
    auto& flags = JITCacheShellInternal::shellFlags();
    // finishHostRun calls delta under --jitcache-delta-at-exit while the VM's production is active, runs the work below,
    // and then flushes the bench report, so that a run without --destroy-vm keeps its bench events.
    JITCache::finishHostRun(vm, flags.deltaAtExit, [&](const std::optional<JITCache::DeltaResult>& deltaResult) {
        if (deltaResult && flags.log)
            JITCacheShellInternal::printJITCacheLine(JITCache::toJSON(*deltaResult));
#if ENABLE(JITCACHE_TWINS)
        if (!flags.bodyEvents.isNull())
            writeBodyEvents(vm, flags.bodyEvents);
        // After delta, so its full collection changes no capture, and after the body-event dump, whose live UCBs it would
        // retire.
        if (!flags.describeHeap.isNull())
            writeReachableHeapDescription(globalObject, flags.describeHeap);
#endif
        if (flags.log)
            JITCacheShellInternal::printJITCacheLine(JITCache::toJSON(JITCache::status(vm)));
    });
}

std::span<const ShellFunction> functions()
{
#if ENABLE(JITCACHE_TWINS)
    // The ICs lane's snapshot, the shell's own functions (ShellFunctions.h) and the five that bun:jsc registers under
    // the same names (JITCacheTwinsHost.h).
    static constexpr ShellFunction twinsFunctions[] {
#if ENABLE(JIT)
        { "jitcacheICsSnapshot"_s, ICs::functionSnapshotBaselineICs, 2 },
#endif
        { "jitcacheDelta"_s, functionJITCacheDelta, 0 },
        { "jitcacheStatus"_s, functionJITCacheStatus, 0 },
        { "jitcacheStartOutcome"_s, functionJITCacheStartOutcome, 0 },
        { "jitcacheProgress"_s, functionJITCacheProgress, 0 },
        { "jitcacheBodyKey"_s, functionJITCacheBodyKey, 2 },
        { "jitcacheBodyEvents"_s, functionJITCacheBodyEvents, 2 },
        { "jitcacheReadSection"_s, functionJITCacheReadSection, 2 },
        { "jitcacheRewriteSection"_s, functionJITCacheRewriteSection, 4 },
        { "jitcacheDescribeHeap"_s, functionJITCacheDescribeHeap, 1 },
        { "jitcacheUCBStatistics"_s, functionJITCacheUCBStatistics, 1 },
    };
    // A shell given --jitcache-test-no-shell-functions registers none, so that the pin comparison's runs of a twins
    // build have the pin shell's global properties.
    if (!JITCacheShellInternal::shellFlags().noShellFunctions)
        return twinsFunctions;
#endif
    return { };
}

} // namespace JSC::JITCache::Shell
