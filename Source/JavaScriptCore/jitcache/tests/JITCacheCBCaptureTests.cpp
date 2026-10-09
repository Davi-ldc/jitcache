/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "BaselineJITCode.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "FunctionExecutable.h"
#include "JIT.h"
#include "JITCacheCBFormat.h"
#include "JITCacheCBState.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

// The capture tests of SPEC-cb.md task 3: U11 (section 11.2), and the ownership a CBStateCapture has of its two sections
// and their charge (section 4.4). Each test runs on a fresh VM with default options and builds its CB as SPEC-ics.md's
// live tests do: it evaluates its source in its own global object, and one call through JSC::call installs an LLInt CB
// that JIT::compileSync then compiles to baseline. The source keeps every value the test passes in its globals, and the
// global object stays reachable from the test's stack.

namespace JSC::JITCache::Tests {

namespace CBCaptureTestsInternal {

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

// The function's baseline CB: the call installs the LLInt CB, and compileSync compiles and installs baseline code in
// that same CB, whose setup arms the baseline counter.
static CodeBlock* bringToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, std::initializer_list<JSValue> arguments)
{
    VM& vm = globalObject->vm();
    if (!callFunction(context, globalObject, function, arguments))
        return nullptr;
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not install an LLInt CB"_s);
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

// A body with an argument, value and array profile, so both sections hold slots beside their headers.
static constexpr auto elementSource = "function element(array, index) { return array[index] + 1; }"
    "var array = [1, 2, 3];"_s;

// Brings element to baseline and runs its baseline code a few more times. Each entry adds executionCounterIncrementForEntry
// to the counter that setup armed, and a handful stays far below optimizeAfterWarmUp's threshold, so no DFG compile
// starts and the counter changes only when the test runs the body.
static CodeBlock* warmBaselineElement(TestContext& context, JSGlobalObject* globalObject)
{
    constexpr unsigned baselineEntries = 3;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    if (!element || !array)
        return nullptr;
    CodeBlock* codeBlock = bringToBaseline(context, globalObject, element, { *array, jsNumber(0) });
    if (!codeBlock)
        return nullptr;
    for (unsigned entry = 0; entry < baselineEntries; ++entry) {
        if (!callFunction(context, globalObject, element, { *array, jsNumber(entry % 3) }))
            return nullptr;
    }
    if (codeBlock->jitType() != JITType::BaselineJIT || element->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("the baseline CB stopped being its executable's code while the test warmed it"_s);
        return nullptr;
    }
    return codeBlock;
}

// The baseline counter's three fields, read as capture reads them.
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

// Section 4.3's counterProgress of a carried triple, computed here from the formula.
static uint32_t expectedProgress(const CounterTriple& triple)
{
    double progress = static_cast<double>(triple.totalCount) + triple.value;
    if (progress <= 0)
        return 0;
    return static_cast<uint32_t>(std::min(std::floor(progress), static_cast<double>(triple.activeThreshold)));
}

template<typename Header>
static std::optional<Header> sectionHeader(TestContext& context, std::span<const uint8_t> section, ASCIILiteral name)
{
    if (section.size() < sizeof(Header)) {
        JITCACHE_FAIL(makeString(name, " is shorter than its header"_s));
        return std::nullopt;
    }
    Header header { };
    memcpySpan(asMutableByteSpan(header), section.first(sizeof(Header)));
    return header;
}

static void failWith(TestContext& context, ASCIILiteral step, const CBFault& fault)
{
    JITCACHE_FAIL(makeString(step, " failed with "_s, fault.kind == CBFaultKind::RecordingFault ? "a recording fault"_s : "invalid material"_s, ": "_s, description(fault.check)));
}

static constexpr uint8_t notCarried = static_cast<uint8_t>(CBFormat::CounterMode::NotCarried);
static constexpr uint8_t carried = static_cast<uint8_t>(CBFormat::CounterMode::Carried);

} // namespace CBCaptureTestsInternal

using namespace CBCaptureTestsInternal;

// U11 (I16): on one baseline CB whose counter has made progress, a strict capture with the polymorphic bit set writes
// NotCarried in both sections, zeros in the three counter fields and no progress, and its score and scoreLive's withhold
// the counter; without the bit, the capture writes Carried in both sections with the live triple, and both scores carry
// that triple's progress, at least one point. Neither call changes the counter.
JITCACHE_TEST(cbCapturePolymorphicBitDecidesTheCounter, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return;
    CodeBlock* codeBlock = warmBaselineElement(context, globalObject);
    if (!codeBlock)
        return;

    CounterTriple live = liveCounter(*codeBlock);
    uint32_t progress = expectedProgress(live);
    if (progress < 1) {
        JITCACHE_FAIL(makeString("the baseline counter made no point of progress: counter "_s, live.value, ", total "_s, live.totalCount, ", threshold "_s, live.activeThreshold));
        return;
    }

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    for (bool hasPolymorphicSite : { true, false }) {
        auto liveScore = CBStateCapture::scoreLive(*codeBlock, true, hasPolymorphicSite);
        if (!liveScore) {
            failWith(context, "scoreLive"_s, liveScore.error());
            continue;
        }
        auto captured = CBStateCapture::capture(*codeBlock, budget.get(), true, hasPolymorphicSite);
        if (!captured) {
            failWith(context, "capture"_s, captured.error());
            continue;
        }
        auto stateHeader = sectionHeader<CBFormat::StateHeader>(context, captured->stateSection(), "cb.state"_s);
        auto summaryHeader = sectionHeader<CBFormat::SummaryHeader>(context, captured->summarySection(), "cb.summary"_s);
        if (!stateHeader || !summaryHeader)
            continue;
        const CBScore& score = captured->score();

        if (hasPolymorphicSite) {
            JITCACHE_CHECK(stateHeader->counterMode == notCarried);
            JITCACHE_CHECK(summaryHeader->counterMode == notCarried);
            JITCACHE_CHECK(!stateHeader->counterValue);
            JITCACHE_CHECK(!stateHeader->counterTotalCount);
            JITCACHE_CHECK(!stateHeader->counterActiveThreshold);
            JITCACHE_CHECK(!summaryHeader->counterProgress);
            JITCACHE_CHECK(score.counterWithheld);
            JITCACHE_CHECK(!score.counterProgress);
            JITCACHE_CHECK(liveScore->counterWithheld);
            JITCACHE_CHECK(!liveScore->counterProgress);
        } else {
            JITCACHE_CHECK(stateHeader->counterMode == carried);
            JITCACHE_CHECK(summaryHeader->counterMode == carried);
            JITCACHE_CHECK(stateHeader->counterValue == live.value);
            JITCACHE_CHECK(stateHeader->counterTotalCount == live.totalCount);
            JITCACHE_CHECK(stateHeader->counterActiveThreshold == live.activeThreshold);
            JITCACHE_CHECK(summaryHeader->counterProgress == progress);
            JITCACHE_CHECK(!score.counterWithheld);
            JITCACHE_CHECK(score.counterProgress == progress);
            JITCACHE_CHECK(!liveScore->counterWithheld);
            JITCACHE_CHECK(liveScore->counterProgress == progress);
        }
    }

    JITCACHE_CHECK(liveCounter(*codeBlock) == live);
}

// Section 4.4, steps 3 and 6: while a capture lives, the budget holds exactly its two section sizes, which are the sizes
// the format computes from their headers; a move hands the sections and the charge to the new object, leaving the old
// one empty and releasing nothing; the last owner's destruction releases the charge. A budget too small for the two
// sections refuses the charge, and capture returns a CaptureCharge recording fault with nothing charged.
JITCACHE_TEST(cbCaptureOwnsItsSectionsAndCharge, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return;
    CodeBlock* codeBlock = warmBaselineElement(context, globalObject);
    if (!codeBlock)
        return;

    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    std::optional<CBStateCapture> owner;
    size_t stateSize = 0;
    size_t summarySize = 0;
    {
        auto captured = CBStateCapture::capture(*codeBlock, budget.get(), true, false);
        if (!captured) {
            failWith(context, "capture"_s, captured.error());
            return;
        }
        std::span<const uint8_t> state = captured->stateSection();
        std::span<const uint8_t> summary = captured->summarySection();
        stateSize = state.size();
        summarySize = summary.size();
        auto stateHeader = sectionHeader<CBFormat::StateHeader>(context, state, "cb.state"_s);
        auto summaryHeader = sectionHeader<CBFormat::SummaryHeader>(context, summary, "cb.summary"_s);
        if (!stateHeader || !summaryHeader)
            return;
        JITCACHE_CHECK(CBFormat::stateSectionSize(*stateHeader) == stateSize);
        JITCACHE_CHECK(CBFormat::summarySectionSize(*summaryHeader) == summarySize);
        JITCACHE_CHECK(budget->chargedBytes() == stateSize + summarySize);

        CBScore score = captured->score();
        owner.emplace(WTF::move(*captured));
        JITCACHE_CHECK(captured->stateSection().empty());
        JITCACHE_CHECK(captured->summarySection().empty());
        JITCACHE_CHECK(owner->stateSection().data() == state.data() && owner->stateSection().size() == stateSize);
        JITCACHE_CHECK(owner->summarySection().data() == summary.data() && owner->summarySection().size() == summarySize);
        JITCACHE_CHECK(owner->score().richnessUnits == score.richnessUnits);
        JITCACHE_CHECK(owner->score().counterWithheld == score.counterWithheld);
        JITCACHE_CHECK(owner->score().counterProgress == score.counterProgress);
    }
    JITCACHE_CHECK(budget->chargedBytes() == stateSize + summarySize);
    owner.reset();
    JITCACHE_CHECK(!budget->chargedBytes());

    Ref<ProducerBudget> smallBudget = ProducerBudget::create(sizeof(CBFormat::StateHeader));
    auto refused = CBStateCapture::capture(*codeBlock, smallBudget.get(), true, false);
    JITCACHE_CHECK(!refused);
    if (!refused) {
        JITCACHE_CHECK(refused.error().kind == CBFaultKind::RecordingFault);
        JITCACHE_CHECK(refused.error().check == CBCheck::CaptureCharge);
    }
    JITCACHE_CHECK(!smallBudget->chargedBytes());
    JITCACHE_CHECK(smallBudget->hasRefused());
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
