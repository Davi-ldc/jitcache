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

// The least time between the starts of two listings that a refresh without inotify, or with a listing pending, makes
// (container sub-SPEC section 6.3), until IB2 tunes it.
constexpr uint64_t fallbackListingIntervalMilliseconds = 1000;

// The number of ConsumerProducer runs IB10 chains between its Producer and its Consumer. Only the bench reads it.
constexpr std::optional<unsigned> benchConsumerProducerGenerations { };

} // namespace JSC::JITCache
