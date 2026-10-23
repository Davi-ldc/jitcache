#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "MacroAssembler.h"
#include "Options.h"
#include "jitcache/store/JITCacheOptions.h"
#include "jitcache/store/JITCachePlatform.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestPack.h"
#include <algorithm>
#include <array>
#include <span>
#include <wtf/HexNumber.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <elf.h>
#include <link.h>
#endif

// Tests of JITCachePlatform and JITCacheOptions (store/JITCachePlatform.h, store/JITCacheOptions.h and their .cpp): the
// fixed options start requires, the must-match options the header records, the CPU feature vector, the build IDs
// processFacts reads from the loaded objects, crafted ones included, and the CRC-32C on each of its paths.

#if OS(LINUX)
namespace JSC::JITCache::PlatformInternal {
// Defined in JITCachePlatform.cpp, where processFacts reads each loaded object's build ID through it.
BuildID buildIDOfObject(const struct dl_phdr_info&);
} // namespace JSC::JITCache::PlatformInternal
#endif

namespace JSC::JITCache::Tests {

namespace PlatformTestsInternal {

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

// CRC-32C of RFC 3720 bit by bit: the reflected Castagnoli polynomial, initial value and final XOR 0xFFFFFFFF.
static uint32_t bitwiseCRC32C(std::span<const uint8_t> bytes)
{
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78 : crc >> 1;
    }
    return ~crc;
}

// The ways a test can run the CRC-32C: crc32cExtend, which takes the CPU's instructions where it has them, and the byte
// table it falls back to.
struct CRC32CPath {
    ASCIILiteral name;
    uint32_t (*extend)(uint32_t, std::span<const uint8_t>);
};
static constexpr std::array<CRC32CPath, 2> crc32cPaths { {
    { "crc32cExtend"_s, crc32cExtend },
    { "the byte table"_s, crc32cExtendWithTableForTesting },
} };

#if ENABLE(ASSEMBLER) && CPU(X86_64)
// Makes the two protected predicates callable, so the feature-vector test compares bits 7 and 8 with the predicates
// themselves.
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

} // namespace PlatformTestsInternal

using namespace PlatformTestsInternal;

// In the default option group: every fixed row's required value is the option's effective value, so the check finds no
// row, and the must-match reads return the effective values in the order of the header's option index.
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

// The check returns the first row of fixedOptionRows that differs, and the mismatch names both values; a Double row
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

// Bit i of the CPU feature vector is predicate i's answer, in the order processFacts gives them, and the bits after the
// last are zero.
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

// The test executable has a build ID of 1 to 64 bytes. While removeMainBuildIDForTesting is set, the facts report the main
// executable without an ID and nothing else changes.
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
// A crafted object's ID is the descriptor of the first GNU build-ID note in its PT_NOTE segments, read past notes of other
// types and owners, in segments of either note alignment; later notes and segments do not replace it.
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

// A descriptor of 1 or 64 bytes is the ID. A descriptor of 0 or 65 bytes, a note that overruns its segment, an object
// without PT_NOTE segments and one whose notes hold no GNU build ID give none.
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

// Every CRC-32C path gives the RFC 3720 vectors, whole and fed byte by byte, and agrees with the bitwise reference over
// random buffers, lengths and alignments, fed whole and in random pieces. The vectors pin the polynomial, the initial
// value and the final XOR, so every checksum the container writes keeps its bytes whichever path computes it.
JITCACHE_TEST(integratorCRC32C, No)
{
    auto checkVector = [&](ASCIILiteral name, std::span<const uint8_t> bytes, uint32_t expected) {
        if (bitwiseCRC32C(bytes) != expected)
            JITCACHE_FAIL(makeString("the bitwise reference of "_s, name, " is "_s, hex(bitwiseCRC32C(bytes)), ", expected "_s, hex(expected)));
        for (auto& path : crc32cPaths) {
            uint32_t whole = ~path.extend(~0u, bytes);
            if (whole != expected)
                JITCACHE_FAIL(makeString(path.name, " gives "_s, name, ' ', hex(whole), ", expected "_s, hex(expected)));
            uint32_t state = ~0u;
            for (size_t index = 0; index < bytes.size(); ++index)
                state = path.extend(state, bytes.subspan(index, 1));
            if (~state != expected)
                JITCACHE_FAIL(makeString(path.name, " gives "_s, name, " fed byte by byte "_s, hex(~state)));
        }
    };

    static constexpr std::array<uint8_t, 9> checkString { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    checkVector("\"123456789\""_s, std::span { checkString }, 0xE3069283);
    std::array<uint8_t, 32> pattern { };
    checkVector("32 zero bytes"_s, std::span { pattern }, 0x8A9136AA);
    pattern.fill(0xFF);
    checkVector("32 0xFF bytes"_s, std::span { pattern }, 0x62A8AB43);
    for (size_t index = 0; index < pattern.size(); ++index)
        pattern[index] = static_cast<uint8_t>(index);
    checkVector("the bytes 0 to 31"_s, std::span { pattern }, 0x46DD794E);
    for (size_t index = 0; index < pattern.size(); ++index)
        pattern[index] = static_cast<uint8_t>(pattern.size() - 1 - index);
    checkVector("the bytes 31 down to 0"_s, std::span { pattern }, 0x113FDB5C);
    for (auto& path : crc32cPaths) {
        JITCACHE_CHECK(!~path.extend(~0u, { }));
        JITCACHE_CHECK(path.extend(0x12345678, { }) == 0x12345678);
    }

    static constexpr unsigned randomSeed = 0x4a495443;
    static constexpr size_t maximumLength = 2048;
    static constexpr size_t maximumMisalignment = 16;
    WeakRandom random(randomSeed);
    Vector<uint8_t> storage(FillWith { }, maximumLength + maximumMisalignment, 0);
    unsigned failures = 0;
    for (unsigned iteration = 0; iteration < 4096 && failures < 8; ++iteration) {
        size_t length = random.getUint32(maximumLength + 1);
        size_t misalignment = random.getUint32(maximumMisalignment);
        auto bytes = storage.mutableSpan().subspan(misalignment, length);
        for (auto& byte : bytes)
            byte = static_cast<uint8_t>(random.getUint32(256));
        uint32_t expected = bitwiseCRC32C(bytes);
        for (auto& path : crc32cPaths) {
            uint32_t whole = ~path.extend(~0u, bytes);
            uint32_t state = ~0u;
            for (size_t offset = 0; offset < length;) {
                size_t piece = 1 + random.getUint32(static_cast<unsigned>(length - offset));
                state = path.extend(state, bytes.subspan(offset, piece));
                offset += piece;
            }
            if (whole != expected || ~state != expected) {
                ++failures;
                JITCACHE_FAIL(makeString(path.name, ", buffer "_s, iteration, " of length "_s, length, " at misalignment "_s, misalignment,
                    ": reference "_s, hex(expected), ", whole "_s, hex(whole), ", in pieces "_s, hex(~state)));
            }
        }
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
