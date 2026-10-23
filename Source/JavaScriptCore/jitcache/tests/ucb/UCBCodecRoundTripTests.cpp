#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DirectEvalExecutable.h"
#include "IndirectEvalExecutable.h"
#include "JSCInlines.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VariableEnvironmentInlines.h"
#include "jitcache/base/ByteFields.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/ucb/UCBCodecTestSupport.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/ucb/UCBFeedback.h"
#include "jitcache/ucb/UCBTwinGeneration.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of the core codec runtime/CachedTypes.cpp gives the UCB lane, through whole cores: the round trip of every root
// record and function core; line starts, which a core leaves to the provider; the payload header's failures; and, through
// runCoreCodecSelfTest, the cases that alter a core's records.

namespace JSC::JITCache::Tests {

using namespace UCBCodecTestSupport;

namespace UCBCodecRoundTripTestsInternal {

// Whether a declaration map holds a symbol key. A symbol's hash is per process, so a hashed map holding one would lay out
// differently in another process, and the tests assert that no corpus program declares one.
static bool holdsSymbolKey(const VariableEnvironment& environment)
{
    for (const auto& entry : environment) {
        if (entry.key->isSymbol())
            return true;
    }
    return false;
}

static bool sameDeclarationOrder(const VariableEnvironment& a, const VariableEnvironment& b)
{
    if (a.mapSize() != b.mapSize())
        return false;
    auto other = b.begin();
    for (const auto& entry : a) {
        if (other == b.end() || entry.key.get() != other->key.get() || !(entry.value == other->value))
            return false;
        ++other;
    }
    return true;
}

// What the twin checks compare between a UCB and the UCB its core decodes to: the same encoding, every string constant
// register an atom, the same global parse fields and declaration orders, the same feedback layout, and children with the
// same fields, unparsed, whose chains have the same shape over the holder's.
static bool sameAsDecoded(VM& vm, TestPart& part, UnlinkedCodeBlock& original, UnlinkedCodeBlock& decoded, const UnlinkedFunctionExecutable* holder, std::span<const uint8_t> core, ASCIILiteral what)
{
    auto encoding = encodeCoreBytes(vm, decoded, holder);
    if (!part.check(encoding && equalSpans(encoding->span(), core), makeString("decoding and re-encoding "_s, what, " does not give its core back"_s)))
        return false;
    if (!part.check(allStringConstantsAreAtoms(decoded), makeString("a string constant register of decoded "_s, what, " is not an atom"_s)))
        return false;
    if (!part.check(liveFeedbackCounts(original, BodyImage::Absent) == liveFeedbackCounts(decoded, BodyImage::Absent), makeString("decoded "_s, what, " has other profile, child or constant counts"_s)))
        return false;
    // UnlinkedGlobalCodeBlock declares no ClassInfo, so a dynamic cast to it accepts a function UCB, whose cell lacks these
    // fields. A function body's parse fields are its UFE's; they travel in the identity section, and both sides share the
    // holder.
    if (!is<UnlinkedFunctionCodeBlock>(original)) {
        auto& originalGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(original);
        auto& decodedGlobal = uncheckedDowncast<UnlinkedGlobalCodeBlock>(decoded);
        bool sameParse = originalGlobal.codeFeatures() == decodedGlobal.codeFeatures() && originalGlobal.lexicallyScopedFeatures() == decodedGlobal.lexicallyScopedFeatures()
            && originalGlobal.hasCapturedVariables() == decodedGlobal.hasCapturedVariables() && WTF::equal(originalGlobal.sourceURLDirective(), decodedGlobal.sourceURLDirective())
            && WTF::equal(originalGlobal.sourceMappingURLDirective(), decodedGlobal.sourceMappingURLDirective());
        if (!part.check(sameParse, makeString("decoded "_s, what, " has other global parse fields"_s)))
            return false;
    }
    if (auto* originalProgram = dynamicDowncast<UnlinkedProgramCodeBlock>(original)) {
        auto& decodedProgram = uncheckedDowncast<UnlinkedProgramCodeBlock>(decoded);
        bool sameOrder = sameDeclarationOrder(originalProgram->variableDeclarations(), decodedProgram.variableDeclarations())
            && sameDeclarationOrder(originalProgram->lexicalDeclarations(), decodedProgram.lexicalDeclarations());
        if (!part.check(sameOrder, makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalModule = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(original)) {
        auto& decodedModule = uncheckedDowncast<UnlinkedModuleProgramCodeBlock>(decoded);
        if (!part.check(sameDeclarationOrder(originalModule->variableDeclarations(), decodedModule.variableDeclarations()), makeString("decoded "_s, what, " iterates its declarations in another order"_s)))
            return false;
    }
    if (auto* originalEval = dynamicDowncast<UnlinkedEvalCodeBlock>(original)) {
        auto& decodedEval = uncheckedDowncast<UnlinkedEvalCodeBlock>(decoded);
        bool sameNames = std::ranges::equal(originalEval->variables(), decodedEval.variables())
            && std::ranges::equal(originalEval->functionHoistingCandidates(), decodedEval.functionHoistingCandidates());
        if (!part.check(sameNames, makeString("decoded "_s, what, " has other variables or function hoisting candidates"_s)))
            return false;
    }
    const TDZEnvironmentLink* holderChain = holder ? holder->parentScopeTDZVariables().get() : nullptr;
    auto sameChild = [&](UnlinkedFunctionExecutable* child, UnlinkedFunctionExecutable* decodedChild) {
        if (!child || !decodedChild)
            return false;
        auto descriptor = encodeDescriptorBytes(vm, *child);
        auto decodedDescriptor = encodeDescriptorBytes(vm, *decodedChild);
        return descriptor && decodedDescriptor && *descriptor == *decodedDescriptor && !decodedChild->features() && !decodedChild->hasCapturedVariables()
            && decodedChild->lexicallyScopedFeatures() == child->lexicallyScopedFeatures()
            && chainShape(decodedChild->parentScopeTDZVariables().get(), holderChain) == chainShape(child->parentScopeTDZVariables().get(), holderChain);
    };
    for (unsigned i = 0; i < original.numberOfFunctionDecls(); ++i) {
        if (!part.check(sameChild(original.functionDecl(i), decoded.functionDecl(i)), makeString("function declaration "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    for (unsigned i = 0; i < original.numberOfFunctionExprs(); ++i) {
        if (!part.check(sameChild(original.functionExpr(i), decoded.functionExpr(i)), makeString("function expression "_s, i, " of decoded "_s, what, " differs from the original's"_s)))
            return false;
    }
    return true;
}

// Encodes twice, decodes the core, and compares the decode with the original.
static bool roundTripsCore(VM& vm, TestPart& part, UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, ASCIILiteral what)
{
    auto core = encodeCoreBytes(vm, codeBlock, holder);
    auto again = encodeCoreBytes(vm, codeBlock, holder);
    if (!part.check(core && again && *core == *again, makeString("encoding "_s, what, " twice does not give equal bytes"_s)))
        return false;
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, core->span(), source, kind, holder, failure) };
    if (!part.check(decoded && failure == CoreDecodeFailure::None, makeString("the core of "_s, what, " does not decode"_s)))
        return false;
    return sameAsDecoded(vm, part, codeBlock, *decoded.get(), holder, core->span(), what);
}

// The corpus module, both corpus evals, every corpus program and the functions the programs declare encode
// deterministically, decode to UCBs equal field by field, and re-encode to their cores.
static bool testCodecCorpus(VM& vm, JSGlobalObject* globalObject, TestPart& part)
{
    // An import decodes every core kind, so the round trip covers each root record the codec has.
    {
        SourceCode moduleSource = makeSource(String { moduleCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
        ParserError error;
        Strong<UnlinkedModuleProgramCodeBlock> module { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, moduleSource, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
        bool hashedDeclarations = module && module.get()->variableDeclarations().mapSize() > VariableEnvironment::inlineMapCapacity;
        if (!part.check(hashedDeclarations, "the corpus module failed to generate, or its declarations fit inline storage, so the hashed layout goes untested"_s))
            return false;
        if (!part.check(!holdsSymbolKey(module.get()->variableDeclarations()), "the corpus module declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        if (!roundTripsCore(vm, part, *module.get(), nullptr, moduleSource, UnlinkedCodeBlockCoreKind::Module, "the corpus module"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { indirectEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        Strong<IndirectEvalExecutable> indirectEval { vm, IndirectEvalExecutable::tryCreate(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, false, EvalContextType::None) };
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, indirectEval ? indirectEval.get()->unlinkedCodeBlock() : nullptr };
        bool namesDeclarations = codeBlock && codeBlock.get()->numVariables() && codeBlock.get()->numFunctionHoistingCandidates();
        if (!part.check(namesDeclarations, "the corpus indirect eval failed to generate, or lacks its variables or its hoisting candidate"_s))
            return false;
        if (!roundTripsCore(vm, part, *codeBlock.get(), nullptr, evalSource, UnlinkedCodeBlockCoreKind::Eval, "the corpus indirect eval"_s))
            return false;
    }
    {
        SourceCode evalSource = makeSource(String { directEvalCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        TDZEnvironment variablesUnderTDZ = tdzNames(vm, { "jitcacheU7DirectOuter"_s });
        PrivateNameEnvironment privateNames;
        DirectEvalExecutable* directEval = DirectEvalExecutable::create(globalObject, evalSource, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
            PrivateBrandRequirement::None, false, true, EvalContextType::FunctionEvalContext, &variablesUnderTDZ, &privateNames);
        Strong<UnlinkedEvalCodeBlock> codeBlock { vm, directEval ? directEval->unlinkedCodeBlock() : nullptr };
        UnlinkedFunctionExecutable* function = nullptr;
        if (codeBlock) {
            function = selectChild(*codeBlock.get(), [](UnlinkedFunctionExecutable&) {
                return true;
            });
        }
        if (!part.check(function && function->parentScopeTDZVariables(), "the corpus direct eval failed to generate, or its function holds no TDZ chain"_s))
            return false;
        if (!roundTripsCore(vm, part, *codeBlock.get(), nullptr, evalSource, UnlinkedCodeBlockCoreKind::Eval, "the corpus direct eval"_s))
            return false;
    }

    Vector<String> texts;
    for (ASCIILiteral text : codecCorpus)
        texts.append(String { text });
    texts.append(bigProgramText());
    for (const String& text : texts) {
        // Only roundTripsChild generates children, never a class field initializer's (see there).
        ProgramFixture fixture = generateProgramFixture(vm, text, 0);
        if (!part.check(!!fixture.codeBlock, "a corpus program failed to generate"_s))
            return false;
        UnlinkedProgramCodeBlock& program = *fixture.codeBlock.get();
        bool declaresSymbol = holdsSymbolKey(program.variableDeclarations()) || holdsSymbolKey(program.lexicalDeclarations());
        if (!part.check(!declaresSymbol, "a corpus program declares a symbol, whose per-process hash would make its hashed layout differ between processes"_s))
            return false;
        auto roundTripsChild = [&](UnlinkedFunctionExecutable* child) {
            if (!child)
                return false;
            // A class field initializer's body is never generated here. Its parse makes one DefineFieldNode per field, a
            // parser-arena node whose destructor never runs but which holds the field's Identifier, so each field name's atom
            // leaks with JITCache off too; testjitcache has no oracle run to show that the leak is the engine's own, so the
            // test avoids input the engine leaks on. The initializer's descriptor, class element definitions included, goes
            // through the program's core.
            if (child->parseMode() == SourceParseMode::ClassFieldInitializerMode)
                return true;
            CodeSpecializationKind kind = child->isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
            ParserError error;
            // The core decodes against the child's own source, as an import decodes against the request's.
            SourceCode childSource = child->linkedSourceCode(fixture.source);
            Strong<UnlinkedFunctionCodeBlock> body { vm, child->unlinkedCodeBlockFor(vm, childSource, kind, { }, error, child->parseMode()) };
            if (!body)
                return true; // a body that does not generate on its own
            return roundTripsCore(vm, part, *body.get(), child, childSource, UnlinkedCodeBlockCoreKind::Function, "a corpus function"_s);
        };
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionDecls(), 8); ++i) {
            if (!roundTripsChild(program.functionDecl(i)))
                return false;
        }
        for (unsigned i = 0; i < std::min<size_t>(program.numberOfFunctionExprs(), 8); ++i) {
            if (!roundTripsChild(program.functionExpr(i)))
                return false;
        }
        // After the children's round trips, whose generation parsed them and filled their slots: the program's decoded
        // children must still come back unparsed, with the lexically scoped features generation gave them.
        if (!roundTripsCore(vm, part, program, nullptr, fixture.source, UnlinkedCodeBlockCoreKind::Program, "a corpus program"_s))
            return false;
    }
    return true;
}

// A program generated after something built its provider's line-start table holds the table, and its native encoding
// writes it, while its core leaves the table out: the core equals the core of the same text generated with no table built,
// and decodes with no line starts.
static bool testCodecLineStarts(VM& vm, TestPart& part)
{
    // Shorter than SourceProvider::minimumLengthToHaveLineStartsWithTheCode, so that generation's own parse builds no table.
    String text = "var jitcacheU7Lines = 1;\nfunction jitcacheU7LineFunction() {\n    return jitcacheU7Lines;\n}\n"_s;
    auto generate = [&](const SourceCode& source) {
        ParserError error;
        return Strong<UnlinkedProgramCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
    };

    SourceCode withTable = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    withTable.provider()->positionInfoForOffset(text.length() - 1);
    Strong<UnlinkedProgramCodeBlock> holder = generate(withTable);
    if (!part.check(holder && withTable.provider()->lineStartTableIsBuilt() && holder.get()->lineStarts(), "a program generated after its provider's line-start table was built does not hold the table"_s))
        return false;
    SourceCodeKey key = programCacheKey(withTable);
    RefPtr<CachedBytecode> native = encodeCodeBlock(vm, key, holder.get());
    Strong<UnlinkedProgramCodeBlock> nativeDecode { vm, native ? decodeCodeBlock<UnlinkedProgramCodeBlock>(vm, key, Ref<CachedBytecode> { *native }) : nullptr };
    bool nativeWritesTable = nativeDecode && equalSpans(nativeDecode.get()->lineStarts().bytes, holder.get()->lineStarts().bytes);
    if (!part.check(nativeWritesTable, "the native bytecode cache does not write a program's line-start table"_s))
        return false;

    SourceCode withoutTable = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedProgramCodeBlock> plain = generate(withoutTable);
    if (!part.check(plain && !plain.get()->lineStarts() && !withoutTable.provider()->lineStartTableIsBuilt(), "a program generated with no line-start table built holds one"_s))
        return false;
    auto core = encodeCoreBytes(vm, *holder.get(), nullptr);
    if (!part.check(core && core == encodeCoreBytes(vm, *plain.get(), nullptr), "a program's core depends on whether its provider's line-start table was built before generation"_s))
        return false;
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, core->span(), withTable, UnlinkedCodeBlockCoreKind::Program, nullptr, failure) };
    auto* program = decoded ? dynamicDowncast<UnlinkedProgramCodeBlock>(decoded.get()) : nullptr;
    return part.check(program && !program->lineStarts(), "a program's core decodes with line starts"_s);
}

static bool failsToDecodeWith(VM& vm, std::span<const uint8_t> bytes, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, CoreDecodeFailure expected)
{
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* codeBlock = decodeCoreBytes(vm, bytes, source, kind, nullptr, failure);
    return !codeBlock && failure == expected;
}

// Each Malformed and KindMismatch condition the payload's header holds. The cases that alter a core's records are
// runCoreCodecSelfTest's, in ucb/codec/UCBCoreCodecSelfTest.cpp, which runtime/CachedTypes.cpp includes after the record
// types.
static bool testCodecHeader(VM& vm, TestPart& part)
{
    ProgramFixture fixture = generateFixture(vm, "var jitcacheU7Header = 'jitcache-u7-header';\n"_s);
    if (!part.check(!!fixture.codeBlock, "the malformed-payload program failed to generate"_s))
        return false;
    auto core = encodeCoreBytes(vm, *fixture.codeBlock.get(), nullptr);
    if (!part.check(core && core->size() > 16, "the malformed-payload program does not encode"_s))
        return false;
    const SourceCode& source = fixture.source;
    const Vector<uint8_t>& valid = *core;
    auto altered = [&](const auto& mutate) {
        Vector<uint8_t> bytes = valid;
        mutate(bytes);
        return bytes;
    };

    uint32_t rootOffset = readField<uint32_t>(valid.span(), 8);
    Vector<uint8_t> misaligned(FillWith { }, valid.size() + 8, 0);
    memcpySpan(misaligned.mutableSpan().subspan(1, valid.size()), valid.span());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    bool misalignedFails = !decodeUnlinkedCodeBlockCore(vm, CachedBytecode::create(misaligned.mutableSpan().subspan(1, valid.size()), [](const void*) { }, { }), source, UnlinkedCodeBlockCoreKind::Program, nullptr, failure)
        && failure == CoreDecodeFailure::Malformed;
    bool headerConditions = failsToDecodeWith(vm, valid.span().first(15), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && misalignedFails
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[0] ^= 0xff; }).span(), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[5] = 1; }).span(), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([](auto& bytes) { bytes[12] = 1; }).span(), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), 8, rootOffset + 1); }).span(), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, altered([&](auto& bytes) { writeField<uint32_t>(bytes.mutableSpan(), 8, static_cast<uint32_t>(bytes.size())); }).span(), source, UnlinkedCodeBlockCoreKind::Program, CoreDecodeFailure::Malformed)
        && failsToDecodeWith(vm, valid.span(), source, UnlinkedCodeBlockCoreKind::Module, CoreDecodeFailure::KindMismatch);
    return part.check(headerConditions, "a payload breaking a condition of its header does not decode to Malformed or KindMismatch"_s);
}

} // namespace UCBCodecRoundTripTestsInternal

using namespace UCBCodecRoundTripTestsInternal;

JITCACHE_TEST(ucbCodecCorpusRoundTrips, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecCorpus(vm, entered.globalObject(), part);
}

JITCACHE_TEST(ucbCodecLineStarts, Yes)
{
    TestPart part { context };
    testCodecLineStarts(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecHeaderConditions, Yes)
{
    TestPart part { context };
    testCodecHeader(*context.vm(), part);
}

// The cases runCoreCodecSelfTest runs against the record types of runtime/CachedTypes.cpp. A program core and a function
// core decode unaltered, and every other case fails as it names: InconsistentMapLayout for a declaration map layout the
// map cannot restore, UnresolvedSymbol for a private name this VM lacks, and Malformed for a start record decoded
// without a start chain or with an empty handle and a parent.
JITCACHE_TEST(ucbCodecAlteredRecords, Yes)
{
    String failure;
    if (!runCoreCodecSelfTest(*context.vm(), failure))
        JITCACHE_FAIL(failure);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
