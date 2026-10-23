#include "config.h"
#include "jitcache/harness/HostFunctions.h"

#if ENABLE(JITCACHE_TWINS)

#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "MarkedSpaceInlines.h"
#include "ObjectConstructor.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/harness/JITCacheTwinsHarness.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/twins/JITCacheBodyEvents.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBTwinsHost.h"
#include <optional>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

// What hosts call in twins builds beside the placement calls, which JITCacheTwinsHost.h declares: the host functions
// the jsc shell registers in its realms and bun:jsc under the same names, the body-event dump both write at the end of
// a run, and the module-analysis twin Bun's loader calls. jitcacheUCBStatistics and the module-analysis twin forward
// to the UCB lane (UCBTwinsHost.h). countValue and functionCodeBlockArgument serve the jsc shell's own functions too
// (HostFunctions.h). Plain builds have none of them.

// The fields that jitcacheProgress, and jitcacheBodyEvents with the body-event dump, write by name.
#define JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(v) \
    v(indexedBodies) v(bodyOpens) v(transientOpenFailures) v(imports) v(attaches) v(gateDrops) v(misses) v(installs) \
    v(bakedFactMismatches) v(captureCandidates) v(capturesDeferred) v(capturesCommitted) v(bytesCommitted) v(deltaRuns)
#define JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(v) \
    v(llintInstructions) v(baselineCompiles) v(dfgCompiles) v(ftlCompiles) v(osrExits) v(jettisons) v(reoptimizations)

namespace JSC::JITCache {

namespace HostFunctionsInternal {

// A field the lists above miss would go unreported, so a struct that grows fails the build here.
#define JITCACHE_HARNESS_COUNT_FIELD(field) +1
static_assert(sizeof(Progress) == (0 JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
static_assert(sizeof(BodyEventCounts) == (0 JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
#undef JITCACHE_HARNESS_COUNT_FIELD

static JSObject* bodyEventCountsObject(JSGlobalObject* globalObject, const BodyEventCounts& counts)
{
    VM& vm = globalObject->vm();
    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_COUNT(field) result->putDirect(vm, Identifier::fromString(vm, #field ""_s), countValue(counts.field));
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_PUT_COUNT)
#undef JITCACHE_HARNESS_PUT_COUNT
    return result;
}

static void appendBodyEventCounts(StringBuilder& builder, const BodyEventCounts& counts)
{
    ASCIILiteral separator = ""_s;
#define JITCACHE_HARNESS_APPEND_COUNT(field) \
    builder.append(separator, "\"" #field "\":"_s, counts.field); \
    separator = ","_s;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_APPEND_COUNT)
#undef JITCACHE_HARNESS_APPEND_COUNT
}

static void addBodyEventCounts(BodyEventCounts& total, const BodyEventCounts& counts)
{
#define JITCACHE_HARNESS_ADD_COUNT(field) total.field += counts.field;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_ADD_COUNT)
#undef JITCACHE_HARNESS_ADD_COUNT
}

static bool hasBodyEvents(const BodyEventCounts& counts)
{
    bool any = false;
#define JITCACHE_HARNESS_TEST_COUNT(field) any = any || counts.field;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_TEST_COUNT)
#undef JITCACHE_HARNESS_TEST_COUNT
    return any;
}

} // namespace HostFunctionsInternal

JSValue countValue(uint64_t count)
{
    return jsNumber(static_cast<double>(count));
}

std::optional<CodeBlock*> functionCodeBlockArgument(JSGlobalObject* globalObject, CallFrame* callFrame, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* function = dynamicDowncast<JSFunction>(callFrame->argument(0));
    auto* executable = function ? dynamicDowncast<FunctionExecutable>(function->executable()) : nullptr;
    std::optional<CodeSpecializationKind> kind;
    JSValue kindValue = callFrame->argument(1);
    if (kindValue.isString()) {
        String kindName = asString(kindValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (kindName == "call"_s)
            kind = CodeSpecializationKind::CodeForCall;
        else if (kindName == "construct"_s)
            kind = CodeSpecializationKind::CodeForConstruct;
    }
    if (!executable || !kind) {
        throwTypeError(globalObject, scope, makeString(caller, " expects a function with JS code and \"call\" or \"construct\""_s));
        return std::nullopt;
    }
    return executable->codeBlockFor(*kind);
}

void writeBodyEvents(VM& vm, const String& path)
{
    using namespace HostFunctionsInternal;
    JSLockHolder locker(vm);

    // A UCB a collection found dead retires its counts only when a sweep destroys it: the epilogue of a collection the
    // collector thread ended sweeps the precise allocations, and the synchronous sweep every block. Neither starts a
    // collection.
    vm.heap.stopIfNecessary();
    vm.heap.sweepSynchronously();

    struct KeyedCounts {
        BodyKey key;
        BodyEventCounts counts;
    };
    Vector<KeyedCounts> keyed;
    BodyEventCounts total = retiredBodyEvents(vm);
    VMState* state = vm.jitCacheState();
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return IterationStatus::Continue;
            auto* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(static_cast<JSCell*>(cell));
            if (!codeBlock)
                return IterationStatus::Continue;
            const BodyEventCounts& counts = codeBlock->jitCacheEventCounts();
            addBodyEventCounts(total, counts);
            // Keys exist only in a VM start configured.
            if (state && hasBodyEvents(counts)) {
                if (auto key = state->registry().keyOf(*codeBlock))
                    keyed.append({ *key, counts });
            }
            return IterationStatus::Continue;
        });
    }

    StringBuilder dump;
    for (auto& [key, counts] : keyed) {
        dump.append("{\"key\":\""_s, bodyKeyHex(key), "\","_s);
        appendBodyEventCounts(dump, counts);
        dump.append("}\n"_s);
    }
    dump.append("{\"total\":{"_s);
    appendBodyEventCounts(dump, total);
    dump.append("}}\n"_s);
    writeTestFile(path, dump.toString().utf8(), TestFileWrite::Replace);
}

void verifyModuleAnalysisTwin(VM& vm, JSModuleRecord& fromArtifact, JSModuleRecord& fromParse)
{
    verifyModuleAnalysis(vm, fromArtifact, fromParse);
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheStatus, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    Status current = status(vm);
    if (!current.firstFault)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, current.firstFault->stepName()));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheProgress, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    Progress progress = status(vm).progress;
    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_PROGRESS(field) \
    result->putDirect(vm, Identifier::fromString(vm, #field ""_s), countValue(progress.field));
    JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_PUT_PROGRESS)
#undef JITCACHE_HARNESS_PUT_PROGRESS
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheUCBStatistics, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return functionUCBStatistics(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheDescribeHeap, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // The default roots serve only the shell's end-of-run description.
    if (!callFrame->argumentCount())
        return throwVMTypeError(globalObject, scope, "jitcacheDescribeHeap expects at least one value"_s);
    Vector<JSValue> roots;
    for (size_t index = 0; index < callFrame->argumentCount(); ++index)
        roots.append(callFrame->uncheckedArgument(index));
    String description = describeReachableHeap(*globalObject, roots.span());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, description));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheBodyEvents, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<CodeBlock*> codeBlock = functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyEvents"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // The UCB of the function's current CB, which every tier's CB of the body reaches. There is no fallback to the UCB the
    // function's UnlinkedFunctionExecutable holds, whose public accessor would decode, generate or import one.
    if (!*codeBlock)
        return JSValue::encode(jsNull());
    return JSValue::encode(HostFunctionsInternal::bodyEventCountsObject(globalObject, (*codeBlock)->unlinkedCodeBlock()->jitCacheEventCounts()));
}

} // namespace JSC::JITCache

// Unified sources compile this file with others.
#undef JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD
#undef JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT

#endif // ENABLE(JITCACHE_TWINS)
