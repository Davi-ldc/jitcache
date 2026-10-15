#include "config.h"
#include "CBState.h"

#if ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "BaselineJITCode.h"
#include "ByteFields.h"
#include "CBFormat.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "ExecutionCounter.h"
#include "IterationModeMetadata.h"
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
#include <initializer_list>
#include <utility>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

// Capture of a baseline CodeBlock's state into cb.state and cb.summary, and the score of a body, live or saved. One walk
// reads each scored slot once, with the UCB copy it pairs with, and hands the values to a visitor: capture's writes the
// cb.state record and, from the same value, the cb.summary slot, and scores that slot; scoreLive's only scores.
// decodeSummary scores a saved body from its cb.summary alone, since scoring a saved body has no CodeBlock. All three
// count richness through Richness and build the score through scoreOf, so the score capture returns is the one
// decodeSummary later reads from the summary capture wrote.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(CBStateCapture);

namespace CBCaptureInternal {

// Linking fixes the metadata counts, and only the VM thread appends a lazy-operand profile, so the counts hold for the
// whole call, which runs on that thread with JS paused.
struct SlotCounts {
    uint32_t numArguments { 0 };
    uint32_t numValueProfiles { 0 };
    uint32_t numLazyOperandProfiles { 0 };
    uint32_t numArrayProfiles { 0 }; // A0..A15, which the native merge pairs with unlinkedArrayProfiles()
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

// First that the CB is eligible as the reads assume, then that its UCB copies pair with its profiles as the native
// merges assume: they walk the CB's profiles with a running index into the UCB vectors and check nothing else. Neither
// check dereferences baselineJITData() nor indexes a profile.
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

// The checks capture and scoreLive run before any read beyond the counts. With strict on a failure is a recording fault,
// since the CB is the producer's own state; in normal mode the glue has already decided eligibility, so both checks are
// assertions.
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

// The baseline counter as capture records it: the live triple when the counter travels, and zeros, with no counter field
// read, when a polymorphic site withholds it.
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
    // which costs only precision.
    const BaselineExecutionCounter& counter = codeBlock.baselineJITData()->executeCounter();
    return { CBFormat::CounterMode::Carried, counter.m_counter, counter.m_totalCount, counter.m_activeThreshold };
}

// The progress the score and cb.summary hold, which the glue also stamps in the body's envelope: m_totalCount +
// m_counter, floored at zero and capped at the active threshold, and 0 when the counter does not travel. The negated
// comparisons send a NaN to zero, and the clamp keeps a negative threshold, which no native counter holds and strict
// validation rejects, from converting a negative double.
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

// The summary's array flag at a position: the CB flags ORed with its UCB copy's. Every native flag fits a byte
// (CBFormat::allArrayProfileFlags).
static uint8_t summaryArrayFlags(OptionSet<ArrayProfileFlag> flags, OptionSet<ArrayProfileFlag> unlinkedFlags)
{
    return static_cast<uint8_t>((flags | unlinkedFlags).toRaw());
}

// First-run pruning sets this flag when it discards modes, which says nothing new about what the site met, so richness
// leaves it out.
static constexpr uint8_t pruningMark = static_cast<uint8_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

// The unit rule of richness, applied slot by slot to the values cb.summary holds: one unit per category bit of an
// argument, value or lazy-operand slot, and one per array flag other than the pruning mark. A category is one bit of a
// SpeculatedType.
class Richness {
public:
    void addCategories(SpeculatedType categories) { m_units += std::popcount(static_cast<uint64_t>(categories)); }
    void addArrayFlags(uint8_t flags) { m_units += std::popcount(static_cast<unsigned>(flags & ~pruningMark)); }
    uint64_t units() const { return m_units; }

private:
    uint64_t m_units { 0 };
};

// A score from its richness and the two counter fields of cb.summary's header. A withheld counter is scored apart from a
// zero progress, because a capture whose progress a polymorphic site withholds ranks above one whose progress travels.
static CBScore scoreOf(const Richness& richness, uint8_t counterMode, uint32_t progress)
{
    return { richness.units(), counterMode == static_cast<uint8_t>(CBFormat::CounterMode::NotCarried), progress };
}

// Walks the slots the score counts, in the native merges' order, and hands the visitor each CB value with the UCB copy it
// pairs with: argument i with UCB value profile i, value offset k with UCB value profile numArguments + k - 1, each
// lazy-operand profile in the holder's order (it has no UCB copy), and array position p, the running index over
// A0..A15, with UCB array profile p. Each field is read once. The vectors are indexed through std::span's hardened
// operator[], as the native merges index the UCB copies, so a broken pairing that strict did not catch fails as it would
// natively.
template<typename Visitor>
static void forEachScoredSlot(CodeBlock& codeBlock, const SlotCounts& counts, Visitor& visitor)
{
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock.unlinkedCodeBlock();
    std::span<const UnlinkedValueProfile> unlinkedValueProfiles = unlinkedCodeBlock.unlinkedValueProfiles().span();
    std::span<const UnlinkedArrayProfile> unlinkedArrayProfiles = unlinkedCodeBlock.unlinkedArrayProfiles().span();

    // Argument profiles.
    std::span<const ArgumentValueProfile> argumentProfiles = codeBlock.argumentValueProfiles().span();
    for (unsigned index = 0; index < counts.numArguments; ++index)
        visitor.argument(index, argumentProfiles[index].m_prediction, unlinkedValueProfiles[index].prediction());

    // The value profiles in front of the metadata table.
    CBFormat::forEachMetadataValueProfile(codeBlock, [&](unsigned offset, ValueProfile& profile) {
        unsigned index = offset - 1;
        visitor.value(index, profile.m_prediction, unlinkedValueProfiles[counts.numArguments + index].prediction());
    });

    // Lazy-operand profiles: key() and m_prediction only, never the buckets.
#if ENABLE(DFG_JIT)
    unsigned lazyIndex = 0;
    codeBlock.lazyValueProfiles().forEachOperandValueProfile([&](const LazyOperandValueProfile& profile) {
        visitor.lazyOperand(lazyIndex++, profile.key(), profile.m_prediction);
    });
#endif

    // Array profiles: the accumulated modes and flags, never the StructureID samples.
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

// capture's visitor: writes each slot's cb.state record and, from the same value, its cb.summary slot, and scores that
// slot, so the summary holds no second read of the live CB. The buffers start zeroed, so every reserved field and padding
// byte the layouts leave unwritten is zero.
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
        writeField<uint8_t>(m_summary, m_summaryLayout.elementOffset(CBFormat::SummaryArray::ArrayFlags, position), categories);
        m_richness.addArrayFlags(categories);
    }

    // Element index of the array, whose element type the record's size matches.
    template<typename Record>
    void writeState(CBFormat::StateArray array, size_t index, const Record& record)
    {
        ASSERT(sizeof(Record) == CBFormat::elementSize(array));
        ASSERT(index < m_stateLayout.count(array));
        writeField<Record>(m_state, m_stateLayout.elementOffset(array, index), record);
    }

    const Richness& richness() const { return m_richness; }

private:
    void writeCategories(CBFormat::SummaryArray array, size_t index, SpeculatedType categories)
    {
        ASSERT(index < m_summaryLayout.count(array));
        writeField<uint64_t>(m_summary, m_summaryLayout.elementOffset(array, index), categories);
        m_richness.addCategories(categories);
    }

    std::span<uint8_t> m_state;
    const CBFormat::StateLayout& m_stateLayout;
    std::span<uint8_t> m_summary;
    const CBFormat::SummaryLayout& m_summaryLayout;
    Richness m_richness;
};

// The cb.state record of one entry of an unscored family: the allocation hint through the concurrent readers, which
// never read the last array, and every other field as stored.
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

// The unscored families, F16 to F30: each family's entries in metadata-ID order, from the family's first record in its
// kind's array.
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

    SlotCounts counts = countSlots(codeBlock);
    if (auto fault = checkCodeBlock(codeBlock, counts, strict))
        return std::unexpected(*fault);

    // The layouts depend only on the counts. A size past size_t is a charge no budget can hold.
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

    // cb.state's header: the tier-up history and the counter as readCounter recorded it.
    CounterRecord counter = readCounter(codeBlock, hasPolymorphicSite);
    stateHeader.counterMode = static_cast<uint8_t>(counter.mode);
    stateHeader.optimizationDelayCounter = static_cast<uint16_t>(codeBlock.optimizationDelayCounter());
    stateHeader.reoptimizationRetryCounter = static_cast<uint16_t>(codeBlock.reoptimizationRetryCounter());
    stateHeader.counterValue = counter.value;
    stateHeader.counterTotalCount = counter.totalCount;
    stateHeader.counterActiveThreshold = counter.activeThreshold;
    writeField<CBFormat::StateHeader>(state, 0, stateHeader);

    // cb.summary's header takes its counter fields from the same record. One walk then writes every slot of both
    // sections, each summary slot from the value just written to cb.state and the UCB copy read beside it, and the score
    // comes from the summary as written.
    summaryHeader.counterMode = stateHeader.counterMode;
    summaryHeader.counterProgress = counterProgress(counter);
    writeField<CBFormat::SummaryHeader>(summary, 0, summaryHeader);

    SectionWriter writer(state, *stateLayout, summary, *summaryLayout);
    forEachScoredSlot(codeBlock, counts, writer);
    writeUnscoredFamilies(codeBlock, counts, writer);
    result.m_score = scoreOf(writer.richness(), summaryHeader.counterMode, summaryHeader.counterProgress);

    // With strict on, the built sections must pass the checks a strict consumer runs on them, so a producer never commits
    // material its strict consumers reject. The checks read the bytes in place and allocate nothing; a failure drops the
    // object, which frees both buffers and releases their charge.
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
    return scoreOf(scorer.richness(), static_cast<uint8_t>(counter.mode), counterProgress(counter));
}

std::expected<CBScore, CBFault> decodeSummary(std::span<const uint8_t> summarySection, bool strict)
{
    using namespace CBCaptureInternal;
    using CBFormat::SummaryArray;

    // Normal mode trusts the section, whose integrity the integrator checked before the lane reads it.
    if (strict) {
        if (auto check = validateSummary(summarySection))
            return std::unexpected(CBFault { CBFaultKind::InvalidMaterial, *check });
    }

    auto header = readField<CBFormat::SummaryHeader>(summarySection, 0);
    // Four 32-bit counts cannot overflow a 64-bit size_t, and every supported target is 64-bit.
    auto layout = CBFormat::summaryLayout(header);
    RELEASE_ASSERT(layout);

    // Each slot capture wrote is already the union of the CB and UCB copies, so adding the slots' units gives back the
    // score capture computed while writing them.
    Richness richness;
    for (auto array : { SummaryArray::ArgumentCategories, SummaryArray::ValueCategories, SummaryArray::LazyOperandCategories }) {
        for (size_t index = 0; index < layout->count(array); ++index)
            richness.addCategories(readField<uint64_t>(summarySection, layout->elementOffset(array, index)));
    }
    for (size_t index = 0; index < layout->count(SummaryArray::ArrayFlags); ++index)
        richness.addArrayFlags(readField<uint8_t>(summarySection, layout->elementOffset(SummaryArray::ArrayFlags, index)));
    return scoreOf(richness, header.counterMode, header.counterProgress);
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
