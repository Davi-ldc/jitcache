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

#if ENABLE(JIT)

#include "ICSection.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Capture of the ICsBaseline section from a live baseline CB (SPEC-ics.md section 5). The integrator calls these
// functions at THREAD's two capture points, the end of the success path of BaselineJITPlan::finalize and delta, on an
// eligible CB whose BaselineJITData is published, on the VM's thread with JS paused and no CodeBlock::m_lock held.
// summarizeBaselineICs and captureBaselineICs take the CB's m_lock and release it before returning. No function here
// drains a profile, materializes a property table, allocates a cell or memory, or stops for the collector.

namespace JSC {
class CallLinkInfo;
class CodeBlock;
class ConcurrentJSLocker;
class HandlerPropertyInlineCache;
} // namespace JSC

namespace JSC::JITCache::ICs {

// What summarizing or capturing a live CB yields: the summary and the polymorphic bit (section 5.3).
struct CaptureSummary {
    Summary summary;
    bool hasPolymorphicSite { false };
};

enum class CaptureCheck : uint8_t {
    NoBaselineJITData,
    OutputSizeMismatch,
    MoldMismatch, // C1
};

struct CaptureError {
    CaptureCheck check;
    uint32_t siteIndex; // IC index for C1, 0 otherwise
};

// The exact byte size of the CB's ICsBaseline section: the IC count of BaselineJITData and
// callLinkSiteCounts (zero groups without a metadata table). Reads counts only, takes no lock.
// Precondition: jitType() == BaselineJIT and baselineJITData() is non-null.
size_t baselineICsSectionSize(CodeBlock&);

// The summary and polymorphic bit the CB would capture now, for scoring candidates before
// capturing the winner. Takes CodeBlock::m_lock. Allocates no cell (section 5.1).
CaptureSummary summarizeBaselineICs(CodeBlock&);

// Writes the whole section into output, whose size must equal baselineICsSectionSize(codeBlock),
// and returns its summary and polymorphic bit. Takes CodeBlock::m_lock. Allocates no cell
// (section 5.1). Reads only.
std::expected<CaptureSummary, CaptureError> captureBaselineICs(CodeBlock&, std::span<uint8_t> output, StrictChecks);

// The one reader of live state that capture and the twin snapshot (ICTwins.cpp) share, so a snapshot and a capture of
// the same state agree field for field (section 11.1). Each returns the record capture writes for one site (sections
// 5.2 and 5.4) and must run under the site's CodeBlock::m_lock, which the locker witnesses.
PropertyICRecord readPropertyICRecord(const ConcurrentJSLocker&, const HandlerPropertyInlineCache&);
CallLinkRecord readCallLinkRecord(const ConcurrentJSLocker&, CallLinkInfo&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
