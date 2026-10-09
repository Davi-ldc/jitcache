#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "BaselineJITCode.h"
#include "CallData.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "ConcurrentJSLock.h"
#include "DeferGC.h"
#include "FunctionExecutable.h"
#include "ICCapture.h"
#include "ICRestore.h"
#include "ICSection.h"
#include "ICSites.h"
#include "ICTwins.h"
#include "JIT.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "PolymorphicCallStubRoutine.h"
#include "PropertyInlineCache.h"
#include "Repatch.h"
#include "SourceCode.h"
#include "TopExceptionScope.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The live C++ tests of SPEC-ics.md section 11.2 (T11 to T14), of the restore steps of sections 6.2 to 6.4 and of the
// shell function of section 11.1, each on a fresh VM with default options. A test builds its own global object, evaluates its source with JSC::evaluate and reads
// its functions from the global object. A baseline CB comes from one call through JSC::call, which installs an LLInt CB,
// and JIT::compileSync, which compiles that CB and runs door 1's finalization; the later calls run its baseline code. A
// newborn CB comes from newCodeBlockFor on a function never called, inside a DeferGCForAWhile, and is used only inside
// that scope. Two functions with the same body text have separate UCBs with the same metadata layout and molds, so one's
// BaselineJITCode and section pair with the other's newborn CB. The source keeps every object and function the test
// passes in its globals, and the global object stays reachable from the test's stack, so no collection resets a case or
// unlinks a callee between two reads of the same state.

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

// hasSource with a twin of has that the restore tests never call, whose newborn CB pairs with has's code and section.
static constexpr auto hasWithTwinSource = "function has(o) { return \"x\" in o; }"
    "function hasTwin(o) { return \"x\" in o; }"
    "var a1 = { x: 1 };"
    "var a2 = { x: 2 };"
    "var b = { y: 1, x: 2 };"_s;

static ASCIILiteral checkName(ICs::Check check)
{
    switch (check) {
    case ICs::Check::SectionSize:
        return "SectionSize"_s;
    case ICs::Check::SummaryBound:
        return "SummaryBound"_s;
    case ICs::Check::CallLinkGroups:
        return "CallLinkGroups"_s;
    case ICs::Check::ReservedBits:
        return "ReservedBits"_s;
    case ICs::Check::EnumRange:
        return "EnumRange"_s;
    case ICs::Check::SummaryCount:
        return "SummaryCount"_s;
    case ICs::Check::MoldPairing:
        return "MoldPairing"_s;
    case ICs::Check::MetadataLayout:
        return "MetadataLayout"_s;
    case ICs::Check::MoldMegamorphicBit:
        return "MoldMegamorphicBit"_s;
    case ICs::Check::NewbornCallLinks:
        return "NewbornCallLinks"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String describe(const ICs::Invalid& invalid)
{
    return makeString(checkName(invalid.check), " at site "_s, invalid.siteIndex);
}

static void expectPasses(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& invalid)
{
    if (invalid)
        JITCACHE_FAIL(makeString(what, " failed "_s, describe(*invalid)));
}

static void expectFails(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& invalid, ICs::Check check, uint32_t siteIndex)
{
    if (!invalid) {
        JITCACHE_FAIL(makeString(what, " passed, expected "_s, checkName(check), " at site "_s, siteIndex));
        return;
    }
    if (invalid->check != check || invalid->siteIndex != siteIndex)
        JITCACHE_FAIL(makeString(what, " failed "_s, describe(*invalid), ", expected "_s, checkName(check), " at site "_s, siteIndex));
}

static unsigned totalSites(const ICs::CallLinkSiteCounts& counts)
{
    unsigned total = 0;
    for (uint32_t count : counts)
        total += count;
    return total;
}

// The code a baseline CB runs, held by the test so that it outlives anything done to the CB.
static Ref<BaselineJITCode> baselineCodeOf(CodeBlock& codeBlock)
{
    RefPtr jitCode = codeBlock.jitCode();
    RELEASE_ASSERT(jitCode && jitCode->jitType() == JITType::BaselineJIT);
    return Ref { static_cast<BaselineJITCode&>(*jitCode) };
}

// The body T11 and T13 warm: a property load, a store and an in, a plain call, a varargs call and a construct. Its twin,
// never called, has the same metadata layout and molds.
static constexpr auto warmSource = "function warm(o, f, C, args) { var x = o.p; o.q = x; f(); f(...args); new C(); return \"p\" in o; }"
    "function warmTwin(o, f, C, args) { var x = o.p; o.q = x; f(); f(...args); new C(); return \"p\" in o; }"
    "function callee() { return 1; }"
    "function C1() { }"
    "function C2() { }"
    "var a = { p: 1, q: 0 };"
    "var b = { r: 1, p: 2, q: 0 };"
    "var args1 = [1];"
    "var args2 = [1, 2, 3];"_s;

struct WarmBody {
    JSGlobalObject* globalObject;
    JSFunction* twin;
    CodeBlock* codeBlock;
};

// Brings warm to baseline and runs it six more times, alternating two shapes, two constructors and two argument lists:
// its property ICs list cases, its plain call links to one callee, its construct site goes virtual and its varargs site
// records a maximum. The source's globals keep every structure and callee alive.
static std::optional<WarmBody> warmUp(TestContext& context, VM& vm)
{
    JSGlobalObject* globalObject = createRealm(context, vm, warmSource);
    if (!globalObject)
        return std::nullopt;
    JSFunction* warm = globalFunction(context, globalObject, "warm"_s);
    JSFunction* twin = globalFunction(context, globalObject, "warmTwin"_s);
    JSFunction* callee = globalFunction(context, globalObject, "callee"_s);
    JSFunction* c1 = globalFunction(context, globalObject, "C1"_s);
    JSFunction* c2 = globalFunction(context, globalObject, "C2"_s);
    auto a = globalObjectValue(context, globalObject, "a"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    auto args1 = globalObjectValue(context, globalObject, "args1"_s);
    auto args2 = globalObjectValue(context, globalObject, "args2"_s);
    if (!warm || !twin || !callee || !c1 || !c2 || !a || !b || !args1 || !args2)
        return std::nullopt;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, warm, { *a, callee, c1, *args1 });
    if (!codeBlock)
        return std::nullopt;
    for (unsigned callIndex = 0; callIndex < 6; ++callIndex) {
        bool odd = callIndex % 2;
        if (!callFunction(context, globalObject, warm, { odd ? *b : *a, callee, odd ? c2 : c1, odd ? *args2 : *args1 }))
            return std::nullopt;
    }
    return WarmBody { globalObject, twin, codeBlock };
}

// Fails unless the capture holds what warmUp drives: a property IC with cases, a call-link site past Init and a varargs
// maximum, so the tests that use it compare warm state.
static bool expectWarm(TestContext& context, const LiveCapture& capture)
{
    bool hasCases = std::ranges::any_of(capture.propertyICs, [](const ICs::PropertyICRecord& record) {
        return record.caseCount > 0;
    });
    bool hasLinkedSite = std::ranges::any_of(capture.callLinks, [](const ICs::CallLinkRecord& record) {
        return modeOf(record) != ICs::CallLinkModeCode::Init;
    });
    bool hasVarargsMaximum = std::ranges::any_of(capture.callLinks, [](const ICs::CallLinkRecord& record) {
        return record.maxArgumentCountIncludingThisForVarargs > 0;
    });
    if (hasCases && hasLinkedSite && hasVarargsMaximum)
        return true;
    JITCACHE_FAIL("the warm body's capture lacks a property IC with cases, a linked call-link site or a varargs maximum"_s);
    return false;
}

static ICs::CallLinkModeCode modeCodeOf(CallLinkInfo::Mode mode)
{
    switch (mode) {
    case CallLinkInfo::Mode::Init:
        return ICs::CallLinkModeCode::Init;
    case CallLinkInfo::Mode::Monomorphic:
        return ICs::CallLinkModeCode::Monomorphic;
    case CallLinkInfo::Mode::Polymorphic:
        return ICs::CallLinkModeCode::Polymorphic;
    case CallLinkInfo::Mode::Virtual:
        return ICs::CallLinkModeCode::Virtual;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Element>
static std::optional<size_t> firstDifference(const Vector<Element>& expected, const Vector<Element>& actual)
{
    size_t count = std::min(expected.size(), actual.size());
    for (size_t index = 0; index < count; ++index) {
        if (!(expected[index] == actual[index]))
            return index;
    }
    if (expected.size() != actual.size())
        return count;
    return std::nullopt;
}

static void expectSameSnapshot(TestContext& context, ASCIILiteral what, const ICs::BaselineICsSnapshot& expected, const ICs::BaselineICsSnapshot& actual)
{
    if (expected == actual)
        return;
    if (expected.jitType != actual.jitType)
        JITCACHE_FAIL(makeString(what, ": the JIT type differs"_s));
    if (auto index = firstDifference(expected.propertyICs, actual.propertyICs))
        JITCACHE_FAIL(makeString(what, ": property IC "_s, *index, " differs"_s));
    if (auto index = firstDifference(expected.callLinks, actual.callLinks))
        JITCACHE_FAIL(makeString(what, ": call-link site "_s, *index, " differs"_s));
    if (auto index = firstDifference(expected.superConstructs, actual.superConstructs))
        JITCACHE_FAIL(makeString(what, ": super_construct cache "_s, *index, " differs"_s));
}

static ASCIILiteral siteName(ICs::TwinMismatch::Site site)
{
    switch (site) {
    case ICs::TwinMismatch::Site::PropertyIC:
        return "PropertyIC"_s;
    case ICs::TwinMismatch::Site::CallLink:
        return "CallLink"_s;
    case ICs::TwinMismatch::Site::Capture:
        return "Capture"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static String describe(const ICs::TwinMismatch& mismatch)
{
    return makeString(siteName(mismatch.site), ' ', mismatch.index, ' ', mismatch.field, ": expected "_s, mismatch.expected, ", actual "_s, mismatch.actual);
}

// Evaluates a script in the realm that returns the empty string when its checks pass and a description of the first
// failure otherwise.
static void expectScriptPasses(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral source)
{
    VM& vm = globalObject->vm();
    NakedPtr<Exception> exception;
    JSValue result = evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("the checking script threw"_s);
        return;
    }
    if (!result.isString()) {
        JITCACHE_FAIL("the checking script returned no string"_s);
        return;
    }
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    String failure = result.toWTFString(globalObject);
    if (scope.exception()) {
        scope.clearException();
        JITCACHE_FAIL("reading the checking script's result threw"_s);
        return;
    }
    if (!failure.isEmpty())
        JITCACHE_FAIL(failure);
}

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

// T12 item 1, restore: the newborn CB of a body without a metadata table passes S2, and the body's section, a bare
// header, prepares with strict on against the body's code and the newborn CB of a never-called twin, whose seeding
// visits no site.
JITCACHE_TEST(icsLiveRestoreWithoutMetadataTable, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function id(x) { return x; }"
        "function idTwin(x) { return x; }"_s);
    if (!globalObject)
        return;
    JSFunction* id = globalFunction(context, globalObject, "id"_s);
    JSFunction* idTwin = globalFunction(context, globalObject, "idTwin"_s);
    if (!id || !idTwin)
        return;

    withNewbornCodeBlock(context, id, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(!newborn.metadataTable());
        expectPasses(context, "the newborn CB of id"_s, ICs::checkNewbornCodeBlock(newborn));
    });

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, id, { jsNumber(1) });
    if (!codeBlock)
        return;
    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    JITCACHE_CHECK(capture->bytes.size() == ICs::sectionHeaderSize);
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);
    JITCACHE_CHECK(!code->m_unlinkedPropertyInlineCaches.size());

    withNewbornCodeBlock(context, idTwin, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(!newborn.metadataTable());
        auto prepared = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!prepared) {
            JITCACHE_FAIL(makeString("prepareBaselineICs failed "_s, describe(prepared.error())));
            return;
        }
        JITCACHE_CHECK(prepared->propertyICs().empty());
        JITCACHE_CHECK(prepared->callLinks().empty());
        JITCACHE_CHECK(!prepared->summary().icSitesWithCases);

        unsigned visitedSites = 0;
        ICs::forEachCallLinkSite(newborn, [&](unsigned, unsigned, CallLinkInfo&) {
            ++visitedSites;
        });
        JITCACHE_CHECK(!visitedSites);
        ICs::seedCallLinkHistory(*prepared, newborn);
        JITCACHE_CHECK(newborn.jitType() == JITType::None);
        expectPasses(context, "the twin's newborn CB after seeding"_s, ICs::checkNewbornCodeBlock(newborn));
    });
}

// T12 item 3: a CB that already ran fails S2, and the newborn CB of a never-called function with the same body passes.
JITCACHE_TEST(icsLiveNewbornCheck, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function caller(g) { return g(); }"
        "function callerTwin(g) { return g(); }"
        "function callee() { return 1; }"_s);
    if (!globalObject)
        return;
    JSFunction* caller = globalFunction(context, globalObject, "caller"_s);
    JSFunction* callerTwin = globalFunction(context, globalObject, "callerTwin"_s);
    JSFunction* callee = globalFunction(context, globalObject, "callee"_s);
    if (!caller || !callerTwin || !callee)
        return;

    for (unsigned callIndex = 0; callIndex < 2; ++callIndex) {
        if (!callFunction(context, globalObject, caller, { callee }))
            return;
    }
    CodeBlock* codeBlock = caller->jsExecutable()->codeBlockForCall();
    if (!codeBlock) {
        JITCACHE_FAIL("calling caller installed no CB"_s);
        return;
    }
    auto invalid = ICs::checkNewbornCodeBlock(*codeBlock);
    if (!invalid || invalid->check != ICs::Check::NewbornCallLinks)
        JITCACHE_FAIL(makeString("the CB that ran gives "_s, invalid ? describe(*invalid) : String("no failure"_s), " instead of NewbornCallLinks"_s));

    withNewbornCodeBlock(context, callerTwin, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(totalSites(ICs::callLinkSiteCounts(newborn)) == 1);
        expectPasses(context, "the newborn CB of callerTwin"_s, ICs::checkNewbornCodeBlock(newborn));
    });
}

// S2 field by field: on newborn CBs of a never-called body with two call sites, each call-link field that seeding
// writes over, changed at the second site, fails the check at that site's index.
JITCACHE_TEST(icsLiveNewbornCheckPerSite, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, "function twoCalls(f, g) { return f() + g(); }"_s);
    if (!globalObject)
        return;
    JSFunction* twoCalls = globalFunction(context, globalObject, "twoCalls"_s);
    if (!twoCalls)
        return;

    withNewbornCodeBlock(context, twoCalls, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(totalSites(ICs::callLinkSiteCounts(newborn)) == 2);
        expectPasses(context, "the untouched newborn CB"_s, ICs::checkNewbornCodeBlock(newborn));
    });

    struct Change {
        ASCIILiteral name;
        void (*apply)(VM&, CallLinkInfo&);
    };
    const Change changes[] = {
        { "the seen bit"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setSeen(); } },
        { "hasSeenClosure"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setHasSeenClosure(); } },
        { "clearedByGC"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setClearedByGC(); } },
        { "clearedByVirtual"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setClearedByVirtual(); } },
        { "a varargs maximum"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.updateMaxArgumentCountIncludingThisForVarargs(3); } },
        { "the virtual mode"_s, [](VM& siteVM, CallLinkInfo& callLinkInfo) { callLinkInfo.setVirtualCall(siteVM); } },
        { "another call type"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setCallType(CallLinkInfo::TailCall); } },
    };
    for (const Change& change : changes) {
        withNewbornCodeBlock(context, twoCalls, [&](CodeBlock& newborn) {
            unsigned siteIndex = 0;
            ICs::forEachCallLinkSite(newborn, [&](unsigned, unsigned, CallLinkInfo& callLinkInfo) {
                if (siteIndex++ == 1)
                    change.apply(vm, callLinkInfo);
            });
            if (siteIndex != 2) {
                JITCACHE_FAIL(makeString("the newborn CB has "_s, siteIndex, " call-link sites instead of two"_s));
                return;
            }
            expectFails(context, change.name, ICs::checkNewbornCodeBlock(newborn), ICs::Check::NewbornCallLinks, 1);
        });
    }
}

// T12 item 4: has's section with its record's access type changed to GetById breaks A7 and no other check, so prepare,
// given has's code and the newborn CB of a never-called twin, fails with MoldPairing under strict and succeeds without,
// which checks nothing (section 4.5). The intact section prepares under strict.
JITCACHE_TEST(icsLiveRestoreMoldPairing, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, hasWithTwinSource);
    if (!globalObject)
        return;
    JSFunction* has = globalFunction(context, globalObject, "has"_s);
    JSFunction* hasTwin = globalFunction(context, globalObject, "hasTwin"_s);
    auto a1 = globalObjectValue(context, globalObject, "a1"_s);
    auto a2 = globalObjectValue(context, globalObject, "a2"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    if (!has || !hasTwin || !a1 || !a2 || !b)
        return;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, has, { *a1 });
    if (!codeBlock)
        return;
    for (JSValue argument : { *a1, *a2, *b }) {
        if (!callFunction(context, globalObject, has, { argument }))
            return;
    }
    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    if (capture->propertyICs.size() != 1 || capture->propertyICs[0].accessType != static_cast<uint8_t>(AccessType::InById)) {
        JITCACHE_FAIL("has's capture does not hold exactly one InById record"_s);
        return;
    }
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    // Record 0 follows the header and the groups. GetById is a valid access type and the summary counts cases, so the
    // changed bytes still pass A1 to A6, and has's molds carry no fold bit (S1).
    Vector<uint8_t> changed = capture->bytes;
    changed[ICs::sectionHeaderSize + ICs::callLinkGroupSize * capture->header.callLinkGroupCount] = static_cast<uint8_t>(AccessType::GetById);
    auto changedView = ICs::parseSection(changed.span(), ICs::StrictChecks::Yes);
    if (!changedView) {
        JITCACHE_FAIL(makeString("the changed section fails "_s, describe(changedView.error())));
        return;
    }
    JITCACHE_CHECK(changedView->propertyICs[0].accessType == static_cast<uint8_t>(AccessType::GetById));
    expectPasses(context, "has's molds"_s, ICs::checkMolds(code->m_unlinkedPropertyInlineCaches.span()));

    withNewbornCodeBlock(context, hasTwin, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(!newborn.metadataTable());
        auto intact = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!intact)
            JITCACHE_FAIL(makeString("the intact section fails "_s, describe(intact.error())));
        else {
            JITCACHE_CHECK(intact->propertyICs().size() == 1);
            JITCACHE_CHECK(intact->summary().icSitesWithCases == capture->header.icSitesWithCases);
        }

        auto strict = ICs::prepareBaselineICs(changed.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (strict)
            JITCACHE_FAIL("the changed section prepares under strict"_s);
        else if (strict.error().check != ICs::Check::MoldPairing || strict.error().siteIndex)
            JITCACHE_FAIL(makeString("the changed section fails "_s, describe(strict.error()), " instead of MoldPairing at site 0"_s));

        auto normal = ICs::prepareBaselineICs(changed.span(), code.get(), newborn, ICs::StrictChecks::No);
        if (!normal)
            JITCACHE_FAIL(makeString("the changed section fails without strict: "_s, describe(normal.error())));
        else {
            JITCACHE_CHECK(normal->propertyICs().size() == 1);
            JITCACHE_CHECK(normal->propertyICs()[0].accessType == static_cast<uint8_t>(AccessType::GetById));
        }
    });
}

// Sections 6.3 and 6.4 on a live CB, in the install function's order: the capture of a warm baseline CB, prepared under
// strict against its code and the newborn CB of a never-called twin, then seeded, set up with the same code and
// attached, leaves every call-link site and every property IC of the newborn CB in the state section 6.1 derives from
// its record (I3 and I4). The drive leaves a property IC with cases, a linked call site and a construct site gone
// virtual, so seeding takes both the setVirtualCall and the setSeen branch.
JITCACHE_TEST(icsLiveSeedAndAttach, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function body(o, f, C) { var x = o.p; f(); new C(); return x; }"
        "function bodyTwin(o, f, C) { var x = o.p; f(); new C(); return x; }"
        "function callee() { return 1; }"
        "function C1() { }"
        "function C2() { }"
        "var a = { p: 1 };"
        "var b = { q: 1, p: 2 };"_s);
    if (!globalObject)
        return;
    JSFunction* body = globalFunction(context, globalObject, "body"_s);
    JSFunction* bodyTwin = globalFunction(context, globalObject, "bodyTwin"_s);
    JSFunction* callee = globalFunction(context, globalObject, "callee"_s);
    JSFunction* c1 = globalFunction(context, globalObject, "C1"_s);
    JSFunction* c2 = globalFunction(context, globalObject, "C2"_s);
    auto a = globalObjectValue(context, globalObject, "a"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    if (!body || !bodyTwin || !callee || !c1 || !c2 || !a || !b)
        return;

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, body, { *a, callee, c1 });
    if (!codeBlock)
        return;
    for (unsigned callIndex = 0; callIndex < 6; ++callIndex) {
        bool odd = callIndex % 2;
        if (!callFunction(context, globalObject, body, { odd ? *b : *a, callee, odd ? c2 : c1 }))
            return;
    }
    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    bool hasVirtualSite = std::ranges::any_of(capture->callLinks, [](const ICs::CallLinkRecord& record) {
        return modeOf(record) == ICs::CallLinkModeCode::Virtual;
    });
    // Every site the producer did not leave virtual comes back seen once, so a non-virtual record takes the setSeen
    // branch.
    bool hasNonVirtualSite = std::ranges::any_of(capture->callLinks, [](const ICs::CallLinkRecord& record) {
        return modeOf(record) != ICs::CallLinkModeCode::Virtual;
    });
    bool hasCases = std::ranges::any_of(capture->propertyICs, [](const ICs::PropertyICRecord& record) {
        return record.caseCount > 0;
    });
    if (!hasVirtualSite || !hasNonVirtualSite || !hasCases) {
        JITCACHE_FAIL("the drive left no virtual site, no other call-link site or no property IC with cases"_s);
        return;
    }
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, bodyTwin, [&](CodeBlock& newborn) {
        auto prepared = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!prepared) {
            JITCACHE_FAIL(makeString("prepareBaselineICs failed "_s, describe(prepared.error())));
            return;
        }

        ICs::seedCallLinkHistory(*prepared, newborn);
        size_t siteIndex = 0;
        ICs::forEachCallLinkSite(newborn, [&](unsigned, unsigned, CallLinkInfo& callLinkInfo) {
            size_t index = siteIndex++;
            if (index >= prepared->callLinks().size())
                return;
            ICs::RestoredCallLink expected = ICs::restoredCallLink(prepared->callLinks()[index]);
            bool matches = callLinkInfo.mode() == (expected.isVirtual ? CallLinkInfo::Mode::Virtual : CallLinkInfo::Mode::Init)
                && callLinkInfo.seenOnce() == expected.seenOnce
                && callLinkInfo.hasSeenClosure() == expected.hasSeenClosure
                && callLinkInfo.clearedByGC() == expected.clearedByGC
                && callLinkInfo.clearedByVirtual() == expected.clearedByVirtual
                && callLinkInfo.maxArgumentCountIncludingThisForVarargs() == expected.maxArgumentCountIncludingThisForVarargs
                && !callLinkInfo.stub()
                && !callLinkInfo.haveLastSeenCallee()
                && !callLinkInfo.isOnList();
            if (!matches)
                JITCACHE_FAIL(makeString("call-link site "_s, index, " does not hold the state its record derives"_s));
        });
        JITCACHE_CHECK(siteIndex == prepared->callLinks().size());

        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(*prepared, newborn);
        BaselineJITData* jitData = newborn.baselineJITData();
        if (!jitData || jitData->propertyInlineCaches().size() != prepared->propertyICs().size()) {
            JITCACHE_FAIL("setup built another number of property ICs than the section holds"_s);
            return;
        }
        ConcurrentJSLocker locker(newborn.m_lock);
        for (size_t index = 0; index < prepared->propertyICs().size(); ++index) {
            const HandlerPropertyInlineCache& propertyCache = jitData->propertyCache(static_cast<unsigned>(index));
            ICs::RestoredPropertyIC expected = ICs::restoredPropertyIC(prepared->propertyICs()[index]);
            bool moldBit = code->m_unlinkedPropertyInlineCaches[index].canBeMegamorphic;
            bool holdsGaveUp = propertyCache.m_slowOperation == gaveUpOperationFor(propertyCache.accessType);
            bool matches = propertyCache.everConsidered == expected.everConsidered
                && propertyCache.sawNonCell == expected.sawNonCell
                && propertyCache.tookSlowPath == expected.tookSlowPath
                && propertyCache.resetByGC == expected.resetByGC
                && propertyCache.canBeMegamorphic == (moldBit || expected.foldsAtFirstCase)
                && propertyCache.countdown == expected.countdown
                && propertyCache.repatchCount == expected.repatchCount
                && propertyCache.numberOfCoolDowns == expected.numberOfCoolDowns
                && holdsGaveUp == expected.givenUp
                && !propertyCache.m_inlinedHandler;
            if (!matches)
                JITCACHE_FAIL(makeString("property IC "_s, index, " does not hold the state its record derives"_s));
        }
    });
}

// T11: capture is read-only and deterministic. On a baseline CB with warm property ICs and call sites, two captures with
// a snapshot between them write identical bytes, and the snapshots taken before, between and after them are equal.
JITCACHE_TEST(icsLiveCaptureIsReadOnly, Yes)
{
    VM& vm = *context.vm();
    auto body = warmUp(context, vm);
    if (!body)
        return;
    CodeBlock& codeBlock = *body->codeBlock;

    ICs::BaselineICsSnapshot before = ICs::snapshotBaselineICs(codeBlock);
    auto first = captureLive(context, codeBlock);
    ICs::BaselineICsSnapshot between = ICs::snapshotBaselineICs(codeBlock);
    auto second = captureLive(context, codeBlock);
    ICs::BaselineICsSnapshot after = ICs::snapshotBaselineICs(codeBlock);
    if (!first || !second || !expectWarm(context, *first))
        return;

    if (first->bytes != second->bytes) {
        size_t count = std::min(first->bytes.size(), second->bytes.size());
        size_t index = 0;
        while (index < count && first->bytes[index] == second->bytes[index])
            ++index;
        JITCACHE_FAIL(makeString("the two captures differ at byte "_s, index, " of "_s, first->bytes.size(), " and "_s, second->bytes.size()));
    }
    JITCACHE_CHECK(first->summary.summary.icSitesWithCases == second->summary.summary.icSitesWithCases);
    JITCACHE_CHECK(first->summary.hasPolymorphicSite == second->summary.hasPolymorphicSite);
    expectSameSnapshot(context, "the snapshots before and between the captures"_s, before, between);
    expectSameSnapshot(context, "the snapshots between and after the captures"_s, between, after);
}

// T13 item 1: on T11's baseline CB, snapshotBaselineICs lists one property-IC snapshot per IC and one call-link snapshot
// per call-link site, and each agrees, in every field the record holds, with the record captureBaselineICs writes at the
// same position. Each call-link snapshot also names the opcode and metadata ID its group and position give.
JITCACHE_TEST(icsLiveSnapshotAgreesWithCapture, Yes)
{
    VM& vm = *context.vm();
    auto body = warmUp(context, vm);
    if (!body)
        return;
    CodeBlock& codeBlock = *body->codeBlock;

    ICs::BaselineICsSnapshot snapshot = ICs::snapshotBaselineICs(codeBlock);
    auto capture = captureLive(context, codeBlock);
    if (!capture || !expectWarm(context, *capture))
        return;

    JITCACHE_CHECK(snapshot.jitType == JITType::BaselineJIT);
    JITCACHE_CHECK(snapshot.superConstructs.isEmpty());
    BaselineJITData* jitData = codeBlock.baselineJITData();
    if (!jitData || snapshot.propertyICs.size() != jitData->propertyInlineCaches().size() || snapshot.propertyICs.size() != capture->propertyICs.size()) {
        JITCACHE_FAIL(makeString("the snapshot lists "_s, snapshot.propertyICs.size(), " property ICs, the capture "_s, capture->propertyICs.size()));
        return;
    }
    if (snapshot.callLinks.size() != totalSites(ICs::callLinkSiteCounts(codeBlock)) || snapshot.callLinks.size() != capture->callLinks.size()) {
        JITCACHE_FAIL(makeString("the snapshot lists "_s, snapshot.callLinks.size(), " call-link sites, the capture "_s, capture->callLinks.size()));
        return;
    }

    for (size_t index = 0; index < snapshot.propertyICs.size(); ++index) {
        const ICs::PropertyICSnapshot& site = snapshot.propertyICs[index];
        const ICs::PropertyICRecord& record = capture->propertyICs[index];
        auto expectAgrees = [&](ASCIILiteral field, unsigned fromSnapshot, unsigned fromRecord) {
            if (fromSnapshot != fromRecord)
                JITCACHE_FAIL(makeString("property IC "_s, index, ": the snapshot's "_s, field, " is "_s, fromSnapshot, ", the record's "_s, fromRecord));
        };
        expectAgrees("accessType"_s, static_cast<uint8_t>(site.accessType), record.accessType);
        expectAgrees("everConsidered"_s, site.everConsidered, hasBit(record.learningBits, ICs::LearningBit::everConsidered));
        expectAgrees("sawNonCell"_s, site.sawNonCell, hasBit(record.learningBits, ICs::LearningBit::sawNonCell));
        expectAgrees("tookSlowPath"_s, site.tookSlowPath, hasBit(record.learningBits, ICs::LearningBit::tookSlowPath));
        expectAgrees("resetByGC"_s, site.resetByGC, hasBit(record.learningBits, ICs::LearningBit::resetByGC));
        expectAgrees("repatchCount"_s, site.repatchCount, record.repatchCount);
        expectAgrees("numberOfCoolDowns"_s, site.numberOfCoolDowns, record.numberOfCoolDowns);
        expectAgrees("caseCount"_s, site.caseCount, record.caseCount);
        expectAgrees("megamorphicCaseListed"_s, site.megamorphicCaseListed, hasBit(record.stateBits, ICs::StateBit::megamorphicCaseListed));
        expectAgrees("canBeMegamorphic"_s, site.canBeMegamorphic, hasBit(record.stateBits, ICs::StateBit::canBeMegamorphic));
        expectAgrees("holdsGaveUp"_s, site.holdsGaveUp, hasBit(record.stateBits, ICs::StateBit::holdsGaveUp));
    }

    // The opcode and metadata ID of each site, in canonical order, from the capture's groups.
    Vector<std::pair<uint32_t, unsigned>> sites;
    for (const ICs::CallLinkGroup& group : capture->groups) {
        for (unsigned metadataID = 0; metadataID < group.siteCount; ++metadataID)
            sites.append({ group.opcodeID, metadataID });
    }
    JITCACHE_CHECK(sites.size() == snapshot.callLinks.size());
    for (size_t index = 0; index < snapshot.callLinks.size(); ++index) {
        const ICs::CallLinkSnapshot& site = snapshot.callLinks[index];
        const ICs::CallLinkRecord& record = capture->callLinks[index];
        auto expectAgrees = [&](ASCIILiteral field, unsigned fromSnapshot, unsigned fromCapture) {
            if (fromSnapshot != fromCapture)
                JITCACHE_FAIL(makeString("call-link site "_s, index, ": the snapshot's "_s, field, " is "_s, fromSnapshot, ", the capture's "_s, fromCapture));
        };
        if (index < sites.size()) {
            expectAgrees("opcodeID"_s, static_cast<unsigned>(site.opcodeID), sites[index].first);
            expectAgrees("metadataID"_s, site.metadataID, sites[index].second);
        }
        expectAgrees("mode"_s, static_cast<unsigned>(modeCodeOf(site.mode)), static_cast<unsigned>(modeOf(record)));
        expectAgrees("seenOnce"_s, site.seenOnce, hasBit(record.bits, ICs::CallLinkBit::seenOnce));
        expectAgrees("hasSeenClosure"_s, site.hasSeenClosure, hasBit(record.bits, ICs::CallLinkBit::hasSeenClosure));
        expectAgrees("clearedByGC"_s, site.clearedByGC, hasBit(record.bits, ICs::CallLinkBit::clearedByGC));
        expectAgrees("clearedByVirtual"_s, site.clearedByVirtual, hasBit(record.bits, ICs::CallLinkBit::clearedByVirtual));
        expectAgrees("maxArgumentCountIncludingThisForVarargs"_s, site.maxArgumentCountIncludingThisForVarargs, record.maxArgumentCountIncludingThisForVarargs);
    }
}

// T13 item 2: T11's capture, prepared under strict against the CB's code and the newborn CB of a never-called twin, then
// seeded, set up with the same code and attached in the install function's order (section 6), leaves
// checkRestoredBaselineICs with no mismatch. Clearing everConsidered afterwards on one IC whose record has it set makes
// the check report mismatches only as PropertyIC at that IC's index, one of them naming everConsidered. The newborn CB
// is never run or installed, and the test uses it inside the deferral scope it was created in.
JITCACHE_TEST(icsLiveTwinCheck, Yes)
{
    VM& vm = *context.vm();
    auto body = warmUp(context, vm);
    if (!body)
        return;
    auto capture = captureLive(context, *body->codeBlock);
    if (!capture || !expectWarm(context, *capture))
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*body->codeBlock);

    withNewbornCodeBlock(context, body->twin, [&](CodeBlock& newborn) {
        auto prepared = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!prepared) {
            JITCACHE_FAIL(makeString("prepareBaselineICs failed "_s, describe(prepared.error())));
            return;
        }
        ICs::seedCallLinkHistory(*prepared, newborn);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(*prepared, newborn);

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(*prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));

        std::optional<size_t> considered;
        for (size_t index = 0; index < prepared->propertyICs().size() && !considered; ++index) {
            if (hasBit(prepared->propertyICs()[index].learningBits, ICs::LearningBit::everConsidered))
                considered = index;
        }
        BaselineJITData* jitData = newborn.baselineJITData();
        if (!considered || !jitData) {
            JITCACHE_FAIL("the restored CB has no IC whose record has everConsidered set"_s);
            return;
        }
        {
            ConcurrentJSLocker locker(newborn.m_lock);
            jitData->propertyCache(static_cast<unsigned>(*considered)).everConsidered = false;
        }

        Vector<ICs::TwinMismatch> mismatches = ICs::checkRestoredBaselineICs(*prepared, newborn);
        JITCACHE_CHECK(!mismatches.isEmpty());
        bool namesEverConsidered = false;
        for (const ICs::TwinMismatch& mismatch : mismatches) {
            if (mismatch.site != ICs::TwinMismatch::Site::PropertyIC || mismatch.index != *considered)
                JITCACHE_FAIL(makeString("clearing everConsidered on IC "_s, *considered, " also gave "_s, describe(mismatch)));
            if (mismatch.field == "everConsidered"_s)
                namesEverConsidered = true;
        }
        JITCACHE_CHECK(namesEverConsidered);
    });
}

// The shell function of section 11.1, which the integrator registers as jitcacheICsSnapshot: it throws a TypeError for
// anything but a function with JS code and a kind of "call" or "construct", returns undefined for an executable with no
// CB of the kind, and builds the object section 11.1 spells from a snapshot, a super_construct cache's callee included.
JITCACHE_TEST(icsLiveShellSnapshot, Yes)
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

    // As in T12 item 2, the IC lists shapes A and B after these calls.
    if (!bringToBaseline(context, globalObject, has, { *a1 }))
        return;
    for (JSValue argument : { *a1, *a2, *b }) {
        if (!callFunction(context, globalObject, has, { argument }))
            return;
    }

    Identifier name = Identifier::fromString(vm, "jitcacheICsSnapshot"_s);
    globalObject->putDirect(vm, name, JSFunction::create(vm, globalObject, 2, name.string(), ICs::functionSnapshotBaselineICs, ImplementationVisibility::Public));

    expectScriptPasses(context, globalObject,
        "(function () {"
        "    function throwsTypeError(thunk) {"
        "        try {"
        "            thunk();"
        "        } catch (error) {"
        "            return error instanceof TypeError;"
        "        }"
        "        return false;"
        "    }"
        "    function lacks(object, names, type) {"
        "        for (var name of names) {"
        "            if (typeof object[name] !== type)"
        "                return name;"
        "        }"
        "        return null;"
        "    }"
        "    if (!throwsTypeError(() => jitcacheICsSnapshot({ })))"
        "        return 'an object that is not a function did not throw a TypeError';"
        "    if (!throwsTypeError(() => jitcacheICsSnapshot(Math.max)))"
        "        return 'a host function did not throw a TypeError';"
        "    if (!throwsTypeError(() => jitcacheICsSnapshot(has, 'apply')))"
        "        return 'an unknown kind did not throw a TypeError';"
        "    if (!throwsTypeError(() => jitcacheICsSnapshot(has, 0)))"
        "        return 'a kind that is not a string did not throw a TypeError';"
        "    if (jitcacheICsSnapshot(has, 'construct') !== undefined)"
        "        return 'has, never constructed, has a construct snapshot';"
        ""
        "    var snapshot = jitcacheICsSnapshot(has);"
        "    if (JSON.stringify(snapshot) !== JSON.stringify(jitcacheICsSnapshot(has, 'call')))"
        "        return 'the default kind differs from call';"
        "    if (snapshot.jitType !== 'Baseline')"
        "        return 'the jitType of has is ' + snapshot.jitType;"
        "    if (snapshot.propertyICs.length !== 1 || snapshot.callLinks.length !== 0 || snapshot.superConstructs.length !== 0)"
        "        return 'the snapshot of has does not hold exactly one property IC';"
        "    var ic = snapshot.propertyICs[0];"
        "    var missing = lacks(ic, ['accessType', 'cacheType', 'summary'], 'string')"
        "        || lacks(ic, ['bytecodeIndex', 'caseCount', 'countdown', 'repatchCount', 'numberOfCoolDowns'], 'number')"
        "        || lacks(ic, ['holdsGaveUp', 'megamorphicCaseListed', 'everConsidered', 'sawNonCell', 'tookSlowPath', 'resetByGC', 'canBeMegamorphic'], 'boolean');"
        "    if (missing)"
        "        return 'the property IC lacks ' + missing;"
        "    if (ic.accessType !== 'InById' || ic.cacheType !== 'Stub' || ic.summary !== 'Simple')"
        "        return 'the property IC reads ' + ic.accessType + ', ' + ic.cacheType + ', ' + ic.summary;"
        "    if (ic.caseCount !== 2 || !ic.everConsidered || ic.holdsGaveUp)"
        "        return 'the property IC lists ' + ic.caseCount + ' cases';"
        ""
        "    class Base { }"
        "    class Derived extends Base { constructor() { super(); } }"
        "    if (jitcacheICsSnapshot(Derived, 'construct') !== undefined)"
        "        return 'Derived has a construct snapshot before its first construction';"
        "    new Derived();"
        "    if (jitcacheICsSnapshot(Derived) !== undefined)"
        "        return 'Derived, a class constructor, has a call snapshot';"
        "    var derived = jitcacheICsSnapshot(Derived, 'construct');"
        "    if (derived.jitType !== 'LLInt' || derived.propertyICs.length !== 0)"
        "        return 'the construct snapshot of Derived reads ' + derived.jitType + ' with ' + derived.propertyICs.length + ' property ICs';"
        "    if (derived.callLinks.length !== 1 || derived.superConstructs.length !== 1)"
        "        return 'Derived does not hold exactly one call-link site and one super_construct cache';"
        "    var site = derived.callLinks[0];"
        "    missing = lacks(site, ['opcode', 'mode'], 'string')"
        "        || lacks(site, ['metadataID', 'maxArgumentCountIncludingThisForVarargs'], 'number')"
        "        || lacks(site, ['seenOnce', 'hasSeenClosure', 'clearedByGC', 'clearedByVirtual', 'hasStub', 'hasLastSeenCallee'], 'boolean');"
        "    if (missing)"
        "        return 'the call-link site lacks ' + missing;"
        "    if (site.opcode !== 'op_super_construct' || site.metadataID !== 0)"
        "        return 'the call-link site is ' + site.opcode + ' ' + site.metadataID;"
        "    if (['Init', 'Monomorphic', 'Polymorphic', 'Virtual'].indexOf(site.mode) < 0)"
        "        return 'the call-link site has mode ' + site.mode;"
        "    var cache = derived.superConstructs[0];"
        "    if (cache.opcode !== 'op_super_construct' || cache.metadataID !== 0 || cache.state !== 'Single' || cache.cachedCallee !== Derived)"
        "        return 'the super_construct cache reads ' + cache.opcode + ' ' + cache.metadataID + ' ' + cache.state;"
        "    return '';"
        "})()"_s);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
