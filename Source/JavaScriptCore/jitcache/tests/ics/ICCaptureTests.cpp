#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "PolymorphicCallStubRoutine.h"
#include "PropertyInlineCache.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ics/ICSites.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/tests/ics/ICTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// The live tests of ICCapture (ics/ICCapture.h and .cpp): the capture of bodies with and without a metadata table, the
// polymorphic bit of property ICs and call sites, and that capture only reads. Every capture goes through captureLive,
// which also checks that summarizeBaselineICs and captureBaselineICs agree. icsLiveCaptureIsReadOnly also depends on
// ICTwins: it reads the CB through snapshotBaselineICs before, between and after its two captures.

namespace JSC::JITCache::Tests {

namespace ICCaptureTestsInternal {

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
        LiveCapture capture = captureLive(context, codeBlock);
        if (capture.callLinks.size() != 1) {
            JITCACHE_FAIL(makeString("the body has "_s, capture.callLinks.size(), " call-link sites instead of one"_s));
            return std::nullopt;
        }
        if (done(capture.callLinks[0]))
            return capture;
    }
    JITCACHE_FAIL("the call-link site never reached the state the test drives it to"_s);
    return std::nullopt;
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

} // namespace ICCaptureTestsInternal

using namespace ICCaptureTestsInternal;

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

    LiveCapture capture = captureLive(context, *codeBlock);
    JITCACHE_CHECK(capture.bytes.size() == ICs::sectionHeaderSize);
    JITCACHE_CHECK(!capture.header.propertyICCount);
    JITCACHE_CHECK(!capture.header.icSitesWithCases);
    JITCACHE_CHECK(!capture.header.callLinkGroupCount);
    JITCACHE_CHECK(!capture.header.callLinkSiteCount);
    JITCACHE_CHECK(capture.groups.isEmpty());
    JITCACHE_CHECK(capture.propertyICs.isEmpty());
    JITCACHE_CHECK(capture.callLinks.isEmpty());
    JITCACHE_CHECK(!capture.summary.summary.icSitesWithCases);
    JITCACHE_CHECK(!capture.summary.hasPolymorphicSite);
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
    LiveCapture capture = captureLive(context, *codeBlock);
    JITCACHE_CHECK(capture.groups.isEmpty());
    JITCACHE_CHECK(capture.callLinks.isEmpty());
    if (capture.propertyICs.size() != 1) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture.propertyICs.size(), " property-IC records instead of one"_s));
        return;
    }
    const ICs::PropertyICRecord& record = capture.propertyICs[0];
    JITCACHE_CHECK(record.accessType == static_cast<uint8_t>(AccessType::InById));
    JITCACHE_CHECK(record.caseCount == 2);
    JITCACHE_CHECK(hasBit(record.learningBits, ICs::LearningBit::everConsidered));
    JITCACHE_CHECK(capture.summary.summary.icSitesWithCases == 1);
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
        LiveCapture capture = captureLive(context, *codeBlock);
        if (capture.propertyICs.size() != 1) {
            JITCACHE_FAIL(makeString("the capture holds "_s, capture.propertyICs.size(), " property-IC records instead of one"_s));
            return;
        }
        const ICs::PropertyICRecord& record = capture.propertyICs[0];
        if (record.caseCount != step.caseCount)
            JITCACHE_FAIL(makeString("the IC lists "_s, static_cast<unsigned>(record.caseCount), " cases instead of "_s, static_cast<unsigned>(step.caseCount)));
        JITCACHE_CHECK(!hasBit(record.stateBits, ICs::StateBit::megamorphicCaseListed));
        JITCACHE_CHECK(ICs::isPolymorphicPropertyIC(record) == step.hasPolymorphicSite);
        if (capture.summary.hasPolymorphicSite != step.hasPolymorphicSite)
            JITCACHE_FAIL(makeString("with "_s, static_cast<unsigned>(record.caseCount), " cases listed, hasPolymorphicSite is "_s, static_cast<unsigned>(capture.summary.hasPolymorphicSite)));
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
        LiveCapture capture = captureLive(context, *codeBlock);
        if (capture.propertyICs.size() != 1) {
            JITCACHE_FAIL(makeString("the capture holds "_s, capture.propertyICs.size(), " property-IC records instead of one"_s));
            return;
        }
        if (hasBit(capture.propertyICs[0].stateBits, ICs::StateBit::megamorphicCaseListed))
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
    LiveCapture first = captureLive(context, codeBlock);
    ICs::BaselineICsSnapshot between = ICs::snapshotBaselineICs(codeBlock);
    LiveCapture second = captureLive(context, codeBlock);
    ICs::BaselineICsSnapshot after = ICs::snapshotBaselineICs(codeBlock);
    if (!expectWarm(context, first))
        return;

    if (first.bytes != second.bytes) {
        size_t count = std::min(first.bytes.size(), second.bytes.size());
        size_t index = 0;
        while (index < count && first.bytes[index] == second.bytes[index])
            ++index;
        JITCACHE_FAIL(makeString("the two captures differ at byte "_s, index, " of "_s, first.bytes.size(), " and "_s, second.bytes.size()));
    }
    JITCACHE_CHECK(first.summary.summary.icSitesWithCases == second.summary.summary.icSitesWithCases);
    JITCACHE_CHECK(first.summary.hasPolymorphicSite == second.summary.hasPolymorphicSite);
    expectSameSnapshot(context, "the snapshots before and between the captures"_s, before, between);
    expectSameSnapshot(context, "the snapshots between and after the captures"_s, between, after);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
