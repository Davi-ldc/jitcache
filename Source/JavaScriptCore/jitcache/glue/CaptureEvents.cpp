#include "config.h"
#include "jitcache/glue/CaptureEvents.h"

#include "CodeBlock.h"
#include "JSCInlines.h"
#include "jitcache/glue/JITCacheGlue.h"
#include "jitcache/session/JITCacheBench.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <wtf/text/WTFString.h>

#if ENABLE(JIT)

namespace JSC::JITCache {

namespace CaptureEventsInternal {

// The first field in beats order in which two scores differ, or none.
static ASCIILiteral firstDifference(const CaptureScore& a, const CaptureScore& b)
{
    if (a.tier != b.tier)
        return "tier"_s;
    if (a.richness != b.richness)
        return "richness"_s;
    if (a.icSitesWithCases != b.icSitesWithCases)
        return "icSitesWithCases"_s;
    if (a.counterWithheld != b.counterWithheld)
        return "counterWithheld"_s;
    if (a.counterProgress != b.counterProgress)
        return "counterProgress"_s;
    return { };
}

} // namespace CaptureEventsInternal

// Each section kind's two writer fields, writer.<name>.stream and writer.<name>.bytes, in the table's order.
#define JITCACHE_CAPTURE_EVENT_SECTION_FIELDS(kind, typeId, tier, name, presence) \
    { "writer." name ".stream"_s, stream(SectionKind::kind) }, { "writer." name ".bytes"_s, bytes(SectionKind::kind) },

void recordCaptureEvent(BenchReport* report, const CaptureTrace& trace, ASCIILiteral outcome)
{
    if (!report)
        return;
    using BenchValue = decltype(BenchField::value);
    auto number = [](std::optional<uint64_t> value) {
        return value ? BenchValue { *value } : BenchValue { nullptr };
    };
    auto field = [&](const std::optional<CaptureScore>& score, auto member) {
        return score ? number(static_cast<uint64_t>((*score).*member)) : BenchValue { nullptr };
    };
    auto flag = [](const std::optional<CaptureScore>& score) {
        return score ? BenchValue { score->counterWithheld } : BenchValue { nullptr };
    };
    std::optional<CaptureScore> candidate = trace.live.score;
    ASCIILiteral difference = trace.saved ? CaptureEventsInternal::firstDifference(*candidate, *trace.saved) : ASCIILiteral { };
    auto build = [&](uint64_t CaptureTrace::BuildTimes::* member) {
        return trace.build ? number((*trace.build).*member) : BenchValue { nullptr };
    };
    auto stream = [&](SectionKind kind) {
        return trace.commit ? number(trace.commit->timing.streamNanoseconds[static_cast<size_t>(kind)]) : BenchValue { nullptr };
    };
    auto bytes = [&](SectionKind kind) {
        return trace.commit ? number(trace.commit->sectionBytes[static_cast<size_t>(kind)]) : BenchValue { nullptr };
    };
    auto commit = [&](uint64_t CaptureTrace::CommitTrace::* member) {
        return trace.commit ? number((*trace.commit).*member) : BenchValue { nullptr };
    };
    report->record("capture"_s, {
        { "trigger"_s, String { trace.trigger } },
        { "key"_s, bodyKeyHex(trace.key) },
        { "outcome"_s, String { outcome } },
        { "candidate.tier"_s, field(candidate, &CaptureScore::tier) },
        { "candidate.richness"_s, field(candidate, &CaptureScore::richness) },
        { "candidate.exitSiteUnits"_s, trace.live.exitSiteUnits },
        { "candidate.icSitesWithCases"_s, field(candidate, &CaptureScore::icSitesWithCases) },
        { "candidate.counterWithheld"_s, flag(candidate) },
        { "candidate.counterProgress"_s, field(candidate, &CaptureScore::counterProgress) },
        { "committed.tier"_s, field(trace.committed, &CaptureScore::tier) },
        { "committed.richness"_s, field(trace.committed, &CaptureScore::richness) },
        { "committed.exitSiteUnits"_s, number(trace.committed ? std::optional<uint64_t> { trace.committedExitSiteUnits } : std::nullopt) },
        { "committed.icSitesWithCases"_s, field(trace.committed, &CaptureScore::icSitesWithCases) },
        { "committed.counterWithheld"_s, flag(trace.committed) },
        { "committed.counterProgress"_s, field(trace.committed, &CaptureScore::counterProgress) },
        { "saved.tier"_s, field(trace.saved, &CaptureScore::tier) },
        { "saved.richness"_s, field(trace.saved, &CaptureScore::richness) },
        { "saved.icSitesWithCases"_s, field(trace.saved, &CaptureScore::icSitesWithCases) },
        { "saved.counterWithheld"_s, flag(trace.saved) },
        { "saved.counterProgress"_s, field(trace.saved, &CaptureScore::counterProgress) },
        { "decidingField"_s, difference.isNull() ? BenchValue { nullptr } : BenchValue { String { difference } } },
        { "hasPolymorphicSite"_s, trace.hasPolymorphicSite },
        { "scoring.summarizeBaselineICs"_s, trace.live.summarizeBaselineICsNanoseconds },
        { "scoring.scoreLive"_s, trace.live.scoreLiveNanoseconds },
        { "scoring.liveRichness"_s, trace.live.liveRichnessNanoseconds },
        { "scoring.savedRead"_s, number(trace.savedReadNanoseconds) },
        { "build.captureImage"_s, build(&CaptureTrace::BuildTimes::captureImage) },
        { "build.buildSections"_s, build(&CaptureTrace::BuildTimes::buildSections) },
        { "build.captureBaselineICs"_s, build(&CaptureTrace::BuildTimes::captureBaselineICs) },
        { "build.cbStateCapture"_s, build(&CaptureTrace::BuildTimes::cbStateCapture) },
        JITCACHE_FOR_EACH_SECTION_KIND(JITCACHE_CAPTURE_EVENT_SECTION_FIELDS)
        { "writer.reread"_s, trace.commit ? number(trace.commit->timing.rereadNanoseconds) : BenchValue { nullptr } },
        { "writer.publish"_s, trace.commit ? number(trace.commit->timing.publishNanoseconds) : BenchValue { nullptr } },
        { "charges.budget"_s, commit(&CaptureTrace::CommitTrace::budgetChargedBytes) },
        { "charges.keptSummaries"_s, commit(&CaptureTrace::CommitTrace::keptSummariesBytes) },
        { "charges.indexEntry"_s, commit(&CaptureTrace::CommitTrace::indexEntryBytes) },
    });
}

#undef JITCACHE_CAPTURE_EVENT_SECTION_FIELDS

void recordCompileEvent(VMState& state, CodeBlock& codeBlock, const BaselineCompileTiming& timing)
{
    BenchReport* report = state.benchReport();
    if (!report)
        return;
    using BenchValue = decltype(BenchField::value);
    std::optional<BodyKey> key = state.registry().keyOf(*codeBlock.unlinkedCodeBlock());
    RefPtr<JITCode> code = codeBlock.jitCode();
    report->record("compile"_s, {
        { "key"_s, key ? BenchValue { bodyKeyHex(*key) } : BenchValue { nullptr } },
        { "compileNanoseconds"_s, timing.compileNanoseconds },
        { "finalizeNanoseconds"_s, timing.finalizeNanoseconds },
        { "codeSize"_s, static_cast<uint64_t>(code ? code->size() : 0) },
        { "supportGenerated"_s, timing.supportGenerated },
    });
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
