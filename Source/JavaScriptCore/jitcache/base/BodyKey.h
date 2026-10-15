#pragma once

#include "CodeSpecializationKind.h"
#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/OptionSet.h>

// The body key, which names a body the same way in every process, and the types it is made of. Every part of JITCache may
// include this file: storage indexes and stamps bodies by key, maintenance and the twin reports print keys, and the UCB
// lane derives them (UCBKeys.h). Nothing here takes a lock or allocates, and any thread may call it.

namespace JSC {

// Defined in ParserModes.h, which BodyKey.cpp includes for its values; declaring it here keeps parser headers out of the
// files that only name a key.
enum class CodeGenerationMode : uint8_t;

} // namespace JSC

namespace JSC::JITCache {

// A SHA-256 digest, as JITCacheSHA256.h computes it.
using Digest256 = std::array<uint8_t, 32>;

// The kind of body a key names: a program, a module or an indirect eval, the body of a `new Function` or of a builtin, a
// function nested in another body, or a direct eval.
enum class IdentityKind : uint8_t {
    Program = 1, Module = 2, IndirectEval = 3, FunctionConstructor = 4, Builtin = 5, Child = 6, DirectEval = 7,
};

// The table of its parent's UnlinkedCodeBlock that holds a nested function: the declarations or the expressions.
enum class ChildTable : uint8_t { Declarations = 0, Expressions = 1 };

// A body key is 40 canonical bytes, which the integrator's body-key hash reads:
//   0  u8      version, 1
//   1  u8      identityKind
//   2  u8      specialization: 0 for CodeForCall, 1 for CodeForConstruct
//   3  u8      codeGenerationMode: OptionSet<CodeGenerationMode>::toRaw() of the mode the UCB stores
//   4  u8[4]   reserved, 0
//   8  u8[32]  identityDigest: the SHA-256 of the body's identity record, which UCBKeys.h derives
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

} // namespace JSC::JITCache
