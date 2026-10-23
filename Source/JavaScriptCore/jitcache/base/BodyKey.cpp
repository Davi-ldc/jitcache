#include "config.h"
#include "jitcache/base/BodyKey.h"

#include "ParserModes.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/WTFString.h>

// Builds, checks and reads the 40 bytes of a body key, and spells them in hex. The identity digest a key carries comes
// from UCBKeys.cpp.

namespace JSC::JITCache {

namespace BodyKeyInternal {

// The fixed layout BodyKey.h describes.
static constexpr uint8_t bodyKeyVersion = 1;
static constexpr size_t versionOffset = 0;
static constexpr size_t identityKindOffset = 1;
static constexpr size_t specializationOffset = 2;
static constexpr size_t codeGenerationModeOffset = 3;
static constexpr size_t reservedOffset = 4;
static constexpr size_t reservedSize = 4;
static constexpr size_t identityDigestOffset = 8;
static_assert(reservedOffset + reservedSize == identityDigestOffset);
static_assert(identityDigestOffset + std::tuple_size_v<Digest256> == BodyKey::byteSize);
// identityDigest() reads its 32 bytes in place, as a Digest256.
static_assert(sizeof(Digest256) == std::tuple_size_v<Digest256> && alignof(Digest256) == 1);

// Every value CodeGenerationMode defines; fromBytes accepts no other mode bit.
static constexpr OptionSet<CodeGenerationMode> definedCodeGenerationModes {
    CodeGenerationMode::Debugger, CodeGenerationMode::TypeProfiler, CodeGenerationMode::ControlFlowProfiler,
};

// Programs, modules, indirect evals and direct evals have no construct form, so their keys always have specialization 0.
static constexpr bool isCallOnlyKind(IdentityKind kind)
{
    return kind == IdentityKind::Program || kind == IdentityKind::Module || kind == IdentityKind::IndirectEval || kind == IdentityKind::DirectEval;
}

static constexpr bool isDefinedIdentityKind(uint8_t value)
{
    return value >= static_cast<uint8_t>(IdentityKind::Program) && value <= static_cast<uint8_t>(IdentityKind::DirectEval);
}

// The value of a digit bodyKeyHexDigits writes; nothing for any other character, an uppercase digit included.
static std::optional<uint8_t> lowercaseHexDigitValue(char character)
{
    if (isASCIIDigit(character))
        return static_cast<uint8_t>(character - '0');
    if (character >= 'a' && character <= 'f')
        return static_cast<uint8_t>(character - 'a' + 10);
    return std::nullopt;
}

} // namespace BodyKeyInternal

BodyKey BodyKey::make(IdentityKind kind, CodeSpecializationKind specialization, OptionSet<CodeGenerationMode> mode, const Digest256& identityDigest)
{
    using namespace BodyKeyInternal;

    ASSERT(isDefinedIdentityKind(static_cast<uint8_t>(kind)));
    ASSERT(specialization == CodeSpecializationKind::CodeForCall || !isCallOnlyKind(kind));
    ASSERT(mode.containsOnly(definedCodeGenerationModes));

    BodyKey key { };
    key.m_bytes[versionOffset] = bodyKeyVersion;
    key.m_bytes[identityKindOffset] = static_cast<uint8_t>(kind);
    key.m_bytes[specializationOffset] = specialization == CodeSpecializationKind::CodeForConstruct ? 1 : 0;
    key.m_bytes[codeGenerationModeOffset] = mode.toRaw();
    memcpySpan(std::span { key.m_bytes }.subspan<identityDigestOffset>(), std::span { identityDigest });
    return key;
}

std::optional<BodyKey> BodyKey::fromBytes(std::span<const uint8_t, byteSize> bytes)
{
    using namespace BodyKeyInternal;

    if (bytes[versionOffset] != bodyKeyVersion)
        return std::nullopt;
    if (!isDefinedIdentityKind(bytes[identityKindOffset]))
        return std::nullopt;
    auto kind = static_cast<IdentityKind>(bytes[identityKindOffset]);
    uint8_t specialization = bytes[specializationOffset];
    if (specialization > 1 || (specialization && isCallOnlyKind(kind)))
        return std::nullopt;
    if (!OptionSet<CodeGenerationMode>::fromRaw(bytes[codeGenerationModeOffset]).containsOnly(definedCodeGenerationModes))
        return std::nullopt;
    if (std::ranges::any_of(bytes.subspan<reservedOffset, reservedSize>(), [](uint8_t byte) { return !!byte; }))
        return std::nullopt;

    BodyKey key;
    memcpySpan(std::span { key.m_bytes }, bytes);
    return key;
}

std::span<const uint8_t, BodyKey::byteSize> BodyKey::bytes() const
{
    return std::span { m_bytes };
}

IdentityKind BodyKey::identityKind() const
{
    return static_cast<IdentityKind>(m_bytes[BodyKeyInternal::identityKindOffset]);
}

CodeSpecializationKind BodyKey::specialization() const
{
    return m_bytes[BodyKeyInternal::specializationOffset] ? CodeSpecializationKind::CodeForConstruct : CodeSpecializationKind::CodeForCall;
}

OptionSet<CodeGenerationMode> BodyKey::codeGenerationMode() const
{
    return OptionSet<CodeGenerationMode>::fromRaw(m_bytes[BodyKeyInternal::codeGenerationModeOffset]);
}

const Digest256& BodyKey::identityDigest() const
{
    return reinterpretCastSpanStartTo<Digest256>(std::span { m_bytes }.subspan<BodyKeyInternal::identityDigestOffset>());
}

std::array<char, bodyKeyHexLength> bodyKeyHexDigits(const BodyKey& key)
{
    std::array<char, bodyKeyHexLength> digits { };
    auto bytes = key.bytes();
    for (size_t index = 0; index < bytes.size(); ++index) {
        digits[2 * index] = upperNibbleToLowercaseASCIIHexDigit(bytes[index]);
        digits[2 * index + 1] = lowerNibbleToLowercaseASCIIHexDigit(bytes[index]);
    }
    return digits;
}

String bodyKeyHex(const BodyKey& key)
{
    // One encoding serves the reports and the shell, which reads it back with bodyKeyFromHex.
    auto digits = bodyKeyHexDigits(key);
    return String::fromLatin1(std::span<const char> { digits });
}

std::optional<BodyKey> bodyKeyFromHex(std::span<const char> digits)
{
    using namespace BodyKeyInternal;
    if (digits.size() != bodyKeyHexLength)
        return std::nullopt;
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    for (size_t index = 0; index < bytes.size(); ++index) {
        auto high = lowercaseHexDigitValue(digits[2 * index]);
        auto low = lowercaseHexDigitValue(digits[2 * index + 1]);
        if (!high || !low)
            return std::nullopt;
        bytes[index] = static_cast<uint8_t>(*high << 4 | *low);
    }
    return BodyKey::fromBytes(bytes);
}

} // namespace JSC::JITCache
