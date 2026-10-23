#include "config.h"
#include "jitcache/ucb/UCBRequestCost.h"

#include "jitcache/session/JITCacheVMState.h"
#include <algorithm>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

RequestTimer::RequestTimer(VMState& vmState)
{
    if (BenchReport* report = vmState.benchReport())
        m_measurements.emplace(*report);
}

void RequestTimer::record(ASCIILiteral action, const BodyKey& key, uint64_t sourceCharacters)
{
    if (!m_measurements)
        return;
    Measurements& measurements = *m_measurements;
    uint64_t counted = 0;
    uint64_t apart = 0;
    if (measurements.started) {
        if (measurements.account == CostAccount::Counted)
            measurements.countedNanoseconds += benchMonotonicNanoseconds() - measurements.wallMark;
        uint64_t cpu = benchThreadCPUNanoseconds() - measurements.cpuStart;
        // The cap also keeps apart from wrapping.
        counted = std::min(measurements.countedNanoseconds, cpu);
        apart = cpu - counted;
        measurements.started = false;
    }

    // A part that did not run, and without breakdowns a counted part, reports null.
    measurements.report.record("request"_s, {
        { "key"_s, bodyKeyHex(key) },
        { "action"_s, String { action } },
        { "counted"_s, counted },
        { "apart"_s, apart },
        // A child's holder digest beyond its environments' first digests; the section parses have their own fields.
        { "matching"_s, partValue(RequestPart::HolderDigest) },
        { "keyDigest"_s, partValue(RequestPart::KeyDigest) },
        { "lookup"_s, partValue(RequestPart::Lookup) },
        { "identityParse"_s, partValue(RequestPart::IdentityParse) },
        { "contextDigest"_s, partValue(RequestPart::ContextDigest) },
        { "rootHolderDigest"_s, partValue(RequestPart::RootHolderDigest) },
        { "environmentDigests"_s, partValue(RequestPart::EnvironmentDigests) },
        { "holderDigest"_s, partValue(RequestPart::HolderDigest) },
        { "feedbackParse"_s, partValue(RequestPart::FeedbackParse) },
        { "decode"_s, partValue(RequestPart::Decode) },
        { "butterflyRebuild"_s, partValue(RequestPart::ButterflyRebuild) },
        { "seeding"_s, partValue(RequestPart::Seeding) },
        { "parseFields"_s, partValue(RequestPart::ParseFields) },
        { "extraMemory"_s, partValue(RequestPart::ExtraMemory) },
        { "records"_s, partValue(RequestPart::Records) },
        { "sourceCharacters"_s, sourceCharacters },
        { "coreBytes"_s, measurements.coreBytes ? BenchValue { *measurements.coreBytes } : BenchValue { nullptr } },
    });
}

} // namespace JSC::JITCache
