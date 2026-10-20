#pragma once

#if ENABLE(JIT)

#include "ICSection.h"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Capture of the ICsBaseline section from a live baseline CB. The integrator calls these functions at the two capture
// points, the end of the success path of BaselineJITPlan::finalize and delta, on an eligible CB whose BaselineJITData is
// published, on the VM's thread with JS paused and no CodeBlock::m_lock held.
// summarizeBaselineICs and captureBaselineICs take the CB's m_lock and release it before returning. No function here
// drains a profile, materializes a property table, allocates a cell or memory, or stops for the collector.

namespace JSC {
class CodeBlock;
class ConcurrentJSLocker;
class HandlerPropertyInlineCache;
} // namespace JSC

namespace JSC::JITCache::ICs {

struct CallLinkSite; // ICSites.h

// What summarizing or capturing a live CB yields: the summary and the polymorphic bit, which the CB lane takes to decide
// whether the baseline counter travels.
struct CaptureSummary {
    Summary summary;
    bool hasPolymorphicSite { false };
};

enum class CaptureCheck : uint8_t {
    NoBaselineJITData, // the CB has no BaselineJITData
    OutputSizeMismatch, // the output span is not baselineICsSectionSize bytes long
    MoldMismatch, // IC i's access type differs from mold i's
};

struct CaptureError {
    CaptureCheck check;
    uint32_t siteIndex; // the IC's index for MoldMismatch, 0 otherwise
};

// The check's name in a fault's step, after "ics.": one lowercase token, its words joined by '-'.
ASCIILiteral description(CaptureCheck);

// The exact byte size of the CB's ICsBaseline section: the IC count of BaselineJITData and
// callLinkSiteCounts (zero groups without a metadata table). Reads counts only, takes no lock.
// Precondition: jitType() == BaselineJIT and baselineJITData() is non-null.
size_t baselineICsSectionSize(CodeBlock&);

// The summary and polymorphic bit the CB would capture now, for scoring candidates before capturing the winner. Takes
// CodeBlock::m_lock. Allocates no cell and no memory.
CaptureSummary summarizeBaselineICs(CodeBlock&);

// Writes the whole section into output, whose size must equal baselineICsSectionSize(codeBlock), and returns its summary
// and polymorphic bit. Takes CodeBlock::m_lock. Allocates no cell and no memory. Reads only.
std::expected<CaptureSummary, CaptureError> captureBaselineICs(CodeBlock&, std::span<uint8_t> output, StrictChecks);

// The one reader of live state that capture and the twin snapshot (ICTwins.cpp) share, so a snapshot and a capture of
// the same state agree field for field. Each returns the record capture writes for one site and must run under the
// site's CodeBlock::m_lock, which the locker witnesses. A site with a CallLinkInfo of its own gives that CallLinkInfo's
// mode, bits and varargs maximum and, at an opcode with a this profile, the accumulated state of the profile in its own
// CallSiteData. A lazy site without data of its own gives a record that is zero except the seen bit,
// LazyCallLinkInfo::hasExecutedOnce().
PropertyICRecord readPropertyICRecord(const ConcurrentJSLocker&, const HandlerPropertyInlineCache&);
CallLinkRecord readCallLinkRecord(const ConcurrentJSLocker&, const CallLinkSite&);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JIT)
