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
