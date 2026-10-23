#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "CodeSpecializationKind.h"
#include "DeferGC.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JIT.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "TopExceptionScope.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestVM.h"
#include <initializer_list>
#include <wtf/Ref.h>

// What the tests do with a function's CodeBlocks: create the newborn CB an import starts from, and compile the LLInt CB a
// call installed to baseline in place. A helper that can fail records the failure in the test's context and returns
// null, or skips the functor in withNewbornCodeBlock, so its caller only has to stop.

namespace JSC::JITCache::Tests {

// The newborn CB of a function never called: linked by finishCreation inside a DeferGCForAWhile, as newCodeBlockFor
// requires, and never installed or run. The function yields a new one at each call. The deferral ends when this returns;
// withNewbornCodeBlock keeps one open around its functor.
inline CodeBlock* createNewbornCodeBlock(TestContext& context, JSFunction* function)
{
    VM& vm = function->vm();
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (function->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("the function for a newborn CB was already called"_s);
        return nullptr;
    }
    CodeBlock* codeBlock = function->jsExecutable()->newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope());
    if (scope.exception()) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor threw"_s);
        return nullptr;
    }
    if (!codeBlock || codeBlock->jitType() != JITType::None) {
        JITCACHE_FAIL("newCodeBlockFor gave no newborn CB"_s);
        return nullptr;
    }
    return codeBlock;
}

// Calls functor(CodeBlock&) with the newborn CB of a function never called, which it uses only inside the deferral scope
// that created it.
template<typename Functor>
void withNewbornCodeBlock(TestContext& context, JSFunction* function, const Functor& functor)
{
    DeferGCForAWhile deferGC(function->vm());
    if (CodeBlock* codeBlock = createNewbornCodeBlock(context, function))
        functor(*codeBlock);
}

// Compiles the LLInt CB the function's calls installed to baseline, in that same CB, as JIT::compileSync does for any CB
// the LLInt has run, with collection deferred while it compiles; the setup that installs the code arms the baseline
// counter. Then checks that the CB is still its executable's code.
inline CodeBlock* compileToBaseline(TestContext& context, JSFunction* function)
{
    VM& vm = function->vm();
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not leave an LLInt CB"_s);
        return nullptr;
    }
    CompilationResult result;
    {
        DeferGCForAWhile deferGC(vm);
        result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    }
    if (result != CompilationResult::CompilationSuccessful || codeBlock->jitType() != JITType::BaselineJIT || !codeBlock->baselineJITData()) {
        JITCACHE_FAIL("JIT::compileSync did not install baseline code in the LLInt CB"_s);
        return nullptr;
    }
    if (function->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("the baseline CB is not its executable's code"_s);
        return nullptr;
    }
    return codeBlock;
}

// The function's baseline CB: one call installs its LLInt CB, which compileToBaseline then compiles.
inline CodeBlock* bringToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, std::initializer_list<JSValue> arguments)
{
    if (!callFunction(context, globalObject, function, arguments))
        return nullptr;
    return compileToBaseline(context, function);
}

// The code a baseline CB runs, held by the test so that it outlives anything done to the CB.
inline Ref<BaselineJITCode> baselineCodeOf(CodeBlock& codeBlock)
{
    RefPtr jitCode = codeBlock.jitCode();
    RELEASE_ASSERT(jitCode && jitCode->jitType() == JITType::BaselineJIT);
    return Ref { static_cast<BaselineJITCode&>(*jitCode) };
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
