#include "config.h"
#include "jitcache/image/ImageSupport.h"

#if ENABLE(JIT)

#include "CPU.h"
#include "CallMode.h"
#include "InlineCacheCompiler.h"
#include "JIT.h"
#include "JITThunks.h"
#include "JSCConfig.h"
#include "JSCInlines.h"
#include "JSSentinel.h"
#include "JSTemplateObjectDescriptor.h"
#include "LLIntEntrypoint.h"
#include "Options.h"
#include "PropertyInlineCache.h"
#include "SlowPathFunction.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <wtf/HashMap.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC::JITCache {

namespace ImageSupportInternal {

// x86_64 instructions start at any byte; ARM64 instructions are four-byte words.
static constexpr uintptr_t instructionAlignment = isARM64() ? 4 : 1;

static uintptr_t anchorAddress()
{
    return reinterpret_cast<uintptr_t>(&codeSymbolAnchor);
}

// The engine's text segment (EngineObject.h), copied at the first use. Only Linux targets carry code symbols; elsewhere
// the segment is empty, so no symbol is valid and a recording compilation that makes a far call is
// Unrecordable(ForeignCodeSymbol).
static const LoadedSegment& engineTextSegment()
{
    static const LoadedSegment segment = [] {
        LoadedSegment engineText = loadedObjects().engineText;
#if OS(LINUX)
        RELEASE_ASSERT(engineText.start < engineText.end);
#endif
        return engineText;
    }();
    return segment;
}

static UnlinkedCodeBlock& unlinkedCodeBlockOf(const ResolutionContext& context)
{
    ASSERT(context.ucb);
    return *context.ucb;
}

static JSValue constantOf(UnlinkedCodeBlock& unlinkedCodeBlock, uint32_t index)
{
    ASSERT(index < unlinkedCodeBlock.constantRegisters().size());
    return unlinkedCodeBlock.constantRegisters()[index].get();
}

[[maybe_unused]] static bool isOwnedConstantCell(UnlinkedCodeBlock& unlinkedCodeBlock, uint32_t index)
{
    if (unlinkedCodeBlock.constantSourceCodeRepresentation(index) != SourceCodeRepresentation::Other)
        return false;
    JSValue value = constantOf(unlinkedCodeBlock, index);
    if (!value || !value.isCell())
        return false;
    JSCell* cell = value.asCell();
    return !cell->inherits<SymbolTable>() && !cell->inherits<JSTemplateObjectDescriptor>();
}

} // namespace ImageSupportInternal

std::optional<CodeSymbol> CodeSymbol::of(const void* function)
{
    auto offset = static_cast<int64_t>(reinterpret_cast<uintptr_t>(function) - ImageSupportInternal::anchorAddress());
    if (!isValid(offset))
        return std::nullopt;
    return CodeSymbol { offset };
}

const void* CodeSymbol::address() const
{
    RELEASE_ASSERT(isValid(offset));
    return reinterpret_cast<const void*>(ImageSupportInternal::anchorAddress() + static_cast<uintptr_t>(offset));
}

bool CodeSymbol::isValid(int64_t offset)
{
    auto& segment = ImageSupportInternal::engineTextSegment();
    uintptr_t address = ImageSupportInternal::anchorAddress() + static_cast<uintptr_t>(offset);
    return segment.contains(address) && !(address % ImageSupportInternal::instructionAlignment);
}

const void* vmAddress(VM& vm, VMAddress entry)
{
    switch (entry) {
    case VMAddress::VM:
        return &vm;
    case VMAddress::SoftStackLimit:
        return vm.addressOfSoftStackLimit();
    case VMAddress::TrapBits:
        return vm.traps().trapBitsAddress();
    case VMAddress::Exception:
        return vm.addressOfException();
    case VMAddress::TopEntryFrame:
        return &vm.topEntryFrame;
    case VMAddress::TargetMachinePCForThrow:
        return &vm.targetMachinePCForThrow;
    case VMAddress::TopCallFrame:
        return &vm.topCallFrame;
    case VMAddress::MightBeExecutingTaintedCode:
        return vm.addressOfMightBeExecutingTaintedCode();
    case VMAddress::BarrierThreshold:
        return vm.heap.addressOfBarrierThreshold();
    case VMAddress::MutatorShouldBeFenced:
        return vm.heap.addressOfMutatorShouldBeFenced();
    case VMAddress::SyncResumeCallCache:
        return &vm.syncResumeCallCache();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<VMAddress> vmAddressFor(VM& vm, const void* address)
{
    for (uint8_t value = 1; value <= numberOfVMAddresses; ++value) {
        auto entry = static_cast<VMAddress>(value);
        if (vmAddress(vm, entry) == address)
            return entry;
    }
    return std::nullopt;
}

JSCell* vmCell(VM& vm, VMCell entry)
{
    switch (entry) {
    case VMCell::EmptyString:
        return jsEmptyString(vm);
    case VMCell::SmallStringsSentinel:
        return vm.smallStrings.sentinelString();
    case VMCell::FastArraySentinel:
        return vm.fastArraySentinel();
    case VMCell::FastStringSentinel:
        return vm.fastStringSentinel();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isValidCommonThunk(uint32_t value)
{
    return value < numberOfCommonThunkIDs;
}

bool isValidBaselineThunk(uint32_t value)
{
    if (value < static_cast<uint32_t>(BaselineThunk::OpEnterHandler))
        return false;
#if ASSERT_ENABLED
    return value <= static_cast<uint32_t>(BaselineThunk::ConsistencyCheck);
#else
    return value < static_cast<uint32_t>(BaselineThunk::ConsistencyCheck);
#endif
}

bool isValidAccessType(uint32_t value)
{
    return value < numberOfAccessTypes;
}

bool isValidCallMode(uint32_t value)
{
    if (value > std::numeric_limits<uint8_t>::max())
        return false;
    // Without a default, the switch stops building when the engine adds a mode.
    switch (static_cast<CallMode>(value)) {
    case CallMode::Regular:
    case CallMode::Tail:
    case CallMode::Construct:
        return true;
    }
    return false;
}

bool isValidVMAddress(uint32_t value)
{
    return value >= 1 && value <= numberOfVMAddresses;
}

bool isValidVMCell(uint32_t value)
{
    return value >= 1 && value <= numberOfVMCells;
}

bool hasInlineStringSwitch(size_t keyCount)
{
    return keyCount && keyCount <= Options::maximumInlineStringSwitchCaseCount();
}

StringSwitchRanks rankStringSwitches(const UnlinkedCodeBlock& unlinkedCodeBlock)
{
    StringSwitchRanks ranks;
    size_t tableCount = unlinkedCodeBlock.numberOfUnlinkedStringSwitchJumpTables();
    ranks.tables = Vector<Vector<StringSwitchRank>>(tableCount);
    for (size_t tableIndex = 0; tableIndex < tableCount; ++tableIndex) {
        auto& table = unlinkedCodeBlock.unlinkedStringSwitchJumpTable(static_cast<int>(tableIndex));
        if (!hasInlineStringSwitch(table.m_offsetTable.size()))
            continue;
        auto& keys = ranks.tables[tableIndex];
        keys.reserveInitialCapacity(table.m_offsetTable.size());
        for (auto& entry : table.m_offsetTable) {
            // BytecodeGenerator::endSwitch makes every key an atom, which JIT::emit_op_switch_string asserts too.
            ASSERT(entry.key->isAtom());
            keys.append(StringSwitchRank { static_cast<UniquedStringImpl*>(entry.key.get()), entry.value.m_indexInTable });
        }
        // BinarySwitch::build sorts its IntPtr cases by signed value.
        std::sort(keys.begin(), keys.end(), [](const StringSwitchRank& a, const StringSwitchRank& b) {
            return std::bit_cast<intptr_t>(a.key) < std::bit_cast<intptr_t>(b.key);
        });
    }
    return ranks;
}

WTF_MAKE_TZONE_ALLOCATED_IMPL(SupportTargetTable);

CodePtr<NoPtrTag> SupportTargetTable::resolve(VM& vm, const ImageTarget& target)
{
    static_assert(numberOfAccessTypeSlots == numberOfAccessTypes);
    CodePtr<NoPtrTag>* slot = nullptr;
    switch (target.kind) {
    case TargetKind::BaselineThunk:
        ASSERT(isValidBaselineThunk(target.a));
        slot = &m_baselineThunks[target.a];
        break;
    case TargetKind::InlineCacheSlowPathThunk:
        ASSERT(isValidAccessType(target.a));
        slot = &m_inlineCacheSlowPathThunks[target.a];
        break;
    case TargetKind::SlowPathThunk:
        slot = &m_slowPathThunks.add(reinterpret_cast<uintptr_t>(CodeSymbol { target.payload }.address()), CodePtr<NoPtrTag> { }).iterator->value;
        break;
    default:
        return resolveSupport(vm, target);
    }
    if (!*slot)
        *slot = resolveSupport(vm, target);
    return *slot;
}

const void* resolveTarget(const ResolutionContext& context, const ImageTarget& target)
{
    switch (target.kind) {
    case TargetKind::Operation:
    case TargetKind::CommonThunk:
    case TargetKind::BaselineThunk:
    case TargetKind::SlowPathThunk:
    case TargetKind::InlineCacheSlowPathThunk:
    case TargetKind::VirtualCallThunk:
    case TargetKind::ProcessThunk:
        if (context.supportTargets)
            return context.supportTargets->resolve(context.vm, target).taggedPtr();
        return resolveSupport(context.vm, target).taggedPtr();
    case TargetKind::VMAddress:
        ASSERT(isValidVMAddress(target.a));
        return vmAddress(context.vm, static_cast<VMAddress>(target.a));
    case TargetKind::VMCell:
        ASSERT(isValidVMCell(target.a));
        return vmCell(context.vm, static_cast<VMCell>(target.a));
    case TargetKind::StructureIDBase:
        return reinterpret_cast<const void*>(structureIDBase());
    case TargetKind::UCBConstantCell: {
        auto& unlinkedCodeBlock = ImageSupportInternal::unlinkedCodeBlockOf(context);
        ASSERT(ImageSupportInternal::isOwnedConstantCell(unlinkedCodeBlock, target.a));
        return ImageSupportInternal::constantOf(unlinkedCodeBlock, target.a).asCell();
    }
    case TargetKind::UCBConstantAtom: {
        JSValue constant = ImageSupportInternal::constantOf(ImageSupportInternal::unlinkedCodeBlockOf(context), target.a);
        ASSERT(constant.isString() && asString(constant)->tryGetValueImpl() && asString(constant)->tryGetValueImpl()->isAtom());
        return asString(constant)->tryGetValueImpl();
    }
    case TargetKind::UCBIdentifier: {
        auto& unlinkedCodeBlock = ImageSupportInternal::unlinkedCodeBlockOf(context);
        ASSERT(target.a < unlinkedCodeBlock.numberOfIdentifiers());
        return unlinkedCodeBlock.identifier(target.a).impl();
    }
    case TargetKind::UCBBinaryArithProfile: {
        auto& unlinkedCodeBlock = ImageSupportInternal::unlinkedCodeBlockOf(context);
        ASSERT(target.a < unlinkedCodeBlock.numberOfBinaryArithProfiles());
        return &unlinkedCodeBlock.binaryArithProfile(target.a);
    }
    case TargetKind::UCBUnaryArithProfile: {
        auto& unlinkedCodeBlock = ImageSupportInternal::unlinkedCodeBlockOf(context);
        ASSERT(target.a < unlinkedCodeBlock.numberOfUnaryArithProfiles());
        return &unlinkedCodeBlock.unaryArithProfile(target.a);
    }
    case TargetKind::SwitchStringRankAtom:
    case TargetKind::SwitchStringRankCase: {
        // Only a table with an inline tree has ranks, so a rank below its table's size is below the key count and the
        // count is at most maximumInlineStringSwitchCaseCount.
        ASSERT(context.ranks && target.a < context.ranks->tables.size() && target.b < context.ranks->tables[target.a].size());
        auto& rank = context.ranks->tables[target.a][target.b];
        if (target.kind == TargetKind::SwitchStringRankAtom)
            return rank.key;
        // The location the string table holds for the rank's key: JIT::link filled it in the producer, and preparation
        // fills it in the consumer before anything resolves.
        ASSERT(target.a < context.stringSwitchTables.size());
        auto& offsets = context.stringSwitchTables[target.a].m_ctiOffsets;
        ASSERT(rank.indexInTable < offsets.size());
        return offsets[rank.indexInTable].untaggedPtr();
    }
    case TargetKind::MathIC:
        // Recording names only MathICs with inline code; the span holds every MathIC of the image.
        ASSERT(target.a < context.mathICs.size() && context.mathICs[target.a]);
        return context.mathICs[target.a];
    case TargetKind::SwitchTableBase:
        // A dense table's storage, which exists once the table is sized; recording names only a dense table.
        ASSERT(target.a < context.switchTableBases.size() && context.switchTableBases[target.a]);
        return context.switchTableBases[target.a];
    case TargetKind::ImageOffset:
        ASSERT(context.imageStart);
        return static_cast<const uint8_t*>(context.imageStart) + target.a;
    case TargetKind::SnippetEntry:
        ASSERT(target.a < context.snippetStarts.size() && context.snippetStarts[target.a]);
        return context.snippetStarts[target.a];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

CodePtr<NoPtrTag> resolveSupport(VM& vm, const ImageTarget& target)
{
    switch (target.kind) {
    case TargetKind::Operation:
        return CodePtr<NoPtrTag>::fromUntaggedPtr(const_cast<void*>(CodeSymbol { target.payload }.address()));
    case TargetKind::CommonThunk:
        ASSERT(isValidCommonThunk(target.a));
        return vm.getCTIStub(static_cast<CommonJITThunkID>(target.a)).retaggedCode<NoPtrTag>();
    case TargetKind::BaselineThunk:
        ASSERT(isValidBaselineThunk(target.a));
        return vm.getCTIStub(JIT::baselineThunkGenerator(static_cast<BaselineThunk>(target.a))).retaggedCode<NoPtrTag>();
    case TargetKind::SlowPathThunk: {
        // A slow-path function is only data to the lookup, which embeds it in the thunk it generates.
        auto function = std::bit_cast<SlowPathFunction>(CodeSymbol { target.payload }.address());
        return vm.jitStubs->ctiSlowPathFunctionStub(vm, function).retaggedCode<NoPtrTag>();
    }
    case TargetKind::InlineCacheSlowPathThunk:
        ASSERT(isValidAccessType(target.a));
        return InlineCacheCompiler::generateSlowPathCode(vm, static_cast<AccessType>(target.a)).retaggedCode<NoPtrTag>();
    case TargetKind::VirtualCallThunk:
        ASSERT(isValidCallMode(target.a));
        return vm.getCTIVirtualCall(static_cast<CallMode>(target.a)).retaggedCode<NoPtrTag>();
    case TargetKind::ProcessThunk:
        switch (static_cast<ProcessThunk>(target.a)) {
        case ProcessThunk::DefaultCall:
            return LLInt::defaultCall().retaggedCode<NoPtrTag>();
        case ProcessThunk::ArityFixup:
            return LLInt::arityFixup().retagged<NoPtrTag>();
        }
        break;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
