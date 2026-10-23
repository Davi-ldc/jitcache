#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "VM.h"
#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/maintenance/JITCacheMaintenanceTesting.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/store/ArtifactAccess.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/tests/maintenance/MaintenanceTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <optional>
#include <span>
#include <unistd.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of compact's whole deletion, its confirmation and its interruption (maintenance/JITCacheMaintenance.h and .cpp),
// on temporary directories whose bodies the writer committed to the pack. Only the test that checks that the next Producer
// reuses a deletion's remnant needs a VM, for start.

namespace JSC::JITCache::Tests {

using namespace MaintenanceTestSupport;

namespace MaintenanceDeletionTestsInternal {

// The first call, which asks for confirmation, on a plan that deletes the artifact.
static std::optional<Maintenance::Plan> askForDeletion(TestContext& context, const String& label, MaintenanceArtifact& artifact)
{
    Maintenance::Report report = Maintenance::compact(artifact.path(), 1, { });
    if (!checkOutcome(context, label, report, Maintenance::Outcome::NeedsConfirmation) || !report.plan)
        return std::nullopt;
    return WTF::move(report.plan);
}

// The second call, which applies the confirmed plan.
static Maintenance::Report confirm(MaintenanceArtifact& artifact, const Maintenance::Plan& plan)
{
    Maintenance::CompactOptions options;
    options.answer = Maintenance::Answer::Yes;
    options.confirmedPlan = plan;
    return Maintenance::compact(artifact.path(), 1, options);
}

} // namespace MaintenanceDeletionTestsInternal

using namespace MaintenanceDeletionTestsInternal;

// Ratio 1, a ratio whose target needs every body, and any ratio above 0 on an artifact with no body plan to delete the
// whole artifact: with Ask that is NeedsConfirmation and with No Declined, and neither changes anything, temporaries
// included.
JITCACHE_TEST(maintenanceWholeDeletionWaitsForConfirmation, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
        return;
    auto& order = *bodies;
    uint64_t bodiesBytes = totalBytes(order.span());
    double everyBody = (static_cast<double>(bodiesBytes) - static_cast<double>(order[2].bytes) / 2) / static_cast<double>(bodiesBytes + artifact->headerBytes().size());
    JITCACHE_CHECK(everyBody < 1);

    auto unchanged = [&](MaintenanceArtifact& checked, std::span<const CommittedBody> held) {
        if (checked.temporariesInCache() != 1 || !checked.exists("cache/header"))
            return false;
        return std::ranges::all_of(held, [&](const CommittedBody& body) {
            return checked.holdsBody(body.key);
        });
    };

    for (double ratio : { 1.0, everyBody }) {
        for (auto answer : { Maintenance::Answer::Ask, Maintenance::Answer::No }) {
            String label = makeString("ratio "_s, ratio, answer == Maintenance::Answer::Ask ? " with Ask"_s : " with No"_s);
            Maintenance::Report report = compactAnswering(*artifact, ratio, answer);
            auto expected = answer == Maintenance::Answer::Ask ? Maintenance::Outcome::NeedsConfirmation : Maintenance::Outcome::Declined;
            if (!checkOutcome(context, label, report, expected) || !report.plan)
                continue;
            if (!report.plan->deletesArtifact)
                JITCACHE_FAIL(makeString(label, ": the plan keeps the artifact"_s));
            checkEvictions(context, label, *report.plan, order.span());
            if (report.temporariesRemoved || report.bodiesEvicted || report.bytesReclaimed || !unchanged(*artifact, order.span()))
                JITCACHE_FAIL(makeString(label, ": the call changed the artifact"_s));
        }
    }

    auto empty = MaintenanceArtifact::create(context);
    if (!empty || !empty->placeTemporary(context, TemporaryKind::Pack, 10))
        return;
    for (auto answer : { Maintenance::Answer::Ask, Maintenance::Answer::No }) {
        String label = answer == Maintenance::Answer::Ask ? "no body with Ask"_s : "no body with No"_s;
        Maintenance::Report report = compactAnswering(*empty, 0.01, answer);
        auto expected = answer == Maintenance::Answer::Ask ? Maintenance::Outcome::NeedsConfirmation : Maintenance::Outcome::Declined;
        if (!checkOutcome(context, label, report, expected) || !report.plan)
            continue;
        if (!report.plan->deletesArtifact || !report.plan->evictions.isEmpty() || report.plan->postCleanBytes != empty->headerBytes().size())
            JITCACHE_FAIL(makeString(label, ": the plan is not a deletion of an artifact without bodies"_s));
        if (!unchanged(*empty, { }))
            JITCACHE_FAIL(makeString(label, ": the call changed the artifact"_s));
    }
}

// Yes deletes the temporaries, the pack, the header and cache/, and keeps the lock file.
JITCACHE_TEST(maintenanceWholeDeletionRemovesEverythingButTheLockFile, No)
{
    HookScope hooks;
    for (unsigned bodyCount : { 3u, 0u }) {
        String label = makeString("a deletion of "_s, bodyCount, " bodies"_s);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies || !artifact->placeTemporary(context, TemporaryKind::Pack, 100) || !artifact->placeTemporary(context, TemporaryKind::Header, 30))
            return;
        uint64_t packBytes = artifact->packFileSize();

        Maintenance::Report report = compactAnswering(*artifact, bodyCount ? 1 : 0.5, Maintenance::Answer::Yes);
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done) || !report.plan)
            continue;
        JITCACHE_CHECK(report.plan->deletesArtifact);
        JITCACHE_CHECK(report.diagnostics.isEmpty());
        JITCACHE_CHECK(report.temporariesRemoved == 2 && report.bodiesEvicted == bodyCount);
        uint64_t everything = 130 + packBytes + artifact->headerBytes().size();
        if (report.bytesReclaimed != everything)
            JITCACHE_FAIL(makeString(label, ": "_s, report.bytesReclaimed, " bytes reclaimed where "_s, everything, " were expected"_s));
        JITCACHE_CHECK(!artifact->exists("cache"));
        JITCACHE_CHECK(artifact->exists(ArtifactNames::lockFile.characters()));
    }
}

// With the pack's replacement made to fail, Yes keeps every body and the header, returns Failed with unlink-failed,
// and the store's open accepts what remains.
JITCACHE_TEST(maintenanceWholeDeletionKeepsTheHeaderWhenThePackStays, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 3);
    if (!bodies)
        return;
    auto& order = *bodies;
    Maintenance::Testing::setFailingBodyUnlink(1);

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    Maintenance::Testing::setFailingBodyUnlink(std::nullopt);
    if (!checkOutcome(context, "compact 1 --yes"_s, report, Maintenance::Outcome::Failed) || !report.plan)
        return;
    checkEvictions(context, "the plan"_s, *report.plan, order.span());
    JITCACHE_CHECK(report.diagnostics.size() == 1);
    JITCACHE_CHECK(hasDiagnosticStartingWith(report, "unlink-failed"_s, "cache/pack: "_s));
    JITCACHE_CHECK(!report.bodiesEvicted && !report.bytesReclaimed);
    JITCACHE_CHECK(artifact->exists("cache/header"));

    RefPtr<OpenedArtifact> object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->indexedBodies() == 3);
    BodyOpen opened = object->open(order[1].key);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Found && opened.body && opened.body->version() == order[1].version);
}

// With an unknown file in cache/, Yes returns Done with rmdir-failed, and the next Producer reuses the remnant.
JITCACHE_TEST(maintenanceWholeDeletionLeavesARemnantTheNextProducerReuses, Yes)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies || !artifact->placeFile(context, artifact->cacheFd(), "stray", 4))
        return;

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    if (!checkOutcome(context, "compact 1 --yes"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.bodiesEvicted == 2);
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/stray"_s));
    JITCACHE_CHECK(countDiagnostics(report, "rmdir-failed"_s) == 1 && report.diagnostics.size() == 2);
    JITCACHE_CHECK(!artifact->exists("cache/header") && artifact->exists("cache/stray") && !artifact->exists("cache/pack"));

    JITCache::Config config;
    config.artifactPath = artifact->path();
    config.role = Role::Producer;
    config.producerLimitBytes = 1 << 20;
    StartResult started = JITCache::start(*context.vm(), config);
    if (started.outcome != StartOutcome::Started)
        JITCACHE_FAIL(makeString("the Producer's start is "_s, JITCache::name(started.outcome), " at "_s, started.step, ": "_s, started.detail));
    JITCACHE_CHECK(artifact->exists("cache/header") && artifact->exists("cache/stray"));
}

// A plan confirmed after a commit, a recapture of a key, an eviction or a header change in between gives PlanChanged
// with the new plan and evicts nothing; an unchanged one applies.
JITCACHE_TEST(maintenanceConfirmationRequiresAnUnchangedPlan, No)
{
    HookScope hooks;
    enum class Change : uint8_t { Commit, Recapture, Eviction, Header, None };
    for (Change change : { Change::Commit, Change::Recapture, Change::Eviction, Change::Header, Change::None }) {
        static constexpr std::array<ASCIILiteral, 5> labels { "a commit"_s, "a recapture"_s, "an eviction"_s, "a header change"_s, "no change"_s };
        String label = labels[static_cast<size_t>(change)];
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, 3);
        if (!bodies)
            return;
        auto plan = askForDeletion(context, label, *artifact);
        if (!plan)
            continue;

        size_t bodiesAfterChange = 3;
        switch (change) {
        case Change::Commit:
            if (!artifact->commit(context, maintenanceKey(9), 500, 5, 64))
                return;
            bodiesAfterChange = 4;
            break;
        case Change::Recapture:
            if (!artifact->commit(context, (*bodies)[0].key, 700, 30, 64))
                return;
            break;
        case Change::Eviction: {
            Maintenance::Report eviction = Maintenance::compact(artifact->path(), 1 / static_cast<double>(plan->postCleanBytes), { });
            if (!checkOutcome(context, makeString(label, ", the eviction"_s), eviction, Maintenance::Outcome::Done) || eviction.bodiesEvicted != 1)
                return;
            bodiesAfterChange = 2;
            break;
        }
        case Change::Header:
            if (!artifact->replaceHeader(context, maintenanceHeader(0x2A)))
                return;
            // The pack belongs to the old header, which a reader under the new one rejects.
            bodiesAfterChange = 0;
            break;
        case Change::None:
            break;
        }

        Maintenance::Report report = confirm(*artifact, *plan);
        if (change == Change::None) {
            if (checkOutcome(context, label, report, Maintenance::Outcome::Done) && report.plan)
                JITCACHE_CHECK(*report.plan == *plan && report.bodiesEvicted == 3);
            JITCACHE_CHECK(!artifact->exists("cache"));
            continue;
        }
        if (!checkOutcome(context, label, report, Maintenance::Outcome::PlanChanged) || !report.plan)
            continue;
        if (*report.plan == *plan)
            JITCACHE_FAIL(makeString(label, ": PlanChanged returned the confirmed plan"_s));
        if (change == Change::Header && equalSpans(std::span { report.plan->headerDigest }, std::span { plan->headerDigest }))
            JITCACHE_FAIL(makeString(label, ": the new plan has the old header digest"_s));
        if (report.bodiesEvicted || report.temporariesRemoved || report.bytesReclaimed)
            JITCACHE_FAIL(makeString(label, ": PlanChanged removed something: "_s, describe(report)));
        if (artifact->currentBodies() != bodiesAfterChange || !artifact->exists("cache/header"))
            JITCACHE_FAIL(makeString(label, ": the artifact changed after PlanChanged"_s));
    }
}

// An apply stopped after the pack's emptying leaves a valid artifact without bodies, and one stopped after the
// header's unlink a remnant whose pack holds no body, which the next compact finishes.
JITCACHE_TEST(maintenanceInterruptedDeletionLeavesWholeBodies, No)
{
    HookScope hooks;
    constexpr unsigned bodyCount = 3;
    for (unsigned n = 1; n <= 2; ++n) {
        String label = makeString("stopped after step "_s, n);
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, bodyCount);
        if (!bodies)
            return;
        auto& order = *bodies;

        Maintenance::Testing::setStopAfterUnlink(n);
        Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
        Maintenance::Testing::setStopAfterUnlink(std::nullopt);
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Failed) || !report.plan)
            continue;
        checkEvictions(context, label, *report.plan, order.span());

        if (n == 2) {
            if (artifact->exists("cache/header") || !artifact->exists("cache/pack"))
                JITCACHE_FAIL(makeString(label, ": the artifact is not a bodiless remnant"_s));
            Maintenance::Report cleaned = Maintenance::compact(artifact->path(), 0, { });
            if (checkOutcome(context, makeString(label, ", the next compact"_s), cleaned, Maintenance::Outcome::NoArtifact) && artifact->exists("cache"))
                JITCACHE_FAIL(makeString(label, ": the next compact did not finish the remnant"_s));
            continue;
        }

        if (report.bodiesEvicted != bodyCount || !artifact->exists("cache/header") || artifact->bodiesInPack())
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        RefPtr<OpenedArtifact> object = artifact->take(context);
        if (object && object->indexedBodies())
            JITCACHE_FAIL(makeString(label, ": the index lists "_s, object->indexedBodies(), " bodies"_s));
    }
}

// A read between a whole deletion's emptying and the empty pack's unlink leaves the object mapping the empty pack. An
// apply stopped after the header's unlink leaves that pack in a remnant, and the compact that finishes the remnant marks
// it retired before it unlinks it, so the object looks at the path again at its next read.
JITCACHE_TEST(maintenanceFinishedRemnantRetiresItsPack, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies)
        return;
    RefPtr<OpenedArtifact> held = artifact->take(context);
    if (!held)
        return;
    JITCACHE_CHECK(held->indexedBodies() == 2);

    Maintenance::Testing::setStopAfterUnlink(2);
    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    Maintenance::Testing::setStopAfterUnlink(std::nullopt);
    if (!checkOutcome(context, "compact 1 --yes stopped after the header's unlink"_s, report, Maintenance::Outcome::Failed))
        return;
    // The read meets the old pack's mark and maps the empty pack.
    JITCACHE_CHECK(!held->token((*bodies)[0].key) && !held->indexedBodies());
    int fd = openat(artifact->cacheFd(), ArtifactNames::pack.characters(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot open the remnant's pack: "_s, errnoText(errno)));
        return;
    }

    Maintenance::Report cleaned = Maintenance::compact(artifact->path(), 0, { });
    uint64_t mark = 0;
    bool read = pread(fd, &mark, sizeof(mark), static_cast<off_t>(packRetiredOffset)) == static_cast<ssize_t>(sizeof(mark));
    ::close(fd);
    if (checkOutcome(context, "the compact that finishes the remnant"_s, cleaned, Maintenance::Outcome::NoArtifact))
        JITCACHE_CHECK(!artifact->exists("cache"));
    JITCACHE_CHECK(read && mark == 1);
    JITCACHE_CHECK(!held->token((*bodies)[1].key) && !held->indexedBodies());
}

// A Producer of another process that reuses that remnant renames its new pack over the empty pack, which an object of
// this process may map. The creation marks the remnant's pack retired first, so the registry's object for cache/ follows
// to the new pack, and a VM that starts afterwards lists the bodies the Producer committed. take follows the object
// before it decides, so the object it returns lists them at once.
JITCACHE_TEST(maintenanceReusedRemnantMovesAnObjectToTheNewPack, No)
{
    HookScope hooks;
    // The header a Producer writes for a config that names no app, so the Producer below writes the same bytes again.
    auto artifact = MaintenanceArtifact::create(context, Layout::Artifact, headerNamingApp(std::nullopt));
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies)
        return;
    RefPtr<OpenedArtifact> held = artifact->take(context);
    if (!held)
        return;
    JITCACHE_CHECK(held->indexedBodies() == 2);

    Maintenance::Testing::setStopAfterUnlink(2);
    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Yes);
    Maintenance::Testing::setStopAfterUnlink(std::nullopt);
    if (!checkOutcome(context, "compact 1 --yes stopped after the header's unlink"_s, report, Maintenance::Outcome::Failed))
        return;
    JITCACHE_CHECK(!held->token((*bodies)[0].key) && !held->indexedBodies());

    // With registry sharing off, the Producer's objects stand for another process's, so none of them moves the held one.
    StoreTesting::setRegistrySharing(false);
    bool created = false;
    {
        // The access goes at the end of the block, and with it the producer lock the commits below take.
        auto access = openArtifact(artifact->path(), ArtifactAccessMode::Create, std::nullopt);
        created = !!access;
        if (!access)
            JITCACHE_FAIL(makeString("the Producer's creation in the remnant failed: "_s, access.error().detail));
    }
    std::optional<Vector<CommittedBody>> recommitted;
    if (created)
        recommitted = commitBodies(context, *artifact, 2, 11);
    StoreTesting::setRegistrySharing(true);
    if (!recommitted)
        return;

    RefPtr<OpenedArtifact> object = artifact->take(context);
    if (!object)
        return;
    JITCACHE_CHECK(object->indexedBodies() == 2);
    for (auto& body : *recommitted)
        JITCACHE_CHECK(object->token(body.key) == body.version);
    for (auto& body : *bodies)
        JITCACHE_CHECK(!object->token(body.key));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
