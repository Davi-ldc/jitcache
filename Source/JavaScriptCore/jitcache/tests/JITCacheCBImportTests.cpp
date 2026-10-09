#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "ArrayConventions.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeIndex.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "FunctionExecutable.h"
#include "IndexingType.h"
#include "IterationModeMetadata.h"
#include "JIT.h"
#include "JITCacheCBFormat.h"
#include "JITCacheCBState.h"
#include "JITCacheTest.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSPropertyNameEnumerator.h"
#include "Operands.h"
#include "Options.h"
#include "ParserModes.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "SpeculatedType.h"
#include "ToThisStatus.h"
#include "TopExceptionScope.h"
#include "UnlinkedCodeBlock.h"
#include "VirtualRegister.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TriState.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The import tests of SPEC-cb.md task 5 (section 11.2): U2, U3, U4, U7 and U8. Each test runs on a fresh VM and builds its
// CBs as SPEC-ics.md's live tests do: it evaluates its source in its own global object, one call through JSC::call
// installs an LLInt CB, and JIT::compileSync compiles that CB to baseline. A newborn CB comes from newCodeBlockFor on a
// function never called, inside a DeferGCForAWhile; it is never installed, so the same function yields one each time,
// and the test keeps it in a local. Two functions with the same body text have separate UCBs with the same metadata
// layout, so a section captured from one's baseline CB pairs with the other's newborn CB. A CB after setup is such a
// newborn CB taken through prepare, seedLinkedState, setupWithUnlinkedBaselineCode with the first function's code, and
// finishCounter. Every lane call here runs with strict on, and every section starts as a strict capture of a real CB,
// which the tests decode, change and lay out again.

namespace JSC::JITCache::Tests {

namespace CBImportTestsInternal {

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

static JSValue globalValue(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return globalObject->get(globalObject, Identifier::fromString(globalObject->vm(), name));
}

static JSFunction* globalFunction(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, name));
    if (!function || function->isHostFunction()) {
        JITCACHE_FAIL(makeString("the test source defines no JS function "_s, name));
        return nullptr;
    }
    return function;
}

static std::optional<JSValue> globalObjectValue(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    JSValue value = globalValue(globalObject, name);
    if (!value.isObject()) {
        JITCACHE_FAIL(makeString("the test source defines no object "_s, name));
        return std::nullopt;
    }
    return value;
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

// Compiles the function's LLInt CB to baseline in place, as JIT::compileSync does for any CB the LLInt has run, and
// checks that the CB is still its executable's code.
static CodeBlock* compileToBaseline(TestContext& context, JSFunction* function)
{
    VM& vm = function->vm();
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not leave an LLInt CB"_s);
        return nullptr;
    }
    CompilationResult result;
    {
        DeferGCForAWhile deferGC(vm);
        result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    }
    if (result != CompilationResult::CompilationSuccessful || codeBlock->jitType() != JITType::BaselineJIT || !codeBlock->baselineJITData()) {
        JITCACHE_FAIL("JIT::compileSync did not install baseline code in the LLInt CB"_s);
        return nullptr;
    }
    if (function->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("the baseline CB is not its executable's code"_s);
        return nullptr;
    }
    return codeBlock;
}

// The function's baseline CB: one call installs its LLInt CB, and compileSync compiles baseline code into that same CB.
static CodeBlock* bringToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, std::initializer_list<JSValue> arguments)
{
    if (!callFunction(context, globalObject, function, arguments))
        return nullptr;
    return compileToBaseline(context, function);
}

// The body of the async generator a global holds. An async generator's wrapper creates its body as a function expression
// and stores it in the generator's Next field without calling it (FunctionNode::emitBytecode,
// AsyncGeneratorWrapperFunctionMode), and the body first runs when something resumes the generator, as its first next()
// does (asyncGeneratorBodyCall). The body of a generator nothing has resumed is therefore a function never called, whose
// executable has no CB yet, so createNewbornCodeBlock takes it as SPEC-cb.md section 11.2 takes any such function.
static JSFunction* asyncGeneratorBody(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral generatorName)
{
    auto* generator = dynamicDowncast<JSAsyncGenerator>(globalValue(globalObject, generatorName));
    auto* body = generator ? dynamicDowncast<JSFunction>(generator->next()) : nullptr;
    if (!body || body->isHostFunction() || body->jsExecutable()->parseMode() != SourceParseMode::AsyncGeneratorBodyMode) {
        JITCACHE_FAIL(makeString("the test source defines no async generator "_s, generatorName, " holding its body"_s));
        return nullptr;
    }
    return body;
}

// The baseline CB of an async generator's body: one call of start, which calls the generator's next(), runs the body in
// the LLInt until its first await, and compileSync compiles baseline code into that same CB.
static CodeBlock* bringAsyncGeneratorBodyToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* start, JSFunction* body)
{
    if (!callFunction(context, globalObject, start, { }))
        return nullptr;
    return compileToBaseline(context, body);
}

// The newborn CB of a function never called: linked by finishCreation inside a DeferGCForAWhile, as newCodeBlockFor
// requires, and never installed or run.
static CodeBlock* createNewbornCodeBlock(TestContext& context, JSFunction* function)
{
    VM& vm = function->vm();
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (function->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("the function for a newborn CB was already called"_s);
        return nullptr;
    }
    CodeBlock* codeBlock = function->jsExecutable()->newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope());
    if (scope.exception()) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor threw"_s);
        return nullptr;
    }
    if (!codeBlock || codeBlock->jitType() != JITType::None) {
        JITCACHE_FAIL("newCodeBlockFor gave no newborn CB"_s);
        return nullptr;
    }
    return codeBlock;
}

// The code a baseline CB runs, held by the test so that it outlives anything done to the CB.
static Ref<BaselineJITCode> baselineCodeOf(CodeBlock& codeBlock)
{
    RefPtr jitCode = codeBlock.jitCode();
    RELEASE_ASSERT(jitCode && jitCode->jitType() == JITType::BaselineJIT);
    return Ref { static_cast<BaselineJITCode&>(*jitCode) };
}

static void failWith(TestContext& context, const String& step, const CBFault& fault)
{
    JITCACHE_FAIL(makeString(step, " failed with "_s, fault.kind == CBFaultKind::RecordingFault ? "a recording fault"_s : "invalid material"_s, ": "_s, description(fault.check)));
}

// cb.state decoded into its header and its nine arrays (section 3.2), so a test can change a field or a count and lay
// the section out again.
struct StateImage {
    CBFormat::StateHeader header { };
    Vector<uint64_t> argumentPredictions;
    Vector<uint64_t> valuePredictions;
    Vector<CBFormat::ArrayProfileRecord> arrayProfiles;
    Vector<CBFormat::LazyOperandRecord> lazyOperandProfiles;
    Vector<uint16_t> allocationHints;
    Vector<uint16_t> iterationModes;
    Vector<uint8_t> enumeratorModes;
    Vector<uint8_t> toThisStatuses;
    Vector<uint8_t> branchBits;
};

// Calls functor(StateArray, elements) for each array of the image, in section order.
template<typename Image, typename Functor>
static void forEachImageArray(Image& image, const Functor& functor)
{
    using CBFormat::StateArray;
    functor(StateArray::ArgumentPredictions, image.argumentPredictions);
    functor(StateArray::ValuePredictions, image.valuePredictions);
    functor(StateArray::ArrayProfiles, image.arrayProfiles);
    functor(StateArray::LazyOperandProfiles, image.lazyOperandProfiles);
    functor(StateArray::AllocationHints, image.allocationHints);
    functor(StateArray::IterationModes, image.iterationModes);
    functor(StateArray::EnumeratorModes, image.enumeratorModes);
    functor(StateArray::ToThisStatuses, image.toThisStatuses);
    functor(StateArray::BranchBits, image.branchBits);
}

static std::optional<StateImage> decodeState(TestContext& context, std::span<const uint8_t> section)
{
    StateImage image;
    if (section.size() < sizeof(CBFormat::StateHeader)) {
        JITCACHE_FAIL("the cb.state to decode is shorter than its header"_s);
        return std::nullopt;
    }
    memcpySpan(asMutableByteSpan(image.header), section.first(sizeof(CBFormat::StateHeader)));
    auto layout = CBFormat::stateLayout(image.header);
    if (!layout || layout->size != section.size()) {
        JITCACHE_FAIL("the cb.state to decode does not have the length its header lays out"_s);
        return std::nullopt;
    }
    forEachImageArray(image, [&](CBFormat::StateArray array, auto& elements) {
        using Element = typename std::remove_cvref_t<decltype(elements)>::value_type;
        for (size_t index = 0; index < layout->count(array); ++index) {
            Element element { };
            memcpySpan(asMutableByteSpan(element), section.subspan(layout->elementOffset(array, index), sizeof(Element)));
            elements.append(element);
        }
    });
    return image;
}

// The section an image lays out: its header as it stands, the arrays at the offsets its counts give, and zero padding,
// in 64-bit words so that it starts 8-byte aligned (R-INT-1). A test that changes a count changes the matching array
// with it, so arrays that disagree with the header are the test's own mistake.
static std::optional<Vector<uint64_t>> encodeState(TestContext& context, const StateImage& image)
{
    auto layout = CBFormat::stateLayout(image.header);
    if (!layout) {
        JITCACHE_FAIL("the image's counts overflow the cb.state layout"_s);
        return std::nullopt;
    }
    bool consistent = true;
    forEachImageArray(image, [&](CBFormat::StateArray array, const auto& elements) {
        consistent &= elements.size() == layout->count(array);
    });
    if (!consistent) {
        JITCACHE_FAIL("the image's arrays disagree with its header's counts"_s);
        return std::nullopt;
    }
    Vector<uint64_t> words(FillWith { }, layout->size / sizeof(uint64_t), 0);
    std::span<uint8_t> bytes = asMutableByteSpan(words.mutableSpan());
    memcpySpan(bytes.first(sizeof(CBFormat::StateHeader)), asByteSpan(image.header));
    forEachImageArray(image, [&](CBFormat::StateArray array, const auto& elements) {
        for (size_t index = 0; index < elements.size(); ++index)
            memcpySpan(bytes.subspan(layout->elementOffset(array, index), sizeof(elements[index])), asByteSpan(elements[index]));
    });
    return words;
}

static std::span<const uint8_t> bytesOf(const Vector<uint64_t>& words)
{
    return asByteSpan(words.span());
}

// A strict capture of the CB with its counter carried, decoded. The test's codec must lay the image out again as capture
// did, so every changed section the tests build differs from a real one only where the test changed it.
static std::optional<StateImage> captureState(TestContext& context, CodeBlock& codeBlock)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto captured = CBStateCapture::capture(codeBlock, budget.get(), true, false);
    if (!captured) {
        failWith(context, "capture"_s, captured.error());
        return std::nullopt;
    }
    auto image = decodeState(context, captured->stateSection());
    if (!image)
        return std::nullopt;
    auto words = encodeState(context, *image);
    if (!words)
        return std::nullopt;
    if (!equalSpans(bytesOf(*words), captured->stateSection())) {
        JITCACHE_FAIL("the test's codec does not reproduce the captured cb.state"_s);
        return std::nullopt;
    }
    return image;
}

// The family of the table (section 3.1) that holds the field of Op of the given kind.
template<CBFormat::FamilyKind kind, typename Op>
static unsigned familyOf()
{
    unsigned result = CBFormat::numberOfFamilies;
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        if constexpr (FamilyType::kind == kind && std::is_same_v<typename FamilyType::Op, Op>)
            result = FamilyType::index;
    });
    RELEASE_ASSERT(result < CBFormat::numberOfFamilies);
    return result;
}

// The index, within its kind's array, of entry `entry` of the family.
static size_t recordIndex(const StateImage& image, unsigned family, uint32_t entry)
{
    return CBFormat::firstRecordOfFamily(image.header.familyEntryCount, family) + entry;
}

static bool hasEntries(TestContext& context, const StateImage& image, unsigned family, uint32_t minimum, ASCIILiteral what)
{
    if (image.header.familyEntryCount[family] >= minimum)
        return true;
    JITCACHE_FAIL(makeString("the subject has "_s, image.header.familyEntryCount[family], " entries in family "_s, family, ", fewer than the "_s, minimum, " "_s, what, " needs"_s));
    return false;
}

// Adds a record after the family's last one and counts it, keeping the header and the arrays in step.
template<typename Record>
static void appendFamilyRecord(StateImage& image, Vector<Record>& records, unsigned family, const Record& record)
{
    records.insert(recordIndex(image, family, image.header.familyEntryCount[family]), record);
    ++image.header.familyEntryCount[family];
}

template<typename Record>
static void removeLastFamilyRecord(StateImage& image, Vector<Record>& records, unsigned family)
{
    --image.header.familyEntryCount[family];
    records.removeAt(recordIndex(image, family, image.header.familyEntryCount[family]));
}

// The baseline counter's three fields.
struct CounterTriple {
    int32_t value { 0 };
    float totalCount { 0 };
    int32_t activeThreshold { 0 };

    friend bool operator==(const CounterTriple&, const CounterTriple&) = default;
};

static CounterTriple liveCounter(CodeBlock& codeBlock)
{
    const BaselineExecutionCounter& counter = codeBlock.baselineJITData()->executeCounter();
    return { counter.m_counter, counter.m_totalCount, counter.m_activeThreshold };
}

static void setCounter(StateImage& image, CBFormat::CounterMode mode, const CounterTriple& triple)
{
    image.header.counterMode = static_cast<uint8_t>(mode);
    image.header.counterValue = triple.value;
    image.header.counterTotalCount = triple.totalCount;
    image.header.counterActiveThreshold = triple.activeThreshold;
}

static CBFormat::LazyOperandRecord lazyOperandRecord(uint32_t bytecodeIndexBits, uint32_t operandKind, int32_t operandValue, uint64_t prediction = SpecInt32Only)
{
    return { .bytecodeIndexBits = bytecodeIndexBits, .operandKind = operandKind, .operandValue = operandValue, .reserved = 0, .prediction = prediction };
}

static void setLazyOperands(StateImage& image, const Vector<CBFormat::LazyOperandRecord>& records)
{
    image.lazyOperandProfiles = records;
    image.header.numLazyOperandProfiles = static_cast<uint32_t>(records.size());
}

static constexpr uint32_t argumentKind = static_cast<uint32_t>(OperandKind::Argument);
static constexpr uint32_t localKind = static_cast<uint32_t>(OperandKind::Local);
static constexpr uint32_t tmpKind = static_cast<uint32_t>(OperandKind::Tmp);

// A prediction bit no bytecode slot holds (V6), and an array mode bit no structure has (V7).
static_assert(!!(SpecFullTop & ~SpecBytecodeTop));
static constexpr SpeculatedType predictionOutsideBytecodeTop = SpeculatedType { 1 } << std::countr_zero(static_cast<uint64_t>(SpecFullTop & ~SpecBytecodeTop));
static_assert(!!static_cast<ArrayModes>(~ALL_ARRAY_MODES));
static constexpr ArrayModes arrayModeOutsideAllModes = ArrayModes { 1 } << std::countr_zero(static_cast<ArrayModes>(~ALL_ARRAY_MODES));

static constexpr uint16_t iterationModeBit(IterationMode mode)
{
    return static_cast<uint16_t>(mode);
}

static ASCIILiteral outcomeName(std::optional<CBCheck> check)
{
    return check ? description(*check) : "acceptance"_s;
}

// One newborn CB with the image of a section captured from a baseline CB of the same body text, and that image laid out.
struct Subject {
    CodeBlock* newborn { nullptr };
    StateImage image;
    Vector<uint64_t> words;

    std::span<const uint8_t> section() const { return bytesOf(words); }
};

static std::optional<Subject> makeSubject(TestContext& context, CodeBlock* newborn, std::optional<StateImage>&& image)
{
    if (!newborn || !image)
        return std::nullopt;
    auto words = encodeState(context, *image);
    if (!words)
        return std::nullopt;
    return Subject { newborn, WTF::move(*image), WTF::move(*words) };
}

// U2's oracle: validateState and a strict prepare on the newborn CB both give `expected`, a fault of prepare is invalid
// material, and the CB still passes S2 with the unchanged section afterwards, so prepare wrote nothing (I5).
static void expectOutcome(TestContext& context, const String& what, std::span<const uint8_t> section, const Subject& subject, std::optional<CBCheck> expected)
{
    std::optional<CBCheck> validated = validateState(section, *subject.newborn);
    if (validated != expected)
        JITCACHE_FAIL(makeString(what, ": validateState gave "_s, outcomeName(validated), ", expected "_s, outcomeName(expected)));

    auto prepared = CBStateImport::prepare(section, *subject.newborn, true);
    std::optional<CBCheck> preparedCheck;
    if (!prepared) {
        if (prepared.error().kind != CBFaultKind::InvalidMaterial)
            JITCACHE_FAIL(makeString(what, ": prepare reported a recording fault"_s));
        preparedCheck = prepared.error().check;
    }
    if (preparedCheck != expected)
        JITCACHE_FAIL(makeString(what, ": prepare gave "_s, outcomeName(preparedCheck), ", expected "_s, outcomeName(expected)));

    auto probe = CBStateImport::prepare(subject.section(), *subject.newborn, true);
    if (!probe)
        JITCACHE_FAIL(makeString(what, ": the CB no longer passes the unchanged section ("_s, description(probe.error().check), ')'));
}

// Changes a copy of the subject's image, lays it out and checks the outcome.
template<typename Mutation>
static void expectMutation(TestContext& context, const Subject& subject, const String& what, std::optional<CBCheck> expected, const Mutation& mutate)
{
    StateImage image = subject.image;
    mutate(image);
    auto words = encodeState(context, image);
    if (!words)
        return;
    expectOutcome(context, what, bytesOf(*words), subject, expected);
}

// The body of U2's subject, which has an entry in every family a rule below changes. It is sloppy, so `this` gives a
// to_this; Array(n) gives a jneq_ptr and a new_array_with_size; the nested literals give two new_array and three
// new_array_buffer of each copy-on-write type; for-of gives iterator_open and iterator_next; for-in gives the enumerator
// opcodes, and hasOwnProperty inside it a second jneq_ptr.
#define CB_IMPORT_TESTS_SUBJECT_BODY \
    "(array, object, n) {\n" \
    "    var sum = 0;\n" \
    "    for (var i = 0; i < array.length; ++i)\n" \
    "        sum += array[i];\n" \
    "    array[0] = sum;\n" \
    "    if (0 in array)\n" \
    "        sum += 1;\n" \
    "    for (var x of array)\n" \
    "        sum += x;\n" \
    "    for (var key in object) {\n" \
    "        if (key in object)\n" \
    "            sum += object[key];\n" \
    "        if (object.hasOwnProperty(key))\n" \
    "            object[key] = 1;\n" \
    "    }\n" \
    "    var made = Array(n);\n" \
    "    var pair = [sum, n];\n" \
    "    var literals = [[1, 2], [3, 4], [5, 6], [1.5, 2.5], [3.5, 4.5], [5.5, 6.5], [\"a\", \"b\"], [\"c\", \"d\"], [\"e\", \"f\"]];\n" \
    "    helper(pair);\n" \
    "    return helper(this) && made.length + pair.length + literals.length + sum;\n" \
    "}\n"

// U2's sources: the subject and its twin, and an async generator and its twin, whose bodies hold the async_iterator_open
// and async_iterator_next entries no sync body can. The program creates both generators, which leaves both bodies never
// called, and keeps them in globals; one call of startAsyncLoop then runs the first body.
static constexpr auto subjectSource = "function helper(value) { return value; }\n"
    "function subject" CB_IMPORT_TESTS_SUBJECT_BODY
    "function subjectTwin" CB_IMPORT_TESTS_SUBJECT_BODY
    "var array = [1, 2, 3];\n"
    "var object = { a: 1, b: 2 };\n"
    "var asyncSource = [1, 2];\n"
    "async function* asyncLoop() { for await (var x of asyncSource) { } }\n"
    "async function* asyncLoopTwin() { for await (var x of asyncSource) { } }\n"
    "var asyncLoopGenerator = asyncLoop();\n"
    "var asyncLoopTwinGenerator = asyncLoopTwin();\n"
    "function startAsyncLoop() { asyncLoopGenerator.next(); }\n"_s;

#undef CB_IMPORT_TESTS_SUBJECT_BODY

// A small body with an argument, value and array profile, for the tests that need no particular family.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }\n"
    "function elementTwin(array, index) { return array[index] + 1; }\n"
    "var array = [1, 2, 3];\n"_s;

// A body with one iterator_open site, for the realm step.
static constexpr auto iterateSource = "function iterate(iterable) { var sum = 0; for (var x of iterable) sum += x; return sum; }\n"
    "function iterateTwin(iterable) { var sum = 0; for (var x of iterable) sum += x; return sum; }\n"
    "var array = [1, 2, 3];\n"_s;

// V1.
static void expectHeaderRules(TestContext& context, const Subject& subject)
{
    expectOutcome(context, "V1: an empty span"_s, subject.section().first(0), subject, CBCheck::StateHeader);
    expectOutcome(context, "V1: a span one byte short of a header"_s, subject.section().first(sizeof(CBFormat::StateHeader) - 1), subject, CBCheck::StateHeader);
    expectMutation(context, subject, "V1: layout version 0"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.layoutVersion = 0;
    });
    expectMutation(context, subject, "V1: layout version 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.layoutVersion = 2;
    });
    expectMutation(context, subject, "V1: tier 0"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.tier = 0;
    });
    expectMutation(context, subject, "V1: tier 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.tier = 2;
    });
    expectMutation(context, subject, "V1: counter mode 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.counterMode = 2;
    });
    expectMutation(context, subject, "V1: counter mode NotCarried with zero counter fields"_s, std::nullopt, [](StateImage& image) {
        setCounter(image, CBFormat::CounterMode::NotCarried, { });
    });
    expectMutation(context, subject, "V1: reserved1 1"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.reserved1 = 1;
    });
}

// V2 to V5.
static void expectCountAndLengthRules(TestContext& context, const Subject& subject, const Subject& asyncSubject)
{
    expectMutation(context, subject, "V2: one argument more"_s, CBCheck::ArgumentCount, [](StateImage& image) {
        ++image.header.numArguments;
        image.argumentPredictions.append(SpecNone);
    });
    expectMutation(context, subject, "V2: one argument fewer"_s, CBCheck::ArgumentCount, [](StateImage& image) {
        --image.header.numArguments;
        image.argumentPredictions.removeLast();
    });

    if (subject.image.header.numValueProfiles) {
        expectMutation(context, subject, "V3: one value profile more"_s, CBCheck::ValueProfileCount, [](StateImage& image) {
            ++image.header.numValueProfiles;
            image.valuePredictions.append(SpecNone);
        });
        expectMutation(context, subject, "V3: one value profile fewer"_s, CBCheck::ValueProfileCount, [](StateImage& image) {
            --image.header.numValueProfiles;
            image.valuePredictions.removeLast();
        });
    } else
        JITCACHE_FAIL("the subject has no value profile for V3"_s);

    unsigned getByVal = familyOf<CBFormat::FamilyKind::ArrayProfile, OpGetByVal>();
    if (hasEntries(context, subject.image, getByVal, 1, "V4"_s)) {
        expectMutation(context, subject, "V4: one get_by_val entry more"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            appendFamilyRecord(image, image.arrayProfiles, getByVal, CBFormat::ArrayProfileRecord { });
        });
        expectMutation(context, subject, "V4: one get_by_val entry fewer"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            removeLastFamilyRecord(image, image.arrayProfiles, getByVal);
        });
    }
    // Only builtins emit new_array_with_species, so the subject has no entry of that family.
    unsigned species = familyOf<CBFormat::FamilyKind::ArrayProfile, OpNewArrayWithSpecies>();
    if (!subject.image.header.familyEntryCount[species]) {
        expectMutation(context, subject, "V4: an entry in a family the body lacks"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            appendFamilyRecord(image, image.arrayProfiles, species, CBFormat::ArrayProfileRecord { });
        });
    } else
        JITCACHE_FAIL("the subject has a new_array_with_species entry"_s);

    Vector<uint64_t> longer = subject.words;
    longer.append(0);
    expectOutcome(context, "V5: eight bytes past the section"_s, bytesOf(longer), subject, CBCheck::StateLength);
    expectOutcome(context, "V5: eight bytes short of the section"_s, subject.section().first(subject.section().size() - sizeof(uint64_t)), subject, CBCheck::StateLength);

    // The padding rule needs a section whose byte-sized arrays end short of an 8-byte boundary.
    bool checkedPadding = false;
    for (const Subject* candidate : { &subject, &asyncSubject }) {
        auto layout = CBFormat::stateLayout(candidate->image.header);
        if (!layout || layout->paddingOffset == layout->size)
            continue;
        Vector<uint64_t> padded = candidate->words;
        asMutableByteSpan(padded.mutableSpan())[layout->size - 1] = 1;
        expectOutcome(context, "V5: a nonzero padding byte"_s, bytesOf(padded), *candidate, CBCheck::StateLength);
        checkedPadding = true;
        break;
    }
    if (!checkedPadding)
        JITCACHE_FAIL("neither subject's section has a padding byte for V5"_s);
}

// V6 and V7.
static void expectPredictionAndArrayProfileRules(TestContext& context, const Subject& subject)
{
    expectMutation(context, subject, "V6: argument 0 at SpecBytecodeTop"_s, std::nullopt, [](StateImage& image) {
        image.argumentPredictions[0] = SpecBytecodeTop;
    });
    expectMutation(context, subject, "V6: argument 0 past SpecBytecodeTop"_s, CBCheck::ValuePrediction, [](StateImage& image) {
        image.argumentPredictions[0] = SpecBytecodeTop | predictionOutsideBytecodeTop;
    });
    if (!subject.image.valuePredictions.isEmpty()) {
        expectMutation(context, subject, "V6: value profile 1 at SpecBytecodeTop"_s, std::nullopt, [](StateImage& image) {
            image.valuePredictions[0] = SpecBytecodeTop;
        });
        expectMutation(context, subject, "V6: value profile 1 past SpecBytecodeTop"_s, CBCheck::ValuePrediction, [](StateImage& image) {
            image.valuePredictions[0] = SpecBytecodeTop | predictionOutsideBytecodeTop;
        });
    }

    if (subject.image.arrayProfiles.isEmpty()) {
        JITCACHE_FAIL("the subject has no array profile for V7"_s);
        return;
    }
    expectMutation(context, subject, "V7: every array mode"_s, std::nullopt, [](StateImage& image) {
        image.arrayProfiles[0].observedArrayModes = ALL_ARRAY_MODES;
    });
    expectMutation(context, subject, "V7: an array mode past ALL_ARRAY_MODES"_s, CBCheck::ArrayProfile, [](StateImage& image) {
        image.arrayProfiles[0].observedArrayModes = ALL_ARRAY_MODES | arrayModeOutsideAllModes;
    });
    expectMutation(context, subject, "V7: all eight flags"_s, std::nullopt, [](StateImage& image) {
        image.arrayProfiles[0].flags = 0xff;
    });
    expectMutation(context, subject, "V7: a ninth flag"_s, CBCheck::ArrayProfile, [](StateImage& image) {
        image.arrayProfiles[0].flags = 0x100;
    });
}

// V8, on three literals of each copy-on-write type.
static void expectAllocationHintRules(TestContext& context, const Subject& subject)
{
    unsigned newArray = familyOf<CBFormat::FamilyKind::AllocationHint, OpNewArray>();
    if (hasEntries(context, subject.image, newArray, 1, "V8"_s)) {
        auto expectNewArrayHint = [&](ASCIILiteral what, IndexingType indexingType, unsigned vectorLength, std::optional<CBCheck> expected) {
            expectMutation(context, subject, makeString("V8: new_array hint "_s, what), expected, [&](StateImage& image) {
                image.allocationHints[recordIndex(image, newArray, 0)] = CBFormat::encodeAllocationHint(indexingType, vectorLength);
            });
        };
        expectNewArrayHint("ArrayWithUndecided"_s, ArrayWithUndecided, 0, std::nullopt);
        expectNewArrayHint("ArrayWithInt32"_s, ArrayWithInt32, 0, std::nullopt);
        expectNewArrayHint("ArrayWithDouble"_s, ArrayWithDouble, 0, std::nullopt);
        expectNewArrayHint("ArrayWithContiguous"_s, ArrayWithContiguous, 0, std::nullopt);
        expectNewArrayHint("ArrayWithArrayStorage"_s, ArrayWithArrayStorage, 0, std::nullopt);
        expectNewArrayHint("ArrayWithSlowPutArrayStorage"_s, ArrayWithSlowPutArrayStorage, 0, std::nullopt);
        expectNewArrayHint("CopyOnWriteArrayWithInt32"_s, CopyOnWriteArrayWithInt32, 0, CBCheck::AllocationHint);
        expectNewArrayHint("at the largest vector length"_s, ArrayWithContiguous, BASE_CONTIGUOUS_VECTOR_LEN_MAX, std::nullopt);
        expectNewArrayHint("one past the largest vector length"_s, ArrayWithContiguous, BASE_CONTIGUOUS_VECTOR_LEN_MAX + 1, CBCheck::AllocationHint);
    }

    // The F19 entry each literal names, by the literal's type (N8).
    std::array<Vector<unsigned>, 3> literals;
    CBFormat::forEachNewArrayBufferLiteral(*subject.newborn, [&](unsigned metadataID, IndexingType recommendedIndexingType) {
        if (recommendedIndexingType == CopyOnWriteArrayWithInt32)
            literals[0].append(metadataID);
        else if (recommendedIndexingType == CopyOnWriteArrayWithDouble)
            literals[1].append(metadataID);
        else if (recommendedIndexingType == CopyOnWriteArrayWithContiguous)
            literals[2].append(metadataID);
    });
    if (literals[0].size() != 3 || literals[1].size() != 3 || literals[2].size() != 3 || !hasEntries(context, subject.image, CBFormat::newArrayBufferFamily, 9, "V8"_s)) {
        JITCACHE_FAIL(makeString("the subject's literals are "_s, literals[0].size(), " Int32, "_s, literals[1].size(), " Double and "_s, literals[2].size(), " Contiguous instead of three of each"_s));
        return;
    }
    auto expectLiteralHint = [&](ASCIILiteral what, unsigned metadataID, IndexingType indexingType, std::optional<CBCheck> expected) {
        expectMutation(context, subject, makeString("V8: "_s, what, " (entry "_s, metadataID, ')'), expected, [&](StateImage& image) {
            uint16_t& hint = image.allocationHints[recordIndex(image, CBFormat::newArrayBufferFamily, metadataID)];
            hint = CBFormat::encodeAllocationHint(indexingType, CBFormat::allocationHintVectorLength(hint));
        });
    };
    for (unsigned metadataID : literals[0]) {
        expectLiteralHint("CopyOnWriteArrayWithDouble at an Int32 literal"_s, metadataID, CopyOnWriteArrayWithDouble, std::nullopt);
        expectLiteralHint("CopyOnWriteArrayWithContiguous at an Int32 literal"_s, metadataID, CopyOnWriteArrayWithContiguous, std::nullopt);
        expectLiteralHint("ArrayWithContiguous in F19"_s, metadataID, ArrayWithContiguous, CBCheck::AllocationHint);
    }
    for (unsigned metadataID : literals[1]) {
        expectLiteralHint("CopyOnWriteArrayWithInt32 at a Double literal"_s, metadataID, CopyOnWriteArrayWithInt32, CBCheck::AllocationHint);
        expectLiteralHint("CopyOnWriteArrayWithContiguous at a Double literal"_s, metadataID, CopyOnWriteArrayWithContiguous, std::nullopt);
    }
    for (unsigned metadataID : literals[2]) {
        expectLiteralHint("CopyOnWriteArrayWithInt32 at a Contiguous literal"_s, metadataID, CopyOnWriteArrayWithInt32, CBCheck::AllocationHint);
        expectLiteralHint("CopyOnWriteArrayWithDouble at a Contiguous literal"_s, metadataID, CopyOnWriteArrayWithDouble, CBCheck::AllocationHint);
    }
}

// V9: each opcode's native bits, whose masks the SPEC states in hexadecimal.
static void expectIterationModeRules(TestContext& context, const Subject& subject, const Subject& asyncSubject)
{
    auto expectModes = [&](const Subject& owner, unsigned family, ASCIILiteral what, uint16_t modes, std::optional<CBCheck> expected) {
        expectMutation(context, owner, makeString("V9: "_s, what), expected, [&](StateImage& image) {
            image.iterationModes[recordIndex(image, family, 0)] = modes;
        });
    };

    if (hasEntries(context, subject.image, CBFormat::iteratorOpenIterationModesFamily, 1, "V9"_s)) {
        unsigned family = CBFormat::iteratorOpenIterationModesFamily;
        expectModes(subject, family, "iterator_open's full mask"_s, 0x1fff, std::nullopt);
        expectModes(subject, family, "FastAsyncGenerator at iterator_open"_s, iterationModeBit(IterationMode::FastAsyncGenerator), CBCheck::IterationModes);
        expectModes(subject, family, "AsyncFromSync at iterator_open"_s, iterationModeBit(IterationMode::AsyncFromSync), CBCheck::IterationModes);
    }
    if (hasEntries(context, subject.image, CBFormat::iteratorNextIterationModesFamily, 1, "V9"_s)) {
        unsigned family = CBFormat::iteratorNextIterationModesFamily;
        expectModes(subject, family, "iterator_next's full mask"_s, 0x1ff1, std::nullopt);
        expectModes(subject, family, "FastArray at iterator_next"_s, iterationModeBit(IterationMode::FastArray), CBCheck::IterationModes);
        expectModes(subject, family, "FastMap at iterator_next"_s, iterationModeBit(IterationMode::FastMap), CBCheck::IterationModes);
        expectModes(subject, family, "FastSet at iterator_next"_s, iterationModeBit(IterationMode::FastSet), CBCheck::IterationModes);
    }
    if (hasEntries(context, asyncSubject.image, CBFormat::asyncIteratorOpenIterationModesFamily, 1, "V9"_s)) {
        unsigned family = CBFormat::asyncIteratorOpenIterationModesFamily;
        expectModes(asyncSubject, family, "async_iterator_open's full mask"_s, 0x6001, std::nullopt);
        expectModes(asyncSubject, family, "FastArray at async_iterator_open"_s, iterationModeBit(IterationMode::FastArray), CBCheck::IterationModes);
    }
    if (hasEntries(context, asyncSubject.image, CBFormat::asyncIteratorNextIterationModesFamily, 1, "V9"_s)) {
        unsigned family = CBFormat::asyncIteratorNextIterationModesFamily;
        expectModes(asyncSubject, family, "async_iterator_next's full mask"_s, 0x2001, std::nullopt);
        expectModes(asyncSubject, family, "AsyncFromSync at async_iterator_next"_s, iterationModeBit(IterationMode::AsyncFromSync), CBCheck::IterationModes);
    }
}

// V10 to V12.
static void expectByteRules(TestContext& context, const Subject& subject)
{
    unsigned enumeratorNext = familyOf<CBFormat::FamilyKind::EnumeratorModes, OpEnumeratorNext>();
    if (hasEntries(context, subject.image, enumeratorNext, 1, "V10"_s)) {
        expectMutation(context, subject, "V10: every enumerator flag"_s, std::nullopt, [&](StateImage& image) {
            image.enumeratorModes[recordIndex(image, enumeratorNext, 0)] = static_cast<uint8_t>(JSPropertyNameEnumerator::IndexedMode | JSPropertyNameEnumerator::OwnStructureMode
                | JSPropertyNameEnumerator::GenericMode | JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch);
        });
        expectMutation(context, subject, "V10: a fifth enumerator flag"_s, CBCheck::EnumeratorModes, [&](StateImage& image) {
            image.enumeratorModes[recordIndex(image, enumeratorNext, 0)] = 0x10;
        });
    }
    if (hasEntries(context, subject.image, CBFormat::toThisStatusFamily, 1, "V11"_s)) {
        expectMutation(context, subject, "V11: ToThisClearedByGC"_s, std::nullopt, [](StateImage& image) {
            image.toThisStatuses[recordIndex(image, CBFormat::toThisStatusFamily, 0)] = static_cast<uint8_t>(ToThisClearedByGC);
        });
        expectMutation(context, subject, "V11: a status past ToThisClearedByGC"_s, CBCheck::ToThisStatus, [](StateImage& image) {
            image.toThisStatuses[recordIndex(image, CBFormat::toThisStatusFamily, 0)] = static_cast<uint8_t>(ToThisClearedByGC + 1);
        });
    }
    if (hasEntries(context, subject.image, CBFormat::branchBitFamily, 1, "V12"_s)) {
        expectMutation(context, subject, "V12: a branch bit of 1"_s, std::nullopt, [](StateImage& image) {
            image.branchBits[recordIndex(image, CBFormat::branchBitFamily, 0)] = 1;
        });
        expectMutation(context, subject, "V12: a branch bit of 2"_s, CBCheck::BranchBit, [](StateImage& image) {
            image.branchBits[recordIndex(image, CBFormat::branchBitFamily, 0)] = 2;
        });
    }
}

// V13: the mutations U2 lists, each in a section whose only lazy-operand records are the ones named.
static void expectLazyOperandRules(TestContext& context, const Subject& subject)
{
    CodeBlock& newborn = *subject.newborn;
    uint32_t firstOffset = BytecodeIndex(0).asBits();
    int32_t firstArgument = virtualRegisterForArgumentIncludingThis(1).offset();
    int32_t firstLocal = virtualRegisterForLocal(0).offset();
    auto expectRecords = [&](ASCIILiteral what, std::optional<CBCheck> expected, std::initializer_list<CBFormat::LazyOperandRecord> records) {
        Vector<CBFormat::LazyOperandRecord> list(records);
        expectMutation(context, subject, makeString("V13: "_s, what), expected, [&](StateImage& image) {
            setLazyOperands(image, list);
        });
    };

    CBFormat::LazyOperandRecord valid = lazyOperandRecord(firstOffset, argumentKind, firstArgument);
    CBFormat::LazyOperandRecord withReserved = valid;
    withReserved.reserved = 1;

    expectRecords("an argument key"_s, std::nullopt, { valid });
    expectRecords("a tmp at 0"_s, std::nullopt, { lazyOperandRecord(firstOffset, tmpKind, 0) });
    expectRecords("a tmp at -1"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, tmpKind, -1) });
    expectRecords("an Argument kind with a negative value"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, argumentKind, firstLocal) });
    expectRecords("a Local kind with a non-negative value"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, localKind, firstArgument) });
    expectRecords("an Argument at invalidVirtualRegister"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, argumentKind, VirtualRegister::invalidVirtualRegister) });
    expectRecords("an Argument at FirstConstantRegisterIndex"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, argumentKind, FirstConstantRegisterIndex) });
    expectRecords("an operand kind of 3"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, 3, firstArgument) });
    expectRecords("the last bytecode offset"_s, std::nullopt, { lazyOperandRecord(BytecodeIndex(newborn.instructionsSize() - 1).asBits(), argumentKind, firstArgument) });
    expectRecords("the offset past the instructions"_s, CBCheck::LazyOperand, { lazyOperandRecord(BytecodeIndex(newborn.instructionsSize()).asBits(), argumentKind, firstArgument) });
    expectRecords("the hash table's empty index"_s, CBCheck::LazyOperand, { lazyOperandRecord(std::numeric_limits<uint32_t>::max(), argumentKind, firstArgument) });
    expectRecords("the hash table's deleted index"_s, CBCheck::LazyOperand, { lazyOperandRecord(std::numeric_limits<uint32_t>::max() - 1, argumentKind, firstArgument) });
    expectRecords("checkpoint 3 at a valid offset"_s, std::nullopt, { lazyOperandRecord(BytecodeIndex(0, 3).asBits(), argumentKind, firstArgument) });
    expectRecords("a nonzero reserved field"_s, CBCheck::LazyOperand, { withReserved });
    expectRecords("a prediction past SpecBytecodeTop"_s, CBCheck::LazyOperand, { lazyOperandRecord(firstOffset, argumentKind, firstArgument, SpecBytecodeTop | predictionOutsideBytecodeTop) });
    expectRecords("a repeated key"_s, std::nullopt, { valid, valid });
    expectRecords("two keys in decreasing order"_s, std::nullopt, { lazyOperandRecord(BytecodeIndex(1).asBits(), argumentKind, firstArgument), valid });
    expectRecords("a local past numCalleeLocals"_s, std::nullopt, { lazyOperandRecord(firstOffset, localKind, virtualRegisterForLocal(newborn.numCalleeLocals() + 4).offset()) });
    expectRecords("a tmp at maxNumCheckpointTmps"_s, std::nullopt, { lazyOperandRecord(firstOffset, tmpKind, maxNumCheckpointTmps) });
    expectRecords("a tmp far past maxNumCheckpointTmps"_s, std::nullopt, { lazyOperandRecord(firstOffset, tmpKind, maxNumCheckpointTmps + 4096) });
}

// V14 and V15. The accepted counters keep a progress that is not negative, so S3 holds.
static void expectTierUpAndCounterRules(TestContext& context, const Subject& subject)
{
    uint16_t maximumDelay = static_cast<uint16_t>(Options::maximumOptimizationDelay());
    uint16_t maximumReoptimizations = static_cast<uint16_t>(Options::reoptimizationRetryCounterMax());
    expectMutation(context, subject, "V14: the most profile deferrals"_s, std::nullopt, [&](StateImage& image) {
        image.header.optimizationDelayCounter = maximumDelay;
    });
    expectMutation(context, subject, "V14: one profile deferral past the most"_s, CBCheck::TierUpHistory, [&](StateImage& image) {
        image.header.optimizationDelayCounter = maximumDelay + 1;
    });
    expectMutation(context, subject, "V14: the most reoptimizations"_s, std::nullopt, [&](StateImage& image) {
        image.header.reoptimizationRetryCounter = maximumReoptimizations;
    });
    expectMutation(context, subject, "V14: one reoptimization past the most"_s, CBCheck::TierUpHistory, [&](StateImage& image) {
        image.header.reoptimizationRetryCounter = maximumReoptimizations + 1;
    });

    auto expectCounter = [&](ASCIILiteral what, CBFormat::CounterMode mode, CounterTriple triple, std::optional<CBCheck> expected) {
        expectMutation(context, subject, makeString("V15: "_s, what), expected, [&](StateImage& image) {
            setCounter(image, mode, triple);
        });
    };
    using CBFormat::CounterMode;
    expectCounter("a carried threshold of 0"_s, CounterMode::Carried, { 0, 0, 0 }, std::nullopt);
    expectCounter("a carried threshold of -1"_s, CounterMode::Carried, { 0, 0, -1 }, CBCheck::Counter);
    expectCounter("a carried total of 0"_s, CounterMode::Carried, { 0, 0, 1000 }, std::nullopt);
    expectCounter("a carried total of -1"_s, CounterMode::Carried, { 0, -1, 1000 }, CBCheck::Counter);
    expectCounter("a carried total that is NaN"_s, CounterMode::Carried, { 0, std::numeric_limits<float>::quiet_NaN(), 1000 }, CBCheck::Counter);
    expectCounter("a carried total that is infinite"_s, CounterMode::Carried, { 0, std::numeric_limits<float>::infinity(), 1000 }, CBCheck::Counter);
    expectCounter("NotCarried with a nonzero counter"_s, CounterMode::NotCarried, { -1, 0, 0 }, CBCheck::Counter);
    expectCounter("NotCarried with a nonzero total"_s, CounterMode::NotCarried, { 0, 1, 0 }, CBCheck::Counter);
    expectCounter("NotCarried with a nonzero threshold"_s, CounterMode::NotCarried, { 0, 0, 1 }, CBCheck::Counter);
}

// Takes a newborn CB of the function through the install sequence of section 5.1 with the section, inside one deferral:
// prepare, seedLinkedState, beforeSetup(codeBlock), native setup with the producer's code, and finishCounter; then calls
// afterFinish(codeBlock, the counter setup armed, what finishCounter returned).
template<typename BeforeSetup, typename AfterFinish>
static void restoreOnNewborn(TestContext& context, JSFunction* function, std::span<const uint8_t> section, BaselineJITCode& code, const BeforeSetup& beforeSetup, const AfterFinish& afterFinish)
{
    DeferGCForAWhile deferGC(function->vm());
    CodeBlock* newborn = createNewbornCodeBlock(context, function);
    if (!newborn)
        return;
    auto prepared = CBStateImport::prepare(section, *newborn, true);
    if (!prepared) {
        failWith(context, "prepare"_s, prepared.error());
        return;
    }
    prepared->seedLinkedState(*newborn);
    beforeSetup(*newborn);
    newborn->setupWithUnlinkedBaselineCode(Ref { code });
    CounterTriple armed = liveCounter(*newborn);
    CBCounterRestore restore = prepared->finishCounter(*newborn);
    afterFinish(*newborn, armed, restore);
}

static String describeRestore(const CounterTriple& counter, const CBCounterRestore& restore)
{
    return makeString(" (counter "_s, counter.value, ", total "_s, counter.totalCount, ", threshold "_s, counter.activeThreshold, "; carried "_s, static_cast<unsigned>(restore.carried),
        ", crossed "_s, static_cast<unsigned>(restore.crossed), ", nativeSlice "_s, restore.nativeSlice, ", slice "_s, restore.slice, ')');
}

// I7 for a carried record, and, for a finite threshold, the native reference: a stack counter given the same triple and
// checked on the same CB right after finishCounter, which no pool change can separate from finishCounter's own check in
// a VM with useConcurrentJIT off that is alone in its process (N4).
static void checkCarriedRestore(TestContext& context, ASCIILiteral what, CodeBlock& codeBlock, const CounterTriple& record, const CBCounterRestore& restore)
{
    CounterTriple live = liveCounter(codeBlock);
    auto fail = [&](ASCIILiteral rule) {
        JITCACHE_FAIL(makeString(what, ": "_s, rule, describeRestore(live, restore)));
    };
    double count = codeBlock.baselineJITData()->executeCounter().count();
    double progress = static_cast<double>(record.totalCount) + record.value;
    int64_t sliceFloor = 2 * static_cast<int64_t>(Options::executionCounterIncrementForEntry());

    if (!restore.carried)
        fail("the restore does not say the counter was carried"_s);
    if (live.activeThreshold != record.activeThreshold)
        fail("the active threshold is not the captured one"_s);
    if (restore.slice != std::max(restore.nativeSlice, sliceFloor))
        fail("the slice is not the larger of the native slice and the floor"_s);
    if (static_cast<int64_t>(live.value) != -restore.slice || live.value > -sliceFloor)
        fail("the counter does not hold the slice at least the floor short of crossing"_s);
    if (count != static_cast<double>(static_cast<float>(progress + restore.slice)) - restore.slice)
        fail("count() is not the captured progress as the float total keeps it"_s);
    if (codeBlock.previousCounterForAging() != static_cast<float>(count))
        fail("the aging sample is not the restored count"_s);

    if (record.activeThreshold == std::numeric_limits<int32_t>::max()) {
        int64_t capturedSlice = record.value < 0 ? -static_cast<int64_t>(record.value) : 0;
        if (restore.crossed || restore.nativeSlice != capturedSlice)
            fail("an infinite threshold does not keep the captured slice"_s);
        return;
    }

    // The envelope M >= 1 leaves to any pool.
    int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
    bool sliceInEnvelope = restore.crossed ? !restore.nativeSlice : restore.nativeSlice >= 0 && restore.nativeSlice <= ceiling;
    if (!sliceInEnvelope)
        fail("the native slice lies outside [0, C], or is not 0 after a crossing"_s);
    double threshold = record.activeThreshold;
    if (progress < threshold - std::min(threshold, static_cast<double>(ceiling)) / 2) {
        if (restore.crossed || restore.nativeSlice < static_cast<int64_t>(std::trunc(std::min(threshold - progress, static_cast<double>(ceiling)))))
            fail("a progress short of the threshold crossed or armed less than T - P"_s);
    }

    BaselineExecutionCounter reference;
    reference.m_activeThreshold = record.activeThreshold;
    reference.m_totalCount = record.totalCount;
    reference.m_counter = record.value;
    bool crossed = reference.checkIfThresholdCrossedAndSet(&codeBlock);
    int64_t nativeSlice = crossed ? 0 : -static_cast<int64_t>(reference.m_counter);
    if (restore.crossed != crossed || restore.nativeSlice != nativeSlice)
        fail("the restore differs from the native check of the same triple"_s);
}

// I7 for a counter that does not travel: finishCounter left setup's arming, which follows the seeded reoptimization
// count and the UCB's quick DFG bit (N4), and setup's aging sample of the fresh counter.
static void checkSetupArming(TestContext& context, const String& what, CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore, bool quickDFGTierUp)
{
    CounterTriple live = liveCounter(codeBlock);
    auto fail = [&](ASCIILiteral rule) {
        JITCACHE_FAIL(makeString(what, ": "_s, rule, describeRestore(live, restore)));
    };
    if (restore.carried || restore.crossed)
        fail("the restore says a counter that did not travel was carried or crossed"_s);
    if (live != armed)
        fail("finishCounter changed the counter setup armed"_s);
    if (restore.nativeSlice != -static_cast<int64_t>(live.value) || restore.slice != restore.nativeSlice)
        fail("the restore does not report setup's slice"_s);

    int32_t warmUp = Options::thresholdForOptimizeAfterWarmUp();
    if (quickDFGTierUp)
        warmUp = static_cast<int32_t>(warmUp * Options::quickDFGTierUpThresholdFactor());
    int32_t threshold = codeBlock.adjustedCounterValue(warmUp);
    if (live.activeThreshold != threshold)
        fail("the active threshold is not optimizeAfterWarmUp's for this history"_s);
    if (threshold == std::numeric_limits<int32_t>::max()) {
        if (live.value != std::numeric_limits<int32_t>::min() || live.totalCount)
            fail("an infinite arming is not the indefinite deferral"_s);
    } else {
        double count = codeBlock.baselineJITData()->executeCounter().count();
        if (count < 0 || count > 1)
            fail("a fresh arming holds more than a point of progress"_s);
    }
    if (codeBlock.previousCounterForAging())
        fail("the aging sample is not setup's sample of the fresh counter"_s);
}

struct ElementRealm {
    JSGlobalObject* globalObject { nullptr };
    JSFunction* twin { nullptr };
    CodeBlock* producer { nullptr };
};

// elementSource with element brought to baseline.
static std::optional<ElementRealm> createElementRealm(TestContext& context, VM& vm)
{
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return std::nullopt;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    JSFunction* twin = globalFunction(context, globalObject, "elementTwin"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    if (!element || !twin || !array)
        return std::nullopt;
    CodeBlock* producer = bringToBaseline(context, globalObject, element, { *array, jsNumber(0) });
    if (!producer)
        return std::nullopt;
    return ElementRealm { globalObject, twin, producer };
}

} // namespace CBImportTestsInternal

using namespace CBImportTestsInternal;

// U2: from strict captures of two real CBs, every field set just inside and just outside its V-rule gives exactly the
// expected outcome from both validateState and a strict prepare on a newborn twin, never a crash, and leaves that CB at
// its link state (I5). The async generator subject supplies the async_iterator families.
JITCACHE_TEST(cbImportValidationGivesExactlyTheExpectedCheck, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, subjectSource);
    if (!globalObject)
        return;
    JSFunction* subjectFunction = globalFunction(context, globalObject, "subject"_s);
    JSFunction* subjectTwin = globalFunction(context, globalObject, "subjectTwin"_s);
    JSFunction* startAsyncLoop = globalFunction(context, globalObject, "startAsyncLoop"_s);
    JSFunction* asyncBody = asyncGeneratorBody(context, globalObject, "asyncLoopGenerator"_s);
    JSFunction* asyncBodyTwin = asyncGeneratorBody(context, globalObject, "asyncLoopTwinGenerator"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    auto object = globalObjectValue(context, globalObject, "object"_s);
    if (!subjectFunction || !subjectTwin || !startAsyncLoop || !asyncBody || !asyncBodyTwin || !array || !object)
        return;

    CodeBlock* producer = bringToBaseline(context, globalObject, subjectFunction, { *array, *object, jsNumber(3) });
    CodeBlock* asyncProducer = bringAsyncGeneratorBodyToBaseline(context, globalObject, startAsyncLoop, asyncBody);
    if (!producer || !asyncProducer)
        return;
    auto image = captureState(context, *producer);
    auto asyncImage = captureState(context, *asyncProducer);
    if (!image || !asyncImage)
        return;

    DeferGCForAWhile deferGC(vm);
    auto subject = makeSubject(context, createNewbornCodeBlock(context, subjectTwin), WTF::move(image));
    auto asyncSubject = makeSubject(context, createNewbornCodeBlock(context, asyncBodyTwin), WTF::move(asyncImage));
    if (!subject || !asyncSubject)
        return;

    expectOutcome(context, "the captured section"_s, subject->section(), *subject, std::nullopt);
    expectOutcome(context, "the captured async section"_s, asyncSubject->section(), *asyncSubject, std::nullopt);

    expectHeaderRules(context, *subject);
    expectCountAndLengthRules(context, *subject, *asyncSubject);
    expectPredictionAndArrayProfileRules(context, *subject);
    expectAllocationHintRules(context, *subject);
    expectIterationModeRules(context, *subject, *asyncSubject);
    expectByteRules(context, *subject);
    expectLazyOperandRules(context, *subject);
    expectTierUpAndCounterRules(context, *subject);
}

// U3: a CB that already ran fails S1, and a newborn CB that seedLinkedState already wrote fails S2, so seeding, which the
// glue calls only after prepare succeeds for that CB (R-INT-4), never reaches either (I6); S3 rejects negative progress
// under a finite threshold and accepts it under INT32_MAX; and lazy-operand keys outside the CB's own frame, as an
// inlined CB holds (N7), pass prepare.
JITCACHE_TEST(cbImportStrictRejectsAStaleCodeBlockOrCounter, Yes)
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    auto image = captureState(context, *realm->producer);
    if (!image)
        return;
    auto words = encodeState(context, *image);
    if (!words)
        return;
    std::span<const uint8_t> section = bytesOf(*words);

    auto expectPrepareFails = [&](ASCIILiteral what, std::span<const uint8_t> bytes, CodeBlock& codeBlock, CBCheck expected) {
        auto prepared = CBStateImport::prepare(bytes, codeBlock, true);
        if (prepared) {
            JITCACHE_FAIL(makeString(what, ": prepare accepted, expected "_s, description(expected)));
            return;
        }
        if (prepared.error().kind != CBFaultKind::InvalidMaterial || prepared.error().check != expected)
            JITCACHE_FAIL(makeString(what, ": prepare gave "_s, description(prepared.error().check), ", expected "_s, description(expected)));
    };

    // S1: the section's counts match the producer's own CB, which already ran.
    JITCACHE_CHECK(!validateState(section, *realm->producer));
    expectPrepareFails("prepare on the producer's baseline CB"_s, section, *realm->producer, CBCheck::StrictNewborn);

    DeferGCForAWhile deferGC(vm);

    // S2: a newborn CB seeded once is no longer at its link state.
    if (CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin)) {
        StateImage seeded = *image;
        seeded.argumentPredictions[0] = SpecInt32Only;
        auto seededWords = encodeState(context, seeded);
        if (seededWords) {
            auto prepared = CBStateImport::prepare(bytesOf(*seededWords), *newborn, true);
            if (prepared) {
                prepared->seedLinkedState(*newborn);
                expectPrepareFails("prepare on a newborn CB already seeded"_s, section, *newborn, CBCheck::StrictLinkState);
            } else
                failWith(context, "prepare of the section to seed"_s, prepared.error());
        }
    }

    // S3.
    if (CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin)) {
        StateImage negative = *image;
        setCounter(negative, CBFormat::CounterMode::Carried, { -10, 5, 1000 });
        auto negativeWords = encodeState(context, negative);
        StateImage infinite = negative;
        infinite.header.counterActiveThreshold = std::numeric_limits<int32_t>::max();
        auto infiniteWords = encodeState(context, infinite);
        if (negativeWords && infiniteWords) {
            JITCACHE_CHECK(!validateState(bytesOf(*negativeWords), *newborn));
            JITCACHE_CHECK(!counterObeysNativeInvariant(negative.header));
            expectPrepareFails("negative progress under a finite threshold"_s, bytesOf(*negativeWords), *newborn, CBCheck::StrictCounter);
            JITCACHE_CHECK(counterObeysNativeInvariant(infinite.header));
            auto prepared = CBStateImport::prepare(bytesOf(*infiniteWords), *newborn, true);
            if (!prepared)
                failWith(context, "prepare of negative progress under INT32_MAX"_s, prepared.error());
        }
    }

    // N7: keys that name slots of an inlining compile's machine frame.
    if (CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin)) {
        StateImage inlined = *image;
        uint32_t firstOffset = BytecodeIndex(0).asBits();
        setLazyOperands(inlined, {
            lazyOperandRecord(firstOffset, localKind, virtualRegisterForLocal(newborn->numCalleeLocals() + 8).offset()),
            lazyOperandRecord(firstOffset, tmpKind, maxNumCheckpointTmps + 7),
            lazyOperandRecord(firstOffset, argumentKind, virtualRegisterForArgumentIncludingThis(newborn->numParameters() + 3).offset()),
        });
        auto inlinedWords = encodeState(context, inlined);
        if (inlinedWords) {
            auto prepared = CBStateImport::prepare(bytesOf(*inlinedWords), *newborn, true);
            if (!prepared)
                failWith(context, "prepare of lazy-operand keys outside the CB's frame"_s, prepared.error());
        }
    }
}

// U4: finishCounter on a CB after setup satisfies I7 for every record, and for a finite threshold equals the native
// reference; two splits of the same P under the same finite T restore identical counters (I1); and a counter that did
// not travel stays as setup armed it, for reoptimization counts of 0 and 3 with the quick DFG bit set and clear.
JITCACHE_TEST_WITH_OPTIONS(cbImportFinishCounterRestoresTheCapturedCounter, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    auto image = captureState(context, *realm->producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);
    auto nothingBeforeSetup = [](CodeBlock&) { };

    struct CarriedCase {
        ASCIILiteral name;
        CounterTriple record;
    };
    constexpr int32_t infinite = std::numeric_limits<int32_t>::max();
    const CarriedCase carriedCases[] = {
        { "T = 0"_s, { 0, 0, 0 } },
        { "a finite T below the progress"_s, { -100, 600, 100 } },
        { "a finite T above the progress"_s, { -990, 1000, 1000 } },
        { "T = INT32_MAX with a zero total"_s, { -1000, 0, infinite } },
        { "T = INT32_MAX with a positive total"_s, { -1000, 1000, infinite } },
        { "the indefinite deferral"_s, { std::numeric_limits<int32_t>::min(), 0, infinite } },
        { "a fractional P"_s, { -300, 500.5f, 1000 } },
        { "a finite counter already past the floor"_s, { -5, 700, 1000 } },
        { "an infinite counter already past the floor"_s, { -5, 1005, infinite } },
        { "an infinite counter already crossed"_s, { 10, 990, infinite } },
    };
    for (const auto& carriedCase : carriedCases) {
        StateImage carried = *image;
        setCounter(carried, CBFormat::CounterMode::Carried, carriedCase.record);
        auto words = encodeState(context, carried);
        if (!words)
            return;
        restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
            checkCarriedRestore(context, carriedCase.name, codeBlock, carriedCase.record, restore);
        });
    }

    // The producer's split between m_counter and m_totalCount stays local.
    std::optional<CounterTriple> firstCounter;
    std::optional<CBCounterRestore> firstRestore;
    for (CounterTriple split : { CounterTriple { -500, 1000.5f, 2000 }, CounterTriple { -1000, 1500.5f, 2000 } }) {
        StateImage carried = *image;
        setCounter(carried, CBFormat::CounterMode::Carried, split);
        auto words = encodeState(context, carried);
        if (!words)
            return;
        restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
            checkCarriedRestore(context, "a split of P = 500.5"_s, codeBlock, split, restore);
            CounterTriple live = liveCounter(codeBlock);
            if (!firstCounter) {
                firstCounter = live;
                firstRestore = restore;
                return;
            }
            JITCACHE_CHECK(live == *firstCounter);
            JITCACHE_CHECK(restore.crossed == firstRestore->crossed);
            JITCACHE_CHECK(restore.nativeSlice == firstRestore->nativeSlice);
            JITCACHE_CHECK(restore.slice == firstRestore->slice);
        });
    }
    JITCACHE_CHECK(firstCounter && firstRestore);

    for (uint16_t reoptimizations : { 0, 3 }) {
        for (bool quickDFGTierUp : { false, true }) {
            StateImage notCarried = *image;
            setCounter(notCarried, CBFormat::CounterMode::NotCarried, { });
            notCarried.header.reoptimizationRetryCounter = reoptimizations;
            auto words = encodeState(context, notCarried);
            if (!words)
                return;
            String what = makeString("NotCarried with "_s, reoptimizations, " reoptimizations and the quick DFG bit "_s, quickDFGTierUp ? "set"_s : "clear"_s);
            restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), [&](CodeBlock& codeBlock) {
                codeBlock.unlinkedCodeBlock()->setQuickDFGTierUp(quickDFGTierUp ? TriState::True : TriState::False);
            }, [&](CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore) {
                JITCACHE_CHECK(codeBlock.reoptimizationRetryCounter() == reoptimizations);
                checkSetupArming(context, what, codeBlock, armed, restore, quickDFGTierUp);
            });
        }
    }
}

// U7: in a fresh realm that has created no Map and no Set, seedLinkedState with an iterator_open record holding FastMap
// (FastSet), inside a DeferGC as the install function's deferral surrounds it, leaves the realm's lazy entries (values)
// function materialized, and a later new Map (new Set) installs that same function; with neither bit, both stay null
// (I14).
JITCACHE_TEST(cbImportRealmStepMaterializesTheIteratorFunction, Yes)
{
    VM& vm = *context.vm();
    enum class Seeded : uint8_t { FastMap, FastSet, Neither };
    for (Seeded seeded : { Seeded::FastMap, Seeded::FastSet, Seeded::Neither }) {
        JSGlobalObject* globalObject = createRealm(context, vm, iterateSource);
        if (!globalObject)
            return;
        JSFunction* iterate = globalFunction(context, globalObject, "iterate"_s);
        JSFunction* twin = globalFunction(context, globalObject, "iterateTwin"_s);
        auto array = globalObjectValue(context, globalObject, "array"_s);
        if (!iterate || !twin || !array)
            return;
        CodeBlock* producer = bringToBaseline(context, globalObject, iterate, { *array });
        if (!producer)
            return;
        auto image = captureState(context, *producer);
        if (!image || !hasEntries(context, *image, CBFormat::iteratorOpenIterationModesFamily, 1, "the realm step"_s))
            return;
        if (globalObject->mapProtoEntriesFunctionConcurrently() || globalObject->setProtoValuesFunctionConcurrently()) {
            JITCACHE_FAIL("the fresh realm created a Map or a Set before the seed"_s);
            return;
        }

        uint16_t mapAndSet = iterationModeBit(IterationMode::FastMap) | iterationModeBit(IterationMode::FastSet);
        uint16_t& modes = image->iterationModes[recordIndex(*image, CBFormat::iteratorOpenIterationModesFamily, 0)];
        if (modes & mapAndSet) {
            JITCACHE_FAIL("the producer recorded a map or set iteration over an array"_s);
            return;
        }
        if (seeded == Seeded::FastMap)
            modes |= iterationModeBit(IterationMode::FastMap);
        else if (seeded == Seeded::FastSet)
            modes |= iterationModeBit(IterationMode::FastSet);
        auto words = encodeState(context, *image);
        if (!words)
            return;

        JSFunction* seededFunction = nullptr;
        {
            DeferGCForAWhile deferGC(vm);
            CodeBlock* newborn = createNewbornCodeBlock(context, twin);
            if (!newborn)
                return;
            auto prepared = CBStateImport::prepare(bytesOf(*words), *newborn, true);
            if (!prepared) {
                failWith(context, "prepare"_s, prepared.error());
                return;
            }
            {
                DeferGC deferSeed(vm);
                prepared->seedLinkedState(*newborn);
            }
            JSFunction* entries = globalObject->mapProtoEntriesFunctionConcurrently();
            JSFunction* values = globalObject->setProtoValuesFunctionConcurrently();
            switch (seeded) {
            case Seeded::FastMap:
                JITCACHE_CHECK(entries && !values);
                seededFunction = entries;
                break;
            case Seeded::FastSet:
                JITCACHE_CHECK(values && !entries);
                seededFunction = values;
                break;
            case Seeded::Neither:
                JITCACHE_CHECK(!entries && !values);
                break;
            }
        }
        if (!seededFunction)
            continue;

        NakedPtr<Exception> exception;
        auto source = seeded == Seeded::FastMap ? "new Map; Map.prototype.entries"_s : "new Set; Set.prototype.values"_s;
        JSValue installed = evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception) {
            JITCACHE_FAIL("creating the Map or Set threw"_s);
            continue;
        }
        JITCACHE_CHECK(installed == JSValue(seededFunction));
    }
}

// U8: capture, scoreLive, prepare, seedLinkedState without the realm step's bits, and finishCounter, each called with no
// GC deferral and so inside the lane's AssertNoGC scope with nothing to excuse an allocation, trip no assertion (I3), and
// capture charges exactly its two section sizes. The section carries a lazy-operand record, so seeding appends through
// the holder.
JITCACHE_TEST(cbImportAllocatesNoCellOutsideADeferral, Yes)
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);
    JITCACHE_CHECK(!vm.heap.isDeferred());

    auto score = CBStateCapture::scoreLive(*realm->producer, true, false);
    if (!score)
        failWith(context, "scoreLive"_s, score.error());

    // U8 asks for a budget that records each charge, but capture takes the integrator's ProducerBudget, which is final,
    // so no test budget can stand in (a recorded spec conflict). A fresh budget's charged and peak totals show instead
    // that the live capture holds exactly its two section sizes, never held more, and releases them when it dies.
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    std::optional<StateImage> image;
    {
        auto captured = CBStateCapture::capture(*realm->producer, budget.get(), true, false);
        if (!captured) {
            failWith(context, "capture"_s, captured.error());
            return;
        }
        size_t sectionBytes = captured->stateSection().size() + captured->summarySection().size();
        JITCACHE_CHECK(budget->chargedBytes() == sectionBytes);
        JITCACHE_CHECK(budget->peakBytes() == sectionBytes);
        image = decodeState(context, captured->stateSection());
    }
    JITCACHE_CHECK(!budget->chargedBytes());
    if (!image)
        return;

    setLazyOperands(*image, { lazyOperandRecord(BytecodeIndex(0).asBits(), argumentKind, virtualRegisterForArgumentIncludingThis(1).offset()) });
    JITCACHE_CHECK(!image->header.familyEntryCount[CBFormat::iteratorOpenIterationModesFamily]);
    auto words = encodeState(context, *image);
    if (!words)
        return;

    CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin);
    if (!newborn)
        return;
    JITCACHE_CHECK(!vm.heap.isDeferred());
    auto prepared = CBStateImport::prepare(bytesOf(*words), *newborn, true);
    if (!prepared) {
        failWith(context, "prepare"_s, prepared.error());
        return;
    }
    prepared->seedLinkedState(*newborn);
    {
        // Native setup, under a deferral as the install function runs it; DeferGCForAWhile never collects at exit.
        DeferGCForAWhile deferGC(vm);
        newborn->setupWithUnlinkedBaselineCode(code.copyRef());
    }
    JITCACHE_CHECK(!vm.heap.isDeferred());
    CBCounterRestore restore = prepared->finishCounter(*newborn);
    JITCACHE_CHECK(restore.carried);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
