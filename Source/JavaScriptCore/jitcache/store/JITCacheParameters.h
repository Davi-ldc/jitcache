#pragma once

#include <optional>
#include <stddef.h>
#include <stdint.h>

// The integrator's named parameters that code reads. The bench loop proposes each value from its first measurement, and a
// value enters the parameter table of HARNESS.md, and this file, only after human approval. An empty optional is a
// parameter the bench has not set yet. The parameters only the bench reads live in HARNESS.md's table alone.

namespace JSC::JITCache {

// The limit a producing role uses when the host passes none. Until the bench sets it, a producing role must pass a limit,
// and start rejects one that does not.
constexpr std::optional<size_t> defaultProducerLimitBytes { };

// The writer's staging buffer, until the bench tunes it.
constexpr size_t writerStagingBytes = 64 * 1024;

// The number of index deltas a pack's chain may hold before a commit writes a full index instead of a delta, which bounds
// the chain a reader walks when it opens the pack, until the bench tunes it.
constexpr uint32_t packDeltaChainBound = 32;

// The least time between two attempts to reopen cache/pack after the mapped pack was marked retired but the path still
// names it, which only a compaction that died between its mark and its rename leaves for long.
constexpr uint64_t retiredPackReopenIntervalMilliseconds = 100;

// The address space a reader maps beyond the pack's valid end, so the pack grows that far without a new mapping: at
// least this many bytes, and at least the valid end itself.
constexpr uint64_t packMappingSlackBytes = 256 * 1024 * 1024;

} // namespace JSC::JITCache
