#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "ArrayPrototype.h"
#include "ByteFields.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CallData.h"
#include "CommonSlowPaths.h"
#include "DeferGC.h"
#include "JITCacheTest.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGlobalObject.h"
#include "StrongInlines.h"
#include "UCBSections.h"
#include "UCBTestSupport.h"
#include "UnlinkedCodeBlock.h"
#include "VMEntryScope.h"
#include <optional>
#include <span>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// The tests of UCBSections: the ucb.identity section that writeIdentitySection writes and parseIdentitySection reads,
// field by field and rule by rule; the constant checks constantMapsFit and markedConstantsAreAtoms; atomizeMarkedConstants;
// and rebuildAtomStringButterflies on a UCB decoded from a generated one's core.

namespace JSC::JITCache::Tests {

namespace UCBSectionsTestsInternal {

// The JSCellButterfly constant of `length` elements whose first element is the string `first`.
static std::optional<uint32_t> butterflyConstantIndex(UnlinkedCodeBlock& codeBlock, ASCIILiteral first, unsigned length)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        auto* butterfly = constantCell<JSCellButterfly>(codeBlock, i);
        if (butterfly && butterfly->length() == length && !hasDouble(butterfly->indexingMode()) && isStringWithContent(butterfly->get(0), first))
            return i;
    }
    return std::nullopt;
}

static bool sameParseFields(const std::optional<FunctionParseFields>& a, const std::optional<FunctionParseFields>& b)
{
    if (!a || !b)
        return !a && !b;
    return a->features == b->features && a->lexicallyScopedFeatures == b->lexicallyScopedFeatures && a->hasCapturedVariables == b->hasCapturedVariables;
}

static bool sameIdentitySection(const IdentitySection& a, const IdentitySection& b)
{
    return a.coreKind == b.coreKind && a.provenance == b.provenance && a.key == b.key && a.contextDigest == b.contextDigest && a.coreDigest == b.coreDigest
        && a.holderDigest == b.holderDigest && sameParseFields(a.functionParseFields, b.functionParseFields) && a.constantCount == b.constantCount
        && equalSpans(a.atomMap, b.atomMap) && equalSpans(a.butterflyMap, b.butterflyMap);
}

// Whether `bytes` hold `expected` where the identity section's layout puts each field, with zero padding.
static bool identityBytesMatch(std::span<const uint8_t> bytes, const IdentitySection& expected)
{
    size_t mapBytes = (static_cast<size_t>(expected.constantCount) + 7) / 8;
    size_t mapsEnd = identityMapsOffset + 2 * mapBytes;
    if (bytes.size() != roundUpToMultipleOf<8>(mapsEnd))
        return false;
    FunctionParseFields parseFields = expected.functionParseFields.value_or(FunctionParseFields { NoFeatures, NoLexicallyScopedFeatures, false });
    return readField<uint32_t>(bytes, 0) == identitySectionMagic
        && readField<uint16_t>(bytes, identityVersionOffset) == 1
        && bytes[identityCoreKindOffset] == static_cast<uint8_t>(expected.coreKind)
        && bytes[identityProvenanceOffset] == static_cast<uint8_t>(expected.provenance)
        && equalSpans(bytes.subspan(identityKeyOffset, BodyKey::byteSize), expected.key.bytes())
        && readField<Digest256>(bytes, identityContextDigestOffset) == expected.contextDigest
        && readField<Digest256>(bytes, identityCoreDigestOffset) == expected.coreDigest
        && readField<Digest256>(bytes, identityHolderDigestOffset) == expected.holderDigest.value_or(Digest256 { })
        && readField<CodeFeatures>(bytes, identityFeaturesOffset) == parseFields.features
        && bytes[identityLexicallyScopedFeaturesOffset] == parseFields.lexicallyScopedFeatures
        && bytes[identityHasCapturedVariablesOffset] == static_cast<uint8_t>(parseFields.hasCapturedVariables)
        && readField<uint32_t>(bytes, identityConstantCountOffset) == expected.constantCount
        && equalSpans(bytes.subspan(identityMapsOffset, mapBytes), expected.atomMap)
        && equalSpans(bytes.subspan(identityMapsOffset + mapBytes, mapBytes), expected.butterflyMap)
        && isAllZero(bytes.subspan(mapsEnd));
}

// ucb.identity: a program core, and a function core whose butterfly map a record kept, round-trip with strict on and off,
// laid out at the identity section's offsets, with the maps generation's constants give; strict parsing rejects each rule
// of the layout broken once, and parsing with strict off still answers wherever the layout stays readable.
static bool testIdentitySection(VM& vm, TestPart& part)
{
    auto fixture = generateSectionFixture(vm);
    if (!part.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(codeBlock.constantRegisters().size());
    size_t mapBytes = (static_cast<size_t>(constantCount) + 7) / 8;

    auto words = butterflyConstantIndex(codeBlock, "alpha"_s, 3);
    auto mixed = butterflyConstantIndex(codeBlock, "alpha"_s, 2);
    auto prefix = butterflyConstantIndex(codeBlock, "delta"_s, 2);
    if (!part.check(words && mixed && prefix, "the fixture program lacks one of its array literals"_s))
        return false;
    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    bool generatedForms = constantCell<JSCellButterfly>(codeBlock, *words)->structure() == atomStringsStructure
        && constantCell<JSCellButterfly>(codeBlock, *mixed)->structure() != atomStringsStructure
        && constantCell<JSCellButterfly>(codeBlock, *prefix)->structure() != atomStringsStructure;
    if (!part.check(generatedForms, "generation did not give the atom form to the all-string literal alone"_s))
        return false;

    // The maps a capture of this UCB writes: every string constant, which generation makes an atom, and the one literal in
    // the atom form. The other literals hold strings too but keep the plain form, so their bits stay clear.
    Vector<uint8_t> atomMap(FillWith { }, mapBytes, 0);
    Vector<uint8_t> butterflyMap(FillWith { }, mapBytes, 0);
    std::optional<uint32_t> someAtom;
    for (uint32_t i = 0; i < constantCount; ++i) {
        auto* string = constantCell<JSString>(codeBlock, i);
        if (!string)
            continue;
        StringImpl* impl = string->tryGetValueImpl();
        if (!part.check(impl && impl->isAtom(), "generation left a string constant that is not an atom"_s))
            return false;
        setBit(atomMap.mutableSpan(), i);
        someAtom = i;
    }
    if (!part.check(!!someAtom, "the fixture program has no string constant"_s))
        return false;
    setBit(butterflyMap.mutableSpan(), *words);
    Vector<uint8_t> keptMap(FillWith { }, mapBytes, 0);
    setBit(keptMap.mutableSpan(), *mixed);
    setBit(keptMap.mutableSpan(), *prefix);

    IdentitySection programExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Program,
        .provenance = CoreProvenance::Generated,
        .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x41)),
        .contextDigest = filledDigest(0x11),
        .coreDigest = filledDigest(0x22),
        .holderDigest = std::nullopt,
        .functionParseFields = std::nullopt,
        .constantCount = constantCount,
        .atomMap = atomMap.span(),
        .butterflyMap = butterflyMap.span(),
        .childHolderDigests = { },
    };
    IdentitySection functionExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Function,
        .provenance = CoreProvenance::EmbedderDecoded,
        .key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, { }, filledDigest(0x42)),
        .contextDigest = filledDigest(0x33),
        .coreDigest = filledDigest(0x44),
        .holderDigest = filledDigest(0x55),
        .functionParseFields = FunctionParseFields { static_cast<CodeFeatures>(ArgumentsFeature | ThisFeature | AsyncFunctionWithoutAwaitFeature), AllLexicallyScopedFeatures, true },
        .constantCount = constantCount,
        .atomMap = atomMap.span(),
        .butterflyMap = keptMap.span(),
        .childHolderDigests = { },
    };
    if (!part.check(identitySectionSize(constantCount) == roundUpToMultipleOf<8>(identityMapsOffset + 2 * mapBytes), "identitySectionSize disagrees with the identity section's layout"_s))
        return false;

    auto write = [&](const IdentitySection& expected, std::optional<std::span<const uint8_t>> keptButterflyMap) {
        Vector<uint8_t> bytes(identitySectionSize(constantCount));
        writeIdentitySection(bytes.mutableSpan(), vm, expected.key, expected.contextDigest, expected.provenance, expected.coreDigest, expected.holderDigest,
            expected.coreKind, expected.functionParseFields, codeBlock, keptButterflyMap);
        return bytes;
    };
    Vector<uint8_t> programSection = write(programExpected, std::nullopt);
    Vector<uint8_t> functionSection = write(functionExpected, keptMap.span());
    if (!part.check(identityBytesMatch(programSection.span(), programExpected), "writeIdentitySection did not lay out a program core at the identity section's offsets"_s))
        return false;
    if (!part.check(identityBytesMatch(functionSection.span(), functionExpected), "writeIdentitySection did not lay out a function core with a kept butterfly map at the identity section's offsets"_s))
        return false;
    for (bool strict : { true, false }) {
        auto program = parseIdentitySection(programSection.span(), strict);
        auto function = parseIdentitySection(functionSection.span(), strict);
        bool roundTrips = program && sameIdentitySection(*program, programExpected) && function && sameIdentitySection(*function, functionExpected);
        if (!part.check(roundTrips, strict ? "ucb.identity does not round-trip with strict on"_s : "ucb.identity does not round-trip with strict off"_s))
            return false;
    }

    auto parseIdentity = [](std::span<const uint8_t> bytes, bool strict) {
        return parseIdentitySection(bytes, strict);
    };
    auto rejects = [&](const Vector<uint8_t>& valid, ASCIILiteral rule, bool keepsLayout, const auto& mutate) {
        return strictRejects(part, "ucb.identity"_s, parseIdentity, valid, rule, keepsLayout, mutate);
    };
    // N % 8 != 0, so the bit at N lies in each map's last byte.
    size_t lastMapByte = constantCount / 8;
    uint8_t bitAtN = 1u << (constantCount % 8);
    size_t mapsEnd = identityMapsOffset + 2 * mapBytes;
    return rejects(functionSection, "a wrong magic"_s, true, [](auto& bytes) { bytes[0] ^= 0xff; })
        && rejects(functionSection, "layout version 2"_s, true, [](auto& bytes) { writeField<uint16_t>(bytes.mutableSpan(), identityVersionOffset, 2); })
        && rejects(functionSection, "core kind 4"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = 4; })
        && rejects(functionSection, "a program core under a child key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Program); })
        && rejects(programSection, "an eval core under a program key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Eval); })
        && rejects(functionSection, "provenance 2"_s, true, [](auto& bytes) { bytes[identityProvenanceOffset] = 2; })
        && rejects(functionSection, "key version 2"_s, true, [](auto& bytes) { bytes[identityKeyOffset] = 2; })
        && rejects(functionSection, "key identity kind 0"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 1] = 0; })
        && rejects(functionSection, "key identity kind 8"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 1] = 8; })
        && rejects(functionSection, "key specialization 2"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 2] = 2; })
        && rejects(functionSection, "a key mode bit outside CodeGenerationMode"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 3] = 0x80; })
        && rejects(functionSection, "a nonzero reserved key byte"_s, true, [](auto& bytes) { bytes[identityKeyOffset + 7] = 1; })
        && rejects(functionSection, "function features at 1 << bitWidthOfCodeFeatures"_s, true, [](auto& bytes) { writeField<CodeFeatures>(bytes.mutableSpan(), identityFeaturesOffset, 1u << bitWidthOfCodeFeatures); })
        && rejects(functionSection, "lexically scoped features above AllLexicallyScopedFeatures"_s, true, [](auto& bytes) { bytes[identityLexicallyScopedFeaturesOffset] = AllLexicallyScopedFeatures + 1; })
        && rejects(functionSection, "has-captured-variables 2"_s, true, [](auto& bytes) { bytes[identityHasCapturedVariablesOffset] = 2; })
        && rejects(programSection, "a nonzero holder digest on a program core"_s, true, [](auto& bytes) { bytes[identityHolderDigestOffset + 31] = 1; })
        && rejects(programSection, "function features on a program core"_s, true, [](auto& bytes) { writeField<CodeFeatures>(bytes.mutableSpan(), identityFeaturesOffset, ArgumentsFeature); })
        && rejects(programSection, "lexically scoped features on a program core"_s, true, [](auto& bytes) { bytes[identityLexicallyScopedFeaturesOffset] = StrictModeLexicallyScopedFeature; })
        && rejects(programSection, "has-captured-variables on a program core"_s, true, [](auto& bytes) { bytes[identityHasCapturedVariablesOffset] = 1; })
        && rejects(programSection, "an atom map bit at N"_s, true, [&](auto& bytes) { bytes[identityMapsOffset + lastMapByte] |= bitAtN; })
        && rejects(programSection, "a butterfly map bit at N"_s, true, [&](auto& bytes) { bytes[identityMapsOffset + mapBytes + lastMapByte] |= bitAtN; })
        && rejects(programSection, "a constant marked in both maps"_s, true, [&](auto& bytes) { setBit(bytes.mutableSpan().subspan(identityMapsOffset + mapBytes, mapBytes), *someAtom); })
        && (programSection.size() == mapsEnd || rejects(programSection, "nonzero padding"_s, true, [&](auto& bytes) { bytes[mapsEnd] = 1; }))
        && rejects(programSection, "a constant count whose maps need eight more bytes"_s, false, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), identityConstantCountOffset, constantCount + 32); })
        && rejects(programSection, "a constant count of 1 << 28"_s, false, [](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), identityConstantCountOffset, sectionCountLimit); })
        && rejects(programSection, "one byte more"_s, true, [](auto& bytes) { bytes.append(static_cast<uint8_t>(0)); })
        && rejects(programSection, "one byte fewer"_s, false, [](auto& bytes) {
            bytes.removeLast();
        });
}

// atomizeMarkedConstants makes a plain-string constant an atom marked as one, leaves an atom and the unmarked constants as
// they are, and allocates no cell. constantMapsFit and markedConstantsAreAtoms answer before and after.
static bool testAtomization(VM& vm, TestPart& part)
{
    auto fixture = generateSectionFixture(vm);
    if (!part.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(codeBlock.constantRegisters().size());
    size_t mapBytes = (static_cast<size_t>(constantCount) + 7) / 8;
    auto plainIndex = stringConstantIndex(codeBlock, "jitcache-u5-plain-constant"_s);
    auto atomIndex = stringConstantIndex(codeBlock, "jitcache-u5-atom-constant"_s);
    auto unmarkedIndex = stringConstantIndex(codeBlock, "jitcache-u5-unmarked-constant"_s);
    auto symbolTable = symbolTableConstantIndex(codeBlock);
    auto mixed = butterflyConstantIndex(codeBlock, "alpha"_s, 2);
    if (!part.check(plainIndex && atomIndex && unmarkedIndex && symbolTable && mixed, "the fixture program lacks a constant the atomization test reads"_s))
        return false;

    // A natively decoded UCB can hold plain-string constants; two such strings take the place of generation's atoms.
    JSString* plain = jsString(vm, makeString("jitcache-u5-plain-"_s, "runtime-string"_s));
    JSString* unmarked = jsString(vm, makeString("jitcache-u5-unmarked-"_s, "runtime-string"_s));
    codeBlock.constantRegister(constantRegisterFor(*plainIndex)).set(vm, &codeBlock, plain);
    codeBlock.constantRegister(constantRegisterFor(*unmarkedIndex)).set(vm, &codeBlock, unmarked);
    JSString* atom = constantCell<JSString>(codeBlock, *atomIndex);
    StringImpl* atomImpl = atom->tryGetValueImpl();
    bool preconditions = !plain->tryGetValueImpl()->isAtom() && !plain->isDefinitelyAtom() && !unmarked->tryGetValueImpl()->isAtom() && atomImpl->isAtom();
    if (!part.check(preconditions, "the runtime strings of the atomization test are atoms already"_s))
        return false;

    Vector<uint8_t> atomMap(FillWith { }, mapBytes, 0);
    Vector<uint8_t> butterflyMap(FillWith { }, mapBytes, 0);
    setBit(atomMap.mutableSpan(), *plainIndex);
    setBit(atomMap.mutableSpan(), *atomIndex);
    auto identityWith = [&](std::span<const uint8_t> atoms, std::span<const uint8_t> butterflies, uint32_t count) {
        return IdentitySection {
            .coreKind = UnlinkedCodeBlockCoreKind::Program,
            .provenance = CoreProvenance::Generated,
            .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x61)),
            .contextDigest = { },
            .coreDigest = { },
            .holderDigest = std::nullopt,
            .functionParseFields = std::nullopt,
            .constantCount = count,
            .atomMap = atoms,
            .butterflyMap = butterflies,
            .childHolderDigests = { },
        };
    };
    IdentitySection identity = identityWith(atomMap.span(), butterflyMap.span(), constantCount);

    // The maps fit, and the marked constants are not all atoms until the plain string is one.
    if (!part.check(constantMapsFit(codeBlock, identity) && !markedConstantsAreAtoms(codeBlock, identity), "constantMapsFit refuses, or markedConstantsAreAtoms accepts, a marked plain-string constant"_s))
        return false;
    // constantMapsFit refuses a map that marks the wrong kind of constant, and a constant count that differs.
    Vector<uint8_t> tableAsAtom(FillWith { }, mapBytes, 0);
    setBit(tableAsAtom.mutableSpan(), *symbolTable);
    Vector<uint8_t> stringAsButterfly(FillWith { }, mapBytes, 0);
    setBit(stringAsButterfly.mutableSpan(), *atomIndex);
    Vector<uint8_t> mixedAsButterfly(FillWith { }, mapBytes, 0);
    setBit(mixedAsButterfly.mutableSpan(), *mixed);
    bool refusesMisfits = !constantMapsFit(codeBlock, identityWith(tableAsAtom.span(), butterflyMap.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(butterflyMap.span(), stringAsButterfly.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(butterflyMap.span(), mixedAsButterfly.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(atomMap.span(), butterflyMap.span(), constantCount + 1));
    if (!part.check(refusesMisfits, "constantMapsFit accepts a map that marks a constant of the wrong kind, or another constant count"_s))
        return false;

    Vector<EncodedJSValue> before;
    for (auto& constant : codeBlock.constantRegisters())
        before.append(JSValue::encode(constant.get()));
    // Allocating a cell outside a GC deferral asserts under AssertNoGC in the debug builds testjitcache runs in
    // (tryAllocateCellHelper), so the call below proves it allocates none.
    if (!part.check(!vm.heap.isDeferred(), "the test runs with GC deferred, where AssertNoGC cannot catch an allocation"_s))
        return false;
    {
        AssertNoGC assertNoGC;
        atomizeMarkedConstants(vm, codeBlock, identity);
    }
    bool sameCells = true;
    for (uint32_t i = 0; i < constantCount; ++i)
        sameCells &= JSValue::encode(codeBlock.constantRegisters()[i].get()) == before[i];
    if (!part.check(sameCells, "atomizeMarkedConstants replaced a constant instead of atomizing it in place"_s))
        return false;
    if (!part.check(plain->tryGetValueImpl()->isAtom() && plain->isDefinitelyAtom(), "atomizeMarkedConstants left a marked plain string plain or unmarked as an atom"_s))
        return false;
    if (!part.check(atom->tryGetValueImpl() == atomImpl && atom->isDefinitelyAtom(), "atomizeMarkedConstants changed a marked atom's value or left it unmarked"_s))
        return false;
    if (!part.check(!unmarked->tryGetValueImpl()->isAtom() && !unmarked->isDefinitelyAtom(), "atomizeMarkedConstants atomized an unmarked constant"_s))
        return false;
    return part.check(markedConstantsAreAtoms(codeBlock, identity), "markedConstantsAreAtoms refuses constants that atomizeMarkedConstants made atoms"_s);
}

// Calls Array.prototype[name] on `array` with one argument through the native call path; empty if it throws.
static std::optional<JSValue> callArrayMethod(JSGlobalObject* globalObject, JSArray* array, ASCIILiteral name, JSValue argument)
{
    VM& vm = globalObject->vm();
    JSValue function = globalObject->arrayPrototype()->getDirect(vm, Identifier::fromString(vm, name));
    if (!function)
        return std::nullopt;
    auto callData = JSC::getCallData(function);
    if (callData.type == CallData::Type::None)
        return std::nullopt;
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    NakedPtr<Exception> exception;
    JSValue result = JSC::call(globalObject, function, callData, array, arguments, exception);
    if (exception)
        return std::nullopt;
    return result;
}

// On a UCB decoded from a generated one's core, rebuildAtomStringButterflies gives each marked butterfly the atom form with
// the VM's canonical JSString for every element, as generation does, and leaves the unmarked butterflies plain and the core
// encoding unchanged; indexOf and includes over an array made from a rebuilt butterfly, as new_array_buffer makes it, find
// every element and miss a string the map does not hold.
static bool testButterflyRebuild(VM& vm, TestPart& part)
{
    auto fixture = generateSectionFixture(vm);
    if (!part.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& generated = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(generated.constantRegisters().size());
    auto words = butterflyConstantIndex(generated, "alpha"_s, 3);
    auto mixed = butterflyConstantIndex(generated, "alpha"_s, 2);
    auto prefix = butterflyConstantIndex(generated, "delta"_s, 2);
    if (!part.check(words && mixed && prefix, "the fixture program lacks one of its array literals"_s))
        return false;

    // The identity section a capture of the generated UCB writes, whose butterfly map marks the literal in the atom form.
    Vector<uint8_t> identityBytes(identitySectionSize(constantCount));
    writeIdentitySection(identityBytes.mutableSpan(), vm, BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x71)), filledDigest(0x72),
        CoreProvenance::Generated, filledDigest(0x73), std::nullopt, UnlinkedCodeBlockCoreKind::Program, std::nullopt, generated);
    auto identity = parseIdentitySection(identityBytes.span(), true);
    bool marksAtomForm = identity && bitIsSet(identity->butterflyMap, *words) && !bitIsSet(identity->butterflyMap, *mixed) && !bitIsSet(identity->butterflyMap, *prefix);
    if (!part.check(marksAtomForm, "the butterfly map of a generated UCB does not mark exactly its literal in the atom form"_s))
        return false;

    CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, generated, nullptr, nullptr, encodeFailure);
    if (!part.check(!!core, "encodeUnlinkedCodeBlockCore failed on the fixture program"_s))
        return false;
    CoreDecodeFailure decodeFailure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* decodedCodeBlock = decodeUnlinkedCodeBlockCore(vm, Ref<CachedBytecode> { *core }, *fixture->source.provider(), UnlinkedCodeBlockCoreKind::Program, nullptr, false, decodeFailure);
    Strong<UnlinkedCodeBlock> decoded { vm, decodedCodeBlock };
    if (!part.check(decodedCodeBlock && decodeFailure == CoreDecodeFailure::None, "decodeUnlinkedCodeBlockCore failed on the fixture program's core"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *decodedCodeBlock;
    if (!part.check(codeBlock.constantRegisters().size() == constantCount, "the decoded fixture program has another constant count"_s))
        return false;

    Structure* atomStringsStructure = vm.cellButterflyOnlyAtomStringsStructure.get();
    auto* decodedWords = constantCell<JSCellButterfly>(codeBlock, *words);
    auto* decodedMixed = constantCell<JSCellButterfly>(codeBlock, *mixed);
    auto* decodedPrefix = constantCell<JSCellButterfly>(codeBlock, *prefix);
    // The decode gives every butterfly the plain form; otherwise the rebuild would go untested.
    if (!part.check(decodedWords && decodedMixed && decodedPrefix && decodedWords->structure() != atomStringsStructure, "the decode did not leave the all-string literal in the plain form"_s))
        return false;
    if (!part.check(constantMapsFit(codeBlock, *identity), "constantMapsFit refuses the UCB decoded from the generated one's core"_s))
        return false;

    Digest256 encodingBefore = coreDigestOf(vm, codeBlock, nullptr);
    {
        DeferGC deferGC(vm);
        rebuildAtomStringButterflies(vm, codeBlock, *identity);
    }
    if (!part.check(coreDigestOf(vm, codeBlock, nullptr) == encodingBefore, "rebuildAtomStringButterflies changed the core encoding"_s))
        return false;

    auto* rebuilt = constantCell<JSCellButterfly>(codeBlock, *words);
    auto* original = constantCell<JSCellButterfly>(generated, *words);
    bool atomForm = rebuilt && rebuilt != decodedWords && rebuilt->structure() == atomStringsStructure && rebuilt->indexingMode() == original->indexingMode() && rebuilt->length() == original->length();
    if (!part.check(atomForm, "rebuildAtomStringButterflies did not give the marked butterfly the atom form"_s))
        return false;
    for (unsigned i = 0; i < rebuilt->length(); ++i) {
        JSValue element = rebuilt->get(i);
        auto* string = element.isString() ? asString(element) : nullptr;
        StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
        bool canonical = impl && impl->isAtom() && string->isDefinitelyAtom() && vm.atomStringToJSStringMap.get(impl) == string && element == original->get(i);
        if (!part.check(canonical, "a rebuilt element is not the JSString atomStringToJSStringMap holds for its atom, as generation's is"_s))
            return false;
    }
    bool unmarkedStayPlain = constantCell<JSCellButterfly>(codeBlock, *mixed) == decodedMixed && decodedMixed->structure() == vm.cellButterflyStructure(decodedMixed->indexingMode())
        && constantCell<JSCellButterfly>(codeBlock, *prefix) == decodedPrefix && decodedPrefix->structure() == vm.cellButterflyStructure(decodedPrefix->indexingMode());
    if (!part.check(unmarkedStayPlain, "rebuildAtomStringButterflies changed an unmarked butterfly"_s))
        return false;
    if (!part.check(constantMapsFit(codeBlock, *identity), "constantMapsFit refuses the rebuilt UCB"_s))
        return false;

    // The array new_array_buffer makes from the rebuilt butterfly, searched through the atom fast paths of indexOf and
    // includes.
    JSGlobalObject* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    if (!part.check(!!globalObject, "the test runs outside a VM entry, so it has no global object to search arrays in"_s))
        return false;
    JSArray* array = CommonSlowPaths::allocateNewArrayBuffer(vm, globalObject->arrayStructureForIndexingTypeDuringAllocation(rebuilt->indexingMode()), rebuilt);
    if (!part.check(isCopyOnWrite(array->indexingMode()) && JSCellButterfly::isOnlyAtomStringsStructure(vm, array->butterfly()), "the array made from the rebuilt butterfly does not share it copy-on-write"_s))
        return false;
    for (unsigned i = 0; i < rebuilt->length(); ++i) {
        JSValue element = rebuilt->get(i);
        // The element itself, and a plain string with its characters, as a run-time search builds one.
        JSValue copy = jsString(vm, makeString(StringView { *asString(element)->tryGetValueImpl() }));
        for (JSValue search : { element, copy }) {
            auto index = callArrayMethod(globalObject, array, "indexOf"_s, search);
            auto included = callArrayMethod(globalObject, array, "includes"_s, search);
            bool found = index && index->isNumber() && index->asNumber() == i && included && included->isTrue();
            if (!part.check(found, "indexOf or includes misses an element of a rebuilt butterfly"_s))
                return false;
        }
    }
    JSValue absent = jsString(vm, makeString("jitcache-u5-"_s, "absent-from-every-literal"_s));
    auto absentIndex = callArrayMethod(globalObject, array, "indexOf"_s, absent);
    auto absentIncluded = callArrayMethod(globalObject, array, "includes"_s, absent);
    bool missed = absentIndex && absentIndex->isNumber() && absentIndex->asNumber() == -1 && absentIncluded && absentIncluded->isFalse();
    return part.check(missed, "indexOf or includes finds, in a rebuilt butterfly, a string the map does not hold"_s);
}

} // namespace UCBSectionsTestsInternal

using namespace UCBSectionsTestsInternal;

JITCACHE_TEST(ucbSectionsIdentityLayout, Yes)
{
    TestPart part { context };
    testIdentitySection(*context.vm(), part);
}

JITCACHE_TEST(ucbSectionsAtomizeMarkedConstants, Yes)
{
    TestPart part { context };
    testAtomization(*context.vm(), part);
}

JITCACHE_TEST(ucbSectionsRebuildAtomStringButterflies, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testButterflyRebuild(vm, part);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
