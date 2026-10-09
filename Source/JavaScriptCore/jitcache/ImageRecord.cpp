#include "config.h"
#include "ImageRecord.h"

#if ENABLE(JIT)

#include "BaselineJITCode.h"
#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "ImageRecorder.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "JITCacheFaults.h"
#include "JITMathIC.h"
#include "LinkBuffer.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageRecord);

namespace ImageRecordInternal {

// What JITMathIC::generateOutOfLine's linkJumpToOutOfLineSnippet writes at the inline start (SPEC-image.md section 6.2).
#if CPU(X86_64)
static constexpr uint32_t inlineStartRewriteSize = 5; // one jmp rel32
#else
static constexpr uint32_t inlineStartRewriteSize = 4; // one b
#endif

template<typename MathIC>
static MathICLocations locationsOf(const MathIC& mathIC)
{
    // untaggedPtr, unlike dataLocation, accepts the null locations of a MathIC without inline code.
    return MathICLocations {
        .inlineStart = mathIC.m_inlineStart.untaggedPtr(),
        .inlineEnd = mathIC.m_inlineEnd.untaggedPtr(),
        .slowPathStart = mathIC.m_slowPathStartLocation.untaggedPtr(),
        .slowPathCall = mathIC.m_slowPathCallLocation.untaggedPtr(),
    };
}

} // namespace ImageRecordInternal

std::optional<size_t> findFixupIndex(std::span<const ImageFixup> fixups, uint32_t site, FixupForm form)
{
    auto first = std::ranges::lower_bound(fixups, site, { }, &ImageFixup::site);
    for (auto it = first; it != fixups.end() && it->site == site; ++it) {
        if (it->form == form)
            return static_cast<size_t>(it - fixups.begin());
    }
    return std::nullopt;
}

MathICLocations mathICLocations(MathICKind kind, const void* mathIC)
{
    using namespace ImageRecordInternal;
    switch (kind) {
    case MathICKind::Add:
        return locationsOf(*static_cast<const JITAddIC*>(mathIC));
    case MathICKind::Sub:
        return locationsOf(*static_cast<const JITSubIC*>(mathIC));
    case MathICKind::Mul:
        return locationsOf(*static_cast<const JITMulIC*>(mathIC));
    case MathICKind::Negate:
        return locationsOf(*static_cast<const JITNegIC*>(mathIC));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ImageRecord::ImageRecord(Ref<ProducerBudget>&& budget, size_t chargedBytes, RecordState state, Unrecordable reason)
    : m_budget(WTF::move(budget))
    , m_chargedBytes(chargedBytes)
    , m_state(state)
    , m_unrecordableReason(reason)
{
    ASSERT(state != RecordState::Complete);
    ASSERT((state == RecordState::Unrecordable) == (reason != Unrecordable::None));
    ASSERT(m_chargedBytes == heldBytes());
}

ImageRecord::ImageRecord(Ref<ProducerBudget>&& budget, size_t chargedBytes, const void* imageStart, uint32_t codeSize, Vector<ImageFixup>&& fixups, Vector<MathICRecord>&& mathICs, BakedFacts&& bakedFacts)
    : m_budget(WTF::move(budget))
    , m_chargedBytes(chargedBytes)
    , m_imageStart(imageStart)
    , m_codeSize(codeSize)
    , m_fixups(WTF::move(fixups))
    , m_mathICs(WTF::move(mathICs))
    , m_bakedFacts(WTF::move(bakedFacts))
{
    ASSERT(m_imageStart);
    ASSERT(std::ranges::is_sorted(m_fixups, precedesInFootprintOrder));
    ASSERT(m_chargedBytes == heldBytes());
}

ImageRecord::~ImageRecord()
{
    if (m_chargedBytes)
        m_budget->release(m_chargedBytes);
}

size_t ImageRecord::storageBytes(size_t fixupCapacity, size_t mathICCapacity, size_t snippetFixupCapacity, size_t scopeFactCapacity)
{
    return sizeof(ImageRecord)
        + fixupCapacity * sizeof(ImageFixup)
        + mathICCapacity * sizeof(MathICRecord)
        + snippetFixupCapacity * sizeof(ImageFixup)
        + scopeFactCapacity * sizeof(ScopeFact);
}

size_t ImageRecord::storageBytes(const SnippetProvenance& provenance)
{
    return provenance.fixups.capacity() * sizeof(ImageFixup);
}

size_t ImageRecord::heldBytes() const
{
    size_t snippetFixupCapacity = 0;
    for (auto& mathIC : m_mathICs) {
        if (mathIC.snippet)
            snippetFixupCapacity += mathIC.snippet->fixups.capacity();
    }
    return storageBytes(m_fixups.capacity(), m_mathICs.capacity(), snippetFixupCapacity, m_bakedFacts.scopeFacts.capacity());
}

std::span<const uint8_t> ImageRecord::imageCode() const
{
    return unsafeMakeSpan(static_cast<const uint8_t*>(m_imageStart), m_codeSize);
}

std::optional<uint32_t> ImageRecord::offsetInImage(const void* location) const
{
    auto address = reinterpret_cast<uintptr_t>(location);
    auto start = reinterpret_cast<uintptr_t>(m_imageStart);
    if (!location || address < start || address - start > m_codeSize)
        return std::nullopt;
    return static_cast<uint32_t>(address - start);
}

void ImageRecord::stop(RecordState state, Unrecordable reason)
{
    ASSERT(state != RecordState::Complete);
    if (m_state != RecordState::Complete)
        return;
    m_state = state;
    m_unrecordableReason = reason;

    m_imageStart = nullptr;
    m_codeSize = 0;
    m_fixups = { };
    m_mathICs = { };
    m_bakedFacts = { };
    size_t objectBytes = sizeof(ImageRecord);
    ASSERT(m_chargedBytes >= objectBytes);
    if (m_chargedBytes > objectBytes) {
        m_budget->release(m_chargedBytes - objectBytes);
        m_chargedBytes = objectBytes;
    }
}

void ImageRecord::markUnrecordable(Unrecordable reason)
{
    ASSERT(reason != Unrecordable::None);
    stop(RecordState::Unrecordable, reason);
}

void ImageRecord::markIncomplete()
{
    stop(RecordState::Incomplete, Unrecordable::None);
}

void ImageRecord::markInconsistent()
{
    markUnrecordable(Unrecordable::InconsistentRecord);
    ASSERT_NOT_REACHED();
}

bool ImageRecord::chargeForGrowth(size_t bytes)
{
    if (!m_budget->tryCharge(bytes)) {
        markIncomplete();
        return false;
    }
    m_chargedBytes += bytes;
    return true;
}

std::optional<unsigned> ImageRecord::mathICIndex(const void* mathIC) const
{
    for (unsigned index = 0; index < m_mathICs.size(); ++index) {
        if (m_mathICs[index].mathIC == mathIC)
            return index;
    }
    return std::nullopt;
}

void ImageRecord::didReplaceSlowCall(unsigned index, CodePtr<CFunctionPtrTag> replacement)
{
    if (m_state != RecordState::Complete)
        return;
    RELEASE_ASSERT(index < m_mathICs.size());
    auto& mathIC = m_mathICs[index];
    // Only a MathIC with inline code has a slow call to repoint (section 6.1).
    auto slowCall = mathIC.slowCallPointerSite ? findFixupIndex(m_fixups.span(), *mathIC.slowCallPointerSite, FixupForm::Pointer) : std::nullopt;
    if (!slowCall || m_fixups[*slowCall].target.kind != TargetKind::Operation) {
        markInconsistent();
        return;
    }
    auto symbol = CodeSymbol::of(replacement.untaggedPtr());
    if (!symbol) {
        markUnrecordable(Unrecordable::ForeignCodeSymbol);
        return;
    }
    m_fixups[*slowCall].target = ImageTarget { .kind = TargetKind::Operation, .a = 0, .b = 0, .payload = symbol->offset };
}

void ImageRecord::didGenerateSnippet(unsigned index, SnippetProvenance&& provenance)
{
    // The record takes over the provenance's charge whatever it then keeps.
    size_t provenanceBytes = storageBytes(provenance);
    m_chargedBytes += provenanceBytes;
    auto dropProvenance = [&] {
        provenance = { };
        m_chargedBytes -= provenanceBytes;
        if (provenanceBytes)
            m_budget->release(provenanceBytes);
    };
    if (m_state != RecordState::Complete) {
        dropProvenance();
        return;
    }
    RELEASE_ASSERT(index < m_mathICs.size());
    auto& mathIC = m_mathICs[index];
    // Only a MathIC with inline code regenerates: without it nothing ever reaches generateOutOfLine (N17).
    if (!mathIC.slowCallPointerSite || !provenance.start) {
        dropProvenance();
        markInconsistent();
        return;
    }
    ASSERT(std::ranges::is_sorted(provenance.fixups, precedesInFootprintOrder));

    size_t replacedBytes = mathIC.snippet ? storageBytes(*mathIC.snippet) : 0;
    mathIC.snippet = WTF::move(provenance);
    if (replacedBytes) {
        m_chargedBytes -= replacedBytes;
        m_budget->release(replacedBytes);
    }
    ASSERT(m_chargedBytes == heldBytes());
}

void ImageRecord::didRewriteInlineStart(unsigned index)
{
    using namespace ImageRecordInternal;
    if (m_state != RecordState::Complete)
        return;
    RELEASE_ASSERT(index < m_mathICs.size());
    auto& mathIC = m_mathICs[index];
    auto inlineStart = offsetInImage(mathICLocations(mathIC.kind, mathIC.mathIC).inlineStart);
    // The rewrite jumps to the snippet whose provenance didGenerateSnippet has just stored (section 6.2, edits 3 and 4).
    if (!mathIC.slowCallPointerSite || !mathIC.snippet || !inlineStart || *inlineStart + inlineStartRewriteSize > m_codeSize) {
        markInconsistent();
        return;
    }
    uint32_t rewrittenBegin = *inlineStart;
    uint32_t rewrittenEnd = rewrittenBegin + inlineStartRewriteSize;

    // The fixups whose footprints the rewritten bytes reach. Footprints are disjoint, in footprint order and at most
    // maximumFootprintSize long, so none with a site before rewrittenBegin - maximumFootprintSize reaches them. An x86_64
    // Jump's length is read from its opcode, which the rewrite changed only inside the rewritten bytes; a footprint that
    // began there lies wholly inside them or straddles their end, since the inline start is an instruction boundary.
    auto code = imageCode();
    uint32_t searchFrom = rewrittenBegin > maximumFootprintSize ? rewrittenBegin - static_cast<uint32_t>(maximumFootprintSize) : 0;
    auto first = static_cast<size_t>(std::ranges::lower_bound(m_fixups, searchFrom, { }, &ImageFixup::site) - m_fixups.begin());
    size_t droppedBegin = first;
    size_t droppedCount = 0;
    for (size_t position = first; position < m_fixups.size(); ++position) {
        auto& fixup = m_fixups[position];
        auto footprint = fixupFootprint(fixup.form, fixup.site, code);
        if (!footprint) {
            markInconsistent();
            return;
        }
        if (footprint->end <= rewrittenBegin)
            continue;
        if (footprint->begin >= rewrittenEnd)
            break;
        // A footprint the rewrite covers only in part would leave producer bits outside every fixup (I1, I8).
        if (footprint->begin < rewrittenBegin || footprint->end > rewrittenEnd) {
            markInconsistent();
            return;
        }
        if (!droppedCount)
            droppedBegin = position;
        ++droppedCount;
    }
    if (droppedCount)
        m_fixups.removeAt(droppedBegin, droppedCount);

    uint64_t entrySite = snippetEntrySite(rewrittenBegin);
    ASSERT(entrySite <= m_codeSize);
    ImageFixup entry {
        .site = static_cast<uint32_t>(entrySite),
        .form = FixupForm::Jump,
        .target = ImageTarget { .kind = TargetKind::SnippetEntry, .a = index, .b = 0, .payload = 0 },
    };
    if (m_fixups.size() == m_fixups.capacity()) {
        if (!chargeForGrowth(sizeof(ImageFixup)))
            return;
        m_fixups.reserveCapacity(m_fixups.size() + 1);
    }
    auto position = static_cast<size_t>(std::ranges::upper_bound(m_fixups, entry, precedesInFootprintOrder) - m_fixups.begin());
    m_fixups.insert(position, entry);
    ASSERT(m_chargedBytes == heldBytes());
}

MathICRegeneration::MathICRegeneration(CodeBlock* codeBlock, const void* mathIC, CodePtr<CFunctionPtrTag> callReplacement, uint16_t profileBitsAtEntry)
{
    UNUSED_PARAM(callReplacement);
    UNUSED_PARAM(profileBitsAtEntry);
    ASSERT(codeBlock);

    // A VM with no JITCache state, or one that does not produce, pays this one test.
    VM& vm = codeBlock->vm();
    if (!producerContext(vm)) [[likely]]
        return;
    // DFG and FTL MathICs share this class and never record.
    if (codeBlock->jitType() != JITType::BaselineJIT)
        return;
    RefPtr<JSC::JITCode> jitCode = codeBlock->jitCode();
    ImageRecord* record = static_cast<BaselineJITCode&>(*jitCode).m_jitCacheImageRecord.get();
    if (!record || record->state() != RecordState::Complete)
        return;

    // A Complete record lists every MathIC of its code, the record an import rebuilds included.
    auto index = record->mathICIndex(mathIC);
    if (!index) {
        ASSERT_NOT_REACHED();
        record->markUnrecordable(Unrecordable::InconsistentRecord);
        return;
    }
    m_record = record;
    m_mathICIndex = *index;
}

MathICRegeneration::~MathICRegeneration()
{
    retireRecorder();
}

uint32_t MathICRegeneration::imageOffset(CodeLocationLabel<JSInternalPtrTag> location) const
{
    if (!m_record || m_record->state() != RecordState::Complete)
        return 0;
    // The done and slow-path-start locations lie in the IC's own image.
    auto offset = m_record->offsetInImage(location.untaggedPtr());
    if (!offset) {
        ASSERT_NOT_REACHED();
        m_record->markUnrecordable(Unrecordable::InconsistentRecord);
        return 0;
    }
    return *offset;
}

void MathICRegeneration::attach(CCallHelpers& jit)
{
    retireRecorder();
    // A record that stopped records nothing more, so the snippet links natively.
    if (!m_record || m_record->state() != RecordState::Complete)
        return;
    // The snippet's profile writes name the arithmetic profiles of the UCB of the CodeBlock it is emitted for, which
    // every CodeBlock sharing the image shares.
    CodeBlock* codeBlock = jit.codeBlock();
    ASSERT(codeBlock);
    m_recorder = makeUnique<ImageRecorder>(RecordingScope::MathICSnippet, codeBlock->vm(), *codeBlock->unlinkedCodeBlock(), m_record->budget());
    m_recorder->attachTo(jit);
}

void MathICRegeneration::emitVeneers(CCallHelpers& jit)
{
    if (!m_recorder)
        return;
    ASSERT(jit.jitCacheRecorder() == m_recorder.get());
    m_recorder->emitVeneers(jit);
}

void MathICRegeneration::didLinkSnippet(LinkBuffer& linkBuffer, const MacroAssemblerCodeRef<JITStubRoutinePtrTag>& code)
{
    if (!m_recorder)
        return;
    auto provenance = m_recorder->finishSnippet(linkBuffer);
    if (!provenance) {
        // The recorder stopped, so the record cannot describe the snippet m_code now holds.
        stopRecordLikeRecorder();
        return;
    }
    // The provenance covers the allocation m_code holds up to its linked size, which the handle's size can exceed (N20).
    ASSERT_UNUSED(code, provenance->start == code.code().untaggedPtr());
    ASSERT(provenance->size <= code.size());
    size_t provenanceBytes = ImageRecord::storageBytes(*provenance);
    m_record->didGenerateSnippet(m_mathICIndex, WTF::move(*provenance));
    m_recorder->handChargeToRecord(provenanceBytes);
}

void MathICRegeneration::didRewriteInlineStart()
{
    if (m_record)
        m_record->didRewriteInlineStart(m_mathICIndex);
}

void MathICRegeneration::didReplaceSlowCall(CodePtr<CFunctionPtrTag> replacement)
{
    if (m_record)
        m_record->didReplaceSlowCall(m_mathICIndex, replacement);
}

void MathICRegeneration::didFailToAllocate(VM& vm)
{
    // Raised inside the operation, before the native fallback writes anything (SPEC-integrator.md section 4.5).
    didFailExecutableAllocation(vm, ExecutableAllocationSite::MathICSnippet);
}

void MathICRegeneration::stopRecordLikeRecorder()
{
    ASSERT(m_record && m_recorder);
    switch (m_recorder->state()) {
    case RecordState::Complete:
        return;
    case RecordState::Incomplete:
        m_record->markIncomplete();
        return;
    case RecordState::Unrecordable:
        m_record->markUnrecordable(m_recorder->unrecordableReason());
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void MathICRegeneration::retireRecorder()
{
    if (!m_recorder)
        return;
    // A recorder that stopped before its snippet was linked, or whose snippet never was, still stops the record: a
    // refused charge or an unrecordable path holds for the rest of the regeneration (section 4.8).
    stopRecordLikeRecorder();
    m_recorder = nullptr;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
