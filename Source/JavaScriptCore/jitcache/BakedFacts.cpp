/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "BakedFacts.h"

#if ENABLE(JIT)

#include <algorithm>
#include <array>
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

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
