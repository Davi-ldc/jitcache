#include "config.h"
#include "BakedFacts.h"

#if ENABLE(JIT)

#include "ByteFields.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "ImageSection.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

// Besides the declarations of its header, the section's field offsets, the rules ImageCheck::BakedFacts applies to its
// header and entries, and the test of one scope fact against a newborn CB's metadata.

namespace JSC::JITCache {

namespace BakedFactsInternal {

// Header fields.
static constexpr size_t levelOffset = 0;
static constexpr size_t taintOffset = 1;
static constexpr size_t headerReservedOffset = 2;
static constexpr size_t countOffset = 4;

// Entry fields.
static constexpr size_t entryBytecodeOffset = 0;
static constexpr size_t entryOpcodeOffset = 4;
static constexpr size_t entryResolveTypeOffset = 5;
static constexpr size_t entryReservedOffset = 6;
static constexpr size_t entryDepthOffset = 8;
static constexpr size_t entryImportSlotOffset = 12;
static_assert(entryImportSlotOffset + sizeof(uint32_t) == bakedFactsEntrySize);

static_assert(sizeof(DFG::CapabilityLevel) == 1);

// The resolve_scope kinds whose code bakes a depth or an import slot: one of a closure variable walks its depth, and one
// of a module variable walks its depth and then loads its import slot.
static bool mayHaveDepth(ScopeOpcode opcode, ResolveType resolveType)
{
    return opcode == ScopeOpcode::ResolveScope && (resolveType == ClosureVar || resolveType == ModuleVar);
}

static bool mayHaveImportSlot(ScopeOpcode opcode, ResolveType resolveType)
{
    return opcode == ScopeOpcode::ResolveScope && resolveType == ModuleVar;
}

static bool isValidCapabilityLevel(uint8_t level)
{
    // The compilation reads its level through capabilityLevel(), which never returns CapabilityLevelNotSet. Without a
    // default, the switch stops building when the engine adds a level.
    switch (static_cast<DFG::CapabilityLevel>(level)) {
    case DFG::CannotCompile:
    case DFG::CanCompile:
    case DFG::CanCompileAndInline:
        return true;
    case DFG::CapabilityLevelNotSet:
        return false;
    }
    return false;
}

static bool isValidScopeOpcode(uint8_t opcode)
{
    return opcode >= static_cast<uint8_t>(ScopeOpcode::ResolveScope) && opcode <= static_cast<uint8_t>(ScopeOpcode::PutToScope);
}

static std::span<const uint8_t> entryAt(std::span<const uint8_t> section, unsigned index)
{
    return section.subspan(bakedFactsHeaderSize + static_cast<size_t>(index) * bakedFactsEntrySize, bakedFactsEntrySize);
}

// ImageCheck::BakedFacts, on a section whose header and counted entries lie inside it: the exact size, known values,
// zero reserved fields, facts strictly increasing by bytecode offset, each a shape recording writes, with a depth only
// on a ClosureVar or ModuleVar resolve_scope and an import slot only on a ModuleVar one.
static bool passesBakedFacts(std::span<const uint8_t> section)
{
    uint32_t count = readField<uint32_t>(section, countOffset);
    if (section.size() != bakedFactsHeaderSize + static_cast<size_t>(count) * bakedFactsEntrySize)
        return false;
    if (!isValidCapabilityLevel(section[levelOffset]) || section[taintOffset] > 1)
        return false;
    if (readField<uint16_t>(section, headerReservedOffset))
        return false;

    std::optional<uint32_t> previousOffset;
    for (unsigned index = 0; index < count; ++index) {
        auto entry = entryAt(section, index);
        uint32_t bytecodeOffset = readField<uint32_t>(entry, entryBytecodeOffset);
        if (previousOffset && bytecodeOffset <= *previousOffset)
            return false;
        previousOffset = bytecodeOffset;

        uint8_t opcodeByte = entry[entryOpcodeOffset];
        if (!isValidScopeOpcode(opcodeByte))
            return false;
        auto opcode = static_cast<ScopeOpcode>(opcodeByte);
        auto resolveType = static_cast<ResolveType>(entry[entryResolveTypeOffset]);
        if (!isBakedScopeShape(opcode, resolveType))
            return false;
        if (readField<uint16_t>(entry, entryReservedOffset))
            return false;
        if (readField<uint32_t>(entry, entryDepthOffset) && !mayHaveDepth(opcode, resolveType))
            return false;
        if (readField<uint32_t>(entry, entryImportSlotOffset) && !mayHaveImportSlot(opcode, resolveType))
            return false;
    }
    return true;
}

// Whether the CB's metadata for the fact's instruction holds what the compilation baked. Only a resolve_scope of type
// ClosureVar or ModuleVar bakes its depth, and only one of type ModuleVar its import slot; the other baked kinds read
// their depth from the metadata at run time.
static bool scopeFactHolds(const ScopeFact& fact, CodeBlock& codeBlock)
{
    auto& instructions = codeBlock.instructions();
    ASSERT(fact.bytecodeOffset < instructions.size());
    auto instruction = instructions.at(BytecodeIndex(fact.bytecodeOffset));
    switch (fact.opcode) {
    case ScopeOpcode::ResolveScope: {
        ASSERT(instruction->is<OpResolveScope>());
        auto& metadata = instruction->as<OpResolveScope>().metadata(&codeBlock);
        if (metadata.m_resolveType != fact.resolveType)
            return false;
        if (mayHaveDepth(fact.opcode, fact.resolveType) && metadata.m_localScopeDepth != fact.localScopeDepth)
            return false;
        return !mayHaveImportSlot(fact.opcode, fact.resolveType) || metadata.m_moduleImportSlot == fact.moduleImportSlot;
    }
    case ScopeOpcode::GetFromScope:
        ASSERT(instruction->is<OpGetFromScope>());
        return instruction->as<OpGetFromScope>().metadata(&codeBlock).m_getPutInfo.resolveType() == fact.resolveType;
    case ScopeOpcode::PutToScope:
        ASSERT(instruction->is<OpPutToScope>());
        return instruction->as<OpPutToScope>().metadata(&codeBlock).m_getPutInfo.resolveType() == fact.resolveType;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace BakedFactsInternal

bool isBakedScopeShape(ScopeOpcode opcode, ResolveType resolveType)
{
    switch (opcode) {
    case ScopeOpcode::ResolveScope:
        return resolveType == ModuleVar || resolveType == ClosureVar || resolveType == ClosureVarWithVarInjectionChecks;
    case ScopeOpcode::GetFromScope:
        return resolveType == ClosureVar || resolveType == LazyClosureVar;
    case ScopeOpcode::PutToScope:
        return resolveType == ClosureVar || resolveType == ResolvedClosureVar || resolveType == ClosureVarWithVarInjectionChecks || resolveType == ModuleVar;
    }
    return false;
}

BakedFactsBuilder::BakedFactsBuilder(StorageAllowance* allowance)
    : m_allowance(allowance)
{
}

void BakedFactsBuilder::setCodeBlockFacts(DFG::CapabilityLevel capabilityLevel, bool couldBeTainted)
{
    ASSERT(capabilityLevel != DFG::CapabilityLevelNotSet);
    if (m_allowance && !m_allowance->allowStorage(0))
        return;
    m_facts.capabilityLevel = capabilityLevel;
    m_facts.couldBeTainted = couldBeTainted;
}

void BakedFactsBuilder::addScopeFact(ScopeFact fact)
{
    ASSERT(isBakedScopeShape(fact.opcode, fact.resolveType));
    ASSERT(!fact.localScopeDepth || BakedFactsInternal::mayHaveDepth(fact.opcode, fact.resolveType));
    ASSERT(!fact.moduleImportSlot || BakedFactsInternal::mayHaveImportSlot(fact.opcode, fact.resolveType));
    auto& facts = m_facts.scopeFacts;
    size_t addedBytes = 0;
    size_t newCapacity = facts.capacity();
    if (facts.size() == facts.capacity()) {
        newCapacity = std::max<size_t>(8, facts.capacity() * 2);
        addedBytes = (newCapacity - facts.capacity()) * sizeof(ScopeFact);
    }
    if (m_allowance && !m_allowance->allowStorage(addedBytes))
        return;
    facts.reserveCapacity(newCapacity);
    facts.append(fact);
}

BakedFacts BakedFactsBuilder::finish()
{
    auto& facts = m_facts.scopeFacts;
    std::sort(facts.begin(), facts.end(), [](const ScopeFact& a, const ScopeFact& b) {
        return a.bytecodeOffset < b.bytecodeOffset;
    });
    // Each emitter adds at most one fact per instruction: the main pass and the slow pass bake disjoint resolve types.
    ASSERT(std::adjacent_find(facts.begin(), facts.end(), [](const ScopeFact& a, const ScopeFact& b) {
        return a.bytecodeOffset == b.bytecodeOffset;
    }) == facts.end());
    facts.shrinkToFit();
    return std::exchange(m_facts, BakedFacts { });
}

size_t bakedFactsSectionSize(const BakedFacts& facts)
{
    return bakedFactsHeaderSize + facts.scopeFacts.size() * bakedFactsEntrySize;
}

bool writeBakedFactsSection(const BakedFacts& facts, const ImageSectionSink& sink)
{
    using namespace BakedFactsInternal;
    ASSERT(facts.capabilityLevel != DFG::CapabilityLevelNotSet);

    std::array<uint8_t, bakedFactsHeaderSize> header { };
    header[levelOffset] = static_cast<uint8_t>(facts.capabilityLevel);
    header[taintOffset] = facts.couldBeTainted ? 1 : 0;
    writeField<uint32_t>(header, countOffset, static_cast<uint32_t>(facts.scopeFacts.size()));
    if (!sink(std::span<const uint8_t>(header)))
        return false;

    for (auto& fact : facts.scopeFacts) {
        std::array<uint8_t, bakedFactsEntrySize> entry { };
        writeField<uint32_t>(entry, entryBytecodeOffset, fact.bytecodeOffset);
        entry[entryOpcodeOffset] = static_cast<uint8_t>(fact.opcode);
        entry[entryResolveTypeOffset] = static_cast<uint8_t>(fact.resolveType);
        writeField<uint32_t>(entry, entryDepthOffset, fact.localScopeDepth);
        writeField<uint32_t>(entry, entryImportSlotOffset, fact.moduleImportSlot);
        if (!sink(std::span<const uint8_t>(entry)))
            return false;
    }
    return true;
}

ScopeFact BakedFactsView::scopeFact(unsigned index) const
{
    using namespace BakedFactsInternal;
    ASSERT(index < m_scopeFactCount);
    auto entry = entryAt(m_bytes, index);
    return ScopeFact {
        .bytecodeOffset = readField<uint32_t>(entry, entryBytecodeOffset),
        .opcode = static_cast<ScopeOpcode>(entry[entryOpcodeOffset]),
        .resolveType = static_cast<ResolveType>(entry[entryResolveTypeOffset]),
        .localScopeDepth = readField<uint32_t>(entry, entryDepthOffset),
        .moduleImportSlot = readField<uint32_t>(entry, entryImportSlotOffset),
    };
}

bool operator==(const BakedFactsView& a, const BakedFactsView& b)
{
    return a.m_bytes.data() == b.m_bytes.data() && a.m_bytes.size() == b.m_bytes.size()
        && a.m_capabilityLevel == b.m_capabilityLevel && a.m_couldBeTainted == b.m_couldBeTainted
        && a.m_scopeFactCount == b.m_scopeFactCount;
}

std::expected<BakedFactsView, ImageCheck> parseBakedFactsSection(std::span<const uint8_t> section, bool strict)
{
    using namespace BakedFactsInternal;
    if (section.size() < bakedFactsHeaderSize)
        return std::unexpected(ImageCheck::BakedFacts);
    uint32_t count = readField<uint32_t>(section, countOffset);
    uint64_t locatedSize = bakedFactsHeaderSize + static_cast<uint64_t>(count) * bakedFactsEntrySize;
    if (locatedSize > section.size())
        return std::unexpected(ImageCheck::BakedFacts);

    if (strict) {
        if (!passesBakedFacts(section))
            return std::unexpected(ImageCheck::BakedFacts);
    } else
        ASSERT(passesBakedFacts(section));

    BakedFactsView view;
    view.m_bytes = section.first(static_cast<size_t>(locatedSize));
    view.m_capabilityLevel = static_cast<DFG::CapabilityLevel>(section[levelOffset]);
    view.m_couldBeTainted = !!section[taintOffset];
    view.m_scopeFactCount = count;
    return view;
}

BakedFactsResult compareBakedFacts(const ImageSectionsView& view, CodeBlock& newborn)
{
    using namespace BakedFactsInternal;
    const BakedFactsView& facts = view.bakedFacts();
    // capabilityLevel() would memoize into m_capabilityLevelState, which this comparison must leave as linking left it.
    DFG::CapabilityLevel level = newborn.capabilityLevelState();
    if (level == DFG::CapabilityLevelNotSet)
        level = newborn.computeCapabilityLevel();
    if ((facts.capabilityLevel() == DFG::CannotCompile) != (level == DFG::CannotCompile))
        return BakedFactsResult::Mismatch;
    if (facts.couldBeTainted() != newborn.couldBeTainted())
        return BakedFactsResult::Mismatch;
    for (unsigned index = 0; index < facts.scopeFactCount(); ++index) {
        if (!scopeFactHolds(facts.scopeFact(index), newborn))
            return BakedFactsResult::Mismatch;
    }
    return BakedFactsResult::Match;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
