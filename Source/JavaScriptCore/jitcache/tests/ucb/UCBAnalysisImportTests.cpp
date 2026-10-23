#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleRecord.h"
#include "ModuleProgramExecutable.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheFaults.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include "jitcache/tests/ucb/UCBAnalysisTestSupport.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/tests/ucb/UCBTestSession.h"
#include "jitcache/ucb/UCBAnalysis.h"
#include "jitcache/ucb/UCBCapture.h"
#include "jitcache/ucb/UCBKeys.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of a module's analysis in a VM that runs JITCache: the import Bun calls, with the body lookup stubbed, and the
// analysis source a module request keeps for a capture, which buildSections encodes or copies.

namespace JSC::JITCache::Tests {

using namespace UCBAnalysisTestSupport;

namespace UCBAnalysisImportTestsInternal {

// Serves the VMState's body lookups while it lives. By default every key has no body. serveAny answers each section read
// with a body made for the key asked, holding the given analysis or none; serve lists one body at its key under an index
// token; serveUnusable answers every lookup as the integrator answers a damaged body. It remembers the keys read.
class AnalysisLookupStub {
    WTF_MAKE_NONCOPYABLE(AnalysisLookupStub);

public:
    explicit AnalysisLookupStub(VMState& state)
        : m_state(state)
    {
        m_state.setBodyLookupForTesting([this](const BodyKey& key) {
            return m_body && m_body->key() == key ? m_token : 0;
        }, [this](const BodyKey& key) {
            m_keysRead.append(key);
            if (m_unusable)
                return BodyLookup::unusable();
            if (m_anyAnalysis)
                return BodyLookup::found(bodyWith(key, *m_anyAnalysis));
            if (m_body && m_body->key() == key)
                return BodyLookup::found(Ref { *m_body });
            return BodyLookup::missing();
        });
    }

    ~AnalysisLookupStub() { m_state.clearBodyLookupForTesting(); }

    void serveNothing()
    {
        m_body = nullptr;
        m_anyAnalysis = std::nullopt;
        m_unusable = false;
    }

    void serveAny(Vector<uint8_t>&& analysis)
    {
        serveNothing();
        m_anyAnalysis = WTF::move(analysis);
    }

    void serve(Ref<ValidatedBody>&& body, uint64_t token)
    {
        serveNothing();
        m_body = WTF::move(body);
        m_token = token;
    }

    void serveUnusable()
    {
        serveNothing();
        m_unusable = true;
    }

    const Vector<BodyKey>& keysRead() const { return m_keysRead; }

private:
    // A body of an identity section that no import reads and, when given, an analysis.
    static Ref<ValidatedBody> bodyWith(const BodyKey& key, const Vector<uint8_t>& analysis)
    {
        std::array<uint8_t, 8> identity { };
        std::array<ValidatedBody::TestSection, 2> sections { {
            { SectionKind::UCBIdentity, identity },
            { SectionKind::UCBAnalysis, analysis.span() },
        } };
        std::span<const ValidatedBody::TestSection> given { sections };
        return ValidatedBody::createForTesting(key, 1, analysis.isEmpty() ? given.first(1) : given);
    }

    VMState& m_state;
    RefPtr<ValidatedBody> m_body;
    uint64_t m_token { 0 };
    std::optional<Vector<uint8_t>> m_anyAnalysis;
    bool m_unusable { false };
    Vector<BodyKey> m_keysRead;
};

// The outcomes of the bench report's module-analysis events, in order.
static Vector<String> moduleAnalysisOutcomes(VM& vm, const String& path)
{
    flushBenchReport(vm);
    constexpr auto outcomeField = "\"outcome\":\""_s;
    Vector<String> outcomes;
    for (const String& line : readTextFile(path).split('\n')) {
        if (!line.startsWith("{\"event\":\"module-analysis\","_s))
            continue;
        size_t start = line.find(outcomeField);
        if (start == notFound) {
            outcomes.append("none"_s);
            continue;
        }
        start += outcomeField.length();
        outcomes.append(line.substring(start, line.find('"', start) - start));
    }
    return outcomes;
}

static bool sameStrings(const Vector<String>& a, std::initializer_list<ASCIILiteral> b)
{
    if (a.size() != b.size())
        return false;
    size_t i = 0;
    for (ASCIILiteral expected : b) {
        if (a[i++] != expected)
            return false;
    }
    return true;
}

// Generates the module's UCB in a frame of its own, under a linker nothing else holds, so a collection can take the linker;
// null unless the request kept that linker as the UCB's analysis source. The linker's address stays in this frame.
static NEVER_INLINE ModuleProgramExecutable* analysisExecutableWithDyingLinker(JSGlobalObject* globalObject, const SourceCode& source)
{
    JSModuleRecord* linker = analyzeModule(globalObject, source);
    if (!linker)
        return nullptr;
    ModuleProgramExecutable* executable = ModuleProgramExecutable::tryCreate(globalObject, source, linker);
    UnlinkedCodeBlock* codeBlock = executable ? executable->unlinkedCodeBlock() : nullptr;
    VMState* state = globalObject->vm().jitCacheState();
    if (!codeBlock || !state || state->registry().moduleAnalysisSource(*codeBlock) != linker)
        return nullptr;
    return executable;
}

// Conservative scanning reads the frames a collection runs in, which reuse the stack the frames above left behind;
// zeroing it first keeps a stale pointer there from holding the linker the test expects to die.
static NEVER_INLINE void analysisClearDeadStack()
{
    std::array<volatile uint8_t, 128 * KB> bytes;
    for (auto& byte : bytes)
        byte = 0;
}

} // namespace UCBAnalysisImportTestsInternal

using namespace UCBAnalysisImportTestsInternal;

// Without JITCache state, importModuleAnalysis returns nothing.
JITCACHE_TEST(ucbAnalysisImportWithoutState, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    JITCACHE_CHECK(!importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, nullptr));
}

// In a Producer, importModuleAnalysis returns nothing.
JITCACHE_TEST(ucbAnalysisImportInAProducer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Producer))
        return;
    EnteredGlobalObject entered { vm };
    JITCACHE_CHECK(!importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, nullptr));
    checkRegistry(context, vm);
}

// With cache activity off, importModuleAnalysis returns nothing.
JITCACHE_TEST(ucbAnalysisImportWithActivityOff, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer))
        return;
    EnteredGlobalObject entered { vm };
    didAttachDebugger(vm);
    JITCACHE_CHECK(!importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, nullptr));
}

// A Consumer with a bench report: the key the import reads is the one the module's request records; a key without a
// body, a body without the section, a body with one, the same body through a supplied digest, which the import takes
// without hashing the text, and a damaged body, each with its event.
JITCACHE_TEST(ucbAnalysisImportInAConsumer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory)
        return;
    String benchPath = directory->path("analysis.bench"_s);
    if (!startJITCache(context, vm, *directory, Role::Consumer, { }, benchPath))
        return;
    EnteredGlobalObject entered { vm };
    JSGlobalObject& globalObject = *entered.globalObject();
    VMState& state = *vm.jitCacheState();
    UCBStatistics& statistics = state.registry().statistics();
    AnalysisLookupStub stub { state };

    String text = "import { jitcacheUA7a } from \"m\"; export const jitcacheUA7b = jitcacheUA7a;"_s;
    Digest256 digest = sourceDigest(text);
    auto section = encodedText(&globalObject, text);
    if (!section) {
        JITCACHE_FAIL("the import fixture did not encode"_s);
        return;
    }

    // No body: the digest alone, from the text.
    UCBStatistics before = statistics;
    auto missed = importModuleAnalysis(globalObject, text, nullptr);
    JITCACHE_CHECK(missed && missed->sourceDigest == digest && missed->moduleInfo.isEmpty());
    JITCACHE_CHECK(statistics.analysisMisses == before.analysisMisses + 1 && statistics.sourceDigests == before.sourceDigests + 1);
    if (stub.keysRead().size() != 1) {
        JITCACHE_FAIL("the import did not read one section"_s);
        return;
    }
    BodyKey readKey = stub.keysRead().last();

    // The module's request, which misses and generates, records that key.
    SourceCode moduleSource = analysisModuleSource(text);
    Strong<ModuleProgramExecutable> executable { vm, ModuleProgramExecutable::tryCreate(&globalObject, moduleSource) };
    UnlinkedCodeBlock* codeBlock = executable ? executable.get()->unlinkedCodeBlock() : nullptr;
    auto record = codeBlock ? state.registry().recordOf(*codeBlock) : std::nullopt;
    JITCACHE_CHECK(record && record->key == readKey);

    // A body without the section, then one with it.
    before = statistics;
    stub.serveAny({ });
    auto absent = importModuleAnalysis(globalObject, text, nullptr);
    JITCACHE_CHECK(absent && absent->sourceDigest == digest && absent->moduleInfo.isEmpty() && statistics.analysisAbsent == before.analysisAbsent + 1);
    before = statistics;
    stub.serveAny(Vector<uint8_t> { *section });
    auto hit = importModuleAnalysis(globalObject, text, nullptr);
    JITCACHE_CHECK(hit && hit->moduleInfo == *section && statistics.analysisImports == before.analysisImports + 1);

    // A supplied digest equal to the text's finds the same body, and the import does not hash the text.
    before = statistics;
    auto supplied = importModuleAnalysis(globalObject, text, &digest);
    JITCACHE_CHECK(supplied && supplied->moduleInfo == *section);
    JITCACHE_CHECK(statistics.sourceDigests == before.sourceDigests);

    // A body the integrator found damaged.
    stub.serveUnusable();
    JITCACHE_CHECK(!importModuleAnalysis(globalObject, text, nullptr));

    Vector<String> outcomes = moduleAnalysisOutcomes(vm, benchPath);
    if (!sameStrings(outcomes, { "miss"_s, "absent"_s, "hit"_s, "hit"_s, "invalid"_s }))
        JITCACHE_FAIL(makeString("the bench report holds "_s, outcomes.size(), " module-analysis events, not the five expected in order"_s));
    executable.clear();
    checkRegistry(context, vm);
}

// A Consumer takes a supplied digest as the key's digest without hashing the text, even one that is not the text's: the
// import returns that digest and misses at its key.
JITCACHE_TEST(ucbAnalysisImportTakesASuppliedDigest, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer))
        return;
    EnteredGlobalObject entered { vm };
    VMState& state = *vm.jitCacheState();
    UCBStatistics& statistics = state.registry().statistics();
    AnalysisLookupStub stub { state };

    Digest256 supplied = filledDigest(0x42);
    UCBStatistics before = statistics;
    auto result = importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, &supplied);
    JITCACHE_CHECK(result && result->sourceDigest == supplied && result->moduleInfo.isEmpty());
    JITCACHE_CHECK(statistics.sourceDigests == before.sourceDigests);
    JITCACHE_CHECK(statistics.analysisMisses == before.analysisMisses + 1);
    checkRegistry(context, vm);
}

// With the switch off the import returns the digest alone and reads no section.
JITCACHE_TEST_WITH_OPTIONS(ucbAnalysisImportWithTheOptionOff, Yes, "--jitCacheImportModuleAnalysis=false")
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory)
        return;
    String benchPath = directory->path("analysis.bench"_s);
    if (!startJITCache(context, vm, *directory, Role::Consumer, { }, benchPath))
        return;
    EnteredGlobalObject entered { vm };
    AnalysisLookupStub stub { *vm.jitCacheState() };
    String text = "export const jitcacheUA7 = 1;"_s;
    auto section = encodedText(entered.globalObject(), text);
    if (!section)
        return;
    stub.serveAny(WTF::move(*section));
    auto result = importModuleAnalysis(*entered.globalObject(), text, nullptr);
    JITCACHE_CHECK(result && result->sourceDigest == sourceDigest(text) && result->moduleInfo.isEmpty() && stub.keysRead().isEmpty());
    JITCACHE_CHECK(sameStrings(moduleAnalysisOutcomes(vm, benchPath), { "off"_s }));
    checkRegistry(context, vm);
}

// In a Producer: a module request with a linker on a Module provider keeps the linker as the UCB's analysis source,
// whether it generated the UCB or the CodeCache served it; a BunTranspiledModule provider and a request without a linker
// keep none; a later request replaces a dead source and keeps a live one; buildSections encodes the live source with or
// without an image, copies the saved body's section without one, writes none when neither exists, and fails without a
// charge on a damaged saved body; and the source goes with the UCB's record.
JITCACHE_TEST(ucbAnalysisSourcesInAProducer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Producer))
        return;
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    VMState& state = *vm.jitCacheState();
    UCBRegistry& registry = state.registry();
    UCBStatistics& statistics = registry.statistics();
    AnalysisLookupStub stub { state };

    auto moduleText = [](ASCIILiteral salt) {
        return makeString("import { jitcacheUA8a } from \"m\"; export function jitcacheUA8"_s, salt, "() { return jitcacheUA8a; }\nexport const jitcacheUA8b = 2;\n"_s);
    };
    auto linkerFor = [&](const SourceCode& source) {
        return Strong<JSModuleRecord> { vm, analyzeModule(globalObject, source) };
    };
    auto request = [&](const SourceCode& source, JSModuleRecord* linker) {
        return Strong<ModuleProgramExecutable> { vm, ModuleProgramExecutable::tryCreate(globalObject, source, linker) };
    };
    auto codeBlockOf = [](const Strong<ModuleProgramExecutable>& executable) -> UnlinkedCodeBlock* {
        return executable ? executable.get()->unlinkedCodeBlock() : nullptr;
    };

    // Generated under a linker.
    SourceCode generatedSource = analysisModuleSource(moduleText("Generated"_s));
    Strong<JSModuleRecord> generatedLinker = linkerFor(generatedSource);
    Strong<ModuleProgramExecutable> generated = request(generatedSource, generatedLinker.get());
    UnlinkedCodeBlock* generatedCodeBlock = codeBlockOf(generated);
    if (!generatedLinker || !generatedCodeBlock) {
        JITCACHE_FAIL("the generated module did not link"_s);
        return;
    }
    JITCACHE_CHECK(registry.moduleAnalysisSource(*generatedCodeBlock) == generatedLinker.get());

    // Without a linker, then served by the CodeCache to a request with one, then to a request with another.
    SourceCode servedSource = analysisModuleSource(moduleText("Served"_s));
    Strong<ModuleProgramExecutable> withoutLinker = request(servedSource, nullptr);
    UnlinkedCodeBlock* servedCodeBlock = codeBlockOf(withoutLinker);
    JITCACHE_CHECK(servedCodeBlock && registry.recordOf(*servedCodeBlock) && !registry.moduleAnalysisSource(*servedCodeBlock));
    Strong<JSModuleRecord> servedLinker = linkerFor(servedSource);
    Strong<ModuleProgramExecutable> served = request(servedSource, servedLinker.get());
    JITCACHE_CHECK(servedCodeBlock && codeBlockOf(served) == servedCodeBlock && registry.moduleAnalysisSource(*servedCodeBlock) == servedLinker.get());
    Strong<JSModuleRecord> laterLinker = linkerFor(servedSource);
    Strong<ModuleProgramExecutable> later = request(servedSource, laterLinker.get());
    JITCACHE_CHECK(servedCodeBlock && codeBlockOf(later) == servedCodeBlock && registry.moduleAnalysisSource(*servedCodeBlock) == servedLinker.get());

    // A BunTranspiledModule provider keeps none, even with a linker.
    SourceCode transpiledSource = analysisModuleSource(moduleText("Transpiled"_s), SourceProviderSourceType::BunTranspiledModule);
    Strong<JSModuleRecord> transpiledLinker = linkerFor(transpiledSource);
    Strong<ModuleProgramExecutable> transpiled = request(transpiledSource, transpiledLinker.get());
    UnlinkedCodeBlock* transpiledCodeBlock = codeBlockOf(transpiled);
    JITCACHE_CHECK(transpiledCodeBlock && registry.recordOf(*transpiledCodeBlock) && !registry.moduleAnalysisSource(*transpiledCodeBlock));

    // A source that died is replaced at the next request the CodeCache serves.
    SourceCode dyingSource = analysisModuleSource(moduleText("Dying"_s));
    Strong<ModuleProgramExecutable> dying { vm, analysisExecutableWithDyingLinker(globalObject, dyingSource) };
    UnlinkedCodeBlock* dyingCodeBlock = codeBlockOf(dying);
    if (!dyingCodeBlock) {
        JITCACHE_FAIL("the module whose linker dies did not link with it as its source"_s);
        return;
    }
    analysisClearDeadStack();
    vm.heap.collectNow(Sync, CollectionScope::Full);
    JITCACHE_CHECK(!registry.moduleAnalysisSource(*dyingCodeBlock) && registry.needsModuleAnalysisSource(*dyingCodeBlock));
    Strong<JSModuleRecord> replacingLinker = linkerFor(dyingSource);
    Strong<ModuleProgramExecutable> replacing = request(dyingSource, replacingLinker.get());
    JITCACHE_CHECK(codeBlockOf(replacing) == dyingCodeBlock && registry.moduleAnalysisSource(*dyingCodeBlock) == replacingLinker.get());

    // buildSections encodes the live source, with an image or without one.
    auto expected = encodedAnalysis(vm, *generatedLinker.get());
    for (BodyImage image : { BodyImage::Absent, BodyImage::Present }) {
        Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
        UCBStatistics before = statistics;
        auto sections = buildSections(vm, *generatedCodeBlock, nullptr, image, budget.get());
        JITCACHE_CHECK(sections && expected && sections->analysis == *expected && statistics.analysisCaptured == before.analysisCaptured + 1);
    }

    // Without a live source: the saved body's section, none and a damaged body.
    Vector<uint8_t> saved = expected ? *expected : Vector<uint8_t> { };
    auto buildWithoutSource = [&](Ref<ProducerBudget>& budget) {
        return buildSections(vm, *servedCodeBlock, nullptr, BodyImage::Absent, budget.get());
    };
    Strong<ModuleProgramExecutable> sourceless = request(analysisModuleSource(moduleText("Sourceless"_s)), nullptr);
    servedCodeBlock = codeBlockOf(sourceless);
    if (!servedCodeBlock || registry.moduleAnalysisSource(*servedCodeBlock)) {
        JITCACHE_FAIL("the module without a source did not link without one"_s);
        return;
    }
    {
        stub.serveAny(Vector<uint8_t> { saved });
        Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
        UCBStatistics before = statistics;
        auto copied = buildWithoutSource(budget);
        JITCACHE_CHECK(copied && copied->analysis == saved && statistics.analysisCopied == before.analysisCopied + 1);
    }
    {
        stub.serveNothing();
        Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
        auto none = buildWithoutSource(budget);
        JITCACHE_CHECK(none && none->analysis.isEmpty());
    }
    {
        stub.serveUnusable();
        Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
        auto unusable = buildWithoutSource(budget);
        JITCACHE_CHECK(!unusable && unusable.error() == UCBCaptureFailure::SavedAnalysisUnusable && !budget->chargedBytes());
    }
    stub.serveNothing();

    // The source goes with the UCB's record, as the UCB's destructor hook removes it.
    registry.unlinkedCodeBlockDestroyed(generatedCodeBlock);
    JITCACHE_CHECK(!registry.moduleAnalysisSource(*generatedCodeBlock) && !registry.needsModuleAnalysisSource(*generatedCodeBlock));
    checkRegistry(context, vm);
}

// In a Consumer no module request keeps an analysis source.
JITCACHE_TEST(ucbAnalysisSourcesInAConsumer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer))
        return;
    EnteredGlobalObject entered { vm };
    VMState& state = *vm.jitCacheState();
    AnalysisLookupStub stub { state };
    SourceCode source = analysisModuleSource("export const jitcacheUA8Consumer = 1;\n"_s);
    Strong<JSModuleRecord> linker { vm, analyzeModule(entered.globalObject(), source) };
    Strong<ModuleProgramExecutable> executable { vm, ModuleProgramExecutable::tryCreate(entered.globalObject(), source, linker.get()) };
    UnlinkedCodeBlock* codeBlock = executable ? executable.get()->unlinkedCodeBlock() : nullptr;
    JITCACHE_CHECK(linker && codeBlock && state.registry().recordOf(*codeBlock) && !state.registry().moduleAnalysisSource(*codeBlock));
    checkRegistry(context, vm);
}

// In a ConsumerProducer, a request that imports the module's UCB keeps its linker as the analysis source. The body
// is the capture of a UCB the VM generated, served at the key of a private module, whose request never reaches the
// CodeCache.
JITCACHE_TEST(ucbAnalysisSourcesInAConsumerProducer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::ConsumerProducer))
        return;
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    VMState& state = *vm.jitCacheState();
    UCBRegistry& registry = state.registry();
    AnalysisLookupStub stub { state };

    SourceCode source = analysisModuleSource("import { jitcacheUA8a } from \"m\"; export function jitcacheUA8Imported() { return jitcacheUA8a; }\n"_s);
    Strong<JSModuleRecord> producerLinker { vm, analyzeModule(globalObject, source) };
    Strong<ModuleProgramExecutable> producer { vm, ModuleProgramExecutable::tryCreate(globalObject, source, producerLinker.get()) };
    UnlinkedCodeBlock* producerCodeBlock = producer ? producer.get()->unlinkedCodeBlock() : nullptr;
    auto producerRecord = producerCodeBlock ? registry.recordOf(*producerCodeBlock) : std::nullopt;
    if (!producerRecord || registry.moduleAnalysisSource(*producerCodeBlock) != producerLinker.get()) {
        JITCACHE_FAIL("the ConsumerProducer's generated module was not recorded with its source"_s);
        return;
    }
    Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
    auto sections = buildSections(vm, *producerCodeBlock, nullptr, BodyImage::Absent, budget.get());
    if (!sections || sections->analysis.isEmpty()) {
        JITCACHE_FAIL("the ConsumerProducer's module did not capture with its analysis"_s);
        return;
    }
    std::array<ValidatedBody::TestSection, 4> bodySections { {
        { SectionKind::UCBIdentity, sections->identity.span() },
        { SectionKind::UCBCore, sections->core->span() },
        { SectionKind::UCBFeedback, sections->feedback.span() },
        { SectionKind::UCBAnalysis, sections->analysis.span() },
    } };
    stub.serve(ValidatedBody::createForTesting(producerRecord->key, 7000, bodySections), 70);

    Strong<JSModuleRecord> importingLinker { vm, analyzeModule(globalObject, source) };
    Strong<SymbolTable> moduleScope { vm, SymbolTable::create(vm) };
    Strong<ModuleProgramExecutable> importing { vm, ModuleProgramExecutable::tryCreate(globalObject, source, importingLinker.get(), { moduleScope.get() }) };
    UnlinkedCodeBlock* importedCodeBlock = importing ? importing.get()->unlinkedCodeBlock() : nullptr;
    auto importedRecord = importedCodeBlock ? registry.recordOf(*importedCodeBlock) : std::nullopt;
    JITCACHE_CHECK(importedRecord && importedRecord->origin == UCBOrigin::Imported && importedCodeBlock != producerCodeBlock);
    JITCACHE_CHECK(importedCodeBlock && registry.moduleAnalysisSource(*importedCodeBlock) == importingLinker.get());
    checkRegistry(context, vm);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
