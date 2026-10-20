#include "config.h"
#include "ImageTwinsSection.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ByteFields.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "GetByIdMetadata.h"
#include "ImageSection.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumerator.h"
#include "UnlinkedCodeBlock.h"
#include <array>
#include <mutex>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/StdLibExtras.h>

// Besides the declarations of its header, the codec's field offsets and the rules the structure checks and the compile
// input check apply.

namespace JSC::JITCache {

namespace ImageTwinsSectionInternal {

// Header fields.
static constexpr size_t flagsOffset = 0;
static constexpr size_t reservedOffset = 1;
static constexpr size_t reservedSize = 3;
static constexpr size_t assemblerSeedOffset = 4;
static constexpr size_t binarySwitchSeedCountOffset = 8;
static constexpr size_t inputCountOffset = 12;
static constexpr size_t binaryArithProfileCountOffset = 16;
static constexpr size_t unaryArithProfileCountOffset = 20;
static constexpr size_t regenerationCountOffset = 24;
static constexpr size_t producerValueCountOffset = 28;
static constexpr size_t tokenOffset = 32;
static_assert(tokenOffset + captureProcessTokenSize == imageTwinsHeaderSize);

// Compile input entry: u32 bytecode offset, u8 kind, u8 value, u16 reserved, u32 local scope depth, u32 module import
// slot.
static constexpr size_t inputBytecodeOffsetOffset = 0;
static constexpr size_t inputKindOffset = 4;
static constexpr size_t inputValueOffset = 5;
static constexpr size_t inputReservedOffset = 6;
static constexpr size_t inputDepthOffset = 8;
static constexpr size_t inputImportSlotOffset = 12;
static_assert(inputImportSlotOffset + sizeof(uint32_t) == imageTwinsCompileInputEntrySize);

// Regeneration entry: u32 MathIC index, u16 profile bits at entry, u8 attach count, u8 seed mask, i64 replacement, then
// one u32 seed per attach, padded to 8.
static constexpr size_t regenerationIndexOffset = 0;
static constexpr size_t regenerationBitsOffset = 4;
static constexpr size_t regenerationAttachCountOffset = 6;
static constexpr size_t regenerationMaskOffset = 7;
static constexpr size_t regenerationReplacementOffset = 8;
static constexpr size_t regenerationSeedsOffset = 16;
static_assert(regenerationSeedsOffset == imageTwinsRegenerationFixedSize);

// A regeneration attaches the fast-path snippet's assembler, the full snippet's, or both (JITMathIC::generateOutOfLine).
static constexpr unsigned maximumAttachCount = 2;

static constexpr uint8_t enumeratorFlagBits = JSPropertyNameEnumerator::IndexedMode | JSPropertyNameEnumerator::OwnStructureMode
    | JSPropertyNameEnumerator::GenericMode | JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch;

static ImageTwinsHeader decodeHeader(std::span<const uint8_t> bytes)
{
    ImageTwinsHeader header {
        .flags = bytes[flagsOffset],
        .assemblerSeed = readField<uint32_t>(bytes, assemblerSeedOffset),
        .binarySwitchSeedCount = readField<uint32_t>(bytes, binarySwitchSeedCountOffset),
        .inputCount = readField<uint32_t>(bytes, inputCountOffset),
        .binaryArithProfileCount = readField<uint32_t>(bytes, binaryArithProfileCountOffset),
        .unaryArithProfileCount = readField<uint32_t>(bytes, unaryArithProfileCountOffset),
        .regenerationCount = readField<uint32_t>(bytes, regenerationCountOffset),
        .producerValueCount = readField<uint32_t>(bytes, producerValueCountOffset),
    };
    memcpySpan(std::span { header.captureProcessToken }, bytes.subspan(tokenOffset, captureProcessTokenSize));
    return header;
}

static std::array<uint8_t, imageTwinsHeaderSize> encodeHeader(const ImageTwinsHeader& header)
{
    std::array<uint8_t, imageTwinsHeaderSize> bytes { };
    bytes[flagsOffset] = header.flags;
    writeField<uint32_t>(bytes, assemblerSeedOffset, header.assemblerSeed);
    writeField<uint32_t>(bytes, binarySwitchSeedCountOffset, header.binarySwitchSeedCount);
    writeField<uint32_t>(bytes, inputCountOffset, header.inputCount);
    writeField<uint32_t>(bytes, binaryArithProfileCountOffset, header.binaryArithProfileCount);
    writeField<uint32_t>(bytes, unaryArithProfileCountOffset, header.unaryArithProfileCount);
    writeField<uint32_t>(bytes, regenerationCountOffset, header.regenerationCount);
    writeField<uint32_t>(bytes, producerValueCountOffset, header.producerValueCount);
    memcpySpan(std::span { bytes }.subspan(tokenOffset, captureProcessTokenSize), std::span<const uint8_t> { header.captureProcessToken });
    return bytes;
}

static std::array<uint8_t, imageTwinsCompileInputEntrySize> encodeCompileInput(const CompileInput& input)
{
    std::array<uint8_t, imageTwinsCompileInputEntrySize> bytes { };
    writeField<uint32_t>(bytes, inputBytecodeOffsetOffset, input.bytecodeOffset);
    bytes[inputKindOffset] = static_cast<uint8_t>(input.kind);
    bytes[inputValueOffset] = input.value;
    writeField<uint32_t>(bytes, inputDepthOffset, input.localScopeDepth);
    writeField<uint32_t>(bytes, inputImportSlotOffset, input.moduleImportSlot);
    return bytes;
}

template<typename Value>
static std::array<uint8_t, sizeof(Value)> encodeValue(Value value)
{
    std::array<uint8_t, sizeof(Value)> bytes { };
    writeField<Value>(bytes, 0, value);
    return bytes;
}

static bool isValidInputValue(CompileInputKind kind, uint8_t value)
{
    switch (kind) {
    case CompileInputKind::ResolveScopeType:
    case CompileInputKind::GetFromScopeType:
        return value <= static_cast<uint8_t>(Dynamic);
    case CompileInputKind::PutToScopeType:
        // Linking never gives a put_to_scope a lazy closure type, and the baseline JIT aborts on one.
        return value <= static_cast<uint8_t>(Dynamic) && value != static_cast<uint8_t>(LazyClosureVar) && value != static_cast<uint8_t>(ResolvedLazyClosureVar);
    case CompileInputKind::GetByIdMode:
    case CompileInputKind::IteratorOpenMode:
    case CompileInputKind::AsyncIteratorOpenMode:
        return value <= static_cast<uint8_t>(GetByIdMode::ArrayLength);
    case CompileInputKind::EnumeratorMetadata:
        return !(value & ~enumeratorFlagBits);
    }
    return false;
}

// The resolve_scope types whose code bakes a depth or an import slot: one of a closure variable walks its depth, and one
// of a module variable walks its depth and then loads its import slot.
static bool mayHaveDepth(const CompileInput& input)
{
    return input.kind == CompileInputKind::ResolveScopeType && (input.value == static_cast<uint8_t>(ClosureVar) || input.value == static_cast<uint8_t>(ModuleVar));
}

static bool mayHaveImportSlot(const CompileInput& input)
{
    return input.kind == CompileInputKind::ResolveScopeType && input.value == static_cast<uint8_t>(ModuleVar);
}

} // namespace ImageTwinsSectionInternal

std::span<const uint8_t, captureProcessTokenSize> captureProcessToken()
{
    static std::array<uint8_t, captureProcessTokenSize> token { };
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        // The structure check rejects an all-zero token, so no process writes one.
        do
            cryptographicallyRandomValues(std::span { token });
        while (isAllZero(token));
    });
    return token;
}

// Twin data.

size_t twinDataStorageBytes(const TwinData& data)
{
    size_t bytes = data.seeds.binarySwitches.capacity() * sizeof(uint32_t)
        + data.compileInputs.inputs.capacity() * sizeof(CompileInput)
        + data.compileInputs.binaryArithBits.capacity() * sizeof(uint16_t)
        + data.compileInputs.unaryArithBits.capacity() * sizeof(uint16_t)
        + data.regenerations.capacity() * sizeof(TwinRegeneration);
    for (auto& regeneration : data.regenerations) {
        // Slots past the vector's inline capacity live in a buffer of their own.
        if (regeneration.assemblerSeeds.capacity() > ImageTwinsSectionInternal::maximumAttachCount)
            bytes += regeneration.assemblerSeeds.capacity() * sizeof(std::optional<uint32_t>);
    }
    return bytes;
}

std::optional<CompileInputKind> snapshotKindOf(OpcodeID opcode)
{
    switch (opcode) {
    case op_resolve_scope:
        return CompileInputKind::ResolveScopeType;
    case op_get_from_scope:
        return CompileInputKind::GetFromScopeType;
    case op_put_to_scope:
        return CompileInputKind::PutToScopeType;
    case op_get_by_id:
        return CompileInputKind::GetByIdMode;
    case op_iterator_open:
        return CompileInputKind::IteratorOpenMode;
    case op_async_iterator_open:
        return CompileInputKind::AsyncIteratorOpenMode;
    case op_enumerator_next:
        return CompileInputKind::EnumeratorMetadata;
    default:
        return std::nullopt;
    }
}

TwinCompileInputs snapshotCompileInputs(CodeBlock& codeBlock)
{
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock.unlinkedCodeBlock();
    auto& instructions = codeBlock.instructions();

    size_t inputCount = 0;
    for (const auto& instruction : instructions) {
        if (snapshotKindOf(instruction->opcodeID()))
            ++inputCount;
    }

    TwinCompileInputs result;
    result.inputs.reserveInitialCapacity(inputCount);
    for (const auto& instruction : instructions) {
        auto kind = snapshotKindOf(instruction->opcodeID());
        if (!kind)
            continue;
        CompileInput input { .bytecodeOffset = instruction.offset(), .kind = *kind };
        switch (*kind) {
        case CompileInputKind::ResolveScopeType: {
            // JIT::emit_op_resolve_scope and emitSlow_op_resolve_scope read the type, the depth of a ClosureVar or a
            // ModuleVar, and the import slot of a ModuleVar.
            auto& metadata = instruction->as<OpResolveScope>().metadata(&codeBlock);
            ResolveType type = metadata.m_resolveType;
            input.value = static_cast<uint8_t>(type);
            if (type == ClosureVar || type == ModuleVar)
                input.localScopeDepth = metadata.m_localScopeDepth;
            if (type == ModuleVar)
                input.moduleImportSlot = metadata.m_moduleImportSlot;
            break;
        }
        case CompileInputKind::GetFromScopeType:
            input.value = static_cast<uint8_t>(instruction->as<OpGetFromScope>().metadata(&codeBlock).m_getPutInfo.resolveType());
            break;
        case CompileInputKind::PutToScopeType:
            input.value = static_cast<uint8_t>(instruction->as<OpPutToScope>().metadata(&codeBlock).m_getPutInfo.resolveType());
            break;
        case CompileInputKind::GetByIdMode:
            input.value = static_cast<uint8_t>(instruction->as<OpGetById>().metadata(&codeBlock).m_modeMetadata.mode);
            break;
        case CompileInputKind::IteratorOpenMode:
            input.value = static_cast<uint8_t>(instruction->as<OpIteratorOpen>().metadata(&codeBlock).m_modeMetadata.mode);
            break;
        case CompileInputKind::AsyncIteratorOpenMode:
            input.value = static_cast<uint8_t>(instruction->as<OpAsyncIteratorOpen>().metadata(&codeBlock).m_modeMetadata.mode);
            break;
        case CompileInputKind::EnumeratorMetadata:
            input.value = instruction->as<OpEnumeratorNext>().metadata(&codeBlock).m_enumeratorMetadata;
            break;
        }
        result.inputs.append(input);
    }

    result.binaryArithBits = Vector<uint16_t>(unlinkedCodeBlock.numberOfBinaryArithProfiles(), [&](size_t index) {
        return static_cast<uint16_t>(unlinkedCodeBlock.binaryArithProfile(static_cast<unsigned>(index)).bits());
    });
    result.unaryArithBits = Vector<uint16_t>(unlinkedCodeBlock.numberOfUnaryArithProfiles(), [&](size_t index) {
        return static_cast<uint16_t>(unlinkedCodeBlock.unaryArithProfile(static_cast<unsigned>(index)).bits());
    });
    // No JS of the VM runs while the thread holding its API lock compiles, so the inputs equal what emission reads.
    result.compiledHoldingAPILock = codeBlock.vm().currentThreadIsHoldingAPILock();
    return result;
}

// The section's view.

std::optional<ImageTwinsView> ImageTwinsView::locate(std::span<const uint8_t> bytes)
{
    using namespace ImageTwinsSectionInternal;
    if (bytes.size() < imageTwinsHeaderSize)
        return std::nullopt;
    ImageTwinsView view;
    view.m_bytes = bytes;
    view.m_header = decodeHeader(bytes);

    // Every step stays inside the span before the next reads from it, so a count no span could hold stops the walk at
    // once; offsets grow by at most 2^36 per step and cannot overflow.
    uint64_t size = bytes.size();
    uint64_t offset = imageTwinsHeaderSize;
    auto advance = [&](uint64_t count) {
        offset += count;
        return offset <= size;
    };
    auto pad = [&] {
        offset = alignImageSectionOffset(offset);
        return offset <= size;
    };

    auto& header = view.m_header;
    auto& layout = view.m_layout;
    layout.binarySwitchSeeds = offset;
    if (!advance(static_cast<uint64_t>(header.binarySwitchSeedCount) * sizeof(uint32_t)))
        return std::nullopt;
    layout.binarySwitchSeedsEnd = offset;
    if (!pad())
        return std::nullopt;
    layout.inputs = offset;
    if (!advance(static_cast<uint64_t>(header.inputCount) * imageTwinsCompileInputEntrySize))
        return std::nullopt;
    layout.inputsEnd = offset;
    if (!pad())
        return std::nullopt;
    layout.binaryArithBits = offset;
    if (!advance(static_cast<uint64_t>(header.binaryArithProfileCount) * sizeof(uint16_t)))
        return std::nullopt;
    layout.binaryArithBitsEnd = offset;
    if (!pad())
        return std::nullopt;
    layout.unaryArithBits = offset;
    if (!advance(static_cast<uint64_t>(header.unaryArithProfileCount) * sizeof(uint16_t)))
        return std::nullopt;
    layout.unaryArithBitsEnd = offset;
    if (!pad())
        return std::nullopt;

    // Each regeneration entry's size follows from its attach count, which ImageCheck::TwinRegenerations bounds.
    layout.regenerations = offset;
    for (uint32_t index = 0; index < header.regenerationCount; ++index) {
        if (offset + imageTwinsRegenerationFixedSize > size)
            return std::nullopt;
        uint8_t attachCount = bytes[offset + regenerationAttachCountOffset];
        if (!advance(imageTwinsRegenerationEntrySize(attachCount)))
            return std::nullopt;
    }
    layout.producerValues = offset;
    if (!advance(static_cast<uint64_t>(header.producerValueCount) * sizeof(uint64_t)))
        return std::nullopt;
    layout.end = offset;
    return view;
}

uint32_t ImageTwinsView::binarySwitchSeed(unsigned index) const
{
    ASSERT(index < m_header.binarySwitchSeedCount);
    return readField<uint32_t>(m_bytes, m_layout.binarySwitchSeeds + static_cast<size_t>(index) * sizeof(uint32_t));
}

CompileInput ImageTwinsView::compileInput(unsigned index) const
{
    using namespace ImageTwinsSectionInternal;
    ASSERT(index < m_header.inputCount);
    auto entry = m_bytes.subspan(m_layout.inputs + static_cast<size_t>(index) * imageTwinsCompileInputEntrySize, imageTwinsCompileInputEntrySize);
    return CompileInput {
        .bytecodeOffset = readField<uint32_t>(entry, inputBytecodeOffsetOffset),
        .kind = static_cast<CompileInputKind>(entry[inputKindOffset]),
        .value = entry[inputValueOffset],
        .localScopeDepth = readField<uint32_t>(entry, inputDepthOffset),
        .moduleImportSlot = readField<uint32_t>(entry, inputImportSlotOffset),
    };
}

uint16_t ImageTwinsView::binaryArithBits(unsigned index) const
{
    ASSERT(index < m_header.binaryArithProfileCount);
    return readField<uint16_t>(m_bytes, m_layout.binaryArithBits + static_cast<size_t>(index) * sizeof(uint16_t));
}

uint16_t ImageTwinsView::unaryArithBits(unsigned index) const
{
    ASSERT(index < m_header.unaryArithProfileCount);
    return readField<uint16_t>(m_bytes, m_layout.unaryArithBits + static_cast<size_t>(index) * sizeof(uint16_t));
}

uint64_t ImageTwinsView::producerValue(unsigned index) const
{
    ASSERT(index < m_header.producerValueCount);
    return readField<uint64_t>(m_bytes, m_layout.producerValues + static_cast<size_t>(index) * sizeof(uint64_t));
}

std::span<const uint8_t> ImageTwinsView::regenerationEntry(size_t offset) const
{
    uint8_t attachCount = m_bytes[offset + ImageTwinsSectionInternal::regenerationAttachCountOffset];
    return m_bytes.subspan(offset, imageTwinsRegenerationEntrySize(attachCount));
}

TwinRegeneration ImageTwinsView::decodeRegeneration(std::span<const uint8_t> entry)
{
    using namespace ImageTwinsSectionInternal;
    uint8_t attachCount = entry[regenerationAttachCountOffset];
    uint8_t mask = entry[regenerationMaskOffset];
    TwinRegeneration regeneration {
        .mathICIndex = readField<uint32_t>(entry, regenerationIndexOffset),
        .profileBitsAtEntry = readField<uint16_t>(entry, regenerationBitsOffset),
        .replacement = CodeSymbol { readField<int64_t>(entry, regenerationReplacementOffset) },
        .assemblerSeeds = { },
    };
    // Exactly attachCount slots, so twinDataStorageBytes counts what the slots hold.
    regeneration.assemblerSeeds.reserveInitialCapacity(attachCount);
    for (unsigned slot = 0; slot < attachCount; ++slot) {
        bool drew = slot < 8 && (mask & (1u << slot));
        regeneration.assemblerSeeds.append(drew ? std::optional<uint32_t> { readField<uint32_t>(entry, regenerationSeedsOffset + slot * sizeof(uint32_t)) } : std::nullopt);
    }
    return regeneration;
}

size_t ImageTwinsView::twinDataStorageBytes() const
{
    size_t bytes = static_cast<size_t>(m_header.binarySwitchSeedCount) * sizeof(uint32_t)
        + static_cast<size_t>(m_header.inputCount) * sizeof(CompileInput)
        + static_cast<size_t>(m_header.binaryArithProfileCount) * sizeof(uint16_t)
        + static_cast<size_t>(m_header.unaryArithProfileCount) * sizeof(uint16_t)
        + static_cast<size_t>(m_header.regenerationCount) * sizeof(TwinRegeneration);
    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        uint8_t attachCount = entry[ImageTwinsSectionInternal::regenerationAttachCountOffset];
        if (attachCount > ImageTwinsSectionInternal::maximumAttachCount)
            bytes += attachCount * sizeof(std::optional<uint32_t>);
        offset += entry.size();
    }
    return bytes;
}

TwinData ImageTwinsView::twinData() const
{
    TwinData data;
    data.seeds.assembler = m_header.assemblerSeed;
    data.seeds.binarySwitches = Vector<uint32_t>(m_header.binarySwitchSeedCount, [&](size_t index) {
        return binarySwitchSeed(static_cast<unsigned>(index));
    });
    data.compileInputs.inputs = Vector<CompileInput>(m_header.inputCount, [&](size_t index) {
        return compileInput(static_cast<unsigned>(index));
    });
    data.compileInputs.binaryArithBits = Vector<uint16_t>(m_header.binaryArithProfileCount, [&](size_t index) {
        return binaryArithBits(static_cast<unsigned>(index));
    });
    data.compileInputs.unaryArithBits = Vector<uint16_t>(m_header.unaryArithProfileCount, [&](size_t index) {
        return unaryArithBits(static_cast<unsigned>(index));
    });
    data.compileInputs.compiledHoldingAPILock = compiledHoldingAPILock();
    data.regenerations.reserveInitialCapacity(m_header.regenerationCount);
    forEachRegeneration([&](unsigned, const TwinRegeneration& regeneration) {
        data.regenerations.append(regeneration);
    });
    ASSERT(JITCache::twinDataStorageBytes(data) == twinDataStorageBytes());
    return data;
}

bool operator==(const ImageTwinsView& a, const ImageTwinsView& b)
{
    return a.m_bytes.data() == b.m_bytes.data() && a.m_bytes.size() == b.m_bytes.size() && a.m_header == b.m_header && a.m_layout == b.m_layout;
}

// Known flags, zero reserved bytes and padding, a token that is not all zero, and a section that ends where the
// header's counts say.
bool ImageTwinsView::passesTwinsHeader() const
{
    using namespace ImageTwinsSectionInternal;
    if (m_header.flags & ~ImageTwinsFlags::compiledHoldingAPILock)
        return false;
    if (!isAllZero(m_bytes.subspan(reservedOffset, reservedSize)))
        return false;
    if (isAllZero(m_header.captureProcessToken))
        return false;
    if (m_layout.end != m_bytes.size())
        return false;

    auto isZeroBetween = [&](size_t begin, size_t end) {
        return isAllZero(m_bytes.subspan(begin, end - begin));
    };
    if (!isZeroBetween(m_layout.binarySwitchSeedsEnd, m_layout.inputs)
        || !isZeroBetween(m_layout.inputsEnd, m_layout.binaryArithBits)
        || !isZeroBetween(m_layout.binaryArithBitsEnd, m_layout.unaryArithBits)
        || !isZeroBetween(m_layout.unaryArithBitsEnd, m_layout.regenerations))
        return false;
    for (unsigned index = 0; index < m_header.inputCount; ++index) {
        size_t entry = m_layout.inputs + static_cast<size_t>(index) * imageTwinsCompileInputEntrySize;
        if (readField<uint16_t>(m_bytes, entry + inputReservedOffset))
            return false;
    }
    // Each regeneration entry pads its seeds to 8.
    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        size_t seedsEnd = regenerationSeedsOffset + static_cast<size_t>(entry[regenerationAttachCountOffset]) * sizeof(uint32_t);
        if (!isAllZero(entry.subspan(seedsEnd)))
            return false;
        offset += entry.size();
    }
    return true;
}

// Compile inputs of known kinds and values, in strictly increasing bytecode order, with a scope depth only on a
// ClosureVar or ModuleVar resolve_scope and an import slot only on a ModuleVar one.
bool ImageTwinsView::passesTwinInputs() const
{
    using namespace ImageTwinsSectionInternal;
    for (unsigned index = 0; index < m_header.inputCount; ++index) {
        auto input = compileInput(index);
        auto kindByte = static_cast<uint8_t>(input.kind);
        if (kindByte < static_cast<uint8_t>(CompileInputKind::ResolveScopeType) || kindByte > static_cast<uint8_t>(CompileInputKind::EnumeratorMetadata))
            return false;
        if (index && input.bytecodeOffset <= compileInput(index - 1).bytecodeOffset)
            return false;
        if (!isValidInputValue(input.kind, input.value))
            return false;
        if (input.localScopeDepth && !mayHaveDepth(input))
            return false;
        if (input.moduleImportSlot && !mayHaveImportSlot(input))
            return false;
    }
    return true;
}

// Agreement with the image section: one producer value per fixup, and each regeneration naming a MathIC with inline
// code, with one or two attaches, seeds only where its mask says the assembler drew, and a valid replacement.
bool ImageTwinsView::passesTwinRegenerations(const ImageSectionsView& view) const
{
    using namespace ImageTwinsSectionInternal;
    auto& image = view.header();
    // One producer value per fixup, the image's and every snippet's; the image's own checks left the snippet fields of a
    // MathIC without a snippet zero.
    uint64_t fixupCount = image.fixupCount;
    for (unsigned index = 0; index < image.mathICCount; ++index)
        fixupCount += view.mathIC(index).snippetFixupCount;
    if (m_header.producerValueCount != fixupCount)
        return false;

    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        offset += entry.size();
        uint32_t mathICIndex = readField<uint32_t>(entry, regenerationIndexOffset);
        if (mathICIndex >= image.mathICCount || !view.mathIC(mathICIndex).hasInlineCode())
            return false;
        uint8_t attachCount = entry[regenerationAttachCountOffset];
        if (attachCount < 1 || attachCount > maximumAttachCount)
            return false;
        uint8_t mask = entry[regenerationMaskOffset];
        if (mask & ~((1u << attachCount) - 1))
            return false;
        for (unsigned slot = 0; slot < attachCount; ++slot) {
            if (!(mask & (1u << slot)) && readField<uint32_t>(entry, regenerationSeedsOffset + slot * sizeof(uint32_t)))
                return false;
        }
        if (!CodeSymbol::isValid(readField<int64_t>(entry, regenerationReplacementOffset)))
            return false;
    }
    return true;
}

std::optional<ImageCheck> firstTwinsStructureFailure(const ImageSectionsView& view)
{
    auto& twins = view.twins();
    if (!twins.passesTwinsHeader())
        return ImageCheck::TwinsHeader;
    if (!twins.passesTwinInputs())
        return ImageCheck::TwinInputs;
    if (!twins.passesTwinRegenerations(view))
        return ImageCheck::TwinRegenerations;
    return std::nullopt;
}

std::optional<ImageCheck> firstTwinsFailureAgainst(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& twins = view.twins();
    if (twins.header().binaryArithProfileCount != unlinkedCodeBlock.numberOfBinaryArithProfiles() || twins.header().unaryArithProfileCount != unlinkedCodeBlock.numberOfUnaryArithProfiles())
        return ImageCheck::TwinInputSites;

    // Inputs and instructions are both in bytecode order, which ImageCheck::TwinInputs checked, so one walk pairs them:
    // every instruction the snapshot reads has exactly one input of its kind, and every input sits at such an
    // instruction's start.
    unsigned next = 0;
    unsigned inputCount = twins.header().inputCount;
    for (const auto& instruction : unlinkedCodeBlock.instructions()) {
        uint32_t offset = instruction.offset();
        if (next < inputCount && twins.compileInput(next).bytecodeOffset < offset)
            return ImageCheck::TwinInputSites;
        bool hasInput = next < inputCount && twins.compileInput(next).bytecodeOffset == offset;
        auto expected = snapshotKindOf(instruction->opcodeID());
        if (expected.has_value() != hasInput)
            return ImageCheck::TwinInputSites;
        if (!hasInput)
            continue;
        if (twins.compileInput(next++).kind != *expected)
            return ImageCheck::TwinInputSites;
    }
    if (next != inputCount)
        return ImageCheck::TwinInputSites;
    return std::nullopt;
}

// The writer.

uint64_t twinsSectionSize(const TwinData& data, size_t producerValueCount)
{
    uint64_t size = imageTwinsHeaderSize
        + alignImageSectionOffset(static_cast<uint64_t>(data.seeds.binarySwitches.size()) * sizeof(uint32_t))
        + alignImageSectionOffset(static_cast<uint64_t>(data.compileInputs.inputs.size()) * imageTwinsCompileInputEntrySize)
        + alignImageSectionOffset(static_cast<uint64_t>(data.compileInputs.binaryArithBits.size()) * sizeof(uint16_t))
        + alignImageSectionOffset(static_cast<uint64_t>(data.compileInputs.unaryArithBits.size()) * sizeof(uint16_t))
        + static_cast<uint64_t>(producerValueCount) * sizeof(uint64_t);
    for (auto& regeneration : data.regenerations)
        size += imageTwinsRegenerationEntrySize(regeneration.assemblerSeeds.size());
    return size;
}

bool writeTwinsSection(const TwinData& data, std::span<const uint64_t> producerValues, std::span<const uint8_t, captureProcessTokenSize> token, const ImageSectionSink& sink)
{
    using namespace ImageTwinsSectionInternal;
    ImageTwinsHeader header {
        .flags = data.compileInputs.compiledHoldingAPILock ? ImageTwinsFlags::compiledHoldingAPILock : static_cast<uint8_t>(0),
        .assemblerSeed = data.seeds.assembler,
        .binarySwitchSeedCount = static_cast<uint32_t>(data.seeds.binarySwitches.size()),
        .inputCount = static_cast<uint32_t>(data.compileInputs.inputs.size()),
        .binaryArithProfileCount = static_cast<uint32_t>(data.compileInputs.binaryArithBits.size()),
        .unaryArithProfileCount = static_cast<uint32_t>(data.compileInputs.unaryArithBits.size()),
        .regenerationCount = static_cast<uint32_t>(data.regenerations.size()),
        .producerValueCount = static_cast<uint32_t>(producerValues.size()),
    };
    memcpySpan(std::span { header.captureProcessToken }, std::span<const uint8_t> { token });

    ImageSectionWriter writer(sink);
    if (!writer.write(encodeHeader(header)))
        return false;
    for (uint32_t seed : data.seeds.binarySwitches) {
        if (!writer.write(encodeValue(seed)))
            return false;
    }
    if (!writer.pad())
        return false;
    for (auto& input : data.compileInputs.inputs) {
        if (!writer.write(encodeCompileInput(input)))
            return false;
    }
    if (!writer.pad())
        return false;
    for (uint16_t bits : data.compileInputs.binaryArithBits) {
        if (!writer.write(encodeValue(bits)))
            return false;
    }
    if (!writer.pad())
        return false;
    for (uint16_t bits : data.compileInputs.unaryArithBits) {
        if (!writer.write(encodeValue(bits)))
            return false;
    }
    if (!writer.pad())
        return false;
    for (auto& regeneration : data.regenerations) {
        ASSERT(regeneration.assemblerSeeds.size() <= 8);
        std::array<uint8_t, imageTwinsRegenerationFixedSize> fixed { };
        writeField<uint32_t>(fixed, regenerationIndexOffset, regeneration.mathICIndex);
        writeField<uint16_t>(fixed, regenerationBitsOffset, regeneration.profileBitsAtEntry);
        fixed[regenerationAttachCountOffset] = static_cast<uint8_t>(regeneration.assemblerSeeds.size());
        uint8_t mask = 0;
        for (size_t slot = 0; slot < regeneration.assemblerSeeds.size(); ++slot) {
            if (regeneration.assemblerSeeds[slot])
                mask |= static_cast<uint8_t>(1u << slot);
        }
        fixed[regenerationMaskOffset] = mask;
        writeField<int64_t>(fixed, regenerationReplacementOffset, regeneration.replacement.offset);
        if (!writer.write(fixed))
            return false;
        for (auto& seed : regeneration.assemblerSeeds) {
            if (!writer.write(encodeValue<uint32_t>(seed.value_or(0))))
                return false;
        }
        if (!writer.pad())
            return false;
    }
    for (uint64_t value : producerValues) {
        if (!writer.write(encodeValue(value)))
            return false;
    }
    ASSERT(writer.offset() == twinsSectionSize(data, producerValues.size()));
    return true;
}

// The replay.

std::optional<uint32_t> TwinReplay::takeAttachSeed()
{
    unsigned slot = m_attachCount++;
    if (slot >= m_regeneration.assemblerSeeds.size())
        return std::nullopt;
    return m_regeneration.assemblerSeeds[slot];
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
