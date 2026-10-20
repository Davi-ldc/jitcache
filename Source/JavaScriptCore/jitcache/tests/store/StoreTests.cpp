#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "JITCacheParameters.h"
#include "TestArtifact.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <wtf/DataLog.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Threading.h>
#include <wtf/threads/BinarySemaphore.h>

// Tests of ArtifactStore and ValidatedBody (store/ArtifactStore.h, store/ValidatedBody.h and their .cpp): the pack's byte
// layouts, the producer lock, an opened artifact that follows the root, borrows bodies from its mapping, reads one section
// of a body and survives compactions, the registry that shares it, and the bodies tests build with their lookups. None needs a VM; a second
// OpenedArtifact built with registry sharing off stands for another process's view of the same directory.

namespace JSC::JITCache::Tests {

namespace StoreTestsInternal {

static bool isEightByteAligned(std::span<const uint8_t> span)
{
    return !(reinterpret_cast<uintptr_t>(span.data()) % 8);
}

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
    BodyKey bodyKey = testKey(7);
    auto digits = bodyKeyHexDigits(bodyKey);
    JITCACHE_CHECK(bodyKeyFromHex(digits) == bodyKey);
    digits[0] = 'G';
    JITCACHE_CHECK(!bodyKeyFromHex(digits));
}

JITCACHE_TEST(storePackLayoutsRoundTrip, No)
{
    PackPrefix prefix { headerDigest(), 0x1234 };
    auto prefixBytes = encodePackPrefix(prefix, 4096);
    auto decoded = decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { headerDigest() });
    JITCACHE_CHECK(decoded && decoded->identifier == 0x1234);
    uint64_t root = 0;
    memcpySpan(asMutableByteSpan(root), std::span { prefixBytes }.subspan(packRootOffset, sizeof(root)));
    JITCACHE_CHECK(root == 4096);
    // The root and the retired word change after the file is created, so the prefix's checks leave them out.
    prefixBytes[packRootOffset] ^= 1;
    prefixBytes[packRetiredOffset] = 1;
    JITCACHE_CHECK(!!decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { headerDigest() }));
    HeaderDigest other = headerDigest();
    other[0] ^= 1;
    JITCACHE_CHECK(!decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { other }));
    prefixBytes[20] ^= 1;
    JITCACHE_CHECK(!decodePackPrefix(prefixBytes, std::span<const uint8_t, 16> { headerDigest() }));

    // A sealed block: its header and CRC over the entries in place, and the trailer giving its own offset.
    JITCACHE_CHECK(packIndexBlockBytes(0) == 40 && packIndexBlockBytes(2) == 200);
    std::array<uint8_t, packIndexBlockBytes(2)> blockBytes { };
    auto blockSpan = std::span { blockBytes };
    encodePackIndexEntry({ testKey(3), 64, 10, 1, 500, 0, 1 }, blockSpan.subspan(packIndexBlockHeaderBytes, packIndexEntryBytes));
    encodePackIndexEntry({ testKey(4), 144, 10, 2, 500, 0, 1 }, blockSpan.subspan(packIndexBlockHeaderBytes + packIndexEntryBytes, packIndexEntryBytes));
    sealPackIndexBlock({ PackIndexKind::Delta, 64, 2 }, 1024, blockSpan);
    auto sealedHeader = blockSpan.first<packIndexBlockHeaderBytes>();
    auto sealed = decodePackIndexBlockHeader(sealedHeader);
    JITCACHE_CHECK(sealed && sealed->kind == PackIndexKind::Delta && sealed->previous == 64 && sealed->count == 2);
    JITCACHE_CHECK(!checkPackIndexBlockCRC(sealedHeader, blockSpan.subspan(packIndexBlockHeaderBytes, 2 * packIndexEntryBytes)));
    JITCACHE_CHECK(decodePackIndexTrailer(blockSpan.last<packIndexTrailerBytes>()) == 1024);

    PackRecordHeader record { testKey(1), 777, 99 };
    auto recordBytes = encodePackRecordHeader(record);
    auto decodedRecord = decodePackRecordHeader(recordBytes);
    JITCACHE_CHECK(decodedRecord && decodedRecord->key == record.key && decodedRecord->size == 777 && decodedRecord->version == 99);
    recordBytes[41] ^= 1;
    JITCACHE_CHECK(!decodePackRecordHeader(recordBytes));

    PackIndexEntry entry { testKey(2), 512, 100, 5, 500, 7, 1 };
    std::array<uint8_t, packIndexEntryBytes> entryBytes { };
    encodePackIndexEntry(entry, entryBytes);
    JITCACHE_CHECK(decodePackIndexEntry(entryBytes) == entry);
    PackIndexEntry tombstone;
    tombstone.key = testKey(2);
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
    JITCACHE_CHECK(!artifact.token(testKey(1)));
    JITCACHE_CHECK(artifact.open(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(artifact.readSavedSummaries(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(!packHoldsBodies(pack->cacheFd()));
    auto statistics = artifact.statistics();
    JITCACHE_CHECK(statistics.packBytes == packPrefixBytes + packIndexBlockBytes(0));
    JITCACHE_CHECK(pack->validEnd() == statistics.packBytes && pack->latestBlock() == packPrefixBytes);
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
    auto first = commitBody(pack->writer(), testKey(1), 11);
    JITCACHE_CHECK(!!first);
    if (!first)
        return;
    JITCACHE_CHECK(other->token(testKey(1)) == first->version);
    BodyOpen opened = other->open(testKey(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Found && opened.body && holdsSections(*opened.body, 11));
    JITCACHE_CHECK(opened.bodyBytes == first->fileSize);

    // A recapture appends a new version; the newest wins, and the body opened before keeps its bytes.
    auto second = commitBody(pack->writer(), testKey(1), 12);
    JITCACHE_CHECK(second && second->version != first->version);
    JITCACHE_CHECK(other->token(testKey(1)) == second->version);
    JITCACHE_CHECK(opened.body && holdsSections(*opened.body, 11));
    BodyOpen reopened = other->open(testKey(1), ValidationMode::Integrity);
    JITCACHE_CHECK(reopened.body && holdsSections(*reopened.body, 12));
    auto statistics = other->statistics();
    JITCACHE_CHECK(statistics.bodies == 1 && statistics.liveBytes < statistics.packBytes);
}

// The trailer that ends the valid bytes leads a reader to the latest index block. A damaged one fails the reader's checks,
// for a view that follows the commit and for a reader that opens the pack afterwards, and no read finds a body through
// it. Each damage gets its own pack, since a view that met damage stays damaged.
JITCACHE_TEST(storeDamagedTrailerFailsWithoutMisreading, No)
{
    HookScope hooks;
    enum class Damage : uint8_t { EarlierBlock, Record, Prefix, Misaligned, ValidEnd, Largest };
    for (auto damage : { Damage::EarlierBlock, Damage::Record, Damage::Prefix, Damage::Misaligned, Damage::ValidEnd, Damage::Largest }) {
        auto pack = PackArtifact::create(context);
        if (!pack)
            return;
        auto first = commitBody(pack->writer(), testKey(1), 21);
        auto firstBlock = pack->latestBlock();
        RefPtr<OpenedArtifact> other = pack->takeAnother(context);
        uint64_t recordOffset = pack->validEnd().value_or(0);
        auto second = commitBody(pack->writer(), testKey(2), 22);
        auto validEnd = pack->validEnd();
        auto latestBlock = pack->latestBlock();
        if (!first || !second || !firstBlock || !other || !validEnd || !latestBlock || other->statistics().packBytes != recordOffset) {
            JITCACHE_FAIL("the two commits did not leave the pack this test damages"_s);
            return;
        }
        uint64_t trailer = 0;
        switch (damage) {
        case Damage::EarlierBlock: // a sound block that does not end at the valid end
            trailer = *firstBlock;
            break;
        case Damage::Record: // the second body's record
            trailer = recordOffset;
            break;
        case Damage::Prefix:
            trailer = 0;
            break;
        case Damage::Misaligned:
            trailer = *latestBlock + 4;
            break;
        case Damage::ValidEnd: // the trailer itself
            trailer = *validEnd - packIndexTrailerBytes;
            break;
        case Damage::Largest:
            trailer = std::numeric_limits<uint64_t>::max() - 7;
            break;
        }
        JITCACHE_CHECK(pack->writePackAt(*validEnd - packIndexTrailerBytes, asByteSpan(trailer)));
        for (uint16_t seed : { 2, 1 }) {
            BodyOpen opened = other->open(testKey(seed), ValidationMode::Full);
            JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
        }
        StoreTesting::setRegistrySharing(false);
        auto taken = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), headerBytes());
        StoreTesting::setRegistrySharing(true);
        JITCACHE_CHECK(!taken && !taken.error().error && !taken.error().packRejection.isNull());
    }
}

// The root carries no checksum. A root past the file fails when the pack is mapped, so no read runs past the file's end,
// and a root that cannot end a pack, or ends it inside a record, fails the walk. A root moved back to an earlier valid
// end is a consistent earlier pack to a new reader, and damage to a view that applied the later one.
JITCACHE_TEST(storeDamagedRootFailsWithoutMisreading, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto first = commitBody(pack->writer(), testKey(1), 23);
    auto firstEnd = pack->validEnd();
    auto second = commitBody(pack->writer(), testKey(2), 24);
    auto validEnd = pack->validEnd();
    if (!first || !second || !firstEnd || !validEnd) {
        JITCACHE_FAIL("the two commits did not leave the pack this test damages"_s);
        return;
    }
    JITCACHE_CHECK(*validEnd == pack->packFileSize());
    auto rejects = [&](uint64_t root) {
        if (!pack->writeValidEnd(root))
            return false;
        StoreTesting::setRegistrySharing(false);
        auto taken = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), headerBytes());
        StoreTesting::setRegistrySharing(true);
        return !taken && !taken.error().error && !taken.error().packRejection.isNull();
    };
    JITCACHE_CHECK(rejects(*validEnd + packAlignment)); // past the file by one word
    JITCACHE_CHECK(rejects(*validEnd + (64 << 20))); // past the file, inside a fresh mapping's room
    JITCACHE_CHECK(rejects(std::numeric_limits<uint64_t>::max() - 7));
    JITCACHE_CHECK(rejects(*validEnd - 4)); // misaligned
    JITCACHE_CHECK(rejects(packPrefixBytes)); // shorter than any pack
    JITCACHE_CHECK(rejects(*firstEnd + packRecordHeaderBytes)); // inside the second record
    JITCACHE_CHECK(!rejects(*validEnd));

    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    if (!other)
        return;
    JITCACHE_CHECK(other->token(testKey(2)) == second->version);
    JITCACHE_CHECK(pack->writeValidEnd(*firstEnd));
    RefPtr<OpenedArtifact> earlier = pack->takeAnother(context);
    JITCACHE_CHECK(earlier && earlier->token(testKey(1)) == first->version && !earlier->token(testKey(2)));
    BodyOpen opened = other->open(testKey(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
}

// A host that rewrites the pack in place, instead of renaming a new file over it, can leave the file shorter than the root
// it holds, and the mapped pages past the file's end fault. A view that follows a moved root checks it against the file's
// size first, so the root is damage and no read runs past the file.
JITCACHE_TEST(storeRootPastAShrunkFileIsDamage, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto first = commitBody(pack->writer(), testKey(1), 25);
    auto firstEnd = pack->validEnd();
    RefPtr<OpenedArtifact> other = pack->takeAnother(context);
    auto second = commitBody(pack->writer(), testKey(2), 26, 1000); // many pages past the first valid end
    auto validEnd = pack->validEnd();
    if (!first || !firstEnd || !other || !second || !validEnd || *validEnd - *firstEnd < 16 * 4096) {
        JITCACHE_FAIL("the two commits did not leave the pack this test shrinks"_s);
        return;
    }
    int fd = openat(pack->cacheFd(), ArtifactNames::pack.characters(), O_WRONLY | O_CLOEXEC);
    bool truncated = fd >= 0 && !ftruncate(fd, static_cast<off_t>(*firstEnd));
    if (fd >= 0)
        ::close(fd);
    JITCACHE_CHECK(truncated);
    BodyOpen opened = other->open(testKey(2), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
    JITCACHE_CHECK(other->open(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Invalid);
}

// Bytes a commit wrote past the valid end before it stopped are invisible, and the next commit overwrites them.
JITCACHE_TEST(storeBytesPastTheValidEndAreInvisible, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    uint64_t validEndBefore = pack->validEnd().value_or(0);
    pack->writer().setFaultForTesting(ArtifactWriter::FaultForTesting { WriterChecks::reread, 1 });
    auto failed = commitBody(pack->writer(), testKey(1), 31);
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::reread);
    JITCACHE_CHECK(!pack->artifact().token(testKey(1)));
    JITCACHE_CHECK(pack->validEnd() == validEndBefore && pack->packFileSize() > validEndBefore);
    pack->writer().setFaultForTesting(std::nullopt);
    auto committed = commitBody(pack->writer(), testKey(2), 32);
    JITCACHE_CHECK(!!committed);
    // The record lands where the failed one began, so the file holds nothing past the new valid end.
    BodyOpen opened = pack->artifact().open(testKey(2), ValidationMode::Full);
    JITCACHE_CHECK(opened.body && holdsSections(*opened.body, 32));
    JITCACHE_CHECK(committed && pack->validEnd() == validEndBefore + *packRecordExtent(committed->fileSize) + packIndexBlockBytes(1));
    JITCACHE_CHECK(pack->packFileSize() == pack->validEnd());
    JITCACHE_CHECK(!pack->artifact().token(testKey(1)) && pack->artifact().token(testKey(2)));
}

JITCACHE_TEST(storeOpenRejectsDamagedBodies, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    auto committed = commitBody(pack->writer(), testKey(1), 41);
    if (!committed) {
        JITCACHE_FAIL("the commit failed"_s);
        return;
    }
    // The body's last byte belongs to its last section, which both modes checksum.
    uint64_t bodyEnd = packPrefixBytes + packIndexBlockBytes(0) + packRecordHeaderBytes + committed->fileSize;
    std::array<uint8_t, 1> damage { 0xEE };
    JITCACHE_CHECK(pack->writePackAt(bodyEnd - 1, damage));
    for (auto mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        BodyOpen opened = pack->artifact().open(testKey(1), mode);
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
    auto first = commitBody(pack->writer(), testKey(1), 51);
    JITCACHE_CHECK(other->token(testKey(1)) == first->version);
    // The second commit's delta is damaged before the other view applies it.
    auto second = commitBody(pack->writer(), testKey(2), 52);
    auto latestBlock = pack->latestBlock();
    if (!second || !latestBlock) {
        JITCACHE_FAIL("the second commit failed"_s);
        return;
    }
    std::array<uint8_t, 1> damage { 0x5A };
    JITCACHE_CHECK(pack->writePackAt(*latestBlock + packIndexBlockHeaderBytes + 3, damage));
    BodyOpen opened = other->open(testKey(2), ValidationMode::Integrity);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
    JITCACHE_CHECK(other->open(testKey(1), ValidationMode::Integrity).outcome == StoreOutcome::Invalid);
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
    auto committed = commitBody(pack->writer(), testKey(1), 61);
    SavedSummaryRead read = other->readSavedSummaries(testKey(1), ValidationMode::Full);
    JITCACHE_CHECK(read.outcome == StoreOutcome::Found && read.summaries && committed);
    if (!read.summaries || !committed)
        return;
    auto expected = bodySections(61);
    JITCACHE_CHECK(read.summaries->version() == committed->version && read.summaries->highestTier() == 1);
    JITCACHE_CHECK(equalSpans(read.summaries->ucbFeedback(), expected->bytes[static_cast<size_t>(SectionKind::UCBFeedback)].span()));
    JITCACHE_CHECK(equalSpans(read.summaries->cbSummary(), expected->bytes[static_cast<size_t>(SectionKind::CBSummaryBaseline)].span()));
    JITCACHE_CHECK(equalSpans(read.summaries->ics(), expected->bytes[static_cast<size_t>(SectionKind::ICsBaseline)].span()));

    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::ReadSavedSummaries, EMFILE, std::nullopt });
    JITCACHE_CHECK(other->readSavedSummaries(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Unavailable);
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, 1 });
    BodyOpen opened = other->open(testKey(1), ValidationMode::Full);
    JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.error == EIO);
    JITCACHE_CHECK(other->open(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Found);
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
        auto committed = commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed));
        JITCACHE_CHECK(!!committed);
        versions.append(committed ? committed->version : 0);
    }
    // Five of the six keys recaptured leave dead bytes.
    for (uint16_t seed = 1; seed <= 6; ++seed) {
        if (seed == 5)
            continue;
        auto committed = commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed + 100));
        versions[seed - 1] = committed ? committed->version : 0;
    }
    BodyOpen held = other->open(testKey(5), ValidationMode::Full);
    JITCACHE_CHECK(held.body && holdsSections(*held.body, 5));

    auto compacted = pack->writer().compactIfNeeded(false);
    JITCACHE_CHECK(compacted && *compacted && (*compacted)->bodies == 6);
    JITCACHE_CHECK(pack->validEnd() == (compacted && *compacted ? (*compacted)->packBytes : 0));
    for (uint16_t seed = 1; seed <= 6; ++seed) {
        JITCACHE_CHECK(other->token(testKey(seed)) == versions[seed - 1]);
        BodyOpen opened = other->open(testKey(seed), ValidationMode::Full);
        JITCACHE_CHECK(opened.body && holdsSections(*opened.body, static_cast<uint8_t>(seed != 5 ? seed + 100 : seed)));
    }
    JITCACHE_CHECK(held.body && holdsSections(*held.body, 5));
    JITCACHE_CHECK(other->statistics().reopens == 1);
    JITCACHE_CHECK(pack->artifact().statistics().liveBytes + packIndexBlockBytes(6) == pack->validEnd());
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
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 71));
    JITCACHE_CHECK(!!other->token(testKey(1)));
    uint64_t retired = 1;
    JITCACHE_CHECK(pack->writePackAt(packRetiredOffset, asByteSpan(retired)));
    JITCACHE_CHECK(!unlinkat(pack->cacheFd(), ArtifactNames::pack.characters(), 0));
    JITCACHE_CHECK(!other->token(testKey(1)));
    JITCACHE_CHECK(other->open(testKey(1), ValidationMode::Full).outcome == StoreOutcome::Absent);
    JITCACHE_CHECK(!other->indexedBodies());
}

// A sound pack renamed over a retired one under another header, as a Producer whose config names another app writes when
// it reuses a whole deletion's remnant, means the artifact the view checked is gone, so the view misses every key. The same
// successor with a damaged digest byte fails the prefix's CRC first and stays damage.
JITCACHE_TEST(storeRetiredPackFollowedByAnotherHeadersPackIsGone, No)
{
    HookScope hooks;
    ArtifactHeaderBytes another = headerNamingApp(0x5eed);
    HeaderDigest anotherDigest = artifactHeaderDigest(another.span());
    HeaderDigest original = headerDigest();
    JITCACHE_CHECK(!equalSpans(std::span { anotherDigest }, std::span { original }));
    for (bool damaged : { false, true }) {
        auto pack = PackArtifact::create(context);
        if (!pack)
            return;
        RefPtr<OpenedArtifact> other = pack->takeAnother(context);
        if (!other)
            return;
        JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 72));
        JITCACHE_CHECK(!!other->token(testKey(1)));

        uint64_t retired = 1;
        JITCACHE_CHECK(pack->writePackAt(packRetiredOffset, asByteSpan(retired)));
        JITCACHE_CHECK(!createEmptyPack(pack->cacheFd(), std::span<const uint8_t, 16> { anotherDigest }));
        if (damaged) {
            std::array<uint8_t, 1> flipped { static_cast<uint8_t>(anotherDigest[4] ^ 1) };
            JITCACHE_CHECK(pack->writePackAt(20, flipped)); // the digest's fifth byte, which the CRC covers
        }
        BodyOpen opened = other->open(testKey(1), ValidationMode::Full);
        if (damaged) {
            JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::pack);
            continue;
        }
        JITCACHE_CHECK(opened.outcome == StoreOutcome::Absent);
        JITCACHE_CHECK(!other->token(testKey(1)) && !other->indexedBodies());
    }
}

// The registry shares an object only with a VM that checked the header the object was built from, and only while the
// object follows a pack. A header naming an app, written in the same cache/ over a pack under its digest, as a Producer
// that reuses a whole deletion's remnant writes one, gets a new object, which later takes of that header share, while the
// object held across the rewrite keeps its digest and the pack it mapped. An object whose pack went gets a new one too,
// under an equal header.
JITCACHE_TEST(storeRegistryRebuildsWhenTheHeaderChanges, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    RefPtr<OpenedArtifact> held = pack->take(context);
    JITCACHE_CHECK(held.get() == &pack->artifact());
    JITCACHE_CHECK(!!commitBody(pack->writer(), testKey(1), 91));
    JITCACHE_CHECK(held && !!held->token(testKey(1)));
    if (!held)
        return;

    HeaderDigest original = headerDigest();
    ArtifactHeaderBytes another = headerNamingApp(0x5eed);
    HeaderDigest anotherDigest = artifactHeaderDigest(another.span());
    JITCACHE_CHECK(!equalSpans(std::span { anotherDigest }, std::span { original }));
    JITCACHE_CHECK(!createEmptyPack(pack->cacheFd(), std::span<const uint8_t, 16> { anotherDigest }));
    auto taken = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), another.span());
    if (!taken) {
        JITCACHE_FAIL("ArtifactRegistry::take failed for the new header"_s);
        return;
    }
    RefPtr<OpenedArtifact> rebuilt = WTF::move(*taken);
    JITCACHE_CHECK(rebuilt != held);
    JITCACHE_CHECK(equalSpans(rebuilt->headerDigest(), std::span { anotherDigest }));
    JITCACHE_CHECK(!rebuilt->indexedBodies() && !rebuilt->token(testKey(1)));
    auto shared = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), another.span());
    JITCACHE_CHECK(shared && shared->ptr() == rebuilt.get());
    JITCACHE_CHECK(equalSpans(held->headerDigest(), std::span { original }));
    JITCACHE_CHECK(!!held->token(testKey(1)));

    // A whole deletion retires the pack and unlinks it, which a read of the object then finds; a pack created again under
    // the same header gets a new object.
    uint64_t retired = 1;
    JITCACHE_CHECK(pack->writePackAt(packRetiredOffset, asByteSpan(retired)));
    JITCACHE_CHECK(!unlinkat(pack->cacheFd(), ArtifactNames::pack.characters(), 0));
    JITCACHE_CHECK(!rebuilt->token(testKey(1)));
    JITCACHE_CHECK(!createEmptyPack(pack->cacheFd(), std::span<const uint8_t, 16> { anotherDigest }));
    auto afterDeletion = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), another.span());
    JITCACHE_CHECK(afterDeletion && afterDeletion->ptr() != rebuilt.get());
    JITCACHE_CHECK(afterDeletion && equalSpans((*afterDeletion)->headerDigest(), std::span { anotherDigest }));
    auto sharedAfterDeletion = ArtifactRegistry::take(pack->parentFd(), pack->cacheFd(), another.span());
    JITCACHE_CHECK(afterDeletion && sharedAfterDeletion && sharedAfterDeletion->ptr() == afterDeletion->ptr());
}

// readSection checksums the one section it returns: a module body's ucb.analysis, an empty span for a body without one,
// and Absent for a key without a body. Damage in another section passes it unseen, which open then reports, while damage
// in that section is Invalid at container.checksum. Each read that reaches a body counts as an open of the index.
JITCACHE_TEST(storeReadSectionChecksOneSection, No)
{
    HookScope hooks;
    auto pack = PackArtifact::create(context);
    if (!pack)
        return;
    constexpr size_t analysisBytes = 37;
    BodyKey module = testKey(1, IdentityKind::Module);
    BodyKey plain = testKey(2);
    BodyKey absent = testKey(3);
    // The module's body is the pack's first record, so its bytes start right after the empty pack's index block.
    auto moduleSections = bodySections(81, 1, analysisBytes);
    auto committed = pack->writer().commit({ module, 500, 81, 1 }, moduleSections->sections());
    if (!committed || !commitBody(pack->writer(), plain, 82)) {
        JITCACHE_FAIL("a commit failed"_s);
        return;
    }
    auto analysis = moduleSections->bytes[static_cast<size_t>(SectionKind::UCBAnalysis)].span();

    for (auto mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        uint64_t opens = pack->artifact().statistics().opens;
        SectionRead read = pack->artifact().readSection(module, SectionKind::UCBAnalysis, mode);
        JITCACHE_CHECK(read.outcome == StoreOutcome::Found && read.pack && equalSpans(read.section, analysis));
        JITCACHE_CHECK(isEightByteAligned(read.section));
        SectionRead without = pack->artifact().readSection(plain, SectionKind::UCBAnalysis, mode);
        JITCACHE_CHECK(without.outcome == StoreOutcome::Found && without.section.empty());
        SectionRead core = pack->artifact().readSection(plain, SectionKind::UCBCore, mode);
        JITCACHE_CHECK(core.outcome == StoreOutcome::Found && equalSpans(core.section, bodySections(82)->bytes[static_cast<size_t>(SectionKind::UCBCore)].span()));
        JITCACHE_CHECK(pack->artifact().readSection(absent, SectionKind::UCBAnalysis, mode).outcome == StoreOutcome::Absent);
        JITCACHE_CHECK(pack->artifact().statistics().opens == opens + 3);
    }

    // The module body's last byte belongs to its last section, ICsBaseline.
    uint64_t bodyStart = packPrefixBytes + packIndexBlockBytes(0) + packRecordHeaderBytes;
    std::array<uint8_t, 1> damage { 0xEE };
    JITCACHE_CHECK(pack->writePackAt(bodyStart + committed->fileSize - 1, damage));
    for (auto mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        SectionRead read = pack->artifact().readSection(module, SectionKind::UCBAnalysis, mode);
        JITCACHE_CHECK(read.outcome == StoreOutcome::Found && equalSpans(read.section, analysis));
        BodyOpen opened = pack->artifact().open(module, mode);
        JITCACHE_CHECK(opened.outcome == StoreOutcome::Invalid && opened.failure.check == ContainerChecks::checksum);
    }

    // Damage in ucb.analysis itself: its offset in the body is where the writer's layout put it.
    Vector<BodyDirectoryEntry> entries;
    for (auto& source : moduleSections->sources)
        entries.append(BodyDirectoryEntry { source.kind, source.size, 0, 0 });
    JITCACHE_CHECK(layOutBodySections(entries.mutableSpan()) == committed->fileSize);
    auto analysisEntry = std::ranges::find_if(entries, [](auto& entry) {
        return entry.kind == SectionKind::UCBAnalysis;
    });
    if (analysisEntry == entries.end())
        return;
    JITCACHE_CHECK(pack->writePackAt(bodyStart + analysisEntry->offset + 5, damage));
    for (auto mode : { ValidationMode::Integrity, ValidationMode::Full }) {
        SectionRead read = pack->artifact().readSection(module, SectionKind::UCBAnalysis, mode);
        JITCACHE_CHECK(read.outcome == StoreOutcome::Invalid && read.failure.check == ContainerChecks::checksum && !read.pack);
        JITCACHE_CHECK(pack->artifact().readSection(module, SectionKind::UCBCore, mode).outcome == StoreOutcome::Found);
    }
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
        if (!commitBody(pack->writer(), testKey(seed), static_cast<uint8_t>(seed))) {
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

// createForTesting copies each section to an 8-byte-aligned address, leaves absent kinds empty, and keeps the key, the
// version and the highest tier of the kinds it was given; a test body has no envelope, so its L and P are 0.
JITCACHE_TEST(integratorValidatedBodyForTesting, No)
{
    struct Expected {
        SectionKind kind;
        Vector<uint8_t> bytes;
    };
    std::array<Expected, 5> expected { {
        { SectionKind::UCBIdentity, patternBytes(0, 1) },
        { SectionKind::UCBCore, patternBytes(1, 2) },
        { SectionKind::ImageBaseline, patternBytes(7, 3) },
        { SectionKind::CBStateBaseline, patternBytes(8, 4) },
        { SectionKind::ICsBaseline, patternBytes(4097, 5) },
    } };
    std::array<ValidatedBody::TestSection, 5> sections;
    size_t totalSize = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        sections[i] = { expected[i].kind, expected[i].bytes.span() };
        totalSize += expected[i].bytes.size();
    }

    BodyKey key = testKey(1);
    Ref<ValidatedBody> body = ValidatedBody::createForTesting(key, 42, sections);
    JITCACHE_CHECK(body->key() == key);
    JITCACHE_CHECK(!(body->key() == testKey(2)));
    JITCACHE_CHECK(body->version() == 42);
    JITCACHE_CHECK(body->highestTier() == 1);
    JITCACHE_CHECK(!body->llintThreshold());
    JITCACHE_CHECK(!body->counterProgress());
    JITCACHE_CHECK(body->fileSize() >= totalSize);

    std::array<bool, numberOfSectionKinds> present { };
    for (auto& section : expected) {
        present[static_cast<unsigned>(section.kind)] = true;
        std::span<const uint8_t> span = body->section(section.kind);
        JITCACHE_CHECK(isEightByteAligned(span));
        JITCACHE_CHECK(span.size() == section.bytes.size());
        JITCACHE_CHECK(equalSpans(span, section.bytes.span()));
        // A copy, not the caller's bytes.
        JITCACHE_CHECK(span.empty() || span.data() != section.bytes.span().data());
    }
    for (unsigned kind = 0; kind < numberOfSectionKinds; ++kind) {
        if (!present[kind])
            JITCACHE_CHECK(body->section(static_cast<SectionKind>(kind)).empty());
    }

    // The highest tier is that of the kinds given: 0 for the UCB sections alone, and for no section at all.
    std::array<ValidatedBody::TestSection, 1> ucbOnly { { { SectionKind::UCBFeedback, sections[1].bytes } } };
    Ref<ValidatedBody> tierZero = ValidatedBody::createForTesting(testKey(3), 1, ucbOnly);
    JITCACHE_CHECK(!tierZero->highestTier());
    JITCACHE_CHECK(equalSpans(tierZero->section(SectionKind::UCBFeedback), sections[1].bytes));
    Ref<ValidatedBody> empty = ValidatedBody::createForTesting(testKey(4), 1, { });
    JITCACHE_CHECK(!empty->highestTier());
    JITCACHE_CHECK(!empty->fileSize());

    // What VMState::openBody answers with it.
    BodyLookup found = BodyLookup::found(body.copyRef());
    JITCACHE_CHECK(found.kind() == BodyLookup::Kind::Found);
    JITCACHE_CHECK(found.body() == body.ptr());
    JITCACHE_CHECK(BodyLookup::missing().kind() == BodyLookup::Kind::Missing);
    JITCACHE_CHECK(!BodyLookup::missing().body());
    JITCACHE_CHECK(BodyLookup::unusable().kind() == BodyLookup::Kind::Unusable);
    JITCACHE_CHECK(!BodyLookup::unusable().body());
}

// onDestroy runs exactly once, from the destructor, on the thread that drops the last reference.
JITCACHE_TEST(integratorValidatedBodyDestroyedByLastReference, No)
{
    std::atomic<unsigned> destroyCount { 0 };
    std::atomic<uint32_t> destroyingThread { 0 };
    std::atomic<uint32_t> workerThread { 0 };
    BinarySemaphore mainDroppedItsReference;

    RefPtr<ValidatedBody> body = ValidatedBody::createForTesting(testKey(5), 7, { }, [&] {
        ++destroyCount;
        destroyingThread = Thread::currentSingleton().uid();
    });
    RefPtr<ValidatedBody> handedOff = body;
    Ref<Thread> worker = Thread::create("JITCache body test"_s, [&, handedOff = WTF::move(handedOff)]() mutable {
        workerThread = Thread::currentSingleton().uid();
        mainDroppedItsReference.wait();
        handedOff = nullptr;
    });

    body = nullptr;
    JITCACHE_CHECK(!destroyCount.load());
    mainDroppedItsReference.signal();
    worker->waitForCompletion();

    JITCACHE_CHECK(destroyCount.load() == 1);
    JITCACHE_CHECK(destroyingThread.load() == workerThread.load());
    JITCACHE_CHECK(destroyingThread.load() != Thread::currentSingleton().uid());
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
