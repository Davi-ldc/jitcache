#include "config.h"
#include "jitcache/ucb/UCBSections.h"

#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"
#include "jitcache/base/ByteFields.h"
#include <bit>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC::JITCache {

namespace UCBSectionsInternal {

// The fixed part of ucb.identity. The butterfly map follows it, ceil(N / 8) bytes, zero padding up to 8 bytes, and then the
// child holder digests, 32 bytes each, to the section's end. The core kind comes last, so every wider field before it is
// naturally aligned.
static constexpr size_t keyOffset = 0;
static constexpr size_t contextDigestOffset = 40;
static constexpr size_t holderDigestOffset = 72;
static constexpr size_t featuresOffset = 104;
static constexpr size_t lexicallyScopedFeaturesOffset = 106;
static constexpr size_t hasCapturedVariablesOffset = 107;
static constexpr size_t constantCountOffset = 108;
static constexpr size_t coreKindOffset = 112;
static constexpr size_t butterflyMapOffset = 113;
static_assert(keyOffset + BodyKey::byteSize == contextDigestOffset);
static_assert(contextDigestOffset + sizeof(Digest256) == holderDigestOffset);
static_assert(holderDigestOffset + sizeof(Digest256) == featuresOffset);

// The key exactly as stored: BodyKey holds its 40 canonical bytes, which are trusted, and the engine compares them with the
// body's key.
static BodyKey storedKey(std::span<const uint8_t> bytes)
{
    std::array<uint8_t, BodyKey::byteSize> copy;
    memcpySpan(std::span { copy }, bytes.subspan<keyOffset, BodyKey::byteSize>());
    return std::bit_cast<BodyKey>(copy);
}

static size_t mapSize(uint32_t constantCount)
{
    return (static_cast<size_t>(constantCount) + 7) / 8;
}

static void mark(std::span<uint8_t> map, uint32_t index)
{
    map[index / 8] |= 1u << (index % 8);
}

// Calls functor(index) for every index below count that the map marks, in increasing order.
template<typename Functor>
static void forEachMarked(std::span<const uint8_t> map, uint32_t count, const Functor& functor)
{
    size_t byteCount = mapSize(count);
    for (size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex) {
        for (unsigned bits = map[byteIndex]; bits; bits &= bits - 1) {
            uint32_t index = static_cast<uint32_t>(byteIndex * 8 + std::countr_zero(bits));
            if (index >= count)
                return;
            functor(index);
        }
    }
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

} // namespace UCBSectionsInternal

IdentitySection parseIdentitySection(std::span<const uint8_t> bytes)
{
    using namespace UCBSectionsInternal;

    auto coreKind = static_cast<UnlinkedCodeBlockCoreKind>(bytes[coreKindOffset]);
    auto constantCount = readField<uint32_t>(bytes, constantCountOffset);
    std::optional<Digest256> holderDigest;
    std::optional<FunctionParseFields> functionParseFields;
    if (coreKind == UnlinkedCodeBlockCoreKind::Function) {
        holderDigest = readField<Digest256>(bytes, holderDigestOffset);
        functionParseFields = FunctionParseFields {
            .features = readField<CodeFeatures>(bytes, featuresOffset),
            .lexicallyScopedFeatures = bytes[lexicallyScopedFeaturesOffset],
            .hasCapturedVariables = !!bytes[hasCapturedVariablesOffset],
        };
    }

    size_t baseSize = identitySectionSize(constantCount);
    // The section's size is trusted to leave whole digests after the map.
    std::span<const uint8_t> childHolderDigests = bytes.subspan(baseSize);
    childHolderDigests = childHolderDigests.first(childHolderDigests.size() - childHolderDigests.size() % sizeof(Digest256));
    return IdentitySection {
        .coreKind = coreKind,
        .key = storedKey(bytes),
        .contextDigest = readField<Digest256>(bytes, contextDigestOffset),
        .holderDigest = holderDigest,
        .functionParseFields = functionParseFields,
        .constantCount = constantCount,
        .butterflyMap = bytes.subspan(butterflyMapOffset, mapSize(constantCount)),
        .childHolderDigests = childHolderDigests,
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
        // The body is trusted to mark only butterflies of strings.
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

size_t identitySectionSize(uint32_t constantCount, size_t childHolderDigestCount)
{
    using namespace UCBSectionsInternal;

    return roundUpToMultipleOf<8>(butterflyMapOffset + mapSize(constantCount)) + childHolderDigestCount * sizeof(Digest256);
}

void writeIdentitySection(std::span<uint8_t> out, VM& vm, const BodyKey& key, const Digest256& contextDigest, const std::optional<Digest256>& holderDigest,
    UnlinkedCodeBlockCoreKind coreKind, const std::optional<FunctionParseFields>& functionParseFields, UnlinkedCodeBlock& ucb, std::span<const Digest256> childHolderDigests)
{
    using namespace UCBSectionsInternal;

    const auto& constants = ucb.constantRegisters();
    auto constantCount = static_cast<uint32_t>(constants.size());
    RELEASE_ASSERT(out.size() == identitySectionSize(constantCount, childHolderDigests.size()));
    ASSERT(childHolderDigests.empty() || childHolderDigests.size() == static_cast<size_t>(ucb.numberOfFunctionDecls()) + ucb.numberOfFunctionExprs());
    ASSERT(coreKind == UnlinkedCodeBlockCoreKind::Function || (!holderDigest && !functionParseFields));

    // The map's padding stays zero.
    zeroSpan(out);
    memcpySpan(out.subspan(keyOffset, BodyKey::byteSize), key.bytes());
    writeField<Digest256>(out, contextDigestOffset, contextDigest);
    if (coreKind == UnlinkedCodeBlockCoreKind::Function) {
        if (holderDigest)
            writeField<Digest256>(out, holderDigestOffset, *holderDigest);
        if (functionParseFields) {
            writeField<CodeFeatures>(out, featuresOffset, functionParseFields->features);
            out[lexicallyScopedFeaturesOffset] = functionParseFields->lexicallyScopedFeatures;
            out[hasCapturedVariablesOffset] = functionParseFields->hasCapturedVariables;
        }
    }
    writeField<uint32_t>(out, constantCountOffset, constantCount);
    out[coreKindOffset] = static_cast<uint8_t>(coreKind);

    // A UCB's butterflies keep the structure they were created with, so a generated or imported UCB's map records
    // generation's form: the butterflies it built from atom strings. A structure read decodes an ID, which allocates
    // nothing.
    auto butterflyMap = out.subspan(butterflyMapOffset, mapSize(constantCount));
    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    for (uint32_t index = 0; index < constantCount; ++index) {
        JSValue value = constants[index].get();
        if (!value)
            continue;
        if (auto* butterfly = dynamicDowncast<JSCellButterfly>(value); butterfly && butterfly->structure() == atomStringsStructure)
            mark(butterflyMap, index);
    }

    size_t digestsAt = identitySectionSize(constantCount);
    for (size_t i = 0; i < childHolderDigests.size(); ++i)
        writeField<Digest256>(out, digestsAt + i * sizeof(Digest256), childHolderDigests[i]);
}

} // namespace JSC::JITCache
