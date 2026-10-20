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
// field by field and rule by rule; the constant check constantMapsFit; atomizeStringConstant; and
// rebuildAtomStringButterflies on a UCB decoded from a generated one's core.

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
    return a.coreKind == b.coreKind && a.key == b.key && a.contextDigest == b.contextDigest && a.coreDigest == b.coreDigest
        && a.holderDigest == b.holderDigest && sameParseFields(a.functionParseFields, b.functionParseFields) && a.constantCount == b.constantCount
        && equalSpans(a.butterflyMap, b.butterflyMap) && equalSpans(a.childHolderDigests, b.childHolderDigests);
}

// Whether `bytes` hold `expected` where the identity section's layout puts each field, with a zero reserved byte, zero
// padding after the map and the child holder digests at the 8-aligned end of the map.
static bool identityBytesMatch(std::span<const uint8_t> bytes, const IdentitySection& expected)
{
    size_t mapBytes = (static_cast<size_t>(expected.constantCount) + 7) / 8;
    size_t mapEnd = identityButterflyMapOffset + mapBytes;
    size_t digestsAt = roundUpToMultipleOf<8>(mapEnd);
    if (bytes.size() != digestsAt + expected.childHolderDigests.size())
        return false;
    FunctionParseFields parseFields = expected.functionParseFields.value_or(FunctionParseFields { NoFeatures, NoLexicallyScopedFeatures, false });
    return readField<uint32_t>(bytes, 0) == identitySectionMagic
        && readField<uint16_t>(bytes, identityVersionOffset) == 1
        && bytes[identityCoreKindOffset] == static_cast<uint8_t>(expected.coreKind)
        && !bytes[identityReservedOffset]
        && equalSpans(bytes.subspan(identityKeyOffset, BodyKey::byteSize), expected.key.bytes())
        && readField<Digest256>(bytes, identityContextDigestOffset) == expected.contextDigest
        && readField<Digest256>(bytes, identityCoreDigestOffset) == expected.coreDigest
        && readField<Digest256>(bytes, identityHolderDigestOffset) == expected.holderDigest.value_or(Digest256 { })
        && readField<CodeFeatures>(bytes, identityFeaturesOffset) == parseFields.features
        && bytes[identityLexicallyScopedFeaturesOffset] == parseFields.lexicallyScopedFeatures
        && bytes[identityHasCapturedVariablesOffset] == static_cast<uint8_t>(parseFields.hasCapturedVariables)
        && readField<uint32_t>(bytes, identityConstantCountOffset) == expected.constantCount
        && equalSpans(bytes.subspan(identityButterflyMapOffset, mapBytes), expected.butterflyMap)
        && isAllZero(bytes.subspan(mapEnd, digestsAt - mapEnd))
        && equalSpans(bytes.subspan(digestsAt), expected.childHolderDigests);
}

// ucb.identity: a program core, and a function core that lists its children's holder digests, round-trip with strict on
// and off, laid out at the identity section's offsets with the butterfly map generation's constants give; strict parsing
// rejects each rule of the layout broken once, and parsing with strict off still answers wherever the layout stays
// readable.
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
    // No string needs a map: generation makes every string constant an atom.
    for (uint32_t i = 0; i < constantCount; ++i) {
        auto* string = constantCell<JSString>(codeBlock, i);
        StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
        if (!part.check(!string || (impl && impl->isAtom()), "generation left a string constant that is not an atom"_s))
            return false;
    }

    // The map a capture of this UCB writes marks the one literal in the atom form. The other literals hold strings too but
    // keep the plain form, so their bits stay clear.
    Vector<uint8_t> butterflyMap(FillWith { }, mapBytes, 0);
    setBit(butterflyMap.mutableSpan(), *words);
    // A holder digest per child UFE, function declarations then expressions.
    size_t childCount = codeBlock.numberOfFunctionDecls() + codeBlock.numberOfFunctionExprs();
    if (!part.check(childCount >= 2, "the fixture program lacks the child UFEs whose holder digests a function core lists"_s))
        return false;
    Vector<Digest256> childDigests;
    for (size_t i = 0; i < childCount; ++i)
        childDigests.append(filledDigest(static_cast<uint8_t>(0x80 + i)));

    IdentitySection programExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Program,
        .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x41)),
        .contextDigest = filledDigest(0x11),
        .coreDigest = filledDigest(0x22),
        .holderDigest = std::nullopt,
        .functionParseFields = std::nullopt,
        .constantCount = constantCount,
        .butterflyMap = butterflyMap.span(),
        .childHolderDigests = { },
    };
    IdentitySection functionExpected {
        .coreKind = UnlinkedCodeBlockCoreKind::Function,
        .key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, { }, filledDigest(0x42)),
        .contextDigest = filledDigest(0x33),
        .coreDigest = filledDigest(0x44),
        .holderDigest = filledDigest(0x55),
        .functionParseFields = FunctionParseFields { static_cast<CodeFeatures>(ArgumentsFeature | ThisFeature | AsyncFunctionWithoutAwaitFeature), AllLexicallyScopedFeatures, true },
        .constantCount = constantCount,
        .butterflyMap = butterflyMap.span(),
        .childHolderDigests = asBytes(childDigests.span()),
    };
    size_t baseSize = roundUpToMultipleOf<8>(identityButterflyMapOffset + mapBytes);
    if (!part.check(identitySectionSize(constantCount) == baseSize && identitySectionSize(constantCount, childCount) == baseSize + childCount * sizeof(Digest256), "identitySectionSize disagrees with the identity section's layout"_s))
        return false;

    auto write = [&](const IdentitySection& expected, std::span<const Digest256> digests) {
        Vector<uint8_t> bytes(identitySectionSize(constantCount, digests.size()));
        writeIdentitySection(bytes.mutableSpan(), vm, expected.key, expected.contextDigest, expected.coreDigest, expected.holderDigest,
            expected.coreKind, expected.functionParseFields, codeBlock, digests);
        return bytes;
    };
    Vector<uint8_t> programSection = write(programExpected, { });
    Vector<uint8_t> functionSection = write(functionExpected, childDigests.span());
    if (!part.check(identityBytesMatch(programSection.span(), programExpected), "writeIdentitySection did not lay out a program core at the identity section's offsets"_s))
        return false;
    if (!part.check(identityBytesMatch(functionSection.span(), functionExpected), "writeIdentitySection did not lay out a function core and its child holder digests at the identity section's offsets"_s))
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
    // N % 8 != 0, so the bit at N lies in the map's last byte.
    size_t lastMapByte = constantCount / 8;
    uint8_t bitAtN = 1u << (constantCount % 8);
    size_t mapEnd = identityButterflyMapOffset + mapBytes;
    return rejects(functionSection, "a wrong magic"_s, true, [](auto& bytes) { bytes[0] ^= 0xff; })
        && rejects(functionSection, "layout version 2"_s, true, [](auto& bytes) { writeField<uint16_t>(bytes.mutableSpan(), identityVersionOffset, 2); })
        && rejects(functionSection, "core kind 4"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = 4; })
        && rejects(functionSection, "a program core under a child key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Program); })
        && rejects(programSection, "an eval core under a program key"_s, true, [](auto& bytes) { bytes[identityCoreKindOffset] = static_cast<uint8_t>(UnlinkedCodeBlockCoreKind::Eval); })
        && rejects(functionSection, "a nonzero reserved byte"_s, true, [](auto& bytes) { bytes[identityReservedOffset] = 1; })
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
        && rejects(programSection, "a map bit at N"_s, true, [&](auto& bytes) { bytes[identityButterflyMapOffset + lastMapByte] |= bitAtN; })
        && (baseSize == mapEnd || rejects(programSection, "nonzero padding"_s, true, [&](auto& bytes) { bytes[mapEnd] = 1; }))
        && rejects(programSection, "a constant count whose map needs eight more bytes"_s, false, [&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), identityConstantCountOffset, constantCount + 64); })
        && rejects(programSection, "a constant count of 1 << 28"_s, false, [](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), identityConstantCountOffset, sectionCountLimit); })
        && rejects(programSection, "one byte more"_s, true, [](auto& bytes) { bytes.append(static_cast<uint8_t>(0)); })
        && rejects(functionSection, "a partial child holder digest"_s, true, [](auto& bytes) { bytes.removeLast(); })
        && rejects(programSection, "one byte fewer"_s, false, [](auto& bytes) {
            bytes.removeLast();
        });
}

// constantMapsFit accepts the butterfly map of the UCB's own constants, and refuses a map that marks a constant of another
// kind or a butterfly not made of strings alone, and another constant count. atomizeStringConstant turns a plain string, as
// a decode leaves a long element of a butterfly, into an atom marked as one, leaves an atom as it is, and allocates no
// cell.
static bool testConstantMapsAndAtomization(VM& vm, TestPart& part)
{
    auto fixture = generateSectionFixture(vm);
    if (!part.check(!!fixture, "the section fixture program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& codeBlock = *fixture->codeBlock.get();
    auto constantCount = static_cast<uint32_t>(codeBlock.constantRegisters().size());
    size_t mapBytes = (static_cast<size_t>(constantCount) + 7) / 8;
    auto atomIndex = stringConstantIndex(codeBlock, "jitcache-u5-atom-constant"_s);
    auto symbolTable = symbolTableConstantIndex(codeBlock);
    auto words = butterflyConstantIndex(codeBlock, "alpha"_s, 3);
    auto mixed = butterflyConstantIndex(codeBlock, "alpha"_s, 2);
    if (!part.check(atomIndex && symbolTable && words && mixed, "the fixture program lacks a constant the test reads"_s))
        return false;

    auto identityWith = [&](std::span<const uint8_t> butterflies, uint32_t count) {
        return IdentitySection {
            .coreKind = UnlinkedCodeBlockCoreKind::Program,
            .key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, filledDigest(0x61)),
            .contextDigest = { },
            .coreDigest = { },
            .holderDigest = std::nullopt,
            .functionParseFields = std::nullopt,
            .constantCount = count,
            .butterflyMap = butterflies,
            .childHolderDigests = { },
        };
    };
    auto mapMarking = [&](uint32_t index) {
        Vector<uint8_t> map(FillWith { }, mapBytes, 0);
        setBit(map.mutableSpan(), index);
        return map;
    };
    Vector<uint8_t> wordsMap = mapMarking(*words);
    if (!part.check(constantMapsFit(codeBlock, identityWith(wordsMap.span(), constantCount)), "constantMapsFit refuses a map that marks the all-string literal"_s))
        return false;
    Vector<uint8_t> tableMarked = mapMarking(*symbolTable);
    Vector<uint8_t> stringMarked = mapMarking(*atomIndex);
    Vector<uint8_t> mixedMarked = mapMarking(*mixed);
    bool refusesMisfits = !constantMapsFit(codeBlock, identityWith(tableMarked.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(stringMarked.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(mixedMarked.span(), constantCount))
        && !constantMapsFit(codeBlock, identityWith(wordsMap.span(), constantCount + 1));
    if (!part.check(refusesMisfits, "constantMapsFit accepts a map that marks a constant of the wrong kind, or another constant count"_s))
        return false;

    // A plain string with characters no other string holds, and generation's atom.
    JSString* plain = jsString(vm, makeString("jitcache-u5-plain-"_s, "runtime-string"_s));
    JSString* atom = constantCell<JSString>(codeBlock, *atomIndex);
    StringImpl* atomImpl = atom->tryGetValueImpl();
    bool preconditions = !plain->tryGetValueImpl()->isAtom() && !plain->isDefinitelyAtom() && atomImpl && atomImpl->isAtom();
    if (!part.check(preconditions, "the plain string of the atomization test is an atom already, or generation's constant is not one"_s))
        return false;
    // Allocating a cell outside a GC deferral asserts under AssertNoGC in the debug builds testjitcache runs in
    // (tryAllocateCellHelper), so the calls below prove they allocate none.
    if (!part.check(!vm.heap.isDeferred(), "the test runs with GC deferred, where AssertNoGC cannot catch an allocation"_s))
        return false;
    {
        AssertNoGC assertNoGC;
        atomizeStringConstant(vm, *plain);
        atomizeStringConstant(vm, *atom);
    }
    if (!part.check(plain->tryGetValueImpl()->isAtom() && plain->isDefinitelyAtom(), "atomizeStringConstant left a plain string plain or unmarked as an atom"_s))
        return false;
    return part.check(atom->tryGetValueImpl() == atomImpl && atom->isDefinitelyAtom(), "atomizeStringConstant changed an atom's value or left it unmarked"_s);
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
        filledDigest(0x73), std::nullopt, UnlinkedCodeBlockCoreKind::Program, std::nullopt, generated);
    auto identity = parseIdentitySection(identityBytes.span(), true);
    bool marksAtomForm = identity && bitIsSet(identity->butterflyMap, *words) && !bitIsSet(identity->butterflyMap, *mixed) && !bitIsSet(identity->butterflyMap, *prefix);
    if (!part.check(marksAtomForm, "the butterfly map of a generated UCB does not mark exactly its literal in the atom form"_s))
        return false;

    CoreEncodeFailure encodeFailure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, generated, nullptr, nullptr, encodeFailure);
    if (!part.check(!!core, "encodeUnlinkedCodeBlockCore failed on the fixture program"_s))
        return false;
    CoreDecodeFailure decodeFailure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* decodedCodeBlock = decodeUnlinkedCodeBlockCore(vm, Ref<CachedBytecode> { *core }, fixture->source, UnlinkedCodeBlockCoreKind::Program, nullptr, false, decodeFailure);
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

JITCACHE_TEST(ucbSectionsConstantMapsAndAtomization, Yes)
{
    TestPart part { context };
    testConstantMapsAndAtomization(*context.vm(), part);
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
