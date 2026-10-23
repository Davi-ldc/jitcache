#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "BuiltinExecutables.h"
#include "CodeCache.h"
#include "DirectEvalExecutable.h"
#include "JSCInlines.h"
#include "JSGlobalObject.h"
#include "ParserError.h"
#include "SourceCode.h"
#include "StrongInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedEvalCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedModuleProgramCodeBlock.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "VariableEnvironmentInlines.h"
#include "jitcache/tests/ucb/UCBTestFixtures.h"
#include "jitcache/tests/ucb/UCBTestPart.h"
#include "jitcache/ucb/UCBKeys.h"
#include <algorithm>
#include <array>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The holder variants: pairs of UFEs that differ in one field a holder digest and a descriptor cover, which the holder
// digest and descriptor tests read, and the generation of each side.

namespace JSC::JITCache::Tests {

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
        && a.unlinkedFunctionEnd() == b.unlinkedFunctionEnd() && a.parametersStartOffset() == b.parametersStartOffset();
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
