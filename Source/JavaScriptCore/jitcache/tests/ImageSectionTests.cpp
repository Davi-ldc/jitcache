#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BakedFacts.h"
#include "BytecodeStructs.h"
#include "CPU.h"
#include "CallMode.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "GetByIdMetadata.h"
#include "GetPutInfo.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "ImageTwins.h"
#include "InlineCacheHandler.h"
#include "JITCacheTest.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "PropertyInlineCache.h"
#include "RegExp.h"
#include "SourceCode.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

// T6 (SPEC-image.md section 16.1): a valid image.baseline, baked-facts.baseline and image-twins.baseline, built for the
// build's architecture against a live UCB, parse to the same view with strict on and off, and each mutation that
// violates exactly one of V1 to V7, U1 to U7 or W1 to W4 is rejected with that check; W4's cases include T18's
// strict-equality inputs. Every section is parsed from an allocation of exactly its size, so ASan reports any read past
// its end. The footprint encodings of section 3.2 are checked on their own first.

namespace JSC::JITCache::Tests {

namespace ImageSectionTestsInternal {

static constexpr size_t fillUnit = isARM64() ? 4 : 1;

static ImageTarget target(TargetKind kind, uint32_t a = 0, uint32_t b = 0, int64_t payload = 0)
{
    return ImageTarget { .kind = kind, .a = a, .b = b, .payload = payload };
}

static int64_t anchorSymbol()
{
    return CodeSymbol::of(&codeSymbolAnchor)->offset;
}

// Code whose references sit in their forms' fixed footprints with live, nonzero variable fields, as linked code holds
// them before capture writes the canonical encoding.
class TestCode {
public:
    uint32_t offset() const { return static_cast<uint32_t>(m_bytes.size()); }
    std::span<const uint8_t> bytes() const { return m_bytes.span(); }
    const Vector<ImageFixup>& fixups() const { return m_fixups; }

    void fill(size_t count)
    {
#if CPU(ARM64)
        RELEASE_ASSERT(!(count % 4));
        for (size_t index = 0; index < count; index += 4)
            appendLittleEndian<uint32_t>(0xD503201F); // nop
#else
        for (size_t index = 0; index < count; ++index)
            m_bytes.append(0xCC); // int3
#endif
    }

    // The indirect call after a far call's pointer move: call r11 (REPATCH_OFFSET_CALL_R11 bytes) or blr x16.
    void farCallInstruction()
    {
#if CPU(ARM64)
        appendLittleEndian<uint32_t>(0xD63F0200);
#else
        m_bytes.append(0x41);
        m_bytes.append(0xFF);
        m_bytes.append(0xD3);
#endif
    }

    uint32_t pointer(const ImageTarget& target, unsigned reg = 1)
    {
#if CPU(ARM64)
        uint32_t site = offset();
        appendLittleEndian<uint32_t>(0xD2800000 | (0x1234u << 5) | reg);
        appendLittleEndian<uint32_t>(0xF2A00000 | (0x5678u << 5) | reg);
        appendLittleEndian<uint32_t>(0xF2C00000 | (0x9ABCu << 5) | reg);
#else
        m_bytes.append(static_cast<uint8_t>(reg >= 8 ? 0x49 : 0x48));
        m_bytes.append(static_cast<uint8_t>(0xB8 + (reg & 7)));
        appendLittleEndian<uint64_t>(0x1122334455667788);
        uint32_t site = offset();
#endif
        return record(site, FixupForm::Pointer, target);
    }

    uint32_t call(const ImageTarget& target)
    {
#if CPU(ARM64)
        appendLittleEndian<uint32_t>(0x94000000 | 0x40);
#else
        m_bytes.append(0xE8);
        appendLittleEndian<uint32_t>(0x01020304);
#endif
        return record(offset(), FixupForm::Call, target);
    }

    uint32_t jump(const ImageTarget& target, bool conditional = false)
    {
#if CPU(ARM64)
        UNUSED_PARAM(conditional);
        uint32_t site = offset();
        appendLittleEndian<uint32_t>(0x14000000 | 0x80);
#else
        if (conditional) {
            m_bytes.append(0x0F);
            m_bytes.append(0x84);
        } else
            m_bytes.append(0xE9);
        appendLittleEndian<uint32_t>(0x05060708);
        uint32_t site = offset();
#endif
        return record(site, FixupForm::Jump, target);
    }

private:
    template<typename T>
    void appendLittleEndian(T value)
    {
        m_bytes.append(asByteSpan(value));
    }

    uint32_t record(uint32_t site, FixupForm form, const ImageTarget& target)
    {
        m_fixups.append(ImageFixup { .site = site, .form = form, .target = target });
        return site;
    }

    Vector<uint8_t> m_bytes;
    Vector<ImageFixup> m_fixups;
};

// Collects what a writer streams, as the integrator's writer buffers it.
template<typename WriteFunction>
static Vector<uint8_t> collect(const WriteFunction& write)
{
    Vector<uint8_t> bytes;
    auto append = [&](std::span<const uint8_t> data) {
        bytes.append(data);
        return true;
    };
    ImageSectionSink sink = append;
    bool written = write(sink);
    RELEASE_ASSERT(written);
    return bytes;
}

static Vector<uint8_t> canonicalCode(const TestCode& code)
{
    return collect([&](const ImageSectionSink& sink) {
        ImageSectionWriter writer(sink);
        return writer.writeCanonicalCode(code.bytes(), code.fixups().span());
    });
}

struct TestSnippet {
    Vector<uint8_t> code; // canonical
    Vector<ImageFixup> fixups;
};

struct TestSimpleTable {
    uint32_t defaultOffset { 0 };
    Vector<uint32_t> offsets;
};

struct TestMathIC {
    ImageMathICEntry entry;
    std::optional<TestSnippet> snippet;
};

// The image-twins.baseline a twins build always reads beside the other two (section 11.2). Its producer values are one
// per fixup of the model, the image's and then each snippet's, as W3 counts them, plus extraProducerValues.
struct TestTwins {
    TestTwins()
    {
        data.seeds.assembler = 0x5eed;
        data.compileInputs.compiledHoldingAPILock = true;
        token[0] = 0x5a;
    }

    TwinData data;
    std::array<uint8_t, captureProcessTokenSize> token { };
    unsigned extraProducerValues { 0 };
};

// A model of the three sections, encoded field by field so that a mutation changes exactly what it names.
struct TestSection {
    uint32_t arityEntryOffset { 0 };
    double livenessRate { 0.5 };
    double fullnessRate { 0.25 };
    Vector<uint8_t> code; // canonical
    Vector<ImageFixup> fixups;
    Vector<ImageCallEntry> calls;
    Vector<ImageMoldEntry> molds;
    Vector<TestSimpleTable> simpleTables;
    Vector<Vector<uint32_t>> stringTables;
    Vector<uint32_t> codeMap;
    Vector<ImageConstantPoolEntry> constantPool;
    Vector<TestMathIC> mathICs;
    BakedFacts bakedFacts;
    TestTwins twins;
};

static ImageSectionHeader headerOf(const TestSection& section)
{
    return ImageSectionHeader {
        .codeSize = static_cast<uint32_t>(section.code.size()),
        .arityEntryOffset = section.arityEntryOffset,
        .fixupCount = static_cast<uint32_t>(section.fixups.size()),
        .callCount = static_cast<uint32_t>(section.calls.size()),
        .moldCount = static_cast<uint32_t>(section.molds.size()),
        .simpleSwitchTableCount = static_cast<uint32_t>(section.simpleTables.size()),
        .stringSwitchTableCount = static_cast<uint32_t>(section.stringTables.size()),
        .codeMapCount = static_cast<uint32_t>(section.codeMap.size()),
        .constantPoolCount = static_cast<uint32_t>(section.constantPool.size()),
        .mathICCount = static_cast<uint32_t>(section.mathICs.size()),
        .livenessRateBits = std::bit_cast<uint64_t>(section.livenessRate),
        .fullnessRateBits = std::bit_cast<uint64_t>(section.fullnessRate),
    };
}

// The section in the order of section 8.2. boundaries, when given, receives the offset after every write, the region
// boundaries among them.
static Vector<uint8_t> encodeImage(const TestSection& section, Vector<size_t>* boundaries = nullptr)
{
    return collect([&](const ImageSectionSink& sink) {
        ImageSectionWriter writer(sink);
        auto mark = [&] {
            if (boundaries)
                boundaries->append(static_cast<size_t>(writer.offset()));
            return true;
        };
        auto put = [&](std::span<const uint8_t> bytes) {
            return writer.write(bytes) && mark();
        };
        auto pad = [&] {
            return writer.pad() && mark();
        };

        bool written = put(encodeImageSectionHeader(headerOf(section))) && put(section.code.span()) && pad();
        for (auto& fixup : section.fixups)
            written = written && put(encodeImageFixup(fixup));
        for (auto& call : section.calls)
            written = written && put(encodeImageCall(call));
        for (auto& mold : section.molds)
            written = written && put(encodeImageMold(mold));
        for (auto& table : section.simpleTables) {
            written = written && put(encodeImageOffset(table.defaultOffset)) && put(encodeImageOffset(table.offsets.size()));
            for (uint32_t offset : table.offsets)
                written = written && put(encodeImageOffset(offset));
        }
        written = written && pad();
        for (auto& table : section.stringTables) {
            written = written && put(encodeImageOffset(table.size()));
            for (uint32_t offset : table)
                written = written && put(encodeImageOffset(offset));
        }
        written = written && pad();
        for (uint32_t offset : section.codeMap)
            written = written && put(encodeImageOffset(offset));
        written = written && pad();
        for (auto& constant : section.constantPool)
            written = written && put(encodeImageConstantPoolEntry(constant));
        for (auto& mathIC : section.mathICs)
            written = written && put(encodeImageMathIC(mathIC.entry));
        for (auto& mathIC : section.mathICs) {
            if (!mathIC.entry.hasSnippet() || !mathIC.snippet)
                continue;
            written = written && put(mathIC.snippet->code.span()) && pad();
            for (auto& fixup : mathIC.snippet->fixups)
                written = written && put(encodeImageFixup(fixup));
        }
        return written;
    });
}

static Vector<uint8_t> encodeBakedFacts(const BakedFacts& facts)
{
    return collect([&](const ImageSectionSink& sink) {
        return writeBakedFactsSection(facts, sink);
    });
}

// One producer value per fixup of the model, in region 6's order, each a distinct made-up address.
static Vector<uint64_t> producerValuesOf(const TestSection& section)
{
    size_t count = section.fixups.size() + section.twins.extraProducerValues;
    for (auto& mathIC : section.mathICs) {
        if (mathIC.entry.hasSnippet() && mathIC.snippet)
            count += mathIC.snippet->fixups.size();
    }
    return Vector<uint64_t>(count, [](size_t index) {
        return static_cast<uint64_t>(0x7f0000001000 + 8 * index);
    });
}

static Vector<uint8_t> encodeTwins(const TestSection& section)
{
    auto producerValues = producerValuesOf(section);
    return collect([&](const ImageSectionSink& sink) {
        return writeTwinsSection(section.twins.data, producerValues.span(), section.twins.token, sink);
    });
}

// Region offsets of the twins section, computed apart from the lane's own arithmetic, for mutations of its bytes.
struct TwinsLayout {
    size_t binarySwitchSeeds { 0 };
    size_t inputs { 0 };
    size_t binaryArithBits { 0 };
    size_t unaryArithBits { 0 };
    Vector<size_t> regenerations;
    size_t producerValues { 0 };
    Vector<size_t> boundaries; // every region's start and the section's end
};

static TwinsLayout twinsLayoutOf(const TestSection& section)
{
    auto& data = section.twins.data;
    TwinsLayout layout;
    size_t offset = imageTwinsHeaderSize;
    auto region = [&](size_t& start, size_t bytes) {
        start = offset;
        layout.boundaries.append(offset);
        offset += alignImageSectionOffset(bytes);
    };
    region(layout.binarySwitchSeeds, 4 * data.seeds.binarySwitches.size());
    region(layout.inputs, 12 * data.compileInputs.inputs.size());
    region(layout.binaryArithBits, 2 * data.compileInputs.binaryArithBits.size());
    region(layout.unaryArithBits, 2 * data.compileInputs.unaryArithBits.size());
    layout.boundaries.append(offset);
    for (auto& regeneration : data.regenerations) {
        layout.regenerations.append(offset);
        offset += alignImageSectionOffset(16 + 4 * regeneration.assemblerSeeds.size());
    }
    region(layout.producerValues, 8 * producerValuesOf(section).size());
    layout.boundaries.append(offset);
    return layout;
}

static uint64_t sectionSize(const TestSection& section)
{
    ImageSectionSize size(headerOf(section));
    for (auto& table : section.simpleTables)
        size.addSimpleSwitchTable(table.offsets.size());
    for (auto& table : section.stringTables)
        size.addStringSwitchTable(table.size());
    for (auto& mathIC : section.mathICs) {
        if (mathIC.entry.hasSnippet() && mathIC.snippet)
            size.addSnippet(mathIC.snippet->code.size(), mathIC.snippet->fixups.size());
    }
    return size.bytes();
}

// Region offsets computed apart from the lane's own arithmetic, for mutations of the encoded bytes.
static size_t codeOffset()
{
    return imageSectionHeaderSize;
}

static size_t fixupOffset(const TestSection& section, size_t fixupIndex)
{
    return codeOffset() + alignImageSectionOffset(section.code.size()) + fixupIndex * imageFixupEntrySize;
}

static size_t mathICOffset(const TestSection& section, size_t mathICIndex)
{
    size_t simpleBytes = 0;
    for (auto& table : section.simpleTables)
        simpleBytes += 8 + 4 * table.offsets.size();
    size_t stringBytes = 0;
    for (auto& table : section.stringTables)
        stringBytes += 4 + 4 * table.size();
    return fixupOffset(section, section.fixups.size())
        + section.calls.size() * imageCallEntrySize
        + section.molds.size() * imageMoldEntrySize
        + alignImageSectionOffset(simpleBytes)
        + alignImageSectionOffset(stringBytes)
        + alignImageSectionOffset(4 * section.codeMap.size())
        + section.constantPool.size() * imageConstantPoolEntrySize
        + mathICIndex * imageMathICEntrySize;
}

// Copies of the sections in allocations of exactly their sizes.
struct ExactSections {
    ExactSections(std::span<const uint8_t> imageBytes, std::span<const uint8_t> bakedFactsBytes, std::span<const uint8_t> twinsBytes)
        : image(imageBytes)
        , bakedFacts(bakedFactsBytes)
        , twins(twinsBytes)
    {
    }

    explicit ExactSections(const TestSection& section)
        : ExactSections(encodeImage(section).span(), encodeBakedFacts(section.bakedFacts).span(), encodeTwins(section).span())
    {
    }

    ImageSectionSpans spans() const
    {
        ImageSectionSpans spans;
        spans.image = image.span();
        spans.bakedFacts = bakedFacts.span();
        spans.twins = twins.span();
        return spans;
    }

    Vector<uint8_t> image;
    Vector<uint8_t> bakedFacts;
    Vector<uint8_t> twins;
};

template<typename Mutation>
static TestSection mutated(const TestSection& section, const Mutation& mutation)
{
    TestSection copy = section;
    mutation(copy);
    return copy;
}

static size_t fixupIndex(const Vector<ImageFixup>& fixups, TargetKind kind, std::optional<FixupForm> form = std::nullopt, std::optional<uint32_t> a = std::nullopt, std::optional<uint32_t> b = std::nullopt)
{
    for (size_t index = 0; index < fixups.size(); ++index) {
        auto& fixup = fixups[index];
        if (fixup.target.kind == kind && (!form || fixup.form == *form) && (!a || fixup.target.a == *a) && (!b || fixup.target.b == *b))
            return index;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static size_t footprintBegin(const TestSection& section, size_t fixupIndex)
{
    auto& fixup = section.fixups[fixupIndex];
    return fixupFootprint(fixup.form, fixup.site, section.code.span())->begin;
}

static void expectStructureFailure(TestContext& context, ASCIILiteral label, const ExactSections& sections, ImageCheck expected)
{
    auto result = parseImageSections(sections.spans(), true);
    if (result) {
        JITCACHE_FAIL(makeString(label, ": parsed under strict, expected "_s, description(expected)));
        return;
    }
    if (result.error() != expected)
        JITCACHE_FAIL(makeString(label, ": rejected with "_s, description(result.error()), ", expected "_s, description(expected)));
}

static void expectModelFailure(TestContext& context, ASCIILiteral label, const TestSection& section, ImageCheck expected)
{
    expectStructureFailure(context, label, ExactSections(section), expected);
}

// Each mutates one encoded section, which ExactSections then copies to an allocation of exactly its new size.
template<typename Mutation>
static void expectImageBytesFailure(TestContext& context, ASCIILiteral label, const TestSection& section, const Mutation& mutation, ImageCheck expected)
{
    auto image = encodeImage(section);
    mutation(image);
    expectStructureFailure(context, label, ExactSections(image.span(), encodeBakedFacts(section.bakedFacts).span(), encodeTwins(section).span()), expected);
}

template<typename Mutation>
static void expectBakedFactsBytesFailure(TestContext& context, ASCIILiteral label, const TestSection& section, const Mutation& mutation)
{
    auto bakedFacts = encodeBakedFacts(section.bakedFacts);
    mutation(bakedFacts);
    expectStructureFailure(context, label, ExactSections(encodeImage(section).span(), bakedFacts.span(), encodeTwins(section).span()), ImageCheck::V7);
}

template<typename Mutation>
static void expectTwinsBytesFailure(TestContext& context, ASCIILiteral label, const TestSection& section, const Mutation& mutation, ImageCheck expected)
{
    auto twins = encodeTwins(section);
    mutation(twins);
    expectStructureFailure(context, label, ExactSections(encodeImage(section).span(), encodeBakedFacts(section.bakedFacts).span(), twins.span()), expected);
}

static void expectValid(TestContext& context, ASCIILiteral label, const TestSection& section)
{
    ExactSections sections(section);
    auto strict = parseImageSections(sections.spans(), true);
    if (!strict) {
        JITCACHE_FAIL(makeString(label, ": a valid section failed "_s, description(strict.error())));
        return;
    }
    // With strict off, a valid section parses to the view strict gives it.
    auto normal = parseImageSections(sections.spans(), false);
    JITCACHE_CHECK(normal && *normal == *strict);
}

[[maybe_unused]] static uint32_t wordAt(std::span<const uint8_t> bytes, size_t offset)
{
    return ImageBytes::read<uint32_t>(bytes, offset);
}

[[maybe_unused]] static void setWord(Vector<uint8_t>& bytes, size_t offset, uint32_t word)
{
    ImageBytes::write<uint32_t>(bytes.mutableSpan(), offset, word);
}

static void setU32(Vector<uint8_t>& bytes, size_t offset, uint32_t value)
{
    ImageBytes::write<uint32_t>(bytes.mutableSpan(), offset, value);
}

// The body the U and W checks run against: a closure variable read and written, the four MathIC kinds, a dense, a list
// and a string switch, a regexp and a string constant, a function declaration and a function expression, and three
// strict equalities: one against an atom constant and one against a number, whose templates reach their atom test, and
// one against null, whose template does not (SPEC-image.md N25).
static constexpr ASCIILiteral bodySource =
    "var jitcacheImageSectionBody = (function () {\n"
    "    var captured = 0;\n"
    "    return function body(a, b, s) {\n"
    "        captured = captured + 1;\n"
    "        var r = /jitcache-section/;\n"
    "        var t = 'jitcache-section-constant';\n"
    "        function inner() { return a; }\n"
    "        var e = function () { return b; };\n"
    "        switch (a) { case 0: a = 10; break; case 1: a = 11; break; case 2: a = 12; break; }\n"
    "        switch (b) { case 1: b = 10; break; case 5000: b = 11; break; case 100000: b = 12; break; }\n"
    "        switch (s) { case 'alpha': s = 1; break; case 'beta': s = 2; break; case 'gamma': s = 3; break; }\n"
    "        return [a + b, a - b, a * b, -a, t.length, r, inner, e, captured, s === 'delta', a === 7, b === null];\n"
    "    };\n"
    "})();\n"
    "jitcacheImageSectionBody(1, 5000, 'beta');\n"_s;

struct Body {
    JSGlobalObject* globalObject { nullptr };
    CodeBlock* codeBlock { nullptr };
    UnlinkedCodeBlock* unlinkedCodeBlock { nullptr };
    Vector<uint32_t> instructionStarts;
    uint32_t addOffset { 0 };
    uint32_t subOffset { 0 };
    uint32_t mulOffset { 0 };
    uint32_t negateOffset { 0 };
    uint32_t resolveScopeOffset { 0 };
    uint32_t getFromScopeOffset { 0 };
    uint32_t putToScopeOffset { 0 };
    uint32_t regExpConstant { 0 };
    uint32_t atomConstant { 0 };
    uint32_t nonCellConstant { 0 };
    uint32_t denseTable { 0 };
    uint32_t listTable { 0 };
    uint32_t inlineStringTable { 0 };
    uint32_t inlineStringKeyCount { 0 };
    // The strict equalities against 'delta', 7 and null.
    uint32_t atomEqualityOffset { 0 };
    uint32_t numberEqualityOffset { 0 };
    uint32_t nullEqualityOffset { 0 };
};

static std::optional<Body> makeBody(TestContext& context, VM& vm)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(bodySource, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the test body threw"_s);
        return std::nullopt;
    }
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "jitcacheImageSectionBody"_s)));
    auto* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock) {
        JITCACHE_FAIL("the test body has no CodeBlock"_s);
        return std::nullopt;
    }

    Body body;
    body.globalObject = globalObject;
    body.codeBlock = codeBlock;
    body.unlinkedCodeBlock = codeBlock->unlinkedCodeBlock();
    auto& unlinkedCodeBlock = *body.unlinkedCodeBlock;

    std::optional<uint32_t> add, sub, mul, negate, resolveScope, getFromScope, putToScope, atomEquality, numberEquality, nullEquality;
    for (const auto& instruction : unlinkedCodeBlock.instructions()) {
        uint32_t offset = instruction.offset();
        body.instructionStarts.append(offset);
        auto remember = [&](std::optional<uint32_t>& slot) {
            if (!slot)
                slot = offset;
        };
        switch (instruction->opcodeID()) {
        case op_add:
            remember(add);
            break;
        case op_sub:
            remember(sub);
            break;
        case op_mul:
            remember(mul);
            break;
        case op_negate:
            remember(negate);
            break;
        case op_resolve_scope:
            remember(resolveScope);
            break;
        case op_get_from_scope:
            remember(getFromScope);
            break;
        case op_put_to_scope:
            remember(putToScope);
            break;
        case op_stricteq: {
            auto bytecode = instruction->as<OpStricteq>();
            if (!bytecode.m_rhs.isConstant())
                break;
            JSValue value = unlinkedCodeBlock.getConstant(bytecode.m_rhs);
            if (value.isString())
                remember(atomEquality);
            else if (value.isInt32() && value.asInt32() == 7)
                remember(numberEquality);
            else if (value.isNull())
                remember(nullEquality);
            break;
        }
        default:
            break;
        }
    }

    std::optional<uint32_t> regExp, atom, nonCell;
    auto& constants = unlinkedCodeBlock.constantRegisters();
    for (uint32_t index = 0; index < constants.size(); ++index) {
        JSValue value = constants[index].get();
        bool isOther = unlinkedCodeBlock.constantSourceCodeRepresentation(index) == SourceCodeRepresentation::Other;
        if (!value.isCell()) {
            if (!nonCell)
                nonCell = index;
            continue;
        }
        if (isOther && value.asCell()->inherits<RegExp>() && !regExp)
            regExp = index;
        if (isOther && value.isString() && !atom) {
            auto* impl = asString(value)->tryGetValueImpl();
            if (impl && impl->isAtom())
                atom = index;
        }
    }

    std::optional<uint32_t> denseTable, listTable, inlineStringTable;
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedSwitchJumpTables(); ++index) {
        auto& table = unlinkedCodeBlock.unlinkedSwitchJumpTable(index);
        auto& slot = table.isList() ? listTable : denseTable;
        if (!slot)
            slot = index;
    }
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables(); ++index) {
        if (!inlineStringTable && hasInlineStringSwitch(unlinkedCodeBlock.unlinkedStringSwitchJumpTable(index).m_offsetTable.size()))
            inlineStringTable = index;
    }

    if (!add || !sub || !mul || !negate || !resolveScope || !getFromScope || !putToScope || !regExp || !atom || !nonCell
        || !atomEquality || !numberEquality || !nullEquality
        || !denseTable || !listTable || !inlineStringTable || body.instructionStarts.size() < 4
        || !unlinkedCodeBlock.numberOfIdentifiers() || !unlinkedCodeBlock.numberOfBinaryArithProfiles() || !unlinkedCodeBlock.numberOfUnaryArithProfiles()
        || unlinkedCodeBlock.functionDecls().empty() || unlinkedCodeBlock.functionExprs().empty()) {
        JITCACHE_FAIL("the test body lacks an instruction, a constant, a table or a profile the section names"_s);
        return std::nullopt;
    }
    body.addOffset = *add;
    body.subOffset = *sub;
    body.mulOffset = *mul;
    body.negateOffset = *negate;
    body.resolveScopeOffset = *resolveScope;
    body.getFromScopeOffset = *getFromScope;
    body.putToScopeOffset = *putToScope;
    body.regExpConstant = *regExp;
    body.atomConstant = *atom;
    body.nonCellConstant = *nonCell;
    body.denseTable = *denseTable;
    body.listTable = *listTable;
    body.inlineStringTable = *inlineStringTable;
    body.inlineStringKeyCount = unlinkedCodeBlock.unlinkedStringSwitchJumpTable(*inlineStringTable).m_offsetTable.size();
    body.atomEqualityOffset = *atomEquality;
    body.numberEqualityOffset = *numberEquality;
    body.nullEqualityOffset = *nullEquality;
    return body;
}

// The left and right operands of a strict equality (stricteq, nstricteq, jstricteq, jnstricteq), or nullopt.
static std::optional<std::pair<VirtualRegister, VirtualRegister>> strictEqualityOperandsOf(const JSInstruction* instruction)
{
    auto operands = [](auto bytecode) {
        return std::pair { bytecode.m_lhs, bytecode.m_rhs };
    };
    switch (instruction->opcodeID()) {
    case op_stricteq:
        return operands(instruction->as<OpStricteq>());
    case op_nstricteq:
        return operands(instruction->as<OpNstricteq>());
    case op_jstricteq:
        return operands(instruction->as<OpJstricteq>());
    case op_jnstricteq:
        return operands(instruction->as<OpJnstricteq>());
    default:
        return std::nullopt;
    }
}

// The twin data a recording compilation of the body would leave, as the producer's JIT reads it: the snapshot of
// section 11.1 from the body's CodeBlock, and a strict-equality input at every strict equality whose template reaches
// its atom test, naming the operand its tryGetAtomStringConstant would choose (N25). Its seeds and its regeneration log
// are the test's own: three switch seeds, so that their region has padding, and two regenerations of MathICs with
// inline code, one with two attaches, the second of which drew nothing, and one with a single attach, whose entry has
// padding.
static TwinData twinDataOf(const Body& body)
{
    auto& unlinkedCodeBlock = *body.unlinkedCodeBlock;
    CodeBlock& codeBlock = *body.codeBlock;
    TwinData data;
    data.seeds.assembler = 0x5eed;
    data.seeds.binarySwitches = Vector<uint32_t> { 0x11, 0x22, 0x33 };
    data.compileInputs = snapshotCompileInputs(codeBlock);

    auto isConstantOperand = [&](VirtualRegister operand) {
        return operand.isConstant() && codeBlock.isConstantOwnedByUnlinkedCodeBlock(operand);
    };
    auto isBitwiseComparable = [&](VirtualRegister operand) {
        if (!isConstantOperand(operand))
            return false;
        JSValue value = unlinkedCodeBlock.getConstant(operand);
        return value.isUndefinedOrNull() || value.isBoolean();
    };
    auto isAtom = [&](VirtualRegister operand) {
        if (!isConstantOperand(operand))
            return false;
        JSValue value = unlinkedCodeBlock.getConstant(operand);
        auto* impl = value.isString() ? asString(value)->tryGetValueImpl() : nullptr;
        return impl && impl->isAtom();
    };
    Vector<CompileInput> strictEqualities;
    for (const auto& instruction : unlinkedCodeBlock.instructions()) {
        auto operands = strictEqualityOperandsOf(instruction.ptr());
        if (!operands || isBitwiseComparable(operands->first) || isBitwiseComparable(operands->second))
            continue;
        auto chosen = isAtom(operands->first) ? StrictEqualityAtomOperand::Lhs : isAtom(operands->second) ? StrictEqualityAtomOperand::Rhs : StrictEqualityAtomOperand::None;
        strictEqualities.append(CompileInput { .bytecodeOffset = instruction.offset(), .kind = CompileInputKind::StrictEqualityAtomOperand, .value = static_cast<uint8_t>(chosen), .localScopeDepth = 0 });
    }
    data.compileInputs.inputs.appendVector(strictEqualities);
    std::sort(data.compileInputs.inputs.begin(), data.compileInputs.inputs.end(), [](const CompileInput& a, const CompileInput& b) {
        return a.bytecodeOffset < b.bytecodeOffset;
    });

    auto anchor = CodeSymbol { anchorSymbol() };
    data.regenerations.append(TwinRegeneration { .mathICIndex = 0, .profileBitsAtEntry = 0x12, .replacement = anchor, .assemblerSeeds = { 0x44u, std::nullopt } });
    data.regenerations.append(TwinRegeneration { .mathICIndex = 1, .profileBitsAtEntry = 0, .replacement = anchor, .assemblerSeeds = { std::nullopt } });
    return data;
}

// Sections that pass V1 to V7 and W1 to W3 and, against the body's UCB, U1 to U7 and W4, with a fixup of every target
// kind, MathICs with and without inline code and a snippet, and padding after its code and its snippet.
static TestSection makeValidSection(const Body& body)
{
    auto& unlinkedCodeBlock = *body.unlinkedCodeBlock;
    int64_t anchor = anchorSymbol();
    auto operation = target(TargetKind::Operation, 0, 0, anchor);

    TestCode code;
    code.fill(8);

    // MathIC 0, add: inline code whose start was rewritten to jump to its snippet.
    uint32_t inlineStart0 = code.offset();
    code.jump(target(TargetKind::SnippetEntry, 0));
    code.fill(8);
    uint32_t inlineEnd0 = code.offset();
    uint32_t slowPathStart0 = code.offset();
    code.pointer(operation);
    code.farCallInstruction();
    uint32_t slowPathCall0 = code.offset();

    // MathICs 1, sub, and 3, negate: inline code without a snippet. MathIC 2, mul, has no inline code.
    auto inlineWithoutSnippet = [&](MathICKind kind, uint32_t bytecodeOffset) {
        ImageMathICEntry entry { .kind = kind, .flags = 0, .bytecodeOffset = bytecodeOffset };
        entry.inlineStart = code.offset();
        code.fill(8);
        entry.inlineEnd = code.offset();
        entry.slowPathStart = code.offset();
        code.pointer(operation, 2);
        code.farCallInstruction();
        entry.slowPathCall = code.offset();
        return entry;
    };
    auto sub = inlineWithoutSnippet(MathICKind::Sub, body.subOffset);
    auto negate = inlineWithoutSnippet(MathICKind::Negate, body.negateOffset);

    // On ARM64 the call and the pointer after it share a site, the call first.
    code.call(target(TargetKind::CommonThunk, 0));
    code.pointer(target(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::VM)), 11);
    code.jump(target(TargetKind::CommonThunk, 1));
#if CPU(X86_64)
    code.jump(target(TargetKind::CommonThunk, 2), true);
#endif
    code.call(target(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::OpEnterHandler)));
    code.jump(target(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::OpThrowHandler)));
    code.call(target(TargetKind::SlowPathThunk, 0, 0, anchor));
    code.call(target(TargetKind::InlineCacheSlowPathThunk, static_cast<uint32_t>(AccessType::GetById)));
    code.call(target(TargetKind::VirtualCallThunk, static_cast<uint32_t>(CallMode::Regular)));
    code.pointer(target(TargetKind::ProcessThunk, static_cast<uint32_t>(ProcessThunk::DefaultCall)));
    code.call(target(TargetKind::ProcessThunk, static_cast<uint32_t>(ProcessThunk::ArityFixup)));
    code.pointer(target(TargetKind::VMCell, static_cast<uint32_t>(VMCell::EmptyString)));
    code.pointer(target(TargetKind::StructureIDBase));
    code.pointer(target(TargetKind::UCBConstantCell, body.regExpConstant));
    code.pointer(target(TargetKind::UCBConstantAtom, body.atomConstant));
    code.pointer(target(TargetKind::UCBIdentifier, 0));
    code.pointer(target(TargetKind::UCBBinaryArithProfile, 0));
    code.pointer(target(TargetKind::UCBUnaryArithProfile, 0));
    for (uint32_t rank = 0; rank < body.inlineStringKeyCount; ++rank) {
        code.pointer(target(TargetKind::SwitchStringRankAtom, body.inlineStringTable, rank));
        code.jump(target(TargetKind::SwitchStringRankCase, body.inlineStringTable, rank));
    }
    code.pointer(target(TargetKind::MathIC, 0));
    code.pointer(target(TargetKind::SwitchTableBase, body.denseTable));
    while (code.offset() % imageSectionAlignment != 4)
        code.fill(fillUnit);

    TestCode snippet;
    snippet.fill(4);
    snippet.jump(target(TargetKind::ImageOffset, inlineEnd0));
    snippet.pointer(target(TargetKind::UCBBinaryArithProfile, 0));
    snippet.jump(target(TargetKind::ImageOffset, slowPathStart0));
    while (snippet.offset() % imageSectionAlignment != 4)
        snippet.fill(fillUnit);

    TestSection section;
    section.arityEntryOffset = 4;
    section.code = canonicalCode(code);
    section.fixups = code.fixups();
    uint32_t codeSize = section.code.size();

    auto& starts = body.instructionStarts;
    section.calls = Vector<ImageCallEntry> {
        ImageCallEntry { .bytecodeIndex = BytecodeIndex(starts[0]), .doneLocation = 4 },
        ImageCallEntry { .bytecodeIndex = BytecodeIndex(starts[1]), .doneLocation = 8 },
    };
    section.molds = Vector<ImageMoldEntry> {
        ImageMoldEntry { .accessType = AccessType::GetById, .preconfiguredCacheType = CacheType::Unset, .flags = 0,
            .identifierKind = MoldIdentifierKind::UCBIdentifier, .identifier = 0, .bytecodeIndex = BytecodeIndex(starts[2]), .doneLocation = 12 },
        ImageMoldEntry { .accessType = AccessType::GetById, .preconfiguredCacheType = CacheType::ArrayLength, .flags = ImageMoldFlags::canBeMegamorphic,
            .identifierKind = MoldIdentifierKind::ImmortalName, .identifier = static_cast<uint32_t>(ImmortalName::Length), .bytecodeIndex = BytecodeIndex(starts[3]), .doneLocation = 16 },
    };
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedSwitchJumpTables(); ++index) {
        auto& unlinkedTable = unlinkedCodeBlock.unlinkedSwitchJumpTable(index);
        TestSimpleTable table;
        table.defaultOffset = 20;
        if (!unlinkedTable.isList())
            table.offsets = Vector<uint32_t>(FillWith { }, unlinkedTable.m_branchOffsets.size(), 24);
        section.simpleTables.append(WTF::move(table));
    }
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables(); ++index)
        section.stringTables.append(Vector<uint32_t>(FillWith { }, unlinkedCodeBlock.unlinkedStringSwitchJumpTable(index).m_offsetTable.size() + 1, 28));
    for (size_t index = 0; index < starts.size(); ++index)
        section.codeMap.append(std::min<uint32_t>(4 * index, codeSize & ~3u));
    section.constantPool = Vector<ImageConstantPoolEntry> {
        ImageConstantPoolEntry { .type = ImageConstantType::FunctionDecl, .index = 0 },
        ImageConstantPoolEntry { .type = ImageConstantType::FunctionExpr, .index = 0 },
    };

    TestMathIC add;
    add.entry = ImageMathICEntry { .kind = MathICKind::Add, .flags = static_cast<uint8_t>(ImageMathICFlags::hasSnippet | ImageMathICFlags::generateFastPathOnRepatch),
        .bytecodeOffset = body.addOffset, .inlineStart = inlineStart0, .inlineEnd = inlineEnd0, .slowPathStart = slowPathStart0, .slowPathCall = slowPathCall0 };
    add.snippet = TestSnippet { canonicalCode(snippet), snippet.fixups() };
    add.entry.snippetSize = add.snippet->code.size();
    add.entry.snippetFixupCount = add.snippet->fixups.size();
    section.mathICs.append(WTF::move(add));
    section.mathICs.append(TestMathIC { sub, std::nullopt });
    section.mathICs.append(TestMathIC { ImageMathICEntry { .kind = MathICKind::Mul, .flags = ImageMathICFlags::noInlineCode, .bytecodeOffset = body.mulOffset }, std::nullopt });
    section.mathICs.append(TestMathIC { negate, std::nullopt });

    section.bakedFacts.capabilityLevel = DFG::CanCompile;
    section.bakedFacts.couldBeTainted = false;
    section.bakedFacts.scopeFacts = Vector<ScopeFact> {
        ScopeFact { .bytecodeOffset = body.resolveScopeOffset, .opcode = ScopeOpcode::ResolveScope, .resolveType = ClosureVar, .localScopeDepth = 1 },
        ScopeFact { .bytecodeOffset = body.getFromScopeOffset, .opcode = ScopeOpcode::GetFromScope, .resolveType = ClosureVar },
        ScopeFact { .bytecodeOffset = body.putToScopeOffset, .opcode = ScopeOpcode::PutToScope, .resolveType = ClosureVar },
    };
    std::sort(section.bakedFacts.scopeFacts.begin(), section.bakedFacts.scopeFacts.end(), [](const ScopeFact& a, const ScopeFact& b) {
        return a.bytecodeOffset < b.bytecodeOffset;
    });
    section.twins.data = twinDataOf(body);
    return section;
}

// The smallest section: code without references, nothing else.
static TestSection minimalSection(size_t codeSize)
{
    TestCode code;
    code.fill(codeSize);
    TestSection section;
    section.code = canonicalCode(code);
    section.bakedFacts.capabilityLevel = DFG::CannotCompile;
    return section;
}

// Everything the view reads back equals the model it was encoded from.
static void checkViewMatches(TestContext& context, const ImageSectionsView& view, const TestSection& section)
{
    JITCACHE_CHECK(view.header() == headerOf(section));
    JITCACHE_CHECK(equalSpans(view.code(), section.code.span()));
    auto fixups = view.fixups();
    JITCACHE_CHECK(fixups.size() == section.fixups.size());
    for (size_t index = 0; index < std::min(fixups.size(), section.fixups.size()); ++index) {
        JITCACHE_CHECK(fixups[index] == section.fixups[index]);
        JITCACHE_CHECK(fixups.find(section.fixups[index].site, section.fixups[index].form) == section.fixups[index]);
    }
    for (unsigned index = 0; index < section.calls.size(); ++index)
        JITCACHE_CHECK(view.call(index) == section.calls[index]);
    for (unsigned index = 0; index < section.molds.size(); ++index)
        JITCACHE_CHECK(view.mold(index) == section.molds[index]);
    unsigned simpleTables = 0;
    view.forEachSimpleSwitchTable([&](unsigned index, const ImageSimpleSwitchTable& table) {
        ++simpleTables;
        auto& expected = section.simpleTables[index];
        JITCACHE_CHECK(table.defaultOffset == expected.defaultOffset && table.offsets.size() == expected.offsets.size());
        for (size_t entry = 0; entry < std::min(table.offsets.size(), expected.offsets.size()); ++entry)
            JITCACHE_CHECK(table.offsets[entry] == expected.offsets[entry]);
    });
    JITCACHE_CHECK(simpleTables == section.simpleTables.size());
    unsigned stringTables = 0;
    view.forEachStringSwitchTable([&](unsigned index, const ImageOffsetArray& offsets) {
        ++stringTables;
        auto& expected = section.stringTables[index];
        JITCACHE_CHECK(offsets.size() == expected.size());
        for (size_t entry = 0; entry < std::min(offsets.size(), expected.size()); ++entry)
            JITCACHE_CHECK(offsets[entry] == expected[entry]);
    });
    JITCACHE_CHECK(stringTables == section.stringTables.size());
    auto codeMap = view.codeMap();
    JITCACHE_CHECK(codeMap.size() == section.codeMap.size());
    for (size_t index = 0; index < std::min(codeMap.size(), section.codeMap.size()); ++index)
        JITCACHE_CHECK(codeMap[index] == section.codeMap[index]);
    for (unsigned index = 0; index < section.constantPool.size(); ++index)
        JITCACHE_CHECK(view.constantPoolEntry(index) == section.constantPool[index]);
    for (unsigned index = 0; index < section.mathICs.size(); ++index)
        JITCACHE_CHECK(view.mathIC(index) == section.mathICs[index].entry);
    unsigned snippets = 0;
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        ++snippets;
        auto& expected = section.mathICs[index].snippet;
        JITCACHE_CHECK(expected && equalSpans(snippet.code, expected->code.span()) && snippet.fixups.size() == expected->fixups.size());
        for (size_t entry = 0; expected && entry < std::min(snippet.fixups.size(), expected->fixups.size()); ++entry)
            JITCACHE_CHECK(snippet.fixups[entry] == expected->fixups[entry]);
    });
    JITCACHE_CHECK(snippets == 1);
    auto& bakedFacts = view.bakedFacts();
    JITCACHE_CHECK(bakedFacts.capabilityLevel() == section.bakedFacts.capabilityLevel);
    JITCACHE_CHECK(bakedFacts.couldBeTainted() == section.bakedFacts.couldBeTainted);
    JITCACHE_CHECK(bakedFacts.scopeFactCount() == section.bakedFacts.scopeFacts.size());
    for (unsigned index = 0; index < std::min<size_t>(bakedFacts.scopeFactCount(), section.bakedFacts.scopeFacts.size()); ++index)
        JITCACHE_CHECK(bakedFacts.scopeFact(index) == section.bakedFacts.scopeFacts[index]);
}

// The twins view reads back the model it was encoded from and decodes to the twin data the model holds.
static void checkTwinsMatch(TestContext& context, const ImageTwinsView& twins, const TestSection& section)
{
    auto& data = section.twins.data;
    auto producerValues = producerValuesOf(section);
    auto& header = twins.header();
    JITCACHE_CHECK(twins.compiledHoldingAPILock() == data.compileInputs.compiledHoldingAPILock);
    JITCACHE_CHECK(header.assemblerSeed == data.seeds.assembler);
    JITCACHE_CHECK(header.producerValueCount == producerValues.size());
    JITCACHE_CHECK(equalSpans(twins.captureProcessToken(), std::span<const uint8_t> { section.twins.token }));
    for (unsigned index = 0; index < std::min<size_t>(header.producerValueCount, producerValues.size()); ++index)
        JITCACHE_CHECK(twins.producerValue(index) == producerValues[index]);

    auto decoded = twins.twinData();
    JITCACHE_CHECK(decoded.seeds.assembler == data.seeds.assembler);
    JITCACHE_CHECK(decoded.seeds.binarySwitches == data.seeds.binarySwitches);
    JITCACHE_CHECK(decoded.compileInputs.inputs == data.compileInputs.inputs);
    JITCACHE_CHECK(decoded.compileInputs.binaryArithBits == data.compileInputs.binaryArithBits);
    JITCACHE_CHECK(decoded.compileInputs.unaryArithBits == data.compileInputs.unaryArithBits);
    JITCACHE_CHECK(decoded.compileInputs.compiledHoldingAPILock == data.compileInputs.compiledHoldingAPILock);
    JITCACHE_CHECK(decoded.regenerations.size() == data.regenerations.size());
    for (size_t index = 0; index < std::min(decoded.regenerations.size(), data.regenerations.size()); ++index) {
        auto& read = decoded.regenerations[index];
        auto& written = data.regenerations[index];
        JITCACHE_CHECK(read.mathICIndex == written.mathICIndex && read.profileBitsAtEntry == written.profileBitsAtEntry);
        JITCACHE_CHECK(read.replacement == written.replacement && read.assemblerSeeds == written.assemblerSeeds);
    }
    // A rebuilt record charges exactly what the decoded data holds (section 10.3, step 14).
    JITCACHE_CHECK(twinDataStorageBytes(decoded) == twins.twinDataStorageBytes());
}

static size_t inputIndexAt(const Vector<CompileInput>& inputs, uint32_t bytecodeOffset)
{
    for (size_t index = 0; index < inputs.size(); ++index) {
        if (inputs[index].bytecodeOffset == bytecodeOffset)
            return index;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static size_t inputIndexOfKind(const Vector<CompileInput>& inputs, CompileInputKind kind)
{
    for (size_t index = 0; index < inputs.size(); ++index) {
        if (inputs[index].kind == kind)
            return index;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void sortInputs(Vector<CompileInput>& inputs)
{
    std::sort(inputs.begin(), inputs.end(), [](const CompileInput& a, const CompileInput& b) {
        return a.bytecodeOffset < b.bytecodeOffset;
    });
}

static void expectUCBFailure(TestContext& context, ASCIILiteral label, const TestSection& section, const UnlinkedCodeBlock& unlinkedCodeBlock, ImageCheck expected)
{
    ExactSections sections(section);
    auto view = parseImageSections(sections.spans(), true);
    if (!view) {
        JITCACHE_FAIL(makeString(label, ": failed "_s, description(view.error()), " before the UCB checks"_s));
        return;
    }
    auto result = validateImageSectionsAgainst(*view, unlinkedCodeBlock);
    if (result) {
        JITCACHE_FAIL(makeString(label, ": passed the UCB checks, expected "_s, description(expected)));
        return;
    }
    if (result.error() != expected)
        JITCACHE_FAIL(makeString(label, ": rejected with "_s, description(result.error()), ", expected "_s, description(expected)));
}

} // namespace ImageSectionTestsInternal

JITCACHE_TEST(imageCheckDescriptions, No)
{
    JITCACHE_CHECK(description(ImageCheck::None) == "none"_s);
    JITCACHE_CHECK(description(ImageCheck::V1) == "v1"_s);
    JITCACHE_CHECK(description(ImageCheck::V7) == "v7"_s);
    JITCACHE_CHECK(description(ImageCheck::U1) == "u1"_s);
    JITCACHE_CHECK(description(ImageCheck::U7) == "u7"_s);
    JITCACHE_CHECK(description(ImageCheck::W4) == "w4"_s);
    JITCACHE_CHECK(description(ImageCheck::S4) == "s4"_s);
    JITCACHE_CHECK(description(ImageCheck::S5) == "s5"_s);
}

JITCACHE_TEST(imageCodeSymbols, No)
{
    auto anchor = CodeSymbol::of(&codeSymbolAnchor);
    JITCACHE_CHECK(anchor && !anchor->offset);
    auto parse = CodeSymbol::of(&parseImageSections);
    JITCACHE_CHECK(parse && CodeSymbol::isValid(parse->offset));
    if (parse)
        JITCACHE_CHECK(parse->address() == std::bit_cast<const void*>(&parseImageSections));
    int local = 0;
    JITCACHE_CHECK(!CodeSymbol::of(&local));
    JITCACHE_CHECK(!CodeSymbol::of(static_cast<const void*>(nullptr)));
    JITCACHE_CHECK(!CodeSymbol::isValid(std::numeric_limits<int64_t>::max()));
    JITCACHE_CHECK(!CodeSymbol::isValid(std::numeric_limits<int64_t>::min()));
#if CPU(ARM64)
    if (parse)
        JITCACHE_CHECK(!CodeSymbol::isValid(parse->offset + 2));
#endif
}

// Section 3.2: each form's footprint, canonical encoding and decoding on the build's architecture.
JITCACHE_TEST(imageSectionFootprints, No)
{
    using namespace ImageSectionTestsInternal;
    TestCode code;
    code.fill(16);
    code.pointer(target(TargetKind::VMAddress, 1), 3);
    code.call(target(TargetKind::CommonThunk, 0));
    code.jump(target(TargetKind::CommonThunk, 0));
#if CPU(X86_64)
    code.jump(target(TargetKind::CommonThunk, 0), true);
    constexpr std::array<size_t, 4> expectedSizes { 10, 5, 5, 6 };
    constexpr uint64_t expectedPointer = 0x1122334455667788;
#else
    constexpr std::array<size_t, 3> expectedSizes { 12, 4, 4 };
    constexpr uint64_t expectedPointer = 0x9ABC56781234;
#endif
    code.fill(fillUnit * 4);

    auto bytes = code.bytes();
    JITCACHE_CHECK(code.fixups().size() == expectedSizes.size());
    for (size_t index = 0; index < std::min(code.fixups().size(), expectedSizes.size()); ++index) {
        auto& fixup = code.fixups()[index];
        auto footprint = fixupFootprint(fixup.form, fixup.site, bytes);
        if (!footprint) {
            JITCACHE_FAIL(makeString("fixup "_s, index, " has no footprint"_s));
            continue;
        }
        JITCACHE_CHECK(footprint->size() == expectedSizes[index]);
        auto live = bytes.subspan(footprint->begin, footprint->size());
        auto canonical = canonicalFootprint(fixup.form, live);
        JITCACHE_CHECK(!isCanonicalFootprint(fixup.form, live));
        JITCACHE_CHECK(isCanonicalFootprint(fixup.form, canonical.span()));
        JITCACHE_CHECK(canonical.size == live.size());
        // A footprint that lies past the code has no extent.
        JITCACHE_CHECK(!fixupFootprint(fixup.form, fixup.site, bytes.first(footprint->end - 1)));

        auto decoded = decodeFootprint(fixup.form, live, footprint->begin);
        auto decodedCanonical = decodeFootprint(fixup.form, canonical.span(), footprint->begin);
        JITCACHE_CHECK(decoded && decodedCanonical);
        if (!decoded || !decodedCanonical)
            continue;
        switch (fixup.form) {
        case FixupForm::Pointer:
            JITCACHE_CHECK(*decoded == expectedPointer);
            JITCACHE_CHECK(!*decodedCanonical);
            break;
        case FixupForm::Call:
        case FixupForm::Jump:
#if CPU(X86_64)
            // rel32 from the end of the instruction, which is the site.
            JITCACHE_CHECK(*decoded == fixup.site + (fixup.form == FixupForm::Call ? 0x01020304u : 0x05060708u));
            JITCACHE_CHECK(*decodedCanonical == fixup.site);
#else
            // imm26 in instructions from the branch itself.
            JITCACHE_CHECK(*decoded == footprint->begin + 4 * (fixup.form == FixupForm::Call ? 0x40u : 0x80u));
            JITCACHE_CHECK(*decodedCanonical == footprint->begin);
#endif
            break;
        }
    }

#if CPU(X86_64)
    JITCACHE_CHECK(farCallPointerSite(16) == 13u);
    JITCACHE_CHECK(!farCallPointerSite(2));
    JITCACHE_CHECK(snippetEntrySite(8) == 13u);
#else
    JITCACHE_CHECK(farCallPointerSite(16) == 0u);
    JITCACHE_CHECK(!farCallPointerSite(12));
    JITCACHE_CHECK(snippetEntrySite(8) == 8u);
#endif
}

// V1 to V7 (section 8.5) and truncation, with strict on; and the same view with strict off for every valid section.
JITCACHE_TEST(imageSectionStructureChecks, Yes)
{
    using namespace ImageSectionTestsInternal;
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto valid = makeValidSection(*body);
    uint32_t codeSize = valid.code.size();

    // The valid section: the writer's size, both modes, and the view reading back the model.
    Vector<size_t> imageBoundaries;
    auto image = encodeImage(valid, &imageBoundaries);
    auto bakedFacts = encodeBakedFacts(valid.bakedFacts);
    auto twins = encodeTwins(valid);
    JITCACHE_CHECK(image.size() == sectionSize(valid));
    JITCACHE_CHECK(bakedFacts.size() == bakedFactsSectionSize(valid.bakedFacts));
    {
        ExactSections sections(image.span(), bakedFacts.span(), twins.span());
        auto strict = parseImageSections(sections.spans(), true);
        auto normal = parseImageSections(sections.spans(), false);
        JITCACHE_CHECK(strict && normal);
        if (strict && normal) {
            JITCACHE_CHECK(*strict == *normal);
            checkViewMatches(context, *strict, valid);
        }
    }

    // Truncation at every region and entry boundary, in both modes: a section that cannot be located fails V1 or V7.
    imageBoundaries.append(0);
    for (size_t boundary : imageBoundaries) {
        if (boundary >= image.size())
            continue;
        ExactSections sections(image.span().first(boundary), bakedFacts.span(), twins.span());
        auto strict = parseImageSections(sections.spans(), true);
        auto normal = parseImageSections(sections.spans(), false);
        if (strict || normal)
            JITCACHE_FAIL(makeString("an image section truncated at "_s, boundary, " parsed"_s));
    }
    for (size_t boundary = 0; boundary < bakedFacts.size(); boundary += (boundary < bakedFactsHeaderSize ? 4 : bakedFactsEntrySize)) {
        ExactSections sections(image.span(), bakedFacts.span().first(boundary), twins.span());
        auto strict = parseImageSections(sections.spans(), true);
        auto normal = parseImageSections(sections.spans(), false);
        if (!strict && strict.error() != ImageCheck::V7)
            JITCACHE_FAIL(makeString("a baked-facts section truncated at "_s, boundary, " failed "_s, description(strict.error())));
        if (strict || normal)
            JITCACHE_FAIL(makeString("a baked-facts section truncated at "_s, boundary, " parsed"_s));
    }

    // V1: header fields, size and padding.
    expectModelFailure(context, "a NaN liveness rate"_s, mutated(valid, [](TestSection& section) { section.livenessRate = std::numeric_limits<double>::quiet_NaN(); }), ImageCheck::V1);
    expectModelFailure(context, "an infinite fullness rate"_s, mutated(valid, [](TestSection& section) { section.fullnessRate = std::numeric_limits<double>::infinity(); }), ImageCheck::V1);
    expectModelFailure(context, "a liveness rate above 1"_s, mutated(valid, [](TestSection& section) { section.livenessRate = 1.5; }), ImageCheck::V1);
    expectModelFailure(context, "a negative fullness rate"_s, mutated(valid, [](TestSection& section) { section.fullnessRate = -0.25; }), ImageCheck::V1);
    expectImageBytesFailure(context, "trailing bytes"_s, valid, [](Vector<uint8_t>& bytes) {
        for (size_t index = 0; index < imageSectionAlignment; ++index)
            bytes.append(0);
    }, ImageCheck::V1);
    expectImageBytesFailure(context, "nonzero padding after the code"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[codeOffset() + codeSize] = 1;
    }, ImageCheck::V1);
    expectImageBytesFailure(context, "nonzero padding after a snippet's code"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[mathICOffset(valid, valid.mathICs.size()) + valid.mathICs[0].snippet->code.size()] = 1;
    }, ImageCheck::V1);

    // V2: code size, arity entry and, on ARM64, instruction alignment.
    expectModelFailure(context, "an empty image"_s, minimalSection(0), ImageCheck::V2);
    expectValid(context, "the smallest image"_s, minimalSection(8));
    expectModelFailure(context, "an arity entry at the code's end"_s, mutated(valid, [&](TestSection& section) { section.arityEntryOffset = codeSize; }), ImageCheck::V2);
#if CPU(ARM64)
    expectModelFailure(context, "a done location inside an instruction"_s, mutated(valid, [](TestSection& section) { section.molds[0].doneLocation = 14; }), ImageCheck::V2);
    expectModelFailure(context, "a code map offset inside an instruction"_s, mutated(valid, [](TestSection& section) { section.codeMap.last() += 2; }), ImageCheck::V2);
    expectModelFailure(context, "a snippet jumping inside an instruction"_s, mutated(valid, [](TestSection& section) {
        auto& fixups = section.mathICs[0].snippet->fixups;
        fixups[fixupIndex(fixups, TargetKind::ImageOffset)].target.a += 2;
    }), ImageCheck::V2);
#endif

    // V3: each form's canonical encoding, on the build's architecture.
    size_t pointerFootprint = codeOffset() + footprintBegin(valid, fixupIndex(valid.fixups, TargetKind::VMCell));
    size_t callFootprint = codeOffset() + footprintBegin(valid, fixupIndex(valid.fixups, TargetKind::BaselineThunk, FixupForm::Call));
    size_t jumpFootprint = codeOffset() + footprintBegin(valid, fixupIndex(valid.fixups, TargetKind::BaselineThunk, FixupForm::Jump));
#if CPU(X86_64)
    expectImageBytesFailure(context, "a Pointer with a nonzero immediate"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[pointerFootprint + 9] = 1; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Call with a nonzero rel32"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[callFootprint + 1] = 1; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Jump with a nonzero rel32"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[jumpFootprint + 4] = 1; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Pointer without REX.W"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[pointerFootprint] = 0x4C; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Pointer that is not a movabs"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[pointerFootprint + 1] = 0x8B; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Call holding a jmp"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[callFootprint] = 0xE9; }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Jump that is neither E9 nor 0F 80 to 0F 8F"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[jumpFootprint] = 0xEB; }, ImageCheck::V3);
    size_t conditionalFootprint = codeOffset() + footprintBegin(valid, fixupIndex(valid.fixups, TargetKind::CommonThunk, FixupForm::Jump, 2));
    expectImageBytesFailure(context, "a conditional Jump outside 0F 80 to 0F 8F"_s, valid, [&](Vector<uint8_t>& bytes) { bytes[conditionalFootprint + 1] = 0x90; }, ImageCheck::V3);
#else
    expectImageBytesFailure(context, "a Pointer with a nonzero imm16"_s, valid, [&](Vector<uint8_t>& bytes) {
        setWord(bytes, pointerFootprint + 8, wordAt(bytes.span(), pointerFootprint + 8) | (1u << 5));
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Call with a nonzero imm26"_s, valid, [&](Vector<uint8_t>& bytes) {
        setWord(bytes, callFootprint, wordAt(bytes.span(), callFootprint) | 1);
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Jump with a nonzero imm26"_s, valid, [&](Vector<uint8_t>& bytes) {
        setWord(bytes, jumpFootprint, wordAt(bytes.span(), jumpFootprint) | 1);
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Pointer starting with movn"_s, valid, [&](Vector<uint8_t>& bytes) {
        setWord(bytes, pointerFootprint, 0x92800000 | (wordAt(bytes.span(), pointerFootprint) & 0x1F));
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Pointer whose words name different registers"_s, valid, [&](Vector<uint8_t>& bytes) {
        uint32_t word = wordAt(bytes.span(), pointerFootprint + 4);
        setWord(bytes, pointerFootprint + 4, (word & ~0x1Fu) | (((word & 0x1F) + 1) & 0x1F));
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Call holding a b"_s, valid, [&](Vector<uint8_t>& bytes) { setWord(bytes, callFootprint, 0x14000000); }, ImageCheck::V3);
    expectImageBytesFailure(context, "a Jump holding a bl"_s, valid, [&](Vector<uint8_t>& bytes) { setWord(bytes, jumpFootprint, 0x94000000); }, ImageCheck::V3);

    // A writer that met a nop at a Jump's site would rewrite the instruction before it, here before the allocation.
    TestCode jumpAtStart;
    jumpAtStart.jump(target(TargetKind::CommonThunk, 0));
    jumpAtStart.fill(4);
    TestSection jumpAtSiteZero = minimalSection(0);
    jumpAtSiteZero.code = canonicalCode(jumpAtStart);
    jumpAtSiteZero.fixups = jumpAtStart.fixups();
    expectValid(context, "a Jump at site 0"_s, jumpAtSiteZero);
    expectImageBytesFailure(context, "a Jump at site 0 holding a nop"_s, jumpAtSiteZero, [&](Vector<uint8_t>& bytes) { setWord(bytes, codeOffset(), 0xD503201F); }, ImageCheck::V3);

    // The valid section already holds a Call and a Pointer at one site, in footprint order.
    size_t sharedCall = fixupIndex(valid.fixups, TargetKind::CommonThunk, FixupForm::Call, 0);
    JITCACHE_CHECK(valid.fixups[sharedCall].site == valid.fixups[sharedCall + 1].site);
    expectModelFailure(context, "a Pointer listed before the Call whose site it shares"_s, mutated(valid, [&](TestSection& section) {
        std::swap(section.fixups[sharedCall], section.fixups[sharedCall + 1]);
    }), ImageCheck::V3);
#endif
    size_t vmCellFixup = fixupIndex(valid.fixups, TargetKind::VMCell);
    expectModelFailure(context, "fixups out of footprint order"_s, mutated(valid, [&](TestSection& section) {
        std::swap(section.fixups[vmCellFixup], section.fixups[vmCellFixup + 1]);
    }), ImageCheck::V3);
    expectModelFailure(context, "overlapping footprints"_s, mutated(valid, [&](TestSection& section) {
        ImageFixup duplicate = section.fixups[vmCellFixup];
        section.fixups.insert(vmCellFixup, duplicate);
    }), ImageCheck::V3);
    expectModelFailure(context, "a footprint past the code"_s, mutated(valid, [&](TestSection& section) {
        section.fixups.append(ImageFixup { .site = codeSize + 64, .form = FixupForm::Pointer, .target = target(TargetKind::StructureIDBase) });
    }), ImageCheck::V3);
    expectModelFailure(context, "a form its kind does not allow"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[vmCellFixup].target.kind = TargetKind::CommonThunk;
    }), ImageCheck::V3);
    expectModelFailure(context, "a nonzero unused field"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[vmCellFixup].target.b = 1;
    }), ImageCheck::V3);
    expectModelFailure(context, "a kind outside table 3.3"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[vmCellFixup].target.kind = static_cast<TargetKind>(numberOfTargetKinds + 1);
    }), ImageCheck::V3);
    expectModelFailure(context, "a VM cell outside table 3.6"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[vmCellFixup].target.a = numberOfVMCells + 1;
    }), ImageCheck::V3);
    expectModelFailure(context, "a common thunk out of range"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::CommonThunk, FixupForm::Jump)].target.a = numberOfCommonThunkIDs;
    }), ImageCheck::V3);
    expectModelFailure(context, "a code symbol outside the text segment"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::SlowPathThunk)].target.payload = std::numeric_limits<int64_t>::max();
    }), ImageCheck::V3);
    expectModelFailure(context, "an ImageOffset in the image"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::BaselineThunk, FixupForm::Jump)].target = target(TargetKind::ImageOffset, 0);
    }), ImageCheck::V3);
    expectModelFailure(context, "a SnippetEntry in a snippet"_s, mutated(valid, [&](TestSection& section) {
        auto& fixups = section.mathICs[0].snippet->fixups;
        fixups[fixupIndex(fixups, TargetKind::ImageOffset)].target = target(TargetKind::SnippetEntry, 0);
    }), ImageCheck::V3);
    expectImageBytesFailure(context, "a nonzero reserved field in a fixup"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[fixupOffset(valid, vmCellFixup) + 6] = 1;
    }, ImageCheck::V3);
    expectImageBytesFailure(context, "a form byte outside 1 to 3"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[fixupOffset(valid, vmCellFixup) + 4] = 4;
    }, ImageCheck::V3);

    // V4: side tables.
    expectModelFailure(context, "calls out of bytecode order"_s, mutated(valid, [](TestSection& section) {
        std::swap(section.calls[0], section.calls[1]);
    }), ImageCheck::V4);
    expectModelFailure(context, "an invalid access type"_s, mutated(valid, [](TestSection& section) {
        section.molds[0].accessType = static_cast<AccessType>(numberOfAccessTypes);
    }), ImageCheck::V4);
    expectModelFailure(context, "an invalid cache type"_s, mutated(valid, [](TestSection& section) {
        section.molds[0].preconfiguredCacheType = static_cast<CacheType>(100);
    }), ImageCheck::V4);
    expectModelFailure(context, "a reserved mold flag"_s, mutated(valid, [](TestSection& section) {
        section.molds[0].flags = ImageMoldFlags::all + 1;
    }), ImageCheck::V4);
    expectModelFailure(context, "an invalid identifier kind"_s, mutated(valid, [](TestSection& section) {
        section.molds[0].identifierKind = static_cast<MoldIdentifierKind>(3);
    }), ImageCheck::V4);
    expectModelFailure(context, "an immortal name outside the table"_s, mutated(valid, [](TestSection& section) {
        section.molds[1].identifier = numberOfImmortalNames + 1;
    }), ImageCheck::V4);
    expectModelFailure(context, "an identifier on a mold without one"_s, mutated(valid, [](TestSection& section) {
        section.molds[0].identifierKind = MoldIdentifierKind::None;
        section.molds[0].identifier = 1;
    }), ImageCheck::V4);
    expectModelFailure(context, "a done location past the code"_s, mutated(valid, [&](TestSection& section) {
        section.molds[0].doneLocation = codeSize + 4;
    }), ImageCheck::V4);
    expectModelFailure(context, "a dense table offset past the code"_s, mutated(valid, [&](TestSection& section) {
        section.simpleTables[body->denseTable].offsets[0] = codeSize + 4;
    }), ImageCheck::V4);
    expectModelFailure(context, "a string table offset past the code"_s, mutated(valid, [&](TestSection& section) {
        section.stringTables[body->inlineStringTable][0] = codeSize + 4;
    }), ImageCheck::V4);
    expectModelFailure(context, "a decreasing code map"_s, mutated(valid, [&](TestSection& section) {
        section.codeMap[0] = codeSize & ~3u;
    }), ImageCheck::V4);
    expectModelFailure(context, "a constant pool type outside 0 and 1"_s, mutated(valid, [](TestSection& section) {
        section.constantPool[0].type = static_cast<ImageConstantType>(2);
    }), ImageCheck::V4);

    // V5: MathICs.
    expectImageBytesFailure(context, "a MathIC kind outside 1 to 4"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[mathICOffset(valid, 1)] = 5;
    }, ImageCheck::V5);
    expectImageBytesFailure(context, "a nonzero reserved field in a MathIC"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[mathICOffset(valid, 1) + 2] = 1;
    }, ImageCheck::V5);
    expectModelFailure(context, "a reserved MathIC flag"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[1].entry.flags |= ImageMathICFlags::all + 1;
    }), ImageCheck::V5);
    expectModelFailure(context, "a MathIC without inline code with a location"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[2].entry.inlineStart = 4;
    }), ImageCheck::V5);
    expectModelFailure(context, "a MathIC without inline code with other flags"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[2].entry.flags |= ImageMathICFlags::generateFastPathOnRepatch;
    }), ImageCheck::V5);
    expectModelFailure(context, "an empty inline range"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[1].entry.inlineStart = section.mathICs[1].entry.inlineEnd;
    }), ImageCheck::V5);
    expectModelFailure(context, "a slow call without its Operation fixup"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[1].entry.slowPathCall += 4;
    }), ImageCheck::V5);
    expectModelFailure(context, "a snippet without its SnippetEntry fixup"_s, mutated(valid, [](TestSection& section) {
        section.fixups.removeAt(fixupIndex(section.fixups, TargetKind::SnippetEntry));
    }), ImageCheck::V5);
    expectModelFailure(context, "a SnippetEntry naming a MathIC without a snippet"_s, mutated(valid, [](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::SnippetEntry)].target.a = 1;
    }), ImageCheck::V5);
    expectModelFailure(context, "a snippet jumping elsewhere in the image"_s, mutated(valid, [](TestSection& section) {
        auto& fixups = section.mathICs[0].snippet->fixups;
        fixups[fixupIndex(fixups, TargetKind::ImageOffset)].target.a += 4;
    }), ImageCheck::V5);
    expectModelFailure(context, "a snippet size without a snippet"_s, mutated(valid, [](TestSection& section) {
        section.mathICs[1].entry.snippetSize = 4;
    }), ImageCheck::V5);

    // V6: artifact targets.
    size_t mathICFixup = fixupIndex(valid.fixups, TargetKind::MathIC);
    size_t switchTableFixup = fixupIndex(valid.fixups, TargetKind::SwitchTableBase);
    expectModelFailure(context, "a MathIC target past the MathICs"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[mathICFixup].target.a = section.mathICs.size();
    }), ImageCheck::V6);
    expectModelFailure(context, "a MathIC target without inline code"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[mathICFixup].target.a = 2;
    }), ImageCheck::V6);
    expectModelFailure(context, "a switch table base of a list table"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[switchTableFixup].target.a = body->listTable;
    }), ImageCheck::V6);
    expectModelFailure(context, "a switch table base past the tables"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[switchTableFixup].target.a = section.simpleTables.size();
    }), ImageCheck::V6);

    // V7: baked facts.
    expectBakedFactsBytesFailure(context, "a capability level of CapabilityLevelNotSet"_s, valid, [](Vector<uint8_t>& bytes) { bytes[0] = DFG::CapabilityLevelNotSet; });
    expectBakedFactsBytesFailure(context, "a taint of 2"_s, valid, [](Vector<uint8_t>& bytes) { bytes[1] = 2; });
    expectBakedFactsBytesFailure(context, "a nonzero reserved header field"_s, valid, [](Vector<uint8_t>& bytes) { bytes[2] = 1; });
    expectBakedFactsBytesFailure(context, "a nonzero reserved entry field"_s, valid, [](Vector<uint8_t>& bytes) { bytes[bakedFactsHeaderSize + 6] = 1; });
    expectBakedFactsBytesFailure(context, "an opcode outside 1 to 3"_s, valid, [](Vector<uint8_t>& bytes) { bytes[bakedFactsHeaderSize + 4] = 4; });
    expectBakedFactsBytesFailure(context, "trailing bytes"_s, valid, [](Vector<uint8_t>& bytes) { bytes.append(0); });
    expectBakedFactsBytesFailure(context, "a count past the entries"_s, valid, [](Vector<uint8_t>& bytes) { setU32(bytes, 4, 4); });
    expectModelFailure(context, "facts out of bytecode order"_s, mutated(valid, [](TestSection& section) {
        std::swap(section.bakedFacts.scopeFacts[0], section.bakedFacts.scopeFacts[1]);
    }), ImageCheck::V7);
    expectModelFailure(context, "two facts for one instruction"_s, mutated(valid, [](TestSection& section) {
        section.bakedFacts.scopeFacts[1].bytecodeOffset = section.bakedFacts.scopeFacts[0].bytecodeOffset;
    }), ImageCheck::V7);
    size_t getFromScopeFact = 0;
    while (valid.bakedFacts.scopeFacts[getFromScopeFact].opcode != ScopeOpcode::GetFromScope)
        ++getFromScopeFact;
    expectModelFailure(context, "a resolve type the opcode does not bake"_s, mutated(valid, [&](TestSection& section) {
        section.bakedFacts.scopeFacts[getFromScopeFact].resolveType = GlobalVar;
    }), ImageCheck::V7);
    expectModelFailure(context, "a depth on a get_from_scope fact"_s, mutated(valid, [&](TestSection& section) {
        section.bakedFacts.scopeFacts[getFromScopeFact].localScopeDepth = 1;
    }), ImageCheck::V7);
}

// U1 to U7 (section 8.5) against the UCB the section was built from, and for the valid section W4 too.
JITCACHE_TEST(imageSectionUCBChecks, Yes)
{
    using namespace ImageSectionTestsInternal;
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto valid = makeValidSection(*body);
    auto& unlinkedCodeBlock = *body->unlinkedCodeBlock;

    {
        ExactSections sections(valid);
        auto view = parseImageSections(sections.spans(), true);
        JITCACHE_CHECK(view);
        if (view) {
            auto result = validateImageSectionsAgainst(*view, unlinkedCodeBlock);
            if (!result)
                JITCACHE_FAIL(makeString("the valid section failed "_s, description(result.error())));
            assertImageSectionsAgainst(*view, unlinkedCodeBlock);
        }
    }

    // U1, U2: switch tables.
    expectUCBFailure(context, "a dense table with an extra entry"_s, mutated(valid, [&](TestSection& section) {
        section.simpleTables[body->denseTable].offsets.append(24);
    }), unlinkedCodeBlock, ImageCheck::U1);
    expectUCBFailure(context, "a list table with entries"_s, mutated(valid, [&](TestSection& section) {
        section.simpleTables[body->listTable].offsets.append(24);
    }), unlinkedCodeBlock, ImageCheck::U1);
    expectUCBFailure(context, "an extra simple table"_s, mutated(valid, [](TestSection& section) {
        section.simpleTables.append(TestSimpleTable { });
    }), unlinkedCodeBlock, ImageCheck::U1);
    expectUCBFailure(context, "an empty string table"_s, mutated(valid, [&](TestSection& section) {
        section.stringTables[body->inlineStringTable].clear();
    }), unlinkedCodeBlock, ImageCheck::U2);

    // U3: indexes into the UCB.
    expectUCBFailure(context, "an identifier past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBIdentifier)].target.a = unlinkedCodeBlock.numberOfIdentifiers();
    }), unlinkedCodeBlock, ImageCheck::U3);
    expectUCBFailure(context, "a binary arithmetic profile past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBBinaryArithProfile)].target.a = unlinkedCodeBlock.numberOfBinaryArithProfiles();
    }), unlinkedCodeBlock, ImageCheck::U3);
    expectUCBFailure(context, "a unary arithmetic profile past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBUnaryArithProfile)].target.a = unlinkedCodeBlock.numberOfUnaryArithProfiles();
    }), unlinkedCodeBlock, ImageCheck::U3);
    expectUCBFailure(context, "a constant past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBConstantCell)].target.a = unlinkedCodeBlock.constantRegisters().size();
    }), unlinkedCodeBlock, ImageCheck::U3);
    expectUCBFailure(context, "a mold identifier past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.molds[0].identifier = unlinkedCodeBlock.numberOfIdentifiers();
    }), unlinkedCodeBlock, ImageCheck::U3);
    expectUCBFailure(context, "a function declaration past the UCB's"_s, mutated(valid, [&](TestSection& section) {
        section.constantPool[0].index = unlinkedCodeBlock.functionDecls().size();
    }), unlinkedCodeBlock, ImageCheck::U3);

    // U4: bytecode indexes.
    expectUCBFailure(context, "a call inside an instruction"_s, mutated(valid, [&](TestSection& section) {
        section.calls[1].bytecodeIndex = BytecodeIndex(body->addOffset + 1);
    }), unlinkedCodeBlock, ImageCheck::U4);
    if (unlinkedCodeBlock.instructions().at(body->addOffset)->numberOfCheckpoints() == 1) {
        expectUCBFailure(context, "a checkpoint on an instruction without one"_s, mutated(valid, [&](TestSection& section) {
            section.molds[0].bytecodeIndex = BytecodeIndex(body->addOffset, 1);
        }), unlinkedCodeBlock, ImageCheck::U4);
    }
    expectUCBFailure(context, "an add MathIC on a sub"_s, mutated(valid, [&](TestSection& section) {
        section.mathICs[0].entry.bytecodeOffset = body->subOffset;
    }), unlinkedCodeBlock, ImageCheck::U4);
    expectUCBFailure(context, "a get_from_scope fact on a put_to_scope"_s, mutated(valid, [](TestSection& section) {
        for (auto& fact : section.bakedFacts.scopeFacts) {
            if (fact.opcode == ScopeOpcode::PutToScope)
                fact.opcode = ScopeOpcode::GetFromScope;
        }
    }), unlinkedCodeBlock, ImageCheck::U4);

    // U5: the code map pairs every instruction start.
    expectUCBFailure(context, "a code map missing one instruction start"_s, mutated(valid, [](TestSection& section) {
        section.codeMap.removeLast();
    }), unlinkedCodeBlock, ImageCheck::U5);

    // U6: string switch ranks.
    uint32_t table = body->inlineStringTable;
    expectUCBFailure(context, "two case jumps for one rank and none for another"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::SwitchStringRankCase, FixupForm::Jump, table, 1)].target.b = 0;
    }), unlinkedCodeBlock, ImageCheck::U6);
    expectUCBFailure(context, "a rank without its case jump"_s, mutated(valid, [&](TestSection& section) {
        section.fixups.removeAt(fixupIndex(section.fixups, TargetKind::SwitchStringRankCase, FixupForm::Jump, table, 2));
    }), unlinkedCodeBlock, ImageCheck::U6);
    expectUCBFailure(context, "a rank without an atom comparison"_s, mutated(valid, [&](TestSection& section) {
        section.fixups.removeAt(fixupIndex(section.fixups, TargetKind::SwitchStringRankAtom, FixupForm::Pointer, table, 0));
    }), unlinkedCodeBlock, ImageCheck::U6);
    expectUCBFailure(context, "a rank past the key count"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::SwitchStringRankAtom, FixupForm::Pointer, table, 0)].target.b = body->inlineStringKeyCount;
    }), unlinkedCodeBlock, ImageCheck::U6);
    expectUCBFailure(context, "a rank of a table past the string tables"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::SwitchStringRankAtom, FixupForm::Pointer, table, 0)].target.a = section.stringTables.size();
    }), unlinkedCodeBlock, ImageCheck::U6);

    // U7: constant kinds.
    expectUCBFailure(context, "a constant cell target on a non-cell constant"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBConstantCell)].target.a = body->nonCellConstant;
    }), unlinkedCodeBlock, ImageCheck::U7);
    expectUCBFailure(context, "a constant atom target on a regexp"_s, mutated(valid, [&](TestSection& section) {
        section.fixups[fixupIndex(section.fixups, TargetKind::UCBConstantAtom)].target.a = body->regExpConstant;
    }), unlinkedCodeBlock, ImageCheck::U7);
}

// W1 to W4 (section 11.2) and truncation of image-twins.baseline, with strict on, W4 against the UCB the twin data was
// taken from; with strict off a valid section parses to the view strict gives it. W4's strict-equality cases are T18's.
JITCACHE_TEST(imageTwinsSectionChecks, Yes)
{
    using namespace ImageSectionTestsInternal;
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto valid = makeValidSection(*body);
    auto& unlinkedCodeBlock = *body->unlinkedCodeBlock;
    auto layout = twinsLayoutOf(valid);
    auto& validInputs = valid.twins.data.compileInputs.inputs;

    // The snapshot and the strict-equality inputs the valid section carries: the producer compared 'delta' by its atom,
    // 7 by the generic comparison, and its template never asked about null.
    JITCACHE_CHECK(valid.twins.data.compileInputs.compiledHoldingAPILock);
    JITCACHE_CHECK(validInputs[inputIndexAt(validInputs, body->atomEqualityOffset)].value == static_cast<uint8_t>(StrictEqualityAtomOperand::Rhs));
    JITCACHE_CHECK(validInputs[inputIndexAt(validInputs, body->numberEqualityOffset)].value == static_cast<uint8_t>(StrictEqualityAtomOperand::None));
    JITCACHE_CHECK(std::ranges::none_of(validInputs, [&](const CompileInput& input) { return input.bytecodeOffset == body->nullEqualityOffset; }));
    JITCACHE_CHECK(validInputs[inputIndexOfKind(validInputs, CompileInputKind::ResolveScopeType)].bytecodeOffset == body->resolveScopeOffset);

    // The valid section: the writer's size, both modes, and the view reading back the model.
    auto image = encodeImage(valid);
    auto bakedFacts = encodeBakedFacts(valid.bakedFacts);
    auto twins = encodeTwins(valid);
    JITCACHE_CHECK(twins.size() == twinsSectionSize(valid.twins.data, producerValuesOf(valid).size()));
    JITCACHE_CHECK(twins.size() == layout.boundaries.last());
    {
        ExactSections sections(image.span(), bakedFacts.span(), twins.span());
        auto strict = parseImageSections(sections.spans(), true);
        auto normal = parseImageSections(sections.spans(), false);
        JITCACHE_CHECK(strict && normal);
        if (strict && normal) {
            JITCACHE_CHECK(*strict == *normal);
            checkTwinsMatch(context, strict->twins(), valid);
        }
    }

    // A body without the section, and truncation at every region boundary and inside the header and each regeneration's
    // fixed fields, in both modes: a section that cannot be located fails W1.
    Vector<size_t> boundaries = layout.boundaries;
    boundaries.append(0);
    boundaries.append(imageTwinsHeaderSize - 1);
    for (size_t offset : layout.regenerations)
        boundaries.append(offset + imageTwinsRegenerationFixedSize - 1);
    for (size_t boundary : boundaries) {
        if (boundary >= twins.size())
            continue;
        ExactSections sections(image.span(), bakedFacts.span(), twins.span().first(boundary));
        auto strict = parseImageSections(sections.spans(), true);
        auto normal = parseImageSections(sections.spans(), false);
        if (!strict && strict.error() != ImageCheck::W1)
            JITCACHE_FAIL(makeString("a twins section truncated at "_s, boundary, " failed "_s, description(strict.error())));
        if (strict || normal)
            JITCACHE_FAIL(makeString("a twins section truncated at "_s, boundary, " parsed"_s));
    }

    // W1: reserved bytes and bits, the token, the size and padding.
    expectTwinsBytesFailure(context, "a flag bit other than bit 0"_s, valid, [](Vector<uint8_t>& bytes) { bytes[0] |= 2; }, ImageCheck::W1);
    expectTwinsBytesFailure(context, "a nonzero reserved header byte"_s, valid, [](Vector<uint8_t>& bytes) { bytes[3] = 1; }, ImageCheck::W1);
    expectModelFailure(context, "an all-zero capture-process token"_s, mutated(valid, [](TestSection& section) {
        section.twins.token = { };
    }), ImageCheck::W1);
    expectTwinsBytesFailure(context, "trailing bytes"_s, valid, [](Vector<uint8_t>& bytes) {
        for (size_t index = 0; index < imageSectionAlignment; ++index)
            bytes.append(0);
    }, ImageCheck::W1);
    expectTwinsBytesFailure(context, "nonzero padding after the switch seeds"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[layout.binarySwitchSeeds + 4 * valid.twins.data.seeds.binarySwitches.size()] = 1;
    }, ImageCheck::W1);
    expectTwinsBytesFailure(context, "a nonzero reserved field in an input"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[layout.inputs + 6] = 1;
    }, ImageCheck::W1);
    if (size_t unaryCount = valid.twins.data.compileInputs.unaryArithBits.size(); unaryCount % 4) {
        expectTwinsBytesFailure(context, "nonzero padding after the unary arithmetic profile bits"_s, valid, [&](Vector<uint8_t>& bytes) {
            bytes[layout.unaryArithBits + 2 * unaryCount] = 1;
        }, ImageCheck::W1);
    }
    expectTwinsBytesFailure(context, "nonzero padding after a regeneration's seeds"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[layout.regenerations[1] + imageTwinsRegenerationFixedSize + 4] = 1;
    }, ImageCheck::W1);

    // W2: inputs.
    size_t resolveScopeInput = inputIndexOfKind(validInputs, CompileInputKind::ResolveScopeType);
    size_t getFromScopeInput = inputIndexOfKind(validInputs, CompileInputKind::GetFromScopeType);
    size_t atomEqualityInput = inputIndexAt(validInputs, body->atomEqualityOffset);
    size_t numberEqualityInput = inputIndexAt(validInputs, body->numberEqualityOffset);
    auto mutatedInputs = [&](const auto& mutation) {
        return mutated(valid, [&](TestSection& section) {
            mutation(section.twins.data.compileInputs.inputs);
        });
    };
    expectModelFailure(context, "inputs out of bytecode order"_s, mutatedInputs([](Vector<CompileInput>& inputs) {
        std::swap(inputs[0], inputs[1]);
    }), ImageCheck::W2);
    expectModelFailure(context, "two inputs for one instruction"_s, mutatedInputs([](Vector<CompileInput>& inputs) {
        inputs[1].bytecodeOffset = inputs[0].bytecodeOffset;
    }), ImageCheck::W2);
    expectModelFailure(context, "an input kind of 0"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].kind = static_cast<CompileInputKind>(0);
    }), ImageCheck::W2);
    expectModelFailure(context, "an input kind of 9"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].kind = static_cast<CompileInputKind>(9);
    }), ImageCheck::W2);
    expectModelFailure(context, "a resolve type past Dynamic"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].value = static_cast<uint8_t>(Dynamic) + 1;
    }), ImageCheck::W2);
    expectModelFailure(context, "a get_by_id mode past the modes"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].kind = CompileInputKind::GetByIdMode;
        inputs[getFromScopeInput].value = static_cast<uint8_t>(GetByIdMode::ArrayLength) + 1;
    }), ImageCheck::W2);
    expectModelFailure(context, "an enumerator byte outside the flag bits"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].kind = CompileInputKind::EnumeratorMetadata;
        inputs[getFromScopeInput].value = 0x80;
    }), ImageCheck::W2);
    expectModelFailure(context, "a strict-equality operand past the right one"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[atomEqualityInput].value = static_cast<uint8_t>(StrictEqualityAtomOperand::Rhs) + 1;
    }), ImageCheck::W2);
    expectModelFailure(context, "a depth on a get_from_scope input"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[getFromScopeInput].localScopeDepth = 1;
    }), ImageCheck::W2);

    // W3: regenerations and the producer value count.
    auto mutatedRegenerations = [&](const auto& mutation) {
        return mutated(valid, [&](TestSection& section) {
            mutation(section.twins.data.regenerations);
        });
    };
    expectModelFailure(context, "a producer value too many"_s, mutated(valid, [](TestSection& section) {
        section.twins.extraProducerValues = 1;
    }), ImageCheck::W3);
    expectModelFailure(context, "a regeneration of a MathIC past the MathICs"_s, mutatedRegenerations([&](Vector<TwinRegeneration>& regenerations) {
        regenerations[0].mathICIndex = valid.mathICs.size();
    }), ImageCheck::W3);
    expectModelFailure(context, "a regeneration of a MathIC without inline code"_s, mutatedRegenerations([](Vector<TwinRegeneration>& regenerations) {
        regenerations[0].mathICIndex = 2;
    }), ImageCheck::W3);
    expectModelFailure(context, "a regeneration that attached nothing"_s, mutatedRegenerations([](Vector<TwinRegeneration>& regenerations) {
        regenerations[0].assemblerSeeds.clear();
    }), ImageCheck::W3);
    expectModelFailure(context, "a regeneration that attached three times"_s, mutatedRegenerations([](Vector<TwinRegeneration>& regenerations) {
        regenerations[1].assemblerSeeds = { 1u, 2u, 3u };
    }), ImageCheck::W3);
    expectModelFailure(context, "a replacement outside the text segment"_s, mutatedRegenerations([](Vector<TwinRegeneration>& regenerations) {
        regenerations[0].replacement = CodeSymbol { std::numeric_limits<int64_t>::max() };
    }), ImageCheck::W3);
    expectTwinsBytesFailure(context, "a seed mask bit past the attaches"_s, valid, [&](Vector<uint8_t>& bytes) {
        bytes[layout.regenerations[0] + 7] |= 1 << 2;
    }, ImageCheck::W3);
    expectTwinsBytesFailure(context, "a seed where the mask has no bit"_s, valid, [&](Vector<uint8_t>& bytes) {
        setU32(bytes, layout.regenerations[0] + imageTwinsRegenerationFixedSize + 4, 1);
    }, ImageCheck::W3);

    // W4: the inputs against the UCB's instructions, and the arithmetic profile counts.
    expectUCBFailure(context, "an extra binary arithmetic profile"_s, mutated(valid, [](TestSection& section) {
        section.twins.data.compileInputs.binaryArithBits.append(0);
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a resolve_scope without its input"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs.removeAt(resolveScopeInput);
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a resolve_scope with a get_from_scope input"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[resolveScopeInput].kind = CompileInputKind::GetFromScopeType;
        inputs[resolveScopeInput].localScopeDepth = 0;
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "an input inside an instruction"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[resolveScopeInput].bytecodeOffset += 1;
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "an input at an instruction that asks none"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs.insert(0, CompileInput { .bytecodeOffset = body->instructionStarts[0], .kind = CompileInputKind::ResolveScopeType, .value = static_cast<uint8_t>(GlobalProperty), .localScopeDepth = 0 });
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a strict-equality input naming an operand that is no constant"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[atomEqualityInput].value = static_cast<uint8_t>(StrictEqualityAtomOperand::Lhs);
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a strict-equality input naming a number"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[numberEqualityInput].value = static_cast<uint8_t>(StrictEqualityAtomOperand::Rhs);
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a strict-equality input moved to an equality with null"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs[numberEqualityInput].bytecodeOffset = body->nullEqualityOffset;
        sortInputs(inputs);
    }), unlinkedCodeBlock, ImageCheck::W4);
    expectUCBFailure(context, "a strict equality without its input"_s, mutatedInputs([&](Vector<CompileInput>& inputs) {
        inputs.removeAt(atomEqualityInput);
    }), unlinkedCodeBlock, ImageCheck::W4);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
