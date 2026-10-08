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
#include "PropertyInlineCache.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <wtf/Assertions.h>

// Restoration of the ICsBaseline section into a newborn CB (SPEC-ics.md sections 6.2 to 6.4). The install function
// calls the three steps on the VM's thread, in THREAD Restoration's order:
//
//   prepareBaselineICs      newborn CB: finishCreation done, no JIT type, never run; takes no lock
//   seedCallLinkHistory     linked CB, no JIT type; takes no lock
//   attachPropertyICState   BaselineJITData published, executable not yet updated; takes CodeBlock::m_lock
//
// Prepare writes nothing and allocates nothing (I1); seed and attach cannot fail and allocate nothing (I2). Seed and
// attach write the state section 6.1 derives from each record, field by field, through the native objects' own
// members, and only into the CB that prepare received.

namespace JSC {
class BaselineJITCode;
class CodeBlock;
} // namespace JSC

namespace JSC::JITCache::ICs {

// The strict checks prepare composes (section 6.2). Each returns the first failure it finds.
// The first three read only their arguments and need no VM; tests build molds as plain
// BaselineUnlinkedPropertyInlineCache values.
std::optional<Invalid> checkMoldPairing(const SectionView&, std::span<const BaselineUnlinkedPropertyInlineCache> molds); // A7
std::optional<Invalid> checkMetadataLayout(const SectionView&, const CallLinkSiteCounts&); // A8
std::optional<Invalid> checkMolds(std::span<const BaselineUnlinkedPropertyInlineCache>); // S1
std::optional<Invalid> checkNewbornCodeBlock(CodeBlock&); // S2

// Views into a body's ICsBaseline payload, validated under strict, for the CB that prepared them. It owns nothing, so
// a preparation another lane's failure drops needs no cleanup, and the next newborn CB of the UCB prepares again. The
// integrator keeps the payload alive and unchanged until attachPropertyICState returns (R-INT-4).
class PreparedBaselineICs {
public:
    std::span<const PropertyICRecord> propertyICs() const { return m_propertyICs; }
    std::span<const CallLinkRecord> callLinks() const { return m_callLinks; }
    Summary summary() const { return m_summary; }

private:
    friend std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t>, const BaselineJITCode&, CodeBlock&, StrictChecks);
    // Seed and attach read m_codeBlock in their debug assertions.
    friend void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);
    friend void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

    std::span<const PropertyICRecord> m_propertyICs; // borrowed from the payload
    std::span<const CallLinkRecord> m_callLinks; // borrowed from the payload, canonical order
    Summary m_summary;
#if ASSERT_ENABLED
    const CodeBlock* m_codeBlock { nullptr }; // seed and attach assert they receive the prepared CB
#endif
};

// preparedCode is PreparedImage::code(), read before commit (R-IMG-1); prepare keeps no reference to it.
// With StrictChecks::Yes it runs A1 to A6 (parseSection), A7, A8, S1 and S2 in that order and returns the first
// failure, which is invalid material; otherwise it only slices the section by its header's counts.
std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks);

// Section 6.3: after linking and before CodeBlock::setupWithUnlinkedBaselineCode, on the prepared CB only.
void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);

// Section 6.4: right after CodeBlock::setupWithUnlinkedBaselineCode returns and before the baseline counter is
// re-sliced, on the prepared CB only. Takes CodeBlock::m_lock, which the caller must not hold.
void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
