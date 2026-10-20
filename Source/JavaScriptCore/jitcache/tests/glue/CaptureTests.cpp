#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "ArtifactStore.h"
#include "CBFormat.h"
#include "CBState.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "ICSection.h"
#include "JIT.h"
#include "JITCacheAPI.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCacheParameters.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ModuleProgramExecutable.h"
#include "ReleaseHeapAccessScope.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "TestArtifact.h"
#include "TestFiles.h"
#include "TestVM.h"
#include "UCBFeedback.h"
#include "UCBRegistry.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "VMEntryScopeInlines.h"
#include "ValidatedBody.h"
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of JITCacheCapture (glue/JITCacheCapture.h and .cpp): how beats orders capture scores, scoreSections over a saved
// body's summary sections, delta's rejections, commits and faults, what a commit charges the producer budget, the counter
// a commit stamps, and delta's capture of bodies that ran without compiling, from the producer's commit at tier 0 to the
// consumer's import. Each test that starts JITCache runs strict on a temporary directory.

namespace JSC::JITCache::Tests {

namespace CaptureTestsInternal {

#if ENABLE(JIT)

// The capture tests' bodies: two functions with one in_by_id site each and no other IC, and the objects that reach them.
// shapeA1 and shapeA2 come from one allocation site, so they share a structure, and shapeB has another. The source keeps
// every object and function in its globals, and the global object stays reachable from the test's stack, so no
// collection resets a case between two reads of the same state.
constexpr ASCIILiteral captureTestSource = "function makeA(value) { return { x: value }; }\n"
    "var shapeA1 = makeA(1);\n"
    "var shapeA2 = makeA(2);\n"
    "var shapeB = { y: 3, x: 4 };\n"
    "function firstBody(o) { return \"x\" in o; }\n"
    "function secondBody(o) { return \"x\" in o; }\n"_s;

// How failure messages name captureTestSource.
constexpr ASCIILiteral captureSourceName = "the capture tests' source"_s;

// A global object of its own over captureTestSource, evaluated after start, so the program and its functions have keys.
static JSGlobalObject* createCaptureRealm(TestContext& context, VM& vm)
{
    return createRealm(context, vm, captureTestSource, captureSourceName);
}

static bool callWith(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, JSValue argument)
{
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    NakedPtr<Exception> exception;
    JSC::call(globalObject, function, getCallData(function), jsUndefined(), arguments, exception);
    if (exception) {
        JITCACHE_FAIL("calling a capture test's function threw"_s);
        return false;
    }
    return true;
}

// The function's baseline CB: one call installs the LLInt CB, and JIT::compileSync, with collection deferred as at the end
// of any compilation, compiles it and runs BaselineJITPlan::finalize, whose finalize capture commits the body in a
// producing VM. It checks less than TestVM.h's bringToBaseline, which also requires the CB's JIT data.
static CodeBlock* bringToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, JSValue argument)
{
    VM& vm = globalObject->vm();
    if (!callWith(context, globalObject, function, argument))
        return nullptr;
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not install an LLInt CB"_s);
        return nullptr;
    }
    CompilationResult result;
    {
        DeferGCForAWhile deferGC(vm);
        result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    }
    if (result != CompilationResult::CompilationSuccessful || codeBlock->jitType() != JITType::BaselineJIT || function->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("JIT::compileSync did not install baseline code in the LLInt CB"_s);
        return nullptr;
    }
    return codeBlock;
}

// The key the parent-key registry recorded for the CB's UCB.
static std::optional<BodyKey> recordedKey(TestContext& context, VM& vm, CodeBlock& codeBlock)
{
    auto key = vm.jitCacheState()->bodyKeyOf(*codeBlock.unlinkedCodeBlock());
    if (!key)
        JITCACHE_FAIL("the parent-key registry holds no key for a capture test's body"_s);
    return key;
}

#endif // ENABLE(JIT)

} // namespace CaptureTestsInternal

using namespace CaptureTestsInternal;

// beats compares scores field by field, tier, richness, IC sites with cases, a withheld counter above one that travels,
// then the counter's progress, and a candidate wins only when it is strictly greater, so a tie keeps the saved body.
JITCACHE_TEST(integratorBeatsOrdersScores, No)
{
    const CaptureScore saved { 1, 10, 2, false, 50 };
    JITCACHE_CHECK(!beats(saved, saved));

    struct OneField {
        ASCIILiteral field;
        CaptureScore higher;
        CaptureScore lower;
    };
    std::array oneFieldChanges {
        OneField { "tier"_s, { 2, 10, 2, false, 50 }, { 0, 10, 2, false, 50 } },
        OneField { "richness"_s, { 1, 11, 2, false, 50 }, { 1, 9, 2, false, 50 } },
        OneField { "icSitesWithCases"_s, { 1, 10, 3, false, 50 }, { 1, 10, 1, false, 50 } },
        OneField { "counterWithheld"_s, { 1, 10, 2, true, 50 }, { 1, 10, 2, false, 50 } },
        OneField { "counterProgress"_s, { 1, 10, 2, false, 51 }, { 1, 10, 2, false, 49 } },
    };
    for (auto& [field, higher, lower] : oneFieldChanges) {
        // The counterWithheld row's lower score is the saved one itself, which ties.
        bool lowerTies = sameScore(lower, saved);
        if (!beats(higher, saved) || beats(saved, higher) || beats(lower, saved) || (!lowerTies && !beats(saved, lower)))
            JITCACHE_FAIL(makeString("beats misorders scores that differ only in "_s, field));
    }

    // A withheld counter beats one that travels with more progress, at equal richness and IC sites, and loses the reverse.
    const CaptureScore withheld { 1, 10, 2, true, 0 };
    const CaptureScore travels { 1, 10, 2, false, 1000 };
    JITCACHE_CHECK(beats(withheld, travels));
    JITCACHE_CHECK(!beats(travels, withheld));

    // An earlier field decides before every later one.
    JITCACHE_CHECK(beats({ 2, 0, 0, false, 0 }, { 1, 100, 100, true, 1000 }));
    JITCACHE_CHECK(beats({ 1, 11, 0, false, 0 }, { 1, 10, 100, true, 1000 }));
    JITCACHE_CHECK(beats({ 1, 10, 3, false, 0 }, { 1, 10, 2, true, 1000 }));
    JITCACHE_CHECK(!beats({ 1, 9, 100, true, 1000 }, { 1, 10, 0, false, 0 }));

    // Any baseline capture beats the empty score that stands for an absent body.
    JITCACHE_CHECK(beats({ 1, 0, 0, false, 0 }, CaptureScore { }));
    JITCACHE_CHECK(!beats(CaptureScore { }, CaptureScore { }));
}

#if ENABLE(JIT)

// scoreSections over a committed body's three summary sections gives its kept score in both modes, and with strict on, a
// malformed feedback, CB summary or ICs span, the others whole, names the reader that rejected it, with its part and
// check.
JITCACHE_TEST(integratorScoreSectionsNamesTheReader, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* function = globalObject ? globalFunction(context, globalObject, "firstBody"_s, captureSourceName) : nullptr;
    CodeBlock* codeBlock = function ? bringToBaseline(context, globalObject, function, globalValue(globalObject, "shapeA1"_s)) : nullptr;
    std::optional<BodyKey> key = codeBlock ? recordedKey(context, vm, *codeBlock) : std::nullopt;
    if (!key)
        return;

    // The finalize capture committed the body and kept the score of the bytes it built.
    std::optional<SavedScore> kept = keptScoreForTesting(vm, *key);
    BodyLookup lookup = state.openBody(*key);
    RefPtr<ValidatedBody> body = lookup.body();
    if (!kept || !body) {
        JITCACHE_FAIL(makeString("the finalize capture left no "_s, kept ? "body"_s : "kept summary"_s));
        return;
    }
    JITCACHE_CHECK(body->version() == kept->version);
    std::span<const uint8_t> feedback = body->section(SectionKind::UCBFeedback);
    std::span<const uint8_t> summary = body->section(SectionKind::CBSummaryBaseline);
    std::span<const uint8_t> ics = body->section(SectionKind::ICsBaseline);
    for (bool strict : { true, false }) {
        auto score = scoreSections(body->highestTier(), feedback, summary, ics, strict);
        if (!score || !sameScore(*score, kept->score))
            JITCACHE_FAIL(makeString("scoreSections with strict "_s, strict ? "on"_s : "off"_s, " differs from the kept score "_s, scoreText(kept->score)));
    }
    if (feedback.empty() || summary.empty() || ics.empty()) {
        JITCACHE_FAIL("a committed body lacks a summary section"_s);
        return;
    }

    auto checkRejected = [&](ASCIILiteral label, std::span<const uint8_t> feedbackSpan, std::span<const uint8_t> summarySpan, std::span<const uint8_t> icsSpan,
        ASCIILiteral part, ASCIILiteral check) {
        auto score = scoreSections(body->highestTier(), feedbackSpan, summarySpan, icsSpan, true);
        if (score) {
            JITCACHE_FAIL(makeString(label, ": scoreSections accepted it"_s));
            return;
        }
        if (score.error().part != part || score.error().check != check)
            JITCACHE_FAIL(makeString(label, ": rejected by "_s, score.error().part, " at "_s, score.error().check, ", expected "_s, part, " at "_s, check));
    };

    // Each span one byte short of its layout, the others whole.
    checkRejected("a short ucb.feedback"_s, feedback.first(feedback.size() - 1), summary, ics, "ucb"_s, "saved-summary"_s);
    auto shortSummary = summary.first(summary.size() - 1);
    auto summaryCheck = decodeSummary(shortSummary, true);
    JITCACHE_CHECK(!summaryCheck);
    if (!summaryCheck)
        checkRejected("a short cb.summary"_s, feedback, shortSummary, ics, "cb"_s, description(summaryCheck.error().check));
    checkRejected("a short ICsBaseline"_s, feedback, summary, ics.first(ics.size() - 1), "ics"_s, ICs::description(ICs::Check::SectionSize));

    // An ICs header that counts more IC sites with cases than ICs fails the bound the ICs lane puts on that count.
    Vector<uint8_t> unbounded;
    unbounded.append(ics);
    ICs::SectionHeader header;
    memcpySpan(asMutableByteSpan(header), unbounded.span().first(sizeof(header)));
    header.icSitesWithCases = header.propertyICCount + 1;
    memcpySpan(unbounded.mutableSpan().first(sizeof(header)), asByteSpan(header));
    checkRejected("an ICsBaseline with more IC sites with cases than ICs"_s, feedback, summary, unbounded.span(), "ics"_s, ICs::description(ICs::Check::SummaryBound));
}

// delta rejects at each step a test can reach, commits a body in a Producer, and reports Faulted after a recording fault.
JITCACHE_TEST(integratorDeltaRejectsCommitsAndFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    auto checkRejected = [&](ASCIILiteral label, const DeltaResult& result, ASCIILiteral step) {
        if (result.outcome != DeltaOutcome::Rejected || result.rejection != step || result.fault || result.eligibleKeys || result.committedBodies)
            JITCACHE_FAIL(makeString(label, ": expected a rejection at "_s, step, ", got "_s, toJSON(result)));
    };

    checkRejected("a VM without state"_s, JITCache::delta(vm), "delta.unconfigured"_s);
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(artifact, Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    {
        ExtraVM consumer;
        if (startsAs(context, "a consumer"_s, JITCache::start(consumer.vm(), strictConfig(artifact, Role::Consumer)), StartOutcome::Started))
            checkRejected("a consumer"_s, JITCache::delta(consumer.vm()), "delta.role"_s);
    }
    {
        ReleaseHeapAccessScope withoutHeapAccess(vm.heap);
        checkRejected("a call without heap access"_s, JITCache::delta(vm), "delta.locks"_s);
    }

    // The finalize capture commits the body with no IC site with cases. In baseline code the first call spends the
    // site's countdown and the second caches a case, so delta's capture beats the saved body.
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* function = globalObject ? globalFunction(context, globalObject, "firstBody"_s, captureSourceName) : nullptr;
    JSValue shape = globalObject ? globalValue(globalObject, "shapeA1"_s) : JSValue();
    CodeBlock* codeBlock = function ? bringToBaseline(context, globalObject, function, shape) : nullptr;
    std::optional<BodyKey> key = codeBlock ? recordedKey(context, vm, *codeBlock) : std::nullopt;
    if (!key || !callWith(context, globalObject, function, shape) || !callWith(context, globalObject, function, shape))
        return;
    std::optional<SavedScore> finalizeCapture = keptScoreForTesting(vm, *key);
    JITCACHE_CHECK(finalizeCapture && !finalizeCapture->score.icSitesWithCases);

    Progress before = state.progress();
    DeltaResult committed = JITCache::delta(vm);
    JITCACHE_CHECK(committed.outcome == DeltaOutcome::Completed);
    JITCACHE_CHECK(committed.rejection.isNull() && !committed.fault);
    JITCACHE_CHECK(committed.eligibleKeys == 1);
    JITCACHE_CHECK(committed.committedBodies == 1);
    JITCACHE_CHECK(!committed.deferredKeys);
    JITCACHE_CHECK(committed.committedBytes && committed.committedBytes == state.progress().bytesCommitted - before.bytesCommitted);
    JITCACHE_CHECK(state.progress().capturesCommitted == before.capturesCommitted + 1);
    JITCACHE_CHECK(state.progress().deltaRuns == before.deltaRuns + 1);
    std::optional<SavedScore> deltaCapture = keptScoreForTesting(vm, *key);
    JITCACHE_CHECK(deltaCapture && deltaCapture->score.icSitesWithCases == 1);
    JITCACHE_CHECK(deltaCapture && finalizeCapture && deltaCapture->version != finalizeCapture->version);

    // A recording fault ends production: delta returns Faulted with it and commits nothing.
    state.raiseRecordingFault("cb"_s, "test-check"_s, { });
    DeltaResult faulted = JITCache::delta(vm);
    JITCACHE_CHECK(faulted.outcome == DeltaOutcome::Faulted);
    JITCACHE_CHECK(!faulted.committedBodies && !faulted.committedBytes);
    checkFault(context, "delta's fault"_s, faulted.fault, FaultClass::RecordingFault, "cb.test-check"_s);
    JITCACHE_CHECK(state.progress().deltaRuns == before.deltaRuns + 1);
}

// Invalid material raised inside a build stops delta. A Producer with the delta capture All holds a module UCB that ran,
// recorded without an analysis source, beside the run-once source's bodies, which ran too, under a lookup override whose
// open, asked for the module's body, raises invalid material at container.io, as the read of a damaged saved ucb.analysis
// would, notes how many bodies the index lists and answers Unusable. delta returns Faulted with that fault, the status
// holds no other, and the index lists exactly the bodies it listed when the override raised: the module's capture wrote
// nothing, and no key after it committed.
JITCACHE_TEST(integratorDeltaStopsAfterInvalidMaterialInABuild, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer, DeltaCapture::All)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createRunOnceRealm(context, vm);
    if (!globalObject)
        return;

    // A module requested without a linker keeps no analysis source, so its capture reads the saved body's section through
    // readBodySection. Its LLInt counter, deferred as the LLInt's tier-up leaves it when the gate refuses a body, shows
    // that it ran.
    SourceCode moduleSource = makeSource("export function jitcacheDeltaModule() { return 1; }\n"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { },
        TextPosition { }, SourceProviderSourceType::Module);
    Strong<ModuleProgramExecutable> moduleExecutable;
    {
        VMEntryScope entryScope(vm, globalObject);
        moduleExecutable = Strong<ModuleProgramExecutable> { vm, ModuleProgramExecutable::tryCreate(globalObject, moduleSource) };
    }
    UnlinkedCodeBlock* moduleCodeBlock = moduleExecutable ? moduleExecutable.get()->unlinkedCodeBlock() : nullptr;
    std::optional<BodyKey> moduleKey = moduleCodeBlock ? state.bodyKeyOf(*moduleCodeBlock) : std::nullopt;
    if (!moduleKey || state.registry().moduleAnalysisSource(*moduleCodeBlock)) {
        JITCACHE_FAIL("the module was not recorded, or was recorded with an analysis source"_s);
        return;
    }
    moduleCodeBlock->llintExecuteCounter().deferIndefinitely();

    std::optional<uint64_t> indexedAtTheRaise;
    state.setBodyLookupForTesting([&](const BodyKey& key) {
        return state.artifact()->token(key);
    }, [&](const BodyKey& key) {
        if (!(key == *moduleKey))
            return BodyLookup::missing();
        indexedAtTheRaise = state.artifact()->indexedBodies();
        state.raiseInvalidMaterial("container.io"_s, makeString(errnoText(EIO), " reading the module's saved ucb.analysis"_s));
        return BodyLookup::unusable();
    });
    DeltaResult result = JITCache::delta(vm);
    state.clearBodyLookupForTesting();

    JITCACHE_CHECK(result.outcome == DeltaOutcome::Faulted);
    checkFault(context, "delta's fault"_s, result.fault, FaultClass::InvalidMaterial, "container.io"_s);
    Status status = JITCache::status(vm);
    checkFault(context, "the activity fault"_s, status.activityFault, FaultClass::InvalidMaterial, "container.io"_s);
    checkFault(context, "the production fault"_s, status.productionFault, FaultClass::InvalidMaterial, "container.io"_s);
    checkFault(context, "the first fault"_s, status.firstFault, FaultClass::InvalidMaterial, "container.io"_s);
    if (!indexedAtTheRaise) {
        JITCACHE_FAIL("the module's capture never read its saved ucb.analysis"_s);
        return;
    }
    JITCACHE_CHECK(state.artifact()->indexedBodies() == *indexedAtTheRaise);
    JITCACHE_CHECK(result.committedBodies == *indexedAtTheRaise);
    JITCACHE_CHECK(!state.artifact()->token(*moduleKey));
    JITCACHE_CHECK(!state.progress().deltaRuns);
}

#endif // ENABLE(JIT)

// A refusal made off the VM thread reaches it at the next delta, which raises budget.limit and returns Faulted with it.
JITCACHE_TEST(integratorDeltaRaisesAnUnraisedRefusal, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(testProducerLimitBytes + 1));
    JITCACHE_CHECK(state.productionActive());
    DeltaResult result = JITCache::delta(vm);
    JITCACHE_CHECK(result.outcome == DeltaOutcome::Faulted);
    checkFault(context, "delta's fault"_s, result.fault, FaultClass::RecordingFault, "budget.limit"_s);
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!state.progress().deltaRuns);
}

#if ENABLE(JIT)

// Once the writer's staging buffer and the kept-summary table exist, a commit of a key the index lacks charges exactly six
// of the kept-summary table's bucket sizes and six of the index's, and a later commit of the same key charges neither. A
// recording fault then releases the kept summaries', the index entries' and the staging buffer's charges at the next
// glue entry, while both keys stay in the index.
JITCACHE_TEST(integratorCommitChargesItsEntries, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* firstFunction = globalObject ? globalFunction(context, globalObject, "firstBody"_s, captureSourceName) : nullptr;
    JSFunction* secondFunction = globalObject ? globalFunction(context, globalObject, "secondBody"_s, captureSourceName) : nullptr;
    if (!firstFunction || !secondFunction)
        return;
    JSValue shape = globalValue(globalObject, "shapeA1"_s);
    auto chargedBytes = [&] {
        return static_cast<uint64_t>(JITCache::status(vm).budget.chargedBytes);
    };

    // The first key's finalize capture commits it, which allocates the staging buffer and creates the kept summaries.
    CodeBlock* firstCodeBlock = bringToBaseline(context, globalObject, firstFunction, shape);
    std::optional<BodyKey> firstKey = firstCodeBlock ? recordedKey(context, vm, *firstCodeBlock) : std::nullopt;
    if (!firstKey)
        return;
    JITCACHE_CHECK(state.progress().capturesCommitted == 1);
    JITCACHE_CHECK(state.keptSummaries());

    // The second key's scoring read finds the body unavailable, so its finalize capture defers and delta commits it first.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::ReadSavedSummaries, EMFILE, std::nullopt });
    CodeBlock* secondCodeBlock = bringToBaseline(context, globalObject, secondFunction, shape);
    StoreTesting::setFault(std::nullopt);
    std::optional<BodyKey> secondKey = secondCodeBlock ? recordedKey(context, vm, *secondCodeBlock) : std::nullopt;
    if (!secondKey)
        return;
    JITCACHE_CHECK(state.progress().capturesCommitted == 1);
    JITCACHE_CHECK(state.progress().capturesDeferred == 1);
    JITCACHE_CHECK(state.bodyVersion(*firstKey));
    JITCACHE_CHECK(!state.bodyVersion(*secondKey));
    JITCACHE_CHECK(!keptScoreForTesting(vm, *secondKey));

    constexpr uint64_t keptBucketBytes = sizeof(KeyValuePair<BodyKey, SavedScore>);
    constexpr uint64_t indexBucketBytes = sizeof(PackIndex::KeyValuePairType);
    uint64_t beforeFirstCommit = chargedBytes();
    DeltaResult firstCommit = JITCache::delta(vm);
    JITCACHE_CHECK(firstCommit.outcome == DeltaOutcome::Completed && firstCommit.committedBodies == 1);
    std::optional<SavedScore> firstCommitScore = keptScoreForTesting(vm, *secondKey);
    JITCACHE_CHECK(firstCommitScore);
    JITCACHE_CHECK(state.bodyVersion(*secondKey));
    uint64_t afterFirstCommit = chargedBytes();
    if (afterFirstCommit != beforeFirstCommit + 6 * keptBucketBytes + 6 * indexBucketBytes)
        JITCACHE_FAIL(makeString("the second key's first commit charged "_s, static_cast<int64_t>(afterFirstCommit - beforeFirstCommit), " bytes, expected "_s, 6 * keptBucketBytes + 6 * indexBucketBytes));

    // Two calls cache a case at the second key's site, which its saved body lacks, so delta commits the key again.
    if (!callWith(context, globalObject, secondFunction, shape) || !callWith(context, globalObject, secondFunction, shape))
        return;
    DeltaResult secondCommit = JITCache::delta(vm);
    JITCACHE_CHECK(secondCommit.outcome == DeltaOutcome::Completed && secondCommit.committedBodies == 1);
    std::optional<SavedScore> secondCommitScore = keptScoreForTesting(vm, *secondKey);
    JITCACHE_CHECK(secondCommitScore && firstCommitScore && secondCommitScore->version != firstCommitScore->version);
    JITCACHE_CHECK(chargedBytes() == afterFirstCommit);

    // A fault entry point frees nothing; the next glue entry releases the kept summaries' table and two entries, the two
    // index entries' charges and the staging buffer. What stays charged is the two image records the bodies' code holds.
    state.raiseRecordingFault("cb"_s, "test-check"_s, { });
    JITCACHE_CHECK(chargedBytes() == afterFirstCommit);
    JITCACHE_CHECK(JITCache::delta(vm).outcome == DeltaOutcome::Faulted);
    uint64_t released = (8 + 2 * 6) * keptBucketBytes + 2 * 6 * indexBucketBytes + writerStagingBytes;
    if (chargedBytes() + released != afterFirstCommit)
        JITCACHE_FAIL(makeString("the end of production released "_s, static_cast<int64_t>(afterFirstCommit - chargedBytes()), " bytes, expected "_s, released));
    JITCACHE_CHECK(!state.keptSummaries());
    JITCACHE_CHECK(state.bodyVersion(*firstKey));
    JITCACHE_CHECK(state.bodyVersion(*secondKey));
}

// Of two bodies whose counters have made progress, the one whose in_by_id site lists two cases has a polymorphic site, so
// delta commits it with its counter withheld: P 0 in the envelope and NotCarried in cb.state and cb.summary. The other
// carries its counter, and its envelope's P is its cb.summary's progress. Each kept score equals scoreSections of the
// sections openBody returns for the body.
JITCACHE_TEST(integratorDeltaStampsTheCounter, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* polymorphicFunction = globalObject ? globalFunction(context, globalObject, "firstBody"_s, captureSourceName) : nullptr;
    JSFunction* monomorphicFunction = globalObject ? globalFunction(context, globalObject, "secondBody"_s, captureSourceName) : nullptr;
    if (!polymorphicFunction || !monomorphicFunction)
        return;
    JSValue shapeA1 = globalValue(globalObject, "shapeA1"_s);
    JSValue shapeA2 = globalValue(globalObject, "shapeA2"_s);
    JSValue shapeB = globalValue(globalObject, "shapeB"_s);
    CodeBlock* polymorphic = bringToBaseline(context, globalObject, polymorphicFunction, shapeA1);
    CodeBlock* monomorphic = bringToBaseline(context, globalObject, monomorphicFunction, shapeA1);
    if (!polymorphic || !monomorphic)
        return;

    // In baseline code each site's first visit spends its countdown and each later one with a new shape caches it: shapes
    // A, A and B leave two cases, and A three times leaves one. Each call adds an entry's points to the baseline counter.
    for (JSValue shape : { shapeA1, shapeA2, shapeB }) {
        if (!callWith(context, globalObject, polymorphicFunction, shape))
            return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!callWith(context, globalObject, monomorphicFunction, shapeA1))
            return;
    }
    DeltaResult result = JITCache::delta(vm);
    JITCACHE_CHECK(result.outcome == DeltaOutcome::Completed);
    JITCACHE_CHECK(result.committedBodies == 2);

    struct Expected {
        ASCIILiteral label;
        CodeBlock* codeBlock;
        bool counterWithheld;
    };
    for (auto& [label, codeBlock, counterWithheld] : std::array { Expected { "the polymorphic body"_s, polymorphic, true }, Expected { "the monomorphic body"_s, monomorphic, false } }) {
        std::optional<BodyKey> key = recordedKey(context, vm, *codeBlock);
        if (!key)
            return;
        std::optional<SavedScore> kept = keptScoreForTesting(vm, *key);
        BodyLookup lookup = state.openBody(*key);
        RefPtr<ValidatedBody> body = lookup.body();
        if (!kept || !body) {
            JITCACHE_FAIL(makeString(label, " has no "_s, kept ? "body"_s : "kept summary"_s));
            continue;
        }
        JITCACHE_CHECK(body->version() == kept->version);
        JITCACHE_CHECK(kept->score.counterWithheld == counterWithheld);

        std::span<const uint8_t> stateSection = body->section(SectionKind::CBStateBaseline);
        std::span<const uint8_t> summarySection = body->section(SectionKind::CBSummaryBaseline);
        if (stateSection.size() < sizeof(CBFormat::StateHeader) || summarySection.size() < sizeof(CBFormat::SummaryHeader)) {
            JITCACHE_FAIL(makeString(label, ": cb.state or cb.summary is shorter than its header"_s));
            continue;
        }
        CBFormat::StateHeader stateHeader;
        memcpySpan(asMutableByteSpan(stateHeader), stateSection.first(sizeof(stateHeader)));
        CBFormat::SummaryHeader summaryHeader;
        memcpySpan(asMutableByteSpan(summaryHeader), summarySection.first(sizeof(summaryHeader)));
        auto expectedMode = static_cast<uint8_t>(counterWithheld ? CBFormat::CounterMode::NotCarried : CBFormat::CounterMode::Carried);
        if (stateHeader.counterMode != expectedMode || summaryHeader.counterMode != expectedMode) {
            JITCACHE_FAIL(makeString(label, ": the counter modes are "_s, static_cast<unsigned>(stateHeader.counterMode), " and "_s,
                static_cast<unsigned>(summaryHeader.counterMode), ", expected "_s, static_cast<unsigned>(expectedMode)));
        }
        if (counterWithheld) {
            JITCACHE_CHECK(!body->counterProgress());
            JITCACHE_CHECK(!summaryHeader.counterProgress);
        } else {
            JITCACHE_CHECK(summaryHeader.counterProgress);
            JITCACHE_CHECK(body->counterProgress() == summaryHeader.counterProgress);
        }
        JITCACHE_CHECK(body->counterProgress() == kept->score.counterProgress);

        auto scored = scoreSections(body->highestTier(), body->section(SectionKind::UCBFeedback), summarySection, body->section(SectionKind::ICsBaseline), true);
        if (!scored || !sameScore(*scored, kept->score))
            JITCACHE_FAIL(makeString(label, ": the kept score "_s, scoreText(kept->score), " differs from scoreSections of its sections"_s));
    }
}

// Delta with the delta capture All, end to end: the program and the functions that ran without compiling commit at tier
// 0, with L the carried LLInt progress and P zero, and the kept score is the one scoreSections gives their sections. The
// delta capture Compiled commits none of them. A consumer imports them with no pending import, so their CodeBlocks start
// in the LLInt, and the counter keeps the carried progress. A later baseline compilation's capture replaces the tier-0
// body.
JITCACHE_TEST(runOnceDeltaCommitsAndConsumerImports, Yes)
{
    auto directory = TemporaryDirectory::create(context, "jitcache-run-once"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();

    // The delta capture Compiled first, on an artifact of its own: nothing compiled, so nothing commits.
    {
        ExtraVM compiled;
        if (!startsAs(context, "a Producer with Compiled"_s, JITCache::start(compiled.vm(), strictConfig(directory->path("compiled"_s), Role::Producer, DeltaCapture::Compiled)), StartOutcome::Started))
            return;
        if (!createRunOnceRealm(context, compiled.vm()))
            return;
        DeltaResult result = JITCache::delta(compiled.vm());
        JITCACHE_CHECK(result.outcome == DeltaOutcome::Completed);
        JITCACHE_CHECK(!result.committedBodies && !result.eligibleKeys);
    }

    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(artifact, Role::Producer, DeltaCapture::All)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createRunOnceRealm(context, vm);
    JSFunction* runTwice = globalObject ? globalFunction(context, globalObject, "runTwice"_s, runOnceSourceName) : nullptr;
    CodeBlock* codeBlock = codeBlockFor(context, runTwice, CodeSpecializationKind::CodeForCall);
    if (!codeBlock)
        return;
    JITCACHE_CHECK(codeBlock->jitType() == JITType::InterpreterThunk);
    UnlinkedCodeBlock& ucb = *codeBlock->unlinkedCodeBlock();
    std::optional<BodyKey> key = state.registry().keyOf(ucb);
    if (!key) {
        JITCACHE_FAIL("runTwice's UCB has no recorded key"_s);
        return;
    }
    double progress = ucb.llintExecuteCounter().count();
    int32_t threshold = ucb.llintExecuteCounter().m_activeThreshold;
    JITCACHE_CHECK(progress == 30);

    // A body prepared and never called, as CachedCall prepares a callee: its UCB is recorded, and its counter shows no run.
    JSFunction* prepared = globalFunction(context, globalObject, "preparedNeverCalled"_s, runOnceSourceName);
    if (!prepared)
        return;
    CodeBlock* preparedCodeBlock = nullptr;
    prepared->jsExecutable()->prepareForExecution<FunctionExecutable>(vm, prepared, prepared->scope(), CodeSpecializationKind::CodeForCall, preparedCodeBlock);
    std::optional<BodyKey> preparedKey = preparedCodeBlock ? state.registry().keyOf(*preparedCodeBlock->unlinkedCodeBlock()) : std::nullopt;
    if (!preparedKey) {
        JITCACHE_FAIL("preparing preparedNeverCalled left no recorded UCB"_s);
        return;
    }
    JITCACHE_CHECK(!llintCounterShowsExecution(*preparedCodeBlock->unlinkedCodeBlock()));

    DeltaResult result = JITCache::delta(vm);
    JITCACHE_CHECK(result.outcome == DeltaOutcome::Completed);
    // runTwice, constructedOnce's construct body and the program, at least.
    JITCACHE_CHECK(result.committedBodies >= 3);
    JITCACHE_CHECK(result.eligibleKeys >= result.committedBodies);
    JITCACHE_CHECK(!state.bodyVersion(*preparedKey));

    RefPtr<ValidatedBody> body = state.openBody(*key).body();
    std::optional<SavedScore> kept = keptScoreForTesting(vm, *key);
    if (!body || !kept) {
        JITCACHE_FAIL(makeString("delta left runTwice no "_s, body ? "kept summary"_s : "body"_s));
        return;
    }
    JITCACHE_CHECK(body->highestTier() == imagelessTier);
    JITCACHE_CHECK(body->llintThreshold() == carriedLLIntPoints(threshold, progress));
    JITCACHE_CHECK(!body->counterProgress());
    JITCACHE_CHECK(body->section(SectionKind::ImageBaseline).empty());
    JITCACHE_CHECK(body->version() == kept->version);
    auto scored = scoreSections(body->highestTier(), body->section(SectionKind::UCBFeedback), body->section(SectionKind::CBSummaryBaseline), body->section(SectionKind::ICsBaseline), true);
    if (!scored || !sameScore(*scored, kept->score))
        JITCACHE_FAIL(makeString("the kept score "_s, scoreText(kept->score), " differs from scoreSections of runTwice's sections"_s));
    JITCACHE_CHECK(kept->score.tier == imagelessTier && kept->score.counterProgress == 30);

    // A second delta with nothing run in between ties every saved body and commits nothing.
    DeltaResult again = JITCache::delta(vm);
    JITCACHE_CHECK(again.outcome == DeltaOutcome::Completed && !again.committedBodies);

    // The consumer imports the program and runTwice without an image: no pending import, CodeBlocks in the LLInt, and
    // runTwice's counter arms at the carried 30 points, to which its two calls add 30.
    {
        ExtraVM consumer;
        if (!startsAs(context, "the consumer"_s, JITCache::start(consumer.vm(), strictConfig(artifact, Role::Consumer, DeltaCapture::All)), StartOutcome::Started))
            return;
        VMState& consumerState = *consumer.vm().jitCacheState();
        uint64_t importsBefore = consumerState.registry().statistics().imports;
        JSGlobalObject* consumerGlobal = createRunOnceRealm(context, consumer.vm());
        JSFunction* consumerRunTwice = consumerGlobal ? globalFunction(context, consumerGlobal, "runTwice"_s, runOnceSourceName) : nullptr;
        CodeBlock* consumerCodeBlock = codeBlockFor(context, consumerRunTwice, CodeSpecializationKind::CodeForCall);
        if (!consumerCodeBlock)
            return;
        JITCACHE_CHECK(consumerState.registry().statistics().imports >= importsBefore + 3);
        JITCACHE_CHECK(!consumerState.progress().installs);
        JITCACHE_CHECK(consumerCodeBlock->jitType() == JITType::InterpreterThunk);
        UnlinkedCodeBlock& imported = *consumerCodeBlock->unlinkedCodeBlock();
        auto record = consumerState.registry().recordOf(imported);
        JITCACHE_CHECK(record && record->origin == UCBOrigin::Imported && !record->hasPendingImport);
        double consumerProgress = imported.llintExecuteCounter().count();
        if (std::abs(consumerProgress - 60) > 1e-3)
            JITCACHE_FAIL(makeString("runTwice's imported counter holds "_s, consumerProgress, " points after two calls, expected 60"_s));
    }

    // A baseline compilation of runTwice commits at tier 1, which beats the tier-0 body.
    {
        CompilationResult compilation;
        {
            DeferGCForAWhile deferGC(vm);
            compilation = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
        }
        JITCACHE_CHECK(compilation == CompilationResult::CompilationSuccessful);
        std::optional<SavedScore> recaptured = keptScoreForTesting(vm, *key);
        JITCACHE_CHECK(recaptured && recaptured->score.tier == baselineTier && recaptured->version != kept->version);
        RefPtr<ValidatedBody> compiledBody = state.openBody(*key).body();
        JITCACHE_CHECK(compiledBody && compiledBody->highestTier() == baselineTier);
    }
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
