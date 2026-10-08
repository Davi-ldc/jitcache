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
#include "UCBSections.h"

#include "JITCacheSHA256.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include <algorithm>
#include <bit>
#include <wtf/MathExtras.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache {

namespace UCBSectionsInternal {

static_assert(std::endian::native == std::endian::little, "ucb.identity is little-endian, as every JITCache target is");

// The fixed part of ucb.identity (SPEC-ucb.md section 4.2). The atom map and then the butterfly map follow it, each
// ceil(N / 8) bytes, and the section ends at the butterfly map rounded up to 8 bytes.
static constexpr uint32_t sectionMagic = 0x49424355;
static constexpr uint16_t layoutVersion = 1;
static constexpr size_t magicOffset = 0;
static constexpr size_t versionOffset = 4;
static constexpr size_t coreKindOffset = 6;
static constexpr size_t provenanceOffset = 7;
static constexpr size_t keyOffset = 8;
static constexpr size_t contextDigestOffset = 48;
static constexpr size_t coreDigestOffset = 80;
static constexpr size_t holderDigestOffset = 112;
static constexpr size_t featuresOffset = 144;
static constexpr size_t lexicallyScopedFeaturesOffset = 146;
static constexpr size_t hasCapturedVariablesOffset = 147;
static constexpr size_t constantCountOffset = 148;
static constexpr size_t mapsOffset = 152;
static constexpr uint32_t constantCountLimit = 1u << 28;
static_assert(keyOffset + BodyKey::byteSize == contextDigestOffset);
static_assert(contextDigestOffset + sizeof(Digest256) == coreDigestOffset);
static_assert(coreDigestOffset + sizeof(Digest256) == holderDigestOffset);
static_assert(holderDigestOffset + sizeof(Digest256) == featuresOffset);

template<typename T>
static T readField(std::span<const uint8_t> bytes, size_t offset)
{
    std::array<uint8_t, sizeof(T)> raw;
    memcpySpan(std::span { raw }, bytes.subspan(offset, sizeof(T)));
    return std::bit_cast<T>(raw);
}

template<typename T>
static void writeField(std::span<uint8_t> bytes, size_t offset, const T& value)
{
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

static Digest256 readDigest(std::span<const uint8_t> bytes, size_t offset)
{
    Digest256 digest;
    memcpySpan(std::span { digest }, bytes.subspan(offset, digest.size()));
    return digest;
}

static void writeDigest(std::span<uint8_t> bytes, size_t offset, const Digest256& digest)
{
    memcpySpan(bytes.subspan(offset, digest.size()), std::span { digest });
}

// The key exactly as stored: BodyKey holds its 40 canonical bytes. With strict on, BodyKey::fromBytes has accepted them;
// normal mode trusts them (THREAD Session), and the engine compares them with the body's key in both modes.
static BodyKey storedKey(std::span<const uint8_t> bytes)
{
    std::array<uint8_t, BodyKey::byteSize> copy;
    memcpySpan(std::span { copy }, bytes.subspan<keyOffset, BodyKey::byteSize>());
    return std::bit_cast<BodyKey>(copy);
}

static bool isZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) {
        return !byte;
    });
}

static size_t mapSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

static void mark(std::span<uint8_t> map, uint32_t index)
{
    map[index / 8] |= 1u << (index % 8);
}

// Only the last byte of a map of ceil(count / 8) bytes can hold a bit at or above count.
static bool marksAtOrAbove(std::span<const uint8_t> map, uint32_t count)
{
    uint32_t usedBits = count % 8;
    return usedBits && (map.back() >> usedBits);
}

// True when predicate(index) holds for every index below count that the map marks, visited in increasing order; stops at
// the first index for which it does not.
template<typename Predicate>
static bool allMarkedSatisfy(std::span<const uint8_t> map, uint32_t count, const Predicate& predicate)
{
    size_t byteCount = mapSize(count);
    for (size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex) {
        for (unsigned bits = map[byteIndex]; bits; bits &= bits - 1) {
            uint32_t index = static_cast<uint32_t>(byteIndex * 8 + std::countr_zero(bits));
            if (index >= count)
                return true;
            if (!predicate(index))
                return false;
        }
    }
    return true;
}

template<typename Functor>
static void forEachMarked(std::span<const uint8_t> map, uint32_t count, const Functor& functor)
{
    allMarkedSatisfy(map, count, [&](uint32_t index) {
        functor(index);
        return true;
    });
}

// A constant register as a cell of class T, or null. An empty constant, which generation emits for TDZ checks, is no cell.
template<typename T>
static T* constantAs(UnlinkedCodeBlock& ucb, uint32_t index)
{
    JSValue value = ucb.constantRegisters()[index].get();
    if (!value)
        return nullptr;
    return dynamicDowncast<T>(value);
}

static bool isResolvedString(const JSString* string)
{
    return string && !string->isRope();
}

static bool isResolvedStringValue(JSValue value)
{
    return value && isResolvedString(dynamicDowncast<JSString>(value));
}

// The rules of section 4.2, which strict checks before anything reads the section.
static bool obeysIdentityRules(std::span<const uint8_t> bytes)
{
    if (bytes.size() < mapsOffset)
        return false;
    if (readField<uint32_t>(bytes, magicOffset) != sectionMagic || readField<uint16_t>(bytes, versionOffset) != layoutVersion)
        return false;
    uint8_t coreKindByte = bytes[coreKindOffset];
    if (coreKindByte > static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Function) || bytes[provenanceOffset] > static_cast<uint8_t>(CoreProvenance::EmbedderDecoded))
        return false;
    auto constantCount = readField<uint32_t>(bytes, constantCountOffset);
    if (constantCount >= constantCountLimit || bytes.size() != identitySectionSize(constantCount))
        return false;
    auto coreKind = static_cast<UnlinkedCodeBlockCoreKind>(coreKindByte);
    auto key = BodyKey::fromBytes(bytes.subspan<keyOffset, BodyKey::byteSize>());
    if (!key || coreKindFor(key->identityKind()) != coreKind)
        return false;

    auto features = readField<CodeFeatures>(bytes, featuresOffset);
    uint8_t lexicallyScopedFeatures = bytes[lexicallyScopedFeaturesOffset];
    uint8_t hasCapturedVariables = bytes[hasCapturedVariablesOffset];
    if (coreKind == UnlinkedCodeBlockCoreKind::Function) {
        if (features >= (1u << bitWidthOfCodeFeatures) || lexicallyScopedFeatures > AllLexicallyScopedFeatures || hasCapturedVariables > 1)
            return false;
    } else {
        // Only a function core has a holder digest and parse fields.
        if (!isZero(bytes.subspan(holderDigestOffset, sizeof(Digest256))) || features || lexicallyScopedFeatures || hasCapturedVariables)
            return false;
    }

    size_t mapBytes = mapSize(constantCount);
    auto atomMap = bytes.subspan(mapsOffset, mapBytes);
    auto butterflyMap = bytes.subspan(mapsOffset + mapBytes, mapBytes);
    if (marksAtOrAbove(atomMap, constantCount) || marksAtOrAbove(butterflyMap, constantCount))
        return false;
    for (size_t i = 0; i < mapBytes; ++i) {
        if (atomMap[i] & butterflyMap[i])
            return false;
    }
    return isZero(bytes.subspan(mapsOffset + 2 * mapBytes));
}

} // namespace UCBSectionsInternal

std::optional<IdentitySection> parseIdentitySection(std::span<const uint8_t> bytes, bool strict)
{
    using namespace UCBSectionsInternal;

    if (strict && !obeysIdentityRules(bytes))
        return std::nullopt;

    auto coreKind = static_cast<UnlinkedCodeBlockCoreKind>(bytes[coreKindOffset]);
    auto constantCount = readField<uint32_t>(bytes, constantCountOffset);
    std::optional<Digest256> holderDigest;
    std::optional<FunctionParseFields> functionParseFields;
    if (coreKind == UnlinkedCodeBlockCoreKind::Function) {
        holderDigest = readDigest(bytes, holderDigestOffset);
        functionParseFields = FunctionParseFields {
            .features = readField<CodeFeatures>(bytes, featuresOffset),
            .lexicallyScopedFeatures = bytes[lexicallyScopedFeaturesOffset],
            .hasCapturedVariables = !!bytes[hasCapturedVariablesOffset],
        };
    }

    size_t mapBytes = mapSize(constantCount);
    return IdentitySection {
        .coreKind = coreKind,
        .provenance = static_cast<CoreProvenance>(bytes[provenanceOffset]),
        .key = storedKey(bytes),
        .contextDigest = readDigest(bytes, contextDigestOffset),
        .coreDigest = readDigest(bytes, coreDigestOffset),
        .holderDigest = holderDigest,
        .functionParseFields = functionParseFields,
        .constantCount = constantCount,
        .atomMap = bytes.subspan(mapsOffset, mapBytes),
        .butterflyMap = bytes.subspan(mapsOffset + mapBytes, mapBytes),
    };
}

UnlinkedCodeBlockCoreKind coreKindFor(IdentityKind kind)
{
    switch (kind) {
    case IdentityKind::Program:
        return UnlinkedCodeBlockCoreKind::Program;
    case IdentityKind::Module:
        return UnlinkedCodeBlockCoreKind::Module;
    case IdentityKind::IndirectEval:
    case IdentityKind::DirectEval:
        return UnlinkedCodeBlockCoreKind::Eval;
    case IdentityKind::FunctionConstructor:
    case IdentityKind::Builtin:
    case IdentityKind::Child:
        return UnlinkedCodeBlockCoreKind::Function;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool constantMapsFit(UnlinkedCodeBlock& ucb, const IdentitySection& identity)
{
    using namespace UCBSectionsInternal;

    if (ucb.constantRegisters().size() != identity.constantCount)
        return false;

    bool atomsAreResolvedStrings = allMarkedSatisfy(identity.atomMap, identity.constantCount, [&](uint32_t index) {
        return isResolvedString(constantAs<JSString>(ucb, index));
    });
    if (!atomsAreResolvedStrings)
        return false;

    return allMarkedSatisfy(identity.butterflyMap, identity.constantCount, [&](uint32_t index) {
        auto* butterfly = constantAs<JSCellButterfly>(ucb, index);
        if (!butterfly || butterfly->indexingMode() != CopyOnWriteArrayWithContiguous)
            return false;
        for (unsigned i = 0; i < butterfly->length(); ++i) {
            if (!isResolvedStringValue(butterfly->get(i)))
                return false;
        }
        return true;
    });
}

bool markedConstantsAreAtoms(UnlinkedCodeBlock& ucb, const IdentitySection& identity)
{
    using namespace UCBSectionsInternal;

    size_t constantCount = ucb.constantRegisters().size();
    return allMarkedSatisfy(identity.atomMap, identity.constantCount, [&](uint32_t index) {
        if (index >= constantCount)
            return false;
        auto* string = constantAs<JSString>(ucb, index);
        StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
        return impl && impl->isAtom();
    });
}

Digest256 coreDigestOf(VM& vm, const UnlinkedCodeBlock& ucb, const UnlinkedFunctionExecutable* holder)
{
    SHA256 hasher;
    auto hashChunk = [&](std::span<const uint8_t> chunk) {
        hasher.update(chunk);
    };
    CoreEncodeFailure failure = forEachUnlinkedCodeBlockCoreChunk(vm, ucb, holder, nullptr, hashChunk);
    // Without a budget no charge can be refused (SPEC-ucb.codec.md, E8).
    ASSERT_UNUSED(failure, failure == CoreEncodeFailure::None);
    return hasher.finalize();
}

void atomizeMarkedConstants(VM& vm, UnlinkedCodeBlock& ucb, const IdentitySection& identity)
{
    using namespace UCBSectionsInternal;

    forEachMarked(identity.atomMap, identity.constantCount, [&](uint32_t index) {
        // C8 holds with strict on; normal mode trusts the body, whose core matched this UCB's (section 7.3.2).
        auto* string = constantAs<JSString>(ucb, index);
        ASSERT(isResolvedString(string));
        atomizeStringConstant(vm, *string);
    });
}

void atomizeStringConstant(VM& vm, const JSString& string)
{
    ASSERT(!string.isRope());
    if (string.existingAtomOrNull())
        return;
    // As JSString::toAtomString: the swap runs even when the add made the StringImpl itself the atom, since it is what
    // sets the cell's isDefinitelyAtom bit.
    string.swapToAtomString(vm, AtomStringImpl::add(string.valueInternal().impl()));
}

void rebuildAtomStringButterflies(VM& vm, UnlinkedCodeBlock& ucb, const IdentitySection& identity)
{
    using namespace UCBSectionsInternal;

    ASSERT(vm.heap.isDeferred());
    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    forEachMarked(identity.butterflyMap, identity.constantCount, [&](uint32_t index) {
        // C8 holds with strict on; normal mode trusts the body to mark only butterflies of strings.
        auto* decoded = constantAs<JSCellButterfly>(ucb, index);
        ASSERT(decoded && decoded->indexingMode() == CopyOnWriteArrayWithContiguous);
        unsigned length = decoded->length();
        auto* rebuilt = JSCellButterfly::tryCreate(vm, atomStringsStructure, length);
        RELEASE_ASSERT(rebuilt);
        for (unsigned i = 0; i < length; ++i) {
            JSString* element = asString(decoded->get(i));
            atomizeStringConstant(vm, *element);
            StringImpl* atom = element->getValueImpl();
            rebuilt->setIndex(vm, i, vm.atomStringToJSStringMap.ensureValue(atom, [&] {
                return element;
            }));
        }
        ucb.constantRegister(VirtualRegister(VirtualRegister::firstConstantRegisterIndex + static_cast<int>(index))).set(vm, &ucb, rebuilt);
    });
}

size_t identitySectionSize(uint32_t constantCount)
{
    using namespace UCBSectionsInternal;

    return roundUpToMultipleOf<8>(mapsOffset + 2 * mapSize(constantCount));
}

void writeIdentitySection(std::span<uint8_t> out, VM& vm, const BodyKey& key, const Digest256& contextDigest, CoreProvenance provenance, const Digest256& coreDigest,
    const std::optional<Digest256>& holderDigest, UnlinkedCodeBlockCoreKind coreKind, const std::optional<FunctionParseFields>& functionParseFields, UnlinkedCodeBlock& ucb,
    std::optional<std::span<const uint8_t>> keptButterflyMap)
{
    using namespace UCBSectionsInternal;

    const auto& constants = ucb.constantRegisters();
    auto constantCount = static_cast<uint32_t>(constants.size());
    RELEASE_ASSERT(out.size() == identitySectionSize(constantCount));
    ASSERT(coreKind == UnlinkedCodeBlockCoreKind::Function || (!holderDigest && !functionParseFields));

    zeroSpan(out);
    writeField<uint32_t>(out, magicOffset, sectionMagic);
    writeField<uint16_t>(out, versionOffset, layoutVersion);
    out[coreKindOffset] = static_cast<uint8_t>(coreKind);
    out[provenanceOffset] = static_cast<uint8_t>(provenance);
    memcpySpan(out.subspan(keyOffset, BodyKey::byteSize), key.bytes());
    writeDigest(out, contextDigestOffset, contextDigest);
    writeDigest(out, coreDigestOffset, coreDigest);
    if (coreKind == UnlinkedCodeBlockCoreKind::Function) {
        if (holderDigest)
            writeDigest(out, holderDigestOffset, *holderDigest);
        if (functionParseFields) {
            writeField<CodeFeatures>(out, featuresOffset, functionParseFields->features);
            out[lexicallyScopedFeaturesOffset] = functionParseFields->lexicallyScopedFeatures;
            out[hasCapturedVariablesOffset] = functionParseFields->hasCapturedVariables;
        }
    }
    writeField<uint32_t>(out, constantCountOffset, constantCount);

    size_t mapBytes = mapSize(constantCount);
    auto atomMap = out.subspan(mapsOffset, mapBytes);
    auto butterflyMap = out.subspan(mapsOffset + mapBytes, mapBytes);
    if (keptButterflyMap) {
        // A decoded UCB seeded or attached from a body of provenance Generated keeps that body's map, since its own
        // butterflies keep the decoder's plain structure (section 8.2).
        RELEASE_ASSERT(keptButterflyMap->size() == mapBytes);
        memcpySpan(butterflyMap, *keptButterflyMap);
    }

    // Each read is a load on the VM thread: string constants are resolved, and a structure read decodes an ID.
    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    for (uint32_t index = 0; index < constantCount; ++index) {
        JSValue value = constants[index].get();
        if (!value)
            continue;
        if (auto* string = dynamicDowncast<JSString>(value)) {
            if (StringImpl* impl = string->tryGetValueImpl(); impl && impl->isAtom())
                mark(atomMap, index);
            continue;
        }
        if (keptButterflyMap)
            continue;
        if (auto* butterfly = dynamicDowncast<JSCellButterfly>(value); butterfly && butterfly->structure() == atomStringsStructure)
            mark(butterflyMap, index);
    }
}

} // namespace JSC::JITCache
