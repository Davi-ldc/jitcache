#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArithProfile.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "SourceCode.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestVM.h"
#include <wtf/text/MakeString.h>

// The test of the negate MathIC's slow operations on a live VM: after the IC regenerates, the slow call still records
// what it observes in the UCB's arithmetic profile, never in the IC.

namespace JSC::JITCache::Tests {

// With the LLInt off, negate's CodeBlock is born in baseline before its site runs, so the site's MathIC starts from an
// empty profile: the first int32 regenerates it with an int32 fast path, and the first double repoints its slow call and
// gives it the full snippet. Each later call reaches that slow call with a type no earlier call showed the profile. The
// type reaches the UCB profile only if the operation observed it through the IC's arithProfile(); an operation that took
// the IC's address as its profile would OR it into that pointer instead. So the UCB profile gaining each type shows both
// that it keeps learning and that no slow call wrote into the IC.
JITCACHE_TEST_WITH_OPTIONS(imageNegateProfilesThroughICAfterRegeneration, Yes, "--useLLInt=false")
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    auto run = [&](ASCIILiteral source) {
        NakedPtr<Exception> exception;
        evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception)
            JITCACHE_FAIL(makeString("evaluating \""_s, source, "\" threw"_s));
        return !exception;
    };

    if (!run("function negate(x) { return -x; } negate(1); negate(1.5);"_s))
        return;
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, "negate"_s));
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("negate has no baseline CodeBlock"_s);
        return;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
    if (unlinkedCodeBlock.numberOfUnaryArithProfiles() != 1) {
        JITCACHE_FAIL("negate's body does not have exactly one unary arithmetic profile"_s);
        return;
    }
    UnaryArithProfile& profile = unlinkedCodeBlock.unaryArithProfile(0);

    // The repatching operation observed each operand before regenerating, and the double's result after.
    JITCACHE_CHECK(profile.argObservedType() == ObservedType().withInt32().withNumber());
    JITCACHE_CHECK(profile.didObserveDouble());
    JITCACHE_CHECK(!profile.didObserveBigInt());

    // After the regeneration: a non-number operand, then a BigInt result.
    if (!run("negate('x');"_s))
        return;
    JITCACHE_CHECK(profile.argObservedType().sawNonNumber());
    if (!run("negate(5n);"_s))
        return;
    JITCACHE_CHECK(profile.didObserveBigInt());

    // Every call ran this baseline code, so each observation came from its MathIC's slow call.
    JITCACHE_CHECK(function->jsExecutable()->codeBlockForCall() == codeBlock);
    JITCACHE_CHECK(codeBlock->jitType() == JITType::BaselineJIT);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
