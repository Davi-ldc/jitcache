#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// What the ICs lane's live tests share: the capture of a live CB decoded into copies, the warm body several tests read,
// and the description of a twin mismatch. Each test runs on a fresh VM with default options. A test builds its own
// global object, evaluates its source with JSC::evaluate and reads its functions from the global object. A baseline CB
// comes from one call through JSC::call, which installs an LLInt CB, and JIT::compileSync, which compiles that CB and
// runs the finalization that ends a compilation; the later calls run its baseline code. A newborn CB comes from
// newCodeBlockFor on a function never called, inside a DeferGCForAWhile, and is used only inside that scope. Two
// functions with the same body text have separate UCBs with the same metadata layout and molds, so one's
// BaselineJITCode and section pair with the other's newborn CB. The source keeps every object and function the test
// passes in its globals, and the global object stays reachable from the test's stack, so no collection resets a case or
// unlinks a callee between two reads of the same state.

namespace JSC::JITCache::Tests {

// A capture of a live CB, with its section decoded into copies.
struct LiveCapture {
    Vector<uint8_t> bytes;
    ICs::CaptureSummary summary;
    ICs::SectionHeader header;
    Vector<ICs::CallLinkGroup> groups;
    Vector<ICs::PropertyICRecord> propertyICs;
    Vector<ICs::CallLinkRecord> callLinks;
};

// Summarizes and then captures the CB, in one pause as the integrator's scoring and capture glue do, and checks that
// the two return the same summary and polymorphic bit and that the section's header holds the returned count.
inline LiveCapture captureLive(TestContext& context, CodeBlock& codeBlock)
{
    ICs::CaptureSummary summarized = ICs::summarizeBaselineICs(codeBlock);
    Vector<uint8_t> bytes(ICs::baselineICsSectionSize(codeBlock));
    ICs::CaptureSummary captured = ICs::captureBaselineICs(codeBlock, bytes.mutableSpan());
    if (captured.hasPolymorphicSite != summarized.hasPolymorphicSite)
        JITCACHE_FAIL(makeString("summarizeBaselineICs and captureBaselineICs disagree on hasPolymorphicSite: "_s, static_cast<unsigned>(summarized.hasPolymorphicSite), " and "_s, static_cast<unsigned>(captured.hasPolymorphicSite)));
    if (captured.summary.icSitesWithCases != summarized.summary.icSitesWithCases)
        JITCACHE_FAIL(makeString("summarizeBaselineICs and captureBaselineICs disagree on icSitesWithCases: "_s, summarized.summary.icSitesWithCases, " and "_s, captured.summary.icSitesWithCases));

    ICs::SectionView view = ICs::parseSection(bytes.span());
    JITCACHE_CHECK(view.header.icSitesWithCases == captured.summary.icSitesWithCases);

    LiveCapture capture {
        .bytes = { },
        .summary = captured,
        .header = view.header,
        .groups = { },
        .propertyICs = { },
        .callLinks = { },
    };
    for (unsigned index = 0; index < view.header.callLinkGroupCount; ++index)
        capture.groups.append(view.group(index));
    for (auto& record : view.propertyICs)
        capture.propertyICs.append(record);
    for (auto& record : view.callLinks)
        capture.callLinks.append(record);
    capture.bytes = WTF::move(bytes);
    return capture;
}

inline bool hasBit(uint8_t bits, uint8_t bit)
{
    return bits & bit;
}

inline ICs::CallLinkModeCode modeOf(const ICs::CallLinkRecord& record)
{
    return static_cast<ICs::CallLinkModeCode>(record.bits & ICs::CallLinkBit::modeMask);
}

// The body of the in_by_id tests, whose objects have two shapes: A (a1 and a2) and B (b).
inline constexpr ASCIILiteral hasSource = "function has(o) { return \"x\" in o; }"
    "var a1 = { x: 1 };"
    "var a2 = { x: 2 };"
    "var b = { y: 1, x: 2 };"_s;

// The body the read-only, snapshot and twin tests warm: a property load, a store and an in, a plain call, a varargs
// call and a construct. Its twin, never called, has the same metadata layout and molds.
inline constexpr ASCIILiteral warmSource = "function warm(o, f, C, args) { var x = o.p; o.q = x; f(); f(...args); new C(); return \"p\" in o; }"
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
inline std::optional<WarmBody> warmUp(TestContext& context, VM& vm)
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
inline bool expectWarm(TestContext& context, const LiveCapture& capture)
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

inline ASCIILiteral siteName(ICs::TwinMismatch::Site site)
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

inline String describe(const ICs::TwinMismatch& mismatch)
{
    return makeString(siteName(mismatch.site), ' ', mismatch.index, ' ', mismatch.field, ": expected "_s, mismatch.expected, ", actual "_s, mismatch.actual);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
