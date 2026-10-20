#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BuiltinExecutables.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DirectEvalExecutable.h"
#include "DirectEvalSite.h"
#include "IndirectEvalExecutable.h"
#include "JITCacheSHA256.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "ModuleProgramExecutable.h"
#include "ProgramExecutable.h"
#include "SourceProvider.h"
#include "StrongInlines.h"
#include "TestFiles.h"
#include "UCBImport.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UCBRequests.h"
#include "UCBTestSupport.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <sys/mman.h>
#include <wtf/ASCIICType.h>
#include <wtf/PageBlock.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// The tests of UCBKeys: sourceDigest and rootSourceDigest; the identity, context, holder and TDZ chain digests, pinned to
// literal vectors; what each kind of request computes through UCBImport's requestKey and requestContext; and the source
// digests of the builtin metadata BuiltinExecutables holds. BodyKey's bytes are tested with base/.

namespace JSC::JITCache::Tests {

namespace UCBKeysTestsInternal {

static Digest256 digestFromHex(ASCIILiteral hex)
{
    Digest256 digest { };
    auto characters = hex.span8();
    RELEASE_ASSERT(characters.size() == 2 * digest.size());
    for (size_t i = 0; i < digest.size(); ++i)
        digest[i] = toASCIIHexValue(characters[2 * i], characters[2 * i + 1]);
    return digest;
}

// The bytes a hex record spells, skipping the spaces that separate its fields.
static Vector<uint8_t> bytesFromHex(ASCIILiteral hex)
{
    Vector<uint8_t> bytes;
    std::optional<uint8_t> highNibble;
    for (auto character : hex.span8()) {
        if (character == ' ')
            continue;
        RELEASE_ASSERT(isASCIIHexDigit(character));
        uint8_t nibble = toASCIIHexValue(character);
        if (!highNibble) {
            highNibble = nibble;
            continue;
        }
        bytes.append(static_cast<uint8_t>(*highNibble << 4 | nibble));
        highNibble = std::nullopt;
    }
    RELEASE_ASSERT(!highNibble);
    return bytes;
}

// The source digest's encoding written out byte by byte, apart from sourceDigest: u8 1 then the Latin-1 bytes when every
// unit is at most 0xFF, u8 2 then UTF-16LE otherwise.
static Digest256 referenceSourceDigest(StringView text)
{
    bool isLatin1 = true;
    for (unsigned i = 0; i < text.length(); ++i)
        isLatin1 &= text[i] <= 0xFF;
    Vector<uint8_t> bytes;
    bytes.append(isLatin1 ? 1 : 2);
    for (unsigned i = 0; i < text.length(); ++i) {
        char16_t unit = text[i];
        bytes.append(static_cast<uint8_t>(unit));
        if (!isLatin1)
            bytes.append(static_cast<uint8_t>(unit >> 8));
    }
    return SHA256::hash(bytes.span());
}

// `length` 8-bit units mixing printable ASCII with the upper half of Latin-1.
static String latin1TestText(unsigned length)
{
    Vector<Latin1Character> characters;
    characters.reserveInitialCapacity(length);
    for (unsigned i = 0; i < length; ++i)
        characters.append(static_cast<Latin1Character>(i % 3 ? 0x20 + (i * 7) % 0x5F : 0xA0 + (i * 13) % 0x60));
    return String { characters.span() };
}

// A 16-bit string with the characters of `text`, its last unit replaced by `lastUnit` when given.
static String sixteenBitCopy(StringView text, std::optional<char16_t> lastUnit = std::nullopt)
{
    Vector<char16_t> units;
    units.reserveInitialCapacity(text.length());
    for (unsigned i = 0; i < text.length(); ++i)
        units.append(text[i]);
    if (lastUnit && !units.isEmpty())
        units.last() = *lastUnit;
    return String { units.span() };
}

// A provider that supplies a digest and whose text lies on a page that cannot be read: reading the length touches no
// character, and any read of a character faults, so a digest taken from it proves that no character was read.
class UnreadableTextProvider final : public SourceProvider {
public:
    static RefPtr<UnreadableTextProvider> create(unsigned length, const Digest256& digest)
    {
        size_t size = roundUpToMultipleOf(pageSize(), std::max<size_t>(length, 1));
        void* page = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return nullptr;
        return adoptRef(*new UnreadableTextProvider(page, size, length, digest));
    }

    ~UnreadableTextProvider() final
    {
        munmap(m_page, m_size);
    }

    unsigned hash() const final { return m_length; }
    StringView source() const final { return StringView { unsafeMakeSpan(static_cast<const Latin1Character*>(m_page), m_length) }; }
    std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const final { return m_digest; }

private:
    UnreadableTextProvider(void* page, size_t size, unsigned length, const Digest256& digest)
        : SourceProvider(SourceOrigin { }, String { }, String { }, SourceTaintedOrigin::Untainted, TextPosition { }, SourceProviderSourceType::Program)
        , m_page(page)
        , m_size(size)
        , m_length(length)
        , m_digest(digest)
    {
    }

    void* const m_page;
    const size_t m_size;
    const unsigned m_length;
    const Digest256 m_digest;
};

static void addPrivateName(VM& vm, PrivateNameEnvironment& environment, const String& name, uint16_t traits)
{
    environment.add(PackedRefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() }, PrivateNameEntry(traits));
}

// sourceDigest's two encodings and the chunks it narrows 16-bit text in, and the forms of rootSourceDigest: a builtin
// metadata digest, a provider's supplied digest for a source that spans the provider, and a digest computed from the text.
static bool testSourceDigests(TestPart& part)
{
    // Pinned from the encoding alone: "abc" as 0x01 then Latin-1, U+0100 U+007A as 0x02 then UTF-16LE.
    if (!part.check(sourceDigest("abc"_s) == digestFromHex("1e18834c426d00e57788444cb3ccd62c771b420c095bb0c4e040a8c122c4570d"_s), "sourceDigest of \"abc\" is not SHA-256 of 0x01 and its Latin-1 bytes"_s))
        return false;
    std::array<char16_t, 2> wideUnits { 0x100, 0x7A };
    if (!part.check(sourceDigest(StringView { std::span<const char16_t> { wideUnits } }) == digestFromHex("e7dd58da0cdf47c2cc0339b6eaf83a8369b7a14a9d050d9a2a88948034744f20"_s), "sourceDigest of U+0100 U+007A is not SHA-256 of 0x02 and its UTF-16LE units"_s))
        return false;

    // 16-bit views are narrowed in chunks of 4 KiB, so lengths around the boundary cross it.
    for (unsigned length : { 0u, 1u, 4095u, 4096u, 4097u, 8193u }) {
        String narrow = latin1TestText(length);
        String wide = sixteenBitCopy(narrow);
        Digest256 expected = referenceSourceDigest(narrow);
        bool sameEncoding = (!length || (narrow.is8Bit() && !wide.is8Bit())) && sourceDigest(narrow) == expected && sourceDigest(wide) == expected;
        if (!part.check(sameEncoding, makeString("an 8-bit and a 16-bit string of "_s, length, " Latin-1 characters do not both digest as 0x01 and their bytes"_s)))
            return false;
        if (!length)
            continue;
        String beyondLatin1 = sixteenBitCopy(narrow, 0x100);
        Digest256 beyondDigest = sourceDigest(beyondLatin1);
        if (!part.check(beyondDigest == referenceSourceDigest(beyondLatin1) && beyondDigest != expected, makeString("a unit above 0xFF in a string of "_s, length, " units does not switch it to UTF-16LE"_s)))
            return false;
    }

    // rootSourceDigest takes a supplied digest as given, so the one supplied here is deliberately not the text's.
    String text = "var jitcacheU2Root = 1;\n"_s;
    Digest256 supplied = filledDigest(0x5d);
    Digest256 builtin = filledDigest(0xb1);
    Ref<SuppliedDigestProvider> provider = SuppliedDigestProvider::create(text, supplied);
    SourceCode whole { Ref<SourceProvider> { provider.get() } };
    auto fromMetadata = rootSourceDigest(whole, &builtin);
    if (!part.check(fromMetadata.digest == builtin && fromMetadata.origin == SourceDigestOrigin::BuiltinMetadata, "a builtin metadata digest does not win over the provider's"_s))
        return false;
    auto fromProvider = rootSourceDigest(whole);
    if (!part.check(fromProvider.digest == supplied && fromProvider.origin == SourceDigestOrigin::Provider, "a source that spans its provider does not take the provider's digest"_s))
        return false;
    SourceCode partial { RefPtr<SourceProvider> { provider.ptr() }, 1, static_cast<int>(text.length()) };
    auto fromPart = rootSourceDigest(partial);
    if (!part.check(fromPart.digest == sourceDigest(partial.view()) && fromPart.origin == SourceDigestOrigin::Computed, "a source over part of a supplying provider does not compute its digest"_s))
        return false;
    Ref<StringSourceProvider> plain = StringSourceProvider::create(text, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    auto fromText = rootSourceDigest(SourceCode { Ref<SourceProvider> { plain.get() } });
    if (!part.check(fromText.digest == sourceDigest(text) && fromText.origin == SourceDigestOrigin::Computed, "a provider that supplies no digest does not have its digest computed"_s))
        return false;

    // A supplied digest is read without a character of the text: any read of this provider's text faults.
    RefPtr<UnreadableTextProvider> unreadable = UnreadableTextProvider::create(64, supplied);
    if (!part.check(!!unreadable, "the page of the unreadable provider could not be mapped"_s))
        return false;
    auto unread = rootSourceDigest(SourceCode { Ref<SourceProvider> { *unreadable } });
    return part.check(unread.digest == supplied && unread.origin == SourceDigestOrigin::Provider, "a source that spans an unreadable supplying provider does not take its digest"_s);
}

// Identity, child, direct-eval, context and TDZ chain digests of fixed inputs equal literal vectors. Each vector is pinned
// beside the record it is the SHA-256 of, laid out as the key derivation writes it: its 16-byte label, then its fields in
// hex, integers little-endian, booleans and enumerations one byte each, and a canonical string as its encoding byte, its
// u32 length and its units. The test also hashes every record, so a vector that is not its record's digest fails with a
// message of its own, and sha256sum over the label's ASCII followed by the record's bytes recomputes any vector outside the
// engine.
static bool testPinnedDigests(VM& vm, TestPart& part)
{
    // Every child and direct-eval record below holds this key's 40 bytes: version 1, kind Program, call, no mode, four
    // reserved bytes, then 0x11 32 times.
    BodyKey parent = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x11));
    TDZEnvironment tdz = tdzNames(vm, { "b"_s, "a"_s });
    PrivateNameEnvironment privateNames;
    addPrivateName(vm, privateNames, "#y"_s, PrivateNameEntry::IsMethod | PrivateNameEntry::IsStatic);
    addPrivateName(vm, privateNames, "#x"_s, PrivateNameEntry::IsMethod);
    // One link over the environment {beta, alpha}, whose names an environment's record writes sorted; one link over
    // {gamma}; and a chain of two links, a link over {beta, alpha} parented on the {gamma} link. The two-link chain's record
    // holds its parent link's digest where the one-link chain's holds zeros, so a chain digest that left parents out would
    // give it the one-link vector.
    RefPtr<TDZEnvironmentLink> link = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "beta"_s, "alpha"_s })), nullptr);
    RefPtr<TDZEnvironmentLink> gammaLink = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "gamma"_s })), nullptr);
    RefPtr<TDZEnvironmentLink> twoLinks = TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, { "beta"_s, "alpha"_s })), gammaLink);
    unsigned environmentsDigested = 0;
    auto chainDigest = tdzChainDigest(link.get(), nullptr, environmentsDigested);
    auto gammaChainDigest = tdzChainDigest(gammaLink.get(), nullptr, environmentsDigested);
    auto twoLinkDigest = tdzChainDigest(twoLinks.get(), nullptr, environmentsDigested);
    if (!part.check(chainDigest && gammaChainDigest && twoLinkDigest, "tdzChainDigest refused a chain with no budget to refuse it"_s))
        return false;

    LexicallyScopedFeatures strictWithScope = StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature;
    struct Pinned {
        std::optional<Digest256> actual; // empty for a record no function digests on its own
        ASCIILiteral label;
        ASCIILiteral record;
        ASCIILiteral expected;
        ASCIILiteral what;
    };
    std::array<Pinned, 15> pinned { {
        { rootIdentityDigest(IdentityKind::Program, filledDigest(0x5a), strictWithScope, std::nullopt), "JITCache.root.v1"_s,
            "01 01 01 00 00000000 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"_s,
            "ddedb7e3c271a41211e9e1fcad9ae83d16e11ecdf5f2bff9df39b97397d45647"_s, "a program's root identity digest"_s },
        { rootIdentityDigest(IdentityKind::FunctionConstructor, filledDigest(0x5a), NoLexicallyScopedFeatures, 17), "JITCache.root.v1"_s,
            "04 00 00 01 11000000 5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a"_s,
            "8922be7f3986be8b237051c463e64d3d639951b82761f48ff4be2452bc6882c3"_s, "a Function-constructor root's identity digest"_s },
        { childIdentityDigest(parent, ChildTable::Expressions, 7), "JITCache.chld.v1"_s,
            "01 01 00 00 00000000 1111111111111111111111111111111111111111111111111111111111111111 01 07000000"_s,
            "eb438a2767b839118e59e6edfefe15c8fe053090eb88c897546e30236b3085d6"_s, "a child identity digest"_s },
        { directEvalIdentityDigest(parent, BytecodeIndex(42), filledDigest(0x22)), "JITCache.deva.v1"_s,
            "01 01 00 00 00000000 1111111111111111111111111111111111111111111111111111111111111111 2a000000 2222222222222222222222222222222222222222222222222222222222222222"_s,
            "030199b882b969d4c9f3fd1d3d96cfd9934f10c11311967ea198290dc49473c4"_s, "a direct-eval identity digest"_s },
        { globalContextDigest(IdentityKind::Program, 5, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false), "JITCache.gctx.v1"_s,
            "01 05000000 00 00 00 00"_s,
            "498d352c51ede63224fee174270af011702eb59bec75909ca7eb804158cac071"_s, "a program's context digest"_s },
        { globalContextDigest(IdentityKind::Module, 0, JSParserScriptMode::Module, DerivedContextType::None, EvalContextType::None, false), "JITCache.gctx.v1"_s,
            "02 00000000 01 00 00 00"_s,
            "b9a1b0671af8f3f49b82e249856886ae6f16316ceee43a6bc7c20a6901133f01"_s, "a module's context digest"_s },
        { globalContextDigest(IdentityKind::IndirectEval, 9, JSParserScriptMode::Classic, DerivedContextType::DerivedConstructorContext, EvalContextType::FunctionEvalContext, true), "JITCache.gctx.v1"_s,
            "03 09000000 00 01 01 01"_s,
            "ab8cd8e7922c98a06b63ecd3076985360624b5fb439c124897e42c29dfbcdf79"_s, "an indirect eval's context digest"_s },
        { executableBodyContextDigest(IdentityKind::Child, 12, std::nullopt), "JITCache.fctx.v1"_s,
            "06 0c000000 00 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "71536cc85693c499e14066e148543c7de6fa86654ae181f436c6359c28216c10"_s, "a child body's context digest"_s },
        { executableBodyContextDigest(IdentityKind::FunctionConstructor, 0, filledDigest(0x33)), "JITCache.fctx.v1"_s,
            "04 00000000 01 3333333333333333333333333333333333333333333333333333333333333333"_s,
            "9a0bc35cdbafc14c6a98109597f11b8deb14df75a9686fb4d736e9e88598e799"_s, "a root body's context digest"_s },
        // Offset 7, strict, DerivedMethodContext, a class field initializer, a private brand, an arrow context outside an
        // ordinary function, FunctionEvalContext; TDZ names "a" and "b" sorted; private names "#x" (IsMethod) and "#y"
        // (IsMethod | IsStatic) sorted, each followed by its u16 bits.
        { directEvalContextDigest(7, StrictModeLexicallyScopedFeature, DerivedContextType::DerivedMethodContext, NeedsClassFieldInitializer::Yes, PrivateBrandRequirement::Needed, true, false, EvalContextType::FunctionEvalContext, tdz, privateNames), "JITCache.dctx.v1"_s,
            "07000000 01 02 01 01 01 00 01 02000000 01 01000000 61 01 01000000 62 02000000 01 02000000 2378 0100 01 02000000 2379 0900"_s,
            "d8ce02a3f2e573c358db10e114a52ca41aae3813a8010a3ddfbfab458e11f995"_s, "a direct eval's context digest"_s },
        // The environment {alpha, beta}: its name count, then each name's EncodingOrder kind (0, a string) and canonical
        // string. Its digest is the first field of the link record below.
        { std::nullopt, "JITCache.tdze.v1"_s,
            "02000000 00 01 05000000 616c706861 00 01 04000000 62657461"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c"_s, "an environment's digest"_s },
        { *chainDigest, "JITCache.tdzl.v1"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "561249017536183aa860f028aab9a0dc08033ea2dcec8ab0f3201a9add063bd7"_s, "a one-link TDZ chain digest"_s },
        // The environment {gamma}, and the link over it with no parent, which is the second link of the chain below.
        { std::nullopt, "JITCache.tdze.v1"_s,
            "01000000 00 01 05000000 67616d6d61"_s,
            "51deeb97086c9a61737f4fa69e4a37666e378e2083d977b90a78de5e9bf97940"_s, "a one-name environment's digest"_s },
        { *gammaChainDigest, "JITCache.tdzl.v1"_s,
            "51deeb97086c9a61737f4fa69e4a37666e378e2083d977b90a78de5e9bf97940 0000000000000000000000000000000000000000000000000000000000000000"_s,
            "49e5849659bb4c9079e410c2b8b6a6a10e03b672637cf42365578bfbadc126f1"_s, "the digest of a link over {gamma}"_s },
        // The {alpha, beta} link's record again, now with the {gamma} link's digest as its parent's.
        { *twoLinkDigest, "JITCache.tdzl.v1"_s,
            "4624b82f66eed95b239b7971debe8b27468359f8e87d2ccf787fb0da04204d5c 49e5849659bb4c9079e410c2b8b6a6a10e03b672637cf42365578bfbadc126f1"_s,
            "d445e4b48abe6c15246c231e61019f1be1dd01a78db2e64a47190a5dec23ed5d"_s, "a two-link TDZ chain digest"_s },
    } };
    for (const Pinned& entry : pinned) {
        Digest256 expected = digestFromHex(entry.expected);
        Vector<uint8_t> fields = bytesFromHex(entry.record);
        Vector<uint8_t> record;
        record.append(asBytes(entry.label.span8()));
        record.append(fields.span());
        if (!part.check(SHA256::hash(record.span()) == expected, makeString("the vector pinned for "_s, entry.what, " is not the SHA-256 of the record beside it"_s)))
            return false;
        if (entry.actual && !part.check(*entry.actual == expected, makeString(entry.what, " differs from the vector pinned from its layout"_s)))
            return false;
    }
    return true;
}

// A direct eval's context digest does not depend on the order its TDZ and private-name sets were built in, and changes with
// each name added to either.
static bool testContextInputs(VM& vm, TestPart& part)
{
    Vector<String> names;
    for (unsigned i = 0; i < 24; ++i)
        names.append(makeString("jitcacheU3Name"_s, i));
    Vector<String> reversed = names;
    reversed.reverse();
    auto privateSet = [&](const Vector<String>& order) {
        PrivateNameEnvironment environment;
        for (const String& name : order)
            addPrivateName(vm, environment, makeString('#', name), name.length() % 2 ? PrivateNameEntry::IsMethod : PrivateNameEntry::IsGetter);
        return environment;
    };
    auto digest = [&](const TDZEnvironment& tdz, const PrivateNameEnvironment& privateNames) {
        return directEvalContextDigest(3, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, true, EvalContextType::None, tdz, privateNames);
    };
    TDZEnvironment forwardTDZ = tdzNames(vm, names);
    TDZEnvironment backwardTDZ = tdzNames(vm, reversed);
    PrivateNameEnvironment forwardNames = privateSet(names);
    PrivateNameEnvironment backwardNames = privateSet(reversed);
    Digest256 base = digest(forwardTDZ, forwardNames);
    if (!part.check(base == digest(backwardTDZ, backwardNames), "a direct eval's context digest depends on the insertion order of its sets"_s))
        return false;
    TDZEnvironment widerTDZ = forwardTDZ;
    widerTDZ.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, "jitcacheU3Added"_s).impl() });
    PrivateNameEnvironment widerNames = forwardNames;
    addPrivateName(vm, widerNames, "#jitcacheU3Added"_s, PrivateNameEntry::IsMethod);
    return part.check(digest(widerTDZ, forwardNames) != base && digest(forwardTDZ, widerNames) != base, "a direct eval's context digest does not change with a name added to its TDZ or private-name set"_s);
}

// Whether a UFE's TDZ chain is the one a TDZ variant's side names (HolderSide::tdzLinks): one link per letter, the UFE's own
// link first, each holding that letter and neither other letter of a, b and c, and no link after the last letter. A side
// that names no chain passes. The check reads the environments without the digest under test: TDZEnvironmentLink::contains
// reads only its own link's environment, and inflates it.
static bool hasTDZLinks(VM& vm, const UnlinkedFunctionExecutable& executable, ASCIILiteral links)
{
    if (links.isNull())
        return true;
    static constexpr std::array<ASCIILiteral, 3> letters { "a"_s, "b"_s, "c"_s };
    TDZEnvironmentLink* link = executable.parentScopeTDZVariables().get();
    for (char expected : links.span()) {
        if (!link)
            return false;
        for (ASCIILiteral letter : letters) {
            if (link->contains(Identifier::fromString(vm, letter).impl()) != (letter[0] == expected))
                return false;
        }
        link = link->parent();
    }
    return !link;
}

// Holder digests are stable across the UFE's own parse and the filling of its slots; they change with each field the
// descriptor holds, its flags one by one included, with each TDZ name of any link and with the order of the links; and
// they are the same over distinct links on the same environments.
static bool testHolderDigests(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "function jitcacheU3Holder(a) { return a; }"_s);
    if (!part.check(!!fixture.codeBlock && fixture.codeBlock.get()->numberOfFunctionDecls() == 1, "the holder-digest program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& executable = *fixture.codeBlock.get()->functionDecl(0);
    auto before = holderDigest(vm, executable);
    executable.recordParse(ArgumentsFeature | ThisFeature, executable.lexicallyScopedFeatures(), true);
    auto afterParse = holderDigest(vm, executable);
    ParserError error;
    UnlinkedFunctionCodeBlock* body = executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(fixture.source), CodeSpecializationKind::CodeForCall, { }, error, executable.parseMode());
    auto afterBody = holderDigest(vm, executable);
    if (!part.check(body && before && before == afterParse && before == afterBody, "a holder digest changes with the UFE's own parse or the filling of its slot"_s))
        return false;

    for (const HolderVariant& variant : holderVariants) {
        HolderFixture base;
        HolderFixture other;
        if (!generateHolderPair(vm, globalObject, variant, part, base, other))
            return false;
        auto baseDigest = holderDigest(vm, *base.executable);
        auto otherDigest = holderDigest(vm, *other.executable);
        // Checked after the digests because contains() inflates an environment, so an environment first made here is digested
        // in the compact form generation gives it (testEnvironmentDigests covers both forms). The two sides come from two
        // generations, so their chains are distinct link objects even where their environments are one.
        bool chainsAsNamed = hasTDZLinks(vm, *base.executable, variant.base.tdzLinks) && hasTDZLinks(vm, *other.executable, variant.variant.tdzLinks)
            && (variant.base.tdzLinks.isNull() || base.executable->parentScopeTDZVariables() != other.executable->parentScopeTDZVariables());
        if (!part.check(chainsAsNamed, makeString("the UFEs of the variant for "_s, variant.field, " do not hold the TDZ chains it names"_s)))
            return false;
        bool shouldDiffer = variant.descriptorDiffers || variant.chainDiffers;
        if (!part.check(baseDigest && otherDigest && ((*baseDigest != *otherDigest) == shouldDiffer), makeString("the holder digest does not follow "_s, variant.field)))
            return false;
    }
    return true;
}

// The environment digest written out apart from the codec: the names sorted by their characters, each as u8 kind (0 for a
// string) and a canonical string; and the digest of a one-link chain over that environment.
static Digest256 referenceOneLinkChainDigest(Vector<String> names)
{
    std::sort(names.begin(), names.end(), [](const String& a, const String& b) {
        return codePointCompareLessThan(a, b);
    });
    Vector<uint8_t> environment;
    environment.append(asBytes("JITCache.tdze.v1"_s.span8()));
    auto appendU32 = [](Vector<uint8_t>& bytes, uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.append(static_cast<uint8_t>(value >> shift));
    };
    appendU32(environment, static_cast<uint32_t>(names.size()));
    for (const String& name : names) {
        RELEASE_ASSERT(name.is8Bit());
        environment.append(0); // EncodingOrder::kind of a string
        environment.append(1); // Latin-1
        appendU32(environment, name.length());
        environment.append(asBytes(name.span8()));
    }
    Digest256 environmentDigest = SHA256::hash(environment.span());
    std::array<uint8_t, 32> noParent { };
    Vector<uint8_t> link;
    link.append(asBytes("JITCache.tdzl.v1"_s.span8()));
    link.append(std::span<const uint8_t> { environmentDigest });
    link.append(std::span<const uint8_t> { noParent });
    return SHA256::hash(link.span());
}

// An environment's digest equals the reference from its names, in its compact form and inflated; each environment is
// digested once while it lives, and again once it was freed and made again; a request's holder digest over environments
// already digested digests none.
static bool testEnvironmentDigests(VM& vm, TestPart& part)
{
    auto oneLink = [&](const Vector<String>& names) {
        return TDZEnvironmentLink::create(vm.m_compactVariableMap->get(tdzNames(vm, names)), nullptr);
    };
    Vector<String> compactNames { "jitcacheU3EnvZeta"_s, "jitcacheU3EnvAlpha"_s, "jitcacheU3EnvMu"_s };
    Vector<String> inflatedNames { "jitcacheU3InflatedB"_s, "jitcacheU3InflatedA"_s };
    Vector<String> remadeNames { "jitcacheU3RemadeOne"_s, "jitcacheU3RemadeTwo"_s };

    RefPtr<TDZEnvironmentLink> compact = oneLink(compactNames);
    unsigned first = 0;
    auto compactDigest = tdzChainDigest(compact.get(), nullptr, first);
    unsigned second = 0;
    auto again = tdzChainDigest(compact.get(), nullptr, second);
    bool compactOK = compactDigest && *compactDigest == referenceOneLinkChainDigest(compactNames) && first == 1 && again == compactDigest && !second;
    if (!part.check(compactOK, "a compact environment's digest differs from its reference, or is computed more than once"_s))
        return false;

    RefPtr<TDZEnvironmentLink> inflated = oneLink(inflatedNames);
    bool contains = inflated->contains(Identifier::fromString(vm, "jitcacheU3InflatedA"_s).impl()); // inflates the environment
    unsigned inflatedCount = 0;
    auto inflatedDigest = tdzChainDigest(inflated.get(), nullptr, inflatedCount);
    if (!part.check(contains && inflatedDigest && *inflatedDigest == referenceOneLinkChainDigest(inflatedNames) && inflatedCount == 1, "an inflated environment's digest differs from its reference"_s))
        return false;

    RefPtr<TDZEnvironmentLink> remade = oneLink(remadeNames);
    unsigned remadeFirst = 0;
    auto remadeDigest = tdzChainDigest(remade.get(), nullptr, remadeFirst);
    remade = nullptr; // the last handle on the environment, which frees it with its kept digest
    remade = oneLink(remadeNames);
    unsigned remadeSecond = 0;
    auto remadeAgain = tdzChainDigest(remade.get(), nullptr, remadeSecond);
    if (!part.check(remadeDigest && remadeAgain == remadeDigest && remadeFirst == 1 && remadeSecond == 1, "an environment freed and made again is not digested again"_s))
        return false;

    // Two sibling UFEs share their chain: the second request's holder digest finds every environment digested. The functions
    // come before the `let`, so their link holds both names, an environment the test has not made before (see the TDZ
    // variants of holderVariants); after the `let` it would be the empty environment, which may be digested already.
    ProgramFixture fixture = generateFixture(vm, "{ (function f() { }); (function g() { }); let jitcacheU3SharedA, jitcacheU3SharedB; }"_s);
    bool shared = !!fixture.codeBlock && fixture.codeBlock.get()->numberOfFunctionExprs() == 2 && fixture.codeBlock.get()->functionExpr(0)->parentScopeTDZVariables()
        && fixture.codeBlock.get()->functionExpr(0)->parentScopeTDZVariables() == fixture.codeBlock.get()->functionExpr(1)->parentScopeTDZVariables();
    if (!part.check(shared, "the shared-chain program failed to generate, or its two functions do not share one link"_s))
        return false;
    UCBStatistics& statistics = vm.jitCacheState()->registry().statistics();
    auto requestHolder = [&](UnlinkedFunctionExecutable& executable) {
        SourceCode source = executable.linkedSourceCode(fixture.source);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = &executable;
        return requestHolderDigest(request);
    };
    uint64_t environmentsBefore = statistics.tdzEnvironmentDigests;
    uint64_t holdersBefore = statistics.holderDigests;
    Digest256 f = requestHolder(*fixture.codeBlock.get()->functionExpr(0));
    uint64_t environmentsAfterFirst = statistics.tdzEnvironmentDigests;
    Digest256 g = requestHolder(*fixture.codeBlock.get()->functionExpr(1));
    bool counted = environmentsAfterFirst == environmentsBefore + 1 && statistics.tdzEnvironmentDigests == environmentsAfterFirst && statistics.holderDigests == holdersBefore + 2;
    return part.check(counted && f != g, "the first holder digest over a one-link chain did not digest its one fresh environment, the second digested one again, or a holder digest is not counted"_s);
}

// What each kind of request computes. contextDigestOf the inputs recordedContext keeps equals requestContext before and
// after the body is generated, for a root UFE body too; a request's key and context read only the snapshot its request
// object took; a program at another offset of its provider has another context, and no request context changes with the
// provider's start position, nor does computing one build the provider's line-start table.
static bool testRequestKeysAndContexts(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    UCBRegistry& registry = vm.jitCacheState()->registry();
    String programText = "var jitcacheU3Request = 1; function jitcacheU3RequestChild(a) { return a + jitcacheU3Request; }\n"_s;
    Ref<StringSourceProvider> provider = StringSourceProvider::create(programText, SourceOrigin { }, String { }, SourceTaintedOrigin::Untainted);
    SourceCode programSource { Ref<SourceProvider> { provider.get() } };
    Strong<ProgramExecutable> executable { vm, ProgramExecutable::create(globalObject, programSource) };

    // Program, before and after the CodeCache generates it, which overwrites the executable's features.
    auto programRequestContext = [&]() -> std::optional<Digest256> {
        RequestState request(vm, RequestKind::Program, programSource, { }, executable.get()->lexicallyScopedFeatures());
        prepareProgramRequest(request, *executable.get());
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::Program, recordedContext(request), std::nullopt) != context)
            return std::nullopt;
        return context;
    };
    auto programBefore = programRequestContext();
    ParserError error;
    Strong<UnlinkedProgramCodeBlock> program { vm, vm.codeCache()->getUnlinkedProgramCodeBlock(vm, executable.get(), programSource, { }, error) };
    auto programAfter = programRequestContext();
    if (!part.check(program && programBefore && programBefore == programAfter, "a program request's context differs from contextDigestOf its recorded inputs, or changes with its generation"_s))
        return false;
    if (!part.check(!!registry.recordOf(*program.get()) && program.get()->numberOfFunctionDecls() == 1, "the CodeCache's generation of a program did not record it"_s))
        return false;

    // Another offset of the provider, another context; another start position of the provider, the line and column a
    // vm.Script lineOffset sets, the same, and its line-start table stays unbuilt.
    SourceCode shifted { RefPtr<SourceProvider> { provider.ptr() }, 1, static_cast<int>(programText.length()) };
    SourceCode otherStart = makeSource(programText, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { OrdinalNumber::fromOneBasedInt(11), OrdinalNumber::fromOneBasedInt(9) });
    auto contextOf = [&](const SourceCode& source) -> std::optional<Digest256> {
        Strong<ProgramExecutable> sourceExecutable { vm, ProgramExecutable::create(globalObject, source) };
        RequestState request(vm, RequestKind::Program, source, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *sourceExecutable.get());
        if (!requestKey(request, { }))
            return std::nullopt;
        return requestContext(request);
    };
    auto shiftedContext = contextOf(shifted);
    auto otherStartContext = contextOf(otherStart);
    if (!part.check(shiftedContext && shiftedContext != programBefore && otherStartContext == programBefore, "a program's context does not follow its offset in the provider, or follows its start position"_s))
        return false;

    // Child body, before and after its generation, and under another start position of its parent's provider.
    UnlinkedFunctionExecutable& child = *program.get()->functionDecl(0);
    auto childContext = [&](const SourceCode& parentSource) -> std::optional<Digest256> {
        SourceCode source = child.linkedSourceCode(parentSource);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = &child;
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::Child, recordedContext(request), std::nullopt) != context)
            return std::nullopt;
        return context;
    };
    auto childBefore = childContext(programSource);
    UnlinkedFunctionCodeBlock* childBody = child.unlinkedCodeBlockFor(vm, child.linkedSourceCode(programSource), CodeSpecializationKind::CodeForCall, { }, error, child.parseMode());
    auto childAfter = childContext(programSource);
    auto childOtherStart = childContext(otherStart);
    if (!part.check(childBody && childBefore && childBefore == childAfter && childOtherStart == childBefore, "a child body's context differs from its recorded inputs, changes with its generation or with the start position"_s))
        return false;
    // No context records a line, and no request derives one: that would build the table from the provider's whole text.
    if (!part.check(!otherStart.provider()->lineStartsIfBuilt(), "computing a request's key and context built its provider's line-start table"_s))
        return false;

    // Function-constructor root body: its context covers the holder digest of its UFE.
    String rootText = "function anonymous(a\n) {\nreturn a + 'jitcache-u3-root';\n}"_s;
    SourceCode rootSource = makeSource(rootText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> root { vm, vm.codeCache()->getUnlinkedGlobalFunctionExecutable(vm, Identifier::fromString(vm, "anonymous"_s), rootSource, NoLexicallyScopedFeatures, { }, 22, error) };
    if (!part.check(root && registry.identityOf(*root.get()), "a Function-constructor root has no identity"_s))
        return false;
    auto rootContext = [&]() -> std::optional<Digest256> {
        SourceCode source = root.get()->linkedSourceCode(rootSource);
        RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
        request.functionExecutable = root.get();
        if (!requestKey(request, { }))
            return std::nullopt;
        Digest256 context = requestContext(request);
        if (contextDigestOf(IdentityKind::FunctionConstructor, recordedContext(request), requestHolderDigest(request)) != context)
            return std::nullopt;
        return context;
    };
    auto rootBefore = rootContext();
    UnlinkedFunctionCodeBlock* rootBody = root.get()->unlinkedCodeBlockFor(vm, root.get()->linkedSourceCode(rootSource), CodeSpecializationKind::CodeForCall, { }, error, root.get()->parseMode());
    auto rootAfter = rootContext();
    if (!part.check(rootBody && rootBefore && rootBefore == rootAfter, "a root body's context differs from its recorded inputs or changes with its generation"_s))
        return false;

    // Module and indirect eval: their executables generate at creation, so their contexts are compared with the inputs.
    SourceCode moduleSource = makeSource("export const jitcacheU3Module = 1;\n"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    Strong<ModuleProgramExecutable> module { vm, ModuleProgramExecutable::tryCreate(globalObject, moduleSource) };
    SourceCode evalSource = makeSource("var jitcacheU3IndirectEval = 2;"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<IndirectEvalExecutable> indirectEval { vm, IndirectEvalExecutable::tryCreate(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, false, EvalContextType::None) };
    if (!part.check(module && indirectEval, "a module or an indirect eval failed to generate"_s))
        return false;
    auto globalContext = [&](RequestKind kind, GlobalExecutable& global, const SourceCode& source, JSParserScriptMode scriptMode, IdentityKind identityKind) {
        RequestState request(vm, kind, source, { }, global.lexicallyScopedFeatures());
        request.globalExecutable = &global;
        request.scriptMode = scriptMode;
        if (!requestKey(request, { }))
            return false;
        Digest256 expected = globalContextDigest(identityKind, source.startOffset(), scriptMode, global.derivedContextType(), EvalContextType::None, global.isArrowFunctionContext());
        return requestContext(request) == expected && contextDigestOf(identityKind, recordedContext(request), std::nullopt) == expected;
    };
    if (!part.check(globalContext(RequestKind::Module, *module.get(), moduleSource, JSParserScriptMode::Module, IdentityKind::Module), "a module request's context differs from its inputs' digest"_s))
        return false;
    if (!part.check(globalContext(RequestKind::IndirectEval, *indirectEval.get(), evalSource, JSParserScriptMode::Classic, IdentityKind::IndirectEval), "an indirect eval request's context differs from its inputs' digest"_s))
        return false;

    // Direct eval inside the recorded program: its record keeps the digest its request computed.
    TDZEnvironment tdz = tdzNames(vm, { "jitcacheU3EvalTDZ"_s });
    PrivateNameEnvironment privateNames;
    SourceCode directSource = makeSource("jitcacheU3Request + 1"_s, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<DirectEvalExecutable> directEval { vm, DirectEvalExecutable::create(globalObject, directSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None, false, false, EvalContextType::None, &tdz, &privateNames) };
    if (!part.check(!!directEval, "a direct eval failed to generate"_s))
        return false;
    DirectEvalSite site { program.get(), BytecodeIndex(0) };
    {
        RequestState request(vm, RequestKind::DirectEval, directSource, { }, NoLexicallyScopedFeatures);
        request.globalExecutable = directEval.get();
        request.site = &site;
        request.variablesUnderTDZ = &tdz;
        request.privateNameEnvironment = &privateNames;
        if (!part.check(!!requestKey(request, { }), "a direct eval whose caller is recorded has no key"_s))
            return false;
        Digest256 context = requestContext(request);
        if (!part.check(contextDigestOf(IdentityKind::DirectEval, recordedContext(request), std::nullopt) == context, "a direct eval's recorded context differs from its request's"_s))
            return false;
    }

    // The snapshot: a GlobalRequest constructed before its executable's features are overwritten records under the key
    // and key features the snapshot gives.
    String snapshotText = "var jitcacheU3Snapshot = 3;\n"_s;
    SourceCode snapshotSource = makeSource(snapshotText, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<ProgramExecutable> snapshotExecutable { vm, ProgramExecutable::create(globalObject, snapshotSource) };
    std::optional<BodyKey> expectedKey;
    std::optional<Digest256> expectedContext;
    {
        RequestState request(vm, RequestKind::Program, snapshotSource, { }, NoLexicallyScopedFeatures);
        prepareProgramRequest(request, *snapshotExecutable.get());
        expectedKey = requestKey(request, { });
        if (expectedKey)
            expectedContext = requestContext(request);
    }
    ProgramFixture snapshotUCB = generateFixture(vm, snapshotText);
    {
        GlobalRequest request(vm, *snapshotExecutable.get(), snapshotSource, SourceCodeType::ProgramType, JSParserScriptMode::Classic, { }, EvalContextType::None);
        snapshotExecutable.get()->recordParse(0, StrictModeLexicallyScopedFeature | TaintedByWithScopeLexicallyScopedFeature, false);
        request.didGenerate(*snapshotUCB.codeBlock.get());
    }
    auto snapshotRecord = registry.recordOf(*snapshotUCB.codeBlock.get());
    if (!part.check(expectedKey && snapshotRecord && snapshotRecord->key == *expectedKey && snapshotRecord->keyFeatures == NoLexicallyScopedFeatures, "a request's key or key features follow features overwritten after its request object was constructed"_s))
        return false;
    RequestState request(vm, RequestKind::Program, snapshotSource, { }, NoLexicallyScopedFeatures);
    prepareProgramRequest(request, *snapshotExecutable.get());
    auto key = requestKey(request, { });
    return part.check(key == expectedKey && requestContext(request) == *expectedContext, "a request's key or context reads the executable's overwritten features instead of the snapshot"_s);
}

// Every JSC builtin's generated metadata carries the digest of the text name##Source() spans.
static bool testBuiltinMetadata(VM& vm, TestPart& part)
{
    BuiltinExecutables& builtins = *vm.builtinExecutables();
    auto check = [&](const BuiltinSourceMetadata& metadata, const SourceCode& source, ASCIILiteral name) {
        if (!part.check(metadata.hasSourceDigest, makeString("the metadata of "_s, name, " carries no source digest"_s)))
            return false;
        Digest256 recorded = metadata.sourceDigest;
        return part.check(recorded == sourceDigest(source.view()), makeString("the metadata digest of "_s, name, " differs from the digest of its source"_s));
    };
#define JITCACHE_CHECK_BUILTIN_METADATA(name, functionName, overriddenName, length) \
    if (!check(s_JSCBuiltinSourceMetadata[static_cast<unsigned>(BuiltinCodeIndex::name)], builtins.name##Source(), #name ""_s)) \
        return false;
    JSC_FOREACH_BUILTIN_CODE(JITCACHE_CHECK_BUILTIN_METADATA)
#undef JITCACHE_CHECK_BUILTIN_METADATA
    return true;
}

// Starts a Producer on the test's VM, whose registry records what the CodeCache generates and counts the digests that
// requests compute, and checks that it tracks keys.
static bool startTrackingKeys(TestContext& context, TestPart& part, VM& vm, const TemporaryDirectory& directory)
{
    if (!startJITCache(context, vm, directory, Role::Producer, true))
        return false;
    return part.check(vm.jitCacheState() && vm.jitCacheState()->tracksKeys(), "the test's VM does not track keys"_s);
}

} // namespace UCBKeysTestsInternal

using namespace UCBKeysTestsInternal;

JITCACHE_TEST(ucbKeysSourceDigests, No)
{
    TestPart part { context };
    testSourceDigests(part);
}

JITCACHE_TEST(ucbKeysPinnedDigests, Yes)
{
    TestPart part { context };
    testPinnedDigests(*context.vm(), part);
}

JITCACHE_TEST(ucbKeysContextInputs, Yes)
{
    TestPart part { context };
    testContextInputs(*context.vm(), part);
}

JITCACHE_TEST(ucbKeysHolderDigests, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testHolderDigests(vm, entered.globalObject(), part);
}

JITCACHE_TEST(ucbKeysEnvironmentDigests, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-keys"_s);
    if (!directory)
        return;
    TestPart part { context };
    if (!startTrackingKeys(context, part, vm, *directory))
        return;
    if (testEnvironmentDigests(vm, part))
        checkRegistry(context, vm);
}

JITCACHE_TEST(ucbKeysRequestKeysAndContexts, Yes)
{
    VM& vm = *context.vm();
    auto directory = TemporaryDirectory::create(context, "jitcache-ucb-keys"_s);
    if (!directory)
        return;
    TestPart part { context };
    if (!startTrackingKeys(context, part, vm, *directory))
        return;
    EnteredGlobalObject entered { vm };
    if (testRequestKeysAndContexts(vm, entered.globalObject(), part))
        checkRegistry(context, vm);
}

JITCACHE_TEST(ucbKeysBuiltinMetadata, Yes)
{
    TestPart part { context };
    testBuiltinMetadata(*context.vm(), part);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
