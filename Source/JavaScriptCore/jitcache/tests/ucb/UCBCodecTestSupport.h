#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "CachedBytecode.h"
#include "CachedTypes.h"
#include "JSCInlines.h"
#include "SourceCode.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include "VM.h"
#include "VariableEnvironment.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

// What the tests of the core codec share: a UFE's descriptor as bytes, the decode of a core from a copy of its bytes, the
// corpus of programs, a module and two evals they encode, and what they compare of a decoded UCB: the shape of a chain and
// whether every string constant is an atom. The names live in a namespace of their own.

namespace JSC::JITCache::Tests::UCBCodecTestSupport {

inline std::optional<Vector<uint8_t>> encodeDescriptorBytes(VM& vm, const UnlinkedFunctionExecutable& executable)
{
    CoreEncodeFailure failure = CoreEncodeFailure::None;
    RefPtr<CachedBytecode> descriptor = encodeUnlinkedFunctionExecutableDescriptor(vm, executable, nullptr, failure);
    if (!descriptor)
        return std::nullopt;
    return Vector<uint8_t> { descriptor->span() };
}

// Decodes a copy of `bytes`, which starts 8-byte aligned in a buffer the payload owns, as the import's body does, against
// `source`, the source the core's root was generated from, as the import decodes against the request's.
inline UnlinkedCodeBlock* decodeCoreBytes(VM& vm, std::span<const uint8_t> bytes, const SourceCode& source, UnlinkedCodeBlockCoreKind kind, const UnlinkedFunctionExecutable* holder, CoreDecodeFailure& failure)
{
    Vector<uint8_t> copy { bytes };
    std::span<uint8_t> data = copy.mutableSpan();
    Ref<CachedBytecode> payload = CachedBytecode::create(data, [copy = WTF::move(copy)](const void*) { }, { });
    failure = CoreDecodeFailure::None;
    return decodeUnlinkedCodeBlockCore(vm, WTF::move(payload), source, kind, holder, failure);
}

// Programs in the manner of JSTests/stress, together covering the forms a core encodes: closures and loops, integer,
// character and string switches, exception handlers, classes with fields, private names, static blocks and super, generators
// and async functions, destructuring with an object rest, whose excluded names make an identifier set, and spread,
// regular expressions, big integers, doubles, templates, butterflies of strings, int32s and doubles, a literal whose
// elements the parser folds to NaNs of both signs, more than nine global declarations, with and eval, accessors and computed
// keys, and nested block scopes.
inline constexpr std::array<ASCIILiteral, 7> codecCorpus { {
    "function jitcacheU7Switch(x, s) {\n"
    "  var total = 0;\n"
    "  outer: for (var i = 0; i < x; i++) {\n"
    "    switch (i % 5) { case 0: total += 1; break; case 1: total -= 2; break; case 2: case 3: total *= 2; break; default: continue outer; }\n"
    "    switch (s[i % s.length]) { case 'a': total++; break; case 'b': total--; break; }\n"
    "    switch (s) { case 'alpha': total += 3; break; case 'beta': total += 4; break; case 'gamma': total += 5; break; }\n"
    "  }\n"
    "  try { if (total > 100) throw new Error('jitcache-u7-big'); } catch (e) { total = -1; } finally { total += arguments.length; }\n"
    "  return function inner(y) { return total + y; };\n"
    "}\n"
    "var jitcacheU7SwitchResult = jitcacheU7Switch(10, 'abba');\n"_s,
    "class JitcacheU7Base { #secret = 1; static count = 0; static { JitcacheU7Base.count++; } get secret() { return this.#secret; } #hidden() { return 2; } reveal() { return this.#hidden() + this.#secret; } }\n"
    "class JitcacheU7Derived extends JitcacheU7Base { extra = [\"alpha\", \"beta\"]; constructor() { super(); this.y = 3; } static make() { return new JitcacheU7Derived(); } }\n"
    "let jitcacheU7Instance = JitcacheU7Derived.make();\n"_s,
    "function* jitcacheU7Gen(n) { for (let i = 0; i < n; i++) yield i; }\n"
    "async function jitcacheU7Async(a, { b, c = 2 } = {}, ...rest) { const [d, ...e] = await Promise.resolve([a, b, c]); return d?.toString() ?? e.length + rest.length; }\n"
    "async function* jitcacheU7AsyncGen() { yield* jitcacheU7Gen(3); }\n"
    "const jitcacheU7Spread = [...jitcacheU7Gen(4), ...\"xyz\"];\n"
    "const { jitcacheU7Kept, ...jitcacheU7Others } = { jitcacheU7Kept: 1, jitcacheU7Other: 2 };\n"_s,
    "var jitcacheU7Literals = { re: /jit(cache)+\\d*/gi, big: 12345678901234567890123n, d: 1.5, n: NaN, z: -0, inf: Infinity, words: [\"one\", \"two\", \"three\"], mixed: [\"one\", 2], hole: [1, , 3],\n"
    "  fractions: [1.5, 2.5], nans: [1e400 * 0, -(1e400 * 0)] };\n"
    "function jitcacheU7Tag(strings, ...values) { return strings.raw.join('|') + values.join(','); }\n"
    "var jitcacheU7Template = jitcacheU7Tag`a${1}b${2}c` + `plain ${jitcacheU7Literals.d}`;\n"_s,
    "var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11, v12 = 12;\n"
    "let l0 = 0, l1 = 1, l2 = 2, l3 = 3, l4 = 4, l5 = 5, l6 = 6, l7 = 7, l8 = 8, l9 = 9, l10 = 10;\n"
    "function jitcacheU7Sloppy(o, code) { with (o) { var r = x; } return eval(code) + r; }\n"_s,
    "var jitcacheU7Object = { get g() { return 1; }, set s(v) { this._s = v; }, ['computed' + 1]: 2, [Symbol.iterator]: function* () { yield 1; } };\n"
    "function jitcacheU7Walk(o) { var keys = []; for (var k in o) keys.push(k); for (var v of [1, 2]) keys.push(v); delete o.g; return ('s' in o) && (o instanceof Object) && typeof o; }\n"_s,
    // Two sibling functions of one block, whose chains of two links share both, and an arrow in each.
    "'use strict';\n"
    "{ let jitcacheU7Outer = 1; { const jitcacheU7Inner = 2;\n"
    "  function jitcacheU7Nested() { return () => jitcacheU7Outer + jitcacheU7Inner; }\n"
    "  function jitcacheU7Sibling() { return () => jitcacheU7Inner; }\n"
    "  jitcacheU7Nested(); jitcacheU7Sibling(); } }\n"_s,
} };
inline constexpr size_t nestedScopesCorpusIndex = 6;
static_assert(nestedScopesCorpusIndex + 1 == codecCorpus.size());

// The codec's other root records: a module, whose var declarations, its import among them, outnumber an InlineMap's inline
// capacity, so its core crosses the hashed layout; an indirect eval with var and function declarations and a sloppy block
// function, which the eval keeps as a hoisting candidate; and a direct eval under a TDZ set, whose function's chain starts
// at the link over that set and goes into the core whole, since an eval core has no holder.
inline constexpr ASCIILiteral moduleCorpusText = "import { jitcacheU7Imported } from 'jitcache-u7-module-dependency';\n"
    "var jitcacheU7M0 = 0, jitcacheU7M1 = 1, jitcacheU7M2 = 2, jitcacheU7M3 = 3, jitcacheU7M4 = 4, jitcacheU7M5 = 5;\n"
    "var jitcacheU7M6 = 6, jitcacheU7M7 = 7, jitcacheU7M8 = 8, jitcacheU7M9 = 9, jitcacheU7M10 = 10, jitcacheU7M11 = 11;\n"
    "let jitcacheU7ModuleLet = 'jitcache-u7-module-let';\n"
    "const jitcacheU7ModuleConst = [jitcacheU7M0, jitcacheU7Imported];\n"
    "export function jitcacheU7ModuleExported(a) { return a + jitcacheU7M1 + jitcacheU7ModuleLet.length; }\n"
    "export default class JitcacheU7ModuleDefault { static m() { return jitcacheU7ModuleConst; } }\n"_s;
inline constexpr ASCIILiteral indirectEvalCorpusText = "var jitcacheU7EvalA = 1, jitcacheU7EvalB = 'jitcache-u7-eval-string';\n"
    "function jitcacheU7EvalFunction(a) { return a + jitcacheU7EvalA; }\n"
    "if (jitcacheU7EvalA) { function jitcacheU7EvalHoisted() { return jitcacheU7EvalB; } }\n"
    "let jitcacheU7EvalLet = [jitcacheU7EvalA, jitcacheU7EvalB];\n"
    "jitcacheU7EvalFunction(jitcacheU7EvalLet.length);\n"_s;
inline constexpr ASCIILiteral directEvalCorpusText = "let jitcacheU7DirectLet = 2;\n"
    "function jitcacheU7DirectFunction() { return jitcacheU7DirectOuter + jitcacheU7DirectLet; }\n"
    "var jitcacheU7DirectVar = jitcacheU7DirectFunction;\n"_s;

// A program whose core spans several pages.
inline String bigProgramText()
{
    StringBuilder text;
    for (unsigned i = 0; i < 400; ++i)
        text.append("function jitcacheU7Big"_s, i, "(a) { return a + \"jitcache-u7-big-constant-"_s, i, "\"; }\n"_s);
    return text.toString();
}

// The chain shape the twin check compares: the number of links, and where the chain reaches `target`.
struct ChainShape {
    unsigned length { 0 };
    std::optional<unsigned> targetPosition;
    friend bool operator==(const ChainShape&, const ChainShape&) = default;
};

inline ChainShape chainShape(TDZEnvironmentLink* link, const TDZEnvironmentLink* target)
{
    ChainShape shape;
    for (; link; link = link->parent()) {
        if (target && link == target && !shape.targetPosition)
            shape.targetPosition = shape.length;
        ++shape.length;
    }
    return shape;
}

inline bool isAtomString(JSValue value)
{
    auto* string = value ? dynamicDowncast<JSString>(value) : nullptr;
    StringImpl* impl = string ? string->tryGetValueImpl() : nullptr;
    return impl && impl->isAtom();
}

inline bool allStringConstantsAreAtoms(UnlinkedCodeBlock& codeBlock)
{
    for (auto& constant : codeBlock.constantRegisters()) {
        JSValue value = constant.get();
        if (value && value.isString() && !isAtomString(value))
            return false;
    }
    return true;
}

} // namespace JSC::JITCache::Tests::UCBCodecTestSupport

#endif // ENABLE(JITCACHE_TWINS)
