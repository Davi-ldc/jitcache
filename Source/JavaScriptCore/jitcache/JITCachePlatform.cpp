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
#include "JITCachePlatform.h"

#include "ImageSupport.h"
#include "JITCacheOptions.h"
#include "MacroAssembler.h"
#include <atomic>
#include <mutex>
#include <type_traits>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/UnalignedAccess.h>

#if OS(LINUX)
#include <elf.h>
#include <link.h>
#endif

// The process facts the header records (SPEC-integrator.md section 5.2), and the CRC-32C the container checksums with.

namespace JSC::JITCache {

namespace PlatformInternal {

static_assert(std::is_same_v<decltype(ProcessFacts::mustMatch), std::array<bool, numberOfMustMatchOptions>>);

#if OS(LINUX)

// An ELF note starts with three 32-bit words in the object's byte order: the name's size, the descriptor's size and
// the type. The name follows, then the descriptor, each padded to the note alignment.
static constexpr size_t noteHeaderSize = 3 * sizeof(uint32_t);
static constexpr std::array<uint8_t, 4> gnuNoteOwner { 'G', 'N', 'U', '\0' };

// A segment aligned to 8 bytes holds notes aligned to 8, as .note.gnu.property is, and one aligned to 4 or less holds
// notes aligned to 4. Readers such as glibc and binutils accept no other note alignment, so neither does this one.
static std::optional<size_t> noteAlignment(uint64_t segmentAlignment)
{
    if (segmentAlignment == 8)
        return 8;
    if (segmentAlignment <= 4)
        return 4;
    return std::nullopt;
}

static uint32_t noteWord(std::span<const uint8_t> segment, size_t offset)
{
    return WTF::unalignedLoad<uint32_t>(segment.subspan(offset, sizeof(uint32_t)).data());
}

// The header holds an ID of 1 to 64 bytes; a descriptor of any other length leaves the object without one.
static BuildID buildIDFromDescriptor(std::span<const uint8_t> descriptor)
{
    BuildID id;
    if (descriptor.empty() || descriptor.size() > id.bytes.size())
        return id;
    memcpySpan(std::span { id.bytes }, descriptor);
    id.size = static_cast<uint8_t>(descriptor.size());
    return id;
}

// The ID of the first NT_GNU_BUILD_ID note whose owner is GNU in one PT_NOTE segment: nullopt when the segment holds no
// such note before its end or before a note that overruns it.
static std::optional<BuildID> firstBuildIDNote(std::span<const uint8_t> segment, size_t alignment)
{
    size_t offset = 0;
    while (segment.size() - offset >= noteHeaderSize) {
        uint32_t nameSize = noteWord(segment, offset);
        uint32_t descriptorSize = noteWord(segment, offset + sizeof(uint32_t));
        uint32_t type = noteWord(segment, offset + 2 * sizeof(uint32_t));
        // 64-bit arithmetic: the offsets stay below the segment's size plus twice 2^32, so nothing overflows.
        uint64_t nameOffset = static_cast<uint64_t>(offset) + noteHeaderSize;
        uint64_t descriptorOffset = roundUpToMultipleOf<uint64_t>(alignment, nameOffset + nameSize);
        if (descriptorOffset > segment.size() || descriptorSize > segment.size() - descriptorOffset)
            return std::nullopt;
        auto name = segment.subspan(static_cast<size_t>(nameOffset), nameSize);
        if (type == NT_GNU_BUILD_ID && equalSpans(name, std::span { gnuNoteOwner }))
            return buildIDFromDescriptor(segment.subspan(static_cast<size_t>(descriptorOffset), descriptorSize));
        uint64_t nextOffset = roundUpToMultipleOf<uint64_t>(alignment, descriptorOffset + descriptorSize);
        if (nextOffset >= segment.size())
            return std::nullopt;
        offset = static_cast<size_t>(nextOffset);
    }
    return std::nullopt;
}

// The build ID of one loaded object: the first GNU build-ID note in its PT_NOTE segments, in program-header order, or
// no ID. External linkage, because T-BUILDID reads crafted objects through it.
BuildID buildIDOfObject(const struct dl_phdr_info& object)
{
    for (auto& header : unsafeMakeSpan(object.dlpi_phdr, object.dlpi_phnum)) {
        if (header.p_type != PT_NOTE)
            continue;
        auto alignment = noteAlignment(header.p_align);
        if (!alignment)
            continue;
        auto* start = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(object.dlpi_addr + header.p_vaddr));
        if (auto id = firstBuildIDNote(unsafeMakeSpan(start, static_cast<size_t>(header.p_memsz)), *alignment))
            return *id;
    }
    return { };
}

static bool loadedSegmentsContain(const struct dl_phdr_info& object, uintptr_t address)
{
    for (auto& header : unsafeMakeSpan(object.dlpi_phdr, object.dlpi_phnum)) {
        if (header.p_type != PT_LOAD)
            continue;
        uintptr_t start = static_cast<uintptr_t>(object.dlpi_addr + header.p_vaddr);
        if (address >= start && address - start < header.p_memsz)
            return true;
    }
    return false;
}

// An address inside the engine object: the code-symbol anchor, since the header's ID must cover the object its offsets
// are measured in (SPEC-image.md R-INT-8). Builds without the JIT have no anchor, and any engine function names the
// same object.
static uintptr_t engineAddress()
{
#if ENABLE(JIT)
    return reinterpret_cast<uintptr_t>(&codeSymbolAnchor);
#else
    return reinterpret_cast<uintptr_t>(&processFacts);
#endif
}

// The main executable is the first object dl_iterate_phdr lists; the engine object is the one whose loaded segments
// hold the engine's address, and it gets an ID of its own only when it is not the main executable.
static void computeBuildIDs(ProcessFacts& facts)
{
    struct Search {
        uintptr_t engine;
        ProcessFacts& facts;
        bool sawMainExecutable;
        bool foundEngine;
    } search { engineAddress(), facts, false, false };
    dl_iterate_phdr([](struct dl_phdr_info* object, size_t, void* data) -> int {
        auto& search = *static_cast<Search*>(data);
        bool isMainExecutable = !search.sawMainExecutable;
        search.sawMainExecutable = true;
        if (isMainExecutable)
            search.facts.mainExecutable = buildIDOfObject(*object);
        if (!loadedSegmentsContain(*object, search.engine))
            return 0;
        if (!isMainExecutable)
            search.facts.engineObject = buildIDOfObject(*object);
        search.foundEngine = true;
        return 1;
    }, &search);
    // An engine that no listed object holds cannot be identified, so it counts as an object of its own without an ID,
    // which start rejects.
    if (!search.foundEngine)
        facts.engineObject = BuildID { };
}

#endif // OS(LINUX)

// Bit i of the CPU feature vector is the answer of predicate i (SPEC-integrator.md section 5.2 and N11).
#if ENABLE(ASSEMBLER) && CPU(X86_64)
static constexpr std::array<bool (*)(), 10> cpuFeaturePredicates { {
    [] { return MacroAssemblerX86_64::supportsSSE3(); },
    [] { return MacroAssemblerX86_64::supportsSupplementalSSE3(); },
    [] { return MacroAssemblerX86_64::supportsSSE4_1(); },
    [] { return MacroAssemblerX86_64::supportsFloatingPointRounding(); },
    [] { return MacroAssemblerX86_64::supportsCountPopulation(); },
    [] { return MacroAssemblerX86_64::supportsAVX(); },
    [] { return MacroAssemblerX86_64::supportsAVX2(); },
    // supportsLZCNT and supportsBMI1 are protected; these are what they return once the features are collected.
    [] { return MacroAssemblerX86_64::s_lzcntCheckState == MacroAssemblerX86_64::CPUIDCheckState::Set; },
    [] { return MacroAssemblerX86_64::s_bmi1CheckState == MacroAssemblerX86_64::CPUIDCheckState::Set; },
    [] { return MacroAssemblerX86_64::supportsFloat16(); },
} };
#elif ENABLE(ASSEMBLER) && CPU(ARM64)
static constexpr std::array<bool (*)(), 8> cpuFeaturePredicates { {
    [] { return MacroAssemblerARM64::supportsFloatingPointRounding(); },
    [] { return MacroAssemblerARM64::supportsCountPopulation(); },
    [] { return MacroAssemblerARM64::supportsFloat16(); },
    [] { return MacroAssemblerARM64::supportsDotProd(); },
    [] { return MacroAssemblerARM64::supportsLSE(); },
    [] { return MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(); },
    [] { return MacroAssemblerARM64::supportsRoundFloatToIntegerFloat(); },
    [] { return MacroAssemblerARM64::supportsSHA3(); },
} };
#else
static constexpr std::array<bool (*)(), 0> cpuFeaturePredicates { };
#endif
static_assert(cpuFeaturePredicates.size() <= 64);

static uint64_t computeCPUFeatures()
{
#if ENABLE(ASSEMBLER) && CPU(X86_64)
    MacroAssemblerX86_64::collectCPUFeatures();
#endif
    uint64_t features = 0;
    for (size_t bit = 0; bit < cpuFeaturePredicates.size(); ++bit) {
        if (cpuFeaturePredicates[bit]())
            features |= uint64_t { 1 } << bit;
    }
    return features;
}

// Computed once per process. Without ELF objects there is no build ID, which start rejects.
static const ProcessFacts& computedProcessFacts()
{
    static ProcessFacts facts;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
#if OS(LINUX)
        computeBuildIDs(facts);
#endif
        facts.mustMatch = mustMatchOptionValues();
        facts.cpuFeatures = computeCPUFeatures();
    });
    return facts;
}

#if ENABLE(JITCACHE_TWINS)
static std::atomic<bool> mainBuildIDRemovedForTesting { false };
#endif

} // namespace PlatformInternal

ProcessFacts processFacts()
{
    ProcessFacts facts = PlatformInternal::computedProcessFacts();
#if ENABLE(JITCACHE_TWINS)
    if (PlatformInternal::mainBuildIDRemovedForTesting.load())
        facts.mainExecutable = { };
#endif
    return facts;
}

uint32_t crc32cExtend(uint32_t state, std::span<const uint8_t> bytes)
{
    return JSC::crc32c(state, bytes);
}

#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool removed)
{
    PlatformInternal::mainBuildIDRemovedForTesting.store(removed);
}
#endif

} // namespace JSC::JITCache
