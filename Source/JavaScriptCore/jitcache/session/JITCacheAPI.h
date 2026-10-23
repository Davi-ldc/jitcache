#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// JITCache's public C++ interface, which hosts call: start, status, delta, the end of a host's run, the bench report's
// flush and the JSON lines for their logs. Exported to Bun as <JavaScriptCore/JITCacheAPI.h>: it includes only WTF headers
// and JSExportMacros.h, and no member exists only under ENABLE(JITCACHE_TWINS), so every translation unit sees one layout.

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

enum class Role : uint8_t { Consumer = 1, Producer = 2, ConsumerProducer = 3 };

// What delta captures beyond the compiled bodies: with All, also the bodies that ran without compiling; with Compiled,
// nothing more. Without a delta only compiled bodies are captured, whatever it says. A Consumer captures nothing and
// ignores it.
enum class DeltaCapture : uint8_t { All = 1, Compiled = 2 };

struct Config {
    String artifactPath; // the parent directory, which holds cache/ and the producer lock
    Role role { Role::Consumer };
    std::optional<size_t> producerLimitBytes; // producing roles; empty: defaultProducerLimitBytes (JITCacheParameters.h)
    DeltaCapture deltaCapture { DeltaCapture::All }; // All unless the host sets Compiled
    // The app the host runs, a value it computes from the code it runs, which JITCache only compares with the app an
    // artifact's header names and writes into the header of an artifact it creates; empty: no app.
    std::optional<uint64_t> appDigest;
    String benchReportPath; // empty: no bench report
    String twinReportPath; // ENABLE(JITCACHE_TWINS) builds; ignored elsewhere
};

// The artifact path of a bare --jitcache and of a maintenance command without one.
static constexpr ASCIILiteral defaultArtifactPath = "./.jitcache"_s;

// The role says what a start did: a Producer created its artifact and the other roles opened theirs, or, for a
// ConsumerProducer, replaced an incompatible one or one made for another app, which the step says.
enum class StartOutcome : uint8_t { Started, Busy, Rejected, Fault };
struct StartResult {
    StartOutcome outcome;
    // For Started, empty, or "start.replaced" when a ConsumerProducer replaced an incompatible artifact or one made for
    // another app.
    ASCIILiteral step;
    String detail;
};

enum class FaultClass : uint8_t { StartFault, InvalidMaterial, ExecutableMemory, RecordingFault, DebuggerAttached };
struct FaultReport {
    FaultClass faultClass;
    ASCIILiteral part; // "ucb", "image", "cb", "ics", or empty when check is a full step name
    ASCIILiteral check;
    String detail;
    String stepName() const; // part + "." + check, or check when part is empty
};

enum class SessionState : uint8_t { Unconfigured, Started, Faulted };
enum class ProductionState : uint8_t { NotProducing, Active, Ended };

struct Progress {
    uint64_t indexedBodies { 0 };
    uint64_t bodyOpens { 0 };
    uint64_t transientOpenFailures { 0 };
    uint64_t imports { 0 }; // UCB statistics: imports, attaches, gateDrops, misses (summed)
    uint64_t attaches { 0 };
    uint64_t gateDrops { 0 };
    uint64_t misses { 0 };
    uint64_t installs { 0 };
    uint64_t bakedFactMismatches { 0 };
    uint64_t captureCandidates { 0 };
    uint64_t capturesDeferred { 0 }; // a saved body that a transient error kept from being read
    uint64_t capturesCommitted { 0 };
    uint64_t bytesCommitted { 0 };
    uint64_t deltaRuns { 0 };
};

struct BudgetSnapshot {
    size_t limitBytes { 0 };
    size_t chargedBytes { 0 };
    size_t peakBytes { 0 };
    bool refused { false };
};

struct Status {
    SessionState state { SessionState::Unconfigured };
    std::optional<Role> role;
    bool activityOn { false };
    std::optional<FaultReport> activityFault;
    ProductionState production { ProductionState::NotProducing };
    std::optional<FaultReport> productionFault;
    std::optional<FaultReport> firstFault; // the earlier of the two
    Progress progress;
    BudgetSnapshot budget;
};

enum class DeltaOutcome : uint8_t { Completed, Rejected, Faulted };
struct DeltaResult {
    DeltaOutcome outcome;
    // Rejected: the requirement the call failed, delta.unconfigured, delta.role, delta.locks or delta.collector.
    ASCIILiteral rejection;
    std::optional<FaultReport> fault; // Faulted: the fault that stopped production or activity
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 }; // committed by this call, before any fault
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 }; // keys whose saved body a transient error kept from being read
};

JS_EXPORT_PRIVATE StartResult start(VM&, const Config&);
JS_EXPORT_PRIVATE Status status(VM&);
// The caller holds no JSC-internal lock, a documented duty that JSC cannot check.
JS_EXPORT_PRIVATE DeltaResult delta(VM&);
// VM thread. Writes out the bench report's buffered lines; does nothing for a VM with no state or no report.
JS_EXPORT_PRIVATE void flushBenchReport(VM&);
// The end of a host's run on one VM, on the VM's thread with its API lock held and no JSC-internal lock. It calls delta
// when the host asks for it and the VM's production is still active, so a Consumer, a VM start never configured and a
// production that ended get none. Then it runs the host's own end-of-run work, which receives that delta's result and
// reads the final status at the point its host's order puts it, and last it flushes the bench report, so a process that
// exits without destroying its VM keeps the events still buffered, in every role.
JS_EXPORT_PRIVATE void finishHostRun(VM&, bool deltaAtExit, const ScopedLambda<void(const std::optional<DeltaResult>&)>& hostWork);
JS_EXPORT_PRIVATE ASCIILiteral name(StartOutcome);
JS_EXPORT_PRIVATE ASCIILiteral name(Role);
JS_EXPORT_PRIVATE ASCIILiteral name(DeltaCapture);
JS_EXPORT_PRIVATE ASCIILiteral name(FaultClass);
JS_EXPORT_PRIVATE std::optional<Role> parseRole(StringView); // "consumer", "producer", "consumer-producer"
JS_EXPORT_PRIVATE std::optional<DeltaCapture> parseDeltaCapture(StringView); // "all", "compiled"

// One-line JSON for hosts' logs: {"jitcache":"start"|"delta"|"status", ...} with every field of the struct by name,
// enums as the names above (lowercase, words joined by "-"), faults as {"class","step","detail"}.
JS_EXPORT_PRIVATE String toJSON(const StartResult&);
JS_EXPORT_PRIVATE String toJSON(const DeltaResult&);
JS_EXPORT_PRIVATE String toJSON(const Status&);

} // namespace JSC::JITCache
