#pragma once

#include "CachedBytecode.h"
#include "UCBRegistry.h"
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

// The sections a capture writes (SPEC-ucb.md section 8). The integrator's capture glue calls these on the VM thread with JS
// paused, for an eligible CodeBlock whose UCB has a record, or, at delta under the delta capture All, for a recorded UCB
// whose body ran without one (section 8.3). liveRichness and savedRichness come from UCBFeedback.h.

class ProducerBudget;

enum class UCBCaptureFailure : uint8_t { BudgetExceeded, StrictCheckFailed };

struct UCBSections { // move-only
    UCBSections(Vector<uint8_t>&& identity, Ref<CachedBytecode>&& core, Vector<uint8_t>&& feedback, Ref<ProducerBudget>&&, size_t chargedBytes);
    UCBSections(UCBSections&&); // takes the source's budget and charge; the source keeps neither
    UCBSections& operator=(UCBSections&&) = delete;
    ~UCBSections(); // releases chargedBytes from budget when budget is non-null

    Vector<uint8_t> identity; // 152 bytes plus the atom and butterfly maps
    Ref<CachedBytecode> core;
    Vector<uint8_t> feedback;
    RefPtr<ProducerBudget> budget; // the budget buildSections charged; null after a move
    size_t chargedBytes;
};

// Empty, and the UCB is not captured, when it has no record, or when it is a direct eval whose record keeps no context
// digest, which no record made while production was active lacks (section 3.4).
std::optional<UCBRegistry::RecordView> captureRecord(VM&, const UnlinkedCodeBlock&);
// L, the LLInt threshold the import skips, as the UCB's history scales it at capture (THREAD Maintenance):
// ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()). A body without an image carries its L as
// carriedLLIntPoints instead (UCBFeedback.h).
uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock&);
// The UFE whose slot the request filled with the CodeBlock's UCB, which the core's encoding and the identity section read:
// its owner executable's, for function code; null for any other.
const UnlinkedFunctionExecutable* holderOf(const CodeBlock&);
// Only for an accepted capture (section 8.2), of a UCB with a record and, for function code, its holder, which a capture
// without a CodeBlock takes from the record (section 8.3). A refused charge releases what the call charged and returns
// BudgetExceeded; SC1 or SC2 failing under strict returns StrictCheckFailed. The integrator raises either as a recording
// fault.
std::expected<UCBSections, UCBCaptureFailure> buildSections(VM&, UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder, ProducerBudget&);

} // namespace JSC::JITCache
