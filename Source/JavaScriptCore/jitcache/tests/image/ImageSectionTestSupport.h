#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CPU.h"
#include "CallMode.h"
#include "CodeBlock.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "GetPutInfo.h"
#include "InlineCacheHandler.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "PropertyInlineCache.h"
#include "RegExp.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/image/BakedFacts.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageSupport.h"
#include "jitcache/image/ImageTwinsSection.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestVM.h"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

// What the Image lane's section tests share: a model of image.baseline, baked-facts.baseline and image-twins.baseline,
// built from a live body's UCB with a fixup of every target kind, the code whose references the model's fixups locate,
// and writers that encode the model entry by entry with the lane's own encoders. The names live in a namespace of their
// own, since names such as TestSection and Body are generic.

namespace JSC::JITCache::Tests::ImageSectionTestSupport {

inline constexpr size_t fillUnit = isARM64() ? 4 : 1;

inline int64_t anchorSymbol()
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
Vector<uint8_t> collect(const WriteFunction& write)
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

inline Vector<uint8_t> canonicalCode(const TestCode& code)
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

// The image-twins.baseline a twins build always reads beside the other two. Its producer values are one per fixup of the
// model, the image's and then each snippet's.
struct TestTwins {
    TestTwins()
    {
        data.seeds.assembler = 0x5eed;
        data.compileInputs.compiledHoldingAPILock = true;
        token[0] = 0x5a;
    }

    TwinData data;
    std::array<uint8_t, captureProcessTokenSize> token { };
};

// A model of the three sections, which the tests encode entry by entry and compare with what the views read back.
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

inline ImageSectionHeader headerOf(const TestSection& section)
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

// The section in its format's region order. boundaries, when given, receives the offset after every write, the region
// boundaries among them.
inline Vector<uint8_t> encodeImage(const TestSection& section, Vector<size_t>* boundaries = nullptr)
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

inline Vector<uint8_t> encodeBakedFacts(const BakedFacts& facts)
{
    return collect([&](const ImageSectionSink& sink) {
        return writeBakedFactsSection(facts, sink);
    });
}

// One producer value per fixup of the model, the image's and then each snippet's, each a distinct made-up address.
inline Vector<uint64_t> producerValuesOf(const TestSection& section)
{
    size_t count = section.fixups.size();
    for (auto& mathIC : section.mathICs) {
        if (mathIC.entry.hasSnippet() && mathIC.snippet)
            count += mathIC.snippet->fixups.size();
    }
    return Vector<uint64_t>(count, [](size_t index) {
        return static_cast<uint64_t>(0x7f0000001000 + 8 * index);
    });
}

inline Vector<uint8_t> encodeTwins(const TestSection& section)
{
    auto producerValues = producerValuesOf(section);
    return collect([&](const ImageSectionSink& sink) {
        return writeTwinsSection(section.twins.data, producerValues.span(), section.twins.token, sink);
    });
}

// Copies of the sections in allocations of exactly their sizes.
struct ExactSections {
    ExactSections(std::span<const uint8_t> imageBytes, std::span<const uint8_t> bakedFactsBytes, std::span<const uint8_t> twinsBytes)
        : image(imageBytes)
        , bakedFacts(bakedFactsBytes)
        , twins(twinsBytes)
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

// The body the valid sections are built from: a closure variable read and written, the four MathIC kinds, a dense, a
// list and a string switch, a regexp and a string constant, a function declaration and a function expression.
inline constexpr ASCIILiteral bodySource =
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
    "        return [a + b, a - b, a * b, -a, t.length, r, inner, e, captured];\n"
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
    uint32_t denseTable { 0 };
    uint32_t inlineStringTable { 0 };
    uint32_t inlineStringKeyCount { 0 };
};

inline std::optional<Body> makeBody(TestContext& context, VM& vm)
{
    auto* globalObject = createRealm(context, vm, bodySource, "the test body"_s);
    if (!globalObject)
        return std::nullopt;
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, "jitcacheImageSectionBody"_s));
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

    std::optional<uint32_t> add, sub, mul, negate, resolveScope, getFromScope, putToScope;
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
        default:
            break;
        }
    }

    std::optional<uint32_t> regExp, atom;
    auto& constants = unlinkedCodeBlock.constantRegisters();
    for (uint32_t index = 0; index < constants.size(); ++index) {
        JSValue value = constants[index].get();
        bool isOther = unlinkedCodeBlock.constantSourceCodeRepresentation(index) == SourceCodeRepresentation::Other;
        if (!value.isCell())
            continue;
        if (isOther && value.asCell()->inherits<RegExp>() && !regExp)
            regExp = index;
        if (isOther && value.isString() && !atom) {
            auto* impl = asString(value)->tryGetValueImpl();
            if (impl && impl->isAtom())
                atom = index;
        }
    }

    std::optional<uint32_t> denseTable, inlineStringTable;
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedSwitchJumpTables(); ++index) {
        if (!denseTable && !unlinkedCodeBlock.unlinkedSwitchJumpTable(index).isList())
            denseTable = index;
    }
    for (uint32_t index = 0; index < unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables(); ++index) {
        if (!inlineStringTable && hasInlineStringSwitch(unlinkedCodeBlock.unlinkedStringSwitchJumpTable(index).m_offsetTable.size()))
            inlineStringTable = index;
    }

    if (!add || !sub || !mul || !negate || !resolveScope || !getFromScope || !putToScope || !regExp || !atom
        || !denseTable || !inlineStringTable || body.instructionStarts.size() < 4
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
    body.denseTable = *denseTable;
    body.inlineStringTable = *inlineStringTable;
    body.inlineStringKeyCount = unlinkedCodeBlock.unlinkedStringSwitchJumpTable(*inlineStringTable).m_offsetTable.size();
    return body;
}

// The twin data a recording compilation of the body would leave, as the producer's JIT reads it: the compile-input
// snapshot of the body's CodeBlock. Its seeds and its regeneration log are the test's own: three switch seeds, so that
// their region has padding, and two regenerations of MathICs with inline code, one with two attaches, the second of
// which drew nothing, and one with a single attach, whose entry has padding.
inline TwinData twinDataOf(const Body& body)
{
    TwinData data;
    data.seeds.assembler = 0x5eed;
    data.seeds.binarySwitches = Vector<uint32_t> { 0x11, 0x22, 0x33 };
    data.compileInputs = snapshotCompileInputs(*body.codeBlock);

    auto anchor = CodeSymbol { anchorSymbol() };
    data.regenerations.append(TwinRegeneration { .mathICIndex = 0, .profileBitsAtEntry = 0x12, .replacement = anchor, .assemblerSeeds = { 0x44u, std::nullopt } });
    data.regenerations.append(TwinRegeneration { .mathICIndex = 1, .profileBitsAtEntry = 0, .replacement = anchor, .assemblerSeeds = { std::nullopt } });
    return data;
}

// Sections built from the body's UCB as capture writes them, with a fixup of every target kind, MathICs with and without
// inline code and a snippet, and padding after its code and its snippet.
inline TestSection makeValidSection(const Body& body)
{
    auto& unlinkedCodeBlock = *body.unlinkedCodeBlock;
    int64_t anchor = anchorSymbol();
    auto operation = makeTarget(TargetKind::Operation, 0, 0, anchor);

    TestCode code;
    code.fill(8);

    // MathIC 0, add: inline code whose start was rewritten to jump to its snippet.
    uint32_t inlineStart0 = code.offset();
    code.jump(makeTarget(TargetKind::SnippetEntry, 0));
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
    code.call(makeTarget(TargetKind::CommonThunk, 0));
    code.pointer(makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::VM)), 11);
    code.jump(makeTarget(TargetKind::CommonThunk, 1));
#if CPU(X86_64)
    code.jump(makeTarget(TargetKind::CommonThunk, 2), true);
#endif
    code.call(makeTarget(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::OpEnterHandler)));
    code.jump(makeTarget(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::OpThrowHandler)));
    code.call(makeTarget(TargetKind::SlowPathThunk, 0, 0, anchor));
    code.call(makeTarget(TargetKind::InlineCacheSlowPathThunk, static_cast<uint32_t>(AccessType::GetById)));
    code.call(makeTarget(TargetKind::VirtualCallThunk, static_cast<uint32_t>(CallMode::Regular)));
    code.pointer(makeTarget(TargetKind::ProcessThunk, static_cast<uint32_t>(ProcessThunk::DefaultCall)));
    code.call(makeTarget(TargetKind::ProcessThunk, static_cast<uint32_t>(ProcessThunk::ArityFixup)));
    code.pointer(makeTarget(TargetKind::VMCell, static_cast<uint32_t>(VMCell::EmptyString)));
    code.pointer(makeTarget(TargetKind::VMCell, static_cast<uint32_t>(VMCell::FastStringSentinel)));
    code.pointer(makeTarget(TargetKind::StructureIDBase));
    code.pointer(makeTarget(TargetKind::UCBConstantCell, body.regExpConstant));
    code.pointer(makeTarget(TargetKind::UCBConstantAtom, body.atomConstant));
    code.pointer(makeTarget(TargetKind::UCBIdentifier, 0));
    code.pointer(makeTarget(TargetKind::UCBBinaryArithProfile, 0));
    code.pointer(makeTarget(TargetKind::UCBUnaryArithProfile, 0));
    for (uint32_t rank = 0; rank < body.inlineStringKeyCount; ++rank) {
        code.pointer(makeTarget(TargetKind::SwitchStringRankAtom, body.inlineStringTable, rank));
        code.jump(makeTarget(TargetKind::SwitchStringRankCase, body.inlineStringTable, rank));
    }
    code.pointer(makeTarget(TargetKind::MathIC, 0));
    code.pointer(makeTarget(TargetKind::SwitchTableBase, body.denseTable));
    while (code.offset() % imageSectionAlignment != 4)
        code.fill(fillUnit);

    TestCode snippet;
    snippet.fill(4);
    snippet.jump(makeTarget(TargetKind::ImageOffset, inlineEnd0));
    snippet.pointer(makeTarget(TargetKind::UCBBinaryArithProfile, 0));
    snippet.jump(makeTarget(TargetKind::ImageOffset, slowPathStart0));
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

} // namespace JSC::JITCache::Tests::ImageSectionTestSupport

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
