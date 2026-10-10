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

// What test builds record so that an import's twin, compiled in the consumer, emits what the producer emitted
// (SPEC-image.md section 11.1): the random draws, every value emission read from mutable state, and each MathIC
// regeneration since the compilation. The record keeps them as its twin data, capture writes them to the
// image-twins.baseline section (section 11.2), and the twin check replays them (section 11.3). Only
// ENABLE(JITCACHE_TWINS) builds have any of it.

namespace JSC {

class BaselineJITCode;
class CodeBlock;
class JSScope;
class UnlinkedCodeBlock;
class VM;

namespace JITCache {

class ImageSectionsView;
class TwinReport;

// The draws of one baseline compilation.
struct TwinSeeds {
    uint32_t assembler { 0 }; // Every baseline compilation draws it for its entry nop.
    Vector<uint32_t> binarySwitches; // One per BinarySwitch, in construction order.
};

// The values are the kind byte of a compile input (section 11.2, region 2).
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

// The operand a strict-equality template compares inline by its atom (SPEC-image.md N25), if any. The values are the
// value byte of a kind-8 compile input.
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
    // inputs and they equal what emission read (section 11.2, flag bit 0).
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

// What a record keeps for its twin (section 5): the original compilation's seeds and inputs, and every regeneration
// since, the ones a ConsumerProducer's rebuilt record carried over from its import included.
struct TwinData {
    TwinSeeds seeds;
    TwinCompileInputs compileInputs;
    Vector<TwinRegeneration> regenerations;
};

// The heap bytes the containers of twin data hold at their capacities, which a record charges (section 4.8).
size_t twinDataStorageBytes(const TwinData&);

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

// The image-twins.baseline section (section 11.2): a 48-byte header, then the seeds, the compile inputs, the two
// arithmetic profile vectors, the regeneration log and the producer values, each region starting 8-byte aligned.
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
// access. Under strict it passed W1 to W3.
class ImageTwinsView {
public:
    ImageTwinsView() = default;

    // The regions the header's counts imply, or nullopt when they do not fit inside the span; such a section fails W1 in
    // either mode, as a body without the section does in a twins build.
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
    // record charges before decoding it (section 10.3, step 14).
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

// W1 to W3 on the view's twins section, which needs the parsed image section for W3 (section 11.2). parseImageSections
// runs it under strict, after V1 to V7; with strict off, debug builds ASSERT it.
std::optional<ImageCheck> firstTwinsStructureFailure(const ImageSectionsView&);
// W4 against the import's UCB. validateImageSectionsAgainst runs it after U1 to U7.
std::optional<ImageCheck> firstTwinsFailureAgainst(const ImageSectionsView&, const UnlinkedCodeBlock&);

// The writer capture streams the section through (section 9): the record's twin data, the producer values of section
// 9, step 6, and the capturing process's token, with no buffer of its own; false once the sink refuses.
uint64_t twinsSectionSize(const TwinData&, size_t producerValueCount);
[[nodiscard]] bool writeTwinsSection(const TwinData&, std::span<const uint64_t> producerValues, std::span<const uint8_t, captureProcessTokenSize> token, const ImageSectionSink&);

// One recorded regeneration as the twin replays it (section 11.3, step 5): the seeds its assemblers drew, in attach
// order, and whether a snippet allocation failed, which the replay reports instead of raising a fault.
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

// The image twin check of section 11.3. The integrator owns one per VM (R-INT-11) and calls checkImage at the end of
// ScriptExecutable::prepareForExecutionImpl, after installCode and before JS runs, for each installed import. It holds no
// state: each twin dies with its check.
class Twins {
public:
    // Reports every difference, every skip and each equal relocation pair to the report. On the VM thread with the API
    // lock; the payload the view reads stays alive until it returns (R-INT-11). The view carries the parsed twins section.
    void checkImage(VM&, CodeBlock& installed, JSScope*, const BaselineJITCode& restored, const ImageSectionsView&, TwinReport&);
};

// The process-wide registry of live twin CBs, under its own lock, so that CodeBlock::~CodeBlock can tell a twin apart on
// whichever thread sweeps; the set and its lock are never destroyed, since that destructor runs during process exit too.
void rememberImageTwin(CodeBlock&);
bool forgetImageTwin(CodeBlock&); // Erases the CB and returns whether it was a twin; CodeBlock::~CodeBlock calls it first.
size_t imageTwinCountForTesting(); // T21

// The test hooks of section 16.1, one per process: set before the process's first VM, read-only afterward.
enum class ImageTestHook : uint8_t {
    None,
    RelocationPairs, // checkImage forces one equal relocation pair in each domain the body's fixups reach (T3)
    OperationPair, // checkImage forces one for the body's first Operation fixup, which the clause skips (T3)
    ChangeRecordedTarget, // capture changes the target of the record's first fixup before S1 runs (T7)
    SkipPatch, // prepareImage skips its first patch (T7)
};
void setImageTestHook(ImageTestHook);
ImageTestHook imageTestHook();

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
