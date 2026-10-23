#pragma once

#if ENABLE(JIT)

#include "BytecodeIndex.h"
#include "jitcache/base/ByteFields.h"
#include "jitcache/image/BakedFacts.h"
#include "jitcache/image/ImageTypes.h"
#include <array>
#include <bit>
#include <expected>
#include <optional>
#include <span>
#include <wtf/Compiler.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/UniquedStringImpl.h>

#if ENABLE(JITCACHE_TWINS)
#include "jitcache/image/ImageTwinsSection.h"
#endif

// The image.baseline section: its entry codecs, the writer capture streams it through with every footprint canonical,
// and the view an import reads it through. The baked-facts.baseline codec lives in BakedFacts.h, the image-twins.baseline
// codec in ImageTwinsSection.h, and the per-architecture footprints in ImageFootprint.h. The Image lane, its glue and its
// tests include this header.

namespace JSC {

class VM;
enum class AccessType : int8_t;
enum class CacheType : int8_t;

namespace JITCache {

inline constexpr size_t imageSectionAlignment = 8; // every region starts 8-byte aligned, padded with zeros
inline constexpr size_t imageSectionHeaderSize = 56;
inline constexpr size_t imageFixupEntrySize = 24;
inline constexpr size_t imageCallEntrySize = 8;
inline constexpr size_t imageMoldEntrySize = 16;
inline constexpr size_t imageConstantPoolEntrySize = 8;
inline constexpr size_t imageMathICEntrySize = 32;

constexpr uint64_t alignImageSectionOffset(uint64_t offset)
{
    return (offset + imageSectionAlignment - 1) & ~static_cast<uint64_t>(imageSectionAlignment - 1);
}

// Entries of the image.baseline section.

struct ImageSectionHeader {
    uint32_t codeSize { 0 }; // the linked size, which LinkBuffer::size() reports
    uint32_t arityEntryOffset { 0 }; // 0 when the image has no arity-check entry
    uint32_t fixupCount { 0 };
    uint32_t callCount { 0 };
    uint32_t moldCount { 0 };
    uint32_t simpleSwitchTableCount { 0 };
    uint32_t stringSwitchTableCount { 0 };
    uint32_t codeMapCount { 0 };
    uint32_t constantPoolCount { 0 };
    uint32_t mathICCount { 0 };
    uint64_t livenessRateBits { 0 }; // m_livenessRate, IEEE bits
    uint64_t fullnessRateBits { 0 }; // m_fullnessRate, IEEE bits

    double livenessRate() const { return std::bit_cast<double>(livenessRateBits); }
    double fullnessRate() const { return std::bit_cast<double>(fullnessRateBits); }

    friend bool operator==(const ImageSectionHeader&, const ImageSectionHeader&) = default;
};

// The calls region: an entry of m_unlinkedCalls.
struct ImageCallEntry {
    BytecodeIndex bytecodeIndex; // stored as asBits()
    uint32_t doneLocation { 0 }; // offset into the image

    friend bool operator==(const ImageCallEntry&, const ImageCallEntry&) = default;
};

// The molds region: a mold of m_unlinkedPropertyInlineCaches.
enum class MoldIdentifierKind : uint8_t {
    None = 0,
    UCBIdentifier = 1, // identifier: an index into the UCB's identifiers
    ImmortalName = 2, // identifier: an ImmortalName
};

// The immortal names a baseline mold's identifier may hold, by the VM's own identifier (vm.propertyNames).
enum class ImmortalName : uint8_t {
    Length = 1,
    Next,
    Done,
    Value,
    SymbolHasInstance,
    Prototype,
};
inline constexpr uint8_t numberOfImmortalNames = static_cast<uint8_t>(ImmortalName::Prototype);

UniquedStringImpl* immortalNameImpl(VM&, ImmortalName);
std::optional<ImmortalName> immortalNameFor(VM&, const UniquedStringImpl*);

namespace ImageMoldFlags {
inline constexpr uint8_t propertyIsInt32 = 1 << 0;
inline constexpr uint8_t propertyIsString = 1 << 1;
inline constexpr uint8_t propertyIsSymbol = 1 << 2;
inline constexpr uint8_t prototypeIsKnownObject = 1 << 3;
inline constexpr uint8_t canBeMegamorphic = 1 << 4;
} // namespace ImageMoldFlags

struct ImageMoldEntry {
    AccessType accessType { };
    CacheType preconfiguredCacheType { };
    uint8_t flags { 0 }; // ImageMoldFlags
    MoldIdentifierKind identifierKind { MoldIdentifierKind::None };
    uint32_t identifier { 0 }; // 0 for None
    BytecodeIndex bytecodeIndex; // stored as asBits()
    uint32_t doneLocation { 0 }; // offset into the image

    friend bool operator==(const ImageMoldEntry&, const ImageMoldEntry&) = default;
};

// The constant pool region: an entry of m_constantPool, with JITConstantPool::Type's values.
enum class ImageConstantType : uint32_t {
    FunctionDecl = 0,
    FunctionExpr = 1,
};
struct ImageConstantPoolEntry {
    ImageConstantType type { };
    uint32_t index { 0 }; // into the UCB's function declarations or expressions

    friend bool operator==(const ImageConstantPoolEntry&, const ImageConstantPoolEntry&) = default;
};

// The MathICs region, by MathIC index.
namespace ImageMathICFlags {
inline constexpr uint8_t generateFastPathOnRepatch = 1 << 0;
inline constexpr uint8_t hasSnippet = 1 << 1;
inline constexpr uint8_t noInlineCode = 1 << 2;
} // namespace ImageMathICFlags

struct ImageMathICEntry {
    MathICKind kind { };
    uint8_t flags { 0 }; // ImageMathICFlags; exactly noInlineCode for a MathIC without inline code
    uint32_t bytecodeOffset { 0 };
    uint32_t inlineStart { 0 }; // offsets into the image, all zero without inline code
    uint32_t inlineEnd { 0 };
    uint32_t slowPathStart { 0 };
    uint32_t slowPathCall { 0 }; // the slow call's return point, m_slowPathCallLocation
    uint32_t snippetSize { 0 }; // the snippet's linked size, 0 without a snippet
    uint32_t snippetFixupCount { 0 };

    bool hasInlineCode() const { return !(flags & ImageMathICFlags::noInlineCode); }
    bool hasSnippet() const { return flags & ImageMathICFlags::hasSnippet; }
    bool generateFastPathOnRepatch() const { return flags & ImageMathICFlags::generateFastPathOnRepatch; }

    friend bool operator==(const ImageMathICEntry&, const ImageMathICEntry&) = default;
};

// Encoders: each entry built on the stack, as capture writes it. Reserved fields and padding are zero.
std::array<uint8_t, imageSectionHeaderSize> encodeImageSectionHeader(const ImageSectionHeader&);
std::array<uint8_t, imageFixupEntrySize> encodeImageFixup(const ImageFixup&);
std::array<uint8_t, imageCallEntrySize> encodeImageCall(const ImageCallEntry&);
std::array<uint8_t, imageMoldEntrySize> encodeImageMold(const ImageMoldEntry&);
std::array<uint8_t, imageConstantPoolEntrySize> encodeImageConstantPoolEntry(const ImageConstantPoolEntry&);
std::array<uint8_t, imageMathICEntrySize> encodeImageMathIC(const ImageMathICEntry&);
std::array<uint8_t, sizeof(uint32_t)> encodeImageOffset(uint32_t); // switch tables and the code map

// The section's size from the header's counts, the switch tables' entry counts and the snippets, so a writer knows it
// before it encodes anything.
class ImageSectionSize {
public:
    explicit ImageSectionSize(const ImageSectionHeader&); // the header and the regions its counts fix
    void addSimpleSwitchTable(uint32_t entryCount); // each simple switch table, in index order
    void addStringSwitchTable(uint32_t entryCount); // each string switch table, the key count plus one
    void addSnippet(uint32_t snippetSize, uint32_t snippetFixupCount); // each MathIC with a snippet
    uint64_t bytes() const;

private:
    uint64_t m_fixedBytes { 0 };
    uint64_t m_simpleSwitchTableBytes { 0 };
    uint64_t m_stringSwitchTableBytes { 0 };
    uint64_t m_snippetBytes { 0 };
};

// Streams a section through a sink with no buffer of its own, tracking the offset so that padding brings each region to
// its 8-byte boundary. Every function returns false once the sink refuses.
class ImageSectionWriter {
    WTF_MAKE_NONCOPYABLE(ImageSectionWriter);
public:
    explicit ImageSectionWriter(const ImageSectionSink&);

    [[nodiscard]] bool write(std::span<const uint8_t>);
    [[nodiscard]] bool pad(); // zeros up to the next 8-byte boundary
    // Code in canonical form: the live bytes from one footprint's end to the next one's start, then that
    // footprint's canonical encoding. The fixups are in footprint order, every footprint inside liveCode.
    [[nodiscard]] bool writeCanonicalCode(std::span<const uint8_t> liveCode, std::span<const ImageFixup>);

    uint64_t offset() const { return m_offset; }

private:
    const ImageSectionSink& m_sink;
    uint64_t m_offset { 0 };
};

// Readers. A view's accessors read the borrowed bytes with memcpy-based loads on access, so they hold no copy.

class ImageOffsetArray {
public:
    ImageOffsetArray() = default;
    explicit ImageOffsetArray(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
    {
        ASSERT(!(bytes.size() % sizeof(uint32_t)));
    }

    size_t size() const { return m_bytes.size() / sizeof(uint32_t); }
    uint32_t operator[](size_t index) const { return readField<uint32_t>(m_bytes, index * sizeof(uint32_t)); }

private:
    std::span<const uint8_t> m_bytes;
};

class ImageFixupArray {
public:
    ImageFixupArray() = default;
    explicit ImageFixupArray(std::span<const uint8_t> bytes)
        : m_bytes(bytes)
    {
        ASSERT(!(bytes.size() % imageFixupEntrySize));
    }

    size_t size() const { return m_bytes.size() / imageFixupEntrySize; }
    ImageFixup operator[](size_t) const;
    // The fixup with this site and form, which name at most one fixup: a binary search, valid on fixups in footprint
    // order.
    std::optional<ImageFixup> find(uint32_t site, FixupForm) const;

    std::span<const uint8_t> entryBytes(size_t index) const { return m_bytes.subspan(index * imageFixupEntrySize, imageFixupEntrySize); }

private:
    std::span<const uint8_t> m_bytes;
};

struct ImageSimpleSwitchTable {
    uint32_t defaultOffset { 0 }; // m_ctiDefault, which JIT::link sets for every table, list tables included
    ImageOffsetArray offsets; // m_ctiOffsets; empty for a list table
};

struct ImageSnippet {
    std::span<const uint8_t> code; // snippetSize canonical bytes
    ImageFixupArray fixups; // sites relative to the snippet, in footprint order
};

// The lane's sections of one body, each a span that starts 8-byte aligned and covers exactly the section, as the
// integrator hands them over.
struct ImageSectionSpans {
    std::span<const uint8_t> image;
    std::span<const uint8_t> bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    std::span<const uint8_t> twins; // image-twins.baseline, which ImageTwinsView parses
#endif
};

// Spans into the borrowed payload, valid while it lives.
class ImageSectionsView {
public:
    const ImageSectionHeader& header() const { return m_header; }
    std::span<const uint8_t> code() const { return m_image.subspan(m_layout.code, m_header.codeSize); } // canonical
    ImageFixupArray fixups() const { return ImageFixupArray(m_image.subspan(m_layout.fixups, static_cast<size_t>(m_header.fixupCount) * imageFixupEntrySize)); }
    ImageCallEntry call(unsigned index) const;
    ImageMoldEntry mold(unsigned index) const; // in mold order
    // functor(unsigned tableIndex, const ImageSimpleSwitchTable&), in table index order.
    template<typename Functor> void forEachSimpleSwitchTable(const Functor&) const;
    // functor(unsigned tableIndex, const ImageOffsetArray& offsets), in table index order: the offsets of m_ctiOffsets by
    // m_indexInTable, the default last.
    template<typename Functor> void forEachStringSwitchTable(const Functor&) const;
    ImageOffsetArray codeMap() const { return ImageOffsetArray(m_image.subspan(m_layout.codeMap, static_cast<size_t>(m_header.codeMapCount) * sizeof(uint32_t))); }
    ImageConstantPoolEntry constantPoolEntry(unsigned index) const;
    ImageMathICEntry mathIC(unsigned index) const;
    // functor(unsigned mathICIndex, const ImageSnippet&), for each MathIC with a snippet, in MathIC index order.
    template<typename Functor> void forEachSnippet(const Functor&) const;
    const BakedFactsView& bakedFacts() const { return m_bakedFacts; }
#if ENABLE(JITCACHE_TWINS)
    // The parsed image-twins.baseline, which a twins build always receives.
    const ImageTwinsView& twins() const LIFETIME_BOUND { return m_twins; }
#endif

private:
    // Offsets into the image section, each region's start, and the end of the regions with padding after them.
    struct Layout {
        size_t code { 0 };
        size_t fixups { 0 };
        size_t calls { 0 };
        size_t molds { 0 };
        size_t simpleSwitchTables { 0 };
        size_t simpleSwitchTablesEnd { 0 };
        size_t stringSwitchTables { 0 };
        size_t stringSwitchTablesEnd { 0 };
        size_t codeMap { 0 };
        size_t constantPool { 0 };
        size_t mathICs { 0 };
        size_t snippets { 0 };
        size_t end { 0 };
    };

    friend std::expected<ImageSectionsView, ImageCheck> parseImageSections(const ImageSectionSpans&);
    // parseImageSections locates the baked facts once it located the image.
    ImageSectionsView(std::span<const uint8_t> image, const ImageSectionHeader&, const Layout&);

    static std::optional<Layout> locate(std::span<const uint8_t> image, const ImageSectionHeader&);

    std::span<const uint8_t> m_image;
    ImageSectionHeader m_header;
    Layout m_layout;
    BakedFactsView m_bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    ImageTwinsView m_twins;
#endif
};

// Locates the regions of image.baseline and baked-facts.baseline, and in twins builds image-twins.baseline, and trusts
// what capture wrote in them. A section whose regions do not fit inside its span cannot be located and fails
// ImageCheck::Header, BakedFacts or TwinsHeader.
std::expected<ImageSectionsView, ImageCheck> parseImageSections(const ImageSectionSpans&);

template<typename Functor>
void ImageSectionsView::forEachSimpleSwitchTable(const Functor& functor) const
{
    // Each table: u32 offset of m_ctiDefault, u32 entryCount, then entryCount u32 offsets. locate kept them in bounds.
    auto region = m_image.subspan(m_layout.simpleSwitchTables, m_layout.simpleSwitchTablesEnd - m_layout.simpleSwitchTables);
    size_t offset = 0;
    for (unsigned index = 0; index < m_header.simpleSwitchTableCount; ++index) {
        uint32_t defaultOffset = readField<uint32_t>(region, offset);
        size_t entryBytes = static_cast<size_t>(readField<uint32_t>(region, offset + sizeof(uint32_t))) * sizeof(uint32_t);
        offset += 2 * sizeof(uint32_t);
        ImageSimpleSwitchTable table { defaultOffset, ImageOffsetArray(region.subspan(offset, entryBytes)) };
        offset += entryBytes;
        functor(index, table);
    }
}

template<typename Functor>
void ImageSectionsView::forEachStringSwitchTable(const Functor& functor) const
{
    // Each table: u32 entryCount, then entryCount u32 offsets.
    auto region = m_image.subspan(m_layout.stringSwitchTables, m_layout.stringSwitchTablesEnd - m_layout.stringSwitchTables);
    size_t offset = 0;
    for (unsigned index = 0; index < m_header.stringSwitchTableCount; ++index) {
        size_t entryBytes = static_cast<size_t>(readField<uint32_t>(region, offset)) * sizeof(uint32_t);
        offset += sizeof(uint32_t);
        ImageOffsetArray offsets(region.subspan(offset, entryBytes));
        offset += entryBytes;
        functor(index, offsets);
    }
}

template<typename Functor>
void ImageSectionsView::forEachSnippet(const Functor& functor) const
{
    // Each snippet: snippetSize bytes, padded to 8, then snippetFixupCount fixup entries. The region starts 8-byte
    // aligned, so padding its offsets pads the section's.
    auto region = m_image.subspan(m_layout.snippets, m_layout.end - m_layout.snippets);
    size_t offset = 0;
    for (unsigned index = 0; index < m_header.mathICCount; ++index) {
        auto entry = mathIC(index);
        if (!entry.hasSnippet())
            continue;
        auto code = region.subspan(offset, entry.snippetSize);
        offset = alignImageSectionOffset(offset + entry.snippetSize);
        size_t fixupBytes = static_cast<size_t>(entry.snippetFixupCount) * imageFixupEntrySize;
        ImageSnippet snippet { code, ImageFixupArray(region.subspan(offset, fixupBytes)) };
        offset += fixupBytes;
        functor(index, snippet);
    }
}

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
