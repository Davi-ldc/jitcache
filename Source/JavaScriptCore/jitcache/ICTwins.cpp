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
#include "ICTwins.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "DeferGC.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "ICCapture.h"
#include "ICSection.h"
#include "ICSites.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "ObjectConstructor.h"
#include "PropertyInlineCache.h"
#include <algorithm>
#include <array>
#include <span>

namespace JSC::JITCache::ICs {

namespace ICTwinsInternal {

// The opcode at each canonical call-link position (section 4.2), which names a call-link snapshot.
static constexpr std::array<OpcodeID, numberOfCallLinkOpcodes> callLinkOpcodes {
#define JITCACHE_ICS_TWINS_CALL_LINK_OPCODE_ID(opcodeStruct) opcodeStruct::opcodeID,
    FOR_EACH_OPCODE_WITH_CALL_LINK_INFO(JITCACHE_ICS_TWINS_CALL_LINK_OPCODE_ID)
#undef JITCACHE_ICS_TWINS_CALL_LINK_OPCODE_ID
};

static bool hasBit(uint8_t bits, uint8_t bit)
{
    return bits & bit;
}

static CallLinkInfo::Mode callLinkMode(CallLinkModeCode code)
{
    switch (code) {
    case CallLinkModeCode::Init:
        return CallLinkInfo::Mode::Init;
    case CallLinkModeCode::Monomorphic:
        return CallLinkInfo::Mode::Monomorphic;
    case CallLinkModeCode::Polymorphic:
        return CallLinkInfo::Mode::Polymorphic;
    case CallLinkModeCode::Virtual:
        return CallLinkInfo::Mode::Virtual;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Every field a record holds comes from the record capture writes, so a snapshot and a capture of the same state agree;
// the fields the record leaves out are read beside it.
static PropertyICSnapshot snapshotPropertyIC(const ConcurrentJSLocker& locker, VM& vm, const HandlerPropertyInlineCache& propertyCache)
{
    PropertyICRecord record = readPropertyICRecord(locker, propertyCache);
    return PropertyICSnapshot {
        .accessType = static_cast<AccessType>(record.accessType),
        .bytecodeIndex = propertyCache.codeOrigin.bytecodeIndex().offset(),
        .cacheType = propertyCache.cacheType(),
        .summary = propertyCache.summary(locker, vm),
        .holdsGaveUp = hasBit(record.stateBits, StateBit::holdsGaveUp),
        .caseCount = record.caseCount,
        .megamorphicCaseListed = hasBit(record.stateBits, StateBit::megamorphicCaseListed),
        .everConsidered = hasBit(record.learningBits, LearningBit::everConsidered),
        .sawNonCell = hasBit(record.learningBits, LearningBit::sawNonCell),
        .tookSlowPath = hasBit(record.learningBits, LearningBit::tookSlowPath),
        .resetByGC = hasBit(record.learningBits, LearningBit::resetByGC),
        .canBeMegamorphic = hasBit(record.stateBits, StateBit::canBeMegamorphic),
        .countdown = propertyCache.countdown,
        .repatchCount = record.repatchCount,
        .numberOfCoolDowns = record.numberOfCoolDowns,
    };
}

static CallLinkSnapshot snapshotCallLink(const ConcurrentJSLocker& locker, OpcodeID opcodeID, unsigned metadataID, CallLinkInfo& callLinkInfo)
{
    CallLinkRecord record = readCallLinkRecord(locker, callLinkInfo);
    return CallLinkSnapshot {
        .opcodeID = opcodeID,
        .metadataID = metadataID,
        .mode = callLinkMode(static_cast<CallLinkModeCode>(record.bits & CallLinkBit::modeMask)),
        .seenOnce = hasBit(record.bits, CallLinkBit::seenOnce),
        .hasSeenClosure = hasBit(record.bits, CallLinkBit::hasSeenClosure),
        .clearedByGC = hasBit(record.bits, CallLinkBit::clearedByGC),
        .clearedByVirtual = hasBit(record.bits, CallLinkBit::clearedByVirtual),
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
        .hasStub = !!callLinkInfo.stub(),
        .hasLastSeenCallee = callLinkInfo.haveLastSeenCallee(),
    };
}

// The cache holds null, a callee, or JSCell::seenMultipleCalleeObjects() once a second callee arrived, which is why it
// is read without validation, as the DFG's ByteCodeParser and CodeBlock::reconcileLLIntInlineCachesAtGCEnd read it.
template<typename Op>
static void appendSuperConstructs(CodeBlock& codeBlock, Vector<SuperConstructSnapshot>& snapshots)
{
    unsigned metadataID = 0;
    forEachMetadataEntry<Op>(codeBlock, [&](typename Op::Metadata& metadata) {
        JSCell* cachedCallee = metadata.m_cachedCallee.unvalidatedGet();
        auto state = SuperConstructSnapshot::State::Single;
        if (!cachedCallee)
            state = SuperConstructSnapshot::State::Empty;
        else if (cachedCallee == JSCell::seenMultipleCalleeObjects())
            state = SuperConstructSnapshot::State::Multiple;
        snapshots.append(SuperConstructSnapshot {
            .opcodeID = Op::opcodeID,
            .metadataID = metadataID++,
            .state = state,
            .cachedCallee = state == SuperConstructSnapshot::State::Single ? cachedCallee : nullptr,
        });
    });
}

static void expectField(Vector<TwinMismatch>& mismatches, TwinMismatch::Site site, size_t index, ASCIILiteral field, uint32_t expected, uint32_t actual)
{
    if (expected == actual)
        return;
    mismatches.append(TwinMismatch {
        .site = site,
        .index = static_cast<uint32_t>(index),
        .field = field,
        .expected = expected,
        .actual = actual,
    });
}

// Section counts are 32-bit, and BaselineJITData and the metadata table count their ICs and sites with unsigned values.
static void expectCount(Vector<TwinMismatch>& mismatches, TwinMismatch::Site site, ASCIILiteral field, size_t expected, size_t actual)
{
    expectField(mismatches, site, 0, field, static_cast<uint32_t>(expected), static_cast<uint32_t>(actual));
}

// I3: every call-link site holds the mode, seen bit, history bits and varargs maximum its record derives, and no callee
// beyond what linking or setVirtualCall left: no stub and no last-seen callee.
static void checkSeededCallLinks(const PreparedBaselineICs& prepared, const BaselineICsSnapshot& snapshot, Vector<TwinMismatch>& mismatches)
{
    constexpr auto site = TwinMismatch::Site::CallLink;
    std::span<const CallLinkRecord> records = prepared.callLinks();
    expectCount(mismatches, site, "callLinkSiteCount"_s, records.size(), snapshot.callLinks.size());
    for (size_t index = 0, count = std::min<size_t>(records.size(), snapshot.callLinks.size()); index < count; ++index) {
        RestoredCallLink expected = restoredCallLink(records[index]);
        const CallLinkSnapshot& actual = snapshot.callLinks[index];
        auto expectedMode = expected.isVirtual ? CallLinkInfo::Mode::Virtual : CallLinkInfo::Mode::Init;
        expectField(mismatches, site, index, "mode"_s, static_cast<uint32_t>(expectedMode), static_cast<uint32_t>(actual.mode));
        expectField(mismatches, site, index, "seenOnce"_s, expected.seenOnce, actual.seenOnce);
        expectField(mismatches, site, index, "hasSeenClosure"_s, expected.hasSeenClosure, actual.hasSeenClosure);
        expectField(mismatches, site, index, "clearedByGC"_s, expected.clearedByGC, actual.clearedByGC);
        expectField(mismatches, site, index, "clearedByVirtual"_s, expected.clearedByVirtual, actual.clearedByVirtual);
        expectField(mismatches, site, index, "maxArgumentCountIncludingThisForVarargs"_s, expected.maxArgumentCountIncludingThisForVarargs, actual.maxArgumentCountIncludingThisForVarargs);
        expectField(mismatches, site, index, "hasStub"_s, false, actual.hasStub);
        expectField(mismatches, site, index, "hasLastSeenCallee"_s, false, actual.hasLastSeenCallee);
    }
}

// I4: every IC holds the learning fields its record derives, the fold bit ORed into its mold's, the *GaveUp operation
// exactly when the site was given up, and, of the fields attach leaves alone, the access type of its record and the
// shape and empty case list installation built.
static void checkAttachedPropertyICs(const PreparedBaselineICs& prepared, CodeBlock& codeBlock, const BaselineICsSnapshot& snapshot, Vector<TwinMismatch>& mismatches)
{
    constexpr auto site = TwinMismatch::Site::PropertyIC;
    std::span<const PropertyICRecord> records = prepared.propertyICs();
    // The molds of the code the CB runs, from which setup built its ICs; none when the CB runs no baseline code.
    // jitCode keeps that code alive while the loop reads the molds.
    RefPtr jitCode = codeBlock.jitCode();
    std::span<const BaselineUnlinkedPropertyInlineCache> molds;
    if (jitCode && jitCode->jitType() == JITType::BaselineJIT)
        molds = static_cast<BaselineJITCode&>(*jitCode).m_unlinkedPropertyInlineCaches.span();
    expectCount(mismatches, site, "propertyICCount"_s, records.size(), snapshot.propertyICs.size());
    for (size_t index = 0, count = std::min<size_t>(records.size(), snapshot.propertyICs.size()); index < count; ++index) {
        const PropertyICRecord& record = records[index];
        RestoredPropertyIC expected = restoredPropertyIC(record);
        const PropertyICSnapshot& actual = snapshot.propertyICs[index];
        bool moldCanBeMegamorphic = index < molds.size() && molds[index].canBeMegamorphic;
        // Installation carries only ArrayLength from the mold's shape into m_cacheType.
        bool moldIsArrayLength = index < molds.size() && molds[index].preconfiguredCacheType == CacheType::ArrayLength;
        CacheType installedCacheType = moldIsArrayLength ? CacheType::ArrayLength : CacheType::Unset;

        expectField(mismatches, site, index, "accessType"_s, record.accessType, static_cast<uint8_t>(actual.accessType));
        expectField(mismatches, site, index, "cacheType"_s, static_cast<uint32_t>(installedCacheType), static_cast<uint32_t>(actual.cacheType));
        expectField(mismatches, site, index, "caseCount"_s, 0, actual.caseCount);
        expectField(mismatches, site, index, "megamorphicCaseListed"_s, false, actual.megamorphicCaseListed);
        expectField(mismatches, site, index, "everConsidered"_s, expected.everConsidered, actual.everConsidered);
        expectField(mismatches, site, index, "sawNonCell"_s, expected.sawNonCell, actual.sawNonCell);
        expectField(mismatches, site, index, "tookSlowPath"_s, expected.tookSlowPath, actual.tookSlowPath);
        expectField(mismatches, site, index, "resetByGC"_s, expected.resetByGC, actual.resetByGC);
        expectField(mismatches, site, index, "canBeMegamorphic"_s, moldCanBeMegamorphic || expected.foldsAtFirstCase, actual.canBeMegamorphic);
        expectField(mismatches, site, index, "holdsGaveUp"_s, expected.givenUp, actual.holdsGaveUp);
        expectField(mismatches, site, index, "countdown"_s, expected.countdown, actual.countdown);
        expectField(mismatches, site, index, "repatchCount"_s, expected.repatchCount, actual.repatchCount);
        expectField(mismatches, site, index, "numberOfCoolDowns"_s, expected.numberOfCoolDowns, actual.numberOfCoolDowns);
    }
}

static ASCIILiteral captureCheckField(CaptureCheck check)
{
    switch (check) {
    case CaptureCheck::NoBaselineJITData:
        return "capture.NoBaselineJITData"_s;
    case CaptureCheck::OutputSizeMismatch:
        return "capture.OutputSizeMismatch"_s;
    case CaptureCheck::MoldMismatch:
        return "capture.MoldMismatch"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral sectionCheckField(Check check)
{
    switch (check) {
    case Check::SectionSize:
        return "capture.SectionSize"_s;
    case Check::SummaryBound:
        return "capture.SummaryBound"_s;
    case Check::CallLinkGroups:
        return "capture.CallLinkGroups"_s;
    case Check::ReservedBits:
        return "capture.ReservedBits"_s;
    case Check::EnumRange:
        return "capture.EnumRange"_s;
    case Check::SummaryCount:
        return "capture.SummaryCount"_s;
    case Check::MoldPairing:
        return "capture.MoldPairing"_s;
    case Check::MetadataLayout:
        return "capture.MetadataLayout"_s;
    case Check::MoldMegamorphicBit:
        return "capture.MoldMegamorphicBit"_s;
    case Check::NewbornCallLinks:
        return "capture.NewbornCallLinks"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// I5: a strict capture of the CB, taken before any of its sites runs, writes for every IC and call-link site the
// recapture of the record it was restored from. A record that differs is reported at its own site and index, so a field
// that seeding or attach got wrong is reported twice at one index, once by I3 or I4 and once here; a capture that fails,
// or that holds another number of records, is reported as Capture.
static void checkRecapture(const PreparedBaselineICs& prepared, CodeBlock& codeBlock, Vector<TwinMismatch>& mismatches)
{
    constexpr auto captureSite = TwinMismatch::Site::Capture;
    // baselineICsSectionSize requires BaselineJITData; without it the strict capture reports NoBaselineJITData before it
    // reads the empty output.
    Vector<uint8_t> bytes(codeBlock.baselineJITData() ? baselineICsSectionSize(codeBlock) : 0);
    auto captured = captureBaselineICs(codeBlock, bytes.mutableSpan(), StrictChecks::Yes);
    if (!captured) {
        expectField(mismatches, captureSite, captured.error().siteIndex, captureCheckField(captured.error().check), false, true);
        return;
    }
    auto view = parseSection(bytes.span(), StrictChecks::Yes);
    if (!view) {
        expectField(mismatches, captureSite, view.error().siteIndex, sectionCheckField(view.error().check), false, true);
        return;
    }

    std::span<const PropertyICRecord> propertyICs = prepared.propertyICs();
    expectCount(mismatches, captureSite, "capture.propertyICCount"_s, propertyICs.size(), view->propertyICs.size());
    for (size_t index = 0, count = std::min(propertyICs.size(), view->propertyICs.size()); index < count; ++index) {
        constexpr auto site = TwinMismatch::Site::PropertyIC;
        PropertyICRecord expected = recapturedPropertyIC(propertyICs[index]);
        const PropertyICRecord& actual = view->propertyICs[index];
        expectField(mismatches, site, index, "recaptured.accessType"_s, expected.accessType, actual.accessType);
        expectField(mismatches, site, index, "recaptured.learningBits"_s, expected.learningBits, actual.learningBits);
        expectField(mismatches, site, index, "recaptured.repatchCount"_s, expected.repatchCount, actual.repatchCount);
        expectField(mismatches, site, index, "recaptured.numberOfCoolDowns"_s, expected.numberOfCoolDowns, actual.numberOfCoolDowns);
        expectField(mismatches, site, index, "recaptured.caseCount"_s, expected.caseCount, actual.caseCount);
        expectField(mismatches, site, index, "recaptured.stateBits"_s, expected.stateBits, actual.stateBits);
        expectField(mismatches, site, index, "recaptured.reserved"_s, expected.reserved[0] | (expected.reserved[1] << 8), actual.reserved[0] | (actual.reserved[1] << 8));
    }

    std::span<const CallLinkRecord> callLinks = prepared.callLinks();
    expectCount(mismatches, captureSite, "capture.callLinkSiteCount"_s, callLinks.size(), view->callLinks.size());
    for (size_t index = 0, count = std::min(callLinks.size(), view->callLinks.size()); index < count; ++index) {
        constexpr auto site = TwinMismatch::Site::CallLink;
        CallLinkRecord expected = recapturedCallLink(callLinks[index]);
        const CallLinkRecord& actual = view->callLinks[index];
        expectField(mismatches, site, index, "recaptured.bits"_s, expected.bits, actual.bits);
        expectField(mismatches, site, index, "recaptured.maxArgumentCountIncludingThisForVarargs"_s, expected.maxArgumentCountIncludingThisForVarargs, actual.maxArgumentCountIncludingThisForVarargs);
    }
}

// The names jitcacheICsSnapshot gives each enumerator. Each switch covers its enumeration without a default, so a new
// enumerator breaks the build until it is named here (section 11.1).

static ASCIILiteral accessTypeName(AccessType accessType)
{
    switch (accessType) {
#define JITCACHE_ICS_TWINS_ACCESS_TYPE_NAME(name) \
    case AccessType::name: \
        return #name ""_s;
        JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE(JITCACHE_ICS_TWINS_ACCESS_TYPE_NAME)
#undef JITCACHE_ICS_TWINS_ACCESS_TYPE_NAME
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral cacheTypeName(CacheType cacheType)
{
    switch (cacheType) {
    case CacheType::Unset:
        return "Unset"_s;
    case CacheType::GetByIdSelf:
        return "GetByIdSelf"_s;
    case CacheType::GetByIdPrototype:
        return "GetByIdPrototype"_s;
    case CacheType::PutByIdReplace:
        return "PutByIdReplace"_s;
    case CacheType::InByIdSelf:
        return "InByIdSelf"_s;
    case CacheType::Stub:
        return "Stub"_s;
    case CacheType::ArrayLength:
        return "ArrayLength"_s;
    case CacheType::StringLength:
        return "StringLength"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral summaryName(PropertyInlineCacheSummary summary)
{
    switch (summary) {
    case PropertyInlineCacheSummary::NoInformation:
        return "NoInformation"_s;
    case PropertyInlineCacheSummary::Simple:
        return "Simple"_s;
    case PropertyInlineCacheSummary::Megamorphic:
        return "Megamorphic"_s;
    case PropertyInlineCacheSummary::MakesCalls:
        return "MakesCalls"_s;
    case PropertyInlineCacheSummary::TakesSlowPath:
        return "TakesSlowPath"_s;
    case PropertyInlineCacheSummary::TakesSlowPathAndMakesCalls:
        return "TakesSlowPathAndMakesCalls"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral modeName(CallLinkInfo::Mode mode)
{
    switch (mode) {
    case CallLinkInfo::Mode::Init:
        return "Init"_s;
    case CallLinkInfo::Mode::Monomorphic:
        return "Monomorphic"_s;
    case CallLinkInfo::Mode::Polymorphic:
        return "Polymorphic"_s;
    case CallLinkInfo::Mode::Virtual:
        return "Virtual"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ASCIILiteral stateName(SuperConstructSnapshot::State state)
{
    switch (state) {
    case SuperConstructSnapshot::State::Empty:
        return "Empty"_s;
    case SuperConstructSnapshot::State::Single:
        return "Single"_s;
    case SuperConstructSnapshot::State::Multiple:
        return "Multiple"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static JSString* nameString(VM& vm, ASCIILiteral name)
{
    return jsString(vm, String { name });
}

static void putField(VM& vm, JSObject* object, ASCIILiteral name, JSValue value)
{
    object->putDirect(vm, Identifier::fromString(vm, name), value);
}

static JSObject* propertyICToJS(JSGlobalObject* globalObject, const PropertyICSnapshot& propertyIC)
{
    VM& vm = globalObject->vm();
    JSObject* object = constructEmptyObject(globalObject);
    putField(vm, object, "accessType"_s, nameString(vm, accessTypeName(propertyIC.accessType)));
    putField(vm, object, "bytecodeIndex"_s, jsNumber(propertyIC.bytecodeIndex));
    putField(vm, object, "cacheType"_s, nameString(vm, cacheTypeName(propertyIC.cacheType)));
    putField(vm, object, "summary"_s, nameString(vm, summaryName(propertyIC.summary)));
    putField(vm, object, "holdsGaveUp"_s, jsBoolean(propertyIC.holdsGaveUp));
    putField(vm, object, "caseCount"_s, jsNumber(propertyIC.caseCount));
    putField(vm, object, "megamorphicCaseListed"_s, jsBoolean(propertyIC.megamorphicCaseListed));
    putField(vm, object, "everConsidered"_s, jsBoolean(propertyIC.everConsidered));
    putField(vm, object, "sawNonCell"_s, jsBoolean(propertyIC.sawNonCell));
    putField(vm, object, "tookSlowPath"_s, jsBoolean(propertyIC.tookSlowPath));
    putField(vm, object, "resetByGC"_s, jsBoolean(propertyIC.resetByGC));
    putField(vm, object, "canBeMegamorphic"_s, jsBoolean(propertyIC.canBeMegamorphic));
    putField(vm, object, "countdown"_s, jsNumber(propertyIC.countdown));
    putField(vm, object, "repatchCount"_s, jsNumber(propertyIC.repatchCount));
    putField(vm, object, "numberOfCoolDowns"_s, jsNumber(propertyIC.numberOfCoolDowns));
    return object;
}

static JSObject* callLinkToJS(JSGlobalObject* globalObject, const CallLinkSnapshot& callLink)
{
    VM& vm = globalObject->vm();
    JSObject* object = constructEmptyObject(globalObject);
    putField(vm, object, "opcode"_s, nameString(vm, opcodeNames[callLink.opcodeID]));
    putField(vm, object, "metadataID"_s, jsNumber(callLink.metadataID));
    putField(vm, object, "mode"_s, nameString(vm, modeName(callLink.mode)));
    putField(vm, object, "seenOnce"_s, jsBoolean(callLink.seenOnce));
    putField(vm, object, "hasSeenClosure"_s, jsBoolean(callLink.hasSeenClosure));
    putField(vm, object, "clearedByGC"_s, jsBoolean(callLink.clearedByGC));
    putField(vm, object, "clearedByVirtual"_s, jsBoolean(callLink.clearedByVirtual));
    putField(vm, object, "maxArgumentCountIncludingThisForVarargs"_s, jsNumber(callLink.maxArgumentCountIncludingThisForVarargs));
    putField(vm, object, "hasStub"_s, jsBoolean(callLink.hasStub));
    putField(vm, object, "hasLastSeenCallee"_s, jsBoolean(callLink.hasLastSeenCallee));
    return object;
}

static JSObject* superConstructToJS(JSGlobalObject* globalObject, const SuperConstructSnapshot& superConstruct)
{
    VM& vm = globalObject->vm();
    JSObject* object = constructEmptyObject(globalObject);
    putField(vm, object, "opcode"_s, nameString(vm, opcodeNames[superConstruct.opcodeID]));
    putField(vm, object, "metadataID"_s, jsNumber(superConstruct.metadataID));
    putField(vm, object, "state"_s, nameString(vm, stateName(superConstruct.state)));
    putField(vm, object, "cachedCallee"_s, superConstruct.cachedCallee ? JSValue(superConstruct.cachedCallee) : jsNull());
    return object;
}

template<typename Element, typename ToJS>
static JSArray* arrayToJS(JSGlobalObject* globalObject, const Vector<Element>& elements, const ToJS& toJS)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSArray* array = constructEmptyArray(globalObject, nullptr);
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (const Element& element : elements) {
        array->push(globalObject, toJS(globalObject, element));
        RETURN_IF_EXCEPTION(scope, nullptr);
    }
    return array;
}

// { jitType, propertyICs, callLinks, superConstructs }, as section 11.1 spells it. Called inside the DeferGC scope that
// took the snapshot, so a cached callee stays valid until the result holds it.
static JSValue snapshotToJS(JSGlobalObject* globalObject, const BaselineICsSnapshot& snapshot)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSArray* propertyICs = arrayToJS(globalObject, snapshot.propertyICs, propertyICToJS);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* callLinks = arrayToJS(globalObject, snapshot.callLinks, callLinkToJS);
    RETURN_IF_EXCEPTION(scope, { });
    JSArray* superConstructs = arrayToJS(globalObject, snapshot.superConstructs, superConstructToJS);
    RETURN_IF_EXCEPTION(scope, { });

    JSObject* result = constructEmptyObject(globalObject);
    putField(vm, result, "jitType"_s, nameString(vm, JITCode::typeName(snapshot.jitType)));
    putField(vm, result, "propertyICs"_s, propertyICs);
    putField(vm, result, "callLinks"_s, callLinks);
    putField(vm, result, "superConstructs"_s, superConstructs);
    return result;
}

} // namespace ICTwinsInternal

BaselineICsSnapshot snapshotBaselineICs(CodeBlock& codeBlock)
{
    VM& vm = codeBlock.vm();
    BaselineICsSnapshot snapshot {
        .jitType = codeBlock.jitType(),
        .propertyICs = { },
        .callLinks = { },
        .superConstructs = { },
    };

    ConcurrentJSLocker locker(codeBlock.m_lock);
    if (BaselineJITData* jitData = codeBlock.baselineJITData()) {
        // BaselineJITData::create sizes the IC array with an unsigned count.
        unsigned count = static_cast<unsigned>(jitData->propertyInlineCaches().size());
        snapshot.propertyICs.reserveInitialCapacity(count);
        for (unsigned index = 0; index < count; ++index)
            snapshot.propertyICs.append(ICTwinsInternal::snapshotPropertyIC(locker, vm, jitData->propertyCache(index)));
    }
    forEachCallLinkSite(codeBlock, [&](unsigned canonicalPosition, unsigned metadataID, CallLinkInfo& callLinkInfo) {
        snapshot.callLinks.append(ICTwinsInternal::snapshotCallLink(locker, ICTwinsInternal::callLinkOpcodes[canonicalPosition], metadataID, callLinkInfo));
    });
    ICTwinsInternal::appendSuperConstructs<OpSuperConstruct>(codeBlock, snapshot.superConstructs);
    ICTwinsInternal::appendSuperConstructs<OpSuperConstructVarargs>(codeBlock, snapshot.superConstructs);
    return snapshot;
}

Vector<TwinMismatch> checkRestoredBaselineICs(const PreparedBaselineICs& prepared, CodeBlock& codeBlock)
{
    Vector<TwinMismatch> mismatches;
    {
        // The snapshot releases the CB's lock before the capture takes it again (L1).
        BaselineICsSnapshot snapshot = snapshotBaselineICs(codeBlock);
        ICTwinsInternal::checkSeededCallLinks(prepared, snapshot, mismatches);
        ICTwinsInternal::checkAttachedPropertyICs(prepared, codeBlock, snapshot, mismatches);
    }
    ICTwinsInternal::checkRecapture(prepared, codeBlock, mismatches);
    return mismatches;
}

JSC_DEFINE_HOST_FUNCTION(functionSnapshotBaselineICs, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* function = dynamicDowncast<JSFunction>(callFrame->argument(0));
    auto* executable = function ? dynamicDowncast<FunctionExecutable>(function->executable()) : nullptr;
    if (!executable)
        return throwVMTypeError(globalObject, scope, "jitcacheICsSnapshot expects a function with JS code as its first argument"_s);

    CodeSpecializationKind kind = CodeSpecializationKind::CodeForCall;
    JSValue kindValue = callFrame->argument(1);
    if (!kindValue.isUndefined()) {
        if (!kindValue.isString())
            return throwVMTypeError(globalObject, scope, "jitcacheICsSnapshot expects \"call\" or \"construct\" as its kind"_s);
        String kindName = kindValue.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (kindName == "construct"_s)
            kind = CodeSpecializationKind::CodeForConstruct;
        else if (kindName != "call"_s)
            return throwVMTypeError(globalObject, scope, "jitcacheICsSnapshot expects \"call\" or \"construct\" as its kind"_s);
    }

    // A super_construct cache's callee is weakly held and the snapshot copies it into a vector no collection scans, so
    // the result is built inside the deferral that took the snapshot: no collection reaches its End phase, where a dead
    // callee would be cleared and then swept, before the result holds the cell.
    DeferGC deferGC(vm);
    CodeBlock* codeBlock = executable->codeBlockFor(kind);
    if (!codeBlock)
        return JSValue::encode(jsUndefined());
    if (JITCode::isOptimizingJIT(codeBlock->jitType()))
        codeBlock = codeBlock->baselineAlternative();
    BaselineICsSnapshot snapshot = snapshotBaselineICs(*codeBlock);
    RELEASE_AND_RETURN(scope, JSValue::encode(ICTwinsInternal::snapshotToJS(globalObject, snapshot)));
}

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
