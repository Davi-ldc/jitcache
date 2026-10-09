#pragma once

#include "UCBKeys.h"
#include <array>
#include <span>

namespace JSC::JITCache {

// Streaming FIPS 180-4 SHA-256 (SPEC-ucb.md section 3.8). The portable implementation is the reference. On x86_64 a path
// using the SHA extensions runs when CPUID reports SHA, SSSE3 and SSE4.1; on ARM64 a path using the SHA-256 instructions
// runs when getauxval(AT_HWCAP) reports HWCAP_SHA2. The choice is made once per process. Any part may use this class.
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

} // namespace JSC::JITCache
