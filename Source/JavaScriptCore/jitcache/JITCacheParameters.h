#pragma once

#include <optional>
#include <stddef.h>
#include <stdint.h>

namespace JSC::JITCache {

// The integrator's named parameters (SPEC-integrator.md section 16). The bench loop proposes each value from its first
// measurement, and a value enters the parameter table of HARNESS.md, and this file, only after human approval. An empty
// optional is a parameter the bench has not set yet.

// The limit a producing role uses when the host passes none. Until the bench sets it, a producing role must pass a limit
// (section 3.2, step 3).
constexpr std::optional<size_t> defaultProducerLimitBytes { };

// THREAD's fixed fraction of the native cost an import may take, and THREAD's smallest body that bound covers (IB3).
// Only the bench reads them, and it raises an alert and fails nothing; the bench chooses the size measure of a body with
// the second's value.
constexpr std::optional<double> installationBoundFraction { };
constexpr std::optional<uint64_t> installationBoundSmallestBody { };

// The writer's staging buffer (container sub-SPEC section 8.1), until IB5 tunes it.
constexpr size_t writerStagingBytes = 64 * 1024;

// The number of index deltas a pack's chain may hold before a commit writes a full index instead of a delta, which bounds
// the chain a reader walks when it opens the pack, until IB2 tunes it.
constexpr uint32_t packDeltaChainBound = 32;

// The least time between two attempts to reopen cache/pack after the mapped pack was marked retired but the path still
// names it, which only a compaction that died between its mark and its rename leaves for long.
constexpr uint64_t retiredPackReopenIntervalMilliseconds = 100;

// The address space a reader maps beyond the pack's valid end, so the pack grows that far without a new mapping: at
// least this many bytes, and at least the valid end itself.
constexpr uint64_t packMappingSlackBytes = 256 * 1024 * 1024;

// The number of ConsumerProducer runs IB10 chains between its Producer and its Consumer. Only the bench reads it.
constexpr std::optional<unsigned> benchConsumerProducerGenerations { };

} // namespace JSC::JITCache
