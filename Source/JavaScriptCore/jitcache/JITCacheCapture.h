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

#include "ArtifactWriter.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// Capture scores (SPEC-integrator.md section 8.2). A live candidate's score is tier 1, the UCB lane's liveRichness
// total plus the CB lane's scoreLive richness units, the ICs lane's IC sites with cases, and scoreLive's counterWithheld
// and counter progress, with the ICs lane called before the CB lane for the same CB in the same pause (II22). A score
// about to be committed comes from what the builders returned for the bytes they wrote (II23).
struct CaptureScore {
    uint8_t tier { 0 }; // 1 for a baseline capture
    uint64_t richness { 0 }; // the UCB lane's units plus the CB lane's units
    uint32_t icSitesWithCases { 0 };
    bool counterWithheld { false }; // the polymorphic rule withheld the counter (SPEC-cb.md section 4.3)
    uint32_t counterProgress { 0 };
};
struct SavedScore {
    CaptureScore score;
    uint64_t version { 0 }; // the body file's commit identifier
};

// Compares the fields lexicographically in declaration order, which is THREAD Capture's order, a true counterWithheld
// above a false one. A candidate wins only when it is strictly greater; a remaining tie keeps the saved body.
bool beats(const CaptureScore& candidate, const CaptureScore& saved);

// A saved body's score, read from its three summary sections by the lanes' readers with the session's strict flag.
// With strict off no reader rejects. With it on, the rejection names the reader: part "ucb" at "saved-summary"
// (savedRichness names no check), "cb" with description(check), or "ics" with the enumerator name of the ICs check.
struct SummaryRejection {
    ASCIILiteral part;
    ASCIILiteral check;
};
std::expected<CaptureScore, SummaryRejection> scoreSections(uint8_t tier, std::span<const uint8_t> ucbFeedback,
    std::span<const uint8_t> cbSummary, std::span<const uint8_t> ics, bool strict);

#if ENABLE(JITCACHE_TWINS)
// Section 8.3. Requires active production; rewrites the key's body through the writer's rewriteSection (container
// sub-SPEC section 8.3) and erases the key's kept summary. The shell's jitcacheRewriteSection calls it.
std::expected<CommitResult, CommitFailure> rewriteSectionForTesting(VM&, const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
// The key's kept summary, for T-STAMP.
std::optional<SavedScore> keptScoreForTesting(VM&, const BodyKey&);
#endif

} // namespace JSC::JITCache
