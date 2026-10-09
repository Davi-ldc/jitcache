#include "config.h"
#include "JITCacheCBState.h"

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "IterationModeMetadata.h"
#include "JITCacheCBFormat.h"
#include "LazyOperandValueProfile.h"
#include "LazyValueProfile.h"
#include "Operands.h"
#include "ProducerBudget.h"
#include "SpeculatedType.h"
#include "ToThisStatus.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedMetadataTable.h"
#include "VM.h"
#include "ValueProfile.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

// Capture of a baseline CodeBlock's state (SPEC-cb.md sections 4.2 to 4.4). One walk reads each scored slot once, with
// the UCB copy it pairs with, and hands the values to a visitor: capture's writes the cb.state record and, from the same
// value, the cb.summary slot, and scores that slot; scoreLive's only scores. Both thus compute one score from the same
// reads, and capture's score is a function of the summary it wrote (I8).

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CBStateCapture);

namespace CBCaptureInternal {

// Step 1. Linking fixes the metadata counts, and only the VM thread appends a lazy-operand profile (N3), so the counts
// hold for the whole call, which runs on that thread with JS paused.
struct SlotCounts {
    uint32_t numArguments { 0 };
    uint32_t numValueProfiles { 0 };
    uint32_t numLazyOperandProfiles { 0 };
    uint32_t numArrayProfiles { 0 }; // A0..A15, which the native merge pairs with unlinkedArrayProfiles() (N2)
    CBFormat::FamilyEntryCounts familyEntryCount { };
};

static SlotCounts countSlots(CodeBlock& codeBlock)
{
    SlotCounts counts;
    counts.numArguments = codeBlock.numParameters();
    counts.numValueProfiles = codeBlock.unlinkedCodeBlock()->metadata().numValueProfiles();
    counts.familyEntryCount = CBFormat::familyEntryCounts(codeBlock);
    for (unsigned family = CBFormat::firstArrayProfileFamily; family < CBFormat::firstArrayProfileFamily + CBFormat::numberOfArrayProfileFamilies; ++family)
        counts.numArrayProfiles += counts.familyEntryCount[family];
#if ENABLE(DFG_JIT)
    codeBlock.lazyValueProfiles().forEachOperandValueProfile([&](const LazyOperandValueProfile&) {
        ++counts.numLazyOperandProfiles;
    });
#endif
    return counts;
}

// SC2, then SC3: the CB is eligible as the reads assume, and its UCB copies pair with its profiles as the native merges
// assume (N2). Neither dereferences baselineJITData() nor indexes a profile.
static std::optional<CBCheck> eligibilityFailure(CodeBlock& codeBlock, const SlotCounts& counts)
{
    if (codeBlock.jitType() != JITType::BaselineJIT
        || !codeBlock.baselineJITData()
        || codeBlock.isJettisoned()
        || codeBlock.m_didFailJITCompilation
        || codeBlock.argumentValueProfiles().size() != counts.numArguments)
        return CBCheck::CaptureStrict;

    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock.unlinkedCodeBlock();
    if (unlinkedCodeBlock.unlinkedValueProfiles().size() != static_cast<uint64_t>(counts.numArguments) + counts.numValueProfiles
        || unlinkedCodeBlock.unlinkedArrayProfiles().size() != counts.numArrayProfiles)
        return CBCheck::CapturePairing;
    return std::nullopt;
}

// Step 2 of capture and scoreLive. With strict on a failure is a recording fault, since the CB is the producer's own
// state; in normal mode the glue has already decided eligibility, so both checks are assertions.
static std::optional<CBFault> checkCodeBlock(CodeBlock& codeBlock, const SlotCounts& counts, bool strict)
{
    if (!strict) {
        ASSERT(!eligibilityFailure(codeBlock, counts));
        return std::nullopt;
    }
    if (auto check = eligibilityFailure(codeBlock, counts))
        return CBFault { CBFaultKind::RecordingFault, *check };
    return std::nullopt;
}

// P12 as I16 states it: the live triple when the counter travels, and zeros, with no counter field read, when the
// polymorphic rule withholds it.
struct CounterRecord {
    CBFormat::CounterMode mode { CBFormat::CounterMode::NotCarried };
    int32_t value { 0 };
    float totalCount { 0 };
    int32_t activeThreshold { 0 };
};

static CounterRecord readCounter(CodeBlock& codeBlock, bool hasPolymorphicSite)
{
    if (hasPolymorphicSite)
        return { };
    // Each field once, with the plain loads native readers use. A DFG plan that finishes meanwhile may zero m_counter,
    // which costs only precision (section 4.1).
    const BaselineExecutionCounter& counter = codeBlock.baselineJITData()->executeCounter();
    return { CBFormat::CounterMode::Carried, counter.m_counter, counter.m_totalCount, counter.m_activeThreshold };
}

// Section 4.3's counterProgress: THREAD Maintenance's P, floored at zero and capped at the active threshold. The negated
// comparisons send a NaN to zero, and the clamp keeps a negative threshold, which no native counter holds and V15 rejects
// with strict on, from converting a negative double.
static uint32_t counterProgress(const CounterRecord& counter)
{
    if (counter.mode == CBFormat::CounterMode::NotCarried)
        return 0;
    double progress = static_cast<double>(counter.totalCount) + counter.value;
    if (!(progress > 0))
        return 0;
    double capped = std::min(std::floor(progress), static_cast<double>(counter.activeThreshold));
    if (!(capped > 0))
        return 0;
    return static_cast<uint32_t>(capped);
}

// The summary's array flag at a position: the CB flags ORed with its UCB copy's (section 3.3). Native flags fit V7's eight
// bits.
static uint8_t summaryArrayFlags(OptionSet<ArrayProfileFlag> flags, OptionSet<ArrayProfileFlag> unlinkedFlags)
{
    return static_cast<uint8_t>((flags | unlinkedFlags).toRaw());
}

// Section 4.3's richnessUnits, added slot by slot from the values cb.summary holds: one unit per category bit of an
// argument, value or lazy-operand slot, and one per array flag other than the pruning mark.
class Richness {
public:
    void addCategories(SpeculatedType categories) { m_units += std::popcount(static_cast<uint64_t>(categories)); }
    void addArrayFlags(uint8_t flags) { m_units += std::popcount(static_cast<unsigned>(flags & ~pruningMark)); }
    uint64_t units() const { return m_units; }

private:
    // Every flag fits eight bits (CBFormat::allArrayProfileFlags).
    static constexpr uint8_t pruningMark = static_cast<uint8_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

    uint64_t m_units { 0 };
};

static CBScore scoreOf(const Richness& richness, const CounterRecord& counter)
{
    return { richness.units(), counter.mode == CBFormat::CounterMode::NotCarried, counterProgress(counter) };
}

// Walks the slots the score counts, in the native merges' order (N2, I11, I12), and hands the visitor each CB value with
// the UCB copy it pairs with: argument i with UCB value profile i, value offset k with UCB value profile
// numArguments + k - 1, each lazy-operand profile in the holder's order (it has no UCB copy), and array position p, the
// running index over A0..A15, with UCB array profile p. Each field is read once (section 4.2). The vectors are indexed
// through std::span's hardened operator[], as the native merges index the UCB copies (SC3).
template<typename Visitor>
static void forEachScoredSlot(CodeBlock& codeBlock, const SlotCounts& counts, Visitor& visitor)
{
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock.unlinkedCodeBlock();
    std::span<const UnlinkedValueProfile> unlinkedValueProfiles = unlinkedCodeBlock.unlinkedValueProfiles().span();
    std::span<const UnlinkedArrayProfile> unlinkedArrayProfiles = unlinkedCodeBlock.unlinkedArrayProfiles().span();

    // P2.
    std::span<const ArgumentValueProfile> argumentProfiles = codeBlock.argumentValueProfiles().span();
    for (unsigned index = 0; index < counts.numArguments; ++index)
        visitor.argument(index, argumentProfiles[index].m_prediction, unlinkedValueProfiles[index].prediction());

    // P1.
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, ValueProfile& profile) {
        unsigned index = offset - 1;
        visitor.value(index, profile.m_prediction, unlinkedValueProfiles[counts.numArguments + index].prediction());
    });

    // P3: key() and m_prediction only, never the buckets (I2).
#if ENABLE(DFG_JIT)
    unsigned lazyIndex = 0;
    codeBlock.lazyValueProfiles().forEachOperandValueProfile([&](const LazyOperandValueProfile& profile) {
        visitor.lazyOperand(lazyIndex++, profile.key(), profile.m_prediction);
    });
#endif

    // P4: the accumulated modes and flags, never the StructureID samples (I2).
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        if constexpr (FamilyType::kind == CBFormat::FamilyKind::ArrayProfile) {
            size_t position = CBFormat::firstRecordOfFamily(counts.familyEntryCount, FamilyType::index);
            CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](ArrayProfile& profile) {
                visitor.arrayProfile(position, profile.observedArrayModes(), profile.arrayProfileFlags(), unlinkedArrayProfiles[position].arrayProfileFlags());
                ++position;
            });
        }
    });
}

// scoreLive's visitor: scores the values cb.summary would hold and writes nothing.
class LiveScorer {
public:
    void argument(unsigned, SpeculatedType prediction, SpeculatedType unlinkedPrediction) { m_richness.addCategories(prediction | unlinkedPrediction); }
    void value(unsigned, SpeculatedType prediction, SpeculatedType unlinkedPrediction) { m_richness.addCategories(prediction | unlinkedPrediction); }
    void lazyOperand(unsigned, const LazyOperandValueProfileKey&, SpeculatedType prediction) { m_richness.addCategories(prediction); }
    void arrayProfile(size_t, ArrayModes, OptionSet<ArrayProfileFlag> flags, OptionSet<ArrayProfileFlag> unlinkedFlags) { m_richness.addArrayFlags(summaryArrayFlags(flags, unlinkedFlags)); }

    const Richness& richness() const { return m_richness; }

private:
    Richness m_richness;
};

template<typename T>
static void store(std::span<uint8_t> section, size_t offset, const T& value)
{
    memcpySpan(section.subspan(offset, sizeof(T)), asByteSpan(value));
}

// capture's visitor: writes each slot's cb.state record and, from the same value, its cb.summary slot, and scores that
// slot, so the summary holds no second read of the live CB (I8). The buffers start zeroed, so every reserved field and
// padding byte the layouts leave unwritten is zero (I10).
class SectionWriter {
public:
    SectionWriter(std::span<uint8_t> state, const CBFormat::StateLayout& stateLayout, std::span<uint8_t> summary, const CBFormat::SummaryLayout& summaryLayout)
        : m_state(state)
        , m_stateLayout(stateLayout)
        , m_summary(summary)
        , m_summaryLayout(summaryLayout)
    {
    }

    void argument(unsigned index, SpeculatedType prediction, SpeculatedType unlinkedPrediction)
    {
        writeState(CBFormat::StateArray::ArgumentPredictions, index, static_cast<uint64_t>(prediction));
        writeCategories(CBFormat::SummaryArray::ArgumentCategories, index, prediction | unlinkedPrediction);
    }

    void value(unsigned index, SpeculatedType prediction, SpeculatedType unlinkedPrediction)
    {
        writeState(CBFormat::StateArray::ValuePredictions, index, static_cast<uint64_t>(prediction));
        writeCategories(CBFormat::SummaryArray::ValueCategories, index, prediction | unlinkedPrediction);
    }

    void lazyOperand(unsigned index, const LazyOperandValueProfileKey& key, SpeculatedType prediction)
    {
        Operand operand = key.operand();
        CBFormat::LazyOperandRecord record {
            .bytecodeIndexBits = key.bytecodeIndex().asBits(),
            .operandKind = static_cast<uint32_t>(operand.kind()),
            .operandValue = operand.value(),
            .reserved = 0,
            .prediction = static_cast<uint64_t>(prediction),
        };
        writeState(CBFormat::StateArray::LazyOperandProfiles, index, record);
        writeCategories(CBFormat::SummaryArray::LazyOperandCategories, index, prediction);
    }

    void arrayProfile(size_t position, ArrayModes modes, OptionSet<ArrayProfileFlag> flags, OptionSet<ArrayProfileFlag> unlinkedFlags)
    {
        writeState(CBFormat::StateArray::ArrayProfiles, position, CBFormat::ArrayProfileRecord { .observedArrayModes = modes, .flags = flags.toRaw() });
        uint8_t categories = summaryArrayFlags(flags, unlinkedFlags);
        ASSERT(position < m_summaryLayout.count(CBFormat::SummaryArray::ArrayFlags));
        store(m_summary, m_summaryLayout.elementOffset(CBFormat::SummaryArray::ArrayFlags, position), categories);
        m_richness.addArrayFlags(categories);
    }

    // Element index of the array, whose element type the record's size matches.
    template<typename Record>
    void writeState(CBFormat::StateArray array, size_t index, const Record& record)
    {
        ASSERT(sizeof(Record) == CBFormat::elementSize(array));
        ASSERT(index < m_stateLayout.count(array));
        store(m_state, m_stateLayout.elementOffset(array, index), record);
    }

    const Richness& richness() const { return m_richness; }

private:
    void writeCategories(CBFormat::SummaryArray array, size_t index, SpeculatedType categories)
    {
        ASSERT(index < m_summaryLayout.count(array));
        store(m_summary, m_summaryLayout.elementOffset(array, index), static_cast<uint64_t>(categories));
        m_richness.addCategories(categories);
    }

    std::span<uint8_t> m_state;
    const CBFormat::StateLayout& m_stateLayout;
    std::span<uint8_t> m_summary;
    const CBFormat::SummaryLayout& m_summaryLayout;
    Richness m_richness;
};

// The cb.state record of one entry of an unscored family (P5 to P9), read as section 4.2 states: the allocation hint
// through the concurrent readers, which never read the last array (I2), and every other field as stored.
template<CBFormat::FamilyKind kind, typename Field>
static auto unscoredRecord(Field& field)
{
    if constexpr (kind == CBFormat::FamilyKind::AllocationHint)
        return CBFormat::encodeAllocationHint(field.selectIndexingTypeConcurrently(), field.vectorLengthHintConcurrently());
    else if constexpr (kind == CBFormat::FamilyKind::IterationModes)
        return static_cast<uint16_t>(field.seenModes);
    else if constexpr (kind == CBFormat::FamilyKind::EnumeratorModes)
        return static_cast<uint8_t>(field);
    else if constexpr (kind == CBFormat::FamilyKind::ToThisStatus)
        return static_cast<uint8_t>(field);
    else {
        static_assert(kind == CBFormat::FamilyKind::BranchBit);
        return static_cast<uint8_t>(field);
    }
}

// P5 to P9: each family's entries in metadata-ID order, from the family's first record in its kind's array.
static void writeUnscoredFamilies(CodeBlock& codeBlock, const SlotCounts& counts, SectionWriter& writer)
{
    CBFormat::forEachFamily([&](auto family) {
        using FamilyType = decltype(family);
        if constexpr (FamilyType::kind != CBFormat::FamilyKind::ArrayProfile) {
            size_t index = CBFormat::firstRecordOfFamily(counts.familyEntryCount, FamilyType::index);
            CBFormat::forEachFamilyField<FamilyType>(codeBlock, [&](auto& field) {
                constexpr CBFormat::StateArray array = CBFormat::stateArrayOf(FamilyType::kind);
                auto record = unscoredRecord<FamilyType::kind>(field);
                static_assert(sizeof(record) == CBFormat::elementSize(array));
                writer.writeState(array, index++, record);
            });
        }
    });
}

} // namespace CBCaptureInternal

std::expected<CBStateCapture, CBFault> CBStateCapture::capture(CodeBlock& codeBlock, ProducerBudget& budget, bool strict, bool hasPolymorphicSite)
{
    using namespace CBCaptureInternal;

    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    AssertNoGC assertNoGC;

    // Step 1.
    SlotCounts counts = countSlots(codeBlock);

    // Step 2, before any read beyond the counts.
    if (auto fault = checkCodeBlock(codeBlock, counts, strict))
        return std::unexpected(*fault);

    // Step 3. The layouts depend only on the counts. A size past size_t is a charge no budget can hold.
    CBFormat::StateHeader stateHeader { };
    stateHeader.layoutVersion = CBFormat::stateLayoutVersion;
    stateHeader.tier = static_cast<uint8_t>(CBFormat::Tier::Baseline);
    stateHeader.numArguments = counts.numArguments;
    stateHeader.numValueProfiles = counts.numValueProfiles;
    stateHeader.numLazyOperandProfiles = counts.numLazyOperandProfiles;
    stateHeader.familyEntryCount = counts.familyEntryCount;

    CBFormat::SummaryHeader summaryHeader { };
    summaryHeader.layoutVersion = CBFormat::summaryLayoutVersion;
    summaryHeader.tier = static_cast<uint8_t>(CBFormat::Tier::Baseline);
    summaryHeader.numArguments = counts.numArguments;
    summaryHeader.numValueProfiles = counts.numValueProfiles;
    summaryHeader.numArrayProfiles = counts.numArrayProfiles;
    summaryHeader.numLazyOperandProfiles = counts.numLazyOperandProfiles;

    auto stateLayout = CBFormat::stateLayout(stateHeader);
    auto summaryLayout = CBFormat::summaryLayout(summaryHeader);
    if (!stateLayout || !summaryLayout)
        return std::unexpected(CBFault { CBFaultKind::RecordingFault, CBCheck::CaptureCharge });
    CheckedSize chargedBytes = stateLayout->size;
    chargedBytes += summaryLayout->size;
    if (chargedBytes.hasOverflowed() || !budget.tryCharge(chargedBytes.value()))
        return std::unexpected(CBFault { CBFaultKind::RecordingFault, CBCheck::CaptureCharge });

    // The two buffers, zeroed, the only memory capture allocates. The object owns them and the charge from here on, so
    // every later return that drops it frees both and releases the charge.
    ASSERT(!(stateLayout->size % sizeof(uint64_t)) && !(summaryLayout->size % sizeof(uint64_t)));
    CBStateCapture result(budget,
        makeUniqueArray<uint64_t>(stateLayout->size / sizeof(uint64_t)), stateLayout->size,
        makeUniqueArray<uint64_t>(summaryLayout->size / sizeof(uint64_t)), summaryLayout->size,
        CBScore { });
    std::span<uint8_t> state = asMutableByteSpan(unsafeMakeSpan(result.m_state.get(), result.m_stateSize / sizeof(uint64_t)));
    std::span<uint8_t> summary = asMutableByteSpan(unsafeMakeSpan(result.m_summary.get(), result.m_summarySize / sizeof(uint64_t)));

    // Step 4: the tier-up history and the counter as I16 states, then every slot.
    CounterRecord counter = readCounter(codeBlock, hasPolymorphicSite);
    stateHeader.counterMode = static_cast<uint8_t>(counter.mode);
    stateHeader.optimizationDelayCounter = static_cast<uint16_t>(codeBlock.optimizationDelayCounter());
    stateHeader.reoptimizationRetryCounter = static_cast<uint16_t>(codeBlock.reoptimizationRetryCounter());
    stateHeader.counterValue = counter.value;
    stateHeader.counterTotalCount = counter.totalCount;
    stateHeader.counterActiveThreshold = counter.activeThreshold;
    CBCaptureInternal::store(state, 0, stateHeader);

    // Step 5, in the same walk: the summary's counter fields come from the same record, and each slot from the value
    // just written to cb.state and the UCB copy read beside it.
    summaryHeader.counterMode = stateHeader.counterMode;
    summaryHeader.counterProgress = counterProgress(counter);
    CBCaptureInternal::store(summary, 0, summaryHeader);

    SectionWriter writer(state, *stateLayout, summary, *summaryLayout);
    forEachScoredSlot(codeBlock, counts, writer);
    writeUnscoredFamilies(codeBlock, counts, writer);
    result.m_score = scoreOf(writer.richness(), counter);

    // Step 6: SC1. The checks read the bytes in place and allocate nothing; a failure drops the object.
    if (strict) {
        if (validateState(result.stateSection(), codeBlock)
            || !counterObeysNativeInvariant(stateHeader)
            || validateSummary(result.summarySection()))
            return std::unexpected(CBFault { CBFaultKind::RecordingFault, CBCheck::CaptureStrict });
    }

    return result;
}

std::expected<CBScore, CBFault> CBStateCapture::scoreLive(CodeBlock& codeBlock, bool strict, bool hasPolymorphicSite)
{
    using namespace CBCaptureInternal;

    ASSERT(codeBlock.vm().currentThreadIsHoldingAPILock());
    AssertNoGC assertNoGC;

    SlotCounts counts = countSlots(codeBlock);
    if (auto fault = checkCodeBlock(codeBlock, counts, strict))
        return std::unexpected(*fault);

    CounterRecord counter = readCounter(codeBlock, hasPolymorphicSite);
    LiveScorer scorer;
    forEachScoredSlot(codeBlock, counts, scorer);
    return scoreOf(scorer.richness(), counter);
}

CBStateCapture::CBStateCapture(CBStateCapture&& other)
    : m_budget(std::exchange(other.m_budget, nullptr))
    , m_state(WTF::move(other.m_state))
    , m_summary(WTF::move(other.m_summary))
    , m_stateSize(std::exchange(other.m_stateSize, 0))
    , m_summarySize(std::exchange(other.m_summarySize, 0))
    , m_score(other.m_score)
{
}

CBStateCapture::~CBStateCapture()
{
    // The buffers go before the charge that paid for them, so the budget never counts less than is allocated.
    m_state = nullptr;
    m_summary = nullptr;
    if (m_budget)
        m_budget->release(m_stateSize + m_summarySize);
}

const CBScore& CBStateCapture::score() const
{
    return m_score;
}

std::span<const uint8_t> CBStateCapture::stateSection() const
{
    return asByteSpan(unsafeMakeSpan(m_state.get(), m_stateSize / sizeof(uint64_t)));
}

std::span<const uint8_t> CBStateCapture::summarySection() const
{
    return asByteSpan(unsafeMakeSpan(m_summary.get(), m_summarySize / sizeof(uint64_t)));
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
