#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "AbstractModuleRecord.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleLoader.h"
#include "JSModuleRecord.h"
#include "ScriptFetchParameters.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestVM.h"
#include "jitcache/tests/ucb/UCBAnalysisTestSupport.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/ucb/UCBAnalysis.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The tests of a module's analysis section: the bytes the encoder writes from a record the analysis parse built, the forms
// they carry and their widths, their determinism, the records the encoder refuses, and the twin comparison.

namespace JSC::JITCache::Tests {

using namespace UCBAnalysisTestSupport;

namespace UCBAnalysisEncodingTestsInternal {

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

// Each text parses and encodes.
static bool formsEncode(TestContext& context, JSGlobalObject* globalObject, std::span<const ASCIILiteral> texts)
{
    for (ASCIILiteral text : texts) {
        if (!encodedText(globalObject, text)) {
            JITCACHE_FAIL(makeString("the analysis of \""_s, text, "\" did not encode"_s));
            return false;
        }
    }
    return true;
}

} // namespace UCBAnalysisEncodingTestsInternal

using namespace UCBAnalysisEncodingTestsInternal;

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

// Every form the analysis parse produces encodes; a host-defined type equal to a name shares its id; the id and offset
// widths step where the counts and the blob's length need them.
JITCACHE_TEST(ucbAnalysisForms, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    JSGlobalObject* globalObject = entered.globalObject();
    if (!formsEncode(context, globalObject, analysisFormTexts))
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
    auto checkWidth = [&](const String& text, bool idWidth, unsigned expected) {
        auto bytes = encodedText(globalObject, text);
        if (!bytes) {
            JITCACHE_FAIL("a width fixture did not encode"_s);
            return;
        }
        AnalysisLayout layout = analysisLayout(bytes->span());
        unsigned width = idWidth ? layout.idWidth(bytes->span()) : layout.offsetWidth;
        if (width != expected)
            JITCACHE_FAIL(makeString(idWidth ? "the id width of "_s : "the offset width of "_s, layout.stringCount, " strings in a blob of "_s, layout.blobLength, " bytes is "_s, width, ", not "_s, expected));
    };
    checkWidth(exportedConstants(254), true, 1);
    checkWidth(exportedConstants(255), true, 2);
    checkWidth(exportedConstants(65534), true, 2);
    checkWidth(exportedConstants(65535), true, 4);
    checkWidth(exportedLongName(254), false, 1);
    checkWidth(exportedLongName(255), false, 2);
    checkWidth(exportedLongName(65534), false, 2);
    checkWidth(exportedLongName(65535), false, 4);
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
    if (!formsEncode(context, entered.globalObject(), deferred))
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

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
