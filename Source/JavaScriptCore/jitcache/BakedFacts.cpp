#include "config.h"
#include "BakedFacts.h"

#if ENABLE(JIT)

#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "ImageSection.h"
#include "JSCInlines.h"
#include <algorithm>
#include <array>
#include <optional>
#include <utility>

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

static_assert(sizeof(DFG::CapabilityLevel) == 1);

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

// V7 (SPEC-image.md section 8.5) on a section whose header and counted entries lie inside it.
static bool passesV7(std::span<const uint8_t> section)
{
    uint32_t count = ImageBytes::read<uint32_t>(section, countOffset);
    if (section.size() != bakedFactsHeaderSize + static_cast<size_t>(count) * bakedFactsEntrySize)
        return false;
    if (!isValidCapabilityLevel(section[levelOffset]) || section[taintOffset] > 1)
        return false;
    if (ImageBytes::read<uint16_t>(section, headerReservedOffset))
        return false;

    std::optional<uint32_t> previousOffset;
    for (unsigned index = 0; index < count; ++index) {
        auto entry = entryAt(section, index);
        uint32_t bytecodeOffset = ImageBytes::read<uint32_t>(entry, entryBytecodeOffset);
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
        if (ImageBytes::read<uint16_t>(entry, entryReservedOffset))
            return false;
        uint32_t depth = ImageBytes::read<uint32_t>(entry, entryDepthOffset);
        if (depth && !(opcode == ScopeOpcode::ResolveScope && resolveType == ClosureVar))
            return false;
    }
    return true;
}

// Whether the CB's metadata for the fact's instruction holds what the compilation baked (section 7). Only the depth of a
// resolve_scope of type ClosureVar is baked; the other baked kinds read their depth from the metadata at run time.
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
        return fact.resolveType != ClosureVar || metadata.m_localScopeDepth == fact.localScopeDepth;
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
        return resolveType == ClosureVar;
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
    ASSERT(!fact.localScopeDepth || (fact.opcode == ScopeOpcode::ResolveScope && fact.resolveType == ClosureVar));
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
    ImageBytes::write<uint32_t>(header, countOffset, static_cast<uint32_t>(facts.scopeFacts.size()));
    if (!sink(std::span<const uint8_t>(header)))
        return false;

    for (auto& fact : facts.scopeFacts) {
        std::array<uint8_t, bakedFactsEntrySize> entry { };
        ImageBytes::write<uint32_t>(entry, entryBytecodeOffset, fact.bytecodeOffset);
        entry[entryOpcodeOffset] = static_cast<uint8_t>(fact.opcode);
        entry[entryResolveTypeOffset] = static_cast<uint8_t>(fact.resolveType);
        ImageBytes::write<uint32_t>(entry, entryDepthOffset, fact.localScopeDepth);
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
        .bytecodeOffset = ImageBytes::read<uint32_t>(entry, entryBytecodeOffset),
        .opcode = static_cast<ScopeOpcode>(entry[entryOpcodeOffset]),
        .resolveType = static_cast<ResolveType>(entry[entryResolveTypeOffset]),
        .localScopeDepth = ImageBytes::read<uint32_t>(entry, entryDepthOffset),
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
        return std::unexpected(ImageCheck::V7);
    uint32_t count = ImageBytes::read<uint32_t>(section, countOffset);
    uint64_t locatedSize = bakedFactsHeaderSize + static_cast<uint64_t>(count) * bakedFactsEntrySize;
    if (locatedSize > section.size())
        return std::unexpected(ImageCheck::V7);

    if (strict) {
        if (!passesV7(section))
            return std::unexpected(ImageCheck::V7);
    } else
        ASSERT(passesV7(section));

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
