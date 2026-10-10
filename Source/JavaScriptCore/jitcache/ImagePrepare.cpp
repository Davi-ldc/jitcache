#include "config.h"
#include "ImagePrepare.h"

#if ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BytecodeStructs.h"
#include "CacheableIdentifierInlines.h"
#include "CodeBlock.h"
#include "ExecutableAllocator.h"
#include "GdbJIT.h"
#include "ImageRecord.h"
#include "ImageSupport.h"
#include "ImageTwins.h"
#include "JIT.h"
#include "JITCodeMap.h"
#include "JITInlines.h"
#include "JITMathIC.h"
#include "JSCInlines.h"
#include "JumpTable.h"
#include "MacroAssembler.h"
#include "Options.h"
#include "PerfLog.h"
#include "ProducerBudget.h"
#include "UnlinkedCodeBlock.h"
#include <wtf/Atomics.h>
#include <wtf/SimpleStats.h>
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/StringPrintStream.h>

namespace JSC::JITCache {

namespace ImagePrepareInternal {

// The writers of table 3.2 patch the two architectures JITCache imports on (THREAD's opening); everywhere else start
// returns rejected, so no import reaches them.
#if CPU(X86_64) || (CPU(ARM64) && !CPU(ARM64E))
static constexpr bool hasFixupWriters = true;
#else
static constexpr bool hasFixupWriters = false;
#endif

static const void* addressAt(const void* start, uint32_t offset)
{
    return reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(start) + offset);
}

// A code location at an offset into an allocation, tagged as the native link tags it.
template<PtrTag tag>
static CodeLocationLabel<tag> locationAt(const void* start, uint32_t offset)
{
    return CodeLocationLabel<tag>(CodePtr<tag>::fromUntaggedPtr(const_cast<void*>(addressAt(start, offset))));
}

// Writes the fixup's resolved value with its form's link writer (table 3.2). Each footprint holds its form's canonical
// encoding (step 1, I8), so the writer writes only inside it, apart from the jump islands the ARM64 writers place.
static void writeFixup(void* code, const ImageFixup& fixup, const void* value)
{
    if constexpr (!hasFixupWriters) {
        UNUSED_PARAM(code);
        UNUSED_PARAM(fixup);
        UNUSED_PARAM(value);
        RELEASE_ASSERT_NOT_REACHED();
    } else {
        using Assembler = MacroAssembler::AssemblerType_T;
        AssemblerLabel site(fixup.site);
        void* target = const_cast<void*>(value);
        switch (fixup.form) {
        case FixupForm::Pointer:
            Assembler::linkPointer(code, site, target);
            return;
        case FixupForm::Call:
            Assembler::linkCall(code, site, target);
            return;
        case FixupForm::Jump:
            Assembler::linkJump(code, site, target);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// Steps 8 and 9 for one allocation: each fixup's target resolved in the consumer's context and written at its site.
// skipsNextPatch is the SkipPatch test hook's, which leaves the first footprint of the import canonical (T7).
static void patchFixups(void* code, const ImageFixupArray& fixups, const ResolutionContext& context, bool& skipsNextPatch)
{
    for (size_t index = 0; index < fixups.size(); ++index) {
        ImageFixup fixup = fixups[index];
        if (std::exchange(skipsNextPatch, false)) [[unlikely]]
            continue;
        writeFixup(code, fixup, resolveTarget(context, fixup.target));
    }
}

// Step 2: the MathIC of one entry, created as the emitter creates it, from the instruction the entry names.
template<typename Op>
static void* createMathICFor(MathICHolder& holder, UnlinkedCodeBlock& unlinkedCodeBlock, const JSInstruction* instruction, bool generateFastPathOnRepatch)
{
    ASSERT(instruction->is<Op>());
    auto bytecode = instruction->as<Op>();
    auto* mathIC = [&] {
        if constexpr (std::is_same_v<Op, OpAdd>)
            return holder.addJITAddIC(&unlinkedCodeBlock.binaryArithProfile(bytecode.m_profileIndex));
        else if constexpr (std::is_same_v<Op, OpSub>)
            return holder.addJITSubIC(&unlinkedCodeBlock.binaryArithProfile(bytecode.m_profileIndex));
        else if constexpr (std::is_same_v<Op, OpMul>)
            return holder.addJITMulIC(&unlinkedCodeBlock.binaryArithProfile(bytecode.m_profileIndex));
        else {
            static_assert(std::is_same_v<Op, OpNegate>);
            return holder.addJITNegIC(&unlinkedCodeBlock.unaryArithProfile(bytecode.m_profileIndex));
        }
    }();
    // JIT::emitMathICFast sets the generator of every IC, with inline code or without (section 6.3).
    mathIC->m_generator = JIT::mathICGeneratorFor<Op>(unlinkedCodeBlock, instruction);
    mathIC->m_generateFastPathOnRepatch = generateFastPathOnRepatch;
    return mathIC;
}

static void* createMathIC(MathICHolder& holder, UnlinkedCodeBlock& unlinkedCodeBlock, const ImageMathICEntry& entry)
{
    auto instruction = unlinkedCodeBlock.instructions().at(BytecodeIndex(entry.bytecodeOffset));
    bool generateFastPathOnRepatch = entry.generateFastPathOnRepatch();
    switch (entry.kind) {
    case MathICKind::Add:
        return createMathICFor<OpAdd>(holder, unlinkedCodeBlock, instruction.ptr(), generateFastPathOnRepatch);
    case MathICKind::Sub:
        return createMathICFor<OpSub>(holder, unlinkedCodeBlock, instruction.ptr(), generateFastPathOnRepatch);
    case MathICKind::Mul:
        return createMathICFor<OpMul>(holder, unlinkedCodeBlock, instruction.ptr(), generateFastPathOnRepatch);
    case MathICKind::Negate:
        return createMathICFor<OpNegate>(holder, unlinkedCodeBlock, instruction.ptr(), generateFastPathOnRepatch);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Step 12 for one MathIC with inline code: the four locations the emitter's link task sets, and the snippet the last
// regeneration left, as m_code over its own allocation.
template<typename MathIC>
static void setMathICCodeOf(MathIC& mathIC, const ImageMathICEntry& entry, const void* imageStart, RefPtr<ExecutableMemoryHandle>&& snippet)
{
    mathIC.m_inlineStart = locationAt<JSInternalPtrTag>(imageStart, entry.inlineStart);
    mathIC.m_inlineEnd = locationAt<JSInternalPtrTag>(imageStart, entry.inlineEnd);
    mathIC.m_slowPathStartLocation = locationAt<JSInternalPtrTag>(imageStart, entry.slowPathStart);
    mathIC.m_slowPathCallLocation = locationAt<JSInternalPtrTag>(imageStart, entry.slowPathCall);
    if (snippet)
        mathIC.m_code = MacroAssemblerCodeRef<JITStubRoutinePtrTag>(snippet.releaseNonNull());
}

static void setMathICCode(void* mathIC, const ImageMathICEntry& entry, const void* imageStart, RefPtr<ExecutableMemoryHandle>&& snippet)
{
    switch (entry.kind) {
    case MathICKind::Add:
        setMathICCodeOf(*static_cast<JITAddIC*>(mathIC), entry, imageStart, WTF::move(snippet));
        return;
    case MathICKind::Sub:
        setMathICCodeOf(*static_cast<JITSubIC*>(mathIC), entry, imageStart, WTF::move(snippet));
        return;
    case MathICKind::Mul:
        setMathICCodeOf(*static_cast<JITMulIC*>(mathIC), entry, imageStart, WTF::move(snippet));
        return;
    case MathICKind::Negate:
        setMathICCodeOf(*static_cast<JITNegIC*>(mathIC), entry, imageStart, WTF::move(snippet));
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Vector<ImageFixup> fixupsOf(const ImageFixupArray& fixups)
{
    Vector<ImageFixup> result;
    result.reserveInitialCapacity(fixups.size());
    for (size_t index = 0; index < fixups.size(); ++index)
        result.append(fixups[index]);
    return result;
}

// Step 14: the record THREAD Capture describes, rebuilt from the sections, so that this image's later MathIC
// regenerations record and a capture of it reproduces the imported sections (I9). Its exact bytes are charged before
// anything is allocated, every container sized from the section's counts; a refusal gives no record. In twins builds the
// record also takes the twin data of the twins section, the original compilation's seeds and inputs and every
// regeneration logged since, to which this image's own regenerations append (section 11.1).
static std::unique_ptr<ImageRecord> rebuildRecord(ProducerBudget& budget, const ImageSectionsView& view, const void* imageStart, std::span<void* const> mathICs, std::span<const void* const> snippetStarts)
{
    const auto& header = view.header();
    const auto& bakedFactsView = view.bakedFacts();
    size_t snippetFixupCount = 0;
    for (unsigned index = 0; index < header.mathICCount; ++index)
        snippetFixupCount += view.mathIC(index).snippetFixupCount;
    size_t recordBytes = ImageRecord::storageBytes(header.fixupCount, header.mathICCount, snippetFixupCount, bakedFactsView.scopeFactCount());
#if ENABLE(JITCACHE_TWINS)
    size_t twinBytes = view.twins().twinDataStorageBytes();
#else
    size_t twinBytes = 0;
#endif
    if (!budget.tryCharge(recordBytes + twinBytes))
        return nullptr;

    Vector<MathICRecord> mathICRecords;
    mathICRecords.reserveInitialCapacity(header.mathICCount);
    for (unsigned index = 0; index < header.mathICCount; ++index) {
        auto entry = view.mathIC(index);
        // The slow call's Operation fixup sits at its pointer placeholder (I7); a MathIC without inline code has none.
        mathICRecords.append(MathICRecord {
            .kind = entry.kind,
            .bytecodeIndex = BytecodeIndex(entry.bytecodeOffset),
            .mathIC = mathICs[index],
            .slowCallPointerSite = entry.hasInlineCode() ? farCallPointerSite(entry.slowPathCall) : std::nullopt,
            .snippet = std::nullopt,
        });
    }
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        mathICRecords[index].snippet = SnippetProvenance {
            .start = snippetStarts[index],
            .size = static_cast<uint32_t>(snippet.code.size()),
            .fixups = fixupsOf(snippet.fixups),
        };
    });

    BakedFacts bakedFacts;
    bakedFacts.capabilityLevel = bakedFactsView.capabilityLevel();
    bakedFacts.couldBeTainted = bakedFactsView.couldBeTainted();
    bakedFacts.scopeFacts.reserveInitialCapacity(bakedFactsView.scopeFactCount());
    for (unsigned index = 0; index < bakedFactsView.scopeFactCount(); ++index)
        bakedFacts.scopeFacts.append(bakedFactsView.scopeFact(index));

    auto record = makeUnique<ImageRecord>(Ref { budget }, recordBytes, imageStart, header.codeSize, fixupsOf(view.fixups()), WTF::move(mathICRecords), WTF::move(bakedFacts));
#if ENABLE(JITCACHE_TWINS)
    record->adoptTwinData(view.twins().twinData(), twinBytes);
#endif
    return record;
}

} // namespace ImagePrepareInternal

PreparedImage::PreparedImage(Ref<BaselineJITCode>&& code, Ref<ExecutableMemoryHandle>&& imageMemory)
    : m_code(WTF::move(code))
    , m_imageMemory(WTF::move(imageMemory))
{
}

PreparedImage::PreparedImage(PreparedImage&&) = default;

PreparedImage::~PreparedImage() = default;

const BaselineJITCode& PreparedImage::code() const
{
    RELEASE_ASSERT(m_code);
    return *m_code;
}

Ref<BaselineJITCode> PreparedImage::commit(VM& vm, CodeBlock& installing) &&
{
    RELEASE_ASSERT(m_code && m_imageMemory);
    Ref<BaselineJITCode> code = m_code.releaseNonNull();
    Ref<ExecutableMemoryHandle> imageMemory = m_imageMemory.releaseNonNull();

    // As JIT::finalizeOnMainThread samples a native compilation: the handle's size, which can exceed the linked size (N20).
    vm.machineCodeBytesPerBytecodeWordForBaselineJIT->add(static_cast<double>(code->size()) / static_cast<double>(installing.unlinkedCodeBlock()->instructionsSize()));
    // BaselineJITPlan::finalize fences before installCode, since the code may have been written on another thread.
    WTF::crossModifyingCodeFence();

    if (Options::useJITDump() || Options::useGdbJITInfo()) [[unlikely]] {
        // The name LinkBuffer::logJITCodeForJITDump gives a baseline compilation of this CB.
        StringPrintStream out;
        out.print("JSC-Baseline: ");
        installing.dumpSimpleName(out);
        auto name = out.toCString();
        MacroAssemblerCodeRef<LinkBufferPtrTag> codeRef(WTF::move(imageMemory));
        if (Options::useGdbJITInfo())
            GdbJIT::log(name, codeRef);
        if (Options::useJITDump())
            PerfLog::log(name, codeRef);
    }
    return code;
}

std::expected<PreparedImage, PrepareFailure> prepareImage(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, const ImageSectionsView& view, ProducerBudget* budget, bool strict)
{
    using namespace ImagePrepareInternal;
    const auto& header = view.header();

    // Step 1. Validation is the glue's, under strict only (R-INT-3); every footprint holds its form's canonical encoding in
    // either mode (I8), which the writers of step 9 rely on.
    if (!strict)
        assertImageSectionsAgainst(view, unlinkedCodeBlock);

    // Step 2: every MathIC, in MathIC index order, in a holder the code adopts at step 13.
    MathICHolder mathICHolder;
    Vector<void*> mathICs;
    mathICs.reserveInitialCapacity(header.mathICCount);
    for (unsigned index = 0; index < header.mathICCount; ++index)
        mathICs.append(createMathIC(mathICHolder, unlinkedCodeBlock, view.mathIC(index)));

    // Step 3: the switch tables at the UCB's counts, each table's offsets sized from the section. Moving the outer vectors
    // into the code at step 13 moves neither their entries nor the offsets' storage, so their addresses are final here.
    FixedVector<SimpleJumpTable> simpleSwitchTables(unlinkedCodeBlock.numberOfUnlinkedSwitchJumpTables());
    ASSERT(simpleSwitchTables.size() == header.simpleSwitchTableCount);
    view.forEachSimpleSwitchTable([&](unsigned index, const ImageSimpleSwitchTable& table) {
        if (table.offsets.size())
            simpleSwitchTables[index].m_ctiOffsets = FixedVector<CodeLocationLabel<JSSwitchPtrTag>>(table.offsets.size());
    });
    FixedVector<StringJumpTable> stringSwitchTables(unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables());
    ASSERT(stringSwitchTables.size() == header.stringSwitchTableCount);
    view.forEachStringSwitchTable([&](unsigned index, const ImageOffsetArray& offsets) {
        stringSwitchTables[index].m_ctiOffsets = FixedVector<CodeLocationLabel<JSSwitchPtrTag>>(offsets.size());
    });

    // Step 4: the consumer's own rank order of each inline string switch's keys (section 3.7).
    StringSwitchRanks ranks = rankStringSwitches(unlinkedCodeBlock);

    // Step 5. A handle can be larger than requested (N20); its slack stays unwritten, as natively.
    RefPtr<ExecutableMemoryHandle> image = ExecutableAllocator::singleton().allocate(header.codeSize, JITCompilationCanFail);
    if (!image)
        return std::unexpected(PrepareFailure { PrepareOutcome::ExecutableMemoryExhausted, ImageCheck::None });
    Vector<RefPtr<ExecutableMemoryHandle>> snippets(header.mathICCount); // by MathIC index, null without a snippet
    bool allocatedSnippets = true;
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        if (allocatedSnippets) {
            snippets[index] = ExecutableAllocator::singleton().allocate(snippet.code.size(), JITCompilationCanFail);
            allocatedSnippets = !!snippets[index];
        }
    });
    if (!allocatedSnippets)
        return std::unexpected(PrepareFailure { PrepareOutcome::ExecutableMemoryExhausted, ImageCheck::None });
    void* imageStart = image->start().untaggedPtr();
    Vector<const void*> snippetStarts(header.mathICCount, [&](size_t index) -> const void* {
        return snippets[index] ? snippets[index]->start().untaggedPtr() : nullptr;
    });

    // Step 6: one copy each of the image's and the snippets' linked bytes.
    performJITMemcpy<jitMemcpyRepatch>(imageStart, view.code().data(), header.codeSize);
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        performJITMemcpy<jitMemcpyRepatch>(const_cast<void*>(snippetStarts[index]), snippet.code.data(), snippet.code.size());
    });

    // Step 7: the tables' code locations, as JIT::link fills them, before any SwitchStringRankCase target reads them.
    view.forEachSimpleSwitchTable([&](unsigned index, const ImageSimpleSwitchTable& table) {
        auto& linkedTable = simpleSwitchTables[index];
        linkedTable.m_ctiDefault = locationAt<JSSwitchPtrTag>(imageStart, table.defaultOffset);
        for (size_t entry = 0; entry < table.offsets.size(); ++entry)
            linkedTable.m_ctiOffsets[entry] = locationAt<JSSwitchPtrTag>(imageStart, table.offsets[entry]);
    });
    view.forEachStringSwitchTable([&](unsigned index, const ImageOffsetArray& offsets) {
        auto& linkedTable = stringSwitchTables[index];
        for (size_t entry = 0; entry < offsets.size(); ++entry)
            linkedTable.m_ctiOffsets[entry] = locationAt<JSSwitchPtrTag>(imageStart, offsets[entry]);
    });

    // Steps 8 and 9: every target resolved in the consumer's context and patched with its form's writer, the snippets
    // first. Resolving a support target may generate it, as the VM's first use of it would. A SwitchTableBase target is a
    // dense table's storage, which JIT::emit_op_switch_imm and emit_op_switch_char move into a register.
    Vector<const void*> switchTableBases(simpleSwitchTables.size(), [&](size_t index) -> const void* {
        auto& offsets = simpleSwitchTables[index].m_ctiOffsets;
        return offsets.isEmpty() ? nullptr : offsets.span().data();
    });
    ResolutionContext context {
        .vm = vm,
        .ucb = &unlinkedCodeBlock,
        .imageStart = imageStart,
        .mathICs = mathICs.span(),
        .switchTableBases = switchTableBases.span(),
        .stringSwitchTables = stringSwitchTables.span(),
        .snippetStarts = snippetStarts.span(),
        .ranks = &ranks,
    };
    bool skipsNextPatch = false;
#if ENABLE(JITCACHE_TWINS)
    skipsNextPatch = imageTestHook() == ImageTestHook::SkipPatch;
#endif
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        patchFixups(const_cast<void*>(snippetStarts[index]), snippet.fixups, context, skipsNextPatch);
    });
    patchFixups(imageStart, view.fixups(), context, skipsNextPatch);

    // Step 10: S5, the read-back after patching: each patched footprint decodes, islands followed, to the consumer's
    // resolution of its target. It guards an installation, so a failure is invalid material.
    if (strict) {
        bool readsBack = true;
        view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
            readsBack = readsBack && fixupsReachTargets(snippetStarts[index], snippet.code.size(), snippet.fixups, context);
        });
        readsBack = readsBack && fixupsReachTargets(imageStart, header.codeSize, view.fixups(), context);
        if (!readsBack)
            return std::unexpected(PrepareFailure { PrepareOutcome::InvalidMaterial, ImageCheck::S5 });
    }

    // Step 11, over the linked bytes, as LinkBuffer::performFinalization flushes.
    MacroAssembler::cacheFlush(imageStart, header.codeSize);
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        MacroAssembler::cacheFlush(const_cast<void*>(snippetStarts[index]), snippet.code.size());
    });

    // Step 12. A MathIC without inline code keeps the null locations and empty m_code it was created with.
    for (unsigned index = 0; index < header.mathICCount; ++index) {
        auto entry = view.mathIC(index);
        if (entry.hasInlineCode())
            setMathICCode(mathICs[index], entry, imageStart, WTF::move(snippets[index]));
    }

    // Step 13: the BaselineJITCode, as JIT::link builds it, with every code pointer at its section offset.
    auto arityEntry = CodePtr<JSEntryPtrTag>::fromUntaggedPtr(const_cast<void*>(addressAt(imageStart, header.arityEntryOffset)));
    Ref<BaselineJITCode> jitCode = adoptRef(*new BaselineJITCode(MacroAssemblerCodeRef<JSEntryPtrTag>(Ref { *image }), arityEntry));

    jitCode->m_unlinkedCalls = FixedVector<BaselineUnlinkedCallLinkInfo>(header.callCount);
    for (unsigned index = 0; index < header.callCount; ++index) {
        auto entry = view.call(index);
        auto& call = jitCode->m_unlinkedCalls[index];
        call.bytecodeIndex = entry.bytecodeIndex;
        call.doneLocation = locationAt<JSInternalPtrTag>(imageStart, entry.doneLocation);
    }

    jitCode->m_unlinkedPropertyInlineCaches = FixedVector<BaselineUnlinkedPropertyInlineCache>(header.moldCount);
    for (unsigned index = 0; index < header.moldCount; ++index) {
        auto entry = view.mold(index);
        auto& mold = jitCode->m_unlinkedPropertyInlineCaches[index];
        mold.accessType = entry.accessType;
        mold.preconfiguredCacheType = entry.preconfiguredCacheType;
        mold.propertyIsInt32 = !!(entry.flags & ImageMoldFlags::propertyIsInt32);
        mold.propertyIsString = !!(entry.flags & ImageMoldFlags::propertyIsString);
        mold.propertyIsSymbol = !!(entry.flags & ImageMoldFlags::propertyIsSymbol);
        mold.prototypeIsKnownObject = !!(entry.flags & ImageMoldFlags::prototypeIsKnownObject);
        mold.canBeMegamorphic = !!(entry.flags & ImageMoldFlags::canBeMegamorphic);
        // Either creation carries the same bits for the same impl (section 9, step 3).
        switch (entry.identifierKind) {
        case MoldIdentifierKind::None:
            mold.m_identifier = CacheableIdentifier();
            break;
        case MoldIdentifierKind::UCBIdentifier:
            mold.m_identifier = CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(&unlinkedCodeBlock, unlinkedCodeBlock.identifier(entry.identifier));
            break;
        case MoldIdentifierKind::ImmortalName:
            mold.m_identifier = CacheableIdentifier::createFromImmortalIdentifier(immortalNameImpl(vm, static_cast<ImmortalName>(entry.identifier)));
            break;
        }
        mold.doneLocation = locationAt<JSInternalPtrTag>(imageStart, entry.doneLocation);
        mold.bytecodeIndex = entry.bytecodeIndex;
    }

    jitCode->m_switchJumpTables = WTF::move(simpleSwitchTables);
    jitCode->m_stringSwitchJumpTables = WTF::move(stringSwitchTables);

    // The code map pairs the UCB's instruction starts, in bytecode order, with the section's offsets (region 7).
    auto codeMap = view.codeMap();
    JITCodeMapBuilder codeMapBuilder;
    size_t codeMapIndex = 0;
    for (const auto& instruction : unlinkedCodeBlock.instructions()) {
        ASSERT(codeMapIndex < codeMap.size());
        codeMapBuilder.append(instruction.index(), locationAt<JSEntryPtrTag>(imageStart, codeMap[codeMapIndex++]));
    }
    ASSERT(codeMapIndex == codeMap.size());
    jitCode->m_jitCodeMap = codeMapBuilder.finalize();

    Vector<JITConstantPool::Value> constants;
    constants.reserveInitialCapacity(header.constantPoolCount);
    for (unsigned index = 0; index < header.constantPoolCount; ++index) {
        auto entry = view.constantPoolEntry(index);
        auto type = entry.type == ImageConstantType::FunctionDecl ? JITConstantPool::Type::FunctionDecl : JITConstantPool::Type::FunctionExpr;
        constants.append(JITConstantPool::Value { std::bit_cast<void*>(static_cast<uintptr_t>(entry.index)), type });
    }
    jitCode->m_constantPool = JITConstantPool(WTF::move(constants));

    jitCode->adoptMathICs(mathICHolder);
    jitCode->m_isShareable = true;
    jitCode->setLivenessRate(header.livenessRate());
    jitCode->setFullnessRate(header.fullnessRate());

    // Step 14.
    if (budget)
        jitCode->m_jitCacheImageRecord = rebuildRecord(*budget, view, imageStart, mathICs.span(), snippetStarts.span());

    return PreparedImage(WTF::move(jitCode), image.releaseNonNull());
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
