#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "JIT.h"
#include "JITCacheAPI.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "SourceCode.h"
#include "UCBFeedback.h"
#include "UCBRegistry.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <array>
#include <cmath>
#include <limits>
#include <stdlib.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

// The capture of bodies that ran without compiling (THREAD Capture; SPEC-ucb.md section 8.3, SPEC-integrator.md sections
// 3.2 and 8.6): the delta capture at start, the registry's walk and the holders its records keep, the tier-0 scores and
// counter arithmetic, container check B2, and delta end to end, from the producer's commit to the consumer's import. The
// tests that configure a VM run strict on temporary directories.

namespace JSC::JITCache::Tests {

namespace RunOnceCaptureTestsInternal {

static String errnoText(int error)
{
    return String::fromUTF8(safeStrerror(error).data());
}

// A fresh temporary directory, removed with everything in it when the object goes.
class TemporaryDirectory {
    WTF_MAKE_NONCOPYABLE(TemporaryDirectory);
public:
    static std::unique_ptr<TemporaryDirectory> create(TestContext& context)
    {
        const char* base = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(base && *base ? base : "/tmp"), "/jitcache-run-once-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        return std::unique_ptr<TemporaryDirectory>(new TemporaryDirectory(String::fromUTF8(path.span().data())));
    }

    ~TemporaryDirectory()
    {
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    String path(StringView relative) const
    {
        return makeString(m_path, '/', relative);
    }

private:
    explicit TemporaryDirectory(String&& path)
        : m_path(WTF::move(path))
    {
    }

    const String m_path;
};

// A VM beside the test's, with its API lock and heap access held, destroyed under that lock when the object goes.
class ExtraVM {
    WTF_MAKE_NONCOPYABLE(ExtraVM);
public:
    ExtraVM()
        : m_vm(VM::create(HeapType::Large).leakRef())
        , m_locker(m_vm)
    {
        m_vm.derefSuppressingSaferCPPChecking();
    }

    VM& vm() { return m_vm; }

private:
    VM& m_vm;
    JSLockHolder m_locker;
};

static Config strictConfig(const String& path, Role role, DeltaCapture deltaCapture = DeltaCapture::All)
{
    Config config;
    config.artifactPath = path;
    config.role = role;
    if (role != Role::Consumer)
        config.producerLimitBytes = 64 * MB;
    config.strict = true;
    config.deltaCapture = deltaCapture;
    return config;
}

static bool startsAs(TestContext& context, ASCIILiteral label, const StartResult& result, StartOutcome outcome, ASCIILiteral step = { })
{
    if (result.outcome == outcome && result.step == step)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, name(outcome), step.isNull() ? ""_s : " at "_s, step, ", got "_s, toJSON(result)));
    return false;
}

static BodyKey testKey(uint8_t fill)
{
    Digest256 identityDigest;
    identityDigest.fill(fill);
    return BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, identityDigest);
}

static UCBRecord testRecord(const BodyKey& key, UnlinkedFunctionExecutable* holder)
{
    UCBRecord record {
        key,
        GlobalContextInputs { 0, 1, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false },
        UCBOrigin::Generated,
        NoLexicallyScopedFeatures,
        std::nullopt,
        std::nullopt,
        nullptr,
        nullptr,
        std::nullopt,
    };
    record.holder = holder;
    return record;
}

// The run-once bodies: runTwice is called twice and never compiles, and the program's top level runs once. The global
// object stays reachable from the test's stack, so no collection takes the bodies before delta.
constexpr ASCIILiteral runOnceSource = "function runTwice(x) { return x * 2 + 1; }\n"
    "function constructedOnce(x) { this.x = x; }\n"
    "function preparedNeverCalled(x) { return x - 1; }\n"
    "var total = runTwice(1) + runTwice(2);\n"
    "var made = new constructedOnce(3);\n"_s;

static JSGlobalObject* evaluateRunOnceSource(TestContext& context, VM& vm)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(runOnceSource, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the run-once source threw"_s);
        return nullptr;
    }
    return globalObject;
}

static JSFunction* globalFunction(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(globalObject->vm(), name)));
    if (!function || function->isHostFunction()) {
        JITCACHE_FAIL(makeString("the run-once source defines no JS function "_s, name));
        return nullptr;
    }
    return function;
}

static CodeBlock* codeBlockFor(TestContext& context, JSFunction* function, CodeSpecializationKind kind)
{
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockFor(kind) : nullptr;
    if (!codeBlock)
        JITCACHE_FAIL("a run-once function has no CodeBlock of the kind"_s);
    return codeBlock;
}

static bool callWith(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, JSValue argument)
{
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    NakedPtr<Exception> exception;
    JSC::call(globalObject, function, getCallData(function), jsUndefined(), arguments, exception);
    if (exception) {
        JITCACHE_FAIL("calling a run-once function threw"_s);
        return false;
    }
    return true;
}

static String scoreText(const CaptureScore& score)
{
    return makeString("{ tier "_s, static_cast<unsigned>(score.tier), ", richness "_s, score.richness, ", IC sites with cases "_s,
        score.icSitesWithCases, ", counter "_s, score.counterWithheld ? "withheld"_s : "carried"_s, ", progress "_s, score.counterProgress, " }"_s);
}

static bool sameScore(const CaptureScore& a, const CaptureScore& b)
{
    return a.tier == b.tier && a.richness == b.richness && a.icSitesWithCases == b.icSitesWithCases
        && a.counterWithheld == b.counterWithheld && a.counterProgress == b.counterProgress;
}

} // namespace RunOnceCaptureTestsInternal

using namespace RunOnceCaptureTestsInternal;

// T-START, the delta capture: the flag's two spellings, All by default, a value outside the enumeration rejected at
// start.config with the VM left unconfigured, and the state's answers. Only a producing role with All records holders.
JITCACHE_TEST(runOnceDeltaCaptureAtStart, Yes)
{
    JITCACHE_CHECK(parseDeltaCapture("all"_s) == DeltaCapture::All);
    JITCACHE_CHECK(parseDeltaCapture("compiled"_s) == DeltaCapture::Compiled);
    JITCACHE_CHECK(!parseDeltaCapture("All"_s));
    JITCACHE_CHECK(!parseDeltaCapture(""_s));
    JITCACHE_CHECK(name(DeltaCapture::All) == "all"_s);
    JITCACHE_CHECK(name(DeltaCapture::Compiled) == "compiled"_s);
    JITCACHE_CHECK(Config { }.deltaCapture == DeltaCapture::All);

    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    for (uint8_t value : { 0, 3, 255 }) {
        StartResult result = JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer, static_cast<DeltaCapture>(value)));
        if (startsAs(context, "a delta capture outside the enumeration"_s, result, StartOutcome::Rejected, "start.config"_s))
            JITCACHE_CHECK(result.detail.contains("deltaCapture"_s));
        JITCACHE_CHECK(!vm.jitCacheState());
    }

    if (!startsAs(context, "a Producer with Compiled"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer, DeltaCapture::Compiled)), StartOutcome::Started))
        return;
    JITCACHE_CHECK(vm.jitCacheState()->deltaCapture() == DeltaCapture::Compiled);
    JITCACHE_CHECK(!vm.jitCacheState()->capturesUncompiledBodies());
    {
        ExtraVM producer;
        if (startsAs(context, "a Producer with All"_s, JITCache::start(producer.vm(), strictConfig(directory->path("other"_s), Role::Producer)), StartOutcome::Started)) {
            JITCACHE_CHECK(producer.vm().jitCacheState()->deltaCapture() == DeltaCapture::All);
            JITCACHE_CHECK(producer.vm().jitCacheState()->capturesUncompiledBodies());
        }
    }
    {
        ExtraVM consumer;
        if (startsAs(context, "a Consumer with All"_s, JITCache::start(consumer.vm(), strictConfig(directory->path("artifact"_s), Role::Consumer)), StartOutcome::Started))
            JITCACHE_CHECK(!consumer.vm().jitCacheState()->capturesUncompiledBodies());
    }
}

// The registry's walk (SPEC-ucb.md section 6.1): the count, a copy of every record with its holder, a copy bounded by
// its span, and the destructor hooks: a UFE's clears the holder of every record that names it, and a UCB's leaves no
// entry its holder's hook would follow.
JITCACHE_TEST(runOnceRegistryWalkAndHolders, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = evaluateRunOnceSource(context, vm);
    if (!globalObject)
        return;
    CodeBlock* called = codeBlockFor(context, globalFunction(context, globalObject, "runTwice"_s), CodeSpecializationKind::CodeForCall);
    CodeBlock* constructed = codeBlockFor(context, globalFunction(context, globalObject, "constructedOnce"_s), CodeSpecializationKind::CodeForConstruct);
    if (!called || !constructed)
        return;
    UnlinkedCodeBlock* calledUCB = called->unlinkedCodeBlock();
    UnlinkedCodeBlock* constructedUCB = constructed->unlinkedCodeBlock();
    UnlinkedFunctionExecutable* calledHolder = uncheckedDowncast<FunctionExecutable>(called->ownerExecutable())->unlinkedExecutable();

    // A registry of the test's own, so its hooks run only when the test calls them.
    UCBRegistry registry;
    JITCACHE_CHECK(!registry.recordedCodeBlockCount());
    // Two records naming one holder, as an older UCB a CodeBlock keeps beside the one in the slot would.
    JITCACHE_CHECK(registry.recordCodeBlock(*calledUCB, testRecord(testKey(1), calledHolder)));
    JITCACHE_CHECK(registry.recordCodeBlock(*constructedUCB, testRecord(testKey(2), calledHolder)));
    JITCACHE_CHECK(registry.recordedCodeBlockCount() == 2);

    auto copied = [&](size_t capacity) {
        Vector<UCBRegistry::RecordedCodeBlock> out(capacity);
        out.shrink(registry.copyRecordedCodeBlocks(out.mutableSpan()));
        return out;
    };
    auto holderIn = [](const Vector<UCBRegistry::RecordedCodeBlock>& records, UnlinkedCodeBlock* codeBlock) -> std::optional<UnlinkedFunctionExecutable*> {
        for (auto& record : records) {
            if (record.codeBlock == codeBlock)
                return record.holder;
        }
        return std::nullopt;
    };

    auto both = copied(4);
    JITCACHE_CHECK(both.size() == 2);
    JITCACHE_CHECK(holderIn(both, calledUCB) == std::optional { calledHolder });
    JITCACHE_CHECK(holderIn(both, constructedUCB) == std::optional { calledHolder });
    JITCACHE_CHECK(copied(1).size() == 1);
    JITCACHE_CHECK(copied(0).isEmpty());

    // The UCB's hook removes its record and its entry under the holder; the holder's hook then clears the other record.
    registry.unlinkedCodeBlockDestroyed(constructedUCB);
    JITCACHE_CHECK(registry.recordedCodeBlockCount() == 1);
    registry.unlinkedFunctionExecutableDestroyed(calledHolder);
    auto remaining = copied(4);
    JITCACHE_CHECK(remaining.size() == 1);
    JITCACHE_CHECK(holderIn(remaining, calledUCB) == std::optional<UnlinkedFunctionExecutable*> { nullptr });
    // A record without a holder stays as it is when the UFE's hook runs again.
    registry.unlinkedFunctionExecutableDestroyed(calledHolder);
    JITCACHE_CHECK(registry.recordedCodeBlockCount() == 1);
    registry.unlinkedCodeBlockDestroyed(calledUCB);
    JITCACHE_CHECK(!registry.recordedCodeBlockCount());
}

// The counter a body without an image carries (SPEC-ucb.md section 5.5) and the order of tiers (THREAD Capture).
JITCACHE_TEST(runOnceCarriedProgressAndTiers, No)
{
    constexpr int32_t deferred = std::numeric_limits<int32_t>::max();
    JITCACHE_CHECK(carriedLLIntProgress(500, 30) == 30);
    JITCACHE_CHECK(carriedLLIntProgress(500, 30.5) == 30.5);
    JITCACHE_CHECK(carriedLLIntProgress(500, 485) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(500, 495) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(500, 800) == 485);
    JITCACHE_CHECK(carriedLLIntProgress(100, 0.37) == 0.37);
    JITCACHE_CHECK(!carriedLLIntProgress(10, 7));
    JITCACHE_CHECK(!carriedLLIntProgress(500, 0));
    JITCACHE_CHECK(!carriedLLIntProgress(500, -4));
    JITCACHE_CHECK(!carriedLLIntProgress(500, std::nan("")));
    JITCACHE_CHECK(!carriedLLIntProgress(deferred, -0x1p31));
    JITCACHE_CHECK(carriedLLIntPoints(500, 30.5) == 30);
    JITCACHE_CHECK(carriedLLIntPoints(500, 900) == 485);
    JITCACHE_CHECK(!carriedLLIntPoints(deferred, -0x1p31));

    // A body with an image keeps the captured progress; a deferred counter stays deferred either way.
    JITCACHE_CHECK(importedLLIntProgress(500, 495, BodyImage::Present) == 495);
    JITCACHE_CHECK(importedLLIntProgress(500, 495, BodyImage::Absent) == 485);
    JITCACHE_CHECK(importedLLIntProgress(deferred, -0x1p31, BodyImage::Absent) == -0x1p31);

    // armLLIntCounter from the carried progress crosses after one full LLInt invocation, 5 points at the prologue and 10
    // at the return.
    BaselineExecutionCounter counter;
    armLLIntCounter(counter, 500, importedLLIntProgress(500, 495, BodyImage::Absent));
    JITCACHE_CHECK(counter.m_activeThreshold == 500);
    JITCACHE_CHECK(counter.m_counter == -15);
    JITCACHE_CHECK(std::abs(counter.count() - 485) < 1e-3);

    // Tier comes first: no capture without an image beats one with an image, and any capture with one beats it.
    const CaptureScore imageless { imagelessTier, 1000, 0, false, 485 };
    const CaptureScore baseline { baselineTier, 0, 0, false, 0 };
    JITCACHE_CHECK(beats(baseline, imageless));
    JITCACHE_CHECK(!beats(imageless, baseline));
    // Between captures without an image, richness, then the carried progress; a tie keeps the saved body.
    JITCACHE_CHECK(beats({ imagelessTier, 1001, 0, false, 0 }, imageless));
    JITCACHE_CHECK(beats({ imagelessTier, 1000, 0, false, 486 }, imageless));
    JITCACHE_CHECK(!beats(imageless, imageless));
}

// The measurement switch: with jitCacheImportLLIntProgress off, a body without an image arms its counter from zero, and a
// body with one keeps its progress.
JITCACHE_TEST_WITH_OPTIONS(runOnceImportFromZeroWithTheOptionOff, No, "--jitCacheImportLLIntProgress=false")
{
    JITCACHE_CHECK(!importedLLIntProgress(500, 30, BodyImage::Absent));
    JITCACHE_CHECK(importedLLIntProgress(500, 30, BodyImage::Present) == 30);
    JITCACHE_CHECK(importedLLIntProgress(std::numeric_limits<int32_t>::max(), -0x1p31, BodyImage::Absent) == -0x1p31);
}

// Container check B2 (container sub-SPEC section 4.5): Full mode accepts a highest tier of 0 or 1 and rejects any other,
// and B7 then requires exactly the UCB lane's three sections of a body of tier 0.
JITCACHE_TEST(runOnceContainerAcceptsTierZero, No)
{
    BodyKey key = testKey(7);
    HeaderDigest headerDigest;
    headerDigest.fill(0x5a);
    Vector<uint8_t> identity(FillWith { }, 24, 1);
    Vector<uint8_t> core(FillWith { }, 40, 2);
    Vector<uint8_t> feedback(FillWith { }, 56, 3);
    Vector<uint8_t> image(FillWith { }, 64, 4);
    std::array ucbOnly {
        ContainerTesting::Section { SectionKind::UCBIdentity, identity.span() },
        ContainerTesting::Section { SectionKind::UCBCore, core.span() },
        ContainerTesting::Section { SectionKind::UCBFeedback, feedback.span() },
    };
    std::array withImage {
        ContainerTesting::Section { SectionKind::UCBIdentity, identity.span() },
        ContainerTesting::Section { SectionKind::UCBCore, core.span() },
        ContainerTesting::Section { SectionKind::UCBFeedback, feedback.span() },
        ContainerTesting::Section { SectionKind::ImageBaseline, image.span() },
    };
    auto stamp = [&](uint8_t tier) {
        BodyEnvelope envelope;
        envelope.highestTier = tier;
        envelope.key = key;
        envelope.version = 0x1234;
        envelope.headerDigest = headerDigest;
        envelope.llintThreshold = 30;
        return envelope;
    };
    auto validate = [&](uint8_t tier, std::span<const ContainerTesting::Section> sections, ValidationMode mode) {
        Vector<uint8_t> file = ContainerTesting::buildBody(stamp(tier), sections);
        return validateBody(file.span(), key, std::span<const uint8_t, 16> { headerDigest }, mode);
    };

    auto tierZero = validate(0, ucbOnly, ValidationMode::Full);
    if (!tierZero)
        JITCACHE_FAIL(makeString("Full mode rejected a body of tier 0 at "_s, tierZero.error()));
    else {
        JITCACHE_CHECK(!tierZero->highestTier);
        JITCACHE_CHECK(tierZero->llintThreshold == 30);
        JITCACHE_CHECK(!tierZero->sections[static_cast<size_t>(SectionKind::ImageBaseline)]);
    }
    JITCACHE_CHECK(validate(0, ucbOnly, ValidationMode::Integrity).has_value());

    auto tierTwo = validate(2, ucbOnly, ValidationMode::Full);
    JITCACHE_CHECK(!tierTwo && tierTwo.error() == ContainerChecks::envelope);
    // Integrity leaves the tier to the structure checks.
    JITCACHE_CHECK(validate(2, ucbOnly, ValidationMode::Integrity).has_value());

    auto imageAtTierZero = validate(0, withImage, ValidationMode::Full);
    JITCACHE_CHECK(!imageAtTierZero && imageAtTierZero.error() == ContainerChecks::required);
}

#if ENABLE(JIT)

// Delta with the delta capture All, end to end (SPEC-integrator.md section 8.6, phase 3): the program and the functions
// that ran without compiling commit at tier 0, with L the carried LLInt progress and P zero, and the kept score is the
// one scoreSections gives their sections. The delta capture Compiled commits none of them. A consumer imports them with no pending
// import, so their CodeBlocks start in the LLInt, and the counter keeps the carried progress. A later baseline
// compilation's capture replaces the tier-0 body.
JITCACHE_TEST(runOnceDeltaCommitsAndConsumerImports, Yes)
{
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();

    // The delta capture Compiled first, on an artifact of its own: nothing compiled, so nothing commits.
    {
        ExtraVM compiled;
        if (!startsAs(context, "a Producer with Compiled"_s, JITCache::start(compiled.vm(), strictConfig(directory->path("compiled"_s), Role::Producer, DeltaCapture::Compiled)), StartOutcome::Started))
            return;
        if (!evaluateRunOnceSource(context, compiled.vm()))
            return;
        DeltaResult result = JITCache::delta(compiled.vm());
        JITCACHE_CHECK(result.outcome == DeltaOutcome::Completed);
        JITCACHE_CHECK(!result.committedBodies && !result.eligibleKeys);
    }

    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(artifact, Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = evaluateRunOnceSource(context, vm);
    JSFunction* runTwice = globalObject ? globalFunction(context, globalObject, "runTwice"_s) : nullptr;
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
    JSFunction* prepared = globalFunction(context, globalObject, "preparedNeverCalled"_s);
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
        if (!startsAs(context, "the consumer"_s, JITCache::start(consumer.vm(), strictConfig(artifact, Role::Consumer)), StartOutcome::Started))
            return;
        VMState& consumerState = *consumer.vm().jitCacheState();
        uint64_t importsBefore = consumerState.registry().statistics().imports;
        JSGlobalObject* consumerGlobal = evaluateRunOnceSource(context, consumer.vm());
        JSFunction* consumerRunTwice = consumerGlobal ? globalFunction(context, consumerGlobal, "runTwice"_s) : nullptr;
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
