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
// strict (container sub-SPEC sections 4.5 and 7.2). It holds the file's bytes in a buffer it owns, read once when the body
// was opened, so they stay readable and unchanged while it lives, whatever happens to the file (THREAD Storage). A
// pending import pins it.
class ValidatedBody final : public ThreadSafeRefCounted<ValidatedBody> {
    WTF_MAKE_TZONE_ALLOCATED(ValidatedBody);
public:
    const BodyKey& key() const;
    uint64_t version() const; // the file's commit identifier, which the writer never makes 0
    uint8_t highestTier() const;
    uint32_t llintThreshold() const; // the envelope's L (container sub-SPEC section 4.1); 0 for a test body
    uint32_t counterProgress() const; // the envelope's P; 0 for a test body
    std::span<const uint8_t> section(SectionKind) const; // empty when absent; starts 8-byte aligned
    size_t fileSize() const; // the file's size, or a test body's buffer size
    ~ValidatedBody(); // frees the buffer; any thread that holds no JITCache lock

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
    friend class OpenedArtifact; // open builds a body from the bytes validateBody accepted (container sub-SPEC section 7.2)
    using SectionSpans = std::array<std::span<const uint8_t>, numberOfSectionKinds>; // by SectionKind; empty when absent
    // Takes the buffer, whose address moving does not change, so the spans into it stay valid. open fills the version, the
    // tier, L, P and the spans from validateBody's BodyLayout, so this header needs nothing from JITCacheContainer.h.
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress,
        MallocSpan<uint8_t>&& bytes, const SectionSpans&);

    const BodyKey m_key;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const uint32_t m_llintThreshold; // the envelope's L; 0 for a test body
    const uint32_t m_counterProgress; // the envelope's P; 0 for a test body
    const MallocSpan<uint8_t> m_bytes; // the file, or createForTesting's copy of its sections; fastMalloc aligns it to at least 8 bytes
    const SectionSpans m_sections; // spans into m_bytes
#if ENABLE(JITCACHE_TWINS)
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
