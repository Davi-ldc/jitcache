#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "InitializeThreading.h"
#include "JSCConfig.h"
#include "JSLock.h"
#include "Options.h"
#include "VM.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <compare>
#include <stdio.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// The C++ test runner. run-jitcache-tests calls `testjitcache --list`, which prints one
// "name<TAB>options<TAB>part<TAB>file" line per test, and then `testjitcache --part=<part> --group=<options>` once per
// part and distinct options string. A group process applies its options while JSC initializes and runs each of its
// tests in turn, each test that needs a VM on a fresh one.

namespace JSC::JITCache::Tests {

namespace TestRunnerInternal {

struct RegisteredTest {
    ASCIILiteral name;
    NeedsVM needsVM;
    ASCIILiteral options;
    ASCIILiteral file; // the registering file, as the compiler names it
    TestFunction function;
    StringView part { }; // main sets the part and the file's path from jitcache/ on
    StringView path { };
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
    SAFE_FPRINTF(stderr, "Usage: testjitcache [--list] [--part=<part>] [--group=<options>] [--test=<name>]... [--filter=<substring>]\n");
    SAFE_FPRINTF(stderr, "  --list               print one \"name<TAB>options<TAB>part<TAB>file\" line per selected test and exit\n");
    SAFE_FPRINTF(stderr, "  --part=<part>        only the tests of this part, the directory under jitcache/tests/ of their file\n");
    SAFE_FPRINTF(stderr, "  --group=<options>    run the tests registered with exactly these JSC options (default: none)\n");
    SAFE_FPRINTF(stderr, "  --test=<name>        only the test of exactly this name; may repeat\n");
    SAFE_FPRINTF(stderr, "  --filter=<substring> only the tests whose name contains the substring\n");
}

// A test's part is the directory under jitcache/tests/ that holds the file registering it, and its path is that file's
// path from jitcache/ on, as Sources.txt lists it. Returns false for a file outside every part's directory.
static bool locatePart(RegisteredTest& test)
{
    constexpr auto testsDirectory = "jitcache/tests/"_s;
    StringView file = test.file;
    size_t start = file.reverseFind(testsDirectory);
    if (start == notFound)
        return false;
    size_t partStart = start + testsDirectory.length();
    size_t partEnd = file.find('/', partStart);
    if (partEnd == notFound || partEnd == partStart)
        return false;
    test.part = file.substring(partStart, partEnd - partStart);
    test.path = file.substring(start);
    return true;
}

// The VM is the test's only one, created after JSC::initialize with the group's options, without a JITCache::start
// call and with its API lock held. This function drops its own reference right after locking, so the holder's reference
// is the last: ~JSLockHolder drops it before unlocking, and VM::~VM, which asserts that the thread holds the API lock,
// runs under the lock, as in runJSC under --destroy-vm.
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

TestRegistration::TestRegistration(ASCIILiteral name, NeedsVM needsVM, ASCIILiteral options, ASCIILiteral file, TestFunction function)
{
    TestRunnerInternal::registeredTests().append({ name, needsVM, options, file, function });
}

} // namespace JSC::JITCache::Tests

int main(int argc, char** argv)
{
    using namespace JSC::JITCache::Tests;
    using namespace JSC::JITCache::Tests::TestRunnerInternal;

    constexpr auto partPrefix = "--part="_s;
    constexpr auto groupPrefix = "--group="_s;
    constexpr auto testPrefix = "--test="_s;
    constexpr auto filterPrefix = "--filter="_s;
    bool list = false;
    String part; // null for every part
    String group = emptyString();
    Vector<String> names; // empty for every name
    String filter = emptyString();
    for (int i = 1; i < argc; ++i) {
        StringView argument = StringView::fromLatin1(argv[i]);
        if (argument == "--list"_s)
            list = true;
        else if (argument.startsWith(partPrefix))
            part = argument.substring(partPrefix.length()).toString();
        else if (argument.startsWith(groupPrefix))
            group = argument.substring(groupPrefix.length()).toString();
        else if (argument.startsWith(testPrefix))
            names.append(argument.substring(testPrefix.length()).toString());
        else if (argument.startsWith(filterPrefix))
            filter = argument.substring(filterPrefix.length()).toString();
        else {
            fprintf(stderr, "testjitcache: unknown argument %s\n", argv[i]);
            printUsage();
            return 2;
        }
    }

    Vector<RegisteredTest>& tests = registeredTests();
    for (auto& test : tests) {
        if (!locatePart(test)) {
            SAFE_FPRINTF(stderr, "testjitcache: %s is registered in %s, which no directory under jitcache/tests/ holds\n", test.name, test.file);
            return 2;
        }
    }

    // A part and a name identify one test in the runner's report, so two registrations of one name in one part, which
    // the static functions of two of its files can make, are refused before anything runs.
    std::ranges::sort(tests, [](const RegisteredTest& a, const RegisteredTest& b) {
        if (auto order = codePointCompare(a.part, b.part); std::is_neq(order))
            return std::is_lt(order);
        return std::is_lt(codePointCompare(StringView(a.name), StringView(b.name)));
    });
    for (size_t i = 1; i < tests.size(); ++i) {
        if (tests[i - 1].part == tests[i].part && StringView(tests[i - 1].name) == StringView(tests[i].name)) {
            SAFE_FPRINTF(stderr, "testjitcache: %s and %s both register %s in part %s\n", tests[i - 1].path.utf8(), tests[i].path.utf8(), tests[i].name, tests[i].part.utf8());
            return 2;
        }
    }

    auto selected = [&](const RegisteredTest& test) {
        return (part.isNull() || StringView(part) == test.part)
            && (names.isEmpty() || names.containsIf([&](const String& name) { return StringView(name) == StringView(test.name); }))
            && (filter.isEmpty() || StringView(test.name).contains(filter));
    };

    if (list) {
        for (auto& test : tests) {
            if (selected(test))
                SAFE_PRINTF("%s\t%s\t%s\t%s\n", test.name, test.options, test.part.utf8(), test.path.utf8());
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
    UTF8CString groupOptions = group.utf8();
    bool groupRejected = false;
    JSC::initialize([&] {
        if (groupOptions.length())
            groupRejected = !JSC::Options::setOptions(groupOptions.legacyCStringPointer());
    });
    if (groupRejected) {
        SAFE_FPRINTF(stderr, "testjitcache: JSC rejected the options of group \"%s\"\n", groupOptions);
        return 2;
    }

    bool anyFailed = false;
    for (auto& test : tests) {
        // View to view: equal(StringView, ASCIILiteral) does not hold between an empty view and ""_s.
        if (StringView(test.options) != StringView(group) || !selected(test))
            continue;
        Vector<String> messages = runTest(test);
        if (messages.isEmpty())
            printf("PASS %s\n", test.name.characters());
        else {
            anyFailed = true;
            printf("FAIL %s\n", test.name.characters());
            for (auto& message : messages)
                SAFE_PRINTF("    %s\n", message.utf8());
        }
        fflush(stdout);
    }
    return anyFailed ? 1 : 0;
}

#endif // ENABLE(JITCACHE_TWINS)
