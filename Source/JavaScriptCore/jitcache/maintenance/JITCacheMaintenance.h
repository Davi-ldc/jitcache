#pragma once

#include <JavaScriptCore/JSExportMacros.h>
#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <stdio.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

// The synchronous, VM-independent backend of compact and its command line, which hosts call. It needs no VM and no
// JSC::initialize, and runs on the calling thread. Exported to Bun as <JavaScriptCore/JITCacheMaintenance.h>: it
// includes only WTF headers and JSExportMacros.h, and no member exists only under ENABLE(JITCACHE_TWINS).

namespace JSC::JITCache::Maintenance {

enum class Outcome : uint8_t { Done, NoArtifact, Busy, NeedsConfirmation, Declined, PlanChanged, Failed };

struct Diagnostic {
    ASCIILiteral code; // such as "damaged-body"; the command line prints it before the detail
    String detail;
};

struct Eviction {
    std::array<uint8_t, 40> key;
    uint64_t bytes { 0 };
    uint64_t version { 0 }; // the body's commit identifier
    double score { 0 }; // (L + P) / bytes: the progress the body saves per byte it takes
    // Plan's comparison reads every eviction field for field.
    friend bool operator==(const Eviction&, const Eviction&) = default;
};

struct Plan {
    std::array<uint8_t, 16> headerDigest; // the artifact's header digest
    uint64_t postCleanBytes { 0 };
    uint64_t targetBytes { 0 };
    uint64_t evictedBytes { 0 };
    // The size of a pack that fails its own checks, whose bodies no reader can trust: evicted whole, before any body,
    // whenever the target is above zero; 0 for a sound pack or none.
    uint64_t damagedPackBytes { 0 };
    Vector<Eviction> evictions; // in eviction order
    bool deletesArtifact { false };
    friend bool operator==(const Plan&, const Plan&) = default;
};

struct Report {
    Outcome outcome { Outcome::Failed };
    uint64_t temporariesRemoved { 0 };
    uint64_t bodiesEvicted { 0 };
    uint64_t bytesReclaimed { 0 };
    std::optional<Plan> plan;
    Vector<Diagnostic> diagnostics;
};

enum class Answer : uint8_t { Ask, Yes, No };
struct CompactOptions {
    Answer answer { Answer::Ask }; // for a plan that deletes the whole artifact
    std::optional<Plan> confirmedPlan; // a plan an answer confirmed, which a second call applies only unchanged
};

JS_EXPORT_PRIVATE Report compact(const String& parentPath, double ratio, const CompactOptions&);

// Whether a command line may leave compact's ratio out: Bun's `bun jitcache` may, and then the ratio is 0 and a first
// argument that is not a number is the path or an option; the jsc shell's --jitcache-maintenance may not.
enum class RatioArgument : uint8_t { Required, Optional };
// "compact <ratio> [<path>] [--yes | --no]", or "compact [<ratio>] [<path>] [--yes | --no]" with
// RatioArgument::Optional; returns the process exit code.
JS_EXPORT_PRIVATE int runCommandLine(std::span<const CString> arguments, FILE* in, FILE* out, FILE* err, RatioArgument = RatioArgument::Required);

} // namespace JSC::JITCache::Maintenance
