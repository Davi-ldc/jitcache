#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "CodeCache.h"
#include "JSCInlines.h"
#include "ParserError.h"
#include "ProgramExecutable.h"
#include "SourceCode.h"
#include "SourceCodeKey.h"
#include "StrongInlines.h"
#include "SymbolTable.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "UnlinkedProgramCodeBlock.h"
#include "VM.h"
#include "VariableEnvironmentInlines.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/ucb/UCBKeys.h"
#include "jitcache/ucb/UCBRegistry.h"
#include "jitcache/ucb/UCBRequests.h"
#include "jitcache/ucb/UCBTwinGeneration.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// The fixtures the UCB lane's tests build: registry records, the children a UCB holds, the identity section's offsets,
// programs generated from text and the constants they hold, the program whose sections the section tests read, TDZ sets,
// and function bodies and cores.

namespace JSC::JITCache::Tests {

// A record under `key` with fixed program context inputs, no stamp, no pending import and no analysis source.
inline UCBRecord testRecord(const BodyKey& key, UCBOrigin origin, UnlinkedFunctionExecutable* holder = nullptr)
{
    return UCBRecord {
        .key = key,
        .context = GlobalContextInputs { 0, JSParserScriptMode::Classic, DerivedContextType::None, EvalContextType::None, false },
        .origin = origin,
        .keyFeatures = NoLexicallyScopedFeatures,
        .missedBodyVersion = std::nullopt,
        .matchedBodyVersion = std::nullopt,
        .pendingImport = nullptr,
        .holder = holder,
    };
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
inline constexpr size_t identityKeyOffset = 0;
inline constexpr size_t identityContextDigestOffset = 40;
inline constexpr size_t identityHolderDigestOffset = 72;
inline constexpr size_t identityFeaturesOffset = 104;
inline constexpr size_t identityLexicallyScopedFeaturesOffset = 106;
inline constexpr size_t identityHasCapturedVariablesOffset = 107;
inline constexpr size_t identityConstantCountOffset = 108;
inline constexpr size_t identityCoreKindOffset = 112;
inline constexpr size_t identityButterflyMapOffset = 113;

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

// The layout checks read the bits at and above N in the last byte of the butterfly map and of the constant bits, which that
// byte holds only when N % 8 != 0, so the fixture program takes one more string constant per try until its constant count
// is not a multiple of eight.
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

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
