#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "BodyKey.h"
#include "CodeSpecializationKind.h"
#include "JITCacheSHA256.h"
#include "JITCacheTest.h"
#include "ParserModes.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of base/: SHA-256 on every path the CPU offers, through runSHA256SelfTest, which JITCacheSHA256.cpp defines
// beside the paths it keeps private, and the 40 canonical bytes of a BodyKey.

namespace JSC::JITCache::Tests {

// The FIPS 180-4 examples on every path and through the SHA256 class, whole and in updates that split them across blocks,
// and random messages of up to four blocks compared with the portable path.
JITCACHE_TEST(baseSHA256AgreesOnEveryPath, No)
{
    String failure;
    if (!runSHA256SelfTest(failure))
        JITCACHE_FAIL(failure);
}

// BodyKey::make lays out the 40 canonical bytes, fromBytes reads them back, and fromBytes rejects each field out of range.
JITCACHE_TEST(baseBodyKeyBytes, No)
{
    Digest256 identityDigest;
    identityDigest.fill(0x42);
    BodyKey key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, CodeGenerationMode::Debugger, identityDigest);
    std::array<uint8_t, BodyKey::byteSize> bytes;
    memcpySpan(std::span { bytes }, key.bytes());
    bool laidOut = bytes[0] == 1 && bytes[1] == static_cast<uint8_t>(IdentityKind::Child) && bytes[2] == 1 && bytes[3] == OptionSet<CodeGenerationMode> { CodeGenerationMode::Debugger }.toRaw()
        && !bytes[4] && !bytes[5] && !bytes[6] && !bytes[7] && key.identityDigest() == identityDigest;
    if (!laidOut)
        JITCACHE_FAIL("BodyKey::make does not lay out the 40 canonical bytes"_s);
    auto parse = [](const std::array<uint8_t, BodyKey::byteSize>& candidate) {
        return BodyKey::fromBytes(std::span<const uint8_t, BodyKey::byteSize> { candidate });
    };
    if (parse(bytes) != key)
        JITCACHE_FAIL("BodyKey::fromBytes does not read back a key's bytes"_s);
    struct Mutation {
        size_t offset;
        uint8_t value;
        ASCIILiteral what;
    };
    static constexpr std::array<Mutation, 11> mutations { {
        { 0, 0, "version 0"_s }, { 0, 2, "version 2"_s }, { 1, 0, "identity kind 0"_s }, { 1, 8, "identity kind 8"_s }, { 2, 2, "specialization 2"_s },
        { 3, 0x08, "a mode bit outside CodeGenerationMode"_s }, { 3, 0x80, "the top mode bit"_s },
        { 4, 1, "reserved byte 4"_s }, { 5, 1, "reserved byte 5"_s }, { 6, 1, "reserved byte 6"_s }, { 7, 1, "reserved byte 7"_s },
    } };
    for (const Mutation& mutation : mutations) {
        auto crafted = bytes;
        crafted[mutation.offset] = mutation.value;
        if (parse(crafted))
            JITCACHE_FAIL(makeString("BodyKey::fromBytes accepts "_s, mutation.what));
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
