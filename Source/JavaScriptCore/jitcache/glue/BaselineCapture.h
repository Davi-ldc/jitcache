#pragma once

#include "jitcache/base/BodyKey.h"
#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <optional>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/Platform.h>
#include <wtf/text/ASCIILiteral.h>

// The capture of one body and its commit: from an eligible CB, at the baseline tier with its image, or, for delta's bodies
// that ran without compiling, from the UCB alone at tier 0. BaselineCapture.cpp defines them beside the finalize capture,
// which BaselineJITPlan::finalize calls. Everything here runs on the VM thread with JS paused; nothing a capture does
// allocates a cell, drains a profile, stops for the collector or finalizes a plan, so captures never nest.

#if ENABLE(JIT)

namespace JSC {

class CodeBlock;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

class CaptureClock;
struct CaptureTrace;

// Captures never nest, since nothing a capture does can start another. Debug builds count the captures in progress, and
// the finalize capture and delta assert on entry that none is.
class CaptureInProgressScope {
    WTF_MAKE_NONCOPYABLE(CaptureInProgressScope);
public:
    explicit CaptureInProgressScope(VMState& state)
#if ASSERT_ENABLED
        : m_state(state)
#endif
    {
#if ASSERT_ENABLED
        ASSERT(!m_state.capturesInProgress());
        ++m_state.capturesInProgress();
#else
        UNUSED_PARAM(state);
#endif
    }

    ~CaptureInProgressScope()
    {
#if ASSERT_ENABLED
        --m_state.capturesInProgress();
#endif
    }

private:
#if ASSERT_ENABLED
    VMState& m_state;
#endif
};

// The CB is a candidate when it runs its own capturable baseline code and the UCB lane recorded its key; the record is
// that key's.
std::optional<UCBRegistry::RecordView> candidateRecord(VM&, CodeBlock&);

struct CommitOutcome {
    enum class Kind : uint8_t { Committed, Beaten, NotEligible, Faulted };
    Kind kind;
    uint64_t fileSize { 0 }; // Committed only
};

// How the capture event names a capture that built its sections: committed, or beaten when the bytes it built no longer
// beat the saved body.
inline ASCIILiteral eventOutcome(CommitOutcome::Kind kind)
{
    return kind == CommitOutcome::Kind::Committed ? "committed"_s : "beaten"_s;
}

// Builds one capture whose live score beats the saved body's and commits it. Each lane's output holds its own charges and
// releases them when it goes, and the ICs buffer is freed before its charge is released, so every return leaves only what
// a commit keeps: the kept entry, the index entry's charge and the body.
CommitOutcome commitCapture(VM&, VMState&, CodeBlock&, const BodyKey&, const std::optional<CaptureScore>& saved, const CaptureClock&,
    CaptureTrace&);

// commitCapture for a body that ran without an image, which delta's phase 3 finds: the UCB lane's sections alone, built
// from the UCB and the holder its record keeps, at tier 0, once its live score in trace has beaten the saved body. Its L
// is the LLInt progress it carries and its P zero, so compact orders it by that progress.
CommitOutcome commitImagelessCapture(VM&, VMState&, UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder, const BodyKey&,
    const CaptureClock&, CaptureTrace&);

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
