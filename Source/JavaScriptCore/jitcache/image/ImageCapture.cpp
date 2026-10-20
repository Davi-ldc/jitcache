#include "config.h"
#include "ImageCapture.h"

#if ENABLE(JIT)

#include "BakedFacts.h"
#include "BaselineJITCode.h"
#include "CacheableIdentifierInlines.h"
#include "CodeBlock.h"
#include "ImageFootprint.h"
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

#if ENABLE(JITCACHE_TWINS)
#include "ImageTwins.h"
#include "ImageTwinsSection.h"
#include "JITThunks.h"
#endif

// Besides the declarations of its header, the index that turns a mold's identifier into the UCB's, the capturing VM's
// resolution context, strict's capture checks (ImageCheck::CodeFixupTargets to SideTablePointers), and in twins builds
// the producer values and the target the ChangeRecordedTarget hook substitutes.

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

    // False once the budget refuses, which raises the recording fault itself.
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

// A MathIC has a snippet when the record holds the provenance of the one its m_code holds, which captureImage matched;
// only a MathIC with inline code regenerates.
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

// The map from the impl of each UCB identifier to its lowest index, which classifies each mold's identifier: a vector
// sorted by impl, whose exact bytes are charged before it is allocated. The lowest index makes the choice the same in
// every process whose UCB has the producer's identifiers, so a capture of an imported image writes the identifier
// fields it imported.
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

// The capturing VM's resolution context, for the producer values and the strict checks: the image start, the record's
// MathICs, the dense switch tables' storage, the string tables JIT::link filled, the snippets' starts and the UCB's
// string switch ranks. storageBytes is what its arrays take, which the caller charges before constructing it.
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

// ImageCheck::MathICCount: the holder owns as many ICs of each kind as the record lists.
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

// ImageCheck::SideTablePointers: every code pointer of the side tables, and the four locations of every MathIC with
// inline code, lie inside [start, start + codeSize]; every MathIC the record lists without inline code still has null
// locations, no m_code and its regeneration flag clear.
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
        auto liveCode = mathICCodeState(mathIC.kind, mathIC.mathIC);
        if (!locations.areAllNull() || liveCode.snippetStart || liveCode.generateFastPathOnRepatch)
            return false;
    }
    return true;
}

// The capture checks, in order, the first two against the producer's resolution of every target: each fixup of the
// image, then of each snippet, decodes as its form, jump islands followed, to it.
static std::optional<ImageCheck> firstStrictFailure(BaselineJITCode& code, const ImageRecord& record, const ResolutionContext& context)
{
    if (!fixupsReachTargets(code.start(), record.codeSize(), record.fixups(), context))
        return ImageCheck::CodeFixupTargets;
    for (auto& mathIC : record.mathICs()) {
        if (!hasSnippet(mathIC))
            continue;
        auto& snippet = *mathIC.snippet;
        if (!fixupsReachTargets(snippet.start, snippet.size, snippet.fixups, context))
            return ImageCheck::SnippetFixupTargets;
    }
    if (!holderMatchesRecord(code, record))
        return ImageCheck::MathICCount;
    if (!pointersLieInImage(code, record))
        return ImageCheck::SideTablePointers;
    return std::nullopt;
}

#if ENABLE(JITCACHE_TWINS)
// The producer values: what every fixup's target resolves to in the capturing VM, the image's in footprint order and
// then each snippet's in MathIC index order, as the twins section lists them.
static size_t producerValueCount(const ImageRecord& record)
{
    size_t count = record.fixups().size();
    for (auto& mathIC : record.mathICs()) {
        if (hasSnippet(mathIC))
            count += mathIC.snippet->fixups.size();
    }
    return count;
}

static void appendProducerValues(Vector<uint64_t>& values, const ImageRecord& record, const ResolutionContext& context)
{
    auto append = [&](const Vector<ImageFixup>& fixups) {
        for (auto& fixup : fixups)
            values.append(reinterpret_cast<uintptr_t>(resolveTarget(context, fixup.target)));
    };
    append(record.fixups());
    for (auto& mathIC : record.mathICs()) {
        if (hasSnippet(mathIC))
            append(mathIC.snippet->fixups);
    }
}

// The ChangeRecordedTarget test hook: another valid target for the fixup's form, which resolves elsewhere, so that
// ImageCheck::CodeFixupTargets finds the footprint does not reach it. A Pointer names another VM field; a Call or a Jump
// the thunk every baseline compilation links for a stack overflow at its prologue, or the exception handler where it
// already names that one.
static ImageTarget changedTargetForTesting(const ImageFixup& fixup)
{
    if (fixup.form == FixupForm::Pointer) {
        bool namesVM = fixup.target.kind == TargetKind::VMAddress && fixup.target.a == static_cast<uint32_t>(VMAddress::VM);
        auto field = namesVM ? VMAddress::SoftStackLimit : VMAddress::VM;
        return ImageTarget { .kind = TargetKind::VMAddress, .a = static_cast<uint32_t>(field), .b = 0, .payload = 0 };
    }
    auto thunk = CommonJITThunkID::ThrowStackOverflowAtPrologue;
    if (fixup.target.kind == TargetKind::CommonThunk && fixup.target.a == static_cast<uint32_t>(thunk))
        thunk = CommonJITThunkID::HandleException;
    return ImageTarget { .kind = TargetKind::CommonThunk, .a = static_cast<uint32_t>(thunk), .b = 0, .payload = 0 };
}
#endif

} // namespace ImageCaptureInternal

bool isImageCapturable(const BaselineJITCode& code)
{
    auto* record = code.m_jitCacheImageRecord.get();
    return record && record->state() == RecordState::Complete;
}

#if ENABLE(JITCACHE_TWINS)
ImageCapture::ImageCapture(BaselineJITCode& code, ProducerBudget& budget, size_t chargedBytes, const ImageSectionHeader& header, Vector<MoldIdentifier>&& moldIdentifiers, Vector<uint64_t>&& producerValues)
#else
ImageCapture::ImageCapture(BaselineJITCode& code, ProducerBudget& budget, size_t chargedBytes, const ImageSectionHeader& header, Vector<MoldIdentifier>&& moldIdentifiers)
#endif
    : m_code(code)
    , m_budget(budget)
    , m_chargedBytes(chargedBytes)
    , m_imageStart(code.start())
    , m_header(header)
    , m_moldIdentifiers(WTF::move(moldIdentifiers))
#if ENABLE(JITCACHE_TWINS)
    , m_producerValues(WTF::move(producerValues))
#endif
{
#if ENABLE(JITCACHE_TWINS)
    ASSERT(m_chargedBytes == m_moldIdentifiers.capacity() * sizeof(MoldIdentifier) + m_producerValues.capacity() * sizeof(uint64_t));
#else
    ASSERT(m_chargedBytes == m_moldIdentifiers.capacity() * sizeof(MoldIdentifier));
#endif
}

ImageCapture::ImageCapture(ImageCapture&& other)
    : m_code(WTF::move(other.m_code))
    , m_budget(WTF::move(other.m_budget))
    , m_chargedBytes(std::exchange(other.m_chargedBytes, 0))
    , m_imageStart(other.m_imageStart)
    , m_header(other.m_header)
    , m_moldIdentifiers(WTF::move(other.m_moldIdentifiers))
#if ENABLE(JITCACHE_TWINS)
    , m_producerValues(WTF::move(other.m_producerValues))
#endif
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
        // Written from the record alone: exactly the no-inline-code flag, every later field zero.
        entry.flags = ImageMathICFlags::noInlineCode;
        return entry;
    }
    auto locations = mathICLocations(mathIC.kind, mathIC.mathIC);
    entry.inlineStart = imageOffsetOf(locations.inlineStart);
    entry.inlineEnd = imageOffsetOf(locations.inlineEnd);
    entry.slowPathStart = imageOffsetOf(locations.slowPathStart);
    entry.slowPathCall = imageOffsetOf(locations.slowPathCall);
    if (mathICCodeState(mathIC.kind, mathIC.mathIC).generateFastPathOnRepatch)
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

    // The code: the live bytes up to the linked size, with every footprint in its canonical encoding.
    if (!writer.writeCanonicalCode(bytesAt(m_imageStart, m_header.codeSize), record.fixups().span()) || !writer.pad())
        return false;

    // The fixups.
    for (auto& fixup : record.fixups()) {
        if (!writer.write(encodeImageFixup(fixup)))
            return false;
    }

    // The calls, in the order JIT::link sorted them.
    for (auto& call : code.m_unlinkedCalls) {
        ImageCallEntry entry {
            .bytecodeIndex = call.bytecodeIndex,
            .doneLocation = imageOffsetOf(call.doneLocation.untaggedPtr()),
        };
        if (!writer.write(encodeImageCall(entry)))
            return false;
    }

    // The molds, in mold order, each identifier field as captureImage classified it.
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

    // The simple switch tables: m_ctiDefault, which JIT::link sets for every table, list tables included, then a dense
    // table's offsets.
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

    // The string switch tables: each table's offsets by m_indexInTable, the default last, as JIT::link filled them.
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

    // The code map: one offset per instruction start, in bytecode order; install pairs them with the UCB's starts again.
    bool wroteCodeMap = true;
    code.m_jitCodeMap.forEach([&](BytecodeIndex, CodeLocationLabel<JSEntryPtrTag> location) {
        wroteCodeMap = wroteCodeMap && writeOffset(location.untaggedPtr());
    });
    if (!wroteCodeMap || !writer.pad())
        return false;

    // The constant pool.
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

    // The MathICs, by MathIC index.
    for (auto& mathIC : record.mathICs()) {
        if (!writer.write(encodeImageMathIC(mathICEntryOf(mathIC))))
            return false;
    }

    // The snippets: each snippet's live bytes up to its linked size, canonical like the image's, then its fixups.
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

#if ENABLE(JITCACHE_TWINS)
size_t ImageCapture::twinsSectionSize() const
{
    return static_cast<size_t>(JITCache::twinsSectionSize(m_code->m_jitCacheImageRecord->twinData(), m_producerValues.size()));
}

bool ImageCapture::writeTwinsSection(const ImageSectionSink& sink) const
{
    // The record's twin data, which a later regeneration could extend only while JS runs, the producer values and the
    // token of this process, which captured the body.
    return JITCache::writeTwinsSection(m_code->m_jitCacheImageRecord->twinData(), m_producerValues.span(), captureProcessToken(), sink);
}
#endif

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

    // The image runs from the normal entry to the record's linked size; the code reference's own size is the handle's,
    // which can extend past the linked bytes. Without an arity-check entry, the arity entry is the normal entry, offset 0.
    const void* imageStart = code.start();
    uint32_t codeSize = record.codeSize();
    auto arityEntryOffset = offsetInCode(imageStart, codeSize, code.addressForCall(ArityCheckMode::MustCheckArity).untaggedPtr());
    ASSERT(arityEntryOffset && *arityEntryOffset < codeSize);

    // The writes convert the side tables' code pointers to offsets as they stream them (imageOffsetOf);
    // ImageCheck::SideTablePointers checks them under strict.

    // Each mold's identifier field, kept for the writes and charged for as long as the capture lives.
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

    // A MathIC with inline code is written from its live IC, whose snippet must be the one the record's provenance
    // describes: the same start, and a linked size within the handle's. A MathIC the record lists without inline code is
    // written from the record alone.
    for (auto& mathIC : record.mathICs()) {
        if (!mathIC.slowCallPointerSite)
            continue;
        auto liveCode = mathICCodeState(mathIC.kind, mathIC.mathIC);
        bool provenanceMatches = mathIC.snippet
            ? liveCode.snippetStart == mathIC.snippet->start && mathIC.snippet->size <= liveCode.snippetHandleSize
            : !liveCode.snippetStart;
        if (!provenanceMatches)
            return notEligible(Unrecordable::MathICProvenance);
    }

    // The section header, with the regions' counts.
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

    // Strict's fixup checks and, in twins builds, the producer values resolve targets in this VM's context,
    // whose arrays are charged while it lives. A support lookup takes JITThunks::m_lock, and every support target
    // already exists, since the compilation generated it.
#if ENABLE(JITCACHE_TWINS)
    constexpr bool resolvesInTwinsBuilds = true;
#else
    constexpr bool resolvesInTwinsBuilds = false;
#endif
    Charge resolutionCharge(budget);
    std::optional<ProducerResolution> resolution;
    if (strict || resolvesInTwinsBuilds) {
        if (!resolutionCharge.add(ProducerResolution::storageBytes(unlinkedCodeBlock, code, record)))
            return chargeRefused();
        resolution.emplace(vm, unlinkedCodeBlock, code, record);
    }

#if ENABLE(JITCACHE_TWINS)
    // The producer values the twins section carries for the twin check's relocation comparison, kept for the writes and
    // charged for as long as the capture lives.
    size_t valueCount = producerValueCount(record);
    if (!keptCharge.add(valueCount * sizeof(uint64_t)))
        return chargeRefused();
    Vector<uint64_t> producerValues;
    producerValues.reserveInitialCapacity(valueCount);
    appendProducerValues(producerValues, record, resolution->context());

    // The ChangeRecordedTarget test hook changes the record's first fixup before ImageCheck::CodeFixupTargets runs.
    if (imageTestHook() == ImageTestHook::ChangeRecordedTarget && !record.fixups().isEmpty()) [[unlikely]]
        record.setFixupTargetForTesting(0, changedTargetForTesting(record.fixups()[0]));
#endif

    // The strict checks. Each guards a capture, so a failure is a recording fault. With strict off, capture
    // trusts that JIT::link and the MathIC hooks left what the record says, and debug builds assert the checks that
    // need no allocation, so debug and release builds of one kind see the same charges.
    if (strict) {
        if (auto check = firstStrictFailure(code, record, resolution->context()))
            return std::unexpected(CaptureFailure { CaptureOutcome::RecordingFault, *check });
    } else {
        ASSERT(holderMatchesRecord(code, record));
        ASSERT(pointersLieInImage(code, record));
    }

#if ENABLE(JITCACHE_TWINS)
    return ImageCapture(code, budget, keptCharge.take(), header, WTF::move(moldIdentifiers), WTF::move(producerValues));
#else
    return ImageCapture(code, budget, keptCharge.take(), header, WTF::move(moldIdentifiers));
#endif
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
