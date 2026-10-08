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
#include "ImageRecord.h"

#if ENABLE(JIT)

#include "ImageSection.h"
#include "ImageSupport.h"
#include "JITMathIC.h"
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

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
