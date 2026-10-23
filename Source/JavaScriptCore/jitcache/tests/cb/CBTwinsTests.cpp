#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeIndex.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "FunctionExecutable.h"
#include "IterationModeMetadata.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LazyOperandValueProfile.h"
#include "LazyValueProfile.h"
#include "Operands.h"
#include "Options.h"
#include "SpeculatedType.h"
#include "StructureID.h"
#include "UnlinkedCodeBlock.h"
#include "ValueProfile.h"
#include "VirtualRegister.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBFormat.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/tests/cb/CBTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include "jitcache/twins/TwinReport.h"
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// Tests of the CB lane's twin check (cb/CBTwins.cpp): verifyTwins passes on restored CBs and reports each counter or
// aging fault, bodies without a metadata table go through every lane call, and verifyTwins passes on a section that
// seeds every family. Each test runs on a fresh VM and builds its CBs as the import tests do: it evaluates its source
// in its own global object, one call through JSC::call installs an LLInt CB, and JIT::compileSync compiles that CB to
// baseline. The newborn CB of a function never called, with the same body text, takes a capture of that baseline CB
// through prepare, seedLinkedState, native setup with the producer's code and finishCounter, inside one
// deferral as the install function runs them; verifyTwins then writes to a twin report in a temporary file, which the
// test reads back.

namespace JSC::JITCache::Tests {

namespace CBTwinsTestsInternal {

// One difference line of a twin report: its check, and the whole line, which a failure prints.
struct ReportedDifference {
    String check;
    String line;
};

// A twin report in a temporary file, which the test reads back and removes. TwinReport writes each line with one write
// and no buffer of its own, so the file holds every difference as soon as verifyTwins returns.
class ScratchReport {
    WTF_MAKE_NONCOPYABLE(ScratchReport);
public:
    static std::unique_ptr<ScratchReport> create(TestContext& context)
    {
        // No suffix: the POSIX openTemporaryFile asserts that it gets none, since mkostemp takes none.
        String path = FileSystem::createTemporaryFile("jitcache-cb-twins"_s);
        if (path.isEmpty()) {
            JITCACHE_FAIL("cannot create a temporary file for the twin report"_s);
            return nullptr;
        }
        auto report = TwinReport::open(path);
        if (!report) {
            FileSystem::deleteFile(path);
            JITCACHE_FAIL("cannot open the twin report"_s);
            return nullptr;
        }
        return std::unique_ptr<ScratchReport>(new ScratchReport(WTF::move(path), WTF::move(report)));
    }

    ~ScratchReport()
    {
        m_report = nullptr;
        FileSystem::deleteFile(m_path);
    }

    TwinReport& report() { return *m_report; }

    // The CB lane's difference lines, in the order the check reported them.
    std::optional<Vector<ReportedDifference>> differences(TestContext& context) const
    {
        Vector<ReportedDifference> reported;
        // An empty file reads back as nothing at all (FileHandle::readAll), so a report with no difference is not read.
        if (!m_report->differences())
            return reported;
        auto bytes = FileSystem::readEntireFile(m_path);
        if (!bytes) {
            JITCACHE_FAIL("cannot read the twin report back"_s);
            return std::nullopt;
        }
        static constexpr auto prefix = "{\"kind\":\"difference\",\"part\":\"cb\",\"check\":\""_s;
        auto checkStart = static_cast<unsigned>(prefix.length());
        for (const String& line : String::fromUTF8(bytes->span()).split('\n')) {
            if (!line.startsWith(prefix))
                continue;
            size_t checkEnd = line.find('"', checkStart);
            if (checkEnd == notFound) {
                JITCACHE_FAIL(makeString("a twin report line has no end to its check: "_s, line));
                return std::nullopt;
            }
            reported.append(ReportedDifference { line.substring(checkStart, static_cast<unsigned>(checkEnd) - checkStart), line });
        }
        if (reported.size() != m_report->differences()) {
            JITCACHE_FAIL(makeString("the twin report counts "_s, m_report->differences(), " differences, and its file holds "_s, reported.size(), " CB difference lines"_s));
            return std::nullopt;
        }
        return reported;
    }

private:
    ScratchReport(String&& path, std::unique_ptr<TwinReport>&& report)
        : m_path(WTF::move(path))
        , m_report(WTF::move(report))
    {
    }

    String m_path;
    std::unique_ptr<TwinReport> m_report;
};

static String describeChecks(const Vector<String>& checks)
{
    if (checks.isEmpty())
        return "nothing"_s;
    StringBuilder builder;
    for (const String& check : checks) {
        if (!builder.isEmpty())
            builder.append(", "_s);
        builder.append(check);
    }
    return builder.toString();
}

// Runs verifyTwins with a fresh report and checks that the distinct checks it reported are exactly `expected`, and that it
// reported no skip, since the CB lane's check needs nothing a run can lack.
static void expectTwinChecks(TestContext& context, const String& what, const CBStateImport& imported, CodeBlock& codeBlock, const CBCounterRestore& restore, std::initializer_list<ASCIILiteral> expected)
{
    auto scratch = ScratchReport::create(context);
    if (!scratch)
        return;
    imported.verifyTwins(codeBlock, restore, scratch->report());
    if (scratch->report().skips() || scratch->report().coincidences())
        JITCACHE_FAIL(makeString(what, ": verifyTwins reported a skip or a relocation coincidence"_s));
    auto differences = scratch->differences(context);
    if (!differences)
        return;

    Vector<String> reported;
    for (const auto& difference : *differences) {
        if (!reported.contains(difference.check))
            reported.append(difference.check);
    }
    Vector<String> wanted;
    for (ASCIILiteral check : expected)
        wanted.append(String { check });
    bool same = reported.size() == wanted.size();
    for (const String& check : wanted)
        same = same && reported.contains(check);
    if (same)
        return;
    StringBuilder lines;
    for (const auto& difference : *differences)
        lines.append('\n', difference.line);
    JITCACHE_FAIL(makeString(what, ": verifyTwins reported "_s, describeChecks(reported), "; expected "_s, describeChecks(wanted), lines.toString()));
}

// Takes a newborn CB of the function through the import's install sequence with the section, inside one deferral:
// prepare, seedLinkedState, beforeSetup(codeBlock), native setup with the producer's code, and
// finishCounter; then calls afterFinish(codeBlock, the prepared import, what finishCounter returned), which runs where
// the install function calls verifyTwins, before installCode.
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
    CBCounterRestore restore = prepared.finishCounter(*newborn);
    afterFinish(*newborn, prepared, restore);
}

// Writes a slice into the counter as finishCounter writes its result for the record, and resamples aging as it does, so
// that a fabricated result agrees with the counter and breaks only the rule the test aims at.
static void holdSlice(CodeBlock& codeBlock, const CounterTriple& record, int64_t slice)
{
    BaselineExecutionCounter& counter = codeBlock.baselineJITData()->executeCounter();
    double progress = static_cast<double>(record.totalCount) + record.value;
    counter.m_counter = static_cast<int32_t>(-slice);
    counter.m_totalCount = static_cast<float>(progress + static_cast<double>(slice));
    codeBlock.snapshotExecutionCounterForAging(static_cast<float>(counter.count()));
}

// The counter tests' body and its twin, with an argument, a value and an array profile.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }\n"
    "function elementTwin(array, index) { return array[index] + 1; }\n"
    "var array = [1, 2, 3];\n"_s;

// The subject of cbTwinsCheckEverySeededField, which has an entry in nearly every family: it is sloppy, so `this` gives
// a to_this; `array.length` a get_length; Array(n) a jneq_ptr and a new_array_with_size; the nested literals new_array
// and new_array_buffer of each copy-on-write type; for-of an iterator_open, an iterator_next and an
// iterator_close_check; for-in the enumerator opcodes; and the calls of helper two call sites.
#define CB_TWINS_TESTS_SUBJECT_BODY \
    "(array, object, n) {\n" \
    "    var sum = 0;\n" \
    "    for (var i = 0; i < array.length; ++i)\n" \
    "        sum += array[i];\n" \
    "    array[0] = sum;\n" \
    "    if (0 in array)\n" \
    "        sum += 1;\n" \
    "    for (var x of array)\n" \
    "        sum += x;\n" \
    "    for (var key in object) {\n" \
    "        if (key in object)\n" \
    "            sum += object[key];\n" \
    "        if (object.hasOwnProperty(key))\n" \
    "            object[key] = 1;\n" \
    "    }\n" \
    "    var made = Array(n);\n" \
    "    var pair = [sum, n];\n" \
    "    var literals = [[1, 2], [3, 4], [5, 6], [1.5, 2.5], [3.5, 4.5], [5.5, 6.5], [\"a\", \"b\"], [\"c\", \"d\"], [\"e\", \"f\"]];\n" \
    "    helper(pair);\n" \
    "    return helper(this) && made.length + pair.length + literals.length + sum;\n" \
    "}\n"

static constexpr auto subjectSource = "function helper(value) { return value; }\n"
    "function subject" CB_TWINS_TESTS_SUBJECT_BODY
    "function subjectTwin" CB_TWINS_TESTS_SUBJECT_BODY
    "var array = [1, 2, 3];\n"
    "var object = { a: 1, b: 2 };\n"_s;

#undef CB_TWINS_TESTS_SUBJECT_BODY

// Bodies without a metadata table, each with its twin: id has none at all, and has has a property IC, since
// in_by_id carries no metadata.
static constexpr auto metadataFreeSource = "function id(x) { return x; }\n"
    "function idTwin(x) { return x; }\n"
    "function has(o) { return \"x\" in o; }\n"
    "function hasTwin(o) { return \"x\" in o; }\n"
    "var text = \"text\";\n"
    "var object = { x: 1 };\n"
    "var other = { y: 2 };\n"_s;

// The sequence for one body without a metadata table: the producer runs in the LLInt once per argument and compiles to
// baseline; a capture holds no value profile, no family entry and both argument predictions, and scoreLive agrees with
// its score. The twin's newborn CB has no metadata table either, both walkers call nothing on it, and prepare,
// seedLinkedState, setup, finishCounter and verifyTwins run on it and report no difference.
static void checkBodyWithoutMetadataTable(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral producerName, ASCIILiteral twinName, std::initializer_list<JSValue> calls)
{
    VM& vm = globalObject->vm();
    JSFunction* producerFunction = globalFunction(context, globalObject, producerName);
    JSFunction* twin = globalFunction(context, globalObject, twinName);
    if (!producerFunction || !twin)
        return;
    for (JSValue argument : calls) {
        if (!callFunction(context, globalObject, producerFunction, { argument }))
            return;
    }
    CodeBlock* producer = compileToBaseline(context, producerFunction);
    if (!producer)
        return;
    if (producer->metadataTable()) {
        JITCACHE_FAIL(makeString(producerName, "'s baseline CB has a metadata table"_s));
        return;
    }

    CBScore liveScore = CBStateCapture::scoreLive(*producer, false);
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto captured = CBStateCapture::capture(*producer, budget.get(), false);
    if (!captured) {
        JITCACHE_FAIL(makeString(producerName, ": capture refused its charge"_s));
        return;
    }
    auto image = decodeState(context, captured->stateSection());
    if (!image)
        return;
    auto isEmptyFamily = [](uint32_t count) {
        return !count;
    };
    JITCACHE_CHECK(!image->header.numValueProfiles);
    JITCACHE_CHECK(std::ranges::all_of(image->header.familyEntryCount, isEmptyFamily));
    JITCACHE_CHECK(producer->numParameters() == 2);
    JITCACHE_CHECK(image->header.numArguments == producer->numParameters());
    std::span<const ArgumentValueProfile> producerProfiles = producer->argumentValueProfiles().span();
    if (image->argumentPredictions.size() != producerProfiles.size())
        JITCACHE_FAIL(makeString(producerName, ": the capture holds "_s, image->argumentPredictions.size(), " argument predictions for "_s, producerProfiles.size(), " argument profiles"_s));
    else {
        for (size_t index = 0; index < producerProfiles.size(); ++index) {
            JITCACHE_CHECK(image->argumentPredictions[index] == producerProfiles[index].m_prediction);
            JITCACHE_CHECK(image->argumentPredictions[index] != SpecNone);
        }
    }
    const CBScore& score = captured->score();
    JITCACHE_CHECK(liveScore.richnessUnits == score.richnessUnits);
    JITCACHE_CHECK(liveScore.counterWithheld == score.counterWithheld);
    JITCACHE_CHECK(liveScore.counterProgress == score.counterProgress);

    Ref<BaselineJITCode> code = baselineCodeOf(*producer);
    DeferGCForAWhile deferGC(vm);
    CodeBlock* newborn = createNewbornCodeBlock(context, twin);
    if (!newborn)
        return;
    if (newborn->metadataTable()) {
        JITCACHE_FAIL(makeString(twinName, "'s newborn CB has a metadata table"_s));
        return;
    }
    unsigned walkerCalls = 0;
    CBFormat::forEachMetadataValueProfile(*newborn, [&](unsigned, const ValueProfileRef&) {
        ++walkerCalls;
    });
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        CBFormat::forEachFamilyEntry<typename FamilyType::Op>(*newborn, [&](auto&) {
            ++walkerCalls;
        });
    });
    JITCACHE_CHECK(!walkerCalls);

    CBStateImport prepared = CBStateImport::prepare(captured->stateSection());
    prepared.seedLinkedState(*newborn);
    newborn->setupWithUnlinkedBaselineCode(code.copyRef());
    CBCounterRestore restore = prepared.finishCounter(*newborn);
    JITCACHE_CHECK(restore.carried);
    expectTwinChecks(context, makeString(producerName, "'s section on "_s, twinName, "'s newborn CB"_s), prepared, *newborn, restore, { });
}

} // namespace CBTwinsTestsInternal

using namespace CBTwinsTestsInternal;

// verifyTwins passes on the counter tests' CBs, each a newborn CB taken through prepare, seedLinkedState, setup and
// finishCounter with its own section, at startup deferral scales of 1 and 4, and reports each counter finishCounter must
// never leave: a threshold that differs from its record, an aging sample that is not the restored count, a native slice
// above C at a scale of 1, a crossed record whose P is below s * T - min(T, C) / 2, a scale other than the VM's, an
// infinite threshold that lost its captured slice, and a counter that did not travel whose threshold differs from
// setup's arming; a native slice above C at a scale of 4 is no difference. Each fabricated result comes with a counter
// that holds its slice, so it breaks one rule only. The test keeps the default options, concurrent JIT included, since
// the check calls nothing that reads the executable pool.
JITCACHE_TEST(cbTwinsCheckTheRestoredCounter, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    JSFunction* twin = globalFunction(context, globalObject, "elementTwin"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    if (!element || !twin || !array)
        return;
    CodeBlock* producer = bringToBaseline(context, globalObject, element, { *array, jsNumber(0) });
    if (!producer)
        return;
    auto image = captureState(context, *producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*producer);
    auto nothingBeforeSetup = [](CodeBlock&) { };
    auto sectionWith = [&](CBFormat::CounterMode mode, const CounterTriple& triple, uint16_t reoptimizations) {
        StateImage changed = *image;
        setCounter(changed, mode, triple);
        changed.header.reoptimizationRetryCounter = reoptimizations;
        return encodeState(context, changed);
    };

    // The counter tests' records, which every rule accepts.
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
        { "one split of P = 500.5"_s, { -500, 1000.5f, 2000 } },
        { "another split of P = 500.5"_s, { -1000, 1500.5f, 2000 } },
        { "P = 2 * T"_s, { -1000, 3000, 1000 } },
    };
    constexpr double scaled = 4;
    for (double scale : { 1.0, scaled }) {
        vm.setStartupJITDeferralScale(scale);
        for (const auto& carriedCase : carriedCases) {
            auto words = sectionWith(CBFormat::CounterMode::Carried, carriedCase.record, 0);
            if (!words)
                return;
            String what = makeString(carriedCase.name, " at a startup deferral scale of "_s, scale);
            restoreOnNewborn(context, twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
                expectTwinChecks(context, what, imported, codeBlock, restore, { });
            });
        }
        for (uint16_t reoptimizations : { 0, 3 }) {
            for (bool quickDFGTierUp : { false, true }) {
                auto words = sectionWith(CBFormat::CounterMode::NotCarried, { }, reoptimizations);
                if (!words)
                    return;
                String what = makeString("NotCarried with "_s, reoptimizations, " reoptimizations and the quick DFG bit "_s, quickDFGTierUp ? "set"_s : "clear"_s, " at a startup deferral scale of "_s, scale);
                restoreOnNewborn(context, twin, bytesOf(*words), code.get(), [&](CodeBlock& codeBlock) {
                    codeBlock.unlinkedCodeBlock()->setQuickDFGTierUp(quickDFGTierUp ? TriState::True : TriState::False);
                }, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
                    expectTwinChecks(context, what, imported, codeBlock, restore, { });
                });
            }
        }
    }
    vm.setStartupJITDeferralScale(1);

    // The records the reports start from: P = 10 under T = 1000, far below T - min(T, C) / 2 for any ceiling, and an
    // infinite threshold whose captured slice is 1000.
    constexpr CounterTriple shortOfThreshold { -990, 1000, 1000 };
    constexpr CounterTriple infiniteWithTotal { -1000, 1000, infinite };
    auto carriedShort = sectionWith(CBFormat::CounterMode::Carried, shortOfThreshold, 0);
    auto carriedInfinite = sectionWith(CBFormat::CounterMode::Carried, infiniteWithTotal, 0);
    auto notCarried = sectionWith(CBFormat::CounterMode::NotCarried, { }, 0);
    if (!carriedShort || !carriedInfinite || !notCarried)
        return;
    int64_t floorSlice = 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());

    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
        codeBlock.baselineJITData()->executeCounter().m_activeThreshold += 1;
        expectTwinChecks(context, "a threshold that differs from its record"_s, imported, codeBlock, restore, { "counter-threshold"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
        codeBlock.snapshotExecutionCounterForAging(static_cast<float>(codeBlock.baselineJITData()->executeCounter().count() + 1));
        expectTwinChecks(context, "an aging sample that is not the restored count"_s, imported, codeBlock, restore, { "counter-aging"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
        CBCounterRestore aboveCeiling { .carried = true, .crossed = false, .nativeSlice = ceiling + 1, .slice = std::max(ceiling + 1, floorSlice) };
        holdSlice(codeBlock, shortOfThreshold, aboveCeiling.slice);
        expectTwinChecks(context, "a native slice above C"_s, imported, codeBlock, aboveCeiling, { "counter-slice-range"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        CBCounterRestore earlyCrossing { .carried = true, .crossed = true, .nativeSlice = 0, .slice = floorSlice };
        holdSlice(codeBlock, shortOfThreshold, earlyCrossing.slice);
        expectTwinChecks(context, "a crossing whose P is below T - min(T, C) / 2"_s, imported, codeBlock, earlyCrossing, { "counter-early-crossing"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedInfinite), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        int64_t capturedSlice = -static_cast<int64_t>(infiniteWithTotal.value);
        CBCounterRestore lostSlice { .carried = true, .crossed = false, .nativeSlice = capturedSlice - 1, .slice = capturedSlice - 1 };
        holdSlice(codeBlock, infiniteWithTotal, lostSlice.slice);
        expectTwinChecks(context, "an infinite threshold that lost its captured slice"_s, imported, codeBlock, lostSlice, { "counter-captured-slice"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*notCarried), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
        codeBlock.baselineJITData()->executeCounter().m_activeThreshold += 1;
        expectTwinChecks(context, "a counter that did not travel, armed above setup's threshold"_s, imported, codeBlock, restore, { "counter-arming"_s });
    });

    // At a scale of 4, where P = 2 * T lies below 4 * T - min(T, C) / 2: a crossing of that record, a result that reports
    // a scale of 1, and a native slice above C, which a scaled slice may pass.
    constexpr CounterTriple twiceThreshold { -1000, 3000, 1000 };
    auto carriedTwice = sectionWith(CBFormat::CounterMode::Carried, twiceThreshold, 0);
    if (!carriedTwice)
        return;
    vm.setStartupJITDeferralScale(scaled);
    restoreOnNewborn(context, twin, bytesOf(*carriedTwice), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        CBCounterRestore earlyCrossing { .carried = true, .crossed = true, .nativeSlice = 0, .slice = floorSlice, .startupDeferralScale = scaled };
        holdSlice(codeBlock, twiceThreshold, earlyCrossing.slice);
        expectTwinChecks(context, "a crossing whose P is below 4 * T - min(T, C) / 2 at a scale of 4"_s, imported, codeBlock, earlyCrossing, { "counter-early-crossing"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
        CBCounterRestore unscaled { .carried = true, .crossed = false, .nativeSlice = ceiling, .slice = std::max(ceiling, floorSlice), .startupDeferralScale = 1 };
        holdSlice(codeBlock, shortOfThreshold, unscaled.slice);
        expectTwinChecks(context, "a result that reports a scale of 1 at a scale of 4"_s, imported, codeBlock, unscaled, { "counter-scale"_s });
    });
    restoreOnNewborn(context, twin, bytesOf(*carriedShort), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore&) {
        int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
        CBCounterRestore aboveCeiling { .carried = true, .crossed = false, .nativeSlice = ceiling + 1, .slice = std::max(ceiling + 1, floorSlice), .startupDeferralScale = scaled };
        holdSlice(codeBlock, shortOfThreshold, aboveCeiling.slice);
        expectTwinChecks(context, "a native slice above C at a scale of 4"_s, imported, codeBlock, aboveCeiling, { });
    });
    vm.setStartupJITDeferralScale(1);
}

// `function id(x) { return x; }` gets no metadata table, and `function has(o) { return "x" in o; }` has a
// property IC and no metadata table either. id is called with an int and a string, has with two objects.
JITCACHE_TEST(cbTwinsReadNoMetadataTable, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, metadataFreeSource);
    if (!globalObject)
        return;
    JSValue text = globalValue(globalObject, "text"_s);
    auto object = globalObjectValue(context, globalObject, "object"_s);
    auto other = globalObjectValue(context, globalObject, "other"_s);
    if (!text.isString()) {
        JITCACHE_FAIL("the test source defines no string text"_s);
        return;
    }
    if (!object || !other)
        return;
    checkBodyWithoutMetadataTable(context, globalObject, "id"_s, "idTwin"_s, { jsNumber(1), text });
    checkBodyWithoutMetadataTable(context, globalObject, "has"_s, "hasTwin"_s, { *object, *other });
}

// verifyTwins on a section that seeds every family the subject has: a capture of its baseline CB, with FastMap added at
// its first iterator_open site, so that the realm step runs, and lazy-operand records that repeat a key, so that
// the holder collapses them. On the twin's newborn CB the check reports nothing. Then, each time on a fresh newborn CB, a
// seeded field changed after finishCounter, a sample written beside a seeded field, or a native field the lane must not
// write is reported under its own check and nothing else.
JITCACHE_TEST(cbTwinsCheckEverySeededField, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, subjectSource);
    if (!globalObject)
        return;
    JSFunction* subjectFunction = globalFunction(context, globalObject, "subject"_s);
    JSFunction* twin = globalFunction(context, globalObject, "subjectTwin"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    auto object = globalObjectValue(context, globalObject, "object"_s);
    if (!subjectFunction || !twin || !array || !object)
        return;
    CodeBlock* producer = bringToBaseline(context, globalObject, subjectFunction, { *array, *object, jsNumber(3) });
    if (!producer)
        return;
    auto image = captureState(context, *producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*producer);

    constexpr unsigned iteratorOpenFamily = CBFormat::iteratorOpenIterationModesFamily;
    if (!image->header.familyEntryCount[iteratorOpenFamily]) {
        JITCACHE_FAIL("the subject has no iterator_open site"_s);
        return;
    }
    uint16_t& iteratorOpenModes = image->iterationModes[recordIndex(*image, iteratorOpenFamily, 0)];
    iteratorOpenModes = static_cast<uint16_t>(iteratorOpenModes | static_cast<uint16_t>(IterationMode::FastMap));
    uint32_t firstOffset = BytecodeIndex(0).asBits();
    int32_t firstArgument = virtualRegisterForArgumentIncludingThis(1).offset();
    setLazyOperands(*image, {
        lazyOperandRecord(firstOffset, argumentKind, firstArgument, SpecInt32Only),
        lazyOperandRecord(firstOffset, localKind, virtualRegisterForLocal(64).offset(), SpecOther),
        lazyOperandRecord(firstOffset, tmpKind, 3, SpecBoolean),
        lazyOperandRecord(firstOffset, argumentKind, firstArgument, SpecBoolean),
    });
    auto words = encodeState(context, *image);
    if (!words)
        return;
    std::span<const uint8_t> section = bytesOf(*words);
    auto nothingBeforeSetup = [](CodeBlock&) { };

    restoreOnNewborn(context, twin, section, code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
        JITCACHE_CHECK(globalObject->mapProtoEntriesFunctionConcurrently());
#if ENABLE(DFG_JIT)
        unsigned lazyProfiles = 0;
        codeBlock.lazyValueProfiles().forEachOperandValueProfile([&](const LazyOperandValueProfile&) {
            ++lazyProfiles;
        });
        JITCACHE_CHECK(lazyProfiles == 3);
#endif
        expectTwinChecks(context, "a section that seeds every family"_s, imported, codeBlock, restore, { });
    });

    // Restores the section on a fresh newborn CB, applies change(codeBlock), which returns whether it found a field to
    // change, and expects verifyTwins to report exactly `check`.
    auto expectReported = [&](ASCIILiteral what, ASCIILiteral check, const auto& change) {
        restoreOnNewborn(context, twin, section, code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
            if (!change(codeBlock)) {
                JITCACHE_FAIL(makeString(what, ": the CB has no such field"_s));
                return;
            }
            expectTwinChecks(context, what, imported, codeBlock, restore, { check });
        });
    };
    StructureID arrayStructureID = asObject(*array)->structureID();

    expectReported("a value prediction"_s, "value-prediction"_s, [](CodeBlock& codeBlock) {
        bool changed = false;
        // A ValueProfileRef only merges into its prediction, so the bit is flipped in the table's predictions, which the
        // walker visits only when the table exists.
        CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, const ValueProfileRef&) {
            if (offset != 1)
                return;
            codeBlock.metadataTable()->ensureValueProfilePredictions()[offset - 1] ^= SpecInt32Only;
            changed = true;
        });
        return changed;
    });
    expectReported("an argument sample"_s, "argument-samples"_s, [](CodeBlock& codeBlock) {
        std::span<ArgumentValueProfile> profiles = codeBlock.argumentValueProfiles().mutableSpan();
        if (profiles.empty())
            return false;
        std::span { profiles[0].m_buckets }.front() = JSValue::encode(jsNumber(1));
        return true;
    });
#if ENABLE(DFG_JIT)
    expectReported("a lazy-operand prediction"_s, "lazy-operand-profile"_s, [&](CodeBlock& codeBlock) {
        LazyOperandValueProfileKey key(BytecodeIndex(0), Operand(OperandKind::Argument, firstArgument));
        codeBlock.lazyValueProfiles().addOperandValueProfile(key)->m_prediction ^= SpecInt32Only;
        return true;
    });
#endif
    expectReported("an array profile's StructureID sample"_s, "array-profile-samples"_s, [&](CodeBlock& codeBlock) {
        bool changed = false;
        CBFormat::forEachFamily([&](auto family) {
            using FamilyType = decltype(family);
            if constexpr (FamilyType::kind == CBFormat::FamilyKind::ArrayProfile && FamilyType::hasField) {
                CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](ArrayProfile& profile) {
                    if (changed)
                        return;
                    profile.observeStructureID(arrayStructureID);
                    changed = true;
                });
            }
        });
        return changed;
    });
#if CPU(ADDRESS64)
    JSArray* lastArray = dynamicDowncast<JSArray>(*array);
    expectReported("an allocation profile's last array"_s, "allocation-last-array"_s, [&](CodeBlock& codeBlock) {
        bool changed = false;
        CBFormat::forEachFamily([&](auto family) {
            using FamilyType = decltype(family);
            if constexpr (FamilyType::kind == CBFormat::FamilyKind::AllocationHint) {
                CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](ArrayAllocationProfile& profile) {
                    if (changed || !lastArray)
                        return;
                    profile.updateLastAllocation(lastArray);
                    changed = true;
                });
            }
        });
        return changed;
    });
#endif
    expectReported("an iterator_open site's modes"_s, "iteration-modes"_s, [](CodeBlock& codeBlock) {
        bool changed = false;
        CBFormat::forEachFamilyEntry<OpIteratorOpen>(codeBlock, [&](OpIteratorOpen::Metadata& metadata) {
            if (changed)
                return;
            metadata.m_iterationMetadata.seenModes = static_cast<uint16_t>(metadata.m_iterationMetadata.seenModes ^ static_cast<uint16_t>(IterationMode::Generic));
            changed = true;
        });
        return changed;
    });
    expectReported("a get_length LLInt cache"_s, "llint-cache"_s, [](CodeBlock& codeBlock) {
        bool changed = false;
        CBFormat::forEachFamilyEntry<OpGetLength>(codeBlock, [&](OpGetLength::Metadata& metadata) {
            if (changed)
                return;
            metadata.m_modeMetadata.setArrayLengthMode();
            changed = true;
        });
        return changed;
    });
    expectReported("a to_this cached structure"_s, "to-this-structure"_s, [&](CodeBlock& codeBlock) {
        bool changed = false;
        CBFormat::forEachFamilyEntry<OpToThis>(codeBlock, [&](OpToThis::Metadata& metadata) {
            if (changed)
                return;
            metadata.m_cachedStructureID = arrayStructureID;
            changed = true;
        });
        return changed;
    });

    // A section whose value records are all SpecNone seeds no prediction, so the metadata table gets none; predictions
    // allocated anyway are reported, since no record asked for them.
    if (image->valuePredictions.isEmpty() || !Options::useLazyValueProfilePredictions()) {
        JITCACHE_FAIL("the subject has no value profile, or every metadata table allocates its predictions when it is linked"_s);
        return;
    }
    StateImage withoutPredictions = *image;
    for (auto& prediction : withoutPredictions.valuePredictions)
        prediction = SpecNone;
    auto withoutPredictionsWords = encodeState(context, withoutPredictions);
    if (!withoutPredictionsWords)
        return;
    restoreOnNewborn(context, twin, bytesOf(*withoutPredictionsWords), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CBStateImport& imported, const CBCounterRestore& restore) {
        expectTwinChecks(context, "a section whose value records are all SpecNone"_s, imported, codeBlock, restore, { });
        codeBlock.metadataTable()->ensureValueProfilePredictions();
        expectTwinChecks(context, "predictions no value record asked for"_s, imported, codeBlock, restore, { "value-predictions-array"_s });
    });
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
