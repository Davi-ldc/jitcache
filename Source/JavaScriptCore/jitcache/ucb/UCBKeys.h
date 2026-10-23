#pragma once

#include "BytecodeIndex.h"
#include "ExecutableInfo.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
#include "jitcache/base/BodyKey.h"
#include <optional>
#include <wtf/Variant.h>
#include <wtf/text/StringView.h>

namespace JSC {

class CoreEncodingBudget;
class SourceCode;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// Key derivation: the identity digests that body keys carry, and the context, holder and source digests. The key itself is
// in BodyKey.h. No context records a line or column: a UCB holds none, so one body serves a source at any start line, and
// deriving one would build the provider's line-start table from its whole text.

// Identity digests: the SHA-256 of a 16-byte label and fixed-width little-endian fields.
Digest256 rootIdentityDigest(IdentityKind, const Digest256& sourceDigest, LexicallyScopedFeatures, std::optional<int32_t> parameterEnd);
Digest256 childIdentityDigest(const BodyKey& parent, ChildTable, uint32_t index);
Digest256 directEvalIdentityDigest(const BodyKey& caller, BytecodeIndex, const Digest256& textDigest);

// Context digests, and the holder digest of a UFE: its descriptor and its TDZ chain.
Digest256 globalContextDigest(IdentityKind, unsigned providerOffset, JSParserScriptMode, DerivedContextType, EvalContextType, bool isArrowFunctionContext);
Digest256 executableBodyContextDigest(IdentityKind, unsigned providerOffset, const std::optional<Digest256>& rootHolderDigest);
// Empty only when a budget refused a page of the descriptor encoding or the sort buffer of an environment it digested
// first; requests pass none. `environmentsDigested`, when given, gains the number of environments this call digested
// first.
std::optional<Digest256> holderDigest(VM&, const UnlinkedFunctionExecutable&, CoreEncodingBudget* = nullptr, unsigned* environmentsDigested = nullptr);
// The digest of a TDZ chain, which a holder digest ends with: 32 zero bytes for a null chain. Each environment on the
// chain is digested at most once while it lives and keeps its digest; `environmentsDigested` gains one for each
// environment this call digested first. A budget, when given, is charged for each such environment's sort buffer while
// the buffer lives, and for the list of the chain's links while it lives when the chain has more links than the 32 the
// list keeps inline, charged before the list moves to the heap. Empty on a refusal; digests already kept stay.
std::optional<Digest256> tdzChainDigest(const TDZEnvironmentLink*, CoreEncodingBudget*, unsigned& environmentsDigested);
Digest256 directEvalContextDigest(unsigned providerOffset, LexicallyScopedFeatures, DerivedContextType, NeedsClassFieldInitializer, PrivateBrandRequirement,
    bool isArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType, const TDZEnvironment&, const PrivateNameEnvironment&);

// What a record keeps of its context: a global's or a UFE body's inputs, or a direct eval's digest.
struct GlobalContextInputs { // Program, Module, IndirectEval: globalContextDigest's arguments after the kind
    unsigned providerOffset;
    JSParserScriptMode scriptMode;
    DerivedContextType derivedContextType;
    EvalContextType evalContextType;
    bool isArrowFunctionContext;
};
struct BodyContextInputs { // FunctionConstructor, Builtin, Child; a root's holder digest comes from its UFE
    unsigned providerOffset;
};
struct DirectEvalContext {
    std::optional<Digest256> digest; // the digest its request computed, if it computed one
};
using RecordedContext = Variant<GlobalContextInputs, BodyContextInputs, DirectEvalContext>;
// The context digest of a record whose key has the given identity kind: globalContextDigest or executableBodyContextDigest
// of the inputs, with rootHolderDigest for a FunctionConstructor or Builtin body, or a direct eval's stored digest. Empty
// for a direct eval that stored none and for a root body passed no holder digest.
std::optional<Digest256> contextDigestOf(IdentityKind, const RecordedContext&, const std::optional<Digest256>& rootHolderDigest);

// Source digests: SHA-256 of u8 encoding then the code units, with encoding 1 (Latin-1) exactly when every
// unit is at most 0xFF and encoding 2 (UTF-16LE) otherwise, so an 8-bit and a 16-bit string with the same characters
// digest alike. No copy of the source is made.
Digest256 sourceDigest(StringView);

enum class SourceDigestOrigin : uint8_t { Computed, BuiltinMetadata, Provider };
struct RootSourceDigest {
    Digest256 digest;
    SourceDigestOrigin origin;
};
// *builtinMetadataDigest when given, else the provider's jitCacheSourceDigest() when rootSource spans its provider and the
// provider supplies one, else sourceDigest(rootSource.view()).
RootSourceDigest rootSourceDigest(const SourceCode& rootSource, const Digest256* builtinMetadataDigest = nullptr);

// All but holderDigest and tdzChainDigest are pure, take no lock, allocate no cell and run on any thread; rootSourceDigest
// calls only the provider's const accessors. holderDigest hashes the codec's descriptor encoding page by page and then the
// chain digest tdzChainDigest returns. Both run on the VM thread and allocate no cell, and the only state they write is
// the digests they keep in environments.

} // namespace JSC::JITCache
