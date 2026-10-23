#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BuiltinExecutables.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DeferGC.h"
#include "HeapIterationScope.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "MarkedSpaceInlines.h"
#include "RegExp.h"
#include "RegExpCache.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/ucb/UCBCodecTestSupport.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The tests of the core codec runtime/CachedTypes.cpp gives the UCB lane, through what an encode charges and the cells a
// core holds: chunked descriptor encodes and encoding budgets; encodes that allocate no cell; string constants that decode
// as atoms; a decode that leaves nothing in its payload; and RegExp constants and the ordered-hash-table sentinel.

namespace JSC::JITCache::Tests {

using namespace UCBCodecTestSupport;

namespace UCBCodecCellTestsInternal {

// A class whose field initializer's descriptor spans several pages.
static String manyFieldsText()
{
    StringBuilder text;
    text.append("class JitcacheU7Fields { "_s);
    for (unsigned i = 0; i < 1500; ++i)
        text.append("jitcacheU7Field"_s, i, " = 0; "_s);
    text.append("}\n"_s);
    return text.toString();
}

// Counts what an encode keeps charged (signed, so a release of something never charged shows), and refuses, for good,
// from the `refuseAt`-th charge of at least a first page's size; zero never refuses.
class TestEncodingBudget final : public CoreEncodingBudget {
public:
    explicit TestEncodingBudget(unsigned refuseAt = 0)
        : m_refuseAt(refuseAt)
    {
    }

    bool charge(size_t bytes) final
    {
        if (m_refused)
            return false;
        if (!m_firstCharge)
            m_firstCharge = bytes;
        if (bytes >= firstPageSize && m_refuseAt && ++m_pageSizedCharges == m_refuseAt) {
            m_refused = true;
            return false;
        }
        m_charged += static_cast<int64_t>(bytes);
        return true;
    }

    void release(size_t bytes) final { m_charged -= static_cast<int64_t>(bytes); }

    int64_t charged() const { return m_charged; }
    bool refused() const { return m_refused; }
    size_t firstCharge() const { return m_firstCharge; }

    static constexpr size_t firstPageSize = 4096;

private:
    const unsigned m_refuseAt;
    unsigned m_pageSizedCharges { 0 };
    bool m_refused { false };
    size_t m_firstCharge { 0 };
    int64_t m_charged { 0 };
};

// A descriptor's chunks concatenate to the payload the releasing encoder returns, and the first page is 4 KiB; with a
// counting budget a payload's size stays charged; under a budget that refuses the third page every entry point returns
// BudgetRefused, the sink sees nothing and the budget is back at zero.
static bool testCodecChunksAndBudgets(VM& vm, TestPart& part)
{
    ProgramFixture big = generateProgramFixture(vm, bigProgramText(), 0);
    ProgramFixture fields = generateProgramFixture(vm, manyFieldsText(), 0);
    ProgramFixture tiny = generateProgramFixture(vm, "function jitcacheU7Tiny() { }\n"_s, 0);
    if (!part.check(big.codeBlock && fields.codeBlock && tiny.codeBlock, "a program of the chunk and budget test failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable* initializer = selectChild(*fields.codeBlock.get(), [](UnlinkedFunctionExecutable& executable) {
        return executable.parseMode() == SourceParseMode::ClassFieldInitializerMode;
    });
    UnlinkedFunctionExecutable* tinyFunction = tiny.codeBlock.get()->functionDecl(0);
    if (!part.check(initializer && tinyFunction, "a chunking fixture lacks its UFE"_s))
        return false;

    auto descriptorChunks = [&](UnlinkedFunctionExecutable& executable, CoreEncodingBudget* budget, Vector<size_t>& sizes, CoreEncodeFailure& failure) {
        Vector<uint8_t> bytes;
        failure = forEachUnlinkedFunctionExecutableDescriptorChunk(vm, executable, budget, [&](std::span<const uint8_t> chunk) {
            sizes.append(chunk.size());
            bytes.append(chunk);
        });
        return bytes;
    };

    for (auto* executable : { initializer, tinyFunction }) {
        auto payload = encodeDescriptorBytes(vm, *executable);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        Vector<uint8_t> concatenated = descriptorChunks(*executable, nullptr, sizes, failure);
        bool chunked = payload && failure == CoreEncodeFailure::None && concatenated == *payload && !sizes.isEmpty() && sizes[0] <= TestEncodingBudget::firstPageSize;
        if (!part.check(chunked, "a descriptor's chunks do not concatenate to its payload, or its first page is larger than 4 KiB"_s))
            return false;
        if (executable == initializer && !part.check(sizes.size() > 2, "the large descriptor does not span three pages"_s))
            return false;
    }

    // A counting budget: the first charge is the first page, and exactly the payload's size stays charged.
    {
        TestEncodingBudget budget;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!part.check(core && budget.firstCharge() == TestEncodingBudget::firstPageSize && budget.charged() == static_cast<int64_t>(core->size()), "a core encode does not start with a 4 KiB page or does not leave its payload's size charged"_s))
            return false;
        TestEncodingBudget descriptorBudget;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &descriptorBudget, failure);
        if (!part.check(descriptor && descriptorBudget.charged() == static_cast<int64_t>(descriptor->size()), "a descriptor encode does not leave its payload's size charged"_s))
            return false;
    }

    // A budget that refuses the third page.
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, *big.codeBlock.get(), nullptr, &budget, failure);
        if (!part.check(!core && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a core encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, *initializer, &budget, failure);
        if (!part.check(!descriptor && failure == CoreEncodeFailure::BudgetRefused && budget.refused() && !budget.charged(), "a descriptor encode under a refusing budget does not return BudgetRefused with nothing charged"_s))
            return false;
    }
    {
        TestEncodingBudget budget(3);
        Vector<size_t> sizes;
        CoreEncodeFailure failure = CoreEncodeFailure::None;
        descriptorChunks(*initializer, &budget, sizes, failure);
        if (!part.check(failure == CoreEncodeFailure::BudgetRefused && sizes.isEmpty() && !budget.charged(), "the descriptor chunks under a refusing budget reach the sink or leave a charge"_s))
            return false;
    }
    return true;
}

// The cells the heap holds now, the ones allocated since the last collection included: the HeapIterationScope stops
// allocation, which records each new cell, so forEachLiveCell walks those too. Heap::objectCount() counts only marked cells,
// so a new cell does not show in it.
static size_t liveCellCount(VM& vm)
{
    size_t count = 0;
    HeapIterationScope iterationScope(vm.heap);
    vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell*, HeapCell::Kind) {
        ++count;
        return IterationStatus::Continue;
    });
    return count;
}

// Encoding cores and descriptors allocates no cell: under one DeferGC, so that no collection frees a cell meanwhile, the heap
// holds as many live cells after two program cores, the cores of their functions with their UFEs as holders and those
// UFEs' descriptors were encoded as before. The functions are generated first. None is a class field initializer, whose
// generation leaks with JITCache off too (see testCodecCorpus).
static bool testCodecAllocatesNoCell(VM& vm, TestPart& part)
{
    ProgramFixture switches = generateProgramFixture(vm, String { codecCorpus[0] }, std::numeric_limits<unsigned>::max());
    ProgramFixture scopes = generateProgramFixture(vm, String { codecCorpus[nestedScopesCorpusIndex] }, std::numeric_limits<unsigned>::max());
    if (!part.check(switches.codeBlock && scopes.codeBlock, "a program of the allocation test failed to generate"_s))
        return false;
    Vector<std::pair<UnlinkedFunctionExecutable*, Strong<UnlinkedFunctionCodeBlock>>> functions;
    for (auto* fixture : { &switches, &scopes }) {
        allChildren(*fixture->codeBlock.get(), [&](UnlinkedFunctionExecutable& child, ChildTable, uint32_t) {
            functions.append({ &child, Strong<UnlinkedFunctionCodeBlock> { vm, decodedBody(vm, child, fixture->source) } });
            return true;
        });
    }

    bool encoded = functions.size() >= 3;
    size_t before = 0;
    size_t after = 0;
    {
        DeferGC deferGC(vm);
        before = liveCellCount(vm);
        for (auto* fixture : { &switches, &scopes })
            encoded &= !!encodeCoreBytes(vm, *fixture->codeBlock.get(), nullptr);
        for (auto& [function, body] : functions)
            encoded &= body && encodeCoreBytes(vm, *body.get(), function) && encodeDescriptorBytes(vm, *function);
        after = liveCellCount(vm);
    }
    if (!part.check(encoded, "a core or descriptor of the allocation test failed to encode, or the programs have fewer than three functions"_s))
        return false;
    return part.check(before == after, makeString("encoding cores and descriptors changed the heap's live cells from "_s, before, " to "_s, after));
}

static std::optional<uint32_t> butterflyConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<JSCellButterfly>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// A function whose generated UCB holds a long string constant and an all-string literal in the atom form decodes from its
// core with every string constant register an atom, while the literal's butterfly keeps what the butterfly decode gives
// it: the plain structure, and string elements that are fresh strings of the same characters, not the VM's canonical
// strings generation put there.
static bool testCodecStringConstants(VM& vm, TestPart& part)
{
    static constexpr ASCIILiteral longConstant = "jitcache-u7-a-long-string-constant-that-is-no-inline-string"_s;
    String text = makeString("function jitcacheU7Atoms(s) { var long = '"_s, longConstant, "'; return ['jitcache-u7-word-one', 'jitcache-u7-word-two'].indexOf(s) + long.length; }\n"_s);
    ProgramFixture generated = generateProgramFixture(vm, text, std::numeric_limits<unsigned>::max());
    if (!part.check(generated.codeBlock && generated.codeBlock.get()->numberOfFunctionDecls() == 1, "the string-constant program failed to generate"_s))
        return false;
    UnlinkedFunctionExecutable& function = *generated.codeBlock.get()->functionDecl(0);
    Strong<UnlinkedFunctionCodeBlock> body { vm, decodedBody(vm, function, generated.source) };
    std::optional<uint32_t> literal = body ? butterflyConstant(*body.get()) : std::nullopt;
    auto* generatedLiteral = literal ? constantCell<JSCellButterfly>(*body.get(), *literal) : nullptr;
    bool generatedForms = generatedLiteral && generatedLiteral->structure() == vm.cellButterflyOnlyAtomStringsStructure.get()
        && stringConstantIndex(*body.get(), longConstant) && allStringConstantsAreAtoms(*body.get());
    if (!part.check(generatedForms, "the generated function lacks its long string constant or its all-string literal in the atom form"_s))
        return false;
    auto core = encodeCoreBytes(vm, *body.get(), &function);
    if (!part.check(!!core, "the string-constant function's core failed to encode"_s))
        return false;
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, core->span(), function.linkedSourceCode(generated.source), UnlinkedCodeBlockCoreKind::Function, &function, failure) };
    if (!part.check(decoded && allStringConstantsAreAtoms(*decoded.get()), "a string constant register of a decoded core is not an atom"_s))
        return false;
    auto* decodedLiteral = constantCell<JSCellButterfly>(*decoded.get(), *literal);
    bool keepsDecode = decodedLiteral && decodedLiteral->structure() == vm.cellButterflyStructure(decodedLiteral->indexingMode())
        && decodedLiteral->length() == generatedLiteral->length();
    for (unsigned i = 0; keepsDecode && i < decodedLiteral->length(); ++i) {
        JSValue element = decodedLiteral->get(i);
        JSValue generatedElement = generatedLiteral->get(i);
        keepsDecode = element.isString() && element != generatedElement && asString(element)->tryGetValueImpl()
            && String { asString(element)->tryGetValueImpl() } == String { asString(generatedElement)->tryGetValueImpl() };
    }
    return part.check(keepsDecode, "a decoded core's butterfly does not keep the plain structure and the string elements its decode gives"_s);
}

// Decodes a copy of `bytes`, as decodeCoreBytes does, from a payload whose destructor sets `destroyed`.
static UnlinkedCodeBlock* decodeCoreBytesNotingRelease(VM& vm, std::span<const uint8_t> bytes, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, bool& destroyed)
{
    Vector<uint8_t> copy { bytes };
    std::span<uint8_t> data = copy.mutableSpan();
    destroyed = false;
    auto destructor = [copy = WTF::move(copy), &destroyed](const void*) {
        destroyed = true;
    };
    Ref<CachedBytecode> payload = CachedBytecode::create(data, WTF::move(destructor), { });
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    UnlinkedCodeBlock* codeBlock = decodeUnlinkedCodeBlockCore(vm, WTF::move(payload), source, kind, nullptr, failure);
    return failure == CoreDecodeFailure::None ? codeBlock : nullptr;
}

// A program and a module whose cores hold named children with TDZ chains and rare data, symbol tables and, in the module,
// heap-allocated function declarations.
static constexpr ASCIILiteral payloadProgramText = "class JitcacheU7Payload { #secret = 1; static jitcacheU7Count = 0; reveal() { return this.#secret; } }\n"
    "{ let jitcacheU7Captured = 'jitcache-u7-captured'; globalThis.jitcacheU7Reader = () => jitcacheU7Captured; }\n"
    "function jitcacheU7Named(a, { b } = { }) { return a + b; }\n"_s;

// A core decoded from a payload whose CachedBytecode has a destructor, which the native decoder counts as owned and would
// leave parts in, decodes everything at once: the destructor runs before the decode returns, since nothing decoded keeps the
// decoder, no child UFE keeps its name in the payload, no symbol table its entries, a module decodes every function
// declaration, and the code block decodes its expression info.
static bool testCodecDecodesEverything(VM& vm, TestPart& part)
{
    ProgramFixture program = generateFixture(vm, String { payloadProgramText });
    SourceCode moduleSource = makeSource(String { moduleCorpusText }, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
    ParserError error;
    Strong<UnlinkedModuleProgramCodeBlock> module { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, moduleSource, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
    if (!part.check(program.codeBlock && module && module.get()->numberOfHeapAllocatedFunctionDecls(), "a payload fixture failed to generate, or the module has no heap-allocated function declaration"_s))
        return false;

    unsigned symbolTables = 0;
    unsigned namedChildren = 0;
    auto decodesEverything = [&](UnlinkedCodeBlock& generated, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, ASCIILiteral what) {
        auto core = encodeCoreBytes(vm, generated, nullptr);
        if (!part.check(!!core, makeString("the core of "_s, what, " failed to encode"_s)))
            return false;
        bool destroyed = false;
        Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytesNotingRelease(vm, core->span(), source, kind, destroyed) };
        if (!part.check(!!decoded, makeString("the core of "_s, what, " does not decode"_s)))
            return false;
        if (!part.check(destroyed, makeString("something decoded from the core of "_s, what, " keeps its payload after the decode returns"_s)))
            return false;
        UnlinkedCodeBlock& codeBlock = *decoded.get();
        bool keepsNothing = !!codeBlock.expressionInfoIfDecoded();
        auto childKeepsNothing = [&](UnlinkedFunctionExecutable* child) {
            if (!child || child->isCached())
                return false;
            if (child->ecmaNameWithoutGC().isNull())
                return true;
            ++namedChildren;
            return !!child->tryGetEcmaNameConcurrently();
        };
        for (unsigned i = 0; keepsNothing && i < codeBlock.numberOfFunctionDecls(); ++i)
            keepsNothing = childKeepsNothing(codeBlock.functionDeclIfDecoded(i));
        for (unsigned i = 0; keepsNothing && i < codeBlock.numberOfFunctionExprs(); ++i)
            keepsNothing = childKeepsNothing(codeBlock.functionExpr(i));
        for (uint32_t i = 0; keepsNothing && i < codeBlock.constantRegisters().size(); ++i) {
            if (auto* symbolTable = constantCell<SymbolTable>(codeBlock, i)) {
                ++symbolTables;
                keepsNothing = !symbolTable->hasCachedEntriesPending();
            }
        }
        if (auto* moduleBlock = dynamicDowncast<UnlinkedModuleProgramCodeBlock>(codeBlock))
            keepsNothing = keepsNothing && !moduleBlock->heapAllocatedFunctionDeclSlots()->hasDecodeSource();
        return part.check(keepsNothing, makeString("a part of "_s, what, " was left in its core's payload"_s));
    };
    if (!decodesEverything(*program.codeBlock.get(), program.source, UnlinkedCodeBlockCoreKind::Program, "the payload program"_s)
        || !decodesEverything(*module.get(), moduleSource, UnlinkedCodeBlockCoreKind::Module, "the corpus module"_s))
        return false;
    return part.check(symbolTables >= 2 && namedChildren >= 2, "the payload fixtures hold too few symbol tables or named children to show that nothing stays in a payload"_s);
}

static std::optional<uint32_t> regExpConstant(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<RegExp>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// A RegExp constant encodes the same whatever its shared cell's compiled state, and a core decodes to the cell the VM holds
// for its pattern and flags.
static bool testCodecRegExps(VM& vm, TestPart& part)
{
    JSGlobalObject* globalObject = vm.entryScope->globalObject();
    String text = "var jitcacheU7Pattern = /jitcache-u7-(a+)b/gi;\n"_s;
    ProgramFixture generated = generateFixture(vm, text);
    if (!part.check(!!generated.codeBlock, "the RegExp program failed to generate"_s))
        return false;
    auto index = regExpConstant(*generated.codeBlock.get());
    if (!part.check(!!index, "the RegExp program holds no RegExp constant"_s))
        return false;
    RegExp* regExp = constantCell<RegExp>(*generated.codeBlock.get(), *index);
    auto encoded = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-aab"_s, 0);
    auto compiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    vm.regExpCache()->deleteAllCode();
    auto cleared = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    regExp->match(globalObject, "jitcache-u7-ab"_s, 0);
    auto recompiled = encodeCoreBytes(vm, *generated.codeBlock.get(), nullptr);
    if (!part.check(encoded && encoded == compiled && encoded == cleared && encoded == recompiled, "a RegExp constant's encoding follows its cell's compiled state"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    ProgramFixture regenerated = generateFixture(vm, text);
    if (!part.check(regenerated.codeBlock && encodeCoreBytes(vm, *regenerated.codeBlock.get(), nullptr) == encoded, "a generation made while the shared cell's code was cleared encodes differently"_s))
        return false;
    vm.regExpCache()->deleteAllCode();
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, decodeCoreBytes(vm, encoded->span(), generated.source, UnlinkedCodeBlockCoreKind::Program, nullptr, failure) };
    bool sameCell = decoded && constantCell<RegExp>(*decoded.get(), *index) == regExp && encodeCoreBytes(vm, *decoded.get(), nullptr) == encoded;
    return part.check(sameCell, "a core's RegExp constant does not decode to the cell the VM holds with its code cleared, or does not re-encode to the core"_s);
}

// Map.prototype.forEach holds the VM's ordered-hash-table sentinel among its constants. Its core writes the sentinel's own
// kind and decodes to this VM's sentinel, re-encoding to the core's bytes, while the native builtin cache writes the cell as
// an immutable butterfly and decodes a new butterfly in its place.
static bool testCodecSentinel(VM& vm, TestPart& part)
{
    // A builtin root of its own over forEach's text alone, as Bun creates its internal modules: the native builtin cache
    // compares its payload with the provider's whole length (decodeBuiltinFunction).
    String text = vm.builtinExecutables()->mapPrototypeForEachCodeSource().view().toString();
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    Strong<UnlinkedFunctionExecutable> executable { vm, BuiltinExecutables::createExecutable(vm, source, Identifier::fromString(vm, "forEach"_s), ImplementationVisibility::Public,
        ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None, NeedsClassFieldInitializer::No) };
    if (!part.check(!!executable, "Map.prototype.forEach's text does not make a builtin root"_s))
        return false;
    ParserError error;
    Strong<UnlinkedFunctionCodeBlock> body { vm, executable.get()->unlinkedCodeBlockFor(vm, executable.get()->linkedSourceCode(source), CodeSpecializationKind::CodeForCall, { }, error, executable.get()->parseMode()) };
    JSCell* sentinel = vm.orderedHashTableSentinel();
    std::optional<uint32_t> index;
    for (uint32_t i = 0; body && !index && i < body.get()->constantRegisters().size(); ++i) {
        if (body.get()->constantRegisters()[i].get() == JSValue(sentinel))
            index = i;
    }
    if (!part.check(!!index, "Map.prototype.forEach's body holds no ordered-hash-table sentinel"_s))
        return false;

    auto core = encodeCoreBytes(vm, *body.get(), executable.get());
    CoreDecodeFailure failure = CoreDecodeFailure::None;
    Strong<UnlinkedCodeBlock> decoded { vm, core ? decodeCoreBytes(vm, core->span(), executable.get()->linkedSourceCode(source), UnlinkedCodeBlockCoreKind::Function, executable.get(), failure) : nullptr };
    bool keepsSentinel = decoded && decoded.get()->constantRegisters()[*index].get() == JSValue(sentinel) && encodeCoreBytes(vm, *decoded.get(), executable.get()) == core;
    if (!part.check(keepsSentinel, "Map.prototype.forEach's core does not decode to this VM's sentinel, or does not re-encode to its bytes"_s))
        return false;

    // The native builtin cache: the encode holds the body the call slot keeps, and the decoded root decodes it at its first
    // request.
    RefPtr<CachedBytecode> native = encodeBuiltinFunction(vm, executable.get(), source, 0);
    Strong<UnlinkedFunctionExecutable> nativeExecutable { vm, native ? decodeBuiltinFunction(vm, Ref<CachedBytecode> { *native }, *source.provider(), 0) : nullptr };
    Strong<UnlinkedFunctionCodeBlock> nativeBody { vm, nativeExecutable ? decodedBody(vm, *nativeExecutable.get(), source) : nullptr };
    JSValue nativeConstant = nativeBody && *index < nativeBody.get()->constantRegisters().size() ? nativeBody.get()->constantRegisters()[*index].get() : JSValue();
    auto* nativeButterfly = nativeConstant ? dynamicDowncast<JSCellButterfly>(nativeConstant) : nullptr;
    return part.check(nativeButterfly && nativeButterfly != sentinel && !nativeButterfly->length(), "the native builtin cache does not write the sentinel as an immutable butterfly"_s);
}

} // namespace UCBCodecCellTestsInternal

using namespace UCBCodecCellTestsInternal;

JITCACHE_TEST(ucbCodecChunksAndBudgets, Yes)
{
    TestPart part { context };
    testCodecChunksAndBudgets(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecAllocatesNoCell, Yes)
{
    TestPart part { context };
    testCodecAllocatesNoCell(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecStringConstants, Yes)
{
    TestPart part { context };
    testCodecStringConstants(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecDecodesEverything, Yes)
{
    TestPart part { context };
    testCodecDecodesEverything(*context.vm(), part);
}

JITCACHE_TEST(ucbCodecRegExpConstants, Yes)
{
    VM& vm = *context.vm();
    EnteredGlobalObject entered { vm };
    TestPart part { context };
    testCodecRegExps(vm, part);
}

JITCACHE_TEST(ucbCodecOrderedHashTableSentinel, Yes)
{
    TestPart part { context };
    testCodecSentinel(*context.vm(), part);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
