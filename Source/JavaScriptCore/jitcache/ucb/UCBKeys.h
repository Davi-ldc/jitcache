#pragma once

#include "BodyKey.h"
#include "BytecodeIndex.h"
#include "ExecutableInfo.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
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
// in BodyKey.h.

// Whether a captured core has generation's form or is a UCB the embedder decoded.
enum class CoreProvenance : uint8_t { Generated = 0, EmbedderDecoded = 1 };

// Identity digests: the SHA-256 of a 16-byte label and fixed-width little-endian fields.
Digest256 rootIdentityDigest(IdentityKind, const Digest256& sourceDigest, LexicallyScopedFeatures, std::optional<int32_t> parameterEnd);
Digest256 childIdentityDigest(const BodyKey& parent, ChildTable, uint32_t index);
Digest256 directEvalIdentityDigest(const BodyKey& caller, BytecodeIndex, const Digest256& textDigest);

// Context digests, and the holder digest of a UFE: its descriptor and its TDZ chain.
Digest256 globalContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, JSParserScriptMode, DerivedContextType, EvalContextType, bool isArrowFunctionContext);
Digest256 executableBodyContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, const std::optional<Digest256>& rootHolderDigest);
// Empty only when a budget refused a page of the descriptor encoding or the sort buffer of an environment it digested
// first; requests pass none. `environmentsDigested`, when given, gains the number of environments this call digested
// first.
std::optional<Digest256> holderDigest(VM&, const UnlinkedFunctionExecutable&, CoreEncodingBudget* = nullptr, unsigned* environmentsDigested = nullptr);
Digest256 directEvalContextDigest(unsigned providerOffset, unsigned firstLine, LexicallyScopedFeatures, DerivedContextType, NeedsClassFieldInitializer, PrivateBrandRequirement,
    bool isArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType, const TDZEnvironment&, const PrivateNameEnvironment&);

// What a record keeps of its context: a global's or a UFE body's inputs, or a direct eval's digest.
struct GlobalContextInputs { // Program, Module, IndirectEval: globalContextDigest's arguments after the kind
    unsigned providerOffset;
    unsigned firstLine;
    JSParserScriptMode scriptMode;
    DerivedContextType derivedContextType;
    EvalContextType evalContextType;
    bool isArrowFunctionContext;
};
struct BodyContextInputs { // FunctionConstructor, Builtin, Child; a root's holder digest comes from its UFE
    unsigned providerOffset;
    unsigned firstLine;
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

// All but holderDigest are pure, take no lock, allocate no cell and run on any thread; rootSourceDigest calls only the
// provider's const accessors. holderDigest hashes the codec's descriptor encoding page by page and then the chain digest
// tdzChainDigest returns, on the VM thread; it allocates no cell, and the only state it writes is the digests it keeps in
// environments.

} // namespace JSC::JITCache
