#pragma once

#include "CachedBytecode.h"
#include "jitcache/ucb/UCBFeedback.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <expected>
#include <optional>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>

namespace JSC {

class CodeBlock;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The UCB lane's sections of a capture: ucb.identity, ucb.core, ucb.feedback and a module's ucb.analysis. The integrator's
// capture glue calls these on the VM thread with JS paused, for an eligible CodeBlock whose UCB has a record, or, at delta
// under the delta capture All, for a recorded UCB whose body ran without one. liveRichness and savedRichness come from
// UCBFeedback.h.

class ProducerBudget;

// SavedAnalysisUnusable: reading the saved body's ucb.analysis, which a capture would copy, found the saved body damaged,
// and the read raised the fault itself, so the caller raises nothing more and commits nothing.
enum class UCBCaptureFailure : uint8_t { BudgetExceeded, SavedAnalysisUnusable };

struct UCBSections { // move-only
    UCBSections(Vector<uint8_t>&& identity, Ref<CachedBytecode>&& core, Vector<uint8_t>&& feedback, Vector<uint8_t>&& analysis, Ref<ProducerBudget>&&, size_t chargedBytes);
    UCBSections(UCBSections&&); // takes the source's budget and charge; the source keeps neither
    UCBSections& operator=(UCBSections&&) = delete;
    ~UCBSections(); // releases chargedBytes from budget when budget is non-null

    Vector<uint8_t> identity; // 113 bytes and the butterfly map, padded to a multiple of 8, then the child holder digests
    Ref<CachedBytecode> core;
    Vector<uint8_t> feedback;
    Vector<uint8_t> analysis; // a module's ucb.analysis, empty when the body gets none
    RefPtr<ProducerBudget> budget; // the budget buildSections charged; null after a move
    size_t chargedBytes;
};

// Empty, and the UCB is not captured, when it has no record, which a UCB a native decode made never has, or when it is a
// direct eval whose record keeps no context digest, which no record made while production was active lacks.
std::optional<UCBRegistry::RecordView> captureRecord(VM&, const UnlinkedCodeBlock&);
// L, the LLInt threshold the import skips, as the UCB's history scales it at capture, which the body's envelope records
// for maintenance: ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()). A body without an image carries its L as
// carriedLLIntPoints instead (UCBFeedback.h).
uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock&);
// The UFE whose slot the request filled with the CodeBlock's UCB, which the core's encoding and the identity section read:
// its owner executable's, for function code; null for any other.
const UnlinkedFunctionExecutable* holderOf(const CodeBlock&);
// Only for an accepted capture; holder is holderOf(cb) for a capture with a CodeBlock, the record's for one without;
// BodyImage is Present for a capture with a CodeBlock and Absent for one without, which writes no UCB copies. A module's
// UCB also gets its analysis: its live analysis source's encoding, or else a copy of the saved body's ucb.analysis, or
// none. A refused charge returns BudgetExceeded. Every failure releases what the call charged.
std::expected<UCBSections, UCBCaptureFailure> buildSections(VM&, UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder, BodyImage, ProducerBudget&);

} // namespace JSC::JITCache
