#include "config.h"
#include "JITCacheTwinsHarness.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "HeapIterationScope.h"
#include "ICTwins.h"
#include "ImageTwins.h"
#include "JITCacheBench.h"
#include "JITCacheBodyEvents.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCacheVMState.h"
#include "JSArray.h"
#include "JSArrayBuffer.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "MacroAssembler.h"
#include "MarkedSpaceInlines.h"
#include "MathCommon.h"
#include "ObjectConstructor.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "UCBTwins.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include <array>
#include <cmath>
#include <errno.h>
#include <optional>
#include <stdio.h>
#include <tuple>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

// The twins harness's entry points for the hosts: the test flags the jsc shell parses before JSC::initialize, what it
// keeps of its main VM's start, the body-event dump both hosts write at the end of a run, and the host functions the shell
// registers in its realms, five of which bun:jsc registers under the same names. Plain builds have none of them.

// The fields that jitcacheProgress, jitcacheBodyEvents with the body-event dump, and jitcacheUCBStatistics write by name.
#define JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(v) \
    v(indexedBodies) v(bodyOpens) v(transientOpenFailures) v(imports) v(seededDecodes) v(attaches) v(gateDrops) v(misses) \
    v(installs) v(bakedFactMismatches) v(captureCandidates) v(capturesDeferred) v(capturesCommitted) v(bytesCommitted) \
    v(deltaRuns)
#define JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(v) \
    v(llintInstructions) v(baselineCompiles) v(dfgCompiles) v(ftlCompiles) v(osrExits) v(jettisons) v(reoptimizations)
#define JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(v) \
    v(imports) v(seededDecodes) v(attaches) v(gateDrops) v(sourceDigests) v(suppliedSourceDigests) \
    v(suppliedDigestVerifications) v(contextDigests) v(holderDigests) v(tdzEnvironmentDigests) v(childHolderDigests) \
    v(vouchedHolderChecks)

namespace JSC::JITCache {

namespace ShellFunctionsInternal {

// A field the lists above miss would go unreported, so a struct that grows fails the build here.
#define JITCACHE_HARNESS_COUNT_FIELD(field) +1
static_assert(sizeof(Progress) == (0 JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
static_assert(sizeof(BodyEventCounts) == (0 JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
static_assert(sizeof(UCBStatistics) == (0 JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t)
    + sizeof(UCBStatistics::misses) + sizeof(UCBStatistics::records) + sizeof(UCBStatistics::invalidMaterial));
#undef JITCACHE_HARNESS_COUNT_FIELD

// The UCB statistics' indexed counters, by the enumerator names the UCB lane's tests use.
static constexpr std::array missReasonNames {
    "NoKey"_s, "NoBody"_s, "BodyUnchanged"_s, "RequestKey"_s, "Context"_s, "Provenance"_s, "Holder"_s, "AtomMap"_s,
    "CoreDigest"_s, "Stack"_s,
};
static constexpr std::array originNames { "Generated"_s, "Decoded"_s, "Imported"_s };
static constexpr std::array invalidMaterialNames {
    "Identity"_s, "Feedback"_s, "Decode"_s, "Closure"_s, "StrictCore"_s, "SuppliedDigest"_s,
};
static_assert(missReasonNames.size() == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(originNames.size() == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(invalidMaterialNames.size() == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

// Process-wide test state. The shell writes it on its main thread while it parses its command line and right after
// start, before any JavaScript runs and before a $262.agent worker thread exists; the host functions only read it, so
// thread creation orders every read after the writes.
static bool forcedBlinding { false };
static std::optional<ArtifactWriter::FaultForTesting> writerFault;
static std::optional<ArtifactWriter::KillForTesting> writerKill;
struct TwinEntry {
    enum class Kind : uint8_t { Difference, Skip, Coincidence };
    Kind kind;
    RelocationDomain domain;
};
static std::optional<TwinEntry> twinEntry;
// The shell's start: its VM, the outcome jitcacheStartOutcome reports, and whether jitcacheDelta logs its results.
struct ShellStart {
    VM* vm { nullptr };
    StartOutcome outcome { StartOutcome::Fault };
    bool logsResults { false };
};
static ShellStart shellStart;

// <value>@<n> splits at its last '@'; n counts from 1.
static std::optional<uint64_t> parseCount(StringView text)
{
    if (text.isEmpty())
        return std::nullopt;
    for (auto character : text.codeUnits()) {
        if (!isASCIIDigit(character))
            return std::nullopt;
    }
    auto count = parseInteger<uint64_t>(text);
    if (!count || !*count)
        return std::nullopt;
    return count;
}

static void writeLogLine(const String& line)
{
    SAFE_FPRINTF(stderr, "%s\n", line.utf8());
}

static JSValue countValue(uint64_t count)
{
    return jsNumber(static_cast<double>(count));
}

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

// The CB of the kind its second argument names of the JSFunction with JS code its first argument holds, null when the
// function has no CB of that kind; a TypeError, and nothing, for any other function or kind.
static std::optional<CodeBlock*> functionCodeBlockArgument(JSGlobalObject* globalObject, CallFrame* callFrame, ASCIILiteral caller)
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

// A body key written as jitcacheBodyKey returns it, 80 lowercase hex digits; a TypeError, and nothing, otherwise.
static std::optional<BodyKey> bodyKeyArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        String hex = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        // The key's hex as bodyKeyHex writes it, which the store's own decoding reads back.
        if (hex.length() == bodyKeyHexLength && hex.containsOnlyASCII()) {
            CString digits = hex.latin1();
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
        return makeString(opened.failure.check, ": "_s, String::fromLatin1(safeStrerror(opened.failure.error).data()));
    return String { opened.failure.check };
}

static JSObject* ucbStatisticsObject(JSGlobalObject* globalObject, const UCBStatistics& statistics, const UCBRegistry::Counts& counts)
{
    VM& vm = globalObject->vm();
    auto put = [&](JSObject* object, ASCIILiteral name, JSValue value) {
        object->putDirect(vm, Identifier::fromString(vm, name), value);
    };
    auto indexedCounters = [&](std::span<const ASCIILiteral> names, std::span<const uint64_t> values) {
        JSObject* object = constructEmptyObject(globalObject);
        for (size_t index = 0; index < names.size(); ++index)
            put(object, names[index], countValue(values[index]));
        return object;
    };

    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_STATISTIC(field) put(result, #field ""_s, countValue(statistics.field));
    JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(JITCACHE_HARNESS_PUT_STATISTIC)
#undef JITCACHE_HARNESS_PUT_STATISTIC
    put(result, "misses"_s, indexedCounters(missReasonNames, statistics.misses));
    put(result, "records"_s, indexedCounters(originNames, statistics.records));
    put(result, "invalidMaterial"_s, indexedCounters(invalidMaterialNames, statistics.invalidMaterial));

    JSObject* registryCounts = constructEmptyObject(globalObject);
    put(registryCounts, "children"_s, countValue(counts.children));
    put(registryCounts, "roots"_s, countValue(counts.roots));
    put(registryCounts, "codeBlocks"_s, countValue(counts.codeBlocks));
    put(registryCounts, "pendingImports"_s, countValue(counts.pendingImports));
    put(result, "counts"_s, registryCounts);
    return result;
}

} // namespace ShellFunctionsInternal

void setForcesBlindingForTesting(bool forces)
{
    ShellFunctionsInternal::forcedBlinding = forces;
}

bool forcesBlindingForTesting()
{
    return ShellFunctionsInternal::forcedBlinding;
}

bool setImageTestHookNamed(const char* name)
{
    StringView hookName = StringView::fromLatin1(name);
#if ENABLE(JIT)
    static constexpr std::array<std::pair<ASCIILiteral, ImageTestHook>, 4> hooks { {
        { "relocation-pairs"_s, ImageTestHook::RelocationPairs },
        { "operation-pair"_s, ImageTestHook::OperationPair },
        { "change-recorded-target"_s, ImageTestHook::ChangeRecordedTarget },
        { "skip-patch"_s, ImageTestHook::SkipPatch },
    } };
    for (auto& [hookFlagName, hook] : hooks) {
        if (hookName == hookFlagName) {
            setImageTestHook(hook);
            return true;
        }
    }
#else
    UNUSED_PARAM(hookName);
#endif
    return false;
}

bool setStoreFaultFlag(const char* value)
{
    // <call>:<error>[@<n>]; bodyVersion makes no system call, so openBody and scoring are the calls a hook can fail.
    StringView text = StringView::fromLatin1(value);
    size_t colon = text.find(':');
    if (colon == notFound)
        return false;
    StringView callName = text.left(colon);
    StringView rest = text.substring(colon + 1);
    size_t at = rest.find('@');
    StringView errorName = at == notFound ? rest : rest.left(at);

    StoreTesting::Call call;
    if (callName == "openBody"_s)
        call = StoreTesting::Call::Open;
    else if (callName == "scoring"_s)
        call = StoreTesting::Call::ReadSavedSummaries;
    else
        return false;

    int error;
    if (errorName == "EMFILE"_s)
        error = EMFILE;
    else if (errorName == "ENFILE"_s)
        error = ENFILE;
    else if (errorName == "ENOMEM"_s)
        error = ENOMEM;
    else if (errorName == "EIO"_s)
        error = EIO;
    else
        return false;

    std::optional<uint64_t> count;
    if (at != notFound) {
        count = ShellFunctionsInternal::parseCount(rest.substring(at + 1));
        if (!count)
            return false;
    }
    StoreTesting::setFault(StoreTesting::Fault { call, error, count });
    return true;
}

bool setWriterFaultFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto check = ArtifactWriter::faultCheckNamed(text.left(at));
    auto count = ShellFunctionsInternal::parseCount(text.substring(at + 1));
    if (!check || !count)
        return false;
    ShellFunctionsInternal::writerFault = ArtifactWriter::FaultForTesting { *check, *count };
    return true;
}

bool setKillFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto point = ArtifactWriter::killPointNamed(text.left(at));
    auto count = ShellFunctionsInternal::parseCount(text.substring(at + 1));
    if (!point || !count)
        return false;
    ShellFunctionsInternal::writerKill = ArtifactWriter::KillForTesting { *point, *count };
    return true;
}

bool setTwinEntryFlag(const char* value)
{
    using TwinEntry = ShellFunctionsInternal::TwinEntry;
    StringView kind = StringView::fromLatin1(value);
    if (kind == "difference"_s) {
        ShellFunctionsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Difference, RelocationDomain::Heap };
        return true;
    }
    if (kind == "skip"_s) {
        ShellFunctionsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Skip, RelocationDomain::Heap };
        return true;
    }
    static constexpr ASCIILiteral coincidencePrefix = "coincidence:"_s;
    if (!kind.startsWith(StringView { coincidencePrefix }))
        return false;
    // The domains as the twin report spells them.
    static constexpr std::array<std::pair<ASCIILiteral, RelocationDomain>, 4> domains { {
        { "engine-image"_s, RelocationDomain::EngineImage },
        { "executable-pool"_s, RelocationDomain::ExecutablePool },
        { "structure-reservation"_s, RelocationDomain::StructureReservation },
        { "heap"_s, RelocationDomain::Heap },
    } };
    StringView domainName = kind.substring(coincidencePrefix.length());
    for (auto& [name, domain] : domains) {
        if (domainName == name) {
            ShellFunctionsInternal::twinEntry = TwinEntry { TwinEntry::Kind::Coincidence, domain };
            return true;
        }
    }
    return false;
}

void didStartShellVM(VM& vm, StartOutcome outcome, bool logsResults)
{
    using namespace ShellFunctionsInternal;
    shellStart = { &vm, outcome, logsResults };

    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // The hooks count this writer's commits from 1.
    if (ArtifactWriter* writer = state->writer()) {
        writer->setFaultForTesting(writerFault);
        writer->setKillForTesting(writerKill);
    }
    // The test entry, of part Integrator and check test-entry, through which the runner's self-tests check how the runner
    // judges a difference, a skip and a coincidence.
    TwinReportSink* report = state->twinReportSink();
    if (!twinEntry || !report)
        return;
    static constexpr ASCIILiteral detail = "--jitcache-test-twin-entry"_s;
    switch (twinEntry->kind) {
    case TwinEntry::Kind::Difference:
        report->difference(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Skip:
        report->skip(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Coincidence:
        report->relocationCoincidence(twinEntry->domain, detail);
        return;
    }
}

void writeBodyEvents(VM& vm, const String& path)
{
    using namespace ShellFunctionsInternal;
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
    result->putDirect(vm, Identifier::fromString(vm, #field ""_s), ShellFunctionsInternal::countValue(progress.field));
    JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_PUT_PROGRESS)
#undef JITCACHE_HARNESS_PUT_PROGRESS
    return JSValue::encode(result);
}

// $vm.jitCacheUCBStatistics() returns this object too: every counter of UCBStatistics by name, the indexed ones as
// objects keyed by their enumerators' names, the registry's counts() as `counts`, and, for { verifyRegistry: true },
// registryViolations, the count verifyRegistry returns after its full collection, reported through the twin report when
// one is open. A VM start never configured has no registry and counts zeros.
JSC_DEFINE_HOST_FUNCTION(functionJITCacheUCBStatistics, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool verifiesRegistry = false;
    if (JSValue options = callFrame->argument(0); options.isObject()) {
        JSValue verify = asObject(options)->get(globalObject, Identifier::fromString(vm, "verifyRegistry"_s));
        RETURN_IF_EXCEPTION(scope, { });
        verifiesRegistry = verify.isTrue();
    }

    VMState* state = vm.jitCacheState();
    std::optional<unsigned> violations;
    if (verifiesRegistry)
        violations = verifyRegistry(vm, state ? state->twinReportSink() : nullptr);

    UCBStatistics statistics { };
    UCBRegistry::Counts counts { };
    if (state) {
        statistics = state->registry().statistics();
        counts = state->registry().counts();
    }
    JSObject* result = ShellFunctionsInternal::ucbStatisticsObject(globalObject, statistics, counts);
    if (violations)
        result->putDirect(vm, Identifier::fromString(vm, "registryViolations"_s), jsNumber(*violations));
    return JSValue::encode(result);
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

    std::optional<CodeBlock*> codeBlock = ShellFunctionsInternal::functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyEvents"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // The UCB of the function's current CB, which every tier's CB of the body reaches. There is no fallback to the UCB the
    // function's UnlinkedFunctionExecutable holds, whose public accessor would decode, generate or import one.
    if (!*codeBlock)
        return JSValue::encode(jsNull());
    return JSValue::encode(ShellFunctionsInternal::bodyEventCountsObject(globalObject, (*codeBlock)->unlinkedCodeBlock()->jitCacheEventCounts()));
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

    std::optional<CodeBlock*> codeBlock = ShellFunctionsInternal::functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyKey"_s);
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
    // The store's own open, in the mode the VM's lookups use, so a test's read neither counts as a lookup nor raises a fault.
    BodyOpen opened = artifact->open(*key, state->strict() ? ValidationMode::Full : ValidationMode::Integrity);
    switch (opened.outcome) {
    case StoreOutcome::Absent:
        return JSValue::encode(jsNull());
    case StoreOutcome::Unavailable:
    case StoreOutcome::Invalid:
        return throwVMError(globalObject, scope, makeString("jitcacheReadSection: "_s, storeFailureText(opened)));
    case StoreOutcome::Found:
        break;
    }
    // A body holds exactly the section kinds its highest tier requires, an empty section included, so any other kind is
    // absent.
    if (!isSectionRequired(*kind, opened.body->highestTier()))
        return JSValue::encode(jsNull());
    RefPtr<ArrayBuffer> buffer = ArrayBuffer::tryCreate(opened.body->section(*kind));
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

#if ENABLE(JIT)
JSC_DEFINE_HOST_FUNCTION(functionJITCacheICsSnapshot, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return ICs::functionSnapshotBaselineICs(globalObject, callFrame);
}
#endif

} // namespace JSC::JITCache

// Unified sources compile this file with others.
#undef JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD
#undef JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT
#undef JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC

#endif // ENABLE(JITCACHE_TWINS)
