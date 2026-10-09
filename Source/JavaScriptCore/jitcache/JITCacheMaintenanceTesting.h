#pragma once

// Maintenance's test hooks (SPEC-integrator.maintenance.md section 7), a private header that only twins builds fill:
// JITCacheMaintenance.h is exported to Bun and declares nothing twins-only (SPEC-integrator.md section 3.5).

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include <optional>
#include <stdint.h>

namespace JSC::JITCache::Maintenance::Testing {

// Both hooks are process-wide and count from 1 within one call of clean or compact; nullopt turns a hook off. Tests M3
// and M5 set them while no maintenance call runs.

// Each apply's n-th body unlink fails with EIO, as if unlinkat had returned it (section 4.5).
void setFailingBodyUnlink(std::optional<uint64_t> n);

// Each apply stops right after its n-th unlink of a body or of header, whether or not that unlink succeeded. A stopped
// apply removes nothing more, bumps no epoch and returns Failed; returning releases the lock, so the artifact is left as a
// process that died at that point would leave it.
void setStopAfterUnlink(std::optional<uint64_t> n);

} // namespace JSC::JITCache::Maintenance::Testing

#endif // ENABLE(JITCACHE_TWINS)
