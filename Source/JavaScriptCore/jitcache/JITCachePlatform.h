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

#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Platform.h>

namespace JSC {

// The engine's CRC-32C of runtime/CachedTypes.cpp, with external linkage (SPEC-integrator.md section 14.2), so the
// container's checksum and the bytecode cache's are one implementation: CPU instructions where they exist, a byte table
// otherwise. It takes and returns the running state, without the initial value or the final XOR.
uint32_t crc32c(uint32_t crc, std::span<const uint8_t> bytes);

} // namespace JSC

namespace JSC::JITCache {

// Process facts (SPEC-integrator.md section 5.2), which the header records and every start compares.

struct BuildID {
    std::array<uint8_t, 64> bytes { }; // the ID in the first size bytes, zero after
    uint8_t size { 0 }; // 0: the object has no ID
};

struct ProcessFacts {
    BuildID mainExecutable;
    std::optional<BuildID> engineObject; // set when the engine is an object of its own
    std::array<bool, 3> mustMatch { }; // by the option index of container sub-SPEC section 3.1
    uint64_t cpuFeatures { 0 }; // bit i is predicate i of section 5.2
};

// Any thread. Computed once, at the first call, which start makes after the first VM froze the options; while
// removeMainBuildIDForTesting is set, the copy it returns has a main executable without an ID.
ProcessFacts processFacts();
// CRC-32C of RFC 3720 over a stream (container sub-SPEC section 4.4): crc32c(bytes) = ~crc32cExtend(~0u, bytes).
uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>);
#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool);
#endif

} // namespace JSC::JITCache
