/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "BytecodeIndex.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <span>
#include <stdint.h>
#include <type_traits>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

// The Image lane's vocabulary (SPEC-image.md sections 3, 4.1, 5 and 8.5): how a reference is emitted (its form), what it
// names (its target), the record a compilation leaves, and the names of the checks that guard what the lane reads. No
// header JSC exports includes this one; JIT.h and the other exported headers declare the enumerations they need opaquely
// (SPEC-image.md section 14.4).

namespace JSC::JITCache {

// A fixup's form: the fixed-footprint instruction that holds the reference and the assembler link writer that patches it
// (SPEC-image.md table 3.2). The values are the form byte of a fixup entry (section 8.2, region 2).
enum class FixupForm : uint8_t {
    Pointer = 1,
    Call = 2,
    Jump = 3,
};

// What a reference names (SPEC-image.md table 3.3). The values are the kind byte of a fixup entry, numbered in the
// table's order (section 8.2, region 2).
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

// A reference's typed target. Every field a kind does not use is zero (SPEC-image.md section 3.3).
struct ImageTarget {
    TargetKind kind { };
    uint32_t a { 0 };
    uint32_t b { 0 };
    int64_t payload { 0 };

    friend bool operator==(const ImageTarget&, const ImageTarget&) = default;
};

// The support thunks whose generators are private members of JIT, keyed by this closed enumeration so that a key read
// from a file never names the code to run (SPEC-image.md section 3.4). JIT::baselineThunkGenerator maps each entry to
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
    GetFromScopeGlobalLexicalVar, // No baseline code links the ClosureVarWithVarInjectionChecks thunk (census D4).
    GetFromScopeGlobalVarWithVarInjectionChecks,
    GetFromScopeGlobalLexicalVarWithVarInjectionChecks,
    ConsistencyCheck, // A key only ASSERT_ENABLED builds emit, since only they have the generator.
};

// The process-wide thunks LLInt hands out (SPEC-image.md N22). DefaultCall is reached by a Pointer, ArityFixup by a Call.
enum class ProcessThunk : uint8_t {
    DefaultCall = 1,
    ArityFixup = 2,
};

// The VM data an image may reference, in the order of SPEC-image.md table 3.6; the value is the a of a VMAddress target.
// Every entry exists from VM construction and outlives every image of the VM (N16).
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

// The VM's cells an image may reference (SPEC-image.md table 3.6); the value is the a of a VMCell target.
enum class VMCell : uint8_t {
    EmptyString = 1,
    SmallStringsSentinel = 2,
};
inline constexpr uint8_t numberOfVMCells = static_cast<uint8_t>(VMCell::SmallStringsSentinel);

// Why a compilation's record is never captured (SPEC-image.md section 4.1). None of them is a fault.
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

// SPEC-image.md section 5. Complete means every reference in the image and in each MathIC's current snippet has exactly
// one fixup; the other two states are final, and a record in either is never captured.
enum class RecordState : uint8_t {
    Complete,
    Unrecordable,
    Incomplete,
};

// One reference: its site (the offset the form's link writer takes), its form and its target. Fixups are kept in
// footprint order: by site, and at a shared site the Call first (SPEC-image.md section 3.2).
struct ImageFixup {
    uint32_t site { 0 };
    FixupForm form { };
    ImageTarget target;

    friend bool operator==(const ImageFixup&, const ImageFixup&) = default;
};

// The provenance of one MathIC snippet.
struct SnippetProvenance {
    const void* start { nullptr }; // the snippet allocation this provenance describes
    uint32_t size { 0 }; // the snippet LinkBuffer's size(), its linked size (SPEC-image.md section 3.1)
    Vector<ImageFixup> fixups; // sites relative to the snippet start, in footprint order
};

// The kind byte of a MathIC entry (SPEC-image.md section 8.2, region 9).
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
    std::optional<uint32_t> slowCallPointerSite; // the slow call's Operation fixup; empty without inline code (section 6.1)
    std::optional<SnippetProvenance> snippet; // only for an IC with inline code
};

// The identifier of every check the lane runs (SPEC-image.md section 8.5); status(vm) reports the failing one through
// description(ImageCheck).
enum class ImageCheck : uint8_t {
    None, // an outcome no check names: NotEligible, ChargeRefused, ExecutableMemoryExhausted
    V1, V2, V3, V4, V5, V6, V7, // section 8.5, structure
    U1, U2, U3, U4, U5, U6, U7, // section 8.5, against the UCB
    W1, W2, W3, W4, // section 11.2, twins builds
    S1, S2, S3, S4, // section 9, capture
    S5, // section 10.3, step 10
};

// The check's name in lowercase, "v1" to "s5", or "none", with static storage (SPEC-integrator.md R-ALL-6).
// ImageSection.cpp defines it.
ASCIILiteral description(ImageCheck);

// What the lane's section writers stream bytes through: false once the receiver refuses, and the writer then stops.
using ImageSectionSink = ScopedLambda<bool(std::span<const uint8_t>)>;

// The lane's sections hold little-endian integers and doubles, read and written with memcpy-based loads and stores at
// any alignment (SPEC-image.md section 8.1).
static_assert(std::endian::native == std::endian::little);

namespace ImageBytes {

template<typename T>
inline T read(std::span<const uint8_t> bytes, size_t offset)
{
    static_assert(std::is_trivially_copyable_v<T>);
    T value;
    memcpySpan(asMutableByteSpan(value), bytes.subspan(offset, sizeof(T)));
    return value;
}

template<typename T>
inline void write(std::span<uint8_t> bytes, size_t offset, T value)
{
    static_assert(std::is_trivially_copyable_v<T>);
    memcpySpan(bytes.subspan(offset, sizeof(T)), asByteSpan(value));
}

inline bool isZero(std::span<const uint8_t> bytes)
{
    return std::ranges::all_of(bytes, [](uint8_t byte) { return !byte; });
}

} // namespace ImageBytes

} // namespace JSC::JITCache
