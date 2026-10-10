#include "config.h"
#include "ImageTwins.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BaselineJITPlan.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "DeferGC.h"
#include "EvalCodeBlock.h"
#include "EvalExecutable.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "GetByIdMetadata.h"
#include "ImageRecord.h"
#include "ImageSection.h"
#include "JIT.h"
#include "JITCacheBench.h"
#include "JITCacheVMState.h"
#include "JITCodeMap.h"
#include "JITMathIC.h"
#include "JSCInlines.h"
#include "JSPropertyNameEnumerator.h"
#include "JSTemplateObjectDescriptor.h"
#include "ModuleProgramCodeBlock.h"
#include "ModuleProgramExecutable.h"
#include "Options.h"
#include "ProducerBudget.h"
#include "ProgramCodeBlock.h"
#include "ProgramExecutable.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "TopExceptionScope.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

#if OS(LINUX)
#include <link.h>
#endif

namespace JSC::JITCache {

namespace ImageTwinsInternal {

// Header fields (section 11.2).
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

// Compile input entry: u32 bytecode offset, u8 kind, u8 value, u16 reserved, u32 local scope depth.
static constexpr size_t inputBytecodeOffsetOffset = 0;
static constexpr size_t inputKindOffset = 4;
static constexpr size_t inputValueOffset = 5;
static constexpr size_t inputReservedOffset = 6;
static constexpr size_t inputDepthOffset = 8;

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
static constexpr unsigned numberOfRelocationDomains = static_cast<unsigned>(RelocationDomain::Heap) + 1;

static constexpr uint8_t enumeratorFlagBits = JSPropertyNameEnumerator::IndexedMode | JSPropertyNameEnumerator::OwnStructureMode
    | JSPropertyNameEnumerator::GenericMode | JSPropertyNameEnumerator::HasSeenOwnStructureModeStructureMismatch;

static ImageTwinsHeader decodeHeader(std::span<const uint8_t> bytes)
{
    ImageTwinsHeader header {
        .flags = bytes[flagsOffset],
        .assemblerSeed = ImageBytes::read<uint32_t>(bytes, assemblerSeedOffset),
        .binarySwitchSeedCount = ImageBytes::read<uint32_t>(bytes, binarySwitchSeedCountOffset),
        .inputCount = ImageBytes::read<uint32_t>(bytes, inputCountOffset),
        .binaryArithProfileCount = ImageBytes::read<uint32_t>(bytes, binaryArithProfileCountOffset),
        .unaryArithProfileCount = ImageBytes::read<uint32_t>(bytes, unaryArithProfileCountOffset),
        .regenerationCount = ImageBytes::read<uint32_t>(bytes, regenerationCountOffset),
        .producerValueCount = ImageBytes::read<uint32_t>(bytes, producerValueCountOffset),
    };
    memcpySpan(std::span { header.captureProcessToken }, bytes.subspan(tokenOffset, captureProcessTokenSize));
    return header;
}

static std::array<uint8_t, imageTwinsHeaderSize> encodeHeader(const ImageTwinsHeader& header)
{
    std::array<uint8_t, imageTwinsHeaderSize> bytes { };
    bytes[flagsOffset] = header.flags;
    ImageBytes::write<uint32_t>(bytes, assemblerSeedOffset, header.assemblerSeed);
    ImageBytes::write<uint32_t>(bytes, binarySwitchSeedCountOffset, header.binarySwitchSeedCount);
    ImageBytes::write<uint32_t>(bytes, inputCountOffset, header.inputCount);
    ImageBytes::write<uint32_t>(bytes, binaryArithProfileCountOffset, header.binaryArithProfileCount);
    ImageBytes::write<uint32_t>(bytes, unaryArithProfileCountOffset, header.unaryArithProfileCount);
    ImageBytes::write<uint32_t>(bytes, regenerationCountOffset, header.regenerationCount);
    ImageBytes::write<uint32_t>(bytes, producerValueCountOffset, header.producerValueCount);
    memcpySpan(std::span { bytes }.subspan(tokenOffset, captureProcessTokenSize), std::span<const uint8_t> { header.captureProcessToken });
    return bytes;
}

static std::array<uint8_t, imageTwinsCompileInputEntrySize> encodeCompileInput(const CompileInput& input)
{
    std::array<uint8_t, imageTwinsCompileInputEntrySize> bytes { };
    ImageBytes::write<uint32_t>(bytes, inputBytecodeOffsetOffset, input.bytecodeOffset);
    bytes[inputKindOffset] = static_cast<uint8_t>(input.kind);
    bytes[inputValueOffset] = input.value;
    ImageBytes::write<uint32_t>(bytes, inputDepthOffset, input.localScopeDepth);
    return bytes;
}

template<typename Value>
static std::array<uint8_t, sizeof(Value)> encodeValue(Value value)
{
    std::array<uint8_t, sizeof(Value)> bytes { };
    ImageBytes::write<Value>(bytes, 0, value);
    return bytes;
}

// The kind of the input snapshotCompileInputs takes at an instruction with this opcode, if any: what the emitters read
// from mutable metadata (section 11.1).
static std::optional<CompileInputKind> snapshotKindOf(OpcodeID opcode)
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

static bool isValidInputValue(CompileInputKind kind, uint8_t value)
{
    switch (kind) {
    case CompileInputKind::ResolveScopeType:
    case CompileInputKind::GetFromScopeType:
    case CompileInputKind::PutToScopeType:
        return value <= static_cast<uint8_t>(Dynamic);
    case CompileInputKind::GetByIdMode:
    case CompileInputKind::IteratorOpenMode:
    case CompileInputKind::AsyncIteratorOpenMode:
        return value <= static_cast<uint8_t>(GetByIdMode::ArrayLength);
    case CompileInputKind::EnumeratorMetadata:
        return !(value & ~enumeratorFlagBits);
    case CompileInputKind::StrictEqualityAtomOperand:
        return value <= static_cast<uint8_t>(StrictEqualityAtomOperand::Rhs);
    }
    return false;
}

// CodeBlock::isConstantOwnedByUnlinkedCodeBlock, which reads only the UCB, so W4 can ask it before any CB exists.
static bool isConstantOwnedByUnlinkedCodeBlock(const UnlinkedCodeBlock& unlinkedCodeBlock, VirtualRegister reg)
{
    switch (unlinkedCodeBlock.constantSourceCodeRepresentation(reg)) {
    case SourceCodeRepresentation::Integer:
    case SourceCodeRepresentation::Double:
        return true;
    case SourceCodeRepresentation::Other: {
        JSValue value = unlinkedCodeBlock.getConstant(reg);
        if (!value || !value.isCell())
            return true;
        JSCell* cell = value.asCell();
        return !cell->inherits<SymbolTable>() && !cell->inherits<JSTemplateObjectDescriptor>();
    }
    case SourceCodeRepresentation::LinkTimeConstant:
        return false;
    }
    return false;
}

// The strict-equality templates' tryGetBitwiseComparableConstant: an operand that answers before the atom test (N25).
static bool isBitwiseComparableConstant(const UnlinkedCodeBlock& unlinkedCodeBlock, VirtualRegister reg)
{
    if (!reg.isConstant() || !isConstantOwnedByUnlinkedCodeBlock(unlinkedCodeBlock, reg))
        return false;
    JSValue value = unlinkedCodeBlock.getConstant(reg);
    return value.isUndefinedOrNull() || value.isBoolean();
}

// Their tryGetAtomStringConstant: an operand the fast path compares by its atom's address.
static bool isAtomStringConstant(const UnlinkedCodeBlock& unlinkedCodeBlock, VirtualRegister reg)
{
    if (!reg.isConstant() || !isConstantOwnedByUnlinkedCodeBlock(unlinkedCodeBlock, reg))
        return false;
    JSValue value = unlinkedCodeBlock.getConstant(reg);
    if (!value.isString())
        return false;
    auto* impl = asString(value)->tryGetValueImpl();
    return impl && impl->isAtom();
}

template<typename Op>
static std::pair<VirtualRegister, VirtualRegister> operandsOf(const JSInstruction* instruction)
{
    auto bytecode = instruction->as<Op>();
    return { bytecode.m_lhs, bytecode.m_rhs };
}

// The left and right operands of stricteq, nstricteq, jstricteq and jnstricteq; nullopt for any other opcode.
static std::optional<std::pair<VirtualRegister, VirtualRegister>> strictEqualityOperands(const JSInstruction* instruction)
{
    switch (instruction->opcodeID()) {
    case op_stricteq:
        return operandsOf<OpStricteq>(instruction);
    case op_nstricteq:
        return operandsOf<OpNstricteq>(instruction);
    case op_jstricteq:
        return operandsOf<OpJstricteq>(instruction);
    case op_jnstricteq:
        return operandsOf<OpJnstricteq>(instruction);
    default:
        return std::nullopt;
    }
}

// The kind of the one input W4 requires at an instruction, or nullopt where it allows none: the snapshot's kinds, and
// kind 8 at every strict equality whose template reaches its atom test.
static std::optional<CompileInputKind> expectedInputKind(const UnlinkedCodeBlock& unlinkedCodeBlock, const JSInstruction* instruction)
{
    if (auto kind = snapshotKindOf(instruction->opcodeID()))
        return kind;
    auto operands = strictEqualityOperands(instruction);
    if (!operands)
        return std::nullopt;
    if (isBitwiseComparableConstant(unlinkedCodeBlock, operands->first) || isBitwiseComparableConstant(unlinkedCodeBlock, operands->second))
        return std::nullopt;
    return CompileInputKind::StrictEqualityAtomOperand;
}

// Every object JITCache can tell loaded at its link-time address, and the object holding the engine, read once per
// process as ImageSupport reads the text segment (section 11.4). Only Linux targets import, so elsewhere both are empty.
struct LoadedSegment {
    uintptr_t start { 0 };
    uintptr_t end { 0 };
};

struct LoadedObjects {
    Vector<LoadedSegment> atLinkTimeAddress; // the PT_LOAD segments of every object whose load bias is zero
    Vector<LoadedSegment> engine; // the PT_LOAD segments of the object holding codeSymbolAnchor
};

static const LoadedObjects& loadedObjects()
{
    static LazyNeverDestroyed<LoadedObjects> objects;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        objects.construct();
#if OS(LINUX)
        struct Search {
            uintptr_t anchor;
            LoadedObjects* objects;
        } search { reinterpret_cast<uintptr_t>(&codeSymbolAnchor), &objects.get() };
        dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
            auto& search = *static_cast<Search*>(data);
            Vector<LoadedSegment> segments;
            bool holdsAnchor = false;
            for (auto& header : unsafeMakeSpan(info->dlpi_phdr, info->dlpi_phnum)) {
                if (header.p_type != PT_LOAD)
                    continue;
                LoadedSegment segment { info->dlpi_addr + header.p_vaddr, info->dlpi_addr + header.p_vaddr + header.p_memsz };
                holdsAnchor = holdsAnchor || (search.anchor >= segment.start && search.anchor < segment.end);
                segments.append(segment);
            }
            if (!info->dlpi_addr)
                search.objects->atLinkTimeAddress.appendVector(segments);
            if (holdsAnchor)
                search.objects->engine.appendVector(segments);
            return 0;
        }, &search);
#endif
    });
    return objects.get();
}

static bool liesIn(const Vector<LoadedSegment>& segments, uintptr_t address)
{
    return std::ranges::any_of(segments, [&](const LoadedSegment& segment) {
        return address >= segment.start && address < segment.end;
    });
}

// The relocation domain a target's resolution lies in (section 11.4), or nullopt for an artifact target, whose addresses
// are the two processes' own allocations and which the decodes compare by logical target instead.
static std::optional<RelocationDomain> relocationDomainOf(TargetKind kind, uintptr_t resolution)
{
    switch (kind) {
    case TargetKind::Operation:
        return RelocationDomain::EngineImage;
    case TargetKind::CommonThunk:
    case TargetKind::BaselineThunk:
    case TargetKind::SlowPathThunk:
    case TargetKind::InlineCacheSlowPathThunk:
    case TargetKind::VirtualCallThunk:
    case TargetKind::ProcessThunk:
        return RelocationDomain::ExecutablePool;
    case TargetKind::StructureIDBase:
        return RelocationDomain::StructureReservation;
    case TargetKind::UCBConstantAtom:
    case TargetKind::UCBIdentifier:
    case TargetKind::SwitchStringRankAtom:
        // A static StringImpl, such as the empty string's, is data of the engine image (N24).
        return liesIn(loadedObjects().engine, resolution) ? RelocationDomain::EngineImage : RelocationDomain::Heap;
    case TargetKind::VMAddress:
    case TargetKind::VMCell:
    case TargetKind::UCBConstantCell:
    case TargetKind::UCBBinaryArithProfile:
    case TargetKind::UCBUnaryArithProfile:
        return RelocationDomain::Heap;
    case TargetKind::SwitchStringRankCase:
    case TargetKind::MathIC:
    case TargetKind::SwitchTableBase:
    case TargetKind::ImageOffset:
    case TargetKind::SnippetEntry:
        return std::nullopt;
    }
    return std::nullopt;
}

static ASCIILiteral nameOf(RelocationDomain domain)
{
    switch (domain) {
    case RelocationDomain::EngineImage:
        return "engine image"_s;
    case RelocationDomain::ExecutablePool:
        return "executable pool"_s;
    case RelocationDomain::StructureReservation:
        return "structure reservation"_s;
    case RelocationDomain::Heap:
        return "heap"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MathICKind kindOf(const JITAddIC&) { return MathICKind::Add; }
static MathICKind kindOf(const JITSubIC&) { return MathICKind::Sub; }
static MathICKind kindOf(const JITMulIC&) { return MathICKind::Mul; }
static MathICKind kindOf(const JITNegIC&) { return MathICKind::Negate; }

// The canonical form of live code under its fixups (section 8.4) compared with expected as the writer streams it, with
// no buffer of its own.
static bool canonicalCodeEquals(std::span<const uint8_t> liveCode, std::span<const ImageFixup> fixups, std::span<const uint8_t> expected)
{
    size_t cursor = 0;
    auto compare = [&](std::span<const uint8_t> chunk) {
        if (chunk.size() > expected.size() - cursor || !equalSpans(chunk, expected.subspan(cursor, chunk.size())))
            return false;
        cursor += chunk.size();
        return true;
    };
    ImageSectionSink sink = compare;
    ImageSectionWriter writer(sink);
    return writer.writeCanonicalCode(liveCode, fixups) && cursor == expected.size();
}

static std::span<const uint8_t> codeAt(const void* start, size_t size)
{
    return unsafeMakeSpan(static_cast<const uint8_t*>(start), size);
}

// The code's normal entry, offset 0. JITCode::start reads only the code reference but is not const.
static const void* codeStartOf(const BaselineJITCode& code)
{
    return const_cast<BaselineJITCode&>(code).start();
}

// The UCB's arithmetic profiles, saved before the check overwrites them with the producer's compile-start bits and
// written back on every exit (section 11.3, steps 3 and 6). The check runs only with concurrent JIT off, so no compiler
// thread reads them meanwhile (I20).
class ArithProfileOverride {
    WTF_MAKE_NONCOPYABLE(ArithProfileOverride);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit ArithProfileOverride(UnlinkedCodeBlock& unlinkedCodeBlock)
        : m_unlinkedCodeBlock(unlinkedCodeBlock)
        , m_binaryBits(unlinkedCodeBlock.numberOfBinaryArithProfiles(), [&](size_t index) {
            return static_cast<uint16_t>(unlinkedCodeBlock.binaryArithProfile(static_cast<unsigned>(index)).bits());
        })
        , m_unaryBits(unlinkedCodeBlock.numberOfUnaryArithProfiles(), [&](size_t index) {
            return static_cast<uint16_t>(unlinkedCodeBlock.unaryArithProfile(static_cast<unsigned>(index)).bits());
        })
    {
    }

    ~ArithProfileOverride()
    {
        restore();
    }

    // The twins section's counts equal the UCB's (W4).
    void overwrite(const ImageTwinsView& twins)
    {
        for (unsigned index = 0; index < m_binaryBits.size(); ++index)
            m_unlinkedCodeBlock.binaryArithProfile(index).restoreBits(twins.binaryArithBits(index));
        for (unsigned index = 0; index < m_unaryBits.size(); ++index)
            m_unlinkedCodeBlock.unaryArithProfile(index).restoreBits(twins.unaryArithBits(index));
    }

    void restore()
    {
        for (unsigned index = 0; index < m_binaryBits.size(); ++index)
            m_unlinkedCodeBlock.binaryArithProfile(index).restoreBits(m_binaryBits[index]);
        for (unsigned index = 0; index < m_unaryBits.size(); ++index)
            m_unlinkedCodeBlock.unaryArithProfile(index).restoreBits(m_unaryBits[index]);
    }

private:
    UnlinkedCodeBlock& m_unlinkedCodeBlock;
    Vector<uint16_t> m_binaryBits;
    Vector<uint16_t> m_unaryBits;
};

// What resolveTarget reads of one side of the comparison besides the VM, the UCB and the ranks: the restored image or the
// twin's, with its MathICs, dense switch tables, string tables and snippets.
struct ComparedSide {
    const void* imageStart { nullptr };
    Vector<void*> mathICs; // by MathIC index; null where the restored holder has none to match
    Vector<const void*> switchTableBases; // by simple switch table; null for a list table
    std::span<const StringJumpTable> stringSwitchTables;
    Vector<const void*> snippetStarts; // by MathIC index; null without a snippet

    ResolutionContext context(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, const StringSwitchRanks& ranks) const
    {
        return ResolutionContext {
            .vm = vm,
            .ucb = &unlinkedCodeBlock,
            .imageStart = imageStart,
            .mathICs = mathICs.span(),
            .switchTableBases = switchTableBases.span(),
            .stringSwitchTables = stringSwitchTables,
            .snippetStarts = snippetStarts.span(),
            .ranks = &ranks,
        };
    }
};

static Vector<const void*> switchTableBasesOf(const BaselineJITCode& code)
{
    return Vector<const void*>(code.m_switchJumpTables.size(), [&](size_t index) -> const void* {
        auto& offsets = code.m_switchJumpTables[index].m_ctiOffsets;
        return offsets.isEmpty() ? nullptr : offsets.span().data();
    });
}

// One image twin check (section 11.3), on the VM thread.
class TwinCheck {
    WTF_MAKE_NONCOPYABLE(TwinCheck);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    TwinCheck(VM& vm, CodeBlock& installed, const BaselineJITCode& restored, const ImageSectionsView& view, TwinReport& report)
        : m_vm(vm)
        , m_installed(installed)
        , m_unlinkedCodeBlock(*installed.unlinkedCodeBlock())
        , m_restored(restored)
        , m_view(view)
        , m_twins(view.twins())
        , m_report(report)
        , m_body(describeBody(vm, installed))
    {
    }

    void run(JSScope*);

private:
    static String describeBody(VM&, CodeBlock&);
    void difference(ASCIILiteral check, const String& specifics);

    CodeBlock* createTwin(JSScope*);
    bool writeCompileInputs(CodeBlock& twin);
    void checkCodeBlockFacts(CodeBlock& twin);
    bool replay(CodeBlock& twin, ImageRecord& twinRecord, const TwinData&);

    bool buildRestoredSide(ComparedSide&);
    ComparedSide buildTwinSide(const BaselineJITCode& twinCode, const ImageRecord& twinRecord);
    void compareBytesAndFixups(const BaselineJITCode& twinCode, const ImageRecord& twinRecord);
    void compareDecodes(const ComparedSide& restored, const BaselineJITCode& twinCode, const ImageRecord& twinRecord, const ComparedSide& twin);
    void compareTables(const BaselineJITCode& twinCode, const ImageRecord& twinRecord);
    void compareMathICs(const ComparedSide& restored, const BaselineJITCode& twinCode, const ImageRecord& twinRecord);
    void compareRates();
    void checkRelocation(const ComparedSide& restored);

    VM& m_vm;
    CodeBlock& m_installed;
    UnlinkedCodeBlock& m_unlinkedCodeBlock;
    const BaselineJITCode& m_restored;
    const ImageSectionsView& m_view;
    const ImageTwinsView& m_twins;
    TwinReport& m_report;
    String m_body;
    StringSwitchRanks m_ranks;
};

String TwinCheck::describeBody(VM& vm, CodeBlock& codeBlock)
{
    String key = "unknown"_s;
    if (auto* state = vm.jitCacheState()) {
        if (auto bodyKey = state->registry().keyOf(*codeBlock.unlinkedCodeBlock()))
            key = bodyKeyHex(*bodyKey);
    }
    return makeString("body "_s, key, " cb "_s, WTF::toString(codeBlock));
}

void TwinCheck::difference(ASCIILiteral check, const String& specifics)
{
    m_report.difference(TwinPart::Image, check, makeString(m_body, ": "_s, specifics));
}

void TwinCheck::run(JSScope* scope)
{
    // The preconditions, one per process: the producer's inputs equal what its emitters read only when it compiled on the
    // thread holding the API lock, and the consumer may overwrite the UCB's profiles only while no compiler thread reads
    // them. A skip is kept apart from a difference (R-INT-11).
    if (Options::useConcurrentJIT()) {
        m_report.skip(TwinPart::Image, "precondition"_s, makeString(m_body, ": the importing process runs with useConcurrentJIT on"_s));
        return;
    }
    if (!m_twins.compiledHoldingAPILock()) {
        m_report.skip(TwinPart::Image, "precondition"_s, makeString(m_body, ": the producer compiled off the thread holding the VM's API lock"_s));
        return;
    }
    if (m_twins.header().binaryArithProfileCount != m_unlinkedCodeBlock.numberOfBinaryArithProfiles() || m_twins.header().unaryArithProfileCount != m_unlinkedCodeBlock.numberOfUnaryArithProfiles()) {
        difference("compile-inputs"_s, "the recorded arithmetic profile counts differ from the UCB's"_s);
        return;
    }

    // Steps 1 and 2. Nothing in the check collects, and the twin stays held and registered until it returns.
    DeferGCForAWhile deferGC(m_vm);
    CodeBlock* twin = createTwin(scope);
    if (!twin)
        return;
    Strong<CodeBlock> twinHolder(m_vm, twin);
    rememberImageTwin(*twin);

    // Step 3.
    if (!writeCompileInputs(*twin))
        return;
    checkCodeBlockFacts(*twin);
    ArithProfileOverride arithProfiles(m_unlinkedCodeBlock);
    arithProfiles.overwrite(m_twins);

    // Step 4: the producer's seeds and inputs, which the twin recorder reads until the JIT dies.
    TwinData twinData = m_twins.twinData();
    RefPtr<BaselineJITCode> twinCode;
    {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(twin));
        JIT jit(m_vm, plan.get(), twin);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), twinData.seeds, twinData.compileInputs);
        twinCode = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
    }
    if (!twinCode) {
        difference("twin-compile"_s, "the twin compilation got no executable memory"_s);
        return;
    }
    ImageRecord* twinRecord = twinCode->m_jitCacheImageRecord.get();
    if (!twinRecord || twinRecord->state() != RecordState::Complete) {
        difference("twin-record"_s, makeString("the twin compilation left no complete record (state "_s, twinRecord ? static_cast<unsigned>(twinRecord->state()) : 0u, ", reason "_s, twinRecord ? static_cast<unsigned>(twinRecord->unrecordableReason()) : 0u, ')'));
        return;
    }
    if (twinRecord->mathICs().size() != m_view.header().mathICCount) {
        difference("mathics"_s, makeString("the twin has "_s, twinRecord->mathICs().size(), " MathICs and the section "_s, m_view.header().mathICCount));
        return;
    }

    // Step 5.
    if (!replay(*twin, *twinRecord, twinData))
        return;

    // Step 6, which the override repeats on every earlier return.
    arithProfiles.restore();

    // Step 7. The twin's code, snippets and record die with twinCode when the check returns.
    m_ranks = rankStringSwitches(m_unlinkedCodeBlock);
    ComparedSide restoredSide;
    if (!buildRestoredSide(restoredSide))
        return;
    ComparedSide twinSide = buildTwinSide(*twinCode, *twinRecord);
    compareBytesAndFixups(*twinCode, *twinRecord);
    compareDecodes(restoredSide, *twinCode, *twinRecord, twinSide);
    compareTables(*twinCode, *twinRecord);
    compareMathICs(restoredSide, *twinCode, *twinRecord);
    compareRates();
    checkRelocation(restoredSide);
}

CodeBlock* TwinCheck::createTwin(JSScope* scope)
{
    // Native linking of one more CB of the body, never through newCodeBlockFor, whose slot installCode has just filled,
    // nor through the CopyParsedBlock constructor, which shares the installed CB's metadata (N18).
    ASSERT(scope);
    auto exceptionScope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
    ScriptExecutable* executable = m_installed.ownerExecutable();
    CodeBlock* twin = nullptr;
    switch (m_installed.codeType()) {
    case FunctionCode:
        twin = FunctionCodeBlock::create(m_vm, uncheckedDowncast<FunctionExecutable>(executable), uncheckedDowncast<UnlinkedFunctionCodeBlock>(&m_unlinkedCodeBlock), scope);
        break;
    case GlobalCode:
        twin = ProgramCodeBlock::create(m_vm, uncheckedDowncast<ProgramExecutable>(executable), uncheckedDowncast<UnlinkedProgramCodeBlock>(&m_unlinkedCodeBlock), scope);
        break;
    case ModuleCode:
        twin = ModuleProgramCodeBlock::create(m_vm, uncheckedDowncast<ModuleProgramExecutable>(executable), uncheckedDowncast<UnlinkedModuleProgramCodeBlock>(&m_unlinkedCodeBlock), scope);
        break;
    case EvalCode:
        twin = EvalCodeBlock::create(m_vm, uncheckedDowncast<EvalExecutable>(executable), uncheckedDowncast<UnlinkedEvalCodeBlock>(&m_unlinkedCodeBlock), scope);
        break;
    }
    if (exceptionScope.exception()) {
        exceptionScope.clearException();
        difference("twin-cb"_s, "linking the twin CB threw"_s);
        return nullptr;
    }
    if (!twin)
        difference("twin-cb"_s, "linking the twin CB gave none"_s);
    return twin;
}

bool TwinCheck::writeCompileInputs(CodeBlock& twin)
{
    // Kinds 1 to 7 go into the twin's own metadata, each entry left in a state the collector reads safely, since a
    // conservative root may keep the twin to a collection's finalization (CodeBlock::reconcileLLIntInlineCachesAtGCEnd).
    // Kind 8 reaches the compilation through the twin recorder.
    auto& instructions = twin.instructions();
    for (unsigned index = 0; index < m_twins.header().inputCount; ++index) {
        auto input = m_twins.compileInput(index);
        if (input.kind == CompileInputKind::StrictEqualityAtomOperand)
            continue;
        // W4 put every input at an instruction start of its kind's opcode.
        if (input.bytecodeOffset >= instructions.size()) {
            difference("compile-inputs"_s, makeString("an input at "_s, input.bytecodeOffset, " lies past the bytecode"_s));
            return false;
        }
        auto instruction = instructions.at(BytecodeIndex(input.bytecodeOffset));
        auto kind = ImageTwinsInternal::snapshotKindOf(instruction->opcodeID());
        if (kind != input.kind) {
            difference("compile-inputs"_s, makeString("the input at "_s, input.bytecodeOffset, " names another opcode"_s));
            return false;
        }
        switch (input.kind) {
        case CompileInputKind::ResolveScopeType: {
            // The collector reads only the entry's cell union, which linking filled with a cell or null whatever the type.
            auto& metadata = instruction->as<OpResolveScope>().metadata(&twin);
            metadata.m_resolveType = static_cast<ResolveType>(input.value);
            if (metadata.m_resolveType == ClosureVar)
                metadata.m_localScopeDepth = input.localScopeDepth;
            break;
        }
        case CompileInputKind::GetFromScopeType:
        case CompileInputKind::PutToScopeType: {
            // For every type but the variable kinds the collector reads the union as a StructureID, so a watchpoint set
            // another type left would decode as garbage: the union is cleared, with the operand.
            auto rewrite = [&](auto& metadata) {
                GetPutInfo linked = metadata.m_getPutInfo;
                metadata.m_getPutInfo = GetPutInfo(linked.resolveMode(), static_cast<ResolveType>(input.value), linked.initializationMode(), linked.ecmaMode());
                metadata.m_watchpointSet = nullptr;
                metadata.m_operand = 0;
            };
            if (input.kind == CompileInputKind::GetFromScopeType)
                rewrite(instruction->as<OpGetFromScope>().metadata(&twin));
            else
                rewrite(instruction->as<OpPutToScope>().metadata(&twin));
            break;
        }
        case CompileInputKind::GetByIdMode:
        case CompileInputKind::IteratorOpenMode:
        case CompileInputKind::AsyncIteratorOpenMode: {
            // The emitters test only mode == ProtoLoad. All-zero ProtoLoad fields set that mode and a zero hit count; the
            // collector reads a structure only from a Default entry, here zero, and reaches a ProtoLoad entry only
            // through the twin's empty m_llintGetByIdWatchpointMap.
            auto rewrite = [&](GetByIdModeMetadata& mode) {
                if (static_cast<GetByIdMode>(input.value) == GetByIdMode::ProtoLoad) {
                    mode.protoLoadMode.structureID = StructureID();
                    mode.protoLoadMode.cachedOffset = 0;
                    mode.protoLoadMode.cachedSlot = 0;
                } else
                    mode.clearToDefaultModeWithoutCache();
            };
            if (input.kind == CompileInputKind::GetByIdMode)
                rewrite(instruction->as<OpGetById>().metadata(&twin).m_modeMetadata);
            else if (input.kind == CompileInputKind::IteratorOpenMode)
                rewrite(instruction->as<OpIteratorOpen>().metadata(&twin).m_modeMetadata);
            else
                rewrite(instruction->as<OpAsyncIteratorOpen>().metadata(&twin).m_modeMetadata);
            break;
        }
        case CompileInputKind::EnumeratorMetadata:
            instruction->as<OpEnumeratorNext>().metadata(&twin).m_enumeratorMetadata = input.value;
            break;
        case CompileInputKind::StrictEqualityAtomOperand:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return true;
}

void TwinCheck::checkCodeBlockFacts(CodeBlock& twin)
{
    // Both follow from the executable and the source provider the twin shares with the installed CB, and the code depends
    // on the level only through CannotCompile (N13), so the twin compiles with the producer's class.
    auto& bakedFacts = m_view.bakedFacts();
    if ((twin.capabilityLevel() == DFG::CannotCompile) != (bakedFacts.capabilityLevel() == DFG::CannotCompile))
        difference("capability"_s, "the twin's capability class differs from the recorded one"_s);
    if (twin.couldBeTainted() != bakedFacts.couldBeTainted())
        difference("taint"_s, "the twin's taint differs from the recorded one"_s);
}

bool TwinCheck::replay(CodeBlock& twin, ImageRecord& twinRecord, const TwinData& twinData)
{
    for (auto& regeneration : twinData.regenerations) {
        // W3 named a MathIC with inline code, and the twin lists as many MathICs as the section.
        if (regeneration.mathICIndex >= twinRecord.mathICs().size() || !twinRecord.mathICs()[regeneration.mathICIndex].slowCallPointerSite) {
            difference("replay"_s, makeString("regeneration of MathIC "_s, regeneration.mathICIndex, " names no MathIC with inline code in the twin"_s));
            return false;
        }
        // The record may stop during the replay, which frees its MathIC list, so the IC is read first.
        MathICKind kind = twinRecord.mathICs()[regeneration.mathICIndex].kind;
        void* mathIC = twinRecord.mathICs()[regeneration.mathICIndex].mathIC;
        auto replacement = CodePtr<CFunctionPtrTag>::fromUntaggedPtr(const_cast<void*>(regeneration.replacement.address()));
        TwinReplay twinReplay(regeneration);
        {
            MathICRegeneration twinRegeneration(twinReplay, twinRecord, mathIC);
            auto regenerate = [&](auto& typedMathIC) {
                // Every baseline MathIC has its UCB profile; overwriting it here is undone with the others (step 6).
                if (auto* profile = typedMathIC.arithProfile())
                    profile->restoreBits(regeneration.profileBitsAtEntry);
                typedMathIC.generateOutOfLine(&twin, replacement, twinRegeneration);
            };
            switch (kind) {
            case MathICKind::Add:
                regenerate(*static_cast<JITAddIC*>(mathIC));
                break;
            case MathICKind::Sub:
                regenerate(*static_cast<JITSubIC*>(mathIC));
                break;
            case MathICKind::Mul:
                regenerate(*static_cast<JITMulIC*>(mathIC));
                break;
            case MathICKind::Negate:
                regenerate(*static_cast<JITNegIC*>(mathIC));
                break;
            }
        }
        if (twinReplay.failed()) {
            difference("replay"_s, makeString("the replay of MathIC "_s, regeneration.mathICIndex, "'s regeneration got no executable memory"_s));
            return false;
        }
        if (twinReplay.attachCount() != regeneration.assemblerSeeds.size())
            difference("replay"_s, makeString("the replay of MathIC "_s, regeneration.mathICIndex, "'s regeneration attached "_s, twinReplay.attachCount(), " assemblers and the producer's "_s, regeneration.assemblerSeeds.size()));
        if (twinRecord.state() != RecordState::Complete) {
            difference("twin-record"_s, makeString("the replay of MathIC "_s, regeneration.mathICIndex, "'s regeneration left the twin's record incomplete"_s));
            return false;
        }
    }
    return true;
}

bool TwinCheck::buildRestoredSide(ComparedSide& side)
{
    // The restored holder's MathICs in MathIC index order: one with inline code found by its inline start, one without
    // taken among those of its kind with null locations, which nothing else tells apart.
    struct Candidate {
        void* mathIC;
        MathICKind kind;
        bool matched { false };
    };
    Vector<Candidate> candidates;
    m_restored.forEachMathIC([&](auto& mathIC) {
        candidates.append(Candidate { &mathIC, kindOf(mathIC) });
    });

    side.imageStart = codeStartOf(m_restored);
    auto imageStart = reinterpret_cast<uintptr_t>(side.imageStart);
    unsigned mathICCount = m_view.header().mathICCount;
    side.mathICs = Vector<void*>(FillWith { }, mathICCount, nullptr);
    side.snippetStarts = Vector<const void*>(FillWith { }, mathICCount, nullptr);
    bool matchedEvery = true;
    for (unsigned index = 0; index < mathICCount; ++index) {
        auto entry = m_view.mathIC(index);
        for (auto& candidate : candidates) {
            if (candidate.matched || candidate.kind != entry.kind)
                continue;
            auto locations = mathICLocations(candidate.kind, candidate.mathIC);
            bool matches = entry.hasInlineCode() ? reinterpret_cast<uintptr_t>(locations.inlineStart) == imageStart + entry.inlineStart : locations.areAllNull();
            if (!matches)
                continue;
            candidate.matched = true;
            side.mathICs[index] = candidate.mathIC;
            side.snippetStarts[index] = mathICCodeState(candidate.kind, candidate.mathIC).snippetStart;
            break;
        }
        matchedEvery = matchedEvery && side.mathICs[index];
    }
    if (!matchedEvery || std::ranges::any_of(candidates, [](const Candidate& candidate) { return !candidate.matched; })) {
        difference("mathics"_s, "the restored code's MathICs do not match the section's"_s);
        return false;
    }
    side.switchTableBases = switchTableBasesOf(m_restored);
    side.stringSwitchTables = m_restored.m_stringSwitchJumpTables.span();
    return true;
}

ComparedSide TwinCheck::buildTwinSide(const BaselineJITCode& twinCode, const ImageRecord& twinRecord)
{
    ComparedSide side;
    side.imageStart = codeStartOf(twinCode);
    auto& mathICs = twinRecord.mathICs();
    side.mathICs = Vector<void*>(mathICs.size(), [&](size_t index) {
        return mathICs[index].mathIC;
    });
    side.snippetStarts = Vector<const void*>(mathICs.size(), [&](size_t index) -> const void* {
        return mathICs[index].snippet ? mathICs[index].snippet->start : nullptr;
    });
    side.switchTableBases = switchTableBasesOf(twinCode);
    side.stringSwitchTables = twinCode.m_stringSwitchJumpTables.span();
    return side;
}

void TwinCheck::compareBytesAndFixups(const BaselineJITCode& twinCode, const ImageRecord& twinRecord)
{
    // By I10 the restored bytes equal the section's outside footprints, so comparing the twin's canonical form with the
    // section compares the restored code with its twin everywhere else. No side reads past a linked size (N20).
    auto& header = m_view.header();
    if (twinRecord.codeSize() != header.codeSize)
        difference("code-size"_s, makeString("the twin's linked size is "_s, twinRecord.codeSize(), " and the section's "_s, header.codeSize));
    else if (!canonicalCodeEquals(codeAt(codeStartOf(twinCode), twinRecord.codeSize()), twinRecord.fixups().span(), m_view.code()))
        difference("code-bytes"_s, "the twin's image differs from the section's outside its footprints"_s);

    auto sectionFixups = m_view.fixups();
    bool fixupsEqual = sectionFixups.size() == twinRecord.fixups().size();
    for (size_t index = 0; fixupsEqual && index < sectionFixups.size(); ++index)
        fixupsEqual = sectionFixups[index] == twinRecord.fixups()[index];
    if (!fixupsEqual)
        difference("fixups"_s, "the twin's image fixups differ from the section's"_s);

    Vector<std::optional<ImageSnippet>> sectionSnippets(FillWith { }, header.mathICCount, std::nullopt);
    m_view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        sectionSnippets[index] = snippet;
    });
    for (unsigned index = 0; index < header.mathICCount; ++index) {
        auto& twinSnippet = twinRecord.mathICs()[index].snippet;
        auto& sectionSnippet = sectionSnippets[index];
        if (!twinSnippet != !sectionSnippet) {
            difference("snippet"_s, makeString("MathIC "_s, index, (twinSnippet ? " has a snippet in the twin only"_s : " has a snippet in the section only"_s)));
            continue;
        }
        if (!twinSnippet)
            continue;
        if (twinSnippet->size != sectionSnippet->code.size()) {
            difference("snippet"_s, makeString("MathIC "_s, index, "'s twin snippet's linked size is "_s, twinSnippet->size, " and the section's "_s, sectionSnippet->code.size()));
            continue;
        }
        if (!canonicalCodeEquals(codeAt(twinSnippet->start, twinSnippet->size), twinSnippet->fixups.span(), sectionSnippet->code))
            difference("snippet"_s, makeString("MathIC "_s, index, "'s twin snippet differs from the section's outside its footprints"_s));
        auto& snippetFixups = sectionSnippet->fixups;
        bool snippetFixupsEqual = snippetFixups.size() == twinSnippet->fixups.size();
        for (size_t fixup = 0; snippetFixupsEqual && fixup < snippetFixups.size(); ++fixup)
            snippetFixupsEqual = snippetFixups[fixup] == twinSnippet->fixups[fixup];
        if (!snippetFixupsEqual)
            difference("fixups"_s, makeString("MathIC "_s, index, "'s twin snippet fixups differ from the section's"_s));
    }
}

void TwinCheck::compareDecodes(const ComparedSide& restored, const BaselineJITCode& twinCode, const ImageRecord& twinRecord, const ComparedSide& twin)
{
    // Each side's footprints decode, islands followed, to its own resolution of each target. Support, VM, UCB, process
    // and structure-base targets resolve alike in both contexts, so for them this also compares the absolute targets;
    // artifact targets compare by logical target only.
    auto restoredContext = restored.context(m_vm, m_unlinkedCodeBlock, m_ranks);
    auto twinContext = twin.context(m_vm, m_unlinkedCodeBlock, m_ranks);
    auto& header = m_view.header();

    if (!fixupsReachTargets(restored.imageStart, header.codeSize, m_view.fixups(), restoredContext))
        difference("decode"_s, "a footprint of the restored image does not reach its target"_s);
    m_view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        if (!fixupsReachTargets(restored.snippetStarts[index], snippet.code.size(), snippet.fixups, restoredContext))
            difference("decode"_s, makeString("a footprint of MathIC "_s, index, "'s restored snippet does not reach its target"_s));
    });

    if (!fixupsReachTargets(codeStartOf(twinCode), twinRecord.codeSize(), twinRecord.fixups(), twinContext))
        difference("decode"_s, "a footprint of the twin's image does not reach its target"_s);
    for (unsigned index = 0; index < twinRecord.mathICs().size(); ++index) {
        auto& snippet = twinRecord.mathICs()[index].snippet;
        if (snippet && !fixupsReachTargets(snippet->start, snippet->size, snippet->fixups, twinContext))
            difference("decode"_s, makeString("a footprint of MathIC "_s, index, "'s twin snippet does not reach its target"_s));
    }
}

void TwinCheck::compareTables(const BaselineJITCode& twinCode, const ImageRecord& twinRecord)
{
    // Code pointers compare as offsets into their own images.
    auto restoredStart = reinterpret_cast<uintptr_t>(codeStartOf(m_restored));
    auto twinStart = reinterpret_cast<uintptr_t>(codeStartOf(twinCode));
    auto restoredOffset = [&](const void* location) {
        return reinterpret_cast<uintptr_t>(location) - restoredStart;
    };
    auto twinOffset = [&](const void* location) {
        return reinterpret_cast<uintptr_t>(location) - twinStart;
    };

    auto& restoredCalls = m_restored.m_unlinkedCalls;
    auto& twinCalls = twinCode.m_unlinkedCalls;
    bool callsEqual = restoredCalls.size() == twinCalls.size();
    for (size_t index = 0; callsEqual && index < restoredCalls.size(); ++index) {
        callsEqual = restoredCalls[index].bytecodeIndex == twinCalls[index].bytecodeIndex
            && restoredOffset(restoredCalls[index].doneLocation.untaggedPtr()) == twinOffset(twinCalls[index].doneLocation.untaggedPtr());
    }
    if (!callsEqual)
        difference("calls"_s, "the restored call records differ from the twin's"_s);

    auto& restoredMolds = m_restored.m_unlinkedPropertyInlineCaches;
    auto& twinMolds = twinCode.m_unlinkedPropertyInlineCaches;
    bool moldsEqual = restoredMolds.size() == twinMolds.size();
    for (size_t index = 0; moldsEqual && index < restoredMolds.size(); ++index) {
        auto& restoredMold = restoredMolds[index];
        auto& twinMold = twinMolds[index];
        moldsEqual = restoredMold.accessType == twinMold.accessType
            && restoredMold.preconfiguredCacheType == twinMold.preconfiguredCacheType
            && restoredMold.propertyIsInt32 == twinMold.propertyIsInt32
            && restoredMold.propertyIsString == twinMold.propertyIsString
            && restoredMold.propertyIsSymbol == twinMold.propertyIsSymbol
            && restoredMold.prototypeIsKnownObject == twinMold.prototypeIsKnownObject
            && restoredMold.canBeMegamorphic == twinMold.canBeMegamorphic
            && restoredMold.m_identifier.uid() == twinMold.m_identifier.uid()
            && restoredMold.bytecodeIndex == twinMold.bytecodeIndex
            && restoredOffset(restoredMold.doneLocation.untaggedPtr()) == twinOffset(twinMold.doneLocation.untaggedPtr());
    }
    if (!moldsEqual)
        difference("molds"_s, "the restored molds differ from the twin's"_s);

    auto& restoredSimple = m_restored.m_switchJumpTables;
    auto& twinSimple = twinCode.m_switchJumpTables;
    bool simpleEqual = restoredSimple.size() == twinSimple.size();
    for (size_t index = 0; simpleEqual && index < restoredSimple.size(); ++index) {
        auto& restoredTable = restoredSimple[index];
        auto& twinTable = twinSimple[index];
        simpleEqual = restoredOffset(restoredTable.m_ctiDefault.untaggedPtr()) == twinOffset(twinTable.m_ctiDefault.untaggedPtr())
            && restoredTable.m_ctiOffsets.size() == twinTable.m_ctiOffsets.size();
        for (size_t entry = 0; simpleEqual && entry < restoredTable.m_ctiOffsets.size(); ++entry)
            simpleEqual = restoredOffset(restoredTable.m_ctiOffsets[entry].untaggedPtr()) == twinOffset(twinTable.m_ctiOffsets[entry].untaggedPtr());
    }
    auto& restoredString = m_restored.m_stringSwitchJumpTables;
    auto& twinString = twinCode.m_stringSwitchJumpTables;
    bool stringEqual = restoredString.size() == twinString.size();
    for (size_t index = 0; stringEqual && index < restoredString.size(); ++index) {
        auto& restoredOffsets = restoredString[index].m_ctiOffsets;
        auto& twinOffsets = twinString[index].m_ctiOffsets;
        stringEqual = restoredOffsets.size() == twinOffsets.size();
        for (size_t entry = 0; stringEqual && entry < restoredOffsets.size(); ++entry)
            stringEqual = restoredOffset(restoredOffsets[entry].untaggedPtr()) == twinOffset(twinOffsets[entry].untaggedPtr());
    }
    if (!simpleEqual || !stringEqual)
        difference("switch-tables"_s, "the restored switch tables differ from the twin's"_s);

    Vector<std::pair<BytecodeIndex, uintptr_t>> restoredMap;
    restoredMap.reserveInitialCapacity(m_restored.m_jitCodeMap.size());
    m_restored.m_jitCodeMap.forEach([&](BytecodeIndex index, CodeLocationLabel<JSEntryPtrTag> location) {
        restoredMap.append({ index, restoredOffset(location.untaggedPtr()) });
    });
    Vector<std::pair<BytecodeIndex, uintptr_t>> twinMap;
    twinMap.reserveInitialCapacity(twinCode.m_jitCodeMap.size());
    twinCode.m_jitCodeMap.forEach([&](BytecodeIndex index, CodeLocationLabel<JSEntryPtrTag> location) {
        twinMap.append({ index, twinOffset(location.untaggedPtr()) });
    });
    if (restoredMap != twinMap)
        difference("code-map"_s, "the restored code map differs from the twin's"_s);

    auto& restoredPool = m_restored.m_constantPool;
    auto& twinPool = twinCode.m_constantPool;
    bool poolEqual = restoredPool.size() == twinPool.size();
    for (size_t index = 0; poolEqual && index < restoredPool.size(); ++index)
        poolEqual = restoredPool.at(index).type() == twinPool.at(index).type() && restoredPool.at(index).pointer() == twinPool.at(index).pointer();
    if (!poolEqual)
        difference("constant-pool"_s, "the restored constant pool differs from the twin's"_s);

    // The restored code's facts are the section's.
    auto& sectionFacts = m_view.bakedFacts();
    auto& twinFacts = twinRecord.bakedFacts();
    bool factsEqual = sectionFacts.capabilityLevel() == twinFacts.capabilityLevel
        && sectionFacts.couldBeTainted() == twinFacts.couldBeTainted
        && sectionFacts.scopeFactCount() == twinFacts.scopeFacts.size();
    for (unsigned index = 0; factsEqual && index < sectionFacts.scopeFactCount(); ++index)
        factsEqual = sectionFacts.scopeFact(index) == twinFacts.scopeFacts[index];
    if (!factsEqual)
        difference("baked-facts"_s, "the section's baked facts differ from the twin's"_s);
}

void TwinCheck::compareMathICs(const ComparedSide& restored, const BaselineJITCode& twinCode, const ImageRecord& twinRecord)
{
    // Generators need no comparison: JIT::emitMathICFast and the import build them with the same function (section 6.3).
    auto restoredStart = reinterpret_cast<uintptr_t>(restored.imageStart);
    auto twinStart = reinterpret_cast<uintptr_t>(codeStartOf(twinCode));
    auto offsetsOf = [](const MathICLocations& locations, uintptr_t start) {
        auto offset = [&](const void* location) -> uintptr_t {
            return location ? reinterpret_cast<uintptr_t>(location) - start : 0;
        };
        return std::array<uintptr_t, 4> { offset(locations.inlineStart), offset(locations.inlineEnd), offset(locations.slowPathStart), offset(locations.slowPathCall) };
    };
    for (unsigned index = 0; index < twinRecord.mathICs().size(); ++index) {
        auto& twinMathIC = twinRecord.mathICs()[index];
        auto entry = m_view.mathIC(index);
        if (twinMathIC.kind != entry.kind) {
            difference("mathics"_s, makeString("MathIC "_s, index, " has another kind in the twin"_s));
            continue;
        }
        auto restoredLocations = mathICLocations(entry.kind, restored.mathICs[index]);
        auto twinLocations = mathICLocations(entry.kind, twinMathIC.mathIC);
        auto restoredState = mathICCodeState(entry.kind, restored.mathICs[index]);
        auto twinState = mathICCodeState(entry.kind, twinMathIC.mathIC);
        bool equal = restoredLocations.areAllSet() == twinLocations.areAllSet()
            && restoredLocations.areAllNull() == twinLocations.areAllNull()
            && offsetsOf(restoredLocations, restoredStart) == offsetsOf(twinLocations, twinStart)
            && restoredState.generateFastPathOnRepatch == twinState.generateFastPathOnRepatch
            && !restoredState.snippetStart == !twinState.snippetStart;
        if (!equal)
            difference("mathics"_s, makeString("MathIC "_s, index, "'s state differs between the restored code and the twin"_s));
    }
}

void TwinCheck::compareRates()
{
    // State the engine cannot recompute: the restored code's rates equal the capture's (THREAD Verification).
    auto& header = m_view.header();
    if (std::bit_cast<uint64_t>(m_restored.livenessRate()) != header.livenessRateBits || std::bit_cast<uint64_t>(m_restored.fullnessRate()) != header.fullnessRateBits)
        difference("rates"_s, "the restored coverage rates differ from the capture's"_s);
}

void TwinCheck::checkRelocation(const ComparedSide& restored)
{
    // A body this process captured, by this VM or another, has no domain that could have moved between its capture and
    // its import, so the clause compares no pair for it; every other comparison still ran.
    if (equalSpans(m_twins.captureProcessToken(), captureProcessToken()))
        return;

    auto context = restored.context(m_vm, m_unlinkedCodeBlock, m_ranks);
    ImageTestHook hook = imageTestHook();
    std::array<bool, numberOfRelocationDomains> forcedDomains { };
    bool forcedOperation = false;
    unsigned producerValueIndex = 0;
    auto compare = [&](const ImageFixup& fixup, ASCIILiteral place) {
        // W3 gave every fixup a producer value, in the order the fixups are walked here.
        unsigned valueIndex = producerValueIndex++;
        if (valueIndex >= m_twins.header().producerValueCount)
            return;
        auto resolution = reinterpret_cast<uintptr_t>(resolveTarget(context, fixup.target));
        auto domain = relocationDomainOf(fixup.target.kind, resolution);
        if (!domain)
            return;
        uint64_t producerValue = m_twins.producerValue(valueIndex);
        if (hook == ImageTestHook::OperationPair && fixup.target.kind == TargetKind::Operation && !forcedOperation) {
            forcedOperation = true;
            producerValue = resolution;
        }
        // THREAD Verification's relocation requirement skips a target inside an object loaded at its link-time address,
        // as Bun's build flags load the engine (N23).
        if (liesIn(loadedObjects().atLinkTimeAddress, resolution))
            return;
        auto& forced = forcedDomains[static_cast<unsigned>(*domain)];
        if (hook == ImageTestHook::RelocationPairs && !forced) {
            forced = true;
            producerValue = resolution;
        }
        if (producerValue != resolution)
            return;
        m_report.relocationCoincidence(*domain, makeString(m_body, ": the "_s, place, " fixup at "_s, fixup.site, " of target kind "_s, static_cast<unsigned>(fixup.target.kind),
            " resolves to the address the capturing process resolved it to, in the "_s, nameOf(*domain)));
    };

    auto imageFixups = m_view.fixups();
    for (size_t index = 0; index < imageFixups.size(); ++index)
        compare(imageFixups[index], "image"_s);
    m_view.forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        for (size_t index = 0; index < snippet.fixups.size(); ++index)
            compare(snippet.fixups[index], "snippet"_s);
    });
}

} // namespace ImageTwinsInternal

// Registry, hooks and token.

static Lock imageTwinRegistryLock;

static HashSet<CodeBlock*>& imageTwinRegistry() WTF_REQUIRES_LOCK(imageTwinRegistryLock)
{
    // Never destroyed, since CodeBlock::~CodeBlock can run during process exit; the first caller creates it, from
    // whichever thread calls first.
    static NeverDestroyed<HashSet<CodeBlock*>> registry;
    return registry.get();
}

void rememberImageTwin(CodeBlock& codeBlock)
{
    Locker locker { imageTwinRegistryLock };
    imageTwinRegistry().add(&codeBlock);
}

bool forgetImageTwin(CodeBlock& codeBlock)
{
    Locker locker { imageTwinRegistryLock };
    return imageTwinRegistry().remove(&codeBlock);
}

size_t imageTwinCountForTesting()
{
    Locker locker { imageTwinRegistryLock };
    return imageTwinRegistry().size();
}

static ImageTestHook imageTestHookValue { ImageTestHook::None };

void setImageTestHook(ImageTestHook hook)
{
    imageTestHookValue = hook;
}

ImageTestHook imageTestHook()
{
    return imageTestHookValue;
}

std::span<const uint8_t, captureProcessTokenSize> captureProcessToken()
{
    static std::array<uint8_t, captureProcessTokenSize> token { };
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        // An all-zero token is what W1 rejects, so it is never one a process writes.
        do
            cryptographicallyRandomValues(std::span { token });
        while (ImageBytes::isZero(token));
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
        if (regeneration.assemblerSeeds.capacity() > ImageTwinsInternal::maximumAttachCount)
            bytes += regeneration.assemblerSeeds.capacity() * sizeof(std::optional<uint32_t>);
    }
    return bytes;
}

TwinCompileInputs snapshotCompileInputs(CodeBlock& codeBlock)
{
    using namespace ImageTwinsInternal;
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
            // JIT::emit_op_resolve_scope and emitSlow_op_resolve_scope read the type, and the depth of a ClosureVar.
            auto& metadata = instruction->as<OpResolveScope>().metadata(&codeBlock);
            ResolveType type = metadata.m_resolveType;
            input.value = static_cast<uint8_t>(type);
            if (type == ClosureVar)
                input.localScopeDepth = metadata.m_localScopeDepth;
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
        case CompileInputKind::StrictEqualityAtomOperand:
            RELEASE_ASSERT_NOT_REACHED();
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
    using namespace ImageTwinsInternal;
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

    // Each regeneration entry's size follows from its attach count, which W3 bounds.
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
    return ImageBytes::read<uint32_t>(m_bytes, m_layout.binarySwitchSeeds + static_cast<size_t>(index) * sizeof(uint32_t));
}

CompileInput ImageTwinsView::compileInput(unsigned index) const
{
    using namespace ImageTwinsInternal;
    ASSERT(index < m_header.inputCount);
    auto entry = m_bytes.subspan(m_layout.inputs + static_cast<size_t>(index) * imageTwinsCompileInputEntrySize, imageTwinsCompileInputEntrySize);
    return CompileInput {
        .bytecodeOffset = ImageBytes::read<uint32_t>(entry, inputBytecodeOffsetOffset),
        .kind = static_cast<CompileInputKind>(entry[inputKindOffset]),
        .value = entry[inputValueOffset],
        .localScopeDepth = ImageBytes::read<uint32_t>(entry, inputDepthOffset),
    };
}

uint16_t ImageTwinsView::binaryArithBits(unsigned index) const
{
    ASSERT(index < m_header.binaryArithProfileCount);
    return ImageBytes::read<uint16_t>(m_bytes, m_layout.binaryArithBits + static_cast<size_t>(index) * sizeof(uint16_t));
}

uint16_t ImageTwinsView::unaryArithBits(unsigned index) const
{
    ASSERT(index < m_header.unaryArithProfileCount);
    return ImageBytes::read<uint16_t>(m_bytes, m_layout.unaryArithBits + static_cast<size_t>(index) * sizeof(uint16_t));
}

uint64_t ImageTwinsView::producerValue(unsigned index) const
{
    ASSERT(index < m_header.producerValueCount);
    return ImageBytes::read<uint64_t>(m_bytes, m_layout.producerValues + static_cast<size_t>(index) * sizeof(uint64_t));
}

std::span<const uint8_t> ImageTwinsView::regenerationEntry(size_t offset) const
{
    uint8_t attachCount = m_bytes[offset + ImageTwinsInternal::regenerationAttachCountOffset];
    return m_bytes.subspan(offset, imageTwinsRegenerationEntrySize(attachCount));
}

TwinRegeneration ImageTwinsView::decodeRegeneration(std::span<const uint8_t> entry)
{
    using namespace ImageTwinsInternal;
    uint8_t attachCount = entry[regenerationAttachCountOffset];
    uint8_t mask = entry[regenerationMaskOffset];
    TwinRegeneration regeneration {
        .mathICIndex = ImageBytes::read<uint32_t>(entry, regenerationIndexOffset),
        .profileBitsAtEntry = ImageBytes::read<uint16_t>(entry, regenerationBitsOffset),
        .replacement = CodeSymbol { ImageBytes::read<int64_t>(entry, regenerationReplacementOffset) },
        .assemblerSeeds = { },
    };
    // Exactly attachCount slots, so twinDataStorageBytes counts what the slots hold.
    regeneration.assemblerSeeds.reserveInitialCapacity(attachCount);
    for (unsigned slot = 0; slot < attachCount; ++slot) {
        bool drew = slot < 8 && (mask & (1u << slot));
        regeneration.assemblerSeeds.append(drew ? std::optional<uint32_t> { ImageBytes::read<uint32_t>(entry, regenerationSeedsOffset + slot * sizeof(uint32_t)) } : std::nullopt);
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
        uint8_t attachCount = entry[ImageTwinsInternal::regenerationAttachCountOffset];
        if (attachCount > ImageTwinsInternal::maximumAttachCount)
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

bool ImageTwinsView::passesW1() const
{
    using namespace ImageTwinsInternal;
    if (m_header.flags & ~ImageTwinsFlags::compiledHoldingAPILock)
        return false;
    if (!ImageBytes::isZero(m_bytes.subspan(reservedOffset, reservedSize)))
        return false;
    if (ImageBytes::isZero(m_header.captureProcessToken))
        return false;
    if (m_layout.end != m_bytes.size())
        return false;

    auto isZeroBetween = [&](size_t begin, size_t end) {
        return ImageBytes::isZero(m_bytes.subspan(begin, end - begin));
    };
    if (!isZeroBetween(m_layout.binarySwitchSeedsEnd, m_layout.inputs)
        || !isZeroBetween(m_layout.inputsEnd, m_layout.binaryArithBits)
        || !isZeroBetween(m_layout.binaryArithBitsEnd, m_layout.unaryArithBits)
        || !isZeroBetween(m_layout.unaryArithBitsEnd, m_layout.regenerations))
        return false;
    for (unsigned index = 0; index < m_header.inputCount; ++index) {
        size_t entry = m_layout.inputs + static_cast<size_t>(index) * imageTwinsCompileInputEntrySize;
        if (ImageBytes::read<uint16_t>(m_bytes, entry + inputReservedOffset))
            return false;
    }
    // Each regeneration entry pads its seeds to 8.
    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        size_t seedsEnd = regenerationSeedsOffset + static_cast<size_t>(entry[regenerationAttachCountOffset]) * sizeof(uint32_t);
        if (!ImageBytes::isZero(entry.subspan(seedsEnd)))
            return false;
        offset += entry.size();
    }
    return true;
}

bool ImageTwinsView::passesW2() const
{
    using namespace ImageTwinsInternal;
    for (unsigned index = 0; index < m_header.inputCount; ++index) {
        auto input = compileInput(index);
        auto kindByte = static_cast<uint8_t>(input.kind);
        if (kindByte < static_cast<uint8_t>(CompileInputKind::ResolveScopeType) || kindByte > static_cast<uint8_t>(CompileInputKind::StrictEqualityAtomOperand))
            return false;
        if (index && input.bytecodeOffset <= compileInput(index - 1).bytecodeOffset)
            return false;
        if (!isValidInputValue(input.kind, input.value))
            return false;
        bool mayHaveDepth = input.kind == CompileInputKind::ResolveScopeType && input.value == static_cast<uint8_t>(ClosureVar);
        if (input.localScopeDepth && !mayHaveDepth)
            return false;
    }
    return true;
}

bool ImageTwinsView::passesW3(const ImageSectionsView& view) const
{
    using namespace ImageTwinsInternal;
    auto& image = view.header();
    // One producer value per fixup, the image's and every snippet's (V5 left snippet fields zero without a snippet).
    uint64_t fixupCount = image.fixupCount;
    for (unsigned index = 0; index < image.mathICCount; ++index)
        fixupCount += view.mathIC(index).snippetFixupCount;
    if (m_header.producerValueCount != fixupCount)
        return false;

    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        offset += entry.size();
        uint32_t mathICIndex = ImageBytes::read<uint32_t>(entry, regenerationIndexOffset);
        if (mathICIndex >= image.mathICCount || !view.mathIC(mathICIndex).hasInlineCode())
            return false;
        uint8_t attachCount = entry[regenerationAttachCountOffset];
        if (attachCount < 1 || attachCount > maximumAttachCount)
            return false;
        uint8_t mask = entry[regenerationMaskOffset];
        if (mask & ~((1u << attachCount) - 1))
            return false;
        for (unsigned slot = 0; slot < attachCount; ++slot) {
            if (!(mask & (1u << slot)) && ImageBytes::read<uint32_t>(entry, regenerationSeedsOffset + slot * sizeof(uint32_t)))
                return false;
        }
        if (!CodeSymbol::isValid(ImageBytes::read<int64_t>(entry, regenerationReplacementOffset)))
            return false;
    }
    return true;
}

std::optional<ImageCheck> firstTwinsStructureFailure(const ImageSectionsView& view)
{
    auto& twins = view.twins();
    if (!twins.passesW1())
        return ImageCheck::W1;
    if (!twins.passesW2())
        return ImageCheck::W2;
    if (!twins.passesW3(view))
        return ImageCheck::W3;
    return std::nullopt;
}

std::optional<ImageCheck> firstTwinsFailureAgainst(const ImageSectionsView& view, const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    using namespace ImageTwinsInternal;
    auto& twins = view.twins();
    if (twins.header().binaryArithProfileCount != unlinkedCodeBlock.numberOfBinaryArithProfiles() || twins.header().unaryArithProfileCount != unlinkedCodeBlock.numberOfUnaryArithProfiles())
        return ImageCheck::W4;

    // Inputs and instructions are both in bytecode order (W2), so one walk pairs them: every instruction that asks has
    // exactly one input of its kind, and every input sits at an instruction start that asks.
    unsigned next = 0;
    unsigned inputCount = twins.header().inputCount;
    for (const auto& instruction : unlinkedCodeBlock.instructions()) {
        uint32_t offset = instruction.offset();
        if (next < inputCount && twins.compileInput(next).bytecodeOffset < offset)
            return ImageCheck::W4;
        bool hasInput = next < inputCount && twins.compileInput(next).bytecodeOffset == offset;
        auto expected = expectedInputKind(unlinkedCodeBlock, instruction.ptr());
        if (expected.has_value() != hasInput)
            return ImageCheck::W4;
        if (!hasInput)
            continue;
        auto input = twins.compileInput(next++);
        if (input.kind != *expected)
            return ImageCheck::W4;
        // The operand the producer compared inline by its atom is an atom here too, since atom-ness only grows, unless the
        // section disagrees with its UCB (R-UCB-1).
        if (input.kind == CompileInputKind::StrictEqualityAtomOperand && input.value != static_cast<uint8_t>(StrictEqualityAtomOperand::None)) {
            auto operands = strictEqualityOperands(instruction.ptr());
            ASSERT(operands);
            auto operand = input.value == static_cast<uint8_t>(StrictEqualityAtomOperand::Lhs) ? operands->first : operands->second;
            if (!isAtomStringConstant(unlinkedCodeBlock, operand))
                return ImageCheck::W4;
        }
    }
    if (next != inputCount)
        return ImageCheck::W4;
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
    using namespace ImageTwinsInternal;
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
        ImageBytes::write<uint32_t>(fixed, regenerationIndexOffset, regeneration.mathICIndex);
        ImageBytes::write<uint16_t>(fixed, regenerationBitsOffset, regeneration.profileBitsAtEntry);
        fixed[regenerationAttachCountOffset] = static_cast<uint8_t>(regeneration.assemblerSeeds.size());
        uint8_t mask = 0;
        for (size_t slot = 0; slot < regeneration.assemblerSeeds.size(); ++slot) {
            if (regeneration.assemblerSeeds[slot])
                mask |= static_cast<uint8_t>(1u << slot);
        }
        fixed[regenerationMaskOffset] = mask;
        ImageBytes::write<int64_t>(fixed, regenerationReplacementOffset, regeneration.replacement.offset);
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

// The check.

std::optional<uint32_t> TwinReplay::takeAttachSeed()
{
    unsigned slot = m_attachCount++;
    if (slot >= m_regeneration.assemblerSeeds.size())
        return std::nullopt;
    return m_regeneration.assemblerSeeds[slot];
}

void Twins::checkImage(VM& vm, CodeBlock& installed, JSScope* scope, const BaselineJITCode& restored, const ImageSectionsView& view, TwinReport& report)
{
    ImageTwinsInternal::TwinCheck check(vm, installed, restored, view, report);
    check.run(scope);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
