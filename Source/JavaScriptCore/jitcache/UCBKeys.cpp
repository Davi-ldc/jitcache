#include "config.h"
#include "UCBKeys.h"

#include "CachedTypes.h"
#include "JITCacheSHA256.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "UnlinkedFunctionExecutable.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringView.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC::JITCache {

namespace UCBKeysInternal {

// UTF-16LE text streams in place, from the units' own bytes, as UCBSections.cpp's little-endian sections assume too.
static_assert(std::endian::native == std::endian::little, "JITCache digests read UTF-16 code units as little-endian bytes");

// The body key's fixed layout (SPEC-ucb.md section 3.1).
static constexpr uint8_t bodyKeyVersion = 1;
static constexpr size_t versionOffset = 0;
static constexpr size_t identityKindOffset = 1;
static constexpr size_t specializationOffset = 2;
static constexpr size_t codeGenerationModeOffset = 3;
static constexpr size_t reservedOffset = 4;
static constexpr size_t reservedSize = 4;
static constexpr size_t identityDigestOffset = 8;
static_assert(reservedOffset + reservedSize == identityDigestOffset);
static_assert(identityDigestOffset + std::tuple_size_v<Digest256> == BodyKey::byteSize);
// identityDigest() reads its 32 bytes in place, as a Digest256.
static_assert(sizeof(Digest256) == std::tuple_size_v<Digest256> && alignof(Digest256) == 1);

// Every value CodeGenerationMode defines; fromBytes accepts no other mode bit.
static constexpr OptionSet<CodeGenerationMode> definedCodeGenerationModes {
    CodeGenerationMode::Debugger, CodeGenerationMode::TypeProfiler, CodeGenerationMode::ControlFlowProfiler,
};

// The roots whose body a UFE holds, which section 3.3 calls the root bodies of a UFE.
static constexpr bool isRootExecutableKind(IdentityKind kind)
{
    return kind == IdentityKind::FunctionConstructor || kind == IdentityKind::Builtin;
}

static constexpr bool isGlobalKind(IdentityKind kind)
{
    return kind == IdentityKind::Program || kind == IdentityKind::Module || kind == IdentityKind::IndirectEval;
}

// Section 3.1: these keys always have specialization 0.
static constexpr bool isCallOnlyKind(IdentityKind kind)
{
    return isGlobalKind(kind) || kind == IdentityKind::DirectEval;
}

static constexpr bool isDefinedIdentityKind(uint8_t value)
{
    return value >= static_cast<uint8_t>(IdentityKind::Program) && value <= static_cast<uint8_t>(IdentityKind::DirectEval);
}

// The text encodings of sections 3.3 and 3.5. Null is a null identifier's, and only canonical strings write it.
enum class TextEncoding : uint8_t { Null = 0, Latin1 = 1, UTF16LittleEndian = 2 };

// Latin-1 exactly when every unit is at most 0xFF, so an 8-bit and a 16-bit string with the same characters encode alike.
static TextEncoding textEncodingOf(StringView text)
{
    if (text.is8Bit() || WTF::charactersAreAllLatin1(text.span16()))
        return TextEncoding::Latin1;
    return TextEncoding::UTF16LittleEndian;
}

// The code units of `text` in the encoding textEncodingOf gave it, with no copy of the text: 8-bit and UTF-16LE text stream
// in place, and 16-bit text narrowed to Latin-1 streams through a 4 KiB chunk (section 3.5).
static void hashCodeUnits(SHA256& hasher, StringView text, TextEncoding encoding)
{
    if (text.is8Bit()) {
        hasher.update(asBytes(text.span8()));
        return;
    }
    auto units = text.span16();
    if (encoding == TextEncoding::UTF16LittleEndian) {
        hasher.update(asBytes(units));
        return;
    }
    std::array<uint8_t, 4096> chunk;
    while (!units.empty()) {
        size_t count = std::min(units.size(), chunk.size());
        for (size_t index = 0; index < count; ++index)
            chunk[index] = static_cast<uint8_t>(units[index]);
        hasher.update(std::span { chunk }.first(count));
        units = units.subspan(count);
    }
}

// One record of sections 3.2 to 3.4: a 16-byte ASCII label without terminator, then fixed-width little-endian fields,
// canonical strings and digests, fed to SHA-256 as they are written.
class RecordHasher {
    WTF_MAKE_NONCOPYABLE(RecordHasher);

public:
    static constexpr size_t labelSize = 16;

    explicit RecordHasher(const char (&label)[labelSize + 1])
    {
        m_hasher.update(asBytes(std::span { label }.first<labelSize>()));
    }

    void u8(uint8_t value)
    {
        std::array<uint8_t, 1> encoded { value };
        m_hasher.update(encoded);
    }

    void u16(uint16_t value)
    {
        std::array<uint8_t, 2> encoded { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8) };
        m_hasher.update(encoded);
    }

    void u32(uint32_t value)
    {
        std::array<uint8_t, 4> encoded {
            static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24),
        };
        m_hasher.update(encoded);
    }

    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void flag(bool value) { u8(value ? 1 : 0); }
    void bytes(std::span<const uint8_t> value) { m_hasher.update(value); }

    // Section 3.3: u8 encoding, u32 length in code units, then the units; a null identifier is encoding 0 with length 0.
    void canonicalString(const StringImpl* string)
    {
        if (!string) {
            u8(static_cast<uint8_t>(TextEncoding::Null));
            u32(0);
            return;
        }
        StringView text { *string };
        TextEncoding encoding = textEncodingOf(text);
        u8(static_cast<uint8_t>(encoding));
        u32(text.length());
        hashCodeUnits(m_hasher, text, encoding);
    }

    Digest256 finalize() { return m_hasher.finalize(); }

private:
    SHA256 m_hasher;
};

} // namespace UCBKeysInternal

BodyKey BodyKey::make(IdentityKind kind, CodeSpecializationKind specialization, OptionSet<CodeGenerationMode> mode, const Digest256& identityDigest)
{
    using namespace UCBKeysInternal;

    ASSERT(isDefinedIdentityKind(static_cast<uint8_t>(kind)));
    ASSERT(specialization == CodeSpecializationKind::CodeForCall || !isCallOnlyKind(kind));
    ASSERT(mode.containsOnly(definedCodeGenerationModes));

    BodyKey key { };
    key.m_bytes[versionOffset] = bodyKeyVersion;
    key.m_bytes[identityKindOffset] = static_cast<uint8_t>(kind);
    key.m_bytes[specializationOffset] = specialization == CodeSpecializationKind::CodeForConstruct ? 1 : 0;
    key.m_bytes[codeGenerationModeOffset] = mode.toRaw();
    memcpySpan(std::span { key.m_bytes }.subspan<identityDigestOffset>(), std::span { identityDigest });
    return key;
}

std::optional<BodyKey> BodyKey::fromBytes(std::span<const uint8_t, byteSize> bytes)
{
    using namespace UCBKeysInternal;

    if (bytes[versionOffset] != bodyKeyVersion)
        return std::nullopt;
    if (!isDefinedIdentityKind(bytes[identityKindOffset]))
        return std::nullopt;
    auto kind = static_cast<IdentityKind>(bytes[identityKindOffset]);
    uint8_t specialization = bytes[specializationOffset];
    if (specialization > 1 || (specialization && isCallOnlyKind(kind)))
        return std::nullopt;
    if (!OptionSet<CodeGenerationMode>::fromRaw(bytes[codeGenerationModeOffset]).containsOnly(definedCodeGenerationModes))
        return std::nullopt;
    if (std::ranges::any_of(bytes.subspan<reservedOffset, reservedSize>(), [](uint8_t byte) { return !!byte; }))
        return std::nullopt;

    BodyKey key;
    memcpySpan(std::span { key.m_bytes }, bytes);
    return key;
}

std::span<const uint8_t, BodyKey::byteSize> BodyKey::bytes() const
{
    return std::span { m_bytes };
}

IdentityKind BodyKey::identityKind() const
{
    return static_cast<IdentityKind>(m_bytes[UCBKeysInternal::identityKindOffset]);
}

CodeSpecializationKind BodyKey::specialization() const
{
    return m_bytes[UCBKeysInternal::specializationOffset] ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
}

OptionSet<CodeGenerationMode> BodyKey::codeGenerationMode() const
{
    return OptionSet<CodeGenerationMode>::fromRaw(m_bytes[UCBKeysInternal::codeGenerationModeOffset]);
}

const Digest256& BodyKey::identityDigest() const
{
    return reinterpretCastSpanStartTo<Digest256>(std::span { m_bytes }.subspan<UCBKeysInternal::identityDigestOffset>());
}

Digest256 rootIdentityDigest(IdentityKind kind, const Digest256& sourceDigest, LexicallyScopedFeatures features, std::optional<int32_t> parameterEnd)
{
    using namespace UCBKeysInternal;

    ASSERT(isGlobalKind(kind) || isRootExecutableKind(kind));
    ASSERT(!parameterEnd || kind == IdentityKind::FunctionConstructor);

    RecordHasher hasher("JITCache.root.v1");
    hasher.u8(static_cast<uint8_t>(kind));
    hasher.flag(features & StrictModeLexicallyScopedFeature);
    hasher.flag(features & TaintedByWithScopeLexicallyScopedFeature);
    hasher.flag(parameterEnd.has_value());
    hasher.i32(parameterEnd.value_or(0));
    hasher.bytes(sourceDigest);
    return hasher.finalize();
}

Digest256 childIdentityDigest(const BodyKey& parent, ChildTable table, uint32_t index)
{
    UCBKeysInternal::RecordHasher hasher("JITCache.chld.v1");
    hasher.bytes(parent.bytes());
    hasher.u8(static_cast<uint8_t>(table));
    hasher.u32(index);
    return hasher.finalize();
}

Digest256 directEvalIdentityDigest(const BodyKey& caller, BytecodeIndex bytecodeIndex, const Digest256& textDigest)
{
    UCBKeysInternal::RecordHasher hasher("JITCache.deva.v1");
    hasher.bytes(caller.bytes());
    hasher.u32(bytecodeIndex.offset());
    hasher.bytes(textDigest);
    return hasher.finalize();
}

Digest256 globalContextDigest(IdentityKind kind, unsigned providerOffset, unsigned firstLine, JSParserScriptMode scriptMode, DerivedContextType derivedContextType, EvalContextType evalContextType, bool isArrowFunctionContext)
{
    ASSERT(UCBKeysInternal::isGlobalKind(kind));

    UCBKeysInternal::RecordHasher hasher("JITCache.gctx.v1");
    hasher.u8(static_cast<uint8_t>(kind));
    hasher.u32(providerOffset);
    hasher.u32(firstLine);
    hasher.u8(static_cast<uint8_t>(scriptMode));
    hasher.u8(static_cast<uint8_t>(derivedContextType));
    hasher.u8(static_cast<uint8_t>(evalContextType));
    hasher.flag(isArrowFunctionContext);
    return hasher.finalize();
}

Digest256 executableBodyContextDigest(IdentityKind kind, unsigned providerOffset, unsigned firstLine, const std::optional<Digest256>& rootHolderDigest)
{
    using namespace UCBKeysInternal;

    // A root body's context carries its UFE's holder digest; a child's writes zero, since its holder UFE is pinned by its
    // parent and C11 compares it instead.
    bool isRoot = isRootExecutableKind(kind);
    ASSERT(isRoot || kind == IdentityKind::Child);
    ASSERT(!isRoot || rootHolderDigest);

    RecordHasher hasher("JITCache.fctx.v1");
    hasher.u8(static_cast<uint8_t>(kind));
    hasher.u32(providerOffset);
    hasher.u32(firstLine);
    hasher.flag(isRoot);
    hasher.bytes(isRoot && rootHolderDigest ? *rootHolderDigest : Digest256 { });
    return hasher.finalize();
}

std::optional<Digest256> holderDigest(VM& vm, const UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, unsigned* environmentsDigested)
{
    // Section 3.4: the label, the UFE's descriptor, which the codec streams page by page without assembling it (codec E13),
    // then its TDZ chain digest, which keeps each environment's digest in the environment.
    UCBKeysInternal::RecordHasher hasher("JITCache.ufed.v1");
    auto hashChunk = [&](std::span<const uint8_t> chunk) {
        hasher.bytes(chunk);
    };
    if (forEachUnlinkedFunctionExecutableDescriptorChunk(vm, executable, budget, hashChunk) != CoreEncodeFailure::None)
        return std::nullopt;

    RefPtr chain = executable.parentScopeTDZVariables();
    unsigned digestedFirst = 0;
    std::optional<Digest256> chainDigest = tdzChainDigest(chain.get(), budget, digestedFirst);
    // Environments digested before a refusal keep their digests, so they count either way.
    if (environmentsDigested)
        *environmentsDigested += digestedFirst;
    if (!chainDigest)
        return std::nullopt;
    hasher.bytes(*chainDigest);
    return hasher.finalize();
}

Digest256 directEvalContextDigest(unsigned providerOffset, unsigned firstLine, LexicallyScopedFeatures lexicallyScopedFeatures, DerivedContextType derivedContextType,
    NeedsClassFieldInitializer needsClassFieldInitializer, PrivateBrandRequirement privateBrandRequirement, bool isArrowFunctionContext, bool isInsideOrdinaryFunction,
    EvalContextType evalContextType, const TDZEnvironment& variablesUnderTDZ, const PrivateNameEnvironment& privateNameEnvironment)
{
    UCBKeysInternal::RecordHasher hasher("JITCache.dctx.v1");
    hasher.u32(providerOffset);
    hasher.u32(firstLine);
    hasher.u8(lexicallyScopedFeatures);
    hasher.u8(static_cast<uint8_t>(derivedContextType));
    hasher.u8(static_cast<uint8_t>(needsClassFieldInitializer));
    hasher.u8(static_cast<uint8_t>(privateBrandRequirement));
    hasher.flag(isArrowFunctionContext);
    hasher.flag(isInsideOrdinaryFunction);
    hasher.u8(static_cast<uint8_t>(evalContextType));

    // Both sets are hash tables, so their names are sorted by their characters (codePointCompare) to make the record
    // independent of insertion order. Two names with the same characters write the same bytes, so their order is moot.
    Vector<const UniquedStringImpl*> names = WTF::map(variablesUnderTDZ, [](const RefPtr<UniquedStringImpl>& name) -> const UniquedStringImpl* {
        return name.get();
    });
    std::ranges::sort(names, WTF::codePointCompareLessThan, [](const UniquedStringImpl* name) {
        return StringView { name };
    });
    hasher.u32(static_cast<uint32_t>(names.size()));
    for (auto* name : names)
        hasher.canonicalString(name);

    // A private name's bits break a tie between equal names, which no generation produces, so the order stays total.
    using PrivateNameRecord = std::pair<const UniquedStringImpl*, uint16_t>;
    Vector<PrivateNameRecord> entries = WTF::map(privateNameEnvironment, [](const auto& entry) {
        return PrivateNameRecord { entry.key.get(), entry.value.bits() };
    });
    std::ranges::sort(entries, [](const PrivateNameRecord& a, const PrivateNameRecord& b) {
        auto order = codePointCompare(StringView { a.first }, StringView { b.first });
        return std::is_lt(order) || (std::is_eq(order) && a.second < b.second);
    });
    hasher.u32(static_cast<uint32_t>(entries.size()));
    for (auto& [name, bits] : entries) {
        hasher.canonicalString(name);
        hasher.u16(bits);
    }
    return hasher.finalize();
}

std::optional<Digest256> contextDigestOf(IdentityKind kind, const RecordedContext& context, const std::optional<Digest256>& rootHolderDigest)
{
    using namespace UCBKeysInternal;

    return WTF::switchOn(context,
        [&](const GlobalContextInputs& inputs) -> std::optional<Digest256> {
            ASSERT(isGlobalKind(kind));
            return globalContextDigest(kind, inputs.providerOffset, inputs.firstLine, inputs.scriptMode, inputs.derivedContextType, inputs.evalContextType, inputs.isArrowFunctionContext);
        },
        [&](const BodyContextInputs& inputs) -> std::optional<Digest256> {
            if (!isRootExecutableKind(kind))
                return executableBodyContextDigest(kind, inputs.providerOffset, inputs.firstLine, std::nullopt);
            if (!rootHolderDigest)
                return std::nullopt;
            return executableBodyContextDigest(kind, inputs.providerOffset, inputs.firstLine, rootHolderDigest);
        },
        [&](const DirectEvalContext& inputs) -> std::optional<Digest256> {
            ASSERT(kind == IdentityKind::DirectEval);
            return inputs.digest;
        });
}

Digest256 sourceDigest(StringView text)
{
    using namespace UCBKeysInternal;

    SHA256 hasher;
    TextEncoding encoding = textEncodingOf(text);
    std::array<uint8_t, 1> encodingByte { static_cast<uint8_t>(encoding) };
    hasher.update(encodingByte);
    hashCodeUnits(hasher, text, encoding);
    return hasher.finalize();
}

RootSourceDigest rootSourceDigest(const SourceCode& rootSource, const Digest256* builtinMetadataDigest)
{
    // Form 1: the builtins generator's digest of the characters name##Source() spans.
    if (builtinMetadataDigest)
        return { *builtinMetadataDigest, SourceDigestOrigin::BuiltinMetadata };

    // Form 2: a digest recorded where the provider's text was produced, for a root that spans that text. The test reads
    // only offsets and the source's length, which a StringImpl holds apart from its characters (F25).
    if (auto* provider = rootSource.provider()) {
        bool spansProvider = !rootSource.startOffset() && rootSource.endOffset() >= 0
            && static_cast<unsigned>(rootSource.endOffset()) == provider->source().length();
        if (spansProvider) {
            if (auto supplied = provider->jitCacheSourceDigest())
                return { *supplied, SourceDigestOrigin::Provider };
        }
    }

    // Form 3: the text itself.
    return { sourceDigest(rootSource.view()), SourceDigestOrigin::Computed };
}

} // namespace JSC::JITCache
