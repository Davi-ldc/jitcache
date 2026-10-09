#pragma once

#include "BytecodeIndex.h"
#include "CodeSpecializationKind.h"
#include "ExecutableInfo.h"
#include "ParserModes.h"
#include "VariableEnvironment.h"
#include <array>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/OptionSet.h>
#include <wtf/Variant.h>
#include <wtf/text/StringView.h>

namespace JSC {

class CoreEncodingBudget;
class SourceCode;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// Body keys, identity digests, context digests, holder digests and source digests (SPEC-ucb.md section 3).

// A body key is 40 canonical bytes, which the integrator's body-key hash reads (section 3.1):
//   0  u8      version, 1
//   1  u8      identityKind
//   2  u8      specialization: 0 for CodeForCall, 1 for CodeForConstruct
//   3  u8      codeGenerationMode: OptionSet<CodeGenerationMode>::toRaw() of the mode the UCB stores
//   4  u8[4]   reserved, 0
//   8  u8[32]  identityDigest: SHA-256 of the identity record of section 3.2
enum class IdentityKind : uint8_t {
    Program = 1, Module = 2, IndirectEval = 3, FunctionConstructor = 4, Builtin = 5, Child = 6, DirectEval = 7,
};
enum class ChildTable : uint8_t { Declarations = 0, Expressions = 1 };
enum class CoreProvenance : uint8_t { Generated = 0, EmbedderDecoded = 1 }; // section 4.2
using Digest256 = std::array<uint8_t, 32>;

// Program, Module, IndirectEval and DirectEval keys always have specialization 0. fromBytes accepts mode bits only within
// CodeGenerationMode's defined values. A key names a body, never a tier: every tier's sections of one body share it.
class BodyKey {
public:
    static constexpr size_t byteSize = 40;
    static BodyKey make(IdentityKind, CodeSpecializationKind, OptionSet<CodeGenerationMode>, const Digest256& identityDigest);
    // Empty when version, kind, specialization, mode bits or reserved bytes are out of range.
    static std::optional<BodyKey> fromBytes(std::span<const uint8_t, byteSize>);
    std::span<const uint8_t, byteSize> bytes() const LIFETIME_BOUND;
    IdentityKind identityKind() const;
    CodeSpecializationKind specialization() const;
    OptionSet<CodeGenerationMode> codeGenerationMode() const;
    const Digest256& identityDigest() const LIFETIME_BOUND;
    friend bool operator==(const BodyKey&, const BodyKey&) = default;

private:
    std::array<uint8_t, byteSize> m_bytes;
};

// The integrator keys its index by the whole key and builds the table's empty and deleted values as byte fills.
static_assert(sizeof(BodyKey) == BodyKey::byteSize);
static_assert(std::is_trivially_copyable_v<BodyKey>);

// Identity digests (section 3.2): the SHA-256 of a 16-byte label and fixed-width little-endian fields.
Digest256 rootIdentityDigest(IdentityKind, const Digest256& sourceDigest, LexicallyScopedFeatures, std::optional<int32_t> parameterEnd);
Digest256 childIdentityDigest(const BodyKey& parent, ChildTable, uint32_t index);
Digest256 directEvalIdentityDigest(const BodyKey& caller, BytecodeIndex, const Digest256& textDigest);

// Context digests (section 3.3).
Digest256 globalContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, JSParserScriptMode, DerivedContextType, EvalContextType, bool isArrowFunctionContext);
Digest256 executableBodyContextDigest(IdentityKind, unsigned providerOffset, unsigned firstLine, const std::optional<Digest256>& rootHolderDigest);
// Empty only when a budget refused a page of the descriptor encoding or the sort buffer of an environment it digested first
// (SPEC-ucb.codec.md, section 2); requests pass none. `environmentsDigested`, when given, gains the number of environments
// this call digested first (section 3.4).
std::optional<Digest256> holderDigest(VM&, const UnlinkedFunctionExecutable&, CoreEncodingBudget* = nullptr, unsigned* environmentsDigested = nullptr);
Digest256 directEvalContextDigest(unsigned providerOffset, unsigned firstLine, LexicallyScopedFeatures, DerivedContextType, NeedsClassFieldInitializer, PrivateBrandRequirement,
    bool isArrowFunctionContext, bool isInsideOrdinaryFunction, EvalContextType, const TDZEnvironment&, const PrivateNameEnvironment&);

// What a record keeps of its context (section 3.4): a global's or a UFE body's inputs, or a direct eval's digest.
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

// Source digests (section 3.5): SHA-256 of u8 encoding then the code units, with encoding 1 (Latin-1) exactly when every
// unit is at most 0xFF and encoding 2 (UTF-16LE) otherwise, so an 8-bit and a 16-bit string with the same characters
// digest alike. No copy of the source is made.
Digest256 sourceDigest(StringView);

enum class SourceDigestOrigin : uint8_t { Computed, BuiltinMetadata, Provider };
struct RootSourceDigest {
    Digest256 digest;
    SourceDigestOrigin origin;
};
// Section 3.5: *builtinMetadataDigest when given (form 1), else the provider's jitCacheSourceDigest() when rootSource
// spans its provider (form 2), else sourceDigest(rootSource.view()) (form 3).
RootSourceDigest rootSourceDigest(const SourceCode& rootSource, const Digest256* builtinMetadataDigest = nullptr);

// All but holderDigest are pure, take no lock, allocate no cell and run on any thread; rootSourceDigest calls only the
// provider's const accessors. holderDigest hashes the codec's descriptor encoding page by page (SPEC-ucb.codec.md, E13)
// and then the chain digest tdzChainDigest returns, on the VM thread; it allocates no cell, and the only state it writes
// is the digests it keeps in environments (section 3.4).

} // namespace JSC::JITCache
