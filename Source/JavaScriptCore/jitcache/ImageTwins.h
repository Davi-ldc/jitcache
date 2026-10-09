#pragma once

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ImageSupport.h"
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Vector.h>

// What test builds record so that an import's twin, compiled in the consumer, emits what the producer emitted
// (SPEC-image.md section 11.1): the random draws, every value emission read from mutable state, and each MathIC
// regeneration since the compilation. The record keeps them as its twin data, capture writes them to the
// image-twins.baseline section, and the twin check replays them. Only ENABLE(JITCACHE_TWINS) builds have any of it.

namespace JSC {

class CodeBlock;

namespace JITCache {

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

// The compile inputs of the CodeBlock being compiled: its metadata through each instruction's metadata(CodeBlock*)
// accessor and its UCB's arithmetic profile bits, with whether the calling thread holds the VM's API lock. On the
// compile thread, once the recorder is attached and before the main pass.
TwinCompileInputs snapshotCompileInputs(CodeBlock&);

// This process's token, 16 bytes drawn once with cryptographicallyRandomValues at the first call, redrawn while all
// zero. Any thread; the first call is serialized with std::call_once.
std::span<const uint8_t, 16> captureProcessToken();

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
