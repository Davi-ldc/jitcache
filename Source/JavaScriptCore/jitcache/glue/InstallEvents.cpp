#include "config.h"
#include "jitcache/glue/InstallEvents.h"

#include "JITThunks.h"
#include "VM.h"
#include "jitcache/session/JITCacheBench.h"

#if ENABLE(JIT)

namespace JSC::JITCache {

InstallMeasurement::InstallMeasurement(VM& vm, BenchReport* report)
    : m_vm(vm)
    , m_report(report)
    , m_timesSteps(report && report->timesBreakdowns())
    , m_supportGenerationsAtStart(report ? vm.jitStubs->supportGenerations() : 0)
{
    if (report)
        m_total.start();
}

bool InstallMeasurement::supportGenerated() const
{
    return m_vm.jitStubs->supportGenerations() != m_supportGenerationsAtStart;
}

void recordInstallEvent(const InstallMeasurement& measurement, const InstalledBody& installed, uint64_t totalNanoseconds, uint64_t bodyReleaseNanoseconds, bool supportGenerated)
{
    using BenchValue = decltype(BenchField::value);
    auto step = [&](InstallStep installStep) {
        std::optional<uint64_t> nanoseconds = measurement.stepNanoseconds(installStep);
        return nanoseconds ? BenchValue { *nanoseconds } : BenchValue { nullptr };
    };
    auto stage = [&](uint64_t nanoseconds) {
        return measurement.timesSteps() ? BenchValue { nanoseconds } : BenchValue { nullptr };
    };
    measurement.report()->record("install"_s, {
        { "key"_s, bodyKeyHex(installed.key) },
        { "total"_s, totalNanoseconds },
        { "steps.imageParse"_s, step(InstallStep::ImageParse) },
        { "steps.bakedFacts"_s, step(InstallStep::BakedFacts) },
        { "steps.gate"_s, step(InstallStep::Gate) },
        { "steps.prepareImage"_s, step(InstallStep::PrepareImage) },
        { "prepareImage.allocate"_s, stage(installed.prepareTimings.allocateNanoseconds) },
        { "prepareImage.copy"_s, stage(installed.prepareTimings.copyNanoseconds) },
        { "prepareImage.patch"_s, stage(installed.prepareTimings.patchNanoseconds) },
        { "prepareImage.flush"_s, stage(installed.prepareTimings.flushNanoseconds) },
        { "steps.cbPrepare"_s, step(InstallStep::CBPrepare) },
        { "steps.prepareBaselineICs"_s, step(InstallStep::PrepareBaselineICs) },
        { "steps.seedCallLinkHistory"_s, step(InstallStep::SeedCallLinkHistory) },
        { "steps.seedLinkedState"_s, step(InstallStep::SeedLinkedState) },
        { "steps.commit"_s, step(InstallStep::Commit) },
        { "steps.setup"_s, step(InstallStep::Setup) },
        { "steps.attachPropertyICState"_s, step(InstallStep::AttachPropertyICState) },
        { "steps.finishCounter"_s, step(InstallStep::FinishCounter) },
        { "steps.installCode"_s, step(InstallStep::InstallCode) },
        { "bodyRelease"_s, bodyReleaseNanoseconds },
        { "restore.carried"_s, installed.restore.carried },
        { "restore.crossed"_s, installed.restore.crossed },
        { "restore.nativeSlice"_s, installed.restore.nativeSlice },
        { "restore.slice"_s, installed.restore.slice },
        { "restore.startupDeferralScale"_s, installed.restore.startupDeferralScale },
        { "codeSize"_s, installed.codeSize },
        { "image.codeSize"_s, static_cast<uint64_t>(installed.imageCodeSize) },
        { "image.fixupCount"_s, static_cast<uint64_t>(installed.imageFixupCount) },
        { "supportGenerated"_s, supportGenerated },
    });
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
