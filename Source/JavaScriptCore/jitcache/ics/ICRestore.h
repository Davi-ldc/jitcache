#pragma once

#if ENABLE(JIT)

#include "jitcache/ics/ICSection.h"
#include <cstdint>
#include <span>

// Restoration of the ICsBaseline section into a newborn CB. The install function calls the three steps on the VM's
// thread, in this order:
//
//   prepareBaselineICs      reads only the section; takes no lock
//   seedCallLinkHistory     linked CB, no JIT type, inside the install's GC deferral; takes no lock
//   attachPropertyICState   BaselineJITData published, executable not yet updated; takes CodeBlock::m_lock
//
// Prepare writes nothing and allocates nothing. Seed and attach cannot fail; attach allocates nothing, and seeding
// allocates, through the native LazyCallLinkInfo::ensure, one CallSiteData for each site that comes back with data of its
// own, and no cell. Seed and attach write the state restoredCallLink and restoredPropertyIC derive from each record,
// field by field, through the native objects' own members.

namespace JSC {
class CodeBlock;
} // namespace JSC

namespace JSC::JITCache::ICs {

// Views into a body's ICsBaseline payload. It owns nothing, so a preparation another lane's failure drops needs no
// cleanup, and the next newborn CB of the UCB prepares again. The integrator keeps the payload alive and unchanged
// until attachPropertyICState returns, and in twins builds until checkRestoredBaselineICs returns.
class PreparedBaselineICs {
public:
    std::span<const PropertyICRecord> propertyICs() const { return m_propertyICs; }
    std::span<const CallLinkRecord> callLinks() const { return m_callLinks; }
    // Index in callLinks() of the first record of each canonical position's group: the sum of the siteCounts of the
    // groups before it, so a position without a group has a count of zero. Record k of a group describes the site of
    // that opcode whose metadata ID is k.
    const CallLinkSiteCounts& callLinkGroupStarts() const { return m_callLinkGroupStarts; }

private:
    friend PreparedBaselineICs prepareBaselineICs(std::span<const uint8_t>);

    std::span<const PropertyICRecord> m_propertyICs; // borrowed from the payload
    std::span<const CallLinkRecord> m_callLinks; // borrowed from the payload, canonical order
    CallLinkSiteCounts m_callLinkGroupStarts { }; // computed from the group table
};

// Slices the section by its header's counts, which it trusts, and computes the group starts from the group table.
PreparedBaselineICs prepareBaselineICs(std::span<const uint8_t> section);

// After linking and before CodeBlock::setupWithUnlinkedBaselineCode, on the newborn CB, inside the install function's
// GC deferral: a CallSiteData that seeding gives a site is reported to the heap in batches, and under the deferral a
// report only records deferred work.
void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);

// Right after CodeBlock::setupWithUnlinkedBaselineCode returns and before the baseline counter is re-sliced, on the CB
// that seeding received. Takes CodeBlock::m_lock, which the caller must not hold.
void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
