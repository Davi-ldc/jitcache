#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "jitcache/image/ImageFootprint.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageSupport.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageSectionTestSupport.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <wtf/text/MakeString.h>

// The Image lane's tests that need no VM: the token status reports for each check (image/ImageTypes.h), the
// CodeSymbol that names a function of the engine (image/ImageSupport.h), and each form's footprint, canonical encoding
// and decoding on the build's architecture (image/ImageFootprint.h).

namespace JSC::JITCache::Tests {

// Each check's token, the failing step status shows after the part, and no two checks share one.
JITCACHE_TEST(imageCheckDescriptions, No)
{
    struct Named {
        ImageCheck check;
        ASCIILiteral token;
    };
    static constexpr std::array checks {
        Named { ImageCheck::Header, "header"_s },
        Named { ImageCheck::BakedFacts, "baked-facts"_s },
        Named { ImageCheck::TwinsHeader, "twins-header"_s },
    };
    // The table lists every enumerator once, in declaration order, so a check added without a row fails here.
    static_assert(static_cast<size_t>(ImageCheck::TwinsHeader) + 1 == checks.size());
    for (size_t index = 0; index < checks.size(); ++index) {
        JITCACHE_CHECK(static_cast<size_t>(checks[index].check) == index);
        if (description(checks[index].check) != checks[index].token)
            JITCACHE_FAIL(makeString("check "_s, index, " has the token "_s, description(checks[index].check), ", not "_s, checks[index].token));
        for (size_t other = 0; other < index; ++other) {
            if (description(checks[other].check) == description(checks[index].check))
                JITCACHE_FAIL(makeString("checks "_s, other, " and "_s, index, " share the token "_s, description(checks[index].check)));
        }
    }
}

JITCACHE_TEST(imageCodeSymbols, No)
{
    auto anchor = CodeSymbol::of(&codeSymbolAnchor);
    JITCACHE_CHECK(anchor && !anchor->offset);
    auto parse = CodeSymbol::of(&parseImageSections);
    JITCACHE_CHECK(parse && CodeSymbol::isValid(parse->offset));
    if (parse)
        JITCACHE_CHECK(parse->address() == std::bit_cast<const void*>(&parseImageSections));
    int local = 0;
    JITCACHE_CHECK(!CodeSymbol::of(&local));
    JITCACHE_CHECK(!CodeSymbol::of(static_cast<const void*>(nullptr)));
    JITCACHE_CHECK(!CodeSymbol::isValid(std::numeric_limits<int64_t>::max()));
    JITCACHE_CHECK(!CodeSymbol::isValid(std::numeric_limits<int64_t>::min()));
#if CPU(ARM64)
    if (parse)
        JITCACHE_CHECK(!CodeSymbol::isValid(parse->offset + 2));
#endif
}

// Each form's footprint, canonical encoding and decoding on the build's architecture.
JITCACHE_TEST(imageSectionFootprints, No)
{
    using namespace ImageSectionTestSupport;
    TestCode code;
    code.fill(16);
    code.pointer(makeTarget(TargetKind::VMAddress, 1), 3);
    code.call(makeTarget(TargetKind::CommonThunk, 0));
    code.jump(makeTarget(TargetKind::CommonThunk, 0));
#if CPU(X86_64)
    code.jump(makeTarget(TargetKind::CommonThunk, 0), true);
    constexpr std::array<size_t, 4> expectedSizes { 10, 5, 5, 6 };
    constexpr uint64_t expectedPointer = 0x1122334455667788;
#else
    constexpr std::array<size_t, 3> expectedSizes { 12, 4, 4 };
    constexpr uint64_t expectedPointer = 0x9ABC56781234;
#endif
    code.fill(fillUnit * 4);

    auto bytes = code.bytes();
    JITCACHE_CHECK(code.fixups().size() == expectedSizes.size());
    for (size_t index = 0; index < std::min(code.fixups().size(), expectedSizes.size()); ++index) {
        auto& fixup = code.fixups()[index];
        auto footprint = fixupFootprint(fixup.form, fixup.site, bytes);
        if (!footprint) {
            JITCACHE_FAIL(makeString("fixup "_s, index, " has no footprint"_s));
            continue;
        }
        JITCACHE_CHECK(footprint->size() == expectedSizes[index]);
        auto live = bytes.subspan(footprint->begin, footprint->size());
        auto canonical = canonicalFootprint(fixup.form, live);
        JITCACHE_CHECK(!isCanonicalFootprint(fixup.form, live));
        JITCACHE_CHECK(isCanonicalFootprint(fixup.form, canonical.span()));
        JITCACHE_CHECK(canonical.size == live.size());
        // A footprint that lies past the code has no extent.
        JITCACHE_CHECK(!fixupFootprint(fixup.form, fixup.site, bytes.first(footprint->end - 1)));

        auto decoded = decodeFootprint(fixup.form, live, footprint->begin);
        auto decodedCanonical = decodeFootprint(fixup.form, canonical.span(), footprint->begin);
        JITCACHE_CHECK(decoded && decodedCanonical);
        if (!decoded || !decodedCanonical)
            continue;
        switch (fixup.form) {
        case FixupForm::Pointer:
            JITCACHE_CHECK(*decoded == expectedPointer);
            JITCACHE_CHECK(!*decodedCanonical);
            break;
        case FixupForm::Call:
        case FixupForm::Jump:
#if CPU(X86_64)
            // rel32 from the end of the instruction, which is the site.
            JITCACHE_CHECK(*decoded == fixup.site + (fixup.form == FixupForm::Call ? 0x01020304u : 0x05060708u));
            JITCACHE_CHECK(*decodedCanonical == fixup.site);
#else
            // imm26 in instructions from the branch itself.
            JITCACHE_CHECK(*decoded == footprint->begin + 4 * (fixup.form == FixupForm::Call ? 0x40u : 0x80u));
            JITCACHE_CHECK(*decodedCanonical == footprint->begin);
#endif
            break;
        }
    }

#if CPU(X86_64)
    JITCACHE_CHECK(farCallPointerSite(16) == 13u);
    JITCACHE_CHECK(!farCallPointerSite(2));
    JITCACHE_CHECK(snippetEntrySite(8) == 13u);
#else
    JITCACHE_CHECK(farCallPointerSite(16) == 0u);
    JITCACHE_CHECK(!farCallPointerSite(12));
    JITCACHE_CHECK(snippetEntrySite(8) == 8u);
#endif
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
