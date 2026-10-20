#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayConventions.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "BytecodeIndex.h"
#include "CBFormat.h"
#include "CBState.h"
#include "CBTestSupport.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "FunctionExecutable.h"
#include "IndexingType.h"
#include "IterationModeMetadata.h"
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
#include "TestVM.h"
#include "ToThisStatus.h"
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

// Tests of the CB lane's import (cb/CBImport.cpp, cb/CBValidate.cpp): validateState and prepare against every check of
// a cb.state section, the strict checks on a newborn CB, finishCounter's counter, the realm step of seedLinkedState,
// and the lane's calls running with no cell allocation. Each test runs on a fresh VM and builds its CBs as the ICs
// lane's live tests do: it evaluates its source in its own global object, one call through JSC::call installs an LLInt
// CB, and JIT::compileSync compiles that CB to baseline. A newborn CB comes from newCodeBlockFor on a
// function never called, inside a DeferGCForAWhile; it is never installed, so the same function yields one each time,
// and the test keeps it in a local. Two functions with the same body text have separate UCBs with the same metadata
// layout, so a section captured from one's baseline CB pairs with the other's newborn CB. A CB after setup is such a
// newborn CB taken through prepare, seedLinkedState, setupWithUnlinkedBaselineCode with the first function's code, and
// finishCounter. Every lane call here runs with strict on, and every section starts as a strict capture of a real CB,
// which the tests decode, change and lay out again.

namespace JSC::JITCache::Tests {

namespace CBImportTestsInternal {

// The body of the async generator a global holds. An async generator's wrapper creates its body as a function expression
// and stores it in the generator's Next field without calling it (FunctionNode::emitBytecode,
// AsyncGeneratorWrapperFunctionMode), and the body first runs when something resumes the generator, as its first next()
// does (asyncGeneratorBodyCall). The body of a generator nothing has resumed is therefore a function never called, whose
// executable has no CB yet, so createNewbornCodeBlock takes it as it takes any function never called.
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

// The family of CBFormat's table that holds the field of Op of the given kind.
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

static bool hasEntries(TestContext& context, const StateImage& image, unsigned family, uint32_t minimum, CBCheck check)
{
    if (image.header.familyEntryCount[family] >= minimum)
        return true;
    JITCACHE_FAIL(makeString("the subject has "_s, image.header.familyEntryCount[family], " entries in family "_s, family, ", fewer than the "_s, minimum, " the "_s, description(check), " cases need"_s));
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

// A prediction bit no bytecode slot holds, and an array mode bit no structure has.
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

// The validation test's oracle: validateState and a strict prepare on the newborn CB both give `expected`, a fault of
// prepare is invalid material, and the CB still passes StrictLinkState with the unchanged section afterwards, so
// prepare wrote nothing.
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

// The body of the validation test's subject, which has an entry in every family a rule below changes. It is sloppy,
// so `this` gives a to_this; Array(n) gives a jneq_ptr and a new_array_with_size; the nested literals give two
// new_array and three new_array_buffer of each copy-on-write type; for-of gives iterator_open, iterator_next and the
// iterator_close_check that closes the loop on an abrupt exit; for-in gives the enumerator opcodes, and hasOwnProperty
// inside it a second jneq_ptr.
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

// The validation test's sources: the subject and its twin, and an async generator and its twin, whose bodies hold the
// async_iterator_open and async_iterator_next entries no sync body can. The program creates both generators, which
// leaves both bodies never called, and keeps them in globals; one call of startAsyncLoop then runs the first body.
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

// A body whose only value-profiled instructions, the two property reads, run only when flag is set.
static constexpr auto coldSource = "function cold(object, flag) { if (flag) return object.a + object.b; return 0; }\n"
    "function coldTwin(object, flag) { if (flag) return object.a + object.b; return 0; }\n"
    "var object = { a: 1, b: 2 };\n"_s;

// StateHeader: the span's size, the layout version, the tier and the counter mode.
static void expectHeaderRules(TestContext& context, const Subject& subject)
{
    expectOutcome(context, "state-header: an empty span"_s, subject.section().first(0), subject, CBCheck::StateHeader);
    expectOutcome(context, "state-header: a span one byte short of a header"_s, subject.section().first(sizeof(CBFormat::StateHeader) - 1), subject, CBCheck::StateHeader);
    expectMutation(context, subject, "state-header: layout version 0"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.layoutVersion = 0;
    });
    expectMutation(context, subject, "state-header: layout version 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.layoutVersion = 2;
    });
    expectMutation(context, subject, "state-header: tier 0"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.tier = 0;
    });
    expectMutation(context, subject, "state-header: tier 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.tier = 2;
    });
    expectMutation(context, subject, "state-header: counter mode 2"_s, CBCheck::StateHeader, [](StateImage& image) {
        image.header.counterMode = 2;
    });
    expectMutation(context, subject, "state-header: counter mode NotCarried with zero counter fields"_s, std::nullopt, [](StateImage& image) {
        setCounter(image, CBFormat::CounterMode::NotCarried, { });
    });
}

// ArgumentCount, ValueProfileCount, FamilyCount and StateLength.
static void expectCountAndLengthRules(TestContext& context, const Subject& subject, const Subject& asyncSubject)
{
    expectMutation(context, subject, "state-argument-count: one argument more"_s, CBCheck::ArgumentCount, [](StateImage& image) {
        ++image.header.numArguments;
        image.argumentPredictions.append(SpecNone);
    });
    expectMutation(context, subject, "state-argument-count: one argument fewer"_s, CBCheck::ArgumentCount, [](StateImage& image) {
        --image.header.numArguments;
        image.argumentPredictions.removeLast();
    });

    if (subject.image.header.numValueProfiles) {
        expectMutation(context, subject, "state-value-profile-count: one value profile more"_s, CBCheck::ValueProfileCount, [](StateImage& image) {
            ++image.header.numValueProfiles;
            image.valuePredictions.append(SpecNone);
        });
        expectMutation(context, subject, "state-value-profile-count: one value profile fewer"_s, CBCheck::ValueProfileCount, [](StateImage& image) {
            --image.header.numValueProfiles;
            image.valuePredictions.removeLast();
        });
    } else
        JITCACHE_FAIL("the subject has no value profile for the state-value-profile-count cases"_s);

    unsigned getByVal = familyOf<CBFormat::FamilyKind::ArrayProfile, OpGetByVal>();
    if (hasEntries(context, subject.image, getByVal, 1, CBCheck::FamilyCount)) {
        expectMutation(context, subject, "state-family-count: one get_by_val entry more"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            appendFamilyRecord(image, image.arrayProfiles, getByVal, CBFormat::ArrayProfileRecord { });
        });
        expectMutation(context, subject, "state-family-count: one get_by_val entry fewer"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            removeLastFamilyRecord(image, image.arrayProfiles, getByVal);
        });
    }
    // Only builtins emit new_array_with_species, so the subject has no entry of that family.
    unsigned species = familyOf<CBFormat::FamilyKind::ArrayProfile, OpNewArrayWithSpecies>();
    if (!subject.image.header.familyEntryCount[species]) {
        expectMutation(context, subject, "state-family-count: an entry in a family the body lacks"_s, CBCheck::FamilyCount, [&](StateImage& image) {
            appendFamilyRecord(image, image.arrayProfiles, species, CBFormat::ArrayProfileRecord { });
        });
    } else
        JITCACHE_FAIL("the subject has a new_array_with_species entry"_s);

    Vector<uint64_t> longer = subject.words;
    longer.append(0);
    expectOutcome(context, "state-length: eight bytes past the section"_s, bytesOf(longer), subject, CBCheck::StateLength);
    expectOutcome(context, "state-length: eight bytes short of the section"_s, subject.section().first(subject.section().size() - sizeof(uint64_t)), subject, CBCheck::StateLength);

    // The padding rule needs a section whose byte-sized arrays end short of an 8-byte boundary.
    bool checkedPadding = false;
    for (const Subject* candidate : { &subject, &asyncSubject }) {
        auto layout = CBFormat::stateLayout(candidate->image.header);
        if (!layout || layout->paddingOffset == layout->size)
            continue;
        Vector<uint64_t> padded = candidate->words;
        asMutableByteSpan(padded.mutableSpan())[layout->size - 1] = 1;
        expectOutcome(context, "state-length: a nonzero padding byte"_s, bytesOf(padded), *candidate, CBCheck::StateLength);
        checkedPadding = true;
        break;
    }
    if (!checkedPadding)
        JITCACHE_FAIL("neither subject's section has a padding byte for the state-length cases"_s);
}

// ValuePrediction and ArrayProfile.
static void expectPredictionAndArrayProfileRules(TestContext& context, const Subject& subject)
{
    expectMutation(context, subject, "state-value-prediction: argument 0 at SpecBytecodeTop"_s, std::nullopt, [](StateImage& image) {
        image.argumentPredictions[0] = SpecBytecodeTop;
    });
    expectMutation(context, subject, "state-value-prediction: argument 0 past SpecBytecodeTop"_s, CBCheck::ValuePrediction, [](StateImage& image) {
        image.argumentPredictions[0] = SpecBytecodeTop | predictionOutsideBytecodeTop;
    });
    if (!subject.image.valuePredictions.isEmpty()) {
        expectMutation(context, subject, "state-value-prediction: value profile 1 at SpecBytecodeTop"_s, std::nullopt, [](StateImage& image) {
            image.valuePredictions[0] = SpecBytecodeTop;
        });
        expectMutation(context, subject, "state-value-prediction: value profile 1 past SpecBytecodeTop"_s, CBCheck::ValuePrediction, [](StateImage& image) {
            image.valuePredictions[0] = SpecBytecodeTop | predictionOutsideBytecodeTop;
        });
    }

    if (subject.image.arrayProfiles.isEmpty()) {
        JITCACHE_FAIL("the subject has no array profile for the state-array-profile cases"_s);
        return;
    }
    expectMutation(context, subject, "state-array-profile: every array mode"_s, std::nullopt, [](StateImage& image) {
        image.arrayProfiles[0].observedArrayModes = ALL_ARRAY_MODES;
    });
    expectMutation(context, subject, "state-array-profile: an array mode past ALL_ARRAY_MODES"_s, CBCheck::ArrayProfile, [](StateImage& image) {
        image.arrayProfiles[0].observedArrayModes = ALL_ARRAY_MODES | arrayModeOutsideAllModes;
    });
    expectMutation(context, subject, "state-array-profile: all eight flags"_s, std::nullopt, [](StateImage& image) {
        image.arrayProfiles[0].flags = 0xff;
    });
    expectMutation(context, subject, "state-array-profile: a ninth flag"_s, CBCheck::ArrayProfile, [](StateImage& image) {
        image.arrayProfiles[0].flags = 0x100;
    });
}

// AllocationHint, on three literals of each copy-on-write type.
static void expectAllocationHintRules(TestContext& context, const Subject& subject)
{
    unsigned newArray = familyOf<CBFormat::FamilyKind::AllocationHint, OpNewArray>();
    if (hasEntries(context, subject.image, newArray, 1, CBCheck::AllocationHint)) {
        auto expectNewArrayHint = [&](ASCIILiteral what, IndexingType indexingType, unsigned vectorLength, std::optional<CBCheck> expected) {
            expectMutation(context, subject, makeString("state-allocation-hint: new_array hint "_s, what), expected, [&](StateImage& image) {
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

    // The F19 entry each literal names, by the literal's type: the bytecode generator gives a constant literal
    // CopyOnWrite ORed with the least upper bound of its constants' types.
    std::array<Vector<unsigned>, 3> literals;
    CBFormat::forEachNewArrayBufferLiteral(*subject.newborn, [&](unsigned metadataID, IndexingType recommendedIndexingType) {
        if (recommendedIndexingType == CopyOnWriteArrayWithInt32)
            literals[0].append(metadataID);
        else if (recommendedIndexingType == CopyOnWriteArrayWithDouble)
            literals[1].append(metadataID);
        else if (recommendedIndexingType == CopyOnWriteArrayWithContiguous)
            literals[2].append(metadataID);
    });
    if (literals[0].size() != 3 || literals[1].size() != 3 || literals[2].size() != 3 || !hasEntries(context, subject.image, CBFormat::newArrayBufferFamily, 9, CBCheck::AllocationHint)) {
        JITCACHE_FAIL(makeString("the subject's literals are "_s, literals[0].size(), " Int32, "_s, literals[1].size(), " Double and "_s, literals[2].size(), " Contiguous instead of three of each"_s));
        return;
    }
    auto expectLiteralHint = [&](ASCIILiteral what, unsigned metadataID, IndexingType indexingType, std::optional<CBCheck> expected) {
        expectMutation(context, subject, makeString("state-allocation-hint: "_s, what, " (entry "_s, metadataID, ')'), expected, [&](StateImage& image) {
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

// IterationModes: each opcode's native bits, whose masks the test spells in hexadecimal.
static void expectIterationModeRules(TestContext& context, const Subject& subject, const Subject& asyncSubject)
{
    auto expectModes = [&](const Subject& owner, unsigned family, ASCIILiteral what, uint16_t modes, std::optional<CBCheck> expected) {
        expectMutation(context, owner, makeString("state-iteration-modes: "_s, what), expected, [&](StateImage& image) {
            image.iterationModes[recordIndex(image, family, 0)] = modes;
        });
    };

    if (hasEntries(context, subject.image, CBFormat::iteratorOpenIterationModesFamily, 1, CBCheck::IterationModes)) {
        unsigned family = CBFormat::iteratorOpenIterationModesFamily;
        expectModes(subject, family, "iterator_open's full mask"_s, 0x1fff, std::nullopt);
        expectModes(subject, family, "FastAsyncGenerator at iterator_open"_s, iterationModeBit(IterationMode::FastAsyncGenerator), CBCheck::IterationModes);
        expectModes(subject, family, "AsyncFromSync at iterator_open"_s, iterationModeBit(IterationMode::AsyncFromSync), CBCheck::IterationModes);
    }
    if (hasEntries(context, subject.image, CBFormat::iteratorNextIterationModesFamily, 1, CBCheck::IterationModes)) {
        unsigned family = CBFormat::iteratorNextIterationModesFamily;
        expectModes(subject, family, "iterator_next's full mask"_s, 0x1ff3, std::nullopt);
        expectModes(subject, family, "FastArray at iterator_next"_s, iterationModeBit(IterationMode::FastArray), std::nullopt);
        expectModes(subject, family, "FastMap at iterator_next"_s, iterationModeBit(IterationMode::FastMap), CBCheck::IterationModes);
        expectModes(subject, family, "FastSet at iterator_next"_s, iterationModeBit(IterationMode::FastSet), CBCheck::IterationModes);
    }
    // The for-of's iterator_close_check, which keeps its modes in a byte.
    if (hasEntries(context, subject.image, CBFormat::iteratorCloseCheckIterationModesFamily, 1, CBCheck::IterationModes)) {
        unsigned family = CBFormat::iteratorCloseCheckIterationModesFamily;
        expectModes(subject, family, "iterator_close_check's full mask"_s, 0x12, std::nullopt);
        expectModes(subject, family, "Generic at iterator_close_check"_s, iterationModeBit(IterationMode::Generic), CBCheck::IterationModes);
        expectModes(subject, family, "FastMap at iterator_close_check"_s, iterationModeBit(IterationMode::FastMap), CBCheck::IterationModes);
    }
    if (hasEntries(context, asyncSubject.image, CBFormat::asyncIteratorOpenIterationModesFamily, 1, CBCheck::IterationModes)) {
        unsigned family = CBFormat::asyncIteratorOpenIterationModesFamily;
        expectModes(asyncSubject, family, "async_iterator_open's full mask"_s, 0x6001, std::nullopt);
        expectModes(asyncSubject, family, "FastArray at async_iterator_open"_s, iterationModeBit(IterationMode::FastArray), CBCheck::IterationModes);
    }
    if (hasEntries(context, asyncSubject.image, CBFormat::asyncIteratorNextIterationModesFamily, 1, CBCheck::IterationModes)) {
        unsigned family = CBFormat::asyncIteratorNextIterationModesFamily;
        expectModes(asyncSubject, family, "async_iterator_next's full mask"_s, 0x2001, std::nullopt);
        expectModes(asyncSubject, family, "AsyncFromSync at async_iterator_next"_s, iterationModeBit(IterationMode::AsyncFromSync), CBCheck::IterationModes);
    }
}

// EnumeratorModes, ToThisStatus and BranchBit.
static void expectByteRules(TestContext& context, const Subject& subject)
{
    unsigned enumeratorNext = familyOf<CBFormat::FamilyKind::EnumeratorModes, OpEnumeratorNext>();
    if (hasEntries(context, subject.image, enumeratorNext, 1, CBCheck::EnumeratorModes)) {
        expectMutation(context, subject, "state-enumerator-modes: every enumerator flag"_s, std::nullopt, [&](StateImage& image) {
            image.enumeratorModes[recordIndex(image, enumeratorNext, 0)] = static_cast<uint8_t>(JSPropertyNameEnumerator::IndexedMode | JSPropertyNameEnumerator::OwnStructureMode
                | JSPropertyNameEnumerator::GenericMode | JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch);
        });
        expectMutation(context, subject, "state-enumerator-modes: a fifth enumerator flag"_s, CBCheck::EnumeratorModes, [&](StateImage& image) {
            image.enumeratorModes[recordIndex(image, enumeratorNext, 0)] = 0x10;
        });
    }
    if (hasEntries(context, subject.image, CBFormat::toThisStatusFamily, 1, CBCheck::ToThisStatus)) {
        expectMutation(context, subject, "state-to-this-status: ToThisClearedByGC"_s, std::nullopt, [](StateImage& image) {
            image.toThisStatuses[recordIndex(image, CBFormat::toThisStatusFamily, 0)] = static_cast<uint8_t>(ToThisClearedByGC);
        });
        expectMutation(context, subject, "state-to-this-status: a status past ToThisClearedByGC"_s, CBCheck::ToThisStatus, [](StateImage& image) {
            image.toThisStatuses[recordIndex(image, CBFormat::toThisStatusFamily, 0)] = static_cast<uint8_t>(ToThisClearedByGC + 1);
        });
    }
    if (hasEntries(context, subject.image, CBFormat::branchBitFamily, 1, CBCheck::BranchBit)) {
        expectMutation(context, subject, "state-branch-bit: a branch bit of 1"_s, std::nullopt, [](StateImage& image) {
            image.branchBits[recordIndex(image, CBFormat::branchBitFamily, 0)] = 1;
        });
        expectMutation(context, subject, "state-branch-bit: a branch bit of 2"_s, CBCheck::BranchBit, [](StateImage& image) {
            image.branchBits[recordIndex(image, CBFormat::branchBitFamily, 0)] = 2;
        });
    }
}

// LazyOperand: each mutation in a section whose only lazy-operand records are the ones named.
static void expectLazyOperandRules(TestContext& context, const Subject& subject)
{
    CodeBlock& newborn = *subject.newborn;
    uint32_t firstOffset = BytecodeIndex(0).asBits();
    int32_t firstArgument = virtualRegisterForArgumentIncludingThis(1).offset();
    int32_t firstLocal = virtualRegisterForLocal(0).offset();
    auto expectRecords = [&](ASCIILiteral what, std::optional<CBCheck> expected, std::initializer_list<CBFormat::LazyOperandRecord> records) {
        Vector<CBFormat::LazyOperandRecord> list(records);
        expectMutation(context, subject, makeString("state-lazy-operand: "_s, what), expected, [&](StateImage& image) {
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

// TierUpHistory and Counter. The accepted counters keep a progress that is not negative, so StrictCounter holds.
static void expectTierUpAndCounterRules(TestContext& context, const Subject& subject)
{
    uint16_t maximumDelay = static_cast<uint16_t>(Options::maximumOptimizationDelay());
    uint16_t maximumReoptimizations = static_cast<uint16_t>(Options::reoptimizationRetryCounterMax());
    expectMutation(context, subject, "state-tier-up-history: the most profile deferrals"_s, std::nullopt, [&](StateImage& image) {
        image.header.optimizationDelayCounter = maximumDelay;
    });
    expectMutation(context, subject, "state-tier-up-history: one profile deferral past the most"_s, CBCheck::TierUpHistory, [&](StateImage& image) {
        image.header.optimizationDelayCounter = maximumDelay + 1;
    });
    expectMutation(context, subject, "state-tier-up-history: the most reoptimizations"_s, std::nullopt, [&](StateImage& image) {
        image.header.reoptimizationRetryCounter = maximumReoptimizations;
    });
    expectMutation(context, subject, "state-tier-up-history: one reoptimization past the most"_s, CBCheck::TierUpHistory, [&](StateImage& image) {
        image.header.reoptimizationRetryCounter = maximumReoptimizations + 1;
    });

    auto expectCounter = [&](ASCIILiteral what, CBFormat::CounterMode mode, CounterTriple triple, std::optional<CBCheck> expected) {
        expectMutation(context, subject, makeString("state-counter: "_s, what), expected, [&](StateImage& image) {
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

// Takes a newborn CB of the function through the import's install sequence with the section, inside one deferral:
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
        ", crossed "_s, static_cast<unsigned>(restore.crossed), ", nativeSlice "_s, restore.nativeSlice, ", slice "_s, restore.slice, ", startupDeferralScale "_s, restore.startupDeferralScale, ')');
}

// The counter finishCounter restores from a carried record under the VM's startup deferral scale, and, for a finite
// threshold, the native reference: a stack counter given the same triple and checked on the same CB with the same scale
// right after finishCounter, which no pool change can separate from finishCounter's own check in a VM with
// useConcurrentJIT off that is alone in its process.
static void checkCarriedRestore(TestContext& context, const String& what, CodeBlock& codeBlock, const CounterTriple& record, const CBCounterRestore& restore, double scale)
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
    if (restore.startupDeferralScale != scale)
        fail("the restore does not report the VM's startup deferral scale"_s);
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

    // The envelope M >= 1 and s >= 1 leave to any pool. The ceiling C bounds the slice only at a scale of 1, since a
    // scaled slice may run to one unscaled period, M * T.
    int64_t ceiling = maximumExecutionCountsBetweenCheckpoints(CountingForBaseline, &codeBlock);
    bool sliceInEnvelope = restore.crossed ? !restore.nativeSlice : restore.nativeSlice >= 0 && (scale != 1 || restore.nativeSlice <= ceiling);
    if (!sliceInEnvelope)
        fail("the native slice is negative, passes C at a scale of 1, or is not 0 after a crossing"_s);
    double threshold = record.activeThreshold;
    double scaledThreshold = scale * threshold;
    if (progress < scaledThreshold - std::min(threshold, static_cast<double>(ceiling)) / 2) {
        if (restore.crossed || restore.nativeSlice < static_cast<int64_t>(std::trunc(std::min(scaledThreshold - progress, static_cast<double>(ceiling)))))
            fail("a progress short of the scaled threshold crossed or armed less than s * T - P"_s);
    }

    BaselineExecutionCounter reference;
    reference.m_activeThreshold = record.activeThreshold;
    reference.m_totalCount = record.totalCount;
    reference.m_counter = record.value;
    bool crossed = reference.checkIfThresholdCrossedAndSet(&codeBlock, scale);
    int64_t nativeSlice = crossed ? 0 : -static_cast<int64_t>(reference.m_counter);
    if (restore.crossed != crossed || restore.nativeSlice != nativeSlice)
        fail("the restore differs from the native check of the same triple and scale"_s);
}

// The counter after finishCounter when it does not travel: finishCounter left setup's arming, which follows the seeded
// reoptimization count and the UCB's quick DFG bit and never the startup deferral scale, and setup's aging sample of the
// fresh counter.
static void checkSetupArming(TestContext& context, const String& what, CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore, bool quickDFGTierUp)
{
    CounterTriple live = liveCounter(codeBlock);
    auto fail = [&](ASCIILiteral rule) {
        JITCACHE_FAIL(makeString(what, ": "_s, rule, describeRestore(live, restore)));
    };
    if (restore.carried || restore.crossed)
        fail("the restore says a counter that did not travel was carried or crossed"_s);
    if (restore.startupDeferralScale != 1)
        fail("the restore of a counter that did not travel reports a scale other than 1"_s);
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

// From strict captures of two real CBs, every field set just inside and just outside the bound its check puts on it
// gives exactly the expected outcome from both validateState and a strict prepare on a newborn twin, never a crash, and
// leaves that CB at its link state. The async generator subject supplies the async_iterator families.
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

// A CB that already ran fails StrictNewborn, and a newborn CB that seedLinkedState already wrote fails StrictLinkState,
// so seeding, which the glue calls only after prepare succeeds for that CB, never reaches either; StrictCounter rejects
// negative progress under a finite threshold when no float rounding of the total explains it, and accepts it under
// INT32_MAX; and lazy-operand keys outside the CB's own frame, as an inlined CB holds, pass prepare.
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

    // StrictNewborn: the section's counts match the producer's own CB, which already ran.
    JITCACHE_CHECK(!validateState(section, *realm->producer));
    expectPrepareFails("prepare on the producer's baseline CB"_s, section, *realm->producer, CBCheck::StrictNewborn);

    DeferGCForAWhile deferGC(vm);

    // StrictLinkState: a newborn CB seeded once is no longer at its link state.
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

    // StrictCounter.
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

    // Keys that name slots of an inlining compile's machine frame, which OSR exit from inlined code records.
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

// finishCounter on a CB after setup restores each carried record's counter as checkCarriedRestore requires, and for a
// finite threshold equals the native reference, with the VM's startup deferral scale at 1 and at 4, where a progress P
// between T and 4 * T - min(T, C) / 2 does not cross; two splits of the same P under the same finite T restore identical
// counters; and a counter that did not travel stays as setup armed it, for reoptimization counts of 0 and 3 with the
// quick DFG bit set and clear, and reports a scale of 1 at either scale.
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
        { "P = 2 * T"_s, { -1000, 3000, 1000 } },
    };
    constexpr double scaled = 4;
    for (double scale : { 1.0, scaled }) {
        vm.setStartupJITDeferralScale(scale);
        for (const auto& carriedCase : carriedCases) {
            StateImage carried = *image;
            setCounter(carried, CBFormat::CounterMode::Carried, carriedCase.record);
            auto words = encodeState(context, carried);
            if (!words)
                return;
            String what = makeString(carriedCase.name, " at a startup deferral scale of "_s, scale);
            restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), nothingBeforeSetup, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
                checkCarriedRestore(context, what, codeBlock, carriedCase.record, restore, scale);
                // P = 2 * T lies between T and 4 * T - min(T, C) / 2 whatever the ceiling: only the scale keeps it from
                // crossing.
                if (scale == scaled && carriedCase.record == CounterTriple { -1000, 3000, 1000 } && restore.crossed)
                    JITCACHE_FAIL(makeString(what, ": crossed"_s));
            });
        }
    }
    vm.setStartupJITDeferralScale(1);

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
            checkCarriedRestore(context, "a split of P = 500.5"_s, codeBlock, split, restore, 1);
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

    for (double scale : { 1.0, scaled }) {
        vm.setStartupJITDeferralScale(scale);
        for (uint16_t reoptimizations : { 0, 3 }) {
            for (bool quickDFGTierUp : { false, true }) {
                StateImage notCarried = *image;
                setCounter(notCarried, CBFormat::CounterMode::NotCarried, { });
                notCarried.header.reoptimizationRetryCounter = reoptimizations;
                auto words = encodeState(context, notCarried);
                if (!words)
                    return;
                String what = makeString("NotCarried with "_s, reoptimizations, " reoptimizations and the quick DFG bit "_s, quickDFGTierUp ? "set"_s : "clear"_s, " at a startup deferral scale of "_s, scale);
                restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), [&](CodeBlock& codeBlock) {
                    codeBlock.unlinkedCodeBlock()->setQuickDFGTierUp(quickDFGTierUp ? TriState::True : TriState::False);
                }, [&](CodeBlock& codeBlock, const CounterTriple& armed, const CBCounterRestore& restore) {
                    JITCACHE_CHECK(codeBlock.reoptimizationRetryCounter() == reoptimizations);
                    checkSetupArming(context, what, codeBlock, armed, restore, quickDFGTierUp);
                });
            }
        }
    }
    vm.setStartupJITDeferralScale(1);
}

// A slice that the startup deferral scale lets past 2^24 can leave the float total below trunc(slice), so count() is
// slightly negative under a finite threshold. With the slice ceiling at 2^25 + 1, a threshold of 10^7 and a scale of 4,
// finishCounter re-slices a fresh arming's progress of 0.25 to exactly that ceiling for any pool multiplier below 3.3,
// and the float nearest 33554433.25 is 33554432, so count() is -1. A strict capture of that CB, as a delta takes of an
// imported body that has not run since, succeeds, and a strict prepare of what it captured accepts the counter. A total
// below the float nearest -counterValue, which no native counter holds, still fails StrictCounter.
JITCACHE_TEST_WITH_OPTIONS(cbImportStrictAcceptsAScaledCounterRoundedBelowItsSlice, Yes, "--useConcurrentJIT=false --maximumExecutionCountsBetweenCheckpointsForBaseline=33554433")
{
    VM& vm = *context.vm();
    auto realm = createElementRealm(context, vm);
    if (!realm)
        return;
    auto image = captureState(context, *realm->producer);
    if (!image)
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*realm->producer);

    constexpr int64_t ceiling = 33554433;
    constexpr int32_t threshold = 10000000;
    constexpr double scale = 4;
    const CounterTriple record { -1000, 1000.25f, threshold };
    StateImage carried = *image;
    setCounter(carried, CBFormat::CounterMode::Carried, record);
    auto words = encodeState(context, carried);
    if (!words)
        return;

    std::optional<StateImage> recaptured;
    vm.setStartupJITDeferralScale(scale);
    restoreOnNewborn(context, realm->twin, bytesOf(*words), code.get(), [](CodeBlock&) { }, [&](CodeBlock& codeBlock, const CounterTriple&, const CBCounterRestore& restore) {
        checkCarriedRestore(context, "a scaled slice past 2^24"_s, codeBlock, record, restore, scale);
        if (restore.crossed || restore.nativeSlice != ceiling || codeBlock.baselineJITData()->executeCounter().count() >= 0) {
            JITCACHE_FAIL(makeString("the restore does not leave a total rounded below its slice"_s, describeRestore(liveCounter(codeBlock), restore)));
            return;
        }
        Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
        auto captured = CBStateCapture::capture(codeBlock, budget.get(), true, false);
        if (!captured) {
            failWith(context, "the strict capture of the restored CB"_s, captured.error());
            return;
        }
        recaptured = decodeState(context, captured->stateSection());
    });
    vm.setStartupJITDeferralScale(1);
    if (!recaptured)
        return;

    JITCACHE_CHECK(recaptured->header.counterValue == -ceiling);
    JITCACHE_CHECK(recaptured->header.counterTotalCount == 33554432.0f);
    JITCACHE_CHECK(counterObeysNativeInvariant(recaptured->header));

    // count() is -3, below the -2 that half the float spacing of 2^25 allows.
    StateImage beyond = *image;
    setCounter(beyond, CBFormat::CounterMode::Carried, { -33554435, 33554432.0f, threshold });
    JITCACHE_CHECK(!counterObeysNativeInvariant(beyond.header));

    auto recapturedWords = encodeState(context, *recaptured);
    auto beyondWords = encodeState(context, beyond);
    if (!recapturedWords || !beyondWords)
        return;
    DeferGCForAWhile deferGC(vm);
    CodeBlock* newborn = createNewbornCodeBlock(context, realm->twin);
    if (!newborn)
        return;
    auto prepared = CBStateImport::prepare(bytesOf(*recapturedWords), *newborn, true);
    if (!prepared)
        failWith(context, "prepare of the recaptured counter"_s, prepared.error());
    auto rejected = CBStateImport::prepare(bytesOf(*beyondWords), *newborn, true);
    if (rejected || rejected.error().kind != CBFaultKind::InvalidMaterial || rejected.error().check != CBCheck::StrictCounter)
        JITCACHE_FAIL("prepare of a total below the float nearest -counterValue does not fail StrictCounter"_s);
}

// In a fresh realm that has created no Map and no Set, seedLinkedState with an iterator_open record holding FastMap
// (FastSet), inside a DeferGC as the install function's deferral surrounds it, leaves the realm's lazy entries (values)
// function materialized, and a later new Map (new Set) installs that same function; with neither bit, both stay null.
// One realm brings iterate to baseline for all three cases: the VM's CodeCache gives every realm that evaluates
// iterateSource the same UCBs, so in a later realm iterate's first call would install the baseline code the first
// compile parked in its UCB's sharing slot. The realm step reads only the seeded CB's realm, so each case seeds a
// newborn CB of iterateTwin in a realm of its own.
JITCACHE_TEST(cbImportRealmStepMaterializesTheIteratorFunction, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* producerRealm = createRealm(context, vm, iterateSource);
    if (!producerRealm)
        return;
    JSFunction* iterate = globalFunction(context, producerRealm, "iterate"_s);
    auto array = globalObjectValue(context, producerRealm, "array"_s);
    if (!iterate || !array)
        return;
    CodeBlock* producer = bringToBaseline(context, producerRealm, iterate, { *array });
    if (!producer)
        return;
    auto captured = captureState(context, *producer);
    if (!captured || !hasEntries(context, *captured, CBFormat::iteratorOpenIterationModesFamily, 1, CBCheck::IterationModes))
        return;
    size_t modesIndex = recordIndex(*captured, CBFormat::iteratorOpenIterationModesFamily, 0);
    uint16_t mapAndSet = iterationModeBit(IterationMode::FastMap) | iterationModeBit(IterationMode::FastSet);
    if (captured->iterationModes[modesIndex] & mapAndSet) {
        JITCACHE_FAIL("the producer recorded a map or set iteration over an array"_s);
        return;
    }

    enum class Seeded : uint8_t { FastMap, FastSet, Neither };
    for (Seeded seeded : { Seeded::FastMap, Seeded::FastSet, Seeded::Neither }) {
        JSGlobalObject* globalObject = createRealm(context, vm, iterateSource);
        if (!globalObject)
            return;
        JSFunction* twin = globalFunction(context, globalObject, "iterateTwin"_s);
        if (!twin)
            return;
        if (globalObject->mapProtoEntriesFunctionConcurrently() || globalObject->setProtoValuesFunctionConcurrently()) {
            JITCACHE_FAIL("the fresh realm created a Map or a Set before the seed"_s);
            return;
        }

        StateImage image = *captured;
        if (seeded == Seeded::FastMap)
            image.iterationModes[modesIndex] |= iterationModeBit(IterationMode::FastMap);
        else if (seeded == Seeded::FastSet)
            image.iterationModes[modesIndex] |= iterationModeBit(IterationMode::FastSet);
        auto words = encodeState(context, image);
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

// A metadata table gets its predictions only when a seeded value record is not SpecNone, as natively only a prediction
// allocates them: seedLinkedState of a section whose value records are all SpecNone leaves the newborn CB's table
// without predictions, and the same section with the record at offset 1 set allocates them, with that offset reading
// the record and every other offset SpecNone. A strict capture of a baseline CB whose table has no predictions, since
// none of its value-profiled instructions ran, writes SpecNone at every offset and allocates none.
JITCACHE_TEST(cbImportAllocatesValuePredictionsOnlyForARecord, Yes)
{
    VM& vm = *context.vm();
    if (!Options::useLazyValueProfilePredictions()) {
        JITCACHE_FAIL("useLazyValueProfilePredictions is off, so every metadata table allocates its predictions when it is linked"_s);
        return;
    }
    JSGlobalObject* globalObject = createRealm(context, vm, coldSource);
    if (!globalObject)
        return;
    JSFunction* cold = globalFunction(context, globalObject, "cold"_s);
    JSFunction* twin = globalFunction(context, globalObject, "coldTwin"_s);
    auto object = globalObjectValue(context, globalObject, "object"_s);
    if (!cold || !twin || !object)
        return;
    CodeBlock* producer = bringToBaseline(context, globalObject, cold, { *object, jsBoolean(false) });
    if (!producer)
        return;
    MetadataTable* producerTable = producer->metadataTable();
    if (!producerTable || !producer->unlinkedCodeBlock()->metadata().numValueProfiles()) {
        JITCACHE_FAIL("cold's baseline CB has no metadata value profile"_s);
        return;
    }
    if (producerTable->valueProfilePredictions()) {
        JITCACHE_FAIL("cold's metadata table has predictions, though none of its value-profiled instructions ran"_s);
        return;
    }

    auto image = captureState(context, *producer);
    if (!image)
        return;
    JITCACHE_CHECK(!producerTable->valueProfilePredictions());
    JITCACHE_CHECK(image->header.numValueProfiles == producer->unlinkedCodeBlock()->metadata().numValueProfiles());
    JITCACHE_CHECK(std::ranges::all_of(image->valuePredictions, [](uint64_t prediction) {
        return prediction == SpecNone;
    }));

    for (bool withRecord : { false, true }) {
        StateImage seeded = *image;
        if (withRecord)
            seeded.valuePredictions[0] = SpecInt32Only;
        auto words = encodeState(context, seeded);
        if (!words)
            return;
        DeferGCForAWhile deferGC(vm);
        CodeBlock* newborn = createNewbornCodeBlock(context, twin);
        if (!newborn)
            return;
        MetadataTable* table = newborn->metadataTable();
        if (!table || table->valueProfilePredictions()) {
            JITCACHE_FAIL("the newborn CB has no metadata table, or a table that already has predictions"_s);
            return;
        }
        auto prepared = CBStateImport::prepare(bytesOf(*words), *newborn, true);
        if (!prepared) {
            failWith(context, "prepare"_s, prepared.error());
            return;
        }
        prepared->seedLinkedState(*newborn);
        if (!withRecord) {
            JITCACHE_CHECK(!table->valueProfilePredictions());
            continue;
        }
        if (!table->valueProfilePredictions()) {
            JITCACHE_FAIL("seeding a value record other than SpecNone allocated no predictions"_s);
            continue;
        }
        unsigned visited = 0;
        CBFormat::forEachMetadataValueProfile(*newborn, [&](unsigned offset, const ValueProfileRef& profile) {
            ++visited;
            SpeculatedType expected = offset == 1 ? SpecInt32Only : SpecNone;
            if (profile.prediction() != expected)
                JITCACHE_FAIL(makeString("the value profile at offset "_s, offset, " predicts "_s, profile.prediction(), " instead of "_s, expected));
        });
        JITCACHE_CHECK(visited == seeded.header.numValueProfiles);
    }
}

// capture, scoreLive, prepare, seedLinkedState without the realm step's bits, and finishCounter, each called with no GC
// deferral and so inside the lane's AssertNoGC scope with nothing to excuse an allocation, trip no assertion, and
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

    // Capture takes the integrator's ProducerBudget, which is final, so no budget that records each charge can stand
    // in. A fresh budget's charged and peak totals show instead that the live capture holds exactly its two section
    // sizes, never held more, and releases them when it dies.
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
