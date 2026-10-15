#pragma once

#if ENABLE(JIT)

#include "ImageSupport.h"
#include "ImageTypes.h"
#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Compiler.h>
#include <wtf/StdLibExtras.h>

// Footprints: the fixed bytes of code each fixup form occupies, and how they are canonicalized, checked and decoded.
// These functions read and write the instruction bytes of the build's architecture, x86_64 or ARM64; on any other the
// footprints are undefined and nothing is canonical. They are the only part of the image format that changes with the
// architecture, and the section codec, the recorder, capture, preparation and the twin check all call them. Pure
// functions over bytes, safe on any thread.

namespace JSC::JITCache {

inline constexpr size_t maximumFootprintSize = 12;

struct FixupFootprint {
    size_t begin { 0 };
    size_t end { 0 };
    size_t size() const { return end - begin; }
};

// The bytes of code a fixup with this form and site occupies, or nullopt when they do not lie inside code. An x86_64
// Jump is five bytes when site-5 holds E9 and six when site-6 holds 0F 80 to 0F 8F, and nullopt when neither holds.
std::optional<FixupFootprint> fixupFootprint(FixupForm, uint32_t site, std::span<const uint8_t> code);

struct CanonicalFootprint {
    std::array<uint8_t, maximumFootprintSize> bytes { };
    uint8_t size { 0 };
    std::span<const uint8_t> span() const LIFETIME_BOUND { return std::span { bytes }.first(size); }
};

// The footprint's bytes with the form's variable field zeroed and every other bit kept: the 64-bit immediate of an
// x86_64 movabs, the rel32 of a call or jump; the imm16 of each ARM64 movz and movk, the imm26 of a bl or b. Capture
// writes this in place of every footprint.
CanonicalFootprint canonicalFootprint(FixupForm, std::span<const uint8_t> footprint);
// Whether the footprint holds exactly its form's canonical encoding, so the form's link writer finds the instruction it
// expects. The section's structure checks run this on every footprint.
bool isCanonicalFootprint(FixupForm, std::span<const uint8_t> footprint);
// What a footprint holds once patched: the pointer of a Pointer, or the branch target of a Call or a Jump given the
// footprint's own address. Nullopt when the bytes do not hold the form's instruction (its opcodes, and the same Xd across
// an ARM64 Pointer's three words). Jump islands are the caller's to follow.
std::optional<uintptr_t> decodeFootprint(FixupForm, std::span<const uint8_t> footprint, uintptr_t footprintAddress);
// Whether a footprint of executable memory at footprintAddress holds its form's instruction and reaches expected: a
// Pointer holds it, and a Call or a Jump branches to it, on ARM64 through the unconditional b of each jump island on the
// way, at most the pool's size divided by half of MacroAssembler::nearJumpRange, rounded up, which bounds the chains
// FixedVMPoolExecutableAllocator::islandForJumpLocation builds for any pool size.
bool footprintReaches(FixupForm, std::span<const uint8_t> footprint, uintptr_t footprintAddress, uintptr_t expected);
// Whether every fixup of the code at start, up to its linked size, decodes as its form to the resolution of its target in
// the context: at capture, after preparation patches, and on each side of the twin check's decodes. Fixups is a
// Vector<ImageFixup> or an ImageFixupArray, in footprint order.
template<typename Fixups>
bool fixupsReachTargets(const void* start, size_t linkedSize, const Fixups&, const ResolutionContext&);

// The site of the Operation fixup of the far call whose return point is callReturnOffset: the offset minus
// REPATCH_OFFSET_CALL_R11 on x86_64, minus (NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS + 1) * 4 on ARM64. Nullopt when the
// subtraction would leave the code.
std::optional<uint32_t> farCallPointerSite(uint32_t callReturnOffset);
// The site of the SnippetEntry fixup the rewrite of a MathIC's inline start leaves: inlineStart on ARM64, inlineStart + 5
// on x86_64.
uint64_t snippetEntrySite(uint32_t inlineStart);

template<typename Fixups>
bool fixupsReachTargets(const void* start, size_t linkedSize, const Fixups& fixups, const ResolutionContext& context)
{
    auto code = unsafeMakeSpan(static_cast<const uint8_t*>(start), linkedSize);
    auto codeStart = reinterpret_cast<uintptr_t>(start);
    for (size_t index = 0; index < fixups.size(); ++index) {
        ImageFixup fixup = fixups[index];
        auto footprint = fixupFootprint(fixup.form, fixup.site, code);
        if (!footprint)
            return false;
        auto expected = reinterpret_cast<uintptr_t>(resolveTarget(context, fixup.target));
        if (!footprintReaches(fixup.form, code.subspan(footprint->begin, footprint->size()), codeStart + footprint->begin, expected))
            return false;
    }
    return true;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
