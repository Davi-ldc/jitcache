#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BaselineJITCode.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "DirectEvalExecutable.h"
#include "DirectEvalSite.h"
#include "FunctionExecutable.h"
#include "JITCacheAPI.h"
#include "JITCacheSHA256.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LLIntData.h"
#include "ProgramExecutable.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
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
#include "UnlinkedProgramCodeBlock.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/FileSystem.h>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of the UCB engine in UCBImport, with the integrator's body lookup stubbed: attaches, imports and seeds, the
// misses and stamps of each, provenance and butterfly maps, decoded slots, supplied digests and the invalid material a wrong
// one raises. The parts act on the VM's own registry and statistics, as the engine does; their UCBs are unpublished or
// test-held, so no CodeBlock installs what they attach. Each test starts JITCache on its VM in one configuration, with a
// twin report in a temporary file, and checks what a run in that configuration must leave: a fault only where the test
// expects one, a registry verifyRegistry finds sound, and no twin difference but the ones the test expects.

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

// The three sections a capture of a UCB writes, in a body with an image: an empty image section gives it highest tier 1,
// so its imports and attaches leave pending imports, which no test here installs.
struct TestBody {
    Vector<uint8_t> identity;
    Vector<uint8_t> core;
    Vector<uint8_t> feedback;

    Ref<ValidatedBody> validated(const BodyKey& key, uint64_t version) const
    {
        std::array<ValidatedBody::TestSection, 4> sections { {
            { SectionKind::UCBIdentity, identity.span() },
            { SectionKind::UCBCore, core.span() },
            { SectionKind::UCBFeedback, feedback.span() },
            { SectionKind::ImageBaseline, { } },
        } };
        return ValidatedBody::createForTesting(key, version, sections);
    }
};

// The sections a capture of `producer` would write under the given key, context and provenance, with a function body's
// parse fields and holder, as buildSections writes them.
static std::optional<TestBody> buildTestBody(VM& vm, UnlinkedCodeBlock& producer, const UnlinkedFunctionExecutable* holder, const BodyKey& key, const Digest256& context, CoreProvenance provenance,
    const std::optional<FunctionParseFields>& parseFields = std::nullopt)
{
    auto core = encodeCoreBytes(vm, producer, holder);
    if (!core)
        return std::nullopt;
    TestBody body;
    body.core = WTF::move(*core);
    FeedbackCounts counts = liveFeedbackCounts(producer);
    body.feedback = Vector<uint8_t>(feedbackSectionSize(counts));
    writeFeedbackSection(body.feedback.mutableSpan(), producer, counts);
    std::optional<Digest256> holderDigestValue = holder ? holderDigest(vm, *holder) : std::nullopt;
    auto constantCount = static_cast<uint32_t>(producer.constantRegisters().size());
    body.identity = Vector<uint8_t>(identitySectionSize(constantCount));
    writeIdentitySection(body.identity.mutableSpan(), vm, key, context, provenance, SHA256::hash(body.core.span()), holderDigestValue, coreKindFor(key.identityKind()), parseFields, producer);
    return body;
}

static FunctionParseFields parseFieldsOf(const TwinParseResults& results)
{
    return FunctionParseFields { results.features, results.lexicallyScopedFeatures, results.hasCapturedVariables };
}

static std::span<const uint8_t> butterflyMapOf(const TestBody& body)
{
    auto identity = parseIdentitySection(body.identity.span(), false);
    return identity ? identity->butterflyMap : std::span<const uint8_t> { };
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
    return globalContextDigest(IdentityKind::Program, source.startOffset(), source.firstLine().oneBasedInt(), JSParserScriptMode::Classic, executable.derivedContextType(), EvalContextType::None, executable.isArrowFunctionContext());
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
    Digest256 context = executableBodyContextDigest(IdentityKind::Child, source.startOffset(), source.firstLine().oneBasedInt(), std::nullopt);
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
    auto body = buildTestBody(vm, *live.get(), nullptr, key, context, CoreProvenance::Generated);
    auto wrongContext = buildTestBody(vm, *live.get(), nullptr, key, filledDigest(0x99), CoreProvenance::Generated);
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
    TestBody unparsable { identityUnderAnotherKey(body->identity), body->core, brokenMagic(body->feedback) };
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

// A decoded UCB that misses AtomMap is not stamped, computes no digest at its next request, and attaches once its marked
// constants are atoms, without parsing more than the identity section; in a VM whose production is active the attach keeps
// the Generated body's butterfly map.
static bool testAtomMapAttach(VM& vm, VMState& state, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    auto parent = recordedParent(vm, registry, "function jitcacheU8AtomMap() { return 'jitcache-u8-atom-map-constant-0123456789abcdef'; }\n"_s, 0x82);
    if (!part.check(!!parent, "the AtomMap parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    ChildInputs inputs = childInputs(child, parent->program.source, parent->key, ChildTable::Declarations, 0);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
    Strong<UnlinkedFunctionCodeBlock> decoded = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
    if (!part.check(producer && decoded, "the AtomMap function failed to generate"_s))
        return false;

    // The long constant as a native decode leaves it: a plain string with the same characters.
    auto index = stringConstantIndex(*decoded.get(), "jitcache-u8-atom-map-constant-0123456789abcdef"_s);
    JSString* plain = jsString(vm, makeString("jitcache-u8-atom-map-constant-"_s, "0123456789abcdef"_s));
    if (!part.check(index && !plain->tryGetValueImpl()->isAtom(), "the AtomMap function lacks its constant, or the runtime string is an atom already"_s))
        return false;
    decoded.get()->constantRegister(constantRegisterFor(*index)).set(vm, decoded.get(), plain);
    BodyContextInputs contextInputs { static_cast<unsigned>(inputs.source.startOffset()), static_cast<unsigned>(inputs.source.firstLine().oneBasedInt()) };
    UCBRecord decodedRecord { inputs.key, contextInputs, UCBOrigin::Decoded, NoLexicallyScopedFeatures, std::nullopt, std::nullopt, nullptr, nullptr, std::nullopt };
    if (!part.check(registry.recordCodeBlock(*decoded.get(), WTF::move(decodedRecord)), "the decoded function could not be recorded"_s))
        return false;
    auto body = buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results));
    if (!part.check(!!body, "the AtomMap function's body could not be built"_s))
        return false;
    artifact.list(inputs.key, 11, body->validated(inputs.key, 2000));
    auto attach = [&] {
        RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, child);
        attachLive(request, *decoded.get());
    };
    auto record = [&] {
        return registry.recordOf(*decoded.get());
    };

    UCBStatistics before = statistics;
    attach();
    bool firstMiss = missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && record() && !record()->missedBodyVersion && record()->matchedBodyVersion == 2000
        && !record()->hasPendingImport && statistics.holderDigests == before.holderDigests;
    if (!part.check(firstMiss, "a decoded UCB whose marked constant is not an atom did not miss AtomMap unstamped at the file's commit identifier"_s))
        return false;
    before = statistics;
    attach();
    if (!part.check(missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && computedNoDigest(before, statistics), "the request after an AtomMap miss computed a digest"_s))
        return false;

    // Once the constant is an atom, the next request attaches; the feedback section, here broken, is never parsed.
    atomizeStringConstant(vm, *plain);
    TestBody identityOnly { body->identity, body->core, brokenMagic(body->feedback) };
    artifact.list(inputs.key, 11, identityOnly.validated(inputs.key, 2000));
    before = statistics;
    attach();
    if (!part.check(state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && computedNoDigest(before, statistics), "a decoded UCB whose marked constants became atoms did not attach at the matched commit identifier"_s))
        return false;
    std::span<const uint8_t> bodyMap = butterflyMapOf(*body);
    Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
    bool keeps = registry.copyGeneratedButterflyMap(*decoded.get(), kept.mutableSpan());
    return part.check(keeps == state.productionActive() && (!keeps || equalSpans(kept.span(), bodyMap)), "an attach from a Generated body did not keep its butterfly map exactly when production is active"_s);
}

// A decoded program in the case above computes only its request key at its next requests: sourceDigests grows by one per
// request while contextDigests and holderDigests stay as they are, and no core digest runs, which the last attach shows
// against a body whose identity section names another core digest and whose feedback section is broken.
static bool testAtomMapAttachToProgram(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider("var jitcacheU8AtomMapProgram = 'jitcache-u8-atom-map-program-0123456789';\n"_s));
    ProgramExecutable& executable = *fixture.executable.get();
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    Strong<UnlinkedCodeBlock> decoded = generateProgramFor(vm, fixture);
    if (!part.check(producer && decoded, "the AtomMap program failed to generate"_s))
        return false;

    // The long constant as a native decode leaves it: a plain string with the same characters.
    auto index = stringConstantIndex(*decoded.get(), "jitcache-u8-atom-map-program-0123456789"_s);
    JSString* plain = jsString(vm, makeString("jitcache-u8-atom-map-program-"_s, "0123456789"_s));
    if (!part.check(index && !plain->tryGetValueImpl()->isAtom(), "the AtomMap program lacks its constant, or the runtime string is an atom already"_s))
        return false;
    decoded.get()->constantRegister(constantRegisterFor(*index)).set(vm, decoded.get(), plain);
    GlobalContextInputs contextInputs { static_cast<unsigned>(fixture.source.startOffset()), static_cast<unsigned>(fixture.source.firstLine().oneBasedInt()), JSParserScriptMode::Classic,
        executable.derivedContextType(), EvalContextType::None, executable.isArrowFunctionContext() };
    UCBRecord decodedRecord { key, contextInputs, UCBOrigin::Decoded, NoLexicallyScopedFeatures, std::nullopt, std::nullopt, nullptr, nullptr, std::nullopt };
    if (!part.check(registry.recordCodeBlock(*decoded.get(), WTF::move(decodedRecord)), "the decoded program could not be recorded"_s))
        return false;
    auto body = buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, executable), CoreProvenance::Generated);
    if (!part.check(!!body, "the AtomMap program's body could not be built"_s))
        return false;
    artifact.list(key, 12, body->validated(key, 2100));
    auto attach = [&] {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, executable.lexicallyScopedFeatures());
        prepareProgramRequest(request, executable);
        attachLive(request, *decoded.get());
    };
    auto record = [&] {
        return registry.recordOf(*decoded.get());
    };
    // What a request at the matched commit identifier computes: its request key, whose root source digest is computed
    // from the text, and nothing else.
    auto computedOnlyItsKey = [&](const UCBStatistics& before) {
        return statistics.sourceDigests == before.sourceDigests + 1 && statistics.suppliedSourceDigests == before.suppliedSourceDigests
            && statistics.contextDigests == before.contextDigests && statistics.holderDigests == before.holderDigests;
    };

    UCBStatistics before = statistics;
    attach();
    bool firstMiss = missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && record() && !record()->missedBodyVersion && record()->matchedBodyVersion == 2100
        && !record()->hasPendingImport;
    if (!part.check(firstMiss, "a decoded program whose marked constant is not an atom did not miss AtomMap unstamped at the file's commit identifier"_s))
        return false;
    before = statistics;
    attach();
    if (!part.check(missesOf(statistics, MissReason::AtomMap) == missesOf(before, MissReason::AtomMap) + 1 && computedOnlyItsKey(before), "the request after a decoded program's AtomMap miss computed more than its request key"_s))
        return false;

    // Once the constant is an atom, the next request attaches without comparing a core digest or parsing the feedback.
    atomizeStringConstant(vm, *plain);
    Vector<uint8_t> otherCoreDigest = body->identity;
    otherCoreDigest[identityCoreDigestOffset] ^= 0xff;
    TestBody unmatchable { WTF::move(otherCoreDigest), body->core, brokenMagic(body->feedback) };
    artifact.list(key, 12, unmatchable.validated(key, 2100));
    before = statistics;
    attach();
    bool attached = state.tracksKeys() && statistics.attaches == before.attaches + 1 && record()->hasPendingImport && computedOnlyItsKey(before);
    return part.check(attached, "a decoded program whose marked constants became atoms did not attach at the matched commit identifier with its request key alone"_s);
}

// A root body's import computes no context and no holder digest while its key has no body, or a body of provenance
// EmbedderDecoded, and neither does seedDecoded for a key with no body; an import that reaches the holder check computes
// the holder digest once.
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
    Digest256 context = executableBodyContextDigest(IdentityKind::FunctionConstructor, bodySource.startOffset(), bodySource.firstLine().oneBasedInt(), rootHolder);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, *root.get(), bodySource, CodeSpecializationKind::CodeForCall, &results);
    Strong<UnlinkedFunctionCodeBlock> unpublished = generateBodyOf(vm, *root.get(), bodySource, CodeSpecializationKind::CodeForCall);
    if (!part.check(producer && unpublished, "the root's body failed to generate"_s))
        return false;
    auto generatedBody = buildTestBody(vm, *producer.get(), root.get(), key, context, CoreProvenance::Generated, parseFieldsOf(results));
    auto embedderBody = buildTestBody(vm, *producer.get(), root.get(), key, context, CoreProvenance::EmbedderDecoded, parseFieldsOf(results));
    if (!part.check(generatedBody && embedderBody, "the root's bodies could not be built"_s))
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
    before = statistics;
    {
        RequestState request(vm, RequestKind::FunctionBody, bodySource, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, *root.get());
        seedDecoded(request, *unpublished.get());
    }
    auto seededRecord = registry.recordOf(*unpublished.get());
    bool seedNoBody = missesOf(statistics, MissReason::NoBody) == missesOf(before, MissReason::NoBody) + 1 && computedNoDigest(before, statistics)
        && seededRecord && seededRecord->origin == UCBOrigin::Decoded && !seededRecord->hasPendingImport && !seededRecord->missedBodyVersion;
    if (!part.check(seedNoBody, "seedDecoded for a key with no body computed a digest, stamped, or did not record the UCB as decoded"_s))
        return false;
    artifact.list(key, 21, embedderBody->validated(key, 3000));
    before = statistics;
    bool provenanceMiss = !importRootBody() && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && computedNoDigest(before, statistics);
    if (!part.check(provenanceMiss, "a root body's import of an EmbedderDecoded body computed a context or holder digest, or missed otherwise than Provenance"_s))
        return false;
    artifact.list(key, 22, generatedBody->validated(key, 3001));
    before = statistics;
    Strong<UnlinkedCodeBlock> imported = importRootBody();
    bool importedOnce = imported && statistics.imports == before.imports + 1 && statistics.holderDigests == before.holderDigests + 1 && statistics.contextDigests == before.contextDigests + 1;
    return part.check(importedOnce, "a root body's import did not compute its holder digest exactly once"_s);
}

// A body of provenance EmbedderDecoded misses Provenance for a program import, a function import and an attach to a
// generated function UCB, and seeds a decoded function whose core matches; so does a body of provenance Generated, and only
// a seed from that one keeps its butterfly map, in a VM whose production is active.
static bool testProvenance(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    // A program import.
    ProgramRequestFixture fixture = programRequestFixture(globalObject, plainProvider("var jitcacheU8Provenance = 1;\n"_s));
    BodyKey programBodyKey = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> programProducer = generateProgramFor(vm, fixture);
    auto programBody = programProducer ? buildTestBody(vm, *programProducer.get(), nullptr, programBodyKey, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::EmbedderDecoded) : std::nullopt;
    if (!part.check(!!programBody, "the provenance program's body could not be built"_s))
        return false;
    artifact.list(programBodyKey, 31, programBody->validated(programBodyKey, 4000));
    UCBStatistics before = statistics;
    {
        RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *fixture.executable.get());
        Strong<UnlinkedCodeBlock> imported { vm, importBody(request) };
        if (!part.check(!imported && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && statistics.contextDigests == before.contextDigests, "a program import of an EmbedderDecoded body did not miss Provenance before its context"_s))
            return false;
    }

    // A function import, an attach to a generated function UCB, and seeds of decoded functions.
    auto parent = recordedParent(vm, registry, "function jitcacheU8Provenance(a) { return [\"jitcache-u8-p-one\", \"jitcache-u8-p-two\"].indexOf(a); }\n"_s, 0x83);
    if (!part.check(!!parent, "the provenance parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    ChildInputs inputs = childInputs(child, parent->program.source, parent->key, ChildTable::Declarations, 0);
    TwinParseResults results;
    Strong<UnlinkedFunctionCodeBlock> producer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
    auto embedderBody = producer ? buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::EmbedderDecoded, parseFieldsOf(results)) : std::nullopt;
    auto generatedBody = producer ? buildTestBody(vm, *producer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results)) : std::nullopt;
    if (!part.check(embedderBody && generatedBody, "the provenance function's bodies could not be built"_s))
        return false;
    auto withChildRequest = [&](const auto& act) {
        RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
        prepareBodyRequest(request, child);
        return act(request);
    };
    artifact.list(inputs.key, 32, embedderBody->validated(inputs.key, 4001));
    before = statistics;
    Strong<UnlinkedCodeBlock> importedFunction { vm, withChildRequest([&](RequestState& request) {
        return importBody(request);
    }) };
    if (!part.check(!importedFunction && missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1, "a function import of an EmbedderDecoded body did not miss Provenance"_s))
        return false;

    Strong<UnlinkedFunctionCodeBlock> generatedFunction = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
    if (!part.check(!!generatedFunction, "the provenance function failed to generate"_s))
        return false;
    withChildRequest([&](RequestState& request) {
        recordGenerated(request, *generatedFunction.get());
        return true;
    });
    artifact.list(inputs.key, 33, embedderBody->validated(inputs.key, 4002));
    before = statistics;
    withChildRequest([&](RequestState& request) {
        attachLive(request, *generatedFunction.get());
        return true;
    });
    auto generatedRecord = registry.recordOf(*generatedFunction.get());
    bool attachMiss = missesOf(statistics, MissReason::Provenance) == missesOf(before, MissReason::Provenance) + 1 && generatedRecord && generatedRecord->missedBodyVersion == 33 && !generatedRecord->hasPendingImport;
    if (!part.check(attachMiss, "an attach of an EmbedderDecoded body to a generated function UCB did not miss Provenance, stamped"_s))
        return false;

    auto seed = [&](const TestBody& body, uint64_t token, CoreProvenance provenance) {
        Strong<UnlinkedFunctionCodeBlock> decoded = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall);
        if (!decoded)
            return false;
        artifact.list(inputs.key, token, body.validated(inputs.key, 4000 + token));
        UCBStatistics seedBefore = statistics;
        withChildRequest([&](RequestState& request) {
            seedDecoded(request, *decoded.get());
            return true;
        });
        auto view = registry.recordOf(*decoded.get());
        std::span<const uint8_t> bodyMap = butterflyMapOf(body);
        Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
        bool keeps = registry.copyGeneratedButterflyMap(*decoded.get(), kept.mutableSpan());
        bool expectsMap = provenance == CoreProvenance::Generated && state.productionActive();
        return statistics.seededDecodes == seedBefore.seededDecodes + 1 && view && view->origin == UCBOrigin::Decoded && view->hasPendingImport
            && keeps == expectsMap && (!keeps || equalSpans(kept.span(), bodyMap));
    };
    if (!part.check(seed(*embedderBody, 34, CoreProvenance::EmbedderDecoded), "an EmbedderDecoded body did not seed a decoded function whose core matches, or left it a butterfly map"_s))
        return false;
    return part.check(seed(*generatedBody, 35, CoreProvenance::Generated), "a Generated body did not seed a decoded function whose core matches, or kept its butterfly map otherwise than while production is active"_s);
}

// didDecodeCachedSlots with the requested slot empty and the other decoded records the other slot and leaves the request
// unsettled, so didGenerate records the generated slot. Any role that tracks keys.
static bool testDecodedSlotRecords(VM& vm, VMState& state, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };
    auto parent = recordedParent(vm, registry, "function jitcacheU8Slots(a) { this.a = a; }\n"_s, 0x84);
    if (!part.check(!!parent, "the slots parent could not be generated and recorded"_s))
        return false;
    UnlinkedFunctionExecutable& child = *parent->program.codeBlock.get()->functionDecl(0);
    SourceCode source = child.linkedSourceCode(parent->program.source);
    Strong<UnlinkedFunctionCodeBlock> forCall = generateBodyOf(vm, child, source, CodeSpecializationKind::CodeForCall);
    Strong<UnlinkedFunctionCodeBlock> forConstruct = generateBodyOf(vm, child, source, CodeSpecializationKind::CodeForConstruct);
    if (!part.check(forCall && forConstruct, "the slots function failed to generate"_s))
        return false;
    {
        FunctionBodyRequest request(vm, child, source, CodeSpecializationKind::CodeForConstruct, { });
        request.didDecodeCachedSlots(forCall.get(), nullptr);
        request.didGenerate(*forConstruct.get());
    }
    Digest256 identityDigest = childIdentityDigest(parent->key, ChildTable::Declarations, 0);
    auto callRecord = registry.recordOf(*forCall.get());
    auto constructRecord = registry.recordOf(*forConstruct.get());
    bool recorded = callRecord && callRecord->origin == UCBOrigin::Decoded && callRecord->key == BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForCall, { }, identityDigest)
        && constructRecord && constructRecord->origin == UCBOrigin::Generated && constructRecord->key == BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, { }, identityDigest);
    return part.check(recorded, "didDecodeCachedSlots did not record the other slot as decoded while leaving the request to record its generated slot"_s);
}

// A decoded function, seeded or attached through the native decode of a bytecode-cache payload, keeps the butterfly map of
// a Generated body in a VM whose production is active, and buildSections writes it with provenance Generated; seeded from
// an EmbedderDecoded body, or in a Consumer, it keeps none and is written EmbedderDecoded.
static bool testDecodedButterflyProvenance(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    String text = "function jitcacheU8Literal(s) { return [\"jitcache-u8-literal-alpha\", \"jitcache-u8-literal-beta\"].indexOf(s); }\n"_s;
    ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
    if (!part.check(generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 1, "the butterfly-provenance program failed to generate"_s))
        return false;
    SourceCodeKey cacheKey = programCacheKey(generated.source);
    RefPtr<CachedBytecode> payload = encodeCodeBlock(vm, cacheKey, generated.codeBlock.get());
    UnlinkedFunctionExecutable& generatedFunction = *generated.codeBlock.get()->functionDecl(0);
    Strong<UnlinkedFunctionCodeBlock> producer { vm, decodedBody(vm, generatedFunction, generated.source) };
    if (!part.check(payload && producer, "the butterfly-provenance program did not encode to a bytecode cache"_s))
        return false;
    FunctionParseFields parseFields { generatedFunction.features(), generatedFunction.lexicallyScopedFeatures(), generatedFunction.hasCapturedVariables() };

    auto scenario = [&](uint8_t parentFill, CoreProvenance provenance, bool attachInstead, bool expectsMap) {
        Strong<UnlinkedProgramCodeBlock> decoded { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, cacheKey, Ref<CachedBytecode> { *payload }) };
        BodyKey parentKey = testKey(parentFill);
        if (!decoded || !registry.recordCodeBlock(*decoded.get(), testRecord(parentKey, UCBOrigin::Decoded)))
            return false;
        UnlinkedFunctionExecutable& function = *decoded.get()->functionDecl(0);
        ChildInputs inputs = childInputs(function, generated.source, parentKey, ChildTable::Declarations, 0);
        auto body = buildTestBody(vm, *producer.get(), &generatedFunction, inputs.key, inputs.context, provenance, parseFields);
        if (!body)
            return false;
        StubbedArtifact artifact;
        StubbedBodyLookup lookup { state, artifact };
        UnlinkedFunctionCodeBlock* slot = nullptr;
        if (attachInstead) {
            slot = decodedBody(vm, function, generated.source); // no body: the decode is recorded as Decoded
            if (!slot)
                return false;
            // A live decoded UCB attaches only once the constants its atom map marks are atoms, which the native decode
            // leaves plain; a property-key use of each would atomize it in place.
            for (auto& constant : slot->constantRegisters()) {
                if (JSValue value = constant.get(); value && value.isString())
                    atomizeStringConstant(vm, *asString(value));
            }
            artifact.list(inputs.key, 41, body->validated(inputs.key, 5000));
            if (decodedBody(vm, function, generated.source) != slot) // the filled slot: an attach
                return false;
        } else {
            artifact.list(inputs.key, 41, body->validated(inputs.key, 5000));
            slot = decodedBody(vm, function, generated.source); // the decode: a seed
        }
        auto view = slot ? registry.recordOf(*slot) : std::nullopt;
        if (!view || view->origin != UCBOrigin::Decoded || !view->hasPendingImport || view->matchedBodyVersion != 5000)
            return false;
        std::span<const uint8_t> bodyMap = butterflyMapOf(*body);
        Vector<uint8_t> kept(FillWith { }, bodyMap.size(), 0);
        if (registry.copyGeneratedButterflyMap(*slot, kept.mutableSpan()) != expectsMap)
            return false;

        // A CodeBlock of the decoded function, which buildSections captures from.
        FunctionExecutable* functionExecutable = function.link(vm, nullptr, generated.source);
        JSFunction* jsFunction = JSFunction::create(vm, globalObject, functionExecutable, globalObject->globalScope());
        DeferGC deferGC(vm);
        CodeBlock* codeBlock = functionExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, jsFunction, globalObject->globalScope());
        if (!codeBlock || codeBlock->unlinkedCodeBlock() != slot)
            return false;
        auto sections = buildSections(vm, *codeBlock->unlinkedCodeBlock(), holderOf(*codeBlock), state.twinBudget().get());
        if (!sections)
            return false;
        auto identity = parseIdentitySection(sections->identity.span(), true);
        if (!identity)
            return false;
        bool anyMarked = std::ranges::any_of(identity->butterflyMap, [](uint8_t byte) {
            return !!byte;
        });
        if (expectsMap)
            return identity->provenance == CoreProvenance::Generated && equalSpans(identity->butterflyMap, bodyMap) && anyMarked;
        return identity->provenance == CoreProvenance::EmbedderDecoded && !anyMarked;
    };

    if (state.productionActive()) {
        return part.check(scenario(0x91, CoreProvenance::Generated, false, true), "a function seeded from a Generated body did not keep its butterfly map or was not written Generated with it"_s)
            && part.check(scenario(0x92, CoreProvenance::Generated, true, true), "a function attached from a Generated body did not keep its butterfly map or was not written Generated with it"_s)
            && part.check(scenario(0x93, CoreProvenance::EmbedderDecoded, false, false), "a function seeded from an EmbedderDecoded body kept a map or was not written EmbedderDecoded"_s);
    }
    return part.check(scenario(0x94, CoreProvenance::Generated, false, false), "a function seeded in a Consumer kept a map or was not written EmbedderDecoded"_s);
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
    Digest256 context = directEvalContextDigest(evalSource.startOffset(), evalSource.firstLine().oneBasedInt(), NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
        PrivateBrandRequirement::None, false, true, EvalContextType::None, tdz, privateNames);
    auto body = buildTestBody(vm, *executable.get()->unlinkedCodeBlock(), nullptr, key, context, CoreProvenance::Generated);
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
// none; a direct eval inside the function imports without a verification, also when the program and the function were
// seeded, and keeps no provider. Every digest here is its text's: the cases of a digest that differs from its text, which
// the twin check reports wherever a request takes one, are the invalid-material test's.
static bool testSuppliedDigestImports(VM& vm, VMState& state, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    bool strict = state.strict();
    StubbedArtifact artifact;
    StubbedBodyLookup lookup { state, artifact };

    // Each chain makes a provider of its own over this text, so both chains compute the same keys. An import verifies a
    // supplied digest once per provider: had the seeded chain reused the provider the import chain verified, a direct eval
    // import that wrongly relied on it would find it verified and count nothing. Seeding verifies nothing, so the seeded
    // chain's provider is still unverified when its direct eval imports, and such an import would verify it there.
    String text = "function jitcacheU8Supplied(a) { return eval('a + 1'); }\n"_s;

    auto importChain = [&](bool seed) {
        Ref<SuppliedDigestProvider> provider = SuppliedDigestProvider::create(text, sourceDigest(text));
        SourceProvider* expectedProvider = strict ? provider.ptr() : nullptr;
        ProgramRequestFixture fixture = programRequestFixture(globalObject, Ref<SourceProvider> { provider.get() });
        BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
        Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
        auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
        if (!body)
            return false;
        artifact.list(key, seed ? 61 : 62, body->validated(key, seed ? 7000 : 7001));
        Strong<UnlinkedCodeBlock> program;
        {
            RequestState request(vm, RequestKind::Program, fixture.source, { }, fixture.executable.get()->lexicallyScopedFeatures());
            prepareProgramRequest(request, *fixture.executable.get());
            if (seed) {
                program = generateProgramFor(vm, fixture);
                if (program)
                    seedDecoded(request, *program.get());
            } else
                program = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto programView = program ? registry.recordOf(*program.get()) : std::nullopt;
        if (!programView || !programView->hasPendingImport || programView->suppliedDigestProvider != expectedProvider)
            return false;

        UnlinkedFunctionExecutable& child = *program.get()->functionDecl(0);
        ChildInputs inputs = childInputs(child, fixture.source, key, ChildTable::Declarations, 0);
        TwinParseResults results;
        Strong<UnlinkedFunctionCodeBlock> childProducer = generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall, &results);
        auto childBody = childProducer ? buildTestBody(vm, *childProducer.get(), &child, inputs.key, inputs.context, CoreProvenance::Generated, parseFieldsOf(results)) : std::nullopt;
        if (!childBody)
            return false;
        artifact.list(inputs.key, seed ? 63 : 64, childBody->validated(inputs.key, seed ? 7002 : 7003));
        Strong<UnlinkedCodeBlock> function;
        {
            RequestState request(vm, RequestKind::FunctionBody, inputs.source, { }, NoLexicallyScopedFeatures);
            prepareBodyRequest(request, child);
            if (seed) {
                function = Strong<UnlinkedCodeBlock> { vm, generateBodyOf(vm, child, inputs.source, CodeSpecializationKind::CodeForCall).get() };
                if (function)
                    seedDecoded(request, *function.get());
            } else
                function = Strong<UnlinkedCodeBlock> { vm, importBody(request) };
        }
        auto functionView = function ? registry.recordOf(*function.get()) : std::nullopt;
        if (!functionView || !functionView->hasPendingImport || functionView->suppliedDigestProvider != expectedProvider)
            return false;
        // Verified now exactly when the program was imported with strict on; the direct eval's import leaves that as it is.
        bool verified = registry.suppliedDigestVerified(provider->asID());
        if (verified != (strict && !seed))
            return false;
        if (!importDirectEval(vm, globalObject, state, artifact, *function.get(), functionView->key, seed ? 1 : 2))
            return false;
        return registry.suppliedDigestVerified(provider->asID()) == verified;
    };

    UCBStatistics before = statistics;
    if (!part.check(importChain(false), "the import of a program, of its function and of a direct eval inside it did not succeed with the records the registry gives them, or did not leave the provider verified exactly with strict on"_s))
        return false;
    if (!part.check(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications + (strict ? 1 : 0), "the imports of a program and its function did not verify its provider exactly once with strict on, and never with it off"_s))
        return false;
    before = statistics;
    if (!part.check(importChain(true), "the seeds of a program and its function, then the import of a direct eval inside it, did not succeed, or the direct eval verified the provider the seeds left unverified"_s))
        return false;
    return part.check(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications, "seeding or a direct eval's import verified a supplied digest"_s);
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

// The engine's parts the VM's configuration allows: the decoded slots in every role; the records of a Producer; and in a
// role that imports, the attaches, imports, seeds and supplied digests.
static bool testEngine(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    VMState* state = vm.jitCacheState();
    if (!part.check(state && state->tracksKeys() && globalObject, "the test's VM does not track keys, or the test has no global object"_s))
        return false;
    if (!testDecodedSlotRecords(vm, *state, part))
        return false;
    if (state->role() == Role::Producer)
        return testProducerRecordsKeepNoProvider(vm, *state, globalObject, part);
    if (!part.check(state->importsEnabled(), "a Consumer or ConsumerProducer does not import"_s))
        return false;
    return testAttachToGeneratedProgram(vm, *state, globalObject, part)
        && testAtomMapAttach(vm, *state, part)
        && testAtomMapAttachToProgram(vm, *state, globalObject, part)
        && testRootImport(vm, *state, part)
        && testProvenance(vm, *state, globalObject, part)
        && testDecodedButterflyProvenance(vm, *state, globalObject, part)
        && testSuppliedDigestImports(vm, *state, globalObject, part);
}

// The two cases of a supplied digest that differs from its text, alone in their test: the twin check reports such a digest
// wherever a request takes it, and the second case turns cache activity off for good. In a Consumer with strict on, a
// decoded program under a wrong supplied digest is first matched by its core, which no digest enters, and seeded with no
// verification, since only an import verifies a supplied digest. Then a program whose import relies on a wrong supplied
// digest is invalid material at ucb.supplied-digest before anything is decoded: its body's core and feedback sections are
// broken, so a decode or a feedback parse would raise another step. With a twin report open, each case adds exactly one
// difference.
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

    // The decoded program.
    Ref<SuppliedDigestProvider> decodedProvider = SuppliedDigestProvider::create("var jitcacheU8WrongDecoded = 'jitcache-u8-wrong-decoded';\n"_s, filledDigest(0xde));
    ProgramRequestFixture decodedFixture = programRequestFixture(globalObject, Ref<SourceProvider> { decodedProvider.get() });
    BodyKey decodedKey = programKey(decodedFixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> decodedProducer = generateProgramFor(vm, decodedFixture);
    Strong<UnlinkedCodeBlock> decoded = generateProgramFor(vm, decodedFixture);
    auto seededBody = decodedProducer ? buildTestBody(vm, *decodedProducer.get(), nullptr, decodedKey, programContext(decodedFixture.source, *decodedFixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
    if (!part.check(decoded && seededBody, "the wrong-digest decoded program could not be built"_s))
        return false;
    artifact.list(decodedKey, 65, seededBody->validated(decodedKey, 7004));
    UCBStatistics beforeSeed = statistics;
    unsigned differencesBeforeSeed = differences();
    {
        RequestState request(vm, RequestKind::Program, decodedFixture.source, { }, decodedFixture.executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *decodedFixture.executable.get());
        seedDecoded(request, *decoded.get());
    }
    bool matchedByCore = state->tracksKeys() && statistics.seededDecodes == beforeSeed.seededDecodes + 1 && statistics.suppliedDigestVerifications == beforeSeed.suppliedDigestVerifications
        && !registry.suppliedDigestVerified(decodedProvider->asID());
    if (!part.check(matchedByCore, "a decoded program under a wrong supplied digest was verified, or not matched by its core"_s))
        return false;
    if (!part.check(!sink || differences() == differencesBeforeSeed + 1, "the seed under a wrong supplied digest did not add exactly one difference to the twin report"_s))
        return false;

    // The import.
    String text = "var jitcacheU8WrongSupplied = 'jitcache-u8-wrong-supplied-digest';\n"_s;
    ProgramRequestFixture fixture = programRequestFixture(globalObject, SuppliedDigestProvider::create(text, filledDigest(0xee)));
    BodyKey key = programKey(fixture.source, NoLexicallyScopedFeatures);
    Strong<UnlinkedCodeBlock> producer = generateProgramFor(vm, fixture);
    auto body = producer ? buildTestBody(vm, *producer.get(), nullptr, key, programContext(fixture.source, *fixture.executable.get()), CoreProvenance::Generated) : std::nullopt;
    if (!part.check(!!body, "the wrong-digest program's body could not be built"_s))
        return false;
    TestBody undecodable { body->identity, Vector<uint8_t>(FillWith { }, body->core.size(), 0), brokenMagic(body->feedback) };
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

JITCACHE_TEST(ucbEngineInANonStrictConsumer, Yes)
{
    runEngineIn(context, Role::Consumer, false);
}

JITCACHE_TEST(ucbEngineInAConsumerProducer, Yes)
{
    runEngineIn(context, Role::ConsumerProducer, true);
}

// A Consumer with strict on runs the two cases of a wrong supplied digest, and is left with cache activity off at
// ucb.supplied-digest, a registry verifyRegistry finds sound, and exactly the two differences the cases report.
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
    if (lines.size() != 2)
        JITCACHE_FAIL(makeString("the twin report holds "_s, lines.size(), " differences, not the two of a wrong supplied digest"_s));
    for (const String& line : lines) {
        if (!line.startsWith("{\"kind\":\"difference\",\"part\":\"ucb\",\"check\":\"T7\","_s))
            JITCACHE_FAIL(makeString("the twin report holds a difference other than a wrong supplied digest's: "_s, line));
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
