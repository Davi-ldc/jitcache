#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArgList.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "CallData.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "Instruction.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "MetadataTable.h"
#include "PropertyInlineCache.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/ics/ICOperations.h"
#include "jitcache/ics/ICRestore.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ics/ICSites.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/tests/ics/ICTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <iterator>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The live tests of ICRestore (ics/ICRestore.h and .cpp): preparing a section, seeding call-link history into a newborn
// CB and attaching property-IC state after its setup, with call sites in each state of their call data and their this
// profiles. Every test restores a section that captureLive takes from a live baseline CB through captureBaselineICs, so
// these tests also depend on capture, and a failure here can come from it. They take most of the state they expect
// from the same captured records, so a well-formed record with a wrong field can be restored as written and still
// pass. Three of them check the restored CB with checkRestoredBaselineICs, and two read it through snapshotBaselineICs.

namespace JSC::JITCache::Tests {

// Restore of a body without a metadata table: the body's section, a bare header, prepares into empty views, and seeding
// the newborn CB of a never-called twin visits no site and leaves it without a JIT type.
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

    CodeBlock* codeBlock = bringToBaseline(context, globalObject, id, { jsNumber(1) });
    if (!codeBlock)
        return;
    LiveCapture capture = captureLive(context, *codeBlock);
    JITCACHE_CHECK(capture.bytes.size() == ICs::sectionHeaderSize);
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);
    JITCACHE_CHECK(!code->m_unlinkedPropertyInlineCaches.size());

    withNewbornCodeBlock(context, idTwin, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(!newborn.metadataTable());
        ICs::PreparedBaselineICs prepared = ICs::prepareBaselineICs(capture.bytes.span());
        JITCACHE_CHECK(prepared.propertyICs().empty());
        JITCACHE_CHECK(prepared.callLinks().empty());

        unsigned visitedSites = 0;
        ICs::forEachCallLinkSite(newborn, [&](const ICs::CallLinkSite&) {
            ++visitedSites;
        });
        JITCACHE_CHECK(!visitedSites);
        ICs::seedCallLinkHistory(prepared, newborn);
        JITCACHE_CHECK(newborn.jitType() == JITType::None);
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
// once, then put through a full collection that folds the this samples. Its capture, prepared and seeded into the
// newborn CB of a never-called twin, which is then set up with the same code and attached, leaves every call-link site
// and every property IC of the newborn CB in the state ICRestore derives from its record: the tail call, the virtual
// site and the site with the array as this with data of their own, the plain site called twice and the one called once
// on the executed-once data, the never-called site as linking left it. Seeding thus takes every branch but
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

    LiveCapture capture = captureLive(context, *codeBlock);

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
    if (capture.callLinks.size() != std::size(expectedSites)) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture.callLinks.size(), " call-link records instead of "_s, std::size(expectedSites)));
        return;
    }
    const ICs::CallLinkRecord& arrayRecord = capture.callLinks[arraySiteIndex];
    bool hasCases = std::ranges::any_of(capture.propertyICs, [](const ICs::PropertyICRecord& record) {
        return record.caseCount > 0;
    });
    if (!ICs::readThisArrayModes(arrayRecord) || modeOf(capture.callLinks[5]) != ICs::CallLinkModeCode::Virtual || !hasCases) {
        JITCACHE_FAIL("the drive left no this profile at the array site, no virtual construct site or no property IC with cases"_s);
        return;
    }
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, bodyTwin, [&](CodeBlock& newborn) {
        ICs::PreparedBaselineICs prepared = ICs::prepareBaselineICs(capture.bytes.span());
        ICs::seedCallLinkHistory(prepared, newborn);
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
            ICs::RestoredCallLink expected = ICs::restoredCallLink(opcode, prepared.callLinks()[index]);
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
        ICs::attachPropertyICState(prepared, newborn);
        BaselineJITData* jitData = newborn.baselineJITData();
        if (!jitData || jitData->propertyInlineCaches().size() != prepared.propertyICs().size()) {
            JITCACHE_FAIL("setup built another number of property ICs than the section holds"_s);
            return;
        }
        {
            ConcurrentJSLocker locker(newborn.m_lock);
            for (size_t index = 0; index < prepared.propertyICs().size(); ++index) {
                const HandlerPropertyInlineCache& propertyCache = jitData->propertyCache(static_cast<unsigned>(index));
                ICs::RestoredPropertyIC expected = ICs::restoredPropertyIC(prepared.propertyICs()[index]);
                bool moldBit = code->m_unlinkedPropertyInlineCaches[index].canBeMegamorphic;
                bool holdsGaveUp = propertyCache.m_slowOperation == ICs::gaveUpOperationFor(propertyCache.accessType);
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

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));
    });
}

// Seeding's report to the heap. A body of 33 call_ignore_result sites a.push(v) is called twice with an array as a, so
// that each site gets data of its own at its second call, brought to baseline and put through a full collection that
// folds each site's sample. Its capture, prepared and applied to a newborn twin in the install function's order, inside
// the test's deferral, gives all 33 sites data of their own, so the 32nd allocation reports to the heap, which under
// the deferral only records deferred work. The newborn CB then owns 33 CallSiteDatas, and the twin check finds no
// mismatch.
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

    LiveCapture capture = captureLive(context, *codeBlock);
    if (capture.callLinks.size() != siteCount) {
        JITCACHE_FAIL(makeString("the capture holds "_s, capture.callLinks.size(), " call-link records instead of "_s, siteCount));
        return;
    }
    if (!std::ranges::all_of(capture.callLinks, [](const ICs::CallLinkRecord& record) { return ICs::readThisArrayModes(record); })) {
        JITCACHE_FAIL("a push site holds no this profile"_s);
        return;
    }
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, pushesTwin, [&](CodeBlock& newborn) {
        ICs::PreparedBaselineICs prepared = ICs::prepareBaselineICs(capture.bytes.span());
        ICs::seedCallLinkHistory(prepared, newborn);
        MetadataTable* metadataTable = newborn.metadataTable();
        JITCACHE_CHECK(metadataTable && metadataTable->numberOfOwnCallSiteDatas() == siteCount);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(prepared, newborn);
        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));
    });
}

// The this profile of a builtin, whose UCB keeps no copy of its profiles for the native merge to fold in, so the site's
// own CallSiteData is the only place it lives. Array.prototype.forEach of one realm runs, through JSC::call, on a
// two-element array with a callback and an array as thisArg, so its callback.@call(thisArg, ...) site runs twice and
// gets data of its own; the builtin's CB is brought to baseline, runs once more, and a full collection folds the site's
// sample. Its capture, prepared and seeded into the newborn CB of Array.prototype.forEach of a second realm, which
// shares the builtin's UCB or generates it again with the same layout, then set up with that CB's code and attached in
// the install function's order, leaves the site with data of its own whose this profile CodeBlock::getArrayProfile
// finds with the record's modes and flags, and the twin check finds no mismatch. With no sample to fold and no UCB copy
// to merge, updateAllArrayProfilePredictions then leaves the profile as it is.
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

    LiveCapture capture = captureLive(context, *codeBlock);
    // The callback site is the only one whose this is a cell.
    std::optional<size_t> callbackRecordIndex;
    for (size_t index = 0; index < capture.callLinks.size(); ++index) {
        if (!ICs::readThisArrayModes(capture.callLinks[index]))
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
    const ICs::CallLinkRecord& callbackRecord = capture.callLinks[*callbackRecordIndex];
    Ref<BaselineJITCode> code = baselineCodeOf(*codeBlock);

    withNewbornCodeBlock(context, secondForEach, [&](CodeBlock& newborn) {
        JITCACHE_CHECK(newborn.unlinkedCodeBlock()->isBuiltinFunction());
        ICs::PreparedBaselineICs prepared = ICs::prepareBaselineICs(capture.bytes.span());
        ICs::seedCallLinkHistory(prepared, newborn);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(prepared, newborn);

        ICs::BaselineICsSnapshot restored = ICs::snapshotBaselineICs(newborn);
        if (restored.callLinks.size() != capture.callLinks.size()) {
            JITCACHE_FAIL(makeString("the newborn CB has "_s, restored.callLinks.size(), " call-link sites instead of "_s, capture.callLinks.size()));
            return;
        }
        JITCACHE_CHECK(restored.callLinks[*callbackRecordIndex].data == ICs::CallSiteDataState::Own);

        // The callback site's instruction: its record's index is its group's start plus its metadata ID.
        std::optional<BytecodeIndex> callbackSite;
        ICs::forEachCallLinkInstruction(newborn, [&](const ICs::CallLinkSite& site, const JSInstruction* instruction) {
            if (prepared.callLinkGroupStarts()[site.canonicalPosition] + site.metadataID == *callbackRecordIndex)
                callbackSite = newborn.bytecodeIndex(instruction);
        });
        ArrayProfile* profile = callbackSite ? newborn.getArrayProfile(*callbackSite) : nullptr;
        if (!profile) {
            JITCACHE_FAIL("CodeBlock::getArrayProfile finds no profile at the callback site"_s);
            return;
        }
        JITCACHE_CHECK(profile->observedArrayModes() == ICs::readThisArrayModes(callbackRecord));
        JITCACHE_CHECK(profile->arrayProfileFlags().toRaw() == ICs::readThisArrayProfileFlags(callbackRecord));

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));

        newborn.updateAllArrayProfilePredictions();
        JITCACHE_CHECK(profile->observedArrayModes() == ICs::readThisArrayModes(callbackRecord));
        JITCACHE_CHECK(profile->arrayProfileFlags().toRaw() == ICs::readThisArrayProfileFlags(callbackRecord));
    });
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
