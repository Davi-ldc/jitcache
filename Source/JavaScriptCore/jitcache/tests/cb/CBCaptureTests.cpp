#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BytecodeIndex.h"
#include "CodeBlock.h"
#include "CollectionScope.h"
#include "FunctionExecutable.h"
#include "Heap.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LazyOperandValueProfile.h"
#include "LazyValueProfile.h"
#include "Operands.h"
#include "SpeculatedType.h"
#include "VirtualRegister.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/cb/CBFormat.h"
#include "jitcache/cb/CBState.h"
#include "jitcache/tests/cb/CBTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestCodeBlocks.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

// Tests of the CB lane's capture (cb/CBCapture.cpp, CBStateCapture in cb/CBState.h): how the polymorphic bit decides
// whether the counter travels, in both sections and in the scores, the ownership a CBStateCapture has of its two
// sections and their budget charge, and that capture leaves a pending lazy-operand sample where it found it. Each test
// runs on a fresh VM with default options and builds its CB as the ICs lane's live tests do: it evaluates its source in
// its own global object, and one call through JSC::call installs an LLInt CB that JIT::compileSync then compiles to
// baseline. The source keeps every value the test passes in its globals, and the global object stays reachable from
// the test's stack.

namespace JSC::JITCache::Tests {

namespace CBCaptureTestsInternal {

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

// The counterProgress of a carried triple, computed here from the formula the capture applies.
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

static constexpr uint8_t notCarried = static_cast<uint8_t>(CBFormat::CounterMode::NotCarried);
static constexpr uint8_t carried = static_cast<uint8_t>(CBFormat::CounterMode::Carried);

} // namespace CBCaptureTestsInternal

using namespace CBCaptureTestsInternal;

// On one baseline CB whose counter has made progress, a capture with the polymorphic bit set writes NotCarried in both
// sections, zeros in the three counter fields and no progress, and its score and scoreLive's withhold the
// counter; without the bit, the capture writes Carried in both sections with the live triple, and both scores carry
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
        CBScore liveScore = CBStateCapture::scoreLive(*codeBlock, hasPolymorphicSite);
        auto captured = CBStateCapture::capture(*codeBlock, budget.get(), hasPolymorphicSite);
        if (!captured) {
            JITCACHE_FAIL("capture refused its charge"_s);
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
            JITCACHE_CHECK(liveScore.counterWithheld);
            JITCACHE_CHECK(!liveScore.counterProgress);
        } else {
            JITCACHE_CHECK(stateHeader->counterMode == carried);
            JITCACHE_CHECK(summaryHeader->counterMode == carried);
            JITCACHE_CHECK(stateHeader->counterValue == live.value);
            JITCACHE_CHECK(stateHeader->counterTotalCount == live.totalCount);
            JITCACHE_CHECK(stateHeader->counterActiveThreshold == live.activeThreshold);
            JITCACHE_CHECK(summaryHeader->counterProgress == progress);
            JITCACHE_CHECK(!score.counterWithheld);
            JITCACHE_CHECK(score.counterProgress == progress);
            JITCACHE_CHECK(!liveScore.counterWithheld);
            JITCACHE_CHECK(liveScore.counterProgress == progress);
        }
    }

    JITCACHE_CHECK(liveCounter(*codeBlock) == live);
}

// While a capture lives, the budget holds exactly its two section sizes, which are the sizes the format computes from
// their headers; a move hands the sections and the charge to the new object, leaving the old one empty and releasing
// nothing; the last owner's destruction releases the charge. A budget too small for the two sections refuses the charge,
// and capture returns nothing, with nothing charged.
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
        auto captured = CBStateCapture::capture(*codeBlock, budget.get(), false);
        if (!captured) {
            JITCACHE_FAIL("capture refused its charge"_s);
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
    auto refused = CBStateCapture::capture(*codeBlock, smallBudget.get(), false);
    JITCACHE_CHECK(!refused);
    JITCACHE_CHECK(!smallBudget->chargedBytes());
    JITCACHE_CHECK(smallBudget->hasRefused());
}

#if ENABLE(DFG_JIT)

// Scoring and capture read each lazy-operand profile's key and prediction and leave its pending sample alone, since
// the sample may name a cell that has died. The test adds a lazy-operand profile to the warm baseline CB, with a
// prediction that lacks int32 and an int32 sample in its bucket, then scores the CB with scoreLive, as production
// scores every capture candidate, and captures it. Afterwards the sample is still pending, the live prediction is
// unchanged, and the captured record holds that prediction without int32. With no JS running and no compile plan
// holding the CB, only a collection could drain the sample besides scoring and capture, as early as a marker's first
// visit to the CB (CodeBlock::visitChildren). So the test runs a synchronous full collection, which ends any collection
// in progress, before it writes the sample, and allocates no cell until its checks, so no other collection starts.
JITCACHE_TEST(cbCaptureLeavesAPendingLazyOperandSample, Yes)
{
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = createRealm(context, vm, elementSource);
    if (!globalObject)
        return;
    CodeBlock* codeBlock = warmBaselineElement(context, globalObject);
    if (!codeBlock)
        return;
    JSFunction* element = globalFunction(context, globalObject, "element"_s);
    if (!element)
        return;

    vm.heap.collectNow(Sync, CollectionScope::Full);
    if (codeBlock->jitType() != JITType::BaselineJIT || element->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("the collection replaced element's baseline CB"_s);
        return;
    }

    const LazyOperandValueProfileKey key(BytecodeIndex(0), Operand(virtualRegisterForArgumentIncludingThis(1)));
    constexpr SpeculatedType prediction = SpecString;
    const EncodedJSValue pendingSample = JSValue::encode(jsNumber(42));
    LazyOperandValueProfile* profile = codeBlock->lazyValueProfiles().addOperandValueProfile(key);
    profile->m_prediction = prediction;
    *profile->specFailBucket(0) = pendingSample;

    CBStateCapture::scoreLive(*codeBlock, false);
    auto image = captureState(context, *codeBlock);
    JITCACHE_CHECK(*profile->specFailBucket(0) == pendingSample);
    JITCACHE_CHECK(profile->m_prediction == prediction);
    if (!image)
        return;
    if (image->lazyOperandProfiles.size() != 1) {
        JITCACHE_FAIL(makeString("the capture holds "_s, image->lazyOperandProfiles.size(), " lazy-operand records, not the one the test added"_s));
        return;
    }
    const CBFormat::LazyOperandRecord& record = image->lazyOperandProfiles[0];
    JITCACHE_CHECK(record.bytecodeIndexBits == key.bytecodeIndex().asBits());
    JITCACHE_CHECK(record.operandKind == static_cast<uint32_t>(key.operand().kind()));
    JITCACHE_CHECK(record.operandValue == key.operand().value());
    JITCACHE_CHECK(record.prediction == prediction);
}

#endif // ENABLE(DFG_JIT)

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
