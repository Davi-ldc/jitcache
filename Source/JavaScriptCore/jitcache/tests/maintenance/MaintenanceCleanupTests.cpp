#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/maintenance/JITCacheMaintenance.h"
#include "jitcache/maintenance/JITCacheMaintenanceTesting.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/JITCacheContainer.h"
#include "jitcache/tests/maintenance/MaintenanceTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Tests of compact's cleanup (maintenance/JITCacheMaintenance.h and .cpp), on temporary directories whose bodies the writer
// committed to the pack. A test lays out by hand what an interrupted run or another binary leaves: temporaries a killed
// producer left, bytes past the pack's valid end, an abandoned retired mark, a .cache.replaced a replacement left,
// header-less remnants, a header that fails its checks, a pack of another layout, and names JITCache never writes.

namespace JSC::JITCache::Tests {

using namespace MaintenanceTestSupport;

// Compact at ratio 0 over a pack without dead bytes removes exactly the temporaries, of both kinds, and the bytes past
// the pack's valid end, and reports their bytes; it leaves the bodies, the header and the names it does not know, and
// reports each of those.
JITCACHE_TEST(maintenanceCompactRemovesOnlyTemporaries, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies)
        return;
    // The commits' index deltas are dead bytes, which a first compact drops, so the second only has the cleanup to do.
    if (!checkOutcome(context, "the first compact"_s, Maintenance::compact(artifact->path(), 0, { }), Maintenance::Outcome::Done))
        return;
    if (!artifact->placeTemporary(context, TemporaryKind::Pack, 100) || !artifact->placeTemporary(context, TemporaryKind::Pack, 200)
        || !artifact->placeTemporary(context, TemporaryKind::Header, 50)
        || !artifact->placeFile(context, artifact->cacheFd(), "notes.txt", 7) || !artifact->placeFile(context, artifact->cacheFd(), "stray", 9))
        return;
    // A commit that stopped before its root left bytes past the valid end.
    uint64_t validEnd = artifact->packValidEnd();
    if (!artifact->appendGarbage(context, 77))
        return;

    Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::Done))
        return;
    JITCACHE_CHECK(report.temporariesRemoved == 3);
    JITCACHE_CHECK(report.bytesReclaimed == 350 + 77);
    JITCACHE_CHECK(!report.bodiesEvicted);
    JITCACHE_CHECK(report.plan && report.plan->evictions.isEmpty());
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->packFileSize() == validEnd && artifact->packValidEnd() == validEnd);
    JITCACHE_CHECK(artifact->holdsBody((*bodies)[0].key) && artifact->holdsBody((*bodies)[1].key));
    JITCACHE_CHECK(artifact->exists("cache/header"));
    JITCACHE_CHECK(artifact->exists("cache/notes.txt") && artifact->exists("cache/stray"));
    JITCACHE_CHECK(report.diagnostics.size() == 2);
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/notes.txt"_s));
    JITCACHE_CHECK(hasDiagnostic(report, "unknown-file"_s, "cache/stray"_s));

    // A compact with nothing to remove reclaims nothing.
    report = Maintenance::compact(artifact->path(), 0, { });
    JITCACHE_CHECK(report.outcome == Maintenance::Outcome::Done && !report.temporariesRemoved && !report.bytesReclaimed);
}

// A compaction that died between its retired mark and its rename leaves the mark on the current pack: compact at
// ratio 0 clears it, in place when the pack has no dead bytes and through the rewrite when it has, and every body stays.
JITCACHE_TEST(maintenanceCompactClearsAnAbandonedRetiredMark, No)
{
    HookScope hooks;
    for (bool dense : { true, false }) {
        String label = dense ? "a pack without dead bytes"_s : "a pack with dead bytes"_s;
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto bodies = commitBodies(context, *artifact, 2);
        if (!bodies)
            return;
        if (dense && !checkOutcome(context, makeString(label, ", the first compact"_s), Maintenance::compact(artifact->path(), 0, { }), Maintenance::Outcome::Done))
            continue;
        int fd = openat(artifact->cacheFd(), ArtifactNames::pack.characters(), O_WRONLY | O_CLOEXEC);
        uint64_t retired = 1;
        bool marked = fd >= 0 && pwrite(fd, &retired, sizeof(retired), static_cast<off_t>(packRetiredOffset)) == static_cast<ssize_t>(sizeof(retired));
        if (fd >= 0)
            ::close(fd);
        if (!marked) {
            JITCACHE_FAIL(makeString(label, ": cannot mark the pack retired: "_s, errnoText(errno)));
            continue;
        }
        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done))
            continue;
        auto pack = readFileAt(artifact->cacheFd(), ArtifactNames::pack.characters());
        uint64_t mark = 1;
        if (pack && pack->size() >= packPrefixBytes)
            memcpySpan(asMutableByteSpan(mark), pack->span().subspan(packRetiredOffset, sizeof(mark)));
        if (mark || artifact->bodiesInPack() != 2)
            JITCACHE_FAIL(makeString(label, ": the mark is "_s, mark, " and the pack lists "_s, artifact->bodiesInPack(), " bodies"_s));
    }
}

// The pack's cleanup runs before its rewrite, so a rewrite that fails, as on a full disk, still leaves the bytes past
// the valid end removed and the retired mark cleared, and every body in place.
JITCACHE_TEST(maintenanceCompactCleansUpBeforeAFailingRewrite, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto bodies = commitBodies(context, *artifact, 2);
    if (!bodies)
        return;
    uint64_t validEnd = artifact->packValidEnd();
    if (!artifact->appendGarbage(context, 77))
        return;
    int fd = openat(artifact->cacheFd(), ArtifactNames::pack.characters(), O_WRONLY | O_CLOEXEC);
    uint64_t retired = 1;
    bool marked = fd >= 0 && pwrite(fd, &retired, sizeof(retired), static_cast<off_t>(packRetiredOffset)) == static_cast<ssize_t>(sizeof(retired));
    if (fd >= 0)
        ::close(fd);
    if (!marked) {
        JITCACHE_FAIL(makeString("cannot mark the pack retired: "_s, errnoText(errno)));
        return;
    }

    // The commits' index deltas are dead bytes, so ratio 0 rewrites the pack, and the hook fails that rewrite.
    Maintenance::Testing::setFailingCompaction(true);
    Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
    Maintenance::Testing::setFailingCompaction(false);
    if (checkOutcome(context, "the failing rewrite"_s, report, Maintenance::Outcome::Failed)) {
        JITCACHE_CHECK(report.bytesReclaimed == 77 && !report.bodiesEvicted);
        JITCACHE_CHECK(hasDiagnosticStartingWith(report, "io"_s, "compaction.write: "_s));
    }
    auto pack = readFileAt(artifact->cacheFd(), ArtifactNames::pack.characters());
    uint64_t mark = 1;
    if (pack && pack->size() >= packPrefixBytes)
        memcpySpan(asMutableByteSpan(mark), pack->span().subspan(packRetiredOffset, sizeof(mark)));
    JITCACHE_CHECK(!mark);
    JITCACHE_CHECK(artifact->packFileSize() == validEnd && artifact->packValidEnd() == validEnd);
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->holdsBody((*bodies)[0].key) && artifact->holdsBody((*bodies)[1].key));

    report = Maintenance::compact(artifact->path(), 0, { });
    JITCACHE_CHECK(checkOutcome(context, "the rewrite after it"_s, report, Maintenance::Outcome::Done));
    JITCACHE_CHECK(artifact->packFileSize() < validEnd && artifact->bodiesInPack() == 2);
}

// compact removes the .cache.replaced a ConsumerProducer's replacement left when it stopped midway, with its header,
// pack and temporaries, and counts their bytes; a name in it that is no artifact file stays, with the directory, and is
// reported.
JITCACHE_TEST(maintenanceCompactRemovesAReplacedArtifact, No)
{
    HookScope hooks;
    for (bool withUnknownFile : { false, true }) {
        String label = withUnknownFile ? "with an unknown file"_s : "without one"_s;
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        int replacedFd = -1;
        if (mkdirat(artifact->parentFd(), ArtifactNames::replacedCacheDirectory.characters(), 0755)
            || (replacedFd = openat(artifact->parentFd(), ArtifactNames::replacedCacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString(label, ": cannot make .cache.replaced: "_s, errnoText(errno)));
            return;
        }
        bool placed = artifact->placeFile(context, replacedFd, ArtifactNames::header.characters(), 64)
            && artifact->placeFile(context, replacedFd, ArtifactNames::pack.characters(), 300)
            && (!withUnknownFile || artifact->placeFile(context, replacedFd, "stray", 5));
        ::close(replacedFd);
        if (!placed)
            return;

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::Done))
            continue;
        if (report.bytesReclaimed != 364)
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (artifact->exists(".cache.replaced") != withUnknownFile || artifact->exists(".cache.replaced/stray") != withUnknownFile)
            JITCACHE_FAIL(makeString(label, ": .cache.replaced is "_s, artifact->exists(".cache.replaced") ? "still there"_s : "gone"_s));
        if (withUnknownFile != hasDiagnosticStartingWith(report, "unlink-failed"_s, ".cache.replaced/"_s))
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        JITCACHE_CHECK(artifact->exists("cache/header"));
    }
}

// A header-less cache/ whose pack holds a body is a remnant compact never finishes: it removes the temporaries only,
// reports the remnant and returns NoArtifact, whatever the ratio.
JITCACHE_TEST(maintenanceCompactKeepsTheBodiesOfARemnant, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Pack, 40))
        return;
    if (unlinkat(artifact->cacheFd(), ArtifactNames::header.characters(), 0)) {
        JITCACHE_FAIL(makeString("cannot remove the header: "_s, errnoText(errno)));
        return;
    }

    Maintenance::Report report = compactAnswering(*artifact, 1, Maintenance::Answer::Ask);
    if (!checkOutcome(context, "compact"_s, report, Maintenance::Outcome::NoArtifact))
        return;
    JITCACHE_CHECK(report.temporariesRemoved == 1 && report.bytesReclaimed == 40 && !report.plan);
    JITCACHE_CHECK(artifact->exists("cache/pack") && artifact->bodiesInPack() == 1);
    JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "remnant-with-bodies"_s) == 1);
}

// compact finishes a header-less remnant whose pack, if any, holds no body, returns NoArtifact and keeps the lock
// file.
JITCACHE_TEST(maintenanceCompactFinishesARemnantWithoutBodies, No)
{
    HookScope hooks;
    for (bool withEmptyPack : { true, false }) {
        String label = withEmptyPack ? "with an empty pack"_s : "without a pack"_s;
        auto artifact = MaintenanceArtifact::create(context, Layout::Remnant);
        if (!artifact)
            return;
        if (!artifact->placeTemporary(context, TemporaryKind::Pack, 30) || !artifact->placeTemporary(context, TemporaryKind::Header, 20))
            return;
        uint64_t packBytes = 0;
        if (withEmptyPack) {
            auto digest = artifact->headerDigest();
            if (int error = createEmptyPack(artifact->cacheFd(), std::span<const uint8_t, 16> { digest })) {
                JITCACHE_FAIL(makeString("createEmptyPack failed: "_s, errnoText(error)));
                return;
            }
            packBytes = artifact->packFileSize();
        }

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::NoArtifact))
            continue;
        if (report.temporariesRemoved != 2 || report.bytesReclaimed != 50 + packBytes || !report.diagnostics.isEmpty() || report.plan)
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (artifact->exists("cache"))
            JITCACHE_FAIL(makeString(label, ": cache/ is still there"_s));
        if (!artifact->exists(ArtifactNames::lockFile.characters()))
            JITCACHE_FAIL(makeString(label, ": the lock file is gone"_s));
    }
}

// A remnant without a body that holds an unknown file stays in place, so the next Producer reuses it, and compact
// returns NoArtifact.
JITCACHE_TEST(maintenanceCompactLeavesARemnantHoldingAnUnknownFile, No)
{
    HookScope hooks;
    {
        String label = "an unknown file in cache/"_s;
        auto artifact = MaintenanceArtifact::create(context, Layout::Remnant);
        if (!artifact)
            return;
        if (!artifact->placeTemporary(context, TemporaryKind::Pack, 30) || !artifact->placeFile(context, artifact->cacheFd(), "keep-me", 5))
            return;

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (!checkOutcome(context, label, report, Maintenance::Outcome::NoArtifact))
            return;
        String unknown = "cache/keep-me"_s;
        if (report.temporariesRemoved != 1 || report.diagnostics.size() != 1 || !hasDiagnostic(report, "unknown-file"_s, unknown))
            JITCACHE_FAIL(makeString(label, ": "_s, describe(report)));
        if (!artifact->exists("cache") || !artifact->exists(unknown) || artifact->temporariesInCache())
            JITCACHE_FAIL(makeString(label, ": the remnant did not stay as it should"_s));
    }
}

// On a path that does not exist, on a directory without cache/ and on one whose cache is not a directory, compact
// returns NoArtifact and creates no lock file, and gets there before it would read its ratio.
JITCACHE_TEST(maintenanceWithoutAnArtifactCreatesNoLockFile, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context, Layout::Parent);
    if (!artifact)
        return;
    String missing = makeString(artifact->path(), "/missing"_s);
    JITCACHE_CHECK(Maintenance::compact(missing, 0, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(missing, 0.5, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists("missing"));

    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0.5, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), std::numeric_limits<double>::quiet_NaN(), { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists(ArtifactNames::lockFile.characters()));

    if (!artifact->placeFile(context, artifact->parentFd(), ArtifactNames::cacheDirectory.characters(), 3))
        return;
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(Maintenance::compact(artifact->path(), 1, { }).outcome == Maintenance::Outcome::NoArtifact);
    JITCACHE_CHECK(!artifact->exists(ArtifactNames::lockFile.characters()));
}

// While a ProducerLock of the test process holds the lock, every call is Busy and changes nothing.
JITCACHE_TEST(maintenanceIsBusyWhileAProducerLockIsHeld, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
    if (!body || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
        return;

    {
        auto held = ProducerLock::tryAcquire(artifact->parentFd());
        if (!held) {
            JITCACHE_FAIL("cannot take the producer lock"_s);
            return;
        }
        JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0, { }).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(Maintenance::compact(artifact->path(), 0.5, { }).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(compactAnswering(*artifact, 1, Maintenance::Answer::Yes).outcome == Maintenance::Outcome::Busy);
        JITCACHE_CHECK(artifact->temporariesInCache() == 1);
        JITCACHE_CHECK(artifact->holdsBody(body->key) && artifact->exists("cache/header"));
    }

    Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
    JITCACHE_CHECK(report.outcome == Maintenance::Outcome::Done && report.temporariesRemoved == 1);
}

// A header that fails its checks stops compact after the cleanup that names alone allow: the temporaries go, the pack and
// its bodies stay, and compact returns Failed with bad-header and no plan.
JITCACHE_TEST(maintenanceBadHeaderStopsCompactAfterItsCleanup, No)
{
    HookScope hooks;
    ArtifactHeaderBytes laterLayout = maintenanceHeader();
    laterLayout.bytes[8] = 3; // layout version 3: incompatible, the rest unread
    Vector<uint8_t> garbage = filler(64, 0x42);

    for (bool corrupt : { true, false }) {
        String label = corrupt ? "a corrupt header"_s : "an incompatible header"_s;
        auto artifact = MaintenanceArtifact::create(context);
        if (!artifact)
            return;
        auto body = artifact->commit(context, maintenanceKey(1), 500, 0, 64);
        if (!body || !artifact->placeTemporary(context, TemporaryKind::Pack, 10))
            return;
        if (!artifact->writeHeader(context, corrupt ? garbage.span() : laterLayout.span()))
            return;

        // The pack is read against the header's digest, so its bytes past the valid end stay too.
        if (!artifact->appendGarbage(context, 77))
            return;
        uint64_t packBytes = artifact->packFileSize();

        Maintenance::Report report = Maintenance::compact(artifact->path(), 0, { });
        if (checkOutcome(context, label, report, Maintenance::Outcome::Failed)) {
            JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "bad-header"_s) == 1);
            JITCACHE_CHECK(!report.plan && report.temporariesRemoved == 1 && report.bytesReclaimed == 10 && !report.bodiesEvicted);
        }
        JITCACHE_CHECK(!artifact->temporariesInCache() && artifact->packFileSize() == packBytes);
        JITCACHE_CHECK(artifact->holdsBody(body->key) && artifact->exists("cache/header"));
    }
}

// A pack of another layout, which compact meets when it runs under another binary than the producer's, is incompatible
// and never damaged: compact stops after the cleanup that names alone allow, with incompatible-pack and no plan, as it does
// for an incompatible header, so no ratio deletes the bodies another binary can still read.
JITCACHE_TEST(maintenanceOtherPackLayoutStopsCompactAfterItsCleanup, No)
{
    HookScope hooks;
    auto artifact = MaintenanceArtifact::create(context);
    if (!artifact)
        return;
    auto packed = commitBodies(context, *artifact, 2);
    if (!packed || !artifact->placeTemporary(context, TemporaryKind::Pack, 10) || !artifact->setPackLayoutVersion(context, 1))
        return;
    uint64_t packBytes = artifact->packFileSize();

    Maintenance::Report report = compactAnswering(*artifact, 0.5, Maintenance::Answer::Yes);
    if (checkOutcome(context, "compact 0.5 --yes"_s, report, Maintenance::Outcome::Failed)) {
        JITCACHE_CHECK(report.diagnostics.size() == 1 && countDiagnostics(report, "incompatible-pack"_s) == 1);
        JITCACHE_CHECK(!report.plan && report.temporariesRemoved == 1 && !report.bodiesEvicted);
    }
    JITCACHE_CHECK(!artifact->temporariesInCache() && artifact->packFileSize() == packBytes);
    JITCACHE_CHECK(artifact->exists("cache/header") && artifact->exists("cache/pack"));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
