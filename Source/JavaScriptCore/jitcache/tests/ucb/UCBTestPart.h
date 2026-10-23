#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "VM.h"
#include "VMEntryScopeInlines.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <source_location>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

// What every UCB test works with: TestPart, which carries a test's checks into the helpers that make them, and a global
// object of the test's own, which the test enters.

namespace JSC::JITCache::Tests {

// The checks of one test, which its helpers take: check records a failed condition in the test's context, with the
// description and the line of the call, and returns the condition, so a helper returns at its first failure.
class TestPart {
    WTF_MAKE_NONCOPYABLE(TestPart);
public:
    explicit TestPart(TestContext& context)
        : m_context(context)
    {
    }

    bool check(bool condition, ASCIILiteral description, std::source_location location = std::source_location::current())
    {
        if (!condition)
            m_context.fail(location.file_name(), static_cast<int>(location.line()), description);
        return condition;
    }

    bool check(bool condition, const String& description, std::source_location location = std::source_location::current())
    {
        if (!condition)
            m_context.fail(location.file_name(), static_cast<int>(location.line()), description);
        return condition;
    }

private:
    TestContext& m_context;
};

// A global object of the test's own, entered as a host function's caller enters it, for the helpers that create
// executables in a realm or read their global object through vm.entryScope. The object lives on the test's stack, which
// keeps the global object alive.
class EnteredGlobalObject {
    WTF_MAKE_NONCOPYABLE(EnteredGlobalObject);
public:
    explicit EnteredGlobalObject(VM& vm)
        : m_globalObject(JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull())))
        , m_entryScope(vm, m_globalObject)
    {
    }

    JSGlobalObject* globalObject() const { return m_globalObject; }

private:
    JSGlobalObject* const m_globalObject;
    VMEntryScope m_entryScope;
};

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
