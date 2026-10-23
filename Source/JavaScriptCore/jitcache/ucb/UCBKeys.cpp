#include "config.h"
#include "jitcache/ucb/UCBKeys.h"

#include "CachedTypes.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "UnlinkedFunctionExecutable.h"
#include "jitcache/base/JITCacheSHA256.h"
#include "jitcache/session/JITCacheBun.h"
#include <algorithm>
#include <bit>
#include <compare>
#include <utility>
#include <wtf/Noncopyable.h>
#include <wtf/Scope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIIFastPath.h>
#include <wtf/text/StringView.h>
#include <wtf/text/SymbolImpl.h>
#include <wtf/text/UniquedStringImpl.h>

namespace JSC::JITCache {

namespace UCBKeysInternal {

// UTF-16LE text streams in place, from the units' own bytes, as UCBSections.cpp's little-endian sections assume too.
static_assert(std::endian::native == std::endian::little, "JITCache digests read UTF-16 code units as little-endian bytes");

// The roots whose body a UFE holds: a Function constructor's and a builtin's.
static constexpr bool isRootExecutableKind(IdentityKind kind)
{
    return kind == IdentityKind::FunctionConstructor || kind == IdentityKind::Builtin;
}

static constexpr bool isGlobalKind(IdentityKind kind)
{
    return kind == IdentityKind::Program || kind == IdentityKind::Module || kind == IdentityKind::IndirectEval;
}

// The text encodings of canonical strings and source digests. Null is a null identifier's, and only canonical strings
// write it.
enum class TextEncoding : uint8_t { Null = 0, Latin1 = 1, UTF16LittleEndian = 2 };

// Latin-1 exactly when every unit is at most 0xFF, so an 8-bit and a 16-bit string with the same characters encode alike.
static TextEncoding textEncodingOf(StringView text)
{
    if (text.is8Bit() || WTF::charactersAreAllLatin1(text.span16()))
        return TextEncoding::Latin1;
    return TextEncoding::UTF16LittleEndian;
}

// The code units of `text` in the encoding textEncodingOf gave it, with no copy of the text: 8-bit and UTF-16LE text stream
// in place, and 16-bit text narrowed to Latin-1 streams through a 4 KiB chunk.
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

// One record of an identity, context or holder digest: a 16-byte ASCII label without terminator, then fixed-width little-endian fields,
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

    // A canonical string: u8 encoding, u32 length in code units, then the units; a null identifier is encoding 0 with
    // length 0.
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

// A TDZ environment's names in a fixed order: by their characters, then by the kind of StringImpl each one is. Two names
// equal in both write the same bytes, so their order is moot.
static unsigned tdzNameKind(const UniquedStringImpl* name)
{
    if (!name->isSymbol())
        return 0;
    auto& symbol = *static_cast<const SymbolImpl*>(name);
    return 1 + symbol.isRegistered() * 2 + symbol.isPrivate();
}

static bool tdzNameLess(const UniquedStringImpl* a, const UniquedStringImpl* b)
{
    auto order = codePointCompare(StringView { a }, StringView { b });
    if (std::is_neq(order))
        return std::is_lt(order);
    return tdzNameKind(a) < tdzNameKind(b);
}

} // namespace UCBKeysInternal

// The TDZ digests a holder digest ends with. They read the names and links that CompactTDZEnvironment and
// TDZEnvironmentLink keep private, so each of those classes befriends this struct.
struct TDZDigests {
    // An environment's digest, computed on the VM thread when a holder digest first reaches the environment and kept in it
    // until it frees its names. The budget is charged for the sort buffer while it lives; on a refusal the digest is empty
    // and the environment keeps none.
    static std::optional<Digest256> ofEnvironment(const CompactTDZEnvironment& environment, CoreEncodingBudget* budget, unsigned& environmentsDigested)
    {
        using namespace UCBKeysInternal;

        if (environment.m_contentDigest)
            return *environment.m_contentDigest;

        size_t count = WTF::switchOn(environment.m_variables,
            [](const CompactTDZEnvironment::Compact& compact) -> size_t { return compact.size(); },
            [](const CompactTDZEnvironment::Inflated& inflated) -> size_t { return inflated.size(); });
        size_t sortBufferBytes = count * sizeof(const UniquedStringImpl*);
        if (budget && sortBufferBytes && !budget->charge(sortBufferBytes))
            return std::nullopt;
        Digest256 digest;
        {
            auto releaseSortBuffer = makeScopeExit([&] {
                if (budget && sortBufferBytes)
                    budget->release(sortBufferBytes);
            });
            Vector<const UniquedStringImpl*> names;
            names.reserveInitialCapacity(count);
            WTF::switchOn(environment.m_variables,
                [&](const CompactTDZEnvironment::Compact& compact) {
                    for (auto& name : compact)
                        names.append(name.get());
                },
                [&](const CompactTDZEnvironment::Inflated& inflated) {
                    for (auto& name : inflated)
                        names.append(name.get());
                });
            std::ranges::sort(names, tdzNameLess);

            // The name count, then each name's kind and canonical string.
            RecordHasher hasher("JITCache.tdze.v1");
            hasher.u32(static_cast<uint32_t>(names.size()));
            for (const UniquedStringImpl* name : names) {
                hasher.u8(static_cast<uint8_t>(tdzNameKind(name)));
                hasher.canonicalString(name);
            }
            digest = hasher.finalize();
        }
        const_cast<CompactTDZEnvironment&>(environment).m_contentDigest = makeUniqueWithoutFastMallocCheck<Digest256>(digest);
        ++environmentsDigested;
        return digest;
    }

    // A chain's digest. A link's digest hashes its environment's digest and its parent's digest, 32 zero bytes after the
    // last link, so the links are listed and digested from the chain's end. A chain longer than the links the list keeps
    // inline moves the list to the heap, which the budget is charged for before the list is allocated and while it lives.
    static std::optional<Digest256> ofChain(const TDZEnvironmentLink* chain, CoreEncodingBudget* budget, unsigned& environmentsDigested)
    {
        Digest256 digest { };
        if (!chain)
            return digest;
        constexpr size_t inlineLinks = 32;
        size_t count = 0;
        for (const TDZEnvironmentLink* link = chain; link; link = link->m_parent.get())
            ++count;
        size_t linksBytes = count > inlineLinks ? count * sizeof(const TDZEnvironmentLink*) : 0;
        if (budget && linksBytes && !budget->charge(linksBytes))
            return std::nullopt;
        auto releaseLinks = makeScopeExit([&] {
            if (budget && linksBytes)
                budget->release(linksBytes);
        });
        Vector<const TDZEnvironmentLink*, inlineLinks> links;
        links.reserveInitialCapacity(count);
        ASSERT((links.capacity() > inlineLinks ? links.capacity() * sizeof(const TDZEnvironmentLink*) : 0) == linksBytes);
        for (const TDZEnvironmentLink* link = chain; link; link = link->m_parent.get())
            links.append(link);
        for (size_t i = links.size(); i--;) {
            // Every live link holds an environment the VM interned: a handle has its map and its environment, or neither.
            const CompactTDZEnvironmentMap::Handle& handle = links[i]->m_handle;
            RELEASE_ASSERT(handle);
            std::optional<Digest256> environmentDigest = ofEnvironment(handle.environment(), budget, environmentsDigested);
            if (!environmentDigest)
                return std::nullopt;
            UCBKeysInternal::RecordHasher hasher("JITCache.tdzl.v1");
            hasher.bytes(*environmentDigest);
            hasher.bytes(digest);
            digest = hasher.finalize();
        }
        return digest;
    }
};

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

Digest256 globalContextDigest(IdentityKind kind, unsigned providerOffset, JSParserScriptMode scriptMode, DerivedContextType derivedContextType, EvalContextType evalContextType, bool isArrowFunctionContext)
{
    ASSERT(UCBKeysInternal::isGlobalKind(kind));

    UCBKeysInternal::RecordHasher hasher("JITCache.gctx.v1");
    hasher.u8(static_cast<uint8_t>(kind));
    hasher.u32(providerOffset);
    hasher.u8(static_cast<uint8_t>(scriptMode));
    hasher.u8(static_cast<uint8_t>(derivedContextType));
    hasher.u8(static_cast<uint8_t>(evalContextType));
    hasher.flag(isArrowFunctionContext);
    return hasher.finalize();
}

Digest256 executableBodyContextDigest(IdentityKind kind, unsigned providerOffset, const std::optional<Digest256>& rootHolderDigest)
{
    using namespace UCBKeysInternal;

    // A root body's context carries its UFE's holder digest; a child's writes zero, since its holder UFE is pinned by its
    // parent and the holder check compares it instead.
    bool isRoot = isRootExecutableKind(kind);
    ASSERT(isRoot || kind == IdentityKind::Child);
    ASSERT(!isRoot || rootHolderDigest);

    RecordHasher hasher("JITCache.fctx.v1");
    hasher.u8(static_cast<uint8_t>(kind));
    hasher.u32(providerOffset);
    hasher.flag(isRoot);
    hasher.bytes(isRoot && rootHolderDigest ? *rootHolderDigest : Digest256 { });
    return hasher.finalize();
}

std::optional<Digest256> tdzChainDigest(const TDZEnvironmentLink* chain, CoreEncodingBudget* budget, unsigned& environmentsDigested)
{
    return TDZDigests::ofChain(chain, budget, environmentsDigested);
}

std::optional<Digest256> holderDigest(VM& vm, const UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, unsigned* environmentsDigested)
{
    // The label, the UFE's descriptor, which the codec streams page by page without assembling it, then its TDZ chain
    // digest, which keeps each environment's digest in the environment.
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

Digest256 directEvalContextDigest(unsigned providerOffset, LexicallyScopedFeatures lexicallyScopedFeatures, DerivedContextType derivedContextType,
    NeedsClassFieldInitializer needsClassFieldInitializer, PrivateBrandRequirement privateBrandRequirement, bool isArrowFunctionContext, bool isInsideOrdinaryFunction,
    EvalContextType evalContextType, const TDZEnvironment& variablesUnderTDZ, const PrivateNameEnvironment& privateNameEnvironment)
{
    UCBKeysInternal::RecordHasher hasher("JITCache.dctx.v1");
    hasher.u32(providerOffset);
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
            return globalContextDigest(kind, inputs.providerOffset, inputs.scriptMode, inputs.derivedContextType, inputs.evalContextType, inputs.isArrowFunctionContext);
        },
        [&](const BodyContextInputs& inputs) -> std::optional<Digest256> {
            if (!isRootExecutableKind(kind))
                return executableBodyContextDigest(kind, inputs.providerOffset, std::nullopt);
            if (!rootHolderDigest)
                return std::nullopt;
            return executableBodyContextDigest(kind, inputs.providerOffset, rootHolderDigest);
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

// JITCacheBun.h exports it for the digest Bun's providers supply, which must equal sourceDigest's.
Digest256 providerSourceDigest(StringView source)
{
    return sourceDigest(source);
}

RootSourceDigest rootSourceDigest(const SourceCode& rootSource, const Digest256* builtinMetadataDigest)
{
    // The builtins generator's digest of the characters name##Source() spans.
    if (builtinMetadataDigest)
        return { *builtinMetadataDigest, SourceDigestOrigin::BuiltinMetadata };

    // A digest recorded where the provider's text was produced, for a root that spans that text. The test reads only
    // offsets and the source's length, which a StringImpl holds apart from its characters.
    if (auto* provider = rootSource.provider()) {
        bool spansProvider = !rootSource.startOffset() && rootSource.endOffset() >= 0
            && static_cast<unsigned>(rootSource.endOffset()) == provider->source().length();
        if (spansProvider) {
            if (auto supplied = provider->jitCacheSourceDigest())
                return { *supplied, SourceDigestOrigin::Provider };
        }
    }

    // Otherwise the text itself.
    return { sourceDigest(rootSource.view()), SourceDigestOrigin::Computed };
}

} // namespace JSC::JITCache
