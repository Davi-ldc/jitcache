#pragma once

// Exported to Bun as <JavaScriptCore/JITCacheAPI.h> (SPEC-integrator.md section 3.5): it includes only WTF headers and
// JSExportMacros.h, and no member exists only under ENABLE(JITCACHE_TWINS), so every translation unit sees one layout.

#include <JavaScriptCore/JSExportMacros.h>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// The public C++ interface of THREAD Session (SPEC-integrator.md section 3.1).

enum class Role : uint8_t { Consumer = 1, Producer = 2, ConsumerProducer = 3 };

struct Config {
    String artifactPath; // the parent directory of THREAD Storage
    Role role { Role::Consumer };
    std::optional<size_t> producerLimitBytes; // producing roles; empty: defaultProducerLimitBytes (section 16)
    bool strict { false }; // off unless the host turns it on (THREAD Session; section 4.3)
    String benchReportPath; // empty: no bench report
    String twinReportPath; // ENABLE(JITCACHE_TWINS) builds; ignored elsewhere
};

// The artifact path of a bare --jitcache and of a maintenance command without one (section 11.1).
static constexpr ASCIILiteral defaultArtifactPath = "./.jitcache"_s;

enum class StartOutcome : uint8_t { Created, Opened, Busy, Rejected, Fault };
struct StartResult {
    StartOutcome outcome;
    ASCIILiteral step; // empty for Created and Opened
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

enum class SessionState : uint8_t { Unconfigured, Created, Opened, Faulted };
enum class ProductionState : uint8_t { NotProducing, Active, Ended };

struct Progress {
    uint64_t indexedBodies { 0 };
    uint64_t bodyOpens { 0 };
    uint64_t transientOpenFailures { 0 };
    uint64_t imports { 0 }; // UCB statistics: imports, seededDecodes, attaches, gateDrops, misses (summed)
    uint64_t seededDecodes { 0 };
    uint64_t attaches { 0 };
    uint64_t gateDrops { 0 };
    uint64_t misses { 0 };
    uint64_t installs { 0 };
    uint64_t bakedFactMismatches { 0 };
    uint64_t captureCandidates { 0 };
    uint64_t capturesDeferred { 0 }; // a saved body that a transient error kept from being read (section 8.3)
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
    bool strict { false };
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
    ASCIILiteral rejection; // Rejected: the step of section 3.4
    std::optional<FaultReport> fault; // Faulted: the fault that stopped production or activity
    uint64_t eligibleKeys { 0 };
    uint64_t committedBodies { 0 }; // committed by this call, before any fault
    uint64_t committedBytes { 0 };
    uint64_t deferredKeys { 0 }; // keys whose saved body a transient error kept from being read (section 8.3)
};

JS_EXPORT_PRIVATE StartResult start(VM&, const Config&);
JS_EXPORT_PRIVATE Status status(VM&);
// The caller holds no JSC-internal lock, a documented duty that JSC cannot check (THREAD Session).
JS_EXPORT_PRIVATE DeltaResult delta(VM&);
// VM thread. Writes out the bench report's buffered lines (harness sub-SPEC section 9.1); does nothing for a VM with no
// state or no report. The hosts call it at exit in every role (section 11).
JS_EXPORT_PRIVATE void flushBenchReport(VM&);
JS_EXPORT_PRIVATE ASCIILiteral name(StartOutcome);
JS_EXPORT_PRIVATE ASCIILiteral name(Role);
JS_EXPORT_PRIVATE ASCIILiteral name(FaultClass);
JS_EXPORT_PRIVATE std::optional<Role> parseRole(StringView); // "consumer", "producer", "consumer-producer"

// One-line JSON for hosts' logs: {"jitcache":"start"|"delta"|"status", ...} with every field of the struct by name,
// enums as the names above (lowercase, words joined by "-"), faults as {"class","step","detail"}.
JS_EXPORT_PRIVATE String toJSON(const StartResult&);
JS_EXPORT_PRIVATE String toJSON(const DeltaResult&);
JS_EXPORT_PRIVATE String toJSON(const Status&);

} // namespace JSC::JITCache
