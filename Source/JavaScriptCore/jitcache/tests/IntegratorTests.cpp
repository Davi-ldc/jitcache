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

#include "JITCacheTest.h"
#include "ProducerBudget.h"
#include "ValidatedBody.h"
#include <array>
#include <atomic>
#include <bit>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/threads/BinarySemaphore.h>

// The integrator's C++ tests (SPEC-integrator.md section 15.1).

namespace JSC::JITCache::Tests {

namespace IntegratorTestsInternal {

constexpr size_t maximumSize = std::numeric_limits<size_t>::max();

// A body key built from its canonical bytes (SPEC-ucb.md section 3.1), as the store's hash traits build theirs, so the
// tests need none of the UCB lane's key functions: version 1, a program body, call specialization, no mode bits, and an
// identity digest the seed varies.
static BodyKey testKey(uint8_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t i = 8; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(seed + i);
    return std::bit_cast<BodyKey>(bytes);
}

static Vector<uint8_t> patternBytes(size_t size, uint8_t seed)
{
    Vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(seed * 31 + i * 7);
    return bytes;
}

static bool isEightByteAligned(std::span<const uint8_t> span)
{
    return !(reinterpret_cast<uintptr_t>(span.data()) % 8);
}

} // namespace IntegratorTestsInternal

using namespace IntegratorTestsInternal;

// T-BUDGET, on one thread: a charge up to the limit succeeds, one past it is refused without changing the total, the
// refusal is sticky, and the peak is the largest total.
JITCACHE_TEST(integratorBudgetLimitAndPeak, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(100);
    JITCACHE_CHECK(budget->limitBytes() == 100);
    JITCACHE_CHECK(budget->tryCharge(30));
    JITCACHE_CHECK(budget->tryCharge(50));
    JITCACHE_CHECK(budget->chargedBytes() == 80);
    budget->release(50);
    JITCACHE_CHECK(budget->chargedBytes() == 30);
    JITCACHE_CHECK(budget->peakBytes() == 80);
    JITCACHE_CHECK(budget->tryCharge(70));
    JITCACHE_CHECK(budget->chargedBytes() == 100);
    JITCACHE_CHECK(budget->peakBytes() == 100);
    JITCACHE_CHECK(!budget->hasRefused());

    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 100);

    budget->release(100);
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->peakBytes() == 100);
}

// T-BUDGET: a charge whose sum overflows is refused, even under a limit no sum can exceed.
JITCACHE_TEST(integratorBudgetRefusesOverflow, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(maximumSize);
    JITCACHE_CHECK(budget->tryCharge(16));
    JITCACHE_CHECK(!budget->tryCharge(maximumSize));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 16);
    budget->release(16);
    JITCACHE_CHECK(!budget->chargedBytes());

    Ref<ProducerBudget> ended = ProducerBudget::create(maximumSize);
    ended->refuseFurtherCharges();
    JITCACHE_CHECK(ended->hasRefused());
    JITCACHE_CHECK(!ended->tryCharge(1));
}

// T-BUDGET: createUnlimited never refuses, whatever is charged and whether or not production ends.
JITCACHE_TEST(integratorBudgetUnlimited, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->chargedBytes() == maximumSize / 2 * 2);
    budget->release(maximumSize / 2);
    budget->release(maximumSize / 2);
    JITCACHE_CHECK(!budget->chargedBytes());
    budget->refuseFurtherCharges();
    JITCACHE_CHECK(budget->tryCharge(1));
    JITCACHE_CHECK(!budget->hasRefused());
    budget->release(1);

    Ref<ProducerBudget> saturated = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(!saturated->hasRefused());
}

// T-BUDGET: eight threads charge and release concurrently, each charging more than it releases, until the limit
// refuses. The total never exceeds the limit, the refusal is sticky on every thread, the peak is the largest total, and
// the balance returns to zero once every thread has released what it holds.
JITCACHE_TEST(integratorBudgetConcurrentCharges, No)
{
    constexpr size_t limit = 1 * MB;
    constexpr size_t largestCharge = 4096;
    constexpr unsigned numberOfThreads = 8;
    constexpr unsigned maximumIterations = 1000000;
    Ref<ProducerBudget> budget = ProducerBudget::create(limit);

    std::atomic<bool> exceededLimit { false };
    std::atomic<bool> refusalWasNotSticky { false };
    std::atomic<bool> neverRefused { false };
    std::atomic<size_t> largestSampledTotal { 0 };

    Vector<Ref<Thread>> threads;
    for (unsigned t = 0; t < numberOfThreads; ++t) {
        threads.append(Thread::create("JITCache budget test"_s, [&, t] {
            Vector<size_t> held;
            uint32_t random = 0x9e3779b9u * (t + 1);
            bool refused = false;
            for (unsigned iteration = 0; iteration < maximumIterations; ++iteration) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                size_t bytes = 1 + random % largestCharge;
                if (!budget->tryCharge(bytes)) {
                    refused = true;
                    if (budget->tryCharge(1)) {
                        refusalWasNotSticky = true;
                        budget->release(1);
                    }
                    break;
                }
                held.append(bytes);
                size_t total = budget->chargedBytes();
                if (total > limit)
                    exceededLimit = true;
                size_t largest = largestSampledTotal.load();
                while (largest < total && !largestSampledTotal.compare_exchange_weak(largest, total)) { }
                // One release per three charges keeps the total climbing toward the limit.
                if (iteration % 3 == 2)
                    budget->release(held.takeLast());
            }
            if (!refused)
                neverRefused = true;
            for (size_t bytes : held)
                budget->release(bytes);
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();

    JITCACHE_CHECK(!neverRefused.load());
    JITCACHE_CHECK(!exceededLimit.load());
    JITCACHE_CHECK(!refusalWasNotSticky.load());
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(budget->peakBytes() <= limit);
    JITCACHE_CHECK(budget->peakBytes() >= largestSampledTotal.load());
    // The first refusal met a total within one charge of the limit, and some charge reached that total.
    JITCACHE_CHECK(budget->peakBytes() > limit - largestCharge);
}

// T-BODY: createForTesting copies each section to an 8-byte-aligned address, leaves absent kinds empty, and keeps the
// key, the version and the highest tier of the kinds it was given.
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

// T-BODY: onDestroy runs exactly once, from the destructor, on the thread that drops the last reference.
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
