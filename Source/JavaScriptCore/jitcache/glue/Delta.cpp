#include "config.h"

#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "CodeBlockSet.h"
#include "DeferGC.h"
#include "HeapInlines.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/glue/BaselineCapture.h"
#include "jitcache/glue/CaptureEvents.h"
#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/glue/KeptSummaries.h"
#include "jitcache/image/ImageCapture.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheBench.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/ucb/UCBCapture.h"
#include "jitcache/ucb/UCBFeedback.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBSections.h"
#include <algorithm>
#include <optional>
#include <wtf/CheckedArithmetic.h>
#include <wtf/HashMap.h>
#include <wtf/Locker.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// delta: its three phases, which capture every eligible CodeBlock and, with the delta capture All, the bodies that ran
// without compiling, and the compaction that ends it. Everything runs on the VM thread with JS paused, under one GC
// deferral.

namespace JSC::JITCache {

#if ENABLE(JIT)

namespace DeltaInternal {

// A candidate of phase 1: an eligible CodeBlock.
struct CodeBlockCandidate {
    CodeBlock* codeBlock;
    LiveScore live;
};

// A candidate of phase 3: a recorded UCB whose body ran without an eligible CodeBlock, with the holder
// its record keeps for function code.
struct ImagelessCandidate {
    UnlinkedCodeBlock* codeBlock;
    const UnlinkedFunctionExecutable* holder;
    LiveScore live;
};

// The candidates of phase 1 or phase 3: each key's candidates in decreasing order under beats, a tie after the ones met
// earlier. The table is charged per key as a production table's entry. A key's vector has one inline slot, which the
// entry's buckets hold, and grows only explicitly: the whole new heap capacity is charged before the growth and the old
// heap capacity released after it. Destroying the table frees it and then releases every charge, since the table lives
// only inside one delta and is no production memory.
template<typename Candidate>
class DeltaCandidates {
    WTF_MAKE_NONCOPYABLE(DeltaCandidates);
public:
    static constexpr size_t inlineCandidates = 1;
    using Candidates = Vector<Candidate, inlineCandidates>;
    using Table = HashMap<BodyKey, Candidates, BodyKeyHash, BodyKeyHashTraits>;
    static constexpr size_t bucketBytes = sizeof(typename Table::KeyValuePairType);

    explicit DeltaCandidates(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~DeltaCandidates()
    {
        m_table.clear();
        if (m_chargedBytes)
            m_budget.release(m_chargedBytes);
    }

    // False when a charge was refused, with nothing added.
    [[nodiscard]] bool add(const BodyKey& key, const Candidate& candidate)
    {
        auto iterator = m_table.find(key);
        if (iterator == m_table.end()) {
            size_t charge = newEntryCharge(bucketBytes, !!m_chargedBytes);
            if (!m_budget.tryCharge(charge))
                return false;
            m_chargedBytes += charge;
            m_table.add(key, Candidates { }).iterator->value.append(candidate);
            return true;
        }

        Candidates& candidates = iterator->value;
        if (candidates.size() == candidates.capacity()) {
            size_t oldCapacity = candidates.capacity();
            CheckedSize newHeapBytes = oldCapacity;
            newHeapBytes *= 2u;
            newHeapBytes *= sizeof(Candidate);
            if (newHeapBytes.hasOverflowed() || !m_budget.tryCharge(newHeapBytes.value()))
                return false;
            candidates.reserveCapacity(oldCapacity * 2);
            // The first growth out of the inline slot releases nothing: the entry's bucket charge covers that slot.
            size_t oldHeapBytes = oldCapacity > inlineCandidates ? oldCapacity * sizeof(Candidate) : 0;
            if (oldHeapBytes)
                m_budget.release(oldHeapBytes);
            m_chargedBytes += newHeapBytes.value() - oldHeapBytes;
        }
        size_t position = candidates.size();
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (beats(candidate.live.score, candidates[index].live.score)) {
                position = index;
                break;
            }
        }
        candidates.insert(position, candidate);
        return true;
    }

    const Table& table() const { return m_table; }
    // The table only grows, so its charge once its phase's walk ends is its largest, which the delta event reports.
    size_t chargedBytes() const { return m_chargedBytes; }

private:
    ProducerBudget& m_budget;
    Table m_table;
    size_t m_chargedBytes { 0 };
};

struct DeltaCounts {
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 };
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 };
    uint64_t phase1Nanoseconds { 0 };
    uint64_t phase2Nanoseconds { 0 };
    uint64_t phase3Nanoseconds { 0 };
    // What delta's duration and memory scale with: the CodeBlocks phase 1 walked, the candidates among them, and the
    // candidate table's charge.
    uint64_t codeBlocks { 0 };
    uint64_t candidates { 0 };
    uint64_t candidateTableBytes { 0 };
    // Phase 3's: the recorded UCBs it walked, those whose body never ran, the candidates among them, and the charges of
    // the registry's copy and of the candidate table.
    uint64_t recordedCodeBlocks { 0 };
    uint64_t neverRan { 0 };
    uint64_t imagelessCandidates { 0 };
    uint64_t registryCopyBytes { 0 };
    uint64_t imagelessTableBytes { 0 };
    bool faulted { false };
};

// Phase 2 and phase 3 commit a key's candidates the same way: score the saved body once, then take the candidates in
// order until one commits or does not beat it. A candidate whose capture is NotEligible is passed over, as an ineligible
// CodeBlock is. Returns false after a fault, and when production is no longer active after a capture: invalid material
// raised inside a lane's build turns it off without failing that build.
template<typename Candidate, typename Commit>
static bool commitKey(VMState& state, ProducerBudget& budget, const BodyKey& key, const Vector<Candidate, 1>& candidates,
    const CaptureClock& clock, DeltaCounts& counts, const Commit& commit)
{
    SavedScoring saved = scoreSavedBody(state, budget, key, clock);
    if (saved.outcome == SavedScoring::Outcome::Faulted)
        return false;
    if (saved.outcome == SavedScoring::Outcome::Deferred) {
        ++counts.deferredKeys;
        const auto& first = candidates.first();
        CaptureTrace trace { "delta"_s, key, first.live, std::nullopt, saved.readNanoseconds, std::nullopt, 0, first.live.hasPolymorphicSite, std::nullopt, std::nullopt };
        recordCaptureEvent(clock.report(), trace, "deferred"_s);
        return true;
    }

    std::optional<CaptureScore> scoredSaved = saved.saved ? std::optional { saved.saved->score } : std::nullopt;
    for (auto& candidate : candidates) {
        CaptureTrace trace { "delta"_s, key, candidate.live, scoredSaved, saved.readNanoseconds, std::nullopt, 0, candidate.live.hasPolymorphicSite, std::nullopt, std::nullopt };
        if (!beatsSavedBody(candidate.live.score, scoredSaved)) {
            recordCaptureEvent(clock.report(), trace, "beaten"_s);
            return true;
        }
        CommitOutcome outcome = commit(candidate, scoredSaved, trace);
        if (outcome.kind == CommitOutcome::Kind::Faulted)
            return false;
        if (outcome.kind == CommitOutcome::Kind::Committed) {
            ++counts.committedBodies;
            counts.committedBytes += outcome.fileSize;
        }
        if (outcome.kind != CommitOutcome::Kind::NotEligible)
            recordCaptureEvent(clock.report(), trace, eventOutcome(outcome.kind));
        if (!state.productionActive())
            return false;
        if (outcome.kind == CommitOutcome::Kind::NotEligible)
            continue;
        return true;
    }
    return true;
}

// Whether phase 1 found an eligible CodeBlock of the key. A CodeBlock whose image phase 2 found unrecordable is no longer
// eligible, since its code is no longer image-capturable, so a key whose every candidate turned out so is captured
// without an image.
static bool hasEligibleCodeBlock(const DeltaCandidates<CodeBlockCandidate>& compiled, const BodyKey& key)
{
    auto iterator = compiled.table().find(key);
    if (iterator == compiled.table().end())
        return false;
    for (auto& candidate : iterator->value) {
        RefPtr<JITCode> code = candidate.codeBlock->jitCode();
        if (code && isImageCapturable(static_cast<const BaselineJITCode&>(*code)))
            return true;
    }
    return false;
}

// Delta's phase 3, with the delta capture All: the bodies that ran without an eligible CodeBlock, reached through the UCB
// registry, so that bodies whose CodeBlocks died are reached too, and captured without an image. The registry's copy and
// the candidate table live only inside this call.
static void runImagelessPhase(VM& vm, VMState& state, ProducerBudget& budget, const DeltaCandidates<CodeBlockCandidate>& compiled,
    const CaptureClock& clock, DeltaCounts& counts)
{
    uint64_t start = clock.now();
    DeltaCandidates<ImagelessCandidate> candidates(budget);
    {
        // The registry copies under its leaf lock, and each cell is tested afterwards, outside it. Nothing between the
        // count and the copy can add or remove a record (UCBRegistry::recordedCodeBlockCount).
        UCBRegistry& registry = state.registry();
        size_t count = registry.recordedCodeBlockCount();
        CheckedSize copyBytes = count;
        copyBytes *= sizeof(UCBRegistry::RecordedCodeBlock);
        ScopedCharge copyCharge(budget);
        if (copyBytes.hasOverflowed() || !copyCharge.tryCharge(copyBytes.value())) {
            raiseBudgetLimit(state, makeString("delta's copy of "_s, count, " UCB records"_s));
            counts.faulted = true;
            return;
        }
        counts.registryCopyBytes = copyBytes.value();
        Vector<UCBRegistry::RecordedCodeBlock> recorded(count);
        recorded.shrink(registry.copyRecordedCodeBlocks(recorded.mutableSpan()));
        counts.recordedCodeBlocks = recorded.size();

        for (auto& entry : recorded) {
            UnlinkedCodeBlock& ucb = *entry.codeBlock;
            // A UCB the last collection found dead is not swept yet, and its constants and child UFEs may name cells already
            // swept. A live one keeps them, and nothing in delta can complete a collection.
            if (ucb.isPendingDestruction())
                continue;
            // A UCB proves only a request, such as a module's link or a CachedCall callee's preparation.
            if (!llintCounterShowsExecution(ucb)) {
                ++counts.neverRan;
                continue;
            }
            auto record = captureRecord(vm, ucb);
            if (!record || hasEligibleCodeBlock(compiled, record->key))
                continue;
            // Function code is encoded through its holder, which the record keeps while the UFE lives; one that is dead but
            // not swept yet is skipped with its body.
            UnlinkedFunctionExecutable* holder = entry.holder;
            if (coreKindFor(record->key.identityKind()) == UnlinkedCodeBlockCoreKind::Function && (!holder || holder->isPendingDestruction()))
                continue;
            ++state.progress().captureCandidates;
            ++counts.imagelessCandidates;
            if (!candidates.add(record->key, ImagelessCandidate { &ucb, holder, scoreImagelessCandidate(ucb, clock) })) {
                raiseBudgetLimit(state, makeString("delta's table of bodies without an image; "_s, bodyDetail(record->key)));
                counts.faulted = true;
                return;
            }
        }
    }
    counts.imagelessTableBytes = candidates.chargedBytes();
    counts.eligibleKeys += candidates.table().size();

    // Commits are independent, so their order is the table's.
    for (auto& entry : candidates.table()) {
        bool committedOrBeaten = commitKey(state, budget, entry.key, entry.value, clock, counts, [&](const ImagelessCandidate& candidate, const std::optional<CaptureScore>&, CaptureTrace& trace) {
            return commitImagelessCapture(vm, state, *candidate.codeBlock, candidate.holder, entry.key, clock, trace);
        });
        if (!committedOrBeaten) {
            counts.faulted = true;
            break;
        }
    }
    counts.phase3Nanoseconds = clock.now() - start;
}

// Delta's three phases: phase 1 scores every eligible CodeBlock into a candidate table, phase 2 commits each key's
// candidates, and phase 3, with the delta capture All, captures without an image the bodies that ran without an eligible
// CodeBlock. The candidate tables live only inside this call.
static DeltaCounts runDeltaPhases(VM& vm, VMState& state, ProducerBudget& budget, const CaptureClock& clock)
{
    DeltaCounts counts;
    DeltaCandidates<CodeBlockCandidate> candidates(budget);

    // Phase 1, under the CodeBlock set's lock, which forEachCodeBlockIgnoringJITPlans requires and which completes no
    // pending plan. The functor allocates no cell and does no I/O; a refused charge for the candidate table ends the
    // walk, and budget.limit is raised once the lock is released.
    std::optional<String> refusedChargeDetail;
    uint64_t start = clock.now();
    {
        Locker locker { vm.heap.codeBlockSet().getLock() };
        vm.heap.forEachCodeBlockIgnoringJITPlans(locker, [&](CodeBlock* codeBlock) {
            if (refusedChargeDetail)
                return;
            ++counts.codeBlocks;
            auto record = candidateRecord(vm, *codeBlock);
            if (!record)
                return;
            ++state.progress().captureCandidates;
            ++counts.candidates;
            if (!candidates.add(record->key, CodeBlockCandidate { codeBlock, scoreLiveCandidate(*codeBlock, clock) }))
                refusedChargeDetail = makeString("delta's candidate table; "_s, bodyDetail(record->key));
        });
    }
    counts.phase1Nanoseconds = clock.now() - start;
    counts.candidateTableBytes = candidates.chargedBytes();
    if (refusedChargeDetail) {
        raiseBudgetLimit(state, WTF::move(*refusedChargeDetail));
        counts.faulted = true;
        return counts;
    }
    counts.eligibleKeys = candidates.table().size();

    // Phase 2, without the lock. The CodeBlock pointers stay valid: each was in the CodeBlock set, and nothing between the
    // phases can complete a collection, since the VM thread holds heap access, reaches no stop point and allocates no cell.
    // Commits are independent, so their order is the table's. A NotEligible image makes that CB an ineligible one, so the
    // key's next candidate gets the same test, until one commits or does not beat the saved body.
    start = clock.now();
    for (auto& entry : candidates.table()) {
        bool committedOrBeaten = commitKey(state, budget, entry.key, entry.value, clock, counts, [&](const CodeBlockCandidate& candidate, const std::optional<CaptureScore>& saved, CaptureTrace& trace) {
            return commitCapture(vm, state, *candidate.codeBlock, entry.key, saved, clock, trace);
        });
        if (!committedOrBeaten) {
            counts.faulted = true;
            break;
        }
    }
    counts.phase2Nanoseconds = clock.now() - start;

    // Phase 3, with the delta capture All, after every key of phase 2 has been decided, so that only a key whose every
    // CodeBlock turned out ineligible can still meet a body without an image.
    if (!counts.faulted && state.deltaCapture() == DeltaCapture::All)
        runImagelessPhase(vm, state, budget, candidates, clock, counts);
    return counts;
}

} // namespace DeltaInternal

DeltaResult delta(VM& vm)
{
    using namespace DeltaInternal;

    // What delta requires of its caller: a configured producing VM, the API lock with heap access, and no collector work
    // on this thread. Each failure is rejected without work.
    VMState* state = vm.jitCacheState();
    if (!state)
        return { DeltaOutcome::Rejected, "delta.unconfigured"_s, std::nullopt };
    if (!state->producing())
        return { DeltaOutcome::Rejected, "delta.role"_s, std::nullopt };
    if (!vm.currentThreadIsHoldingAPILock() || !vm.heap.hasHeapAccess())
        return { DeltaOutcome::Rejected, "delta.locks"_s, std::nullopt };
    if (vm.heap.currentThreadIsDoingGCWork())
        return { DeltaOutcome::Rejected, "delta.collector"_s, std::nullopt };

    CaptureInProgressScope captureInProgress(*state);
    // Freeing what an ended production held is no work in the sense above.
    state->releaseEndedProductionMemory();
    auto faulted = [&](const DeltaCounts& counts) {
        std::optional<FaultReport> fault = state->productionFault();
        if (!fault)
            fault = state->firstFault();
        return DeltaResult { DeltaOutcome::Faulted, { }, WTF::move(fault), counts.eligibleKeys, counts.committedBodies, counts.committedBytes, counts.deferredKeys };
    };
    if (!state->productionActive())
        return faulted({ });

    // No collection completes during delta, so the CodeBlocks and UCBs its phases hold stay valid.
    DeferGCForAWhile deferGC(vm);
    BenchReport* report = state->benchReport();
    auto finish = [&](const DeltaCounts& counts) {
        // The delta event, and a flush after each delta.
        if (!report)
            return;
        report->record("delta"_s, {
            { "outcome"_s, String { counts.faulted ? "faulted"_s : "completed"_s } },
            { "phase1Nanoseconds"_s, counts.phase1Nanoseconds },
            { "phase2Nanoseconds"_s, counts.phase2Nanoseconds },
            { "phase3Nanoseconds"_s, counts.phase3Nanoseconds },
            { "codeBlocks"_s, counts.codeBlocks },
            { "candidates"_s, counts.candidates },
            { "candidateTableBytes"_s, counts.candidateTableBytes },
            { "recordedCodeBlocks"_s, counts.recordedCodeBlocks },
            { "neverRan"_s, counts.neverRan },
            { "imagelessCandidates"_s, counts.imagelessCandidates },
            { "registryCopyBytes"_s, counts.registryCopyBytes },
            { "imagelessTableBytes"_s, counts.imagelessTableBytes },
            { "eligibleKeys"_s, counts.eligibleKeys },
            { "committedBodies"_s, counts.committedBodies },
            { "committedBytes"_s, counts.committedBytes },
            { "deferredKeys"_s, counts.deferredKeys },
        });
        report->flush();
    };

    // A refusal made on a JIT worker only marks the budget there; it reaches the VM thread here.
    ProducerBudget& budget = *state->producerBudget();
    if (budget.hasRefused()) {
        raiseBudgetLimit(*state, "a charge refused before delta"_s);
        DeltaCounts counts;
        counts.faulted = true;
        finish(counts);
        return faulted(counts);
    }

    // The three phases free their candidate tables and release the charges before they return.
    DeltaCounts counts = runDeltaPhases(vm, *state, budget, CaptureClock(report));

    // After its commits, the delta compacts the pack whenever it holds dead bytes. A body that fails its checks on
    // the way is invalid material and any other failure a recording fault; the bodies committed above stay valid either
    // way.
    if (!counts.faulted && state->productionActive()) {
        // The pack as the delta's commits left it, and what the compaction made of it, for the bench's pack event.
        IndexStatistics before = report && state->artifact() ? state->artifact()->statistics() : IndexStatistics { };
        auto compacted = state->writer()->compactIfNeeded(!!report);
        if (!compacted) {
            counts.faulted = true;
            if (compacted.error().kind == CompactionFailure::Kind::Damaged)
                state->raiseInvalidMaterial(compacted.error().check, WTF::move(compacted.error().detail));
            else
                state->raiseRecordingFault({ }, compacted.error().check, WTF::move(compacted.error().detail));
        } else if (report) {
            const CompactionResult* result = *compacted ? &**compacted : nullptr;
            report->record("pack"_s, { { "bodies"_s, before.bodies }, { "bytes"_s, before.packBytes }, { "liveBytes"_s, before.liveBytes },
                { "deadBytes"_s, before.packBytes - std::min(before.packBytes, before.liveBytes) }, { "compacted"_s, static_cast<uint64_t>(!!result) },
                { "compactedBytes"_s, result ? result->packBytes : 0 }, { "compactionNanoseconds"_s, result ? result->nanoseconds : 0 } });
        }
    }
    // A delta during which production ended never reports completed, whatever ended it.
    if (!state->productionActive())
        counts.faulted = true;
    finish(counts);
    if (counts.faulted)
        return faulted(counts);

    // Only a delta that completes counts as a run.
    ++state->progress().deltaRuns;
    return { DeltaOutcome::Completed, { }, std::nullopt, counts.eligibleKeys, counts.committedBodies, counts.committedBytes, counts.deferredKeys };
}

#else // !ENABLE(JIT)

// A build without the JIT configures no VM: start rejects at start.platform before anything else, so delta has no state
// to capture for.
DeltaResult delta(VM& vm)
{
    ASSERT_UNUSED(vm, !vm.jitCacheState());
    return { DeltaOutcome::Rejected, "delta.unconfigured"_s, std::nullopt };
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
