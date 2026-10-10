#include "config.h"
#include "ValidatedBody.h"

#include <sys/mman.h>
#include <wtf/Assertions.h>
#include <wtf/TZoneMallocInlines.h>

#if ENABLE(JITCACHE_TWINS)
#include <algorithm>
#include <optional>
#include <wtf/CheckedArithmetic.h>
#include <wtf/StdLibExtras.h>
#endif

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ValidatedBody);

ValidatedBody::ValidatedBody(const BodyKey& key, uint64_t version, uint8_t highestTier, uint32_t llintThreshold, uint32_t counterProgress, std::span<const uint8_t> mapping, const SectionSpans& sections)
    : m_key(key)
    , m_version(version)
    , m_highestTier(highestTier)
    , m_llintThreshold(llintThreshold)
    , m_counterProgress(counterProgress)
    , m_mapping(mapping)
    , m_sections(sections)
{
}

ValidatedBody::~ValidatedBody()
{
#if ENABLE(JITCACHE_TWINS)
    if (m_onDestroy)
        m_onDestroy();
#endif
    // The store maps a body from a page boundary and hands over exactly what mmap returned (container sub-SPEC
    // section 7.2); a test body has no mapping, and its buffer goes with m_testBuffer.
    if (!m_mapping.empty()) {
        int result = munmap(const_cast<uint8_t*>(m_mapping.data()), m_mapping.size());
        ASSERT_UNUSED(result, !result);
    }
}

const BodyKey& ValidatedBody::key() const
{
    return m_key;
}

uint64_t ValidatedBody::version() const
{
    return m_version;
}

uint8_t ValidatedBody::highestTier() const
{
    return m_highestTier;
}

uint32_t ValidatedBody::llintThreshold() const
{
    return m_llintThreshold;
}

uint32_t ValidatedBody::counterProgress() const
{
    return m_counterProgress;
}

std::span<const uint8_t> ValidatedBody::section(SectionKind kind) const
{
    ASSERT(static_cast<unsigned>(kind) < numberOfSectionKinds);
    return m_sections[static_cast<unsigned>(kind)];
}

size_t ValidatedBody::fileSize() const
{
#if ENABLE(JITCACHE_TWINS)
    if (m_mapping.empty())
        return m_testBuffer.span().size();
#endif
    return m_mapping.size();
}

#if ENABLE(JITCACHE_TWINS)

namespace ValidatedBodyInternal {

// The tier byte of each kind's directory entry (SPEC-integrator.md section 6.1): 0 for the UCB sections every tier
// shares, 1 for the baseline's.
static uint8_t tierOf(SectionKind kind)
{
    switch (kind) {
    case SectionKind::UCBIdentity:
    case SectionKind::UCBCore:
    case SectionKind::UCBFeedback:
        return 0;
    case SectionKind::ImageBaseline:
    case SectionKind::BakedFactsBaseline:
    case SectionKind::ImageTwinsBaseline:
    case SectionKind::CBStateBaseline:
    case SectionKind::CBSummaryBaseline:
    case SectionKind::ICsBaseline:
        return 1;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

} // namespace ValidatedBodyInternal

Ref<ValidatedBody> ValidatedBody::createForTesting(const BodyKey& key, uint64_t version, std::span<const TestSection> sections, Function<void()>&& onDestroy)
{
    // A test's own preconditions, checked in every build: a misbuilt body would test nothing.
    RELEASE_ASSERT(version);

    // Lay the sections out in the order given, each at the next multiple of 8 from the buffer's start.
    std::array<size_t, numberOfSectionKinds> offsets { };
    size_t bufferSize = 0;
    uint8_t highestTier = 0;
    std::optional<unsigned> previousKind;
    for (size_t i = 0; i < sections.size(); ++i) {
        unsigned kind = static_cast<unsigned>(sections[i].kind);
        RELEASE_ASSERT(kind < numberOfSectionKinds);
        RELEASE_ASSERT(!previousKind || *previousKind < kind);
        previousKind = kind;
        CheckedSize aligned = bufferSize;
        aligned += static_cast<size_t>(7);
        RELEASE_ASSERT(!aligned.hasOverflowed());
        offsets[i] = aligned.value() & ~static_cast<size_t>(7);
        CheckedSize end = offsets[i];
        end += sections[i].bytes.size();
        RELEASE_ASSERT(!end.hasOverflowed());
        bufferSize = end.value();
        highestTier = std::max(highestTier, ValidatedBodyInternal::tierOf(sections[i].kind));
    }

    // fastMalloc returns memory aligned to at least 8 bytes, which keeps every offset above 8-byte aligned in memory.
    MallocSpan<uint8_t> buffer;
    if (bufferSize) {
        buffer = MallocSpan<uint8_t>::malloc(bufferSize);
        RELEASE_ASSERT(!(reinterpret_cast<uintptr_t>(buffer.span().data()) % 8));
    }

    SectionSpans spans { };
    for (size_t i = 0; i < sections.size(); ++i) {
        auto destination = buffer.mutableSpan().subspan(offsets[i], sections[i].bytes.size());
        if (!destination.empty())
            memcpySpan(destination, sections[i].bytes);
        spans[static_cast<unsigned>(sections[i].kind)] = buffer.span().subspan(offsets[i], sections[i].bytes.size());
    }

    // Moving the buffer into the body keeps its address, so the spans stay valid. A test body has no envelope, so its L
    // and P are 0.
    Ref<ValidatedBody> body = adoptRef(*new ValidatedBody(key, version, highestTier, 0, 0, { }, spans));
    body->m_testBuffer = WTF::move(buffer);
    body->m_onDestroy = WTF::move(onDestroy);
    return body;
}

#endif // ENABLE(JITCACHE_TWINS)

BodyLookup::BodyLookup(Kind kind, RefPtr<ValidatedBody>&& body)
    : m_kind(kind)
    , m_body(WTF::move(body))
{
    ASSERT((m_kind == Kind::Found) == !!m_body);
}

BodyLookup BodyLookup::missing()
{
    return BodyLookup { Kind::Missing, nullptr };
}

BodyLookup BodyLookup::unusable()
{
    return BodyLookup { Kind::Unusable, nullptr };
}

BodyLookup BodyLookup::found(Ref<ValidatedBody>&& body)
{
    return BodyLookup { Kind::Found, WTF::move(body) };
}

auto BodyLookup::kind() const -> Kind
{
    return m_kind;
}

RefPtr<ValidatedBody> BodyLookup::body() const
{
    return m_body;
}

} // namespace JSC::JITCache
