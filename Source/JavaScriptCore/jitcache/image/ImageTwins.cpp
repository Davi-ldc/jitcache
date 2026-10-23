#include "config.h"
#include "jitcache/image/ImageTwins.h"

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
#include "JIT.h"
#include "JITCodeMap.h"
#include "JITMathIC.h"
#include "JSCInlines.h"
#include "ModuleProgramCodeBlock.h"
#include "ModuleProgramExecutable.h"
#include "Options.h"
#include "ProgramCodeBlock.h"
#include "ProgramExecutable.h"
#include "StrongInlines.h"
#include "TopExceptionScope.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/base/EngineObject.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/image/ImageFootprint.h"
#include "jitcache/image/ImageRecord.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageTwinsSection.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/twins/TwinReport.h"
#include <algorithm>
#include <array>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

// Besides the declarations of its header, the twin check's steps: creating and compiling the twin from the producer's
// inputs, replaying its regenerations, and comparing the restored code, tables and MathICs with the twin's, then each
// relocated address with the one the producer resolved.

namespace JSC::JITCache {

namespace ImageTwinsInternal {

static constexpr unsigned numberOfRelocationDomains = static_cast<unsigned>(RelocationDomain::Heap) + 1;

// Whether the address lies in one of the loaded segments, which loadedObjects lists (EngineObject.h). Only Linux
// targets import, so elsewhere the lists are empty.
static bool liesIn(const Vector<LoadedSegment>& segments, uintptr_t address)
{
    return std::ranges::any_of(segments, [&](const LoadedSegment& segment) {
        return segment.contains(address);
    });
}

// The relocation domain a target's resolution lies in, or nullopt for an artifact target, whose addresses
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
        // A static StringImpl, such as the empty string's, is data of the engine image.
        return liesIn(loadedObjects().engineSegments, resolution) ? RelocationDomain::EngineImage : RelocationDomain::Heap;
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

// The canonical form of live code under its fixups, every footprint's variable field zeroed, compared with expected as
// the writer streams it, with no buffer of its own.
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
// written back on every exit. The check runs only with concurrent JIT off, so no compiler thread reads them meanwhile.
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

    // The twins section's counts equal the UCB's, which run checks before it overwrites anything.
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

// One image twin check, on the VM thread.
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
        if (auto bodyKey = state->bodyKeyOf(*codeBlock.unlinkedCodeBlock()))
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
    // them. The report keeps a skip apart from a difference.
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

    // A twin CB, linked natively. Nothing in the check collects, and the twin stays held and registered until it returns.
    DeferGCForAWhile deferGC(m_vm);
    CodeBlock* twin = createTwin(scope);
    if (!twin)
        return;
    Strong<CodeBlock> twinHolder(m_vm, twin);
    rememberImageTwin(*twin);

    // The producer's compile inputs, in the twin's metadata and the UCB's arithmetic profiles.
    if (!writeCompileInputs(*twin))
        return;
    checkCodeBlockFacts(*twin);
    ArithProfileOverride arithProfiles(m_unlinkedCodeBlock);
    arithProfiles.overwrite(m_twins);

    // The twin compile, with the producer's seeds, which the twin recorder reads until the JIT dies.
    TwinData twinData = m_twins.twinData();
    RefPtr<BaselineJITCode> twinCode;
    {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(twin));
        JIT jit(m_vm, plan.get(), twin);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), twinData.seeds);
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

    // The producer's MathIC regenerations, replayed on the twin.
    if (!replay(*twin, *twinRecord, twinData))
        return;

    // The UCB's own profiles back, which the override also writes back on every earlier return.
    arithProfiles.restore();

    // The comparisons. The twin's code, snippets and record die with twinCode when the check returns.
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
    // nor through the CopyParsedBlock constructor, which shares the installed CB's metadata.
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
    // Every input goes into the twin's own metadata, each entry left in a state the collector reads safely, since a
    // conservative root may keep the twin to a collection's finalization (CodeBlock::reconcileLLIntInlineCachesAtGCEnd).
    auto& instructions = twin.instructions();
    for (unsigned index = 0; index < m_twins.header().inputCount; ++index) {
        auto input = m_twins.compileInput(index);
        // Capture took every input at an instruction start of its kind's opcode.
        if (input.bytecodeOffset >= instructions.size()) {
            difference("compile-inputs"_s, makeString("an input at "_s, input.bytecodeOffset, " lies past the bytecode"_s));
            return false;
        }
        auto instruction = instructions.at(BytecodeIndex(input.bytecodeOffset));
        auto kind = snapshotKindOf(instruction->opcodeID());
        if (kind != input.kind) {
            difference("compile-inputs"_s, makeString("the input at "_s, input.bytecodeOffset, " names another opcode"_s));
            return false;
        }
        switch (input.kind) {
        case CompileInputKind::ResolveScopeType: {
            // The collector reads the entry's cell union as a SymbolTable unless the type is ModuleVar, whose import slot
            // shares it. So the union is cleared first, which also drops an import slot the twin's own linking left there,
            // and only a ModuleVar entry gets its slot back.
            auto& metadata = instruction->as<OpResolveScope>().metadata(&twin);
            metadata.m_resolveType = static_cast<ResolveType>(input.value);
            metadata.m_symbolTable.clear();
            if (metadata.m_resolveType == ClosureVar || metadata.m_resolveType == ModuleVar)
                metadata.m_localScopeDepth = input.localScopeDepth;
            if (metadata.m_resolveType == ModuleVar)
                metadata.m_moduleImportSlot = input.moduleImportSlot;
            break;
        }
        case CompileInputKind::GetFromScopeType:
        case CompileInputKind::PutToScopeType: {
            // For every type but the variable kinds the collector reads put_to_scope's union as a StructureID, so a
            // watchpoint set another type left would decode as garbage: the union is cleared, with the operand.
            // get_from_scope holds only the structure, which is cleared the same way.
            auto rewrite = [&](auto& metadata) {
                GetPutInfo linked = metadata.m_getPutInfo;
                metadata.m_getPutInfo = GetPutInfo(linked.resolveMode(), static_cast<ResolveType>(input.value), linked.initializationMode(), linked.ecmaMode());
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(metadata)>, OpGetFromScope::Metadata>)
                    metadata.m_structureID.clear();
                else
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
        }
    }
    return true;
}

void TwinCheck::checkCodeBlockFacts(CodeBlock& twin)
{
    // Both follow from the executable and the source provider the twin shares with the installed CB, and the code depends
    // on the level only through CannotCompile, so the twin compiles with the producer's class.
    auto& bakedFacts = m_view.bakedFacts();
    if ((twin.capabilityLevel() == DFG::CannotCompile) != (bakedFacts.capabilityLevel() == DFG::CannotCompile))
        difference("capability"_s, "the twin's capability class differs from the recorded one"_s);
    if (twin.couldBeTainted() != bakedFacts.couldBeTainted())
        difference("taint"_s, "the twin's taint differs from the recorded one"_s);
}

bool TwinCheck::replay(CodeBlock& twin, ImageRecord& twinRecord, const TwinData& twinData)
{
    for (auto& regeneration : twinData.regenerations) {
        // Recording makes each regeneration name a MathIC with inline code, and the twin lists as many MathICs as the
        // section.
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
                // Every baseline MathIC has its UCB profile; overwriting it here is undone with the others.
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
    // Preparation copies the section's bytes and patches only footprints, so the restored bytes equal the section's
    // outside footprints, and comparing the twin's canonical form with the section compares the restored code with its
    // twin everywhere else. No side reads past a linked size, which a code handle's size can exceed.
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

    // The restored code's facts are the section's. The level is a fact only through its class, CannotCompile or not,
    // since the code depends on nothing finer. Within the class it follows the executable's inlining state (noInline)
    // and the free inlining-cost options, which the producer and the consumer may set differently, in either direction,
    // for the same code. The taint and the scope facts compare exactly.
    auto& sectionFacts = m_view.bakedFacts();
    auto& twinFacts = twinRecord.bakedFacts();
    bool factsEqual = (sectionFacts.capabilityLevel() == DFG::CannotCompile) == (twinFacts.capabilityLevel == DFG::CannotCompile)
        && sectionFacts.couldBeTainted() == twinFacts.couldBeTainted
        && sectionFacts.scopeFactCount() == twinFacts.scopeFacts.size();
    for (unsigned index = 0; factsEqual && index < sectionFacts.scopeFactCount(); ++index)
        factsEqual = sectionFacts.scopeFact(index) == twinFacts.scopeFacts[index];
    if (!factsEqual)
        difference("baked-facts"_s, "the section's baked facts differ from the twin's"_s);
}

void TwinCheck::compareMathICs(const ComparedSide& restored, const BaselineJITCode& twinCode, const ImageRecord& twinRecord)
{
    // Generators need no comparison: JIT::emitMathICFast and the import build them with the same function.
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
    // State the engine cannot recompute: the restored code's rates equal the capture's.
    auto& header = m_view.header();
    if (std::bit_cast<uint64_t>(m_restored.livenessRate()) != header.livenessRateBits || std::bit_cast<uint64_t>(m_restored.fullnessRate()) != header.fullnessRateBits)
        difference("rates"_s, "the restored coverage rates differ from the capture's"_s);
}

void TwinCheck::checkRelocation(const ComparedSide& restored)
{
    // A body this process captured, by this VM or another, has no domain that could have moved between its capture and
    // its import, so the relocation check compares no pair for it; every other comparison still ran.
    if (equalSpans(m_twins.captureProcessToken(), captureProcessToken()))
        return;

    auto context = restored.context(m_vm, m_unlinkedCodeBlock, m_ranks);
    ImageTestHook hook = imageTestHook();
    std::array<bool, numberOfRelocationDomains> forcedDomains { };
    bool forcedOperation = false;
    unsigned producerValueIndex = 0;
    auto compare = [&](const ImageFixup& fixup, ASCIILiteral place) {
        // Capture writes a producer value for every fixup, in the order the fixups are walked here.
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
        // An address inside an object loaded at its link-time address, as Bun's build flags load the engine, cannot move
        // between processes, so it proves nothing about relocation and is skipped.
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

// Registry and hooks.

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

// The check.

void Twins::checkImage(VM& vm, CodeBlock& installed, JSScope* scope, const BaselineJITCode& restored, const ImageSectionsView& view, TwinReport& report)
{
    ImageTwinsInternal::TwinCheck check(vm, installed, restored, view, report);
    check.run(scope);
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
