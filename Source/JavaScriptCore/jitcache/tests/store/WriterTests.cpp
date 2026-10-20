#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheParameters.h"
#include "TestArtifact.h"
#include <atomic>
#include <wtf/DataLog.h>
#include <wtf/Threading.h>

// Tests of ArtifactWriter (store/ArtifactWriter.h and .cpp): commits appended to the pack and published by their root,
// faults that leave nothing published, tombstones, full indexes once the delta chain is long, and compactions, with a
// consumer reading on another thread meanwhile. None needs a VM. A kill point ends its process, so producer-kill.js and
// kill-points.js test each point instead.

namespace JSC::JITCache::Tests {

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
    auto before = pack->validEnd();
    auto committed = commitBody(pack->writer(), testKey(1), 3);
    JITCACHE_CHECK(!!committed);
    if (!committed || !before)
        return;
    // The record at the old valid end, then a delta of one entry whose trailer ends the new valid bytes.
    auto after = pack->validEnd();
    JITCACHE_CHECK(after == *before + *packRecordExtent(committed->fileSize) + packIndexBlockBytes(1));
    JITCACHE_CHECK(after == pack->packFileSize());
    JITCACHE_CHECK(after && pack->latestBlock() == *after - packIndexBlockBytes(1));
    JITCACHE_CHECK(pack->artifact().token(testKey(1)) == committed->version);
    BodyOpen opened = pack->artifact().open(testKey(1), ValidationMode::Full);
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
        auto committed = pack->writer().commit({ testKey(2), 500, 9, 1 }, CommitSections { sources.span() });
        JITCACHE_CHECK(!!committed);
        BodyOpen opened = pack->artifact().open(testKey(2), ValidationMode::Full);
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
    auto failed = pack->writer().commit({ testKey(1), 500, 4, 1 }, CommitSections { sources.span() });
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::section);
    JITCACHE_CHECK(!pack->artifact().token(testKey(1)));
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 4));
}

// Each injectable step fails as its system call would, publishes nothing, and leaves the next commit to succeed. The
// root's store makes no system call, so nothing past the index block can fail before it.
JITCACHE_TEST(writerFaultsPublishNothing, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto first = commitBody(pack->writer(), testKey(1), 1);
    JITCACHE_CHECK(!!first);
    uint64_t commits = 1;
    for (ASCIILiteral check : { WriterChecks::openPack, WriterChecks::write, WriterChecks::reread }) {
        auto before = pack->validEnd();
        pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { check, ++commits });
        auto failed = commitBody(pack->writer(), testKey(1), 50);
        JITCACHE_CHECK(!failed && failed.error().check == check);
        JITCACHE_CHECK(pack->validEnd() == before);
        JITCACHE_CHECK(first && pack->artifact().token(testKey(1)) == first->version);
    }
    pack->writer().setFaultForTesting(std::nullopt);
    auto next = commitBody(pack->writer(), testKey(1), 51);
    JITCACHE_CHECK(next && pack->artifact().token(testKey(1)) == next->version);
}

JITCACHE_TEST(writerRefusedStagingChargeWritesNothing, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context, 63, 64);
    if (!pack)
        return;
    auto before = pack->validEnd();
    auto failed = commitBody(pack->writer(), testKey(1), 1);
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::budgetLimit);
    JITCACHE_CHECK(pack->validEnd() == before);
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
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 1));
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(2), 2));
    JITCACHE_CHECK(other->token(testKey(1)));
    JITCACHE_CHECK(!pack->writer().remove(testKey(1)));
    JITCACHE_CHECK(!pack->writer().remove(testKey(3))); // a key without a body is left alone
    JITCACHE_CHECK(!other->token(testKey(1)) && other->token(testKey(2)));
    JITCACHE_CHECK(other->open(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    JITCACHE_CHECK(fresh && fresh->indexedBodies() == 1);
}

// Any byte past the live records and one full index compacts, so the index blocks that new keys' commits appended are
// enough. The compacted pack is dense and the next call leaves it alone, until superseded versions make it compact again.
JITCACHE_TEST(writerCompactsUnlessThePackIsDense, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto empty = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(empty && !*empty);
    for (uint16_t seed = 1; seed <= 3; ++seed)
        JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed)));
    auto compacted = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 3);
    JITCACHE_CHECK(pack->artifact().token(testKey(1))); // follows the root to the compacted pack
    auto statistics = pack->artifact().statistics();
    JITCACHE_CHECK(statistics.packBytes - statistics.liveBytes == packIndexBlockBytes(3));
    auto dense = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(dense && !*dense);

    for (uint16_t seed = 1; seed <= 3; ++seed)
        JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed + 40)));
    uint64_t tokens[3];
    for (uint16_t seed = 1; seed <= 3; ++seed)
        tokens[seed - 1] = pack->artifact().token(testKey(seed));
    auto recompacted = pack->writer().compactIfNeeded(true);
    JITCACHE_CHECK(recompacted && *recompacted && (*recompacted)->bodies == 3 && (*recompacted)->packBytes < (*recompacted)->replacedPackBytes);
    for (uint16_t seed = 1; seed <= 3; ++seed) {
        JITCACHE_CHECK(pack->artifact().token(testKey(seed)) == tokens[seed - 1]);
        BodyOpen opened = pack->artifact().open(testKey(seed), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, static_cast<uint8_t>(seed + 40)));
    }
    // The next commit appends to the new pack.
    auto after = commitBody(pack->writer(), testKey(4), 4);
    JITCACHE_CHECK(after && pack->artifact().token(testKey(4)) == after->version);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    JITCACHE_CHECK(fresh && fresh->indexedBodies() == 4);
}

JITCACHE_TEST(writerCompactionFaultsKeepTheOldPack, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 1));
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 2));
    uint64_t compactions = 0;
    for (ASCIILiteral check : { CompactionChecks::create, CompactionChecks::write, CompactionChecks::reread, CompactionChecks::publish }) {
        auto before = pack->validEnd();
        pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { check, ++compactions });
        auto failed = pack->writer().compactIfNeeded(false);
        JITCACHE_CHECK(!failed && failed.error().check == check && failed.error().kind == CompactionFailure::Kind::Storage);
        JITCACHE_CHECK(pack->validEnd() == before);
        Vector<String> temporaries;
        listDirectory(pack->cacheFd(), [&](std::span<const char> name, uint64_t) {
            if (temporaryKindOfFileName(name))
                temporaries.append(String::fromLatin1(name));
        });
        JITCACHE_CHECK(temporaries.isEmpty());
    }
    pack->writer().setFaultForTesting(std::nullopt);
    auto committed = commitBody(pack->writer(), testKey(2), 3);
    JITCACHE_CHECK(!!committed);
    auto compacted = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 2);
}

// A ConsumerProducer chain: each generation recaptures every key, and the end of each delta compacts the dead bytes
// those commits left, so every generation ends with a dense pack.
JITCACHE_TEST(writerChainGrowsDeadBytesAndCompacts, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    uint64_t compactions = 0;
    for (uint8_t generation = 0; generation < 8; ++generation) {
        for (uint16_t seed = 1; seed <= 12; ++seed)
            JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed + generation)));
        auto compacted = pack->writer().compactIfNeeded(false);
        JITCACHE_CHECK(!!compacted);
        if (compacted && *compacted)
            ++compactions;
        auto statistics = pack->artifact().statistics();
        JITCACHE_CHECK(statistics.packBytes - statistics.liveBytes == packIndexBlockBytes(12));
    }
    JITCACHE_CHECK(compactions == 8);
    RefPtr<OpenedArtifact> fresh = pack->takeAnother(context);
    if (!fresh)
        return;
    for (uint16_t seed = 1; seed <= 12; ++seed) {
        BodyOpen opened = fresh->open(testKey(seed), ValidationMode::Full);
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
                BodyOpen body = consumer->open(testKey(seed), ValidationMode::Full);
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
    for (unsigned round = 0; round < 32; ++round) {
        for (uint16_t seed = 1; seed <= keys; ++seed)
            JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed + keys * round)));
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
        BodyOpen body = consumer->open(testKey(seed), ValidationMode::Full);
        JITCACHE_CHECK(body.body && holdsSections(*body.body, static_cast<uint8_t>(seed + keys * 31)));
    }
}

// A reader loads the root on another thread, through its own mapping, while the producer commits deltas and full
// indexes: every valid end it loads ends with an index block whose trailer leads to it, the whole index that block heads
// reads back, and each record it lists repeats its entry, so no valid end is ever seen before the bytes below it.
JITCACHE_TEST(writerReaderNeverSeesARootBeforeItsBlock, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto mapped = MappedPack::map(pack->cacheFd(), std::span<const uint8_t, 16> { headerDigest() }, 64 << 20);
    if (!mapped) {
        JITCACHE_FAIL(makeString("the reader cannot map the pack: "_s, mapped.error().error ? errnoText(mapped.error().error) : String { mapped.error().packRejection }));
        return;
    }
    Ref<MappedPack> view = mapped->pack.copyRef();
    constexpr unsigned commits = 2 * packDeltaChainBound + 8;
    std::atomic<bool> done { false };
    std::atomic<uint64_t> loaded { 0 }; // distinct valid ends read
    std::atomic<uint64_t> lastLoaded { 0 };
    std::atomic<uint64_t> broken { 0 };
    RefPtr<Thread> reader = Thread::create("jitcache root reader"_s, [&] {
        uint64_t last = 0;
        while (true) {
            // done is loaded before the root, so the root loaded after the writer finished is its last one.
            bool finished = done.load();
            uint64_t validEnd = view->validEnd();
            if (validEnd != last) {
                PackIndex index;
                uint64_t liveBytes = packPrefixBytes;
                auto read = readPackIndex(validEnd, std::nullopt, index, liveBytes, [&](uint64_t offset, uint64_t size) {
                    return view->bytes(offset, size);
                });
                bool sound = read && validEnd > last && read->reachedFull;
                for (auto& entry : index.values()) {
                    auto header = decodePackRecordHeader(view->bytes(entry.offset, packRecordHeaderBytes).first<packRecordHeaderBytes>());
                    sound = sound && header && header->key == entry.key && header->size == entry.size && header->version == entry.version;
                }
                if (!sound)
                    broken.fetch_add(1);
                last = validEnd;
                lastLoaded.store(validEnd);
                loaded.fetch_add(1);
            }
            if (finished)
                break;
        }
    });
    // The reader has read the empty pack's root before the first commit.
    while (!loaded.load())
        Thread::yield();
    for (unsigned n = 0; n < commits; ++n)
        JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(static_cast<uint16_t>(n % 8 + 1)), static_cast<uint8_t>(n)));
    done.store(true);
    reader->waitForCompletion();
    JITCACHE_CHECK(!broken.load());
    JITCACHE_CHECK(loaded.load() >= 2);
    JITCACHE_CHECK(lastLoaded.load() == pack->validEnd());
    dataLogLn("writerReaderNeverSeesARootBeforeItsBlock: the reader loaded ", loaded.load(), " valid ends over ", commits, " commits");
}

JITCACHE_TEST(writerRewriteSectionResealsTheBody, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto committed = commitBody(pack->writer(), testKey(1), 8);
    std::array<uint8_t, 2> bytes { 0xAB, 0xCD };
    auto rewritten = pack->writer().rewriteSection(testKey(1), SectionKind::UCBCore, 3, bytes);
    JITCACHE_CHECK(rewritten && committed && rewritten->version != committed->version);
    BodyOpen opened = pack->artifact().open(testKey(1), ValidationMode::Full);
    if (!opened.body) {
        JITCACHE_FAIL("the rewritten body does not open"_s);
        return;
    }
    auto core = opened.body->section(SectionKind::UCBCore);
    JITCACHE_CHECK(core.size() > 4 && core[3] == 0xAB && core[4] == 0xCD);
    auto missing = pack->writer().rewriteSection(testKey(9), SectionKind::UCBCore, 0, bytes);
    JITCACHE_CHECK(!missing && missing.error().check == WriterChecks::rewrite);
}

// The rewrite copies every section the body holds: a module's body of either tier keeps its ucb.analysis through a rewrite
// of another section, and a rewrite reaches ucb.analysis itself. A kind the body does not hold, ucb.analysis in a body
// without one or a section of a tier above the body's, fails at writer.rewrite and writes nothing.
JITCACHE_TEST(writerRewriteSectionCopiesEverySectionTheBodyHolds, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    constexpr size_t analysisBytes = 11;
    const std::array<uint8_t, 2> bytes { 0xAB, 0xCD };

    // The rewrite fails at writer.rewrite, and the pack and the key's token stay as they were.
    auto checkRefused = [&](ASCIILiteral label, const BodyKey& key, SectionKind kind) {
        auto validEnd = pack->validEnd();
        uint64_t token = pack->artifact().token(key);
        auto refused = pack->writer().rewriteSection(key, kind, 0, bytes);
        if (refused || refused.error().check != WriterChecks::rewrite)
            JITCACHE_FAIL(makeString(label, ": the rewrite of "_s, sectionKindDescription(kind).name, " did not fail at writer.rewrite"_s));
        JITCACHE_CHECK(validEnd && pack->validEnd() == validEnd && pack->artifact().token(key) == token);
    };

    for (uint8_t tier : { uint8_t { 0 }, uint8_t { 1 } }) {
        BodyKey moduleKey = testKey(static_cast<uint16_t>(1 + tier), IdentityKind::Module);
        uint8_t seed = static_cast<uint8_t>(20 + tier);
        uint32_t progress = tier ? 9 : 0;
        // The sections of the tier and ucb.analysis, as the body holds them.
        auto body = bodySections(seed, 1, analysisBytes);
        std::array<Vector<uint8_t>, numberOfSectionKinds> expected;
        Vector<SectionSource> sources;
        for (auto& source : body->sources) {
            if (!isSectionRequired(source.kind, tier) && source.kind != SectionKind::UCBAnalysis)
                continue;
            sources.append(source);
            expected[static_cast<size_t>(source.kind)] = body->bytes[static_cast<size_t>(source.kind)];
        }
        auto committed = pack->writer().commit({ moduleKey, 500, progress, tier }, CommitSections { sources.span() });
        if (!committed) {
            JITCACHE_FAIL(makeString("tier "_s, static_cast<unsigned>(tier), ": the module's body did not commit, at "_s, committed.error().check));
            return;
        }

        // Overwrites the bytes at offset in the kind's section and reopens the body: every section it held, the rewritten
        // one changed, under a fresh commit identifier and the body's own tier, L and P.
        uint64_t version = committed->version;
        auto rewriteAndCheck = [&](SectionKind kind, size_t offset) {
            ASCIILiteral sectionName = sectionKindDescription(kind).name;
            auto rewritten = pack->writer().rewriteSection(moduleKey, kind, offset, bytes);
            if (!rewritten) {
                JITCACHE_FAIL(makeString("tier "_s, static_cast<unsigned>(tier), ": the rewrite of "_s, sectionName, " failed at "_s, rewritten.error().check, ": "_s, rewritten.error().detail));
                return false;
            }
            JITCACHE_CHECK(rewritten->version != version);
            version = rewritten->version;
            memcpySpan(expected[static_cast<size_t>(kind)].mutableSpan().subspan(offset, bytes.size()), std::span { bytes });
            BodyOpen opened = pack->artifact().open(moduleKey, ValidationMode::Full);
            if (!opened.body) {
                JITCACHE_FAIL(makeString("tier "_s, static_cast<unsigned>(tier), ": the body does not open after the rewrite of "_s, sectionName));
                return false;
            }
            for (size_t index = 0; index < numberOfSectionKinds; ++index) {
                auto sectionKind = static_cast<SectionKind>(index);
                if (!equalSpans(opened.body->section(sectionKind), expected[index].span()))
                    JITCACHE_FAIL(makeString("tier "_s, static_cast<unsigned>(tier), ": after the rewrite of "_s, sectionName, ", "_s, sectionKindDescription(sectionKind).name, " is not the section expected"_s));
            }
            JITCACHE_CHECK(opened.body->version() == version && opened.body->highestTier() == tier);
            JITCACHE_CHECK(opened.body->llintThreshold() == 500 && opened.body->counterProgress() == progress);
            return true;
        };
        if (!rewriteAndCheck(SectionKind::UCBCore, 3) || !rewriteAndCheck(SectionKind::UCBAnalysis, analysisBytes - bytes.size()))
            return;
        if (!tier)
            checkRefused("a module's body of tier 0"_s, moduleKey, SectionKind::CBStateBaseline);
    }

    // ucb.analysis in a program's body, and in a module's body whose capture built none.
    for (auto identityKind : { IdentityKind::Program, IdentityKind::Module }) {
        BodyKey key = testKey(10, identityKind);
        if (!commitBody(pack->writer(), key, 30)) {
            JITCACHE_FAIL("a body without ucb.analysis did not commit"_s);
            return;
        }
        checkRefused(identityKind == IdentityKind::Module ? "a module's body without ucb.analysis"_s : "a program's body"_s, key, SectionKind::UCBAnalysis);
    }
}

JITCACHE_TEST(writerHookNames, No)
{
    for (auto point : { ArtifactWriter::KillPoint::BeforeAppend, ArtifactWriter::KillPoint::AfterRoot, ArtifactWriter::KillPoint::CompactionAfterRename })
        JITCACHE_CHECK(ArtifactWriter::killPointNamed(StringView { ArtifactWriter::name(point) }) == point);
    JITCACHE_CHECK(ArtifactWriter::faultCheckNamed("compaction.reread"_s) == CompactionChecks::reread);
    JITCACHE_CHECK(!ArtifactWriter::faultCheckNamed("writer.create"_s));
    JITCACHE_CHECK(!ArtifactWriter::faultCheckNamed("writer.publish"_s)); // the root's store cannot fail
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
