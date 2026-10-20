#include "config.h"
#include "UCBAnalysis.h"

#include "AbstractModuleRecord.h"
#include "BodyKey.h"
#include "JITCacheBench.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "JSModuleRecord.h"
#include "Options.h"
#include "ProducerBudget.h"
#include "ScriptFetchParameters.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <compare>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <wtf/HashSet.h>
#include <wtf/HashTraits.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// A module's analysis in Bun's self-contained module-info form, the bytes Bun's ModuleInfoDeserialized::create reads: a
// string table, then a body. Integers are little-endian, and a width is the smallest of 1, 2 and 4 bytes that holds a value.
//
// The string table: the width W of the blob's length, three zero bytes, the string count n, the n + 1 offsets of W bytes
// (each string's start in the blob, then the blob's length), a zero byte when the offsets take an odd number of bytes, and
// the blob. A string whose units all fit in a byte is the tag 1 and its Latin-1 bytes; any other is the tag 0, a zero byte
// when the next blob offset is odd, and its units as UTF-16LE, so every UTF-16 string starts at an even offset.
//
// The body: the flags (import.meta, TypeScript, top-level await), the width V of n + 1, two zero bytes, the requested
// module count q and the record count r; a tag per record (its kind, its fetch kind shifted left by 3 and its same-name
// bit shifted left by 6), then a tag per requested module (its phase and its fetch kind shifted left by 1); then ids of V
// bytes: per requested module its specifier, and its host-defined type when it has one, then per record the ids its kind
// lists and, for a host-defined type, the entry's module name. An id below n names a string; n names the namespace
// import's private name and n + 1 the anonymous default export's.

namespace JSC::JITCache {

namespace UCBAnalysisInternal {

// The record kinds Bun's module-info reader knows. The analysis parse never produces a SingleTypeScriptImport, which only
// Bun's printer writes.
enum class AnalysisRecordKind : uint8_t {
    SingleImport = 0,
    SingleTypeScriptImport = 1,
    NamespaceImport = 2,
    IndirectExport = 3,
    LocalExport = 4,
    NamespaceExport = 5,
    StarExport = 6,
    DeferredNamespaceImport = 7,
};

// A requested module's fetch kind None has no attributes, and an entry's has the type JavaScript; HostDefined is
// followed by the id of a string. Kind JavaScript is a requested module whose attributes name that type, which the
// analysis parse never produces.
enum class AnalysisFetchKind : uint8_t { None = 0, JavaScript = 1, WebAssembly = 2, JSON = 3, HostDefined = 4 };

constexpr uint8_t analysisImportMetaFlag = 1 << 0;
constexpr uint8_t analysisTopLevelAwaitFlag = 1 << 2;
constexpr uint8_t analysisSameNameBit = 1 << 6;
constexpr size_t analysisTableHeaderSize = 8;
constexpr size_t analysisBodyHeaderSize = 12;

static unsigned analysisWidth(uint64_t value)
{
    if (value <= std::numeric_limits<uint8_t>::max())
        return 1;
    if (value <= std::numeric_limits<uint16_t>::max())
        return 2;
    return 4;
}

static void writeAnalysisInteger(std::span<uint8_t> out, uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}

static uint32_t readAnalysisInteger(std::span<const uint8_t> bytes, unsigned width)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
    return value;
}

// What a slot may hold: a name that exists and is no symbol.
static bool analysisWritableName(const StringImpl* name)
{
    return name && !name->isSymbol();
}

static bool analysisFitsLatin1(const StringImpl& string)
{
    if (string.is8Bit())
        return true;
    return std::ranges::all_of(string.span16(), [](char16_t unit) {
        return unit <= 0xFF;
    });
}

// The bytes a string takes in the blob when it starts at `offset`.
static uint64_t analysisStringSize(const StringImpl& string, uint64_t offset)
{
    if (analysisFitsLatin1(string))
        return 1 + string.length();
    return 1 + ((offset + 1) & 1) + 2 * static_cast<uint64_t>(string.length());
}

// Writes the string at the start of `out`, which begins at `offset` in the blob, and returns the bytes it wrote.
static size_t writeAnalysisString(std::span<uint8_t> out, const StringImpl& string, uint64_t offset)
{
    if (analysisFitsLatin1(string)) {
        out[0] = 1;
        size_t at = 1;
        if (string.is8Bit()) {
            for (Latin1Character unit : string.span8())
                out[at++] = unit;
        } else {
            for (char16_t unit : string.span16())
                out[at++] = static_cast<uint8_t>(unit);
        }
        return at;
    }
    out[0] = 0;
    size_t at = 1;
    if ((offset + 1) & 1)
        out[at++] = 0;
    for (char16_t unit : string.span16()) {
        out[at++] = static_cast<uint8_t>(unit);
        out[at++] = static_cast<uint8_t>(unit >> 8);
    }
    return at;
}

// A requested module's attributes as a fetch kind; none for a type the section cannot carry exactly.
static std::optional<AnalysisFetchKind> requestedFetchKind(const ScriptFetchParameters* attributes)
{
    if (!attributes)
        return AnalysisFetchKind::None;
    switch (attributes->type()) {
    case ScriptFetchParameters::Type::WebAssembly:
        return AnalysisFetchKind::WebAssembly;
    case ScriptFetchParameters::Type::JSON:
        return AnalysisFetchKind::JSON;
#if USE(BUN_JSC_ADDITIONS)
    case ScriptFetchParameters::Type::HostDefined:
        return AnalysisFetchKind::HostDefined;
#endif
    case ScriptFetchParameters::Type::None:
    case ScriptFetchParameters::Type::JavaScript:
    case ScriptFetchParameters::Type::Text:
        break;
    }
    return std::nullopt;
}

// An entry's type as a fetch kind, None for JavaScript; none for a type the section cannot carry exactly.
static std::optional<AnalysisFetchKind> entryFetchKind(ScriptFetchParameters::Type type)
{
    switch (type) {
    case ScriptFetchParameters::Type::JavaScript:
        return AnalysisFetchKind::None;
    case ScriptFetchParameters::Type::WebAssembly:
        return AnalysisFetchKind::WebAssembly;
    case ScriptFetchParameters::Type::JSON:
        return AnalysisFetchKind::JSON;
#if USE(BUN_JSC_ADDITIONS)
    case ScriptFetchParameters::Type::HostDefined:
        return AnalysisFetchKind::HostDefined;
#endif
    case ScriptFetchParameters::Type::None:
    case ScriptFetchParameters::Type::Text:
        break;
    }
    return std::nullopt;
}

// Walks a record's analysis in the order the body writes it: the requested modules, then as records the import entries,
// the export entries and the star exports, each in the record's order. For each it gives the visitor the tag's parts,
// then its names in slot order: a string, or the default export's private name. False, before visiting anything, when
// the record holds a form the section cannot carry exactly; the analysis parse produces none. The walk reads the record's
// containers and allocates nothing, so every walk of one record visits the same sequence.
template<typename Visitor>
static bool walkAnalysis(VM& vm, const JSModuleRecord& record, Visitor& visitor)
{
    const StringImpl* starNamespace = vm.propertyNames->starNamespacePrivateName.impl();
    const StringImpl* starDefault = vm.propertyNames->starDefaultPrivateName.impl();

    // Whether the section can carry the record exactly, checked whole before the first visit.
    auto representable = [&] {
#if USE(BUN_JSC_ADDITIONS)
        if (record.m_isTypeScript)
            return false;
#endif
        for (const auto& request : record.requestedModules()) {
            std::optional<AnalysisFetchKind> fetch = requestedFetchKind(request.m_attributes.get());
            if (!fetch || !analysisWritableName(request.m_specifier.impl()))
                return false;
#if USE(BUN_JSC_ADDITIONS)
            if (*fetch == AnalysisFetchKind::HostDefined && !analysisWritableName(request.m_attributes->hostDefinedImportType().impl()))
                return false;
#endif
        }
        for (const auto& [key, entry] : record.importEntries()) {
            if (!entryFetchKind(entry.moduleRequestType) || !analysisWritableName(entry.moduleRequest.impl()) || !analysisWritableName(entry.localName.impl()))
                return false;
            switch (entry.type) {
            case AbstractModuleRecord::ImportEntryType::Single:
                if (entry.phase != AbstractModuleRecord::ModulePhase::Evaluation || !analysisWritableName(entry.importName.impl()))
                    return false;
                break;
#if USE(BUN_JSC_ADDITIONS)
            case AbstractModuleRecord::ImportEntryType::SingleTypeScript:
                return false;
#endif
            case AbstractModuleRecord::ImportEntryType::Namespace:
                if (entry.importName.impl() != starNamespace)
                    return false;
                break;
            }
        }
        for (const auto& [key, entry] : record.exportEntries()) {
            if (!analysisWritableName(entry.exportName.impl()))
                return false;
            switch (entry.type) {
            case AbstractModuleRecord::ExportEntry::Type::Local:
                if (entry.localName.impl() != starDefault && !analysisWritableName(entry.localName.impl()))
                    return false;
                break;
            case AbstractModuleRecord::ExportEntry::Type::Indirect:
                // A symbol import name, the namespace import's private name included, is refused here.
                if (!entryFetchKind(entry.moduleRequestType) || !analysisWritableName(entry.importName.impl()) || !analysisWritableName(entry.moduleName.impl()))
                    return false;
                break;
            case AbstractModuleRecord::ExportEntry::Type::Namespace:
                if (!entryFetchKind(entry.moduleRequestType) || !analysisWritableName(entry.moduleName.impl()))
                    return false;
                break;
            }
        }
        for (const auto& [moduleName, type] : record.starExportEntries()) {
            if (!entryFetchKind(type) || !analysisWritableName(moduleName.get()))
                return false;
        }
        return true;
    };
    if (!representable())
        return false;

    for (const auto& request : record.requestedModules()) {
        AnalysisFetchKind fetch = *requestedFetchKind(request.m_attributes.get());
        visitor.requestedModule(request.m_phase == AbstractModuleRecord::ModulePhase::Defer, fetch);
        visitor.name(*request.m_specifier.impl());
#if USE(BUN_JSC_ADDITIONS)
        if (fetch == AnalysisFetchKind::HostDefined)
            visitor.name(*request.m_attributes->hostDefinedImportType().impl());
#endif
    }
    for (const auto& [key, entry] : record.importEntries()) {
        AnalysisFetchKind fetch = *entryFetchKind(entry.moduleRequestType);
        const StringImpl& moduleName = *entry.moduleRequest.impl();
        if (entry.type == AbstractModuleRecord::ImportEntryType::Single) {
            const StringImpl& importName = *entry.importName.impl();
            bool sameName = WTF::equal(entry.localName.impl(), &importName);
            visitor.record(AnalysisRecordKind::SingleImport, fetch, sameName);
            visitor.name(moduleName);
            visitor.name(importName);
            if (!sameName)
                visitor.name(*entry.localName.impl());
        } else {
            bool deferred = entry.phase == AbstractModuleRecord::ModulePhase::Defer;
            visitor.record(deferred ? AnalysisRecordKind::DeferredNamespaceImport : AnalysisRecordKind::NamespaceImport, fetch, false);
            visitor.name(moduleName);
            visitor.name(*entry.localName.impl());
        }
        // An entry keeps only the type HostDefined, so its host id repeats its module name, which Bun ignores.
        if (fetch == AnalysisFetchKind::HostDefined)
            visitor.name(moduleName);
    }
    for (const auto& [key, entry] : record.exportEntries()) {
        switch (entry.type) {
        case AbstractModuleRecord::ExportEntry::Type::Local:
            visitor.record(AnalysisRecordKind::LocalExport, AnalysisFetchKind::None, false);
            visitor.name(*entry.exportName.impl());
            if (entry.localName.impl() == starDefault)
                visitor.defaultExportName();
            else
                visitor.name(*entry.localName.impl());
            continue;
        case AbstractModuleRecord::ExportEntry::Type::Indirect: {
            AnalysisFetchKind fetch = *entryFetchKind(entry.moduleRequestType);
            visitor.record(AnalysisRecordKind::IndirectExport, fetch, false);
            visitor.name(*entry.exportName.impl());
            visitor.name(*entry.importName.impl());
            visitor.name(*entry.moduleName.impl());
            if (fetch == AnalysisFetchKind::HostDefined)
                visitor.name(*entry.moduleName.impl());
            continue;
        }
        case AbstractModuleRecord::ExportEntry::Type::Namespace: {
            AnalysisFetchKind fetch = *entryFetchKind(entry.moduleRequestType);
            visitor.record(AnalysisRecordKind::NamespaceExport, fetch, false);
            visitor.name(*entry.exportName.impl());
            visitor.name(*entry.moduleName.impl());
            if (fetch == AnalysisFetchKind::HostDefined)
                visitor.name(*entry.moduleName.impl());
            continue;
        }
        }
    }
    for (const auto& [moduleName, type] : record.starExportEntries()) {
        AnalysisFetchKind fetch = *entryFetchKind(type);
        visitor.record(AnalysisRecordKind::StarExport, fetch, false);
        visitor.name(*moduleName);
        if (fetch == AnalysisFetchKind::HostDefined)
            visitor.name(*moduleName);
    }
    return true;
}

// The first walk: the counts the sizes need.
struct AnalysisCounter {
    void requestedModule(bool, AnalysisFetchKind) { ++requestedModules; }
    void record(AnalysisRecordKind, AnalysisFetchKind, bool) { ++records; }
    void name(const StringImpl&)
    {
        ++names;
        ++ids;
    }
    void defaultExportName() { ++ids; }

    uint64_t requestedModules { 0 };
    uint64_t records { 0 };
    uint64_t names { 0 }; // the slots that hold a string
    uint64_t ids { 0 }; // every slot, the default export's private name included
};

// The second walk: each string slot's name, in the order the body writes the slots.
struct AnalysisNameCollector {
    void requestedModule(bool, AnalysisFetchKind) { }
    void record(AnalysisRecordKind, AnalysisFetchKind, bool) { }
    void name(const StringImpl& name) { names[next++] = &name; }
    void defaultExportName() { }

    std::span<const StringImpl*> names;
    size_t next { 0 };
};

// The third walk: the body's tags and ids.
struct AnalysisBodyWriter {
    void requestedModule(bool deferred, AnalysisFetchKind fetch)
    {
        requestedTags[nextRequestedModule++] = static_cast<uint8_t>((deferred ? 1 : 0) | (static_cast<unsigned>(fetch) << 1));
    }
    void record(AnalysisRecordKind kind, AnalysisFetchKind fetch, bool sameName)
    {
        recordTags[nextRecord++] = static_cast<uint8_t>(static_cast<unsigned>(kind) | (static_cast<unsigned>(fetch) << 3) | (sameName ? analysisSameNameBit : 0));
    }
    void name(const StringImpl&) { writeId(nameIds[nextName++]); }
    void defaultExportName() { writeId(defaultExportId); }

    void writeId(uint32_t id)
    {
        writeAnalysisInteger(ids.subspan(nextIdByte, idWidth), id, idWidth);
        nextIdByte += idWidth;
    }

    std::span<uint8_t> recordTags;
    std::span<uint8_t> requestedTags;
    std::span<uint8_t> ids;
    std::span<const uint32_t> nameIds; // each string slot's id
    uint32_t defaultExportId;
    unsigned idWidth;
    size_t nextRecord { 0 };
    size_t nextRequestedModule { 0 };
    size_t nextName { 0 };
    size_t nextIdByte { 0 };
};

// The bytes one encode holds charged. What it has not handed off goes back to the budget when it goes; declared before
// the buffers it pays for, so they are freed before it releases.
class AnalysisEncodeCharges {
    WTF_MAKE_NONCOPYABLE(AnalysisEncodeCharges);

public:
    explicit AnalysisEncodeCharges(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~AnalysisEncodeCharges()
    {
        if (m_bytes)
            m_budget.release(m_bytes);
    }

    [[nodiscard]] bool tryCharge(size_t bytes)
    {
        if (!m_budget.tryCharge(bytes))
            return false;
        m_bytes += bytes;
        return true;
    }

    // After freeing a buffer that does not travel in the section.
    void release(size_t bytes)
    {
        ASSERT(bytes <= m_bytes);
        m_budget.release(bytes);
        m_bytes -= bytes;
    }

    void handOff() { m_bytes = 0; }

private:
    ProducerBudget& m_budget;
    size_t m_bytes { 0 };
};

// Invalid material from the analysis import, counted by its step, with the key of the body that holds it.
static void raiseAnalysisInvalidMaterial(VMState& state, InvalidMaterialStep step, ASCIILiteral stepName, const String& what, const BodyKey& key)
{
    ++state.registry().statistics().invalidMaterial[static_cast<size_t>(step)];
    state.raiseInvalidMaterial(stepName, makeString(what, "; body "_s, bodyKeyHex(key)));
}

// The module-analysis event, from the key's step to the return, while a bench report is open.
static void recordModuleAnalysisEvent(BenchReport* report, uint64_t startNanoseconds, const BodyKey& key, ASCIILiteral outcome, size_t sectionBytes)
{
    if (!report)
        return;
    uint64_t nanoseconds = benchThreadCPUNanoseconds() - startNanoseconds;
    report->record("module-analysis"_s, {
        { "key"_s, bodyKeyHex(key) },
        { "outcome"_s, String { outcome } },
        { "bytes"_s, static_cast<uint64_t>(sectionBytes) },
        { "nanoseconds"_s, nanoseconds },
    });
}

// Reads the ids of a body in order.
class AnalysisIdCursor {
public:
    AnalysisIdCursor(std::span<const uint8_t> ids, unsigned width)
        : m_ids(ids)
        , m_width(width)
    {
    }

    std::optional<uint32_t> next()
    {
        if (m_ids.size() < m_width)
            return std::nullopt;
        uint32_t id = readAnalysisInteger(m_ids, m_width);
        m_ids = m_ids.subspan(m_width);
        return id;
    }

    bool atEnd() const { return m_ids.empty(); }

private:
    std::span<const uint8_t> m_ids;
    unsigned m_width;
};

static bool isAnalysisFetchKind(uint8_t fetch)
{
    return fetch == static_cast<uint8_t>(AnalysisFetchKind::None) || fetch == static_cast<uint8_t>(AnalysisFetchKind::WebAssembly)
        || fetch == static_cast<uint8_t>(AnalysisFetchKind::JSON) || fetch == static_cast<uint8_t>(AnalysisFetchKind::HostDefined);
}

// A requested module's identity for the duplicate and reference checks: its specifier, its type and its phase. Fetch
// kind None counts as JavaScript, which no other kind a valid section holds can be.
static uint64_t requestedModuleKey(uint32_t specifier, uint8_t fetch, bool deferred)
{
    return static_cast<uint64_t>(specifier) | (static_cast<uint64_t>(fetch) << 32) | (static_cast<uint64_t>(deferred) << 40);
}

using AnalysisKeySet = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

} // namespace UCBAnalysisInternal

std::expected<Vector<uint8_t>, AnalysisEncodeFailure> encodeModuleAnalysis(VM& vm, const JSModuleRecord& record, ProducerBudget& budget)
{
    using namespace UCBAnalysisInternal;

    // The counts, which also find a record the section cannot carry.
    AnalysisCounter counter;
    if (!walkAnalysis(vm, record, counter))
        return std::unexpected(AnalysisEncodeFailure::Unrepresentable);
    // Every count is a 32-bit field, and the ids reach n + 1.
    constexpr uint64_t countLimit = std::numeric_limits<uint32_t>::max() - 2;
    if (counter.requestedModules > countLimit || counter.records > countLimit || counter.names > countLimit)
        return std::unexpected(AnalysisEncodeFailure::Unrepresentable);
    auto slotCount = static_cast<size_t>(counter.names);

    AnalysisEncodeCharges charges(budget);

    // The table of names: each string slot's name in write order, then the slots sorted by content, then each slot's id.
    // Strings with equal code units share an id, whatever their storage, numbered in the order of their first slot.
    size_t namesBytes = slotCount * sizeof(const StringImpl*);
    if (!charges.tryCharge(namesBytes))
        return std::unexpected(AnalysisEncodeFailure::BudgetRefused);
    Vector<const StringImpl*> names(slotCount);
    AnalysisNameCollector collector { names.mutableSpan() };
    bool walked = walkAnalysis(vm, record, collector);
    ASSERT_UNUSED(walked, walked && collector.next == slotCount);

    size_t idsBytes = slotCount * sizeof(uint32_t);
    if (!charges.tryCharge(idsBytes))
        return std::unexpected(AnalysisEncodeFailure::BudgetRefused);
    Vector<uint32_t> nameIds(slotCount);
    {
        if (!charges.tryCharge(idsBytes))
            return std::unexpected(AnalysisEncodeFailure::BudgetRefused);
        Vector<uint32_t> order(slotCount);
        std::iota(order.begin(), order.end(), 0);
        std::ranges::sort(order, [&](uint32_t a, uint32_t b) {
            if (auto comparison = codePointCompare(StringView(*names[a]), StringView(*names[b])); std::is_neq(comparison))
                return std::is_lt(comparison);
            return a < b;
        });
        // Each slot first gets the first slot of its group, which sorts first within it.
        for (size_t i = 0; i < slotCount; ++i) {
            uint32_t slot = order[i];
            bool startsGroup = !i || !WTF::equal(names[order[i - 1]], names[slot]);
            nameIds[slot] = startsGroup ? slot : nameIds[order[i - 1]];
        }
        order.clear();
        charges.release(idsBytes);
    }
    // Then the ids, in the order of first slots: a group's first slot is the one whose first slot is itself, and each
    // later slot of the group comes after it.
    uint32_t stringCount = 0;
    for (size_t slot = 0; slot < slotCount; ++slot) {
        uint32_t first = nameIds[slot];
        nameIds[slot] = first == slot ? stringCount++ : nameIds[first];
    }

    // The sizes. A string's first slot is the slot whose id is the number of strings met before it.
    uint64_t blobLength = 0;
    for (uint32_t slot = 0, met = 0; slot < slotCount; ++slot) {
        if (nameIds[slot] != met)
            continue;
        ++met;
        blobLength += analysisStringSize(*names[slot], blobLength);
    }
    if (blobLength > std::numeric_limits<uint32_t>::max())
        return std::unexpected(AnalysisEncodeFailure::Unrepresentable);
    unsigned offsetWidth = analysisWidth(blobLength);
    uint64_t offsetsBytes = static_cast<uint64_t>(offsetWidth) * (static_cast<uint64_t>(stringCount) + 1);
    uint64_t blobStart = analysisTableHeaderSize + offsetsBytes + (offsetsBytes & 1);
    uint64_t tableSize = blobStart + blobLength;
    unsigned idWidth = analysisWidth(static_cast<uint64_t>(stringCount) + 1);
    uint64_t bodySize = analysisBodyHeaderSize + counter.records + counter.requestedModules + idWidth * counter.ids;
    uint64_t sectionSize = tableSize + bodySize;
    if (sectionSize > std::numeric_limits<uint32_t>::max())
        return std::unexpected(AnalysisEncodeFailure::Unrepresentable);

    // The section, charged once at its size.
    if (!charges.tryCharge(sectionSize))
        return std::unexpected(AnalysisEncodeFailure::BudgetRefused);
    Vector<uint8_t> bytes(static_cast<size_t>(sectionSize));
    std::ranges::fill(bytes, 0);
    std::span<uint8_t> out = bytes.mutableSpan();

    // The string table.
    out[0] = static_cast<uint8_t>(offsetWidth);
    writeAnalysisInteger(out.subspan(4, 4), stringCount, 4);
    std::span<uint8_t> blob = out.subspan(blobStart, blobLength);
    uint64_t offset = 0;
    for (uint32_t slot = 0, met = 0; slot < slotCount; ++slot) {
        if (nameIds[slot] != met)
            continue;
        writeAnalysisInteger(out.subspan(analysisTableHeaderSize + offsetWidth * met, offsetWidth), offset, offsetWidth);
        offset += writeAnalysisString(blob.subspan(offset), *names[slot], offset);
        ++met;
    }
    ASSERT(offset == blobLength);
    writeAnalysisInteger(out.subspan(analysisTableHeaderSize + offsetWidth * static_cast<uint64_t>(stringCount), offsetWidth), blobLength, offsetWidth);

    // The body.
    std::span<uint8_t> body = out.subspan(tableSize);
    uint8_t flags = 0;
    if (record.features() & ImportMetaFeature)
        flags |= analysisImportMetaFlag;
    if (record.hasTLA())
        flags |= analysisTopLevelAwaitFlag;
    body[0] = flags;
    body[1] = static_cast<uint8_t>(idWidth);
    writeAnalysisInteger(body.subspan(4, 4), counter.requestedModules, 4);
    writeAnalysisInteger(body.subspan(8, 4), counter.records, 4);
    auto recordCount = static_cast<size_t>(counter.records);
    auto requestedCount = static_cast<size_t>(counter.requestedModules);
    AnalysisBodyWriter writer {
        .recordTags = body.subspan(analysisBodyHeaderSize, recordCount),
        .requestedTags = body.subspan(analysisBodyHeaderSize + recordCount, requestedCount),
        .ids = body.subspan(analysisBodyHeaderSize + recordCount + requestedCount),
        .nameIds = nameIds.span(),
        .defaultExportId = stringCount + 1,
        .idWidth = idWidth,
    };
    walked = walkAnalysis(vm, record, writer);
    ASSERT_UNUSED(walked, walked && writer.nextIdByte == writer.ids.size() && writer.nextName == slotCount);

    // The table of names goes, and with it its charge; the section's stays.
    names.clear();
    nameIds.clear();
    charges.release(namesBytes + idsBytes);
    charges.handOff();
    return bytes;
}

bool validateModuleAnalysis(std::span<const uint8_t> bytes, String& failure)
{
    using namespace UCBAnalysisInternal;

    auto fail = [&](ASCIILiteral what) {
        failure = what;
        return false;
    };

    // The string table's header and offsets.
    if (bytes.size() < analysisTableHeaderSize)
        return fail("the section ends inside the string table's header"_s);
    unsigned offsetWidth = bytes[0];
    if (offsetWidth != 1 && offsetWidth != 2 && offsetWidth != 4)
        return fail("the string table's offset width is not 1, 2 or 4"_s);
    if (bytes[1] || bytes[2] || bytes[3])
        return fail("the three bytes after the string table's offset width are not zero"_s);
    uint32_t stringCount = readAnalysisInteger(bytes.subspan(4), 4);
    uint64_t offsetsBytes = static_cast<uint64_t>(offsetWidth) * (static_cast<uint64_t>(stringCount) + 1);
    uint64_t blobStart = analysisTableHeaderSize + offsetsBytes + (offsetsBytes & 1);
    if (blobStart > bytes.size())
        return fail("the section ends inside the string offsets"_s);
    if ((offsetsBytes & 1) && bytes[analysisTableHeaderSize + offsetsBytes])
        return fail("the padding byte after the string offsets is not zero"_s);
    auto offsetAt = [&](uint64_t index) {
        return readAnalysisInteger(bytes.subspan(analysisTableHeaderSize + offsetWidth * index), offsetWidth);
    };
    uint32_t blobLength = offsetAt(stringCount);
    if (analysisWidth(blobLength) != offsetWidth)
        return fail("the string table's offset width is not the width of the blob's length"_s);
    if (blobLength > bytes.size() - blobStart)
        return fail("the blob ends past the section's end"_s);
    std::span<const uint8_t> blob = bytes.subspan(blobStart, blobLength);

    // The strings. Each takes at least its tag byte, so the count is bounded by the blob, which bounds what the duplicate
    // check allocates.
    if (offsetAt(0))
        return fail("the first string does not start the blob"_s);
    struct AnalysisString {
        uint8_t tag;
        std::span<const uint8_t> units;
    };
    Vector<AnalysisString> strings;
    for (uint32_t index = 0; index < stringCount; ++index) {
        uint32_t start = offsetAt(index);
        uint32_t end = offsetAt(index + 1);
        if (end < start || end > blobLength)
            return fail("the string offsets decrease"_s);
        if (end == start)
            return fail("a string has no tag byte"_s);
        uint8_t tag = blob[start];
        if (tag == 1) {
            strings.append(AnalysisString { tag, blob.subspan(start + 1, end - start - 1) });
            continue;
        }
        if (tag)
            return fail("a string's tag is neither 0 nor 1"_s);
        uint32_t unitsStart = start + 1;
        if (unitsStart & 1) {
            if (unitsStart == end || blob[unitsStart])
                return fail("a UTF-16 string's padding byte is missing or not zero"_s);
            ++unitsStart;
        }
        std::span<const uint8_t> units = blob.subspan(unitsStart, end - unitsStart);
        if (units.size() & 1)
            return fail("a UTF-16 string's units take an odd number of bytes"_s);
        bool aboveLatin1 = false;
        for (size_t i = 0; i < units.size() && !aboveLatin1; i += 2)
            aboveLatin1 = units[i + 1];
        if (!aboveLatin1)
            return fail("a UTF-16 string has no unit above 0xFF"_s);
        strings.append(AnalysisString { tag, units });
    }
    std::ranges::sort(strings, [](const AnalysisString& a, const AnalysisString& b) {
        if (a.tag != b.tag)
            return a.tag < b.tag;
        return std::ranges::lexicographical_compare(a.units, b.units);
    });
    for (size_t i = 1; i < strings.size(); ++i) {
        if (strings[i - 1].tag == strings[i].tag && std::ranges::equal(strings[i - 1].units, strings[i].units))
            return fail("two strings are equal"_s);
    }

    // The body's header.
    std::span<const uint8_t> body = bytes.subspan(blobStart + blobLength);
    if (body.size() < analysisBodyHeaderSize)
        return fail("the section ends inside the body's header"_s);
    if (body[0] & ~(analysisImportMetaFlag | analysisTopLevelAwaitFlag))
        return fail("the flags set a bit other than import.meta and top-level await"_s);
    unsigned idWidth = body[1];
    if (idWidth != analysisWidth(static_cast<uint64_t>(stringCount) + 1))
        return fail("the id width is not the width of the string count plus one"_s);
    if (body[2] || body[3])
        return fail("the two bytes after the id width are not zero"_s);
    uint32_t requestedCount = readAnalysisInteger(body.subspan(4), 4);
    uint32_t recordCount = readAnalysisInteger(body.subspan(8), 4);
    if (static_cast<uint64_t>(recordCount) + requestedCount > body.size() - analysisBodyHeaderSize)
        return fail("the record and requested-module tags run past the section's end"_s);
    std::span<const uint8_t> recordTags = body.subspan(analysisBodyHeaderSize, recordCount);
    std::span<const uint8_t> requestedTags = body.subspan(analysisBodyHeaderSize + recordCount, requestedCount);
    AnalysisIdCursor ids(body.subspan(analysisBodyHeaderSize + recordCount + requestedCount), idWidth);

    // The tags.
    for (uint8_t tag : recordTags) {
        auto kind = static_cast<AnalysisRecordKind>(tag & 7);
        uint8_t fetch = (tag >> 3) & 7;
        if (kind == AnalysisRecordKind::SingleTypeScriptImport)
            return fail("a record has kind 1, which the analysis parse never produces"_s);
        if (tag & 0x80)
            return fail("a record's tag sets bit 7"_s);
        if ((tag & analysisSameNameBit) && kind != AnalysisRecordKind::SingleImport)
            return fail("a record that is no single import sets the same-name bit"_s);
        if (!isAnalysisFetchKind(fetch))
            return fail("a record's fetch kind is not 0, 2, 3 or 4"_s);
        if (kind == AnalysisRecordKind::LocalExport && fetch)
            return fail("a local export has a fetch kind other than 0"_s);
    }
    for (uint8_t tag : requestedTags) {
        if (!isAnalysisFetchKind((tag >> 1) & 7))
            return fail("a requested module's fetch kind is not 0, 2, 3 or 4"_s);
        if (tag & 0xF0)
            return fail("a requested module's tag sets a bit above its fetch kind"_s);
    }

    // The ids, and the records' references and duplicates.
    std::optional<ASCIILiteral> idFailure;
    auto stringId = [&]() -> std::optional<uint32_t> {
        std::optional<uint32_t> id = ids.next();
        if (!id)
            idFailure = "the ids end before the records do"_s;
        else if (*id >= stringCount)
            idFailure = "an id names no string"_s;
        return idFailure ? std::nullopt : id;
    };
    AnalysisKeySet requestedModules;
    for (uint8_t tag : requestedTags) {
        uint8_t fetch = (tag >> 1) & 7;
        std::optional<uint32_t> specifier = stringId();
        if (!specifier || (fetch == static_cast<uint8_t>(AnalysisFetchKind::HostDefined) && !stringId()))
            return fail(*idFailure);
        if (!requestedModules.add(requestedModuleKey(*specifier, fetch, tag & 1)).isNewEntry)
            return fail("two requested modules share a specifier, a type and a phase"_s);
    }
    AnalysisKeySet importLocalNames;
    AnalysisKeySet exportNames;
    AnalysisKeySet starExports;
    auto namesRequestedModule = [&](uint32_t moduleName, uint8_t fetch, bool deferred) {
        return requestedModules.contains(requestedModuleKey(moduleName, fetch, deferred));
    };
    for (uint8_t tag : recordTags) {
        auto kind = static_cast<AnalysisRecordKind>(tag & 7);
        uint8_t fetch = (tag >> 3) & 7;
        std::optional<uint32_t> moduleName;
        switch (kind) {
        case AnalysisRecordKind::SingleImport:
        case AnalysisRecordKind::NamespaceImport:
        case AnalysisRecordKind::DeferredNamespaceImport: {
            moduleName = stringId();
            std::optional<uint32_t> localName;
            if (moduleName && kind == AnalysisRecordKind::SingleImport) {
                std::optional<uint32_t> importName = stringId();
                localName = (!importName || (tag & analysisSameNameBit)) ? importName : stringId();
            } else if (moduleName)
                localName = stringId();
            if (!localName)
                return fail(*idFailure);
            if (!importLocalNames.add(*localName).isNewEntry)
                return fail("two import entries share a local name"_s);
            if (!namesRequestedModule(*moduleName, fetch, kind == AnalysisRecordKind::DeferredNamespaceImport))
                return fail("an import entry names no requested module with its type and phase"_s);
            break;
        }
        case AnalysisRecordKind::IndirectExport:
        case AnalysisRecordKind::NamespaceExport: {
            std::optional<uint32_t> exportName = stringId();
            if (exportName && kind == AnalysisRecordKind::IndirectExport && !stringId())
                return fail(*idFailure);
            if (exportName)
                moduleName = stringId();
            if (!moduleName)
                return fail(*idFailure);
            if (!exportNames.add(*exportName).isNewEntry)
                return fail("two export entries share an export name"_s);
            if (!namesRequestedModule(*moduleName, fetch, false))
                return fail("an indirect or namespace export names no requested module with its type in the evaluation phase"_s);
            break;
        }
        case AnalysisRecordKind::LocalExport: {
            std::optional<uint32_t> exportName = stringId();
            if (!exportName)
                return fail(*idFailure);
            // The local name may also be the anonymous default export's private name, n + 1.
            std::optional<uint32_t> localName = ids.next();
            if (!localName)
                return fail("the ids end before the records do"_s);
            if (*localName >= stringCount && *localName != stringCount + 1)
                return fail("a local export's local name is no string and not the default export's name"_s);
            if (!exportNames.add(*exportName).isNewEntry)
                return fail("two export entries share an export name"_s);
            break;
        }
        case AnalysisRecordKind::StarExport:
            moduleName = stringId();
            if (!moduleName)
                return fail(*idFailure);
            if (!starExports.add(requestedModuleKey(*moduleName, fetch, false)).isNewEntry)
                return fail("two star exports share a module name and a type"_s);
            if (!namesRequestedModule(*moduleName, fetch, false))
                return fail("a star export names no requested module with its type in the evaluation phase"_s);
            break;
        case AnalysisRecordKind::SingleTypeScriptImport:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (fetch == static_cast<uint8_t>(AnalysisFetchKind::HostDefined) && !stringId())
            return fail(*idFailure);
    }
    if (!ids.atEnd())
        return fail("the ids do not end at the section's end"_s);
    return true;
}

std::optional<ModuleAnalysisImport> importModuleAnalysis(JSGlobalObject& globalObject, StringView source, const std::array<uint8_t, 32>* suppliedDigest)
{
    using namespace UCBAnalysisInternal;

    VM& vm = globalObject.vm();
    VMState* state = vm.jitCacheState();
    if (!state || !state->importsEnabled())
        return std::nullopt;
    UCBStatistics& statistics = state->registry().statistics();
    bool strict = state->strict();

    // The key, from the digest of the module's text: with strict off a supplied digest stands for it; with strict on the
    // text is hashed and a supplied digest held to it, before the provider that would supply it exists.
    auto keyFor = [&](const Digest256& digest) {
        return BodyKey::make(IdentityKind::Module, CodeSpecializationKind::CodeForCall, globalObject.defaultCodeGenerationMode(),
            rootIdentityDigest(IdentityKind::Module, digest, StrictModeLexicallyScopedFeature, std::nullopt));
    };
    Digest256 digest;
    if (suppliedDigest && !strict)
        digest = *suppliedDigest;
    else {
        digest = sourceDigest(source);
        if (!suppliedDigest)
            ++statistics.sourceDigests;
        else {
            ++statistics.suppliedDigestVerifications;
            if (digest != *suppliedDigest) {
                raiseAnalysisInvalidMaterial(*state, InvalidMaterialStep::SuppliedDigest, "ucb.supplied-digest"_s, "a module's supplied digest is not the digest of its text"_s, keyFor(digest));
                return std::nullopt;
            }
        }
    }

    // The module's request computes the same key: a module is code for call, strict, with no with-scope bit, and an
    // executable without a symbol table generates in its global object's default mode.
    BenchReport* report = state->benchReport();
    uint64_t startNanoseconds = report ? benchThreadCPUNanoseconds() : 0;
    BodyKey key = keyFor(digest);
    ModuleAnalysisImport result { digest, { } };

    if (!Options::jitCacheImportModuleAnalysis()) {
        recordModuleAnalysisEvent(report, startNanoseconds, key, "off"_s, 0);
        return result;
    }

    // The one section, without checksumming the body's others, which the module's request opens later.
    SectionLookup lookup = state->readBodySection(key, SectionKind::UCBAnalysis);
    switch (lookup.kind()) {
    case SectionLookup::Kind::Absent:
        ++statistics.analysisMisses;
        recordModuleAnalysisEvent(report, startNanoseconds, key, "miss"_s, 0);
        return result;
    case SectionLookup::Kind::Unusable:
        // The integrator found the body damaged and raised the fault.
        recordModuleAnalysisEvent(report, startNanoseconds, key, "invalid"_s, 0);
        return std::nullopt;
    case SectionLookup::Kind::Found:
        break;
    }
    std::span<const uint8_t> section = lookup.bytes();
    if (section.empty()) {
        ++statistics.analysisAbsent;
        recordModuleAnalysisEvent(report, startNanoseconds, key, "absent"_s, 0);
        return result;
    }

    // With strict on, the bytes are an analysis the parse can produce before Bun builds a record from them.
    String failure;
    if (strict && !validateModuleAnalysis(section, failure)) {
        raiseAnalysisInvalidMaterial(*state, InvalidMaterialStep::Analysis, "ucb.analysis"_s, makeString("ucb.analysis is no analysis the parse can produce: "_s, failure), key);
        recordModuleAnalysisEvent(report, startNanoseconds, key, "invalid"_s, section.size());
        return std::nullopt;
    }

    result.moduleInfo = Vector<uint8_t> { section };
    ++statistics.analysisImports;
    recordModuleAnalysisEvent(report, startNanoseconds, key, "hit"_s, section.size());
    return result;
}

#if ENABLE(JITCACHE_TWINS)

namespace UCBAnalysisInternal {

static String analysisNameText(const Identifier& name)
{
    if (name.isNull())
        return "null"_s;
    return makeString('"', name.string(), '"');
}

static String analysisNameText(const UniquedStringImpl* name)
{
    if (!name)
        return "null"_s;
    return makeString('"', StringView(*name), '"');
}

} // namespace UCBAnalysisInternal

String firstModuleAnalysisDifference(const JSModuleRecord& fromArtifact, const JSModuleRecord& fromParse)
{
    using namespace UCBAnalysisInternal;

    // The requested modules.
    const auto& artifactRequests = fromArtifact.requestedModules();
    const auto& parseRequests = fromParse.requestedModules();
    if (artifactRequests.size() != parseRequests.size())
        return makeString("the record has "_s, artifactRequests.size(), " requested modules, the parse's "_s, parseRequests.size());
    for (size_t i = 0; i < artifactRequests.size(); ++i) {
        const auto& artifact = artifactRequests[i];
        const auto& parse = parseRequests[i];
        if (artifact.m_specifier != parse.m_specifier)
            return makeString("requested module "_s, i, " has specifier "_s, analysisNameText(artifact.m_specifier), ", the parse's "_s, analysisNameText(parse.m_specifier));
        if (artifact.m_phase != parse.m_phase)
            return makeString("requested module "_s, i, " has another phase than the parse's"_s);
        if (!artifact.m_attributes != !parse.m_attributes)
            return makeString("requested module "_s, i, artifact.m_attributes ? " has attributes, the parse's none"_s : " has no attributes, the parse's some"_s);
        if (!artifact.m_attributes)
            continue;
        if (artifact.m_attributes->type() != parse.m_attributes->type())
            return makeString("requested module "_s, i, " has type "_s, static_cast<unsigned>(artifact.m_attributes->type()), ", the parse's "_s, static_cast<unsigned>(parse.m_attributes->type()));
#if USE(BUN_JSC_ADDITIONS)
        if (artifact.m_attributes->type() == ScriptFetchParameters::Type::HostDefined && artifact.m_attributes->hostDefinedImportType() != parse.m_attributes->hostDefinedImportType())
            return makeString("requested module "_s, i, " has host-defined type \""_s, artifact.m_attributes->hostDefinedImportType(), "\", the parse's \""_s, parse.m_attributes->hostDefinedImportType(), '"');
#endif
    }

    // The import entries, in order.
    const auto& artifactImports = fromArtifact.importEntries();
    const auto& parseImports = fromParse.importEntries();
    if (artifactImports.size() != parseImports.size())
        return makeString("the record has "_s, artifactImports.size(), " import entries, the parse's "_s, parseImports.size());
    size_t index = 0;
    auto parseImport = parseImports.begin();
    for (const auto& [key, artifact] : artifactImports) {
        const auto& [parseKey, parse] = *parseImport;
        ++parseImport;
        String which = makeString("import entry "_s, index++);
        if (key.get() != parseKey.get())
            return makeString(which, " has key "_s, analysisNameText(key.get()), ", the parse's "_s, analysisNameText(parseKey.get()));
        if (artifact.type != parse.type)
            return makeString(which, " has another type than the parse's"_s);
        if (artifact.phase != parse.phase)
            return makeString(which, " has another phase than the parse's"_s);
        if (artifact.moduleRequestType != parse.moduleRequestType)
            return makeString(which, " has another module request type than the parse's"_s);
        if (artifact.moduleRequest != parse.moduleRequest)
            return makeString(which, " requests "_s, analysisNameText(artifact.moduleRequest), ", the parse's "_s, analysisNameText(parse.moduleRequest));
        if (artifact.importName != parse.importName)
            return makeString(which, " imports "_s, analysisNameText(artifact.importName), ", the parse's "_s, analysisNameText(parse.importName));
        if (artifact.localName != parse.localName)
            return makeString(which, " has local name "_s, analysisNameText(artifact.localName), ", the parse's "_s, analysisNameText(parse.localName));
    }

    // The export entries, in order.
    const auto& artifactExports = fromArtifact.exportEntries();
    const auto& parseExports = fromParse.exportEntries();
    if (artifactExports.size() != parseExports.size())
        return makeString("the record has "_s, artifactExports.size(), " export entries, the parse's "_s, parseExports.size());
    index = 0;
    auto parseExport = parseExports.begin();
    for (const auto& [key, artifact] : artifactExports) {
        const auto& [parseKey, parse] = *parseExport;
        ++parseExport;
        String which = makeString("export entry "_s, index++);
        if (key.get() != parseKey.get())
            return makeString(which, " has key "_s, analysisNameText(key.get()), ", the parse's "_s, analysisNameText(parseKey.get()));
        if (artifact.type != parse.type)
            return makeString(which, " has another type than the parse's"_s);
        if (artifact.moduleRequestType != parse.moduleRequestType)
            return makeString(which, " has another module request type than the parse's"_s);
        if (artifact.exportName != parse.exportName)
            return makeString(which, " exports "_s, analysisNameText(artifact.exportName), ", the parse's "_s, analysisNameText(parse.exportName));
        if (artifact.moduleName != parse.moduleName)
            return makeString(which, " has module name "_s, analysisNameText(artifact.moduleName), ", the parse's "_s, analysisNameText(parse.moduleName));
        if (artifact.importName != parse.importName)
            return makeString(which, " has import name "_s, analysisNameText(artifact.importName), ", the parse's "_s, analysisNameText(parse.importName));
        if (artifact.localName != parse.localName)
            return makeString(which, " has local name "_s, analysisNameText(artifact.localName), ", the parse's "_s, analysisNameText(parse.localName));
    }

    // The star exports, in order.
    const auto& artifactStars = fromArtifact.starExportEntries();
    const auto& parseStars = fromParse.starExportEntries();
    if (artifactStars.size() != parseStars.size())
        return makeString("the record has "_s, artifactStars.size(), " star exports, the parse's "_s, parseStars.size());
    index = 0;
    auto parseStar = parseStars.begin();
    for (const auto& [moduleName, type] : artifactStars) {
        const auto& [parseModuleName, parseType] = *parseStar;
        ++parseStar;
        if (moduleName.get() != parseModuleName.get())
            return makeString("star export "_s, index, " names "_s, analysisNameText(moduleName.get()), ", the parse's "_s, analysisNameText(parseModuleName.get()));
        if (type != parseType)
            return makeString("star export "_s, index, " has another type than the parse's"_s);
        ++index;
    }

    // The flags.
    if ((fromArtifact.features() & ImportMetaFeature) != (fromParse.features() & ImportMetaFeature))
        return "the record's import.meta flag differs from the parse's"_s;
    if (fromArtifact.hasTLA() != fromParse.hasTLA())
        return "the record's top-level-await flag differs from the parse's"_s;
#if USE(BUN_JSC_ADDITIONS)
    if (fromArtifact.m_isTypeScript != fromParse.m_isTypeScript)
        return "the record's TypeScript flag differs from the parse's"_s;
#endif
    return { };
}

#endif // ENABLE(JITCACHE_TWINS)

} // namespace JSC::JITCache
