#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/base/BodyKey.h"
#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/tests/maintenance/MaintenanceTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sys/stat.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of compact's planning and its rewrite of the pack (maintenance/JITCacheMaintenance.h and .cpp), on temporary
// directories whose bodies the writer committed to the pack: the order and the ratio's target the plan follows, the
// rewrite that leaves evicted and superseded bodies behind, and the damaged packs, damaged bodies and FIFOs it meets.

namespace JSC::JITCache::Tests {

using namespace MaintenanceTestSupport;

// Bodies with known L, P and sizes are planned in increasing (L + P) / B, ties by key bytes, and the plan counts the
// header's bytes beside the bodies'.
JITCACHE_TEST(maintenanceCompactOrdersByScoreThenKey, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto low = artifact->commit(context, maintenanceKey(3), 100, 0, 4096);
    auto first = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    auto tieA = artifact->commit(context, maintenanceKey(4), 500, 50, 64);
    auto tieB = artifact->commit(context, maintenanceKey(5), 500, 50, 64);
    auto high = artifact->commit(context, maintenanceKey(2), 500, 100, 64);
    if (!low || !first || !tieA || !tieB || !high)
        return;
    // The data gives the order it is meant to: low, first, the tie, high.
    JITCACHE_CHECK(low->score < first->score && first->score < tieA->score && tieA->score == tieB->score && tieB->score < high->score);

    Vector<CommittedBody> expected = inEvictionOrder({ *low, *first, *tieA, *tieB, *high });
    JITCACHE_CHECK(expected[0].key == low->key && expected[1].key == first->key && expected[4].key == high->key);

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::No);
    if (!checkOutcome(context, "compact 1 --no"_s, report, Maintenance::Outcome::Declined) || !report.plan)
        return;
    const Maintenance::Plan& plan = *report.plan;
    checkEvictions(context, "the whole plan"_s, plan, expected.span());
    uint64_t bodiesBytes = totalBytes(expected.span());
    JITCACHE_CHECK(plan.postCleanBytes == bodiesBytes + artifact->headerBytes().size());
    JITCACHE_CHECK(plan.targetBytes == plan.postCleanBytes);
    JITCACHE_CHECK(plan.evictedBytes == bodiesBytes);
    JITCACHE_CHECK(plan.deletesArtifact);
    JITCACHE_CHECK(plan.headerDigest == artifact->headerDigest());
    JITCACHE_CHECK(report.diagnostics.isEmpty());
    JITCACHE_CHECK(artifact->bodiesInPack() == 5 && artifact->exists("cache/header"));
}

// Ratio 0 evicts nothing and only runs the cleanup, on an artifact with bodies and on one with none, whose header
// stays.
JITCACHE_TEST(maintenanceCompactAtRatioZeroOnlyCleans, No)
{
    HookScope hooks;
    for (unsigned bodyCount : { 2u, 0u }) {
        String label = makeString("ratio 0 over "_s, bodyCount, " bodies"_s);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 40))
            return;
        // Besides the temporary, compact drops the pack's dead bytes: the index deltas the commits appended.
        uint64_t sizeBefore = artifact->artifactBytes();

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done) || !report.plan)
            continue;
        const Maintenance::Plan& plan = *report.plan;
        if (!plan.evictions.isEmpty() || plan.targetBytes || plan.evictedBytes || plan.deletesArtifact)
            JITCACHE_FAIL(makeString(label, ": the plan evicts something"_s));
        if (plan.postCleanBytes != totalBytes(bodies->span()) + artifact->headerBytes().size())
            JITCACHE_FAIL(makeString(label, ": postCleanBytes is "_s, plan.postCleanBytes));
        if (report.temporariesRemoved != 1 || report.bytesReclaimed != sizeBefore - artifact->artifactBytes() || report.bodiesEvicted || !report.diagnostics.isEmpty())
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (!artifact->exists("cache/header") || artifact->bodiesInPack() != bodyCount || artifact->temporariesInCache())
            JITCACHE_FAIL(makeString(label, ": the artifact is not what the cleanup leaves"_s));
    }
}

// A ratio whose target falls inside a body's bytes evicts that body, and every body before it, and nothing after.
JITCACHE_TEST(maintenanceCompactEvictsTheBodyItsTargetFallsIn, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    auto& order = *bodies;
    uint64_t postCleanBytes = totalBytes(order.span()) + artifact->headerBytes().size();
    double ratio = (static_cast<double>(order[0].bytes) + static_cast<double>(order[1].bytes) / 2) / static_cast<double>(postCleanBytes);
    uint64_t sizeBefore = artifact->artifactBytes();

    Maintenance::Report report = Maintenance::compact(artifact->path(), ratio, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    const Maintenance::Plan& plan = *report.plan;
    JITCACHE_CHECK(plan.postCleanBytes == postCleanBytes);
    JITCACHE_CHECK(plan.targetBytes == static_cast<uint64_t>(std::ceil(ratio * static_cast<double>(postCleanBytes))));
    JITCACHE_CHECK(plan.targetBytes > order[0].bytes && plan.targetBytes <= order[0].bytes + order[1].bytes);
    checkEvictions(context, "the plan"_s, plan, order.span().first(2));
    JITCACHE_CHECK(plan.evictedBytes == order[0].bytes + order[1].bytes);
    JITCACHE_CHECK(!plan.deletesArtifact);
    // The compaction reclaims what the old pack held beyond the new one: the evicted records and the index deltas.
    JITCACHE_CHECK(report.bodiesEvicted == 2 && report.bytesReclaimed == sizeBefore - artifact->artifactBytes());
    JITCACHE_CHECK(report.bytesReclaimed > order[0].bytes + order[1].bytes);
    JITCACHE_CHECK(!artifact->holdsBody(order[0].key) && !artifact->holdsBody(order[1].key) && artifact->holdsBody(order[2].key));
    JITCACHE_CHECK(artifact->bodiesInPack() == 1);
    JITCACHE_CHECK(artifact->exists("cache/header"));
}

// Compact plans the bodies the pack's index lists, the newest version of each key only, and applies a plan that keeps
// bodies as a compaction: the evicted bodies and the superseded versions leave with the old pack, and the reclaimed bytes
// are the artifact's own decrease. A ratio of 1 deletes the pack with the rest.
JITCACHE_TEST(maintenanceCompactRewritesThePack, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 4);
    if (!bodies)
        return;
    auto& order = *bodies;
    // A later version of the highest-scored body supersedes its first.
    auto newer = artifact->commit(context, order[3].key, 500, 120, 64);
    if (!newer)
        return;

    Maintenance::Report declined = compactAnswering(*artifact, 1, Maintenance::Answer::No);
    if (!checkOutcome(context, "compact 1 --no"_s, declined, Maintenance::Outcome::Declined) || !declined.plan)
        return;
    JITCACHE_CHECK(declined.plan->evictions.size() == 4);
    uint64_t planned = order[0].bytes + order[1].bytes + order[2].bytes + newer->bytes;
    JITCACHE_CHECK(declined.plan->postCleanBytes == planned + artifact->headerBytes().size());

    uint64_t sizeBefore = artifact->artifactBytes();
    double ratio = static_cast<double>(order[0].bytes) / static_cast<double>(declined.plan->postCleanBytes);
    Maintenance::Report report = Maintenance::compact(artifact->path(), ratio, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    JITCACHE_CHECK(report.plan->evictions.size() == 1 && report.bodiesEvicted == 1);
    JITCACHE_CHECK(!artifact->holdsBody(order[0].key) && artifact->holdsBody(order[1].key) && artifact->holdsBody(order[2].key));
    JITCACHE_CHECK(artifact->bodiesInPack() == 3);
    JITCACHE_CHECK(report.bytesReclaimed == sizeBefore - artifact->artifactBytes());
    RefPtr view = artifact->freshView();
    JITCACHE_CHECK(view && view->token(order[3].key) == newer->version);

    Maintenance::Report deletion = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    if (!checkOutcome(context, "compact 1 --yes"_s, deletion, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(deletion.bodiesEvicted == 3 && !artifact->exists("cache"));
}

// Maintenance has no config to compare an app with, so an artifact whose header names an app plans and rewrites as one
// that names none, under that header's digest, and keeps its header byte for byte.
JITCACHE_TEST(maintenanceCompactKeepsAHeaderThatNamesAnApp, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context, Layout::Artifact, maintenanceHeader(0x15, 0x0123456789abcdef));
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    auto& order = *bodies;
    auto headerBefore = readFileAt(artifact->cacheFd(), ArtifactNames::header.characters());
    uint64_t postCleanBytes = totalBytes(order.span()) + artifact->headerBytes().size();
    double ratio = static_cast<double>(order[0].bytes) / static_cast<double>(postCleanBytes);
    uint64_t sizeBefore = artifact->artifactBytes();

    Maintenance::Report report = Maintenance::compact(artifact->path(), ratio, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    JITCACHE_CHECK(report.plan->headerDigest == artifact->headerDigest());
    JITCACHE_CHECK(artifact->headerDigest() != artifactHeaderDigest(maintenanceHeader().span()));
    JITCACHE_CHECK(report.plan->postCleanBytes == postCleanBytes);
    checkEvictions(context, "the plan"_s, *report.plan, order.span().first(1));
    JITCACHE_CHECK(report.bodiesEvicted == 1 && report.diagnostics.isEmpty());
    JITCACHE_CHECK(report.bytesReclaimed == sizeBefore - artifact->artifactBytes());
    JITCACHE_CHECK(!artifact->holdsBody(order[0].key) && artifact->holdsBody(order[1].key) && artifact->holdsBody(order[2].key));
    auto headerAfter = readFileAt(artifact->cacheFd(), ArtifactNames::header.characters());
    JITCACHE_CHECK(headerBefore && headerAfter && *headerBefore == *headerAfter);
    JITCACHE_CHECK(headerAfter && equalSpans(headerAfter->span(), artifact->headerBytes()));
}

// A pack that fails its own checks is evicted whole, before any body, by any ratio above 0, with damaged-pack; since
// no body is left to keep, the plan deletes the artifact. Ratio 0 leaves it.
JITCACHE_TEST(maintenanceCompactEvictsADamagedPack, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto packed = commitBodies(context, *artifact, 2);
    if (!packed || !artifact->damagePack(context))
        return;

    Maintenance::Report kept = Maintenance::compact(artifact->path(), 0, { });
    if (!checkOutcome(context, "compact 0"_s, kept, Maintenance::Outcome::Done) || !kept.plan)
        return;
    JITCACHE_CHECK(!kept.plan->damagedPackBytes && artifact->exists("cache/pack"));
    bool reported = std::ranges::any_of(kept.diagnostics, [](auto& diagnostic) { return diagnostic.code == "damaged-pack"_s; });
    JITCACHE_CHECK(reported);

    Maintenance::Report report = compactAnswering(*artifact, 0.01, Maintenance::Answer::Yes);
    if (!checkOutcome(context, "compact 0.01 --yes"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    JITCACHE_CHECK(report.plan->damagedPackBytes && report.plan->evictions.isEmpty() && report.plan->deletesArtifact);
    JITCACHE_CHECK(!artifact->exists("cache"));
}

#if OS(LINUX)
// A FIFO at cache/pack is a damaged pack, which compact reads at once instead of waiting for a writer that never opens
// the FIFO: ratio 0 leaves it with damaged-pack, and any ratio above 0 deletes the artifact. A FIFO at cache/header is a
// corrupt header, which stops compact after its cleanup with bad-header. A regression shows as this test hanging.
JITCACHE_TEST(maintenanceCompactTreatsAFIFOAsDamage, No)
{
    HookScope hooks;
    auto packFIFO = MaintenanceArtifact::create(context);
    if (!packFIFO || !replaceWithFIFO(context, makeString(packFIFO->path(), "/cache/pack"_s)))
        return;
    Maintenance::Report kept = Maintenance::compact(packFIFO->path(), 0, { });
    if (checkOutcome(context, "compact 0 over a FIFO at cache/pack"_s, kept, Maintenance::Outcome::Done) && kept.plan) {
        JITCACHE_CHECK(countDiagnostics(kept, "damaged-pack"_s) == 1);
        JITCACHE_CHECK(!kept.plan->deletesArtifact && packFIFO->exists("cache/pack"));
    }
    Maintenance::Report deleted = compactAnswering(*packFIFO, 0.5, Maintenance::Answer::Yes);
    if (checkOutcome(context, "compact 0.5 --yes over a FIFO at cache/pack"_s, deleted, Maintenance::Outcome::Done) && deleted.plan)
        JITCACHE_CHECK(deleted.plan->deletesArtifact && deleted.plan->evictions.isEmpty());
    JITCACHE_CHECK(!packFIFO->exists("cache"));

    auto headerFIFO = MaintenanceArtifact::create(context);
    if (!headerFIFO || !replaceWithFIFO(context, makeString(headerFIFO->path(), "/cache/header"_s)))
        return;
    Maintenance::Report stopped = compactAnswering(*headerFIFO, 0.5, Maintenance::Answer::Yes);
    if (checkOutcome(context, "compact 0.5 --yes over a FIFO at cache/header"_s, stopped, Maintenance::Outcome::Failed))
        JITCACHE_CHECK(countDiagnostics(stopped, "bad-header"_s) == 1 && !stopped.plan);
    JITCACHE_CHECK(headerFIFO->exists("cache/header") && headerFIFO->exists("cache/pack"));
}
#endif // OS(LINUX)

// A body that fails its checks while compact moves it is left out with a damaged-body diagnostic, and the bodies
// around it move whole.
JITCACHE_TEST(maintenanceCompactDropsADamagedBody, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    // The middle record's last byte belongs to its last section, which every check of the body covers.
    RefPtr view = artifact->freshView();
    auto pack = readFileAt(artifact->cacheFd(), ArtifactNames::pack.characters());
    if (!view || !pack) {
        JITCACHE_FAIL("the pack cannot be read"_s);
        return;
    }
    const CommittedBody& damaged = (*bodies)[1];
    uint64_t recordOffset = packPrefixBytes + packIndexBlockBytes(0);
    // Each commit appended its record and a delta of one entry; the record whose header names the damaged key is hit.
    while (recordOffset + packRecordHeaderBytes <= pack->size()) {
        auto header = decodePackRecordHeader(pack->span().subspan(recordOffset).first<packRecordHeaderBytes>());
        if (!header)
            break;
        if (header->key == damaged.key) {
            (*pack)[recordOffset + packRecordHeaderBytes + header->size - 1] ^= 0x20;
            break;
        }
        recordOffset += *packRecordExtent(header->size) + packIndexBlockBytes(1);
    }
    if (!writeFileAt(context, artifact->cacheFd(), ArtifactNames::pack.characters(), pack->span()))
        return;

    // A recapture leaves dead bytes, so even ratio 0 rewrites the pack.
    auto newer = artifact->commit(context, (*bodies)[0].key, 500, 7, 64);
    if (!newer)
        return;
    Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
    if (!checkOutcome(context, "compact 0"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.bodiesEvicted == 1);
    JITCACHE_CHECK(countDiagnostics(report, "damaged-body"_s) == 1);
    JITCACHE_CHECK(!artifact->holdsBody(damaged.key) && artifact->holdsBody((*bodies)[0].key) && artifact->holdsBody((*bodies)[2].key));
}

// Flips the last byte of the key's body, which belongs to its last section, so the section's checksum fails, and writes
// the pack back in place. The records lie from firstRecord on, each followed by gapBytes of index: a one-entry delta in a
// pack the writer's commits left, nothing in a dense pack, whose one full index follows the last record.
static bool maintenanceFlipLastByteOfBody(TestContext& context, MaintenanceArtifact& artifact, const BodyKey& key, uint64_t firstRecord, uint64_t gapBytes)
{
    auto pack = readFileAt(artifact.cacheFd(), ArtifactNames::pack.characters());
    if (!pack) {
        JITCACHE_FAIL("the pack cannot be read"_s);
        return false;
    }
    for (uint64_t recordOffset = firstRecord; recordOffset + packRecordHeaderBytes <= pack->size();) {
        auto header = decodePackRecordHeader(pack->span().subspan(recordOffset).first<packRecordHeaderBytes>());
        if (!header)
            break;
        if (header->key == key) {
            (*pack)[recordOffset + packRecordHeaderBytes + header->size - 1] ^= 0x20;
            return writeFileAt(context, artifact.cacheFd(), ArtifactNames::pack.characters(), pack->span());
        }
        recordOffset += *packRecordExtent(header->size) + gapBytes;
    }
    JITCACHE_FAIL("the pack holds no record of the body to damage"_s);
    return false;
}

// A dense pack, as every delta leaves one, keeps its file at ratio 0 only when every body in it passes the checks a
// compaction runs: without damage compact 0 leaves the file as it was, and with a flipped section byte in one body it
// rewrites the pack without that body, reported as damaged-body, and keeps the others whole.
JITCACHE_TEST(maintenanceCompactChecksTheBodiesOfADensePack, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    // The commits' index deltas are dead bytes, which the first compact drops, so the pack is dense after it.
    if (!checkOutcome(context, "the first compact 0"_s, Maintenance::compact(artifact->path(), 0, { }), Maintenance::Outcome::Done))
        return;
    auto packInode = [&]() -> uint64_t {
        struct stat status;
        if (fstatat(artifact->cacheFd(), ArtifactNames::pack.characters(), &status, AT_SYMLINK_NOFOLLOW))
            return 0;
        return static_cast<uint64_t>(status.st_ino);
    };
    uint64_t denseInode = packInode();
    uint64_t denseBytes = artifact->packFileSize();
    JITCACHE_CHECK(denseInode && denseBytes == artifact->packValidEnd());

    Maintenance::Report sound = Maintenance::compact(artifact->path(), 0, { });
    if (checkOutcome(context, "compact 0 over a sound dense pack"_s, sound, Maintenance::Outcome::Done))
        JITCACHE_CHECK(!sound.bodiesEvicted && !sound.bytesReclaimed && sound.diagnostics.isEmpty());
    JITCACHE_CHECK(packInode() == denseInode && artifact->packFileSize() == denseBytes && artifact->bodiesInPack() == 3);

    const CommittedBody& damaged = (*bodies)[1];
    if (!maintenanceFlipLastByteOfBody(context, *artifact, damaged.key, packPrefixBytes, 0))
        return;
    Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
    if (!checkOutcome(context, "compact 0 over a dense pack with a damaged body"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.plan && report.plan->evictions.isEmpty());
    JITCACHE_CHECK(report.bodiesEvicted == 1 && report.diagnostics.size() == 1 && countDiagnostics(report, "damaged-body"_s) == 1);
    JITCACHE_CHECK(report.bytesReclaimed == denseBytes - artifact->packFileSize());
    JITCACHE_CHECK(packInode() != denseInode);
    JITCACHE_CHECK(!artifact->holdsBody(damaged.key) && artifact->holdsBody((*bodies)[0].key) && artifact->holdsBody((*bodies)[2].key));
    JITCACHE_CHECK(artifact->bodiesInPack() == 2 && artifact->packFileSize() == artifact->packValidEnd());
}

// At a ratio above 0 the rewrite drops a damaged body its plan keeps, as at ratio 0: compact 0.5 evicts the bodies its
// plan names, leaves out the damaged one with damaged-body, and moves the others whole.
JITCACHE_TEST(maintenanceCompactDropsADamagedBodyItsPlanKeeps, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 6);
    if (!bodies)
        return;
    // The last body in eviction order is the one a plan keeps longest.
    const CommittedBody& damaged = bodies->last();
    if (!maintenanceFlipLastByteOfBody(context, *artifact, damaged.key, packPrefixBytes + packIndexBlockBytes(0), packIndexBlockBytes(1)))
        return;
    Maintenance::Report report = Maintenance::compact(artifact->path(), 0.5, { });
    if (!checkOutcome(context, "compact 0.5"_s, report, Maintenance::Outcome::Done) || !report.plan)
        return;
    const Maintenance::Plan& plan = *report.plan;
    auto evicts = [&](const BodyKey& key) {
        return std::ranges::any_of(plan.evictions, [&](const Maintenance::Eviction& eviction) {
            return std::ranges::equal(eviction.key, key.bytes());
        });
    };
    JITCACHE_CHECK(!plan.deletesArtifact && !evicts(damaged.key));
    JITCACHE_CHECK(report.bodiesEvicted == plan.evictions.size() + 1 && countDiagnostics(report, "damaged-body"_s) == 1);
    uint64_t kept = 0;
    for (auto& body : *bodies) {
        bool held = artifact->holdsBody(body.key);
        if (body.key == damaged.key) {
            JITCACHE_CHECK(!held);
            continue;
        }
        JITCACHE_CHECK(held == !evicts(body.key));
        kept += held;
    }
    JITCACHE_CHECK(kept && artifact->bodiesInPack() == kept);
}

// A ratio that is NaN or outside [0, 1] fails at bad-ratio and changes nothing.
JITCACHE_TEST(maintenanceCompactRejectsABadRatio, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
        return;
    for (double ratio : { std::numeric_limits<double>::quiet_NaN(), -0.25, 1.5, std::numeric_limits<double>::infinity() }) {
        Maintenance::Report report = compactAnswering(*artifact, ratio, Maintenance::Answer::Yes);
        if (!checkOutcome(context, makeString("ratio "_s, ratio), report, Maintenance::Outcome::Failed))
            continue;
        if (report.diagnostics.size() != 1 || countDiagnostics(report, "bad-ratio"_s) != 1 || report.plan)
            JITCACHE_FAIL(makeString("ratio "_s, ratio, ": "_s, describe(report)));
    }
    JITCACHE_CHECK(artifact->temporariesInCache() == 1 && artifact->holdsBody(body->key) && artifact->exists("cache/header"));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
