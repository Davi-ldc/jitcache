#include "config.h"
#include "JITCacheTwinsHarness.h"

#if ENABLE(JITCACHE_TWINS)

#include "AbstractModuleRecord.h"
#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "BigIntObject.h"
#include "BooleanObject.h"
#include "BrandedStructure.h"
#include "CodeBlock.h"
#include "CyclicModuleRecord.h"
#include "DateInstance.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "HeapIterationScope.h"
#include "ImageTwins.h"
#include "InternalFieldTuple.h"
#include "InternalFunction.h"
#include "JITCacheBench.h"
#include "JITCacheBodyEvents.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCacheVMState.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSArrayIterator.h"
#include "JSAsyncDisposableStack.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSBoundFunction.h"
#include "JSBoundFunctionInlines.h"
#include "JSCConfig.h"
#include "JSCInlines.h"
#include "JSDisposableStack.h"
#include "JSFinalizationRegistry.h"
#include "JSFunction.h"
#include "JSFunctionInlines.h"
#include "JSGenerator.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSIteratorHelper.h"
#include "JSLexicalEnvironment.h"
#include "JSMap.h"
#include "JSMapIterator.h"
#include "JSModuleEnvironment.h"
#include "JSModuleLoader.h"
#include "JSModuleNamespaceObject.h"
#include "JSPromise.h"
#include "JSRegExpStringIterator.h"
#include "JSSet.h"
#include "JSSetIterator.h"
#include "JSStringIterator.h"
#include "JSWeakMap.h"
#include "JSWeakObjectRef.h"
#include "JSWeakSet.h"
#include "JSWithScope.h"
#include "JSWrapForValidIterator.h"
#include "JSWrapperObject.h"
#include "MarkedSpaceInlines.h"
#include "MarkedVector.h"
#include "MathCommon.h"
#include "ModuleRegistryEntry.h"
#include "NumberObject.h"
#include "ObjectConstructor.h"
#include "ProxyObject.h"
#include "RegExp.h"
#include "RegExpObject.h"
#include "StringObject.h"
#include "Symbol.h"
#include "SymbolObject.h"
#include "SymbolTable.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "UCBTwins.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <errno.h>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/HashMap.h>
#include <wtf/HexNumber.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

// A kernel older than 4.17 ignores the flag and treats the address as a hint, which placePlaceholders detects; the value is
// the same on x86_64 and ARM64.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// The fields that jitcacheProgress, jitcacheBodyEvents with the body-event dump, and jitcacheUCBStatistics write by name.
#define JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(v) \
    v(indexedBodies) v(bodyOpens) v(transientOpenFailures) v(imports) v(seededDecodes) v(attaches) v(gateDrops) v(misses) \
    v(installs) v(bakedFactMismatches) v(captureCandidates) v(capturesDeferred) v(capturesCommitted) v(bytesCommitted) \
    v(deltaRuns)
#define JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(v) \
    v(llintInstructions) v(baselineCompiles) v(dfgCompiles) v(ftlCompiles) v(osrExits) v(jettisons) v(reoptimizations)
#define JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(v) \
    v(imports) v(seededDecodes) v(attaches) v(gateDrops) v(sourceDigests) v(suppliedSourceDigests) \
    v(suppliedDigestVerifications) v(contextDigests) v(holderDigests) v(tdzEnvironmentDigests)

namespace JSC::JITCache {

namespace JITCacheTwinsHarnessInternal {

// A field the lists above miss would go unreported, so a struct that grows fails the build here.
#define JITCACHE_HARNESS_COUNT_FIELD(field) +1
static_assert(sizeof(Progress) == (0 JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
static_assert(sizeof(BodyEventCounts) == (0 JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t));
static_assert(sizeof(UCBStatistics) == (0 JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(JITCACHE_HARNESS_COUNT_FIELD)) * sizeof(uint64_t)
    + sizeof(UCBStatistics::misses) + sizeof(UCBStatistics::records) + sizeof(UCBStatistics::invalidMaterial));
#undef JITCACHE_HARNESS_COUNT_FIELD

// The UCB statistics' indexed counters, by the enumerator names the UCB lane's tests use.
static constexpr std::array missReasonNames {
    "NoKey"_s, "NoBody"_s, "BodyUnchanged"_s, "RequestKey"_s, "Context"_s, "Provenance"_s, "Holder"_s, "AtomMap"_s,
    "CoreDigest"_s, "Stack"_s,
};
static constexpr std::array originNames { "Generated"_s, "Decoded"_s, "Imported"_s };
static constexpr std::array invalidMaterialNames {
    "Identity"_s, "Feedback"_s, "Decode"_s, "Closure"_s, "StrictCore"_s, "SuppliedDigest"_s,
};
static_assert(missReasonNames.size() == std::tuple_size_v<decltype(UCBStatistics::misses)>);
static_assert(originNames.size() == std::tuple_size_v<decltype(UCBStatistics::records)>);
static_assert(invalidMaterialNames.size() == std::tuple_size_v<decltype(UCBStatistics::invalidMaterial)>);

// Process-wide test state. The shell writes it on its main thread while it parses its command line and right after
// start, before any JavaScript runs and before a $262.agent worker thread exists; the host functions only read it, so
// thread creation orders every read after the writes.
static bool forcedBlinding { false };
static std::optional<ArtifactWriter::FaultForTesting> writerFault;
static std::optional<ArtifactWriter::KillForTesting> writerKill;
struct TwinEntry {
    enum class Kind : uint8_t { Difference, Skip, Coincidence };
    Kind kind;
    RelocationDomain domain;
};
static std::optional<TwinEntry> twinEntry;
// The shell's start (section 5.2, jitcacheStartOutcome; SPEC-integrator.md section 11.1, the delta log).
struct ShellStart {
    VM* vm { nullptr };
    StartOutcome outcome { StartOutcome::Fault };
    bool logsResults { false };
};
static ShellStart shellStart;

// Failures in test-only code end the process, so the runner fails the run on its exit code (harness sub-SPEC sections 4
// and 10.3).
[[noreturn]] static void exitAfterError(const String& path, int error)
{
    SAFE_FPRINTF(stderr, "JITCache: %s: %s\n", path.utf8(), safeStrerror(error));
    exit(1);
}

[[noreturn]] static void exitAfterMalformedLine(const String& path, std::span<const char> line)
{
    SAFE_FPRINTF(stderr, "JITCache: %s: %s in line \"%s\"\n", path.utf8(), safeStrerror(EINVAL), CString { line });
    exit(1);
}

// The whole file, or the errno of the call that failed.
static std::expected<Vector<char>, int> readWholeFile(const CString& path)
{
    int fd = ::open(path.data(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected(errno);
    Vector<char> contents;
    std::array<char, 4096> chunk;
    while (true) {
        ssize_t count = ::read(fd, chunk.data(), chunk.size());
        if (count < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            ::close(fd);
            return std::unexpected(error);
        }
        if (!count)
            break;
        contents.append(std::span<const char> { chunk }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return contents;
}

enum class WriteMode : bool { Replace, Append };

static void writeTestFile(const String& path, const CString& bytes, WriteMode mode)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (mode == WriteMode::Append ? O_APPEND : O_TRUNC);
    int fd = ::open(path.utf8().data(), flags, 0644);
    if (fd < 0)
        exitAfterError(path, errno);
    auto remaining = bytes.span();
    while (!remaining.empty()) {
        ssize_t written = ::write(fd, remaining.data(), remaining.size());
        if (written < 0) {
            int error = errno;
            if (error == EINTR)
                continue;
            exitAfterError(path, error);
        }
        remaining = remaining.subspan(static_cast<size_t>(written));
    }
    if (::close(fd) < 0)
        exitAfterError(path, errno);
}

static Vector<std::span<const char>> splitText(std::span<const char> text, char separator)
{
    Vector<std::span<const char>> parts;
    size_t start = 0;
    for (size_t index = 0; index <= text.size(); ++index) {
        if (index < text.size() && text[index] != separator)
            continue;
        if (index > start)
            parts.append(text.subspan(start, index - start));
        start = index + 1;
    }
    return parts;
}

// A hexadecimal value as section 4's layout lines write it, with or without its 0x prefix.
static std::optional<uint64_t> parseHexValue(std::span<const char> token)
{
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        token = token.subspan(2);
    if (token.empty() || token.size() > 16)
        return std::nullopt;
    uint64_t value = 0;
    for (char character : token) {
        if (!isASCIIHexDigit(character))
            return std::nullopt;
        value = (value << 4) | toASCIIHexValue(character);
    }
    return value;
}

struct AddressRange {
    uintptr_t start;
    uintptr_t end;
};

// The pool and structures ranges of a layout file; heap lines are the runner's alone.
static Vector<AddressRange> readLayoutRanges(const String& path)
{
    auto contents = readWholeFile(path.utf8());
    if (!contents)
        exitAfterError(path, contents.error());
    Vector<AddressRange> ranges;
    for (auto line : splitText(contents->span(), '\n')) {
        auto tokens = splitText(line, ' ');
        if (tokens.isEmpty())
            continue;
        StringView name { byteCast<Latin1Character>(tokens[0]) };
        if (name == "heap"_s) {
            if (tokens.size() != 4)
                exitAfterMalformedLine(path, line);
            continue;
        }
        bool isPool = name == "pool"_s;
        if (tokens.size() != 3 || (!isPool && name != "structures"_s))
            exitAfterMalformedLine(path, line);
        auto start = parseHexValue(tokens[1]);
        auto second = parseHexValue(tokens[2]);
        if (!start || !second)
            exitAfterMalformedLine(path, line);
        // pool <start> <end>; structures <start> <size>.
        uint64_t end = isPool ? *second : *start + *second;
        if (end <= *start || end > std::numeric_limits<uintptr_t>::max())
            exitAfterMalformedLine(path, line);
        ranges.append({ static_cast<uintptr_t>(*start), static_cast<uintptr_t>(end) });
    }
    return ranges;
}

static constexpr ASCIILiteral mapsPath = "/proc/self/maps"_s;

// The process's mappings in address order, as /proc/self/maps lists them.
static Vector<AddressRange> readMappings()
{
    auto contents = readWholeFile(CString { mapsPath.span() });
    if (!contents)
        exitAfterError(String { mapsPath }, contents.error());
    Vector<AddressRange> mappings;
    for (auto line : splitText(contents->span(), '\n')) {
        auto fields = splitText(line, ' ');
        auto bounds = fields.isEmpty() ? Vector<std::span<const char>> { } : splitText(fields[0], '-');
        std::optional<uint64_t> start = bounds.size() == 2 ? parseHexValue(bounds[0]) : std::nullopt;
        std::optional<uint64_t> end = bounds.size() == 2 ? parseHexValue(bounds[1]) : std::nullopt;
        if (!start || !end)
            exitAfterMalformedLine(String { mapsPath }, line);
        mappings.append({ static_cast<uintptr_t>(*start), static_cast<uintptr_t>(*end) });
    }
    return mappings;
}

static Vector<AddressRange>& placeholders()
{
    static NeverDestroyed<Vector<AddressRange>> list;
    return list.get();
}

enum class GapPlacement : uint8_t { Placed, Taken };

// One PROT_NONE placeholder over a gap /proc/self/maps listed as free. Taken: something mapped into the gap after the
// listing, which only this process's own allocations can do before JSC::initialize.
static GapPlacement placePlaceholder(const String& path, AddressRange gap)
{
    size_t size = gap.end - gap.start;
    void* requested = reinterpret_cast<void*>(gap.start);
    void* placed = mmap(requested, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (placed == MAP_FAILED) {
        if (errno == EEXIST)
            return GapPlacement::Taken;
        exitAfterError(path, errno);
    }
    if (placed != requested) {
        munmap(placed, size);
        exitAfterError(path, EEXIST);
    }
    placeholders().append(gap);
    return GapPlacement::Placed;
}

// Blocks every gap of a recorded range, so neither reservation JSC::initialize makes can land in it.
static void placePlaceholders(const String& path, AddressRange range)
{
    uintptr_t pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    uintptr_t start = WTF::roundUpToMultipleOf(pageSize, range.start);
    uintptr_t end = range.end & ~(pageSize - 1);
    if (start >= end)
        return;

    // Each attempt lists the mappings again, so a gap an attempt filled is a mapping in the next one.
    static constexpr unsigned maximumAttempts = 8;
    for (unsigned attempt = 1;; ++attempt) {
        bool taken = false;
        uintptr_t cursor = start;
        for (auto& mapping : readMappings()) {
            if (mapping.end <= cursor)
                continue;
            if (mapping.start >= end)
                break;
            if (mapping.start > cursor && placePlaceholder(path, { cursor, mapping.start }) == GapPlacement::Taken) {
                taken = true;
                break;
            }
            cursor = std::max(cursor, mapping.end);
            if (cursor >= end)
                break;
        }
        if (!taken && cursor < end && placePlaceholder(path, { cursor, end }) == GapPlacement::Taken)
            taken = true;
        if (!taken)
            return;
        if (attempt == maximumAttempts)
            exitAfterError(path, EEXIST);
    }
}

// <value>@<n> splits at its last '@'; n counts from 1.
static std::optional<uint64_t> parseCount(StringView text)
{
    if (text.isEmpty())
        return std::nullopt;
    for (auto character : text.codeUnits()) {
        if (!isASCIIDigit(character))
            return std::nullopt;
    }
    auto count = parseInteger<uint64_t>(text);
    if (!count || !*count)
        return std::nullopt;
    return count;
}

static String jsonString(const String& string)
{
    StringBuilder builder;
    builder.appendQuotedJSONString(string.isNull() ? emptyString() : string);
    return builder.toString();
}

static void writeLogLine(const String& line)
{
    SAFE_FPRINTF(stderr, "%s\n", line.utf8());
}

// Numbers in shortest round-trip form, with -0, NaN and the infinities spelled out and no int32 or double distinction.
static String numberText(double number)
{
    if (std::isnan(number))
        return "NaN"_s;
    if (std::isinf(number))
        return number > 0 ? "Infinity"_s : "-Infinity"_s;
    if (!number)
        return std::signbit(number) ? "-0"_s : "0"_s;
    return String::number(number);
}

static ASCIILiteral flagText(bool value)
{
    return value ? "1"_s : "0"_s;
}

static JSValue countValue(uint64_t count)
{
    return jsNumber(static_cast<double>(count));
}

static JSObject* bodyEventCountsObject(JSGlobalObject* globalObject, const BodyEventCounts& counts)
{
    VM& vm = globalObject->vm();
    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_COUNT(field) result->putDirect(vm, Identifier::fromString(vm, #field ""_s), countValue(counts.field));
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_PUT_COUNT)
#undef JITCACHE_HARNESS_PUT_COUNT
    return result;
}

static void appendBodyEventCounts(StringBuilder& builder, const BodyEventCounts& counts)
{
    ASCIILiteral separator = ""_s;
#define JITCACHE_HARNESS_APPEND_COUNT(field) \
    builder.append(separator, "\"" #field "\":"_s, counts.field); \
    separator = ","_s;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_APPEND_COUNT)
#undef JITCACHE_HARNESS_APPEND_COUNT
}

static void addBodyEventCounts(BodyEventCounts& total, const BodyEventCounts& counts)
{
#define JITCACHE_HARNESS_ADD_COUNT(field) total.field += counts.field;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_ADD_COUNT)
#undef JITCACHE_HARNESS_ADD_COUNT
}

static bool hasBodyEvents(const BodyEventCounts& counts)
{
    bool any = false;
#define JITCACHE_HARNESS_TEST_COUNT(field) any = any || counts.field;
    JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT(JITCACHE_HARNESS_TEST_COUNT)
#undef JITCACHE_HARNESS_TEST_COUNT
    return any;
}

// The CB of the kind its second argument names of the JSFunction with JS code its first argument holds, null when the
// function has no CB of that kind; a TypeError, and nothing, for any other function or kind.
static std::optional<CodeBlock*> functionCodeBlockArgument(JSGlobalObject* globalObject, CallFrame* callFrame, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* function = dynamicDowncast<JSFunction>(callFrame->argument(0));
    auto* executable = function ? dynamicDowncast<FunctionExecutable>(function->executable()) : nullptr;
    std::optional<CodeSpecializationKind> kind;
    JSValue kindValue = callFrame->argument(1);
    if (kindValue.isString()) {
        String kindName = asString(kindValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        if (kindName == "call"_s)
            kind = CodeSpecializationKind::CodeForCall;
        else if (kindName == "construct"_s)
            kind = CodeSpecializationKind::CodeForConstruct;
    }
    if (!executable || !kind) {
        throwTypeError(globalObject, scope, makeString(caller, " expects a function with JS code and \"call\" or \"construct\""_s));
        return std::nullopt;
    }
    return executable->codeBlockFor(*kind);
}

// A body key written as jitcacheBodyKey returns it, 80 lowercase hex digits; a TypeError, and nothing, otherwise.
static std::optional<BodyKey> bodyKeyArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        String hex = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        // The key's hex is its body file's name without ".bin" (JITCacheBench.h, bodyKeyHex), so the store's own name
        // decoding reads it back.
        if (hex.length() == 2 * BodyKey::byteSize && hex.containsOnlyASCII()) {
            CString name = makeString(hex, ".bin"_s).latin1();
            if (auto key = bodyKeyFromFileName(name.span()))
                return key;
        }
    }
    throwTypeError(globalObject, scope, makeString(caller, " expects a body key as 80 lowercase hex digits"_s));
    return std::nullopt;
}

// A section kind by the lane's name of SPEC-integrator.md section 6.1; a TypeError, and nothing, for any other name.
static std::optional<SectionKind> sectionKindArgument(JSGlobalObject* globalObject, JSValue value, ASCIILiteral caller)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (value.isString()) {
        String name = asString(value)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            if (name == sectionKindDescriptions[index].name)
                return static_cast<SectionKind>(index);
        }
    }
    throwTypeError(globalObject, scope, makeString(caller, " expects a section name such as \"ucb.core\" or \"cb.summary\""_s));
    return std::nullopt;
}

static String storeFailureText(const BodyOpen& opened)
{
    if (opened.outcome == StoreOutcome::Unavailable)
        return "the body is unavailable for now (EMFILE, ENFILE or ENOMEM)"_s;
    if (opened.failure.error)
        return makeString(opened.failure.check, ": "_s, String::fromLatin1(safeStrerror(opened.failure.error).data()));
    return String { opened.failure.check };
}

// Section 6: the description of the heap JavaScript can reach.
class HeapDescription {
    WTF_MAKE_NONCOPYABLE(HeapDescription);
public:
    // Whether the walk follows the edges of the global object, its JSGlobalProxy and its global lexical environment
    // (section 6.1). From the default roots it does. From given roots it writes each of the three that is no root with its
    // line and no edges: every function's scope chain ends there, so every root reaches them, and Bun's global object holds
    // per-process state.
    enum class GlobalScope : bool { Walked, WithoutEdges };

    // Wherever the walk follows the global object's edges, directly or through its JSGlobalProxy, it leaves out the global
    // object's own property `arguments`, where the jsc shell puts the run's role and paths (section 6.1), so a value only
    // `arguments` reaches changes no description (H5).
    explicit HeapDescription(JSGlobalObject& globalObject)
        : m_globalObject(globalObject)
        , m_vm(globalObject.vm())
    {
    }

    // Called once per HeapDescription. Null when reifying or reading a property threw; the exception stays on the VM.
    String describe(std::span<const JSValue> roots, GlobalScope globalScope)
    {
        ASSERT(m_records.isEmpty());
        if (globalScope == GlobalScope::WithoutEdges) {
            std::array<JSCell*, 3> globalScopeCells { &m_globalObject, m_globalObject.globalThis(), m_globalObject.globalLexicalEnvironment() };
            for (JSCell* cell : globalScopeCells) {
                bool isRoot = std::ranges::any_of(roots, [&](JSValue root) {
                    return root.isCell() && root.asCell() == cell;
                });
                if (cell && !isRoot && !m_cellsWithoutEdges.contains(cell))
                    m_cellsWithoutEdges.append(cell);
            }
        }

        Line rootLine { text("roots"_s) };
        for (JSValue root : roots) {
            if (!meet(root, rootLine))
                return { };
        }
        // Strong edges first, then the weak entries whose keys the walk reached, until a round meets no cell (section
        // 6.3).
        while (true) {
            while (m_nextToDescribe < m_records.size()) {
                if (!describeCell(m_nextToDescribe++))
                    return { };
            }
            size_t cellsBefore = m_records.size();
            if (!meetReachedWeakEntries())
                return { };
            if (m_records.size() == cellsBefore)
                break;
        }
        return output(rootLine);
    }

private:
    // An item of a line: text, or a cell that the line names by its ordinal. A weak target the walk did not reach writes
    // <unreached>, and a finalization registry's held value or token that it did not reach writes <cell> (section 6.3).
    enum class ItemKind : uint8_t { Text, Cell, WeakCell, ListedCell };
    struct Item {
        ItemKind kind;
        String text;
        JSCell* cell { nullptr };
    };
    using Line = Vector<Item, 6>;

    struct Record {
        JSCell* cell { nullptr };
        String line;
        Vector<Line> edges;
        // A weak map's or weak set's entries, the value empty for a set, and the lines of those whose keys the walk reached.
        bool isWeakContainer { false };
        bool weakEntriesHaveValues { false };
        Vector<std::pair<JSCell*, JSValue>> weakEntries;
        Vector<bool> weakEntryMet;
        Vector<std::pair<unsigned, Line>> weakEntryLines; // by the key's ordinal
        // A finalization registry's registrations: the held value and the unregister token.
        Vector<std::pair<Item, Item>> registrations;
    };

    static Item text(String string) { return { ItemKind::Text, WTF::move(string), nullptr }; }
    static Item cellItem(ItemKind kind, JSCell* cell) { return { kind, { }, cell }; }

    void reach(JSCell* cell)
    {
        auto result = m_ordinals.add(cell, static_cast<unsigned>(m_records.size()));
        if (result.isNewEntry)
            m_records.append(Record { cell });
    }

    // A primitive's text, or nothing for a cell: undefined, null and booleans; numbers; BigInts in decimal; strings as
    // JSON string literals, every lone surrogate escaped; the empty value of an uninitialized binding as <empty>.
    std::optional<String> primitiveText(JSValue value, bool& threw)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        threw = false;
        if (!value)
            return "<empty>"_s;
        if (value.isUndefined())
            return "undefined"_s;
        if (value.isNull())
            return "null"_s;
        if (value.isBoolean())
            return value.asBoolean() ? "true"_s : "false"_s;
        if (value.isNumber())
            return numberText(value.asNumber());
        if (value.isBigInt()) {
            String digits = value.toWTFString(&m_globalObject);
            threw = !!scope.exception();
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return makeString(digits, 'n');
        }
        if (value.isString()) {
            String string = asString(value)->value(&m_globalObject);
            threw = !!scope.exception();
            RETURN_IF_EXCEPTION(scope, std::nullopt);
            return jsonString(string);
        }
        return std::nullopt;
    }

    // Appends what a value writes where the walk meets it: a primitive, or the cell's ordinal, which the cell takes the
    // first time the walk meets it. False when an allocation threw.
    bool meet(JSValue value, Line& line)
    {
        bool threw = false;
        if (auto primitive = primitiveText(value, threw)) {
            line.append(text(WTF::move(*primitive)));
            return true;
        }
        if (threw)
            return false;
        reach(value.asCell());
        line.append(cellItem(ItemKind::Cell, value.asCell()));
        return true;
    }

    // A value a finalization registry lists without walking it.
    bool listedItem(JSValue value, Item& item)
    {
        bool threw = false;
        if (auto primitive = primitiveText(value, threw)) {
            item = text(WTF::move(*primitive));
            return true;
        }
        if (threw)
            return false;
        item = cellItem(ItemKind::ListedCell, value.asCell());
        return true;
    }

    bool describeCell(unsigned ordinal)
    {
        JSCell* cell = m_records[ordinal].cell;
        if (auto* symbol = dynamicDowncast<Symbol>(cell)) {
            m_records[ordinal].line = symbolLine(*symbol);
            return true;
        }
        auto* object = dynamicDowncast<JSObject>(cell);
        if (!object) {
            m_records[ordinal].line = makeString("cell "_s, cell->classInfo()->className);
            return true;
        }
        // From given roots, the global scope's cells that are no root: the global lexical environment writes its scope
        // line, and the global object and its JSGlobalProxy, of no class the rows below "any object" name, the any-object
        // line.
        if (m_cellsWithoutEdges.contains(cell)) {
            if (is<JSGlobalLexicalEnvironment>(object))
                m_records[ordinal].line = scopeLine("global-lexical"_s);
            else {
                StringBuilder line;
                appendObjectLine(line, *object);
                m_records[ordinal].line = line.toString();
            }
            return true;
        }
        // A scope is no object JavaScript reaches: its row describes it in full, its variables sorted by name, which
        // keeps the symbol table's hash order out of the description.
        if (auto* withScope = dynamicDowncast<JSWithScope>(object))
            return describeWithScope(ordinal, *withScope);
        if (auto* environment = dynamicDowncast<JSLexicalEnvironment>(object))
            return describeScope(ordinal, *environment, scopeKind(*environment));
        if (auto* globalLexicalEnvironment = dynamicDowncast<JSGlobalLexicalEnvironment>(object))
            return describeScope(ordinal, *globalLexicalEnvironment, "global-lexical"_s);
        return describeObject(ordinal, *object);
    }

    static String symbolLine(Symbol& symbol)
    {
        SymbolImpl& uid = symbol.uid();
        String description = uid.isNullSymbol() ? "undefined"_s : jsonString(String { &uid });
        StringBuilder line;
        line.append("symbol "_s, description);
        // A registered symbol's Symbol.for key is its description.
        if (uid.isRegistered())
            line.append(" for "_s, description);
        if (uid.isPrivate())
            line.append(" private"_s);
        return line.toString();
    }

    static ASCIILiteral scopeKind(JSLexicalEnvironment& environment)
    {
        if (is<JSModuleEnvironment>(&environment))
            return "module"_s;
        switch (environment.symbolTable()->scopeType()) {
        case SymbolTable::ScopeType::VarScope:
            return "var"_s;
        case SymbolTable::ScopeType::GlobalLexicalScope:
            return "global-lexical"_s;
        case SymbolTable::ScopeType::LexicalScope:
            return "lexical"_s;
        case SymbolTable::ScopeType::CatchScope:
        case SymbolTable::ScopeType::CatchScopeWithSimpleParameter:
            return "catch"_s;
        case SymbolTable::ScopeType::FunctionNameScope:
            return "function-name"_s;
        }
        RELEASE_ASSERT_NOT_REACHED();
        return "lexical"_s;
    }

    bool appendNextScope(JSScope& scopeObject, Vector<Line>& edges)
    {
        JSScope* next = scopeObject.next();
        Line line { text("[[NextScope]]"_s) };
        if (!meet(next ? JSValue(next) : jsNull(), line))
            return false;
        edges.append(WTF::move(line));
        return true;
    }

    bool describeWithScope(unsigned ordinal, JSWithScope& withScope)
    {
        Vector<Line> edges;
        Line object { text("[[Object]]"_s) };
        if (!meet(withScope.object(), object))
            return false;
        edges.append(WTF::move(object));
        if (!appendNextScope(withScope, edges))
            return false;
        m_records[ordinal].line = scopeLine("with"_s);
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    // A scope's line, in place of the any-object line.
    static String scopeLine(ASCIILiteral kind)
    {
        return makeString("scope "_s, kind);
    }

    // Each variable the symbol table places in the scope, sorted by name, then the next scope.
    template<typename ScopeType>
    bool describeScope(unsigned ordinal, ScopeType& scopeObject, ASCIILiteral kind)
    {
        Vector<std::pair<String, ScopeOffset>> variables;
        {
            SymbolTable* table = scopeObject.symbolTable();
            ConcurrentJSLocker locker(table->m_lock);
            for (auto iterator = table->begin(locker), end = table->end(locker); iterator != end; ++iterator) {
                if (iterator->value.isNull())
                    continue;
                VarOffset offset = iterator->value.varOffset();
                if (!offset.isScope())
                    continue;
                variables.append({ String { iterator->key.get() }, offset.scopeOffset() });
            }
        }
        std::stable_sort(variables.begin(), variables.end(), [](const auto& a, const auto& b) {
            return codePointCompareLessThan(a.first, b.first);
        });

        Vector<Line> edges;
        for (auto& [name, offset] : variables) {
            Line line { text("variable"_s), text(jsonString(name)), text("="_s) };
            if (!meet(scopeObject.variableAt(offset).get(), line))
                return false;
            edges.append(WTF::move(line));
        }
        if (!appendNextScope(scopeObject, edges))
            return false;
        m_records[ordinal].line = scopeLine(kind);
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    static String attributesText(unsigned attributes, bool isAccessor)
    {
        // The three attributes JavaScript sees; an accessor has no writable one.
        std::array<char, 3> letters {
            !isAccessor && !(attributes & PropertyAttribute::ReadOnly) ? 'w' : '-',
            !(attributes & PropertyAttribute::DontEnum) ? 'e' : '-',
            !(attributes & PropertyAttribute::DontDelete) ? 'c' : '-',
        };
        return String { std::span<const char> { letters } };
    }

    // The own properties in the order getOwnPropertyNames gives them, private names included, each read with a VMInquiry
    // slot, which calls no getter, trap or host accessor.
    bool appendOwnProperties(JSObject& object, bool skipsIndexedProperties, Vector<Line>& edges)
    {
        VM& vm = m_vm;
        auto scope = DECLARE_THROW_SCOPE(vm);

        PropertyNameArrayBuilder names(vm, PropertyNameMode::StringsAndSymbols, PrivateSymbolMode::Include);
        object.methodTable()->getOwnPropertyNames(&object, &m_globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, false);

        for (unsigned nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
            const Identifier& name = names[nameIndex];
            std::optional<uint32_t> index = parseIndex(name);
            if (index && skipsIndexedProperties)
                continue;
            // The global object's own `arguments` stays out whichever way the walk meets it: directly, or through the
            // JSGlobalProxy its globalThis property holds, which forwards every own-property read to it.
            if ((&object == &m_globalObject || &object == m_globalObject.globalThis()) && name == vm.propertyNames->arguments)
                continue;

            Line line { text("property"_s) };
            if (name.isSymbol()) {
                // The symbol cell JavaScript would get for this key, the one the VM keeps for its uid.
                if (!meet(Symbol::create(vm, static_cast<SymbolImpl&>(*name.impl())), line))
                    return false;
            } else
                line.append(text(jsonString(name.string())));

            PropertySlot slot(&object, PropertySlot::InternalMethodType::VMInquiry, &vm);
            bool found = index
                ? object.methodTable()->getOwnPropertySlotByIndex(&object, &m_globalObject, *index, slot)
                : object.methodTable()->getOwnPropertySlot(&object, &m_globalObject, name, slot);
            RETURN_IF_EXCEPTION(scope, false);
            if (!found) {
                line.append(text("<opaque>"_s));
                edges.append(WTF::move(line));
                continue;
            }

            line.append(text(attributesText(slot.attributes(), slot.isAccessor())));
            if (slot.isValue()) {
                line.append(text("="_s));
                if (!meet(slot.getValue(&m_globalObject, name), line))
                    return false;
            } else if (slot.isAccessor()) {
                GetterSetter* accessor = slot.getterSetter();
                line.append(text("get"_s));
                if (!meet(accessor->isGetterNull() ? jsUndefined() : JSValue(accessor->getter()), line))
                    return false;
                line.append(text("set"_s));
                if (!meet(accessor->isSetterNull() ? jsUndefined() : JSValue(accessor->setter()), line))
                    return false;
            } else {
                line.append(text("="_s));
                line.append(text("<custom>"_s));
            }
            edges.append(WTF::move(line));
        }
        return true;
    }

    // The value of the binding an export resolves to, read from its module environment so that nothing is materialized;
    // an empty binding is the empty value. Nothing when the export resolves to no binding of an environment.
    static std::optional<JSValue> exportBindingValue(const AbstractModuleRecord::Resolution& resolution)
    {
        if (resolution.type != AbstractModuleRecord::Resolution::Type::Resolved || !resolution.moduleRecord)
            return std::nullopt;
        JSModuleEnvironment* environment = resolution.moduleRecord->moduleEnvironmentMayBeNull();
        if (!environment)
            return std::nullopt;
        ScopeOffset offset;
        {
            SymbolTable* table = environment->symbolTable();
            ConcurrentJSLocker locker(table->m_lock);
            auto iterator = table->find(locker, resolution.localName.impl());
            if (iterator == table->end(locker) || iterator->value.isNull() || !iterator->value.varOffset().isScope())
                return std::nullopt;
            offset = iterator->value.varOffset().scopeOffset();
        }
        return environment->variableAt(offset).get();
    }

    bool appendExports(JSModuleNamespaceObject& namespaceObject, Vector<Line>& edges)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);

        if (exportsNeedEvaluation(namespaceObject)) {
            edges.append(Line { text("exports"_s), text("<unevaluated>"_s) });
            return true;
        }

        PropertyNameArrayBuilder names(m_vm, PropertyNameMode::Strings, PrivateSymbolMode::Exclude);
        namespaceObject.methodTable()->getOwnPropertyNames(&namespaceObject, &m_globalObject, names, DontEnumPropertiesMode::Include);
        RETURN_IF_EXCEPTION(scope, false);

        AbstractModuleRecord* moduleRecord = namespaceObject.moduleRecord();
        for (unsigned nameIndex = 0; nameIndex < names.size(); ++nameIndex) {
            const Identifier& name = names[nameIndex];
            AbstractModuleRecord::Resolution resolution = moduleRecord->resolveExport(&m_globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            Line line { text("export"_s), text(jsonString(name.string())), text("="_s) };
            std::optional<JSValue> value = exportBindingValue(resolution);
            if (!value)
                line.append(text("<opaque>"_s));
            else if (!meet(*value, line))
                return false;
            edges.append(WTF::move(line));
        }
        return true;
    }

    bool appendEdge(Vector<Line>& edges, ASCIILiteral label, JSValue value)
    {
        Line line { text(label) };
        if (!meet(value, line))
            return false;
        edges.append(WTF::move(line));
        return true;
    }

    // The internal fields of a JSInternalFieldObjectImpl the rows above do not describe (iterators, generators). Nothing
    // when the object is not of the class; false when an allocation threw.
    template<typename CellType>
    std::optional<bool> appendInternalFieldsOf(JSObject& object, Vector<Line>& edges)
    {
        auto* cell = dynamicDowncast<CellType>(&object);
        if (!cell)
            return std::nullopt;
        // Through the base, since several classes hide its indexed accessor behind one taking their Field enumeration.
        using Fields = JSInternalFieldObjectImpl<CellType::numberOfInternalFields>;
        const Fields& fields = *cell;
        for (unsigned index = 0; index < Fields::numberOfInternalFields; ++index) {
            Line line { text("[[InternalField]]"_s), text(String::number(index)), text("="_s) };
            if (!meet(fields.internalField(index).get(), line))
                return false;
            edges.append(WTF::move(line));
        }
        return true;
    }

    template<typename... CellTypes>
    bool appendInternalFields(JSObject& object, Vector<Line>& edges)
    {
        // The first class the object is of stops the fold.
        std::optional<bool> result;
        static_cast<void>(((result = appendInternalFieldsOf<CellTypes>(object, edges)) || ...));
        return result.value_or(true);
    }

    // The five classes whose object wraps a primitive. JSWrapperObject declares no ClassInfo of its own, so a cast to it
    // would test its base's and accept every JSNonFinalObject; each subclass declares its own.
    static JSWrapperObject* primitiveWrapper(JSObject& object)
    {
        if (auto* number = dynamicDowncast<NumberObject>(&object))
            return number;
        if (auto* string = dynamicDowncast<StringObject>(&object))
            return string;
        if (auto* boolean = dynamicDowncast<BooleanObject>(&object))
            return boolean;
        if (auto* symbol = dynamicDowncast<SymbolObject>(&object))
            return symbol;
        if (auto* bigInt = dynamicDowncast<BigIntObject>(&object))
            return bigInt;
        return nullptr;
    }

    // A deferred namespace runs its module's evaluation at the first look at its keys, unless the module's cycle already
    // evaluated without an error (JSModuleNamespaceObject::ensureDeferredNamespaceEvaluation). JavaScript cannot list such
    // a namespace's exports without running that evaluation, so the walk lists them only once it ran. The namespace's own
    // toStringTag tells it apart, since its deferral has no accessor.
    bool exportsNeedEvaluation(JSModuleNamespaceObject& namespaceObject)
    {
        JSValue tag = namespaceObject.getDirect(m_vm, m_vm.propertyNames->toStringTagSymbol);
        // finishCreation stores the tag as a flat string, which needs no resolution.
        StringImpl* tagImpl = tag.isString() ? asString(tag)->tryGetValueImpl() : nullptr;
        if (!tagImpl || !WTF::equal(tagImpl, "Deferred Module"_s))
            return false;
        auto* cyclic = dynamicDowncast<CyclicModuleRecord>(namespaceObject.moduleRecord());
        if (!cyclic)
            return true;
        CyclicModuleRecord* root = cyclic->cycleRoot() ? cyclic->cycleRoot() : cyclic;
        return root->status() != CyclicModuleRecord::Status::Evaluated || root->evaluationError();
    }

    // The any-object line, to which the rows below it append. Extensibility is the structure's, so no trap runs.
    static void appendObjectLine(StringBuilder& line, JSObject& object)
    {
        line.append("object "_s, object.classInfo()->className, " callable="_s, flagText(object.isCallable()),
            " constructor="_s, flagText(object.isConstructor()), " extensible="_s, flagText(object.isStructureExtensible()));
    }

    bool describeObject(unsigned ordinal, JSObject& object)
    {
        VM& vm = m_vm;
        auto scope = DECLARE_THROW_SCOPE(vm);

        StringBuilder line;
        appendObjectLine(line, object);
        Vector<Line> edges;
        auto* view = dynamicDowncast<JSArrayBufferView>(&object);

        if (auto* proxy = dynamicDowncast<ProxyObject>(&object)) {
            // Target and handler in place of the prototype and own properties, which only traps could read; both are null
            // once the proxy is revoked, as JavaScript sees them.
            bool revoked = proxy->isRevoked();
            if (!appendEdge(edges, "[[ProxyTarget]]"_s, revoked ? jsNull() : JSValue(proxy->target())))
                return false;
            if (!appendEdge(edges, "[[ProxyHandler]]"_s, revoked ? jsNull() : proxy->handler()))
                return false;
        } else {
            if (!appendEdge(edges, "[[Prototype]]"_s, object.getPrototypeDirect()))
                return false;
            if (auto* namespaceObject = dynamicDowncast<JSModuleNamespaceObject>(&object)) {
                if (!appendExports(*namespaceObject, edges))
                    return false;
            } else if (!appendOwnProperties(object, !!view, edges))
                return false;
        }

        if (auto* function = dynamicDowncast<JSFunction>(&object)) {
            if (auto* bound = dynamicDowncast<JSBoundFunction>(function)) {
                if (!appendEdge(edges, "[[BoundTargetFunction]]"_s, bound->targetFunction()))
                    return false;
                if (!appendEdge(edges, "[[BoundThis]]"_s, bound->boundThis()))
                    return false;
                bool succeeded = true;
                unsigned argumentIndex = 0;
                bound->forEachBoundArg([&](JSValue argument) {
                    Line argumentLine { text("[[BoundArgument]]"_s), text(String::number(argumentIndex++)), text("="_s) };
                    succeeded = meet(argument, argumentLine);
                    if (!succeeded)
                        return IterationStatus::Done;
                    edges.append(WTF::move(argumentLine));
                    return IterationStatus::Continue;
                });
                if (!succeeded)
                    return false;
            } else if (!function->isHostFunction()) {
                FunctionExecutable* executable = function->jsExecutable();
                if (executable->isBuiltinFunction())
                    line.append(" builtin "_s, jsonString(executable->name().string()));
                else
                    line.append(" source "_s, jsonString(executable->sourceURL()), ' ', executable->source().startOffset(), '-', executable->source().endOffset());
                if (!appendEdge(edges, "[[Scope]]"_s, function->scope()))
                    return false;
            } else
                line.append(" name "_s, jsonString(function->name(vm)));
        } else if (auto* internalFunction = dynamicDowncast<InternalFunction>(&object))
            line.append(" name "_s, jsonString(internalFunction->name()));
        else if (auto* map = dynamicDowncast<JSMap>(&object)) {
            if (map->storage()) {
                auto& storage = map->storageRef();
                for (JSMap::Helper::Entry entry = 0;; ++entry) {
                    auto result = JSMap::Helper::transitAndNext(vm, storage, entry);
                    if (!result.storage)
                        break;
                    Line entryLine { text("entry"_s) };
                    if (!meet(result.key, entryLine))
                        return false;
                    entryLine.append(text("=>"_s));
                    if (!meet(result.value, entryLine))
                        return false;
                    edges.append(WTF::move(entryLine));
                    entry = result.entry;
                }
            }
        } else if (auto* set = dynamicDowncast<JSSet>(&object)) {
            if (set->storage()) {
                auto& storage = set->storageRef();
                for (JSSet::Helper::Entry entry = 0;; ++entry) {
                    auto result = JSSet::Helper::transitAndNext(vm, storage, entry);
                    if (!result.storage)
                        break;
                    Line entryLine { text("entry"_s) };
                    if (!meet(result.key, entryLine))
                        return false;
                    edges.append(WTF::move(entryLine));
                    entry = result.entry;
                }
            }
        } else if (auto* weakMap = dynamicDowncast<JSWeakMap>(&object)) {
            MarkedArgumentBuffer snapshot;
            weakMap->takeSnapshot(snapshot);
            Record& record = m_records[ordinal];
            record.isWeakContainer = true;
            record.weakEntriesHaveValues = true;
            for (size_t index = 0; index + 1 < snapshot.size(); index += 2)
                record.weakEntries.append({ snapshot.at(index).asCell(), snapshot.at(index + 1) });
            record.weakEntryMet.grow(record.weakEntries.size());
            record.weakEntryMet.fill(false);
        } else if (auto* weakSet = dynamicDowncast<JSWeakSet>(&object)) {
            MarkedArgumentBuffer snapshot;
            weakSet->takeSnapshot(snapshot);
            Record& record = m_records[ordinal];
            record.isWeakContainer = true;
            for (size_t index = 0; index < snapshot.size(); ++index)
                record.weakEntries.append({ snapshot.at(index).asCell(), JSValue() });
            record.weakEntryMet.grow(record.weakEntries.size());
            record.weakEntryMet.fill(false);
        } else if (auto* weakRef = dynamicDowncast<JSWeakObjectRef>(&object)) {
            // deref is the only reader the class has; it keeps the target alive for the rest of the job, as
            // WeakRef.prototype.deref does.
            Line target { text("[[WeakRefTarget]]"_s), cellItem(ItemKind::WeakCell, weakRef->deref(vm)) };
            edges.append(WTF::move(target));
        } else if (auto* registry = dynamicDowncast<JSFinalizationRegistry>(&object)) {
            if (!appendEdge(edges, "[[CleanupCallback]]"_s, registry->internalField(JSFinalizationRegistry::Field::Callback).get()))
                return false;
            Vector<std::pair<JSValue, JSCell*>> registrations;
            {
                Locker locker { registry->cellLock() };
                for (auto& live : registry->liveRegistrations(locker))
                    registrations.append({ live.heldValue, live.unregisterToken });
                for (auto& dead : registry->deadRegistrations(locker))
                    registrations.append({ dead.heldValue, dead.unregisterToken });
            }
            for (auto& [heldValue, token] : registrations) {
                Item heldItem = text({ });
                Item tokenItem = text("undefined"_s);
                if (!listedItem(heldValue, heldItem))
                    return false;
                if (token)
                    tokenItem = cellItem(ItemKind::ListedCell, token);
                m_records[ordinal].registrations.append({ WTF::move(heldItem), WTF::move(tokenItem) });
            }
        } else if (auto* promise = dynamicDowncast<JSPromise>(&object)) {
            switch (promise->status()) {
            case JSPromise::Status::Pending:
                line.append(" status pending"_s);
                break;
            case JSPromise::Status::Fulfilled:
                line.append(" status fulfilled"_s);
                break;
            case JSPromise::Status::Rejected:
                line.append(" status rejected"_s);
                break;
            }
            // Its result, not its reactions, which JavaScript observes only through what they run once it settles.
            if (!appendEdge(edges, "[[PromiseResult]]"_s, promise->result()))
                return false;
        } else if (JSWrapperObject* wrapper = primitiveWrapper(object)) {
            if (!appendEdge(edges, "[[PrimitiveValue]]"_s, wrapper->internalValue()))
                return false;
        } else if (auto* date = dynamicDowncast<DateInstance>(&object))
            line.append(" time "_s, numberText(date->internalNumber()));
        else if (auto* regExpObject = dynamicDowncast<RegExpObject>(&object)) {
            RegExp* regExp = regExpObject->regExp();
            auto flags = Yarr::flagsString(regExp->flags());
            line.append(" pattern "_s, jsonString(regExp->pattern()), " flags "_s, jsonString(String::fromLatin1(flags.data())));
        } else if (auto* arrayBuffer = dynamicDowncast<JSArrayBuffer>(&object)) {
            ArrayBuffer* buffer = arrayBuffer->impl();
            line.append(" shared="_s, flagText(buffer->isShared()), " detached="_s, flagText(buffer->isDetached()), " byteLength="_s, buffer->byteLength());
            StringBuilder bytes;
            for (uint8_t byte : buffer->span())
                bytes.append(hex(byte, 2, Lowercase));
            Line bytesLine { text("bytes"_s), text(bytes.toString()) };
            edges.append(WTF::move(bytesLine));
        } else if (view) {
            line.append(" byteOffset="_s, view->byteOffset(), " length="_s, view->length());
            // The buffer, in place of the indexed own properties appendOwnProperties left out.
            JSArrayBuffer* buffer = view->possiblySharedJSBuffer(&m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (!appendEdge(edges, "[[ViewedArrayBuffer]]"_s, buffer ? JSValue(buffer) : jsNull()))
                return false;
        } else if (!appendInternalFields<JSArrayIterator, JSMapIterator, JSSetIterator, JSStringIterator, JSRegExpStringIterator,
            JSIteratorHelper, JSWrapForValidIterator, JSGenerator, JSAsyncGenerator, JSAsyncFunctionGenerator, JSDisposableStack,
            JSAsyncDisposableStack, AbstractModuleRecord, InternalFieldTuple>(object, edges))
            return false;

        m_records[ordinal].line = line.toString();
        m_records[ordinal].edges = WTF::move(edges);
        return true;
    }

    // One round of section 6.3: in each weak container, in ordinal order, the entries whose key the walk has reached and
    // that no earlier round met, in their keys' ordinal order. Their values are walked like any strong edge.
    bool meetReachedWeakEntries()
    {
        size_t recordCount = m_records.size();
        for (size_t ordinal = 0; ordinal < recordCount; ++ordinal) {
            if (!m_records[ordinal].isWeakContainer)
                continue;
            Vector<std::pair<unsigned, size_t>> reached; // the key's ordinal, the entry's index
            for (size_t index = 0; index < m_records[ordinal].weakEntries.size(); ++index) {
                if (m_records[ordinal].weakEntryMet[index])
                    continue;
                auto iterator = m_ordinals.find(m_records[ordinal].weakEntries[index].first);
                if (iterator != m_ordinals.end())
                    reached.append({ iterator->value, index });
            }
            std::ranges::sort(reached);
            for (auto [keyOrdinal, index] : reached) {
                auto [key, value] = m_records[ordinal].weakEntries[index];
                Line line { text("weak-entry"_s), cellItem(ItemKind::Cell, key) };
                if (m_records[ordinal].weakEntriesHaveValues) {
                    line.append(text("=>"_s));
                    if (!meet(value, line))
                        return false;
                }
                m_records[ordinal].weakEntryMet[index] = true;
                m_records[ordinal].weakEntryLines.append({ keyOrdinal, WTF::move(line) });
            }
        }
        return true;
    }

    String itemText(const Item& item) const
    {
        if (item.kind == ItemKind::Text)
            return item.text;
        auto iterator = item.cell ? m_ordinals.find(item.cell) : m_ordinals.end();
        if (iterator != m_ordinals.end())
            return makeString('#', iterator->value);
        // A strong edge always gives its cell an ordinal.
        ASSERT(item.kind != ItemKind::Cell);
        return item.kind == ItemKind::WeakCell ? "<unreached>"_s : "<cell>"_s;
    }

    void appendLine(StringBuilder& output, const Line& line) const
    {
        output.append("  "_s);
        for (size_t index = 0; index < line.size(); ++index)
            output.append(index ? " "_s : ""_s, itemText(line[index]));
        output.append('\n');
    }

    String output(const Line& rootLine)
    {
        // The private brands each object carries, tested against every private symbol the walk reached, in their ordinal
        // order: BrandedStructure exposes its brands only through checkBrand.
        Vector<Symbol*> privateSymbols;
        for (auto& record : m_records) {
            if (auto* symbol = dynamicDowncast<Symbol>(record.cell); symbol && symbol->uid().isPrivate())
                privateSymbols.append(symbol);
        }

        StringBuilder output;
        output.append(rootLine.first().text);
        for (size_t index = 1; index < rootLine.size(); ++index)
            output.append(' ', itemText(rootLine[index]));
        output.append('\n');

        for (size_t ordinal = 0; ordinal < m_records.size(); ++ordinal) {
            Record& record = m_records[ordinal];
            output.append('#', ordinal, ' ', record.line, '\n');
            for (auto& edge : record.edges)
                appendLine(output, edge);

            std::ranges::sort(record.weakEntryLines, { }, [](const auto& entry) { return entry.first; });
            for (auto& [keyOrdinal, line] : record.weakEntryLines)
                appendLine(output, line);

            Vector<String> registrations;
            for (auto& [heldItem, tokenItem] : record.registrations)
                registrations.append(makeString("registration held "_s, itemText(heldItem), " token "_s, itemText(tokenItem)));
            std::ranges::sort(registrations, [](const String& a, const String& b) { return codePointCompareLessThan(a, b); });
            for (auto& registration : registrations)
                output.append("  "_s, registration, '\n');

            auto* object = dynamicDowncast<JSObject>(record.cell);
            if (!object || !object->structure()->isBrandedStructure())
                continue;
            auto* branded = uncheckedDowncast<BrandedStructure>(object->structure());
            for (Symbol* symbol : privateSymbols) {
                if (branded->checkBrand(symbol))
                    output.append("  brand #"_s, m_ordinals.get(symbol), '\n');
            }
        }
        return output.toString();
    }

    JSGlobalObject& m_globalObject;
    VM& m_vm;
    Vector<JSCell*, 3> m_cellsWithoutEdges; // GlobalScope::WithoutEdges: the global scope's cells that are no root
    UncheckedKeyHashMap<JSCell*, unsigned> m_ordinals;
    Vector<Record> m_records; // by ordinal
    size_t m_nextToDescribe { 0 };
};

static JSObject* ucbStatisticsObject(JSGlobalObject* globalObject, const UCBStatistics& statistics, const UCBRegistry::Counts& counts)
{
    VM& vm = globalObject->vm();
    auto put = [&](JSObject* object, ASCIILiteral name, JSValue value) {
        object->putDirect(vm, Identifier::fromString(vm, name), value);
    };
    auto indexedCounters = [&](std::span<const ASCIILiteral> names, std::span<const uint64_t> values) {
        JSObject* object = constructEmptyObject(globalObject);
        for (size_t index = 0; index < names.size(); ++index)
            put(object, names[index], countValue(values[index]));
        return object;
    };

    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_STATISTIC(field) put(result, #field ""_s, countValue(statistics.field));
    JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC(JITCACHE_HARNESS_PUT_STATISTIC)
#undef JITCACHE_HARNESS_PUT_STATISTIC
    put(result, "misses"_s, indexedCounters(missReasonNames, statistics.misses));
    put(result, "records"_s, indexedCounters(originNames, statistics.records));
    put(result, "invalidMaterial"_s, indexedCounters(invalidMaterialNames, statistics.invalidMaterial));

    JSObject* registryCounts = constructEmptyObject(globalObject);
    put(registryCounts, "children"_s, countValue(counts.children));
    put(registryCounts, "roots"_s, countValue(counts.roots));
    put(registryCounts, "codeBlocks"_s, countValue(counts.codeBlocks));
    put(registryCounts, "pendingImports"_s, countValue(counts.pendingImports));
    put(result, "counts"_s, registryCounts);
    return result;
}

} // namespace JITCacheTwinsHarnessInternal

void setForcesBlindingForTesting(bool forces)
{
    JITCacheTwinsHarnessInternal::forcedBlinding = forces;
}

bool forcesBlindingForTesting()
{
    return JITCacheTwinsHarnessInternal::forcedBlinding;
}

bool setImageTestHookNamed(const char* name)
{
    StringView hookName = StringView::fromLatin1(name);
#if ENABLE(JIT)
    static constexpr std::array<std::pair<ASCIILiteral, ImageTestHook>, 4> hooks { {
        { "relocation-pairs"_s, ImageTestHook::RelocationPairs },
        { "operation-pair"_s, ImageTestHook::OperationPair },
        { "change-recorded-target"_s, ImageTestHook::ChangeRecordedTarget },
        { "skip-patch"_s, ImageTestHook::SkipPatch },
    } };
    for (auto& [hookFlagName, hook] : hooks) {
        if (hookName == hookFlagName) {
            setImageTestHook(hook);
            return true;
        }
    }
#else
    UNUSED_PARAM(hookName);
#endif
    return false;
}

bool setStoreFaultFlag(const char* value)
{
    // <call>:<error>[@<n>]; bodyVersion makes no system call, so openBody and scoring are the calls a hook can fail.
    StringView text = StringView::fromLatin1(value);
    size_t colon = text.find(':');
    if (colon == notFound)
        return false;
    StringView callName = text.left(colon);
    StringView rest = text.substring(colon + 1);
    size_t at = rest.find('@');
    StringView errorName = at == notFound ? rest : rest.left(at);

    StoreTesting::Call call;
    if (callName == "openBody"_s)
        call = StoreTesting::Call::Open;
    else if (callName == "scoring"_s)
        call = StoreTesting::Call::ReadSavedSummaries;
    else
        return false;

    int error;
    if (errorName == "EMFILE"_s)
        error = EMFILE;
    else if (errorName == "ENFILE"_s)
        error = ENFILE;
    else if (errorName == "ENOMEM"_s)
        error = ENOMEM;
    else if (errorName == "EIO"_s)
        error = EIO;
    else
        return false;

    std::optional<uint64_t> count;
    if (at != notFound) {
        count = JITCacheTwinsHarnessInternal::parseCount(rest.substring(at + 1));
        if (!count)
            return false;
    }
    StoreTesting::setFault(StoreTesting::Fault { call, error, count });
    return true;
}

bool setWriterFaultFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto check = ArtifactWriter::faultCheckNamed(text.left(at));
    auto count = JITCacheTwinsHarnessInternal::parseCount(text.substring(at + 1));
    if (!check || !count)
        return false;
    JITCacheTwinsHarnessInternal::writerFault = ArtifactWriter::FaultForTesting { *check, *count };
    return true;
}

bool setKillFlag(const char* value)
{
    StringView text = StringView::fromLatin1(value);
    size_t at = text.reverseFind('@');
    if (at == notFound)
        return false;
    auto point = ArtifactWriter::killPointNamed(text.left(at));
    auto count = JITCacheTwinsHarnessInternal::parseCount(text.substring(at + 1));
    if (!point || !count)
        return false;
    JITCacheTwinsHarnessInternal::writerKill = ArtifactWriter::KillForTesting { *point, *count };
    return true;
}

bool setTwinEntryFlag(const char* value)
{
    using TwinEntry = JITCacheTwinsHarnessInternal::TwinEntry;
    StringView kind = StringView::fromLatin1(value);
    if (kind == "difference"_s) {
        JITCacheTwinsHarnessInternal::twinEntry = TwinEntry { TwinEntry::Kind::Difference, RelocationDomain::Heap };
        return true;
    }
    if (kind == "skip"_s) {
        JITCacheTwinsHarnessInternal::twinEntry = TwinEntry { TwinEntry::Kind::Skip, RelocationDomain::Heap };
        return true;
    }
    static constexpr ASCIILiteral coincidencePrefix = "coincidence:"_s;
    if (!kind.startsWith(StringView { coincidencePrefix }))
        return false;
    // The domains as the twin report spells them (harness sub-SPEC section 2).
    static constexpr std::array<std::pair<ASCIILiteral, RelocationDomain>, 4> domains { {
        { "engine-image"_s, RelocationDomain::EngineImage },
        { "executable-pool"_s, RelocationDomain::ExecutablePool },
        { "structure-reservation"_s, RelocationDomain::StructureReservation },
        { "heap"_s, RelocationDomain::Heap },
    } };
    StringView domainName = kind.substring(coincidencePrefix.length());
    for (auto& [name, domain] : domains) {
        if (domainName == name) {
            JITCacheTwinsHarnessInternal::twinEntry = TwinEntry { TwinEntry::Kind::Coincidence, domain };
            return true;
        }
    }
    return false;
}

void didStartShellVM(VM& vm, StartOutcome outcome, bool logsResults)
{
    using namespace JITCacheTwinsHarnessInternal;
    shellStart = { &vm, outcome, logsResults };

    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // Container sub-SPEC section 8.3 and harness sub-SPEC section 12: the hooks count this writer's commits from 1.
    if (ArtifactWriter* writer = state->writer()) {
        writer->setFaultForTesting(writerFault);
        writer->setKillForTesting(writerKill);
    }
    // H1's test entry, of part Integrator and check test-entry.
    TwinReportSink* report = state->twinReportSink();
    if (!twinEntry || !report)
        return;
    static constexpr ASCIILiteral detail = "--jitcache-test-twin-entry"_s;
    switch (twinEntry->kind) {
    case TwinEntry::Kind::Difference:
        report->difference(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Skip:
        report->skip(TwinPart::Integrator, "test-entry"_s, detail);
        return;
    case TwinEntry::Kind::Coincidence:
        report->relocationCoincidence(twinEntry->domain, detail);
        return;
    }
}

// Section 4: placement. The layout files write every value in lowercase hex without a prefix, so the fixed probes read
// "heap 1 2 3" as section 4 spells them; readLayoutRanges also accepts a 0x prefix.

void placeholdersBeforeInitialize(const Vector<String>& layoutPaths)
{
    using namespace JITCacheTwinsHarnessInternal;
    for (auto& path : layoutPaths) {
        for (auto range : readLayoutRanges(path))
            placePlaceholders(path, range);
    }
}

void releasePlaceholdersAfterInitialize()
{
    using namespace JITCacheTwinsHarnessInternal;
    for (auto placeholder : placeholders()) {
        if (munmap(reinterpret_cast<void*>(placeholder.start), placeholder.end - placeholder.start) < 0)
            exitAfterError(String { mapsPath }, errno);
    }
    placeholders().clear();
}

void recordLayout(const String& path)
{
    uintptr_t poolStart = reinterpret_cast<uintptr_t>(g_jscConfig.startExecutableMemory);
    uintptr_t poolEnd = reinterpret_cast<uintptr_t>(g_jscConfig.endExecutableMemory);
    String lines = makeString(
        "pool "_s, hex(poolStart, Lowercase), ' ', hex(poolEnd, Lowercase), '\n',
        "structures "_s, hex(g_jscConfig.startOfStructureHeap, Lowercase), ' ', hex(g_jscConfig.sizeOfStructureHeap, Lowercase), '\n');
    JITCacheTwinsHarnessInternal::writeTestFile(path, lines.utf8(), JITCacheTwinsHarnessInternal::WriteMode::Replace);
}

void recordVMLayout(VM& vm, const String& path, HeapProbes probes)
{
    // Three kinds of heap target an image names: the VM's fields, its cells and the atoms. A heap that never moves would
    // repeat the fixed values (H1).
    uintptr_t vmProbe = 1;
    uintptr_t cellProbe = 2;
    uintptr_t atomProbe = 3;
    if (probes == HeapProbes::Live) {
        vmProbe = reinterpret_cast<uintptr_t>(&vm);
        cellProbe = reinterpret_cast<uintptr_t>(jsEmptyString(vm));
        atomProbe = reinterpret_cast<uintptr_t>(vm.propertyNames->length.impl());
    }
    String line = makeString("heap "_s, hex(vmProbe, Lowercase), ' ', hex(cellProbe, Lowercase), ' ', hex(atomProbe, Lowercase), '\n');
    JITCacheTwinsHarnessInternal::writeTestFile(path, line.utf8(), JITCacheTwinsHarnessInternal::WriteMode::Append);
}

// Section 10.3: the body-event dump.
void writeBodyEvents(VM& vm, const String& path)
{
    using namespace JITCacheTwinsHarnessInternal;
    JSLockHolder locker(vm);

    // A UCB a collection found dead retires its counts only when a sweep destroys it: the epilogue of a collection the
    // collector thread ended sweeps the precise allocations, and the synchronous sweep every block. Neither starts a
    // collection.
    vm.heap.stopIfNecessary();
    vm.heap.sweepSynchronously();

    struct KeyedCounts {
        BodyKey key;
        BodyEventCounts counts;
    };
    Vector<KeyedCounts> keyed;
    BodyEventCounts total = retiredBodyEvents(vm);
    VMState* state = vm.jitCacheState();
    {
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* cell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return IterationStatus::Continue;
            auto* codeBlock = dynamicDowncast<UnlinkedCodeBlock>(static_cast<JSCell*>(cell));
            if (!codeBlock)
                return IterationStatus::Continue;
            const BodyEventCounts& counts = codeBlock->jitCacheEventCounts();
            addBodyEventCounts(total, counts);
            // Keys exist only in a VM start configured.
            if (state && hasBodyEvents(counts)) {
                if (auto key = state->registry().keyOf(*codeBlock))
                    keyed.append({ *key, counts });
            }
            return IterationStatus::Continue;
        });
    }

    StringBuilder dump;
    for (auto& [key, counts] : keyed) {
        dump.append("{\"key\":\""_s, bodyKeyHex(key), "\","_s);
        appendBodyEventCounts(dump, counts);
        dump.append("}\n"_s);
    }
    dump.append("{\"total\":{"_s);
    appendBodyEventCounts(dump, total);
    dump.append("}}\n"_s);
    writeTestFile(path, dump.toString().utf8(), WriteMode::Replace);
}

// Section 6: the reachable-heap description.
String describeReachableHeap(JSGlobalObject& globalObject, std::span<const JSValue> roots)
{
    using namespace JITCacheTwinsHarnessInternal;
    VM& vm = globalObject.vm();
    JSLockHolder locker(vm);
    vm.heap.collectNow(Sync, CollectionScope::Full);
    // No collection interleaves with the walk, so the cells it holds by pointer stay alive, even while a host's reification
    // runs JavaScript inside it (section 6.1).
    DeferGCForAWhile deferGC(vm);

    HeapDescription description(globalObject);
    if (!roots.empty())
        return description.describe(roots, HeapDescription::GlobalScope::WithoutEdges);

    // The default roots: the global object, its global lexical environment and the module environment of each module
    // record the loader holds, in key order.
    Vector<JSValue> defaultRoots;
    defaultRoots.append(&globalObject);
    defaultRoots.append(globalObject.globalLexicalEnvironment());
    Vector<ModuleRegistryEntry*> entries;
    for (auto& entry : globalObject.moduleLoader()->moduleMap())
        entries.append(entry.value.get());
    std::ranges::sort(entries, [](ModuleRegistryEntry* a, ModuleRegistryEntry* b) {
        if (a->key() != b->key())
            return codePointCompareLessThan(a->key().string(), b->key().string());
        return a->moduleType() < b->moduleType();
    });
    for (auto* entry : entries) {
        AbstractModuleRecord* record = entry->record();
        if (JSModuleEnvironment* environment = record ? record->moduleEnvironmentMayBeNull() : nullptr)
            defaultRoots.append(environment);
    }
    return description.describe(defaultRoots.span(), HeapDescription::GlobalScope::Walked);
}

void writeReachableHeapDescription(JSGlobalObject& globalObject, const String& path)
{
    VM& vm = globalObject.vm();
    JSLockHolder locker(vm);
    // The run is over, and the shell has recorded the termination that ended it, if one did, by forbidding execution
    // (SPEC-integrator.md section 11.1). Its exception, or a request or trap still pending, would stop the walk at its
    // first exception check, and putting the exception back afterwards would need the request that the exit of the
    // outermost VM entry scope withdrew. So the description withdraws the termination for good; nothing the shell runs
    // afterwards reads it.
    vm.cancelTermination();
    // A description is never empty, so a null one is a walk that threw.
    String description = describeReachableHeap(globalObject, { });
    if (description.isNull()) {
        SAFE_FPRINTF(stderr, "JITCache: %s: describing the heap threw\n", path.utf8());
        exit(1);
    }
    JITCacheTwinsHarnessInternal::writeTestFile(path, description.utf8(), JITCacheTwinsHarnessInternal::WriteMode::Replace);
}

// Section 5: the host functions.

JSC_DEFINE_HOST_FUNCTION(functionJITCacheStatus, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    Status current = status(vm);
    if (!current.firstFault)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, current.firstFault->stepName()));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheProgress, (JSGlobalObject* globalObject, CallFrame*))
{
    VM& vm = globalObject->vm();
    Progress progress = status(vm).progress;
    JSObject* result = constructEmptyObject(globalObject);
#define JITCACHE_HARNESS_PUT_PROGRESS(field) \
    result->putDirect(vm, Identifier::fromString(vm, #field ""_s), JITCacheTwinsHarnessInternal::countValue(progress.field));
    JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD(JITCACHE_HARNESS_PUT_PROGRESS)
#undef JITCACHE_HARNESS_PUT_PROGRESS
    return JSValue::encode(result);
}

// $vm.jitCacheUCBStatistics() returns this object too (SPEC-ucb.md M4): every counter of UCBStatistics by name, the
// indexed ones as objects keyed by their enumerators' names, the registry's counts() as `counts`, and, for
// { verifyRegistry: true }, registryViolations, the count verifyRegistry returns after its full collection, reported
// through the twin report when one is open. A VM start never configured has no registry and counts zeros.
JSC_DEFINE_HOST_FUNCTION(functionJITCacheUCBStatistics, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    bool verifiesRegistry = false;
    if (JSValue options = callFrame->argument(0); options.isObject()) {
        JSValue verify = asObject(options)->get(globalObject, Identifier::fromString(vm, "verifyRegistry"_s));
        RETURN_IF_EXCEPTION(scope, { });
        verifiesRegistry = verify.isTrue();
    }

    VMState* state = vm.jitCacheState();
    std::optional<unsigned> violations;
    if (verifiesRegistry)
        violations = verifyRegistry(vm, state ? state->twinReportSink() : nullptr);

    UCBStatistics statistics { };
    UCBRegistry::Counts counts { };
    if (state) {
        statistics = state->registry().statistics();
        counts = state->registry().counts();
    }
    JSObject* result = JITCacheTwinsHarnessInternal::ucbStatisticsObject(globalObject, statistics, counts);
    if (violations)
        result->putDirect(vm, Identifier::fromString(vm, "registryViolations"_s), jsNumber(*violations));
    return JSValue::encode(result);
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheDescribeHeap, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    // The default roots serve only the shell's end-of-run description.
    if (!callFrame->argumentCount())
        return throwVMTypeError(globalObject, scope, "jitcacheDescribeHeap expects at least one value"_s);
    Vector<JSValue> roots;
    for (size_t index = 0; index < callFrame->argumentCount(); ++index)
        roots.append(callFrame->uncheckedArgument(index));
    String description = describeReachableHeap(*globalObject, roots.span());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(jsString(vm, description));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheBodyEvents, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<CodeBlock*> codeBlock = JITCacheTwinsHarnessInternal::functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyEvents"_s);
    RETURN_IF_EXCEPTION(scope, { });
    // Section 10.2: the UCB of the function's current CB, which every tier's CB of the body reaches; no fallback to the UCB
    // the function's UnlinkedFunctionExecutable holds.
    if (!*codeBlock)
        return JSValue::encode(jsNull());
    return JSValue::encode(JITCacheTwinsHarnessInternal::bodyEventCountsObject(globalObject, (*codeBlock)->unlinkedCodeBlock()->jitCacheEventCounts()));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheDelta, (JSGlobalObject* globalObject, CallFrame*))
{
    using namespace JITCacheTwinsHarnessInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    DeltaResult result = delta(vm);
    if (shellStart.vm == &vm && shellStart.logsResults)
        writeLogLine(toJSON(result));

    switch (result.outcome) {
    case DeltaOutcome::Completed: {
        JSObject* counts = constructEmptyObject(globalObject);
        counts->putDirect(vm, Identifier::fromString(vm, "eligibleKeys"_s), countValue(result.eligibleKeys));
        counts->putDirect(vm, Identifier::fromString(vm, "committedBodies"_s), countValue(result.committedBodies));
        counts->putDirect(vm, Identifier::fromString(vm, "committedBytes"_s), countValue(result.committedBytes));
        counts->putDirect(vm, Identifier::fromString(vm, "deferredKeys"_s), countValue(result.deferredKeys));
        return JSValue::encode(counts);
    }
    case DeltaOutcome::Rejected:
        return throwVMError(globalObject, scope, String { result.rejection });
    case DeltaOutcome::Faulted:
        ASSERT(result.fault);
        return throwVMError(globalObject, scope, result.fault ? result.fault->stepName() : emptyString());
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheStartOutcome, (JSGlobalObject* globalObject, CallFrame*))
{
    using namespace JITCacheTwinsHarnessInternal;
    VM& vm = globalObject->vm();
    if (shellStart.vm != &vm)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, String { name(shellStart.outcome) }));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheBodyKey, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<CodeBlock*> codeBlock = JITCacheTwinsHarnessInternal::functionCodeBlockArgument(globalObject, callFrame, "jitcacheBodyKey"_s);
    RETURN_IF_EXCEPTION(scope, { });
    VMState* state = vm.jitCacheState();
    if (!*codeBlock || !state)
        return JSValue::encode(jsNull());
    CodeBlock* baseline = *codeBlock;
    if (JITCode::isOptimizingJIT(baseline->jitType()))
        baseline = baseline->baselineAlternative();
    std::optional<BodyKey> key = state->registry().keyOf(*baseline->unlinkedCodeBlock());
    if (!key)
        return JSValue::encode(jsNull());
    return JSValue::encode(jsString(vm, bodyKeyHex(*key)));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheReadSection, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    using namespace JITCacheTwinsHarnessInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<BodyKey> key = bodyKeyArgument(globalObject, callFrame->argument(0), "jitcacheReadSection"_s);
    RETURN_IF_EXCEPTION(scope, { });
    std::optional<SectionKind> kind = sectionKindArgument(globalObject, callFrame->argument(1), "jitcacheReadSection"_s);
    RETURN_IF_EXCEPTION(scope, { });

    VMState* state = vm.jitCacheState();
    OpenedArtifact* artifact = state && state->activityOn() ? state->artifact() : nullptr;
    if (!artifact)
        return JSValue::encode(jsNull());
    // The store's own open, in the mode the VM's lookups use, so a test's read neither counts as a lookup nor raises a fault.
    BodyOpen opened = artifact->open(*key, state->strict() ? ValidationMode::Full : ValidationMode::Integrity);
    switch (opened.outcome) {
    case StoreOutcome::Absent:
        return JSValue::encode(jsNull());
    case StoreOutcome::Unavailable:
    case StoreOutcome::Invalid:
        return throwVMError(globalObject, scope, makeString("jitcacheReadSection: "_s, storeFailureText(opened)));
    case StoreOutcome::Found:
        break;
    }
    // A body holds exactly the kinds its highest tier requires, an empty one included (container check B7).
    if (!isSectionRequired(*kind, opened.body->highestTier()))
        return JSValue::encode(jsNull());
    RefPtr<ArrayBuffer> buffer = ArrayBuffer::tryCreate(opened.body->section(*kind));
    if (!buffer) {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }
    return JSValue::encode(JSArrayBuffer::create(vm, globalObject->arrayBufferStructure(ArrayBufferSharingMode::Default), WTF::move(buffer)));
}

JSC_DEFINE_HOST_FUNCTION(functionJITCacheRewriteSection, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    using namespace JITCacheTwinsHarnessInternal;
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    std::optional<BodyKey> key = bodyKeyArgument(globalObject, callFrame->argument(0), "jitcacheRewriteSection"_s);
    RETURN_IF_EXCEPTION(scope, { });
    std::optional<SectionKind> kind = sectionKindArgument(globalObject, callFrame->argument(1), "jitcacheRewriteSection"_s);
    RETURN_IF_EXCEPTION(scope, { });

    JSValue offsetValue = callFrame->argument(2);
    double offset = offsetValue.isNumber() ? offsetValue.asNumber() : -1;
    if (!(offset >= 0) || offset > maxSafeInteger() || std::trunc(offset) != offset)
        return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects a byte offset"_s);

    auto* array = dynamicDowncast<JSArray>(callFrame->argument(3));
    if (!array)
        return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects an array of byte values"_s);
    Vector<uint8_t> bytes;
    for (unsigned index = 0; index < array->length(); ++index) {
        JSValue element = array->getIndex(globalObject, index);
        RETURN_IF_EXCEPTION(scope, { });
        double byte = element.isNumber() ? element.asNumber() : -1;
        if (!(byte >= 0 && byte <= 255) || std::trunc(byte) != byte)
            return throwVMTypeError(globalObject, scope, "jitcacheRewriteSection expects an array of byte values"_s);
        bytes.append(static_cast<uint8_t>(byte));
    }

    VMState* state = vm.jitCacheState();
    if (!state || !state->producing())
        return throwVMError(globalObject, scope, String { "jitcacheRewriteSection: the VM does not produce"_s });
    auto result = rewriteSectionForTesting(vm, *key, *kind, static_cast<uint64_t>(offset), bytes.span());
    if (!result)
        return throwVMError(globalObject, scope, makeString(result.error().check, ": "_s, result.error().detail));
    return JSValue::encode(jsUndefined());
}

} // namespace JSC::JITCache

// Unified sources compile this file with others.
#undef JITCACHE_HARNESS_FOR_EACH_PROGRESS_FIELD
#undef JITCACHE_HARNESS_FOR_EACH_BODY_EVENT_COUNT
#undef JITCACHE_HARNESS_FOR_EACH_UCB_STATISTIC

#endif // ENABLE(JITCACHE_TWINS)
