#pragma once

#if ENABLE(JIT)

#include "ICSection.h"
#include "PropertyInlineCache.h"
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <wtf/Assertions.h>

// Restoration of the ICsBaseline section into a newborn CB. The install function calls the three steps on the VM's
// thread, in this order:
//
//   prepareBaselineICs      newborn CB: finishCreation done, no JIT type, never run; takes no lock
//   seedCallLinkHistory     linked CB, no JIT type; takes no lock
//   attachPropertyICState   BaselineJITData published, executable not yet updated; takes CodeBlock::m_lock
//
// Prepare writes nothing and allocates nothing; seed and attach cannot fail and allocate nothing. Seed and attach write
// the state restoredCallLink and restoredPropertyIC derive from each record, field by field, through the native objects'
// own members, and only into the CB that prepare received.

namespace JSC {
class BaselineJITCode;
class CodeBlock;
} // namespace JSC

namespace JSC::JITCache::ICs {

// The strict checks prepare composes after parseSection's, in its order. Each returns the first failure it finds, with
// Check::MoldPairing, MetadataLayout, MoldMegamorphicBit and NewbornCallLinks respectively. The first three read only
// their arguments and need no VM; tests build molds as plain BaselineUnlinkedPropertyInlineCache values.
std::optional<Invalid> checkMoldPairing(const SectionView&, std::span<const BaselineUnlinkedPropertyInlineCache> molds);
std::optional<Invalid> checkMetadataLayout(const SectionView&, const CallLinkSiteCounts&);
std::optional<Invalid> checkMolds(std::span<const BaselineUnlinkedPropertyInlineCache>);
std::optional<Invalid> checkNewbornCodeBlock(CodeBlock&);

// Views into a body's ICsBaseline payload, validated under strict, for the CB that prepared them. It owns nothing, so
// a preparation another lane's failure drops needs no cleanup, and the next newborn CB of the UCB prepares again. The
// integrator keeps the payload alive and unchanged until attachPropertyICState returns.
class PreparedBaselineICs {
public:
    std::span<const PropertyICRecord> propertyICs() const { return m_propertyICs; }
    std::span<const CallLinkRecord> callLinks() const { return m_callLinks; }
    Summary summary() const { return m_summary; }

private:
    friend std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t>, const BaselineJITCode&, CodeBlock&, StrictChecks);
    // Seed and attach read m_codeBlock in their debug assertions.
    friend void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);
    friend void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

    std::span<const PropertyICRecord> m_propertyICs; // borrowed from the payload
    std::span<const CallLinkRecord> m_callLinks; // borrowed from the payload, canonical order
    Summary m_summary;
#if ASSERT_ENABLED
    const CodeBlock* m_codeBlock { nullptr }; // seed and attach assert they receive the prepared CB
#endif
};

// preparedCode is PreparedImage::code(), read before the image's commit; it holds the producer's molds in mold order, and
// prepare keeps no reference to it. With StrictChecks::Yes prepare runs parseSection's checks, then checkMoldPairing,
// checkMetadataLayout, checkMolds and checkNewbornCodeBlock, in that order, and returns the first failure, which is
// invalid material; otherwise it only slices the section by its header's counts.
std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks);

// After linking and before CodeBlock::setupWithUnlinkedBaselineCode, on the prepared CB only.
void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);

// Right after CodeBlock::setupWithUnlinkedBaselineCode returns and before the baseline counter is re-sliced, on the
// prepared CB only. Takes CodeBlock::m_lock, which the caller must not hold.
void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
