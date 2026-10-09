#include "config.h"
#include "ImageSection.h"

#include "ImageTypes.h"

#if ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeConventions.h"
#include "ImageSupport.h"
#include "InlineCacheHandler.h"
#include "JSCInlines.h"
#include "JSTemplateObjectDescriptor.h"
#include "MacroAssembler.h"
#include "Opcode.h"
#include "Options.h"
#include "PropertyInlineCache.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include <cmath>
#include <limits>
#include <wtf/BitVector.h>

#endif // ENABLE(JIT)

namespace JSC::JITCache {

// Defined in every build: the integrator names image checks in its fault reports.
ASCIILiteral description(ImageCheck check)
{
    switch (check) {
    case ImageCheck::None:
        return "none"_s;
    case ImageCheck::V1:
        return "v1"_s;
    case ImageCheck::V2:
        return "v2"_s;
    case ImageCheck::V3:
        return "v3"_s;
    case ImageCheck::V4:
        return "v4"_s;
    case ImageCheck::V5:
        return "v5"_s;
    case ImageCheck::V6:
        return "v6"_s;
    case ImageCheck::V7:
        return "v7"_s;
    case ImageCheck::U1:
        return "u1"_s;
    case ImageCheck::U2:
        return "u2"_s;
    case ImageCheck::U3:
        return "u3"_s;
    case ImageCheck::U4:
        return "u4"_s;
    case ImageCheck::U5:
        return "u5"_s;
    case ImageCheck::U6:
        return "u6"_s;
    case ImageCheck::U7:
        return "u7"_s;
    case ImageCheck::W1:
        return "w1"_s;
    case ImageCheck::W2:
        return "w2"_s;
    case ImageCheck::W3:
        return "w3"_s;
    case ImageCheck::W4:
        return "w4"_s;
    case ImageCheck::S1:
        return "s1"_s;
    case ImageCheck::S2:
        return "s2"_s;
    case ImageCheck::S3:
        return "s3"_s;
    case ImageCheck::S4:
        return "s4"_s;
    case ImageCheck::S5:
        return "s5"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

#if ENABLE(JIT)

namespace ImageSectionInternal {

// Header fields (section 8.2).
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
static constexpr size_t fixupReservedOffset = 6;
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
static constexpr size_t mathICReservedOffset = 2;
static constexpr size_t mathICBytecodeOffsetOffset = 4;
static constexpr size_t mathICInlineStartOffset = 8;
static constexpr size_t mathICInlineEndOffset = 12;
static constexpr size_t mathICSlowPathStartOffset = 16;
static constexpr size_t mathICSlowPathCallOffset = 20;
static constexpr size_t mathICSnippetSizeOffset = 24;
static constexpr size_t mathICSnippetFixupCountOffset = 28;

static_assert(static_cast<uint32_t>(ImageConstantType::FunctionDecl) == static_cast<uint32_t>(JITConstantPool::Type::FunctionDecl));
static_assert(static_cast<uint32_t>(ImageConstantType::FunctionExpr) == static_cast<uint32_t>(JITConstantPool::Type::FunctionExpr));

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
    return ImageBytes::read<uint32_t>(bytes, index * instructionSize);
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

static ImageSectionHeader decodeHeader(std::span<const uint8_t> image)
{
    return ImageSectionHeader {
        .codeSize = ImageBytes::read<uint32_t>(image, codeSizeOffset),
        .arityEntryOffset = ImageBytes::read<uint32_t>(image, arityEntryOffsetOffset),
        .fixupCount = ImageBytes::read<uint32_t>(image, fixupCountOffset),
        .callCount = ImageBytes::read<uint32_t>(image, callCountOffset),
        .moldCount = ImageBytes::read<uint32_t>(image, moldCountOffset),
        .simpleSwitchTableCount = ImageBytes::read<uint32_t>(image, simpleSwitchTableCountOffset),
        .stringSwitchTableCount = ImageBytes::read<uint32_t>(image, stringSwitchTableCountOffset),
        .codeMapCount = ImageBytes::read<uint32_t>(image, codeMapCountOffset),
        .constantPoolCount = ImageBytes::read<uint32_t>(image, constantPoolCountOffset),
        .mathICCount = ImageBytes::read<uint32_t>(image, mathICCountOffset),
        .livenessRateBits = ImageBytes::read<uint64_t>(image, livenessRateOffset),
        .fullnessRateBits = ImageBytes::read<uint64_t>(image, fullnessRateOffset),
    };
}

static bool isValidCacheType(uint8_t value)
{
    // Without a default, the switch stops building when the engine adds a cache type.
    switch (static_cast<CacheType>(static_cast<int8_t>(value))) {
    case CacheType::Unset:
    case CacheType::GetByIdSelf:
    case CacheType::GetByIdPrototype:
    case CacheType::PutByIdReplace:
    case CacheType::InByIdSelf:
    case CacheType::Stub:
    case CacheType::ArrayLength:
    case CacheType::StringLength:
        return true;
    }
    return false;
}

enum class FixupPlace : bool { Image, Snippet };

// The rows of table 3.3 that need no UCB and no other region: the form each kind allows, the fields it uses (every other
// is zero), the range of the fields that name an enumeration and the validity of a CodeSymbol payload (V3).
static bool isValidTargetForForm(const ImageTarget& target, FixupForm form, FixupPlace place)
{
    bool usesA = false;
    bool usesB = false;
    bool usesPayload = false;
    bool formIsAllowed = false;
    bool fieldsAreValid = true;
    switch (target.kind) {
    case TargetKind::Operation:
        formIsAllowed = form == FixupForm::Pointer;
        usesPayload = true;
        fieldsAreValid = CodeSymbol::isValid(target.payload);
        break;
    case TargetKind::CommonThunk:
        formIsAllowed = form == FixupForm::Call || form == FixupForm::Jump;
        usesA = true;
        fieldsAreValid = isValidCommonThunk(target.a);
        break;
    case TargetKind::BaselineThunk:
        formIsAllowed = form == FixupForm::Call || form == FixupForm::Jump;
        usesA = true;
        fieldsAreValid = isValidBaselineThunk(target.a);
        break;
    case TargetKind::SlowPathThunk:
        formIsAllowed = form == FixupForm::Call;
        usesPayload = true;
        fieldsAreValid = CodeSymbol::isValid(target.payload);
        break;
    case TargetKind::InlineCacheSlowPathThunk:
        formIsAllowed = form == FixupForm::Call;
        usesA = true;
        fieldsAreValid = isValidAccessType(target.a);
        break;
    case TargetKind::VirtualCallThunk:
        formIsAllowed = form == FixupForm::Call;
        usesA = true;
        fieldsAreValid = isValidCallMode(target.a);
        break;
    case TargetKind::ProcessThunk:
        usesA = true;
        fieldsAreValid = isValidProcessThunk(target.a);
        formIsAllowed = static_cast<ProcessThunk>(target.a) == ProcessThunk::DefaultCall ? form == FixupForm::Pointer : form == FixupForm::Call;
        break;
    case TargetKind::VMAddress:
        formIsAllowed = form == FixupForm::Pointer;
        usesA = true;
        fieldsAreValid = isValidVMAddress(target.a);
        break;
    case TargetKind::VMCell:
        formIsAllowed = form == FixupForm::Pointer;
        usesA = true;
        fieldsAreValid = isValidVMCell(target.a);
        break;
    case TargetKind::StructureIDBase:
        formIsAllowed = form == FixupForm::Pointer;
        break;
    case TargetKind::UCBConstantCell:
    case TargetKind::UCBConstantAtom:
    case TargetKind::UCBIdentifier:
    case TargetKind::UCBBinaryArithProfile:
    case TargetKind::UCBUnaryArithProfile:
    case TargetKind::MathIC:
    case TargetKind::SwitchTableBase:
        // Their indexes are checked against the UCB (U3, U7) and against the section's other regions (V6).
        formIsAllowed = form == FixupForm::Pointer;
        usesA = true;
        break;
    case TargetKind::SwitchStringRankAtom:
        formIsAllowed = form == FixupForm::Pointer;
        usesA = true;
        usesB = true;
        break;
    case TargetKind::SwitchStringRankCase:
        formIsAllowed = form == FixupForm::Jump;
        usesA = true;
        usesB = true;
        break;
    case TargetKind::ImageOffset:
        formIsAllowed = form == FixupForm::Jump && place == FixupPlace::Snippet;
        usesA = true;
        break;
    case TargetKind::SnippetEntry:
        formIsAllowed = form == FixupForm::Jump && place == FixupPlace::Image;
        usesA = true;
        break;
    default:
        return false; // a kind byte outside table 3.3
    }
    if (!formIsAllowed || !fieldsAreValid)
        return false;
    return (usesA || !target.a) && (usesB || !target.b) && (usesPayload || !target.payload);
}

// V3 on one list of fixups and the code it describes.
static bool fixupsAreValid(const ImageFixupArray& fixups, std::span<const uint8_t> code, FixupPlace place)
{
    size_t previousEnd = 0;
    for (size_t index = 0; index < fixups.size(); ++index) {
        auto entry = fixups.entryBytes(index);
        uint8_t form = entry[fixupFormOffset];
        if (form < static_cast<uint8_t>(FixupForm::Pointer) || form > static_cast<uint8_t>(FixupForm::Jump))
            return false;
        if (ImageBytes::read<uint16_t>(entry, fixupReservedOffset))
            return false;
        auto fixup = fixups[index];
        // In footprint order: each footprint starts at or after the end of the one before it, inside the code.
        auto footprint = fixupFootprint(fixup.form, fixup.site, code);
        if (!footprint || footprint->begin < previousEnd)
            return false;
        if (!isCanonicalFootprint(fixup.form, code.subspan(footprint->begin, footprint->size())))
            return false;
        if (!isValidTargetForForm(fixup.target, fixup.form, place))
            return false;
        previousEnd = footprint->end;
    }
    return true;
}

static OpcodeID opcodeFor(MathICKind kind)
{
    switch (kind) {
    case MathICKind::Add:
        return op_add;
    case MathICKind::Sub:
        return op_sub;
    case MathICKind::Mul:
        return op_mul;
    case MathICKind::Negate:
        return op_negate;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static OpcodeID opcodeFor(ScopeOpcode opcode)
{
    switch (opcode) {
    case ScopeOpcode::ResolveScope:
        return op_resolve_scope;
    case ScopeOpcode::GetFromScope:
        return op_get_from_scope;
    case ScopeOpcode::PutToScope:
        return op_put_to_scope;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Applies functor to the image's fixups and then to each snippet's.
template<typename Functor>
static void forEachFixupList(const ImageSectionsView& view, const Functor& functor)
{
    functor(view.fixups());
    view.forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        functor(snippet.fixups);
    });
}

static bool passesU1(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    if (view.header().simpleSwitchTableCount != unlinkedCodeBlock.numberOfUnlinkedSwitchJumpTables())
        return false;
    bool valid = true;
    view.forEachSimpleSwitchTable([&](unsigned index, const ImageSimpleSwitchTable& table) {
        auto& unlinkedTable = unlinkedCodeBlock.unlinkedSwitchJumpTable(index);
        size_t expected = unlinkedTable.isList() ? 0 : unlinkedTable.m_branchOffsets.size();
        if (table.offsets.size() != expected)
            valid = false;
    });
    return valid;
}

static bool passesU2(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    if (view.header().stringSwitchTableCount != unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables())
        return false;
    bool valid = true;
    view.forEachStringSwitchTable([&](unsigned index, const ImageOffsetArray& offsets) {
        if (offsets.size() != unlinkedCodeBlock.unlinkedStringSwitchJumpTable(index).m_offsetTable.size() + 1)
            valid = false;
    });
    return valid;
}

static size_t constantCountOf(const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    // constantRegisters() has no const overload and reads nothing it could change.
    return const_cast<UnlinkedCodeBlock&>(unlinkedCodeBlock).constantRegisters().size();
}

static bool passesU3(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    size_t constantCount = constantCountOf(unlinkedCodeBlock);
    bool valid = true;
    forEachFixupList(view, [&](const ImageFixupArray& fixups) {
        for (size_t index = 0; index < fixups.size(); ++index) {
            auto target = fixups[index].target;
            switch (target.kind) {
            case TargetKind::UCBConstantCell:
            case TargetKind::UCBConstantAtom:
                valid &= target.a < constantCount;
                break;
            case TargetKind::UCBIdentifier:
                valid &= target.a < unlinkedCodeBlock.numberOfIdentifiers();
                break;
            case TargetKind::UCBBinaryArithProfile:
                valid &= target.a < unlinkedCodeBlock.numberOfBinaryArithProfiles();
                break;
            case TargetKind::UCBUnaryArithProfile:
                valid &= target.a < unlinkedCodeBlock.numberOfUnaryArithProfiles();
                break;
            default:
                break;
            }
        }
    });
    for (unsigned index = 0; index < view.header().moldCount; ++index) {
        auto mold = view.mold(index);
        if (mold.identifierKind == MoldIdentifierKind::UCBIdentifier)
            valid &= mold.identifier < unlinkedCodeBlock.numberOfIdentifiers();
    }
    for (unsigned index = 0; index < view.header().constantPoolCount; ++index) {
        auto constant = view.constantPoolEntry(index);
        size_t count = constant.type == ImageConstantType::FunctionDecl ? unlinkedCodeBlock.functionDecls().size() : unlinkedCodeBlock.functionExprs().size();
        valid &= constant.index < count;
    }
    return valid;
}

// The UCB's instruction starts, which U4 and U5 read.
struct InstructionStarts {
    BitVector starts;
    unsigned count { 0 };
};

static InstructionStarts instructionStartsOf(const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& instructions = unlinkedCodeBlock.instructions();
    InstructionStarts result { BitVector(instructions.size()), 0 };
    for (const auto& instruction : instructions) {
        result.starts.quickSet(instruction.offset());
        ++result.count;
    }
    return result;
}

static bool passesU4(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock, const InstructionStarts& instructionStarts)
{
    auto& instructions = unlinkedCodeBlock.instructions();
    auto isStart = [&](uint32_t offset) {
        return offset < instructions.size() && instructionStarts.starts.quickGet(offset);
    };
    // A checkpoint only where the instruction has one.
    auto isInstructionIndex = [&](BytecodeIndex index) {
        return isStart(index.offset()) && index.checkpoint() < instructions.at(index.offset())->numberOfCheckpoints();
    };
    for (unsigned index = 0; index < view.header().callCount; ++index) {
        if (!isInstructionIndex(view.call(index).bytecodeIndex))
            return false;
    }
    for (unsigned index = 0; index < view.header().moldCount; ++index) {
        if (!isInstructionIndex(view.mold(index).bytecodeIndex))
            return false;
    }
    for (unsigned index = 0; index < view.header().mathICCount; ++index) {
        auto entry = view.mathIC(index);
        if (!isStart(entry.bytecodeOffset) || instructions.at(entry.bytecodeOffset)->opcodeID() != opcodeFor(entry.kind))
            return false;
    }
    auto& bakedFacts = view.bakedFacts();
    for (unsigned index = 0; index < bakedFacts.scopeFactCount(); ++index) {
        auto fact = bakedFacts.scopeFact(index);
        if (!isStart(fact.bytecodeOffset) || instructions.at(fact.bytecodeOffset)->opcodeID() != opcodeFor(fact.opcode))
            return false;
    }
    return true;
}

static bool passesU6(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    struct RankCount {
        unsigned cases { 0 };
        bool hasAtom { false };
    };
    size_t tableCount = unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables();
    Vector<Vector<RankCount>> tables(tableCount);
    bool valid = true;
    forEachFixupList(view, [&](const ImageFixupArray& fixups) {
        for (size_t index = 0; index < fixups.size(); ++index) {
            auto target = fixups[index].target;
            if (target.kind != TargetKind::SwitchStringRankAtom && target.kind != TargetKind::SwitchStringRankCase)
                continue;
            // Table 3.3's bounds: a string table with an inline tree, and a rank below its key count.
            if (target.a >= tableCount) {
                valid = false;
                continue;
            }
            size_t keyCount = unlinkedCodeBlock.unlinkedStringSwitchJumpTable(target.a).m_offsetTable.size();
            if (!hasInlineStringSwitch(keyCount) || target.b >= keyCount) {
                valid = false;
                continue;
            }
            auto& ranks = tables[target.a];
            if (ranks.isEmpty())
                ranks.grow(keyCount);
            if (target.kind == TargetKind::SwitchStringRankCase)
                ++ranks[target.b].cases;
            else
                ranks[target.b].hasAtom = true;
        }
    });
    if (!valid)
        return false;
    // A table with rank fixups has exactly one case jump and at least one atom comparison per rank.
    for (auto& ranks : tables) {
        for (auto& rank : ranks) {
            if (rank.cases != 1 || !rank.hasAtom)
                return false;
        }
    }
    return true;
}

static bool passesU7(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto constantAt = [&](uint32_t index) {
        return unlinkedCodeBlock.getConstant(VirtualRegister(FirstConstantRegisterIndex + static_cast<int>(index)));
    };
    bool valid = true;
    forEachFixupList(view, [&](const ImageFixupArray& fixups) {
        for (size_t index = 0; index < fixups.size(); ++index) {
            auto target = fixups[index].target;
            if (target.kind == TargetKind::UCBConstantCell) {
                // Owned by the UCB, as CodeBlock::isConstantOwnedByUnlinkedCodeBlock decides, and a cell.
                if (unlinkedCodeBlock.constantSourceCodeRepresentation(target.a) != SourceCodeRepresentation::Other) {
                    valid = false;
                    continue;
                }
                JSValue value = constantAt(target.a);
                if (!value || !value.isCell() || value.asCell()->inherits<SymbolTable>() || value.asCell()->inherits<JSTemplateObjectDescriptor>())
                    valid = false;
            } else if (target.kind == TargetKind::UCBConstantAtom) {
                JSValue value = constantAt(target.a);
                if (!value.isString()) {
                    valid = false;
                    continue;
                }
                auto* impl = asString(value)->tryGetValueImpl();
                if (!impl || !impl->isAtom())
                    valid = false;
            }
        }
    });
    return valid;
}

static std::optional<ImageCheck> firstFailureAgainst(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    if (!passesU1(view, unlinkedCodeBlock))
        return ImageCheck::U1;
    if (!passesU2(view, unlinkedCodeBlock))
        return ImageCheck::U2;
    if (!passesU3(view, unlinkedCodeBlock))
        return ImageCheck::U3;
    auto instructionStarts = instructionStartsOf(unlinkedCodeBlock);
    if (!passesU4(view, unlinkedCodeBlock, instructionStarts))
        return ImageCheck::U4;
    if (view.header().codeMapCount != instructionStarts.count)
        return ImageCheck::U5;
    if (!passesU6(view, unlinkedCodeBlock))
        return ImageCheck::U6;
    if (!passesU7(view, unlinkedCodeBlock))
        return ImageCheck::U7;
    return std::nullopt;
}

} // namespace ImageSectionInternal

std::optional<FixupFootprint> fixupFootprint(FixupForm form, uint32_t site, std::span<const uint8_t> code)
{
    using namespace ImageSectionInternal;
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
    using namespace ImageSectionInternal;
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
            ImageBytes::write<uint32_t>(bytes, index * instructionSize, wordAt(bytes, index) & ~mask);
    }
#else
    UNUSED_PARAM(form);
    UNUSED_PARAM(bytes);
#endif
    return result;
}

bool isCanonicalFootprint(FixupForm form, std::span<const uint8_t> footprint)
{
    using namespace ImageSectionInternal;
#if CPU(X86_64)
    switch (form) {
    case FixupForm::Pointer:
        return footprint.size() == pointerFootprintSize
            && (footprint[0] == rexW || footprint[0] == rexWB)
            && footprint[1] >= movImm64First && footprint[1] <= movImm64Last
            && ImageBytes::isZero(footprint.subspan(2));
    case FixupForm::Call:
        return footprint.size() == callFootprintSize && footprint[0] == callRel32 && ImageBytes::isZero(footprint.subspan(1));
    case FixupForm::Jump:
        if (footprint.size() == jumpFootprintSize)
            return footprint[0] == jmpRel32 && ImageBytes::isZero(footprint.subspan(1));
        if (footprint.size() == conditionalJumpFootprintSize)
            return isConditionalJumpOpcode(footprint[0], footprint[1]) && ImageBytes::isZero(footprint.subspan(2));
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
    using namespace ImageSectionInternal;
#if CPU(X86_64)
    switch (form) {
    case FixupForm::Pointer:
        if (footprint.size() != pointerFootprintSize || (footprint[0] != rexW && footprint[0] != rexWB) || footprint[1] < movImm64First || footprint[1] > movImm64Last)
            return std::nullopt;
        return static_cast<uintptr_t>(ImageBytes::read<uint64_t>(footprint, 2));
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
        int32_t displacement = ImageBytes::read<int32_t>(footprint, opcodeBytes);
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

std::optional<uint32_t> farCallPointerSite(uint32_t callReturnOffset)
{
#if CPU(X86_64)
    constexpr uint32_t distance = REPATCH_OFFSET_CALL_R11;
#elif CPU(ARM64) && !CPU(ARM64E)
    // MacroAssemblerARM64::REPATCH_OFFSET_CALL_TO_POINTER, which is protected, from the public constant it derives from.
    constexpr uint32_t distance = (ARM64Assembler::NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS + 1) * ImageSectionInternal::instructionSize;
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
    return static_cast<uint64_t>(inlineStart) + ImageSectionInternal::jumpFootprintSize;
#else
    // The rewrite writes one b at the inline start; its site is the instruction.
    return inlineStart;
#endif
}

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
    ImageBytes::write<uint32_t>(bytes, codeSizeOffset, header.codeSize);
    ImageBytes::write<uint32_t>(bytes, arityEntryOffsetOffset, header.arityEntryOffset);
    ImageBytes::write<uint32_t>(bytes, fixupCountOffset, header.fixupCount);
    ImageBytes::write<uint32_t>(bytes, callCountOffset, header.callCount);
    ImageBytes::write<uint32_t>(bytes, moldCountOffset, header.moldCount);
    ImageBytes::write<uint32_t>(bytes, simpleSwitchTableCountOffset, header.simpleSwitchTableCount);
    ImageBytes::write<uint32_t>(bytes, stringSwitchTableCountOffset, header.stringSwitchTableCount);
    ImageBytes::write<uint32_t>(bytes, codeMapCountOffset, header.codeMapCount);
    ImageBytes::write<uint32_t>(bytes, constantPoolCountOffset, header.constantPoolCount);
    ImageBytes::write<uint32_t>(bytes, mathICCountOffset, header.mathICCount);
    ImageBytes::write<uint64_t>(bytes, livenessRateOffset, header.livenessRateBits);
    ImageBytes::write<uint64_t>(bytes, fullnessRateOffset, header.fullnessRateBits);
    return bytes;
}

std::array<uint8_t, imageFixupEntrySize> encodeImageFixup(const ImageFixup& fixup)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageFixupEntrySize> bytes { };
    ImageBytes::write<uint32_t>(bytes, fixupSiteOffset, fixup.site);
    bytes[fixupFormOffset] = static_cast<uint8_t>(fixup.form);
    bytes[fixupKindOffset] = static_cast<uint8_t>(fixup.target.kind);
    ImageBytes::write<uint32_t>(bytes, fixupAOffset, fixup.target.a);
    ImageBytes::write<uint32_t>(bytes, fixupBOffset, fixup.target.b);
    ImageBytes::write<int64_t>(bytes, fixupPayloadOffset, fixup.target.payload);
    return bytes;
}

std::array<uint8_t, imageCallEntrySize> encodeImageCall(const ImageCallEntry& call)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageCallEntrySize> bytes { };
    ImageBytes::write<uint32_t>(bytes, callBytecodeIndexOffset, call.bytecodeIndex.asBits());
    ImageBytes::write<uint32_t>(bytes, callDoneLocationOffset, call.doneLocation);
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
    ImageBytes::write<uint32_t>(bytes, moldIdentifierOffset, mold.identifier);
    ImageBytes::write<uint32_t>(bytes, moldBytecodeIndexOffset, mold.bytecodeIndex.asBits());
    ImageBytes::write<uint32_t>(bytes, moldDoneLocationOffset, mold.doneLocation);
    return bytes;
}

std::array<uint8_t, imageConstantPoolEntrySize> encodeImageConstantPoolEntry(const ImageConstantPoolEntry& constant)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageConstantPoolEntrySize> bytes { };
    ImageBytes::write<uint32_t>(bytes, constantTypeOffset, static_cast<uint32_t>(constant.type));
    ImageBytes::write<uint32_t>(bytes, constantIndexOffset, constant.index);
    return bytes;
}

std::array<uint8_t, imageMathICEntrySize> encodeImageMathIC(const ImageMathICEntry& mathIC)
{
    using namespace ImageSectionInternal;
    std::array<uint8_t, imageMathICEntrySize> bytes { };
    bytes[mathICKindOffset] = static_cast<uint8_t>(mathIC.kind);
    bytes[mathICFlagsOffset] = mathIC.flags;
    ImageBytes::write<uint32_t>(bytes, mathICBytecodeOffsetOffset, mathIC.bytecodeOffset);
    ImageBytes::write<uint32_t>(bytes, mathICInlineStartOffset, mathIC.inlineStart);
    ImageBytes::write<uint32_t>(bytes, mathICInlineEndOffset, mathIC.inlineEnd);
    ImageBytes::write<uint32_t>(bytes, mathICSlowPathStartOffset, mathIC.slowPathStart);
    ImageBytes::write<uint32_t>(bytes, mathICSlowPathCallOffset, mathIC.slowPathCall);
    ImageBytes::write<uint32_t>(bytes, mathICSnippetSizeOffset, mathIC.snippetSize);
    ImageBytes::write<uint32_t>(bytes, mathICSnippetFixupCountOffset, mathIC.snippetFixupCount);
    return bytes;
}

std::array<uint8_t, sizeof(uint32_t)> encodeImageOffset(uint32_t offset)
{
    std::array<uint8_t, sizeof(uint32_t)> bytes { };
    ImageBytes::write<uint32_t>(bytes, 0, offset);
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
            // Finishing checked every footprint and its order before the record became Complete (section 4.7).
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
        .site = ImageBytes::read<uint32_t>(entry, fixupSiteOffset),
        .form = static_cast<FixupForm>(entry[fixupFormOffset]),
        .target = ImageTarget {
            .kind = static_cast<TargetKind>(entry[fixupKindOffset]),
            .a = ImageBytes::read<uint32_t>(entry, fixupAOffset),
            .b = ImageBytes::read<uint32_t>(entry, fixupBOffset),
            .payload = ImageBytes::read<int64_t>(entry, fixupPayloadOffset),
        },
    };
}

std::optional<ImageFixup> ImageFixupArray::find(uint32_t site, FixupForm form) const
{
    using namespace ImageSectionInternal;
    auto siteAt = [&](size_t index) {
        return ImageBytes::read<uint32_t>(entryBytes(index), fixupSiteOffset);
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
        .bytecodeIndex = BytecodeIndex::fromBits(ImageBytes::read<uint32_t>(entry, callBytecodeIndexOffset)),
        .doneLocation = ImageBytes::read<uint32_t>(entry, callDoneLocationOffset),
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
        .identifier = ImageBytes::read<uint32_t>(entry, moldIdentifierOffset),
        .bytecodeIndex = BytecodeIndex::fromBits(ImageBytes::read<uint32_t>(entry, moldBytecodeIndexOffset)),
        .doneLocation = ImageBytes::read<uint32_t>(entry, moldDoneLocationOffset),
    };
}

ImageConstantPoolEntry ImageSectionsView::constantPoolEntry(unsigned index) const
{
    using namespace ImageSectionInternal;
    ASSERT(index < m_header.constantPoolCount);
    auto entry = m_image.subspan(m_layout.constantPool + static_cast<size_t>(index) * imageConstantPoolEntrySize, imageConstantPoolEntrySize);
    return ImageConstantPoolEntry {
        .type = static_cast<ImageConstantType>(ImageBytes::read<uint32_t>(entry, constantTypeOffset)),
        .index = ImageBytes::read<uint32_t>(entry, constantIndexOffset),
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
        .bytecodeOffset = ImageBytes::read<uint32_t>(entry, mathICBytecodeOffsetOffset),
        .inlineStart = ImageBytes::read<uint32_t>(entry, mathICInlineStartOffset),
        .inlineEnd = ImageBytes::read<uint32_t>(entry, mathICInlineEndOffset),
        .slowPathStart = ImageBytes::read<uint32_t>(entry, mathICSlowPathStartOffset),
        .slowPathCall = ImageBytes::read<uint32_t>(entry, mathICSlowPathCallOffset),
        .snippetSize = ImageBytes::read<uint32_t>(entry, mathICSnippetSizeOffset),
        .snippetFixupCount = ImageBytes::read<uint32_t>(entry, mathICSnippetFixupCountOffset),
    };
}

bool operator==(const ImageSectionsView& a, const ImageSectionsView& b)
{
    bool equal = a.m_image.data() == b.m_image.data() && a.m_image.size() == b.m_image.size()
        && a.m_header == b.m_header && a.m_layout == b.m_layout && a.m_bakedFacts == b.m_bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    equal = equal && a.m_twins.data() == b.m_twins.data() && a.m_twins.size() == b.m_twins.size();
#endif
    return equal;
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
        uint32_t entryCount = ImageBytes::read<uint32_t>(image, offset - sizeof(uint32_t));
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
        uint32_t entryCount = ImageBytes::read<uint32_t>(image, offset - sizeof(uint32_t));
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

    // A snippet follows for each MathIC whose flags say it has one; V5 checks the flags against the other fields.
    layout.snippets = offset;
    for (uint32_t index = 0; index < header.mathICCount; ++index) {
        auto entry = image.subspan(layout.mathICs + static_cast<size_t>(index) * imageMathICEntrySize, imageMathICEntrySize);
        if (!(entry[mathICFlagsOffset] & ImageMathICFlags::hasSnippet))
            continue;
        uint32_t snippetSize = ImageBytes::read<uint32_t>(entry, mathICSnippetSizeOffset);
        uint32_t snippetFixupCount = ImageBytes::read<uint32_t>(entry, mathICSnippetFixupCountOffset);
        if (!advance(snippetSize) || !pad() || !advance(static_cast<uint64_t>(snippetFixupCount) * imageFixupEntrySize))
            return std::nullopt;
    }
    layout.end = offset;
    return layout;
}

std::optional<ImageCheck> ImageSectionsView::firstStructureFailure() const
{
    if (!passesV1())
        return ImageCheck::V1;
    if (!passesV2())
        return ImageCheck::V2;
    if (!passesV3())
        return ImageCheck::V3;
    if (!passesV4())
        return ImageCheck::V4;
    if (!passesV5())
        return ImageCheck::V5;
    if (!passesV6())
        return ImageCheck::V6;
    return std::nullopt;
}

bool ImageSectionsView::passesV1() const
{
    // Both coverage rates are the ratios CodeBlock::shouldOptimizeNowFromBaseline computes.
    auto isRate = [](double rate) {
        return std::isfinite(rate) && rate >= 0 && rate <= 1;
    };
    if (!isRate(m_header.livenessRate()) || !isRate(m_header.fullnessRate()))
        return false;
    if (m_layout.end != m_image.size())
        return false;

    auto isZeroBetween = [&](size_t begin, size_t end) {
        return ImageBytes::isZero(m_image.subspan(begin, end - begin));
    };
    if (!isZeroBetween(m_layout.code + m_header.codeSize, m_layout.fixups)
        || !isZeroBetween(m_layout.simpleSwitchTablesEnd, m_layout.stringSwitchTables)
        || !isZeroBetween(m_layout.stringSwitchTablesEnd, m_layout.codeMap)
        || !isZeroBetween(m_layout.codeMap + static_cast<size_t>(m_header.codeMapCount) * sizeof(uint32_t), m_layout.constantPool))
        return false;

    size_t offset = m_layout.snippets;
    for (unsigned index = 0; index < m_header.mathICCount; ++index) {
        auto entry = mathIC(index);
        if (!entry.hasSnippet())
            continue;
        size_t codeEnd = offset + entry.snippetSize;
        size_t fixupsStart = alignImageSectionOffset(codeEnd);
        if (!isZeroBetween(codeEnd, fixupsStart))
            return false;
        offset = fixupsStart + static_cast<size_t>(entry.snippetFixupCount) * imageFixupEntrySize;
    }
    return true;
}

bool ImageSectionsView::passesV2() const
{
    if (!m_header.codeSize || m_header.arityEntryOffset >= m_header.codeSize)
        return false;
#if CPU(ARM64)
    // Every code offset names a four-byte instruction boundary.
    auto aligned = [](uint64_t offset) {
        return !(offset % 4);
    };
    bool allAligned = aligned(m_header.codeSize) && aligned(m_header.arityEntryOffset);
    // A fixup's site is an offset into its code, and an ImageOffset target an offset into the image.
    auto checkFixups = [&](const ImageFixupArray& fixups) {
        for (size_t index = 0; index < fixups.size(); ++index) {
            auto fixup = fixups[index];
            allAligned &= aligned(fixup.site);
            if (fixup.target.kind == TargetKind::ImageOffset)
                allAligned &= aligned(fixup.target.a);
        }
    };
    checkFixups(fixups());
    for (unsigned index = 0; index < m_header.callCount; ++index)
        allAligned &= aligned(call(index).doneLocation);
    for (unsigned index = 0; index < m_header.moldCount; ++index)
        allAligned &= aligned(mold(index).doneLocation);
    forEachSimpleSwitchTable([&](unsigned, const ImageSimpleSwitchTable& table) {
        allAligned &= aligned(table.defaultOffset);
        for (size_t index = 0; index < table.offsets.size(); ++index)
            allAligned &= aligned(table.offsets[index]);
    });
    forEachStringSwitchTable([&](unsigned, const ImageOffsetArray& offsets) {
        for (size_t index = 0; index < offsets.size(); ++index)
            allAligned &= aligned(offsets[index]);
    });
    auto codeMap = this->codeMap();
    for (size_t index = 0; index < codeMap.size(); ++index)
        allAligned &= aligned(codeMap[index]);
    for (unsigned index = 0; index < m_header.mathICCount; ++index) {
        auto entry = mathIC(index);
        allAligned &= aligned(entry.inlineStart) && aligned(entry.inlineEnd) && aligned(entry.slowPathStart) && aligned(entry.slowPathCall) && aligned(entry.snippetSize);
    }
    forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        checkFixups(snippet.fixups);
    });
    return allAligned;
#else
    return true;
#endif
}

bool ImageSectionsView::passesV3() const
{
    using namespace ImageSectionInternal;
    if (!fixupsAreValid(fixups(), code(), FixupPlace::Image))
        return false;
    bool valid = true;
    forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        valid = valid && fixupsAreValid(snippet.fixups, snippet.code, FixupPlace::Snippet);
    });
    return valid;
}

bool ImageSectionsView::passesV4() const
{
    using namespace ImageSectionInternal;
    // Code offsets lie within the code, its end included, as capture's own range check does (section 9, step 2).
    uint32_t codeSize = m_header.codeSize;
    auto inCode = [&](uint32_t offset) {
        return offset <= codeSize;
    };

    // Calls in the order JIT::link sorted them.
    for (unsigned index = 0; index < m_header.callCount; ++index) {
        auto entry = call(index);
        if (index && entry.bytecodeIndex.asBits() < call(index - 1).bytecodeIndex.asBits())
            return false;
        if (!inCode(entry.doneLocation))
            return false;
    }

    for (unsigned index = 0; index < m_header.moldCount; ++index) {
        auto entry = m_image.subspan(m_layout.molds + static_cast<size_t>(index) * imageMoldEntrySize, imageMoldEntrySize);
        if (!isValidAccessType(entry[moldAccessTypeOffset]) || !isValidCacheType(entry[moldCacheTypeOffset]))
            return false;
        if (entry[moldFlagsOffset] & ~ImageMoldFlags::all)
            return false;
        uint32_t identifier = ImageBytes::read<uint32_t>(entry, moldIdentifierOffset);
        switch (entry[moldIdentifierKindOffset]) {
        case static_cast<uint8_t>(MoldIdentifierKind::None):
            if (identifier)
                return false;
            break;
        case static_cast<uint8_t>(MoldIdentifierKind::UCBIdentifier):
            break; // U3 bounds the index.
        case static_cast<uint8_t>(MoldIdentifierKind::ImmortalName):
            if (identifier < 1 || identifier > numberOfImmortalNames)
                return false;
            break;
        default:
            return false;
        }
        if (!inCode(ImageBytes::read<uint32_t>(entry, moldDoneLocationOffset)))
            return false;
    }

    bool valid = true;
    forEachSimpleSwitchTable([&](unsigned, const ImageSimpleSwitchTable& table) {
        valid &= inCode(table.defaultOffset);
        for (size_t index = 0; index < table.offsets.size(); ++index)
            valid &= inCode(table.offsets[index]);
    });
    forEachStringSwitchTable([&](unsigned, const ImageOffsetArray& offsets) {
        for (size_t index = 0; index < offsets.size(); ++index)
            valid &= inCode(offsets[index]);
    });
    if (!valid)
        return false;

    // The main pass emits instructions in bytecode order, so the code map's offsets never decrease.
    auto codeMap = this->codeMap();
    for (size_t index = 0; index < codeMap.size(); ++index) {
        if (!inCode(codeMap[index]) || (index && codeMap[index] < codeMap[index - 1]))
            return false;
    }

    for (unsigned index = 0; index < m_header.constantPoolCount; ++index) {
        auto type = constantPoolEntry(index).type;
        if (type != ImageConstantType::FunctionDecl && type != ImageConstantType::FunctionExpr)
            return false;
    }
    return true;
}

bool ImageSectionsView::passesV5() const
{
    using namespace ImageSectionInternal;
    uint32_t codeSize = m_header.codeSize;
    auto imageFixups = fixups();

    for (unsigned index = 0; index < m_header.mathICCount; ++index) {
        auto raw = m_image.subspan(m_layout.mathICs + static_cast<size_t>(index) * imageMathICEntrySize, imageMathICEntrySize);
        uint8_t kind = raw[mathICKindOffset];
        if (kind < static_cast<uint8_t>(MathICKind::Add) || kind > static_cast<uint8_t>(MathICKind::Negate))
            return false;
        if ((raw[mathICFlagsOffset] & ~ImageMathICFlags::all) || ImageBytes::read<uint16_t>(raw, mathICReservedOffset))
            return false;

        auto entry = mathIC(index);
        if (!entry.hasInlineCode()) {
            // The record lists it, and native emission left it with null locations, no snippet and no slow call.
            if (entry.flags != ImageMathICFlags::noInlineCode)
                return false;
            if (entry.inlineStart || entry.inlineEnd || entry.slowPathStart || entry.slowPathCall || entry.snippetSize || entry.snippetFixupCount)
                return false;
            continue;
        }

        if (!(entry.inlineStart < entry.inlineEnd && entry.inlineEnd <= codeSize))
            return false;
        if (entry.slowPathStart > codeSize || entry.slowPathCall > codeSize)
            return false;
        // The slow call's Operation fixup (I7).
        auto slowCallSite = farCallPointerSite(entry.slowPathCall);
        if (!slowCallSite)
            return false;
        auto slowCall = imageFixups.find(*slowCallSite, FixupForm::Pointer);
        if (!slowCall || slowCall->target.kind != TargetKind::Operation)
            return false;

        if (!entry.hasSnippet()) {
            if (entry.snippetSize || entry.snippetFixupCount)
                return false;
            continue;
        }
        // A snippet holds at least its jumps back to the image, so it is never empty.
        if (!entry.snippetSize)
            return false;
        uint64_t entrySite = snippetEntrySite(entry.inlineStart);
        if (entrySite > std::numeric_limits<uint32_t>::max())
            return false;
        auto snippetEntry = imageFixups.find(static_cast<uint32_t>(entrySite), FixupForm::Jump);
        if (!snippetEntry || snippetEntry->target.kind != TargetKind::SnippetEntry || snippetEntry->target.a != index)
            return false;
    }

    // Every SnippetEntry fixup names a MathIC with a snippet, at the jump site of its inline start; since no two jumps
    // share a site, each such MathIC has exactly one.
    for (size_t index = 0; index < imageFixups.size(); ++index) {
        auto fixup = imageFixups[index];
        if (fixup.target.kind != TargetKind::SnippetEntry)
            continue;
        if (fixup.target.a >= m_header.mathICCount)
            return false;
        auto entry = mathIC(fixup.target.a);
        if (!entry.hasInlineCode() || !entry.hasSnippet() || snippetEntrySite(entry.inlineStart) != fixup.site)
            return false;
    }

    // A snippet jumps back only to its MathIC's inline end or slow path start.
    bool valid = true;
    forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        auto entry = mathIC(index);
        for (size_t fixupIndex = 0; fixupIndex < snippet.fixups.size(); ++fixupIndex) {
            auto target = snippet.fixups[fixupIndex].target;
            if (target.kind == TargetKind::ImageOffset && target.a != entry.inlineEnd && target.a != entry.slowPathStart)
                valid = false;
        }
    });
    return valid;
}

bool ImageSectionsView::passesV6() const
{
    // Only JIT::emitMathICSlow passes an IC's address, so a MathIC target names a MathIC with inline code; a switch table
    // base names a dense table.
    Vector<uint32_t> simpleEntryCounts;
    simpleEntryCounts.reserveInitialCapacity(m_header.simpleSwitchTableCount);
    forEachSimpleSwitchTable([&](unsigned, const ImageSimpleSwitchTable& table) {
        simpleEntryCounts.append(static_cast<uint32_t>(table.offsets.size()));
    });
    bool valid = true;
    ImageSectionInternal::forEachFixupList(*this, [&](const ImageFixupArray& fixups) {
        for (size_t index = 0; index < fixups.size(); ++index) {
            auto target = fixups[index].target;
            if (target.kind == TargetKind::MathIC)
                valid &= target.a < m_header.mathICCount && mathIC(target.a).hasInlineCode();
            else if (target.kind == TargetKind::SwitchTableBase)
                valid &= target.a < simpleEntryCounts.size() && simpleEntryCounts[target.a] > 0;
        }
    });
    return valid;
}

std::expected<ImageSectionsView, ImageCheck> parseImageSections(const ImageSectionSpans& spans, bool strict)
{
    if (spans.image.size() < imageSectionHeaderSize)
        return std::unexpected(ImageCheck::V1);
    auto header = ImageSectionInternal::decodeHeader(spans.image);
    auto layout = ImageSectionsView::locate(spans.image, header);
    if (!layout)
        return std::unexpected(ImageCheck::V1);

    ImageSectionsView view(spans.image, header, *layout);
    if (strict) {
        if (auto failure = view.firstStructureFailure())
            return std::unexpected(*failure);
    } else
        ASSERT(!view.firstStructureFailure());

    auto bakedFacts = parseBakedFactsSection(spans.bakedFacts, strict);
    if (!bakedFacts)
        return std::unexpected(bakedFacts.error());
    view.m_bakedFacts = *bakedFacts;
#if ENABLE(JITCACHE_TWINS)
    view.m_twins = spans.twins;
#endif
    return view;
}

std::expected<void, ImageCheck> validateImageSectionsAgainst(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    if (auto failure = ImageSectionInternal::firstFailureAgainst(view, unlinkedCodeBlock))
        return std::unexpected(*failure);
    return { };
}

void assertImageSectionsAgainst(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
#if ASSERT_ENABLED
    ASSERT(!ImageSectionInternal::firstFailureAgainst(view, unlinkedCodeBlock));
#else
    UNUSED_PARAM(view);
    UNUSED_PARAM(unlinkedCodeBlock);
#endif
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache
