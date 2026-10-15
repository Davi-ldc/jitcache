#include "config.h"
#include "ImageFootprint.h"

#if ENABLE(JIT)

#include "ByteFields.h"
#include "ExecutableAllocator.h"
#include "MacroAssembler.h"
#include <algorithm>
#include <limits>

// Besides the declarations of its header, each architecture's instruction encodings: the opcodes, field masks and sizes
// the footprint functions test and rewrite.

namespace JSC::JITCache {

namespace ImageFootprintInternal {

#if CPU(X86_64)
static constexpr bool hasFootprints = true;
static constexpr size_t pointerFootprintSize = 10; // REX.W B8+r imm64
static constexpr size_t callFootprintSize = 5; // E8 rel32
static constexpr size_t jumpFootprintSize = 5; // E9 rel32
static constexpr size_t conditionalJumpFootprintSize = 6; // 0F 80+cc rel32
static constexpr uint8_t rexW = 0x48;
static constexpr uint8_t rexWB = 0x49;
static constexpr uint8_t movImm64First = 0xB8;
static constexpr uint8_t movImm64Last = 0xBF;
static constexpr uint8_t callRel32 = 0xE8;
static constexpr uint8_t jmpRel32 = 0xE9;
static constexpr uint8_t twoByteEscape = 0x0F;
static constexpr uint8_t jccRel32First = 0x80;
static constexpr uint8_t jccRel32Last = 0x8F;

static bool isConditionalJumpOpcode(uint8_t escape, uint8_t opcode)
{
    return escape == twoByteEscape && opcode >= jccRel32First && opcode <= jccRel32Last;
}
#elif CPU(ARM64) && !CPU(ARM64E)
static constexpr bool hasFootprints = true;
static_assert(ARM64Assembler::NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS == 3, "a Pointer is one movz and two movk");
static constexpr size_t instructionSize = 4;
static constexpr size_t pointerFootprintSize = 3 * instructionSize;
static constexpr std::array<uint32_t, 3> pointerOpcodes { 0xD2800000, 0xF2A00000, 0xF2C00000 }; // movz; movk lsl 16; movk lsl 32
static constexpr uint32_t registerMask = 0x1F; // Xd, bits 0 to 4
static constexpr uint32_t imm16Mask = 0xFFFFu << 5; // bits 5 to 20
static constexpr uint32_t blOpcode = 0x94000000;
static constexpr uint32_t bOpcode = 0x14000000;
static constexpr uint32_t imm26Mask = 0x03FFFFFF; // bits 0 to 25

static uint32_t wordAt(std::span<const uint8_t> bytes, size_t index)
{
    return readField<uint32_t>(bytes, index * instructionSize);
}

static int64_t branchDisplacement(uint32_t word)
{
    // imm26, sign-extended, in instructions.
    int64_t immediate = word & imm26Mask;
    if (immediate & (1 << 25))
        immediate -= 1 << 26;
    return immediate * static_cast<int64_t>(instructionSize);
}
#else
static constexpr bool hasFootprints = false;
#endif

} // namespace ImageFootprintInternal

std::optional<FixupFootprint> fixupFootprint(FixupForm form, uint32_t site, std::span<const uint8_t> code)
{
    using namespace ImageFootprintInternal;
    if constexpr (!hasFootprints)
        return std::nullopt;
    size_t begin = 0;
    size_t end = 0;
#if CPU(X86_64)
    // Every x86_64 site ends its footprint.
    if (site > code.size())
        return std::nullopt;
    size_t length = 0;
    switch (form) {
    case FixupForm::Pointer:
        length = pointerFootprintSize;
        break;
    case FixupForm::Call:
        length = callFootprintSize;
        break;
    case FixupForm::Jump:
        if (site >= jumpFootprintSize && code[site - jumpFootprintSize] == jmpRel32)
            length = jumpFootprintSize;
        else if (site >= conditionalJumpFootprintSize && isConditionalJumpOpcode(code[site - conditionalJumpFootprintSize], code[site - conditionalJumpFootprintSize + 1]))
            length = conditionalJumpFootprintSize;
        else
            return std::nullopt;
        break;
    default:
        return std::nullopt;
    }
    if (site < length)
        return std::nullopt;
    begin = site - length;
    end = site;
#elif CPU(ARM64) && !CPU(ARM64E)
    // A Call's site ends its bl; a Pointer's or a Jump's site starts its first word.
    switch (form) {
    case FixupForm::Pointer:
        begin = site;
        end = begin + pointerFootprintSize;
        break;
    case FixupForm::Call:
        if (site < instructionSize)
            return std::nullopt;
        begin = site - instructionSize;
        end = site;
        break;
    case FixupForm::Jump:
        begin = site;
        end = begin + instructionSize;
        break;
    default:
        return std::nullopt;
    }
#else
    UNUSED_PARAM(form);
    UNUSED_PARAM(site);
#endif
    if (end > code.size())
        return std::nullopt;
    return FixupFootprint { begin, end };
}

CanonicalFootprint canonicalFootprint(FixupForm form, std::span<const uint8_t> footprint)
{
    using namespace ImageFootprintInternal;
    CanonicalFootprint result;
    size_t size = std::min(footprint.size(), maximumFootprintSize);
    memcpySpan(std::span { result.bytes }.first(size), footprint.first(size));
    result.size = static_cast<uint8_t>(size);
    auto bytes = std::span { result.bytes }.first(size);
#if CPU(X86_64)
    // Zero the immediate or displacement after the opcode bytes.
    size_t opcodeBytes = 0;
    switch (form) {
    case FixupForm::Pointer:
        if (size == pointerFootprintSize)
            opcodeBytes = 2;
        break;
    case FixupForm::Call:
        if (size == callFootprintSize)
            opcodeBytes = 1;
        break;
    case FixupForm::Jump:
        if (size == jumpFootprintSize)
            opcodeBytes = 1;
        else if (size == conditionalJumpFootprintSize)
            opcodeBytes = 2;
        break;
    }
    if (opcodeBytes)
        zeroSpan(bytes.subspan(opcodeBytes));
#elif CPU(ARM64) && !CPU(ARM64E)
    // Zero each word's immediate field.
    uint32_t mask = 0;
    switch (form) {
    case FixupForm::Pointer:
        if (size == pointerFootprintSize)
            mask = imm16Mask;
        break;
    case FixupForm::Call:
    case FixupForm::Jump:
        if (size == instructionSize)
            mask = imm26Mask;
        break;
    }
    if (mask) {
        for (size_t index = 0; index < size / instructionSize; ++index)
            writeField<uint32_t>(bytes, index * instructionSize, wordAt(bytes, index) & ~mask);
    }
#else
    UNUSED_PARAM(form);
    UNUSED_PARAM(bytes);
#endif
    return result;
}

bool isCanonicalFootprint(FixupForm form, std::span<const uint8_t> footprint)
{
    using namespace ImageFootprintInternal;
#if CPU(X86_64)
    switch (form) {
    case FixupForm::Pointer:
        return footprint.size() == pointerFootprintSize
            && (footprint[0] == rexW || footprint[0] == rexWB)
            && footprint[1] >= movImm64First && footprint[1] <= movImm64Last
            && isAllZero(footprint.subspan(2));
    case FixupForm::Call:
        return footprint.size() == callFootprintSize && footprint[0] == callRel32 && isAllZero(footprint.subspan(1));
    case FixupForm::Jump:
        if (footprint.size() == jumpFootprintSize)
            return footprint[0] == jmpRel32 && isAllZero(footprint.subspan(1));
        if (footprint.size() == conditionalJumpFootprintSize)
            return isConditionalJumpOpcode(footprint[0], footprint[1]) && isAllZero(footprint.subspan(2));
        return false;
    }
    return false;
#elif CPU(ARM64) && !CPU(ARM64E)
    switch (form) {
    case FixupForm::Pointer: {
        if (footprint.size() != pointerFootprintSize)
            return false;
        uint32_t reg = wordAt(footprint, 0) & registerMask;
        for (size_t index = 0; index < pointerOpcodes.size(); ++index) {
            if (wordAt(footprint, index) != (pointerOpcodes[index] | reg))
                return false;
        }
        return true;
    }
    case FixupForm::Call:
        return footprint.size() == instructionSize && wordAt(footprint, 0) == blOpcode;
    case FixupForm::Jump:
        return footprint.size() == instructionSize && wordAt(footprint, 0) == bOpcode;
    }
    return false;
#else
    UNUSED_PARAM(form);
    UNUSED_PARAM(footprint);
    return false;
#endif
}

std::optional<uintptr_t> decodeFootprint(FixupForm form, std::span<const uint8_t> footprint, uintptr_t footprintAddress)
{
    using namespace ImageFootprintInternal;
#if CPU(X86_64)
    switch (form) {
    case FixupForm::Pointer:
        if (footprint.size() != pointerFootprintSize || (footprint[0] != rexW && footprint[0] != rexWB) || footprint[1] < movImm64First || footprint[1] > movImm64Last)
            return std::nullopt;
        return static_cast<uintptr_t>(readField<uint64_t>(footprint, 2));
    case FixupForm::Call:
    case FixupForm::Jump: {
        size_t opcodeBytes = 0;
        if (form == FixupForm::Call && footprint.size() == callFootprintSize && footprint[0] == callRel32)
            opcodeBytes = 1;
        else if (form == FixupForm::Jump && footprint.size() == jumpFootprintSize && footprint[0] == jmpRel32)
            opcodeBytes = 1;
        else if (form == FixupForm::Jump && footprint.size() == conditionalJumpFootprintSize && isConditionalJumpOpcode(footprint[0], footprint[1]))
            opcodeBytes = 2;
        else
            return std::nullopt;
        // rel32 counts from the end of the instruction, which is the site.
        int32_t displacement = readField<int32_t>(footprint, opcodeBytes);
        return footprintAddress + footprint.size() + static_cast<intptr_t>(displacement);
    }
    }
    return std::nullopt;
#elif CPU(ARM64) && !CPU(ARM64E)
    switch (form) {
    case FixupForm::Pointer: {
        if (footprint.size() != pointerFootprintSize)
            return std::nullopt;
        uint32_t reg = wordAt(footprint, 0) & registerMask;
        uint64_t value = 0;
        for (size_t index = 0; index < pointerOpcodes.size(); ++index) {
            uint32_t word = wordAt(footprint, index);
            if ((word & ~imm16Mask) != (pointerOpcodes[index] | reg))
                return std::nullopt;
            value |= static_cast<uint64_t>((word & imm16Mask) >> 5) << (16 * index);
        }
        return static_cast<uintptr_t>(value);
    }
    case FixupForm::Call:
    case FixupForm::Jump: {
        if (footprint.size() != instructionSize)
            return std::nullopt;
        uint32_t word = wordAt(footprint, 0);
        if ((word & ~imm26Mask) != (form == FixupForm::Call ? blOpcode : bOpcode))
            return std::nullopt;
        // The displacement counts from the branch itself.
        return footprintAddress + static_cast<uintptr_t>(branchDisplacement(word));
    }
    }
    return std::nullopt;
#else
    UNUSED_PARAM(form);
    UNUSED_PARAM(footprint);
    UNUSED_PARAM(footprintAddress);
    return std::nullopt;
#endif
}

bool footprintReaches(FixupForm form, std::span<const uint8_t> footprint, uintptr_t footprintAddress, uintptr_t expected)
{
    auto decoded = decodeFootprint(form, footprint, footprintAddress);
    if (!decoded)
        return false;
    uintptr_t target = *decoded;
    if (target == expected)
        return true;
    if (form == FixupForm::Pointer)
        return false;
#if ENABLE(JUMP_ISLANDS)
    // An island is one unconditional b in the pool, and a chain crosses each region at most once.
    constexpr size_t islandSize = 4;
    auto poolStart = startOfFixedExecutableMemoryPool<uintptr_t>();
    auto poolEnd = endOfFixedExecutableMemoryPool<uintptr_t>();
    if (poolEnd < poolStart || poolEnd - poolStart < islandSize)
        return false;
    constexpr size_t halfRange = MacroAssembler::nearJumpRange / 2;
    size_t maximumIslands = (poolEnd - poolStart + halfRange - 1) / halfRange;
    for (size_t island = 0; island < maximumIslands; ++island) {
        if (target < poolStart || target > poolEnd - islandSize || target % islandSize)
            return false;
        auto next = decodeFootprint(FixupForm::Jump, unsafeMakeSpan(reinterpret_cast<const uint8_t*>(target), islandSize), target);
        if (!next)
            return false;
        target = *next;
        if (target == expected)
            return true;
    }
#endif
    return false;
}

std::optional<uint32_t> farCallPointerSite(uint32_t callReturnOffset)
{
#if CPU(X86_64)
    constexpr uint32_t distance = REPATCH_OFFSET_CALL_R11;
#elif CPU(ARM64) && !CPU(ARM64E)
    // MacroAssemblerARM64::REPATCH_OFFSET_CALL_TO_POINTER, which is protected, from the public constant it derives from.
    constexpr uint32_t distance = (ARM64Assembler::NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS + 1) * ImageFootprintInternal::instructionSize;
#else
    constexpr uint32_t distance = std::numeric_limits<uint32_t>::max();
#endif
    if (callReturnOffset < distance)
        return std::nullopt;
    return callReturnOffset - distance;
}

uint64_t snippetEntrySite(uint32_t inlineStart)
{
#if CPU(X86_64)
    // The rewrite writes one jmp rel32 at the inline start; its site ends it.
    return static_cast<uint64_t>(inlineStart) + ImageFootprintInternal::jumpFootprintSize;
#else
    // The rewrite writes one b at the inline start; its site is the instruction.
    return inlineStart;
#endif
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
