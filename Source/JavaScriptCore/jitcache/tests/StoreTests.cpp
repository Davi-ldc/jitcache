#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheParameters.h"
#include "PackTestSupport.h"
#include <wtf/DataLog.h>
#include <wtf/MonotonicTime.h>

// The store: the pack's byte layouts, the producer lock, and an opened artifact that follows the root, borrows bodies
// from its mapping and survives compactions. None needs a VM; a second OpenedArtifact built with registry sharing off
// stands for another process's view of the same directory.

namespace JSC::JITCache::Tests {

using namespace PackTestSupport;

namespace StoreTestsInternal {

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

} // namespace StoreTestsInternal

using namespace StoreTestsInternal;

JITCACHE_TEST(storeNames, No)
{
    for (auto kind : { TemporaryKind::Header, TemporaryKind::Pack }) {
        TemporaryFileName name = temporaryFileName(kind);
        auto characters = unsafeSpan(name.data());
        JITCACHE_CHECK(temporaryKindOfFileName(characters) == kind);
    }
    JITCACHE_CHECK(!temporaryKindOfFileName(std::span { "pack" }.first(4)));
    BodyKey bodyKey = key(7);
    auto digits = bodyKeyHexDigits(bodyKey);
    JITCACHE_CHECK(bodyKeyFromHex(digits) == bodyKey);
    digits[0] = 'G';
    JITCACHE_CHECK(!bodyKeyFromHex(digits));
}

JITCACHE_TEST(storePackLayoutsRoundTrip, No)
{
    PackPrefix prefix { headerDigest(), 0x1234 };
    auto prefixBytes = encodePackPrefix(prefix);
    auto decoded = decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { headerDigest() });
    JITCACHE_CHECK(decoded && decoded->identifier == 0x1234);
    HeaderDigest other = headerDigest();
    other[0] ^= 1;
    JITCACHE_CHECK(!decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { other }));
    prefixBytes[20] ^= 1;
    JITCACHE_CHECK(!decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { headerDigest() }));

    PackRoot root { 3, 4096, 1024 };
    auto rootBytes = encodePackRoot(root);
    JITCACHE_CHECK(decodePackRoot(rootBytes) == root);
    rootBytes[9] ^= 1; // a torn root fails its CRC
    JITCACHE_CHECK(!decodePackRoot(rootBytes));
    JITCACHE_CHECK(!decodePackRoot(encodePackRoot({ 0, 4096, 1024 }))); // an empty slot
    JITCACHE_CHECK(!decodePackRoot(encodePackRoot({ 1, 4096, 4090 }))); // an index header past the valid end

    PackRecordHeader record { key(1), 777, 99 };
    auto recordBytes = encodePackRecordHeader(record);
    auto decodedRecord = decodePackRecordHeader(recordBytes);
    JITCACHE_CHECK(decodedRecord && decodedRecord->key == record.key && decodedRecord->size == 777 && decodedRecord->version == 99);
    recordBytes[41] ^= 1;
    JITCACHE_CHECK(!decodePackRecordHeader(recordBytes));

    PackIndexEntry entry { key(2), 512, 100, 5, 500, 7, 1 };
    std::array<uint8_t, packIndexEntryBytes> entryBytes { };
    encodePackIndexEntry(entry, entryBytes);
    JITCACHE_CHECK(decodePackIndexEntry(entryBytes) == entry);
    PackIndexEntry tombstone;
    tombstone.key = key(2);
    encodePackIndexEntry(tombstone, entryBytes);
    auto decodedTombstone = decodePackIndexEntry(entryBytes);
    JITCACHE_CHECK(decodedTombstone && decodedTombstone->isTombstone());
    entryBytes[56] = 1; // a tombstone with a commit identifier
    JITCACHE_CHECK(!decodePackIndexEntry(entryBytes));

    auto blockHeader = encodePackIndexBlockHeader({ PackIndexKind::Delta, 256, 0 }, { });
    auto block = decodePackIndexBlockHeader(blockHeader);
    JITCACHE_CHECK(block && block->kind == PackIndexKind::Delta && block->previous == 256);
    JITCACHE_CHECK(!checkPackIndexBlockCRC(blockHeader, { }));
    JITCACHE_CHECK(!decodePackIndexBlockHeader(encodePackIndexBlockHeader({ PackIndexKind::Full, 256, 0 }, { })));
    JITCACHE_CHECK(packRecordExtent(1) == 72u && packRecordExtent(8) == 72u && packRecordExtent(9) == 80u);
}

JITCACHE_TEST(storeProducerLockIsExclusive, No)
{
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto second = ProducerLock::tryAcquire(pack->parentFd());
    JITCACHE_CHECK(!second && second.error().busy);
}

JITCACHE_TEST(storeEmptyPackListsNothing, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    OpenedArtifact& artifact = pack->artifact();
    JITCACHE_CHECK(!artifact.indexedBodies());
    JITCACHE_CHECK(!artifact.token(key(1)));
    JITCACHE_CHECK(artifact.open(key(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(artifact.readSavedSummaries(key(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(!packHoldsBodies(pack->cacheFd()));
    auto statistics = artifact.statistics();
    JITCACHE_CHECK(statistics.generation == 1 && statistics.packBytes == packPrefixBytes + packIndexBlockHeaderBytes);
}

// Another process's view follows the root: a commit is invisible until its root, and visible after it without a
// system call.
JITCACHE_TEST(storeOtherViewFollowsTheRoot, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    auto first = commitBody(pack->writer(), key(1), 11);
    JITCACHE_CHECK(!!first);
    if (!first)
        return;
    JITCACHE_CHECK(other->token(key(1)) == first->version);
    BodyOpen opened = other->open(key(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Found && opened.body && holdsSections(*opened.body, 11));
    JITCACHE_CHECK(opened.bodyBytes == first->fileSize);

    // A recapture appends a new version; the newest wins, and the body opened before keeps its bytes.
    auto second = commitBody(pack->writer(), key(1), 12);
    JITCACHE_CHECK(second && second->version != first->version);
    JITCACHE_CHECK(other->token(key(1)) == second->version);
    JITCACHE_CHECK(opened.body && holdsSections(*opened.body, 11));
    BodyOpen reopened = other->open(key(1), ValidationMode::Integrity);
    JITCACHE_CHECK(reopened.body && holdsSections(*reopened.body, 12));
    auto statistics = other->statistics();
    JITCACHE_CHECK(statistics.bodies == 1 && statistics.liveBytes < statistics.packBytes);
}

// A root half written fails its CRC, and readers keep the other slot.
JITCACHE_TEST(storeTornRootKeepsTheOtherSlot, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto first = commitBody(pack->writer(), key(1), 21);
    if (!first) {
        JITCACHE_FAIL("the commit failed"_s);
        return;
    }
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    auto root = pack->root();
    if (!root) {
        JITCACHE_FAIL("the pack has no root"_s);
        return;
    }
    // The next generation's slot, with a valid-looking generation and a wrong CRC.
    auto torn = encodePackRoot({ root->generation + 1, root->validEnd + 4096, root->indexOffset });
    torn[60 - 4] ^= 0xFF;
    JITCACHE_CHECK(pack->writePackAt(packRootSlotOffset(root->generation + 1), torn));
    JITCACHE_CHECK(other->token(key(1)) == first->version);
    JITCACHE_CHECK(other->open(key(1), ValidationMode::Full).outcome == StoreOutcome::Found);
    JITCACHE_CHECK(other->statistics().generation == root->generation);
}

// Bytes a commit wrote past the valid end before it stopped are invisible, and the next commit overwrites them.
JITCACHE_TEST(storeBytesPastTheValidEndAreInvisible, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { WriterChecks::publish, 1 });
    auto failed = commitBody(pack->writer(), key(1), 31);
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::publish);
    JITCACHE_CHECK(!pack->artifact().token(key(1)));
    uint64_t sizeAfterFailure = pack->packFileSize();
    JITCACHE_CHECK(sizeAfterFailure > pack->root()->validEnd);
    pack->writer().setFaultForTesting(std::nullopt);
    auto committed = commitBody(pack->writer(), key(2), 32);
    JITCACHE_CHECK(!!committed);
    JITCACHE_CHECK(pack->packFileSize() == sizeAfterFailure);
    JITCACHE_CHECK(!pack->artifact().token(key(1)) && pack->artifact().token(key(2)));
}

JITCACHE_TEST(storeOpenRejectsDamagedBodies, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto committed = commitBody(pack->writer(), key(1), 41);
    if (!committed) {
        JITCACHE_FAIL("the commit failed"_s);
        return;
    }
    // The body's last byte belongs to its last section, which both modes checksum.
    auto root = pack->root();
    uint64_t bodyEnd = packPrefixBytes + packIndexBlockHeaderBytes + packRecordHeaderBytes + committed->fileSize;
    std::array<uint8_t, 1> damage { 0xEE };
    JITCACHE_CHECK(root && pack->writePackAt(bodyEnd - 1, damage));
    for (auto mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        BodyOpen opened = pack->artifact().open(key(1), mode);
        JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::checksum);
    }
}

JITCACHE_TEST(storeDamagedIndexMakesReadsInvalid, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    auto first = commitBody(pack->writer(), key(1), 51);
    JITCACHE_CHECK(other->token(key(1)) == first->version);
    // The second commit's delta is damaged before the other view applies it.
    auto second = commitBody(pack->writer(), key(2), 52);
    auto root = pack->root();
    if (!second || !root) {
        JITCACHE_FAIL("the second commit failed"_s);
        return;
    }
    std::array<uint8_t, 1> damage { 0x5A };
    JITCACHE_CHECK(pack->writePackAt(root->indexOffset + packIndexBlockHeaderBytes + 3, damage));
    BodyOpen opened = other->open(key(2), ValidationMode::Integrity);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
    JITCACHE_CHECK(other->open(key(1), ValidationMode::Integrity).outcome == StoreOutcome::Invalid);
    // A new reader rejects the pack at its start.
    StoreTesting::setRegistrySharing(false);
    auto taken = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), headerBytes());
    StoreTesting::setRegistrySharing(true);
    JITCACHE_CHECK(!taken && !taken.error().error && !taken.error().packRejection.isNull());
}

JITCACHE_TEST(storeScoringReadFollowsTheRoot, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    auto committed = commitBody(pack->writer(), key(1), 61);
    SavedSummaryRead read = other->readSavedSummaries(key(1), ValidationMode::Full);
    JITCACHE_CHECK(read.outcome == StoreOutcome::Found && read.summaries && committed);
    if (!read.summaries || !committed)
        return;
    auto expected = bodySections(61);
    JITCACHE_CHECK(read.summaries->version() == committed->version && read.summaries->highestTier() == 1);
    JITCACHE_CHECK(equalSpans(read.summaries->ucbFeedback(), expected->bytes[static_cast<size_t>(SectionKind::UCBFeedback)].span()));
    JITCACHE_CHECK(equalSpans(read.summaries->cbSummary(), expected->bytes[static_cast<size_t>(SectionKind::CBSummaryBaseline)].span()));
    JITCACHE_CHECK(equalSpans(read.summaries->ics(), expected->bytes[static_cast<size_t>(SectionKind::ICsBaseline)].span()));

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::ReadSavedSummaries, EMFILE, std::nullopt });
    JITCACHE_CHECK(other->readSavedSummaries(key(1), ValidationMode::Full).outcome == StoreOutcome::Unavailable);
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, 1 });
    BodyOpen opened = other->open(key(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.error == EIO);
    JITCACHE_CHECK(other->open(key(1), ValidationMode::Full).outcome == StoreOutcome::Found);
}

// A compaction retires the old pack and renames a new one over it: another view reopens the path at its next read, each
// key keeps its token, and a body opened before keeps reading the old mapping.
JITCACHE_TEST(storeRetiredPackReopensAndBodiesOutliveIt, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    Vector<uint64_t> versions;
    for (uint16_t seed = 1; seed <= 6; ++seed) {
        auto committed = commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed));
        JITCACHE_CHECK(!!committed);
        versions.append(committed ? committed->version : 0);
    }
    // Five of the six keys recaptured leave more dead bytes than live ones.
    for (uint16_t seed = 1; seed <= 6; ++seed) {
        if (seed == 5)
            continue;
        auto committed = commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed + 100));
        versions[seed - 1] = committed ? committed->version : 0;
    }
    BodyOpen held = other->open(key(5), ValidationMode::Full);
    JITCACHE_CHECK(held.body && holdsSections(*held.body, 5));

    auto compacted = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 6);
    JITCACHE_CHECK(pack->root() && pack->root()->validEnd == (compacted && *compacted ? (*compacted)->packBytes : 0));
    for (uint16_t seed = 1; seed <= 6; ++seed) {
        JITCACHE_CHECK(other->token(key(seed)) == versions[seed - 1]);
        BodyOpen opened = other->open(key(seed), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, static_cast<uint8_t>(seed != 5 ? seed + 100 : seed)));
    }
    JITCACHE_CHECK(held.body && holdsSections(*held.body, 5));
    JITCACHE_CHECK(other->statistics().reopens == 1);
    JITCACHE_CHECK(pack->artifact().statistics().liveBytes + packIndexBlockHeaderBytes + 6 * packIndexEntryBytes == pack->root()->validEnd);
}

// A whole deletion retires the pack before it unlinks it: a view that meets the mark and finds no pack misses every key.
JITCACHE_TEST(storeDeletedPackMissesEveryKey, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    JITCACHE_CHECK(!!commitBody(pack->writer(), key(1), 71));
    JITCACHE_CHECK(!!other->token(key(1)));
    uint64_t retired = 1;
    JITCACHE_CHECK(pack->writePackAt(packRetiredOffset, asByteSpan(retired)));
    JITCACHE_CHECK(!unlinkat(pack->cacheFd(), ArtifactNames::pack.characters(), 0));
    JITCACHE_CHECK(!other->token(key(1)));
    JITCACHE_CHECK(other->open(key(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(!other->indexedBodies());
}

// The delta chain a reader walks when it opens the pack: the producer writes a full index once the chain reaches
// packDeltaChainBound, and a reader's build over the longest chain stays cheap.
JITCACHE_TEST(storeDeltaChainIsBounded, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    uint64_t longestBuild = 0;
    for (uint16_t seed = 1; seed <= packDeltaChainBound + 10; ++seed) {
        if (!commitBody(pack->writer(), key(seed), static_cast<uint8_t>(seed))) {
            JITCACHE_FAIL(makeString("commit "_s, seed, " failed"_s));
            return;
        }
        if (seed == packDeltaChainBound - 1 || seed == packDeltaChainBound + 10) {
            RefPtr<OpenedArtifact> other = pack->takeAnother(context);
            if (!other)
                return;
            auto statistics = other->statistics();
            JITCACHE_CHECK(statistics.bodies == seed);
            if (seed == packDeltaChainBound - 1) {
                longestBuild = statistics.buildNanoseconds;
                JITCACHE_CHECK(statistics.appliedBlocks == seed + 1u);
            } else
                JITCACHE_CHECK(statistics.appliedBlocks == 11);
        }
    }
    dataLogLn("storeDeltaChainIsBounded: a reader's build over ", packDeltaChainBound - 1, " deltas took ", longestBuild, " ns");
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
