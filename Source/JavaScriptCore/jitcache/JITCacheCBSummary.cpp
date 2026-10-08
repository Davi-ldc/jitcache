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
#include "JITCacheCBState.h"

#if ENABLE(JIT)

#include "ArrayProfile.h"
#include "JITCacheCBFormat.h"
#include <bit>
#include <initializer_list>
#include <wtf/UnalignedAccess.h>

// decodeSummary (SPEC-cb.md section 4.4): a saved body's CBScore, read from its cb.summary alone, since scoring runs
// without a CodeBlock. Each slot capture wrote there is already the union of the CB and UCB copies, so counting section
// 4.3's units over the bytes gives the score capture computed while writing them (I8).

namespace JSC::JITCache {

namespace CBSummaryInternal {

// Byte-wise loads, as validateSummary makes. R-INT-1 hands the section over 8-byte aligned, but nothing here relies on
// that, and each load goes through std::span's hardened subspan, so a section normal mode trusts is never read past its
// end.
template<typename T>
static T load(std::span<const uint8_t> section, size_t offset)
{
    return WTF::unalignedLoad<T>(section.subspan(offset, sizeof(T)).data());
}

// First-run pruning sets this flag when it discards modes, not when the site meets anything new, so richness leaves it
// out (THREAD Capture). Every flag fits a byte (CBFormat::allArrayProfileFlags).
static constexpr uint8_t pruningMark = static_cast<uint8_t>(ArrayProfileFlag::DidPerformFirstRunPruning);

} // namespace CBSummaryInternal

std::expected<CBScore, CBFault> decodeSummary(std::span<const uint8_t> summarySection, bool strict)
{
    using CBFormat::SummaryArray;
    using CBSummaryInternal::load;

    // Section 3.4. Normal mode trusts the section past the integrity checks the integrator made (THREAD Session).
    if (strict) {
        if (auto check = validateSummary(summarySection))
            return std::unexpected(CBFault { CBFaultKind::InvalidMaterial, *check });
    }

    auto header = load<CBFormat::SummaryHeader>(summarySection, 0);
    // Four 32-bit counts cannot overflow a 64-bit size_t, and every supported target is 64-bit.
    auto layout = CBFormat::summaryLayout(header);
    RELEASE_ASSERT(layout);

    // Section 4.3: one unit per category bit of an argument, value or lazy-operand slot, and one per array flag other
    // than the pruning mark.
    CBScore score;
    for (auto array : { SummaryArray::ArgumentCategories, SummaryArray::ValueCategories, SummaryArray::LazyOperandCategories }) {
        for (size_t index = 0; index < layout->count(array); ++index)
            score.richnessUnits += std::popcount(load<uint64_t>(summarySection, layout->elementOffset(array, index)));
    }
    for (size_t index = 0; index < layout->count(SummaryArray::ArrayFlags); ++index) {
        auto flags = load<uint8_t>(summarySection, layout->elementOffset(SummaryArray::ArrayFlags, index));
        score.richnessUnits += std::popcount(static_cast<unsigned>(flags & ~CBSummaryInternal::pruningMark));
    }
    score.counterWithheld = header.counterMode == static_cast<uint8_t>(CBFormat::CounterMode::NotCarried);
    score.counterProgress = header.counterProgress;
    return score;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
