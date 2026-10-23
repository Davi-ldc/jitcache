#include "config.h"
#include "jitcache/ucb/UCBTwinsHost.h"

#if ENABLE(JITCACHE_TWINS)

#include "JSCInlines.h"
#include "JSModuleRecord.h"
#include "ObjectConstructor.h"
#include "VM.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/twins/TwinReport.h"
#include "jitcache/ucb/UCBAnalysis.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBTwins.h"
#include <array>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/text/MakeString.h>

// The counters of UCBStatistics that jitcacheUCBStatistics writes by name, beside its three indexed ones.
#define JITCACHE_UCB_FOR_EACH_STATISTIC(v) \
    v(imports) v(attaches) v(gateDrops) v(sourceDigests) v(suppliedSourceDigests) v(contextDigests) v(holderDigests) \
    v(tdzEnvironmentDigests) v(childHolderDigests) v(vouchedHolderChecks) \
    v(analysisImports) v(analysisMisses) v(analysisAbsent) v(analysisCaptured) v(analysisCopied) v(analysisUnrepresentable)

namespace JSC::JITCache {

namespace UCBTwinsHostInternal {

// A counter the list above misses would go unreported, so a struct that grows fails the build here.
#define JITCACHE_UCB_COUNT_STATISTIC(field) +1
static_assert(sizeof(UCBStatistics) == (0 JITCACHE_UCB_FOR_EACH_STATISTIC(JITCACHE_UCB_COUNT_STATISTIC)) * sizeof(uint64_t)
    + sizeof(UCBStatistics::misses) + sizeof(UCBStatistics::records) + sizeof(UCBStatistics::invalidMaterial));
#undef JITCACHE_UCB_COUNT_STATISTIC

// The UCB statistics' indexed counters, by the enumerator names the UCB lane's tests use.
static constexpr std::array missReasonNames { "NoKey"_s, "NoBody"_s, "BodyUnchanged"_s, "RequestKey"_s, "Context"_s, "Holder"_s, "Stack"_s };
static constexpr std::array originNames { "Generated"_s, "Imported"_s };
static constexpr std::array invalidMaterialNames { "Identity"_s, "Decode"_s };
static_assert(missReasonNames.size() == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(originNames.size() == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(invalidMaterialNames.size() == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

static JSValue countValue(uint64_t count)
{
    return jsNumber(static_cast<double>(count));
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
#define JITCACHE_UCB_PUT_STATISTIC(field) put(result, #field ""_s, countValue(statistics.field));
    JITCACHE_UCB_FOR_EACH_STATISTIC(JITCACHE_UCB_PUT_STATISTIC)
#undef JITCACHE_UCB_PUT_STATISTIC
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

} // namespace UCBTwinsHostInternal

JSC_DEFINE_HOST_FUNCTION(functionUCBStatistics, (JSGlobalObject* globalObject, CallFrame* callFrame))
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
    JSObject* result = UCBTwinsHostInternal::ucbStatisticsObject(globalObject, statistics, counts);
    if (violations)
        result->putDirect(vm, Identifier::fromString(vm, "registryViolations"_s), jsNumber(*violations));
    return JSValue::encode(result);
}

void verifyModuleAnalysis(VM& vm, JSModuleRecord& fromArtifact, JSModuleRecord& fromParse)
{
    VMState* state = vm.jitCacheState();
    TwinReportSink* report = state ? state->twinReportSink() : nullptr;
    if (!report)
        return;
    String difference = firstModuleAnalysisDifference(fromArtifact, fromParse);
    if (difference.isNull())
        return;
    report->difference(TwinPart::UCB, "analysis"_s, makeString(difference, "; module "_s, fromArtifact.moduleKey().string()));
}

} // namespace JSC::JITCache

// Unified sources compile this file with others.
#undef JITCACHE_UCB_FOR_EACH_STATISTIC

#endif // ENABLE(JITCACHE_TWINS)
