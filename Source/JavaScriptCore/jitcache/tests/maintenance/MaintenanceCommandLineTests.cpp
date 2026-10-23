#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/tests/maintenance/MaintenanceTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include <array>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// Tests of compact's command line (maintenance/JITCacheMaintenance.h and .cpp), on temporary directories whose bodies the
// writer committed to the pack: its exit codes and output, the directory it works on without a path, and its prompt on a
// terminal.

namespace JSC::JITCache::Tests {

using namespace MaintenanceTestSupport;

namespace MaintenanceCommandLineTestsInternal {

// What a stream written through open_memstream holds.
class CapturedOutput {
    WTF_MAKE_NONCOPYABLE(CapturedOutput);
public:
    CapturedOutput()
        : m_file(open_memstream(&m_buffer, &m_size))
    {
    }

    ~CapturedOutput()
    {
        if (m_file)
            fclose(m_file);
        free(m_buffer);
    }

    FILE* file() const { return m_file; }

    String text()
    {
        if (!m_file)
            return { };
        fflush(m_file);
        if (!m_buffer)
            return emptyString();
        return String::fromUTF8(unsafeMakeSpan(static_cast<const char*>(m_buffer), m_size));
    }

private:
    char* m_buffer { nullptr };
    size_t m_size { 0 };
    FILE* m_file { nullptr };
};

struct CommandLineRun {
    int exitCode;
    String out;
    String err;
};

static CommandLineRun runMaintenanceCommand(std::initializer_list<UTF8CString> arguments, FILE* in = nullptr)
{
    Vector<UTF8CString> vector(arguments);
    CapturedOutput out;
    CapturedOutput err;
    int exitCode = Maintenance::runCommandLine(vector.span(), in, out.file(), err.file());
    return { exitCode, out.text(), err.text() };
}

static constexpr ASCIILiteral confirmationPrompt = "This operation will delete the entire cache. Ok? (y/n)"_s;

// A pseudo-terminal: a command line reads its terminal device as input, and the test types the answer on the controlling
// side.
class Pseudoterminal {
    WTF_MAKE_NONCOPYABLE(Pseudoterminal);
public:
    Pseudoterminal()
    {
        m_controller = posix_openpt(O_RDWR | O_NOCTTY);
        if (m_controller < 0)
            return;
        std::array<char, 128> name { };
        if (grantpt(m_controller) || unlockpt(m_controller) || ptsname_r(m_controller, name.data(), name.size()))
            return;
        int device = ::open(name.data(), O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (device < 0)
            return;
        m_input = fdopen(device, "r");
        if (!m_input)
            ::close(device);
    }

    ~Pseudoterminal()
    {
        if (m_input)
            fclose(m_input);
        if (m_controller >= 0)
            ::close(m_controller);
    }

    bool isValid() const { return m_controller >= 0 && m_input; }
    FILE* input() const { return m_input; }

    // The line discipline hands what the controlling side writes to the terminal device as typed input.
    bool type(const char* text) const
    {
        size_t length = strlen(text);
        return ::write(m_controller, text, length) == static_cast<ssize_t>(length);
    }

private:
    int m_controller { -1 };
    FILE* m_input { nullptr };
};

} // namespace MaintenanceCommandLineTestsInternal

using namespace MaintenanceCommandLineTestsInternal;

// The command line's exit codes: 0 for Done and NoArtifact, 1 for Failed, 2 for a usage error, 3 for Busy, 4 for
// NeedsConfirmation and Declined; a summary goes to out and diagnostics to err. Without a terminal there is no prompt and
// no change, even with an answer waiting on in. PlanChanged's 5 needs a change while the user answers, the last test.
JITCACHE_TEST(maintenanceCommandLineExitCodes, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
        return;
    UTF8CString path = artifact->pathArgument();

    auto checkUsage = [&](const String& label, std::initializer_list<UTF8CString> arguments) {
        CommandLineRun run = runMaintenanceCommand(arguments);
        if (run.exitCode != 2 || !run.out.isEmpty() || !run.err.contains("usage: compact <ratio> [<path>] [--yes | --no]"_s))
            JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, "\", err \""_s, run.err, '"'));
    };
    checkUsage("no arguments"_s, { });
    checkUsage("an unknown command"_s, { "prune"_s, path });
    checkUsage("clean, which compact at ratio 0 replaced"_s, { "clean"_s, path });
    checkUsage("compact without a ratio"_s, { "compact"_s });
    checkUsage("a path before the ratio"_s, { "compact"_s, path, "0.5"_s });
    checkUsage("a ratio that is no number"_s, { "compact"_s, "half"_s, path });
    checkUsage("an empty ratio"_s, { "compact"_s, ""_s, path });
    checkUsage("compact with two paths"_s, { "compact"_s, "0.5"_s, path, path });
    checkUsage("an unknown option"_s, { "compact"_s, "0.5"_s, path, "--maybe"_s });
    checkUsage("a mistyped answer without a path"_s, { "compact"_s, "0.5"_s, "--yse"_s });
    checkUsage("both answers"_s, { "compact"_s, "0.5"_s, path, "--yes"_s, "--no"_s });
    JITCACHE_CHECK(artifact->temporariesInCache() == 1);

    CommandLineRun run = runMaintenanceCommand({ "compact"_s, "0"_s, makeString(artifact->path(), "/missing"_s).utf8() });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: no-artifact"_s));

    // An answer waits on in, but in is no terminal.
    std::array<char, 3> answer { 'y', '\n', '\0' };
    FILE* input = fmemopen(answer.data(), 2, "r");
    if (!input) {
        JITCACHE_FAIL(makeString("fmemopen failed: "_s, errnoText(errno)));
        return;
    }
    run = runMaintenanceCommand({ "compact"_s, "1"_s, path }, input);
    fclose(input);
    JITCACHE_CHECK(run.exitCode == 4);
    JITCACHE_CHECK(run.out.contains("compact: needs-confirmation"_s) && run.out.contains("deletes the whole artifact"_s));
    JITCACHE_CHECK(!run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->bodiesInPack() == 2 && artifact->exists("cache/header"));

    run = runMaintenanceCommand({ "compact"_s, "1"_s, path, "--no"_s });
    JITCACHE_CHECK(run.exitCode == 4 && run.out.contains("compact: declined"_s) && !run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->bodiesInPack() == 2);

    run = runMaintenanceCommand({ "compact"_s, "2"_s, path });
    JITCACHE_CHECK(run.exitCode == 1 && run.out.contains("compact: failed"_s) && run.err.contains("bad-ratio"_s));

    {
        auto held = ProducerLock::tryAcquire(artifact->parentFd());
        if (!held) {
            JITCACHE_FAIL("cannot take the producer lock"_s);
            return;
        }
        run = runMaintenanceCommand({ "compact"_s, "0"_s, path });
        JITCACHE_CHECK(run.exitCode == 3 && run.out.contains("compact: busy"_s));
    }

    if (!artifact->placeFile(context, artifact->cacheFd(), "notes.txt", 3))
        return;
    run = runMaintenanceCommand({ "compact"_s, "0"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: done, 1 temporaries removed, 0 bodies evicted, "_s));
    JITCACHE_CHECK(run.err == "unknown-file: cache/notes.txt\n"_s);

    run = runMaintenanceCommand({ "compact"_s, "0"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: done, 0 temporaries removed, 0 bodies evicted, 0 bytes reclaimed"_s));
    JITCACHE_CHECK(artifact->bodiesInPack() == 2);

    run = runMaintenanceCommand({ "compact"_s, "1"_s, "--yes"_s, path });
    JITCACHE_CHECK(!run.exitCode && run.out.contains("compact: done"_s) && !run.out.contains(confirmationPrompt));
    JITCACHE_CHECK(!artifact->exists("cache/header") && !artifact->bodiesInPack());
}

// Without a path, compact works on ./.jitcache in the working directory, here a symbolic link to the artifact, with
// and without an answer.
JITCACHE_TEST(maintenanceCommandLineDefaultsToTheJITCacheDirectory, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    auto workingDirectory = MaintenanceArtifact::create(context, Layout::Parent);
    if (!artifact || !workingDirectory)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
        return;
    if (symlinkat(artifact->pathArgument().legacyCStringPointer(), workingDirectory->parentFd(), ".jitcache")) {
        JITCACHE_FAIL(makeString("symlinkat failed: "_s, errnoText(errno)));
        return;
    }

    // The tests of a process run one at a time, so this one may move the working directory while it runs.
    int previous = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (previous < 0 || fchdir(workingDirectory->parentFd())) {
        JITCACHE_FAIL(makeString("cannot enter the working directory: "_s, errnoText(errno)));
        if (previous >= 0)
            ::close(previous);
        return;
    }
    CommandLineRun cleanup = runMaintenanceCommand({ "compact"_s, "0"_s });
    CommandLineRun compact = runMaintenanceCommand({ "compact"_s, "1"_s, "--yes"_s });
    bool restored = !fchdir(previous);
    ::close(previous);

    JITCACHE_CHECK(restored);
    JITCACHE_CHECK(!cleanup.exitCode && cleanup.out.contains("compact: done, 1 temporaries removed, 0 bodies evicted, "_s));
    JITCACHE_CHECK(!compact.exitCode && compact.out.contains("compact: done"_s));
    JITCACHE_CHECK(!artifact->exists("cache/header") && !artifact->bodiesInPack());
}

// With a terminal on in (a pseudo-terminal here), compact prints the plan and the prompt, reads one line, and applies
// the plan on y or Y; any other answer is Declined and changes nothing.
JITCACHE_TEST(maintenanceCommandLinePromptsOnATerminal, No)
{
    HookScope hooks;
    Pseudoterminal terminal;
    if (!terminal.isValid()) {
        JITCACHE_FAIL(makeString("no pseudo-terminal: "_s, errnoText(errno)));
        return;
    }
    struct TypedAnswer {
        const char* typed;
        bool applies;
    };
    for (const TypedAnswer& answer : { TypedAnswer { "n\n", false }, TypedAnswer { "yes\n", false }, TypedAnswer { "y\n", true }, TypedAnswer { "Y\n", true } }) {
        const char* typed = answer.typed;
        bool applies = answer.applies;
        String label = makeString("typing "_s, String::fromUTF8(typed).trim(isASCIIWhitespace<char16_t>));
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, 2);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
            return;
        if (!terminal.type(typed)) {
            JITCACHE_FAIL(makeString(label, ": the terminal refused the answer"_s));
            return;
        }

        CommandLineRun run = runMaintenanceCommand({ "compact"_s, "1"_s, artifact->pathArgument() }, terminal.input());
        if (!run.out.contains(confirmationPrompt) || !run.out.contains("deletes the whole artifact"_s))
            JITCACHE_FAIL(makeString(label, ": no plan and prompt in \""_s, run.out, '"'));
        if (applies) {
            if (run.exitCode || !run.out.contains("compact: done"_s) || artifact->exists("cache"))
                JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, '"'));
        } else {
            if (run.exitCode != 4 || !run.out.contains("compact: declined"_s))
                JITCACHE_FAIL(makeString(label, ": exit "_s, run.exitCode, ", out \""_s, run.out, '"'));
            if (artifact->temporariesInCache() != 1 || artifact->bodiesInPack() != 2 || !artifact->exists("cache/header"))
                JITCACHE_FAIL(makeString(label, ": the declined plan changed the artifact"_s));
        }
    }
}

// A commit while the user reads the prompt, which the free lock allows, makes the confirmed call return PlanChanged,
// exit code 5, and evict nothing.
JITCACHE_TEST(maintenanceCommandLineReportsAPlanChangedAtThePrompt, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    Pseudoterminal terminal;
    if (!terminal.isValid()) {
        JITCACHE_FAIL(makeString("no pseudo-terminal: "_s, errnoText(errno)));
        return;
    }
    std::array<int, 2> pipeFds { -1, -1 };
    if (pipe2(pipeFds.data(), O_CLOEXEC)) {
        JITCACHE_FAIL(makeString("pipe2 failed: "_s, errnoText(errno)));
        return;
    }
    FILE* out = fdopen(pipeFds[1], "w");
    if (!out) {
        JITCACHE_FAIL(makeString("fdopen failed: "_s, errnoText(errno)));
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return;
    }

    // The command line runs on a thread of its own and writes to the pipe, which this thread reads until the prompt.
    Vector<UTF8CString> arguments { UTF8CString { "compact"_s }, UTF8CString { "1"_s }, artifact->pathArgument() };
    CapturedOutput err;
    std::atomic<int> exitCode { -1 };
    FILE* input = terminal.input();
    FILE* errFile = err.file();
    Ref<Thread> thread = Thread::create("jitcache maintenance"_s, [&] {
        exitCode.store(Maintenance::runCommandLine(arguments.span(), input, out, errFile));
        fclose(out);
    });

    Vector<char> received;
    auto readOutput = [&](bool untilPrompt) {
        std::array<char, 256> chunk;
        while (!untilPrompt || !StringView { std::span<const char> { received.span() } }.contains(confirmationPrompt)) {
            ssize_t count = ::read(pipeFds[0], chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return;
            received.append(std::span<const char> { chunk }.first(static_cast<size_t>(count)));
        }
    };
    readOutput(true);
    bool prompted = StringView { std::span<const char> { received.span() } }.contains(confirmationPrompt);
    JITCACHE_CHECK(prompted);
    if (prompted)
        JITCACHE_CHECK(artifact->commit(context, maintenanceKey(9), 500, 5, 64).has_value());
    // The answer goes in whatever happened, so the thread never waits on it.
    terminal.type("y\n");
    thread->waitForCompletion();
    readOutput(false);
    ::close(pipeFds[0]);

    String output = String::fromUTF8(received.span());
    JITCACHE_CHECK(exitCode.load() == 5);
    if (!output.contains("compact: plan-changed"_s))
        JITCACHE_FAIL(makeString("the output is \""_s, output, '"'));
    JITCACHE_CHECK(artifact->bodiesInPack() == 4 && artifact->exists("cache/header"));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
