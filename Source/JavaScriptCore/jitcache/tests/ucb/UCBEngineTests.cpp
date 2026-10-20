#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BaselineJITCode.h"
#include "BuiltinExecutables.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DeferGC.h"
#include "DirectEvalExecutable.h"
#include "DirectEvalSite.h"
#include "JITCacheAPI.h"
#include "JITCacheSHA256.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "LLIntData.h"
#include "ModuleProgramExecutable.h"
#include "ProgramExecutable.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "TestArtifact.h"
#include "TestFiles.h"
#include "TwinReport.h"
#include "UCBCapture.h"
#include "UCBFeedback.h"
#include "UCBImport.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UCBRequests.h"
#include "UCBSections.h"
#include "UCBTestSupport.h"
#include "UCBTwinGeneration.h"
#include "UCBTwins.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "ValidatedBody.h"
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of the UCB engine in UCBImport, with the integrator's body lookup stubbed: attaches and imports with the misses
// and stamps of each, the copies an attach or an import leaves, an attach with strict off to a UCB whose encoding is not
// the body's coreDigest, code a native decode makes, which nothing imports or records, a private module's request,
// supplied digests and the invalid material a wrong one raises. The parts act on the VM's own registry
// and statistics, as the engine does; their UCBs are unpublished or test-held, so no CodeBlock installs what they attach.
// Each test starts JITCache on its VM in one configuration, with a twin report in a temporary file, and checks what a run
// in that configuration must leave: a fault only where the test expects one, a registry verifyRegistry finds sound, and no
// twin difference but the ones the test expects.

namespace JSC::JITCache::Tests {

namespace UCBEngineTestsInternal {

// The stubbed index and store: each listed key has the index token bodyVersion answers and the body openBody returns, or
// no body for one the index lists but the store no longer holds.
class StubbedArtifact {
    WTF_MAKE_NONCOPYABLE(StubbedArtifact);

public:
    StubbedArtifact() = default;

    void list(const BodyKey& key, uint64_t token, RefPtr<ValidatedBody>&& body)
    {
        if (Entry* entry = find(key)) {
            entry->token = token;
            entry->body = WTF::move(body);
            return;
        }
        m_entries.append(Entry { key, token, WTF::move(body), 0 });
    }

    void unlist(const BodyKey& key)
    {
        m_entries.removeFirstMatching([&](const Entry& entry) {
            return entry.key == key;
        });
    }

    uint64_t token(const BodyKey& key) const
    {
        const Entry* entry = find(key);
        return entry ? entry->token : 0;
    }

    BodyLookup open(const BodyKey& key)
    {
        Entry* entry = find(key);
        if (!entry)
            return BodyLookup::missing();
        ++entry->opens;
        if (!entry->body)
            return BodyLookup::missing();
        return BodyLookup::found(Ref<ValidatedBody> { *entry->body });
    }

    unsigned opens(const BodyKey& key) const
    {
        const Entry* entry = find(key);
        return entry ? entry->opens : 0;
    }

private:
    struct Entry {
        BodyKey key;
        uint64_t token;
        RefPtr<ValidatedBody> body;
        unsigned opens;
    };

    const Entry* find(const BodyKey& key) const
    {
        for (const Entry& entry : m_entries) {
            if (entry.key == key)
                return &entry;
        }
        return nullptr;
    }

    Entry* find(const BodyKey& key) { return const_cast<Entry*>(std::as_const(*this).find(key)); }

    Vector<Entry> m_entries;
};

// Serves a StubbedArtifact through the VMState's lookup override while it lives.
class StubbedBodyLookup {
    WTF_MAKE_NONCOPYABLE(StubbedBodyLookup);

public:
    StubbedBodyLookup(VMState& state, StubbedArtifact& artifact)
        : m_state(state)
    {
        m_state.setBodyLookupForTesting([&artifact](const BodyKey& key) {
            return artifact.token(key);
        }, [&artifact](const BodyKey& key) {
            return artifact.open(key);
        });
    }

    ~StubbedBodyLookup() { m_state.clearBodyLookupForTesting(); }

private:
    VMState& m_state;
};

// The three sections a capture of a UCB writes, and for a body with an image an empty image section, which gives it highest
// tier 1, so its imports and attaches leave pending imports, which no test here installs. A body without one has highest
// tier 0: its import leaves no pending import.
struct TestBody {
    Vector<uint8_t> identity;
    Vector<uint8_t> core;
    Vector<uint8_t> feedback;
    BodyImage image { BodyImage::Present };

    Ref<ValidatedBody> validated(const BodyKey& key, uint64_t version) const
    {
        std::array<ValidatedBody::TestSection, 4> sections { {
            { SectionKind::UCBIdentity, identity.span() },
            { SectionKind::UCBCore, core.span() },
            { SectionKind::UCBFeedback, feedback.span() },
            { SectionKind::ImageBaseline, { } },
        } };
        std::span<const ValidatedBody::TestSection> given { sections };
        return ValidatedBody::createForTesting(key, version, image == BodyImage::Present ? given : given.first(3));
    }
};

// The sections a capture of `producer` would write under the given key and context, with a function body's parse fields
// and holder, as buildSections writes them. A body with an image comes from a UCB that a baseline compilation reached,
// which allocated its value and array copies, so the producer gets them first; its feedback section then carries them.
static std::optional<TestBody> buildTestBody(VM& vm, UnlinkedCodeBlock& producer, const UnlinkedFunctionExecutable* holder, const BodyKey& key, const Digest256& context,
    const std::optional<FunctionParseFields>& parseFields = std::nullopt, BodyImage image = BodyImage::Present)
{
    if (image == BodyImage::Present)
        producer.ensureValueAndArrayProfiles();
    auto core = encodeCoreBytes(vm, producer, holder);
    if (!core)
        return std::nullopt;
    TestBody body;
    body.core = WTF::move(*core);
    body.image = image;
    FeedbackCounts counts = liveFeedbackCounts(producer, image);
    body.feedback = Vector<uint8_t>(feedbackSectionSize(counts));
    writeFeedbackSection(body.feedback.mutableSpan(), producer, counts);
    std::optional<Digest256> holderDigestValue = holder ? holderDigest(vm, *holder) : std::nullopt;
    auto constantCount = static_cast<uint32_t>(producer.constantRegisters().size());
    body.identity = Vector<uint8_t>(identitySectionSize(constantCount));
    writeIdentitySection(body.identity.mutableSpan(), vm, key, context, SHA256::hash(body.core.span()), holderDigestValue, coreKindFor(key.identityKind()), parseFields, producer);
    return body;
}

static FunctionParseFields parseFieldsOf(const TwinParseResults& results)
{
    return FunctionParseFields { results.features, results.lexicallyScopedFeatures, results.hasCapturedVariables };
}

static FunctionParseFields parseFieldsOf(const UnlinkedFunctionExecutable& executable)
{
    return FunctionParseFields { executable.features(), executable.lexicallyScopedFeatures(), executable.hasCapturedVariables() };
}

struct ProgramRequestFixture {
    SourceCode source;
    Strong<ProgramExecutable> executable;
};

static ProgramRequestFixture programRequestFixture(JSGlobalObject* globalObject, Ref<SourceProvider>&& provider)
{
    VM& vm = globalObject->vm();
    SourceCode source { WTF::move(provider) };
    Strong<ProgramExecutable> executable { vm, ProgramExecutable::create(globalObject, source) };
    return ProgramRequestFixture { WTF::move(source), WTF::move(executable) };
}

static Ref<SourceProvider> plainProvider(const String& text)
{
    return StringSourceProvider::create(text, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
}

// The key and context a program request computes, from the inputs alone.
static BodyKey programKey(const SourceCode& source, LexicallyScopedFeatures snapshot)
{
    return BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, rootIdentityDigest(IdentityKind::Program, rootSourceDigest(source).digest, snapshot, std::nullopt));
}

static Digest256 programContext(const SourceCode& source, ProgramExecutable& executable)
{
    return globalContextDigest(IdentityKind::Program, source.startOffset(), JSParserScriptMode::Classic, executable.derivedContextType(), EvalContextType::None, executable.isArrowFunctionContext());
}

// The key and context a module request computes, from the inputs alone: a module's code is strict, and its executable
// derives no context.
static BodyKey moduleKey(const SourceCode& source)
{
    return BodyKey::make(IdentityKind::Module, CodeSpecializationKind::CodeForCall, { }, rootIdentityDigest(IdentityKind::Module, rootSourceDigest(source).digest, StrictModeLexicallyScopedFeature, std::nullopt));
}

static Digest256 moduleContext(const SourceCode& source)
{
    return globalContextDigest(IdentityKind::Module, source.startOffset(), JSParserScriptMode::Module, DerivedContextType::None, EvalContextType::None, false);
}

static Strong<UnlinkedCodeBlock> generateProgramFor(VM& vm, ProgramRequestFixture& fixture)
{
    RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
    prepareProgramRequest(request, *fixture.executable.get());
    TwinParseResults results;
    ParserError error;
    return Strong<UnlinkedCodeBlock> { vm, generateGlobalTwin(request, results, error) };
}

// A program whose UCB the registry records under a test key, so its children have identities.
struct RecordedParent {
    ProgramFixture program;
    BodyKey key;
};

static std::optional<RecordedParent> recordedParent(VM& vm, UCBRegistry& registry, const String& text, uint8_t keyFill)
{
    ProgramFixture program = generateFixture(vm, text);
    if (!program.codeBlock)
        return std::nullopt;
    BodyKey key = testKey(keyFill);
    if (!registry.recordCodeBlock(*program.codeBlock.get(), testRecord(key, UCBOrigin::Generated)))
        return std::nullopt;
    return RecordedParent { WTF::move(program), key };
}

// What a child body's request computes, from the inputs alone.
struct ChildInputs {
    SourceCode source;
    BodyKey key;
    Digest256 context;
};

static ChildInputs childInputs(const UnlinkedFunctionExecutable& child, const SourceCode& parentSource, const BodyKey& parentKey, ChildTable table, uint32_t index,
    CodeSpecializationKind kind = CodeSpecializationKind::CodeForCall)
{
    SourceCode source = child.linkedSourceCode(parentSource);
    BodyKey key = BodyKey::make(IdentityKind::Child, kind, { }, childIdentityDigest(parentKey, table, index));
    Digest256 context = executableBodyContextDigest(IdentityKind::Child, source.startOffset(), std::nullopt);
    return ChildInputs { WTF::move(source), key, context };
}

static void prepareBodyRequest(RequestState& request, UnlinkedFunctionExecutable& executable, CodeSpecializationKind kind = CodeSpecializationKind::CodeForCall)
{
    request.functionExecutable = &executable;
    request.specialization = kind;
}

static uint64_t missesOf(const UCBStatistics& statistics, MissReason reason)
{
    return statistics.misses[static_cast<size_t>(reason)];
}

static bool computedNoDigest(const UCBStatistics& before, const UCBStatistics& after)
{
    return before.sourceDigests == after.sourceDigests && before.suppliedSourceDigests == after.suppliedSourceDigests && before.contextDigests == after.contextDigests
        && before.holderDigests == after.holderDigests;
}

// A copy of an identity section under another key, structurally valid, which the engine refuses wherever it parses one.
static Vector<uint8_t> identityUnderAnotherKey(const Vector<uint8_t>& identity)
{
    Vector<uint8_t> bytes = identity;
    bytes[identityKeyOffset + BodyKey::byteSize - 1] ^= 0xff;
    return bytes;
}

static Vector<uint8_t> brokenMagic(const Vector<uint8_t>& section)
{
    Vector<uint8_t> bytes = section;
    bytes[0] ^= 0xff;
    return bytes;
}

// attachLive on a generated program. With no body, with code parked in its sharing slot, or with a miss stamped at the
// current token it computes no digest; a request whose snapshot differs from the record's key features misses RequestKey
// unstamped; a miss is stamped with the index token, never the file's commit identifier, and a match records the commit
// identifier, never the token; at a matched commit identifier it attaches without parsing a section; an index that lists a
// body whose file is gone costs one open, stamped, and nothing at the next request.
static bool testAttachToGeneratedProgram(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    String text = "var jitcacheU8Attach = [\"jitcache-u8-attach-alpha\", \"jitcache-u8-attach-beta\"]; function jitcacheU8AttachChild() { return 1; }\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider(text));
    ProgramExecutable& executable = *fixture.executable.get();
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Digest256 context = programContext(fixture.source, executable);

    // The live UCB, generated at its request and recorded there, as the CodeCache's generation records it.
    Strong<UnlinkedCodeBlock> live = generateProgramFor(vm, fixture);
    if (!part.check(!!live, "the attach program failed to generate"_s))
        return false;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        recordGenerated(request, *live.get());
    }
    auto record = [&] {
        return registry.recordOf(*live.get());
    };
    if (!part.check(record() && record()->key == key && record()->origin == UCBOrigin::Generated, "recordGenerated did not record the program under its request's key"_s))
        return false;
    auto body = buildTestBody(vm, *live.get(), nullptr, key, context);
    auto wrongContext = buildTestBody(vm, *live.get(), nullptr, key, filledDigest(0x99));
    if (!part.check(body && wrongContext, "the attach program's body could not be built"_s))
        return false;
    auto attach = [&](UnlinkedCodeBlock& codeBlock) {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        attachLive(request, codeBlock);
    };

    // No body.
    UCBStatistics before = statistics;
    attach(*live.get());
    if (!part.check(missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics) && !record()->missedBodyVersion, "an attach for a key with no body computed a digest, stamped or missed otherwise than NoBody"_s))
        return false;

#if ENABLE(JIT)
    // Code parked in the sharing slot wins, so the request reads nothing.
    artifact.list(key, 5, body->validated(key, 1000));
    live.get()->m_unlinkedBaselineCode = adoptRef(*new BaselineJITCode(LLInt::getCodeRef<JSEntryPtrTag>(llint_function_for_call_prologue), LLInt::getCodePtr<JSEntryPtrTag>(llint_function_for_call_arity_check)));
    before = statistics;
    attach(*live.get());
    live.get()->m_unlinkedBaselineCode = nullptr;
    bool parkedReadNothing = computedNoDigest(before, statistics) && statistics.misses == before.misses && statistics.attaches == before.attaches && !artifact.opens(key) && !record()->hasPendingImport;
    if (!part.check(parkedReadNothing, "an attach to a UCB with parked code read the artifact or computed a digest"_s))
        return false;
#endif

    // A request whose snapshot differs from the record's key features.
    Strong<UnlinkedCodeBlock> other = generateProgramFor(vm, fixture);
    BodyKey otherKey = testKey(0x81);
    UCBRecord otherRecord = testRecord(otherKey, UCBOrigin::Generated);
    otherRecord.keyFeatures = TaintedByWithScopeLexicallyScopedFeature;
    if (!part.check(other && registry.recordCodeBlock(*other.get(), WTF::move(otherRecord)), "the RequestKey fixture could not be recorded"_s))
        return false;
    artifact.list(otherKey, 3, body->validated(otherKey, 1001));
    before = statistics;
    attach(*other.get());
    auto otherView = registry.recordOf(*other.get());
    bool requestKeyMiss = missesOf(statistics, MissReason::RequestKey) == missesOf(before, MissReason::RequestKey) + 1 && otherView && !otherView->missedBodyVersion && !artifact.opens(otherKey);
    if (!part.check(requestKeyMiss, "an attach whose snapshot differs from the record's key features did not miss RequestKey, or stamped or opened"_s))
        return false;

    // A miss is stamped with the index token, here behind the file's commit identifier.
    artifact.list(key, 5, wrongContext->validated(key, 1000));
    before = statistics;
    attach(*live.get());
    bool stampedWithToken = missesOf(statistics, MissReason::Context) == missesOf(before, MissReason::Context) + 1 && statistics.contextDigests == before.contextDigests + 1
        && record()->missedBodyVersion == 5 && !record()->matchedBodyVersion && artifact.opens(key) == 1;
    if (!part.check(stampedWithToken, "a Context miss was not stamped with the index token"_s))
        return false;
    before = statistics;
    attach(*live.get());
    bool skipped = missesOf(statistics, MissReason::BodyUnchanged) == missesOf(before, MissReason::BodyUnchanged) + 1 && computedNoDigest(before, statistics) && artifact.opens(key) == 1;
    if (!part.check(skipped, "a request at the stamped index token read the artifact or computed a digest"_s))
        return false;

    // Another body at the key: the match records the commit identifier.
    artifact.list(key, 6, body->validated(key, 1000));
    before = statistics;
    attach(*live.get());
    if (!part.check(statistics.attaches == before.attaches + 1 && record()->hasPendingImport && record()->matchedBodyVersion == 1000, "an attach to a matching generated program did not attach at the file's commit identifier"_s))
        return false;

    // At the matched commit identifier the attach parses nothing: a parse of this identity section would find another key,
    // and of this feedback section a wrong magic, both invalid material.
    RefPtr<PendingImport> attached = registry.pendingImport(*live.get());
    registry.resolvePendingImport(*live.get(), *attached, ImportResolution::Installed);
    TestBody unparsable { identityUnderAnotherKey(body->identity), body->core, brokenMagic(body->feedback), BodyImage::Present };
    artifact.list(key, 7, unparsable.validated(key, 1000));
    before = statistics;
    attach(*live.get());
    bool withoutParsing = state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && statistics.contextDigests == before.contextDigests;
    if (!part.check(withoutParsing, "an attach at a matched commit identifier parsed a section or computed a context"_s))
        return false;

    // The index lists a body whose file is gone.
    attached = registry.pendingImport(*live.get());
    registry.resolvePendingImport(*live.get(), *attached, ImportResolution::Installed);
    artifact.list(key, 9, nullptr);
    unsigned opensBefore = artifact.opens(key);
    before = statistics;
    attach(*live.get());
    bool goneOnce = missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && record()->missedBodyVersion == 9 && artifact.opens(key) == opensBefore + 1;
    attach(*live.get());
    bool thenNothing = missesOf(statistics, MissReason::BodyUnchanged) == missesOf(before, MissReason::BodyUnchanged) + 1 && artifact.opens(key) == opensBefore + 1;
    return part.check(goneOnce && thenNothing, "a listed body whose file is gone did not cost one stamped open and then nothing"_s);
}

// An attach leaves a live UCB that had no value and array copies with copies allocated and nothing seeded into them, and,
// with strict off, attaches to a live UCB whose core encoding differs from the body's coreDigest, since only strict holds a
// reused UCB to it. The body comes from another generation of the same program, which carries a prediction in its copies,
// so the live UCB has no copies of its own before the attach.
static bool testAttachCopies(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    String text = "var jitcacheU8AttachCopies = Math.max(1, 2); function jitcacheU8AttachCopiesChild(a) { return a.x; }\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider(text));
    ProgramExecutable& executable = *fixture.executable.get();
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Digest256 context = programContext(fixture.source, executable);

    // The producer's generation, with a prediction in its copies, which its body's feedback carries.
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    if (!part.check(producer && producer.get()->numberOfValueProfiles(), "the attach-copies program failed to generate with a value profile"_s))
        return false;
    producer.get()->ensureValueAndArrayProfiles();
    producer.get()->valueAndArrayProfiles()->valueProfiles()[0].restorePrediction(SpecInt32Only);
    auto body = buildTestBody(vm, *producer.get(), nullptr, key, context);
    if (!part.check(!!body, "the attach-copies program's body could not be built"_s))
        return false;

    // A live UCB of its own generation, recorded at its request.
    auto generateLive = [&] {
        Strong<UnlinkedCodeBlock> live = generateProgramFor(vm, fixture);
        if (!live)
            return live;
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        recordGenerated(request, *live.get());
        return live;
    };
    auto attach = [&](UnlinkedCodeBlock& codeBlock) {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        attachLive(request, codeBlock);
    };

    Strong<UnlinkedCodeBlock> live = generateLive();
    if (!part.check(live && registry.recordOf(*live.get()) && !live.get()->valueAndArrayProfiles(), "the live UCB is not recorded, or has copies before any attach"_s))
        return false;
    artifact.list(key, 31, body->validated(key, 3100));
    UCBStatistics before = statistics;
    attach(*live.get());
    auto view = registry.recordOf(*live.get());
    auto* copies = live.get()->valueAndArrayProfiles();
    bool allocatedUnseeded = statistics.attaches == before.attaches + 1 && view && view->hasPendingImport && copies && copies->valueProfiles()[0].prediction() == SpecNone;
    if (!part.check(allocatedUnseeded, "an attach did not leave the live UCB's copies allocated with nothing seeded into them"_s))
        return false;
    if (state.strict())
        return true;

    // Strict off: the identity section's coreDigest differs from the core encoding of the live UCB, whose core section is
    // the body's own, and the attach succeeds.
    TestBody otherDigest = *body;
    Digest256 wrongDigest = filledDigest(0x5a);
    memcpySpan(otherDigest.identity.mutableSpan().subspan(identityCoreDigestOffset, wrongDigest.size()), std::span { wrongDigest });
    Strong<UnlinkedCodeBlock> unchecked = generateLive();
    if (!part.check(!!unchecked, "the second live UCB failed to generate"_s))
        return false;
    artifact.list(key, 32, otherDigest.validated(key, 3200));
    before = statistics;
    attach(*unchecked.get());
    auto uncheckedView = registry.recordOf(*unchecked.get());
    return part.check(state.tracksKeys() && statistics.attaches == before.attaches + 1 && uncheckedView && uncheckedView->hasPendingImport,
        "with strict off, an attach to a live UCB whose core encoding differs from the body's coreDigest did not succeed"_s);
}

// A root body's import computes no context and no holder digest while its key has no body; an import that reaches the
// holder check computes the holder digest once.
static bool testRootImport(VM& vm, VMState& state, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    SourceCode rootSource;
    Strong<UnlinkedFunctionExecutable> root = functionConstructorRoot(vm, "return a + 'jitcache-u8-root';"_s, rootSource);
    std::optional<ExecutableIdentity> identity = root ? registry.identityOf(*root.get()) : std::nullopt;
    auto* rootIdentity = identity ? std::get_if<RootIdentity>(&*identity) : nullptr;
    if (!part.check(!!rootIdentity, "the Function-constructor root has no identity"_s))
        return false;
    BodyKey key = BodyKey::make(rootIdentity->kind, CodeSpecializationKind::CodeForCall, { }, rootIdentity->identityDigest);
    SourceCode bodySource = root.get()->linkedSourceCode(rootSource);
    auto rootHolder = holderDigest(vm, *root.get());
    Digest256 context = executableBodyContextDigest(IdentityKind::FunctionConstructor, bodySource.startOffset(), rootHolder);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, *root.get(), bodySource, CodeSpecializationKind::CodeForCall, &results);
    if (!part.check(!!producer, "the root's body failed to generate"_s))
        return false;
    auto body = buildTestBody(vm, *producer.get(), root.get(), key, context, parseFieldsOf(results));
    if (!part.check(!!body, "the root's body could not be built"_s))
        return false;
    auto importRootBody = [&] {
        RequestState request(vm, RequestKind::FunctionBody, bodySource, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, *root.get());
        return Strong<UnlinkedCodeBlock> { vm, importBody(request) };
    };

    UCBStatistics before = statistics;
    bool noBody = !importRootBody() && missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics);
    if (!part.check(noBody, "a root body's import with no body computed a context or holder digest"_s))
        return false;
    artifact.list(key, 22, body->validated(key, 3001));
    before = statistics;
    Strong<UnlinkedCodeBlock> imported = importRootBody();
    bool importedOnce = imported && statistics.imports == before.imports + 1 && statistics.holderDigests == before.holderDigests + 1 && statistics.contextDigests == before.contextDigests + 1;
    return part.check(importedOnce, "a root body's import did not compute its holder digest exactly once"_s);
}

// An import of a child body with an image leaves the imported UCB's value and array copies allocated and seeded with the
// producer's, and an import of one without an image leaves it none.
static bool testImportCopies(VM& vm, VMState& state, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    auto parent = recordedParent(vm, registry, "function jitcacheU8CopiesWith(a) { return a + 1; }\nfunction jitcacheU8CopiesWithout(a) { return a + 2; }\n"_s, 0x85);
    if (!part.check(parent && parent->program.codeBlock.get()->numberOfFunctionDecls() == 2, "the copies parent could not be generated and recorded"_s))
        return false;
    auto importChild = [&](unsigned index, BodyImage image, uint64_t token) -> Strong<UnlinkedCodeBlock> {
        UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(index);
        ChildInputs inputs = childInputs(child, parent->program.source, parent->key, ChildTable::Declarations, index);
        TwinParseResults results;
        Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
        if (!producer)
            return { };
        // A prediction in the producer's copies, which only a body that carries them seeds.
        producer.get()->ensureValueAndArrayProfiles();
        producer.get()->valueAndArrayProfiles()->valueProfiles()[0].restorePrediction(SpecInt32Only);
        auto body = buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, parseFieldsOf(results), image);
        if (!body)
            return { };
        artifact.list(inputs.key, token, body->validated(inputs.key, 9100 + token));
        RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, child);
        return Strong<UnlinkedCodeBlock> { vm, importBody(request) };
    };

    UCBStatistics before = statistics;
    Strong<UnlinkedCodeBlock> withImage = importChild(0, BodyImage::Present, 91);
    auto* copies = withImage ? withImage.get()->valueAndArrayProfiles() : nullptr;
    bool seeded = copies && copies->valueProfiles()[0].prediction() == SpecInt32Only && statistics.imports == before.imports + 1;
    if (!part.check(seeded, "the import of a body with an image did not leave the UCB's copies allocated and seeded"_s))
        return false;
    before = statistics;
    Strong<UnlinkedCodeBlock> withoutImage = importChild(1, BodyImage::Absent, 92);
    auto view = withoutImage ? registry.recordOf(*withoutImage.get()) : std::nullopt;
    bool none = view && !view->hasPendingImport && !withoutImage.get()->valueAndArrayProfiles() && statistics.imports == before.imports + 1;
    return part.check(none, "the import of a body without an image left the UCB copies or a pending import"_s);
}

// A provider that hands the CodeCache a bytecode payload for its text, as an embedder's provider of precompiled code does:
// the payload itself, or a persistent view of its bytes, which the decode borrows instead of copying. The view's bytes are
// the payload's, which lives as long as this provider, and whatever is decoded from the view keeps the provider.
class CachedBytecodeProvider final : public StringSourceProvider {
public:
    static Ref<CachedBytecodeProvider> create(const String& text, Ref<CachedBytecode>&& payload, bool persistent)
    {
        return adoptRef(*new CachedBytecodeProvider(text, WTF::move(payload), persistent));
    }

    RefPtr<CachedBytecode> cachedBytecode() const final { return m_served.copyRef(); }

private:
    CachedBytecodeProvider(const String& text, Ref<CachedBytecode>&& payload, bool persistent)
        : StringSourceProvider(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Program)
        , m_owned(WTF::move(payload))
        , m_served(persistent ? persistentView(m_owned.get()) : m_owned.copyRef())
    {
    }

    static Ref<CachedBytecode> persistentView(CachedBytecode& owned)
    {
        Ref<CachedBytecode> view = CachedBytecode::create(spanConstCast<uint8_t>(owned.span()), nullptr, { });
        view->setPayloadIsPersistent();
        return view;
    }

    const Ref<CachedBytecode> m_owned;
    const Ref<CachedBytecode> m_served;
};

static bool sameCounts(const UCBRegistry::Counts& a, const UCBRegistry::Counts& b)
{
    return a.children == b.children && a.roots == b.roots && a.codeBlocks == b.codeBlocks && a.pendingImports == b.pendingImports;
}

// Code a native decode makes, with bodies in the artifact at every key it would have: nothing in it imports, records or is
// captured. A program generated with its functions' code, encoded with encodeCodeBlock and handed back by its provider as
// its cached bytecode, once owned and once persistent, is decoded by the CodeCache: the decode gets no record, and serving
// it counts no miss and computes no digest. Its functions' first requests decode their slots and record and import
// nothing; the construct slot the decode left empty generates and misses NoKey. A builtin that encodeBuiltinFunction
// encoded and decodeBuiltinFunction decodes has no identity, and its body misses NoKey. These requests leave the
// registry's counts as they were, and no decoded UCB has a record a capture could use. A full collection runs first and
// none after it, so that no record of an earlier part goes while the counts are compared.
static bool testDecodedCode(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    auto decodedProgram = [&](bool persistent) {
        // The text differs per run, so that the second run misses the CodeCache's map, where the first run's decode went.
        String text = makeString("function jitcacheU8DecodedA(x) { return x + "_s, persistent ? 2 : 1, "; }\nfunction jitcacheU8DecodedB(x) { this.x = x; }\n"_s);
        ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
        if (!part.check(generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 2, "the decoded program failed to generate"_s))
            return false;
        RefPtr<CachedBytecode> payload = encodeCodeBlock(vm, programCacheKey(generated.source), generated.codeBlock.get());
        if (!part.check(!!payload, "the decoded program did not encode to a bytecode cache"_s))
            return false;
        ProgramRequestFixture fixture = programRequestFixture(globalObject, CachedBytecodeProvider::create(text, payload.releaseNonNull(), persistent));

        // The bodies, at the program's key, at A's call key and at B's construct key.
        UnlinkedFunctionExecutable& generatedA = *generated.codeBlock.get()->functionDecl(0);
        UnlinkedFunctionExecutable& generatedB = *generated.codeBlock.get()->functionDecl(1);
        BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
        ChildInputs inputsA = childInputs(generatedA, fixture.source, key, ChildTable::Declarations, 0);
        ChildInputs inputsB = childInputs(generatedB, fixture.source, key, ChildTable::Declarations, 1, CodeSpecializationKind::CodeForConstruct);
        Strong<UnlinkedFunctionCodeBlock> producerA { vm, decodedBody(vm, generatedA, generated.source) };
        Strong<UnlinkedFunctionCodeBlock> producerB = generateBodyOf(vm, generatedB, inputsB.source, CodeSpecializationKind::CodeForConstruct);
        auto body = buildTestBody(vm, *generated.codeBlock.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get()));
        auto bodyA = producerA ? buildTestBody(vm, *producerA.get(), &generatedA, inputsA.key, inputsA.context, parseFieldsOf(generatedA)) : std::nullopt;
        auto bodyB = producerB ? buildTestBody(vm, *producerB.get(), &generatedB, inputsB.key, inputsB.context, parseFieldsOf(generatedB)) : std::nullopt;
        if (!part.check(body && bodyA && bodyB, "the decoded program's bodies could not be built"_s))
            return false;
        artifact.list(key, 101, body->validated(key, 10000));
        artifact.list(inputsA.key, 102, bodyA->validated(inputsA.key, 10001));
        artifact.list(inputsB.key, 103, bodyB->validated(inputsB.key, 10002));
        auto readNothing = [&] {
            return !artifact.opens(key) && !artifact.opens(inputsA.key) && !artifact.opens(inputsB.key);
        };

        // The program.
        UCBStatistics before = statistics;
        UCBRegistry::Counts counts = registry.counts();
        ParserError error;
        Strong<UnlinkedProgramCodeBlock> decoded { vm, vm.codeCache()->getUnlinkedProgramCodeBlock(vm, fixture.executable.get(), fixture.source, { }, error) };
        bool servedLive = decoded && decoded.get() != generated.codeBlock.get() && !registry.recordOf(*decoded.get()) && statistics.misses == before.misses
            && statistics.imports == before.imports && statistics.attaches == before.attaches && computedNoDigest(before, statistics) && readNothing();
        if (!part.check(servedLive, "a program the CodeCache decoded got a record, counted a miss, computed a digest or read a body"_s))
            return false;
        UnlinkedFunctionExecutable& decodedA = *decoded.get()->functionDecl(0);
        UnlinkedFunctionExecutable& decodedB = *decoded.get()->functionDecl(1);
        if (!part.check(!registry.identityOf(decodedA) && !registry.identityOf(decodedB), "a function the decode made has an identity"_s))
            return false;

        // A's call slot, which the decode filled.
        Strong<UnlinkedFunctionCodeBlock> slotA { vm, decodedBody(vm, decodedA, fixture.source) };
        bool decodedSlot = slotA && !registry.recordOf(*slotA.get()) && statistics.misses == before.misses && statistics.imports == before.imports && computedNoDigest(before, statistics)
            && readNothing();
        if (!part.check(decodedSlot, "a decoded function's call slot got a record, counted a miss, computed a digest or read a body"_s))
            return false;

        // B first constructed: the decode left the construct slot empty, so the request generates it, without a key.
        SourceCode sourceB = decodedB.linkedSourceCode(fixture.source);
        Strong<UnlinkedFunctionCodeBlock> constructB { vm, decodedB.unlinkedCodeBlockFor(vm, sourceB, CodeSpecializationKind::CodeForConstruct, { }, error, decodedB.parseMode()) };
        Strong<UnlinkedFunctionCodeBlock> callB { vm, decodedBody(vm, decodedB, fixture.source) };
        bool generatedWithoutKey = constructB && callB && !registry.recordOf(*constructB.get()) && !registry.recordOf(*callB.get())
            && missesOf(statistics, MissReason::NoKey) == missesOf(before, MissReason::NoKey) + 1 && statistics.imports == before.imports && computedNoDigest(before, statistics) && readNothing();
        if (!part.check(generatedWithoutKey, "a decoded function's empty construct slot did not generate with exactly one NoKey miss and no record"_s))
            return false;
        bool noCapture = !captureRecord(vm, *decoded.get()) && !captureRecord(vm, *slotA.get()) && !captureRecord(vm, *callB.get()) && !captureRecord(vm, *constructB.get());
        return part.check(noCapture && sameCounts(registry.counts(), counts), "a UCB of decoded code has a record a capture would use, or the registry's counts changed"_s);
    };

    auto decodedBuiltin = [&] {
        SourceCode source = makeSource("(function (a) { return a + 'jitcache-u8-decoded-builtin'; })"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<UnlinkedFunctionExecutable> builtin { vm, BuiltinExecutables::createExecutable(vm, source, Identifier { }, ImplementationVisibility::Public, ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No) };
        std::optional<ExecutableIdentity> identity = builtin ? registry.identityOf(*builtin.get()) : std::nullopt;
        auto* rootIdentity = identity ? std::get_if<RootIdentity>(&*identity) : nullptr;
        if (!part.check(!!rootIdentity, "the builtin has no identity at its creation"_s))
            return false;
        // Encoded before any of its bodies exists, so the decoded executable's call slot is empty.
        constexpr unsigned embedderStamp = 0x55380008;
        RefPtr<CachedBytecode> payload = encodeBuiltinFunction(vm, builtin.get(), source, embedderStamp);
        if (!part.check(!!payload, "the builtin did not encode"_s))
            return false;

        // The body at the builtin's key.
        BodyKey key = BodyKey::make(IdentityKind::Builtin, CodeSpecializationKind::CodeForCall, { }, rootIdentity->identityDigest);
        SourceCode bodySource = builtin.get()->linkedSourceCode(source);
        TwinParseResults results;
        Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, *builtin.get(), bodySource, CodeSpecializationKind::CodeForCall, &results);
        auto body = producer ? buildTestBody(vm, *producer.get(), builtin.get(), key, executableBodyContextDigest(IdentityKind::Builtin, bodySource.startOffset(), holderDigest(vm, *builtin.get())), parseFieldsOf(results)) : std::nullopt;
        if (!part.check(!!body, "the builtin's body could not be built"_s))
            return false;
        artifact.list(key, 104, body->validated(key, 10003));

        UCBStatistics before = statistics;
        UCBRegistry::Counts counts = registry.counts();
        Strong<UnlinkedFunctionExecutable> decoded { vm, decodeBuiltinFunction(vm, payload.releaseNonNull(), *source.provider(), embedderStamp) };
        if (!part.check(decoded && !registry.identityOf(*decoded.get()), "a builtin decodeBuiltinFunction decoded is missing or has an identity"_s))
            return false;
        Strong<UnlinkedFunctionCodeBlock> decodedCall { vm, decodedBody(vm, *decoded.get(), source) };
        bool noKey = decodedCall && !registry.recordOf(*decodedCall.get()) && !captureRecord(vm, *decodedCall.get()) && missesOf(statistics, MissReason::NoKey) == missesOf(before, MissReason::NoKey) + 1
            && statistics.imports == before.imports && computedNoDigest(before, statistics) && !artifact.opens(key) && sameCounts(registry.counts(), counts);
        return part.check(noKey, "a decoded builtin's body did not miss NoKey without a record, or the registry's counts changed"_s);
    };

    vm.heap.collectNow(Sync, CollectionScope::Full);
    DeferGC deferGC(vm);
    return decodedProgram(false) && decodedProgram(true) && decodedBuiltin();
}

// A private module's request, for a module whose loader has a module scope of its own, records and imports under the
// shared module's key, and nothing it generates or imports enters the CodeCache: a later request of the shared module for
// the same source gets a UCB of its own.
static bool testPrivateModule(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    SourceCode source = makeSource("export function jitcacheU8Private(a) { return [a, 'jitcache-u8-private-module']; }\n"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { },
        SourceProviderSourceType::Module);
    BodyKey key = moduleKey(source);
    Strong<SymbolTable> moduleScope { vm, SymbolTable::create(vm) };
    auto privateModule = [&] {
        return Strong<ModuleProgramExecutable> { vm, ModuleProgramExecutable::tryCreate(globalObject, source, nullptr, { moduleScope.get() }) };
    };
    auto codeBlockOf = [](const Strong<ModuleProgramExecutable>& executable) -> UnlinkedCodeBlock* {
        return executable ? executable.get()->unlinkedCodeBlock() : nullptr;
    };

    // No body yet: the request generates and records.
    UCBStatistics before = statistics;
    Strong<ModuleProgramExecutable> generated = privateModule();
    UnlinkedCodeBlock* generatedCodeBlock = codeBlockOf(generated);
    auto generatedView = generatedCodeBlock ? registry.recordOf(*generatedCodeBlock) : std::nullopt;
    bool recorded = generated && !generated.get()->resolvesInGlobalScope() && generatedView && generatedView->key == key && generatedView->origin == UCBOrigin::Generated
        && missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1;
    if (!part.check(recorded, "a private module's request did not miss NoBody and record its generated block under the shared module's key"_s))
        return false;

    // A body at the key: the request imports.
    auto body = buildTestBody(vm, *generatedCodeBlock, nullptr, key, moduleContext(source));
    if (!part.check(!!body, "the private module's body could not be built"_s))
        return false;
    artifact.list(key, 111, body->validated(key, 11000));
    before = statistics;
    Strong<ModuleProgramExecutable> imported = privateModule();
    UnlinkedCodeBlock* importedCodeBlock = codeBlockOf(imported);
    auto importedView = importedCodeBlock ? registry.recordOf(*importedCodeBlock) : std::nullopt;
    bool importedUnderKey = importedCodeBlock && importedCodeBlock != generatedCodeBlock && importedView && importedView->key == key && importedView->origin == UCBOrigin::Imported
        && importedView->hasPendingImport && statistics.imports == before.imports + 1;
    if (!part.check(importedUnderKey, "a private module's request did not import and record under the shared module's key"_s))
        return false;

    // The shared module: a CodeCache that held either block would serve it here.
    Strong<ModuleProgramExecutable> shared { vm, ModuleProgramExecutable::tryCreate(globalObject, source) };
    UnlinkedCodeBlock* sharedCodeBlock = codeBlockOf(shared);
    return part.check(sharedCodeBlock && sharedCodeBlock != generatedCodeBlock && sharedCodeBlock != importedCodeBlock, "a private module's block entered the CodeCache"_s);
}

// A direct eval inside `caller`, whose record has `callerKey`, imported from a body the test serves: it relies on no
// supplied digest, so it verifies none, and its record keeps no provider.
static bool importDirectEval(VM& vm, JSGlobalObject* globalObject, VMState& state, StubbedArtifact& artifact, UnlinkedCodeBlock& caller, const BodyKey& callerKey, uint8_t salt)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    String evalText = makeString("jitcacheU8Eval"_s, salt, " + 1"_s);
    SourceCode evalSource = makeSource(evalText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    TDZEnvironment tdz = tdzNames(vm, { "jitcacheU8EvalTDZ"_s });
    PrivateNameEnvironment privateNames;
    Strong<DirectEvalExecutable> executable { vm, DirectEvalExecutable::create(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, true, EvalContextType::None, &tdz, &privateNames) };
    if (!executable || !executable.get()->unlinkedCodeBlock())
        return false;
    BytecodeIndex siteIndex(7);
    DirectEvalSite site { &caller, siteIndex };
    BodyKey key = BodyKey::make(IdentityKind::DirectEval, CodeSpecializationKind::CodeForCall, { }, directEvalIdentityDigest(callerKey, siteIndex, sourceDigest(evalSource.view())));
    Digest256 context = directEvalContextDigest(evalSource.startOffset(), NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false,
        true, EvalContextType::None, tdz, privateNames);
    auto body = buildTestBody(vm, *executable.get()->unlinkedCodeBlock(), nullptr, key, context);
    if (!body)
        return false;
    artifact.list(key, 51, body->validated(key, 6000 + salt));
    UCBStatistics before = statistics;
    RequestState request(vm, RequestKind::DirectEval, evalSource, { }, NoLexicallyScopedFeatures);
    request.globalExecutable = executable.get();
    request.site = &site;
    request.variablesUnderTDZ = &tdz;
    request.privateNameEnvironment = &privateNames;
    Strong<UnlinkedCodeBlock> imported { vm, importBody(request) };
    auto view = imported ? registry.recordOf(*imported.get()) : std::nullopt;
    return view && !view->suppliedDigestProvider && statistics.imports == before.imports + 1 && statistics.suppliedDigestVerifications == before.suppliedDigestVerifications;
}

// Supplied digests. With strict on, an import of a program whose provider supplies its digest and an import of one of its
// functions verify the provider once in all, and their records keep it; with strict off they verify nothing and keep
// none; a direct eval inside the function imports without a verification and keeps no provider. Every digest here is its
// text's: the case of a digest that differs from its text, which the twin check reports wherever a request takes one, is
// the invalid-material test's.
static bool testSuppliedDigestImports(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    bool strict = state.strict();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    String text = "function jitcacheU8Supplied(a) { return eval('a + 1'); }\n"_s;
    auto importChain = [&] {
        Ref<SuppliedDigestProvider> provider = SuppliedDigestProvider::create(text, sourceDigest(text));
        SourceProvider* expectedProvider = strict ? provider.ptr() : nullptr;
        ProgramRequestFixture fixture = programRequestFixture(globalObject, Ref<SourceProvider> { provider.get() });
        BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
        Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
        auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get())) : std::nullopt;
        if (!body)
            return false;
        artifact.list(key, 62, body->validated(key, 7001));
        Strong<UnlinkedCodeBlock> program;
        {
            RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
            prepareProgramRequest(request, *fixture.executable.get());
            program = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto programView = program ? registry.recordOf(*program.get()) : std::nullopt;
        if (!programView || !programView->hasPendingImport || programView->suppliedDigestProvider != expectedProvider)
            return false;

        UnlinkedFunctionExecutable& child = *program.get()->functionDecl(0);
        ChildInputs inputs = childInputs(child, fixture.source, key, ChildTable::Declarations, 0);
        TwinParseResults results;
        Strong<UnlinkedFunctionCodeBlock> childProducer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
        auto childBody = childProducer ? buildTestBody(vm, *childProducer.get(), &child, inputs.key, inputs.context, parseFieldsOf(results)) : std::nullopt;
        if (!childBody)
            return false;
        artifact.list(inputs.key, 64, childBody->validated(inputs.key, 7003));
        Strong<UnlinkedCodeBlock> function;
        {
            RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
            prepareBodyRequest(request, child);
            function = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto functionView = function ? registry.recordOf(*function.get()) : std::nullopt;
        if (!functionView || !functionView->hasPendingImport || functionView->suppliedDigestProvider != expectedProvider)
            return false;
        // Verified now exactly with strict on; the direct eval's import leaves that as it is.
        bool verified = registry.suppliedDigestVerified(provider->asID());
        if (verified != strict)
            return false;
        if (!importDirectEval(vm, globalObject, state, artifact, *function.get(), functionView->key, 2))
            return false;
        return registry.suppliedDigestVerified(provider->asID()) == verified;
    };

    UCBStatistics before = statistics;
    if (!part.check(importChain(), "the import of a program, of its function and of a direct eval inside it did not succeed with the records the registry gives them, or did not leave the provider verified exactly with strict on"_s))
        return false;
    return part.check(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications + (strict ? 1 : 0), "the imports of a program, its function and a direct eval inside it did not verify the provider exactly once with strict on, and never with it off"_s);
}

// In a Producer no record keeps a provider, since only a VM that imports with strict on keeps one.
static bool testProducerRecordsKeepNoProvider(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    String text = "function jitcacheU8ProducerSupplied() { return 1; }\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, SuppliedDigestProvider::create(text, sourceDigest(text)));
    Strong<UnlinkedCodeBlock> program = generateProgramFor(vm, fixture);
    if (!part.check(!!program, "the Producer's program failed to generate"_s))
        return false;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        recordGenerated(request, *program.get());
    }
    auto view = registry.recordOf(*program.get());
    std::optional<ExecutableIdentity> childIdentity = registry.identityOf(*program.get()->functionDecl(0));
    auto* child = childIdentity ? std::get_if<ChildIdentity>(&*childIdentity) : nullptr;
    bool keepsNone = view && !view->suppliedDigestProvider && child && child->parent && !child->parent->suppliedDigestProvider();
    return part.check(keepsNone, "a Producer's record or its children's identities keep a supplied digest's provider"_s);
}

// The engine's parts the VM's configuration allows: the records of a Producer; and in a role that imports, the attaches
// and imports with the copies they leave, an attach that strict off does not hold to the body's coreDigest, decoded code,
// a private module's request and supplied digests.
static bool testEngine(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    VMState* state = vm.jitCacheState();
    if (!part.check(state && state->tracksKeys() && globalObject, "the test's VM does not track keys, or the test has no global object"_s))
        return false;
    if (state->role() == Role::Producer)
        return testProducerRecordsKeepNoProvider(vm, *state, globalObject, part);
    if (!part.check(state->importsEnabled(), "a Consumer or ConsumerProducer does not import"_s))
        return false;
    return testAttachToGeneratedProgram(vm, *state, globalObject, part)
        && testAttachCopies(vm, *state, globalObject, part)
        && testRootImport(vm, *state, part)
        && testImportCopies(vm, *state, part)
        && testDecodedCode(vm, *state, globalObject, part)
        && testPrivateModule(vm, *state, globalObject, part)
        && testSuppliedDigestImports(vm, *state, globalObject, part);
}

// The case of a supplied digest that differs from its text, alone in its test: the twin check reports such a digest
// wherever a request takes it, and the case turns cache activity off for good. In a Consumer with strict on, a program
// whose import relies on a wrong supplied digest is invalid material at ucb.supplied-digest before anything is decoded:
// its body's core and feedback sections are broken, so a decode or a feedback parse would raise another step. With a twin
// report open, the case adds exactly one difference.
static bool testSuppliedDigestInvalidMaterial(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    VMState* state = vm.jitCacheState();
    if (!part.check(state && globalObject && state->importsEnabled() && state->strict(), "the invalid-material cases need a VM that imports with strict on"_s))
        return false;
    UCBRegistry& registry = state->registry();
    UCBStatistics& statistics = registry.statistics();
    TwinReportSink* sink = state->twinReportSink();
    auto differences = [&] {
        return sink ? sink->differences() : 0;
    };
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { *state, artifact };

    String text = "var jitcacheU8WrongSupplied = 'jitcache-u8-wrong-supplied-digest';\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, SuppliedDigestProvider::create(text, filledDigest(0xee)));
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get())) : std::nullopt;
    if (!part.check(!!body, "the wrong-digest program's body could not be built"_s))
        return false;
    TestBody undecodable { body->identity, Vector<uint8_t>(FillWith { }, body->core.size(), 0), brokenMagic(body->feedback), BodyImage::Present };
    artifact.list(key, 71, undecodable.validated(key, 8000));
    UCBStatistics before = statistics;
    unsigned differencesBeforeImport = differences();
    Strong<UnlinkedCodeBlock> imported;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        imported = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
    }
    auto step = [&](InvalidMaterialStep invalidStep) {
        return statistics.invalidMaterial[static_cast<size_t>(invalidStep)] - before.invalidMaterial[static_cast<size_t>(invalidStep)];
    };
    bool raised = !imported && !state->tracksKeys() && step(InvalidMaterialStep::SuppliedDigest) == 1 && !step(InvalidMaterialStep::Decode) && !step(InvalidMaterialStep::Feedback)
        && statistics.imports == before.imports;
    if (!part.check(raised, "an import relying on a supplied digest that differs from its text did not raise invalid material at ucb.supplied-digest before decoding"_s))
        return false;
    return part.check(!sink || differences() == differencesBeforeImport + 1, "the import under a wrong supplied digest did not add exactly one difference to the twin report"_s);
}

// The difference lines of the twin report at `path`, which the VM's sink writes. TwinReport writes each line with one write
// and keeps no buffer, so the file holds every difference the sink has counted; an empty file reads back as nothing at all
// (FileHandle::readAll), so a report with no difference is not read.
static Vector<String> twinDifferenceLines(TestContext& context, VMState& state, const String& path)
{
    Vector<String> lines;
    TwinReportSink* sink = state.twinReportSink();
    if (!sink) {
        JITCACHE_FAIL("the test's VM has no twin report"_s);
        return lines;
    }
    if (!sink->differences())
        return lines;
    auto bytes = FileSystem::readEntireFile(path);
    if (!bytes) {
        JITCACHE_FAIL("cannot read the twin report back"_s);
        return lines;
    }
    for (const String& line : String::fromUTF8(bytes->span()).split('\n')) {
        if (line.startsWith("{\"kind\":\"difference\","_s))
            lines.append(line);
    }
    if (lines.size() != sink->differences())
        JITCACHE_FAIL(makeString("the twin report counts "_s, sink->differences(), " differences, and its file holds "_s, lines.size(), " difference lines"_s));
    return lines;
}

// Starts JITCache on the test's VM in the configuration, with a twin report in a temporary file, and runs the engine's
// parts there. The VM starts with no fault, and the parts leave no fault, a registry verifyRegistry finds sound and a twin
// report with no difference.
static void runEngineIn(TestContext& context, Role role, bool strict)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-engine"_s);
    if (!directory)
        return;
    String twinReportPath = directory->path("twins.jsonl"_s);
    if (!startJITCache(context, vm, *directory, role, strict, twinReportPath))
        return;
    if (auto fault = JITCache::status(vm).firstFault) {
        JITCACHE_FAIL(makeString("the VM starts with a fault at "_s, fault->stepName()));
        return;
    }
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    if (!testEngine(vm, entered.globalObject(), part))
        return;
    if (auto fault = JITCache::status(vm).firstFault)
        JITCACHE_FAIL(makeString("the engine's parts left a fault at "_s, fault->stepName()));
    checkRegistry(context, vm);
    for (const String& line : twinDifferenceLines(context, *vm.jitCacheState(), twinReportPath))
        JITCACHE_FAIL(makeString("the twin report holds a difference: "_s, line));
}

} // namespace UCBEngineTestsInternal

using namespace UCBEngineTestsInternal;

JITCACHE_TEST(ucbEngineInAProducer, Yes)
{
    runEngineIn(context, Role::Producer, true);
}

JITCACHE_TEST(ucbEngineInAStrictConsumer, Yes)
{
    runEngineIn(context, Role::Consumer, true);
}

// Strict off on purpose, the one engine test that is: every run the test runner starts has strict on, so normal mode's
// paths, such as an import that verifies no supplied digest, run only here.
JITCACHE_TEST(ucbEngineInANonStrictConsumer, Yes)
{
    runEngineIn(context, Role::Consumer, false);
}

JITCACHE_TEST(ucbEngineInAConsumerProducer, Yes)
{
    runEngineIn(context, Role::ConsumerProducer, true);
}

// A Consumer with strict on runs the case of a wrong supplied digest, and is left with cache activity off at
// ucb.supplied-digest, a registry verifyRegistry finds sound, and exactly the one difference the case reports.
JITCACHE_TEST(ucbEngineSuppliedDigestInvalidMaterial, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-engine"_s);
    if (!directory)
        return;
    String twinReportPath = directory->path("twins.jsonl"_s);
    if (!startJITCache(context, vm, *directory, Role::Consumer, true, twinReportPath))
        return;
    if (auto fault = JITCache::status(vm).firstFault) {
        JITCACHE_FAIL(makeString("the VM starts with a fault at "_s, fault->stepName()));
        return;
    }
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    if (!testSuppliedDigestInvalidMaterial(vm, entered.globalObject(), part))
        return;
    auto fault = JITCache::status(vm).firstFault;
    if (!fault || fault->stepName() != "ucb.supplied-digest"_s)
        JITCACHE_FAIL(makeString("the invalid material left the fault at "_s, fault ? fault->stepName() : String { "none"_s }, ", not at ucb.supplied-digest"_s));
    checkRegistry(context, vm);
    Vector<String> lines = twinDifferenceLines(context, *vm.jitCacheState(), twinReportPath);
    if (lines.size() != 1)
        JITCACHE_FAIL(makeString("the twin report holds "_s, lines.size(), " differences, not the one of a wrong supplied digest"_s));
    for (const String& line : lines) {
        if (!line.startsWith("{\"kind\":\"difference\",\"part\":\"ucb\",\"check\":\"T7\","_s))
            JITCACHE_FAIL(makeString("the twin report holds a difference other than a wrong supplied digest's: "_s, line));
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
