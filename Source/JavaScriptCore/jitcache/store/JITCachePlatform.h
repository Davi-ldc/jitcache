#pragma once

#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Platform.h>

// What JITCache reads from the process and the CPU: the facts the artifact header records, and the CRC-32C every
// checksum of the container uses. Every function runs on any thread. No lane includes it.

namespace JSC::JITCache {

// Process facts, which the header records and every start compares.

struct BuildID {
    std::array<uint8_t, 64> bytes { }; // the ID in the first size bytes, zero after
    uint8_t size { 0 }; // 0: the object has no ID
};

struct ProcessFacts {
    BuildID mainExecutable;
    std::optional<BuildID> engineObject; // set when the engine is an object of its own
    std::array<bool, 3> mustMatch { }; // by the option index of the header's option records (JITCacheOptions.h)
    uint64_t cpuFeatures { 0 }; // bit i is the answer of predicate i of the CPU feature vector (JITCachePlatform.cpp)
};

// Any thread. Computed once, at the first call, which start makes after the first VM froze the options; while
// removeMainBuildIDForTesting is set, the copy it returns has a main executable without an ID.
ProcessFacts processFacts();
// Any thread. CRC-32C of RFC 3720 over a stream: crc32c(bytes) = ~crc32cExtend(~0u, bytes). It uses SSE4.2's crc32 on
// an x86_64 CPU that has it, the CRC extension's crc32c on an ARM64 target built with it, and a byte table otherwise.
uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t>);
#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool);
// The byte table crc32cExtend falls back to, so a test can compare it with the instructions on a CPU that has them.
uint32_t crc32cExtendWithTableForTesting(uint32_t state, std::span<const uint8_t>);
#endif

} // namespace JSC::JITCache
