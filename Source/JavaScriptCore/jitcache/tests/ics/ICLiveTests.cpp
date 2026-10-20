#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "ArrayProfile.h"
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
#include "Instruction.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "MetadataTable.h"
#include "PolymorphicCallStubRoutine.h"
#include "PropertyInlineCache.h"
#include "Repatch.h"
#include "SourceCode.h"
#include "TestVM.h"
#include "TopExceptionScope.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The live tests of the ICs lane (ics/ICCapture, ICRestore, ICSection, ICSites and ICTwins, .h and .cpp): capture, the
// polymorphic bit, the newborn check, preparing, seeding and attaching ICs on a newborn CB, call sites in each state of
// their call data and their this profiles, the snapshot and the twin check, and the shell function that exposes the
// snapshot, each on a fresh VM with default options. A test builds its own global object, evaluates its source with
// JSC::evaluate and reads its functions from the global object. A baseline CB comes from one call through JSC::call,
// which installs an LLInt CB, and JIT::compileSync, which compiles that CB and runs the finalization that ends a
// compilation; the later calls run its baseline code. A newborn CB comes from newCodeBlockFor on a function never
// called, inside a DeferGCForAWhile, and is used only inside that scope. Two functions with the same body text have
// separate UCBs with the same metadata layout and molds, so one's BaselineJITCode and section pair with the other's
// newborn CB. The source keeps every object and function the test passes in its globals, and the global object stays
// reachable from the test's stack, so no collection resets a case or unlinks a callee between two reads of the same
// state.

namespace JSC::JITCache::Tests {

namespace ICLiveTestsInternal {

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
// checks that the two return the same summary and polymorphic bit and that the bytes pass every structural check of a
// strict import with the returned count in their header. The buffer starts filled with a nonzero byte, so padding,
// reserved bytes or unused bits the capture left unwritten fail those checks.
static std::optional<LiveCapture> captureLive(TestContext& context, CodeBlock& codeBlock)
{
    ICs::CaptureSummary summarized = ICs::summarizeBaselineICs(codeBlock);
    Vector<uint8_t> bytes(FillWith { }, ICs::baselineICsSectionSize(codeBlock), 0xcc);
    auto captured = ICs::captureBaselineICs(codeBlock, bytes.mutableSpan(), ICs::StrictChecks::Yes);
    if (!captured) {
        JITCACHE_FAIL(makeString("captureBaselineICs failed "_s, ICs::description(captured.error().check), " at site "_s, captured.error().siteIndex));
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
    ICs::forEachCallLinkSite(codeBlock, [&](const ICs::CallLinkSite& site) {
        CallLinkInfo* callLinkInfo = site.ownCallLinkInfo();
        if (auto* stub = callLinkInfo ? callLinkInfo->stub() : nullptr)
            stub->forEachDependentCell([&](JSCell*) { ++slotCount; });
    });
    return slotCount;
}

// Calls the function with first, then second, and so on in turn, until the record of its only call-link site satisfies
// done or eight calls have run. Every step summarizes and captures, which checks that the two agree.
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

static String describe(const ICs::Invalid& invalid)
{
    return makeString(ICs::description(invalid.check), " at site "_s, invalid.siteIndex);
}

static void expectPasses(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& invalid)
{
    if (invalid)
        JITCACHE_FAIL(makeString(what, " failed "_s, describe(*invalid)));
}

static void expectFails(TestContext& context, ASCIILiteral what, const std::optional<ICs::Invalid>& invalid, ICs::Check check, uint32_t siteIndex)
{
    if (!invalid) {
        JITCACHE_FAIL(makeString(what, " passed, expected "_s, ICs::description(check), " at site "_s, siteIndex));
        return;
    }
    if (invalid->check != check || invalid->siteIndex != siteIndex)
        JITCACHE_FAIL(makeString(what, " failed "_s, describe(*invalid), ", expected "_s, ICs::description(check), " at site "_s, siteIndex));
}

static unsigned totalSites(const ICs::CallLinkSiteCounts& counts)
{
    unsigned total = 0;
    for (uint32_t count : counts)
        total += count;
    return total;
}

// The body the read-only, snapshot and twin tests warm: a property load, a store and an in, a plain call, a varargs
// call and a construct. Its twin, never called, has the same metadata layout and molds.
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

// Capture of a body whose bytecode adds no metadata entry and no value profile (enter and ret): it has no metadata
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

// in_by_id carries no metadata, so the body has no metadata table, yet its property IC lives in BaselineJITData and its
// capture holds the IC's record. The first baseline visit only spends countdown, which starts at 1, so of the three
// calls the second caches shape A and the third shape B.
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

// With strict on, capture refuses a CB without BaselineJITData and an output of another size than the section's,
// before it writes a byte.
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

// A property IC that lists two cases, none megamorphic, sets the polymorphic bit.
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

// A get_by_id site that folds into one megamorphic case after caching plain loads of distinct shapes does not set the
// polymorphic bit, although its record listed two or more cases on the way.
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

// A Polymorphic call site whose stub holds two callees of different executables sets the polymorphic bit; one whose
// stub holds closures of one executable, merged into one despecified slot with hasSeenClosure, does not.
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

// A construct site that met a second constructor goes Virtual, which a status answers with a slow path, so it does not
// set the polymorphic bit.
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

// Restore of a body without a metadata table: its newborn CB passes NewbornCallLinks, and the body's section, a bare
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
        ICs::forEachCallLinkSite(newborn, [&](const ICs::CallLinkSite&) {
            ++visitedSites;
        });
        JITCACHE_CHECK(!visitedSites);
        ICs::seedCallLinkHistory(*prepared, newborn);
        JITCACHE_CHECK(newborn.jitType() == JITType::None);
        expectPasses(context, "the twin's newborn CB after seeding"_s, ICs::checkNewbornCodeBlock(newborn));
    });
}

// A CB that already ran fails NewbornCallLinks on its JIT type, and the newborn CB of a never-called function with the
// same body passes. icsLiveNewbornCheckPerSite tests the check's clauses on each site, on newborn CBs.
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

    if (!callFunction(context, globalObject, caller, { callee }))
        return;
    CodeBlock* codeBlock = caller->jsExecutable()->codeBlockForCall();
    if (!codeBlock) {
        JITCACHE_FAIL("calling caller installed no CB"_s);
        return;
    }
    JITCACHE_CHECK(codeBlock->jitType() != JITType::None);
    auto invalid = ICs::checkNewbornCodeBlock(*codeBlock);
    if (!invalid || invalid->check != ICs::Check::NewbornCallLinks)
        JITCACHE_FAIL(makeString("the CB that ran gives "_s, invalid ? describe(*invalid) : String("no failure"_s), " instead of "_s, ICs::description(ICs::Check::NewbornCallLinks)));

    withNewbornCodeBlock(context, callerTwin, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(totalSites(ICs::callLinkSiteCounts(newborn)) == 1);
        expectPasses(context, "the newborn CB of callerTwin"_s, ICs::checkNewbornCodeBlock(newborn));
    });
}

// NewbornCallLinks site by site, on newborn CBs of never-called bodies with two call sites each: two op_call sites, two
// op_tail_call sites and two op_call_varargs sites, whose call links are inline. Each untouched newborn CB passes. One
// change at the second site, on a fresh newborn CB each time, fails the check at that site's index: moving an op_call
// site to the executed-once data or giving it data of its own, giving an op_tail_call site data of its own, and, at an
// inline site, each call-link field that seeding writes over.
JITCACHE_TEST(icsLiveNewbornCheckPerSite, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm,
        "function twoCalls(f, g) { return f() + g(); }"
        "function twoTailCalls(f, g) { \"use strict\"; if (f) return f(); return g(); }"
        "function twoSpreads(f, g, a) { return f(...a) + g(...a); }"_s);
    if (!globalObject)
        return;
    JSFunction* twoCalls = globalFunction(context, globalObject, "twoCalls"_s);
    JSFunction* twoTailCalls = globalFunction(context, globalObject, "twoTailCalls"_s);
    JSFunction* twoSpreads = globalFunction(context, globalObject, "twoSpreads"_s);
    if (!twoCalls || !twoTailCalls || !twoSpreads)
        return;

    struct Body {
        ASCIILiteral name;
        JSFunction* function;
        OpcodeID opcodeID;
    };
    const Body bodies[] = {
        { "twoCalls"_s, twoCalls, OpCall::opcodeID },
        { "twoTailCalls"_s, twoTailCalls, OpTailCall::opcodeID },
        { "twoSpreads"_s, twoSpreads, OpCallVarargs::opcodeID },
    };
    for (const Body& body : bodies) {
        withNewbornCodeBlock(context, body.function, [&](CodeBlock& newborn) {
            unsigned siteCount = 0;
            ICs::forEachCallLinkSite(newborn, [&](const ICs::CallLinkSite& site) {
                ++siteCount;
                if (site.opcode().opcodeID != body.opcodeID)
                    JITCACHE_FAIL(makeString(body.name, " has a call-link site of "_s, opcodeNames[site.opcode().opcodeID]));
            });
            if (siteCount != 2)
                JITCACHE_FAIL(makeString(body.name, " has "_s, siteCount, " call-link sites instead of two"_s));
            expectPasses(context, body.name, ICs::checkNewbornCodeBlock(newborn));
        });
    }

    // Applies change to the second site of a fresh newborn CB of the function, and checks that NewbornCallLinks fails at
    // index 1. The change receives the site and its instruction.
    auto expectChangeFails = [&](ASCIILiteral name, JSFunction* function, const auto& change) {
        withNewbornCodeBlock(context, function, [&](CodeBlock& newborn) {
            unsigned siteIndex = 0;
            ICs::forEachCallLinkInstruction(newborn, [&](const ICs::CallLinkSite& site, const JSInstruction* instruction) {
                if (siteIndex++ == 1)
                    change(newborn, site, instruction);
            });
            if (siteIndex != 2) {
                JITCACHE_FAIL(makeString(name, ": the newborn CB has "_s, siteIndex, " call-link instructions instead of two"_s));
                return;
            }
            expectFails(context, name, ICs::checkNewbornCodeBlock(newborn), ICs::Check::NewbornCallLinks, 1);
        });
    };

    expectChangeFails("twoCalls with its second site executed once"_s, twoCalls, [&](CodeBlock&, const ICs::CallLinkSite& site, const JSInstruction*) {
        site.lazyInfo->setExecutedOnce(vm);
    });
    expectChangeFails("twoCalls with data of its second site's own"_s, twoCalls, [&](CodeBlock& newborn, const ICs::CallLinkSite&, const JSInstruction* instruction) {
        newborn.ensureCallLinkInfoAt(instruction);
    });
    expectChangeFails("twoTailCalls with data of its second site's own"_s, twoTailCalls, [&](CodeBlock& newborn, const ICs::CallLinkSite&, const JSInstruction* instruction) {
        newborn.ensureCallLinkInfoAt(instruction);
    });

    struct Change {
        ASCIILiteral name;
        void (*apply)(VM&, CallLinkInfo&);
    };
    const Change changes[] = {
        { "twoSpreads with the seen bit"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setSeen(); } },
        { "twoSpreads with hasSeenClosure"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setHasSeenClosure(); } },
        { "twoSpreads with clearedByGC"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setClearedByGC(); } },
        { "twoSpreads with clearedByVirtual"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setClearedByVirtual(); } },
        { "twoSpreads with a varargs maximum"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.updateMaxArgumentCountIncludingThisForVarargs(3); } },
        { "twoSpreads with the virtual mode"_s, [](VM& siteVM, CallLinkInfo& callLinkInfo) { callLinkInfo.setVirtualCall(siteVM); } },
        { "twoSpreads with another call type"_s, [](VM&, CallLinkInfo& callLinkInfo) { callLinkInfo.setCallType(CallLinkInfo::TailCallVarargs); } },
    };
    for (const Change& change : changes) {
        expectChangeFails(change.name, twoSpreads, [&](CodeBlock&, const ICs::CallLinkSite& site, const JSInstruction*) {
            change.apply(vm, *site.inlineInfo);
        });
    }
}

// has's section with its record's access type changed to GetById breaks MoldPairing and no other check, so prepare,
// given has's code and the newborn CB of a never-called twin, fails with MoldPairing under strict and succeeds without,
// which checks nothing. The intact section prepares under strict.
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
    // changed bytes still pass parseSection's checks, and has's molds carry no fold bit.
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
            JITCACHE_FAIL(makeString("the changed section fails "_s, describe(strict.error()), " instead of "_s, ICs::description(ICs::Check::MoldPairing), " at site 0"_s));

        auto normal = ICs::prepareBaselineICs(changed.span(), code.get(), newborn, ICs::StrictChecks::No);
        if (!normal)
            JITCACHE_FAIL(makeString("the changed section fails without strict: "_s, describe(normal.error())));
        else {
            JITCACHE_CHECK(normal->propertyICs().size() == 1);
            JITCACHE_CHECK(normal->propertyICs()[0].accessType == static_cast<uint8_t>(AccessType::GetById));
        }
    });
}

// The body of icsLiveSeedAndAttach, which its twin repeats. op selects the site a call reaches: three plain call sites,
// a call site whose this is the array a, a construct site, a varargs site and, since the body is strict, a tail call.
// Every call reads o.p first.
static constexpr auto seededBodyText = "(op, o, f, C, a, args) {"
    "    \"use strict\";"
    "    var r = o.p;"
    "    if (op === 1)"
    "        r = f();"
    "    else if (op === 2)"
    "        r = f();"
    "    else if (op === 3)"
    "        r = f();"
    "    else if (op === 4)"
    "        r = a.push(op);"
    "    else if (op === 5)"
    "        r = new C();"
    "    else if (op === 6)"
    "        r = f(...args);"
    "    else if (op === 7)"
    "        return f();"
    "    return r;"
    "}"_s;

// Seeding and attaching on a live CB, in the install function's order. The body is brought to baseline and driven until
// it holds a property IC with cases, a call site never called, one called once, one called twice as a plain f(), a call
// site called twice with an array as this, a construct site gone virtual, a tail call run once and a varargs site called
// once, then put through a full collection that folds the this samples. Its capture, prepared under strict against its
// code and the newborn CB of a never-called twin, then seeded, set up with the same code and attached, leaves every
// call-link site and every property IC of the newborn CB in the state ICRestore derives from its record: the tail call,
// the virtual site and the site with the array as this with data of their own, the plain site called twice and the one
// called once on the executed-once data, the never-called site as linking left it. Seeding thus takes every branch but
// setHasSeenClosure and setClearedByGC, which the capture-states script reaches. The newborn CB then owns three
// CallSiteDatas, CodeBlock::getArrayProfile at the array site finds the record's this profile, and the twin check finds
// no mismatch. The source's globals keep every callee and every structure the cases name alive.
JITCACHE_TEST(icsLiveSeedAndAttach, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, makeString("function body"_s, seededBodyText,
        "function bodyTwin"_s, seededBodyText,
        "function callee() { return 1; }"
        "function C1() { }"
        "function C2() { }"
        "var a = { p: 1 };"
        "var b = { q: 1, p: 2 };"
        "var array = [1];"
        "var args = [1, 2];"_s));
    if (!globalObject)
        return;
    JSFunction* body = globalFunction(context, globalObject, "body"_s);
    JSFunction* bodyTwin = globalFunction(context, globalObject, "bodyTwin"_s);
    JSFunction* callee = globalFunction(context, globalObject, "callee"_s);
    JSFunction* c1 = globalFunction(context, globalObject, "C1"_s);
    JSFunction* c2 = globalFunction(context, globalObject, "C2"_s);
    auto a = globalObjectValue(context, globalObject, "a"_s);
    auto b = globalObjectValue(context, globalObject, "b"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    auto args = globalObjectValue(context, globalObject, "args"_s);
    if (!body || !bodyTwin || !callee || !c1 || !c2 || !a || !b || !array || !args)
        return;

    // The call that installs the LLInt CB reaches no call site.
    CodeBlock* codeBlock = bringToBaseline(context, globalObject, body, { jsNumber(0), *a, callee, c1, *array, *args });
    if (!codeBlock)
        return;
    auto drive = [&](int op, unsigned callCount) {
        for (unsigned callIndex = 0; callIndex < callCount; ++callIndex) {
            bool odd = callIndex % 2;
            if (!callFunction(context, globalObject, body, { jsNumber(op), odd ? *b : *a, callee, odd ? c2 : c1, *array, *args }))
                return false;
        }
        return true;
    };
    if (!drive(2, 1) || !drive(3, 2) || !drive(4, 2) || !drive(5, 4) || !drive(6, 1) || !drive(7, 1))
        return;
    vm.heap.collectNow(Sync, CollectionScope::Full);

    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;

    // The sites in canonical order: the four op_call sites, the tail call, the construct site and the varargs site, and
    // the state each comes back in.
    struct ExpectedSite {
        OpcodeID opcodeID;
        ICs::CallSiteDataState data;
    };
    const ExpectedSite expectedSites[] = {
        { OpCall::opcodeID, ICs::CallSiteDataState::NotExecuted },
        { OpCall::opcodeID, ICs::CallSiteDataState::ExecutedOnce },
        { OpCall::opcodeID, ICs::CallSiteDataState::ExecutedOnce },
        { OpCall::opcodeID, ICs::CallSiteDataState::Own },
        { OpTailCall::opcodeID, ICs::CallSiteDataState::Own },
        { OpConstruct::opcodeID, ICs::CallSiteDataState::Own },
        { OpCallVarargs::opcodeID, ICs::CallSiteDataState::Inline },
    };
    constexpr size_t arraySiteIndex = 3;
    constexpr unsigned expectedOwnSites = 3;
    if (capture->callLinks.size() != std::size(expectedSites)) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture->callLinks.size(), " call-link records instead of "_s, std::size(expectedSites)));
        return;
    }
    const ICs::CallLinkRecord& arrayRecord = capture->callLinks[arraySiteIndex];
    bool hasCases = std::ranges::any_of(capture->propertyICs, [](const ICs::PropertyICRecord& record) {
        return record.caseCount > 0;
    });
    if (!ICs::readThisArrayModes(arrayRecord) || modeOf(capture->callLinks[5]) != ICs::CallLinkModeCode::Virtual || !hasCases) {
        JITCACHE_FAIL("the drive left no this profile at the array site, no virtual construct site or no property IC with cases"_s);
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
        // Before setup the CB has no BaselineJITData, so the snapshot lists only its call-link sites.
        ICs::BaselineICsSnapshot seeded = ICs::snapshotBaselineICs(newborn);
        if (seeded.callLinks.size() != std::size(expectedSites)) {
            JITCACHE_FAIL(makeString("the newborn CB has "_s, seeded.callLinks.size(), " call-link sites instead of "_s, std::size(expectedSites)));
            return;
        }
        for (size_t index = 0; index < seeded.callLinks.size(); ++index) {
            const ICs::CallLinkSnapshot& site = seeded.callLinks[index];
            const ExpectedSite& expectedSite = expectedSites[index];
            const ICs::CallLinkOpcode& opcode = ICs::callLinkOpcodes[*ICs::canonicalCallLinkPosition(site.opcodeID)];
            ICs::RestoredCallLink expected = ICs::restoredCallLink(opcode, prepared->callLinks()[index]);
            bool matches = site.opcodeID == expectedSite.opcodeID
                && expected.data == expectedSite.data
                && site.data == expected.data
                && site.mode == (expected.isVirtual ? CallLinkInfo::Mode::Virtual : CallLinkInfo::Mode::Init)
                && site.seenOnce == expected.seenOnce
                && site.hasSeenClosure == expected.hasSeenClosure
                && site.clearedByGC == expected.clearedByGC
                && site.clearedByVirtual == expected.clearedByVirtual
                && site.maxArgumentCountIncludingThisForVarargs == expected.maxArgumentCountIncludingThisForVarargs
                && site.thisArrayModes == expected.thisArrayModes
                && site.thisArrayProfileFlags == expected.thisArrayProfileFlags
                && !site.hasStub
                && !site.hasLastSeenCallee;
            if (!matches)
                JITCACHE_FAIL(makeString("call-link site "_s, index, " ("_s, opcodeNames[site.opcodeID], ") does not hold the state its record derives"_s));
        }
        MetadataTable* metadataTable = newborn.metadataTable();
        JITCACHE_CHECK(metadataTable && metadataTable->numberOfOwnCallSiteDatas() == expectedOwnSites);

        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(*prepared, newborn);
        BaselineJITData* jitData = newborn.baselineJITData();
        if (!jitData || jitData->propertyInlineCaches().size() != prepared->propertyICs().size()) {
            JITCACHE_FAIL("setup built another number of property ICs than the section holds"_s);
            return;
        }
        {
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
        }

        // The native reader of a site's this profile finds the restored one: the array site is the fourth op_call site.
        std::optional<BytecodeIndex> arraySite;
        ICs::forEachCallLinkInstruction(newborn, [&](const ICs::CallLinkSite& site, const JSInstruction* instruction) {
            if (site.opcode().opcodeID == OpCall::opcodeID && site.metadataID == arraySiteIndex)
                arraySite = newborn.bytecodeIndex(instruction);
        });
        ArrayProfile* arrayProfile = arraySite ? newborn.getArrayProfile(*arraySite) : nullptr;
        if (!arrayProfile)
            JITCACHE_FAIL("CodeBlock::getArrayProfile finds no profile at the array site"_s);
        else {
            JITCACHE_CHECK(arrayProfile->observedArrayModes() == ICs::readThisArrayModes(arrayRecord));
            JITCACHE_CHECK(arrayProfile->arrayProfileFlags().toRaw() == ICs::readThisArrayProfileFlags(arrayRecord));
        }

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(*prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));
    });
}

// Seeding's report to the heap. A body of 33 call_ignore_result sites a.push(v) is called twice with an array as a, so
// that each site gets data of its own at its second call, brought to baseline and put through a full collection that
// folds each site's sample. Its capture, prepared under strict and applied to a newborn twin in the install function's
// order, inside the test's deferral, gives all 33 sites data of their own, so the 32nd allocation reports to the heap,
// which under the deferral only records deferred work. The newborn CB then owns 33 CallSiteDatas, and the twin check
// finds no mismatch.
JITCACHE_TEST(icsLiveSeedingReportsToTheHeap, Yes)
{
    constexpr unsigned siteCount = 33;
    StringBuilder bodyText;
    bodyText.append("(a, v) {"_s);
    for (unsigned site = 0; site < siteCount; ++site)
        bodyText.append(" a.push(v);"_s);
    bodyText.append(" }"_s);
    String text = bodyText.toString();

    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, makeString("function pushes"_s, text, "function pushesTwin"_s, text, "var array = [1];"_s));
    if (!globalObject)
        return;
    JSFunction* pushes = globalFunction(context, globalObject, "pushes"_s);
    JSFunction* pushesTwin = globalFunction(context, globalObject, "pushesTwin"_s);
    auto array = globalObjectValue(context, globalObject, "array"_s);
    if (!pushes || !pushesTwin || !array)
        return;

    // The call that installs the LLInt CB moves each site to the executed-once data, and the first call in baseline code
    // gives each one data of its own.
    CodeBlock* codeBlock = bringToBaseline(context, globalObject, pushes, { *array, jsNumber(1) });
    if (!codeBlock || !callFunction(context, globalObject, pushes, { *array, jsNumber(2) }))
        return;
    vm.heap.collectNow(Sync, CollectionScope::Full);

    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    if (capture->callLinks.size() != siteCount) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture->callLinks.size(), " call-link records instead of "_s, siteCount));
        return;
    }
    if (!std::ranges::all_of(capture->callLinks, [](const ICs::CallLinkRecord& record) { return ICs::readThisArrayModes(record); })) {
        JITCACHE_FAIL("a push site holds no this profile"_s);
        return;
    }
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, pushesTwin, [&](CodeBlock& newborn) {
        auto prepared = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!prepared) {
            JITCACHE_FAIL(makeString("prepareBaselineICs failed "_s, describe(prepared.error())));
            return;
        }
        ICs::seedCallLinkHistory(*prepared, newborn);
        MetadataTable* metadataTable = newborn.metadataTable();
        JITCACHE_CHECK(metadataTable && metadataTable->numberOfOwnCallSiteDatas() == siteCount);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(*prepared, newborn);
        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(*prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));
    });
}

// The this profile of a builtin, whose UCB keeps no copy of its profiles for the native merge to fold in, so the site's
// own CallSiteData is the only place it lives. Array.prototype.forEach of one realm runs, through JSC::call, on a
// two-element array with a callback and an array as thisArg, so its callback.@call(thisArg, ...) site runs twice and
// gets data of its own; the builtin's CB is brought to baseline, runs once more, and a full collection folds the site's
// sample. Its capture, prepared under strict with that CB's code and the newborn CB of Array.prototype.forEach of a
// second realm, which shares the builtin's UCB or generates it again with the same layout, then seeded, set up and
// attached in the install function's order, leaves the site with data of its own whose this profile CodeBlock::
// getArrayProfile finds with the record's modes and flags, and the twin check finds no mismatch. With no sample to
// fold and no UCB copy to merge, updateAllArrayProfilePredictions then leaves the profile as it is.
JITCACHE_TEST(icsLiveBuiltinThisProfile, Yes)
{
    VM& vm = *context.vm();
    constexpr auto source = "var forEach = Array.prototype.forEach;"
        "var visits = 0;"
        "var callback = function () { ++visits; };"
        "var elements = [1, 2];"
        "var thisArgument = [3];"_s;
    JSGlobalObject* globalObject = createRealm(context, vm, source);
    JSGlobalObject* secondGlobalObject = createRealm(context, vm, source);
    if (!globalObject || !secondGlobalObject)
        return;
    JSFunction* forEach = globalFunction(context, globalObject, "forEach"_s);
    JSFunction* secondForEach = globalFunction(context, secondGlobalObject, "forEach"_s);
    JSFunction* callback = globalFunction(context, globalObject, "callback"_s);
    auto elements = globalObjectValue(context, globalObject, "elements"_s);
    auto thisArgument = globalObjectValue(context, globalObject, "thisArgument"_s);
    if (!forEach || !secondForEach || !callback || !elements || !thisArgument)
        return;
    JITCACHE_CHECK(forEach->jsExecutable() != secondForEach->jsExecutable());

    // forEach(callback, thisArgument) on elements: callFunction passes undefined as this, which forEach rejects.
    auto callForEach = [&] {
        MarkedArgumentBuffer arguments;
        arguments.append(callback);
        arguments.append(*thisArgument);
        NakedPtr<Exception> exception;
        JSC::call(globalObject, forEach, getCallData(forEach), *elements, arguments, exception);
        if (exception) {
            JITCACHE_FAIL("calling Array.prototype.forEach threw"_s);
            return false;
        }
        return true;
    };
    // The call that installs the LLInt CB runs the callback site twice, which gives it data of its own.
    if (!callForEach())
        return;
    CodeBlock* codeBlock = compileToBaseline(context, forEach);
    if (!codeBlock || !callForEach())
        return;
    vm.heap.collectNow(Sync, CollectionScope::Full);

    UnlinkedCodeBlock* unlinkedCodeBlock = codeBlock->unlinkedCodeBlock();
    JITCACHE_CHECK(unlinkedCodeBlock->isBuiltinFunction());
    JITCACHE_CHECK(!unlinkedCodeBlock->valueAndArrayProfiles());

    auto capture = captureLive(context, *codeBlock);
    if (!capture)
        return;
    // The callback site is the only one whose this is a cell.
    std::optional<size_t> callbackRecordIndex;
    for (size_t index = 0; index < capture->callLinks.size(); ++index) {
        if (!ICs::readThisArrayModes(capture->callLinks[index]))
            continue;
        if (callbackRecordIndex) {
            JITCACHE_FAIL("two call-link records of forEach hold this modes"_s);
            return;
        }
        callbackRecordIndex = index;
    }
    if (!callbackRecordIndex) {
        JITCACHE_FAIL("no call-link record of forEach holds this modes"_s);
        return;
    }
    const ICs::CallLinkRecord& callbackRecord = capture->callLinks[*callbackRecordIndex];
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, secondForEach, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(newborn.unlinkedCodeBlock()->isBuiltinFunction());
        auto prepared = ICs::prepareBaselineICs(capture->bytes.span(), code.get(), newborn, ICs::StrictChecks::Yes);
        if (!prepared) {
            JITCACHE_FAIL(makeString("prepareBaselineICs failed "_s, describe(prepared.error())));
            return;
        }
        ICs::seedCallLinkHistory(*prepared, newborn);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(*prepared, newborn);

        ICs::BaselineICsSnapshot restored = ICs::snapshotBaselineICs(newborn);
        if (restored.callLinks.size() != capture->callLinks.size()) {
            JITCACHE_FAIL(makeString("the newborn CB has "_s, restored.callLinks.size(), " call-link sites instead of "_s, capture->callLinks.size()));
            return;
        }
        JITCACHE_CHECK(restored.callLinks[*callbackRecordIndex].data == ICs::CallSiteDataState::Own);

        // The callback site's instruction: its record's index is its group's start plus its metadata ID.
        std::optional<BytecodeIndex> callbackSite;
        ICs::forEachCallLinkInstruction(newborn, [&](const ICs::CallLinkSite& site, const JSInstruction* instruction) {
            if (prepared->callLinkGroupStarts()[site.canonicalPosition] + site.metadataID == *callbackRecordIndex)
                callbackSite = newborn.bytecodeIndex(instruction);
        });
        ArrayProfile* profile = callbackSite ? newborn.getArrayProfile(*callbackSite) : nullptr;
        if (!profile) {
            JITCACHE_FAIL("CodeBlock::getArrayProfile finds no profile at the callback site"_s);
            return;
        }
        JITCACHE_CHECK(profile->observedArrayModes() == ICs::readThisArrayModes(callbackRecord));
        JITCACHE_CHECK(profile->arrayProfileFlags().toRaw() == ICs::readThisArrayProfileFlags(callbackRecord));

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(*prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));

        newborn.updateAllArrayProfilePredictions();
        JITCACHE_CHECK(profile->observedArrayModes() == ICs::readThisArrayModes(callbackRecord));
        JITCACHE_CHECK(profile->arrayProfileFlags().toRaw() == ICs::readThisArrayProfileFlags(callbackRecord));
    });
}

// Capture is read-only and deterministic. On a baseline CB with warm property ICs and call sites, two captures with a
// snapshot between them write identical bytes, and the snapshots taken before, between and after them are equal.
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

// On the read-only test's baseline CB, snapshotBaselineICs lists one property-IC snapshot per IC and one call-link
// snapshot per call-link site, and each agrees, in every field the record holds, with the record captureBaselineICs
// writes at the same position. Each call-link snapshot also names the opcode and metadata ID its group and position
// give.
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
        expectAgrees("thisArrayModes"_s, site.thisArrayModes, ICs::readThisArrayModes(record));
        expectAgrees("thisArrayProfileFlags"_s, site.thisArrayProfileFlags, ICs::readThisArrayProfileFlags(record));
    }
}

// The read-only test's capture, prepared under strict against the CB's code and the newborn CB of a never-called twin,
// then seeded, set up with the same code and attached in the install function's order, leaves checkRestoredBaselineICs
// with no mismatch. Clearing everConsidered afterwards on one IC whose record has it set makes the check report
// mismatches only as PropertyIC at that IC's index, one of them naming everConsidered. The newborn CB is never run or
// installed, and the test uses it inside the deferral scope it was created in.
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

// The shell function the integrator registers as jitcacheICsSnapshot: it throws a TypeError for anything but a
// function with JS code and a kind of "call" or "construct", returns undefined for an executable with no CB of the
// kind, and builds the snapshot's object from a snapshot, a super_construct cache's callee included.
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

    // As in icsLiveCaptureInByIdWithoutMetadataTable, the IC lists shapes A and B after these calls.
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
        "    missing = lacks(site, ['opcode', 'data', 'mode'], 'string')"
        "        || lacks(site, ['metadataID', 'maxArgumentCountIncludingThisForVarargs', 'thisArrayModes', 'thisArrayProfileFlags'], 'number')"
        "        || lacks(site, ['seenOnce', 'hasSeenClosure', 'clearedByGC', 'clearedByVirtual', 'hasStub', 'hasLastSeenCallee'], 'boolean');"
        "    if (missing)"
        "        return 'the call-link site lacks ' + missing;"
        "    if (site.opcode !== 'op_super_construct' || site.metadataID !== 0)"
        "        return 'the call-link site is ' + site.opcode + ' ' + site.metadataID;"
        "    if (['Inline', 'NotExecuted', 'ExecutedOnce', 'Own'].indexOf(site.data) < 0)"
        "        return 'the call-link site has data ' + site.data;"
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
