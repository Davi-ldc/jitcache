#pragma once

#include <wtf/Platform.h>

#if ENABLE(JIT)

#include <array>
#include <optional>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UniqueArray.h>

// The CB lane's interface: capture of a baseline CodeBlock's state into cb.state and cb.summary, scoring, and the seeding
// of a newborn CodeBlock. CBFormat.h holds the sections' layouts.

namespace JSC {

class CodeBlock;

} // namespace JSC

namespace JSC::JITCache {

namespace CBFormat {

struct StateHeader;

} // namespace CBFormat

class ProducerBudget; // the integrator's producer budget, which capture charges for its two buffers
#if ENABLE(JITCACHE_TWINS)
class TwinReport; // the integrator's sink for twin differences
#endif

struct CBScore {
    uint64_t richnessUnits { 0 };
    bool counterWithheld { false }; // the counter did not travel (CounterMode::NotCarried)
    uint32_t counterProgress { 0 };
};

// What finishCounter decided.
struct CBCounterRestore {
    bool carried { false }; // the section carried the counter (CounterMode::Carried)
    bool crossed { false }; // carried, finite T: checkIfThresholdCrossedAndSet found the consumer's threshold reached
    // carried, finite T: 0 when crossed, else the slice native code armed; carried, T == INT32_MAX: the captured slice;
    // not carried: the slice setup armed
    int64_t nativeSlice { 0 };
    int64_t slice { 0 }; // the slice the counter holds: max(nativeSlice, floor) when carried, else nativeSlice
    double startupDeferralScale { 1 }; // carried: the VM's scale finishCounter passed to the check; not carried: 1
};

class CBStateCapture {
    WTF_MAKE_NONCOPYABLE(CBStateCapture);
    WTF_MAKE_TZONE_ALLOCATED(CBStateCapture);
public:
    // VM thread, JS paused, API lock and heap access, no collector phase. hasPolymorphicSite is the ICs lane's bit for
    // the same CB, read in the same pause; when set, the counter does not travel. capture returns nothing when the charge
    // for its two buffers fails.
    static std::optional<CBStateCapture> capture(CodeBlock&, ProducerBudget&, bool hasPolymorphicSite);
    static CBScore scoreLive(CodeBlock&, bool hasPolymorphicSite);

    CBStateCapture(CBStateCapture&&);
    ~CBStateCapture(); // frees both buffers and releases their charge

    const CBScore& score() const;
    std::span<const uint8_t> stateSection() const;
    std::span<const uint8_t> summarySection() const;

private:
    // How capture makes the object it returns: adopts both buffers, which hold stateSize and summarySize bytes, and the
    // charge of stateSize + summarySize against the budget, which the destructor releases.
    CBStateCapture(ProducerBudget& budget, UniqueArray<uint64_t>&& state, size_t stateSize, UniqueArray<uint64_t>&& summary, size_t summarySize, const CBScore& score)
        : m_budget(&budget)
        , m_state(WTF::move(state))
        , m_summary(WTF::move(summary))
        , m_stateSize(stateSize)
        , m_summarySize(summarySize)
        , m_score(score)
    {
    }

    ProducerBudget* m_budget { nullptr };
    UniqueArray<uint64_t> m_state; // 8-byte aligned
    UniqueArray<uint64_t> m_summary;
    size_t m_stateSize { 0 };
    size_t m_summarySize { 0 };
    CBScore m_score;
};

// Any thread that owns the material; reads only the span.
CBScore decodeSummary(std::span<const uint8_t> summarySection);

class CBStateImport {
public:
    // The install steps, in this order, on the VM thread inside the install function's GC deferral, for a newborn CB
    // that no marker or compiler thread can reach before installCode. prepare reads only the span and writes nothing,
    // so after a failure elsewhere the next newborn CB of the body can prepare again from the same bytes.
    static CBStateImport prepare(std::span<const uint8_t> stateSection);

    void seedLinkedState(CodeBlock&) const; // after linking, before the CB has a JIT type; infallible
    CBCounterRestore finishCounter(CodeBlock&) const; // after native setup; infallible

#if ENABLE(JITCACHE_TWINS)
    // After finishCounter, with its result, and before installCode, after which a concurrent marker can merge into the
    // seeded profiles.
    void verifyTwins(CodeBlock&, const CBCounterRestore&, TwinReport&) const;
#endif

private:
    std::span<const uint8_t> m_bytes; // borrowed from the body until finishCounter and verifyTwins return
    const CBFormat::StateHeader* m_header { nullptr };
    std::array<uint32_t, 9> m_arrayOffsets { }; // where each of the nine arrays after the header starts
};

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
