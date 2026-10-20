#pragma once

#include "BytecodeIndex.h"
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/ScopedLambda.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

// The Image lane's vocabulary: how a reference is emitted (its form), what it names (its target), the record a
// compilation leaves, and the names of the checks that guard what the lane reads. No header JSC exports includes this
// one; JIT.h and the other exported headers declare the enumerations they need opaquely, so that no exported class
// depends on the lane's types.

namespace JSC::JITCache {

// A fixup's form: the fixed-footprint instruction that holds the reference and the assembler link writer that patches
// it. ImageFootprint.h encodes each form for each architecture. The values are the form byte of a fixup entry.
enum class FixupForm : uint8_t {
    Pointer = 1,
    Call = 2,
    Jump = 3,
};

// What a reference names. The values are the kind byte of a fixup entry; the comment on each says which fields the
// kind uses.
enum class TargetKind : uint8_t {
    Operation = 1, // payload: CodeSymbol of the C++ function
    CommonThunk, // a: CommonJITThunkID
    BaselineThunk, // a: BaselineThunk
    SlowPathThunk, // payload: CodeSymbol of the SlowPathFunction
    InlineCacheSlowPathThunk, // a: AccessType
    VirtualCallThunk, // a: CallMode
    ProcessThunk, // a: ProcessThunk
    VMAddress, // a: VMAddress
    VMCell, // a: VMCell
    StructureIDBase, // no field
    UCBConstantCell, // a: constant index
    UCBConstantAtom, // a: constant index
    UCBIdentifier, // a: identifier index
    UCBBinaryArithProfile, // a: binary arithmetic profile index
    UCBUnaryArithProfile, // a: unary arithmetic profile index
    SwitchStringRankAtom, // a: string switch table, b: rank
    SwitchStringRankCase, // a: string switch table, b: rank
    MathIC, // a: MathIC index
    SwitchTableBase, // a: simple switch table
    ImageOffset, // a: offset into the image; snippets only
    SnippetEntry, // a: MathIC index; the image only
};
inline constexpr uint8_t numberOfTargetKinds = static_cast<uint8_t>(TargetKind::SnippetEntry);

// A reference's typed target. Every field a kind does not use is zero.
struct ImageTarget {
    TargetKind kind { };
    uint32_t a { 0 };
    uint32_t b { 0 };
    int64_t payload { 0 };

    friend bool operator==(const ImageTarget&, const ImageTarget&) = default;
};

// The support thunks whose generators are private members of JIT, keyed by this closed enumeration so that a key read
// from a file never names the code to run. JIT::baselineThunkGenerator maps each entry to
// its generator. JIT.h declares this enumeration opaquely with the same underlying type.
enum class BaselineThunk : uint8_t {
    OpEnterHandler = 1,
    OpCheckTrapsHandler,
    OpThrowHandler,
    ValueIsTruthy,
    ValueIsFalsey,
    SlowOpPutToScope,
    ResolveScopeClosureVarWithVarInjectionChecks,
    ResolveScopeGlobalVar,
    ResolveScopeGlobalProperty,
    ResolveScopeGlobalLexicalVar,
    ResolveScopeGlobalVarWithVarInjectionChecks,
    ResolveScopeGlobalPropertyWithVarInjectionChecks,
    ResolveScopeGlobalLexicalVarWithVarInjectionChecks,
    GetFromScopeGlobalVar,
    GetFromScopeGlobalProperty,
    GetFromScopeGlobalLexicalVar, // No baseline code links the ClosureVarWithVarInjectionChecks thunk.
    GetFromScopeGlobalVarWithVarInjectionChecks,
    GetFromScopeGlobalLexicalVarWithVarInjectionChecks,
    ConsistencyCheck, // A key only ASSERT_ENABLED builds emit, since only they have the generator.
};

// The process-wide thunks LLInt hands out. DefaultCall is reached by a Pointer, ArityFixup by a Call.
enum class ProcessThunk : uint8_t {
    DefaultCall = 1,
    ArityFixup = 2,
};

// The VM data an image may reference; the value is the a of a VMAddress target. Every entry exists from VM construction
// and outlives every image of the VM.
enum class VMAddress : uint8_t {
    VM = 1,
    SoftStackLimit,
    TrapBits,
    Exception,
    TopEntryFrame,
    TargetMachinePCForThrow,
    TopCallFrame, // Only debug builds reference it.
    MightBeExecutingTaintedCode,
    BarrierThreshold,
    MutatorShouldBeFenced, // Only ARM64 code references it.
    SyncResumeCallCache,
};
inline constexpr uint8_t numberOfVMAddresses = static_cast<uint8_t>(VMAddress::SyncResumeCallCache);

// The VM's cells an image may reference; the value is the a of a VMCell target. Every entry exists from VM construction
// and outlives every image of the VM. The two iteration sentinels are the iterators a fast-array and a fast-string
// iteration run with, which iterator_next and iterator_close_check compare.
enum class VMCell : uint8_t {
    EmptyString = 1,
    SmallStringsSentinel = 2,
    FastArraySentinel = 3,
    FastStringSentinel = 4,
};
inline constexpr uint8_t numberOfVMCells = static_cast<uint8_t>(VMCell::FastStringSentinel);

// Why a compilation's record is never captured. None of them is a fault.
enum class Unrecordable : uint8_t {
    None, // The record is not Unrecordable.
    NotShareable,
    SuperSamplerOpcode,
    UnannotatedSupportLink,
    UnannotatedPointerArgument,
    UnannotatedReference,
    ForeignCodeSymbol,
    UnknownVMAddress,
    InconsistentRecord,
    UnknownIdentifier,
    MathICProvenance,
};

// A record's state. Complete means every reference in the image and in each MathIC's current snippet has exactly one
// fixup; the other two states are final, and a record in either is never captured.
enum class RecordState : uint8_t {
    Complete,
    Unrecordable,
    Incomplete,
};

// One reference: its site (the offset the form's link writer takes), its form and its target. Fixups are kept in
// footprint order: by site, and at a shared site the Call first.
struct ImageFixup {
    uint32_t site { 0 };
    FixupForm form { };
    ImageTarget target;

    friend bool operator==(const ImageFixup&, const ImageFixup&) = default;
};

// The provenance of one MathIC snippet.
struct SnippetProvenance {
    const void* start { nullptr }; // the snippet allocation this provenance describes
    uint32_t size { 0 }; // the snippet LinkBuffer's size(), its linked size
    Vector<ImageFixup> fixups; // sites relative to the snippet start, in footprint order
};

// The kind byte of a MathIC entry.
enum class MathICKind : uint8_t {
    Add = 1,
    Sub = 2,
    Mul = 3,
    Negate = 4,
};

struct MathICRecord {
    MathICKind kind { };
    BytecodeIndex bytecodeIndex;
    void* mathIC { nullptr }; // JITAddIC*, JITSubIC*, JITMulIC* or JITNegIC*, owned by the BaselineJITCode
    std::optional<uint32_t> slowCallPointerSite; // the slow call's Operation fixup; empty without inline code
    std::optional<SnippetProvenance> snippet; // only for an IC with inline code
};

// Every check the lane runs; status(vm) reports the failing one through description(ImageCheck).
enum class ImageCheck : uint8_t {
    None, // an outcome no check names: NotEligible, ChargeRefused, ExecutableMemoryExhausted
    // The structure of image.baseline, an import's first checks, then of baked-facts.baseline.
    Header, CodeBounds, Fixups, SideTables, MathICEntries, MathICAndSwitchTargets, BakedFacts,
    // image.baseline against the import's UCB.
    SimpleSwitchTables, StringSwitchTables, UCBIndexes, BytecodeIndexes, CodeMapCount, SwitchRanks, ConstantTargets,
    // image-twins.baseline in twins builds: its structure, then its compile inputs against the import's UCB.
    TwinsHeader, TwinInputs, TwinRegenerations, TwinInputSites,
    // A capture: fixups reach their targets, and the code's MathICs and side tables match the record.
    CodeFixupTargets, SnippetFixupTargets, MathICCount, SideTablePointers,
    // Preparation: every patched fixup reaches its target.
    PatchReadBack,
};

// The failing step status(vm) shows after the part, one hyphenated lowercase token per check such as "patch-read-back",
// or "none", with static storage. ImageSection.cpp defines it.
ASCIILiteral description(ImageCheck);

// What the lane's section writers stream bytes through: false once the receiver refuses, and the writer then stops.
using ImageSectionSink = ScopedLambda<bool(std::span<const uint8_t>)>;

} // namespace JSC::JITCache
