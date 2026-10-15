#pragma once

#include <wtf/Platform.h>

#if ENABLE(JIT)

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <stddef.h>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/UniqueArray.h>
#include <wtf/text/ASCIILiteral.h>

// The CB lane's interface: capture of a baseline CodeBlock's state into cb.state and cb.summary, scoring, validation, and
// the seeding of a newborn CodeBlock. CBFormat.h holds the sections' layouts.

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

enum class CBFaultKind : uint8_t { InvalidMaterial, RecordingFault };

enum class CBCheck : uint8_t {
    // validateState's checks of cb.state, in the order it runs them.
    StateHeader, ArgumentCount, ValueProfileCount, FamilyCount, StateLength,
    ValuePrediction, ArrayProfile, AllocationHint, IterationModes, EnumeratorModes,
    ToThisStatus, BranchBit, LazyOperand, TierUpHistory, Counter,
    // validateSummary's checks of cb.summary.
    SummaryHeader, SummaryLength, SummaryValue,
    // prepare's strict checks of the CodeBlock and of the captured counter, after validateState.
    StrictNewborn, StrictLinkState, StrictCounter,
    // capture's and scoreLive's own failures.
    CapturePairing, CaptureCharge, CaptureStrict,
};

struct CBFault {
    CBFaultKind kind;
    CBCheck check;
};

ASCIILiteral description(CBCheck); // names the failing step for status(vm)

// The structural rules of the two sections, which a strict capture applies to what it built and a strict prepare or
// decodeSummary to what it reads. Each reads the span (and the CB) in place, allocates nothing and returns the first
// failing check, or nothing.
std::optional<CBCheck> validateState(std::span<const uint8_t> stateSection, CodeBlock&);
std::optional<CBCheck> validateSummary(std::span<const uint8_t> summarySection);
// Whether a carried counter keeps the native invariant: its progress is not negative unless its threshold is infinite.
bool counterObeysNativeInvariant(const CBFormat::StateHeader&);

struct CBScore {
    uint64_t richnessUnits { 0 };
    bool counterWithheld { false }; // CounterMode::NotCarried: a polymorphic site kept the counter from traveling
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
};

class CBStateCapture {
    WTF_MAKE_NONCOPYABLE(CBStateCapture);
    WTF_MAKE_TZONE_ALLOCATED(CBStateCapture);
public:
    // VM thread, JS paused, API lock and heap access, no collector phase. hasPolymorphicSite is the ICs lane's bit for
    // the same CB, read in the same pause; when set, the counter does not travel.
    static std::expected<CBStateCapture, CBFault> capture(CodeBlock&, ProducerBudget&, bool strict, bool hasPolymorphicSite);
    static std::expected<CBScore, CBFault> scoreLive(CodeBlock&, bool strict, bool hasPolymorphicSite);

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

// Any thread that owns the material; reads only the span, validating it first with strict on.
std::expected<CBScore, CBFault> decodeSummary(std::span<const uint8_t> summarySection, bool strict);

class CBStateImport {
public:
    // The install steps, in this order, on the VM thread inside the install function's GC deferral, for a newborn CB
    // that no marker or compiler thread can reach before installCode. prepare reads the span and the CB and writes
    // neither, so after a failure elsewhere the next newborn CB of the body can prepare again from the same bytes.
    static std::expected<CBStateImport, CBFault> prepare(std::span<const uint8_t> stateSection, CodeBlock&, bool strict);

    void seedLinkedState(CodeBlock&) const; // after linking, before the CB has a JIT type; infallible
    CBCounterRestore finishCounter(CodeBlock&) const; // after native setup and the ICs lane's attach; infallible

#if ENABLE(JITCACHE_TWINS)
    // After finishCounter, with its result, and before installCode, after which a concurrent marker can merge into the
    // seeded profiles.
    void verifyTwins(CodeBlock&, const CBCounterRestore&, TwinReport&) const;
#endif

private:
    std::span<const uint8_t> m_bytes; // borrowed from the integrator, 8-byte aligned, alive until the last step returns
    const CBFormat::StateHeader* m_header { nullptr };
    std::array<uint32_t, 9> m_arrayOffsets { }; // the offsets of cb.state's nine arrays, in CBFormat::StateArray order
};

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
