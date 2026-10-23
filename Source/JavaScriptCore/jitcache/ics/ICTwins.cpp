#include "config.h"
#include "jitcache/ics/ICTwins.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "ConcurrentJSLock.h"
#include "DeferGC.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JITCode.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "MetadataTable.h"
#include "ObjectConstructor.h"
#include "PropertyInlineCache.h"
#include "jitcache/ics/ICCapture.h"
#include "jitcache/ics/ICSection.h"
#include "jitcache/ics/ICSites.h"
#include <algorithm>
#include <array>
#include <span>

// The snapshot of a CB's IC and call-link state, the twin check that compares restored state with its records and with
// a fresh capture, and the shell function that returns a snapshot as a JS object.

namespace JSC::JITCache::ICs {

namespace ICTwinsInternal {

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

// Which CallSiteData a site points at: none for an inline site, and for a lazy site data of its own once
// LazyCallLinkInfo::get() finds some, the VM's executed-once data, or what linking gave it.
static CallSiteDataState dataStateOf(const CallLinkSite& site)
{
    if (site.inlineInfo)
        return CallSiteDataState::Inline;
    if (site.lazyInfo->get())
        return CallSiteDataState::Own;
    if (site.lazyInfo->hasExecutedOnce())
        return CallSiteDataState::ExecutedOnce;
    return CallSiteDataState::NotExecuted;
}

// A lazy site without data of its own holds no callee: its shared CallLinkInfo describes no site.
static CallLinkSnapshot snapshotCallLink(const ConcurrentJSLocker& locker, const CallLinkSite& site)
{
    CallLinkRecord record = readCallLinkRecord(locker, site);
    CallLinkInfo* callLinkInfo = site.ownCallLinkInfo();
    return CallLinkSnapshot {
        .opcodeID = site.opcode().opcodeID,
        .metadataID = site.metadataID,
        .data = dataStateOf(site),
        .mode = callLinkMode(static_cast<CallLinkModeCode>(record.bits & CallLinkBit::modeMask)),
        .seenOnce = hasBit(record.bits, CallLinkBit::seenOnce),
        .hasSeenClosure = hasBit(record.bits, CallLinkBit::hasSeenClosure),
        .clearedByGC = hasBit(record.bits, CallLinkBit::clearedByGC),
        .clearedByVirtual = hasBit(record.bits, CallLinkBit::clearedByVirtual),
        .maxArgumentCountIncludingThisForVarargs = record.maxArgumentCountIncludingThisForVarargs,
        .thisArrayModes = readThisArrayModes(record),
        .thisArrayProfileFlags = readThisArrayProfileFlags(record),
        .hasStub = callLinkInfo && callLinkInfo->stub(),
        .hasLastSeenCallee = callLinkInfo && callLinkInfo->haveLastSeenCallee(),
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

// After seeding, every call-link site points at the CallSiteData its record derives and reads as that derivation: the
// mode, seen bit, history bits, varargs maximum and this profile, with no callee beyond what linking, ensure or
// setVirtualCall left, so no stub and no last-seen callee. A site without a CallLinkInfo of its own reads as the
// derivation gives it too, since its record is then zero but for the seen bit of the executed-once data. The CB owns
// exactly one CallSiteData for each site that comes back with data of its own.
static void checkSeededCallLinks(const PreparedBaselineICs& prepared, CodeBlock& codeBlock, const BaselineICsSnapshot& snapshot, Vector<TwinMismatch>& mismatches)
{
    constexpr auto site = TwinMismatch::Site::CallLink;
    std::span<const CallLinkRecord> records = prepared.callLinks();
    expectCount(mismatches, site, "callLinkSiteCount"_s, records.size(), snapshot.callLinks.size());
    size_t ownSiteCount = 0;
    for (size_t index = 0, count = std::min<size_t>(records.size(), snapshot.callLinks.size()); index < count; ++index) {
        const CallLinkSnapshot& actual = snapshot.callLinks[index];
        // A snapshot names only call-link opcodes.
        const CallLinkOpcode& opcode = callLinkOpcodes[*canonicalCallLinkPosition(actual.opcodeID)];
        RestoredCallLink expected = restoredCallLink(opcode, records[index]);
        if (expected.data == CallSiteDataState::Own)
            ++ownSiteCount;
        auto expectedMode = expected.isVirtual ? CallLinkInfo::Mode::Virtual : CallLinkInfo::Mode::Init;
        expectField(mismatches, site, index, "data"_s, static_cast<uint32_t>(expected.data), static_cast<uint32_t>(actual.data));
        expectField(mismatches, site, index, "mode"_s, static_cast<uint32_t>(expectedMode), static_cast<uint32_t>(actual.mode));
        expectField(mismatches, site, index, "seenOnce"_s, expected.seenOnce, actual.seenOnce);
        expectField(mismatches, site, index, "hasSeenClosure"_s, expected.hasSeenClosure, actual.hasSeenClosure);
        expectField(mismatches, site, index, "clearedByGC"_s, expected.clearedByGC, actual.clearedByGC);
        expectField(mismatches, site, index, "clearedByVirtual"_s, expected.clearedByVirtual, actual.clearedByVirtual);
        expectField(mismatches, site, index, "maxArgumentCountIncludingThisForVarargs"_s, expected.maxArgumentCountIncludingThisForVarargs, actual.maxArgumentCountIncludingThisForVarargs);
        expectField(mismatches, site, index, "thisArrayModes"_s, expected.thisArrayModes, actual.thisArrayModes);
        expectField(mismatches, site, index, "thisArrayProfileFlags"_s, expected.thisArrayProfileFlags, actual.thisArrayProfileFlags);
        expectField(mismatches, site, index, "hasStub"_s, false, actual.hasStub);
        expectField(mismatches, site, index, "hasLastSeenCallee"_s, false, actual.hasLastSeenCallee);
    }
    // The newborn CB owned no CallSiteData before seeding, so every one it owns now came from seeding.
    MetadataTable* metadataTable = codeBlock.metadataTable();
    expectCount(mismatches, site, "ownCallSiteDatas"_s, ownSiteCount, metadataTable ? metadataTable->numberOfOwnCallSiteDatas() : 0);
}

// After attach, every IC holds the learning fields its record derives, the fold bit ORed into its mold's, the *GaveUp
// operation exactly when the site was given up, and, of the fields attach leaves alone, the access type of its record
// and the shape and empty case list installation built.
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

// The round trip: a capture of the CB, taken before any of its sites runs, writes for every IC and call-link site the
// recapture of the record it was restored from. A record that differs is reported at its own site and index, so a field
// that seeding or attach got wrong is reported twice at one index, once by checkSeededCallLinks or
// checkAttachedPropertyICs and once here; a capture that holds another number of records is reported as Capture.
static void checkRecapture(const PreparedBaselineICs& prepared, CodeBlock& codeBlock, Vector<TwinMismatch>& mismatches)
{
    constexpr auto captureSite = TwinMismatch::Site::Capture;
    Vector<uint8_t> bytes(baselineICsSectionSize(codeBlock));
    captureBaselineICs(codeBlock, bytes.mutableSpan());
    SectionView view = parseSection(bytes.span());

    std::span<const PropertyICRecord> propertyICs = prepared.propertyICs();
    expectCount(mismatches, captureSite, "capture.propertyICCount"_s, propertyICs.size(), view.propertyICs.size());
    for (size_t index = 0, count = std::min(propertyICs.size(), view.propertyICs.size()); index < count; ++index) {
        constexpr auto site = TwinMismatch::Site::PropertyIC;
        PropertyICRecord expected = recapturedPropertyIC(propertyICs[index]);
        const PropertyICRecord& actual = view.propertyICs[index];
        expectField(mismatches, site, index, "recaptured.accessType"_s, expected.accessType, actual.accessType);
        expectField(mismatches, site, index, "recaptured.learningBits"_s, expected.learningBits, actual.learningBits);
        expectField(mismatches, site, index, "recaptured.repatchCount"_s, expected.repatchCount, actual.repatchCount);
        expectField(mismatches, site, index, "recaptured.numberOfCoolDowns"_s, expected.numberOfCoolDowns, actual.numberOfCoolDowns);
        expectField(mismatches, site, index, "recaptured.caseCount"_s, expected.caseCount, actual.caseCount);
        expectField(mismatches, site, index, "recaptured.stateBits"_s, expected.stateBits, actual.stateBits);
    }

    // The captured groups give each captured record the opcode of its site; capture takes each group's opcode from
    // callLinkOpcodes.
    Vector<const CallLinkOpcode*> opcodes;
    opcodes.reserveInitialCapacity(view.callLinks.size());
    for (unsigned groupIndex = 0; groupIndex < view.header.callLinkGroupCount; ++groupIndex) {
        CallLinkGroup group = view.group(groupIndex);
        const CallLinkOpcode& opcode = callLinkOpcodes[*canonicalCallLinkPosition(static_cast<OpcodeID>(group.opcodeID))];
        for (uint32_t siteInGroup = 0; siteInGroup < group.siteCount; ++siteInGroup)
            opcodes.append(&opcode);
    }

    std::span<const CallLinkRecord> callLinks = prepared.callLinks();
    expectCount(mismatches, captureSite, "capture.callLinkSiteCount"_s, callLinks.size(), view.callLinks.size());
    for (size_t index = 0, count = std::min({ callLinks.size(), view.callLinks.size(), opcodes.size() }); index < count; ++index) {
        constexpr auto site = TwinMismatch::Site::CallLink;
        CallLinkRecord expected = recapturedCallLink(*opcodes[index], callLinks[index]);
        const CallLinkRecord& actual = view.callLinks[index];
        expectField(mismatches, site, index, "recaptured.bits"_s, expected.bits, actual.bits);
        expectField(mismatches, site, index, "recaptured.maxArgumentCountIncludingThisForVarargs"_s, expected.maxArgumentCountIncludingThisForVarargs, actual.maxArgumentCountIncludingThisForVarargs);
        expectField(mismatches, site, index, "recaptured.thisArrayModes"_s, readThisArrayModes(expected), readThisArrayModes(actual));
        expectField(mismatches, site, index, "recaptured.thisArrayProfileFlags"_s, readThisArrayProfileFlags(expected), readThisArrayProfileFlags(actual));
    }
}

// The names jitcacheICsSnapshot gives each enumerator. Each switch covers its enumeration without a default, so a new
// enumerator breaks the build until it is named here.

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

static ASCIILiteral dataStateName(CallSiteDataState data)
{
    switch (data) {
    case CallSiteDataState::Inline:
        return "Inline"_s;
    case CallSiteDataState::NotExecuted:
        return "NotExecuted"_s;
    case CallSiteDataState::ExecutedOnce:
        return "ExecutedOnce"_s;
    case CallSiteDataState::Own:
        return "Own"_s;
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
    putField(vm, object, "data"_s, nameString(vm, dataStateName(callLink.data)));
    putField(vm, object, "mode"_s, nameString(vm, modeName(callLink.mode)));
    putField(vm, object, "seenOnce"_s, jsBoolean(callLink.seenOnce));
    putField(vm, object, "hasSeenClosure"_s, jsBoolean(callLink.hasSeenClosure));
    putField(vm, object, "clearedByGC"_s, jsBoolean(callLink.clearedByGC));
    putField(vm, object, "clearedByVirtual"_s, jsBoolean(callLink.clearedByVirtual));
    putField(vm, object, "maxArgumentCountIncludingThisForVarargs"_s, jsNumber(callLink.maxArgumentCountIncludingThisForVarargs));
    putField(vm, object, "thisArrayModes"_s, jsNumber(callLink.thisArrayModes));
    putField(vm, object, "thisArrayProfileFlags"_s, jsNumber(callLink.thisArrayProfileFlags));
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

// { jitType, propertyICs, callLinks, superConstructs }. Called inside the DeferGC scope that took the snapshot, so a
// cached callee stays valid until the result holds it.
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
    forEachCallLinkSite(codeBlock, [&](const CallLinkSite& site) {
        snapshot.callLinks.append(ICTwinsInternal::snapshotCallLink(locker, site));
    });
    ICTwinsInternal::appendSuperConstructs<OpSuperConstruct>(codeBlock, snapshot.superConstructs);
    ICTwinsInternal::appendSuperConstructs<OpSuperConstructVarargs>(codeBlock, snapshot.superConstructs);
    return snapshot;
}

Vector<TwinMismatch> checkRestoredBaselineICs(const PreparedBaselineICs& prepared, CodeBlock& codeBlock)
{
    Vector<TwinMismatch> mismatches;
    {
        // The snapshot releases the CB's lock before the capture takes it again.
        BaselineICsSnapshot snapshot = snapshotBaselineICs(codeBlock);
        ICTwinsInternal::checkSeededCallLinks(prepared, codeBlock, snapshot, mismatches);
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
