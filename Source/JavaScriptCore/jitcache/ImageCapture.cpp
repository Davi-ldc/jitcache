#include "config.h"
#include "ImageCapture.h"

#if ENABLE(JIT)

#include "BakedFacts.h"
#include "BaselineJITCode.h"
#include "CacheableIdentifierInlines.h"
#include "CodeBlock.h"
#include "ExecutableAllocator.h"
#include "ImageRecord.h"
#include "ImageSupport.h"
#include "JITCodeMap.h"
#include "JITMathIC.h"
#include "JumpTable.h"
#include "MacroAssembler.h"
#include "ProducerBudget.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <utility>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache {

namespace ImageCaptureInternal {

// A charge to the producer budget, made before the allocation it pays for and released when it goes out of scope,
// unless the capture takes it over.
class Charge {
    WTF_MAKE_NONCOPYABLE(Charge);
public:
    explicit Charge(ProducerBudget& budget)
        : m_budget(budget)
    {
    }

    ~Charge()
    {
        if (m_bytes)
            m_budget.release(m_bytes);
    }

    // False once the budget refuses, which raises the recording fault itself (R-INT-2).
    [[nodiscard]] bool add(size_t bytes)
    {
        if (!bytes)
            return true;
        if (!m_budget.tryCharge(bytes))
            return false;
        m_bytes += bytes;
        return true;
    }

    size_t take() { return std::exchange(m_bytes, 0); }

private:
    ProducerBudget& m_budget;
    size_t m_bytes { 0 };
};

static std::span<const uint8_t> bytesAt(const void* start, size_t size)
{
    return unsafeMakeSpan(static_cast<const uint8_t*>(start), size);
}

// The offset of a location in code of this size, inside [0, size]; nullopt outside it.
static std::optional<uint32_t> offsetInCode(const void* start, uint32_t size, const void* location)
{
    auto address = reinterpret_cast<uintptr_t>(location);
    auto base = reinterpret_cast<uintptr_t>(start);
    if (address < base || address - base > size)
        return std::nullopt;
    return static_cast<uint32_t>(address - base);
}

// What capture reads from a MathIC besides its locations: the regeneration flag and the snippet m_code holds.
struct MathICCode {
    bool generateFastPathOnRepatch { false };
    const void* snippetStart { nullptr }; // null without m_code
    size_t snippetHandleSize { 0 }; // m_code.size(), the handle's size, which can exceed the linked size (N20)
};

template<typename MathIC>
static MathICCode codeOf(const MathIC& mathIC)
{
    return MathICCode {
        .generateFastPathOnRepatch = mathIC.m_generateFastPathOnRepatch,
        .snippetStart = mathIC.m_code ? mathIC.m_code.code().untaggedPtr() : nullptr,
        .snippetHandleSize = mathIC.m_code.size(),
    };
}

static MathICCode mathICCode(MathICKind kind, const void* mathIC)
{
    switch (kind) {
    case MathICKind::Add:
        return codeOf(*static_cast<const JITAddIC*>(mathIC));
    case MathICKind::Sub:
        return codeOf(*static_cast<const JITSubIC*>(mathIC));
    case MathICKind::Mul:
        return codeOf(*static_cast<const JITMulIC*>(mathIC));
    case MathICKind::Negate:
        return codeOf(*static_cast<const JITNegIC*>(mathIC));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename MathIC>
static constexpr MathICKind mathICKindOf()
{
    if constexpr (std::is_same_v<MathIC, JITAddIC>)
        return MathICKind::Add;
    else if constexpr (std::is_same_v<MathIC, JITSubIC>)
        return MathICKind::Sub;
    else if constexpr (std::is_same_v<MathIC, JITMulIC>)
        return MathICKind::Mul;
    else {
        static_assert(std::is_same_v<MathIC, JITNegIC>);
        return MathICKind::Negate;
    }
}

// A MathIC has a snippet when the record holds the provenance of the one its m_code holds, which step 4 matched; only a
// MathIC with inline code regenerates (N17).
static bool hasSnippet(const MathICRecord& mathIC)
{
    ASSERT(!mathIC.snippet || mathIC.slowCallPointerSite);
    return mathIC.snippet.has_value();
}

static uint8_t moldFlagsOf(const BaselineUnlinkedPropertyInlineCache& mold)
{
    uint8_t flags = 0;
    if (mold.propertyIsInt32)
        flags |= ImageMoldFlags::propertyIsInt32;
    if (mold.propertyIsString)
        flags |= ImageMoldFlags::propertyIsString;
    if (mold.propertyIsSymbol)
        flags |= ImageMoldFlags::propertyIsSymbol;
    if (mold.prototypeIsKnownObject)
        flags |= ImageMoldFlags::prototypeIsKnownObject;
    if (mold.canBeMegamorphic)
        flags |= ImageMoldFlags::canBeMegamorphic;
    return flags;
}

static ImageConstantType constantTypeOf(JITConstantPool::Type type)
{
    switch (type) {
    case JITConstantPool::Type::FunctionDecl:
        return ImageConstantType::FunctionDecl;
    case JITConstantPool::Type::FunctionExpr:
        return ImageConstantType::FunctionExpr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Step 3's map from the impl of each UCB identifier to its lowest index: a vector sorted by impl, whose exact bytes are
// charged before it is allocated. The lowest index makes the choice the same in every process whose UCB has the
// producer's identifiers (R-UCB-1), so a capture of an imported image writes the identifier fields it imported (I9).
class UCBIdentifierIndex {
    WTF_MAKE_NONCOPYABLE(UCBIdentifierIndex);
public:
    static size_t storageBytes(const UnlinkedCodeBlock& unlinkedCodeBlock)
    {
        return unlinkedCodeBlock.numberOfIdentifiers() * sizeof(Entry);
    }

    explicit UCBIdentifierIndex(const UnlinkedCodeBlock& unlinkedCodeBlock)
        : m_entries(unlinkedCodeBlock.numberOfIdentifiers(), [&](size_t index) {
            return Entry { unlinkedCodeBlock.identifier(static_cast<int>(index)).impl(), static_cast<uint32_t>(index) };
        })
    {
        std::ranges::sort(m_entries.mutableSpan(), [](const Entry& a, const Entry& b) {
            if (a.impl != b.impl)
                return std::less<const UniquedStringImpl*> { }(a.impl, b.impl);
            return a.index < b.index;
        });
    }

    std::optional<uint32_t> indexOf(const UniquedStringImpl* impl) const
    {
        auto entries = m_entries.span();
        auto entry = std::ranges::lower_bound(entries, impl, std::less<const UniquedStringImpl*> { }, &Entry::impl);
        if (entry == entries.end() || entry->impl != impl)
            return std::nullopt;
        return entry->index;
    }

private:
    struct Entry {
        const UniquedStringImpl* impl;
        uint32_t index;
    };

    Vector<Entry> m_entries;
};

// The capturing VM's resolution context (section 9, step 7): the image start, the record's MathICs, the dense switch
// tables' storage, the string tables JIT::link filled, the snippets' starts and the UCB's string switch ranks.
// storageBytes is what its arrays take, which the caller charges before constructing it.
class ProducerResolution {
    WTF_MAKE_NONCOPYABLE(ProducerResolution);
public:
    static size_t storageBytes(const UnlinkedCodeBlock& unlinkedCodeBlock, const BaselineJITCode& code, const ImageRecord& record)
    {
        size_t bytes = 2 * record.mathICs().size() * sizeof(void*) + code.m_switchJumpTables.size() * sizeof(void*);
        // rankStringSwitches holds one vector per string table, and in each table with an inline tree one rank per key.
        size_t tableCount = unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables();
        bytes += tableCount * sizeof(Vector<StringSwitchRank>);
        for (size_t index = 0; index < tableCount; ++index) {
            size_t keyCount = unlinkedCodeBlock.unlinkedStringSwitchJumpTable(static_cast<int>(index)).m_offsetTable.size();
            if (hasInlineStringSwitch(keyCount))
                bytes += keyCount * sizeof(StringSwitchRank);
        }
        return bytes;
    }

    ProducerResolution(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, BaselineJITCode& code, const ImageRecord& record)
        : m_mathICs(record.mathICs().size(), [&](size_t index) {
            return record.mathICs()[index].mathIC;
        })
        , m_switchTableBases(code.m_switchJumpTables.size(), [&](size_t index) -> const void* {
            // The storage JIT::emit_op_switch_imm and emit_op_switch_char move into a register; a list table has none.
            auto& offsets = code.m_switchJumpTables[index].m_ctiOffsets;
            return offsets.isEmpty() ? nullptr : offsets.span().data();
        })
        , m_snippetStarts(record.mathICs().size(), [&](size_t index) -> const void* {
            auto& mathIC = record.mathICs()[index];
            return hasSnippet(mathIC) ? mathIC.snippet->start : nullptr;
        })
        , m_ranks(rankStringSwitches(unlinkedCodeBlock))
        , m_context {
            .vm = vm,
            .ucb = &unlinkedCodeBlock,
            .imageStart = code.start(),
            .mathICs = m_mathICs.span(),
            .switchTableBases = m_switchTableBases.span(),
            .stringSwitchTables = code.m_stringSwitchJumpTables.span(),
            .snippetStarts = m_snippetStarts.span(),
            .ranks = &m_ranks,
        }
    {
        ASSERT(heldBytes() == storageBytes(unlinkedCodeBlock, code, record));
    }

    const ResolutionContext& context() const LIFETIME_BOUND { return m_context; }

private:
    size_t heldBytes() const
    {
        size_t bytes = (m_mathICs.capacity() + m_switchTableBases.capacity() + m_snippetStarts.capacity()) * sizeof(void*);
        bytes += m_ranks.tables.capacity() * sizeof(Vector<StringSwitchRank>);
        for (auto& table : m_ranks.tables)
            bytes += table.capacity() * sizeof(StringSwitchRank);
        return bytes;
    }

    Vector<void*> m_mathICs; // by MathIC index
    Vector<const void*> m_switchTableBases; // by simple switch table, null for a list table
    Vector<const void*> m_snippetStarts; // by MathIC index, null without a snippet
    StringSwitchRanks m_ranks;
    ResolutionContext m_context;
};

// S1 and S2 for one footprint at footprintAddress: it holds its form's instruction (table 3.2) and reaches expected. A
// Pointer holds it; a Call or a Jump branches to it, on ARM64 through the unconditional b of each jump island on the
// way, at most the pool's size divided by half of MacroAssembler::nearJumpRange, rounded up, which bounds the chains
// FixedVMPoolExecutableAllocator::islandForJumpLocation builds for any pool size.
static bool footprintReachesTarget(FixupForm form, std::span<const uint8_t> footprint, uintptr_t footprintAddress, uintptr_t expected)
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
    constexpr size_t islandSize = 4; // one b
    auto poolStart = startOfFixedExecutableMemoryPool<uintptr_t>();
    auto poolEnd = endOfFixedExecutableMemoryPool<uintptr_t>();
    if (poolEnd < poolStart || poolEnd - poolStart < islandSize)
        return false;
    constexpr size_t halfRange = MacroAssembler::nearJumpRange / 2;
    size_t maximumIslands = (poolEnd - poolStart + halfRange - 1) / halfRange;
    for (size_t island = 0; island < maximumIslands; ++island) {
        if (target < poolStart || target > poolEnd - islandSize || target % islandSize)
            return false;
        auto next = decodeFootprint(FixupForm::Jump, bytesAt(reinterpret_cast<const void*>(target), islandSize), target);
        if (!next)
            return false;
        target = *next;
        if (target == expected)
            return true;
    }
#endif
    return false;
}

// S1 over the image, S2 over a snippet: every fixup decodes as its form to the producer's resolution of its target.
static bool fixupsReachTargets(std::span<const uint8_t> code, std::span<const ImageFixup> fixups, const ResolutionContext& context)
{
    auto codeStart = reinterpret_cast<uintptr_t>(code.data());
    for (auto& fixup : fixups) {
        auto footprint = fixupFootprint(fixup.form, fixup.site, code);
        if (!footprint)
            return false;
        auto expected = reinterpret_cast<uintptr_t>(resolveTarget(context, fixup.target));
        if (!footprintReachesTarget(fixup.form, code.subspan(footprint->begin, footprint->size()), codeStart + footprint->begin, expected))
            return false;
    }
    return true;
}

// S3: the holder owns as many ICs of each kind as the record lists.
static bool holderMatchesRecord(const BaselineJITCode& code, const ImageRecord& record)
{
    constexpr size_t kindCount = static_cast<size_t>(MathICKind::Negate) + 1;
    std::array<size_t, kindCount> listed { };
    for (auto& mathIC : record.mathICs())
        ++listed[static_cast<size_t>(mathIC.kind)];
    std::array<size_t, kindCount> owned { };
    code.forEachMathIC([&]<typename MathIC>(MathIC&) {
        ++owned[static_cast<size_t>(mathICKindOf<MathIC>())];
    });
    return listed == owned;
}

// S4: every code pointer of the side tables, and the four locations of every MathIC with inline code, lie inside
// [start, start + codeSize]; every MathIC the record lists without inline code still has null locations, no m_code and
// its regeneration flag clear.
static bool pointersLieInImage(BaselineJITCode& code, const ImageRecord& record)
{
    const void* start = code.start();
    uint32_t codeSize = record.codeSize();
    auto inImage = [&](const void* location) {
        return offsetInCode(start, codeSize, location).has_value();
    };

    for (auto& call : code.m_unlinkedCalls) {
        if (!inImage(call.doneLocation.untaggedPtr()))
            return false;
    }
    for (auto& mold : code.m_unlinkedPropertyInlineCaches) {
        if (!inImage(mold.doneLocation.untaggedPtr()))
            return false;
    }
    for (auto& table : code.m_switchJumpTables) {
        if (!inImage(table.m_ctiDefault.untaggedPtr()))
            return false;
        for (auto& location : table.m_ctiOffsets) {
            if (!inImage(location.untaggedPtr()))
                return false;
        }
    }
    for (auto& table : code.m_stringSwitchJumpTables) {
        for (auto& location : table.m_ctiOffsets) {
            if (!inImage(location.untaggedPtr()))
                return false;
        }
    }
    bool codeMapInImage = true;
    code.m_jitCodeMap.forEach([&](BytecodeIndex, CodeLocationLabel<JSEntryPtrTag> location) {
        codeMapInImage = codeMapInImage && inImage(location.untaggedPtr());
    });
    if (!codeMapInImage)
        return false;

    for (auto& mathIC : record.mathICs()) {
        auto locations = mathICLocations(mathIC.kind, mathIC.mathIC);
        if (mathIC.slowCallPointerSite) {
            if (!inImage(locations.inlineStart) || !inImage(locations.inlineEnd) || !inImage(locations.slowPathStart) || !inImage(locations.slowPathCall))
                return false;
            continue;
        }
        auto liveCode = mathICCode(mathIC.kind, mathIC.mathIC);
        if (!locations.areAllNull() || liveCode.snippetStart || liveCode.generateFastPathOnRepatch)
            return false;
    }
    return true;
}

// Step 7: S1 to S4, in order. The resolution S1 and S2 compare against is charged while it lives.
static std::optional<CaptureFailure> firstStrictFailure(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, BaselineJITCode& code, const ImageRecord& record, ProducerBudget& budget)
{
    Charge charge(budget);
    if (!charge.add(ProducerResolution::storageBytes(unlinkedCodeBlock, code, record)))
        return CaptureFailure { CaptureOutcome::ChargeRefused, ImageCheck::None };
    ProducerResolution resolution(vm, unlinkedCodeBlock, code, record);

    if (!fixupsReachTargets(bytesAt(code.start(), record.codeSize()), record.fixups().span(), resolution.context()))
        return CaptureFailure { CaptureOutcome::RecordingFault, ImageCheck::S1 };
    for (auto& mathIC : record.mathICs()) {
        if (!hasSnippet(mathIC))
            continue;
        auto& snippet = *mathIC.snippet;
        if (!fixupsReachTargets(bytesAt(snippet.start, snippet.size), snippet.fixups.span(), resolution.context()))
            return CaptureFailure { CaptureOutcome::RecordingFault, ImageCheck::S2 };
    }
    if (!holderMatchesRecord(code, record))
        return CaptureFailure { CaptureOutcome::RecordingFault, ImageCheck::S3 };
    if (!pointersLieInImage(code, record))
        return CaptureFailure { CaptureOutcome::RecordingFault, ImageCheck::S4 };
    return std::nullopt;
}

} // namespace ImageCaptureInternal

bool isImageCapturable(const BaselineJITCode& code)
{
    auto* record = code.m_jitCacheImageRecord.get();
    return record && record->state() == RecordState::Complete;
}

ImageCapture::ImageCapture(BaselineJITCode& code, ProducerBudget& budget, size_t chargedBytes, const ImageSectionHeader& header, Vector<MoldIdentifier>&& moldIdentifiers)
    : m_code(code)
    , m_budget(budget)
    , m_chargedBytes(chargedBytes)
    , m_imageStart(code.start())
    , m_header(header)
    , m_moldIdentifiers(WTF::move(moldIdentifiers))
{
    ASSERT(m_chargedBytes == m_moldIdentifiers.capacity() * sizeof(MoldIdentifier));
}

ImageCapture::ImageCapture(ImageCapture&& other)
    : m_code(WTF::move(other.m_code))
    , m_budget(WTF::move(other.m_budget))
    , m_chargedBytes(std::exchange(other.m_chargedBytes, 0))
    , m_imageStart(other.m_imageStart)
    , m_header(other.m_header)
    , m_moldIdentifiers(WTF::move(other.m_moldIdentifiers))
{
}

ImageCapture::~ImageCapture()
{
    // A moved-from capture holds no charge and never touches its moved budget.
    if (m_chargedBytes)
        m_budget->release(m_chargedBytes);
}

uint32_t ImageCapture::imageOffsetOf(const void* location) const
{
    ASSERT(ImageCaptureInternal::offsetInCode(m_imageStart, m_header.codeSize, location));
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(location) - reinterpret_cast<uintptr_t>(m_imageStart));
}

ImageMathICEntry ImageCapture::mathICEntryOf(const MathICRecord& mathIC) const
{
    using namespace ImageCaptureInternal;
    ImageMathICEntry entry {
        .kind = mathIC.kind,
        .bytecodeOffset = mathIC.bytecodeIndex.offset(),
    };
    if (!mathIC.slowCallPointerSite) {
        // Written from the record alone: exactly the no-inline-code flag, every later field zero (section 8.2, region 9).
        entry.flags = ImageMathICFlags::noInlineCode;
        return entry;
    }
    auto locations = mathICLocations(mathIC.kind, mathIC.mathIC);
    entry.inlineStart = imageOffsetOf(locations.inlineStart);
    entry.inlineEnd = imageOffsetOf(locations.inlineEnd);
    entry.slowPathStart = imageOffsetOf(locations.slowPathStart);
    entry.slowPathCall = imageOffsetOf(locations.slowPathCall);
    if (mathICCode(mathIC.kind, mathIC.mathIC).generateFastPathOnRepatch)
        entry.flags |= ImageMathICFlags::generateFastPathOnRepatch;
    if (hasSnippet(mathIC)) {
        entry.flags |= ImageMathICFlags::hasSnippet;
        entry.snippetSize = mathIC.snippet->size;
        entry.snippetFixupCount = static_cast<uint32_t>(mathIC.snippet->fixups.size());
    }
    return entry;
}

size_t ImageCapture::imageSectionSize() const
{
    using namespace ImageCaptureInternal;
    auto& code = m_code.get();
    auto& record = *code.m_jitCacheImageRecord;
    ImageSectionSize size(m_header);
    for (auto& table : code.m_switchJumpTables)
        size.addSimpleSwitchTable(static_cast<uint32_t>(table.m_ctiOffsets.size()));
    for (auto& table : code.m_stringSwitchJumpTables)
        size.addStringSwitchTable(static_cast<uint32_t>(table.m_ctiOffsets.size()));
    for (auto& mathIC : record.mathICs()) {
        if (hasSnippet(mathIC))
            size.addSnippet(mathIC.snippet->size, static_cast<uint32_t>(mathIC.snippet->fixups.size()));
    }
    return static_cast<size_t>(size.bytes());
}

size_t ImageCapture::bakedFactsSectionSize() const
{
    return JITCache::bakedFactsSectionSize(m_code->m_jitCacheImageRecord->bakedFacts());
}

bool ImageCapture::writeBakedFactsSection(const ImageSectionSink& sink) const
{
    return JITCache::writeBakedFactsSection(m_code->m_jitCacheImageRecord->bakedFacts(), sink);
}

bool ImageCapture::writeImageSection(const ImageSectionSink& sink) const
{
    using namespace ImageCaptureInternal;
    auto& code = m_code.get();
    auto& record = *code.m_jitCacheImageRecord;
    ASSERT(record.state() == RecordState::Complete);
    ImageSectionWriter writer(sink);
    auto writeOffset = [&](const void* location) {
        return writer.write(encodeImageOffset(imageOffsetOf(location)));
    };
    auto writeCount = [&](size_t count) {
        uint32_t value = static_cast<uint32_t>(count);
        return writer.write(asByteSpan(value));
    };

    if (!writer.write(encodeImageSectionHeader(m_header)))
        return false;

    // Region 1: the live bytes up to the linked size, with every footprint in its canonical encoding (section 8.4).
    if (!writer.writeCanonicalCode(bytesAt(m_imageStart, m_header.codeSize), record.fixups().span()) || !writer.pad())
        return false;

    // Region 2.
    for (auto& fixup : record.fixups()) {
        if (!writer.write(encodeImageFixup(fixup)))
            return false;
    }

    // Region 3, in the order JIT::link sorted the calls.
    for (auto& call : code.m_unlinkedCalls) {
        ImageCallEntry entry {
            .bytecodeIndex = call.bytecodeIndex,
            .doneLocation = imageOffsetOf(call.doneLocation.untaggedPtr()),
        };
        if (!writer.write(encodeImageCall(entry)))
            return false;
    }

    // Region 4, in mold order, each identifier field as step 3 classified it.
    for (size_t index = 0; index < code.m_unlinkedPropertyInlineCaches.size(); ++index) {
        auto& mold = code.m_unlinkedPropertyInlineCaches[index];
        auto& identifier = m_moldIdentifiers[index];
        ImageMoldEntry entry {
            .accessType = mold.accessType,
            .preconfiguredCacheType = mold.preconfiguredCacheType,
            .flags = moldFlagsOf(mold),
            .identifierKind = identifier.kind,
            .identifier = identifier.value,
            .bytecodeIndex = mold.bytecodeIndex,
            .doneLocation = imageOffsetOf(mold.doneLocation.untaggedPtr()),
        };
        if (!writer.write(encodeImageMold(entry)))
            return false;
    }

    // Region 5: m_ctiDefault, which JIT::link sets for every table, list tables included, then a dense table's offsets.
    for (auto& table : code.m_switchJumpTables) {
        if (!writeOffset(table.m_ctiDefault.untaggedPtr()) || !writeCount(table.m_ctiOffsets.size()))
            return false;
        for (auto& location : table.m_ctiOffsets) {
            if (!writeOffset(location.untaggedPtr()))
                return false;
        }
    }
    if (!writer.pad())
        return false;

    // Region 6: each table's offsets by m_indexInTable, the default last, as JIT::link filled them.
    for (auto& table : code.m_stringSwitchJumpTables) {
        if (!writeCount(table.m_ctiOffsets.size()))
            return false;
        for (auto& location : table.m_ctiOffsets) {
            if (!writeOffset(location.untaggedPtr()))
                return false;
        }
    }
    if (!writer.pad())
        return false;

    // Region 7: one offset per instruction start, in bytecode order; install pairs them with the UCB's starts again.
    bool wroteCodeMap = true;
    code.m_jitCodeMap.forEach([&](BytecodeIndex, CodeLocationLabel<JSEntryPtrTag> location) {
        wroteCodeMap = wroteCodeMap && writeOffset(location.untaggedPtr());
    });
    if (!wroteCodeMap || !writer.pad())
        return false;

    // Region 8.
    for (size_t index = 0; index < code.m_constantPool.size(); ++index) {
        auto constant = code.m_constantPool.at(index);
        // JIT::addToConstantPool stores the declaration or expression index as the payload pointer.
        ImageConstantPoolEntry entry {
            .type = constantTypeOf(constant.type()),
            .index = static_cast<uint32_t>(std::bit_cast<uintptr_t>(constant.pointer())),
        };
        if (!writer.write(encodeImageConstantPoolEntry(entry)))
            return false;
    }

    // Region 9, by MathIC index.
    for (auto& mathIC : record.mathICs()) {
        if (!writer.write(encodeImageMathIC(mathICEntryOf(mathIC))))
            return false;
    }

    // Region 10: each snippet's live bytes up to its linked size, canonical like the image's, then its fixups.
    for (auto& mathIC : record.mathICs()) {
        if (!hasSnippet(mathIC))
            continue;
        auto& snippet = *mathIC.snippet;
        if (!writer.writeCanonicalCode(bytesAt(snippet.start, snippet.size), snippet.fixups.span()) || !writer.pad())
            return false;
        for (auto& fixup : snippet.fixups) {
            if (!writer.write(encodeImageFixup(fixup)))
                return false;
        }
    }

    ASSERT(writer.offset() == imageSectionSize());
    return true;
}

std::expected<ImageCapture, CaptureFailure> captureImage(VM& vm, CodeBlock& codeBlock, BaselineJITCode& code, ProducerBudget& budget, bool strict)
{
    using namespace ImageCaptureInternal;
    ASSERT(codeBlock.jitCode().get() == static_cast<JSC::JITCode*>(&code));
    if (!isImageCapturable(code))
        return std::unexpected(CaptureFailure { CaptureOutcome::NotEligible, ImageCheck::None });
    auto& record = *code.m_jitCacheImageRecord;
    auto& unlinkedCodeBlock = *codeBlock.unlinkedCodeBlock();
    auto notEligible = [&](Unrecordable reason) {
        record.markUnrecordable(reason);
        return std::unexpected(CaptureFailure { CaptureOutcome::NotEligible, ImageCheck::None });
    };
    auto chargeRefused = [] {
        return std::unexpected(CaptureFailure { CaptureOutcome::ChargeRefused, ImageCheck::None });
    };

    // Step 1. The image runs from the normal entry to the record's linked size; the code reference's own size is the
    // handle's, which can extend past the linked bytes (N20). Without an arity-check entry, the arity entry is the normal
    // entry, offset 0.
    const void* imageStart = code.start();
    uint32_t codeSize = record.codeSize();
    auto arityEntryOffset = offsetInCode(imageStart, codeSize, code.addressForCall(ArityCheckMode::MustCheckArity).untaggedPtr());
    ASSERT(arityEntryOffset && *arityEntryOffset < codeSize);

    // Step 2 converts the side tables' code pointers to offsets as the writes stream them (imageOffsetOf); S4 checks
    // them under strict.

    // Step 3: each mold's identifier field, kept for the writes and charged for as long as the capture lives.
    auto& molds = code.m_unlinkedPropertyInlineCaches;
    Charge keptCharge(budget);
    if (!keptCharge.add(molds.size() * sizeof(ImageCapture::MoldIdentifier)))
        return chargeRefused();
    Vector<ImageCapture::MoldIdentifier> moldIdentifiers;
    moldIdentifiers.reserveInitialCapacity(molds.size());
    {
        bool hasIdentifier = std::ranges::any_of(molds.span(), [](auto& mold) {
            return !!mold.m_identifier;
        });
        Charge mapCharge(budget);
        std::optional<UCBIdentifierIndex> ucbIdentifiers;
        if (hasIdentifier) {
            if (!mapCharge.add(UCBIdentifierIndex::storageBytes(unlinkedCodeBlock)))
                return chargeRefused();
            ucbIdentifiers.emplace(unlinkedCodeBlock);
        }
        for (auto& mold : molds) {
            if (!mold.m_identifier) {
                moldIdentifiers.append({ MoldIdentifierKind::None, 0 });
                continue;
            }
            auto* impl = mold.m_identifier.uid();
            if (auto index = ucbIdentifiers->indexOf(impl)) {
                moldIdentifiers.append({ MoldIdentifierKind::UCBIdentifier, *index });
                continue;
            }
            if (auto name = immortalNameFor(vm, impl)) {
                moldIdentifiers.append({ MoldIdentifierKind::ImmortalName, static_cast<uint32_t>(*name) });
                continue;
            }
            return notEligible(Unrecordable::UnknownIdentifier);
        }
    }

    // Step 4: a MathIC with inline code is written from its live IC, whose snippet must be the one the record's provenance
    // describes: the same start, and a linked size within the handle's. A MathIC the record lists without inline code is
    // written from the record alone.
    for (auto& mathIC : record.mathICs()) {
        if (!mathIC.slowCallPointerSite)
            continue;
        auto liveCode = mathICCode(mathIC.kind, mathIC.mathIC);
        bool provenanceMatches = mathIC.snippet
            ? liveCode.snippetStart == mathIC.snippet->start && mathIC.snippet->size <= liveCode.snippetHandleSize
            : !liveCode.snippetStart;
        if (!provenanceMatches)
            return notEligible(Unrecordable::MathICProvenance);
    }

    // Step 5, with the header's counts.
    ImageSectionHeader header {
        .codeSize = codeSize,
        .arityEntryOffset = arityEntryOffset.value_or(0),
        .fixupCount = static_cast<uint32_t>(record.fixups().size()),
        .callCount = static_cast<uint32_t>(code.m_unlinkedCalls.size()),
        .moldCount = static_cast<uint32_t>(molds.size()),
        .simpleSwitchTableCount = static_cast<uint32_t>(code.m_switchJumpTables.size()),
        .stringSwitchTableCount = static_cast<uint32_t>(code.m_stringSwitchJumpTables.size()),
        .codeMapCount = code.m_jitCodeMap.size(),
        .constantPoolCount = static_cast<uint32_t>(code.m_constantPool.size()),
        .mathICCount = static_cast<uint32_t>(record.mathICs().size()),
        .livenessRateBits = std::bit_cast<uint64_t>(code.livenessRate()),
        .fullnessRateBits = std::bit_cast<uint64_t>(code.fullnessRate()),
    };

    // Step 7. Each check guards a capture, so a failure is a recording fault (THREAD Session). With strict off, capture
    // trusts that JIT::link and the MathIC hooks left what the record says, and debug builds assert the checks that need
    // no allocation, so the budget sees the same charges in every build.
    if (strict) {
        if (auto failure = firstStrictFailure(vm, unlinkedCodeBlock, code, record, budget))
            return std::unexpected(*failure);
    } else {
        ASSERT(holderMatchesRecord(code, record));
        ASSERT(pointersLieInImage(code, record));
    }

    return ImageCapture(code, budget, keptCharge.take(), header, WTF::move(moldIdentifiers));
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
