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

#include "CachedBytecode.h"
#include "UCBRegistry.h"
#include <expected>
#include <optional>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The sections a capture writes (SPEC-ucb.md section 8). The integrator's capture glue calls these on the VM thread with JS
// paused, for an eligible CodeBlock whose UCB has a record. liveRichness and savedRichness come from UCBFeedback.h.

class ProducerBudget;

enum class UCBCaptureFailure : uint8_t { BudgetExceeded, StrictCheckFailed };

struct UCBSections { // move-only
    UCBSections(Vector<uint8_t>&& identity, Ref<CachedBytecode>&& core, Vector<uint8_t>&& feedback, Ref<ProducerBudget>&&, size_t chargedBytes);
    UCBSections(UCBSections&&); // takes the source's budget and charge; the source keeps neither
    UCBSections& operator=(UCBSections&&) = delete;
    ~UCBSections(); // releases chargedBytes from budget when budget is non-null

    Vector<uint8_t> identity; // 152 bytes plus the atom and butterfly maps
    Ref<CachedBytecode> core;
    Vector<uint8_t> feedback;
    RefPtr<ProducerBudget> budget; // the budget buildSections charged; null after a move
    size_t chargedBytes;
};

// Empty, and the UCB is not captured, when it has no record, or when it is a direct eval whose record keeps no context
// digest, which no record made while production was active lacks (section 3.4).
std::optional<UCBRegistry::RecordView> captureRecord(VM&, const UnlinkedCodeBlock&);
// L, the LLInt threshold the import skips, as the UCB's history scales it at capture (THREAD Maintenance):
// ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()).
uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock&);
// Only for an accepted capture (section 8.2). A refused charge releases what the call charged and returns BudgetExceeded;
// SC1 or SC2 failing under strict returns StrictCheckFailed. The integrator raises either as a recording fault.
std::expected<UCBSections, UCBCaptureFailure> buildSections(VM&, const CodeBlock&, ProducerBudget&);

} // namespace JSC::JITCache
