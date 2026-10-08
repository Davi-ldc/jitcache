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

#pragma once

#include "ArrayProfile.h"
#include "DFGExitProfile.h"
#include "ExecutionCounter.h"
#include "SpeculatedType.h"
#include "UCBKeys.h"
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/TriState.h>

namespace JSC {

class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The ucb.feedback section, its checks, seeding, richness and the LLInt counter (SPEC-ucb.md section 5).

// The lane's share of THREAD Capture's richness score (section 5.6): the arithmetic categories and the exit sites.
struct UCBRichness {
    uint64_t arithmeticUnits { 0 }; // one per set category bit of each arithmetic profile slot
    uint64_t exitSiteUnits { 0 }; // one per exit site
    uint64_t total() const { return arithmeticUnits + exitSiteUnits; }
};

struct FeedbackCounts {
    uint32_t valueProfiles { 0 };
    uint32_t arrayProfiles { 0 };
    uint32_t binaryArithProfiles { 0 };
    uint32_t unaryArithProfiles { 0 };
    uint32_t exitSites { 0 };
    uint32_t functionDecls { 0 };
    uint32_t functionExprs { 0 };
    uint32_t constants { 0 };
    friend bool operator==(const FeedbackCounts&, const FeedbackCounts&) = default;
};

class FeedbackSection { // a parsed ucb.feedback section; borrows its bytes
public:
    // With strict, empty when the section breaks a rule of section 5.2. Without, never empty: the counts and arrays are read
    // where the layout puts them and trusted (THREAD Session).
    static std::optional<FeedbackSection> parse(std::span<const uint8_t>, bool strict);
    const FeedbackCounts& counts() const;
    SpeculatedType prediction(unsigned) const;
    ArrayModes observedArrayModes(unsigned) const;
    OptionSet<ArrayProfileFlag> arrayProfileFlags(unsigned) const;
    uint16_t binaryArithBits(unsigned) const;
    uint16_t unaryArithBits(unsigned) const;
    DFG::FrequentExitSite exitSite(unsigned) const;
    bool childSingletonInvalidated(ChildTable, unsigned index) const;
    bool constantSingletonInvalidated(unsigned constantIndex) const;
    TriState didOptimize() const;
    TriState quickDFGTierUp() const;
    bool quickFTLTierUp() const;
    int32_t llintActiveThreshold() const;
    double llintProgress() const; // double(m_totalCount) + m_counter

private:
    std::span<const uint8_t> m_bytes;
    FeedbackCounts m_counts;
};

FeedbackCounts liveFeedbackCounts(UnlinkedCodeBlock&); // VM thread; exit sites under UnlinkedCodeBlock::m_lock
bool feedbackFits(UnlinkedCodeBlock&, const FeedbackSection&); // C3 and C7
bool constantBitsFit(UnlinkedCodeBlock&, const FeedbackSection&); // C9
void seedFeedback(VM&, UnlinkedCodeBlock&, const FeedbackSection&); // section 5.3
size_t feedbackSectionSize(const FeedbackCounts&);
// Fills exactly feedbackSectionSize(counts) bytes from the UCB's accumulated state (section 8.2).
void writeFeedbackSection(std::span<uint8_t> out, UnlinkedCodeBlock&, const FeedbackCounts&);
void armLLIntCounter(BaselineExecutionCounter&, int32_t threshold, double progress); // section 5.5
UCBRichness liveRichness(UnlinkedCodeBlock&); // VM thread, during a capture
std::optional<UCBRichness> savedRichness(std::span<const uint8_t> feedbackSection, bool strict); // empty as parse is

} // namespace JSC::JITCache
