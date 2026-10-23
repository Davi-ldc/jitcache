#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "CodeSpecializationKind.h"
#include "Completion.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "SourceCode.h"
#include "VM.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <initializer_list>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What the tests of every part do with a VM: run a second VM beside the test's own, evaluate a source in a global object
// of its own, read the globals the source defines, call its functions through JSC::call and find a function's CB. A helper
// that can fail records the failure in the test's context and returns null, nullopt or false, so its caller only has to
// stop. The helpers root nothing: a test keeps every value it passes in the source's globals and the global object on its
// stack, so no collection takes what the test still reads.

namespace JSC::JITCache::Tests {

// A VM beside the test's own, for a test that needs another configuration or a VM it destroys while it runs. The object
// holds the VM's API lock and heap access, and destroys the VM under that lock as testjitcache destroys the test's VM, so
// VM::~VM runs JITCache's teardown hooks.
class ExtraVM {
    WTF_MAKE_NONCOPYABLE(ExtraVM);
public:
    ExtraVM()
        : m_vm(VM::create(HeapType::Large).leakRef())
        , m_locker(m_vm)
    {
        // The holder's reference is the last, which ~JSLockHolder drops before unlocking.
        m_vm.derefSuppressingSaferCPPChecking();
    }

    VM& vm() { return m_vm; }

private:
    VM& m_vm;
    JSLockHolder m_locker;
};

// A new global object with the source evaluated in it as a classic script. A throw fails the test with "evaluating
// <sourceName> threw".
inline JSGlobalObject* createRealm(TestContext& context, VM& vm, const String& source, ASCIILiteral sourceName = "the test source"_s)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL(makeString("evaluating "_s, sourceName, " threw"_s));
        return nullptr;
    }
    return globalObject;
}

inline JSValue globalValue(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return globalObject->get(globalObject, Identifier::fromString(globalObject->vm(), name));
}

// The JS function the global holds. Anything else, a host function included, fails the test with "<sourceName> defines no
// JS function <name>".
inline JSFunction* globalFunction(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name, ASCIILiteral sourceName = "the test source"_s)
{
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, name));
    if (!function || function->isHostFunction()) {
        JITCACHE_FAIL(makeString(sourceName, " defines no JS function "_s, name));
        return nullptr;
    }
    return function;
}

inline std::optional<JSValue> globalObjectValue(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    JSValue value = globalValue(globalObject, name);
    if (!value.isObject()) {
        JITCACHE_FAIL(makeString("the test source defines no object "_s, name));
        return std::nullopt;
    }
    return value;
}

// Calls the function with undefined as this. False when the call threw.
inline bool callFunction(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, std::initializer_list<JSValue> arguments)
{
    MarkedArgumentBuffer argumentList;
    for (JSValue argument : arguments)
        argumentList.append(argument);
    if (argumentList.hasOverflowed()) {
        JITCACHE_FAIL("the argument list overflowed"_s);
        return false;
    }
    NakedPtr<Exception> exception;
    JSC::call(globalObject, function, getCallData(function), jsUndefined(), argumentList, exception);
    if (exception) {
        JITCACHE_FAIL("calling a test function threw"_s);
        return false;
    }
    return true;
}

// The function's CB of the kind, or null with a failure when there is no function or it has no such CB.
inline CodeBlock* codeBlockFor(TestContext& context, JSFunction* function, CodeSpecializationKind kind)
{
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockFor(kind) : nullptr;
    if (!codeBlock)
        JITCACHE_FAIL("the function has no CodeBlock of the kind"_s);
    return codeBlock;
}

// A program whose bodies run without compiling, for the tests of what the registry records and what delta captures of
// such bodies: runTwice is called twice, constructedOnce is constructed once, preparedNeverCalled is never called, and
// the top level runs once.
inline constexpr ASCIILiteral runOnceSource = "function runTwice(x) { return x * 2 + 1; }\n"
    "function constructedOnce(x) { this.x = x; }\n"
    "function preparedNeverCalled(x) { return x - 1; }\n"
    "var total = runTwice(1) + runTwice(2);\n"
    "var made = new constructedOnce(3);\n"_s;

// How failure messages name runOnceSource.
inline constexpr ASCIILiteral runOnceSourceName = "the run-once source"_s;

inline JSGlobalObject* createRunOnceRealm(TestContext& context, VM& vm)
{
    return createRealm(context, vm, runOnceSource, runOnceSourceName);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
