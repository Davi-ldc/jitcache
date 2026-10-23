#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "ConcurrentJSLock.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "SourceCode.h"
#include "TopExceptionScope.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/ics/ICRestore.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ics/ICSites.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/tests/ics/ICTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <initializer_list>
#include <optional>
#include <utility>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The live tests of ICTwins (ics/ICTwins.h and .cpp): the snapshot against the capture, the twin check with a
// mismatch it must report, and the shell function that exposes the snapshot.

namespace JSC::JITCache::Tests {

namespace ICTwinsTestsInternal {

static unsigned totalSites(const ICs::CallLinkSiteCounts& counts)
{
    unsigned total = 0;
    for (uint32_t count : counts)
        total += count;
    return total;
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

} // namespace ICTwinsTestsInternal

using namespace ICTwinsTestsInternal;

// On the baseline CB warmUp gives, snapshotBaselineICs lists one property-IC snapshot per IC and one call-link snapshot
// per call-link site, and each agrees, in every field the record holds, with the record captureBaselineICs writes at
// the same position. Each call-link snapshot also names the opcode and metadata ID its group and position give.
JITCACHE_TEST(icsLiveSnapshotAgreesWithCapture, Yes)
{
    VM& vm = *context.vm();
    auto body = warmUp(context, vm);
    if (!body)
        return;
    CodeBlock& codeBlock = *body->codeBlock;

    ICs::BaselineICsSnapshot snapshot = ICs::snapshotBaselineICs(codeBlock);
    LiveCapture capture = captureLive(context, codeBlock);
    if (!expectWarm(context, capture))
        return;

    JITCACHE_CHECK(snapshot.jitType == JITType::BaselineJIT);
    JITCACHE_CHECK(snapshot.superConstructs.isEmpty());
    BaselineJITData* jitData = codeBlock.baselineJITData();
    if (!jitData || snapshot.propertyICs.size() != jitData->propertyInlineCaches().size() || snapshot.propertyICs.size() != capture.propertyICs.size()) {
        JITCACHE_FAIL(makeString("the snapshot lists "_s, snapshot.propertyICs.size(), " property ICs, the capture "_s, capture.propertyICs.size()));
        return;
    }
    if (snapshot.callLinks.size() != totalSites(ICs::callLinkSiteCounts(codeBlock)) || snapshot.callLinks.size() != capture.callLinks.size()) {
        JITCACHE_FAIL(makeString("the snapshot lists "_s, snapshot.callLinks.size(), " call-link sites, the capture "_s, capture.callLinks.size()));
        return;
    }

    for (size_t index = 0; index < snapshot.propertyICs.size(); ++index) {
        const ICs::PropertyICSnapshot& site = snapshot.propertyICs[index];
        const ICs::PropertyICRecord& record = capture.propertyICs[index];
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
    for (const ICs::CallLinkGroup& group : capture.groups) {
        for (unsigned metadataID = 0; metadataID < group.siteCount; ++metadataID)
            sites.append({ group.opcodeID, metadataID });
    }
    JITCACHE_CHECK(sites.size() == snapshot.callLinks.size());
    for (size_t index = 0; index < snapshot.callLinks.size(); ++index) {
        const ICs::CallLinkSnapshot& site = snapshot.callLinks[index];
        const ICs::CallLinkRecord& record = capture.callLinks[index];
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

// The capture of the baseline CB warmUp gives, prepared and seeded into the newborn CB of a never-called twin, which is
// then set up with the CB's code and attached in the install function's order, leaves checkRestoredBaselineICs with no
// mismatch. Clearing everConsidered afterwards on one IC whose record has it set makes the check report mismatches
// only as PropertyIC at that IC's index, one of them naming everConsidered. The newborn CB is never run or installed,
// and the test uses it inside the deferral scope it was created in.
JITCACHE_TEST(icsLiveTwinCheck, Yes)
{
    VM& vm = *context.vm();
    auto body = warmUp(context, vm);
    if (!body)
        return;
    LiveCapture capture = captureLive(context, *body->codeBlock);
    if (!expectWarm(context, capture))
        return;
    Ref<BaselineJITCode> code = baselineCodeOf(*body->codeBlock);

    withNewbornCodeBlock(context, body->twin, [&](CodeBlock& newborn) {
        ICs::PreparedBaselineICs prepared = ICs::prepareBaselineICs(capture.bytes.span());
        ICs::seedCallLinkHistory(prepared, newborn);
        newborn.setupWithUnlinkedBaselineCode(code.copyRef());
        ICs::attachPropertyICState(prepared, newborn);

        for (const ICs::TwinMismatch& mismatch : ICs::checkRestoredBaselineICs(prepared, newborn))
            JITCACHE_FAIL(makeString("the restored CB differs from its twin: "_s, describe(mismatch)));

        std::optional<size_t> considered;
        for (size_t index = 0; index < prepared.propertyICs().size() && !considered; ++index) {
            if (hasBit(prepared.propertyICs()[index].learningBits, ICs::LearningBit::everConsidered))
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

        Vector<ICs::TwinMismatch> mismatches = ICs::checkRestoredBaselineICs(prepared, newborn);
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
