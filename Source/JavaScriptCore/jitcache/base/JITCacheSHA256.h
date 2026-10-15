#pragma once

#include "BodyKey.h"
#include <array>
#include <span>
#include <wtf/Forward.h>
#include <wtf/Platform.h>

// Streaming FIPS 180-4 SHA-256, which every digest of JITCache uses: body keys, the header digest, the UCB lane's digests.
// The portable implementation is the reference. On x86_64 a path using the SHA extensions runs when CPUID reports SHA,
// SSSE3 and SSE4.1; on ARM64 a path using the SHA-256 instructions runs when getauxval(AT_HWCAP) reports HWCAP_SHA2. The
// choice is made once per process. Any part may use this class, on any thread; an object is one stream.

namespace JSC::JITCache {

class SHA256 {
public:
    SHA256();
    void update(std::span<const uint8_t>);
    // Pads and compresses the last block; the stream ends here.
    Digest256 finalize();
    static Digest256 hash(std::span<const uint8_t>);

private:
    std::array<uint32_t, 8> m_state; // the eight hash words
    std::array<uint8_t, 64> m_buffer; // the partial block update has not compressed yet
    size_t m_bufferLength; // bytes held in m_buffer, below 64
    uint64_t m_messageLength; // bytes passed to update so far
};

#if ENABLE(JITCACHE_TWINS)
// The FIPS 180-4 examples and random messages on every path the CPU offers and through the class, each whole and in
// updates that split it across blocks, compared with the expected digests and with the portable path. testjitcache runs it;
// it lives in JITCacheSHA256.cpp beside the paths, which that file keeps private. Writes the first difference to `failure`
// and returns false.
bool runSHA256SelfTest(String& failure);
#endif

} // namespace JSC::JITCache
