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

#pragma once

#include "UCBKeys.h"
#include "ValidatedBody.h"
#include <array>
#include <expected>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/ScopedLambda.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

class OpenedArtifact;
class ProducerBudget;
class ProducerLock;

// The capture glue hands an accepted capture to the writer as an ordered list of section sources (SPEC-integrator.md
// section 6.3). Each source is a section kind, a size, and either the section's bytes in memory or a function through
// which an object streams them, so the writer sees no lane type and reads no lane format.

using SectionSink = ScopedLambda<bool(std::span<const uint8_t>)>; // false once the commit has failed; the source then stops

struct SectionSource {
    SectionKind kind;
    uint64_t size; // the bytes the source promises
    std::span<const uint8_t> bytes; // a section held in memory: size == bytes.size()
    bool (*stream)(const void* object, const SectionSink&) { nullptr }; // or a section its object streams
    const void* object { nullptr };

    static SectionSource inMemory(SectionKind, std::span<const uint8_t>);
    static SectionSource streamed(SectionKind, uint64_t size, bool (*)(const void*, const SectionSink&), const void* object);
};

class CommitSections { // copies its sources; borrows the bytes and objects they name, which outlive the commit
public:
    explicit CommitSections(std::span<const SectionSource>); // at most numberOfSectionKinds, kinds strictly increasing; copied without allocating
    std::span<const SectionSource> sources() const LIFETIME_BOUND;

private:
    std::array<SectionSource, numberOfSectionKinds> m_sources;
    unsigned m_count { 0 };
};

struct CommitStamp {
    BodyKey key;
    uint32_t llintThreshold; // L: UCB envelopeLLIntThreshold (THREAD Maintenance)
    uint32_t counterProgress; // P: the CB lane's counterProgress (SPEC-cb.md section 4.3), 0 when the counter does not travel
    uint8_t highestTier; // 1
};
struct CommitResult {
    uint64_t version;
    uint64_t fileSize;
};
struct CommitFailure {
    ASCIILiteral check; // budget.limit, or a writer.* step of container sub-SPEC section 8.2
    String detail;
};

// The writer (container sub-SPEC section 8). Each producing VMState owns one, over the shared OpenedArtifact, the
// ProducerLock and the budget it holds; the state destroys the writer before all three. A commit runs on the VM thread
// inside the capture: it lays the sources out, streams, checksums and counts their bytes through the staging buffer into
// a temporary in cache/, rereads and validates the file in Full mode, publishes it with renameat, bumps the epoch and
// updates the index. Any failure after the temporary exists unlinks it.
class ArtifactWriter {
    WTF_MAKE_NONCOPYABLE(ArtifactWriter);
    WTF_MAKE_TZONE_ALLOCATED(ArtifactWriter);
public:
    ArtifactWriter(OpenedArtifact&, ProducerLock&, ProducerBudget&, size_t stagingBytes);
    ~ArtifactWriter(); // frees the staging buffer and releases its charge
    std::expected<CommitResult, CommitFailure> commit(const CommitStamp&, const CommitSections&);
    void releaseStagingBuffer(); // the end of production

#if ENABLE(JITCACHE_TWINS)
    // Container sub-SPEC section 8.3. n counts this writer's commits from 1.
    struct FaultForTesting {
        ASCIILiteral check; // writer.create, writer.write, writer.reread or writer.publish
        uint64_t n;
    };
    void setFaultForTesting(std::optional<FaultForTesting>);
    // Harness sub-SPEC section 12: raise(SIGKILL) at the point of the n-th commit.
    enum class KillPoint : uint8_t { BeforeCreate, AfterCreate, MidStream, AfterStream, AfterEnvelope, AfterReread, AfterRename };
    struct KillForTesting {
        KillPoint point;
        uint64_t n;
    };
    void setKillForTesting(std::optional<KillForTesting>);
    std::expected<CommitResult, CommitFailure> rewriteSection(const BodyKey&, SectionKind, uint64_t offset, std::span<const uint8_t> bytes);
#endif

private:
    OpenedArtifact& m_artifact;
    ProducerLock& m_producerLock;
    ProducerBudget& m_budget;
    const size_t m_stagingBytes; // at least one byte
    MallocSpan<uint8_t> m_staging; // allocated and charged at the first commit; empty before it and once released
#if ENABLE(JITCACHE_TWINS)
    uint64_t m_commits { 0 }; // commits started, which the fault injection's and the kill point's n count
    std::optional<FaultForTesting> m_faultForTesting;
    std::optional<KillForTesting> m_killForTesting;
#endif
};

} // namespace JSC::JITCache
