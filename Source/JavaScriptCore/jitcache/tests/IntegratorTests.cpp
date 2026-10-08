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

#include "JITCacheOptions.h"
#include "JITCachePlatform.h"
#include "JITCacheTest.h"
#include "MacroAssembler.h"
#include "Options.h"
#include "ProducerBudget.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/threads/BinarySemaphore.h>

#if OS(LINUX)
#include <elf.h>
#include <link.h>

namespace JSC::JITCache::PlatformInternal {
// Defined in JITCachePlatform.cpp, where processFacts reads each loaded object's build ID through it.
BuildID buildIDOfObject(const struct dl_phdr_info&);
} // namespace JSC::JITCache::PlatformInternal
#endif

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

static bool sameBuildID(const BuildID& a, const BuildID& b)
{
    return a.size == b.size && a.bytes == b.bytes;
}

// The bytes after an ID's size are zero, as the header's build-ID records require.
static bool hasZeroTail(const BuildID& id)
{
    return static_cast<size_t>(id.size) <= id.bytes.size() && std::ranges::all_of(std::span { id.bytes }.subspan(id.size), [](uint8_t byte) {
        return !byte;
    });
}

#if ENABLE(ASSEMBLER) && CPU(X86_64)
// Makes the two protected predicates of N11 callable, so T-CPU compares bits 7 and 8 with the predicates themselves.
struct IntegratorCPUProbe : MacroAssemblerX86_64 {
    using MacroAssemblerX86_64::supportsBMI1;
    using MacroAssemblerX86_64::supportsLZCNT;
};
#endif

#if OS(LINUX)
constexpr std::array<uint8_t, 4> gnuNoteOwner { 'G', 'N', 'U', '\0' };
constexpr std::array<uint8_t, 4> otherNoteOwner { 'G', 'N', 'X', '\0' };
constexpr uint32_t gnuPropertyNoteType = 5; // NT_GNU_PROPERTY_TYPE_0, which older <elf.h> headers lack

// One ELF note as a PT_NOTE segment holds it: the name's size, the descriptor's size and the type, then the name and
// the descriptor, each padded to the alignment from the segment's start.
static void appendNote(Vector<uint8_t>& segment, size_t alignment, uint32_t type, std::span<const uint8_t> name, std::span<const uint8_t> descriptor)
{
    auto pad = [&] {
        while (segment.size() % alignment)
            segment.append(uint8_t { 0 });
    };
    uint32_t nameSize = static_cast<uint32_t>(name.size());
    uint32_t descriptorSize = static_cast<uint32_t>(descriptor.size());
    segment.append(asByteSpan(nameSize));
    segment.append(asByteSpan(descriptorSize));
    segment.append(asByteSpan(type));
    segment.append(name);
    pad();
    segment.append(descriptor);
    pad();
}

static Vector<uint8_t> buildIDNotes(size_t alignment, std::span<const uint8_t> descriptor)
{
    Vector<uint8_t> segment;
    appendNote(segment, alignment, NT_GNU_BUILD_ID, gnuNoteOwner, descriptor);
    return segment;
}

struct CraftedSegment {
    uint32_t type;
    uint64_t alignment;
    std::span<const uint8_t> bytes;
};

// An object loaded at address 0, so each segment's virtual address is where its bytes are.
static BuildID buildIDOfCraftedObject(std::span<const CraftedSegment> segments)
{
    Vector<ElfW(Phdr)> headers;
    for (auto& segment : segments) {
        ElfW(Phdr) header { };
        header.p_type = segment.type;
        header.p_vaddr = reinterpret_cast<uintptr_t>(segment.bytes.data());
        header.p_memsz = segment.bytes.size();
        header.p_filesz = segment.bytes.size();
        header.p_align = segment.alignment;
        headers.append(header);
    }
    struct dl_phdr_info object { };
    object.dlpi_name = "crafted";
    object.dlpi_phdr = headers.span().data();
    object.dlpi_phnum = static_cast<ElfW(Half)>(headers.size());
    return PlatformInternal::buildIDOfObject(object);
}
#endif // OS(LINUX)

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

// T-OPT, in the default option group: every fixed row's required value is the option's effective value, so the check
// finds no row, and the must-match reads return the effective values in the order of the header's option index.
JITCACHE_TEST(integratorFixedOptionsHoldByDefault, No)
{
    for (auto& row : fixedOptionRows()) {
        if (!row.holds())
            JITCACHE_FAIL(makeString("the default differs from options.md: "_s, describeFixedOptionMismatch(row)));
    }
    JITCACHE_CHECK(!checkFixedOptions());

    std::array<bool, numberOfMustMatchOptions> effective { { Options::evalMode(), Options::useExplicitResourceManagement(), Options::useImportDefer() } };
    JITCACHE_CHECK(mustMatchOptionValues() == effective);
    JITCACHE_CHECK(processFacts().mustMatch == effective);
    JITCACHE_CHECK(mustMatchOptionName(0) == "evalMode"_s);
    JITCACHE_CHECK(mustMatchOptionName(1) == "useExplicitResourceManagement"_s);
    JITCACHE_CHECK(mustMatchOptionName(2) == "useImportDefer"_s);
}

// The check returns the first row in options.md's order that differs, and the mismatch names both values; a Double row
// compares by value.
JITCACHE_TEST_WITH_OPTIONS(integratorFixedOptionCheckNamesFirstDifference, No, "--thresholdForJITSoon=99 --quickDFGTierUpThresholdFactor=0.3")
{
    Vector<String> differing;
    for (auto& row : fixedOptionRows()) {
        if (!row.holds())
            differing.append(describeFixedOptionMismatch(row));
    }
    JITCACHE_CHECK(differing.size() == 2);
    JITCACHE_CHECK(differing.size() == 2 && differing[0] == "thresholdForJITSoon: required 100, effective 99"_s);
    JITCACHE_CHECK(differing.size() == 2 && differing[1] == "quickDFGTierUpThresholdFactor: required 0.2, effective 0.3"_s);
    const FixedOptionRow* first = checkFixedOptions();
    JITCACHE_CHECK(first && first->name == "thresholdForJITSoon"_s);
}

// T-CPU: bit i of the CPU feature vector is the answer of predicate i of N11, and the bits after the last are zero.
JITCACHE_TEST(integratorCPUFeatureVector, No)
{
#if ENABLE(ASSEMBLER) && CPU(X86_64)
    std::array predicates {
        MacroAssemblerX86_64::supportsSSE3(),
        MacroAssemblerX86_64::supportsSupplementalSSE3(),
        MacroAssemblerX86_64::supportsSSE4_1(),
        MacroAssemblerX86_64::supportsFloatingPointRounding(),
        MacroAssemblerX86_64::supportsCountPopulation(),
        MacroAssemblerX86_64::supportsAVX(),
        MacroAssemblerX86_64::supportsAVX2(),
        IntegratorCPUProbe::supportsLZCNT(),
        IntegratorCPUProbe::supportsBMI1(),
        MacroAssemblerX86_64::supportsFloat16(),
    };
#elif ENABLE(ASSEMBLER) && CPU(ARM64)
    std::array predicates {
        MacroAssemblerARM64::supportsFloatingPointRounding(),
        MacroAssemblerARM64::supportsCountPopulation(),
        MacroAssemblerARM64::supportsFloat16(),
        MacroAssemblerARM64::supportsDotProd(),
        MacroAssemblerARM64::supportsLSE(),
        MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(),
        MacroAssemblerARM64::supportsRoundFloatToIntegerFloat(),
        MacroAssemblerARM64::supportsSHA3(),
    };
#else
    std::array<bool, 0> predicates { };
#endif
    uint64_t features = processFacts().cpuFeatures;
    for (size_t bit = 0; bit < predicates.size(); ++bit) {
        bool isSet = features & (uint64_t { 1 } << bit);
        if (isSet != predicates[bit])
            JITCACHE_FAIL(makeString("CPU feature bit "_s, bit, isSet ? " is set and its predicate is false"_s : " is clear and its predicate is true"_s));
    }
    JITCACHE_CHECK(!(features >> predicates.size()));
}

// T-BUILDID: the test executable has a build ID of 1 to 64 bytes. While removeMainBuildIDForTesting is set, the facts
// report the main executable without an ID and nothing else changes.
JITCACHE_TEST(integratorBuildIDOfTestExecutable, No)
{
    ProcessFacts facts = processFacts();
    JITCACHE_CHECK(facts.mainExecutable.size >= 1 && static_cast<size_t>(facts.mainExecutable.size) <= facts.mainExecutable.bytes.size());
    JITCACHE_CHECK(hasZeroTail(facts.mainExecutable));
    JITCACHE_CHECK(!facts.engineObject || hasZeroTail(*facts.engineObject));

    removeMainBuildIDForTesting(true);
    ProcessFacts removed = processFacts();
    removeMainBuildIDForTesting(false);
    JITCACHE_CHECK(sameBuildID(removed.mainExecutable, BuildID { }));
    JITCACHE_CHECK(removed.engineObject.has_value() == facts.engineObject.has_value());
    JITCACHE_CHECK(!facts.engineObject || sameBuildID(*removed.engineObject, *facts.engineObject));
    JITCACHE_CHECK(removed.mustMatch == facts.mustMatch);
    JITCACHE_CHECK(removed.cpuFeatures == facts.cpuFeatures);
    JITCACHE_CHECK(sameBuildID(processFacts().mainExecutable, facts.mainExecutable));
}

#if OS(LINUX)
// T-BUILDID: a crafted object's ID is the descriptor of the first GNU build-ID note in its PT_NOTE segments, read past
// notes of other types and owners, in segments of either note alignment; later notes and segments do not replace it.
JITCACHE_TEST(integratorBuildIDFromCraftedNotes, No)
{
    Vector<uint8_t> loaded = patternBytes(64, 0);
    Vector<uint8_t> descriptor = patternBytes(20, 1);
    Vector<uint8_t> properties;
    appendNote(properties, 8, gnuPropertyNoteType, gnuNoteOwner, patternBytes(16, 2).span());
    Vector<uint8_t> notes;
    appendNote(notes, 4, NT_GNU_ABI_TAG, gnuNoteOwner, patternBytes(16, 3).span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, otherNoteOwner, patternBytes(20, 4).span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, gnuNoteOwner, descriptor.span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, gnuNoteOwner, patternBytes(20, 5).span());
    Vector<uint8_t> laterNotes = buildIDNotes(4, patternBytes(20, 6).span());
    std::array object {
        CraftedSegment { PT_LOAD, 4096, loaded.span() },
        CraftedSegment { PT_NOTE, 8, properties.span() },
        CraftedSegment { PT_NOTE, 4, notes.span() },
        CraftedSegment { PT_NOTE, 4, laterNotes.span() },
    };
    BuildID id = buildIDOfCraftedObject(object);
    JITCACHE_CHECK(static_cast<size_t>(id.size) == descriptor.size());
    JITCACHE_CHECK(equalSpans(std::span { id.bytes }.first(id.size), descriptor.span()));
    JITCACHE_CHECK(hasZeroTail(id));

    Vector<uint8_t> eightAlignedNotes = buildIDNotes(8, descriptor.span());
    std::array eightAligned { CraftedSegment { PT_NOTE, 8, eightAlignedNotes.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(eightAligned), id));
}

// T-BUILDID: a descriptor of 1 or 64 bytes is the ID. A descriptor of 0 or 65 bytes, a note that overruns its segment,
// an object without PT_NOTE segments and one whose notes hold no GNU build ID give none.
JITCACHE_TEST(integratorBuildIDBounds, No)
{
    for (size_t size : std::array<size_t, 2> { 1, 64 }) {
        Vector<uint8_t> descriptor = patternBytes(size, 7);
        Vector<uint8_t> notes = buildIDNotes(4, descriptor.span());
        std::array object { CraftedSegment { PT_NOTE, 4, notes.span() } };
        BuildID id = buildIDOfCraftedObject(object);
        JITCACHE_CHECK(static_cast<size_t>(id.size) == size);
        JITCACHE_CHECK(equalSpans(std::span { id.bytes }.first(id.size), descriptor.span()));
        JITCACHE_CHECK(hasZeroTail(id));
    }
    for (size_t size : std::array<size_t, 2> { 0, 65 }) {
        Vector<uint8_t> notes = buildIDNotes(4, patternBytes(size, 8).span());
        std::array object { CraftedSegment { PT_NOTE, 4, notes.span() } };
        JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(object), BuildID { }));
    }

    Vector<uint8_t> truncated = buildIDNotes(4, patternBytes(20, 9).span());
    truncated.shrink(truncated.size() - 4);
    std::array overrun { CraftedSegment { PT_NOTE, 4, truncated.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(overrun), BuildID { }));

    Vector<uint8_t> loaded = patternBytes(64, 10);
    std::array withoutNotes { CraftedSegment { PT_LOAD, 4096, loaded.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(withoutNotes), BuildID { }));

    Vector<uint8_t> otherNotes;
    appendNote(otherNotes, 4, NT_GNU_ABI_TAG, gnuNoteOwner, patternBytes(16, 11).span());
    appendNote(otherNotes, 4, NT_GNU_BUILD_ID, otherNoteOwner, patternBytes(20, 12).span());
    std::array withoutBuildID {
        CraftedSegment { PT_LOAD, 4096, loaded.span() },
        CraftedSegment { PT_NOTE, 4, otherNotes.span() },
    };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(withoutBuildID), BuildID { }));

    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject({ }), BuildID { }));
}
#endif // OS(LINUX)

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
