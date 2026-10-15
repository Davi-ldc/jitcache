#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "BuiltinExecutables.h"
#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "DirectEvalExecutable.h"
#include "JITCacheAPI.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGlobalObject.h"
#include "ParserError.h"
#include "ProgramExecutable.h"
#include "SourceCode.h"
#include "SourceCodeKey.h"
#include "SourceProvider.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "TestArtifact.h"
#include "TestFiles.h"
#include "TestVM.h"
#include "UCBKeys.h"
#include "UCBRegistry.h"
#include "UCBRequests.h"
#include "UCBSections.h"
#include "UCBTwinGeneration.h"
#include "UCBTwins.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "VMEntryScopeInlines.h"
#include "VariableEnvironmentInlines.h"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>
#include <source_location>
#include <span>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// What the UCB lane's tests share. TestPart carries a test's checks into the helpers that make them; a test enters a global
// object of its own and starts JITCache on its VM in the role it needs, and a test that started JITCache ends with the
// registry check. The rest are the fixtures several test files build: programs generated from text and the constants they
// hold, registry records, the identity section's offsets, providers that supply a digest, TDZ sets, function bodies and
// cores, and pairs of UFEs that differ in one field a holder digest and a descriptor cover.

namespace JSC::JITCache::Tests {

// The checks of one test, which its helpers take: check records a failed condition in the test's context, with the
// description and the line of the call, and returns the condition, so a helper returns at its first failure.
class TestPart {
    WTF_MAKE_NONCOPYABLE(TestPart);
public:
    explicit TestPart(TestContext& context)
        : m_context(context)
    {
    }

    bool check(bool condition, ASCIILiteral description, std::source_location location = std::source_location::current())
    {
        if (!condition)
            m_context.fail(location.file_name(), static_cast<int>(location.line()), description);
        return condition;
    }

    bool check(bool condition, const String& description, std::source_location location = std::source_location::current())
    {
        if (!condition)
            m_context.fail(location.file_name(), static_cast<int>(location.line()), description);
        return condition;
    }

private:
    TestContext& m_context;
};

// A global object of the test's own, entered as a host function's caller enters it, for the helpers that create
// executables in a realm or read their global object through vm.entryScope. The object lives on the test's stack, which
// keeps the global object alive.
class EnteredGlobalObject {
    WTF_MAKE_NONCOPYABLE(EnteredGlobalObject);
public:
    explicit EnteredGlobalObject(VM& vm)
        : m_globalObject(JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull())))
        , m_entryScope(vm, m_globalObject)
    {
    }

    JSGlobalObject* globalObject() const { return m_globalObject; }

private:
    JSGlobalObject* const m_globalObject;
    VMEntryScope m_entryScope;
};

// Starts JITCache on the test's VM in `role`, strict or not, with the delta capture All, on an artifact in `directory`, and
// with a twin report at `twinReportPath` unless it is null. A Consumer and a ConsumerProducer open an artifact that exists,
// so for them a Producer on a VM of its own creates one first, and that VM goes before the test's VM starts, releasing the
// producer lock a ConsumerProducer takes. Fails the test unless every start returns Started.
inline bool startJITCache(TestContext& context, VM& vm, const TemporaryDirectory& directory, Role role, bool strict, const String& twinReportPath = { })
{
    String artifact = directory.path("artifact"_s);
    if (role != Role::Producer) {
        ExtraVM creator;
        if (!startsAs(context, "the Producer that creates the artifact"_s, JITCache::start(creator.vm(), strictConfig(artifact, Role::Producer, DeltaCapture::All)), StartOutcome::Started))
            return false;
    }
    Config config = strictConfig(artifact, role, DeltaCapture::All);
    config.strict = strict;
    config.twinReportPath = twinReportPath;
    return startsAs(context, "the test's VM"_s, JITCache::start(vm, config), StartOutcome::Started);
}

// How a test that started JITCache ends: after a full collection, the registry holds no entry of a dead cell, and each
// child UFE of a recorded UCB has the identity its parent's record gives it. verifyRegistry also reports each violation to
// the VM's twin report when one is open.
inline void checkRegistry(TestContext& context, VM& vm)
{
    VMState* state = vm.jitCacheState();
    unsigned violations = verifyRegistry(vm, state ? state->twinReportSink() : nullptr);
    if (violations)
        JITCACHE_FAIL(makeString("verifyRegistry found "_s, violations, " violations"_s));
}

// A record under `key` with fixed program context inputs, no stamp and no pending import.
inline UCBRecord testRecord(const BodyKey& key, UCBOrigin origin, RefPtr<SourceProvider>&& suppliedDigestProvider = nullptr, UnlinkedFunctionExecutable* holder = nullptr)
{
    UCBRecord record {
        key,
        GlobalContextInputs { 0, 1, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false },
        origin,
        NoLexicallyScopedFeatures,
        std::nullopt,
        std::nullopt,
        nullptr,
        WTF::move(suppliedDigestProvider),
        std::nullopt,
    };
    record.holder = holder;
    return record;
}

// Calls functor(child, table, index) for each child UFE in table order until it returns false.
template<typename Functor>
bool allChildren(UnlinkedCodeBlock& codeBlock, const Functor& functor)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i) {
        if (!functor(*codeBlock.functionDecl(i), ChildTable::Declarations, i))
            return false;
    }
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i) {
        if (!functor(*codeBlock.functionExpr(i), ChildTable::Expressions, i))
            return false;
    }
    return true;
}

// The identity section's layout, which the tests read at these offsets. They are written out apart from the lane's own
// constants, so that a writer or a parser that moves a field fails the tests.
inline constexpr uint32_t identitySectionMagic = 0x49424355;
inline constexpr size_t identityVersionOffset = 4;
inline constexpr size_t identityCoreKindOffset = 6;
inline constexpr size_t identityProvenanceOffset = 7;
inline constexpr size_t identityKeyOffset = 8;
inline constexpr size_t identityContextDigestOffset = 48;
inline constexpr size_t identityCoreDigestOffset = 80;
inline constexpr size_t identityHolderDigestOffset = 112;
inline constexpr size_t identityFeaturesOffset = 144;
inline constexpr size_t identityLexicallyScopedFeaturesOffset = 146;
inline constexpr size_t identityHasCapturedVariablesOffset = 147;
inline constexpr size_t identityConstantCountOffset = 148;
inline constexpr size_t identityMapsOffset = 152;

// The smallest count of the identity and feedback sections that strict parsing refuses.
inline constexpr uint32_t sectionCountLimit = 1u << 28;

inline void setBit(std::span<uint8_t> map, uint32_t index)
{
    map[index / 8] |= 1u << (index % 8);
}

inline bool bitIsSet(std::span<const uint8_t> map, uint32_t index)
{
    return (map[index / 8] >> (index % 8)) & 1;
}

inline Digest256 filledDigest(uint8_t fill)
{
    Digest256 digest;
    digest.fill(fill);
    return digest;
}

inline VirtualRegister constantRegisterFor(uint32_t index)
{
    return VirtualRegister(VirtualRegister::firstConstantRegisterIndex + static_cast<int>(index));
}

// A constant register as a cell of class T, or null. An empty constant, which generation emits for TDZ checks, is no cell.
template<typename T>
T* constantCell(UnlinkedCodeBlock& codeBlock, uint32_t index)
{
    JSValue value = codeBlock.constantRegisters()[index].get();
    return value ? dynamicDowncast<T>(value) : nullptr;
}

inline bool isStringWithContent(JSValue value, ASCIILiteral content)
{
    auto* string = value ? dynamicDowncast<JSString>(value) : nullptr;
    StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
    return impl && String { impl } == content;
}

inline std::optional<uint32_t> stringConstantIndex(UnlinkedCodeBlock& codeBlock, ASCIILiteral content)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (isStringWithContent(codeBlock.constantRegisters()[i].get(), content))
            return i;
    }
    return std::nullopt;
}

inline std::optional<uint32_t> symbolTableConstantIndex(UnlinkedCodeBlock& codeBlock)
{
    for (uint32_t i = 0; i < codeBlock.constantRegisters().size(); ++i) {
        if (constantCell<SymbolTable>(codeBlock, i))
            return i;
    }
    return std::nullopt;
}

// A program generated with its child UFEs and none of their bodies, with the source that a decode of its core needs as
// its provider. Depth 0 reaches no request point, so the VM's registry records nothing, and the Strong keeps the cells
// alive.
struct ProgramFixture {
    String text;
    SourceCode source;
    Strong<UnlinkedProgramCodeBlock> codeBlock;
};

inline ProgramFixture generateFixture(VM& vm, const String& text)
{
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    UnlinkedProgramCodeBlock* codeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0);
    return ProgramFixture { text, WTF::move(source), Strong<UnlinkedProgramCodeBlock> { vm, codeBlock } };
}

// A program's generation with its children generated to `depth` levels (0: none), as generateFixture makes one.
inline ProgramFixture generateProgramFixture(VM& vm, const String& text, unsigned depth, LexicallyScopedFeatures features = NoLexicallyScopedFeatures, OptionSet<CodeGenerationMode> mode = { })
{
    SourceCode source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    UnlinkedProgramCodeBlock* codeBlock = recursivelyGenerateUnlinkedCodeBlockForProgram(vm, source, features, JSParserScriptMode::Classic, mode, error, EvalContextType::None, depth);
    return ProgramFixture { text, WTF::move(source), Strong<UnlinkedProgramCodeBlock> { vm, codeBlock } };
}

// A program whose UCB holds what the section tests read: string constants, an all-string literal in the atom form, a mixed
// literal and an all-string prefix in the plain form, value, array and both kinds of arithmetic profiles, function
// declarations and expressions, and the SymbolTable of a block scope that a closure captures. ArrayNode::emitBytecode
// builds the prefix butterfly before a spread only when the literal also has an elision; a spread without one emits
// new_array_with_spread, element by element, and leaves no butterfly constant.
inline constexpr ASCIILiteral sectionFixtureSource = "var words = [\"alpha\", \"beta\", \"gamma\"];\n"
    "var mixed = [\"alpha\", 1];\n"
    "var prefix = [\"delta\", \"epsilon\", , ...words];\n"
    "var plain = \"jitcache-u5-plain-constant\";\n"
    "var atom = \"jitcache-u5-atom-constant\";\n"
    "var unmarked = \"jitcache-u5-unmarked-constant\";\n"
    "var sum = words.length + mixed[1];\n"
    "var negated = -sum;\n"
    "function declared() { return 1; }\n"
    "(function expressed() { return 2; });\n"
    "{ let captured = 1; (function closure() { return captured; }); }\n"_s;

// The map checks set the bit at N, which lies inside the maps' last byte only when N % 8 != 0, so the fixture program takes
// one more string constant per try until its constant count is not a multiple of eight.
inline std::optional<ProgramFixture> generateSectionFixture(VM& vm)
{
    StringBuilder text;
    text.append(sectionFixtureSource);
    for (unsigned padding = 0; padding < 8; ++padding) {
        ProgramFixture fixture = generateFixture(vm, text.toString());
        if (!fixture.codeBlock)
            return std::nullopt;
        if (fixture.codeBlock.get()->constantRegisters().size() % 8)
            return fixture;
        text.append("var padding"_s, padding, " = \"jitcache-u5-padding-"_s, padding, "\";\n"_s);
    }
    return std::nullopt;
}

// Copies `valid`, breaks one rule in the copy, and checks that strict parsing rejects it while parsing with strict off,
// which reads each field where the layout puts it and trusts it, still answers when the fixed layout stays readable.
template<typename Parse, typename Mutate>
bool strictRejects(TestPart& part, ASCIILiteral sectionName, const Parse& parse, const Vector<uint8_t>& valid, ASCIILiteral rule, bool keepsLayout, const Mutate& mutate)
{
    Vector<uint8_t> crafted = valid;
    mutate(crafted);
    if (!part.check(!parse(crafted.span(), true), makeString("strict "_s, sectionName, " parsing accepts "_s, rule)))
        return false;
    return part.check(!keepsLayout || !!parse(crafted.span(), false), makeString(sectionName, " parsing with strict off refuses "_s, rule));
}

// A provider that supplies a digest for its text, as Bun's providers for compiled modules do. The digest is whatever the
// test gives, so a test can supply one that differs from the text.
class SuppliedDigestProvider final : public StringSourceProvider {
public:
    static Ref<SuppliedDigestProvider> create(const String& text, const std::optional<Digest256>& digest)
    {
        return adoptRef(*new SuppliedDigestProvider(text, digest));
    }

    std::optional<std::array<uint8_t, 32>> jitCacheSourceDigest() const final { return m_digest; }

private:
    SuppliedDigestProvider(const String& text, const std::optional<Digest256>& digest)
        : StringSourceProvider(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Program)
        , m_digest(digest)
    {
    }

    const std::optional<Digest256> m_digest;
};

inline TDZEnvironment tdzNames(VM& vm, std::initializer_list<ASCIILiteral> names)
{
    TDZEnvironment environment;
    for (ASCIILiteral name : names)
        environment.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() });
    return environment;
}

inline TDZEnvironment tdzNames(VM& vm, const Vector<String>& names)
{
    TDZEnvironment environment;
    for (const String& name : names)
        environment.add(RefPtr<UniquedStringImpl> { Identifier::fromString(vm, name).impl() });
    return environment;
}

// What a UFE body's native generation makes, without touching the UFE: its slots stay as they are and its parse results go
// to `parseResults`. The request object is only the twin generator's input.
inline Strong<UnlinkedFunctionCodeBlock> generateBodyOf(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& source, CodeSpecializationKind kind, TwinParseResults* parseResults = nullptr)
{
    RequestState request(vm, RequestKind::FunctionBody, source, { }, NoLexicallyScopedFeatures);
    request.functionExecutable = &executable;
    request.specialization = kind;
    TwinParseResults results;
    ParserError error;
    Strong<UnlinkedFunctionCodeBlock> codeBlock { vm, generateFunctionBodyTwin(request, results, error) };
    if (parseResults)
        *parseResults = results;
    return codeBlock;
}

inline void prepareProgramRequest(RequestState& request, ProgramExecutable& executable)
{
    request.globalExecutable = &executable;
    request.scriptMode = JSParserScriptMode::Classic;
    request.evalContextType = EvalContextType::None;
}

inline std::optional<Vector<uint8_t>> encodeCoreBytes(VM& vm, const UnlinkedCodeBlock& codeBlock, const UnlinkedFunctionExecutable* holder)
{
    CoreEncodeFailure failure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> core = encodeUnlinkedCodeBlockCore(vm, codeBlock, holder, nullptr, failure);
    if (!core)
        return std::nullopt;
    return Vector<uint8_t> { core->span() };
}

inline SourceCodeKey programCacheKey(const SourceCode& source)
{
    return SourceCodeKey { source, String { }, SourceCodeType::ProgramType, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false, { }, std::nullopt };
}

// The function body a natively decoded program's lazily cached slot decodes to, at its first request.
inline UnlinkedFunctionCodeBlock* decodedBody(VM& vm, UnlinkedFunctionExecutable& executable, const SourceCode& parentSource)
{
    ParserError error;
    CodeSpecializationKind kind = executable.isClassConstructorFunction() ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
    return executable.unlinkedCodeBlockFor(vm, executable.linkedSourceCode(parentSource), kind, { }, error, executable.parseMode());
}

// A Function-constructor root, whose UFE has no TDZ chain (CodeCache::getUnlinkedGlobalFunctionExecutable creates it with none).
inline Strong<UnlinkedFunctionExecutable> functionConstructorRoot(VM& vm, const String& body, SourceCode& source)
{
    String text = makeString("function anonymous(a\n) {\n"_s, body, "\n}"_s);
    source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
    ParserError error;
    return Strong<UnlinkedFunctionExecutable> { vm, vm.codeCache()->getUnlinkedGlobalFunctionExecutable(vm, Identifier::fromString(vm, "anonymous"_s), source, NoLexicallyScopedFeatures, { }, 22, error) };
}

// The first child UFE, declarations before expressions, that `select` accepts.
inline UnlinkedFunctionExecutable* selectChild(UnlinkedCodeBlock& codeBlock, const Function<bool(UnlinkedFunctionExecutable&)>& select)
{
    for (unsigned i = 0; i < codeBlock.numberOfFunctionDecls(); ++i) {
        if (select(*codeBlock.functionDecl(i)))
            return codeBlock.functionDecl(i);
    }
    for (unsigned i = 0; i < codeBlock.numberOfFunctionExprs(); ++i) {
        if (select(*codeBlock.functionExpr(i)))
            return codeBlock.functionExpr(i);
    }
    return nullptr;
}

// One step down to a variant's UFE: the first child UFE that the step accepts, of the UCB reached so far.
enum class HolderStep : uint8_t {
    End,
    Any,
    FieldInitializer, // the class field initializer, which holds the class element definitions
    DefaultConstructor, // the default constructor, which holds the class source range
    ClassConstructor, // a constructor the class declares
    MethodNamedM,
};

inline bool acceptsStep(HolderStep step, UnlinkedFunctionExecutable& executable)
{
    switch (step) {
    case HolderStep::End:
        return false;
    case HolderStep::Any:
        return true;
    case HolderStep::FieldInitializer:
        return executable.parseMode() == SourceParseMode::ClassFieldInitializerMode;
    case HolderStep::DefaultConstructor:
        return executable.isBuiltinDefaultClassConstructor();
    case HolderStep::ClassConstructor:
        return executable.isClassConstructorFunction() && !executable.isBuiltinDefaultClassConstructor();
    case HolderStep::MethodNamedM:
        return executable.name().string() == "m"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

// Where a side of a variant starts. A program or a module is generated from the text, and the path starts at its UCB. A
// builtin root is made from the text, which reads "(function (...) { ... })", by BuiltinExecutables::createExecutable with
// the side's four builtin flags and no name, and the path starts at that root. A direct eval of the text is generated
// under the side's eval context type, with empty TDZ and private-name sets, and the path starts at its UCB. Each step
// after a UFE has been reached generates that UFE's body and looks among its children.
enum class HolderOrigin : uint8_t { Program, Module, Builtin, DirectEval };

struct HolderSide {
    ASCIILiteral text;
    HolderOrigin origin { HolderOrigin::Program };
    std::array<HolderStep, 2> path { HolderStep::Any, HolderStep::End };
    ImplementationVisibility implementationVisibility { ImplementationVisibility::Public }; // Builtin
    ConstructAbility constructAbility { ConstructAbility::CannotConstruct }; // Builtin
    InlineAttribute inlineAttribute { InlineAttribute::None }; // Builtin
    PrivateBrandRequirement privateBrandRequirement { PrivateBrandRequirement::None }; // Builtin
    EvalContextType evalContextType { EvalContextType::FunctionEvalContext }; // DirectEval
    // A TDZ variant's chain, one letter per link, the UFE's own link first: the one name among a, b and c that the link's
    // environment holds. Empty for the variants that leave the chain as generation makes it.
    ASCIILiteral tdzLinks { };
};

// The scalar flags the descriptor holds for a UFE, which CachedFunctionExecutable::packScalars writes, in the order
// descriptorFlags reads them through the UFE's accessors.
enum class DescriptorFlag : uint8_t {
    ScriptMode,
    SuperBinding,
    ConstructAbility,
    HasName,
    ConstructorKind,
    FunctionMode,
    ImplementationVisibility,
    DerivedContextType,
    EvalContextType,
    PrivateBrandRequirement,
    InlineAttribute,
    NeedsClassFieldInitializer,
    IsBuiltinFunction,
    IsBuiltinDefaultClassConstructor,
};
inline constexpr size_t descriptorFlagCount = static_cast<size_t>(DescriptorFlag::IsBuiltinDefaultClassConstructor) + 1;

// Two UFEs that differ in what `field` names. `descriptorDiffers` and `chainDiffers` say whether their descriptors and
// their TDZ chains differ; the holder digest covers both, so it differs exactly when one of them does. A flag variant names
// the one flag it changes, and generateHolderPair checks that its two UFEs agree on every other flag, every position and
// every rare field the descriptor holds, and, unless chainDiffers, on the chain digest. Only then can a difference in the
// digest or the descriptor come from the flag alone.
//
// Every flag has a variant but IsBuiltinDefaultClassConstructor: only BuiltinExecutables makes a default constructor, from a
// constructor kind other than None, which also makes it a normal function rather than a builtin's, so that flag never
// moves alone. The constructor kind and super binding variants keep the other one fixed: a constructor that extends
// another always needs a super binding, so the base class's constructor uses super too.
//
// The TDZ variants create their function before the blocks' `let`s run. A block's link is made, and cached, when the first
// function inside the block is created (BytecodeGenerator::getVariablesUnderTDZ), and holds only the names still under TDZ
// then; a `let` that has run is lifted out (EmptyLetExpression::emitBytecode, liftTDZCheckIfPossible), so
// `{ let a; (function f() { }); }` gives f a link over no name at all. A program pushes no TDZ entry of its own, so each
// block that declares a name is one link. The holder digest test checks each side's chain against its tdzLinks before it
// compares the digests.
struct HolderVariant {
    ASCIILiteral field;
    HolderSide base;
    HolderSide variant;
    bool descriptorDiffers;
    bool chainDiffers;
    std::optional<DescriptorFlag> flag;
};

inline constexpr ASCIILiteral builtinHolderText = "(function (a) { return a; })"_s;
inline constexpr ASCIILiteral nestingHolderText = "(function (a) { return function () { return a; }; })"_s;

inline constexpr std::array<HolderVariant, 26> holderVariants { {
    { "the name"_s, { "function f(a) { return a; }"_s }, { "function g(a) { return a; }"_s }, true, false, std::nullopt },
    { "the parameter count"_s, { "function f(a,b){return a;}"_s }, { "function f(ab) {return a;}"_s }, true, false, std::nullopt },
    { "the parse mode"_s, { "function f(a) { return a; }"_s }, { "function*f(a) { return a; }"_s }, true, false, std::nullopt },
    { "the lexically scoped features"_s, { "function f(a) { 'use strict';  }"_s }, { "function f(a) { 'use strict!'; }"_s }, true, false, std::nullopt },
    { "the source position"_s, { "function f(a) { return a; }"_s }, { " function f(a) { return a; }"_s }, true, false, std::nullopt },
    { "the class element definitions"_s, { "class C { x = 1; }"_s, HolderOrigin::Program, { HolderStep::FieldInitializer } },
        { "class C { y = 1; }"_s, HolderOrigin::Program, { HolderStep::FieldInitializer } }, true, false, std::nullopt },
    { "the class source range"_s, { "class C { }"_s, HolderOrigin::Program, { HolderStep::DefaultConstructor } },
        { "class C {  }"_s, HolderOrigin::Program, { HolderStep::DefaultConstructor } }, true, false, std::nullopt },
    { "the parent's private names"_s, { "class C { #x; m() { return this.#x; } }"_s, HolderOrigin::Program, { HolderStep::MethodNamedM } },
        { "class C { #y; m() { return this.#y; } }"_s, HolderOrigin::Program, { HolderStep::MethodNamedM } }, true, false, std::nullopt },
    // The first function of the async function's body, which holds the wrapper's parameter names.
    { "the async wrapper's parameter names"_s, { "async function f(a) { await a; }"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } },
        { "async function f(b) { await b; }"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } }, true, false, std::nullopt },
    { "a TDZ name"_s, { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s },
        { .text = "{ (function f() { }); let b; }"_s, .tdzLinks = "b"_s }, false, true, std::nullopt },
    // The first links are over one environment, so only the second link's digest can set the two chains apart.
    { "a TDZ name in a parent link"_s, { .text = "{ { (function f() { }); let c; } let a; }"_s, .tdzLinks = "ca"_s },
        { .text = "{ { (function f() { }); let c; } let b; }"_s, .tdzLinks = "cb"_s }, false, true, std::nullopt },
    { "the order of the TDZ links"_s, { .text = "{ { (function f() { }); let b; } let a; }"_s, .tdzLinks = "ba"_s },
        { .text = "{ { (function f() { }); let a; } let b; }"_s, .tdzLinks = "ab"_s }, false, true, std::nullopt },
    { "nothing: distinct links over the same environments"_s, { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s },
        { .text = "{ (function f() { }); let a; }"_s, .tdzLinks = "a"_s }, false, false, std::nullopt },
    // The flags. Each pair of texts has one length and puts its UFE at one offset, so positions agree.
    { "the function mode"_s, { "  function f(a) { return a; }"_s }, { "x=function f(a) { return a; }"_s }, true, false, DescriptorFlag::FunctionMode },
    // Anonymous, the function still takes the binding's name as its ecmaName, as the named one has it.
    { "whether the function names itself"_s, { "var f = function  (a) { return a; };"_s }, { "var f = function f(a) { return a; };"_s }, true, false, DescriptorFlag::HasName },
    { "the constructor kind"_s, { "class A           { constructor(a) { super.x; } }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } },
        { "class A extends B { constructor(a) { super.x; } }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } }, true, false, DescriptorFlag::ConstructorKind },
    { "the super binding"_s, { "({ m() { return super.x; } });"_s }, { "({ m() { return this.x;  } });"_s }, true, false, DescriptorFlag::SuperBinding },
    // The arrow inside the method. Generation gives it DerivedMethodContext when the method's own super binding makes the
    // method's body a class context (generateUnlinkedFunctionCodeBlock, BytecodeGenerator::makeFunction), so the method's
    // use of super sets it. An extends clause alone changes neither a method that never uses super nor its arrow.
    { "the derived context type"_s, { "({ m() { super.x; return () => 1; } });"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } },
        { "({ m() { this.x;  return () => 1; } });"_s, HolderOrigin::Program, { HolderStep::Any, HolderStep::Any } }, true, false, DescriptorFlag::DerivedContextType },
    // An arrow takes the eval context type of the code it is generated in, here the eval's own.
    { "the eval context type"_s, { .text = "() => 1;"_s, .origin = HolderOrigin::DirectEval, .evalContextType = EvalContextType::FunctionEvalContext },
        { .text = "() => 1;"_s, .origin = HolderOrigin::DirectEval, .evalContextType = EvalContextType::InstanceFieldEvalContext }, true, false, DescriptorFlag::EvalContextType },
    { "the class field initializer flag"_s, { "class A { constructor() { } x;    }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } },
        { "class A { constructor() { } x(){} }"_s, HolderOrigin::Program, { HolderStep::ClassConstructor } }, true, false, DescriptorFlag::NeedsClassFieldInitializer },
    // A strict program's block function against a module's: both strict, and both chains one link over an empty environment,
    // since a block that declares only a function pushes a TDZ entry whose names need none.
    { "the script mode"_s, { "'use strict';{ function f(a) { return a; } }"_s },
        { "             { function f(a) { return a; } }"_s, HolderOrigin::Module }, true, false, DescriptorFlag::ScriptMode },
    { "the implementation visibility"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .implementationVisibility = ImplementationVisibility::Private }, true, false, DescriptorFlag::ImplementationVisibility },
    { "the construct ability"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .constructAbility = ConstructAbility::CanConstruct }, true, false, DescriptorFlag::ConstructAbility },
    { "the inline attribute"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .inlineAttribute = InlineAttribute::Always }, true, false, DescriptorFlag::InlineAttribute },
    { "the private brand requirement"_s, { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { } },
        { .text = builtinHolderText, .origin = HolderOrigin::Builtin, .path = { }, .privateBrandRequirement = PrivateBrandRequirement::Needed }, true, false, DescriptorFlag::PrivateBrandRequirement },
    // The inner function of one text, generated inside a builtin root's body and inside a program's function. Debug builds
    // check that a builtin root's metadata is what the parser gives the same text (BuiltinExecutables::createExecutable), so
    // the two bodies parse the same source and place the inner function alike. Both outer functions are anonymous:
    // createExecutable makes every builtin root a function expression, and a named one's body pushes a scope for its own
    // name (BytecodeGenerator::emitPushFunctionNameScope) whose TDZ entry gives the inner function a chain link over an
    // empty environment, which the program side lacks.
    { "whether the function is a builtin's"_s, { .text = nestingHolderText, .origin = HolderOrigin::Builtin, .path = { HolderStep::Any } },
        { .text = nestingHolderText, .origin = HolderOrigin::Program, .path = { HolderStep::Any, HolderStep::Any } }, true, false, DescriptorFlag::IsBuiltinFunction },
} };

inline std::array<unsigned, descriptorFlagCount> descriptorFlags(const UnlinkedFunctionExecutable& executable)
{
    return { {
        static_cast<unsigned>(executable.scriptMode()),
        static_cast<unsigned>(executable.superBinding()),
        static_cast<unsigned>(executable.constructAbility()),
        static_cast<unsigned>(!executable.name().isNull()), // name() is the ecmaName exactly when the UFE has its own name
        static_cast<unsigned>(executable.constructorKind()),
        static_cast<unsigned>(executable.functionMode()),
        static_cast<unsigned>(executable.implementationVisibility()),
        static_cast<unsigned>(executable.derivedContextType()),
        static_cast<unsigned>(executable.evalContextType()),
        static_cast<unsigned>(executable.privateBrandRequirement()),
        static_cast<unsigned>(executable.inlineAttribute()),
        static_cast<unsigned>(executable.needsClassFieldInitializer()),
        static_cast<unsigned>(executable.isBuiltinFunction()),
        static_cast<unsigned>(executable.isBuiltinDefaultClassConstructor()),
    } };
}

inline Vector<std::pair<String, uint16_t>> parentPrivateNamesOf(const UnlinkedFunctionExecutable& executable)
{
    Vector<std::pair<String, uint16_t>> names;
    if (auto* environment = executable.parentPrivateNameEnvironment()) {
        for (const auto& entry : *environment)
            names.append({ String { entry.key.get() }, entry.value.bits() });
    }
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
        return codePointCompareLessThan(a.first, b.first);
    });
    return names;
}

// Whether two UFEs differ, among the flags, positions and rare fields the descriptor holds (CachedFunctionExecutable's
// header, scalars and rare data), in `flag` alone, and, when `sameChain`, have chains with one digest.
inline bool differsOnlyInFlag(UnlinkedFunctionExecutable& a, UnlinkedFunctionExecutable& b, DescriptorFlag flag, bool sameChain)
{
    auto flagsOfA = descriptorFlags(a);
    auto flagsOfB = descriptorFlags(b);
    for (size_t i = 0; i < descriptorFlagCount; ++i) {
        if ((flagsOfA[i] != flagsOfB[i]) != (i == static_cast<size_t>(flag)))
            return false;
    }
    bool samePositions = a.startOffset() == b.startOffset() && a.sourceLength() == b.sourceLength() && a.unlinkedFunctionStart() == b.unlinkedFunctionStart()
        && a.unlinkedFunctionEnd() == b.unlinkedFunctionEnd() && a.parametersStartOffset() == b.parametersStartOffset()
        && a.unlinkedBodyStartColumn() == b.unlinkedBodyStartColumn() && a.unlinkedBodyEndColumn() == b.unlinkedBodyEndColumn() && a.lineCount() == b.lineCount();
    bool sameScalars = a.ecmaName() == b.ecmaName() && a.parameterCount() == b.parameterCount() && a.parseMode() == b.parseMode()
        && a.lexicallyScopedFeatures() == b.lexicallyScopedFeatures();
    auto sizeOf = [](const auto* vector) -> size_t {
        return vector ? vector->size() : 0;
    };
    SourceCode classOfA = a.classSource();
    SourceCode classOfB = b.classSource();
    bool sameRareFields = classOfA.startOffset() == classOfB.startOffset() && classOfA.endOffset() == classOfB.endOffset()
        && parentPrivateNamesOf(a) == parentPrivateNamesOf(b)
        && sizeOf(a.generatorOrAsyncWrapperFunctionParameterNames()) == sizeOf(b.generatorOrAsyncWrapperFunctionParameterNames())
        && sizeOf(a.classElementDefinitions()) == sizeOf(b.classElementDefinitions());
    if (!samePositions || !sameScalars || !sameRareFields)
        return false;
    if (!sameChain)
        return true;
    unsigned digested = 0;
    auto chainOfA = tdzChainDigest(a.parentScopeTDZVariables().get(), nullptr, digested);
    auto chainOfB = tdzChainDigest(b.parentScopeTDZVariables().get(), nullptr, digested);
    return chainOfA && chainOfA == chainOfB;
}

// A side of a variant, generated, with every cell on the way to its UFE held.
struct HolderFixture {
    SourceCode source;
    Strong<UnlinkedCodeBlock> codeBlock;
    Strong<UnlinkedFunctionExecutable> root;
    Vector<Strong<UnlinkedFunctionCodeBlock>> bodies;
    UnlinkedFunctionExecutable* executable { nullptr }; // null when the side failed to generate or its path found nothing
};

inline HolderFixture generateHolderSide(VM& vm, JSGlobalObject* globalObject, const HolderSide& side)
{
    HolderFixture fixture;
    String text { side.text };
    ParserError error;
    switch (side.origin) {
    case HolderOrigin::Program:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForProgram(vm, fixture.source, NoLexicallyScopedFeatures, JSParserScriptMode::Classic, { }, error, EvalContextType::None, 0) };
        break;
    case HolderOrigin::Module:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted, String { }, TextPosition { }, SourceProviderSourceType::Module);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, recursivelyGenerateUnlinkedCodeBlockForModuleProgram(vm, fixture.source, StrictModeLexicallyScopedFeature, JSParserScriptMode::Module, { }, error, EvalContextType::None, 0) };
        break;
    case HolderOrigin::Builtin:
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        fixture.root = Strong<UnlinkedFunctionExecutable> { vm, BuiltinExecutables::createExecutable(vm, fixture.source, Identifier { }, side.implementationVisibility,
            ConstructorKind::None, side.constructAbility, side.inlineAttribute, NeedsClassFieldInitializer::No, side.privateBrandRequirement) };
        break;
    case HolderOrigin::DirectEval: {
        fixture.source = makeSource(text, SourceOrigin { }, SourceTaintedOrigin::Untainted);
        TDZEnvironment variablesUnderTDZ;
        PrivateNameEnvironment privateNames;
        DirectEvalExecutable* executable = DirectEvalExecutable::create(globalObject, fixture.source, NoLexicallyScopedFeatures, DerivedContextType::None, NeedsClassFieldInitializer::No,
            PrivateBrandRequirement::None, false, false, side.evalContextType, &variablesUnderTDZ, &privateNames);
        fixture.codeBlock = Strong<UnlinkedCodeBlock> { vm, executable ? executable->unlinkedCodeBlock() : nullptr };
        break;
    }
    }

    SourceCode source = fixture.source;
    UnlinkedCodeBlock* codeBlock = fixture.codeBlock.get();
    UnlinkedFunctionExecutable* executable = fixture.root.get();
    if (!codeBlock && !executable)
        return fixture;
    for (HolderStep step : side.path) {
        if (step == HolderStep::End)
            break;
        if (executable) {
            source = executable->linkedSourceCode(source);
            Strong<UnlinkedFunctionCodeBlock> body = generateBodyOf(vm, *executable, source, CodeSpecializationKind::CodeForCall);
            if (!body)
                return fixture;
            codeBlock = body.get();
            fixture.bodies.append(body);
        }
        executable = selectChild(*codeBlock, [step](UnlinkedFunctionExecutable& candidate) {
            return acceptsStep(step, candidate);
        });
        if (!executable)
            return fixture;
    }
    fixture.executable = executable;
    return fixture;
}

// Generates both sides of a variant; for a flag variant, also checks that the flag alone sets their UFEs apart.
inline bool generateHolderPair(VM& vm, JSGlobalObject* globalObject, const HolderVariant& variant, TestPart& part, HolderFixture& base, HolderFixture& other)
{
    base = generateHolderSide(vm, globalObject, variant.base);
    other = generateHolderSide(vm, globalObject, variant.variant);
    if (!part.check(base.executable && other.executable, makeString("a holder variant for "_s, variant.field, " lacks its UFE"_s)))
        return false;
    if (!variant.flag)
        return true;
    bool alone = differsOnlyInFlag(*base.executable, *other.executable, *variant.flag, !variant.chainDiffers);
    return part.check(alone, makeString("the two UFEs of the variant for "_s, variant.field, " differ in more than that flag, so they cannot show that it is covered"_s));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
