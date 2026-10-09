#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC {
class VM;
} // namespace JSC

// The C++ test framework of testjitcache (harness sub-SPEC section 8.1). A test is a function registered by name, with a
// flag saying whether it needs a VM and the JSC options its process needs, and it fails through a macro that records a
// message. The options string is a space-separated list of --name=value JSC options; tests with the same string share a
// process. tests/testjitcache.cpp defines the members below and the registry the registrations fill.

namespace JSC::JITCache::Tests {

enum class NeedsVM : bool { No, Yes };

class TestContext {
    WTF_MAKE_NONCOPYABLE(TestContext);
public:
    explicit TestContext(VM*); // testjitcache's main: the test's fresh VM, or null
    VM* vm() const; // NeedsVM::Yes: a fresh VM whose API lock the test holds; null otherwise
    void fail(const char* file, int line, String message);
    bool failed() const;
    const Vector<String>& messages() const; // what fail recorded, which main prints after FAIL <name>

private:
    VM* const m_vm;
    Vector<String> m_messages;
};

using TestFunction = void (*)(TestContext&);

struct TestRegistration {
    TestRegistration(ASCIILiteral name, NeedsVM, ASCIILiteral options, TestFunction);
};

} // namespace JSC::JITCache::Tests

#define JITCACHE_TEST_WITH_OPTIONS(name, needsVM, options) \
    static void name(JSC::JITCache::Tests::TestContext&); \
    static JSC::JITCache::Tests::TestRegistration name##Registration { #name ""_s, JSC::JITCache::Tests::NeedsVM::needsVM, options ""_s, name }; \
    static void name(JSC::JITCache::Tests::TestContext& context)
#define JITCACHE_TEST(name, needsVM) JITCACHE_TEST_WITH_OPTIONS(name, needsVM, "")
#define JITCACHE_CHECK(condition) \
    do { \
        if (!(condition)) \
            context.fail(__FILE__, __LINE__, "check failed: " #condition ""_s); \
    } while (false)
#define JITCACHE_FAIL(message) context.fail(__FILE__, __LINE__, message)

#endif // ENABLE(JITCACHE_TWINS)
