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
#include "CallData.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "ICCapture.h"
#include "ICSection.h"
#include "ICSites.h"
#include "JIT.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "PolymorphicCallStubRoutine.h"
#include "PropertyInlineCache.h"
#include "SourceCode.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The live C++ tests of SPEC-ics.md section 11.2 (T11 to T14), each on a fresh VM with default options. A test builds its
// own global object, evaluates its source with JSC::evaluate and reads its functions from the global object. A baseline
// CB comes from one call through JSC::call, which installs an LLInt CB, and JIT::compileSync, which compiles that CB and
// runs door 1's finalization; the later calls run its baseline code. A newborn CB comes from newCodeBlockFor on a
// function never called, inside a DeferGCForAWhile, and is used only inside that scope. The source keeps every object
// and function the test passes in its globals, and the global object stays reachable from the test's stack, so no
// collection resets a case or unlinks a callee between two reads of the same state.

namespace JSC::JITCache::Tests {

namespace ICLiveTestsInternal {

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
    auto* function = jsDynamicCast<JSFunction*>(globalValue(globalObject, name));
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

// The function's baseline CB, made as the file comment says: the call installs the LLInt CB, and compileSync compiles
// and installs baseline code in that same CB.
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

// Calls functor(CodeBlock&) with the newborn CB of a function never called: linked by finishCreation, never installed,
// never run, and used only inside the deferral scope that created it.
template<typename Functor>
static void withNewbornCodeBlock(TestContext& context, JSFunction* function, const Functor& functor)
{
    VM& vm = function->vm();
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    if (function->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("the function for a newborn CB was already called"_s);
        return;
    }
    CodeBlock* codeBlock = function->jsExecutable()->newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope());
    if (scope.exception()) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor threw"_s);
        return;
    }
    if (!codeBlock || codeBlock->jitType() != JITType::None) {
        JITCACHE_FAIL("newCodeBlockFor gave no newborn CB"_s);
        return;
    }
    functor(*codeBlock);
}

static ASCIILiteral captureCheckName(ICs::CaptureCheck check)
{
    switch (check) {
    case ICs::CaptureCheck::NoBaselineJITData:
        return "NoBaselineJITData"_s;
    case ICs::CaptureCheck::OutputSizeMismatch:
        return "OutputSizeMismatch"_s;
    case ICs::CaptureCheck::MoldMismatch:
        return "MoldMismatch"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// A strict capture of a live CB, with its section decoded into copies.
struct LiveCapture {
    Vector<uint8_t> bytes;
    ICs::CaptureSummary summary;
    ICs::SectionHeader header;
    Vector<ICs::CallLinkGroup> groups;
    Vector<ICs::PropertyICRecord> propertyICs;
    Vector<ICs::CallLinkRecord> callLinks;
};

// Summarizes and then captures the CB with strict on, in one pause as the integrator's scoring and capture glue do, and
// checks that the two return the same summary and polymorphic bit (section 5.6) and that the bytes pass every structural
// check of a strict import with the returned count in their header. The buffer starts filled with a nonzero byte, so
// padding, reserved bytes or unused bits the capture left unwritten fail those checks.
static std::optional<LiveCapture> captureLive(TestContext& context, CodeBlock& codeBlock)
{
    ICs::CaptureSummary summarized = ICs::summarizeBaselineICs(codeBlock);
    Vector<uint8_t> bytes(FillWith { }, ICs::baselineICsSectionSize(codeBlock), 0xcc);
    auto captured = ICs::captureBaselineICs(codeBlock, bytes.mutableSpan(), ICs::StrictChecks::Yes);
    if (!captured) {
        JITCACHE_FAIL(makeString("captureBaselineICs failed "_s, captureCheckName(captured.error().check), " at site "_s, captured.error().siteIndex));
        return std::nullopt;
    }
    if (captured->hasPolymorphicSite != summarized.hasPolymorphicSite)
        JITCACHE_FAIL(makeString("summarizeBaselineICs and captureBaselineICs disagree on hasPolymorphicSite: "_s, static_cast<unsigned>(summarized.hasPolymorphicSite), " and "_s, static_cast<unsigned>(captured->hasPolymorphicSite)));
    if (captured->summary.icSitesWithCases != summarized.summary.icSitesWithCases)
        JITCACHE_FAIL(makeString("summarizeBaselineICs and captureBaselineICs disagree on icSitesWithCases: "_s, summarized.summary.icSitesWithCases, " and "_s, captured->summary.icSitesWithCases));

    auto view = ICs::parseSection(bytes.span(), ICs::StrictChecks::Yes);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured section fails strict check "_s, static_cast<unsigned>(view.error().check), " at site "_s, view.error().siteIndex));
        return std::nullopt;
    }
    JITCACHE_CHECK(view->header.icSitesWithCases == captured->summary.icSitesWithCases);

    LiveCapture capture {
        .bytes = { },
        .summary = *captured,
        .header = view->header,
        .groups = { },
        .propertyICs = { },
        .callLinks = { },
    };
    for (unsigned index = 0; index < view->header.callLinkGroupCount; ++index)
        capture.groups.append(view->group(index));
    for (auto& record : view->propertyICs)
        capture.propertyICs.append(record);
    for (auto& record : view->callLinks)
        capture.callLinks.append(record);
    capture.bytes = WTF::move(bytes);
    return capture;
}

static bool hasBit(uint8_t bits, uint8_t bit)
{
    return bits & bit;
}

static ICs::CallLinkModeCode modeOf(const ICs::CallLinkRecord& record)
{
    return static_cast<ICs::CallLinkModeCode>(record.bits & ICs::CallLinkBit::modeMask);
}

// The slots of the stubs at the CB's call-link sites, counted under its lock as capture counts them.
static unsigned stubSlotCount(CodeBlock& codeBlock)
{
    ConcurrentJSLocker locker(codeBlock.m_lock);
    unsigned slotCount = 0;
    ICs::forEachCallLinkSite(codeBlock, [&](unsigned, unsigned, CallLinkInfo& callLinkInfo) {
        if (auto* stub = callLinkInfo.stub())
            stub->forEachDependentCell([&](JSCell*) { ++slotCount; });
    });
    return slotCount;
}

// Calls the function with first, then second, and so on in turn, until the record of its only call-link site satisfies
// done or eight calls have run. Every step summarizes and captures, which checks that the two agree (T14).
template<typename Done>
static std::optional<LiveCapture> driveCallSiteUntil(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, CodeBlock& codeBlock, JSValue first, JSValue second, const Done& done)
{
    constexpr unsigned maximumCalls = 8;
    for (unsigned callIndex = 0; callIndex < maximumCalls; ++callIndex) {
        JSValue argument = callIndex % 2 ? second : first;
        if (!callFunction(context, globalObject, function, { argument }))
            return std::nullopt;
        auto capture = captureLive(context, codeBlock);
        if (!capture)
            return std::nullopt;
        if (capture->callLinks.size() != 1) {
            JITCACHE_FAIL(makeString("the body has "_s, capture->callLinks.size(), " call-link sites instead of one"_s));
            return std::nullopt;
        }
        if (done(capture->callLinks[0]))
            return capture;
    }
    JITCACHE_FAIL("the call-link site never reached the state the test drives it to"_s);
    return std::nullopt;
}

static constexpr auto hasSource = "function has(o) { return \"x\" in o; }"
    "var a1 = { x: 1 };"
    "var a2 = { x: 2 };"
    "var b = { y: 1, x: 2 };"_s;

} // namespace ICLiveTestsInternal

using namespace ICLiveTestsInternal;

// T12 item 1, capture: a body whose bytecode adds no metadata entry and no value profile (enter and ret) has no metadata
// table, so its section is a bare header with no group and no record.
JITCACHE_TEST(icsLiveCaptureWithoutMetadataTable, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, "function id(x) { return x; }"_s);
    if (!globalObject)
        return;
    JSFunction* id = globalFunction(context, globalObject, "id"_s);
    if (!id)
        return;

    withNewbornCodeBlock(context, id, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(!newborn.metadataTable());
        JITCACHE_CHECK(ICs::callLinkSiteCounts(newborn) == ICs::CallLinkSiteCounts { });
    });

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, id, { jsNumber(1) });
    if (!codeBlock)
        return;
    JITCACHE_CHECK(!codeBlock->metadataTable());
    JITCACHE_CHECK(ICs::callLinkSiteCounts(*codeBlock) == ICs::CallLinkSiteCounts { });
    JITCACHE_CHECK(ICs::baselineICsSectionSize(*codeBlock) == ICs::sectionHeaderSize);

    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    JITCACHE_CHECK(capture->bytes.size() == ICs::sectionHeaderSize);
    JITCACHE_CHECK(!capture->header.propertyICCount);
    JITCACHE_CHECK(!capture->header.icSitesWithCases);
    JITCACHE_CHECK(!capture->header.callLinkGroupCount);
    JITCACHE_CHECK(!capture->header.callLinkSiteCount);
    JITCACHE_CHECK(capture->groups.isEmpty());
    JITCACHE_CHECK(capture->propertyICs.isEmpty());
    JITCACHE_CHECK(capture->callLinks.isEmpty());
    JITCACHE_CHECK(!capture->summary.summary.icSitesWithCases);
    JITCACHE_CHECK(!capture->summary.hasPolymorphicSite);
}

// T12 item 2: in_by_id carries no metadata, so the body has no metadata table, yet its property IC lives in
// BaselineJITData and its capture holds the IC's record. The first baseline visit only spends countdown, which starts at
// 1, so of the three calls the second caches shape A and the third shape B.
JITCACHE_TEST(icsLiveCaptureInByIdWithoutMetadataTable, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, hasSource);
    if (!globalObject)
        return;
    JSFunction* has = globalFunction(context, globalObject, "has"_s);
    auto a1 = globalObjectValue(context, globalObject, "a1"_s);
    auto a2 = globalObjectValue(context, globalObject, "a2"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    if (!has || !a1 || !a2 || !b)
        return;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, has, { *a1 });
    if (!codeBlock)
        return;
    for (JSValue argument : { *a1, *a2, *b }) {
        if (!callFunction(context, globalObject, has, { argument }))
            return;
    }

    JITCACHE_CHECK(!codeBlock->metadataTable());
    JITCACHE_CHECK(ICs::baselineICsSectionSize(*codeBlock) == ICs::sectionHeaderSize + sizeof(ICs::PropertyICRecord));
    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    JITCACHE_CHECK(capture->groups.isEmpty());
    JITCACHE_CHECK(capture->callLinks.isEmpty());
    if (capture->propertyICs.size() != 1) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture->propertyICs.size(), " property-IC records instead of one"_s));
        return;
    }
    const ICs::PropertyICRecord& record = capture->propertyICs[0];
    JITCACHE_CHECK(record.accessType == static_cast<uint8_t>(AccessType::InById));
    JITCACHE_CHECK(record.caseCount == 2);
    JITCACHE_CHECK(hasBit(record.learningBits, ICs::LearningBit::everConsidered));
    JITCACHE_CHECK(capture->summary.summary.icSitesWithCases == 1);
}

// Section 5.5: with strict on, capture refuses a CB without BaselineJITData and an output of another size than the
// section's, before it writes a byte.
JITCACHE_TEST(icsLiveCaptureStrictChecksTheCall, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, hasSource);
    if (!globalObject)
        return;
    JSFunction* has = globalFunction(context, globalObject, "has"_s);
    auto a1 = globalObjectValue(context, globalObject, "a1"_s);
    if (!has || !a1)
        return;

    auto untouched = [](const Vector<uint8_t>& bytes) {
        return std::ranges::all_of(bytes, [](uint8_t byte) { return byte == 0xcc; });
    };

    if (!callFunction(context, globalObject, has, { *a1 }))
        return;
    CodeBlock* llintCodeBlock = has->jsExecutable()->codeBlockForCall();
    if (!llintCodeBlock || llintCodeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not install an LLInt CB"_s);
        return;
    }
    Vector<uint8_t> headerOnly(FillWith { }, ICs::sectionHeaderSize, 0xcc);
    auto withoutJITData = ICs::captureBaselineICs(*llintCodeBlock, headerOnly.mutableSpan(), ICs::StrictChecks::Yes);
    JITCACHE_CHECK(!withoutJITData && withoutJITData.error().check == ICs::CaptureCheck::NoBaselineJITData);
    JITCACHE_CHECK(untouched(headerOnly));

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, has, { *a1 });
    if (!codeBlock)
        return;
    JITCACHE_CHECK(codeBlock == llintCodeBlock);
    size_t size = ICs::baselineICsSectionSize(*codeBlock);
    for (size_t wrongSize : { size - 1, size + 1 }) {
        Vector<uint8_t> bytes(FillWith { }, wrongSize, 0xcc);
        auto result = ICs::captureBaselineICs(*codeBlock, bytes.mutableSpan(), ICs::StrictChecks::Yes);
        JITCACHE_CHECK(!result && result.error().check == ICs::CaptureCheck::OutputSizeMismatch);
        JITCACHE_CHECK(untouched(bytes));
    }
}

// T14 item 1: a property IC that lists two cases, none megamorphic, sets the polymorphic bit.
JITCACHE_TEST(icsLivePolymorphicBitPropertyIC, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, hasSource);
    if (!globalObject)
        return;
    JSFunction* has = globalFunction(context, globalObject, "has"_s);
    auto a1 = globalObjectValue(context, globalObject, "a1"_s);
    auto a2 = globalObjectValue(context, globalObject, "a2"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    if (!has || !a1 || !a2 || !b)
        return;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, has, { *a1 });
    if (!codeBlock)
        return;

    struct Step {
        JSValue argument;
        uint8_t caseCount;
        bool hasPolymorphicSite;
    };
    for (const Step& step : { Step { *a1, 0, false }, Step { *a2, 1, false }, Step { *b, 2, true } }) {
        if (!callFunction(context, globalObject, has, { step.argument }))
            return;
        auto capture = captureLive(context, *codeBlock);
        if (!capture)
            return;
        if (capture->propertyICs.size() != 1) {
            JITCACHE_FAIL(makeString("the capture holds "_s, capture->propertyICs.size(), " property-IC records instead of one"_s));
            return;
        }
        const ICs::PropertyICRecord& record = capture->propertyICs[0];
        if (record.caseCount != step.caseCount)
            JITCACHE_FAIL(makeString("the IC lists "_s, static_cast<unsigned>(record.caseCount), " cases instead of "_s, static_cast<unsigned>(step.caseCount)));
        JITCACHE_CHECK(!hasBit(record.stateBits, ICs::StateBit::megamorphicCaseListed));
        JITCACHE_CHECK(ICs::isPolymorphicPropertyIC(record) == step.hasPolymorphicSite);
        if (capture->summary.hasPolymorphicSite != step.hasPolymorphicSite)
            JITCACHE_FAIL(makeString("with "_s, static_cast<unsigned>(record.caseCount), " cases listed, hasPolymorphicSite is "_s, static_cast<unsigned>(capture->summary.hasPolymorphicSite)));
    }
}

// T14 item 2: a get_by_id site that folds into one megamorphic case after caching plain loads of distinct shapes does
// not set the polymorphic bit, although its record listed two or more cases on the way.
JITCACHE_TEST(icsLivePolymorphicBitMegamorphicFold, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function getX(o) { return o.x; }"
        "var shapes = ["
        "    { x: 0 }, { x: 0, a: 0 }, { x: 0, b: 0 }, { x: 0, c: 0 }, { x: 0, d: 0 }, { x: 0, e: 0 }, { x: 0, f: 0 }, { x: 0, g: 0 },"
        "    { x: 0, h: 0 }, { x: 0, i: 0 }, { x: 0, j: 0 }, { x: 0, k: 0 }, { x: 0, l: 0 }, { x: 0, m: 0 }, { x: 0, n: 0 }, { x: 0, o: 0 }"
        "];"_s);
    if (!globalObject)
        return;
    JSFunction* getX = globalFunction(context, globalObject, "getX"_s);
    auto shapes = globalObjectValue(context, globalObject, "shapes"_s);
    if (!getX || !shapes)
        return;
    constexpr unsigned shapeCount = 16;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, getX, { shapes->get(globalObject, 0u) });
    if (!codeBlock)
        return;

    std::optional<LiveCapture> folded;
    for (unsigned index = 0; index < shapeCount && !folded; ++index) {
        JSValue shape = shapes->get(globalObject, index);
        if (!shape.isObject()) {
            JITCACHE_FAIL(makeString("shapes["_s, index, "] is not an object"_s));
            return;
        }
        if (!callFunction(context, globalObject, getX, { shape }))
            return;
        auto capture = captureLive(context, *codeBlock);
        if (!capture)
            return;
        if (capture->propertyICs.size() != 1) {
            JITCACHE_FAIL(makeString("the capture holds "_s, capture->propertyICs.size(), " property-IC records instead of one"_s));
            return;
        }
        if (hasBit(capture->propertyICs[0].stateBits, ICs::StateBit::megamorphicCaseListed))
            folded = WTF::move(capture);
    }
    if (!folded) {
        JITCACHE_FAIL(makeString("the get_by_id site did not fold after "_s, shapeCount, " distinct shapes"_s));
        return;
    }
    const ICs::PropertyICRecord& record = folded->propertyICs[0];
    JITCACHE_CHECK(record.accessType == static_cast<uint8_t>(AccessType::GetById));
    JITCACHE_CHECK(record.caseCount == 1);
    JITCACHE_CHECK(!ICs::isPolymorphicPropertyIC(record));
    JITCACHE_CHECK(!folded->summary.hasPolymorphicSite);
}

// T14 item 3: a Polymorphic call site whose stub holds two callees of different executables sets the polymorphic bit;
// one whose stub holds closures of one executable, merged into one despecified slot with hasSeenClosure, does not.
JITCACHE_TEST(icsLivePolymorphicBitCallSites, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function callIt(f) { return f(); }"
        "function callClosures(f) { return f(); }"
        "function one() { return 1; }"
        "function two() { return 2; }"
        "function makeClosure(k) { return function () { return k; }; }"
        "var closureA = makeClosure(1);"
        "var closureB = makeClosure(2);"_s);
    if (!globalObject)
        return;
    JSFunction* callIt = globalFunction(context, globalObject, "callIt"_s);
    JSFunction* callClosures = globalFunction(context, globalObject, "callClosures"_s);
    JSFunction* one = globalFunction(context, globalObject, "one"_s);
    JSFunction* two = globalFunction(context, globalObject, "two"_s);
    JSFunction* closureA = globalFunction(context, globalObject, "closureA"_s);
    JSFunction* closureB = globalFunction(context, globalObject, "closureB"_s);
    if (!callIt || !callClosures || !one || !two || !closureA || !closureB)
        return;
    JITCACHE_CHECK(closureA->executable() == closureB->executable());

    auto isPolymorphic = [](const ICs::CallLinkRecord& record) {
        return modeOf(record) == ICs::CallLinkModeCode::Polymorphic;
    };

    // The LLInt call marks the site seen, the first baseline call links it to one and a call with two makes it
    // polymorphic over both.
    if (CodeBlock* codeBlock = bringToBaseline(context, globalObject, callIt, { one })) {
        if (auto capture = driveCallSiteUntil(context, globalObject, callIt, *codeBlock, one, two, isPolymorphic)) {
            const ICs::CallLinkRecord& record = capture->callLinks[0];
            JITCACHE_CHECK(!hasBit(record.bits, ICs::CallLinkBit::clearedByGC));
            JITCACHE_CHECK(!hasBit(record.bits, ICs::CallLinkBit::hasSeenClosure));
            JITCACHE_CHECK(stubSlotCount(*codeBlock) == 2);
            JITCACHE_CHECK(capture->summary.hasPolymorphicSite);
        }
    }

    // The second callee shares the first one's executable, so linkPolymorphicCall keys the stub by executable.
    if (CodeBlock* codeBlock = bringToBaseline(context, globalObject, callClosures, { closureA })) {
        if (auto capture = driveCallSiteUntil(context, globalObject, callClosures, *codeBlock, closureA, closureB, isPolymorphic)) {
            const ICs::CallLinkRecord& record = capture->callLinks[0];
            JITCACHE_CHECK(!hasBit(record.bits, ICs::CallLinkBit::clearedByGC));
            JITCACHE_CHECK(hasBit(record.bits, ICs::CallLinkBit::hasSeenClosure));
            JITCACHE_CHECK(stubSlotCount(*codeBlock) == 1);
            JITCACHE_CHECK(!capture->summary.hasPolymorphicSite);
        }
    }
}

// T14 item 4: a construct site that met a second constructor goes Virtual, which a status answers with a slow path, so
// it does not set the polymorphic bit.
JITCACHE_TEST(icsLivePolymorphicBitVirtualConstruct, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function make(C) { return new C(); }"
        "function C1() { }"
        "function C2() { }"_s);
    if (!globalObject)
        return;
    JSFunction* make = globalFunction(context, globalObject, "make"_s);
    JSFunction* c1 = globalFunction(context, globalObject, "C1"_s);
    JSFunction* c2 = globalFunction(context, globalObject, "C2"_s);
    if (!make || !c1 || !c2)
        return;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, make, { c1 });
    if (!codeBlock)
        return;
    auto capture = driveCallSiteUntil(context, globalObject, make, *codeBlock, c1, c2, [](const ICs::CallLinkRecord& record) {
        return modeOf(record) == ICs::CallLinkModeCode::Virtual;
    });
    if (!capture)
        return;
    JITCACHE_CHECK(hasBit(capture->callLinks[0].bits, ICs::CallLinkBit::clearedByVirtual));
    JITCACHE_CHECK(!capture->summary.hasPolymorphicSite);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
