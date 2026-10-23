#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "CodeBlock.h"
#include "Completion.h"
#include "ConcurrentJSLock.h"
#include "Debugger.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ReleaseHeapAccessScope.h"
#include "SourceCode.h"
#include "VM.h"
#include "jitcache/base/VMPart.h"
#include "jitcache/glue/KeptSummaries.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheFaults.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/session/ProducerContext.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/store/JITCacheParameters.h"
#include "jitcache/store/JITCachePlatform.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/ModuleSources.h"
#include "jitcache/tests/support/TestArtifact.h"
#include "jitcache/tests/support/TestFiles.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <optional>
#include <wtf/FileSystem.h>
#include <wtf/HashSet.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// Tests of the session part: JITCacheAPI's start, status, delta capture setting and JSON lines; the switches, faults,
// body lookups and per-VM parts of JITCacheVMState with the entry points of JITCacheFaults; the start event and summary
// lines JITCacheBench writes; and that the exported headers Bun compiles against include only exported JITCache
// headers. A test that needs several VMs, because each fault and each configuration is permanent for a VM, creates them
// beside the test's own on its thread and destroys each under its API lock, which runs VM::~VM's teardown hooks.

namespace JSC::JITCache::Tests {

namespace SessionTestsInternal {

// An artifact a Producer created and left: its VM is destroyed, which releases the producer lock.
static bool createTestArtifact(TestContext& context, const String& path)
{
    ExtraVM producer;
    return startsAs(context, "the producer that creates the artifact"_s, JITCache::start(producer.vm(), testConfig(path, Role::Producer)), StartOutcome::Started);
}

// A ConsumerProducer over an artifact: both switches start on.
static bool startConsumerProducer(TestContext& context, VM& vm, const String& path)
{
    if (!startsAs(context, "a ConsumerProducer"_s, JITCache::start(vm, testConfig(path, Role::ConsumerProducer)), StartOutcome::Started))
        return false;
    VMState& state = *vm.jitCacheState();
    if (state.activityOn() && state.importsEnabled() && state.productionActive() && producerContext(vm))
        return true;
    JITCACHE_FAIL(makeString("a ConsumerProducer started with a switch off: "_s, toJSON(JITCache::status(vm))));
    return false;
}

// Two parts for the test of VMState::part, one of each lifetime, each with a flag its destructor sets.
struct ProductionTestPart final : VMPart {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ProductionTestPart);

    static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilProductionEnds;

    explicit ProductionTestPart(bool& flag)
        : destroyed(flag)
    {
    }

    ~ProductionTestPart() final { destroyed = true; }

    bool& destroyed;
};

struct VMTestPart final : VMPart {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(VMTestPart);

    static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilVMDestruction;

    VMTestPart(bool& flag, VM& owner)
        : destroyedBeforeTheState(flag)
        , vm(owner)
    {
    }

    // didFinalizeHeap stores null into the VM's pointer before it destroys the state, so the flag is set only by a
    // destruction that comes earlier, such as willDestroyVM's.
    ~VMTestPart() final { destroyedBeforeTheState = !!vm.jitCacheState(); }

    bool& destroyedBeforeTheState;
    VM& vm;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ProductionTestPart);
WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(VMTestPart);

// The state after a fault that turned activity off: both switches off, so nothing is tracked, imported or recorded, and
// status names the fault as the activity fault, as the end of production and as the first fault.
static void checkActivityOff(TestContext& context, VM& vm, FaultClass faultClass, ASCIILiteral step)
{
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.activityOn());
    JITCACHE_CHECK(!state.tracksKeys());
    JITCACHE_CHECK(!state.importsEnabled());
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!state.producerContextIfActive());
    JITCACHE_CHECK(!producerContext(vm));
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.state == SessionState::Started);
    JITCACHE_CHECK(!reported.activityOn);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    checkFault(context, "the activity fault"_s, reported.activityFault, faultClass, step);
    checkFault(context, "the production fault"_s, reported.productionFault, faultClass, step);
    checkFault(context, "the first fault"_s, reported.firstFault, faultClass, step);
}

// A JS function's CB after one call, which the LLInt runs, in a global object of its own.
static CodeBlock* calledCodeBlock(TestContext& context, JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("function f() { return 1; } f();"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the test source threw"_s);
        return nullptr;
    }
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "f"_s)));
    CodeBlock* codeBlock = function && !function->isHostFunction() ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock)
        JITCACHE_FAIL("the call left no CB"_s);
    return codeBlock;
}

// The L and P commitTestBody stamps in the envelope, which a ValidatedBody the store opens returns.
constexpr uint32_t testBodyLLIntThreshold = 500;
constexpr uint32_t testBodyCounterProgress = 7;

// Commits a body through a producing VM's writer: every section a body of highest tier 1 holds in this build, with
// bytes drawn from the seed, and the analysis as its ucb.analysis when it is not empty, which only a module's key may
// carry. The writer reads no lane format, so any bytes make a body the container accepts.
static std::optional<CommitResult> commitTestBody(TestContext& context, VMState& producer, const BodyKey& key, uint8_t seed, std::span<const uint8_t> analysis = { })
{
    ArtifactWriter* writer = producer.writer();
    if (!writer) {
        JITCACHE_FAIL("the producer has no writer"_s);
        return std::nullopt;
    }
    std::array<Vector<uint8_t>, numberOfSectionKinds> bytes;
    Vector<SectionSource, numberOfSectionKinds> sources;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (kind == SectionKind::UCBAnalysis) {
            if (!analysis.empty())
                sources.append(SectionSource::inMemory(kind, analysis));
            continue;
        }
        if (!isSectionRequired(kind, 1))
            continue;
        bytes[index] = patternBytes(16 + 8 * index, static_cast<uint8_t>(seed + index));
        sources.append(SectionSource::inMemory(kind, bytes[index].span()));
    }
    auto committed = writer->commit(CommitStamp { key, testBodyLLIntThreshold, testBodyCounterProgress, 1 }, CommitSections { sources.span() });
    if (!committed) {
        JITCACHE_FAIL(makeString("the commit failed at "_s, committed.error().check, ": "_s, committed.error().detail));
        return std::nullopt;
    }
    return *committed;
}

// The header's CRC over the bytes before it, written in place.
static void resealHeader(Vector<uint8_t>& header)
{
    uint32_t crc = ~crc32cExtend(~0u, header.span().first(header.size() - 8));
    memcpySpan(header.mutableSpan().subspan(header.size() - 8, sizeof(crc)), asByteSpan(crc));
}

// The process's header with one must-match option's value flipped and its CRC recomputed, so only the comparison with
// the process's own header fails, at that option. The build-ID records start at offset 40, after the app record.
static Vector<uint8_t> headerWithFlippedOption(unsigned optionIndex)
{
    Vector<uint8_t> header;
    header.append(expectedHeader().span());
    size_t buildIDRecords = header[11];
    header[40 + 72 * buildIDRecords + 8 * optionIndex + 3] ^= 1;
    resealHeader(header);
    return header;
}

// The header with an app record naming app, bytes [28, 40), and its CRC recomputed.
static Vector<uint8_t> headerNamingAppOf(Vector<uint8_t> header, uint64_t app)
{
    header[28] = 1;
    memcpySpan(header.mutableSpan().subspan(32, sizeof(app)), asByteSpan(app));
    resealHeader(header);
    return header;
}

// The app the header at path names, or none; a header that cannot be read or does not parse fails the test.
static std::optional<uint64_t> appOfHeaderAt(TestContext& context, const String& path)
{
    auto bytes = readFileBytes(path);
    if (!bytes) {
        JITCACHE_FAIL(makeString(path, " cannot be read"_s));
        return std::nullopt;
    }
    auto parsed = readArtifactHeader(bytes->span());
    if (!parsed) {
        JITCACHE_FAIL(makeString(path, " holds no header that parses: "_s, parsed.error().reason));
        return std::nullopt;
    }
    return parsed->app;
}

// The name of every header in directory and in the directories below it: the module's headers sit in one directory per
// part, and the copies Bun includes are flat, so an exported header names another by its name alone.
static void integratorAddHeaderNamesBelow(const String& directory, HashSet<String>& headers)
{
    for (auto& name : FileSystem::listDirectory(directory)) {
        String path = FileSystem::pathByAppendingComponent(directory, name);
        if (FileSystem::fileType(path) == FileSystem::FileType::Directory)
            integratorAddHeaderNamesBelow(path, headers);
        else if (name.endsWith(".h"_s))
            headers.add(name);
    }
}

} // namespace SessionTestsInternal

using namespace SessionTestsInternal;

// Start for each role. A Producer creates a missing parent one level deep and is Started; a second producing VM of the
// process is Busy while the first VM's state lives and stays unconfigured; a Consumer beside the producer is Started and
// shares its opened artifact; and once the producer's state is destroyed, which releases the lock, the VM that was busy
// starts as a ConsumerProducer.
JITCACHE_TEST(integratorStartOutcomesByRole, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();

    std::optional<ExtraVM> producer;
    producer.emplace();
    if (!startsAs(context, "the producer"_s, JITCache::start(producer->vm(), testConfig(artifact, Role::Producer)), StartOutcome::Started))
        return;
    JITCACHE_CHECK(pathExists(directory->path("artifact/.cache.producer.lock"_s)));
    JITCACHE_CHECK(pathExists(directory->path("artifact/cache/header"_s)));
    JITCACHE_CHECK(pathExists(directory->path("artifact/cache/pack"_s)));
    Status producerStatus = JITCache::status(producer->vm());
    JITCACHE_CHECK(producerStatus.state == SessionState::Started);
    JITCACHE_CHECK(producerStatus.role == Role::Producer);
    JITCACHE_CHECK(producerStatus.activityOn);
    JITCACHE_CHECK(producerStatus.production == ProductionState::Active);
    JITCACHE_CHECK(!producerStatus.activityFault && !producerStatus.productionFault && !producerStatus.firstFault);
    JITCACHE_CHECK(producerStatus.budget.limitBytes == testProducerLimitBytes);
    JITCACHE_CHECK(!producerStatus.budget.refused);
    JITCACHE_CHECK(!producerStatus.progress.indexedBodies);
    JITCACHE_CHECK(producer->vm().jitCacheState()->writer());
    JITCACHE_CHECK(!producer->vm().jitCacheState()->importsEnabled());
    JITCACHE_CHECK(producerContext(producer->vm()));

    for (Role role : { Role::Producer, Role::ConsumerProducer }) {
        startsAs(context, "a second producing VM"_s, JITCache::start(vm, testConfig(artifact, role)), StartOutcome::Busy, "start.busy"_s);
        JITCACHE_CHECK(!vm.jitCacheState());
        JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Unconfigured);
    }

    {
        ExtraVM consumer;
        if (startsAs(context, "a consumer beside the producer"_s, JITCache::start(consumer.vm(), testConfig(artifact, Role::Consumer)), StartOutcome::Started)) {
            VMState& state = *consumer.vm().jitCacheState();
            JITCACHE_CHECK(state.artifact() && state.artifact() == producer->vm().jitCacheState()->artifact());
            JITCACHE_CHECK(state.importsEnabled());
            JITCACHE_CHECK(!state.writer());
            JITCACHE_CHECK(!state.producerBudget());
            JITCACHE_CHECK(!producerContext(consumer.vm()));
            Status consumerStatus = JITCache::status(consumer.vm());
            JITCACHE_CHECK(consumerStatus.state == SessionState::Started);
            JITCACHE_CHECK(consumerStatus.role == Role::Consumer);
            JITCACHE_CHECK(consumerStatus.production == ProductionState::NotProducing);
            JITCACHE_CHECK(!consumerStatus.productionFault);
            JITCACHE_CHECK(!consumerStatus.budget.limitBytes);
        }
    }

    producer.reset();
    if (!startsAs(context, "the ConsumerProducer once the producer is gone"_s, JITCache::start(vm, testConfig(artifact, Role::ConsumerProducer)), StartOutcome::Started))
        return;
    Status consumerProducer = JITCache::status(vm);
    JITCACHE_CHECK(consumerProducer.state == SessionState::Started);
    JITCACHE_CHECK(consumerProducer.role == Role::ConsumerProducer);
    JITCACHE_CHECK(consumerProducer.production == ProductionState::Active);
    JITCACHE_CHECK(vm.jitCacheState()->importsEnabled());
    JITCACHE_CHECK(vm.jitCacheState()->writer());
}

// Each rejection a test can reach in the default option group leaves the VM unconfigured, and a start on the same VM
// afterwards succeeds.
JITCACHE_TEST(integratorStartRejections, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    auto checkRejected = [&](ASCIILiteral label, const Config& config, ASCIILiteral step) {
        startsAs(context, label, JITCache::start(vm, config), StartOutcome::Rejected, step);
        JITCACHE_CHECK(!vm.jitCacheState());
    };

    {
        ReleaseHeapAccessScope withoutHeapAccess(vm.heap);
        checkRejected("a call without heap access"_s, testConfig(artifact, Role::Consumer), "start.locks"_s);
    }

    Config emptyPath = testConfig(artifact, Role::Consumer);
    emptyPath.artifactPath = String();
    checkRejected("an empty artifactPath"_s, emptyPath, "start.config"_s);
    Config badRole = testConfig(artifact, Role::Consumer);
    badRole.role = static_cast<Role>(7);
    checkRejected("a role out of range"_s, badRole, "start.config"_s);
    if (!defaultProducerLimitBytes) {
        Config noLimit = testConfig(artifact, Role::Producer);
        noLimit.producerLimitBytes = std::nullopt;
        checkRejected("a producer without a limit"_s, noLimit, "start.config"_s);
    }
    Config badBenchReport = testConfig(artifact, Role::Consumer);
    badBenchReport.benchReportPath = directory->path("missing/bench.jsonl"_s);
    checkRejected("a bench report that does not open"_s, badBenchReport, "start.config"_s);
    Config badTwinReport = testConfig(artifact, Role::Consumer);
    badTwinReport.twinReportPath = directory->path("missing/twins.jsonl"_s);
    checkRejected("a twin report that does not open"_s, badTwinReport, "start.config"_s);

    // The build-ID step precedes every step that touches the artifact.
    removeMainBuildIDForTesting(true);
    checkRejected("a main executable without a build ID"_s, testConfig(artifact, Role::Producer), "start.build-id"_s);
    removeMainBuildIDForTesting(false);
    JITCACHE_CHECK(!pathExists(artifact));

    // A missing parent, a parent without cache/ and a cache/ without a header, for both roles that open an artifact.
    String bare = directory->path("bare"_s);
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a missing parent"_s, testConfig(bare, role), "start.artifact-missing"_s);
    if (!makeDirectory(context, bare))
        return;
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a parent without cache/"_s, testConfig(bare, role), "start.artifact-missing"_s);
    if (!makeDirectory(context, directory->path("bare/cache"_s)))
        return;
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a cache/ without a header"_s, testConfig(bare, role), "start.artifact-missing"_s);

    // A debugger attached before start turned the PC-to-origin maps on for good, so that VM is rejected.
    {
        ExtraVM attached;
        JSGlobalObject* globalObject = JSGlobalObject::create(attached.vm(), JSGlobalObject::createStructure(attached.vm(), jsNull()));
        Debugger debugger(attached.vm());
        debugger.attach(globalObject);
        startsAs(context, "a VM a debugger attached to"_s, JITCache::start(attached.vm(), testConfig(artifact, Role::Producer)), StartOutcome::Rejected, "start.pc-maps"_s);
        JITCACHE_CHECK(!attached.vm().jitCacheState());
        debugger.detach(globalObject, Debugger::TerminatingDebuggingSession);
    }
    JITCACHE_CHECK(!pathExists(artifact));

    if (!createTestArtifact(context, artifact))
        return;
    checkRejected("a producer over an artifact"_s, testConfig(artifact, Role::Producer), "start.artifact-exists"_s);

    // A header that differs from the process's in one must-match option is incompatible and names the option. A Producer
    // is rejected at start.artifact-exists after taking the lock, which creates the lock file, and a Consumer at
    // start.incompatible. A ConsumerProducer replaces the artifact with an empty one and says why at start.replaced: it
    // removes the old artifact and the .cache.replaced an earlier replacement left. A Consumer then opens the new artifact.
    String incompatible = directory->path("incompatible"_s);
    Vector<uint8_t> header = headerWithFlippedOption(0);
    Vector<uint8_t> packBytes = patternBytes(33, 4);
    if (!makeDirectory(context, incompatible) || !makeDirectory(context, directory->path("incompatible/cache"_s))
        || !writeFile(context, directory->path("incompatible/cache/header"_s), header.span())
        || !writeFile(context, directory->path("incompatible/cache/pack"_s), packBytes.span()))
        return;
    checkRejected("a Producer over an incompatible artifact"_s, testConfig(incompatible, Role::Producer), "start.artifact-exists"_s);
    StartResult rejected = JITCache::start(vm, testConfig(incompatible, Role::Consumer));
    startsAs(context, "a Consumer of an incompatible header"_s, rejected, StartOutcome::Rejected, "start.incompatible"_s);
    JITCACHE_CHECK(rejected.detail.contains("evalMode"_s));
    JITCACHE_CHECK(!vm.jitCacheState());
    // What an earlier replacement left when it stopped before removing the old artifact.
    if (!makeDirectory(context, directory->path("incompatible/.cache.replaced"_s))
        || !writeFile(context, directory->path("incompatible/.cache.replaced/header"_s), header.span())
        || !writeFile(context, directory->path("incompatible/.cache.replaced/pack"_s), packBytes.span()))
        return;
    {
        ExtraVM replacing;
        StartResult replaced = JITCache::start(replacing.vm(), testConfig(incompatible, Role::ConsumerProducer));
        if (startsAs(context, "a ConsumerProducer of an incompatible header"_s, replaced, StartOutcome::Started, "start.replaced"_s)) {
            JITCACHE_CHECK(replaced.detail.contains("evalMode"_s));
            JITCACHE_CHECK(pathExists(directory->path("incompatible/cache/header"_s)) && pathExists(directory->path("incompatible/cache/pack"_s)));
            auto newPack = readFileBytes(directory->path("incompatible/cache/pack"_s));
            JITCACHE_CHECK(newPack && !equalSpans(newPack->span(), packBytes.span()));
            JITCACHE_CHECK(!pathExists(directory->path("incompatible/.cache.replaced"_s)));
            ExtraVM reader;
            startsAs(context, "a Consumer of the replaced artifact"_s, JITCache::start(reader.vm(), testConfig(incompatible, Role::Consumer)), StartOutcome::Started);
        }
    }

    // After every rejection, the VM still configures, and then it is configured for good.
    if (!startsAs(context, "a consumer after the rejections"_s, JITCache::start(vm, testConfig(artifact, Role::Consumer)), StartOutcome::Started))
        return;
    startsAs(context, "a second start"_s, JITCache::start(vm, testConfig(artifact, Role::Consumer)), StartOutcome::Rejected, "start.already-configured"_s);
    JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Started);
}

// A fixed option without its required value rejects at start.fixed-option, naming the first such option of the table and
// both values. This group's process sets two fixed options off their required values.
JITCACHE_TEST_WITH_OPTIONS(integratorStartRejectsAFixedOption, Yes, "--thresholdForJITSoon=99 --quickDFGTierUpThresholdFactor=0.3")
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    StartResult result = JITCache::start(vm, testConfig(artifact, Role::Producer));
    startsAs(context, "a process with a fixed option changed"_s, result, StartOutcome::Rejected, "start.fixed-option"_s);
    JITCACHE_CHECK(result.detail == "thresholdForJITSoon: required 100, effective 99"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(!pathExists(artifact));
}

// A failing pack mapping is a Fault at start.io, a damaged pack one at start.pack and a corrupt header one at
// start.header. A faulted VM reports Faulted, with activity off and the start fault as its first fault, and rejects a
// second start; a faulted producing role released the lock, so another producing VM takes it while the faulted state
// lives.
JITCACHE_TEST(integratorStartFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    auto checkFaulted = [&](VM& faultedVM, ASCIILiteral step) {
        VMState& state = *faultedVM.jitCacheState();
        JITCACHE_CHECK(!state.activityOn());
        JITCACHE_CHECK(!state.productionActive());
        JITCACHE_CHECK(!state.artifact());
        JITCACHE_CHECK(!state.writer());
        JITCACHE_CHECK(!producerContext(faultedVM));
        Status reported = JITCache::status(faultedVM);
        JITCACHE_CHECK(reported.state == SessionState::Faulted);
        JITCACHE_CHECK(!reported.activityOn);
        checkFault(context, "the start fault"_s, reported.activityFault, FaultClass::StartFault, step);
        checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::StartFault, step);
        JITCACHE_CHECK(toJSON(reported).contains(makeString("\"step\":\""_s, step, '"')));
        startsAs(context, "a second start on a faulted VM"_s, JITCache::start(faultedVM, testConfig(artifact, Role::Consumer)), StartOutcome::Rejected, "start.already-configured"_s);
    };

    {
        StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::PackMap, EIO, std::nullopt });
        ExtraVM faulted;
        StartResult result = JITCache::start(faulted.vm(), testConfig(artifact, Role::ConsumerProducer));
        StoreTesting::setFault(std::nullopt);
        if (startsAs(context, "a pack mapping that fails"_s, result, StartOutcome::Fault, "start.io"_s)) {
            checkFaulted(faulted.vm(), "start.io"_s);
            Status reported = JITCache::status(faulted.vm());
            JITCACHE_CHECK(reported.role == Role::ConsumerProducer);
            JITCACHE_CHECK(reported.production == ProductionState::Ended);
            checkFault(context, "the end of production"_s, reported.productionFault, FaultClass::StartFault, "start.io"_s);
            JITCACHE_CHECK(reported.budget.limitBytes == testProducerLimitBytes);

            ExtraVM next;
            startsAs(context, "a producing VM beside the faulted one"_s, JITCache::start(next.vm(), testConfig(artifact, Role::ConsumerProducer)), StartOutcome::Started);
        }
    }

    // A pack that fails its own checks is the artifact's damage, which start reports as it reports a corrupt header.
    {
        String damaged = directory->path("damaged-pack"_s);
        if (!createTestArtifact(context, damaged))
            return;
        std::array<uint8_t, 64> notAPack;
        notAPack.fill('p');
        if (!writeFile(context, directory->path("damaged-pack/cache/pack"_s), notAPack))
            return;
        ExtraVM faulted;
        if (startsAs(context, "a damaged pack"_s, JITCache::start(faulted.vm(), testConfig(damaged, Role::Consumer)), StartOutcome::Fault, "start.pack"_s))
            checkFaulted(faulted.vm(), "start.pack"_s);
    }

    String corrupt = directory->path("corrupt"_s);
    std::array<uint8_t, 64> notAHeader;
    notAHeader.fill('x');
    if (!makeDirectory(context, corrupt) || !makeDirectory(context, directory->path("corrupt/cache"_s))
        || !writeFile(context, directory->path("corrupt/cache/header"_s), notAHeader))
        return;
    VM& vm = *context.vm();
    if (startsAs(context, "a corrupt header"_s, JITCache::start(vm, testConfig(corrupt, Role::Consumer)), StartOutcome::Fault, "start.header"_s)) {
        checkFaulted(vm, "start.header"_s);
        Status reported = JITCache::status(vm);
        JITCACHE_CHECK(reported.production == ProductionState::NotProducing);
        JITCACHE_CHECK(!reported.productionFault);
    }
}

#if OS(LINUX)
// A FIFO at cache/header is a corrupt header and one at cache/pack a damaged pack, so start faults at once instead of
// waiting for a writer that never opens the FIFO. A regression shows as this test hanging.
JITCACHE_TEST(integratorStartFaultsOnAFIFO, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;

    String headerFIFO = directory->path("header-fifo"_s);
    if (!createTestArtifact(context, headerFIFO) || !replaceWithFIFO(context, directory->path("header-fifo/cache/header"_s)))
        return;
    {
        ExtraVM faulted;
        if (startsAs(context, "a FIFO at cache/header"_s, JITCache::start(faulted.vm(), testConfig(headerFIFO, Role::Consumer)), StartOutcome::Fault, "start.header"_s))
            JITCACHE_CHECK(JITCache::status(faulted.vm()).state == SessionState::Faulted);
    }

    String packFIFO = directory->path("pack-fifo"_s);
    if (!createTestArtifact(context, packFIFO) || !replaceWithFIFO(context, directory->path("pack-fifo/cache/pack"_s)))
        return;
    for (Role role : { Role::Consumer, Role::ConsumerProducer }) {
        ExtraVM faulted;
        if (startsAs(context, "a FIFO at cache/pack"_s, JITCache::start(faulted.vm(), testConfig(packFIFO, role)), StartOutcome::Fault, "start.pack"_s))
            JITCACHE_CHECK(JITCache::status(faulted.vm()).state == SessionState::Faulted);
    }
}
#endif // OS(LINUX)

// A header-less cache/ holding only temporaries is reset and reused, while names that are not temporaries stay; one whose
// pack may hold a body is start.not-an-artifact and keeps everything; an artifact with a header is
// start.artifact-exists.
JITCACHE_TEST(integratorStartCreatesOverARemnant, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    Vector<uint8_t> filler = patternBytes(33, 3);

    String remnant = directory->path("remnant"_s);
    TemporaryFileName bodyTemporary = temporaryFileName(TemporaryKind::Pack);
    TemporaryFileName headerTemporary = temporaryFileName(TemporaryKind::Header);
    String bodyTemporaryPath = makeString(remnant, "/cache/"_s, String::fromUTF8(bodyTemporary.data()));
    String headerTemporaryPath = makeString(remnant, "/cache/"_s, String::fromUTF8(headerTemporary.data()));
    String otherPath = directory->path("remnant/cache/notes.txt"_s);
    if (!makeDirectory(context, remnant) || !makeDirectory(context, directory->path("remnant/cache"_s))
        || !writeFile(context, bodyTemporaryPath, filler.span()) || !writeFile(context, headerTemporaryPath, filler.span()) || !writeFile(context, otherPath, filler.span()))
        return;
    if (!createTestArtifact(context, remnant))
        return;
    JITCACHE_CHECK(!pathExists(bodyTemporaryPath));
    JITCACHE_CHECK(!pathExists(headerTemporaryPath));
    JITCACHE_CHECK(pathExists(otherPath));
    JITCACHE_CHECK(pathExists(directory->path("remnant/cache/pack"_s)));
    auto header = readFileBytes(directory->path("remnant/cache/header"_s));
    JITCACHE_CHECK(header && equalSpans(header->span(), expectedHeader().span()));
    {
        ExtraVM consumer;
        startsAs(context, "a consumer of the reused remnant"_s, JITCache::start(consumer.vm(), testConfig(remnant, Role::Consumer)), StartOutcome::Started);
    }

    String withBody = directory->path("with-body"_s);
    String bodyPath = makeString(withBody, "/cache/pack"_s);
    String temporaryPath = makeString(withBody, "/cache/"_s, String::fromUTF8(temporaryFileName(TemporaryKind::Pack).data()));
    if (!makeDirectory(context, withBody) || !makeDirectory(context, directory->path("with-body/cache"_s))
        || !writeFile(context, bodyPath, filler.span()) || !writeFile(context, temporaryPath, filler.span()))
        return;
    startsAs(context, "a remnant holding a body"_s, JITCache::start(vm, testConfig(withBody, Role::Producer)), StartOutcome::Rejected, "start.not-an-artifact"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(pathExists(bodyPath));
    JITCACHE_CHECK(pathExists(temporaryPath));
    JITCACHE_CHECK(!pathExists(directory->path("with-body/cache/header"_s)));

    String withPack = directory->path("with-pack"_s);
    if (!makeDirectory(context, withPack) || !makeDirectory(context, directory->path("with-pack/cache"_s))
        || !writeFile(context, directory->path("with-pack/cache/pack"_s), filler.span()))
        return;
    startsAs(context, "a remnant holding a pack"_s, JITCache::start(vm, testConfig(withPack, Role::Producer)), StartOutcome::Rejected, "start.not-an-artifact"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(pathExists(directory->path("with-pack/cache/pack"_s)));

    startsAs(context, "a producer over an artifact"_s, JITCache::start(vm, testConfig(remnant, Role::Producer)), StartOutcome::Rejected, "start.artifact-exists"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
}

// The app a config names. A Producer naming X writes a header naming X, which a Consumer naming X and one naming no app
// open, while one naming Y is rejected at start.another-app with both apps in the detail, and a ConsumerProducer naming Y
// replaces it with an artifact of Y. A ConsumerProducer naming no app opens an artifact of X and keeps its header. A
// Producer naming no app writes a header naming none, which Consumers of every app open. A header both incompatible and
// made for another app is incompatible.
JITCACHE_TEST(integratorStartApps, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    constexpr uint64_t appX = 0x0123456789abcdef;
    constexpr uint64_t appY = 0xfedcba9876543210;
    auto configNaming = [](const String& path, Role role, std::optional<uint64_t> app) {
        Config config = testConfig(path, role);
        config.appDigest = app;
        return config;
    };
    auto startsAnother = [&](ASCIILiteral label, std::optional<uint64_t> app, const String& path, Role role) {
        ExtraVM other;
        return startsAs(context, label, JITCache::start(other.vm(), configNaming(path, role, app)), StartOutcome::Started);
    };
    String detailXInYRun = "app 0123456789abcdef in the cache, fedcba9876543210 in this run"_s;

    String ofX = directory->path("of-x"_s);
    if (!startsAnother("a Producer naming X"_s, appX, ofX, Role::Producer))
        return;
    String ofXHeader = directory->path("of-x/cache/header"_s);
    JITCACHE_CHECK(appOfHeaderAt(context, ofXHeader) == appX);
    auto headerOfX = readFileBytes(ofXHeader);
    JITCACHE_CHECK(headerOfX && equalSpans(headerOfX->span(), headerNamingApp(appX).span()));
    startsAnother("a Consumer naming X"_s, appX, ofX, Role::Consumer);
    startsAnother("a Consumer naming no app"_s, std::nullopt, ofX, Role::Consumer);

    StartResult refused = JITCache::start(vm, configNaming(ofX, Role::Consumer, appY));
    startsAs(context, "a Consumer naming Y"_s, refused, StartOutcome::Rejected, "start.another-app"_s);
    JITCACHE_CHECK(refused.detail == detailXInYRun);
    JITCACHE_CHECK(!vm.jitCacheState());

    {
        ExtraVM replacing;
        StartResult replaced = JITCache::start(replacing.vm(), configNaming(ofX, Role::ConsumerProducer, appY));
        if (startsAs(context, "a ConsumerProducer naming Y"_s, replaced, StartOutcome::Started, "start.replaced"_s)) {
            JITCACHE_CHECK(replaced.detail == detailXInYRun);
            JITCACHE_CHECK(appOfHeaderAt(context, ofXHeader) == appY);
            JITCACHE_CHECK(pathExists(directory->path("of-x/cache/pack"_s)));
            JITCACHE_CHECK(!pathExists(directory->path("of-x/.cache.replaced"_s)));
        }
    }
    {
        ExtraVM consumer;
        StartResult result = JITCache::start(consumer.vm(), configNaming(ofX, Role::Consumer, appX));
        startsAs(context, "a Consumer naming X after the replacement"_s, result, StartOutcome::Rejected, "start.another-app"_s);
        JITCACHE_CHECK(result.detail == "app fedcba9876543210 in the cache, 0123456789abcdef in this run"_s);
    }

    // A ConsumerProducer that names no app opens another app's artifact and leaves its header as it was.
    String alsoOfX = directory->path("also-of-x"_s);
    if (!startsAnother("another Producer naming X"_s, appX, alsoOfX, Role::Producer))
        return;
    auto headerBefore = readFileBytes(directory->path("also-of-x/cache/header"_s));
    {
        ExtraVM opening;
        StartResult opened = JITCache::start(opening.vm(), configNaming(alsoOfX, Role::ConsumerProducer, std::nullopt));
        startsAs(context, "a ConsumerProducer naming no app"_s, opened, StartOutcome::Started);
    }
    auto headerAfter = readFileBytes(directory->path("also-of-x/cache/header"_s));
    JITCACHE_CHECK(headerBefore && headerAfter && *headerBefore == *headerAfter);

    // A Producer that names no app writes the process's own header, which every config opens.
    String ofNone = directory->path("of-none"_s);
    if (!startsAnother("a Producer naming no app"_s, std::nullopt, ofNone, Role::Producer))
        return;
    String ofNoneHeader = directory->path("of-none/cache/header"_s);
    JITCACHE_CHECK(appOfHeaderAt(context, ofNoneHeader) == std::nullopt);
    auto headerOfNone = readFileBytes(ofNoneHeader);
    JITCACHE_CHECK(headerOfNone && equalSpans(headerOfNone->span(), expectedHeader().span()));
    startsAnother("a Consumer naming X of an artifact naming none"_s, appX, ofNone, Role::Consumer);
    startsAnother("a Consumer naming Y of an artifact naming none"_s, appY, ofNone, Role::Consumer);

    // A header that differs in a must-match option and names X is incompatible, not made for another app.
    String both = directory->path("both"_s);
    Vector<uint8_t> header = headerNamingAppOf(headerWithFlippedOption(0), appX);
    Vector<uint8_t> packBytes = patternBytes(33, 5);
    if (!makeDirectory(context, both) || !makeDirectory(context, directory->path("both/cache"_s))
        || !writeFile(context, directory->path("both/cache/header"_s), header.span())
        || !writeFile(context, directory->path("both/cache/pack"_s), packBytes.span()))
        return;
    StartResult incompatible = JITCache::start(vm, configNaming(both, Role::Consumer, appY));
    startsAs(context, "a Consumer naming Y of a header both incompatible and of X"_s, incompatible, StartOutcome::Rejected, "start.incompatible"_s);
    JITCACHE_CHECK(incompatible.detail.contains("evalMode"_s));
    JITCACHE_CHECK(!vm.jitCacheState());
}

// Invalid material, in either form, turns activity off, which ends production, and later faults keep the first.
JITCACHE_TEST(integratorInvalidMaterialTurnsActivityOff, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        VMState& state = *extra.vm().jitCacheState();
        state.raiseInvalidMaterial("ucb.identity"_s, "a test's detail"_s);
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "ucb.identity"_s);
        JITCACHE_CHECK(JITCache::status(extra.vm()).activityFault->detail == "a test's detail"_s);
        state.raiseRecordingFault("cb"_s, "later-check"_s, { });
        state.raiseInvalidMaterial("image"_s, "later-check"_s, { });
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "ucb.identity"_s);
    }
    {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        extra.vm().jitCacheState()->raiseInvalidMaterial("image"_s, "test-check"_s, { });
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "image.test-check"_s);
    }
}

// A recording fault ends production for good and leaves activity on, so a ConsumerProducer goes on importing, and its
// budget refuses every later charge. Invalid material afterwards turns activity off, and the recording fault stays the
// first fault.
JITCACHE_TEST(integratorRecordingFaultEndsProduction, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    if (!startConsumerProducer(context, vm, artifact))
        return;
    VMState& state = *vm.jitCacheState();

    state.raiseRecordingFault("ucb"_s, "capture-budget"_s, { });
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(state.tracksKeys());
    JITCACHE_CHECK(state.importsEnabled());
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!producerContext(vm));
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(1));
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.activityOn);
    JITCACHE_CHECK(!reported.activityFault);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    JITCACHE_CHECK(reported.budget.refused);
    checkFault(context, "the recording fault"_s, reported.productionFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);

    // The end of production frees what production held, at the next glue entry.
    state.releaseEndedProductionMemory();
#if ENABLE(JIT)
    JITCACHE_CHECK(!state.partIfExists<KeptSummaries>());
#endif
    JITCACHE_CHECK(!state.producerBudget()->chargedBytes());

    state.raiseInvalidMaterial("ucb.feedback"_s, { });
    reported = JITCache::status(vm);
    JITCACHE_CHECK(!reported.activityOn);
    JITCACHE_CHECK(!state.importsEnabled());
    checkFault(context, "the later activity fault"_s, reported.activityFault, FaultClass::InvalidMaterial, "ucb.feedback"_s);
    checkFault(context, "the recording fault"_s, reported.productionFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
}

// VMState::part creates a part at its type's first use and returns the same part afterwards, and partIfExists finds it
// only from then on. The release of ended production memory destroys the production parts alone, and willDestroyVM the
// others, while the VM still holds its state.
JITCACHE_TEST(integratorVMPartsEndWithTheirLifetimes, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    bool productionPartDestroyed = false;
    bool vmPartDestroyedBeforeTheState = false;
    {
        ExtraVM producer;
        if (!startsAs(context, "the producer"_s, JITCache::start(producer.vm(), testConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
            return;
        VMState& state = *producer.vm().jitCacheState();
        JITCACHE_CHECK(!state.partIfExists<ProductionTestPart>());
        ProductionTestPart& productionPart = state.part<ProductionTestPart>(productionPartDestroyed);
        JITCACHE_CHECK(&state.part<ProductionTestPart>(productionPartDestroyed) == &productionPart);
        JITCACHE_CHECK(state.partIfExists<ProductionTestPart>() == &productionPart);
        JITCACHE_CHECK(!state.partIfExists<VMTestPart>());
        VMTestPart& vmPart = state.part<VMTestPart>(vmPartDestroyedBeforeTheState, producer.vm());
        JITCACHE_CHECK(state.partIfExists<VMTestPart>() == &vmPart);

        // A fault entry point destroys nothing; the release that follows destroys the production part alone.
        state.raiseRecordingFault("cb"_s, "test-check"_s, { });
        JITCACHE_CHECK(!productionPartDestroyed);
        state.releaseEndedProductionMemory();
        JITCACHE_CHECK(productionPartDestroyed);
        JITCACHE_CHECK(!state.partIfExists<ProductionTestPart>());
        JITCACHE_CHECK(!vmPartDestroyedBeforeTheState);
        JITCACHE_CHECK(state.partIfExists<VMTestPart>() == &vmPart);
    }
    JITCACHE_CHECK(vmPartDestroyedBeforeTheState);
}

// A budget that refused a charge the VM thread has not raised yet reports production Ended at budget.limit, while the
// switch itself stays on until a capture or delta raises it.
JITCACHE_TEST(integratorUnraisedRefusalReportsBudgetLimit, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    Config config = testConfig(directory->path("artifact"_s), Role::Producer);
    config.producerLimitBytes = 16;
    if (!startsAs(context, "a producer with a small limit"_s, JITCache::start(vm, config), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(17));
    JITCACHE_CHECK(state.productionActive());
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.activityOn);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    JITCACHE_CHECK(reported.budget.refused);
    JITCACHE_CHECK(reported.budget.limitBytes == 16);
    JITCACHE_CHECK(!reported.activityFault);
    checkFault(context, "the unraised refusal"_s, reported.productionFault, FaultClass::RecordingFault, "budget.limit"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "budget.limit"_s);
}

// Each executable-allocation site turns activity off and names its own step, and a second report keeps the first.
JITCACHE_TEST(integratorExecutableAllocationFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    struct SiteStep {
        ExecutableAllocationSite site;
        ASCIILiteral step;
    };
    std::array sites {
        SiteStep { ExecutableAllocationSite::BaselinePlan, "exec-alloc.baseline-plan"_s },
        SiteStep { ExecutableAllocationSite::DFGPlan, "exec-alloc.dfg-plan"_s },
        SiteStep { ExecutableAllocationSite::FTLPlan, "exec-alloc.ftl-plan"_s },
        SiteStep { ExecutableAllocationSite::InlineCacheHandler, "exec-alloc.ic-handler"_s },
        SiteStep { ExecutableAllocationSite::MathICSnippet, "exec-alloc.mathic-snippet"_s },
        SiteStep { ExecutableAllocationSite::JITCacheImage, "exec-alloc.jitcache-image"_s },
    };
    for (auto& [site, step] : sites) {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        didFailExecutableAllocation(extra.vm(), site);
        checkActivityOff(context, extra.vm(), FaultClass::ExecutableMemory, step);
        didFailExecutableAllocation(extra.vm(), site == ExecutableAllocationSite::JITCacheImage ? ExecutableAllocationSite::BaselinePlan : ExecutableAllocationSite::JITCacheImage);
        checkActivityOff(context, extra.vm(), FaultClass::ExecutableMemory, step);
    }
}

// didFailExecutableAllocation called with a CB's m_lock held through a GCSafeConcurrentJSLocker returns, which it would
// not if it took that lock, and records its fault. The CB's function runs before start, so none of its bodies is
// JITCache's.
JITCACHE_TEST(integratorExecutableAllocationFaultUnderCodeBlockLock, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    CodeBlock* codeBlock = calledCodeBlock(context, globalObject);
    if (!codeBlock || !startConsumerProducer(context, vm, artifact))
        return;
    {
        GCSafeConcurrentJSLocker locker(codeBlock->m_lock, vm);
        didFailExecutableAllocation(vm, ExecutableAllocationSite::InlineCacheHandler);
    }
    checkActivityOff(context, vm, FaultClass::ExecutableMemory, "exec-alloc.ic-handler"_s);
}

// A JSC::Debugger attached to a global object of a configured VM turns activity off, and status names debugger.attach
// as the activity fault and as the end of production. A fault raised afterwards finds activity off and records nothing.
JITCACHE_TEST(integratorDebuggerAttachTurnsActivityOff, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    if (!startConsumerProducer(context, vm, artifact))
        return;
    Debugger debugger(vm);
    debugger.attach(globalObject);
    checkActivityOff(context, vm, FaultClass::DebuggerAttached, "debugger.attach"_s);
    vm.jitCacheState()->raiseInvalidMaterial("ucb.identity"_s, { });
    didFailExecutableAllocation(vm, ExecutableAllocationSite::BaselinePlan);
    checkActivityOff(context, vm, FaultClass::DebuggerAttached, "debugger.attach"_s);
    debugger.detach(globalObject, Debugger::TerminatingDebuggingSession);
}

// A VM without state ignores every entry point, and start still configures it afterwards.
JITCACHE_TEST(integratorFaultEntryPointsIgnoreAnUnconfiguredVM, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    constexpr std::array sites { ExecutableAllocationSite::BaselinePlan, ExecutableAllocationSite::DFGPlan, ExecutableAllocationSite::FTLPlan,
        ExecutableAllocationSite::InlineCacheHandler, ExecutableAllocationSite::MathICSnippet, ExecutableAllocationSite::JITCacheImage };
    for (auto site : sites)
        didFailExecutableAllocation(vm, site);
    didAttachDebugger(vm);
    flushBenchReport(vm);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(!producerContext(vm));
    JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Unconfigured);
    startConsumerProducer(context, vm, artifact);
}

// Body lookups in a Consumer over an artifact holding a module's body the writer committed with its ucb.analysis:
// bodyVersion from the index alone, openBody, readBodySection in step with them, a transient error, the test override,
// and last an error that turns activity off.
JITCACHE_TEST(integratorBodyLookups, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    BodyKey committedKey = testKey(21, IdentityKind::Module);
    BodyKey absentKey = testKey(22);
    Vector<uint8_t> committedAnalysis = patternBytes(29, 21);
    std::optional<CommitResult> committed;
    {
        ExtraVM producer;
        if (!startsAs(context, "the producer"_s, JITCache::start(producer.vm(), testConfig(artifact, Role::Producer)), StartOutcome::Started))
            return;
        committed = commitTestBody(context, *producer.vm().jitCacheState(), committedKey, 21, committedAnalysis.span());
        if (!committed)
            return;
    }

    VM& vm = *context.vm();
    if (!startsAs(context, "the consumer"_s, JITCache::start(vm, testConfig(artifact, Role::Consumer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(JITCache::status(vm).progress.indexedBodies == 1);

    // bodyVersion reads the index alone: a hook that fails every open, and then every mapping, never fires for it.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    uint64_t token = state.bodyVersion(committedKey);
    JITCACHE_CHECK(token);
    JITCACHE_CHECK(!state.bodyVersion(absentKey));
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::PackMap, EIO, std::nullopt });
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(!state.progress().bodyOpens);

    // openBody maps and checks the file, whose body carries the file's commit identifier, size, L and P.
    BodyLookup found = state.openBody(committedKey);
    JITCACHE_CHECK(found.kind() == BodyLookup::Kind::Found);
    if (RefPtr body = found.body()) {
        JITCACHE_CHECK(body->key() == committedKey);
        JITCACHE_CHECK(body->version() == committed->version);
        JITCACHE_CHECK(body->fileSize() == committed->fileSize);
        JITCACHE_CHECK(body->llintThreshold() == testBodyLLIntThreshold);
        JITCACHE_CHECK(body->counterProgress() == testBodyCounterProgress);
    }
    JITCACHE_CHECK(state.openBody(absentKey).kind() == BodyLookup::Kind::Missing);
    JITCACHE_CHECK(state.progress().bodyOpens == 1);

    // readBodySection reads one section of the same body, which its lookup keeps readable, and counts no body open.
    {
        SectionLookup analysis = state.readBodySection(committedKey, SectionKind::UCBAnalysis);
        JITCACHE_CHECK(analysis.kind() == SectionLookup::Kind::Found && equalSpans(analysis.bytes(), committedAnalysis.span()));
        SectionLookup moved = WTF::move(analysis);
        JITCACHE_CHECK(moved.kind() == SectionLookup::Kind::Found && equalSpans(moved.bytes(), committedAnalysis.span()));
        JITCACHE_CHECK(state.readBodySection(absentKey, SectionKind::UCBAnalysis).kind() == SectionLookup::Kind::Absent);
    }
    JITCACHE_CHECK(state.progress().bodyOpens == 1);
    JITCACHE_CHECK(state.activityOn());

    // A transient error is a miss, counted, and the artifact is not at fault.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EMFILE, std::nullopt });
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Missing);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(JITCache::status(vm).progress.transientOpenFailures == 1);
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);

    // The override answers alone, a test body included, whose commit identifier differs from the token it gives, and
    // moves no counter.
    constexpr uint64_t overrideToken = 7;
    constexpr uint64_t testVersion = 99;
    std::array<uint8_t, 8> feedback { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::array<ValidatedBody::TestSection, 1> testSections { { { SectionKind::UCBFeedback, feedback } } };
    Ref<ValidatedBody> testBody = ValidatedBody::createForTesting(committedKey, testVersion, testSections);
    auto setOverride = [&] {
        auto token = [&](const BodyKey& key) -> uint64_t {
            return key == committedKey ? overrideToken : 0;
        };
        auto open = [&](const BodyKey& key) {
            return key == committedKey ? BodyLookup::found(testBody.copyRef()) : BodyLookup::missing();
        };
        state.setBodyLookupForTesting(WTF::move(token), WTF::move(open));
    };
    setOverride();
    JITCACHE_CHECK(state.bodyVersion(committedKey) == overrideToken);
    JITCACHE_CHECK(!state.bodyVersion(absentKey));
    BodyLookup overridden = state.openBody(committedKey);
    JITCACHE_CHECK(overridden.kind() == BodyLookup::Kind::Found && overridden.body() == testBody.ptr());
    JITCACHE_CHECK(testBody->version() != state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(absentKey).kind() == BodyLookup::Kind::Missing);
    // readBodySection answers with the override's body's section, empty for a kind the body lacks.
    SectionLookup overriddenSection = state.readBodySection(committedKey, SectionKind::UCBFeedback);
    JITCACHE_CHECK(overriddenSection.kind() == SectionLookup::Kind::Found && equalSpans(overriddenSection.bytes(), std::span<const uint8_t> { feedback }));
    SectionLookup overriddenAnalysis = state.readBodySection(committedKey, SectionKind::UCBAnalysis);
    JITCACHE_CHECK(overriddenAnalysis.kind() == SectionLookup::Kind::Found && overriddenAnalysis.bytes().empty());
    JITCACHE_CHECK(state.readBodySection(absentKey, SectionKind::UCBAnalysis).kind() == SectionLookup::Kind::Absent);
    JITCACHE_CHECK(state.progress().bodyOpens == 1);
    JITCACHE_CHECK(state.progress().transientOpenFailures == 1);

    state.clearBodyLookupForTesting();
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);
    BodyLookup again = state.openBody(committedKey);
    JITCACHE_CHECK(again.kind() == BodyLookup::Kind::Found && again.body() && again.body()->version() == committed->version);
    SectionLookup analysisAgain = state.readBodySection(committedKey, SectionKind::UCBAnalysis);
    JITCACHE_CHECK(analysisAgain.kind() == SectionLookup::Kind::Found && equalSpans(analysisAgain.bytes(), committedAnalysis.span()));

    // Last: any other error is invalid material at container.io, which turns activity off; the three calls then answer
    // 0, Unusable and Unusable, with or without the override.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(!state.activityOn());
    checkFault(context, "the open's fault"_s, JITCache::status(vm).activityFault, FaultClass::InvalidMaterial, "container.io"_s);
    JITCACHE_CHECK(!state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    JITCACHE_CHECK(state.readBodySection(committedKey, SectionKind::UCBAnalysis).kind() == SectionLookup::Kind::Unusable);
    setOverride();
    JITCACHE_CHECK(!state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    JITCACHE_CHECK(state.readBodySection(committedKey, SectionKind::UCBFeedback).kind() == SectionLookup::Kind::Unusable);
    state.clearBodyLookupForTesting();
}

// The bench report's start event and summary lines. A rejected start closes its report again with its start event
// written and no summary line. A start that configures the VM keeps its report and hands it the artifact and, for a
// producing role, the producer budget. flushBenchReport then writes the index line for every role, so a Consumer's names
// the one body its artifact holds, and a ConsumerProducer's budget line holds the producer limit.
JITCACHE_TEST(integratorStartBenchEvent, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    {
        ExtraVM producer;
        if (!startsAs(context, "the producer"_s, JITCache::start(producer.vm(), testConfig(artifact, Role::Producer)), StartOutcome::Started))
            return;
        if (!commitTestBody(context, *producer.vm().jitCacheState(), testKey(23), 23))
            return;
    }

    String rejectedReport = directory->path("rejected.jsonl"_s);
    {
        ExtraVM rejected;
        Config config = testConfig(directory->path("missing"_s), Role::Consumer);
        config.benchReportPath = rejectedReport;
        startsAs(context, "a rejected start with a bench report"_s, JITCache::start(rejected.vm(), config), StartOutcome::Rejected, "start.artifact-missing"_s);
    }
    String rejectedLines = readTextFile(rejectedReport);
    JITCACHE_CHECK(rejectedLines.contains("\"event\":\"start\""_s));
    JITCACHE_CHECK(rejectedLines.contains("\"outcome\":\"rejected\",\"role\":\"consumer\",\"nanoseconds\":"_s));
    JITCACHE_CHECK(rejectedLines.contains("\"indexedBodies\":0}"_s));
    JITCACHE_CHECK(!rejectedLines.contains("\"event\":\"budget\""_s));
    JITCACHE_CHECK(!rejectedLines.contains("\"event\":\"index\""_s));

    // No line but the index line writes bodies.
    String consumerReport = directory->path("consumer.jsonl"_s);
    {
        ExtraVM consumer;
        Config config = testConfig(artifact, Role::Consumer);
        config.benchReportPath = consumerReport;
        if (!startsAs(context, "a Consumer with a bench report"_s, JITCache::start(consumer.vm(), config), StartOutcome::Started))
            return;
        flushBenchReport(consumer.vm());
        String consumerLines = readTextFile(consumerReport);
        JITCACHE_CHECK(consumerLines.contains("\"event\":\"index\""_s));
        JITCACHE_CHECK(consumerLines.contains("\"bodies\":1,"_s));
    }

    VM& vm = *context.vm();
    String startedReport = directory->path("started.jsonl"_s);
    Config config = testConfig(artifact, Role::ConsumerProducer);
    config.benchReportPath = startedReport;
    if (!startsAs(context, "a start with a bench report"_s, JITCache::start(vm, config), StartOutcome::Started))
        return;
    JITCACHE_CHECK(vm.jitCacheState()->benchReport());
    flushBenchReport(vm);
    String startedLines = readTextFile(startedReport);
    JITCACHE_CHECK(startedLines.contains("\"outcome\":\"started\",\"role\":\"consumer-producer\",\"nanoseconds\":"_s));
    JITCACHE_CHECK(startedLines.contains("\"event\":\"budget\""_s));
    JITCACHE_CHECK(startedLines.contains(makeString("\"limit\":"_s, static_cast<uint64_t>(testProducerLimitBytes))));
}

// The JSON lines of start, delta and status: every field of the struct by name, enums by their names, an absent optional
// as null and a fault as {"class","step","detail"}; and the names and parseRole they share.
JITCACHE_TEST(integratorJSONLines, No)
{
    StartResult rejected { StartOutcome::Rejected, "start.config"_s, "artifactPath \"x\" is empty"_s };
    JITCACHE_CHECK(toJSON(rejected) == "{\"jitcache\":\"start\",\"outcome\":\"rejected\",\"step\":\"start.config\",\"detail\":\"artifactPath \\\"x\\\" is empty\"}"_s);
    StartResult started { StartOutcome::Started, { }, { } };
    JITCACHE_CHECK(toJSON(started) == "{\"jitcache\":\"start\",\"outcome\":\"started\",\"step\":\"\",\"detail\":\"\"}"_s);
    StartResult replaced { StartOutcome::Started, "start.replaced"_s, "its header differs in evalMode"_s };
    JITCACHE_CHECK(toJSON(replaced) == "{\"jitcache\":\"start\",\"outcome\":\"started\",\"step\":\"start.replaced\",\"detail\":\"its header differs in evalMode\"}"_s);

    DeltaResult faulted { DeltaOutcome::Faulted, { }, FaultReport { FaultClass::RecordingFault, "ucb"_s, "capture-budget"_s, "a detail"_s }, 3, 1, 4096, 2 };
    JITCACHE_CHECK(toJSON(faulted) == "{\"jitcache\":\"delta\",\"outcome\":\"faulted\",\"rejection\":\"\",\"fault\":{\"class\":\"recording-fault\",\"step\":\"ucb.capture-budget\",\"detail\":\"a detail\"},\"eligibleKeys\":3,\"committedBodies\":1,\"committedBytes\":4096,\"deferredKeys\":2}"_s);
    DeltaResult rejectedDelta { DeltaOutcome::Rejected, "delta.role"_s, std::nullopt };
    JITCACHE_CHECK(toJSON(rejectedDelta) == "{\"jitcache\":\"delta\",\"outcome\":\"rejected\",\"rejection\":\"delta.role\",\"fault\":null,\"eligibleKeys\":0,\"committedBodies\":0,\"committedBytes\":0,\"deferredKeys\":0}"_s);

    JITCACHE_CHECK(toJSON(Status { }) == "{\"jitcache\":\"status\",\"state\":\"unconfigured\",\"role\":null,\"activityOn\":false,\"activityFault\":null,\"production\":\"not-producing\",\"productionFault\":null,\"firstFault\":null,\"progress\":{\"indexedBodies\":0,\"bodyOpens\":0,\"transientOpenFailures\":0,\"imports\":0,\"attaches\":0,\"gateDrops\":0,\"misses\":0,\"installs\":0,\"bakedFactMismatches\":0,\"captureCandidates\":0,\"capturesDeferred\":0,\"capturesCommitted\":0,\"bytesCommitted\":0,\"deltaRuns\":0},\"budget\":{\"limitBytes\":0,\"chargedBytes\":0,\"peakBytes\":0,\"refused\":false}}"_s);
    Status faultedStatus;
    faultedStatus.state = SessionState::Started;
    faultedStatus.role = Role::ConsumerProducer;
    faultedStatus.activityFault = FaultReport { FaultClass::DebuggerAttached, { }, "debugger.attach"_s, { } };
    faultedStatus.production = ProductionState::Ended;
    faultedStatus.progress.installs = 5;
    faultedStatus.budget.refused = true;
    String json = toJSON(faultedStatus);
    JITCACHE_CHECK(json.contains("\"state\":\"started\""_s));
    JITCACHE_CHECK(json.contains("\"role\":\"consumer-producer\""_s));
    JITCACHE_CHECK(json.contains("\"activityFault\":{\"class\":\"debugger-attached\",\"step\":\"debugger.attach\",\"detail\":\"\"}"_s));
    JITCACHE_CHECK(json.contains("\"production\":\"ended\""_s));
    JITCACHE_CHECK(json.contains("\"installs\":5"_s));
    JITCACHE_CHECK(json.contains("\"refused\":true}}"_s));
    faultedStatus.state = SessionState::Faulted;
    JITCACHE_CHECK(toJSON(faultedStatus).contains("\"state\":\"faulted\""_s));

    JITCACHE_CHECK((FaultReport { FaultClass::InvalidMaterial, { }, "container.io"_s, { } }.stepName() == "container.io"_s));
    JITCACHE_CHECK(name(StartOutcome::Started) == "started"_s);
    JITCACHE_CHECK(name(StartOutcome::Busy) == "busy"_s);
    JITCACHE_CHECK(name(StartOutcome::Rejected) == "rejected"_s);
    JITCACHE_CHECK(name(StartOutcome::Fault) == "fault"_s);
    JITCACHE_CHECK(name(FaultClass::ExecutableMemory) == "executable-memory"_s);
    for (Role role : { Role::Consumer, Role::Producer, Role::ConsumerProducer })
        JITCACHE_CHECK(parseRole(StringView { name(role) }) == role);
    JITCACHE_CHECK(!parseRole("Consumer"_s));
    JITCACHE_CHECK(!parseRole("p-c"_s));
    JITCACHE_CHECK(name(static_cast<Role>(7)).isNull());
}

// The delta capture at start: the flag's two spellings, All by default, a value outside the enumeration rejected at
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

    auto directory = TemporaryDirectory::create(context, "jitcache-run-once"_s);
    if (!directory)
        return;
    VM& vm = *context.vm();
    for (uint8_t value : { 0, 3, 255 }) {
        StartResult result = JITCache::start(vm, testConfig(directory->path("artifact"_s), Role::Producer, static_cast<DeltaCapture>(value)));
        if (startsAs(context, "a delta capture outside the enumeration"_s, result, StartOutcome::Rejected, "start.config"_s))
            JITCACHE_CHECK(result.detail.contains("deltaCapture"_s));
        JITCACHE_CHECK(!vm.jitCacheState());
    }

    if (!startsAs(context, "a Producer with Compiled"_s, JITCache::start(vm, testConfig(directory->path("artifact"_s), Role::Producer, DeltaCapture::Compiled)), StartOutcome::Started))
        return;
    JITCACHE_CHECK(vm.jitCacheState()->deltaCapture() == DeltaCapture::Compiled);
    JITCACHE_CHECK(!vm.jitCacheState()->capturesUncompiledBodies());
    {
        ExtraVM producer;
        if (startsAs(context, "a Producer with All"_s, JITCache::start(producer.vm(), testConfig(directory->path("other"_s), Role::Producer, DeltaCapture::All)), StartOutcome::Started)) {
            JITCACHE_CHECK(producer.vm().jitCacheState()->deltaCapture() == DeltaCapture::All);
            JITCACHE_CHECK(producer.vm().jitCacheState()->capturesUncompiledBodies());
        }
    }
    {
        ExtraVM consumer;
        if (startsAs(context, "a Consumer with All"_s, JITCache::start(consumer.vm(), testConfig(directory->path("artifact"_s), Role::Consumer, DeltaCapture::All)), StartOutcome::Started))
            JITCACHE_CHECK(!consumer.vm().jitCacheState()->capturesUncompiledBodies());
    }
}

// The check over the copied headers; the other half, Bun's build against those copies, is a build. Bun compiles against
// the copies JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS puts in <build>/JavaScriptCore/PrivateHeaders/JavaScriptCore and
// against no source directory, so no copy other than the module's own exported headers may include a jitcache/ header,
// and every jitcache/ header any copy includes must be a copy itself. testjitcache runs from <build>/bin, and the
// build's CMake cache names the source root whose jitcache/ directory lists the module's headers, so a copy that comes
// to include a header added there later fails as well.
JITCACHE_TEST(integratorExportedHeadersIncludeOnlyExportedHeaders, No)
{
    auto directories = moduleDirectories(context);
    if (!directories)
        return;
    String jitcacheDirectory = directories->jitcache;
    std::array<StringView, 3> copiesComponents { "JavaScriptCore"_s, "PrivateHeaders"_s, "JavaScriptCore"_s };
    String copiesDirectory = FileSystem::pathByAppendingComponents(directories->build, copiesComponents);

    HashSet<String> jitcacheHeaders;
    integratorAddHeaderNamesBelow(jitcacheDirectory, jitcacheHeaders);
    HashSet<String> copies;
    for (auto& name : FileSystem::listDirectory(copiesDirectory)) {
        if (name.endsWith(".h"_s))
            copies.add(name);
    }
    if (!jitcacheHeaders.contains("ImageRecorder.h"_s) || !jitcacheHeaders.contains("JITCacheAPI.h"_s)) {
        JITCACHE_FAIL(makeString(jitcacheDirectory, " does not list the module's headers"_s));
        return;
    }
    bool everyOwnHeaderCopied = true;
    for (auto exported : exportedJITCacheHeaders) {
        if (!copies.contains(exported)) {
            JITCACHE_FAIL(makeString(copiesDirectory, " holds no copy of the exported "_s, exported));
            everyOwnHeaderCopied = false;
        }
    }
    if (!everyOwnHeaderCopied)
        return;

    for (auto& copy : copies) {
        auto text = readSourceText(FileSystem::pathByAppendingComponent(copiesDirectory, copy));
        if (!text) {
            JITCACHE_FAIL(makeString("the copy "_s, copy, " cannot be read"_s));
            continue;
        }
        bool ownHeader = std::ranges::any_of(exportedJITCacheHeaders, [&](ASCIILiteral exported) { return copy == exported; });
        for (auto& path : includedPaths(*text)) {
            String included = fileNameOfPath(path);
            if (!jitcacheHeaders.contains(included))
                continue;
            // An engine header reaches the module only through declarations of its own, never through a jitcache/
            // header, exported or not.
            if (!ownHeader)
                JITCACHE_FAIL(makeString("the exported header "_s, copy, " includes the jitcache/ header "_s, included));
            // A jitcache/ header that an exported header includes must resolve among the copies Bun sees.
            if (!copies.contains(included))
                JITCACHE_FAIL(makeString("the exported header "_s, copy, " includes "_s, included, ", a jitcache/ header that is not exported"_s));
        }
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
