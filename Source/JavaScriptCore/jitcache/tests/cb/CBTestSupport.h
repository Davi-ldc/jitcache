#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "CBFormat.h"
#include "CBState.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "FunctionExecutable.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "Operands.h"
#include "ProducerBudget.h"
#include "SpeculatedType.h"
#include "TopExceptionScope.h"
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

// What the CB lane's tests share: the newborn CB an import starts from, cb.state decoded into an image that a test changes
// and lays out again, the strict capture of a real CB that every such image starts as, and the baseline counter's three
// fields as a cb.state header records them.

namespace JSC::JITCache::Tests {

// The newborn CB of a function never called: linked by finishCreation inside a DeferGCForAWhile, as newCodeBlockFor
// requires, and never installed or run. The function yields a new one at each call.
inline CodeBlock* createNewbornCodeBlock(TestContext& context, JSFunction* function)
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

// Fails the test with the step and the fault the lane returned.
inline void failWith(TestContext& context, StringView step, const CBFault& fault)
{
    JITCACHE_FAIL(makeString(step, " failed with "_s, fault.kind == CBFaultKind::RecordingFault ? "a recording fault"_s : "invalid material"_s, ": "_s, description(fault.check)));
}

// cb.state decoded into its header and its nine arrays, so a test can change a field or a count and lay the section out
// again.
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
inline void forEachImageArray(Image& image, const Functor& functor)
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

inline std::optional<StateImage> decodeState(TestContext& context, std::span<const uint8_t> section)
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

// The section an image lays out: its header as it stands, the arrays at the offsets its counts give, and zero padding, in
// 64-bit words so that the section starts 8-byte aligned, as the integrator hands every section to the lane. A test that
// changes a count changes the matching array with it, so arrays that disagree with the header are the test's own mistake.
inline std::optional<Vector<uint64_t>> encodeState(TestContext& context, const StateImage& image)
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

inline std::span<const uint8_t> bytesOf(const Vector<uint64_t>& words)
{
    return asByteSpan(words.span());
}

// A strict capture of the CB with its counter carried, decoded. The test's codec must lay the image out again exactly as
// capture did, so every section a test builds from it differs from a real one only where the test changed it.
inline std::optional<StateImage> captureState(TestContext& context, CodeBlock& codeBlock)
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

// The index, within its kind's array, of entry `entry` of the family.
inline size_t recordIndex(const StateImage& image, unsigned family, uint32_t entry)
{
    return CBFormat::firstRecordOfFamily(image.header.familyEntryCount, family) + entry;
}

// The baseline counter's three fields, as capture reads them and a cb.state header records them.
struct CounterTriple {
    int32_t value { 0 };
    float totalCount { 0 };
    int32_t activeThreshold { 0 };

    friend bool operator==(const CounterTriple&, const CounterTriple&) = default;
};

inline CounterTriple liveCounter(CodeBlock& codeBlock)
{
    const BaselineExecutionCounter& counter = codeBlock.baselineJITData()->executeCounter();
    return { counter.m_counter, counter.m_totalCount, counter.m_activeThreshold };
}

inline void setCounter(StateImage& image, CBFormat::CounterMode mode, const CounterTriple& triple)
{
    image.header.counterMode = static_cast<uint8_t>(mode);
    image.header.counterValue = triple.value;
    image.header.counterTotalCount = triple.totalCount;
    image.header.counterActiveThreshold = triple.activeThreshold;
}

inline CBFormat::LazyOperandRecord lazyOperandRecord(uint32_t bytecodeIndexBits, uint32_t operandKind, int32_t operandValue, uint64_t prediction = SpecInt32Only)
{
    return { .bytecodeIndexBits = bytecodeIndexBits, .operandKind = operandKind, .operandValue = operandValue, .reserved = 0, .prediction = prediction };
}

// Replaces the image's lazy-operand records and their count together.
inline void setLazyOperands(StateImage& image, const Vector<CBFormat::LazyOperandRecord>& records)
{
    image.lazyOperandProfiles = records;
    image.header.numLazyOperandProfiles = static_cast<uint32_t>(records.size());
}

// The operand kinds a lazy-operand record stores.
inline constexpr uint32_t argumentKind = static_cast<uint32_t>(OperandKind::Argument);
inline constexpr uint32_t localKind = static_cast<uint32_t>(OperandKind::Local);
inline constexpr uint32_t tmpKind = static_cast<uint32_t>(OperandKind::Tmp);

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
