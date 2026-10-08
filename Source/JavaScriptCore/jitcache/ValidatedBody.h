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

#include "UCBKeys.h"
#include <array>
#include <span>
#include <stdint.h>
#include <wtf/Function.h>
#include <wtf/MallocSpan.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC::JITCache {

// The section kinds the lanes name when they ask a body for a section (SPEC-integrator.md section 6.1):
//
//   kind                 the lane's name           type id  tier  in a body of highest tier 1
//   UCBIdentity          ucb.identity              0x0101   0     required
//   UCBCore              ucb.core                  0x0102   0     required
//   UCBFeedback          ucb.feedback              0x0103   0     required
//   ImageBaseline        image.baseline            0x0201   1     required
//   BakedFactsBaseline   baked-facts.baseline      0x0202   1     required
//   ImageTwinsBaseline   image-twins.baseline      0x0203   1     required in ENABLE(JITCACHE_TWINS) builds, forbidden elsewhere
//   CBStateBaseline      cb.state                  0x0301   1     required
//   CBSummaryBaseline    cb.summary                0x0302   1     required
//   ICsBaseline          ICsBaseline               0x0401   1     required
//
// The high byte of a type id names the lane and the low byte the section family; the directory entry's tier byte
// separates tiers, 0 for the sections every tier shares. The order of the enumerators is the directory's order.
enum class SectionKind : uint8_t {
    UCBIdentity, UCBCore, UCBFeedback,
    ImageBaseline, BakedFactsBaseline, ImageTwinsBaseline,
    CBStateBaseline, CBSummaryBaseline,
    ICsBaseline,
};
constexpr unsigned numberOfSectionKinds = 9;
static_assert(static_cast<unsigned>(SectionKind::ICsBaseline) + 1 == numberOfSectionKinds);

class OpenedArtifact;

// One body file that passed the container checks its VM runs: the integrity checks always and the structure checks under
// strict (container sub-SPEC sections 4.5 and 7.2). It holds a private read-only mapping of the file, so its bytes stay
// readable and unchanged while it lives, whatever happens to the file (THREAD Storage). A pending import pins it.
class ValidatedBody final : public ThreadSafeRefCounted<ValidatedBody> {
    WTF_MAKE_TZONE_ALLOCATED(ValidatedBody);
public:
    const BodyKey& key() const;
    uint64_t version() const; // the file's commit identifier, which the writer never makes 0
    uint8_t highestTier() const;
    std::span<const uint8_t> section(SectionKind) const; // empty when absent; starts 8-byte aligned
    size_t fileSize() const; // the mapped file's size, or a test body's buffer size
    ~ValidatedBody(); // unmaps or frees; any thread that holds no JITCache lock

#if ENABLE(JITCACHE_TWINS)
    // A body no container check has seen, for the parts' own tests: the sections are copied into one buffer, in the
    // order given (kinds strictly increasing), each at an 8-byte-aligned address; highestTier() is the highest tier of
    // the given kinds (section 6.1). onDestroy runs first in the destructor, on the destroying thread. version is not 0.
    struct TestSection {
        SectionKind kind;
        std::span<const uint8_t> bytes;
    };
    static Ref<ValidatedBody> createForTesting(const BodyKey&, uint64_t version, std::span<const TestSection>,
        Function<void()>&& onDestroy = { });
#endif

private:
    friend class OpenedArtifact; // open builds a body from a mapping validateBody accepted (container sub-SPEC section 7.2)
    using SectionSpans = std::array<std::span<const uint8_t>, numberOfSectionKinds>; // by SectionKind; empty when absent
    // Takes the mapping, which the destructor unmaps. open fills the spans from validateBody's BodyLayout, so this
    // header needs nothing from JITCacheContainer.h.
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, std::span<const uint8_t> mapping, const SectionSpans&);

    const BodyKey m_key;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const std::span<const uint8_t> m_mapping; // the private mapping the destructor unmaps; empty for a test body
    const SectionSpans m_sections; // spans into m_mapping, or into m_testBuffer for a test body
#if ENABLE(JITCACHE_TWINS)
    MallocSpan<uint8_t> m_testBuffer; // createForTesting's copy of its sections; fastMalloc aligns it to at least 8 bytes
    Function<void()> m_onDestroy;
#endif
};

// What VMState::openBody answers (UCB R-INT-3): Missing, Unusable (the integrator has already raised invalid material)
// or Found with the body.
class BodyLookup {
public:
    enum class Kind : uint8_t { Missing, Unusable, Found };
    static BodyLookup missing();
    static BodyLookup unusable();
    static BodyLookup found(Ref<ValidatedBody>&&);
    Kind kind() const;
    RefPtr<ValidatedBody> body() const; // non-null exactly for Found

private:
    BodyLookup(Kind, RefPtr<ValidatedBody>&&);

    Kind m_kind;
    RefPtr<ValidatedBody> m_body;
};

} // namespace JSC::JITCache
