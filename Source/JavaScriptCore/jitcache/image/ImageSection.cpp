#include "config.h"
#include "jitcache/image/ImageSection.h"

#include "jitcache/image/ImageTypes.h"

#if ENABLE(JIT)

#include "BaselineJITCode.h"
#include "JSCInlines.h"
#include "jitcache/base/ByteFields.h"
#include "jitcache/image/ImageFootprint.h"

#if ENABLE(JITCACHE_TWINS)
#include "jitcache/image/ImageTwinsSection.h"
#endif

#endif // ENABLE(JIT)

// Besides the declarations of its header, the section's field offsets.

namespace JSC::JITCache {

// Defined in every build: the integrator names image checks in its fault reports.
ASCIILiteral description(ImageCheck check)
{
    switch (check) {
    case ImageCheck::Header:
        return "header"_s;
    case ImageCheck::BakedFacts:
        return "baked-facts"_s;
    case ImageCheck::TwinsHeader:
        return "twins-header"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#if ENABLE(JIT)

namespace ImageSectionInternal {

// Header fields.
static constexpr size_t codeSizeOffset = 0;
static constexpr size_t arityEntryOffsetOffset = 4;
static constexpr size_t fixupCountOffset = 8;
static constexpr size_t callCountOffset = 12;
static constexpr size_t moldCountOffset = 16;
static constexpr size_t simpleSwitchTableCountOffset = 20;
static constexpr size_t stringSwitchTableCountOffset = 24;
static constexpr size_t codeMapCountOffset = 28;
static constexpr size_t constantPoolCountOffset = 32;
static constexpr size_t mathICCountOffset = 36;
static constexpr size_t livenessRateOffset = 40;
static constexpr size_t fullnessRateOffset = 48;

// Fixup entry: u32 site, u8 form, u8 kind, u16 reserved, u32 a, u32 b, i64 payload.
static constexpr size_t fixupSiteOffset = 0;
static constexpr size_t fixupFormOffset = 4;
static constexpr size_t fixupKindOffset = 5;
static constexpr size_t fixupAOffset = 8;
static constexpr size_t fixupBOffset = 12;
static constexpr size_t fixupPayloadOffset = 16;

// Call entry: u32 bytecode index bits, u32 doneLocation.
static constexpr size_t callBytecodeIndexOffset = 0;
static constexpr size_t callDoneLocationOffset = 4;

// Mold entry: u8 accessType, u8 preconfiguredCacheType, u8 flags, u8 identifier kind, u32 identifier, u32 bytecode
// index bits, u32 doneLocation.
static constexpr size_t moldAccessTypeOffset = 0;
static constexpr size_t moldCacheTypeOffset = 1;
static constexpr size_t moldFlagsOffset = 2;
static constexpr size_t moldIdentifierKindOffset = 3;
static constexpr size_t moldIdentifierOffset = 4;
static constexpr size_t moldBytecodeIndexOffset = 8;
static constexpr size_t moldDoneLocationOffset = 12;

// Constant pool entry: u32 type, u32 index.
static constexpr size_t constantTypeOffset = 0;
static constexpr size_t constantIndexOffset = 4;

// MathIC entry: u8 kind, u8 flags, u16 reserved, u32 bytecode offset, u32 inlineStart, u32 inlineEnd, u32 slowPathStart,
// u32 slowPathCall, u32 snippetSize, u32 snippetFixupCount.
static constexpr size_t mathICKindOffset = 0;
static constexpr size_t mathICFlagsOffset = 1;
static constexpr size_t mathICBytecodeOffsetOffset = 4;
static constexpr size_t mathICInlineStartOffset = 8;
static constexpr size_t mathICInlineEndOffset = 12;
static constexpr size_t mathICSlowPathStartOffset = 16;
static constexpr size_t mathICSlowPathCallOffset = 20;
static constexpr size_t mathICSnippetSizeOffset = 24;
static constexpr size_t mathICSnippetFixupCountOffset = 28;

static_assert(static_cast<uint32_t>(ImageConstantType::FunctionDecl) == static_cast<uint32_t>(JITConstantPool::Type::FunctionDecl));
static_assert(static_cast<uint32_t>(ImageConstantType::FunctionExpr) == static_cast<uint32_t>(JITConstantPool::Type::FunctionExpr));

static ImageSectionHeader decodeHeader(std::span<const uint8_t> image)
{
    return ImageSectionHeader {
        .codeSize = readField<uint32_t>(image, codeSizeOffset),
        .arityEntryOffset = readField<uint32_t>(image, arityEntryOffsetOffset),
        .fixupCount = readField<uint32_t>(image, fixupCountOffset),
        .callCount = readField<uint32_t>(image, callCountOffset),
        .moldCount = readField<uint32_t>(image, moldCountOffset),
        .simpleSwitchTableCount = readField<uint32_t>(image, simpleSwitchTableCountOffset),
        .stringSwitchTableCount = readField<uint32_t>(image, stringSwitchTableCountOffset),
        .codeMapCount = readField<uint32_t>(image, codeMapCountOffset),
        .constantPoolCount = readField<uint32_t>(image, constantPoolCountOffset),
        .mathICCount = readField<uint32_t>(image, mathICCountOffset),
        .livenessRateBits = readField<uint64_t>(image, livenessRateOffset),
        .fullnessRateBits = readField<uint64_t>(image, fullnessRateOffset),
    };
}

} // namespace ImageSectionInternal

UniquedStringImpl* immortalNameImpl(VM& vm, ImmortalName name)
{
    switch (name) {
    case ImmortalName::Length:
        return vm.propertyNames->length.impl();
    case ImmortalName::Next:
        return vm.propertyNames->next.impl();
    case ImmortalName::Done:
        return vm.propertyNames->done.impl();
    case ImmortalName::Value:
        return vm.propertyNames->value.impl();
    case ImmortalName::SymbolHasInstance:
        return vm.propertyNames->hasInstanceSymbol.impl();
    case ImmortalName::Prototype:
        return vm.propertyNames->prototype.impl();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<ImmortalName> immortalNameFor(VM& vm, const UniquedStringImpl* impl)
{
    for (uint8_t value = 1; value <= numberOfImmortalNames; ++value) {
        auto name = static_cast<ImmortalName>(value);
        if (immortalNameImpl(vm, name) == impl)
            return name;
    }
    return std::nullopt;
}

std::array<uint8_t, imageSectionHeaderSize> encodeImageSectionHeader(const ImageSectionHeader& header)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageSectionHeaderSize> bytes { };
    writeField<uint32_t>(bytes, codeSizeOffset, header.codeSize);
    writeField<uint32_t>(bytes, arityEntryOffsetOffset, header.arityEntryOffset);
    writeField<uint32_t>(bytes, fixupCountOffset, header.fixupCount);
    writeField<uint32_t>(bytes, callCountOffset, header.callCount);
    writeField<uint32_t>(bytes, moldCountOffset, header.moldCount);
    writeField<uint32_t>(bytes, simpleSwitchTableCountOffset, header.simpleSwitchTableCount);
    writeField<uint32_t>(bytes, stringSwitchTableCountOffset, header.stringSwitchTableCount);
    writeField<uint32_t>(bytes, codeMapCountOffset, header.codeMapCount);
    writeField<uint32_t>(bytes, constantPoolCountOffset, header.constantPoolCount);
    writeField<uint32_t>(bytes, mathICCountOffset, header.mathICCount);
    writeField<uint64_t>(bytes, livenessRateOffset, header.livenessRateBits);
    writeField<uint64_t>(bytes, fullnessRateOffset, header.fullnessRateBits);
    return bytes;
}

std::array<uint8_t, imageFixupEntrySize> encodeImageFixup(const ImageFixup& fixup)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageFixupEntrySize> bytes { };
    writeField<uint32_t>(bytes, fixupSiteOffset, fixup.site);
    bytes[fixupFormOffset] = static_cast<uint8_t>(fixup.form);
    bytes[fixupKindOffset] = static_cast<uint8_t>(fixup.target.kind);
    writeField<uint32_t>(bytes, fixupAOffset, fixup.target.a);
    writeField<uint32_t>(bytes, fixupBOffset, fixup.target.b);
    writeField<int64_t>(bytes, fixupPayloadOffset, fixup.target.payload);
    return bytes;
}

std::array<uint8_t, imageCallEntrySize> encodeImageCall(const ImageCallEntry& call)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageCallEntrySize> bytes { };
    writeField<uint32_t>(bytes, callBytecodeIndexOffset, call.bytecodeIndex.asBits());
    writeField<uint32_t>(bytes, callDoneLocationOffset, call.doneLocation);
    return bytes;
}

std::array<uint8_t, imageMoldEntrySize> encodeImageMold(const ImageMoldEntry& mold)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageMoldEntrySize> bytes { };
    bytes[moldAccessTypeOffset] = static_cast<uint8_t>(static_cast<int8_t>(mold.accessType));
    bytes[moldCacheTypeOffset] = static_cast<uint8_t>(static_cast<int8_t>(mold.preconfiguredCacheType));
    bytes[moldFlagsOffset] = mold.flags;
    bytes[moldIdentifierKindOffset] = static_cast<uint8_t>(mold.identifierKind);
    writeField<uint32_t>(bytes, moldIdentifierOffset, mold.identifier);
    writeField<uint32_t>(bytes, moldBytecodeIndexOffset, mold.bytecodeIndex.asBits());
    writeField<uint32_t>(bytes, moldDoneLocationOffset, mold.doneLocation);
    return bytes;
}

std::array<uint8_t, imageConstantPoolEntrySize> encodeImageConstantPoolEntry(const ImageConstantPoolEntry& constant)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageConstantPoolEntrySize> bytes { };
    writeField<uint32_t>(bytes, constantTypeOffset, static_cast<uint32_t>(constant.type));
    writeField<uint32_t>(bytes, constantIndexOffset, constant.index);
    return bytes;
}

std::array<uint8_t, imageMathICEntrySize> encodeImageMathIC(const ImageMathICEntry& mathIC)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageMathICEntrySize> bytes { };
    bytes[mathICKindOffset] = static_cast<uint8_t>(mathIC.kind);
    bytes[mathICFlagsOffset] = mathIC.flags;
    writeField<uint32_t>(bytes, mathICBytecodeOffsetOffset, mathIC.bytecodeOffset);
    writeField<uint32_t>(bytes, mathICInlineStartOffset, mathIC.inlineStart);
    writeField<uint32_t>(bytes, mathICInlineEndOffset, mathIC.inlineEnd);
    writeField<uint32_t>(bytes, mathICSlowPathStartOffset, mathIC.slowPathStart);
    writeField<uint32_t>(bytes, mathICSlowPathCallOffset, mathIC.slowPathCall);
    writeField<uint32_t>(bytes, mathICSnippetSizeOffset, mathIC.snippetSize);
    writeField<uint32_t>(bytes, mathICSnippetFixupCountOffset, mathIC.snippetFixupCount);
    return bytes;
}

std::array<uint8_t, sizeof(uint32_t)> encodeImageOffset(uint32_t offset)
{
    std::array<uint8_t, sizeof(uint32_t)> bytes { };
    writeField<uint32_t>(bytes, 0, offset);
    return bytes;
}

ImageSectionSize::ImageSectionSize(const ImageSectionHeader& header)
    : m_fixedBytes(imageSectionHeaderSize
        + alignImageSectionOffset(header.codeSize)
        + static_cast<uint64_t>(header.fixupCount) * imageFixupEntrySize
        + static_cast<uint64_t>(header.callCount) * imageCallEntrySize
        + static_cast<uint64_t>(header.moldCount) * imageMoldEntrySize
        + alignImageSectionOffset(static_cast<uint64_t>(header.codeMapCount) * sizeof(uint32_t))
        + static_cast<uint64_t>(header.constantPoolCount) * imageConstantPoolEntrySize
        + static_cast<uint64_t>(header.mathICCount) * imageMathICEntrySize)
{
}

void ImageSectionSize::addSimpleSwitchTable(uint32_t entryCount)
{
    m_simpleSwitchTableBytes += 2 * sizeof(uint32_t) + static_cast<uint64_t>(entryCount) * sizeof(uint32_t);
}

void ImageSectionSize::addStringSwitchTable(uint32_t entryCount)
{
    m_stringSwitchTableBytes += sizeof(uint32_t) + static_cast<uint64_t>(entryCount) * sizeof(uint32_t);
}

void ImageSectionSize::addSnippet(uint32_t snippetSize, uint32_t snippetFixupCount)
{
    m_snippetBytes += alignImageSectionOffset(snippetSize) + static_cast<uint64_t>(snippetFixupCount) * imageFixupEntrySize;
}

uint64_t ImageSectionSize::bytes() const
{
    return m_fixedBytes + alignImageSectionOffset(m_simpleSwitchTableBytes) + alignImageSectionOffset(m_stringSwitchTableBytes) + m_snippetBytes;
}

ImageSectionWriter::ImageSectionWriter(const ImageSectionSink& sink)
    : m_sink(sink)
{
}

bool ImageSectionWriter::write(std::span<const uint8_t> bytes)
{
    if (bytes.empty())
        return true;
    if (!m_sink(bytes))
        return false;
    m_offset += bytes.size();
    return true;
}

bool ImageSectionWriter::pad()
{
    static constexpr std::array<uint8_t, imageSectionAlignment> zeros { };
    size_t padding = alignImageSectionOffset(m_offset) - m_offset;
    return write(std::span { zeros }.first(padding));
}

bool ImageSectionWriter::writeCanonicalCode(std::span<const uint8_t> liveCode, std::span<const ImageFixup> fixups)
{
    size_t cursor = 0;
    for (auto& fixup : fixups) {
        auto footprint = fixupFootprint(fixup.form, fixup.site, liveCode);
        if (!footprint || footprint->begin < cursor) {
            // Finishing checked every footprint and its order before the record became Complete.
            ASSERT_NOT_REACHED();
            return false;
        }
        if (!write(liveCode.subspan(cursor, footprint->begin - cursor)))
            return false;
        if (!write(canonicalFootprint(fixup.form, liveCode.subspan(footprint->begin, footprint->size())).span()))
            return false;
        cursor = footprint->end;
    }
    return write(liveCode.subspan(cursor));
}

ImageFixup ImageFixupArray::operator[](size_t index) const
{
    using namespace ImageSectionInternal;
    auto entry = entryBytes(index);
    return ImageFixup {
        .site = readField<uint32_t>(entry, fixupSiteOffset),
        .form = static_cast<FixupForm>(entry[fixupFormOffset]),
        .target = ImageTarget {
            .kind = static_cast<TargetKind>(entry[fixupKindOffset]),
            .a = readField<uint32_t>(entry, fixupAOffset),
            .b = readField<uint32_t>(entry, fixupBOffset),
            .payload = readField<int64_t>(entry, fixupPayloadOffset),
        },
    };
}

std::optional<ImageFixup> ImageFixupArray::find(uint32_t site, FixupForm form) const
{
    using namespace ImageSectionInternal;
    auto siteAt = [&](size_t index) {
        return readField<uint32_t>(entryBytes(index), fixupSiteOffset);
    };
    size_t low = 0;
    size_t high = size();
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (siteAt(middle) < site)
            low = middle + 1;
        else
            high = middle;
    }
    // On ARM64 a Call and a Pointer or a Jump can share a site, the Call first.
    for (size_t index = low; index < size() && siteAt(index) == site; ++index) {
        if (entryBytes(index)[fixupFormOffset] == static_cast<uint8_t>(form))
            return (*this)[index];
    }
    return std::nullopt;
}

ImageSectionsView::ImageSectionsView(std::span<const uint8_t> image, const ImageSectionHeader& header, const Layout& layout)
    : m_image(image)
    , m_header(header)
    , m_layout(layout)
{
}

ImageCallEntry ImageSectionsView::call(unsigned index) const
{
    using namespace ImageSectionInternal;
    ASSERT(index < m_header.callCount);
    auto entry = m_image.subspan(m_layout.calls + static_cast<size_t>(index) * imageCallEntrySize, imageCallEntrySize);
    return ImageCallEntry {
        .bytecodeIndex = BytecodeIndex::fromBits(readField<uint32_t>(entry, callBytecodeIndexOffset)),
        .doneLocation = readField<uint32_t>(entry, callDoneLocationOffset),
    };
}

ImageMoldEntry ImageSectionsView::mold(unsigned index) const
{
    using namespace ImageSectionInternal;
    ASSERT(index < m_header.moldCount);
    auto entry = m_image.subspan(m_layout.molds + static_cast<size_t>(index) * imageMoldEntrySize, imageMoldEntrySize);
    return ImageMoldEntry {
        .accessType = static_cast<AccessType>(static_cast<int8_t>(entry[moldAccessTypeOffset])),
        .preconfiguredCacheType = static_cast<CacheType>(static_cast<int8_t>(entry[moldCacheTypeOffset])),
        .flags = entry[moldFlagsOffset],
        .identifierKind = static_cast<MoldIdentifierKind>(entry[moldIdentifierKindOffset]),
        .identifier = readField<uint32_t>(entry, moldIdentifierOffset),
        .bytecodeIndex = BytecodeIndex::fromBits(readField<uint32_t>(entry, moldBytecodeIndexOffset)),
        .doneLocation = readField<uint32_t>(entry, moldDoneLocationOffset),
    };
}

ImageConstantPoolEntry ImageSectionsView::constantPoolEntry(unsigned index) const
{
    using namespace ImageSectionInternal;
    ASSERT(index < m_header.constantPoolCount);
    auto entry = m_image.subspan(m_layout.constantPool + static_cast<size_t>(index) * imageConstantPoolEntrySize, imageConstantPoolEntrySize);
    return ImageConstantPoolEntry {
        .type = static_cast<ImageConstantType>(readField<uint32_t>(entry, constantTypeOffset)),
        .index = readField<uint32_t>(entry, constantIndexOffset),
    };
}

ImageMathICEntry ImageSectionsView::mathIC(unsigned index) const
{
    using namespace ImageSectionInternal;
    ASSERT(index < m_header.mathICCount);
    auto entry = m_image.subspan(m_layout.mathICs + static_cast<size_t>(index) * imageMathICEntrySize, imageMathICEntrySize);
    return ImageMathICEntry {
        .kind = static_cast<MathICKind>(entry[mathICKindOffset]),
        .flags = entry[mathICFlagsOffset],
        .bytecodeOffset = readField<uint32_t>(entry, mathICBytecodeOffsetOffset),
        .inlineStart = readField<uint32_t>(entry, mathICInlineStartOffset),
        .inlineEnd = readField<uint32_t>(entry, mathICInlineEndOffset),
        .slowPathStart = readField<uint32_t>(entry, mathICSlowPathStartOffset),
        .slowPathCall = readField<uint32_t>(entry, mathICSlowPathCallOffset),
        .snippetSize = readField<uint32_t>(entry, mathICSnippetSizeOffset),
        .snippetFixupCount = readField<uint32_t>(entry, mathICSnippetFixupCountOffset),
    };
}

auto ImageSectionsView::locate(std::span<const uint8_t> image, const ImageSectionHeader& header) -> std::optional<Layout>
{
    using namespace ImageSectionInternal;
    // Every step stays inside the span before the next reads from it, so a count no span could hold stops the walk at
    // once. Offsets grow by at most 2^37 per step and cannot overflow.
    uint64_t size = image.size();
    uint64_t offset = imageSectionHeaderSize;
    auto advance = [&](uint64_t bytes) {
        offset += bytes;
        return offset <= size;
    };
    auto pad = [&] {
        offset = alignImageSectionOffset(offset);
        return offset <= size;
    };

    Layout layout;
    layout.code = offset;
    if (!advance(header.codeSize) || !pad())
        return std::nullopt;
    layout.fixups = offset;
    if (!advance(static_cast<uint64_t>(header.fixupCount) * imageFixupEntrySize))
        return std::nullopt;
    layout.calls = offset;
    if (!advance(static_cast<uint64_t>(header.callCount) * imageCallEntrySize))
        return std::nullopt;
    layout.molds = offset;
    if (!advance(static_cast<uint64_t>(header.moldCount) * imageMoldEntrySize))
        return std::nullopt;

    layout.simpleSwitchTables = offset;
    for (uint32_t index = 0; index < header.simpleSwitchTableCount; ++index) {
        if (!advance(2 * sizeof(uint32_t)))
            return std::nullopt;
        uint32_t entryCount = readField<uint32_t>(image, offset - sizeof(uint32_t));
        if (!advance(static_cast<uint64_t>(entryCount) * sizeof(uint32_t)))
            return std::nullopt;
    }
    layout.simpleSwitchTablesEnd = offset;
    if (!pad())
        return std::nullopt;

    layout.stringSwitchTables = offset;
    for (uint32_t index = 0; index < header.stringSwitchTableCount; ++index) {
        if (!advance(sizeof(uint32_t)))
            return std::nullopt;
        uint32_t entryCount = readField<uint32_t>(image, offset - sizeof(uint32_t));
        if (!advance(static_cast<uint64_t>(entryCount) * sizeof(uint32_t)))
            return std::nullopt;
    }
    layout.stringSwitchTablesEnd = offset;
    if (!pad())
        return std::nullopt;

    layout.codeMap = offset;
    if (!advance(static_cast<uint64_t>(header.codeMapCount) * sizeof(uint32_t)) || !pad())
        return std::nullopt;
    layout.constantPool = offset;
    if (!advance(static_cast<uint64_t>(header.constantPoolCount) * imageConstantPoolEntrySize))
        return std::nullopt;
    layout.mathICs = offset;
    if (!advance(static_cast<uint64_t>(header.mathICCount) * imageMathICEntrySize))
        return std::nullopt;

    // A snippet follows for each MathIC whose flags say it has one.
    layout.snippets = offset;
    for (uint32_t index = 0; index < header.mathICCount; ++index) {
        auto entry = image.subspan(layout.mathICs + static_cast<size_t>(index) * imageMathICEntrySize, imageMathICEntrySize);
        if (!(entry[mathICFlagsOffset] & ImageMathICFlags::hasSnippet))
            continue;
        uint32_t snippetSize = readField<uint32_t>(entry, mathICSnippetSizeOffset);
        uint32_t snippetFixupCount = readField<uint32_t>(entry, mathICSnippetFixupCountOffset);
        if (!advance(snippetSize) || !pad() || !advance(static_cast<uint64_t>(snippetFixupCount) * imageFixupEntrySize))
            return std::nullopt;
    }
    layout.end = offset;
    return layout;
}

std::expected<ImageSectionsView, ImageCheck> parseImageSections(const ImageSectionSpans& spans)
{
    if (spans.image.size() < imageSectionHeaderSize)
        return std::unexpected(ImageCheck::Header);
    auto header = ImageSectionInternal::decodeHeader(spans.image);
    auto layout = ImageSectionsView::locate(spans.image, header);
    if (!layout)
        return std::unexpected(ImageCheck::Header);

    ImageSectionsView view(spans.image, header, *layout);
    auto bakedFacts = parseBakedFactsSection(spans.bakedFacts);
    if (!bakedFacts)
        return std::unexpected(ImageCheck::BakedFacts);
    view.m_bakedFacts = *bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    // A twins build always receives the section, so a body without it cannot be located and is invalid material there.
    auto twins = ImageTwinsView::locate(spans.twins);
    if (!twins)
        return std::unexpected(ImageCheck::TwinsHeader);
    view.m_twins = *twins;
#endif
    return view;
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
