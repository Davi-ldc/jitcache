#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeIndex.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "IterationModeMetadata.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "Options.h"
#include "SourceCode.h"
#include "SpeculatedType.h"
#include "UnlinkedCodeBlock.h"
#include "VirtualRegister.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBFormat.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/tests/cb/CBTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of the CB lane's import (cb/CBImport.cpp): finishCounter's counter, the realm step of seedLinkedState, the value
// predictions seeding allocates, and the lane's calls running with no cell allocation. Each test runs on a fresh VM and
// builds its CBs as the ICs lane's live tests do: it evaluates its source in its own global object, one call through
// JSC::call installs an LLInt CB, and JIT::compileSync compiles that CB to baseline. A newborn CB comes from
// newCodeBlockFor on a function never called, inside a DeferGCForAWhile; it is never installed, so the same function
// yields one each time, and the test keeps it in a local. Two functions with the same body text have separate UCBs with
// the same metadata layout, so a section captured from one's baseline CB pairs with the other's newborn CB. A CB after
// setup is such a newborn CB taken through prepare, seedLinkedState, setupWithUnlinkedBaselineCode with the first
// function's code, and finishCounter. Every section starts as a capture of a real CB, which the tests decode, change and
// lay out again.

namespace JSC::JITCache::Tests {

namespace CBImportTestsInternal {

static constexpr uint16_t iterationModeBit(IterationMode mode)
{
    return static_cast<uint16_t>(mode);
}

// A small body with an argument, value and array profile, for the tests that need no particular family.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }\n"
    "function elementTwin(array, index) { return array[index] + 1; }\n"
    "var array = [1, 2, 3];\n"_s;

// A body with one iterator_open site, for the realm step.
static constexpr auto iterateSource = "function iterate(iterable) { var sum = 0; for (var x of iterable) sum += x; return sum; }\n"
    "function iterateTwin(iterable) { var sum = 0; for (var x of iterable) sum += x; return sum; }\n"
    "var array = [1, 2, 3];\n"_s;

// A body whose only value-profiled instructions, the two property reads, run only when flag is set.
static constexpr auto coldSource = "function cold(object, flag) { if (flag) return object.a + object.b; return 0; }\n"
    "function coldTwin(object, flag) { if (flag) return object.a + object.b; return 0; }\n"
    "var object = { a: 1, b: 2 };\n"_s;

// Takes a newborn CB of the function through the import's install sequence with the section, inside one deferral:
// prepare, seedLinkedState, beforeSetup(codeBlock), native setup with the producer's code, and finishCounter; then calls
// afterFinish(codeBlock, the counter setup armed, what finishCounter returned).
template<typename BeforeSetup, typename AfterFinish>
static void restoreOnNewborn(TestContext& context, JSFunction* function, std::span<const uint8_t> section, BaselineJITCode& code, const BeforeSetup& beforeSetup, const AfterFinish& afterFinish)
{
    DeferGCForAWhile deferGC(function->vm());
    CodeBlock* newborn = createNewbornCodeBlock(context, function);
    if (!newborn)
        return;
    CBStateImport prepared = CBStateImport::prepare(section);
    prepared.seedLinkedState(*newborn);
    beforeSetup(*newborn);
    newborn->setupWithUnlinkedBaselineCode(Ref { code });
    CounterTriple armed = liveCounter(*newborn);
    CBCounterRestore restore = prepared.finishCounter(*newborn);
    afterFinish(*newborn, armed, restore);
}

static String describeRestore(const CounterTriple& counter, const CBCounterRestore& restore)
{
    return makeString(" (counter "_s, counter.value, ", total "_s, counter.totalCount, ", threshold "_s, counter.activeThreshold, "; carried "_s, static_cast<unsigned>(restore.carried),
        ", crossed "_s, static_cast<unsigned>(restore.crossed), ", nativeSlice "_s, restore.nativeSlice, ", slice "_s, restore.slice, ", startupDeferralScale "_s, restore.startupDeferralScale, ')');
}

// The counter finishCounter restores from a carried record under the VM's startup deferral scale, and, for a finite
// threshold, the native reference: a stack counter given the same triple and checked on the same CB with the same scale
// right after finishCounter, which no pool change can separate from finishCounter's own check in a VM with
// useConcurrentJIT off that is alone in its process.
static void checkCarriedRestore(TestContext& context, const String& what, CodeBlock& codeBlock, const CounterTriple& record, const CBCounterRestore& restore, double scale)
{
    CounterTriple live = liveCounter(codeBlock);
    auto fail = [&](ASCIILiteral rule) {
        JITCACHE_FAIL(makeString(what, ": "_s, rule, describeRestore(live, restore)));
    };
    double count = codeBlock.baselineJITData()->executeCounter().count();
    double progress = static_cast<double>(record.totalCount) + record.value;
    int64_t sliceFloor = 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());

    if (!restore.carried)
        fail("the restore does not say the counter was carried"_s);
    if (restore.startupDeferralScale != scale)
        fail("the restore does not report the VM's startup deferral scale"_s);
    if (live.activeThreshold != record.activeThreshold)
        fail("the active threshold is not the captured one"_s);
    if (restore.slice != std::max(restore.nativeSlice, sliceFloor))
        fail("the slice is not the larger of the native slice and the floor"_s);
    if (static_cast<int64_t>(live.value) != -restore.slice || live.value > -sliceFloor)
        fail("the counter does not hold the slice at least the floor short of crossing"_s);
    if (count != static_cast<double>(static_cast<float>(progress + restore.slice)) - restore.slice)
        fail("count() is not the captured progress as the float total keeps it"_s);
    if (codeBlock.previousCounterForAging() != static_cast<float>(count))
        fail("the aging sample is not the restored count"_s);

    if (record.activeThreshold == std::numeric_limits<int32_t>::max()) {
        int64_t capturedSlice = record.value < 0 ? -static_cast<int64_t>(record.value) : 0;
        if (restore.crossed || restore.nativeSlice != capturedSlice)
            fail("an infinite threshold does not keep the captured slice"_s);
        return;
    }

    // The envelope M >= 1 and s >= 1 leave to any pool. The ceiling C bounds the slice only at a scale of 1, since a
    // scaled slice may run to one unscaled period, M * T.
    int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
    bool sliceInEnvelope = restore.crossed ? !restore.nativeSlice : restore.nativeSlice >= 0 && (scale != 1 || restore.nativeSlice <= ceiling);
    if (!sliceInEnvelope)
        fail("the native slice is negative, passes C at a scale of 1, or is not 0 after a crossing"_s);
    double threshold = record.activeThreshold;
    double scaledThreshold = scale * threshold;
    if (progress < scaledThreshold - std::min(threshold, static_cast<double>(ceiling)) / 2) {
        if (restore.crossed || restore.nativeSlice < static_cast<int64_t>(std::trunc(std::min(scaledThreshold - progress, static_cast<double>(ceiling)))))
            fail("a progress short of the scaled threshold crossed or armed less than s * T - P"_s);
    }

    BaselineExecutionCounter reference;
    reference.m_activeThreshold = record.activeThreshold;
    reference.m_totalCount = record.totalCount;
    reference.m_counter = record.value;
    bool crossed = reference.checkIfThresholdCrossedAndSet(&codeBlock, scale);
    int64_t nativeSlice = crossed ? 0 : -static_cast<int64_t>(reference.m_counter);
    if (restore.crossed != crossed || restore.nativeSlice != nativeSlice)
        fail("the restore differs from the native check of the same triple and scale"_s);
}

// The counter after finishCounter when it does not travel: finishCounter left setup's arming, which follows the seeded
// reoptimization count and the UCB's quick DFG bit and never the startup deferral scale, and setup's aging sample of the
// fresh counter.
static void checkSetupArming(TestContext& context, const String& what, CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore, bool quickDFGTierUp)
{
    CounterTriple live = liveCounter(codeBlock);
    auto fail = [&](ASCIILiteral rule) {
        JITCACHE_FAIL(makeString(what, ": "_s, rule, describeRestore(live, restore)));
    };
    if (restore.carried || restore.crossed)
        fail("the restore says a counter that did not travel was carried or crossed"_s);
    if (restore.startupDeferralScale != 1)
        fail("the restore of a counter that did not travel reports a scale other than 1"_s);
    if (live != armed)
        fail("finishCounter changed the counter setup armed"_s);
    if (restore.nativeSlice != -static_cast<int64_t>(live.value) || restore.slice != restore.nativeSlice)
        fail("the restore does not report setup's slice"_s);

    int32_t warmUp = Options::thresholdForOptimizeAfterWarmUp();
    if (quickDFGTierUp)
        warmUp = static_cast<int32_t>(warmUp * Options::quickDFGTierUpThresholdFactor());
    int32_t threshold = codeBlock.adjustedCounterValue(warmUp);
    if (live.activeThreshold != threshold)
        fail("the active threshold is not optimizeAfterWarmUp's for this history"_s);
    if (threshold == std::numeric_limits<int32_t>::max()) {
        if (live.value != std::numeric_limits<int32_t>::min() || live.totalCount)
            fail("an infinite arming is not the indefinite deferral"_s);
    } else {
        double count = codeBlock.baselineJITData()->executeCounter().count();
        if (count < 0 || count > 1)
            fail("a fresh arming holds more than a point of progress"_s);
    }
    if (codeBlock.previousCounterForAging())
        fail("the aging sample is not setup's sample of the fresh counter"_s);
}

struct ElementRealm {
    JSGlobalObject* globalObject { nullptr };
    JSFunction* twin { nullptr };
    CodeBlock* producer { nullptr };
};

// elementSource with element brought to baseline.
static std::optional<ElementRealm> createElementRealm(TestContext& context, VM& vm)
{
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return std::nullopt;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    JSFunction* twin = globalFunction(context, globalObject, "elementTwin"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    if (!element || !twin || !array)
        return std::nullopt;
    CodeBlock* producer = bringToBaseline(context, globalObject, element, { *array, jsNumber(0) });
    if (!producer)
        return std::nullopt;
    return ElementRealm { globalObject, twin, producer };
}

} // namespace CBImportTestsInternal

using namespace CBImportTestsInternal;

// finishCounter on a CB after setup restores each carried record's counter as checkCarriedRestore requires, and for a
// finite threshold equals the native reference, with the VM's startup deferral scale at 1 and at 4, where a progress P
// between T and 4 * T - min(T, C) / 2 does not cross; two splits of the same P under the same finite T restore identical
// counters; and a counter that did not travel stays as setup armed it, for reoptimization counts of 0 and 3 with the
// quick DFG bit set and clear, and reports a scale of 1 at either scale.
JITCACHE_TEST_WITH_OPTIONS(cbImportFinishCounterRestoresTheCapturedCounter, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    auto image = captureState(context, *realm->producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);
    auto nothingBeforeSetup = [](CodeBlock&) { };

    struct CarriedCase {
        ASCIILiteral name;
        CounterTriple record;
    };
    constexpr int32_t infinite = std::numeric_limits<int32_t>::max();
    const CarriedCase carriedCases[] = {
        { "T = 0"_s, { 0, 0, 0 } },
        { "a finite T below the progress"_s, { -100, 600, 100 } },
        { "a finite T above the progress"_s, { -990, 1000, 1000 } },
        { "T = INT32_MAX with a zero total"_s, { -1000, 0, infinite } },
        { "T = INT32_MAX with a positive total"_s, { -1000, 1000, infinite } },
        { "the indefinite deferral"_s, { std::numeric_limits<int32_t>::min(), 0, infinite } },
        { "a fractional P"_s, { -300, 500.5f, 1000 } },
        { "a finite counter already past the floor"_s, { -5, 700, 1000 } },
        { "an infinite counter already past the floor"_s, { -5, 1005, infinite } },
        { "an infinite counter already crossed"_s, { 10, 990, infinite } },
        { "P = 2 * T"_s, { -1000, 3000, 1000 } },
    };
    constexpr double scaled = 4;
    for (double scale : { 1.0, scaled }) {
        vm.setStartupJITDeferralScale(scale);
        for (const auto& carriedCase : carriedCases) {
            StateImage carried = *image;
            setCounter(carried, CBFormat::CounterMode::Carried, carriedCase.record);
            auto words = encodeState(context, carried);
            if (!words)
                return;
            String what = makeString(carriedCase.name, " at a startup deferral scale of "_s, scale);
            restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
                checkCarriedRestore(context, what, codeBlock, carriedCase.record, restore, scale);
                // P = 2 * T lies between T and 4 * T - min(T, C) / 2 whatever the ceiling: only the scale keeps it from
                // crossing.
                if (scale == scaled && carriedCase.record == CounterTriple { -1000, 3000, 1000 } && restore.crossed)
                    JITCACHE_FAIL(makeString(what, ": crossed"_s));
            });
        }
    }
    vm.setStartupJITDeferralScale(1);

    // The producer's split between m_counter and m_totalCount stays local.
    std::optional<CounterTriple> firstCounter;
    std::optional<CBCounterRestore> firstRestore;
    for (CounterTriple split : { CounterTriple { -500, 1000.5f, 2000 }, CounterTriple { -1000, 1500.5f, 2000 } }) {
        StateImage carried = *image;
        setCounter(carried, CBFormat::CounterMode::Carried, split);
        auto words = encodeState(context, carried);
        if (!words)
            return;
        restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
            checkCarriedRestore(context, "a split of P = 500.5"_s, codeBlock, split, restore, 1);
            CounterTriple live = liveCounter(codeBlock);
            if (!firstCounter) {
                firstCounter = live;
                firstRestore = restore;
                return;
            }
            JITCACHE_CHECK(live == *firstCounter);
            JITCACHE_CHECK(restore.crossed == firstRestore->crossed);
            JITCACHE_CHECK(restore.nativeSlice == firstRestore->nativeSlice);
            JITCACHE_CHECK(restore.slice == firstRestore->slice);
        });
    }
    JITCACHE_CHECK(firstCounter && firstRestore);

    for (double scale : { 1.0, scaled }) {
        vm.setStartupJITDeferralScale(scale);
        for (uint16_t reoptimizations : { 0, 3 }) {
            for (bool quickDFGTierUp : { false, true }) {
                StateImage notCarried = *image;
                setCounter(notCarried, CBFormat::CounterMode::NotCarried, { });
                notCarried.header.reoptimizationRetryCounter = reoptimizations;
                auto words = encodeState(context, notCarried);
                if (!words)
                    return;
                String what = makeString("NotCarried with "_s, reoptimizations, " reoptimizations and the quick DFG bit "_s, quickDFGTierUp ? "set"_s : "clear"_s, " at a startup deferral scale of "_s, scale);
                restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), [&](CodeBlock& codeBlock) {
                    codeBlock.unlinkedCodeBlock()->setQuickDFGTierUp(quickDFGTierUp ? TriState::True : TriState::False);
                }, [&](CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore) {
                    JITCACHE_CHECK(codeBlock.reoptimizationRetryCounter() == reoptimizations);
                    checkSetupArming(context, what, codeBlock, armed, restore, quickDFGTierUp);
                });
            }
        }
    }
    vm.setStartupJITDeferralScale(1);
}

// A slice that the startup deferral scale lets past 2^24 can leave the float total below trunc(slice), so count() is
// slightly negative under a finite threshold. With the slice ceiling at 2^25 + 1, a threshold of 10^7 and a scale of 4,
// finishCounter re-slices a fresh arming's progress of 0.25 to exactly that ceiling for any pool multiplier below 3.3,
// and the float nearest 33554433.25 is 33554432, so count() is -1. A capture of that CB, as a delta takes of an imported
// body that has not run since, records that counter as it stands.
JITCACHE_TEST_WITH_OPTIONS(cbImportRestoresAScaledCounterRoundedBelowItsSlice, Yes, "--useConcurrentJIT=false --maximumExecutionCountsBetweenCheckpointsForBaseline=33554433")
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    auto image = captureState(context, *realm->producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);

    constexpr int64_t ceiling = 33554433;
    constexpr int32_t threshold = 10000000;
    constexpr double scale = 4;
    const CounterTriple record { -1000, 1000.25f, threshold };
    StateImage carried = *image;
    setCounter(carried, CBFormat::CounterMode::Carried, record);
    auto words = encodeState(context, carried);
    if (!words)
        return;

    std::optional<StateImage> recaptured;
    vm.setStartupJITDeferralScale(scale);
    restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), [](CodeBlock&) { }, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
        checkCarriedRestore(context, "a scaled slice past 2^24"_s, codeBlock, record, restore, scale);
        if (restore.crossed || restore.nativeSlice != ceiling || codeBlock.baselineJITData()->executeCounter().count() >= 0) {
            JITCACHE_FAIL(makeString("the restore does not leave a total rounded below its slice"_s, describeRestore(liveCounter(codeBlock), restore)));
            return;
        }
        Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
        auto captured = CBStateCapture::capture(codeBlock, budget.get(), false);
        if (!captured) {
            JITCACHE_FAIL("the capture of the restored CB refused its charge"_s);
            return;
        }
        recaptured = decodeState(context, captured->stateSection());
    });
    vm.setStartupJITDeferralScale(1);
    if (!recaptured)
        return;

    JITCACHE_CHECK(recaptured->header.counterValue == -ceiling);
    JITCACHE_CHECK(recaptured->header.counterTotalCount == 33554432.0f);
}

// In a fresh realm that has created no Map and no Set, seedLinkedState with an iterator_open record holding FastMap
// (FastSet), inside a DeferGC as the install function's deferral surrounds it, leaves the realm's lazy entries (values)
// function materialized, and a later new Map (new Set) installs that same function; with neither bit, both stay null.
// One realm brings iterate to baseline for all three cases: the VM's CodeCache gives every realm that evaluates
// iterateSource the same UCBs, so in a later realm iterate's first call would install the baseline code the first
// compile parked in its UCB's sharing slot. The realm step reads only the seeded CB's realm, so each case seeds a
// newborn CB of iterateTwin in a realm of its own.
JITCACHE_TEST(cbImportRealmStepMaterializesTheIteratorFunction, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* producerRealm = createRealm(context, vm, iterateSource);
    if (!producerRealm)
        return;
    JSFunction* iterate = globalFunction(context, producerRealm, "iterate"_s);
    auto array = globalObjectValue(context, producerRealm, "array"_s);
    if (!iterate || !array)
        return;
    CodeBlock* producer = bringToBaseline(context, producerRealm, iterate, { *array });
    if (!producer)
        return;
    auto captured = captureState(context, *producer);
    if (!captured)
        return;
    if (!captured->header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily]) {
        JITCACHE_FAIL("the producer has no iterator_open site"_s);
        return;
    }
    size_t modesIndex = recordIndex(*captured, CBFormat::iteratorOpenIterationModesFamily, 0);
    uint16_t mapAndSet = iterationModeBit(IterationMode::FastMap) | iterationModeBit(IterationMode::FastSet);
    if (captured->iterationModes[modesIndex] & mapAndSet) {
        JITCACHE_FAIL("the producer recorded a map or set iteration over an array"_s);
        return;
    }

    enum class Seeded : uint8_t { FastMap, FastSet, Neither };
    for (Seeded seeded : { Seeded::FastMap, Seeded::FastSet, Seeded::Neither }) {
        JSGlobalObject* globalObject = createRealm(context, vm, iterateSource);
        if (!globalObject)
            return;
        JSFunction* twin = globalFunction(context, globalObject, "iterateTwin"_s);
        if (!twin)
            return;
        if (globalObject->mapProtoEntriesFunctionConcurrently() || globalObject->setProtoValuesFunctionConcurrently()) {
            JITCACHE_FAIL("the fresh realm created a Map or a Set before the seed"_s);
            return;
        }

        StateImage image = *captured;
        if (seeded == Seeded::FastMap)
            image.iterationModes[modesIndex] |= iterationModeBit(IterationMode::FastMap);
        else if (seeded == Seeded::FastSet)
            image.iterationModes[modesIndex] |= iterationModeBit(IterationMode::FastSet);
        auto words = encodeState(context, image);
        if (!words)
            return;

        JSFunction* seededFunction = nullptr;
        {
            DeferGCForAWhile deferGC(vm);
            CodeBlock* newborn = createNewbornCodeBlock(context, twin);
            if (!newborn)
                return;
            CBStateImport prepared = CBStateImport::prepare(bytesOf(*words));
            {
                DeferGC deferSeed(vm);
                prepared.seedLinkedState(*newborn);
            }
            JSFunction* entries = globalObject->mapProtoEntriesFunctionConcurrently();
            JSFunction* values = globalObject->setProtoValuesFunctionConcurrently();
            switch (seeded) {
            case Seeded::FastMap:
                JITCACHE_CHECK(entries && !values);
                seededFunction = entries;
                break;
            case Seeded::FastSet:
                JITCACHE_CHECK(values && !entries);
                seededFunction = values;
                break;
            case Seeded::Neither:
                JITCACHE_CHECK(!entries && !values);
                break;
            }
        }
        if (!seededFunction)
            continue;

        NakedPtr<Exception> exception;
        auto source = seeded == Seeded::FastMap ? "new Map; Map.prototype.entries"_s : "new Set; Set.prototype.values"_s;
        JSValue installed = evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception) {
            JITCACHE_FAIL("creating the Map or Set threw"_s);
            continue;
        }
        JITCACHE_CHECK(installed == JSValue(seededFunction));
    }
}

// A metadata table gets its predictions only when a seeded value record is not SpecNone, as natively only a prediction
// allocates them: seedLinkedState of a section whose value records are all SpecNone leaves the newborn CB's table
// without predictions, and the same section with the record at offset 1 set allocates them, with that offset reading
// the record and every other offset SpecNone. A capture of a baseline CB whose table has no predictions, since none of its
// value-profiled instructions ran, writes SpecNone at every offset and allocates none.
JITCACHE_TEST(cbImportAllocatesValuePredictionsOnlyForARecord, Yes)
{
    VM& vm = *context.vm();
    if (!Options::useLazyValueProfilePredictions()) {
        JITCACHE_FAIL("useLazyValueProfilePredictions is off, so every metadata table allocates its predictions when it is linked"_s);
        return;
    }
    JSGlobalObject* globalObject = createRealm(context, vm, coldSource);
    if (!globalObject)
        return;
    JSFunction* cold = globalFunction(context, globalObject, "cold"_s);
    JSFunction* twin = globalFunction(context, globalObject, "coldTwin"_s);
    auto object = globalObjectValue(context, globalObject, "object"_s);
    if (!cold || !twin || !object)
        return;
    CodeBlock* producer = bringToBaseline(context, globalObject, cold, { *object, jsBoolean(false) });
    if (!producer)
        return;
    MetadataTable* producerTable = producer->metadataTable();
    if (!producerTable || !producer->unlinkedCodeBlock()->metadata().numValueProfiles()) {
        JITCACHE_FAIL("cold's baseline CB has no metadata value profile"_s);
        return;
    }
    if (producerTable->valueProfilePredictions()) {
        JITCACHE_FAIL("cold's metadata table has predictions, though none of its value-profiled instructions ran"_s);
        return;
    }

    auto image = captureState(context, *producer);
    if (!image)
        return;
    JITCACHE_CHECK(!producerTable->valueProfilePredictions());
    JITCACHE_CHECK(image->header.numValueProfiles == producer->unlinkedCodeBlock()->metadata().numValueProfiles());
    JITCACHE_CHECK(std::ranges::all_of(image->valuePredictions, [](uint64_t prediction) {
        return prediction == SpecNone;
    }));

    for (bool withRecord : { false, true }) {
        StateImage seeded = *image;
        if (withRecord)
            seeded.valuePredictions[0] = SpecInt32Only;
        auto words = encodeState(context, seeded);
        if (!words)
            return;
        DeferGCForAWhile deferGC(vm);
        CodeBlock* newborn = createNewbornCodeBlock(context, twin);
        if (!newborn)
            return;
        MetadataTable* table = newborn->metadataTable();
        if (!table || table->valueProfilePredictions()) {
            JITCACHE_FAIL("the newborn CB has no metadata table, or a table that already has predictions"_s);
            return;
        }
        CBStateImport prepared = CBStateImport::prepare(bytesOf(*words));
        prepared.seedLinkedState(*newborn);
        if (!withRecord) {
            JITCACHE_CHECK(!table->valueProfilePredictions());
            continue;
        }
        if (!table->valueProfilePredictions()) {
            JITCACHE_FAIL("seeding a value record other than SpecNone allocated no predictions"_s);
            continue;
        }
        unsigned visited = 0;
        CBFormat::forEachMetadataValueProfile(*newborn, [&](unsigned offset, const ValueProfileRef& profile) {
            ++visited;
            SpeculatedType expected = offset == 1 ? SpecInt32Only : SpecNone;
            if (profile.prediction() != expected)
                JITCACHE_FAIL(makeString("the value profile at offset "_s, offset, " predicts "_s, profile.prediction(), " instead of "_s, expected));
        });
        JITCACHE_CHECK(visited == seeded.header.numValueProfiles);
    }
}

// capture, scoreLive, prepare, seedLinkedState without the realm step's bits, and finishCounter, each called with no GC
// deferral and so inside the lane's AssertNoGC scope with nothing to excuse an allocation, trip no assertion, and
// capture charges exactly its two section sizes. The section carries a lazy-operand record, so seeding appends through
// the holder.
JITCACHE_TEST(cbImportAllocatesNoCellOutsideADeferral, Yes)
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);
    JITCACHE_CHECK(!vm.heap.isDeferred());

    CBStateCapture::scoreLive(*realm->producer, false);

    // Capture takes the integrator's ProducerBudget, which is final, so no budget that records each charge can stand
    // in. A fresh budget's charged and peak totals show instead that the live capture holds exactly its two section
    // sizes, never held more, and releases them when it dies.
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    std::optional<StateImage> image;
    {
        auto captured = CBStateCapture::capture(*realm->producer, budget.get(), false);
        if (!captured) {
            JITCACHE_FAIL("capture refused its charge"_s);
            return;
        }
        size_t sectionBytes = captured->stateSection().size() + captured->summarySection().size();
        JITCACHE_CHECK(budget->chargedBytes() == sectionBytes);
        JITCACHE_CHECK(budget->peakBytes() == sectionBytes);
        image = decodeState(context, captured->stateSection());
    }
    JITCACHE_CHECK(!budget->chargedBytes());
    if (!image)
        return;

    setLazyOperands(*image, { lazyOperandRecord(BytecodeIndex(0).asBits(), argumentKind, virtualRegisterForArgumentIncludingThis(1).offset()) });
    JITCACHE_CHECK(!image->header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily]);
    auto words = encodeState(context, *image);
    if (!words)
        return;

    CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin);
    if (!newborn)
        return;
    JITCACHE_CHECK(!vm.heap.isDeferred());
    CBStateImport prepared = CBStateImport::prepare(bytesOf(*words));
    prepared.seedLinkedState(*newborn);
    {
        // Native setup, under a deferral as the install function runs it; DeferGCForAWhile never collects at exit.
        DeferGCForAWhile deferGC(vm);
        newborn->setupWithUnlinkedBaselineCode(code.copyRef());
    }
    JITCACHE_CHECK(!vm.heap.isDeferred());
    CBCounterRestore restore = prepared.finishCounter(*newborn);
    JITCACHE_CHECK(restore.carried);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
