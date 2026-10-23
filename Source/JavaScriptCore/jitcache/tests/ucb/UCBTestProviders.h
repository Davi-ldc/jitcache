#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "SourceProvider.h"
#include "jitcache/base/BodyKey.h"
#include <array>
#include <optional>
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>

// The source providers of the UCB lane's tests: one that supplies a digest for its text.

namespace JSC::JITCache::Tests {

// A provider that supplies a digest for its text, as Bun's providers for compiled modules do. The digest is whatever the
// test gives, so a test can supply one that differs from the text.
class SuppliedDigestProvider final : public StringSourceProvider {
public:
    static Ref<SuppliedDigestProvider> create(const String& text, const std::optional<Digest256>& digest)
    {
        return adoptRef(*new SuppliedDigestProvider(text, digest));
    }

    std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const final { return m_digest; }

private:
    SuppliedDigestProvider(const String& text, const std::optional<Digest256>& digest)
        : StringSourceProvider(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Program)
        , m_digest(digest)
    {
    }

    const std::optional<Digest256> m_digest;
};

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
