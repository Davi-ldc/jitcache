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
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The sections a capture writes (SPEC-ucb.md section 8). The integrator's capture glue calls these on the VM thread with JS
// paused, for an eligible CodeBlock whose UCB has a record. liveRichness and savedRichness come from UCBFeedback.h.

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
// ucb.thresholdForJIT(Options::thresholdForJITAfterWarmUp()).
uint32_t envelopeLLIntThreshold(UnlinkedCodeBlock&);
// Only for an accepted capture (section 8.2). A refused charge releases what the call charged and returns BudgetExceeded;
// SC1 or SC2 failing under strict returns StrictCheckFailed. The integrator raises either as a recording fault.
std::expected<UCBSections, UCBCaptureFailure> buildSections(VM&, const CodeBlock&, ProducerBudget&);

} // namespace JSC::JITCache
