#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "PackTestSupport.h"
#include <atomic>
#include <wtf/Threading.h>

// The writer: commits appended to the pack and published by their root, faults that leave nothing published, tombstones,
// full indexes once the delta chain is long, and compactions, with a consumer reading on another thread meanwhile. None
// needs a VM. A kill point ends its process, so producer-kill.js and kill-points.js test each point instead.

namespace JSC::JITCache::Tests {

using namespace PackTestSupport;

namespace WriterTestsInternal {

class HookScope {
    WTF_MAKE_NONCOPYABLE(HookScope);
public:
    HookScope() = default;
    ~HookScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
    }
};

// A section a streamed source hands the sink in chunks of chunkSize bytes, at most declaredSize of them reported.
struct StreamedSection {
    std::span<const uint8_t> bytes;
    size_t chunkSize;

    static bool stream(const void* object, const SectionSink& sink)
    {
        auto& section = *static_cast<const StreamedSection*>(object);
        auto rest = section.bytes;
        while (!rest.empty()) {
            size_t count = std::min(rest.size(), section.chunkSize);
            if (!sink(rest.first(count)))
                return false;
            rest = rest.subspan(count);
        }
        return true;
    }
};

} // namespace WriterTestsInternal

using namespace WriterTestsInternal;

JITCACHE_TEST(writerCommitPublishesTheSources, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto before = pack->root();
    auto committed = commitBody(pack->writer(), key(1), 3);
    JITCACHE_CHECK(!!committed);
    if (!committed || !before)
        return;
    auto after = pack->root();
    JITCACHE_CHECK(after && after->generation == before->generation + 1);
    JITCACHE_CHECK(after && after->validEnd == pack->packFileSize());
    JITCACHE_CHECK(pack->artifact().token(key(1)) == committed->version);
    BodyOpen opened = pack->artifact().open(key(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.body && holdsSections(*opened.body, 3) && opened.body->version() == committed->version);
    JITCACHE_CHECK(opened.body && opened.body->llintThreshold() == 500 && opened.body->counterProgress() == 3 && opened.body->highestTier() == 1);
    JITCACHE_CHECK(packHoldsBodies(pack->cacheFd()));
}

JITCACHE_TEST(writerStreamsThroughSmallStagingBuffers, No)
{
    HookScope hooks;
    for (size_t staging : { size_t { 1 }, size_t { 7 }, size_t { 64 }, size_t { 4096 } }) {
        auto pack = PackArtifact::create(context, 64 << 20, staging);
        if (!pack)
            return;
        auto body = bodySections(9, 5);
        std::array<StreamedSection, numberOfSectionKinds> streamed { };
        Vector<SectionSource> sources;
        for (auto& source : body->sources) {
            auto& section = streamed[static_cast<size_t>(source.kind)];
            section = { source.bytes, 3 };
            sources.append(SectionSource::streamed(source.kind, source.size, StreamedSection::stream, &section));
        }
        auto committed = pack->writer().commit({ key(2), 500, 9, 1 }, CommitSections { sources.span() });
        JITCACHE_CHECK(!!committed);
        BodyOpen opened = pack->artifact().open(key(2), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, 9, 5));
    }
}

JITCACHE_TEST(writerRejectsMiscountedSections, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto body = bodySections(4);
    Vector<SectionSource> sources = body->sources;
    sources[1].size += 1;
    auto failed = pack->writer().commit({ key(1), 500, 4, 1 }, CommitSections { sources.span() });
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::section);
    JITCACHE_CHECK(!pack->artifact().token(key(1)));
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(1), 4));
}

// Each injectable step fails as its system call would, publishes nothing, and leaves the next commit to succeed.
JITCACHE_TEST(writerFaultsPublishNothing, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto first = commitBody(pack->writer(), key(1), 1);
    JITCACHE_CHECK(!!first);
    uint64_t commits = 1;
    for (ASCIILiteral check : { WriterChecks::openPack, WriterChecks::write, WriterChecks::reread, WriterChecks::publish }) {
        auto before = pack->root();
        pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { check, ++commits });
        auto failed = commitBody(pack->writer(), key(1), 50);
        JITCACHE_CHECK(!failed && failed.error().check == check);
        JITCACHE_CHECK(pack->root() == before);
        JITCACHE_CHECK(first && pack->artifact().token(key(1)) == first->version);
    }
    pack->writer().setFaultForTesting(std::nullopt);
    auto next = commitBody(pack->writer(), key(1), 51);
    JITCACHE_CHECK(next && pack->artifact().token(key(1)) == next->version);
}

JITCACHE_TEST(writerRefusedStagingChargeWritesNothing, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context, 63, 64);
    if (!pack)
        return;
    auto before = pack->root();
    auto failed = commitBody(pack->writer(), key(1), 1);
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::budgetLimit);
    JITCACHE_CHECK(pack->root() == before);
}

JITCACHE_TEST(writerTombstoneRemovesTheKey, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(1), 1));
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(2), 2));
    JITCACHE_CHECK(other->token(key(1)));
    JITCACHE_CHECK(!pack->writer().remove(key(1)));
    JITCACHE_CHECK(!pack->writer().remove(key(3))); // a key without a body is left alone
    JITCACHE_CHECK(!other->token(key(1)) && other->token(key(2)));
    JITCACHE_CHECK(other->open(key(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    JITCACHE_CHECK(fresh && fresh->indexedBodies() == 1);
}

JITCACHE_TEST(writerCompactsOnceDeadBytesPassHalf, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    for (uint16_t seed = 1; seed <= 4; ++seed)
        JITCACHE_CHECK(!!commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed)));
    auto notYet = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(notYet && !*notYet);
    // Superseding every body leaves under half of the pack live.
    for (uint16_t seed = 1; seed <= 4; ++seed)
        JITCACHE_CHECK(!!commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed + 40)));
    auto statistics = pack->artifact().statistics();
    JITCACHE_CHECK(2 * (statistics.packBytes - statistics.liveBytes) > statistics.packBytes);
    uint64_t tokens[4];
    for (uint16_t seed = 1; seed <= 4; ++seed)
        tokens[seed - 1] = pack->artifact().token(key(seed));
    auto compacted = pack->writer().compactIfNeeded(true);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 4 && (*compacted)->packBytes < (*compacted)->replacedPackBytes);
    for (uint16_t seed = 1; seed <= 4; ++seed) {
        JITCACHE_CHECK(pack->artifact().token(key(seed)) == tokens[seed - 1]);
        BodyOpen opened = pack->artifact().open(key(seed), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, static_cast<uint8_t>(seed + 40)));
    }
    // The next commit appends to the new pack.
    auto after = commitBody(pack->writer(), key(5), 5);
    JITCACHE_CHECK(after && pack->artifact().token(key(5)) == after->version);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    JITCACHE_CHECK(fresh && fresh->indexedBodies() == 5);
}

JITCACHE_TEST(writerCompactionFaultsKeepTheOldPack, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(1), 1));
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(1), 2));
    pack->writer().setCompactsEagerlyForTesting(true);
    uint64_t compactions = 0;
    for (ASCIILiteral check : { CompactionChecks::create, CompactionChecks::write, CompactionChecks::reread, CompactionChecks::publish }) {
        auto before = pack->root();
        pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { check, ++compactions });
        auto failed = pack->writer().compactIfNeeded(false);
        JITCACHE_CHECK(!failed && failed.error().check == check && failed.error().kind == CompactionFailure::Kind::Storage);
        JITCACHE_CHECK(pack->root() == before);
        Vector<String> temporaries;
        listDirectory(pack->cacheFd(), [&](std::span<const char> name, uint64_t) {
            if (temporaryKindOfFileName(name))
                temporaries.append(String { name });
        });
        JITCACHE_CHECK(temporaries.isEmpty());
    }
    pack->writer().setFaultForTesting(std::nullopt);
    auto committed = commitBody(pack->writer(), key(2), 3);
    JITCACHE_CHECK(!!committed);
    auto compacted = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 2);
}

// A ConsumerProducer chain: each generation recaptures every key, the pack's dead bytes grow, and the end of each delta
// compacts once they pass half, so the pack stays within about twice its live bytes.
JITCACHE_TEST(writerChainGrowsDeadBytesAndCompacts, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    uint64_t compactions = 0;
    for (uint8_t generation = 0; generation < 8; ++generation) {
        for (uint16_t seed = 1; seed <= 12; ++seed)
            JITCACHE_CHECK(!!commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed + generation)));
        auto compacted = pack->writer().compactIfNeeded(false);
        JITCACHE_CHECK(!!compacted);
        if (compacted && *compacted)
            ++compactions;
        auto statistics = pack->artifact().statistics();
        JITCACHE_CHECK(statistics.packBytes <= 2 * statistics.liveBytes);
    }
    JITCACHE_CHECK(compactions >= 3);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    if (!fresh)
        return;
    for (uint16_t seed = 1; seed <= 12; ++seed) {
        BodyOpen opened = fresh->open(key(seed), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, static_cast<uint8_t>(seed + 7)));
    }
}

// A consumer imports on another thread while the producer commits and compacts: every body it opens is whole and holds
// a version the producer committed for its key, and it ends seeing the last ones.
JITCACHE_TEST(writerConsumerReadsWhileProducerCommitsAndCompacts, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> consumer = pack->takeAnother(context);
    if (!consumer)
        return;
    constexpr uint16_t keys = 8;
    std::atomic<bool> done { false };
    std::atomic<uint64_t> opened { 0 };
    std::atomic<uint64_t> broken { 0 };
    RefPtr<Thread> reader = Thread::create("jitcache reader"_s, [&] {
        while (!done.load()) {
            for (uint16_t seed = 1; seed <= keys; ++seed) {
                BodyOpen body = consumer->open(key(seed), ValidationMode::Full);
                if (body.outcome == StoreOutcome::Found) {
                    opened.fetch_add(1);
                    // Every committed version of a key holds the sections of one seed congruent to the key's.
                    bool whole = false;
                    for (unsigned round = 0; round < 32 && !whole; ++round)
                        whole = holdsSections(*body.body, static_cast<uint8_t>(seed + keys * round));
                    if (!whole)
                        broken.fetch_add(1);
                } else if (body.outcome != StoreOutcome::Absent)
                    broken.fetch_add(1);
            }
        }
    });
    pack->writer().setCompactsEagerlyForTesting(true);
    for (unsigned round = 0; round < 32; ++round) {
        for (uint16_t seed = 1; seed <= keys; ++seed)
            JITCACHE_CHECK(!!commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed + keys * round)));
        if (round % 4 == 3) {
            auto compacted = pack->writer().compactIfNeeded(false);
            JITCACHE_CHECK(compacted && *compacted);
        }
    }
    done.store(true);
    reader->waitForCompletion();
    JITCACHE_CHECK(!broken.load());
    JITCACHE_CHECK(opened.load() > 0);
    for (uint16_t seed = 1; seed <= keys; ++seed) {
        BodyOpen body = consumer->open(key(seed), ValidationMode::Full);
        JITCACHE_CHECK(body.body && holdsSections(*body.body, static_cast<uint8_t>(seed + keys * 31)));
    }
}

JITCACHE_TEST(writerRewriteSectionResealsTheBody, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto committed = commitBody(pack->writer(), key(1), 8);
    std::array<uint8_t, 2> bytes { 0xAB, 0xCD };
    auto rewritten = pack->writer().rewriteSection(key(1), SectionKind::UCBCore, 3, bytes);
    JITCACHE_CHECK(rewritten && committed && rewritten->version != committed->version);
    BodyOpen opened = pack->artifact().open(key(1), ValidationMode::Full);
    if (!opened.body) {
        JITCACHE_FAIL("the rewritten body does not open"_s);
        return;
    }
    auto core = opened.body->section(SectionKind::UCBCore);
    JITCACHE_CHECK(core.size() > 4 && core[3] == 0xAB && core[4] == 0xCD);
    auto missing = pack->writer().rewriteSection(key(9), SectionKind::UCBCore, 0, bytes);
    JITCACHE_CHECK(!missing && missing.error().check == WriterChecks::rewrite);
}

JITCACHE_TEST(writerHookNames, No)
{
    for (auto point : { ArtifactWriter::KillPoint::BeforeAppend, ArtifactWriter::KillPoint::AfterRoot, ArtifactWriter::KillPoint::CompactionAfterRename })
        JITCACHE_CHECK(ArtifactWriter::killPointNamed(StringView { ArtifactWriter::name(point) }) == point);
    JITCACHE_CHECK(ArtifactWriter::faultCheckNamed("compaction.reread"_s) == CompactionChecks::reread);
    JITCACHE_CHECK(!ArtifactWriter::faultCheckNamed("writer.create"_s));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
