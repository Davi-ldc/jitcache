#pragma once

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ImageSupport.h"
#include "ImageTypes.h"
#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

// What test builds record so that an import's twin, compiled in the consumer, emits what the producer emitted: the
// random draws, every value emission read from mutable state, and each MathIC regeneration since the compilation. The
// record keeps them as its twin data, capture writes them to the image-twins.baseline section this file encodes and
// checks, and the twin check in ImageTwins.h replays them. The Image lane's recorder headers include this one; only
// ENABLE(JITCACHE_TWINS) builds have any of it.

namespace JSC {

class CodeBlock;
class UnlinkedCodeBlock;
enum OpcodeID : unsigned;

namespace JITCache {

class ImageSectionsView;

// The draws of one baseline compilation.
struct TwinSeeds {
    uint32_t assembler { 0 }; // Every baseline compilation draws it for its entry nop.
    Vector<uint32_t> binarySwitches; // One per BinarySwitch, in construction order.
};

// The values are the kind byte of a compile input in the section.
enum class CompileInputKind : uint8_t {
    ResolveScopeType = 1,
    GetFromScopeType,
    PutToScopeType,
    GetByIdMode,
    IteratorOpenMode,
    AsyncIteratorOpenMode,
    EnumeratorMetadata,
    StrictEqualityAtomOperand,
};

// The operand a strict-equality template compares inline by its atom, if any. Atom-ness only grows, so the twin cannot
// recreate it in its metadata; the template records its choice and the twin reads it back. The values are the value
// byte of a StrictEqualityAtomOperand compile input.
enum class StrictEqualityAtomOperand : uint8_t {
    None = 0,
    Lhs = 1,
    Rhs = 2,
};

// One value emission read at one instruction.
struct CompileInput {
    uint32_t bytecodeOffset { 0 };
    CompileInputKind kind { };
    uint8_t value { 0 }; // A ResolveType, a GetByIdMode, the enumerator byte or a StrictEqualityAtomOperand.
    uint32_t localScopeDepth { 0 }; // ResolveScopeType of type ClosureVar only, else 0.

    friend bool operator==(const CompileInput&, const CompileInput&) = default;
};

// Every value baseline emission reads from mutable state, besides the capability level and the taint, which travel as
// baked facts.
struct TwinCompileInputs {
    Vector<CompileInput> inputs; // Sorted by bytecode offset, one per instruction.
    Vector<uint16_t> binaryArithBits; // By binary arithmetic profile index, at compile start.
    Vector<uint16_t> unaryArithBits; // By unary arithmetic profile index, at compile start.
    // Whether the compilation ran on the thread holding the VM's API lock, so that no JS of the VM ran while it read these
    // inputs and they equal what emission read. The section stores it as flag bit 0.
    bool compiledHoldingAPILock { false };
};

// One native MathIC regeneration of a recorded image, logged when it starts, whatever then happens to its snippets.
struct TwinRegeneration {
    uint32_t mathICIndex { 0 };
    uint16_t profileBitsAtEntry { 0 };
    CodeSymbol replacement;
    // One slot per assembler the regeneration attaches, in attach order: the seed that assembler drew, or nothing when it
    // drew none.
    Vector<std::optional<uint32_t>, 2> assemblerSeeds;
};

// What a record keeps for its twin: the original compilation's seeds and inputs, and every regeneration since, the ones
// a ConsumerProducer's rebuilt record carried over from its import included.
struct TwinData {
    TwinSeeds seeds;
    TwinCompileInputs compileInputs;
    Vector<TwinRegeneration> regenerations;
};

// The heap bytes the containers of twin data hold at their capacities, which the record that keeps them is charged for.
size_t twinDataStorageBytes(const TwinData&);

// The kind of the input snapshotCompileInputs takes at an instruction with this opcode, if any: the metadata the
// emitters read from mutable state. The twin check uses it to write each input back where it came from.
std::optional<CompileInputKind> snapshotKindOf(OpcodeID);

// The compile inputs of the CodeBlock being compiled: its metadata through each instruction's metadata(CodeBlock*)
// accessor and its UCB's arithmetic profile bits, with whether the calling thread holds the VM's API lock. On the
// compile thread, once the recorder is attached and before the main pass. Every vector is at its exact size; the
// recorder charges them when it receives them.
TwinCompileInputs snapshotCompileInputs(CodeBlock&);

// This process's token, 16 bytes drawn once with cryptographicallyRandomValues at the first call, redrawn while all
// zero. Any thread; the first call is serialized with std::call_once. A process forked from another inherits it with the
// address space it describes.
inline constexpr size_t captureProcessTokenSize = 16;
std::span<const uint8_t, captureProcessTokenSize> captureProcessToken();

// The image-twins.baseline section: a 48-byte header, then the seeds, the compile inputs, the two arithmetic profile
// vectors, the regeneration log and the producer values, each region starting 8-byte aligned.
inline constexpr size_t imageTwinsHeaderSize = 48;
inline constexpr size_t imageTwinsCompileInputEntrySize = 12;
inline constexpr size_t imageTwinsRegenerationFixedSize = 16; // MathIC index, profile bits, attach count, mask, replacement

namespace ImageTwinsFlags {
inline constexpr uint8_t compiledHoldingAPILock = 1 << 0;
} // namespace ImageTwinsFlags

// A regeneration entry: its fixed fields and one u32 seed per attach, padded to 8.
constexpr size_t imageTwinsRegenerationEntrySize(unsigned attachCount)
{
    return (imageTwinsRegenerationFixedSize + 4 * static_cast<size_t>(attachCount) + 7) & ~static_cast<size_t>(7);
}

struct ImageTwinsHeader {
    uint8_t flags { 0 }; // ImageTwinsFlags
    uint32_t assemblerSeed { 0 };
    uint32_t binarySwitchSeedCount { 0 };
    uint32_t inputCount { 0 };
    uint32_t binaryArithProfileCount { 0 };
    uint32_t unaryArithProfileCount { 0 };
    uint32_t regenerationCount { 0 };
    uint32_t producerValueCount { 0 };
    std::array<uint8_t, captureProcessTokenSize> captureProcessToken { };

    friend bool operator==(const ImageTwinsHeader&, const ImageTwinsHeader&) = default;
};

// A located image-twins.baseline: a span into the borrowed payload, whose accessors read with memcpy-based loads on
// access. Under strict it passed the structure checks, ImageCheck::W1 to W3.
class ImageTwinsView {
public:
    ImageTwinsView() = default;

    // The regions the header's counts imply, or nullopt when they do not fit inside the span; such a section fails
    // ImageCheck::W1 in either mode, as a body without the section does in a twins build.
    static std::optional<ImageTwinsView> locate(std::span<const uint8_t>);

    const ImageTwinsHeader& header() const LIFETIME_BOUND { return m_header; }
    bool compiledHoldingAPILock() const { return m_header.flags & ImageTwinsFlags::compiledHoldingAPILock; }
    std::span<const uint8_t, captureProcessTokenSize> captureProcessToken() const LIFETIME_BOUND { return m_header.captureProcessToken; }
    uint32_t binarySwitchSeed(unsigned index) const;
    CompileInput compileInput(unsigned index) const;
    uint16_t binaryArithBits(unsigned index) const;
    uint16_t unaryArithBits(unsigned index) const;
    // functor(unsigned index, const TwinRegeneration&), in regeneration order.
    template<typename Functor> void forEachRegeneration(const Functor&) const;
    uint64_t producerValue(unsigned index) const; // the image's fixups in footprint order, then each snippet's
    std::span<const uint8_t> bytes() const { return m_bytes; }

    // The twin data these bytes hold, every container at its exact size, and the heap bytes it allocates, which a rebuilt
    // record is charged for before it decodes them.
    TwinData twinData() const;
    size_t twinDataStorageBytes() const;

    friend bool operator==(const ImageTwinsView&, const ImageTwinsView&);

private:
    friend std::optional<ImageCheck> firstTwinsStructureFailure(const ImageSectionsView&);

    // Offsets into the section: each region's start, and the end of the bytes before the padding that follows it.
    struct Layout {
        size_t binarySwitchSeeds { 0 };
        size_t binarySwitchSeedsEnd { 0 };
        size_t inputs { 0 };
        size_t inputsEnd { 0 };
        size_t binaryArithBits { 0 };
        size_t binaryArithBitsEnd { 0 };
        size_t unaryArithBits { 0 };
        size_t unaryArithBitsEnd { 0 };
        size_t regenerations { 0 };
        size_t producerValues { 0 };
        size_t end { 0 };

        friend bool operator==(const Layout&, const Layout&) = default;
    };

    std::span<const uint8_t> regenerationEntry(size_t offset) const;
    static TwinRegeneration decodeRegeneration(std::span<const uint8_t> entry);

    bool passesW1() const;
    bool passesW2() const;
    bool passesW3(const ImageSectionsView&) const;

    std::span<const uint8_t> m_bytes;
    ImageTwinsHeader m_header;
    Layout m_layout;
};

// ImageCheck::W1 to W3 on the view's twins section, which needs the parsed image section for W3. parseImageSections runs
// it under strict, after the image's own structure checks; with strict off, debug builds ASSERT it.
std::optional<ImageCheck> firstTwinsStructureFailure(const ImageSectionsView&);
// ImageCheck::W4, the compile inputs against the import's UCB. validateImageSectionsAgainst runs it after the image's
// checks against the UCB.
std::optional<ImageCheck> firstTwinsFailureAgainst(const ImageSectionsView&, const UnlinkedCodeBlock&);

// The writer capture streams the section through: the record's twin data, the producer values capture computed by
// resolving every fixup, and the capturing process's token, with no buffer of its own; false once the sink refuses.
uint64_t twinsSectionSize(const TwinData&, size_t producerValueCount);
[[nodiscard]] bool writeTwinsSection(const TwinData&, std::span<const uint64_t> producerValues, std::span<const uint8_t, captureProcessTokenSize> token, const ImageSectionSink&);

// One recorded regeneration as the twin replays it: the seeds its assemblers drew, in attach order, and whether a
// snippet allocation failed, which the replay reports instead of raising a fault.
class TwinReplay {
    WTF_MAKE_NONCOPYABLE(TwinReplay);
public:
    explicit TwinReplay(const TwinRegeneration& regeneration)
        : m_regeneration(regeneration)
    {
    }

    const TwinRegeneration& regeneration() const LIFETIME_BOUND { return m_regeneration; }
    // At each attach, before anything is emitted: the seed the next slot holds, or nullopt when the producer's assembler
    // drew none there or the producer attached fewer times.
    std::optional<uint32_t> takeAttachSeed();
    unsigned attachCount() const { return m_attachCount; }
    void markFailed() { m_failed = true; }
    bool failed() const { return m_failed; }

private:
    const TwinRegeneration& m_regeneration;
    unsigned m_attachCount { 0 };
    bool m_failed { false };
};

template<typename Functor>
void ImageTwinsView::forEachRegeneration(const Functor& functor) const
{
    // locate kept every entry inside the region.
    size_t offset = m_layout.regenerations;
    for (unsigned index = 0; index < m_header.regenerationCount; ++index) {
        auto entry = regenerationEntry(offset);
        functor(index, decodeRegeneration(entry));
        offset += entry.size();
    }
}

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
