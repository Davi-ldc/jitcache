#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "AbstractModuleRecord.h"
#include "JITCacheAPI.h"
#include "JITCacheFaults.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "ModuleAnalyzer.h"
#include "ModuleProgramExecutable.h"
#include "Parser.h"
#include "ProducerBudget.h"
#include "ScriptFetchParameters.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "TestArtifact.h"
#include "TestFiles.h"
#include "TestVM.h"
#include "UCBAnalysis.h"
#include "UCBCapture.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UCBTestSupport.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "ValidatedBody.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The tests of a module's analysis: the bytes the encoder writes from a record the analysis parse built, the forms they
// carry and their widths, their determinism, the records the encoder refuses, the rules validateModuleAnalysis holds a
// section to, the twin comparison, the import Bun calls with the body lookup stubbed, and the analysis source a module
// request keeps for a capture, which buildSections encodes or copies.

namespace JSC::JITCache::Tests {

namespace UCBAnalysisTestsInternal {

static SourceCode analysisModuleSource(const String& text, SourceProviderSourceType type = SourceProviderSourceType::Module)
{
    return makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, type);
}

// The record JSC's analysis parse builds for the source, as JSModuleLoader::makeModule builds it; null on a parse or an
// analysis error. The caller holds it.
static JSModuleRecord* analyzeModule(JSGlobalObject* globalObject, const SourceCode& source)
{
    VM& vm = globalObject->vm();
    ParserError error;
    std::unique_ptr<ModuleProgramNode> node = parseRootNode<ModuleProgramNode>(vm, source, ImplementationVisibility::Public, JSParserBuiltinMode::NotBuiltin,
        StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, SourceParseMode::ModuleAnalyzeMode, error);
    if (error.isValid() || !node)
        return nullptr;
    ModuleAnalyzer analyzer(globalObject, globalObject->moduleLoader(), Identifier::fromString(vm, "jitcache-analysis-module"_s), source, node->features());
    auto record = analyzer.analyze(*node);
    return record ? *record : nullptr;
}

static std::optional<Vector<uint8_t>> encodedAnalysis(VM& vm, const JSModuleRecord& record)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto bytes = encodeModuleAnalysis(vm, record, budget.get());
    if (!bytes)
        return std::nullopt;
    return WTF::move(*bytes);
}

// The analysis section of a module text, from the parse to the encoder.
static std::optional<Vector<uint8_t>> encodedText(JSGlobalObject* globalObject, const String& text)
{
    VM& vm = globalObject->vm();
    Strong<JSModuleRecord> record { vm, analyzeModule(globalObject, analysisModuleSource(text)) };
    if (!record)
        return std::nullopt;
    return encodedAnalysis(vm, *record.get());
}

static uint32_t readLittleEndian(std::span<const uint8_t> bytes, size_t at, unsigned width)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
    return value;
}

// Where the parts of a well-formed section lie, read as Bun's reader reads them.
struct AnalysisLayout {
    unsigned offsetWidth;
    uint32_t stringCount;
    size_t blobStart;
    uint32_t blobLength;
    size_t bodyStart;

    size_t stringStart(std::span<const uint8_t> bytes, uint32_t index) const { return blobStart + readLittleEndian(bytes, 8 + offsetWidth * index, offsetWidth); }
    unsigned idWidth(std::span<const uint8_t> bytes) const { return bytes[bodyStart + 1]; }
};

static AnalysisLayout analysisLayout(std::span<const uint8_t> bytes)
{
    AnalysisLayout layout;
    layout.offsetWidth = bytes[0];
    layout.stringCount = readLittleEndian(bytes, 4, 4);
    size_t offsetsBytes = layout.offsetWidth * (static_cast<size_t>(layout.stringCount) + 1);
    layout.blobStart = 8 + offsetsBytes + (offsetsBytes & 1);
    layout.blobLength = readLittleEndian(bytes, 8 + offsetsBytes - layout.offsetWidth, layout.offsetWidth);
    layout.bodyStart = layout.blobStart + layout.blobLength;
    return layout;
}

// A section holds exactly its bytes: every shorter prefix fails validateModuleAnalysis.
static bool everyPrefixFails(std::span<const uint8_t> bytes)
{
    for (size_t length = 0; length < bytes.size(); ++length) {
        String failure;
        if (validateModuleAnalysis(bytes.first(length), failure))
            return false;
    }
    return true;
}

// A section the tests write themselves, by the layout's rules and apart from the encoder, to craft each case validation
// rejects.
struct CraftedRequest {
    uint32_t specifier;
    uint8_t fetch;
    bool deferred;
    std::optional<uint32_t> host;
};

struct CraftedRecord {
    uint8_t kind;
    uint8_t fetch;
    bool sameName;
    Vector<uint32_t> ids; // the ids its kind lists, then its host id
};

struct CraftedSection {
    Vector<String> strings; // in id order
    uint8_t flags { 0 };
    Vector<CraftedRequest> requests;
    Vector<CraftedRecord> records;
    std::optional<unsigned> offsetWidth; // written in place of the blob length's width when set
};

static unsigned craftedWidth(uint64_t value)
{
    if (value <= 0xFF)
        return 1;
    if (value <= 0xFFFF)
        return 2;
    return 4;
}

static void appendLittleEndian(Vector<uint8_t>& out, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        out.append(static_cast<uint8_t>(value >> (8 * i)));
}

static Vector<uint8_t> craftedBytes(const CraftedSection& section)
{
    Vector<uint8_t> blob;
    Vector<uint32_t> offsets;
    for (const String& string : section.strings) {
        offsets.append(blob.size());
        bool latin1 = true;
        for (unsigned i = 0; i < string.length(); ++i)
            latin1 = latin1 && string[i] <= 0xFF;
        if (latin1) {
            blob.append(1);
            for (unsigned i = 0; i < string.length(); ++i)
                blob.append(static_cast<uint8_t>(string[i]));
            continue;
        }
        blob.append(0);
        if (blob.size() & 1)
            blob.append(0);
        for (unsigned i = 0; i < string.length(); ++i) {
            blob.append(static_cast<uint8_t>(string[i]));
            blob.append(static_cast<uint8_t>(string[i] >> 8));
        }
    }
    offsets.append(blob.size());

    Vector<uint8_t> out;
    unsigned offsetWidth = section.offsetWidth.value_or(craftedWidth(blob.size()));
    out.append(static_cast<uint8_t>(offsetWidth));
    appendLittleEndian(out, 0, 3);
    appendLittleEndian(out, section.strings.size(), 4);
    for (uint32_t offset : offsets)
        appendLittleEndian(out, offset, offsetWidth);
    if (out.size() & 1)
        out.append(0);
    out.appendVector(blob);

    unsigned idWidth = craftedWidth(section.strings.size() + 1);
    out.append(section.flags);
    out.append(static_cast<uint8_t>(idWidth));
    appendLittleEndian(out, 0, 2);
    appendLittleEndian(out, section.requests.size(), 4);
    appendLittleEndian(out, section.records.size(), 4);
    for (const CraftedRecord& record : section.records)
        out.append(static_cast<uint8_t>(record.kind | (record.fetch << 3) | (record.sameName ? 1 << 6 : 0)));
    for (const CraftedRequest& request : section.requests)
        out.append(static_cast<uint8_t>((request.deferred ? 1 : 0) | (request.fetch << 1)));
    for (const CraftedRequest& request : section.requests) {
        appendLittleEndian(out, request.specifier, idWidth);
        if (request.host)
            appendLittleEndian(out, *request.host, idWidth);
    }
    for (const CraftedRecord& record : section.records) {
        for (uint32_t id : record.ids)
            appendLittleEndian(out, id, idWidth);
    }
    return out;
}

// A valid section with every record kind but 1, both phases, a host-defined type and a UTF-16 string, which starts at an
// even blob offset and so has its padding byte.
static constexpr uint32_t craftedStringCount = 11;
static constexpr uint32_t craftedUTF16String = 9;
static CraftedSection craftedBase()
{
    CraftedSection section;
    std::array<char16_t, 2> utf16Name { 0x100, 'e' };
    section.strings = { "m"_s, "a"_s, "b"_s, "ns"_s, "default"_s, "x"_s, "toml"_s, "d"_s, "c"_s, String { std::span<const char16_t> { utf16Name } }, "z"_s };
    section.flags = 1 | 4;
    section.requests.append(CraftedRequest { 0, 0, false, std::nullopt });
    section.requests.append(CraftedRequest { 0, 0, true, std::nullopt });
    section.requests.append(CraftedRequest { 5, 4, false, 6 });
    constexpr uint32_t defaultName = craftedStringCount + 1;
    section.records.append(CraftedRecord { 0, 0, true, { 0, 1 } }); // import { a } from "m"
    section.records.append(CraftedRecord { 2, 0, false, { 0, 3 } }); // import * as ns from "m"
    section.records.append(CraftedRecord { 7, 0, false, { 0, 7 } }); // import defer * as d from "m"
    section.records.append(CraftedRecord { 0, 4, false, { 5, 4, 2, 5 } }); // import b from "x" with { type: "toml" }
    section.records.append(CraftedRecord { 3, 0, false, { 8, 1, 0 } }); // export { a as c } from "m"
    section.records.append(CraftedRecord { 4, 0, false, { 4, defaultName } }); // export default class { }
    section.records.append(CraftedRecord { 5, 0, false, { 9, 0 } }); // export * as Āe from "m"
    section.records.append(CraftedRecord { 6, 0, false, { 0 } }); // export * from "m"
    section.records.append(CraftedRecord { 4, 0, false, { 10, 2 } }); // export { b as z }
    return section;
}

// The parts of a record the twin comparison reads, to build a copy with one of them changed.
struct RecordContents {
    CodeFeatures features { 0 };
    bool hasTLA { false };
    bool isTypeScript { false };
    Vector<AbstractModuleRecord::ModuleRequest> requests;
    Vector<AbstractModuleRecord::ImportEntry> imports;
    Vector<AbstractModuleRecord::ExportEntry> exports;
    Vector<AbstractModuleRecord::StarExportEntry> stars;
};

static RecordContents contentsOf(const JSModuleRecord& record)
{
    RecordContents contents;
    contents.features = record.features();
    contents.hasTLA = record.hasTLA();
    contents.isTypeScript = record.m_isTypeScript;
    for (const auto& request : record.requestedModules())
        contents.requests.append(request);
    for (const auto& [key, entry] : record.importEntries())
        contents.imports.append(entry);
    for (const auto& [key, entry] : record.exportEntries())
        contents.exports.append(entry);
    for (const auto& star : record.starExportEntries())
        contents.stars.append(star);
    return contents;
}

// A record built through the record API, as Bun's builder builds one.
static JSModuleRecord* recordFrom(JSGlobalObject* globalObject, const SourceCode& source, const RecordContents& contents)
{
    VM& vm = globalObject->vm();
    JSModuleRecord* record = JSModuleRecord::create(globalObject, vm, globalObject->moduleRecordStructure(), globalObject->moduleLoader(), Identifier::fromString(vm, "jitcache-analysis-copy"_s), source, contents.features);
    for (const auto& request : contents.requests)
        record->appendRequestedModule(request.m_specifier, RefPtr { request.m_attributes }, request.m_phase);
    for (const auto& entry : contents.imports)
        record->addImportEntry(entry);
    for (const auto& entry : contents.exports)
        record->addExportEntry(entry);
    for (const auto& [moduleName, type] : contents.stars)
        record->addStarExportEntry(Identifier::fromUid(vm, moduleName.get()), type);
    record->setHasTLA(contents.hasTLA);
    record->m_isTypeScript = contents.isTypeScript;
    return record;
}

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

static String exportedConstants(unsigned count)
{
    StringBuilder builder;
    builder.append("export const "_s);
    for (unsigned i = 0; i < count; ++i)
        builder.append(i ? ", s"_s : "s"_s, i, " = 0"_s);
    builder.append(';');
    return builder.toString();
}

static String exportedLongName(unsigned length)
{
    StringBuilder builder;
    builder.append("export const s"_s);
    for (unsigned i = 1; i < length; ++i)
        builder.append('a');
    builder.append(" = 0;"_s);
    return builder.toString();
}

// The analysis forms of the parse, each in a module of its own.
static constexpr std::array analysisFormTexts {
    "import jitcacheDefault from \"m\";"_s,
    "import { a, b as c } from \"m\";"_s,
    "import * as ns from \"m\";"_s,
    "import \"m\";"_s,
    "import j from \"./j.json\" with { type: \"json\" }; import w from \"./w.wasm\" with { type: \"webassembly\" }; import t from \"x\" with { type: \"text\" }; import u from \"x\" with { type: \"toml\" };"_s,
    "export default 1 + 1;"_s,
    "export default function jitcacheNamed() { }"_s,
    "export default class { }"_s,
    "const v = 1; export { v, v as w };"_s,
    "const v = 1; export { v as \"a b\", v as \"\" };"_s,
    "export { x } from \"m\";"_s,
    "export * from \"m\"; export * from \"m\";"_s,
    "export * as ns from \"m\";"_s,
    "import a from \"m\"; export { a };"_s,
    "import * as ns from \"m\"; export { ns };"_s,
    "export const meta = import.meta.url;"_s,
    "await 0; export const later = 1;"_s,
    "export const \\u0100x = 1;"_s,
    "export const \\u00e9x = 1;"_s,
};

// Each text parses, encodes, validates, and holds exactly its bytes.
static bool formsEncodeAndValidate(TestContext& context, JSGlobalObject* globalObject, std::span<const ASCIILiteral> texts)
{
    for (ASCIILiteral text : texts) {
        auto bytes = encodedText(globalObject, text);
        String failure;
        if (!bytes) {
            JITCACHE_FAIL(makeString("the analysis of \""_s, text, "\" did not encode"_s));
            return false;
        }
        if (!validateModuleAnalysis(bytes->span(), failure)) {
            JITCACHE_FAIL(makeString("the analysis of \""_s, text, "\" fails validation: "_s, failure));
            return false;
        }
        if (!everyPrefixFails(bytes->span())) {
            JITCACHE_FAIL(makeString("a prefix of the analysis of \""_s, text, "\" passes validation"_s));
            return false;
        }
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

} // namespace UCBAnalysisTestsInternal

using namespace UCBAnalysisTestsInternal;

// The layout of a small module, and of one without imports or exports.
JITCACHE_TEST(ucbAnalysisLayout, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    auto layout = encodedText(entered.globalObject(), "import { a } from \"m\"; export const b = a;"_s);
    constexpr std::array<uint8_t, 38> expected {
        0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x06, 0x01, 0x6D, 0x01, 0x61, 0x01, 0x62,
        0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x01, 0x02, 0x02,
    };
    JITCACHE_CHECK(layout && std::ranges::equal(layout->span(), expected));

    constexpr std::array<uint8_t, 22> empty {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    auto noRecords = encodedText(entered.globalObject(), "let jitcacheUA1 = 1;"_s);
    JITCACHE_CHECK(noRecords && std::ranges::equal(noRecords->span(), empty));
    auto awaiting = encodedText(entered.globalObject(), "await 0;"_s);
    JITCACHE_CHECK(awaiting && awaiting->size() == empty.size() && (*awaiting)[10] == 4 && std::ranges::equal(awaiting->span().subspan(11), std::span { empty }.subspan(11)));
}

// Every form the analysis parse produces encodes to bytes that validate, each prefix of which fails; a host-defined
// type equal to a name shares its id; the id and offset widths step where the counts and the blob's length need them.
JITCACHE_TEST(ucbAnalysisForms, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    if (!formsEncodeAndValidate(context, globalObject, analysisFormTexts))
        return;

    // A host-defined type equal to a name: "x" and "toml", nothing else.
    auto shared = encodedText(globalObject, "import { toml } from \"x\" with { type: \"toml\" };"_s);
    JITCACHE_CHECK(shared && analysisLayout(shared->span()).stringCount == 2);

    // UTF-16 for a name with a unit above 0xFF, Latin-1 for one whose units fit a byte.
    auto wide = encodedText(globalObject, "export const \\u0100x = 1;"_s);
    auto latin1 = encodedText(globalObject, "export const \\u00e9x = 1;"_s);
    if (wide) {
        AnalysisLayout layout = analysisLayout(wide->span());
        JITCACHE_CHECK(layout.stringCount == 1 && !(*wide)[layout.stringStart(wide->span(), 0)]);
    } else
        JITCACHE_FAIL("the UTF-16 name did not encode"_s);
    if (latin1) {
        AnalysisLayout layout = analysisLayout(latin1->span());
        size_t start = layout.stringStart(latin1->span(), 0);
        JITCACHE_CHECK(layout.stringCount == 1 && (*latin1)[start] == 1 && (*latin1)[start + 1] == 0xE9);
    } else
        JITCACHE_FAIL("the Latin-1 name did not encode"_s);

    // The id width is the width of n + 1, and the offset width that of the blob's length.
    auto checkWidth = [&](const String& text, bool idWidth, unsigned expected, bool prefixes) {
        auto bytes = encodedText(globalObject, text);
        String failure;
        if (!bytes || !validateModuleAnalysis(bytes->span(), failure)) {
            JITCACHE_FAIL(makeString("a width fixture did not encode or validate: "_s, failure));
            return;
        }
        AnalysisLayout layout = analysisLayout(bytes->span());
        unsigned width = idWidth ? layout.idWidth(bytes->span()) : layout.offsetWidth;
        if (width != expected)
            JITCACHE_FAIL(makeString(idWidth ? "the id width of "_s : "the offset width of "_s, layout.stringCount, " strings in a blob of "_s, layout.blobLength, " bytes is "_s, width, ", not "_s, expected));
        if (prefixes && !everyPrefixFails(bytes->span()))
            JITCACHE_FAIL("a prefix of a width fixture passes validation"_s);
    };
    checkWidth(exportedConstants(254), true, 1, true);
    checkWidth(exportedConstants(255), true, 2, true);
    checkWidth(exportedConstants(65534), true, 2, false);
    checkWidth(exportedConstants(65535), true, 4, false);
    checkWidth(exportedLongName(254), false, 1, true);
    checkWidth(exportedLongName(255), false, 2, true);
    checkWidth(exportedLongName(65534), false, 2, false);
    checkWidth(exportedLongName(65535), false, 4, false);
}

// The same for the deferred forms, which the parser takes only with useImportDefer on.
JITCACHE_TEST_WITH_OPTIONS(ucbAnalysisDeferredForms, Yes, "--useImportDefer=true")
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    constexpr std::array deferred {
        "import defer * as ns from \"m\";"_s,
        "import defer * as ns from \"m\"; export { ns };"_s,
    };
    if (!formsEncodeAndValidate(context, entered.globalObject(), deferred))
        return;
    // One record, a deferred namespace import, and one request in the defer phase.
    auto bytes = encodedText(entered.globalObject(), deferred[0]);
    if (!bytes)
        return;
    AnalysisLayout layout = analysisLayout(bytes->span());
    size_t tags = layout.bodyStart + 12;
    JITCACHE_CHECK(readLittleEndian(bytes->span(), layout.bodyStart + 8, 4) == 1 && (*bytes)[tags] == 7 && ((*bytes)[tags + 1] & 1));
}

// One text encodes to equal bytes in two VMs, and from a provider that holds it as a 16-bit string.
JITCACHE_TEST(ucbAnalysisDeterminism, Yes)
{
    VM& vm = *context.vm();
    String text = "import d, { a as jitcacheUA3a } from \"m\"; import t from \"x\" with { type: \"text\" }; export * from \"n\"; export * as jitcacheUA3n from \"n\";"
        " export { jitcacheUA3a, t as \\u00e9, d as \\u0100 }; export default class { }; export const jitcacheUA3u = import.meta.url; await 0;"_s;
    Vector<char16_t> units;
    for (unsigned i = 0; i < text.length(); ++i)
        units.append(text[i]);
    String sixteen { units.span() };

    EnteredGlobalObject entered { vm };
    auto fromSixteen = encodedText(entered.globalObject(), sixteen);
    auto first = encodedText(entered.globalObject(), text);
    std::optional<Vector<uint8_t>> second;
    {
        ExtraVM other;
        EnteredGlobalObject otherEntered { other.vm() };
        second = encodedText(otherEntered.globalObject(), text);
    }
    JITCACHE_CHECK(!sixteen.is8Bit());
    JITCACHE_CHECK(first && second && fromSixteen);
    if (first && second && fromSixteen) {
        JITCACHE_CHECK(*first == *second);
        JITCACHE_CHECK(*first == *fromSixteen);
    }
}

// The encoder refuses each form the section cannot carry exactly, and a budget that refuses, leaving no charge.
JITCACHE_TEST(ucbAnalysisRefusals, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    SourceCode source = analysisModuleSource("export const jitcacheUA4 = 1;"_s);
    Identifier m = Identifier::fromString(vm, "m"_s);
    Identifier a = Identifier::fromString(vm, "a"_s);
    Identifier e = Identifier::fromString(vm, "e"_s);
    const Identifier& starNamespace = vm.propertyNames->starNamespacePrivateName;
    const Identifier& starDefault = vm.propertyNames->starDefaultPrivateName;
    using Type = ScriptFetchParameters::Type;
    using ImportEntryType = AbstractModuleRecord::ImportEntryType;
    using ModulePhase = AbstractModuleRecord::ModulePhase;
    using ExportEntry = AbstractModuleRecord::ExportEntry;

    auto refuses = [&](ASCIILiteral label, const Function<void(JSModuleRecord&)>& build) {
        Strong<JSModuleRecord> record { vm, recordFrom(globalObject, source, { }) };
        build(*record.get());
        Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
        auto encoded = encodeModuleAnalysis(vm, *record.get(), budget.get());
        if (encoded || encoded.error() != AnalysisEncodeFailure::Unrepresentable || budget->chargedBytes())
            JITCACHE_FAIL(makeString("the encoder did not refuse "_s, label, " as unrepresentable, leaving nothing charged"_s));
    };
    refuses("a requested module of type None"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, ScriptFetchParameters::create(Type::None));
    });
    refuses("a requested module of type Text"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, ScriptFetchParameters::create(Type::Text));
    });
    refuses("a requested module whose attributes have type JavaScript"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, ScriptFetchParameters::create(Type::JavaScript));
    });
    refuses("an entry of type None"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr);
        record.addImportEntry({ ImportEntryType::Single, ModulePhase::Evaluation, Type::None, m, a, a });
    });
    refuses("an entry of type Text"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr);
        record.addExportEntry(ExportEntry::createIndirect(e, a, m, Type::Text));
    });
    refuses("a SingleTypeScript import"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr);
        record.addImportEntry({ ImportEntryType::SingleTypeScript, ModulePhase::Evaluation, Type::JavaScript, m, a, a });
    });
    refuses("a single import in the defer phase"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr, ModulePhase::Defer);
        record.addImportEntry({ ImportEntryType::Single, ModulePhase::Defer, Type::JavaScript, m, a, a });
    });
    refuses("a namespace import of another name than the namespace's"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr);
        record.addImportEntry({ ImportEntryType::Namespace, ModulePhase::Evaluation, Type::JavaScript, m, a, e });
    });
    refuses("an indirect export of the namespace's name"_s, [&](JSModuleRecord& record) {
        record.appendRequestedModule(m, nullptr);
        record.addExportEntry(ExportEntry::createIndirect(e, starNamespace, m, Type::JavaScript));
    });
    refuses("a TypeScript record"_s, [&](JSModuleRecord& record) {
        record.m_isTypeScript = true;
    });
    refuses("a null identifier"_s, [&](JSModuleRecord& record) {
        record.addExportEntry(ExportEntry::createLocal(e, Identifier { }));
    });
    refuses("a symbol as a local name"_s, [&](JSModuleRecord& record) {
        record.addExportEntry(ExportEntry::createLocal(e, starNamespace));
    });
    refuses("a symbol as an export name"_s, [&](JSModuleRecord& record) {
        record.addExportEntry(ExportEntry::createLocal(starDefault, a));
    });

    // A budget that refuses the first table charge, and one that refuses the section's charge after the tables'.
    Strong<JSModuleRecord> record { vm, analyzeModule(globalObject, analysisModuleSource("import { a } from \"m\"; export const b = a;"_s)) };
    if (!record) {
        JITCACHE_FAIL("the budget fixture did not parse"_s);
        return;
    }
    for (size_t limit : { 0, 80 }) {
        Ref<ProducerBudget> budget = ProducerBudget::create(limit);
        auto encoded = encodeModuleAnalysis(vm, *record.get(), budget.get());
        if (encoded || encoded.error() != AnalysisEncodeFailure::BudgetRefused || budget->chargedBytes())
            JITCACHE_FAIL(makeString("a budget of "_s, limit, " bytes did not refuse the encode, leaving nothing charged"_s));
    }
}

// validateModuleAnalysis rejects a crafted section for each of its rules.
JITCACHE_TEST(ucbAnalysisValidation, No)
{
    String failure;
    Vector<uint8_t> base = craftedBytes(craftedBase());
    if (!validateModuleAnalysis(base.span(), failure)) {
        JITCACHE_FAIL(makeString("the crafted base fails validation: "_s, failure));
        return;
    }
    AnalysisLayout layout = analysisLayout(base.span());
    size_t utf16Tag = layout.stringStart(base.span(), craftedUTF16String);
    if (layout.stringCount != craftedStringCount || layout.offsetWidth != 1 || ((utf16Tag - layout.blobStart) & 1)) {
        JITCACHE_FAIL("the crafted base's layout is not the one its cases assume"_s);
        return;
    }
    size_t recordTags = layout.bodyStart + 12;
    size_t requestTags = recordTags + craftedBase().records.size();

    auto rejectsBytes = [&](ASCIILiteral label, const Function<void(Vector<uint8_t>&)>& mutate) {
        Vector<uint8_t> bytes = base;
        mutate(bytes);
        String why;
        if (validateModuleAnalysis(bytes.span(), why))
            JITCACHE_FAIL(makeString("validation accepted "_s, label));
    };
    auto rejectsSection = [&](ASCIILiteral label, const Function<void(CraftedSection&)>& mutate) {
        CraftedSection section = craftedBase();
        mutate(section);
        Vector<uint8_t> bytes = craftedBytes(section);
        String why;
        if (validateModuleAnalysis(bytes.span(), why))
            JITCACHE_FAIL(makeString("validation accepted "_s, label));
    };

    // The string table.
    rejectsBytes("an offset width of 3"_s, [](auto& bytes) { bytes[0] = 3; });
    rejectsBytes("a nonzero byte after the offset width"_s, [](auto& bytes) { bytes[2] = 1; });
    rejectsSection("an offset width wider than the blob's length needs"_s, [](auto& section) { section.offsetWidth = 2; });
    // A section without strings has one offset byte, then its padding byte.
    Vector<uint8_t> empty = craftedBytes(CraftedSection { });
    JITCACHE_CHECK(empty.size() == 22 && validateModuleAnalysis(empty.span(), failure));
    empty[9] = 1;
    JITCACHE_CHECK(!validateModuleAnalysis(empty.span(), failure));
    rejectsBytes("decreasing offsets"_s, [](auto& bytes) { std::swap(bytes[9], bytes[10]); });
    rejectsBytes("a first offset past the blob's start"_s, [](auto& bytes) { bytes[8] = 1; });
    rejectsBytes("a blob length past the section's end"_s, [&](auto& bytes) { bytes[8 + craftedStringCount] = 0xFF; });
    rejectsBytes("a string tag of 2"_s, [&](auto& bytes) { bytes[layout.stringStart(base.span(), 0)] = 2; });
    rejectsBytes("a nonzero UTF-16 padding byte"_s, [&](auto& bytes) { bytes[utf16Tag + 1] = 1; });
    rejectsBytes("a UTF-16 string of an odd number of bytes"_s, [&](auto& bytes) { --bytes[8 + craftedUTF16String + 1]; });
    rejectsBytes("a UTF-16 string with no unit above 0xFF"_s, [&](auto& bytes) { bytes[utf16Tag + 3] = 0; });
    rejectsBytes("two equal strings"_s, [&](auto& bytes) { bytes[layout.stringStart(base.span(), 8) + 1] = 'd'; });

    // The body's header.
    rejectsBytes("the TypeScript flag"_s, [&](auto& bytes) { bytes[layout.bodyStart] |= 2; });
    rejectsBytes("an unknown flag"_s, [&](auto& bytes) { bytes[layout.bodyStart] |= 8; });
    rejectsBytes("an id width of 2 for 12 ids"_s, [&](auto& bytes) { bytes[layout.bodyStart + 1] = 2; });
    rejectsBytes("a nonzero byte after the id width"_s, [&](auto& bytes) { bytes[layout.bodyStart + 3] = 1; });
    rejectsBytes("more tags than the section holds"_s, [&](auto& bytes) { bytes[layout.bodyStart + 11] = 0x7F; });
    rejectsBytes("a record count one below the records written"_s, [&](auto& bytes) { --bytes[layout.bodyStart + 8]; });

    // The tags.
    rejectsSection("a record of kind 1"_s, [](auto& section) { section.records[0].kind = 1; });
    rejectsBytes("a record tag with bit 7"_s, [&](auto& bytes) { bytes[recordTags] |= 0x80; });
    rejectsSection("the same-name bit on a namespace import"_s, [](auto& section) { section.records[1].sameName = true; });
    rejectsSection("a record of fetch kind 1"_s, [](auto& section) { section.records[1].fetch = 1; });
    rejectsSection("a record of fetch kind 5"_s, [](auto& section) { section.records[1].fetch = 5; });
    rejectsSection("a local export with a fetch kind"_s, [](auto& section) { section.records[5].fetch = 3; });
    rejectsSection("a requested module of fetch kind 1"_s, [](auto& section) { section.requests[0].fetch = 1; });
    rejectsBytes("a requested module tag with bit 4"_s, [&](auto& bytes) { bytes[requestTags] |= 0x10; });

    // The ids.
    rejectsSection("an id past n + 1"_s, [](auto& section) { section.records[0].ids[0] = craftedStringCount + 2; });
    rejectsSection("n + 1 as an import name"_s, [](auto& section) { section.records[0].ids[1] = craftedStringCount + 1; });
    rejectsSection("n as an export name"_s, [](auto& section) { section.records[4].ids[0] = craftedStringCount; });
    rejectsSection("n as a local export's local name"_s, [](auto& section) { section.records[5].ids[1] = craftedStringCount; });
    rejectsSection("a host id that names no string"_s, [](auto& section) { section.requests[2].host = craftedStringCount + 1; });
    rejectsBytes("an id after the last record's"_s, [](auto& bytes) { bytes.append(0); });
    rejectsSection("a single import missing its local name"_s, [](auto& section) { section.records[3].ids = { 5, 4 }; });

    // Duplicates.
    rejectsSection("two requested modules of one specifier, type and phase"_s, [](auto& section) { section.requests.append(CraftedRequest { 0, 0, false, std::nullopt }); });
    rejectsSection("two import entries of one local name"_s, [](auto& section) { section.records[1].ids[1] = 1; });
    rejectsSection("two export entries of one export name"_s, [](auto& section) { section.records[8].ids[0] = 8; });
    rejectsSection("two star exports of one module and type"_s, [](auto& section) { section.records.append(CraftedRecord { 6, 0, false, { 0 } }); });

    // References to the requested modules.
    rejectsSection("an import entry of a module never requested"_s, [](auto& section) { section.records[0].ids[0] = 1; });
    rejectsSection("a deferred import of a module requested only for evaluation"_s, [](auto& section) { section.records[2].ids[0] = 5; section.records[2].fetch = 4; section.records[2].ids.append(5); });
    rejectsSection("an indirect export of a type never requested"_s, [](auto& section) { section.records[4].fetch = 3; });
    rejectsSection("a namespace export of a module never requested"_s, [](auto& section) { section.records[6].ids[1] = 2; });
    rejectsSection("a star export of a module never requested"_s, [](auto& section) { section.records[7].ids[0] = 3; });
}

// The twin comparison names each field changed alone on a copy of a record, and nothing for two parses of one text
// or for records whose features differ only in a bit nothing reads.
JITCACHE_TEST(ucbAnalysisTwinDifference, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    SourceCode source = analysisModuleSource("import d, { a, b as c } from \"m\"; import * as ns from \"n\"; import j from \"./j.json\" with { type: \"json\" };"
        " import t from \"./t.txt\" with { type: \"text\" }; export { a, c as e }; export { x as y } from \"o\"; export * as z from \"p\"; export * from \"q\";"
        " export default 1; export const f = import.meta.url + d + ns + j + t; await 0;"_s);
    Strong<JSModuleRecord> parsed { vm, analyzeModule(globalObject, source) };
    Strong<JSModuleRecord> again { vm, analyzeModule(globalObject, source) };
    if (!parsed || !again) {
        JITCACHE_FAIL("the twin fixture did not parse"_s);
        return;
    }
    JITCACHE_CHECK(firstModuleAnalysisDifference(*parsed.get(), *again.get()).isNull());
    RecordContents contents = contentsOf(*parsed.get());
    auto copyWithFeatures = contents;
    copyWithFeatures.features |= ArrowFunctionFeature;
    Strong<JSModuleRecord> otherBit { vm, recordFrom(globalObject, source, copyWithFeatures) };
    JITCACHE_CHECK(firstModuleAnalysisDifference(*otherBit.get(), *parsed.get()).isNull());

    // The entries each case changes, found by form.
    auto requestIndex = [&](const Function<bool(const AbstractModuleRecord::ModuleRequest&)>& matches) -> std::optional<size_t> {
        for (size_t i = 0; i < contents.requests.size(); ++i) {
            if (matches(contents.requests[i]))
                return i;
        }
        return std::nullopt;
    };
    auto withJSON = requestIndex([](const auto& request) { return request.m_attributes && request.m_attributes->type() == ScriptFetchParameters::Type::JSON; });
    auto withHostType = requestIndex([](const auto& request) { return request.m_attributes && request.m_attributes->type() == ScriptFetchParameters::Type::HostDefined; });
    auto plain = requestIndex([](const auto& request) { return !request.m_attributes; });
    auto exportIndex = [&](AbstractModuleRecord::ExportEntry::Type type, bool needsLocalName) -> std::optional<size_t> {
        for (size_t i = 0; i < contents.exports.size(); ++i) {
            if (contents.exports[i].type == type && (!needsLocalName || !contents.exports[i].localName.isNull()))
                return i;
        }
        return std::nullopt;
    };
    auto indirect = exportIndex(AbstractModuleRecord::ExportEntry::Type::Indirect, false);
    auto local = exportIndex(AbstractModuleRecord::ExportEntry::Type::Local, true);
    if (!withJSON || !withHostType || !plain || !indirect || !local || contents.imports.size() < 2 || contents.stars.isEmpty()) {
        JITCACHE_FAIL("the twin fixture lacks a form a case changes"_s);
        return;
    }
    Identifier other = Identifier::fromString(vm, "jitcacheUA6Other"_s);

    auto names = [&](ASCIILiteral fragment, const Function<void(RecordContents&)>& change) {
        RecordContents changed = contents;
        change(changed);
        Strong<JSModuleRecord> copy { vm, recordFrom(globalObject, source, changed) };
        String difference = firstModuleAnalysisDifference(*copy.get(), *parsed.get());
        if (difference.isNull() || !difference.contains(fragment))
            JITCACHE_FAIL(makeString("the twin comparison did not name "_s, fragment, ": "_s, difference.isNull() ? String { "nothing"_s } : difference));
    };
    names("requested modules"_s, [](auto& changed) { changed.requests.removeLast(); });
    names("specifier"_s, [&](auto& changed) { changed.requests[*plain].m_specifier = other; });
    names("another phase"_s, [&](auto& changed) { changed.requests[*plain].m_phase = AbstractModuleRecord::ModulePhase::Defer; });
    names("attributes"_s, [&](auto& changed) { changed.requests[*plain].m_attributes = ScriptFetchParameters::create(ScriptFetchParameters::Type::JSON); });
    names("has type"_s, [&](auto& changed) { changed.requests[*withJSON].m_attributes = ScriptFetchParameters::create(ScriptFetchParameters::Type::WebAssembly); });
    names("host-defined type"_s, [&](auto& changed) { changed.requests[*withHostType].m_attributes = ScriptFetchParameters::create("jitcache-ua6"_s); });
    names("import entries"_s, [](auto& changed) { changed.imports.removeLast(); });
    names("import entry 0 has key"_s, [&](auto& changed) { changed.imports[0].localName = other; });
    names("import entry 0 has another type"_s, [](auto& changed) { changed.imports[0].type = AbstractModuleRecord::ImportEntryType::Namespace; });
    names("import entry 0 has another phase"_s, [](auto& changed) { changed.imports[0].phase = AbstractModuleRecord::ModulePhase::Defer; });
    names("import entry 0 has another module request type"_s, [](auto& changed) { changed.imports[0].moduleRequestType = ScriptFetchParameters::Type::WebAssembly; });
    names("import entry 0 requests"_s, [&](auto& changed) { changed.imports[0].moduleRequest = other; });
    names("import entry 0 imports"_s, [&](auto& changed) { changed.imports[0].importName = other; });
    names("export entries"_s, [](auto& changed) { changed.exports.removeLast(); });
    names("has key"_s, [&](auto& changed) { changed.exports[*indirect].exportName = other; });
    names("has another type"_s, [&](auto& changed) { changed.exports[*indirect].type = AbstractModuleRecord::ExportEntry::Type::Namespace; });
    names("has another module request type"_s, [&](auto& changed) { changed.exports[*indirect].moduleRequestType = ScriptFetchParameters::Type::JSON; });
    names("has module name"_s, [&](auto& changed) { changed.exports[*indirect].moduleName = other; });
    names("has import name"_s, [&](auto& changed) { changed.exports[*indirect].importName = other; });
    names("has local name"_s, [&](auto& changed) { changed.exports[*local].localName = other; });
    names("star exports"_s, [](auto& changed) { changed.stars.removeLast(); });
    names("star export 0 names"_s, [&](auto& changed) { changed.stars[0].first = other.impl(); });
    names("star export 0 has another type"_s, [](auto& changed) { changed.stars[0].second = ScriptFetchParameters::Type::JSON; });
    names("import.meta"_s, [](auto& changed) { changed.features ^= ImportMetaFeature; });
    names("top-level-await"_s, [](auto& changed) { changed.hasTLA = !changed.hasTLA; });
    names("TypeScript"_s, [](auto& changed) { changed.isTypeScript = !changed.isTypeScript; });
}

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
    if (!directory || !startJITCache(context, vm, *directory, Role::Producer, true))
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
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer, true))
        return;
    EnteredGlobalObject entered { vm };
    didAttachDebugger(vm);
    JITCACHE_CHECK(!importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, nullptr));
}

// A Consumer with strict on and a bench report: the key the import reads is the one the module's request records; a
// key without a body, a body without the section, a body with one, a supplied digest held to the text, a damaged body
// and, last, a malformed section, each with its event.
JITCACHE_TEST(ucbAnalysisImportInAStrictConsumer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory)
        return;
    String benchPath = directory->path("analysis.bench"_s);
    if (!startJITCache(context, vm, *directory, Role::Consumer, true, { }, benchPath))
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

    // A supplied digest, which strict holds to the text with one hash.
    before = statistics;
    auto supplied = importModuleAnalysis(globalObject, text, &digest);
    JITCACHE_CHECK(supplied && supplied->moduleInfo == *section);
    JITCACHE_CHECK(statistics.suppliedDigestVerifications == before.suppliedDigestVerifications + 1 && statistics.sourceDigests == before.sourceDigests);

    // A body the integrator found damaged.
    stub.serveUnusable();
    JITCACHE_CHECK(!importModuleAnalysis(globalObject, text, nullptr));

    // A malformed section, the TypeScript flag set, is invalid material.
    Vector<uint8_t> malformed = *section;
    malformed[analysisLayout(malformed.span()).bodyStart] |= 2;
    stub.serveAny(WTF::move(malformed));
    before = statistics;
    JITCACHE_CHECK(!importModuleAnalysis(globalObject, text, nullptr));
    JITCACHE_CHECK(statistics.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::Analysis)] == before.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::Analysis)] + 1);
    auto fault = JITCache::status(vm).firstFault;
    JITCACHE_CHECK(fault && fault->stepName() == "ucb.analysis"_s);

    Vector<String> outcomes = moduleAnalysisOutcomes(vm, benchPath);
    if (!sameStrings(outcomes, { "miss"_s, "absent"_s, "hit"_s, "hit"_s, "invalid"_s, "invalid"_s }))
        JITCACHE_FAIL(makeString("the bench report holds "_s, outcomes.size(), " module-analysis events, not the six expected in order"_s));
    executable.clear();
    checkRegistry(context, vm);
}

// A Consumer with strict on: a supplied digest that differs from the text is invalid material before the key, so it
// records no event.
JITCACHE_TEST(ucbAnalysisImportOfAWrongSuppliedDigest, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory)
        return;
    String benchPath = directory->path("analysis.bench"_s);
    if (!startJITCache(context, vm, *directory, Role::Consumer, true, { }, benchPath))
        return;
    EnteredGlobalObject entered { vm };
    VMState& state = *vm.jitCacheState();
    UCBStatistics& statistics = state.registry().statistics();
    AnalysisLookupStub stub { state };

    Digest256 wrong = filledDigest(0x5a);
    UCBStatistics before = statistics;
    JITCACHE_CHECK(!importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, &wrong));
    JITCACHE_CHECK(statistics.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::SuppliedDigest)] == before.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::SuppliedDigest)] + 1);
    JITCACHE_CHECK(stub.keysRead().isEmpty());
    auto fault = JITCache::status(vm).firstFault;
    JITCACHE_CHECK(fault && fault->stepName() == "ucb.supplied-digest"_s);
    JITCACHE_CHECK(moduleAnalysisOutcomes(vm, benchPath).isEmpty());
}

// A Consumer with strict off, on purpose: every run the test runner starts has strict on, and only normal mode takes a
// supplied digest without hashing the text.
JITCACHE_TEST(ucbAnalysisImportInANonStrictConsumer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer, false))
        return;
    EnteredGlobalObject entered { vm };
    VMState& state = *vm.jitCacheState();
    UCBStatistics& statistics = state.registry().statistics();
    AnalysisLookupStub stub { state };

    Digest256 supplied = filledDigest(0x42);
    UCBStatistics before = statistics;
    auto result = importModuleAnalysis(*entered.globalObject(), "export const jitcacheUA7 = 1;"_s, &supplied);
    JITCACHE_CHECK(result && result->sourceDigest == supplied && result->moduleInfo.isEmpty());
    JITCACHE_CHECK(statistics.sourceDigests == before.sourceDigests && statistics.suppliedDigestVerifications == before.suppliedDigestVerifications);
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
    if (!startJITCache(context, vm, *directory, Role::Consumer, true, { }, benchPath))
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
// charge on a damaged or, with strict on, malformed saved section, saying what the check found in the malformed one; and
// the source goes with the UCB's record.
JITCACHE_TEST(ucbAnalysisSourcesInAProducer, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-analysis"_s);
    if (!directory || !startJITCache(context, vm, *directory, Role::Producer, true))
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

    // Without a live source: the saved body's section, none, a damaged body and a malformed section.
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
        JITCACHE_CHECK(!unusable && unusable.error().failure == UCBCaptureFailure::SavedAnalysisUnusable && !budget->chargedBytes());
    }
    {
        Vector<uint8_t> malformed = saved;
        if (!malformed.isEmpty())
            malformed[analysisLayout(malformed.span()).bodyStart] |= 2;
        // The failure carries what the strict check finds wrong with these bytes.
        String reason;
        bool rejected = !validateModuleAnalysis(malformed.span(), reason);
        stub.serveAny(WTF::move(malformed));
        Ref<ProducerBudget> budget = ProducerBudget::create(64 * MB);
        UCBStatistics before = statistics;
        auto invalid = buildWithoutSource(budget);
        JITCACHE_CHECK(!invalid && invalid.error().failure == UCBCaptureFailure::SavedAnalysisInvalid && !budget->chargedBytes());
        JITCACHE_CHECK(rejected && !invalid && invalid.error().detail == reason);
        JITCACHE_CHECK(statistics.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::Analysis)] == before.invalidMaterial[static_cast<size_t>(InvalidMaterialStep::Analysis)] + 1);
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
    if (!directory || !startJITCache(context, vm, *directory, Role::Consumer, true))
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
    if (!directory || !startJITCache(context, vm, *directory, Role::ConsumerProducer, true))
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
