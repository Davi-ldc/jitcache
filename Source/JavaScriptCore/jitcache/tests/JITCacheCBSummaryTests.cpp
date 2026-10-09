#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "ArrayProfile.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "CollectionScope.h"
#include "Completion.h"
#include "DeferGC.h"
#include "EnsureStillAliveHere.h"
#include "FunctionExecutable.h"
#include "Heap.h"
#include "JIT.h"
#include "JITCacheCBFormat.h"
#include "JITCacheCBState.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "SpeculatedType.h"
#include "UnlinkedCodeBlock.h"
#include "ValueProfile.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// U5 of SPEC-cb.md section 11.2: the score of a capture is a function of the cb.summary it wrote (I8), so capture,
// scoreLive and decodeSummary agree; after the native merges, each slot capture wrote pairs with the UCB copy I11 names
// (I11, I12); and decodeSummary with strict on accepts or rejects each field of section 3.4 exactly at its rule's bound.
// Live tests build their CBs as the capture tests do: one call installs an LLInt CB, which JIT::compileSync compiles to
// baseline. Each test keeps its global object on its stack, and its source keeps every value the test passes in globals.

namespace JSC::JITCache::Tests {

namespace CBSummaryTestsInternal {

static constexpr uint8_t notCarried = static_cast<uint8_t>(CBFormat::CounterMode::NotCarried);
static constexpr uint8_t carried = static_cast<uint8_t>(CBFormat::CounterMode::Carried);
static constexpr uint8_t pruningMark = static_cast<uint8_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

// The lowest SpeculatedType bit outside SpecBytecodeTop, the bound V6 puts on every category.
static_assert(~SpecBytecodeTop);
static constexpr SpeculatedType outsideBytecodeTop = SpeculatedType { 1 } << std::countr_zero(~SpecBytecodeTop);

static void checkEqual(TestContext& context, const char* file, int line, StringView what, uint64_t actual, uint64_t expected)
{
    if (actual != expected)
        context.fail(file, line, makeString(what, ": "_s, actual, " instead of "_s, expected));
}

#define JITCACHE_CB_SUMMARY_CHECK_EQUAL(what, actual, expected) \
    CBSummaryTestsInternal::checkEqual(context, __FILE__, __LINE__, what, static_cast<uint64_t>(actual), static_cast<uint64_t>(expected))

static void checkScore(TestContext& context, const char* file, int line, StringView what, const CBScore& actual, const CBScore& expected)
{
    checkEqual(context, file, line, makeString(what, ", richnessUnits"_s), actual.richnessUnits, expected.richnessUnits);
    checkEqual(context, file, line, makeString(what, ", counterWithheld"_s), actual.counterWithheld, expected.counterWithheld);
    checkEqual(context, file, line, makeString(what, ", counterProgress"_s), actual.counterProgress, expected.counterProgress);
}

#define JITCACHE_CB_SUMMARY_CHECK_SCORE(what, actual, expected) \
    CBSummaryTestsInternal::checkScore(context, __FILE__, __LINE__, what, actual, expected)

static void failWith(TestContext& context, StringView step, const CBFault& fault)
{
    JITCACHE_FAIL(makeString(step, " failed with "_s, fault.kind == CBFaultKind::RecordingFault ? "a recording fault"_s : "invalid material"_s, ": "_s, description(fault.check)));
}

template<typename T>
static T readAt(std::span<const uint8_t> bytes, size_t offset)
{
    T value { };
    memcpySpan(asMutableByteSpan(value), bytes.subspan(offset, sizeof(T)));
    return value;
}

template<typename T>
static void writeAt(std::span<uint8_t> bytes, size_t offset, const T& value)
{
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

// Section bytes of any length in 8-byte aligned storage, as R-INT-1 hands a section over.
class SectionBytes {
public:
    explicit SectionBytes(size_t size)
        : m_words(FillWith { }, (size + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0)
        , m_size(size)
    {
    }

    explicit SectionBytes(std::span<const uint8_t> bytes)
        : SectionBytes(bytes.size())
    {
        if (!bytes.empty())
            memcpySpan(mutableSpan(), bytes);
    }

    size_t size() const { return m_size; }
    std::span<const uint8_t> span() const { return asByteSpan(m_words.span()).first(m_size); }
    std::span<uint8_t> mutableSpan() { return asMutableByteSpan(m_words.mutableSpan()).first(m_size); }

    // A copy `size` bytes long: truncated, or extended with zero bytes.
    SectionBytes resized(size_t size) const
    {
        SectionBytes copy(size);
        if (size_t copied = std::min(size, m_size))
            memcpySpan(copy.mutableSpan(), span().first(copied));
        return copy;
    }

    template<typename T> T read(size_t offset) const { return readAt<T>(span(), offset); }
    template<typename T> void write(size_t offset, const T& value) { writeAt(mutableSpan(), offset, value); }

private:
    Vector<uint64_t> m_words;
    size_t m_size { 0 };
};

// Section 3.3's layout, computed from the SPEC's element sizes rather than by the code under test. Counts are 32-bit, so
// nothing here overflows a 64-bit size_t.
struct SummaryOffsets {
    size_t argumentCategories { 0 };
    size_t valueCategories { 0 };
    size_t lazyOperandCategories { 0 };
    size_t arrayFlags { 0 };
    size_t padding { 0 };
    size_t size { 0 };
};

static SummaryOffsets summaryOffsets(const CBFormat::SummaryHeader& header)
{
    SummaryOffsets offsets;
    offsets.argumentCategories = sizeof(CBFormat::SummaryHeader);
    offsets.valueCategories = offsets.argumentCategories + static_cast<size_t>(header.numArguments) * sizeof(uint64_t);
    offsets.lazyOperandCategories = offsets.valueCategories + static_cast<size_t>(header.numValueProfiles) * sizeof(uint64_t);
    offsets.arrayFlags = offsets.lazyOperandCategories + static_cast<size_t>(header.numLazyOperandProfiles) * sizeof(uint64_t);
    offsets.padding = offsets.arrayFlags + header.numArrayProfiles;
    offsets.size = (offsets.padding + 7) / 8 * 8;
    return offsets;
}

// A cb.summary as the test reads it. serializeSummary takes the counts from the vectors and every other header field
// from `header`.
struct Summary {
    CBFormat::SummaryHeader header { };
    Vector<uint64_t> argumentCategories;
    Vector<uint64_t> valueCategories;
    Vector<uint64_t> lazyOperandCategories;
    Vector<uint8_t> arrayFlags;
};

static Vector<uint64_t> readSlots(std::span<const uint8_t> bytes, size_t offset, uint32_t count)
{
    Vector<uint64_t> slots;
    for (size_t index = 0; index < count; ++index)
        slots.append(readAt<uint64_t>(bytes, offset + index * sizeof(uint64_t)));
    return slots;
}

static void writeSlots(SectionBytes& bytes, size_t offset, const Vector<uint64_t>& slots)
{
    for (size_t index = 0; index < slots.size(); ++index)
        bytes.write(offset + index * sizeof(uint64_t), slots[index]);
}

// Fails unless the bytes have exactly the length their counts lay out, with zero padding.
static std::optional<Summary> parseSummary(TestContext& context, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(CBFormat::SummaryHeader)) {
        JITCACHE_FAIL("cb.summary is shorter than its header"_s);
        return std::nullopt;
    }
    Summary summary;
    summary.header = readAt<CBFormat::SummaryHeader>(bytes, 0);
    auto offsets = summaryOffsets(summary.header);
    if (bytes.size() != offsets.size) {
        JITCACHE_FAIL(makeString("cb.summary is "_s, bytes.size(), " bytes long where its counts lay out "_s, offsets.size));
        return std::nullopt;
    }
    summary.argumentCategories = readSlots(bytes, offsets.argumentCategories, summary.header.numArguments);
    summary.valueCategories = readSlots(bytes, offsets.valueCategories, summary.header.numValueProfiles);
    summary.lazyOperandCategories = readSlots(bytes, offsets.lazyOperandCategories, summary.header.numLazyOperandProfiles);
    for (size_t index = 0; index < summary.header.numArrayProfiles; ++index)
        summary.arrayFlags.append(bytes[offsets.arrayFlags + index]);
    for (size_t offset = offsets.padding; offset < offsets.size; ++offset) {
        if (bytes[offset]) {
            JITCACHE_FAIL(makeString("cb.summary has a nonzero padding byte at "_s, offset));
            return std::nullopt;
        }
    }
    return summary;
}

static SectionBytes serializeSummary(const Summary& summary)
{
    CBFormat::SummaryHeader header = summary.header;
    header.numArguments = static_cast<uint32_t>(summary.argumentCategories.size());
    header.numValueProfiles = static_cast<uint32_t>(summary.valueCategories.size());
    header.numLazyOperandProfiles = static_cast<uint32_t>(summary.lazyOperandCategories.size());
    header.numArrayProfiles = static_cast<uint32_t>(summary.arrayFlags.size());
    auto offsets = summaryOffsets(header);
    SectionBytes bytes(offsets.size);
    bytes.write(0, header);
    writeSlots(bytes, offsets.argumentCategories, summary.argumentCategories);
    writeSlots(bytes, offsets.valueCategories, summary.valueCategories);
    writeSlots(bytes, offsets.lazyOperandCategories, summary.lazyOperandCategories);
    for (size_t index = 0; index < summary.arrayFlags.size(); ++index)
        bytes.write(offsets.arrayFlags + index, summary.arrayFlags[index]);
    return bytes;
}

// The written bytes with their header edited in place, at the same length.
template<typename Edit>
static SectionBytes withHeader(const SectionBytes& bytes, const Edit& edit)
{
    SectionBytes edited = bytes;
    auto header = edited.read<CBFormat::SummaryHeader>(0);
    edit(header);
    edited.write(0, header);
    return edited;
}

enum class CountPruningMark : bool { No, Yes };

// Section 4.3's richness recounted from the summary: one unit per category bit of an argument, value or lazy-operand
// slot, and one per array flag other than the pruning mark unless asked to count it too.
static uint64_t recountRichness(const Summary& summary, CountPruningMark countPruningMark = CountPruningMark::No)
{
    uint64_t units = 0;
    for (auto* slots : { &summary.argumentCategories, &summary.valueCategories, &summary.lazyOperandCategories }) {
        for (uint64_t categories : *slots)
            units += std::popcount(categories);
    }
    unsigned countedFlags = countPruningMark == CountPruningMark::Yes ? 0xffu : static_cast<unsigned>(0xff & ~pruningMark);
    for (uint8_t flags : summary.arrayFlags)
        units += std::popcount(static_cast<unsigned>(flags & countedFlags));
    return units;
}

// The score section 4.3 assigns to the summary's bytes alone (I8).
static CBScore expectedScore(const Summary& summary)
{
    return { recountRichness(summary), summary.header.counterMode == notCarried, summary.header.counterProgress };
}

static void checkDecodes(TestContext& context, StringView what, std::span<const uint8_t> bytes, bool strict, const CBScore& expected)
{
    auto score = decodeSummary(bytes, strict);
    String described = makeString(what, strict ? " (strict)"_s : " (normal)"_s);
    if (!score) {
        failWith(context, described, score.error());
        return;
    }
    checkScore(context, __FILE__, __LINE__, described, *score, expected);
}

static void checkRejects(TestContext& context, StringView what, std::span<const uint8_t> bytes, CBCheck expected)
{
    auto score = decodeSummary(bytes, true);
    if (score) {
        JITCACHE_FAIL(makeString(what, ": decodeSummary accepted the section instead of failing with "_s, description(expected)));
        return;
    }
    if (score.error().kind != CBFaultKind::InvalidMaterial)
        JITCACHE_FAIL(makeString(what, ": decodeSummary failed with a recording fault instead of invalid material"_s));
    if (score.error().check != expected)
        JITCACHE_FAIL(makeString(what, ": decodeSummary failed with "_s, description(score.error().check), " instead of "_s, description(expected)));
}

// A summary within every rule: both modes decode it to the score recounted from its bytes.
static void checkAcceptedModel(TestContext& context, StringView what, const Summary& model)
{
    SectionBytes bytes = serializeSummary(model);
    for (bool strict : { true, false })
        checkDecodes(context, what, bytes.span(), strict, expectedScore(model));
}

// A summary with a valid layout that breaks one rule: strict rejects it with `expected`, and normal mode, which only reads,
// still returns the score of its bytes.
static void checkRejectedModel(TestContext& context, StringView what, const Summary& model, CBCheck expected)
{
    SectionBytes bytes = serializeSummary(model);
    checkRejects(context, what, bytes.span(), expected);
    checkDecodes(context, what, bytes.span(), false, expectedScore(model));
}

// Bytes edited outside the model whose length and padding pass: both modes decode them to the score of their bytes.
static void checkAcceptedBytes(TestContext& context, StringView what, const SectionBytes& bytes)
{
    auto model = parseSummary(context, bytes.span());
    if (!model) {
        JITCACHE_FAIL(makeString(what, ": the edited bytes do not parse"_s));
        return;
    }
    for (bool strict : { true, false })
        checkDecodes(context, what, bytes.span(), strict, expectedScore(*model));
}

// The slots of cb.state that pair with UCB copies (arrays 1 to 4 of section 3.2), at offsets computed here.
struct ScoredState {
    CBFormat::StateHeader header { };
    Vector<uint64_t> argumentPredictions;
    Vector<uint64_t> valuePredictions;
    Vector<CBFormat::ArrayProfileRecord> arrayProfiles;
    Vector<uint64_t> lazyOperandPredictions;
};

static std::optional<ScoredState> parseState(TestContext& context, std::span<const uint8_t> bytes)
{
    if (bytes.size() < sizeof(CBFormat::StateHeader)) {
        JITCACHE_FAIL("cb.state is shorter than its header"_s);
        return std::nullopt;
    }
    ScoredState state;
    state.header = readAt<CBFormat::StateHeader>(bytes, 0);
    size_t numArrayProfiles = 0;
    for (unsigned family = 0; family < CBFormat::numberOfArrayProfileFamilies; ++family)
        numArrayProfiles += state.header.familyEntryCount[family];
    size_t argumentOffset = sizeof(CBFormat::StateHeader);
    size_t valueOffset = argumentOffset + static_cast<size_t>(state.header.numArguments) * sizeof(uint64_t);
    size_t arrayOffset = valueOffset + static_cast<size_t>(state.header.numValueProfiles) * sizeof(uint64_t);
    size_t lazyOperandOffset = arrayOffset + numArrayProfiles * sizeof(CBFormat::ArrayProfileRecord);
    size_t lazyOperandEnd = lazyOperandOffset + static_cast<size_t>(state.header.numLazyOperandProfiles) * sizeof(CBFormat::LazyOperandRecord);
    if (lazyOperandEnd > bytes.size()) {
        JITCACHE_FAIL(makeString("cb.state is "_s, bytes.size(), " bytes long, too short for the "_s, lazyOperandEnd, " its first four arrays need"_s));
        return std::nullopt;
    }
    state.argumentPredictions = readSlots(bytes, argumentOffset, state.header.numArguments);
    state.valuePredictions = readSlots(bytes, valueOffset, state.header.numValueProfiles);
    for (size_t position = 0; position < numArrayProfiles; ++position)
        state.arrayProfiles.append(readAt<CBFormat::ArrayProfileRecord>(bytes, arrayOffset + position * sizeof(CBFormat::ArrayProfileRecord)));
    for (size_t index = 0; index < state.header.numLazyOperandProfiles; ++index)
        state.lazyOperandPredictions.append(readAt<CBFormat::LazyOperandRecord>(bytes, lazyOperandOffset + index * sizeof(CBFormat::LazyOperandRecord)).prediction);
    return state;
}

// Section 4.3's counterProgress of the triple cb.state carries, computed here from the formula.
static uint32_t expectedProgress(const CBFormat::StateHeader& header)
{
    if (header.counterMode != carried)
        return 0;
    double progress = static_cast<double>(header.counterTotalCount) + header.counterValue;
    if (!(progress > 0))
        return 0;
    return static_cast<uint32_t>(std::min(std::floor(progress), static_cast<double>(header.counterActiveThreshold)));
}

// U5's condition on its body: neighbouring profiles in each native walk saw different types (different array modes, for
// array profiles), so a lane walk one slot out of step with the native one would read a value that differs from the UCB
// copy it is compared with.
static void checkNeighboursDiffer(TestContext& context, UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& valueProfiles = unlinkedCodeBlock.unlinkedValueProfiles();
    for (size_t index = 1; index < valueProfiles.size(); ++index) {
        if (valueProfiles[index - 1].prediction() == valueProfiles[index].prediction())
            JITCACHE_FAIL(makeString("the subject's UCB value profiles "_s, index - 1, " and "_s, index, " both hold "_s, valueProfiles[index].prediction(), ", so a walk out of step there would pass"_s));
    }
    auto& arrayProfiles = unlinkedCodeBlock.unlinkedArrayProfiles();
    for (size_t position = 1; position < arrayProfiles.size(); ++position) {
        if (arrayProfiles[position - 1].observedArrayModes() == arrayProfiles[position].observedArrayModes())
            JITCACHE_FAIL(makeString("the subject's UCB array profiles "_s, position - 1, " and "_s, position, " both hold modes "_s, arrayProfiles[position].observedArrayModes(), ", so a walk out of step there would pass"_s));
    }
}

// After the native merges, each cb.state slot equals the UCB copy I11 pairs it with: the prediction for an argument or
// value slot; the modes, and the flags without the pruning mark, which the merge keeps out of the UCB copy, for an array
// profile (I11, I12). Each summary slot is its cb.state slot ORed with that copy, and a lazy-operand slot, which has no
// copy, is its cb.state prediction (I8).
static void checkPairing(TestContext& context, StringView what, const ScoredState& state, const Summary& summary, UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& unlinkedValueProfiles = unlinkedCodeBlock.unlinkedValueProfiles();
    auto& unlinkedArrayProfiles = unlinkedCodeBlock.unlinkedArrayProfiles();
    size_t numArguments = state.argumentPredictions.size();
    if (unlinkedValueProfiles.size() != numArguments + state.valuePredictions.size() || unlinkedArrayProfiles.size() != state.arrayProfiles.size()) {
        JITCACHE_FAIL(makeString(what, ": the UCB copies are not as many as the cb.state slots they pair with"_s));
        return;
    }
    if (summary.argumentCategories.size() != numArguments || summary.valueCategories.size() != state.valuePredictions.size()
        || summary.lazyOperandCategories.size() != state.lazyOperandPredictions.size() || summary.arrayFlags.size() != state.arrayProfiles.size()) {
        JITCACHE_FAIL(makeString(what, ": cb.summary and cb.state count different slots"_s));
        return;
    }

    auto checkValueSlot = [&](ASCIILiteral slot, size_t index, uint64_t statePrediction, uint64_t summaryCategories, SpeculatedType unlinkedPrediction) {
        if (statePrediction != unlinkedPrediction)
            JITCACHE_FAIL(makeString(what, ": "_s, slot, ' ', index, " holds "_s, statePrediction, " in cb.state where its UCB copy holds "_s, unlinkedPrediction));
        if (summaryCategories != (statePrediction | unlinkedPrediction))
            JITCACHE_FAIL(makeString(what, ": "_s, slot, ' ', index, " holds "_s, summaryCategories, " in cb.summary, not its cb.state slot ORed with its UCB copy"_s));
    };
    for (size_t index = 0; index < numArguments; ++index)
        checkValueSlot("argument"_s, index, state.argumentPredictions[index], summary.argumentCategories[index], unlinkedValueProfiles[index].prediction());
    for (size_t index = 0; index < state.valuePredictions.size(); ++index)
        checkValueSlot("the value profile at offset"_s, index + 1, state.valuePredictions[index], summary.valueCategories[index], unlinkedValueProfiles[numArguments + index].prediction());
    for (size_t index = 0; index < state.lazyOperandPredictions.size(); ++index) {
        if (summary.lazyOperandCategories[index] != state.lazyOperandPredictions[index])
            JITCACHE_FAIL(makeString(what, ": lazy-operand profile "_s, index, " holds "_s, summary.lazyOperandCategories[index], " in cb.summary and "_s, state.lazyOperandPredictions[index], " in cb.state"_s));
    }
    for (size_t position = 0; position < state.arrayProfiles.size(); ++position) {
        const auto& record = state.arrayProfiles[position];
        const auto& unlinked = unlinkedArrayProfiles[position];
        uint32_t unlinkedFlags = unlinked.arrayProfileFlags().toRaw();
        if (record.observedArrayModes != unlinked.observedArrayModes())
            JITCACHE_FAIL(makeString(what, ": array position "_s, position, " holds modes "_s, record.observedArrayModes, " in cb.state where its UCB copy holds "_s, unlinked.observedArrayModes()));
        if ((record.flags & ~static_cast<uint32_t>(pruningMark)) != unlinkedFlags)
            JITCACHE_FAIL(makeString(what, ": array position "_s, position, " holds flags "_s, record.flags, " in cb.state where its UCB copy holds "_s, unlinkedFlags));
        if (summary.arrayFlags[position] != static_cast<uint8_t>(record.flags | unlinkedFlags))
            JITCACHE_FAIL(makeString(what, ": array position "_s, position, " holds flags "_s, static_cast<unsigned>(summary.arrayFlags[position]), " in cb.summary, not its cb.state flags ORed with its UCB copy's"_s));
    }
}

static JSGlobalObject* createRealm(TestContext& context, VM& vm, ASCIILiteral source)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the test source threw"_s);
        return nullptr;
    }
    return globalObject;
}

static JSFunction* globalFunction(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    JSValue value = globalObject->get(globalObject, Identifier::fromString(globalObject->vm(), name));
    auto* function = dynamicDowncast<JSFunction>(value);
    if (!function || function->isHostFunction()) {
        JITCACHE_FAIL(makeString("the test source defines no JS function "_s, name));
        return nullptr;
    }
    return function;
}

static bool callFunction(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, std::initializer_list<JSValue> arguments)
{
    MarkedArgumentBuffer argumentList;
    for (JSValue argument : arguments)
        argumentList.append(argument);
    if (argumentList.hasOverflowed()) {
        JITCACHE_FAIL("the argument list overflowed"_s);
        return false;
    }
    NakedPtr<Exception> exception;
    JSC::call(globalObject, function, getCallData(function), jsUndefined(), argumentList, exception);
    if (exception) {
        JITCACHE_FAIL("calling a test function threw"_s);
        return false;
    }
    return true;
}

// Compiles the LLInt CB the function's first call installed to baseline, in place, as the LLInt's tier-up would.
static CodeBlock* compileToBaseline(TestContext& context, VM& vm, JSFunction* function)
{
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the function's first calls did not leave an LLInt CB"_s);
        return nullptr;
    }
    CompilationResult result;
    {
        DeferGCForAWhile deferGC(vm);
        result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    }
    if (result != CompilationResult::CompilationSuccessful || codeBlock->jitType() != JITType::BaselineJIT || function->jsExecutable()->codeBlockForCall() != codeBlock || !codeBlock->baselineJITData()) {
        JITCACHE_FAIL("JIT::compileSync did not install baseline code in the LLInt CB"_s);
        return nullptr;
    }
    return codeBlock;
}

// U5's live body. `subject` has an A15 entry, the iterable profile of the iterator_next that destructuring emits, and
// warm runs it on both iteration paths: a fast one over an Int32 array and a generic one over `custom`, whose iterator
// is an array of objects carrying next and return, so every profile of the iterator opcodes and of the iterator close
// (the get_by_id of return and its call) sees a value. In the order the generator gives them, the profiles see:
//   arguments: undefined (this), an array, an array or a plain object, an array, a string;
//   value profiles: a function (the Symbol.iterator get), an array or a plain object (the iterable), an array
//   iterator or an array (the iterator), a function (next), a plain object (next's result), a boolean (done), an int or
//   a double (value), undefined or a function (the return get), a plain object (return's result), an int (ints[0]),
//   a double (doubles[1]);
//   array profiles: Int32 (ints[0]), Double (doubles[1]), Contiguous (the return call's this), NonArray (the
//   iterable of a generic iterator_open), Int32 then Double (A15, after iterateDoubles).
static constexpr auto subjectSource = "function subject(ints, iterable, doubles, tag) {"
    "    var [first] = iterable;"
    "    return [first, ints[0], doubles[1], tag];"
    "}"
    "var ints = [1, 2, 3];"
    "var doubles = [1.5, 2.5, 3.5];"
    "var custom = {"
    "    [Symbol.iterator]() {"
    "        var iterator = [{}];"
    "        iterator.next = function() { return { value: 0.5, done: false }; };"
    "        iterator.return = function() { return {}; };"
    "        return iterator;"
    "    }"
    "};"
    "function warm(rounds) {"
    "    for (var round = 0; round < rounds; ++round) {"
    "        subject(ints, ints, doubles, 'fast');"
    "        subject(ints, custom, doubles, 'generic');"
    "    }"
    "}"
    "function iterateDoubles(rounds) {"
    "    for (var round = 0; round < rounds; ++round)"
    "        subject(ints, doubles, doubles, 'fast');"
    "}"_s;

// U5's body for the decode rules: argument, value and array slots beside the header, and padding after its one flag.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }"
    "var array = [1, 2, 3];"
    "function run(rounds) { for (var round = 0; round < rounds; ++round) element(array, round % 3); }"_s;

} // namespace CBSummaryTestsInternal

using namespace CBSummaryTestsInternal;

// U5, live (I8, I11, I12): right after a synchronous full collection, in a VM with useConcurrentJIT off and with no JS run
// in between, capture and scoreLive give one score, which decodeSummary and a recount give again from the written
// summary, in both modes and with the polymorphic bit set and clear. The recount leaves the pruning mark out, and the
// collection's merges make every cb.state slot equal the UCB copy I11 pairs it with.
JITCACHE_TEST_WITH_OPTIONS(cbSummaryScoreAgreesAcrossCaptureScoreLiveAndDecode, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, subjectSource);
    if (!globalObject)
        return;
    JSFunction* subject = globalFunction(context, globalObject, "subject"_s);
    JSFunction* warm = globalFunction(context, globalObject, "warm"_s);
    JSFunction* iterateDoubles = globalFunction(context, globalObject, "iterateDoubles"_s);
    if (!subject || !warm || !iterateDoubles)
        return;

    // Both iteration paths in the LLInt, then in baseline code. Four LLInt calls stay far below the LLInt's threshold,
    // and six baseline entries far below optimizeAfterWarmUp's, so no tier-up compiles anything meanwhile.
    if (!callFunction(context, globalObject, warm, { jsNumber(2) }))
        return;
    CodeBlock* codeBlock = compileToBaseline(context, vm, subject);
    if (!codeBlock)
        return;
    if (!callFunction(context, globalObject, warm, { jsNumber(3) }))
        return;

    // This collection's drain gives A15 the Int32 array's mode. Iterating a Double array then gives it a second mode,
    // which the next drain prunes, setting the pruning mark (ArrayProfile::computeUpdatedPrediction).
    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (!callFunction(context, globalObject, iterateDoubles, { jsNumber(2) }))
        return;

    // The finalization of this collection drains every visited CB and runs the native merges (N2). From here on the test
    // runs no JS, and with useConcurrentJIT off no compiler thread drains, so nothing changes the CB between the calls.
    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (subject->jsExecutable()->codeBlockForCall() != codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("the collections replaced the subject's baseline CB"_s);
        return;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
    checkNeighboursDiffer(context, unlinkedCodeBlock);

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    std::array<std::optional<CBScore>, 2> strictScoreByBit;
    for (bool strict : { true, false }) {
        for (bool hasPolymorphicSite : { false, true }) {
            String what = makeString(strict ? "strict capture"_s : "normal capture"_s, hasPolymorphicSite ? " with the polymorphic bit"_s : " without the polymorphic bit"_s);
            auto liveScore = CBStateCapture::scoreLive(*codeBlock, strict, hasPolymorphicSite);
            if (!liveScore) {
                failWith(context, makeString(what, ", scoreLive"_s), liveScore.error());
                continue;
            }
            auto captured = CBStateCapture::capture(*codeBlock, budget.get(), strict, hasPolymorphicSite);
            if (!captured) {
                failWith(context, what, captured.error());
                continue;
            }
            const CBScore& score = captured->score();
            JITCACHE_CB_SUMMARY_CHECK_SCORE(makeString(what, ", scoreLive against score()"_s), *liveScore, score);

            auto state = parseState(context, captured->stateSection());
            auto summary = parseSummary(context, captured->summarySection());
            if (!state || !summary)
                continue;

            // The summary's bytes alone give the score (I8): a recount, and decodeSummary in both modes.
            JITCACHE_CB_SUMMARY_CHECK_SCORE(makeString(what, ", recount of the written summary"_s), expectedScore(*summary), score);
            for (bool decodeStrict : { true, false })
                checkDecodes(context, makeString(what, ", decodeSummary of the written summary"_s), captured->summarySection(), decodeStrict, score);

            // The recount leaves out the pruning mark, which the A15 flags carry after the second collection.
            auto markedFlags = static_cast<size_t>(std::ranges::count_if(summary->arrayFlags, [](uint8_t flags) -> bool {
                return flags & pruningMark;
            }));
            if (!markedFlags)
                JITCACHE_FAIL(makeString(what, ": no array flag carries the pruning mark, so the summary cannot show that richness ignores it"_s));
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", richness with the pruning mark counted"_s), recountRichness(*summary, CountPruningMark::Yes), score.richnessUnits + markedFlags);

            // The counter as I16 states it, and section 4.3's progress of the triple cb.state carries.
            uint8_t counterMode = hasPolymorphicSite ? notCarried : carried;
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.state counterMode"_s), state->header.counterMode, counterMode);
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.summary counterMode"_s), summary->header.counterMode, counterMode);
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", counterWithheld"_s), score.counterWithheld, hasPolymorphicSite);
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", counterProgress"_s), score.counterProgress, expectedProgress(state->header));
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.summary counterProgress"_s), summary->header.counterProgress, score.counterProgress);

            // The body's A15 entry, and the pairing of every slot with its UCB copy.
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", A15 entries"_s), state->header.familyEntryCount[CBFormat::numberOfArrayProfileFamilies - 1], 1);
            checkPairing(context, what, *state, *summary, unlinkedCodeBlock);

            // Strict mode only adds checks, so a normal capture with the same bit gives the strict capture's score.
            auto& strictScore = strictScoreByBit[hasPolymorphicSite];
            if (strict)
                strictScore = score;
            else if (strictScore)
                JITCACHE_CB_SUMMARY_CHECK_SCORE(makeString(what, " against the strict capture"_s), score, *strictScore);
        }
    }

    ensureStillAliveHere(globalObject);
}

// U5, decode rules: strict decodeSummary of the summaries capture wrote, each header field and value moved just inside
// and just outside its rule of section 3.4, returns the score of the bytes or fails with exactly the rule's CBCheck;
// normal mode returns the score of any edit whose layout still holds. Every check compares with a score the test recounts
// from the bytes, which is also capture's own for the written summaries (I8).
JITCACHE_TEST(cbSummaryDecodeChecksEachRuleAtItsBoundary, Yes)
{
    using CBFormat::SummaryHeader;

    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    JSFunction* run = globalFunction(context, globalObject, "run"_s);
    if (!element || !run)
        return;
    if (!callFunction(context, globalObject, run, { jsNumber(1) }))
        return;
    CodeBlock* codeBlock = compileToBaseline(context, vm, element);
    if (!codeBlock)
        return;
    if (!callFunction(context, globalObject, run, { jsNumber(3) }))
        return;

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto carriedCapture = CBStateCapture::capture(*codeBlock, budget.get(), true, false);
    auto withheldCapture = CBStateCapture::capture(*codeBlock, budget.get(), true, true);
    if (!carriedCapture || !withheldCapture) {
        failWith(context, "capture"_s, carriedCapture ? withheldCapture.error() : carriedCapture.error());
        return;
    }
    ensureStillAliveHere(globalObject);

    SectionBytes written(carriedCapture->summarySection());
    SectionBytes writtenWithheld(withheldCapture->summarySection());
    auto parsed = parseSummary(context, written.span());
    auto parsedWithheld = parseSummary(context, writtenWithheld.span());
    if (!parsed || !parsedWithheld)
        return;
    const Summary& summary = *parsed;
    auto offsets = summaryOffsets(summary.header);

    // The written summaries: capture's score, through the recount and through decodeSummary in both modes (I8).
    JITCACHE_CB_SUMMARY_CHECK_SCORE("recount of the carried summary"_s, expectedScore(summary), carriedCapture->score());
    JITCACHE_CB_SUMMARY_CHECK_SCORE("recount of the withheld summary"_s, expectedScore(*parsedWithheld), withheldCapture->score());
    for (bool strict : { true, false }) {
        checkDecodes(context, "the carried summary"_s, written.span(), strict, carriedCapture->score());
        checkDecodes(context, "the withheld summary"_s, writtenWithheld.span(), strict, withheldCapture->score());
    }

    // V1: the header must fit, with layout version 1, the baseline tier and a known counter mode.
    checkRejects(context, "an empty section"_s, written.resized(0).span(), CBCheck::SummaryHeader);
    checkRejects(context, "a section one byte shorter than its header"_s, written.resized(sizeof(SummaryHeader) - 1).span(), CBCheck::SummaryHeader);
    for (unsigned layoutVersion : { 0u, 2u }) {
        Summary model = summary;
        model.header.layoutVersion = static_cast<uint16_t>(layoutVersion);
        checkRejectedModel(context, makeString("layout version "_s, layoutVersion), model, CBCheck::SummaryHeader);
    }
    for (unsigned tier : { 0u, 2u }) {
        Summary model = summary;
        model.header.tier = static_cast<uint8_t>(tier);
        checkRejectedModel(context, makeString("tier "_s, tier), model, CBCheck::SummaryHeader);
    }
    {
        Summary model = summary;
        model.header.counterMode = 2;
        checkRejectedModel(context, "counter mode 2"_s, model, CBCheck::SummaryHeader);
    }

    // The counter progress: at most INT32_MAX, and zero when the counter does not travel.
    {
        Summary model = summary;
        model.header.counterProgress = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
        checkAcceptedModel(context, "a carried progress of INT32_MAX"_s, model);
        model.header.counterProgress = static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) + 1;
        checkRejectedModel(context, "a carried progress of INT32_MAX + 1"_s, model, CBCheck::SummaryValue);
        model.header.counterProgress = std::numeric_limits<uint32_t>::max();
        checkRejectedModel(context, "a carried progress of UINT32_MAX"_s, model, CBCheck::SummaryValue);
        model.header.counterMode = notCarried;
        model.header.counterProgress = 0;
        checkAcceptedModel(context, "a withheld counter with no progress"_s, model);
        model.header.counterProgress = 1;
        checkRejectedModel(context, "a withheld counter with a progress of 1"_s, model, CBCheck::SummaryValue);
    }
    {
        Summary model = *parsedWithheld;
        model.header.counterProgress = 1;
        checkRejectedModel(context, "the withheld summary with a progress of 1"_s, model, CBCheck::SummaryValue);
        model.header.counterMode = carried;
        model.header.counterProgress = 0;
        checkAcceptedModel(context, "the withheld summary turned carried with no progress"_s, model);
    }

    // V5: the length is exactly what the counts lay out, padding included, and the padding is zero. decodeSummary
    // compares no count with a CodeBlock, so a count may change wherever the length still matches.
    checkRejects(context, "a section that holds only its header"_s, written.resized(sizeof(SummaryHeader)).span(), CBCheck::SummaryLength);
    checkRejects(context, "a section one byte shorter"_s, written.resized(written.size() - 1).span(), CBCheck::SummaryLength);
    checkRejects(context, "a section one byte longer"_s, written.resized(written.size() + 1).span(), CBCheck::SummaryLength);
    checkRejects(context, "a section one zero word longer"_s, written.resized(written.size() + sizeof(uint64_t)).span(), CBCheck::SummaryLength);
    for (size_t offset = offsets.padding; offset < offsets.size; ++offset) {
        SectionBytes bytes = written;
        bytes.mutableSpan()[offset] = 1;
        checkRejects(context, makeString("a nonzero padding byte at "_s, offset), bytes.span(), CBCheck::SummaryLength);
    }
    struct CountField {
        ASCIILiteral name;
        uint32_t SummaryHeader::* field;
    };
    const std::array countFields {
        CountField { "numArguments"_s, &SummaryHeader::numArguments },
        CountField { "numValueProfiles"_s, &SummaryHeader::numValueProfiles },
        CountField { "numArrayProfiles"_s, &SummaryHeader::numArrayProfiles },
        CountField { "numLazyOperandProfiles"_s, &SummaryHeader::numLazyOperandProfiles },
    };
    for (const auto& count : countFields) {
        SectionBytes atMaximum = withHeader(written, [&](SummaryHeader& header) {
            header.*count.field = std::numeric_limits<uint32_t>::max();
        });
        checkRejects(context, makeString(count.name, " at UINT32_MAX"_s), atMaximum.span(), CBCheck::SummaryLength);
        // One more 8-byte slot always changes the length; one more array flag can fall in the padding, which the edits
        // after this loop cover.
        if (count.field == &SummaryHeader::numArrayProfiles)
            continue;
        SectionBytes oneMore = withHeader(written, [&](SummaryHeader& header) {
            ++(header.*count.field);
        });
        checkRejects(context, makeString(count.name, " one higher"_s), oneMore.span(), CBCheck::SummaryLength);
    }
    SectionBytes oneArgumentFewer = withHeader(written, [](SummaryHeader& header) {
        --header.numArguments;
    });
    checkRejects(context, "numArguments one lower"_s, oneArgumentFewer.span(), CBCheck::SummaryLength);
    checkAcceptedBytes(context, "the last argument slot read as the first value slot"_s, withHeader(written, [](SummaryHeader& header) {
        --header.numArguments;
        ++header.numValueProfiles;
    }));
    if (summary.header.numValueProfiles) {
        checkAcceptedBytes(context, "the last value slot read as the first lazy-operand slot"_s, withHeader(written, [](SummaryHeader& header) {
            --header.numValueProfiles;
            ++header.numLazyOperandProfiles;
        }));
    }
    auto paddingBytes = static_cast<uint32_t>(offsets.size - offsets.padding);
    if (paddingBytes) {
        checkAcceptedBytes(context, "every padding byte read as an array flag"_s, withHeader(written, [&](SummaryHeader& header) {
            header.numArrayProfiles += paddingBytes;
        }));
    }
    SectionBytes pastPadding = withHeader(written, [&](SummaryHeader& header) {
        header.numArrayProfiles += paddingBytes + 1;
    });
    checkRejects(context, "one array flag more than the padding holds"_s, pastPadding.span(), CBCheck::SummaryLength);
    if (!summary.arrayFlags.isEmpty()) {
        // One array flag fewer turns the last flag into padding: the length still matches only when the padding absorbs
        // it, and then the section passes only if that flag is zero.
        for (uint8_t lastFlags : { uint8_t { 0 }, static_cast<uint8_t>(ArrayProfileFlag::OutOfBounds) }) {
            Summary model = summary;
            model.arrayFlags.last() = lastFlags;
            SectionBytes bytes = withHeader(serializeSummary(model), [](SummaryHeader& header) {
                --header.numArrayProfiles;
            });
            String what = makeString("one array flag fewer, the last one "_s, static_cast<unsigned>(lastFlags));
            if (summaryOffsets(bytes.read<SummaryHeader>(0)).size != bytes.size() || lastFlags)
                checkRejects(context, what, bytes.span(), CBCheck::SummaryLength);
            else
                checkAcceptedBytes(context, what, bytes);
        }
    }

    // V6 on each category: every slot within SpecBytecodeTop. A lazy-operand slot, with its count, gives the third array
    // a slot to edit.
    Summary withLazySlot = summary;
    withLazySlot.lazyOperandCategories.append(SpecInt32Only);
    checkAcceptedModel(context, "an added lazy-operand slot"_s, withLazySlot);
    struct CategoryArray {
        ASCIILiteral name;
        Vector<uint64_t> Summary::* slots;
    };
    for (auto array : { CategoryArray { "argument"_s, &Summary::argumentCategories }, CategoryArray { "value"_s, &Summary::valueCategories }, CategoryArray { "lazy-operand"_s, &Summary::lazyOperandCategories } }) {
        for (size_t index = 0; index < (withLazySlot.*array.slots).size(); ++index) {
            Summary model = withLazySlot;
            (model.*array.slots)[index] = SpecBytecodeTop;
            checkAcceptedModel(context, makeString(array.name, " slot "_s, index, " at SpecBytecodeTop"_s), model);
            (model.*array.slots)[index] = SpecBytecodeTop | outsideBytecodeTop;
            checkRejectedModel(context, makeString(array.name, " slot "_s, index, " one bit past SpecBytecodeTop"_s), model, CBCheck::SummaryValue);
            (model.*array.slots)[index] = outsideBytecodeTop;
            checkRejectedModel(context, makeString(array.name, " slot "_s, index, " holding only a bit outside SpecBytecodeTop"_s), model, CBCheck::SummaryValue);
        }
    }

    // V7's flag bound on each array flag: the eight ArrayProfileFlag bits fill a byte, so every byte is within the bound
    // and no flag value lies just outside it. The richness of a full byte leaves the pruning mark out.
    for (size_t position = 0; position < summary.arrayFlags.size(); ++position) {
        Summary model = summary;
        model.arrayFlags[position] = 0xff;
        checkAcceptedModel(context, makeString("array flags "_s, position, " with every bit"_s), model);
        model.arrayFlags[position] = pruningMark;
        checkAcceptedModel(context, makeString("array flags "_s, position, " with only the pruning mark"_s), model);
    }

    // The pruning mark adds no richness: marking every flag leaves the score of the unmarked summary.
    {
        Summary marked = summary;
        Summary unmarked = summary;
        for (auto& flags : marked.arrayFlags)
            flags = static_cast<uint8_t>(flags | pruningMark);
        for (auto& flags : unmarked.arrayFlags)
            flags = static_cast<uint8_t>(flags & ~pruningMark);
        auto markedScore = decodeSummary(serializeSummary(marked).span(), true);
        auto unmarkedScore = decodeSummary(serializeSummary(unmarked).span(), true);
        if (!markedScore || !unmarkedScore)
            JITCACHE_FAIL("decodeSummary rejected a summary whose flags differ only in the pruning mark"_s);
        else
            JITCACHE_CB_SUMMARY_CHECK_SCORE("every flag marked against every flag unmarked"_s, *markedScore, *unmarkedScore);
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
