#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayProfile.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "CollectionScope.h"
#include "EnsureStillAliveHere.h"
#include "FunctionExecutable.h"
#include "Heap.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "MetadataTable.h"
#include "SpeculatedType.h"
#include "UnlinkedCodeBlock.h"
#include "ValueProfile.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBFormat.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/tests/cb/CBTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// Tests of the CB lane's score (cb/CBCapture.cpp, decodeSummary in cb/CBCapture.cpp): the score of a capture is a
// function of the cb.summary it wrote, so capture, scoreLive and decodeSummary agree; after the native merges, each
// slot capture wrote equals the UCB copy it pairs with (argument i with UCB value profile i, value offset k with UCB
// value profile numArguments + k - 1, array position p with UCB array profile p), and the summary slot is the two ORed,
// a call site's slot holding its own this profile's flags; a builtin, whose UCB keeps no copies, has its summary slots
// from its own profiles alone; and decodeSummary scores any summary from its bytes alone, leaving out the pruning mark.
// Live tests build their CBs as the capture tests do: one call installs an LLInt CB, which JIT::compileSync compiles to
// baseline. Each test keeps its global object on its stack, and its source keeps every value the test passes in
// globals.

namespace JSC::JITCache::Tests {

namespace CBSummaryTestsInternal {

static constexpr uint8_t notCarried = static_cast<uint8_t>(CBFormat::CounterMode::NotCarried);
static constexpr uint8_t carried = static_cast<uint8_t>(CBFormat::CounterMode::Carried);
static constexpr uint8_t pruningMark = static_cast<uint8_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

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

// Section bytes of any length in 8-byte aligned storage, as the integrator hands a section over.
class SectionBytes {
public:
    explicit SectionBytes(size_t size)
        : m_words(FillWith { }, (size + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0)
        , m_size(size)
    {
    }

    std::span<const uint8_t> span() const { return asByteSpan(m_words.span()).first(m_size); }
    std::span<uint8_t> mutableSpan() { return asMutableByteSpan(m_words.mutableSpan()).first(m_size); }

    template<typename T> void write(size_t offset, const T& value) { writeAt(mutableSpan(), offset, value); }

private:
    Vector<uint64_t> m_words;
    size_t m_size { 0 };
};

// cb.summary's layout, computed from the format's element sizes, written out here, rather than by the code under test.
// Counts are 32-bit, so nothing here overflows a 64-bit size_t.
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

enum class CountPruningMark : bool { No, Yes };

// The score's richness recounted from the summary: one unit per category bit of an argument, value or lazy-operand
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

// The score the summary's bytes alone give.
static CBScore expectedScore(const Summary& summary)
{
    return { recountRichness(summary), summary.header.counterMode == notCarried, summary.header.counterProgress };
}

static void checkDecodes(TestContext& context, StringView what, std::span<const uint8_t> bytes, const CBScore& expected)
{
    checkScore(context, __FILE__, __LINE__, what, decodeSummary(bytes), expected);
}

// The bytes a model lays out decode to the score recounted from the model.
static void checkDecodesModel(TestContext& context, StringView what, const Summary& model)
{
    checkDecodes(context, what, serializeSummary(model).span(), expectedScore(model));
}

// The call-site families of the format's table, A11..A13 (op_call, op_call_ignore_result and op_tail_call), whose
// entries count as array positions and hold no cb.state record.
static bool isCallSiteFamily(unsigned family)
{
    return family >= 11 && family <= 13;
}

// The slots of cb.state that pair with UCB copies, which are its first four arrays (argument and value predictions,
// array profiles and lazy-operand predictions), at offsets computed here.
struct ScoredState {
    CBFormat::StateHeader header { };
    Vector<uint64_t> argumentPredictions;
    Vector<uint64_t> valuePredictions;
    Vector<CBFormat::ArrayProfileRecord> arrayProfiles; // the records of A0..A10, A14 and A15
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
    for (unsigned family = 0; family < CBFormat::numberOfArrayProfileFamilies; ++family) {
        if (!isCallSiteFamily(family))
            numArrayProfiles += state.header.familyEntryCount[family];
    }
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
    for (size_t record = 0; record < numArrayProfiles; ++record)
        state.arrayProfiles.append(readAt<CBFormat::ArrayProfileRecord>(bytes, arrayOffset + record * sizeof(CBFormat::ArrayProfileRecord)));
    for (size_t index = 0; index < state.header.numLazyOperandProfiles; ++index)
        state.lazyOperandPredictions.append(readAt<CBFormat::LazyOperandRecord>(bytes, lazyOperandOffset + index * sizeof(CBFormat::LazyOperandRecord)).prediction);
    return state;
}

// The counterProgress of the triple cb.state carries, computed here from the formula the capture applies.
static uint32_t expectedProgress(const CBFormat::StateHeader& header)
{
    if (header.counterMode != carried)
        return 0;
    double progress = static_cast<double>(header.counterTotalCount) + header.counterValue;
    if (!(progress > 0))
        return 0;
    return static_cast<uint32_t>(std::min(std::floor(progress), static_cast<double>(header.counterActiveThreshold)));
}

// What the live score test requires of its body: neighbouring profiles in each native walk saw different types
// (different array modes, for array profiles), so a lane walk one slot out of step with the native one would read a
// value that differs from the UCB copy it is compared with.
static void checkNeighboursDiffer(TestContext& context, UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto* profiles = unlinkedCodeBlock.valueAndArrayProfiles();
    if (!profiles) {
        JITCACHE_FAIL("the subject's UCB has no value and array profile copies"_s);
        return;
    }
    std::span<UnlinkedValueProfile> valueProfiles = profiles->valueProfiles();
    for (size_t index = 1; index < valueProfiles.size(); ++index) {
        if (valueProfiles[index - 1].prediction() == valueProfiles[index].prediction())
            JITCACHE_FAIL(makeString("the subject's UCB value profiles "_s, index - 1, " and "_s, index, " both hold "_s, valueProfiles[index].prediction(), ", so a walk out of step there would pass"_s));
    }
    std::span<UnlinkedArrayProfile> arrayProfiles = profiles->arrayProfiles();
    for (size_t position = 1; position < arrayProfiles.size(); ++position) {
        if (arrayProfiles[position - 1].observedArrayModes() == arrayProfiles[position].observedArrayModes())
            JITCACHE_FAIL(makeString("the subject's UCB array profiles "_s, position - 1, " and "_s, position, " both hold modes "_s, arrayProfiles[position].observedArrayModes(), ", so a walk out of step there would pass"_s));
    }
}

// The flags of each call site's own this profile, in the format's order: A11 (op_call), A12 (op_call_ignore_result) and
// A13 (op_tail_call), each in metadata-ID order. Nothing for a site that has no data of its own.
static Vector<std::optional<uint32_t>> callSiteThisFlags(CodeBlock& codeBlock)
{
    Vector<std::optional<uint32_t>> flags;
    MetadataTable* metadataTable = codeBlock.metadataTable();
    if (!metadataTable)
        return flags;
    auto append = [&](auto& metadata) {
        ArrayProfile* profile = metadata.m_callLinkInfo.arrayProfile();
        flags.append(profile ? std::optional<uint32_t> { profile->arrayProfileFlags().toRaw() } : std::nullopt);
    };
    metadataTable->forEach<OpCall>(append);
    metadataTable->forEach<OpCallIgnoreResult>(append);
    metadataTable->forEach<OpTailCall>(append);
    return flags;
}

// The summary slot of every call site: the flags of its own this profile ORed with its UCB copy's, which a UCB without
// copies, a builtin's among them, leaves out; a site with no data of its own adds no flag. Walks the array positions of
// A0..A15 in the format's order and returns, per call site, its position.
static Vector<size_t> checkCallSiteSlots(TestContext& context, StringView what, const ScoredState& state, const Summary& summary, const Vector<std::optional<uint32_t>>& callSiteFlags, std::span<const UnlinkedArrayProfile> unlinkedArrayProfiles)
{
    Vector<size_t> positions;
    size_t position = 0;
    for (unsigned family = 0; family < CBFormat::numberOfArrayProfileFamilies; ++family) {
        for (uint32_t entry = 0; entry < state.header.familyEntryCount[family]; ++entry, ++position) {
            if (isCallSiteFamily(family))
                positions.append(position);
        }
    }
    if (positions.size() != callSiteFlags.size() || summary.arrayFlags.size() != position) {
        JITCACHE_FAIL(makeString(what, ": cb.state counts "_s, positions.size(), " call sites and "_s, position, " array positions, the CB "_s, callSiteFlags.size(), " call sites and cb.summary "_s, summary.arrayFlags.size(), " array flags"_s));
        return { };
    }
    for (size_t site = 0; site < positions.size(); ++site) {
        size_t sitePosition = positions[site];
        uint32_t unlinkedFlags = unlinkedArrayProfiles.empty() ? 0 : unlinkedArrayProfiles[sitePosition].arrayProfileFlags().toRaw();
        auto expected = static_cast<uint8_t>(callSiteFlags[site].value_or(0) | unlinkedFlags);
        if (summary.arrayFlags[sitePosition] != expected)
            JITCACHE_FAIL(makeString(what, ": the call site at array position "_s, sitePosition, " holds flags "_s, static_cast<unsigned>(summary.arrayFlags[sitePosition]), " in cb.summary, not its own this profile's ORed with its UCB copy's, "_s, static_cast<unsigned>(expected)));
    }
    return positions;
}

// After the native merges, each cb.state slot equals the UCB copy it pairs with: the prediction for an argument or
// value slot; the modes, and the flags without the pruning mark, which the merge keeps out of the UCB copy, for an array
// profile, whose families follow the native merge order. Each summary slot is its cb.state slot ORed with that copy,
// a call site's is its own this profile's flags ORed with that copy, and a lazy-operand slot, which has no copy, is its
// cb.state prediction. A call site has no cb.state record but keeps its array position, so the records of A14 and A15
// pair with the copies past the call sites.
static void checkPairing(TestContext& context, StringView what, const ScoredState& state, const Summary& summary, UnlinkedCodeBlock& unlinkedCodeBlock, const Vector<std::optional<uint32_t>>& callSiteFlags)
{
    auto* profiles = unlinkedCodeBlock.valueAndArrayProfiles();
    if (!profiles) {
        JITCACHE_FAIL(makeString(what, ": the UCB has no value and array profile copies"_s));
        return;
    }
    std::span<UnlinkedValueProfile> unlinkedValueProfiles = profiles->valueProfiles();
    std::span<UnlinkedArrayProfile> unlinkedArrayProfiles = profiles->arrayProfiles();
    size_t numArguments = state.argumentPredictions.size();
    size_t numCallSites = 0;
    for (unsigned family = 0; family < CBFormat::numberOfArrayProfileFamilies; ++family) {
        if (isCallSiteFamily(family))
            numCallSites += state.header.familyEntryCount[family];
    }
    size_t numArrayPositions = state.arrayProfiles.size() + numCallSites;
    if (unlinkedValueProfiles.size() != numArguments + state.valuePredictions.size() || unlinkedArrayProfiles.size() != numArrayPositions) {
        JITCACHE_FAIL(makeString(what, ": the UCB copies are not as many as the slots they pair with"_s));
        return;
    }
    if (summary.argumentCategories.size() != numArguments || summary.valueCategories.size() != state.valuePredictions.size()
        || summary.lazyOperandCategories.size() != state.lazyOperandPredictions.size() || summary.arrayFlags.size() != numArrayPositions) {
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
    size_t recordIndex = 0;
    size_t position = 0;
    for (unsigned family = 0; family < CBFormat::numberOfArrayProfileFamilies; ++family) {
        for (uint32_t entry = 0; entry < state.header.familyEntryCount[family]; ++entry, ++position) {
            if (isCallSiteFamily(family))
                continue;
            const auto& record = state.arrayProfiles[recordIndex++];
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
    checkCallSiteSlots(context, what, state, summary, callSiteFlags, unlinkedArrayProfiles);
}

// The live score test's body. `subject` has an A15 entry, the iterable profile of the iterator_next that
// destructuring emits, and warm runs it on both iteration paths: a fast one over an Int32 array and a generic one over
// `custom`, whose iterator is an array of objects carrying next and return, so every profile of the iterator opcodes
// and of the iterator close (the get_by_id of return and its call) sees a value. In the order the generator gives
// them, the profiles see:
//   arguments: undefined (this), an array, an array or a plain object, an array, a string;
//   value profiles: a function (the Symbol.iterator get), an array or a plain object (the iterable), an array
//   iterator or an array (the iterator), a function (next), a plain object (next's result), a boolean (done), an int or
//   a double (value), undefined or a function (the return get), a plain object (return's result), an int (ints[0]),
//   a double (doubles[1]);
//   array profiles: Int32 (ints[0]), Double (doubles[1]), Contiguous (the return call's this, an A11 call site whose
//   profile its own CallSiteData holds once the call ran twice), NonArray (the iterable of a generic iterator_open),
//   Int32 then Double (A15, after iterateDoubles).
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

// The decode test's body: argument, value and array slots beside the header, and padding after its one flag.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }"
    "var array = [1, 2, 3];"
    "function run(rounds) { for (var round = 0; round < rounds; ++round) element(array, round % 3); }"_s;

// The builtin test's source. One call of run makes Array.prototype.forEach call its callback four times with an object
// as this: the callback's call site gets data of its own at its second call, and its own this profile sees the
// receiver at the later ones. forEach's call of @toLength runs once, so that site never gets data of its own.
static constexpr auto builtinSource = "var forEach = Array.prototype.forEach;"
    "var elements = [1, 2, 3, 4];"
    "var receiver = { weight: 2 };"
    "var total = 0;"
    "function add(element) { total += element * this.weight; }"
    "function run() { elements.forEach(add, receiver); }"_s;

} // namespace CBSummaryTestsInternal

using namespace CBSummaryTestsInternal;

// Right after a synchronous full collection, in a VM with useConcurrentJIT off and with no JS run in between, capture
// and scoreLive give one score, which decodeSummary and a recount give again from the written summary, with the
// polymorphic bit set and clear. The recount leaves the pruning mark out, and the collection's merges make
// every cb.state slot equal the UCB copy it pairs with, by array positions that count the call sites, whose summary
// slots hold their own this profiles' flags ORed with their copies'. The subject stays below
// maximumOptimizationCandidateBytecodeCost, so the collection's drain folds its value samples into predictions.
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
    CodeBlock* codeBlock = compileToBaseline(context, subject);
    if (!codeBlock)
        return;
    if (!callFunction(context, globalObject, warm, { jsNumber(3) }))
        return;

    // This collection's drain gives A15 the Int32 array's mode. Iterating a Double array then gives it a second mode,
    // which the next drain prunes, setting the pruning mark (ArrayProfile::computeUpdatedPrediction).
    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (!callFunction(context, globalObject, iterateDoubles, { jsNumber(2) }))
        return;

    // The finalization of this collection drains every visited CB and runs the native merges. From here on the test
    // runs no JS, and with useConcurrentJIT off no compiler thread drains, so nothing changes the CB between the calls.
    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (subject->jsExecutable()->codeBlockForCall() != codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("the collections replaced the subject's baseline CB"_s);
        return;
    }
    if (codeBlock->valueProfilePredictionsAreNeverRead()) {
        JITCACHE_FAIL("the subject's drains keep its value samples in their buckets"_s);
        return;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
    checkNeighboursDiffer(context, unlinkedCodeBlock);

    // The call site whose own this profile the summary reads: the return call ran five times, so it has data of its
    // own.
    Vector<std::optional<uint32_t>> callSiteFlags = callSiteThisFlags(*codeBlock);
    if (!std::ranges::any_of(callSiteFlags, [](const std::optional<uint32_t>& flags) { return flags.has_value(); })) {
        JITCACHE_FAIL("no call site of the subject has data of its own"_s);
        return;
    }

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    for (bool hasPolymorphicSite : { false, true }) {
        String what = hasPolymorphicSite ? "the capture with the polymorphic bit"_s : "the capture without the polymorphic bit"_s;
        CBScore liveScore = CBStateCapture::scoreLive(*codeBlock, hasPolymorphicSite);
        auto captured = CBStateCapture::capture(*codeBlock, budget.get(), hasPolymorphicSite);
        if (!captured) {
            JITCACHE_FAIL(makeString(what, ": capture refused its charge"_s));
            continue;
        }
        const CBScore& score = captured->score();
        JITCACHE_CB_SUMMARY_CHECK_SCORE(makeString(what, ", scoreLive against score()"_s), liveScore, score);

        auto state = parseState(context, captured->stateSection());
        auto summary = parseSummary(context, captured->summarySection());
        if (!state || !summary)
            continue;

        // The summary's bytes alone give the score: a recount, and decodeSummary.
        JITCACHE_CB_SUMMARY_CHECK_SCORE(makeString(what, ", recount of the written summary"_s), expectedScore(*summary), score);
        checkDecodes(context, makeString(what, ", decodeSummary of the written summary"_s), captured->summarySection(), score);

        // The recount leaves out the pruning mark, which the A15 flags carry after the second collection.
        auto markedFlags = static_cast<size_t>(std::ranges::count_if(summary->arrayFlags, [](uint8_t flags) -> bool {
            return flags & pruningMark;
        }));
        if (!markedFlags)
            JITCACHE_FAIL(makeString(what, ": no array flag carries the pruning mark, so the summary cannot show that richness ignores it"_s));
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", richness with the pruning mark counted"_s), recountRichness(*summary, CountPruningMark::Yes), score.richnessUnits + markedFlags);

        // The counter as the polymorphic bit decides it, and the progress of the triple cb.state carries.
        uint8_t counterMode = hasPolymorphicSite ? notCarried : carried;
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.state counterMode"_s), state->header.counterMode, counterMode);
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.summary counterMode"_s), summary->header.counterMode, counterMode);
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", counterWithheld"_s), score.counterWithheld, hasPolymorphicSite);
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", counterProgress"_s), score.counterProgress, expectedProgress(state->header));
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", cb.summary counterProgress"_s), summary->header.counterProgress, score.counterProgress);

        // The body's A15 entry, and the pairing of every slot with its UCB copy.
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString(what, ", A15 entries"_s), state->header.familyEntryCount[CBFormat::numberOfArrayProfileFamilies - 1], 1);
        checkPairing(context, what, *state, *summary, unlinkedCodeBlock, callSiteFlags);
    }

    ensureStillAliveHere(globalObject);
}

// A baseline CB of a builtin, whose UCB keeps no copies, has its summary slots from its own profiles alone. Right after a
// synchronous full collection, whose finalization drains forEach's baseline CB, in a VM with useConcurrentJIT off and
// with no JS run in between, the slot of the callback's call site holds the flags of that site's own this profile,
// UsesNonOriginalArrayStructures among them, and richnessUnits counts them; the slot of the call site that never got
// data of its own is 0.
JITCACHE_TEST_WITH_OPTIONS(cbSummaryBuiltinCallSitesScoreTheirOwnThisProfiles, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, builtinSource);
    if (!globalObject)
        return;
    JSFunction* forEach = globalFunction(context, globalObject, "forEach"_s);
    JSFunction* run = globalFunction(context, globalObject, "run"_s);
    if (!forEach || !run)
        return;
    if (!callFunction(context, globalObject, run, { }))
        return;
    CodeBlock* codeBlock = compileToBaseline(context, forEach);
    if (!codeBlock)
        return;
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
    if (!unlinkedCodeBlock.isBuiltinFunction() || unlinkedCodeBlock.valueAndArrayProfiles()) {
        JITCACHE_FAIL("forEach's UCB is no builtin's, or keeps profile copies"_s);
        return;
    }

    // The collection's drain gives the callback site's own this profile the receiver's structure, which is no original
    // array structure.
    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (forEach->jsExecutable()->codeBlockForCall() != codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("the collection replaced forEach's baseline CB"_s);
        return;
    }
    constexpr uint32_t nonOriginal = static_cast<uint32_t>(ArrayProfileFlag::UsesNonOriginalArrayStructures);
    Vector<std::optional<uint32_t>> callSiteFlags = callSiteThisFlags(*codeBlock);
    bool hasOwnNonOriginalProfile = std::ranges::any_of(callSiteFlags, [&](const std::optional<uint32_t>& flags) {
        return flags && (*flags & nonOriginal);
    });
    bool hasSiteWithoutData = std::ranges::any_of(callSiteFlags, [](const std::optional<uint32_t>& flags) {
        return !flags;
    });
    if (!hasOwnNonOriginalProfile || !hasSiteWithoutData) {
        JITCACHE_FAIL(makeString("forEach's "_s, callSiteFlags.size(), " call sites hold no own this profile with UsesNonOriginalArrayStructures, or none lacks data of its own"_s));
        return;
    }

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    CBScore liveScore = CBStateCapture::scoreLive(*codeBlock, false);
    auto captured = CBStateCapture::capture(*codeBlock, budget.get(), false);
    if (!captured) {
        JITCACHE_FAIL("capture refused its charge"_s);
        return;
    }
    const CBScore& score = captured->score();
    JITCACHE_CB_SUMMARY_CHECK_SCORE("forEach's scoreLive against score()"_s, liveScore, score);
    auto state = parseState(context, captured->stateSection());
    auto summary = parseSummary(context, captured->summarySection());
    if (!state || !summary)
        return;
    JITCACHE_CB_SUMMARY_CHECK_SCORE("the recount of forEach's summary"_s, expectedScore(*summary), score);

    // Every call site's slot is its own this profile's flags alone, since a builtin has no copies to OR in.
    Vector<size_t> positions = checkCallSiteSlots(context, "forEach"_s, *state, *summary, callSiteFlags, { });
    if (positions.size() != callSiteFlags.size())
        return;
    bool countedOwnFlags = false;
    for (size_t site = 0; site < positions.size(); ++site) {
        uint8_t slot = summary->arrayFlags[positions[site]];
        if (!callSiteFlags[site]) {
            JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString("the slot of forEach's call site "_s, site, ", which has no data of its own"_s), slot, 0);
            continue;
        }
        if (!(slot & nonOriginal))
            continue;
        // A summary without the slot's flags scores exactly their units fewer.
        Summary withoutSlot = *summary;
        withoutSlot.arrayFlags[positions[site]] = 0;
        auto slotUnits = static_cast<uint64_t>(std::popcount(static_cast<unsigned>(slot & ~pruningMark)));
        JITCACHE_CB_SUMMARY_CHECK_EQUAL(makeString("forEach's richness without the flags of call site "_s, site), expectedScore(withoutSlot).richnessUnits + slotUnits, score.richnessUnits);
        countedOwnFlags = true;
    }
    if (!countedOwnFlags)
        JITCACHE_FAIL("no summary slot of forEach holds UsesNonOriginalArrayStructures"_s);

    ensureStillAliveHere(globalObject);
}

// decodeSummary gives the score of a summary's bytes alone: the summaries capture wrote, with the counter carried and
// withheld, decode to capture's own scores, and summaries built from the carried one decode to the score the test
// recounts from their bytes, through an added lazy-operand slot and through flag bytes with every bit or only the pruning
// mark; marking every flag leaves the score of every flag unmarked.
JITCACHE_TEST(cbSummaryDecodeScoresItsBytes, Yes)
{
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
    CodeBlock* codeBlock = compileToBaseline(context, element);
    if (!codeBlock)
        return;
    if (!callFunction(context, globalObject, run, { jsNumber(3) }))
        return;

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto carriedCapture = CBStateCapture::capture(*codeBlock, budget.get(), false);
    auto withheldCapture = CBStateCapture::capture(*codeBlock, budget.get(), true);
    if (!carriedCapture || !withheldCapture) {
        JITCACHE_FAIL("capture refused its charge"_s);
        return;
    }
    ensureStillAliveHere(globalObject);

    auto parsed = parseSummary(context, carriedCapture->summarySection());
    auto parsedWithheld = parseSummary(context, withheldCapture->summarySection());
    if (!parsed || !parsedWithheld)
        return;
    const Summary& summary = *parsed;

    // The written summaries: capture's score, through the recount and through decodeSummary.
    JITCACHE_CB_SUMMARY_CHECK_SCORE("recount of the carried summary"_s, expectedScore(summary), carriedCapture->score());
    JITCACHE_CB_SUMMARY_CHECK_SCORE("recount of the withheld summary"_s, expectedScore(*parsedWithheld), withheldCapture->score());
    checkDecodes(context, "the carried summary"_s, carriedCapture->summarySection(), carriedCapture->score());
    checkDecodes(context, "the withheld summary"_s, withheldCapture->summarySection(), withheldCapture->score());

    // A lazy-operand slot, with its count, adds its categories as the slots of the other two arrays do.
    Summary withLazySlot = summary;
    withLazySlot.lazyOperandCategories.append(SpecInt32Only);
    checkDecodesModel(context, "an added lazy-operand slot"_s, withLazySlot);

    // A flag byte with every bit, and one with only the pruning mark, score their flags without the mark.
    for (size_t position = 0; position < summary.arrayFlags.size(); ++position) {
        Summary model = summary;
        model.arrayFlags[position] = 0xff;
        checkDecodesModel(context, makeString("array flags "_s, position, " with every bit"_s), model);
        model.arrayFlags[position] = pruningMark;
        checkDecodesModel(context, makeString("array flags "_s, position, " with only the pruning mark"_s), model);
    }

    // The pruning mark adds no richness: marking every flag leaves the score of the unmarked summary.
    {
        Summary marked = summary;
        Summary unmarked = summary;
        for (auto& flags : marked.arrayFlags)
            flags = static_cast<uint8_t>(flags | pruningMark);
        for (auto& flags : unmarked.arrayFlags)
            flags = static_cast<uint8_t>(flags & ~pruningMark);
        CBScore markedScore = decodeSummary(serializeSummary(marked).span());
        CBScore unmarkedScore = decodeSummary(serializeSummary(unmarked).span());
        JITCACHE_CB_SUMMARY_CHECK_SCORE("every flag marked against every flag unmarked"_s, markedScore, unmarkedScore);
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
