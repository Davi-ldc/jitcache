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

#include <wtf/Platform.h>

#if ENABLE(JIT)

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UniqueArray.h>
#include <wtf/text/ASCIILiteral.h>

// The CB lane's interface (SPEC-cb.md section 6.1): capture of a baseline CodeBlock's state into cb.state and cb.summary,
// scoring, validation, and the seeding of a newborn CodeBlock. JITCacheCBFormat.h holds the sections' layouts.

namespace JSC {

class CodeBlock;

} // namespace JSC

namespace JSC::JITCache {

namespace CBFormat {

struct StateHeader;

} // namespace CBFormat

class ProducerBudget; // integrator, R-INT-2
#if ENABLE(JITCACHE_TWINS)
class TwinReport; // integrator, R-INT-6
#endif

enum class CBFaultKind : uint8_t { InvalidMaterial, RecordingFault };

enum class CBCheck : uint8_t {
    StateHeader, ArgumentCount, ValueProfileCount, FamilyCount, StateLength, // V1..V5
    ValuePrediction, ArrayProfile, AllocationHint, IterationModes, EnumeratorModes, // V6..V10
    ToThisStatus, BranchBit, LazyOperand, TierUpHistory, Counter, // V11..V15
    SummaryHeader, SummaryLength, SummaryValue, // decodeSummary
    StrictNewborn, StrictLinkState, StrictCounter, // S1..S3
    CapturePairing, CaptureCharge, CaptureStrict, // section 4.4
};

struct CBFault {
    CBFaultKind kind;
    CBCheck check;
};

ASCIILiteral description(CBCheck); // names the failing step for status(vm)

// The structural rules of section 3.4, shared by SC1 and prepare. Each reads the span (and the CB)
// in place, allocates nothing and returns the first failing check, or nothing.
std::optional<CBCheck> validateState(std::span<const uint8_t> stateSection, CodeBlock&); // V1..V15
std::optional<CBCheck> validateSummary(std::span<const uint8_t> summarySection); // decodeSummary's rules
bool counterObeysNativeInvariant(const CBFormat::StateHeader&); // S3

struct CBScore {
    uint64_t richnessUnits { 0 };
    bool counterWithheld { false }; // CounterMode::NotCarried (section 4.3)
    uint32_t counterProgress { 0 };
};

// What finishCounter decided (section 5.3).
struct CBCounterRestore {
    bool carried { false }; // the section carried the counter (CounterMode::Carried)
    bool crossed { false }; // carried, finite T: checkIfThresholdCrossedAndSet found the consumer's threshold reached
    // carried, finite T: 0 when crossed, else the slice native code armed; carried, T == INT32_MAX: the captured slice;
    // not carried: the slice setup armed
    int64_t nativeSlice { 0 };
    int64_t slice { 0 }; // the slice the counter holds: max(nativeSlice, floor) when carried, else nativeSlice
};

class CBStateCapture {
    WTF_MAKE_NONCOPYABLE(CBStateCapture);
    WTF_MAKE_TZONE_ALLOCATED(CBStateCapture);
public:
    // VM thread, JS paused, API lock and heap access, no collector phase (section 4.1).
    // hasPolymorphicSite comes from the ICs lane for the same CB, in the same pause (R-INT-5).
    static std::expected<CBStateCapture, CBFault> capture(CodeBlock&, ProducerBudget&, bool strict, bool hasPolymorphicSite);
    static std::expected<CBScore, CBFault> scoreLive(CodeBlock&, bool strict, bool hasPolymorphicSite);

    CBStateCapture(CBStateCapture&&);
    ~CBStateCapture(); // frees both buffers and releases their charge

    const CBScore& score() const;
    std::span<const uint8_t> stateSection() const;
    std::span<const uint8_t> summarySection() const;

private:
    // How capture makes the object it returns: adopts both buffers, which hold stateSize and summarySize bytes, and the
    // charge of stateSize + summarySize against the budget, which the destructor releases.
    CBStateCapture(ProducerBudget& budget, UniqueArray<uint64_t>&& state, size_t stateSize, UniqueArray<uint64_t>&& summary, size_t summarySize, const CBScore& score)
        : m_budget(&budget)
        , m_state(WTF::move(state))
        , m_summary(WTF::move(summary))
        , m_stateSize(stateSize)
        , m_summarySize(summarySize)
        , m_score(score)
    {
    }

    ProducerBudget* m_budget { nullptr };
    UniqueArray<uint64_t> m_state; // 8-byte aligned
    UniqueArray<uint64_t> m_summary;
    size_t m_stateSize { 0 };
    size_t m_summarySize { 0 };
    CBScore m_score;
};

// Any thread that owns the material; reads only the span, validating it first with strict on.
std::expected<CBScore, CBFault> decodeSummary(std::span<const uint8_t> summarySection, bool strict);

class CBStateImport {
public:
    // Section 5.1, step 1. Reads the span and the CB; writes neither.
    static std::expected<CBStateImport, CBFault> prepare(std::span<const uint8_t> stateSection, CodeBlock&, bool strict);

    void seedLinkedState(CodeBlock&) const; // step 2; infallible
    CBCounterRestore finishCounter(CodeBlock&) const; // step 5; infallible

#if ENABLE(JITCACHE_TWINS)
    // Step 6: after finishCounter, with its result, before installCode (section 5.1).
    void verifyTwins(CodeBlock&, const CBCounterRestore&, TwinReport&) const;
#endif

private:
    std::span<const uint8_t> m_bytes; // borrowed, R-INT-1
    const CBFormat::StateHeader* m_header { nullptr };
    std::array<uint32_t, 9> m_arrayOffsets { }; // arrays 1..9 of section 3.2
};

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
