#pragma once

#include "jitcache/base/BodyKey.h"
#include <array>
#include <span>
#include <stdint.h>
#include <wtf/Function.h>
#include <wtf/MallocSpan.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

// What the lanes see of storage, the one store header they include: the section kinds, a body that passed the container
// checks, and what VMState::openBody and VMState::readBodySection answer.

namespace JSC::JITCache {

// The section kinds, one row each, in the directory's order of (type id, tier): v(kind, type id, tier, name, presence).
// The high byte of a type id names the lane (1 UCB, 2 Image, 3 CB, 4 ICs) and the low byte the section family; the
// directory entry's tier byte separates tiers, 0 for the sections every tier shares. The name is the lane's, which the
// writer's diagnostics, the shell functions and the capture event use. The presence says when a body holds the kind
// (SectionPresence in JITCacheContainer.h). SectionKind, its count, the container's description of each kind and the
// capture event's fields for each section all come from these rows.
#define JITCACHE_FOR_EACH_SECTION_KIND(v) \
    v(UCBIdentity, 0x0101, 0, "ucb.identity"_s, Required) \
    v(UCBCore, 0x0102, 0, "ucb.core"_s, Required) \
    v(UCBFeedback, 0x0103, 0, "ucb.feedback"_s, Required) \
    v(UCBAnalysis, 0x0104, 0, "ucb.analysis"_s, ModuleOnly) \
    v(ImageBaseline, 0x0201, 1, "image.baseline"_s, Required) \
    v(BakedFactsBaseline, 0x0202, 1, "baked-facts.baseline"_s, Required) \
    v(ImageTwinsBaseline, 0x0203, 1, "image-twins.baseline"_s, TwinsOnly) \
    v(CBStateBaseline, 0x0301, 1, "cb.state"_s, Required) \
    v(CBSummaryBaseline, 0x0302, 1, "cb.summary"_s, Required) \
    v(ICsBaseline, 0x0401, 1, "ICsBaseline"_s, Required)

// The kinds the lanes name when they ask a body for a section.
#define JITCACHE_SECTION_KIND_ENUMERATOR(kind, typeId, tier, name, presence) kind,
enum class SectionKind : uint8_t {
    JITCACHE_FOR_EACH_SECTION_KIND(JITCACHE_SECTION_KIND_ENUMERATOR)
};
#undef JITCACHE_SECTION_KIND_ENUMERATOR

#define JITCACHE_COUNT_SECTION_KIND(kind, typeId, tier, name, presence) +1
constexpr unsigned numberOfSectionKinds = 0 JITCACHE_FOR_EACH_SECTION_KIND(JITCACHE_COUNT_SECTION_KIND);
#undef JITCACHE_COUNT_SECTION_KIND

class MappedPack;
class OpenedArtifact;

// One body that passed the container's integrity checks. Its bytes are borrowed from the pack's read-only mapping, which
// the body keeps alive, so they stay readable and unchanged while it lives, whatever happens to the file: nothing in a
// pack is overwritten in place. A pending import pins it.
class ValidatedBody final : public ThreadSafeRefCounted<ValidatedBody> {
    WTF_MAKE_TZONE_ALLOCATED(ValidatedBody);
public:
    const BodyKey& key() const;
    uint64_t version() const; // the envelope's commit identifier, which the writer never makes 0
    uint8_t highestTier() const;
    uint32_t llintThreshold() const; // the envelope's L, the LLInt warm-up compact scores by; 0 for a test body
    uint32_t counterProgress() const; // the envelope's P; 0 for a test body
    std::span<const uint8_t> section(SectionKind) const; // empty when absent; starts 8-byte aligned
    size_t fileSize() const; // the body's size: its extent in the pack, or a test body's buffer size
    ~ValidatedBody(); // drops the pack's mapping, or frees a test body's buffer; any thread that holds no JITCache lock

#if ENABLE(JITCACHE_TWINS)
    // A body no container check has seen, for the parts' own tests: the sections are copied into one buffer, in the
    // order given (kinds strictly increasing), each at an 8-byte-aligned address; highestTier() is the highest tier of
    // the given kinds, by the rows above. onDestroy runs first in the destructor, on the destroying thread. version is
    // not 0.
    struct TestSection {
        SectionKind kind;
        std::span<const uint8_t> bytes;
    };
    static Ref<ValidatedBody> createForTesting(const BodyKey&, uint64_t version, std::span<const TestSection>,
        Function<void()>&& onDestroy = { });
#endif

private:
    friend class OpenedArtifact; // open builds a body from the bytes validateBody accepted
    using SectionSpans = std::array<std::span<const uint8_t>, numberOfSectionKinds>; // by SectionKind; empty when absent
    // open fills the version, the tier, L, P and the spans from validateBody's BodyLayout, so this header needs nothing
    // from JITCacheContainer.h. A test body takes the buffer, whose address moving does not change, so the spans into it
    // stay valid; a body from the pack takes a reference to the mapping and the spans into it.
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress,
        MallocSpan<uint8_t>&& bytes, const SectionSpans&);
    ValidatedBody(const BodyKey&, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress,
        Ref<MappedPack>&&, std::span<const uint8_t> bytes, const SectionSpans&);

    const BodyKey m_key;
    const uint64_t m_version;
    const uint8_t m_highestTier;
    const uint32_t m_llintThreshold; // the envelope's L; 0 for a test body
    const uint32_t m_counterProgress; // the envelope's P; 0 for a test body
    const MallocSpan<uint8_t> m_buffer; // createForTesting's copy of its sections; fastMalloc aligns it to at least 8 bytes
    const RefPtr<MappedPack> m_pack; // the mapping the body borrows from; null for a test body
    const std::span<const uint8_t> m_bytes; // the body: its extent in m_pack's mapping, 8-aligned in a page-aligned mapping, or m_buffer
    const SectionSpans m_sections; // spans into m_bytes
#if ENABLE(JITCACHE_TWINS)
    Function<void()> m_onDestroy;
#endif
};

// What VMState::openBody answers: Missing, Unusable (the integrator has already raised invalid material) or Found with
// the body.
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

// What VMState::readBodySection answers: one section of a key's newest body, read without checksumming the body's other
// sections. Absent: no body has the key. Unusable: the integrator has already raised invalid material. Found: the section,
// which the lookup keeps readable while it lives. Movable.
class SectionLookup {
public:
    enum class Kind : uint8_t { Absent, Unusable, Found };
    Kind kind() const;
    std::span<const uint8_t> bytes() const LIFETIME_BOUND; // Found: the section, empty when the body lacks it; 8-byte aligned
    SectionLookup(SectionLookup&&);
    SectionLookup& operator=(SectionLookup&&);
    ~SectionLookup(); // drops what keeps the bytes; any thread that holds no JITCache lock

private:
    friend class VMState;
    SectionLookup(Kind, RefPtr<MappedPack>&&, RefPtr<ValidatedBody>&&, std::span<const uint8_t>);

    Kind m_kind;
    RefPtr<MappedPack> m_pack; // the store's mapping the bytes lie in
    RefPtr<ValidatedBody> m_body; // or the test override's body they lie in
    std::span<const uint8_t> m_bytes;
};

} // namespace JSC::JITCache
