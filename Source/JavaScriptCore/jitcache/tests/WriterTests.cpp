/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "JITCacheContainer.h"
#include "JITCacheTest.h"
#include "ProducerBudget.h"
#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/FileSystem.h>
#include <wtf/Noncopyable.h>
#include <wtf/Nonmovable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/SafeStrerror.h>
#include <wtf/Seconds.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Test C7 of SPEC-integrator.container.md section 9: the writer, on CommitSections built from crafted spans and streamed
// sources. None needs a VM. Each test works in a fresh temporary <parent> whose producer lock the writer holds; the
// registry hook builds a second OpenedArtifact for a directory where a test needs another process's view of it, and a
// body the test renames into bodies/ by hand, without a bump, stands for a change the committing object has not seen.
// A kill point ends its process, so harness sub-SPEC H8 and producer-kill.js test each point rather than C7.

namespace JSC::JITCache::Tests {

namespace WriterTestsInternal {

// The store's hooks are process-wide, and the tests of a group share a process, so every test restores the defaults when
// it ends.
class HookScope {
    WTF_MAKE_NONCOPYABLE(HookScope);
public:
    HookScope() = default;
    ~HookScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
        StoreTesting::setInotify(true);
        StoreTesting::setFallbackListingInterval(std::nullopt);
    }
};

// A body key from its canonical bytes (SPEC-ucb.md section 3.1): version 1, a program body, call specialization, no mode
// bits, and an identity digest drawn from the seed, so distinct seeds give distinct keys.
static BodyKey writerKey(uint8_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t index = 8; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(7 * seed + 31 * (index - 8));
    auto key = BodyKey::fromBytes(bytes);
    RELEASE_ASSERT(key);
    return *key;
}

// The store reads no header file: start checks the header and hands its bytes to ArtifactRegistry::take, which digests
// them. Any bytes stand for a header here.
static std::span<const uint8_t> writerHeaderBytes()
{
    static const std::array<uint8_t, 48> bytes = [] {
        std::array<uint8_t, 48> result { };
        for (size_t index = 0; index < result.size(); ++index)
            result[index] = static_cast<uint8_t>(5 * index + 3);
        return result;
    }();
    return bytes;
}

static HeaderDigest writerHeaderDigest()
{
    return artifactHeaderDigest(writerHeaderBytes());
}

static String errnoText(int error)
{
    return String::fromUTF8(safeStrerror(error).data());
}

static String describe(const CommitFailure& failure)
{
    return makeString(failure.check, ": "_s, failure.detail);
}

// The bytes of every section a body of highest tier 1 holds in this build (the twins build, so image-twins.baseline
// too), by SectionKind, each filled from a seed so two bodies differ in every non-empty section.
using SectionSizes = std::array<size_t, numberOfSectionKinds>;
static constexpr SectionSizes defaultSizes { 40, 300, 17, 1000, 33, 9, 120, 64, 250 };
static constexpr SectionSizes otherSizes { 8, 129, 64, 4097, 1, 16, 7, 300, 24 };
static constexpr SectionSizes placedSizes { 24, 40, 17, 64, 8, 12, 33, 9, 20 };

struct SectionBytes {
    std::array<Vector<uint8_t>, numberOfSectionKinds> bytes;
};

static SectionBytes makeSectionBytes(const SectionSizes& sizes, uint8_t seed)
{
    SectionBytes sections;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        if (!isSectionRequired(static_cast<SectionKind>(index), 1))
            continue;
        for (size_t offset = 0; offset < sizes[index]; ++offset)
            sections.bytes[index].append(static_cast<uint8_t>(31 * seed + 17 * index + 7 * offset + (offset >> 8)));
    }
    return sections;
}

// The bytes of every section together: what a rewrite of the body copies, and charges.
static size_t totalBytes(const SectionBytes& sections)
{
    size_t total = 0;
    for (auto& bytes : sections.bytes)
        total += bytes.size();
    return total;
}

// A section a test object streams through the sink in pieces of piece bytes, the last one shorter, as a lane's write
// function does; it can stream a byte too few or too many, or fail on its own after streaming every byte.
struct StreamedSection {
    std::span<const uint8_t> bytes;
    size_t piece { 1 };
    int miscount { 0 }; // -1: one byte fewer than bytes; +1: one byte more
    bool fails { false }; // returns false although the sink refused nothing
};

static bool streamTestSection(const void* object, const SectionSink& sink)
{
    auto& section = *static_cast<const StreamedSection*>(object);
    auto bytes = section.bytes;
    if (section.miscount < 0 && !bytes.empty())
        bytes = bytes.first(bytes.size() - 1);
    while (!bytes.empty()) {
        size_t count = std::min(section.piece, bytes.size());
        if (!sink(bytes.first(count)))
            return false;
        bytes = bytes.subspan(count);
    }
    if (section.miscount > 0) {
        static constexpr std::array<uint8_t, 1> extraByte { 0x5A };
        if (!sink(std::span<const uint8_t> { extraByte }))
            return false;
    }
    return !section.fails;
}

// Which sections a test streams; the rest are held in memory. The capture glue streams the three image sections.
enum class Streaming : uint8_t { None, Image, All };

static bool streams(Streaming streaming, SectionKind kind)
{
    switch (streaming) {
    case Streaming::None:
        return false;
    case Streaming::Image:
        return kind == SectionKind::ImageBaseline || kind == SectionKind::BakedFactsBaseline || kind == SectionKind::ImageTwinsBaseline;
    case Streaming::All:
        return true;
    }
    return false;
}

// The sources of one body, in section-kind order. The streamed sources point into the object, so it never moves.
class TestSources {
    WTF_MAKE_NONCOPYABLE(TestSources);
    WTF_MAKE_NONMOVABLE(TestSources);
public:
    TestSources(const SectionBytes& sections, Streaming streaming, size_t piece)
    {
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            auto kind = static_cast<SectionKind>(index);
            if (!isSectionRequired(kind, 1))
                continue;
            auto bytes = sections.bytes[index].span();
            m_streamed[index] = StreamedSection { bytes, piece, 0, false };
            if (streams(streaming, kind))
                m_sources.append(SectionSource::streamed(kind, bytes.size(), streamTestSection, &m_streamed[index]));
            else
                m_sources.append(SectionSource::inMemory(kind, bytes));
        }
    }

    StreamedSection& streamed(SectionKind kind) { return m_streamed[static_cast<size_t>(kind)]; }

    void replace(const SectionSource& source)
    {
        for (auto& existing : m_sources) {
            if (existing.kind == source.kind)
                existing = source;
        }
    }

    CommitSections commitSections() const { return CommitSections { m_sources.span() }; }

private:
    std::array<StreamedSection, numberOfSectionKinds> m_streamed { };
    Vector<SectionSource, numberOfSectionKinds> m_sources;
};

static constexpr uint32_t testLLIntThreshold = 500;
static constexpr uint32_t testCounterProgress = 77;

static CommitStamp testStamp(const BodyKey& key)
{
    return CommitStamp { key, testLLIntThreshold, testCounterProgress, 1 };
}

static bool writeFileAt(TestContext& context, int directoryFd, const char* name, std::span<const uint8_t> bytes)
{
    int fd = openat(directoryFd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot create "_s, String::fromUTF8(name), ": "_s, errnoText(errno)));
        return false;
    }
    while (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR)
                continue;
            JITCACHE_FAIL(makeString("cannot write "_s, String::fromUTF8(name), ": "_s, errnoText(errno)));
            ::close(fd);
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    ::close(fd);
    return true;
}

static std::optional<Vector<uint8_t>> readFileAt(int directoryFd, const char* name)
{
    int fd = openat(directoryFd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    Vector<uint8_t> bytes;
    std::array<uint8_t, 4096> buffer;
    while (true) {
        ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        bytes.append(std::span { buffer }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return bytes;
}

// A temporary <parent> with cache/, cache/bodies/ and the producer lock the writer holds, removed with everything in it
// when the object goes. A test bumps the epoch through that lock as another process's producer would.
class WriterTestArtifact {
    WTF_MAKE_NONCOPYABLE(WriterTestArtifact);
public:
    static std::unique_ptr<WriterTestArtifact> create(TestContext& context)
    {
        const char* temporaryDirectory = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(temporaryDirectory && *temporaryDirectory ? temporaryDirectory : "/tmp"), "/jitcache-writer-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        std::unique_ptr<WriterTestArtifact> artifact(new WriterTestArtifact(String::fromUTF8(path.span().data())));
        artifact->m_parentFd = ::open(path.span().data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (artifact->m_parentFd < 0
            || mkdirat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), 0755)
            || (artifact->m_cacheFd = openat(artifact->m_parentFd, ArtifactNames::cacheDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0
            || mkdirat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), 0755)
            || (artifact->m_bodiesFd = openat(artifact->m_cacheFd, ArtifactNames::bodiesDirectory.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            JITCACHE_FAIL(makeString("cannot lay out a test artifact: "_s, errnoText(errno)));
            return nullptr;
        }
        auto lock = ProducerLock::tryAcquire(artifact->m_parentFd);
        if (!lock) {
            JITCACHE_FAIL(makeString("cannot take the producer lock: "_s, errnoText(lock.error().error)));
            return nullptr;
        }
        artifact->m_lock = WTF::move(*lock);
        return artifact;
    }

    ~WriterTestArtifact()
    {
        m_lock = nullptr;
        for (int fd : { m_bodiesFd, m_cacheFd, m_parentFd }) {
            if (fd >= 0)
                ::close(fd);
        }
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    int cacheFd() const { return m_cacheFd; }
    int bodiesFd() const { return m_bodiesFd; }
    ProducerLock& lock() { return *m_lock; }

    RefPtr<OpenedArtifact> take(TestContext& context)
    {
        auto taken = ArtifactRegistry::take(m_parentFd, m_cacheFd, writerHeaderBytes());
        if (!taken) {
            JITCACHE_FAIL(makeString("ArtifactRegistry::take failed: "_s, errnoText(taken.error())));
            return nullptr;
        }
        return WTF::move(*taken);
    }

    // Stands for another process's commit or maintenance run.
    void bump() { m_lock->bumpEpoch(); }

    // The commit epoch, read through the lock file (container sub-SPEC section 2).
    std::optional<uint64_t> epoch() const
    {
        int fd = openat(m_parentFd, ArtifactNames::lockFile.characters(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return std::nullopt;
        std::array<uint8_t, sizeof(uint64_t)> bytes { };
        ssize_t count = ::pread(fd, bytes.data(), bytes.size(), 16);
        ::close(fd);
        if (count != static_cast<ssize_t>(bytes.size()))
            return std::nullopt;
        uint64_t epoch = 0;
        memcpySpan(asMutableByteSpan(epoch), std::span { bytes });
        return epoch;
    }

    // A synthetic body renamed from cache/ into bodies/ without a bump: a change no object has been told of.
    bool placeBody(TestContext& context, const BodyKey& key, uint8_t seed)
    {
        SectionBytes sections = makeSectionBytes(placedSizes, seed);
        Vector<ContainerTesting::Section> list;
        for (size_t index = 0; index < numberOfSectionKinds; ++index) {
            auto kind = static_cast<SectionKind>(index);
            if (isSectionRequired(kind, 1))
                list.append({ kind, sections.bytes[index].span() });
        }
        BodyEnvelope stamp;
        stamp.highestTier = 1;
        stamp.key = key;
        stamp.version = 1 + seed;
        stamp.headerDigest = writerHeaderDigest();
        Vector<uint8_t> file = ContainerTesting::buildBody(stamp, list.span());
        return replaceBody(context, key, file.span());
    }

    // Any bytes renamed from cache/ over the key's name in bodies/, without a bump.
    bool replaceBody(TestContext& context, const BodyKey& key, std::span<const uint8_t> file)
    {
        TemporaryFileName temporary = temporaryFileName(TemporaryKind::Body);
        if (!writeFileAt(context, m_cacheFd, temporary.data(), file))
            return false;
        if (renameat(m_cacheFd, temporary.data(), m_bodiesFd, bodyFileName(key).data())) {
            JITCACHE_FAIL(makeString("cannot rename a body into bodies/: "_s, errnoText(errno)));
            return false;
        }
        return true;
    }

    // The names in a directory, or nothing when it cannot be listed.
    std::optional<Vector<String>> names(int directoryFd) const
    {
        Vector<String> result;
        int error = listDirectory(directoryFd, [&](std::span<const char> name, uint64_t) {
            result.append(String { name });
        });
        if (error)
            return std::nullopt;
        return result;
    }

    // The temporaries in cache/, which a failed commit must not leave.
    size_t temporariesInCache() const
    {
        size_t count = 0;
        int error = listDirectory(m_cacheFd, [&](std::span<const char> name, uint64_t) {
            if (temporaryKindOfFileName(name))
                ++count;
        });
        return error ? SIZE_MAX : count;
    }

    // Whether bodies/ holds exactly the given keys' files.
    bool bodiesAre(std::initializer_list<BodyKey> keys) const
    {
        auto listed = names(m_bodiesFd);
        if (!listed || listed->size() != keys.size())
            return false;
        return std::ranges::all_of(keys, [&](const BodyKey& key) {
            return listed->contains(String { bodyFileName(key).span() });
        });
    }

private:
    explicit WriterTestArtifact(String&& path)
        : m_path(WTF::move(path))
    {
    }

    const String m_path;
    int m_parentFd { -1 };
    int m_cacheFd { -1 };
    int m_bodiesFd { -1 };
    std::unique_ptr<ProducerLock> m_lock;
};

// The events a watch of the test's own reads from a directory, standing for the queue of another object that watches it.
class DirectoryWatch {
    WTF_MAKE_NONCOPYABLE(DirectoryWatch);
public:
    struct Event {
        uint32_t mask;
        Vector<char> name; // without the kernel's NUL padding
    };

    DirectoryWatch(int directoryFd, uint32_t mask)
    {
        m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (m_fd < 0)
            return;
        CString path = makeString("/proc/self/fd/"_s, directoryFd).utf8();
        if (inotify_add_watch(m_fd, path.data(), mask) < 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    ~DirectoryWatch()
    {
        if (m_fd >= 0)
            ::close(m_fd);
    }

    bool isValid() const { return m_fd >= 0; }

    Vector<Event> takeEvents()
    {
        Vector<Event> events;
        std::array<uint8_t, 4096> buffer;
        while (true) {
            ssize_t count = ::read(m_fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                break;
            auto bytes = std::span { buffer }.first(static_cast<size_t>(count));
            while (bytes.size() >= sizeof(struct inotify_event)) {
                uint32_t mask = 0;
                uint32_t length = 0;
                memcpySpan(asMutableByteSpan(mask), bytes.subspan(offsetof(struct inotify_event, mask), sizeof(mask)));
                memcpySpan(asMutableByteSpan(length), bytes.subspan(offsetof(struct inotify_event, len), sizeof(length)));
                size_t eventBytes = sizeof(struct inotify_event) + length;
                if (eventBytes > bytes.size())
                    break;
                auto nameBytes = spanReinterpretCast<const char>(bytes.subspan(sizeof(struct inotify_event), length));
                auto nameEnd = std::ranges::find(nameBytes, '\0');
                Vector<char> name;
                name.append(nameBytes.first(static_cast<size_t>(nameEnd - nameBytes.begin())));
                events.append({ mask, WTF::move(name) });
                bytes = bytes.subspan(eventBytes);
            }
        }
        return events;
    }

private:
    int m_fd { -1 };
};

// The mask every OpenedArtifact watches bodies/ with (container sub-SPEC section 6.3).
static constexpr uint32_t bodiesWatchMask = IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

// The body the commit published, through the store's own open in Full mode: its commit identifier and size are the
// commit's, every section is byte for byte the sources' bytes, and its envelope holds the stamp's L and P.
static void checkPublished(TestContext& context, const String& label, WriterTestArtifact& artifact, OpenedArtifact& object,
    const BodyKey& key, const SectionBytes& expected, const CommitResult& result, uint32_t llintThreshold = testLLIntThreshold,
    uint32_t counterProgress = testCounterProgress)
{
    BodyOpen opened = object.open(key, ValidationMode::Full);
    if (opened.outcome != StoreOutcome::Found || !opened.body) {
        JITCACHE_FAIL(makeString(label, ": open does not find the published body"_s));
        return;
    }
    const ValidatedBody& body = *opened.body;
    if (!result.version || body.version() != result.version)
        JITCACHE_FAIL(makeString(label, ": the body's commit identifier is "_s, body.version(), " where the commit returned "_s, result.version));
    if (body.fileSize() != result.fileSize)
        JITCACHE_FAIL(makeString(label, ": the body holds "_s, body.fileSize(), " bytes where the commit returned "_s, result.fileSize));
    JITCACHE_CHECK(body.key() == key && body.highestTier() == 1);
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (!equalSpans(body.section(kind), expected.bytes[index].span()))
            JITCACHE_FAIL(makeString(label, ": section "_s, sectionKindDescription(kind).name, " differs from the source's bytes"_s));
    }

    auto file = readFileAt(artifact.bodiesFd(), bodyFileName(key).data());
    if (!file) {
        JITCACHE_FAIL(makeString(label, ": the body file cannot be read"_s));
        return;
    }
    auto layout = validateBody(file->span(), key, writerHeaderDigest(), ValidationMode::Full);
    if (!layout) {
        JITCACHE_FAIL(makeString(label, ": the body file fails "_s, layout.error()));
        return;
    }
    if (layout->llintThreshold != llintThreshold || layout->counterProgress != counterProgress)
        JITCACHE_FAIL(makeString(label, ": the envelope holds L "_s, layout->llintThreshold, " and P "_s, layout->counterProgress));
}

static std::optional<CommitResult> commitOrFail(TestContext& context, const String& label, ArtifactWriter& writer, const CommitStamp& stamp, const TestSources& sources)
{
    auto committed = writer.commit(stamp, sources.commitSections());
    if (!committed) {
        JITCACHE_FAIL(makeString(label, ": the commit failed at "_s, describe(committed.error())));
        return std::nullopt;
    }
    return *committed;
}

// Step 8 of container sub-SPEC section 8.2 leaves the committing object current (test C7): it applied the events queued
// for it, its own commit's included, and took its own bump as seen, so its next lookup neither refreshes nor lists. Call
// this right after a commit through object and before any lookup through it, because a lookup that refreshed would make
// the object current by itself and hide a step 8 that did not. A body renamed in without a bump must then be missed by
// the object's next lookups, which still find every key of present. The refresh the next bump makes must find that body
// and keep each present key's token, which an event of the commit left queued, or a listing that met another inode than
// the one step 8 recorded, would have replaced.
static void checkCurrentAfterCommit(TestContext& context, const String& label, WriterTestArtifact& artifact, OpenedArtifact& object,
    std::initializer_list<BodyKey> present, const BodyKey& unseen, uint8_t seed)
{
    if (!artifact.placeBody(context, unseen, seed))
        return;
    if (object.token(unseen))
        JITCACHE_FAIL(makeString(label, ": the first lookup after the commit refreshed"_s));
    Vector<uint64_t> tokens;
    for (const BodyKey& key : present) {
        tokens.append(object.token(key));
        if (!tokens.last())
            JITCACHE_FAIL(makeString(label, ": the index lacks present key "_s, tokens.size() - 1, " after the commit"_s));
    }

    artifact.bump();
    if (!object.token(unseen))
        JITCACHE_FAIL(makeString(label, ": the refresh after the next bump missed the body renamed in without a bump"_s));
    size_t position = 0;
    for (const BodyKey& key : present) {
        if (object.token(key) != tokens[position])
            JITCACHE_FAIL(makeString(label, ": the refresh after the next bump gave present key "_s, position, " a fresh token"_s));
        ++position;
    }
}

} // namespace WriterTestsInternal

using namespace WriterTestsInternal;

// A commit publishes a body that open validates, whose sections are byte for byte the sources' bytes, in memory or
// streamed, with the stamp's L and P and a fresh commit identifier per write; it leaves no temporary, and a second commit
// of the key replaces the body whole and gives the key a fresh token. The staging buffer is charged at the first commit
// alone and released at the end of production.
JITCACHE_TEST(writerCommitPublishesTheSources, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 64);
    BodyKey key = writerKey(1);
    JITCACHE_CHECK(!budget->chargedBytes());

    SectionBytes first = makeSectionBytes(defaultSizes, 1);
    TestSources firstSources(first, Streaming::Image, 7);
    auto committed = commitOrFail(context, "the first commit"_s, writer, testStamp(key), firstSources);
    if (!committed)
        return;
    checkPublished(context, "the first commit"_s, *artifact, *object, key, first, *committed);
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->bodiesAre({ key }));
    JITCACHE_CHECK(budget->chargedBytes() == 64);
    uint64_t firstToken = object->token(key);
    JITCACHE_CHECK(firstToken);

    SectionBytes second = makeSectionBytes(otherSizes, 2);
    TestSources secondSources(second, Streaming::All, 13);
    auto recommitted = commitOrFail(context, "the second commit"_s, writer, CommitStamp { key, 1000, 0, 1 }, secondSources);
    if (!recommitted)
        return;
    JITCACHE_CHECK(recommitted->version != committed->version);
    uint64_t secondToken = object->token(key);
    JITCACHE_CHECK(secondToken && secondToken != firstToken);
    checkPublished(context, "the second commit"_s, *artifact, *object, key, second, *recommitted, 1000, 0);
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->bodiesAre({ key }));
    JITCACHE_CHECK(budget->chargedBytes() == 64);

    writer.releaseStagingBuffer();
    JITCACHE_CHECK(!budget->chargedBytes());
}

// The temporary is created in cache/ and renamed from there, so the commit raises one event in bodies/, the rename's
// IN_MOVED_TO; the publish bumps the epoch once; a second object for the directory sees the body at its first lookup
// after the bump; and the committing object is current (checkCurrentAfterCommit).
JITCACHE_TEST(writerCommitRaisesOneEventAndBumpsOnce, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr committing = artifact->take(context);
    RefPtr second = artifact->take(context);
    if (!committing || !second)
        return;
    JITCACHE_CHECK(committing != second);
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*committing, artifact->lock(), budget.get(), 256);
    BodyKey key = writerKey(11);
    BodyKey placed = writerKey(12);

    DirectoryWatch cacheWatch(artifact->cacheFd(), IN_CREATE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE | IN_ONLYDIR);
    DirectoryWatch bodiesWatch(artifact->bodiesFd(), bodiesWatchMask);
    if (!cacheWatch.isValid() || !bodiesWatch.isValid()) {
        JITCACHE_FAIL("cannot watch the artifact's directories"_s);
        return;
    }
    auto epochBefore = artifact->epoch();
    JITCACHE_CHECK(epochBefore.has_value());
    JITCACHE_CHECK(!second->token(key));

    SectionBytes sections = makeSectionBytes(defaultSizes, 11);
    TestSources sources(sections, Streaming::Image, 64);
    if (!commitOrFail(context, "the commit"_s, writer, testStamp(key), sources))
        return;
    auto epochAfter = artifact->epoch();
    JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);

    auto cacheEvents = cacheWatch.takeEvents();
    if (cacheEvents.size() != 2
        || !(cacheEvents[0].mask & IN_CREATE) || !(cacheEvents[1].mask & IN_MOVED_FROM)
        || cacheEvents[0].name != cacheEvents[1].name
        || temporaryKindOfFileName(cacheEvents[0].name.span()) != TemporaryKind::Body)
        JITCACHE_FAIL(makeString("cache/ saw "_s, cacheEvents.size(), " events where a body temporary's creation and rename were expected"_s));
    auto bodiesEvents = bodiesWatch.takeEvents();
    if (bodiesEvents.size() != 1 || bodiesEvents[0].mask != IN_MOVED_TO || !equalSpans(bodiesEvents[0].name.span(), bodyFileName(key).span()))
        JITCACHE_FAIL(makeString("bodies/ saw "_s, bodiesEvents.size(), " events where the rename's IN_MOVED_TO alone was expected"_s));

    // The second object's lookup reads only its own index and queue, so it may come first.
    JITCACHE_CHECK(second->token(key));
    // No lookup through the committing object has run since the commit.
    checkCurrentAfterCommit(context, "the committing object"_s, *artifact, *committing, { key }, placed, 12);
}

// Without inotify, step 8 records the commit's epoch and the published file's inode: the committing object's next lookup
// does not list, and the listing the next epoch change makes keeps the committed key's token, which a listing does only
// for an entry whose inode matches the file it lists (checkCurrentAfterCommit).
JITCACHE_TEST(writerCommitRecordsTheInodeWithoutInotify, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(false);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 256);
    BodyKey key = writerKey(21);
    BodyKey placed = writerKey(22);

    SectionBytes sections = makeSectionBytes(defaultSizes, 21);
    TestSources sources(sections, Streaming::Image, 3);
    if (!commitOrFail(context, "the commit"_s, writer, testStamp(key), sources))
        return;
    checkCurrentAfterCommit(context, "without inotify"_s, *artifact, *object, { key }, placed, 22);
}

// A commit through an object whose queue holds an earlier change's event applies it: the index holds both changes, and
// the object is current (checkCurrentAfterCommit), so only step 8's drain can have brought the earlier change in.
JITCACHE_TEST(writerCommitAppliesQueuedEvents, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 256);
    BodyKey earlier = writerKey(31);
    BodyKey key = writerKey(32);
    BodyKey later = writerKey(33);

    if (!artifact->placeBody(context, earlier, 31))
        return;
    SectionBytes sections = makeSectionBytes(defaultSizes, 32);
    TestSources sources(sections, Streaming::Image, 11);
    if (!commitOrFail(context, "the commit"_s, writer, testStamp(key), sources))
        return;
    checkCurrentAfterCommit(context, "after applying a queued event"_s, *artifact, *object, { earlier, key }, later, 33);
}

// A commit through an object that has not seen an earlier bump leaves an index holding both changes, and does not take
// its own bump as proof that the object is current: the object's next lookup refreshes, which a body renamed in after the
// commit without a bump shows.
static void checkCommitAfterUnseenBump(TestContext& context, bool withInotify)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(withInotify);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 256);
    BodyKey earlier = writerKey(41);
    BodyKey key = writerKey(42);
    BodyKey later = writerKey(43);
    String mode = withInotify ? "with inotify"_s : "with inotify forced off"_s;

    // Another process's commit, which the object has not looked up since.
    if (!artifact->placeBody(context, earlier, 41))
        return;
    artifact->bump();
    SectionBytes sections = makeSectionBytes(defaultSizes, 42);
    TestSources sources(sections, Streaming::Image, 5);
    if (!commitOrFail(context, makeString("the commit "_s, mode), writer, testStamp(key), sources))
        return;

    if (!artifact->placeBody(context, later, 43))
        return;
    if (!object->token(later))
        JITCACHE_FAIL(makeString(mode, ": the lookup after the commit did not refresh"_s));
    if (!object->token(earlier) || !object->token(key))
        JITCACHE_FAIL(makeString(mode, ": the index lacks the earlier change or the commit"_s));
}

JITCACHE_TEST(writerCommitAfterUnseenBumpWithInotify, No)
{
    checkCommitAfterUnseenBump(context, true);
}

JITCACHE_TEST(writerCommitAfterUnseenBumpWithoutInotify, No)
{
    checkCommitAfterUnseenBump(context, false);
}

// A commit through an object whose last listing failed, which leaves a listing pending, does not take its own bump as
// proof that the object is current, although the bump replaced the last epoch the object saw: the object's next lookup
// lists and finds what the failed listing missed. Inotify is forced off, where a failed listing is what leaves one
// pending; with inotify, only an overflowed queue or a failed read of it does.
JITCACHE_TEST(writerCommitWithAListingPending, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    StoreTesting::setInotify(false);
    StoreTesting::setFallbackListingInterval(Seconds(0));
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 256);
    BodyKey earlier = writerKey(46);
    BodyKey key = writerKey(47);

    // Another process's commit, whose listing the store's fault hook fails: the object records the epoch that listing
    // read, marks a listing pending and keeps its index without the body.
    if (!artifact->placeBody(context, earlier, 46))
        return;
    artifact->bump();
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Listing, EIO, 1 });
    JITCACHE_CHECK(!object->token(earlier));
    StoreTesting::setFault(std::nullopt);

    SectionBytes sections = makeSectionBytes(defaultSizes, 47);
    TestSources sources(sections, Streaming::Image, 5);
    if (!commitOrFail(context, "the commit with a listing pending"_s, writer, testStamp(key), sources))
        return;
    if (!object->token(earlier))
        JITCACHE_FAIL("the first lookup after the commit did not list"_s);
    JITCACHE_CHECK(object->token(key));
}

// A commit whose step 8 finds the pinned bodies/ directory gone, here moved away before the commit renamed its body into
// it, learns nothing: the object's index stays empty, so every later lookup misses without refreshing (container sub-SPEC
// section 6.3). The rename still published the body there, and the epoch still moves.
JITCACHE_TEST(writerCommitIntoAGoneDirectory, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 256);
    BodyKey key = writerKey(56);

    // The pinned descriptors follow the directory, so the commit's rename lands in it under its new name, and the
    // object's queue holds IN_MOVE_SELF until step 8 drains it.
    if (renameat(artifact->cacheFd(), ArtifactNames::bodiesDirectory.characters(), artifact->cacheFd(), "bodies-moved")) {
        JITCACHE_FAIL(makeString("cannot move bodies/ away: "_s, errnoText(errno)));
        return;
    }
    auto epochBefore = artifact->epoch();
    SectionBytes sections = makeSectionBytes(defaultSizes, 56);
    TestSources sources(sections, Streaming::Image, 9);
    if (!commitOrFail(context, "the commit into a moved directory"_s, writer, testStamp(key), sources))
        return;
    auto epochAfter = artifact->epoch();
    JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);
    JITCACHE_CHECK(artifact->bodiesAre({ key }));
    JITCACHE_CHECK(!object->token(key) && !object->indexedBodies());
    artifact->bump();
    JITCACHE_CHECK(!object->token(key) && !object->indexedBodies());
}

// Each fault injection of container sub-SPEC section 8.3 fails its commit at its check, as if the call had failed with
// EIO, and leaves no temporary, no new file and no bump, whether the key has an earlier version, which stays readable,
// or none. The injection fires at the n-th commit alone.
JITCACHE_TEST(writerFaultInjectionLeavesNothing, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 32);
    BodyKey key = writerKey(51);
    BodyKey fresh = writerKey(52);
    uint64_t commits = 0;

    SectionBytes earlier = makeSectionBytes(defaultSizes, 51);
    TestSources earlierSources(earlier, Streaming::Image, 5);
    auto first = commitOrFail(context, "the first commit"_s, writer, testStamp(key), earlierSources);
    ++commits;
    if (!first)
        return;

    SectionBytes later = makeSectionBytes(otherSizes, 53);
    TestSources laterSources(later, Streaming::Image, 5);
    for (ASCIILiteral check : { WriterChecks::create, WriterChecks::write, WriterChecks::reread, WriterChecks::publish }) {
        for (const BodyKey& target : { key, fresh }) {
            String label = makeString(check, target == key ? " over an earlier version"_s : " of a new key"_s);
            writer.setFaultForTesting(ArtifactWriter::FaultForTesting { check, commits + 1 });
            auto epochBefore = artifact->epoch();
            auto failed = writer.commit(testStamp(target), laterSources.commitSections());
            ++commits;
            if (failed || failed.error().check != check) {
                JITCACHE_FAIL(makeString(label, ": the commit did not fail at its check"_s));
                continue;
            }
            if (failed.error().detail != errnoText(EIO))
                JITCACHE_FAIL(makeString(label, ": the detail is "_s, failed.error().detail));
            if (artifact->temporariesInCache())
                JITCACHE_FAIL(makeString(label, ": a temporary is left in cache/"_s));
            if (!artifact->bodiesAre({ key }))
                JITCACHE_FAIL(makeString(label, ": bodies/ holds another file than the earlier version"_s));
            if (artifact->epoch() != epochBefore)
                JITCACHE_FAIL(makeString(label, ": the epoch moved"_s));
        }
        checkPublished(context, makeString("the earlier version after "_s, check), *artifact, *object, key, earlier, *first);
        JITCACHE_CHECK(!object->token(fresh));
    }

    writer.setFaultForTesting(ArtifactWriter::FaultForTesting { WriterChecks::create, commits + 2 });
    auto beforeTheFault = commitOrFail(context, "the commit before the armed one"_s, writer, testStamp(key), laterSources);
    ++commits;
    auto atTheFault = writer.commit(testStamp(key), laterSources.commitSections());
    ++commits;
    JITCACHE_CHECK(!atTheFault && atTheFault.error().check == WriterChecks::create);
    writer.setFaultForTesting(std::nullopt);
    auto afterTheFault = commitOrFail(context, "the commit after the fault"_s, writer, testStamp(key), laterSources);
    ++commits;
    if (beforeTheFault && afterTheFault) {
        JITCACHE_CHECK(beforeTheFault->version != afterTheFault->version);
        checkPublished(context, "the commit after the fault"_s, *artifact, *object, key, later, *afterTheFault);
    }
}

// A streamed source that emits one byte fewer or one more than its size fails at writer.section, with the section's name
// and both counts in the detail and no temporary left; so does a source that fails on its own after streaming every byte,
// and a source in memory whose size is not the size of the bytes it holds.
JITCACHE_TEST(writerRejectsMiscountedSections, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 16);
    BodyKey key = writerKey(61);
    SectionBytes sections = makeSectionBytes(defaultSizes, 61);

    auto expectSectionFailure = [&](const String& label, const TestSources& sources, SectionKind kind, uint64_t declared, uint64_t streamed) {
        auto failed = writer.commit(testStamp(key), sources.commitSections());
        if (failed || failed.error().check != WriterChecks::section) {
            JITCACHE_FAIL(makeString(label, ": the commit did not fail at writer.section"_s));
            return;
        }
        const String& detail = failed.error().detail;
        if (!detail.contains(sectionKindDescription(kind).name) || !detail.contains(makeString("declared "_s, declared, ' '))
            || !detail.contains(makeString("streamed "_s, streamed)))
            JITCACHE_FAIL(makeString(label, ": the detail \""_s, detail, "\" names another section or other counts"_s));
        if (artifact->temporariesInCache())
            JITCACHE_FAIL(makeString(label, ": a temporary is left in cache/"_s));
        if (!artifact->bodiesAre({ }))
            JITCACHE_FAIL(makeString(label, ": a body was published"_s));
    };

    uint64_t imageSize = sections.bytes[static_cast<size_t>(SectionKind::ImageBaseline)].size();
    for (int miscount : { -1, 1 }) {
        TestSources sources(sections, Streaming::Image, 9);
        sources.streamed(SectionKind::ImageBaseline).miscount = miscount;
        expectSectionFailure(makeString("a streamed source "_s, miscount < 0 ? "one byte short"_s : "one byte over"_s), sources,
            SectionKind::ImageBaseline, imageSize, miscount < 0 ? imageSize - 1 : imageSize + 1);
    }

    {
        TestSources sources(sections, Streaming::Image, 9);
        sources.streamed(SectionKind::BakedFactsBaseline).fails = true;
        uint64_t size = sections.bytes[static_cast<size_t>(SectionKind::BakedFactsBaseline)].size();
        expectSectionFailure("a streamed source that fails on its own"_s, sources, SectionKind::BakedFactsBaseline, size, size);
    }

    {
        TestSources sources(sections, Streaming::None, 9);
        auto bytes = sections.bytes[static_cast<size_t>(SectionKind::CBStateBaseline)].span();
        sources.replace(SectionSource { SectionKind::CBStateBaseline, bytes.size() + 1, bytes });
        expectSectionFailure("a source in memory whose size exceeds its bytes"_s, sources, SectionKind::CBStateBaseline, bytes.size() + 1, bytes.size());
    }

    // The writer stays usable: the next commit publishes.
    TestSources sources(sections, Streaming::Image, 9);
    if (auto committed = commitOrFail(context, "the commit after the failures"_s, writer, testStamp(key), sources))
        checkPublished(context, "the commit after the failures"_s, *artifact, *object, key, sections, *committed);
}

// A staging buffer of one byte, and one smaller than every section, still writes the right bytes, whether the sources
// are held in memory, which then go straight to the file, or streamed in pieces smaller or larger than the buffer.
JITCACHE_TEST(writerWritesThroughSmallStagingBuffers, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    static constexpr SectionSizes sizes { 6, 23, 17, 129, 9, 13, 70, 11, 200 };
    uint8_t seed = 70;
    for (size_t stagingBytes : { size_t { 1 }, size_t { 5 } }) {
        ArtifactWriter writer(*object, artifact->lock(), budget.get(), stagingBytes);
        for (Streaming streaming : { Streaming::None, Streaming::Image, Streaming::All }) {
            for (size_t piece : { size_t { 1 }, size_t { 3 }, size_t { 64 } }) {
                ++seed;
                BodyKey key = writerKey(seed);
                SectionBytes sections = makeSectionBytes(sizes, seed);
                TestSources sources(sections, streaming, piece);
                String label = makeString("a staging buffer of "_s, stagingBytes, " bytes, streaming mode "_s, static_cast<unsigned>(streaming),
                    ", pieces of "_s, piece, " bytes"_s);
                if (auto committed = commitOrFail(context, label, writer, testStamp(key), sources))
                    checkPublished(context, label, *artifact, *object, key, sections, *committed);
            }
        }
        JITCACHE_CHECK(budget->chargedBytes() == stagingBytes);
    }
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(!artifact->temporariesInCache());
}

// A refused staging charge fails at budget.limit before any file exists, and the refusal is the budget's own.
JITCACHE_TEST(writerRefusedStagingChargeWritesNothing, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(63);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 64);
    BodyKey key = writerKey(81);
    DirectoryWatch cacheWatch(artifact->cacheFd(), IN_CREATE | IN_MOVED_FROM | IN_DELETE | IN_ONLYDIR);
    auto epochBefore = artifact->epoch();

    SectionBytes sections = makeSectionBytes(defaultSizes, 81);
    TestSources sources(sections, Streaming::Image, 7);
    auto failed = writer.commit(testStamp(key), sources.commitSections());
    JITCACHE_CHECK(!failed && failed.error().check == WriterChecks::budgetLimit);
    JITCACHE_CHECK(budget->hasRefused() && !budget->chargedBytes());
    JITCACHE_CHECK(cacheWatch.isValid() && cacheWatch.takeEvents().isEmpty());
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->bodiesAre({ }));
    JITCACHE_CHECK(artifact->epoch() == epochBefore);
    JITCACHE_CHECK(!object->token(key));
}

// Sections whose bytes no lane would accept, an empty one and a one-byte one among them, are published unchanged, since
// the writer reads no lane format.
JITCACHE_TEST(writerPublishesOpaqueBytes, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), 16);
    BodyKey key = writerKey(91);

    SectionBytes sections;
    sections.bytes[static_cast<size_t>(SectionKind::UCBIdentity)].fill(0xFF, 41);
    sections.bytes[static_cast<size_t>(SectionKind::UCBCore)].fill(0x00, 3);
    sections.bytes[static_cast<size_t>(SectionKind::UCBFeedback)].fill(0xA5, 1);
    sections.bytes[static_cast<size_t>(SectionKind::ImageBaseline)].fill(0xCC, 257);
    // baked-facts.baseline stays empty.
    sections.bytes[static_cast<size_t>(SectionKind::ImageTwinsBaseline)].fill(0x7F, 2);
    sections.bytes[static_cast<size_t>(SectionKind::CBStateBaseline)].fill(0xFE, 5);
    sections.bytes[static_cast<size_t>(SectionKind::CBSummaryBaseline)].fill(0x01, 1);
    sections.bytes[static_cast<size_t>(SectionKind::ICsBaseline)].fill(0xEE, 99);
    TestSources sources(sections, Streaming::Image, 4);
    if (auto committed = commitOrFail(context, "opaque sections"_s, writer, testStamp(key), sources))
        checkPublished(context, "opaque sections"_s, *artifact, *object, key, sections, *committed);
}

// The staging buffer is charged at a writer's first commit and not again, releaseStagingBuffer releases the charge once,
// and destroying a writer releases a buffer still held.
JITCACHE_TEST(writerStagingBufferLifecycle, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    SectionBytes sections = makeSectionBytes(defaultSizes, 101);
    TestSources sources(sections, Streaming::Image, 7);
    {
        ArtifactWriter writer(*object, artifact->lock(), budget.get(), 100);
        JITCACHE_CHECK(!budget->chargedBytes());
        commitOrFail(context, "the first commit"_s, writer, testStamp(writerKey(101)), sources);
        JITCACHE_CHECK(budget->chargedBytes() == 100);
        commitOrFail(context, "the second commit"_s, writer, testStamp(writerKey(102)), sources);
        JITCACHE_CHECK(budget->chargedBytes() == 100);
        writer.releaseStagingBuffer();
        JITCACHE_CHECK(!budget->chargedBytes());
        writer.releaseStagingBuffer();
        JITCACHE_CHECK(!budget->chargedBytes());
    }
    {
        ArtifactWriter writer(*object, artifact->lock(), budget.get(), 100);
        commitOrFail(context, "a second writer's commit"_s, writer, testStamp(writerKey(103)), sources);
        JITCACHE_CHECK(budget->chargedBytes() == 100);
    }
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(budget->peakBytes() == 100);
}

// rewriteSection (container sub-SPEC section 8.3) overwrites bytes inside one section of the current body and commits the
// copy under the envelope's key, tier, L and P with a fresh commit identifier, so the container sees a sound file and the
// key's entry takes a fresh token. The copy is every section's bytes, charged beside the staging buffer while the rewrite
// runs and released after it. A range that ends at the section's end lies inside it; one that passes that end, an offset
// past it and a key without a body fail at writer.rewrite and write nothing.
JITCACHE_TEST(writerRewriteSectionResealsTheBody, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    static constexpr size_t stagingBytes = 32;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), stagingBytes);
    BodyKey key = writerKey(111);

    SectionBytes sections = makeSectionBytes(defaultSizes, 111);
    TestSources sources(sections, Streaming::Image, 7);
    auto committed = commitOrFail(context, "the commit"_s, writer, CommitStamp { key, 321, 45, 1 }, sources);
    if (!committed)
        return;
    uint64_t committedToken = object->token(key);

    static constexpr std::array<uint8_t, 4> patch { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t stateSize = sections.bytes[static_cast<size_t>(SectionKind::CBStateBaseline)].size();
    auto rewritten = writer.rewriteSection(key, SectionKind::CBStateBaseline, stateSize - patch.size(), patch);
    if (!rewritten) {
        JITCACHE_FAIL(makeString("the rewrite failed at "_s, describe(rewritten.error())));
        return;
    }
    JITCACHE_CHECK(rewritten->version != committed->version);
    SectionBytes expected = sections;
    memcpySpan(expected.bytes[static_cast<size_t>(SectionKind::CBStateBaseline)].mutableSpan().last(patch.size()), std::span { patch });
    checkPublished(context, "the rewritten body"_s, *artifact, *object, key, expected, *rewritten, 321, 45);
    uint64_t rewrittenToken = object->token(key);
    JITCACHE_CHECK(rewrittenToken && rewrittenToken != committedToken);
    JITCACHE_CHECK(budget->peakBytes() == stagingBytes + totalBytes(sections));
    JITCACHE_CHECK(budget->chargedBytes() == stagingBytes);

    auto pastTheEnd = writer.rewriteSection(key, SectionKind::CBStateBaseline, stateSize - 2, patch);
    JITCACHE_CHECK(!pastTheEnd && pastTheEnd.error().check == WriterChecks::rewrite);
    auto offsetPastTheEnd = writer.rewriteSection(key, SectionKind::CBStateBaseline, stateSize + 1, { });
    JITCACHE_CHECK(!offsetPastTheEnd && offsetPastTheEnd.error().check == WriterChecks::rewrite);
    auto missing = writer.rewriteSection(writerKey(112), SectionKind::CBStateBaseline, 0, patch);
    JITCACHE_CHECK(!missing && missing.error().check == WriterChecks::rewrite);
    checkPublished(context, "the body after failed rewrites"_s, *artifact, *object, key, expected, *rewritten, 321, 45);
    JITCACHE_CHECK(object->token(key) == rewrittenToken);
    JITCACHE_CHECK(artifact->bodiesAre({ key }));
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(budget->chargedBytes() == stagingBytes);
}

// rewriteSection opens the key's current body through the store's open (container sub-SPEC section 7.2), so each of the
// store's other outcomes fails it at writer.rewrite with nothing written: a body renamed in without a bump, which the index
// lacks, is absent before any system call, so the store's fault hook cannot fire, and it gets no index entry; the fault
// hook on open, transient or not, keeps the key's entry; a damaged body is invalid at its container check; and an unlinked
// file is absent, which erases the key's stale entry.
JITCACHE_TEST(writerRewriteSectionOpensThroughTheStore, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    static constexpr size_t stagingBytes = 32;
    Ref<ProducerBudget> budget = ProducerBudget::create(1 << 20);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), stagingBytes);
    BodyKey key = writerKey(122);
    static constexpr std::array<uint8_t, 2> patch { 0x12, 0x34 };

    SectionBytes sections = makeSectionBytes(defaultSizes, 122);
    TestSources sources(sections, Streaming::Image, 7);
    auto committed = commitOrFail(context, "the commit"_s, writer, testStamp(key), sources);
    if (!committed)
        return;
    uint64_t token = object->token(key);
    auto epochAfterCommit = artifact->epoch();

    BodyKey unindexed = writerKey(121);
    if (!artifact->placeBody(context, unindexed, 121))
        return;
    auto placedFile = readFileAt(artifact->bodiesFd(), bodyFileName(unindexed).data());
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    auto absent = writer.rewriteSection(unindexed, SectionKind::CBStateBaseline, 0, patch);
    StoreTesting::setFault(std::nullopt);
    if (absent || absent.error().check != WriterChecks::rewrite || absent.error().detail.contains("container.io"_s))
        JITCACHE_FAIL("a body the index lacks: the rewrite did not fail as an absent body"_s);
    JITCACHE_CHECK(placedFile && readFileAt(artifact->bodiesFd(), bodyFileName(unindexed).data()) == *placedFile);
    JITCACHE_CHECK(!object->token(unindexed));

    for (int error : { EMFILE, EIO }) {
        StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, error, std::nullopt });
        auto failed = writer.rewriteSection(key, SectionKind::CBStateBaseline, 0, patch);
        StoreTesting::setFault(std::nullopt);
        if (failed || failed.error().check != WriterChecks::rewrite)
            JITCACHE_FAIL(makeString("the store's fault "_s, errnoText(error), " did not fail the rewrite at writer.rewrite"_s));
        else if (error == EIO && !failed.error().detail.contains("container.io"_s))
            JITCACHE_FAIL(makeString("the detail \""_s, failed.error().detail, "\" does not name container.io"_s));
        JITCACHE_CHECK(object->token(key) == token);
    }
    checkPublished(context, "the body after the store's faults"_s, *artifact, *object, key, sections, *committed);

    // A damaged body renamed over the key's name without a bump: open's Full validation meets it under the key's token.
    auto file = readFileAt(artifact->bodiesFd(), bodyFileName(key).data());
    if (!file) {
        JITCACHE_FAIL("the committed body cannot be read"_s);
        return;
    }
    auto layout = validateBody(file->span(), key, writerHeaderDigest(), ValidationMode::Full);
    std::optional<SectionExtent> icsExtent;
    if (layout)
        icsExtent = layout->sections[static_cast<size_t>(SectionKind::ICsBaseline)];
    if (!icsExtent || !icsExtent->size) {
        JITCACHE_FAIL("the committed body has no ICsBaseline bytes to damage"_s);
        return;
    }
    Vector<uint8_t> damaged = *file;
    size_t damagedByte = static_cast<size_t>(icsExtent->offset);
    damaged[damagedByte] = static_cast<uint8_t>(damaged[damagedByte] ^ 0x01);
    if (!artifact->replaceBody(context, key, damaged.span()))
        return;
    auto invalid = writer.rewriteSection(key, SectionKind::CBStateBaseline, 0, patch);
    if (invalid || invalid.error().check != WriterChecks::rewrite || !invalid.error().detail.contains(ContainerChecks::checksum))
        JITCACHE_FAIL("a damaged body: the rewrite did not fail at writer.rewrite naming container.checksum"_s);
    JITCACHE_CHECK(readFileAt(artifact->bodiesFd(), bodyFileName(key).data()) == damaged);

    // The key's file unlinked without a bump, so the key's entry still holds the commit's token until open's ENOENT.
    if (unlinkat(artifact->bodiesFd(), bodyFileName(key).data(), 0)) {
        JITCACHE_FAIL(makeString("cannot unlink the body: "_s, errnoText(errno)));
        return;
    }
    JITCACHE_CHECK(object->token(key) == token);
    auto vanished = writer.rewriteSection(key, SectionKind::CBStateBaseline, 0, patch);
    JITCACHE_CHECK(!vanished && vanished.error().check == WriterChecks::rewrite);
    JITCACHE_CHECK(!object->token(key));

    JITCACHE_CHECK(artifact->bodiesAre({ unindexed }));
    JITCACHE_CHECK(!artifact->temporariesInCache());
    JITCACHE_CHECK(artifact->epoch() == epochAfterCommit);
    JITCACHE_CHECK(budget->chargedBytes() == stagingBytes);
}

// The rewrite's copy is charged before it is allocated: a budget with room for the staging buffer and every section's
// bytes but one refuses it, and the rewrite fails at budget.limit with nothing written.
JITCACHE_TEST(writerRewriteSectionChargesItsCopy, No)
{
    HookScope hooks;
    StoreTesting::setRegistrySharing(false);
    auto artifact = WriterTestArtifact::create(context);
    if (!artifact)
        return;
    RefPtr object = artifact->take(context);
    if (!object)
        return;
    static constexpr size_t stagingBytes = 32;
    BodyKey key = writerKey(131);
    SectionBytes sections = makeSectionBytes(defaultSizes, 131);
    Ref<ProducerBudget> budget = ProducerBudget::create(stagingBytes + totalBytes(sections) - 1);
    ArtifactWriter writer(*object, artifact->lock(), budget.get(), stagingBytes);
    TestSources sources(sections, Streaming::Image, 7);
    auto committed = commitOrFail(context, "the commit"_s, writer, testStamp(key), sources);
    if (!committed)
        return;
    auto epochBefore = artifact->epoch();

    static constexpr std::array<uint8_t, 1> patch { 0x42 };
    auto refused = writer.rewriteSection(key, SectionKind::UCBCore, 0, patch);
    JITCACHE_CHECK(!refused && refused.error().check == WriterChecks::budgetLimit);
    JITCACHE_CHECK(budget->hasRefused() && budget->chargedBytes() == stagingBytes);
    JITCACHE_CHECK(artifact->epoch() == epochBefore);
    JITCACHE_CHECK(!artifact->temporariesInCache());
    checkPublished(context, "the body after the refused copy"_s, *artifact, *object, key, sections, *committed);
}

// The names the shell's flags take (harness sub-SPEC sections 5.1 and 12) map to the writer's hooks and back.
JITCACHE_TEST(writerHookNames, No)
{
    using KillPoint = ArtifactWriter::KillPoint;
    static constexpr std::array<KillPoint, 7> points { KillPoint::BeforeCreate, KillPoint::AfterCreate, KillPoint::MidStream, KillPoint::AfterStream, KillPoint::AfterEnvelope, KillPoint::AfterReread, KillPoint::AfterRename };
    for (KillPoint point : points)
        JITCACHE_CHECK(ArtifactWriter::killPointNamed(StringView { ArtifactWriter::name(point) }) == point);
    JITCACHE_CHECK(ArtifactWriter::name(KillPoint::MidStream) == "mid-stream"_s);
    JITCACHE_CHECK(ArtifactWriter::name(KillPoint::AfterRename) == "after-rename"_s);
    JITCACHE_CHECK(!ArtifactWriter::killPointNamed("after-publish"_s));

    for (ASCIILiteral check : { WriterChecks::create, WriterChecks::write, WriterChecks::reread, WriterChecks::publish })
        JITCACHE_CHECK(ArtifactWriter::faultCheckNamed(StringView { check }) == check);
    JITCACHE_CHECK(!ArtifactWriter::faultCheckNamed(StringView { WriterChecks::section }));
    JITCACHE_CHECK(!ArtifactWriter::faultCheckNamed("writer.unknown"_s));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
