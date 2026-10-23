#include "config.h"
#include "jitcache/store/JITCachePlatform.h"

#include "MacroAssembler.h"
#include "jitcache/base/EngineObject.h"
#include "jitcache/store/JITCacheOptions.h"
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

#if CPU(X86_64)
#include <cpuid.h>
#endif

// The process facts the header records, and the CRC-32C the container checksums with.

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
// no ID. External linkage, because a test reads crafted objects through it.
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

// The main executable's build ID, and the engine object's (EngineObject.h) when it is not the main executable. An
// engine that no loaded object holds cannot be identified, so it counts as an object of its own without an ID, which
// start rejects.
static void computeBuildIDs(ProcessFacts& facts)
{
    const LoadedObjects& objects = loadedObjects();
    if (objects.mainExecutable)
        facts.mainExecutable = buildIDOfObject(*objects.mainExecutable);
    if (!objects.engine)
        facts.engineObject = BuildID { };
    else if (objects.engine != objects.mainExecutable)
        facts.engineObject = buildIDOfObject(*objects.engine);
}

#endif // OS(LINUX)

// Bit i of the CPU feature vector is the answer of predicate i, and JITCacheContainer.cpp names the predicates in the
// same order.
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

// CRC-32C of RFC 3720: the Castagnoli polynomial 0x1EDC6F41, here reflected. Each path takes and returns the running
// state, without the initial value or the final XOR, and all of them give the same state for the same bytes.
static constexpr uint32_t crc32cReflectedPolynomial = 0x82F63B78;

static constexpr std::array<uint32_t, 256> crc32cTable = [] {
    std::array<uint32_t, 256> table { };
    for (size_t index = 0; index < table.size(); ++index) {
        uint32_t crc = static_cast<uint32_t>(index);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = crc & 1 ? (crc >> 1) ^ crc32cReflectedPolynomial : crc >> 1;
        table[index] = crc;
    }
    return table;
}();

static uint32_t crc32cExtendWithTable(uint32_t state, std::span<const uint8_t> bytes)
{
    for (uint8_t byte : bytes)
        state = crc32cTable[(state ^ byte) & 0xff] ^ (state >> 8);
    return state;
}

#if CPU(X86_64)
// cpuid directly: __builtin_cpu_supports needs compiler-rt's __cpu_model, which not every link provides.
static bool cpuHasSSE42()
{
    unsigned eax = 0;
    unsigned ebx = 0;
    unsigned ecx = 0;
    unsigned edx = 0;
    return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_SSE4_2);
}

// SSE4.2's crc32 instruction computes CRC-32C, eight bytes at a time.
__attribute__((target("sse4.2"))) static uint32_t crc32cExtendWithSSE42(uint32_t state, std::span<const uint8_t> bytes)
{
    uint64_t crc = state;
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t)))
        crc = __builtin_ia32_crc32di(crc, WTF::unalignedLoad<uint64_t>(bytes.data()));
    for (uint8_t byte : bytes)
        crc = __builtin_ia32_crc32qi(static_cast<uint32_t>(crc), byte);
    return static_cast<uint32_t>(crc);
}
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
// The CRC extension's crc32c instructions, eight bytes at a time. A build whose target defines __ARM_FEATURE_CRC32, as
// Bun's -march=armv8-a+crc does, runs only on CPUs that have them.
static uint32_t crc32cExtendWithARMv8(uint32_t state, std::span<const uint8_t> bytes)
{
    for (; bytes.size() >= sizeof(uint64_t); bytes = bytes.subspan(sizeof(uint64_t)))
        state = __builtin_arm_crc32cd(state, WTF::unalignedLoad<uint64_t>(bytes.data()));
    for (uint8_t byte : bytes)
        state = __builtin_arm_crc32cb(state, byte);
    return state;
}
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
    using namespace PlatformInternal;
#if CPU(X86_64)
    static const bool hasSSE42 = cpuHasSSE42();
    if (hasSSE42)
        return crc32cExtendWithSSE42(state, bytes);
#elif CPU(ARM64) && defined(__ARM_FEATURE_CRC32)
    return crc32cExtendWithARMv8(state, bytes);
#endif
    return crc32cExtendWithTable(state, bytes);
}

#if ENABLE(JITCACHE_TWINS)
void removeMainBuildIDForTesting(bool removed)
{
    PlatformInternal::mainBuildIDRemovedForTesting.store(removed);
}

uint32_t crc32cExtendWithTableForTesting(uint32_t state, std::span<const uint8_t> bytes)
{
    return PlatformInternal::crc32cExtendWithTable(state, bytes);
}
#endif

} // namespace JSC::JITCache
