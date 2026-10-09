#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "InitializeThreading.h"
#include "JITCacheTest.h"
#include "JSCConfig.h"
#include "JSLock.h"
#include "Options.h"
#include "VM.h"
#include <algorithm>
#include <compare>
#include <stdio.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// The C++ test runner (SPEC-integrator.harness.md section 8.2). The runner of section 7.8 calls `testjitcache --list`,
// which prints one "name<TAB>options" line per test, and then `testjitcache --group=<options>` once per distinct
// options string. A group process applies its options while JSC initializes and runs each of its tests in turn, each
// test that needs a VM on a fresh one.

namespace JSC::JITCache::Tests {

namespace TestRunnerInternal {

struct RegisteredTest {
    ASCIILiteral name;
    NeedsVM needsVM;
    ASCIILiteral options;
    TestFunction function;
};

// Registrations run as static initializers of the test files, in an order C++ leaves unspecified, so the list is
// created at the first one.
static Vector<RegisteredTest>& registeredTests()
{
    static NeverDestroyed<Vector<RegisteredTest>> tests;
    return tests.get();
}

static void printUsage()
{
    fprintf(stderr, "Usage: testjitcache [--list] [--group=<options>] [--filter=<substring>]\n");
    fprintf(stderr, "  --list               print one \"name<TAB>options\" line per test and exit\n");
    fprintf(stderr, "  --group=<options>    run the tests registered with exactly these JSC options (default: none)\n");
    fprintf(stderr, "  --filter=<substring> only the tests whose name contains the substring\n");
}

// The VM is the test's only one, created after JSC::initialize with the group's options, without a JITCache::start
// call and with its API lock held (SPEC-ics.md R-INT-9). This function drops its own reference right after locking, so
// the holder's reference is the last: ~JSLockHolder drops it before unlocking, and VM::~VM, which asserts that the
// thread holds the API lock, runs under the lock, as in runJSC under --destroy-vm.
static Vector<String> runTest(const RegisteredTest& test)
{
    if (test.needsVM == NeedsVM::No) {
        TestContext context(nullptr);
        test.function(context);
        return context.messages();
    }

    VM& vm = VM::create(HeapType::Large).leakRef();
    JSLockHolder locker(vm);
    vm.derefSuppressingSaferCPPChecking();
    TestContext context(&vm);
    test.function(context);
    return context.messages();
}

} // namespace TestRunnerInternal

TestContext::TestContext(VM* vm)
    : m_vm(vm)
{
}

VM* TestContext::vm() const
{
    return m_vm;
}

void TestContext::fail(const char* file, int line, String message)
{
    m_messages.append(makeString(StringView::fromLatin1(file), ':', line, ": "_s, message));
}

bool TestContext::failed() const
{
    return !m_messages.isEmpty();
}

const Vector<String>& TestContext::messages() const
{
    return m_messages;
}

TestRegistration::TestRegistration(ASCIILiteral name, NeedsVM needsVM, ASCIILiteral options, TestFunction function)
{
    TestRunnerInternal::registeredTests().append({ name, needsVM, options, function });
}

} // namespace JSC::JITCache::Tests

int main(int argc, char** argv)
{
    using namespace JSC::JITCache::Tests;
    using namespace JSC::JITCache::Tests::TestRunnerInternal;

    constexpr auto groupPrefix = "--group="_s;
    constexpr auto filterPrefix = "--filter="_s;
    bool list = false;
    String group = emptyString();
    String filter = emptyString();
    for (int i = 1; i < argc; ++i) {
        StringView argument = StringView::fromLatin1(argv[i]);
        if (argument == "--list"_s)
            list = true;
        else if (argument.startsWith(groupPrefix))
            group = argument.substring(groupPrefix.length()).toString();
        else if (argument.startsWith(filterPrefix))
            filter = argument.substring(filterPrefix.length()).toString();
        else {
            fprintf(stderr, "testjitcache: unknown argument %s\n", argv[i]);
            printUsage();
            return 2;
        }
    }

    // A name identifies one test in the output and in the runner's report, so two registrations of one name, which
    // the static functions of two test files can make, are refused before anything runs.
    Vector<RegisteredTest>& tests = registeredTests();
    std::ranges::sort(tests, [](const RegisteredTest& a, const RegisteredTest& b) {
        return std::is_lt(codePointCompare(StringView(a.name), StringView(b.name)));
    });
    for (size_t i = 1; i < tests.size(); ++i) {
        if (StringView(tests[i - 1].name) == StringView(tests[i].name)) {
            fprintf(stderr, "testjitcache: two tests are registered as %s\n", tests[i].name.characters());
            return 2;
        }
    }

    auto matchesFilter = [&](const RegisteredTest& test) {
        return filter.isEmpty() || StringView(test.name).contains(filter);
    };

    if (list) {
        for (auto& test : tests) {
            if (matchesFilter(test))
                printf("%s\t%s\n", test.name.characters(), test.options.characters());
        }
        fflush(stdout);
        return 0;
    }

    // As jscmain does, before options are touched.
    JSC::Config::enableRestrictedOptions();
    WTF::initializeMainThread();

    // Options::initializeWithOptionsCustomization runs the callback once every option holds its default and the
    // override bookkeeping Options::setOptions writes exists, and before notifyOptionsChanged derives the dependent
    // options, which is where Bun's JSCInitialize and the testb3 and testmasm drivers apply theirs.
    CString groupOptions = group.utf8();
    bool groupRejected = false;
    JSC::initialize([&] {
        if (groupOptions.length())
            groupRejected = !JSC::Options::setOptions(groupOptions.data());
    });
    if (groupRejected) {
        fprintf(stderr, "testjitcache: JSC rejected the options of group \"%s\"\n", groupOptions.data());
        return 2;
    }

    bool anyFailed = false;
    for (auto& test : tests) {
        // View to view: equal(StringView, ASCIILiteral) does not hold between an empty view and ""_s.
        if (StringView(test.options) != StringView(group) || !matchesFilter(test))
            continue;
        Vector<String> messages = runTest(test);
        if (messages.isEmpty())
            printf("PASS %s\n", test.name.characters());
        else {
            anyFailed = true;
            printf("FAIL %s\n", test.name.characters());
            for (auto& message : messages)
                printf("    %s\n", message.utf8().data());
        }
        fflush(stdout);
    }
    return anyFailed ? 1 : 0;
}

#endif // ENABLE(JITCACHE_TWINS)
