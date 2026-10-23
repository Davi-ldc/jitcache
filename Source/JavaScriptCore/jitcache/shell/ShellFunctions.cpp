#include "config.h"
#include "jitcache/shell/ShellFunctions.h"

#if ENABLE(JITCACHE_TWINS)

#include "CodeBlock.h"
#include "JSArray.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "ObjectConstructor.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/harness/HostFunctions.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <cmath>
#include <optional>
#include <span>
#include <stdio.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

// The shell functions only the jsc shell registers. They write counts and read a function's CB as the host functions
// both hosts register do (HostFunctions.h).

namespace JSC::JITCache {

namespace ShellFunctionsInternal {

// The shell's start: its VM, the outcome jitcacheStartOutcome reports, and whether jitcacheDelta logs its results. The
// shell writes it on its main thread right after start, before any JavaScript runs and before a $262.agent worker
// thread exists; the host functions only read it, so thread creation orders every read after the write.
struct ShellStart {
    VM* vm { nullptr };
    StartOutcome outcome { StartOutcome::Fault };
    bool logsResults { false };
};
static ShellStart shellStart;

static void writeLogLine(const String& line)
{
    SAFE_FPRINTF(stderr, "%s\n", line.utf8());
}

// A body key written as jitcacheBodyKey returns it, 80 lowercase hex digits; a TypeError, and nothing, otherwise.
static std::optional<BodyKey> bodyKeyArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        String hex = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        // The key's hex as bodyKeyHex writes it, which bodyKeyFromHex reads back.
        if (hex.length() == bodyKeyHexLength && hex.containsOnlyASCII()) {
            ASCIICString digits = hex.ascii();
            if (auto key = bodyKeyFromHex(digits.span()))
                return key;
        }
    }
    throwTypeError(globalObject, scope, makeString(caller, " expects a body key as 80 lowercase hex digits"_s));
    return std::nullopt;
}

// A section kind by the name sectionKindDescriptions gives it, such as "ucb.core"; a TypeError, and nothing, for any
// other name.
static std::optional<SectionKind> sectionKindArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        String name = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            if (name == sectionKindDescriptions[index].name)
                return static_cast<SectionKind>(index);
        }
    }
    throwTypeError(globalObject, scope, makeString(caller, " expects a section name such as \"ucb.core\" or \"cb.summary\""_s));
    return std::nullopt;
}

static String storeFailureText(const BodyOpen& opened)
{
    if (opened.outcome == StoreOutcome::Unavailable)
        return "the body is unavailable for now (EMFILE, ENFILE or ENOMEM)"_s;
    if (opened.failure.error)
        return makeString(opened.failure.check, ": "_s, String::fromUTF8(safeStrerror(opened.failure.error).span()));
    return String { opened.failure.check };
}

} // namespace ShellFunctionsInternal

void didStartShellVM(VM& vm, StartOutcome outcome, bool logsResults)
{
    ShellFunctionsInternal::shellStart = { &vm, outcome, logsResults };
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheDelta, (JSGlobalObject* globalObject, CallFrame*))
{
    using namespace ShellFunctionsInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    DeltaResult result = delta(vm);
    if (shellStart.vm == &vm && shellStart.logsResults)
        writeLogLine(toJSON(result));

    switch (result.outcome) {
    case DeltaOutcome::Completed: {
        JSObject* counts = constructEmptyObject(globalObject);
        counts->putDirect(vm, Identifier::fromString(vm, "eligibleKeys"_s), countValue(result.eligibleKeys));
        counts->putDirect(vm, Identifier::fromString(vm, "committedBodies"_s), countValue(result.committedBodies));
        counts->putDirect(vm, Identifier::fromString(vm, "committedBytes"_s), countValue(result.committedBytes));
        counts->putDirect(vm, Identifier::fromString(vm, "deferredKeys"_s), countValue(result.deferredKeys));
        return JSValue::encode(counts);
    }
    case DeltaOutcome::Rejected:
        return throwVMError(globalObject, scope, String { result.rejection });
    case DeltaOutcome::Faulted:
        ASSERT(result.fault);
        return throwVMError(globalObject, scope, result.fault ? result.fault->stepName() : emptyString());
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheStartOutcome, (JSGlobalObject* globalObject, CallFrame*))
{
    using namespace ShellFunctionsInternal;
    VM& vm = globalObject->vm();
    if (shellStart.vm != &vm)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, String { name(shellStart.outcome) }));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheBodyKey, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<CodeBlock*> codeBlock = functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyKey"_s);
    RETURN_IF_EXCEPTION(scope, { });
    VMState* state = vm.jitCacheState();
    if (!*codeBlock || !state)
        return JSValue::encode(jsNull());
    CodeBlock* baseline = *codeBlock;
    if (JITCode::isOptimizingJIT(baseline->jitType()))
        baseline = baseline->baselineAlternative();
    std::optional<BodyKey> key = state->registry().keyOf(*baseline->unlinkedCodeBlock());
    if (!key)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, bodyKeyHex(*key)));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheReadSection, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    using namespace ShellFunctionsInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<BodyKey> key = bodyKeyArgument(globalObject, callFrame->argument(0), "jitcacheReadSection"_s);
    RETURN_IF_EXCEPTION(scope, { });
    std::optional<SectionKind> kind = sectionKindArgument(globalObject, callFrame->argument(1), "jitcacheReadSection"_s);
    RETURN_IF_EXCEPTION(scope, { });

    VMState* state = vm.jitCacheState();
    OpenedArtifact* artifact = state && state->activityOn() ? state->artifact() : nullptr;
    if (!artifact)
        return JSValue::encode(jsNull());
    // The store's own open, with the checks the VM's lookups run, so a test's read neither counts as a lookup nor
    // raises a fault.
    BodyOpen opened = artifact->open(*key);
    switch (opened.outcome) {
    case StoreOutcome::Absent:
        return JSValue::encode(jsNull());
    case StoreOutcome::Unavailable:
    case StoreOutcome::Invalid:
        return throwVMError(globalObject, scope, makeString("jitcacheReadSection: "_s, storeFailureText(opened)));
    case StoreOutcome::Found:
        break;
    }
    // A body holds every section kind its highest tier requires, an empty section included, and ucb.analysis only when the
    // UCB lane built one, which is never empty; any other kind is absent.
    std::span<const uint8_t> section = opened.body->section(*kind);
    if (!isSectionRequired(*kind, opened.body->highestTier()) && section.empty())
        return JSValue::encode(jsNull());
    RefPtr<ArrayBuffer> buffer = ArrayBuffer::tryCreate(section);
    if (!buffer) {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return JSValue::encode(JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(buffer)));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheRewriteSection, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    using namespace ShellFunctionsInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<BodyKey> key = bodyKeyArgument(globalObject, callFrame->argument(0), "jitcacheRewriteSection"_s);
    RETURN_IF_EXCEPTION(scope, { });
    std::optional<SectionKind> kind = sectionKindArgument(globalObject, callFrame->argument(1), "jitcacheRewriteSection"_s);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue offsetValue = callFrame->argument(2);
    double offset = offsetValue.isNumber() ? offsetValue.asNumber() : -1;
    if (!(offset >= 0) || offset > maxSafeInteger() || std::trunc(offset) != offset)
        return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects a byte offset"_s);

    auto* array = dynamicDowncast<JSArray>(callFrame->argument(3));
    if (!array)
        return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects an array of byte values"_s);
    Vector<uint8_t> bytes;
    for (unsigned index = 0; index < array->length(); ++index) {
        JSValue element = array->getIndex(globalObject, index);
        RETURN_IF_EXCEPTION(scope, { });
        double byte = element.isNumber() ? element.asNumber() : -1;
        if (!(byte >= 0 && byte <= 255) || std::trunc(byte) != byte)
            return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects an array of byte values"_s);
        bytes.append(static_cast<uint8_t>(byte));
    }

    VMState* state = vm.jitCacheState();
    if (!state || !state->producing())
        return throwVMError(globalObject, scope, String { "jitcacheRewriteSection: the VM does not produce"_s });
    auto result = rewriteSectionForTesting(vm, *key, *kind, static_cast<uint64_t>(offset), bytes.span());
    if (!result)
        return throwVMError(globalObject, scope, makeString(result.error().check, ": "_s, result.error().detail));
    return JSValue::encode(jsUndefined());
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
