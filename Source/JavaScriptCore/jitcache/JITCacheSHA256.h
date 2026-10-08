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
