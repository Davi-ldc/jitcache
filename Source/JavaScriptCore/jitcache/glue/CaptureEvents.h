#pragma once

#include "jitcache/base/BenchClock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/ValidatedBody.h"
#include <array>
#include <optional>
#include <stdint.h>
#include <wtf/Platform.h>
#include <wtf/text/ASCIILiteral.h>

// The capture glue's bench events: the clock that times a capture's steps while a bench report is open, what one scored
// capture's event reports, and the capture and compile events, which CaptureEvents.cpp writes.

#if ENABLE(JIT)

namespace JSC {

class CodeBlock;

} // namespace JSC

namespace JSC::JITCache {

class BenchReport;
class VMState;
struct BaselineCompileTiming;

// Reads the bench report's clock, the thread CPU time, only while a report is open, so a run without one pays a load and
// a test.
class CaptureClock {
public:
    explicit CaptureClock(BenchReport* report)
        : m_report(report)
    {
    }

    BenchReport* report() const { return m_report; }
    uint64_t now() const { return m_report ? benchThreadCPUNanoseconds() : 0; }

private:
    BenchReport* const m_report;
};

// What one scored capture's bench event reports.
struct CaptureTrace {
    ASCIILiteral trigger; // "finalize" or "delta"
    BodyKey key;
    LiveScore live;
    std::optional<CaptureScore> saved; // set when a saved body was scored, from the kept summary or the scoring read
    std::optional<uint64_t> savedReadNanoseconds; // the scoring read with its scoreSections; none when the kept summary answered
    std::optional<CaptureScore> committed; // the score of the bytes built, once the capture built its sections
    uint64_t committedExitSiteUnits { 0 };
    bool hasPolymorphicSite { false }; // the bit last passed to the CB lane
    struct BuildTimes {
        uint64_t captureImage { 0 };
        uint64_t buildSections { 0 }; // with liveRichness
        uint64_t captureBaselineICs { 0 };
        uint64_t cbStateCapture { 0 };
    };
    std::optional<BuildTimes> build; // once every lane built its sections
    // What a commit adds, once the writer published while the report is open: the time of each of the writer's parts
    // beside each section's size, and the producer memory the commit holds.
    struct CommitTrace {
        CommitTiming timing;
        std::array<uint64_t, numberOfSectionKinds> sectionBytes { }; // by SectionKind; 0 for a kind the body lacks
        uint64_t budgetChargedBytes { 0 }; // the budget's charge at the commit, the capture's sections and records still held
        uint64_t keptSummariesBytes { 0 }; // the kept summaries' charge once the commit's entry is kept
        uint64_t indexEntryBytes { 0 }; // what the commit charged for an index entry the writer added; 0 when the index had the key
    };
    std::optional<CommitTrace> commit;
};

// The capture event: the trigger, the key, how the capture ended, the candidate's score with the UCB lane's exit-site
// units apart, the committed score once the sections were built, the saved score with the first field in which the
// candidate's differs from it, the polymorphic bit the CB lane received, the thread CPU time of each lane's scoring and
// build call and of the scoring read, and, for a commit, the writer's parts from its CommitTiming beside each section's
// size and the producer memory the commit holds. A field the capture did not reach is null. Nothing without a report.
void recordCaptureEvent(BenchReport*, const CaptureTrace&, ASCIILiteral outcome);

// The compile event, the native cost the bench compares an import's installation with: the key the parent-key registry
// holds for the CB's UCB, or null, whether or not the code carries an image record; the compilation's time after its
// profile drain; the finalization's without relinking incoming calls; the code's size, its executable allocation as the
// native code-size sample counts it; and whether per-VM support was generated during either span, which sets the body
// aside in that comparison. Nothing without a report.
void recordCompileEvent(VMState&, CodeBlock&, const BaselineCompileTiming&);

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
