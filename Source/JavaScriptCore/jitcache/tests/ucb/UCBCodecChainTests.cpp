#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "JSCInlines.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/ucb/UCBCodecTestSupport.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestHolders.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/ucb/UCBKeys.h"
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <wtf/ASCIICType.h>
#include <wtf/NotFound.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The tests of the core codec runtime/CachedTypes.cpp gives the UCB lane, through what a UFE carries: TDZ chains against the
// holder's, descriptors, and the default class constructors no descriptor variant can vary alone.

namespace JSC::JITCache::Tests {

using namespace UCBCodecTestSupport;

namespace UCBCodecChainTestsInternal {

static String moduleWithBindings(unsigned bindings)
{
    StringBuilder text;
    // The functions come first, so their own positions do not depend on how many bindings follow.
    text.append("export function jitcacheU7ModuleOuter() { return function jitcacheU7ModuleInner() { return binding0000; }; }\n"_s);
    for (unsigned i = 0; i < bindings; ++i) {
        String number = String::number(i);
        text.append("const binding"_s);
        for (unsigned digits = number.length(); digits < 4; ++digits)
            text.append('0');
        text.append(number, " = "_s, i, ";\n"_s);
    }
    return text.toString();
}

struct ModuleChainFixture {
    SourceCode source;
    Strong<UnlinkedModuleProgramCodeBlock> module;
    UnlinkedFunctionExecutable* outer { nullptr };
    Strong<UnlinkedFunctionCodeBlock> outerBody;
    UnlinkedFunctionExecutable* inner { nullptr };
    Strong<UnlinkedFunctionCodeBlock> innerBody;
};

static ModuleChainFixture generateModuleChain(VM& vm, unsigned bindings)
{
    ModuleChainFixture fixture;
    fixture.source = makeSource(moduleWithBindings(bindings), SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    ParserError error;
    fixture.module = Strong<UnlinkedModuleProgramCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, fixture.source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
    if (!fixture.module || !fixture.module.get()->numberOfFunctionDecls())
        return fixture;
    fixture.outer = fixture.module.get()->functionDecl(0);
    SourceCode outerSource = fixture.outer->linkedSourceCode(fixture.source);
    fixture.outerBody = generateBodyOf(vm, *fixture.outer, outerSource, CodeSpecializationKind::CodeForCall);
    if (!fixture.outerBody || !fixture.outerBody.get()->numberOfFunctionExprs())
        return fixture;
    fixture.inner = fixture.outerBody.get()->functionExpr(0);
    fixture.innerBody = generateBodyOf(vm, *fixture.inner, fixture.inner->linkedSourceCode(outerSource), CodeSpecializationKind::CodeForCall);
    return fixture;
}

// The names "binding" followed by four digits that a payload holds, each with its number of occurrences.
static std::optional<unsigned> countEachBindingOnce(std::span<const uint8_t> payload, unsigned bindings)
{
    static constexpr std::array<uint8_t, 7> prefix { 'b', 'i', 'n', 'd', 'i', 'n', 'g' };
    Vector<unsigned> occurrences(FillWith { }, bindings, 0);
    for (size_t i = 0; i + prefix.size() + 4 <= payload.size(); ++i) {
        if (!equalSpans(payload.subspan(i, prefix.size()), std::span<const uint8_t> { prefix }))
            continue;
        unsigned number = 0;
        bool digits = true;
        for (size_t d = 0; d < 4; ++d) {
            uint8_t character = payload[i + prefix.size() + d];
            digits &= isASCIIDigit(character);
            number = number * 10 + (character - '0');
        }
        if (!digits || number >= bindings)
            return std::nullopt;
        ++occurrences[number];
    }
    for (unsigned count : occurrences) {
        if (count != 1)
            return std::nullopt;
    }
    return bindings;
}

// The links of the chains of a UCB's children, children in table order and each chain from its child's own link, each link
// numbered where it first appears. Two UCBs share links among their children alike exactly when their numberings are equal.
// `sharedLinks` gains one for each link met again.
static Vector<unsigned> childLinkNumbering(UnlinkedCodeBlock& codeBlock, unsigned& sharedLinks)
{
    Vector<const TDZEnvironmentLink*> seen;
    Vector<unsigned> numbering;
    allChildren(codeBlock, [&](UnlinkedFunctionExecutable& child, ChildTable, uint32_t) {
        for (TDZEnvironmentLink* link = child.parentScopeTDZVariables().get(); link; link = link->parent()) {
            size_t number = seen.find(link);
            if (number == notFound) {
                number = seen.size();
                seen.append(link);
            } else
                ++sharedLinks;
            numbering.append(static_cast<unsigned>(number));
        }
        numbering.append(std::numeric_limits<unsigned>::max()); // ends the child's chain
        return true;
    });
    return numbering;
}

// Walks a generated tree and the native bytecode cache's decode of it in step, one function at a time. Each decoded child
// must hold its generated twin's TDZ chain whole, link by link (the same number of links and the same chain digest), and the
// decoded children must share their links where the generated ones do. The decoded side only goes through the native
// cache, since a JITCacheCore encoder never meets a UCB a native decode made. Along the way the generated child goes
// through each JITCacheCore operation that writes a chain against a holder's: its holder digest, which keeps its
// environments' digests, its descriptor, whose chain becomes a start record, and its body's core with it as the holder,
// whose children's chains end in a start record too. `multiLinkChains` counts the chains of more than one link the walk
// compared, and `sharedLinks` the links siblings share.
static bool nativeDecodeKeepsChains(VM& vm, TestPart& part, UnlinkedCodeBlock& generated, UnlinkedCodeBlock& decoded, const SourceCode& source, unsigned& multiLinkChains, unsigned& sharedLinks)
{
    bool sameTables = generated.numberOfFunctionDecls() == decoded.numberOfFunctionDecls() && generated.numberOfFunctionExprs() == decoded.numberOfFunctionExprs();
    if (!part.check(sameTables, "a UCB the native bytecode cache decoded has other child tables than its generated twin"_s))
        return false;
    unsigned decodedSharedLinks = 0;
    if (!part.check(childLinkNumbering(generated, sharedLinks) == childLinkNumbering(decoded, decodedSharedLinks), "the children of a native decode do not share their links where generation shared them"_s))
        return false;
    auto compare = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!part.check(child && decodedChild, "a child UFE is missing from a native decode or from its generated twin"_s))
            return false;
        unsigned digested = 0;
        TDZEnvironmentLink* chain = child->parentScopeTDZVariables().get();
        TDZEnvironmentLink* decodedChain = decodedChild->parentScopeTDZVariables().get();
        auto chainDigest = tdzChainDigest(chain, nullptr, digested);
        unsigned length = chainShape(chain, nullptr).length;
        bool sameChain = chainDigest && tdzChainDigest(decodedChain, nullptr, digested) == chainDigest && chainShape(decodedChain, nullptr).length == length;
        if (!part.check(sameChain, "a child the native bytecode cache decoded does not hold its generated twin's TDZ chain, link by link"_s))
            return false;
        if (length > 1)
            ++multiLinkChains;
        if (!part.check(holderDigest(vm, *child) && encodeDescriptorBytes(vm, *child), "a generated child's holder digest or descriptor failed"_s))
            return false;
        Strong<UnlinkedFunctionCodeBlock> body { vm, decodedBody(vm, *child, source) };
        Strong<UnlinkedFunctionCodeBlock> decodedChildBody { vm, decodedBody(vm, *decodedChild, source) };
        if (!part.check(body && decodedChildBody, "a body of the native round trip is missing"_s))
            return false;
        if (!part.check(!!encodeCoreBytes(vm, *body.get(), child), "a generated function's core with its holder failed to encode"_s))
            return false;
        return nativeDecodeKeepsChains(vm, part, *body.get(), *decodedChildBody.get(), child->linkedSourceCode(source), multiLinkChains, sharedLinks);
    };
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i) {
        if (!compare(generated.functionDecl(i), decoded.functionDecl(i)))
            return false;
    }
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i) {
        if (!compare(generated.functionExpr(i), decoded.functionExpr(i)))
            return false;
    }
    return true;
}

// Function cores and descriptors stay the size they are whatever the enclosing module declares, and the module's own core
// holds each binding once; a chain that does not end in the holder's is written whole; decoding with the holder ends the
// children's chains in its chain object; a start record decoded without a start chain is Malformed; and the native
// bytecode cache writes and reads chains as it does without JITCache.
static bool testCodecTDZChains(VM& vm, TestPart& part)
{
    ModuleChainFixture large = generateModuleChain(vm, 1000);
    ModuleChainFixture small = generateModuleChain(vm, 10);
    if (!part.check(large.innerBody && small.innerBody, "a module of the chain test failed to generate"_s))
        return false;
    auto size = [&](const std::optional<Vector<uint8_t>>& bytes) {
        return bytes ? bytes->size() : 0;
    };
    auto largeOuterCore = encodeCoreBytes(vm, *large.outerBody.get(), large.outer);
    auto largeInnerCore = encodeCoreBytes(vm, *large.innerBody.get(), large.inner);
    bool sameSizes = largeOuterCore && size(largeOuterCore) == size(encodeCoreBytes(vm, *small.outerBody.get(), small.outer))
        && size(largeInnerCore) && size(largeInnerCore) == size(encodeCoreBytes(vm, *small.innerBody.get(), small.inner))
        && size(encodeDescriptorBytes(vm, *large.outer)) && size(encodeDescriptorBytes(vm, *large.outer)) == size(encodeDescriptorBytes(vm, *small.outer))
        && size(encodeDescriptorBytes(vm, *large.inner)) && size(encodeDescriptorBytes(vm, *large.inner)) == size(encodeDescriptorBytes(vm, *small.inner));
    if (!part.check(sameSizes, "a function core or descriptor grows with the bindings its module declares"_s))
        return false;
    auto moduleCore = encodeCoreBytes(vm, *large.module.get(), nullptr);
    if (!part.check(moduleCore && countEachBindingOnce(moduleCore->span(), 1000), "the module's own core does not hold each of its bindings exactly once"_s))
        return false;

    // Encoded with a holder whose chain is another, the children's chains are written link by link, the module's 1,000 names
    // included.
    auto foreignHolder = encodeCoreBytes(vm, *large.outerBody.get(), small.outer);
    if (!part.check(foreignHolder && size(foreignHolder) >= size(largeOuterCore) + 1000 * 11, "a chain that does not end in the holder's chain is not written whole"_s))
        return false;

    // Decoded with the holder, the children's chains end in its chain object and equal the generated children's.
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    SourceCode largeOuterSource = large.outer->linkedSourceCode(large.source);
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, largeOuterCore->span(), largeOuterSource, UnlinkedCodeBlockCoreKind::Function, large.outer, failure) };
    if (!part.check(decoded && decoded.get()->numberOfFunctionExprs() == 1, "a function core of the chain test does not decode with its holder"_s))
        return false;
    UnlinkedFunctionExecutable& decodedInner = *decoded.get()->functionExpr(0);
    TDZEnvironmentLink* holderChain = large.outer->parentScopeTDZVariables().get();
    ChainShape decodedShape = chainShape(decodedInner.parentScopeTDZVariables().get(), holderChain);
    ChainShape generatedShape = chainShape(large.inner->parentScopeTDZVariables().get(), holderChain);
    unsigned digested = 0;
    auto decodedChain = tdzChainDigest(decodedInner.parentScopeTDZVariables().get(), nullptr, digested);
    auto generatedChain = tdzChainDigest(large.inner->parentScopeTDZVariables().get(), nullptr, digested);
    bool chainsMatch = holderChain && decodedShape.targetPosition && decodedShape == generatedShape && decodedChain && decodedChain == generatedChain;
    if (!part.check(chainsMatch, "a decoded child's chain does not end in the holder's chain object or differs from the generated child's"_s))
        return false;
    if (!part.check(encodeCoreBytes(vm, *decoded.get(), large.outer) == largeOuterCore, "decoding and re-encoding a function core with its holder does not give the core back"_s))
        return false;

    // The core's start record, decoded by a holder without a chain.
    SourceCode rootSource;
    Strong<UnlinkedFunctionExecutable> chainless = functionConstructorRoot(vm, "return 'jitcache-u7-chainless';"_s, rootSource);
    if (!part.check(chainless && !chainless.get()->parentScopeTDZVariables(), "the chainless holder has a TDZ chain"_s))
        return false;
    Strong<UnlinkedCodeBlock> withoutStartChain { vm, decodeCoreBytes(vm, largeOuterCore->span(), largeOuterSource, UnlinkedCodeBlockCoreKind::Function, chainless.get(), failure) };
    if (!part.check(!withoutStartChain && failure == CoreDecodeFailure::Malformed, "a start record decoded without a start chain is not Malformed"_s))
        return false;

    // The native bytecode cache has no start chain, so it never writes a start record and its decode is unchanged: it must
    // write and read chains as it does without JITCache. No encoder without start records exists in the process to compare
    // bytes with, so the test checks what start records could have changed. A tree whose sibling functions share chains of
    // two links, and whose arrows' chains end in their functions' own chain objects, goes through the native cache and back
    // with every chain whole and every link shared where generation shared it; and the native bytes for the tree stay the
    // same after the walk has run the JITCacheCore operations on it, which turn those chains into start records and keep
    // each environment's digest.
    ProgramFixture program = generateProgramFixture(vm, String { codecCorpus[nestedScopesCorpusIndex] }, std::numeric_limits<unsigned>::max());
    if (!part.check(!!program.codeBlock, "the native round-trip program failed to generate"_s))
        return false;
    SourceCodeKey key = programCacheKey(program.source);
    RefPtr<CachedBytecode> native = encodeCodeBlock(vm, key, program.codeBlock.get());
    if (!part.check(!!native, "the native bytecode cache did not encode the round-trip program"_s))
        return false;
    Strong<UnlinkedProgramCodeBlock> nativeDecode { vm, decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, Ref<CachedBytecode> { *native }) };
    if (!part.check(!!nativeDecode, "the native bytecode cache did not decode the round-trip program"_s))
        return false;
    unsigned multiLinkChains = 0;
    unsigned sharedLinks = 0;
    if (!nativeDecodeKeepsChains(vm, part, *program.codeBlock.get(), *nativeDecode.get(), program.source, multiLinkChains, sharedLinks))
        return false;
    if (!part.check(multiLinkChains >= 2 && sharedLinks, "the native round trip compared fewer than two chains of several links, or no link that siblings share, so it cannot show the cache keeps them"_s))
        return false;
    RefPtr<CachedBytecode> nativeAgain = encodeCodeBlock(vm, key, program.codeBlock.get());
    return part.check(nativeAgain && equalSpans(native->span(), nativeAgain->span()), "the native bytecode cache writes other bytes for a tree after the JITCacheCore operations on it"_s);
}

// A UFE's descriptor is the same before and after its body is parsed and its slots filled, differs with each field it
// holds, its flags one by one included, and is the same for UFEs that differ only in their TDZ chains (the holder variants
// the holder digest test reads too).
static bool testCodecDescriptors(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "function jitcacheU7Descriptor(a, b) { return a + b; }"_s);
    if (!part.check(!!fixture.codeBlock, "the descriptor program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& executable = *fixture.codeBlock.get()->functionDecl(0);
    auto before = encodeDescriptorBytes(vm, executable);
    executable.recordParse(ArgumentsFeature, executable.lexicallyScopedFeatures(), true);
    ParserError error;
    UnlinkedFunctionCodeBlock* body = executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(fixture.source), CodeSpecializationKind::CodeForCall, { }, error, executable.parseMode());
    auto after = encodeDescriptorBytes(vm, executable);
    if (!part.check(body && before && before == after, "a descriptor changes with its UFE's own parse or the filling of its slot"_s))
        return false;
    for (const HolderVariant& variant : holderVariants) {
        HolderFixture base;
        HolderFixture other;
        if (!generateHolderPair(vm, globalObject, variant, part, base, other))
            return false;
        auto baseDescriptor = encodeDescriptorBytes(vm, *base.executable);
        auto otherDescriptor = encodeDescriptorBytes(vm, *other.executable);
        if (!part.check(baseDescriptor && otherDescriptor && ((*baseDescriptor != *otherDescriptor) == variant.descriptorDiffers), makeString("the descriptor does not follow "_s, variant.field)))
            return false;
    }
    return true;
}

// A default class constructor, which only BuiltinExecutables::createDefaultConstructor makes, keeps
// isBuiltinDefaultClassConstructor and its constructor kind through a core, as generation left them. No holder variant can
// vary that flag alone, so the round trip checks it.
static bool testCodecDefaultConstructors(VM& vm, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "class JitcacheU7DefaultBase { }\nclass JitcacheU7DefaultDerived extends JitcacheU7DefaultBase { }\n"_s);
    if (!part.check(!!fixture.codeBlock, "the default-constructor program failed to generate"_s))
        return false;
    UnlinkedCodeBlock& generated = *fixture.codeBlock.get();
    auto core = encodeCoreBytes(vm, generated, nullptr);
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), fixture.source, UnlinkedCodeBlockCoreKind::Program, nullptr, failure) : nullptr };
    bool sameTables = decoded && decoded.get()->numberOfFunctionDecls() == generated.numberOfFunctionDecls() && decoded.get()->numberOfFunctionExprs() == generated.numberOfFunctionExprs();
    if (!part.check(sameTables, "the default-constructor program's core does not decode to its child tables"_s))
        return false;
    unsigned defaultConstructors = 0;
    auto sameKind = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        defaultConstructors += child->isBuiltinDefaultClassConstructor();
        return child->isBuiltinDefaultClassConstructor() == decodedChild->isBuiltinDefaultClassConstructor() && child->constructorKind() == decodedChild->constructorKind();
    };
    bool kept = true;
    for (unsigned i = 0; i < generated.numberOfFunctionDecls(); ++i)
        kept &= sameKind(generated.functionDecl(i), decoded.get()->functionDecl(i));
    for (unsigned i = 0; i < generated.numberOfFunctionExprs(); ++i)
        kept &= sameKind(generated.functionExpr(i), decoded.get()->functionExpr(i));
    if (!part.check(defaultConstructors == 2, "the default-constructor program does not hold a base and a derived default constructor"_s))
        return false;
    return part.check(kept, "a default constructor decoded from a core lost isBuiltinDefaultClassConstructor or its constructor kind"_s);
}

} // namespace UCBCodecChainTestsInternal

using namespace UCBCodecChainTestsInternal;

JITCACHE_TEST(ucbCodecTDZChains, Yes)
{
    TestPart part { context };
    testCodecTDZChains(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecDescriptors, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecDescriptors(vm, entered.globalObject(), part);
}

JITCACHE_TEST(ucbCodecDefaultConstructors, Yes)
{
    TestPart part { context };
    testCodecDefaultConstructors(*context.vm(), part);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
