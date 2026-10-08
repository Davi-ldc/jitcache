/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(JIT)

#include "ArithProfile.h"
#include "BinarySwitch.h"
#include "CCallHelpers.h"
#include "ImageTypes.h"
#include "VirtualRegister.h"
#include <optional>
#include <wtf/Noncopyable.h>

namespace JSC {

class UnlinkedCodeBlock;
class VM;

namespace JITCache {

class ImageRecorder;

// The helpers every baseline emitter uses for a reference: a value that depends on the process, the VM, a UCB or another
// allocation. Each finds the recorder through the assembler's jitCacheRecorder(). Without one it emits exactly the
// native sequence; with one it emits the reference in its form's fixed footprint, unblinded and unfolded, and records a
// fixup there (SPEC-image section 4.3). After the recorder stops recording, the helpers keep emitting the recorded forms,
// which are correct code.

// A reference's typed target and the value it has in this process. A factory that takes a VM computes the value with
// resolveTarget, the function the consumer resolves the target with.
class ImageReference final : public CCallHelpers::ConstantMaterializer {
public:
    ImageReference(const ImageTarget&, const void* value);
    static ImageReference vmAddress(VM&, VMAddress);
    static ImageReference vmCell(VM&, VMCell);
    static ImageReference processThunk(ProcessThunk); // DefaultCall only.
    static ImageReference ucbConstantCell(VM&, UnlinkedCodeBlock&, VirtualRegister);
    static ImageReference ucbConstantAtom(VM&, UnlinkedCodeBlock&, VirtualRegister);
    static ImageReference ucbIdentifier(VM&, UnlinkedCodeBlock&, unsigned identifierIndex);
    static ImageReference arithProfile(VM&, UnlinkedCodeBlock&, const BinaryArithProfile&);
    static ImageReference arithProfile(VM&, UnlinkedCodeBlock&, const UnaryArithProfile&);
    static ImageReference mathIC(const void* mathIC); // Its index comes from the recorder that records it.
    static ImageReference switchTableBase(unsigned tableIndex, const void* base);

    void materialize(CCallHelpers&, GPRReg) const; // For setupArguments.
    void store(CCallHelpers&, CCallHelpers::Address) const; // For an argument setupArguments pokes.
    const void* value() const { return m_value; }
    const ImageTarget& target() const { return m_target; }

    // The target recorder records for this reference: the MathIC index it noted for a MathIC reference, and nullopt for
    // a profile outside its UCB's vectors, a programming error that makes the record unrecordable.
    std::optional<ImageTarget> recordedTarget(const ImageRecorder&) const;

private:
    enum class TargetState : uint8_t { Known, MathICIndexFromRecorder, Unknown };
    ImageReference(const ImageTarget&, const void* value, TargetState);
    static ImageReference arithProfileIn(VM&, UnlinkedCodeBlock&, const void* profile, TargetKind);

    ImageTarget m_target;
    const void* m_value;
    TargetState m_targetState { TargetState::Known };
};

enum class StoreValueKind : uint8_t { Value, Trusted }; // AssemblyHelpers::storeValue or storeTrustedValue.

// moveReference serves the native trusted moves. A site whose native code moves or stores a cell as an untrusted JSValue,
// which on x86_64 draws from the assembler's random source and may blind it, goes through moveReferenceValue or
// storeReferenceValue, so that its native sequence and every later blinding decision stay as they were.
void moveReference(MacroAssembler&, const ImageReference&, GPRReg);
MacroAssembler::Jump branchPtrWithReference(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg left, const ImageReference& right);
void storeReferenceValue(AssemblyHelpers&, const ImageReference& cell, MacroAssembler::Address, StoreValueKind);
void moveReferenceValue(AssemblyHelpers&, const ImageReference& cell, GPRReg);

// The absolute-address forms: the address is a Pointer into the temp (r11 on x86_64, the memory temp x17 on ARM64),
// and the operation then works on Address(temp).
MacroAssembler::Jump branchPtrAtReference(MacroAssembler&, MacroAssembler::RelationalCondition, const ImageReference& address, GPRReg right);
MacroAssembler::Jump branchTestPtrAtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address);
MacroAssembler::Jump branchTest32AtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address, MacroAssembler::TrustedImm32 mask);
MacroAssembler::Jump branchTest8AtReference(MacroAssembler&, MacroAssembler::ResultCondition, const ImageReference& address);
MacroAssembler::Jump branch32WithReferenceAt(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg left, const ImageReference& rightAddress);
void store8AtReference(MacroAssembler&, MacroAssembler::TrustedImm32, const ImageReference& address);
void storePtrAtReference(MacroAssembler&, GPRReg, const ImageReference& address);
void loadPtrAtReference(MacroAssembler&, const ImageReference& address, GPRReg);
// On ARM64 a recorded immediate mask must be a valid 32-bit logical immediate, which every baseline mask is.
void or16AtReference(MacroAssembler&, MacroAssembler::TrustedImm32, const ImageReference& address);
void or16AtReference(MacroAssembler&, GPRReg, const ImageReference& address);
void orStructureIDBase(MacroAssembler&, GPRReg source, GPRReg dest);

// Support code, reached through the key the consumer resolves (SPEC-image section 3.4).
void nearCallSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void nearTailCallSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void jumpSupport(MacroAssembler&, VM&, const ImageTarget& supportKey);
void linkJumpToSupport(MacroAssembler&, VM&, MacroAssembler::Jump, const ImageTarget& supportKey);

// A MathIC snippet's branches back to its image, at imageOffset.
void jumpToImage(MacroAssembler&, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag>);
void linkJumpsToImage(MacroAssembler&, const MacroAssembler::JumpList&, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag>);

// The comparisons and case jumps of an inline switch_string, whose tree the consumer keeps (SPEC-image section 3.7).
// JIT::emit_op_switch_string creates one only when its assembler has a recorder, and hands it to its BinarySwitch. Each
// comparison against the key of rank r is a Pointer fixup with target SwitchStringRankAtom(table, r), and the leaf jump
// that executes rank r a Jump fixup with target SwitchStringRankCase(table, r), so that the consumer writes its own key
// of rank r and points the case jump at that key's case.
class StringSwitchRecording final : public BinarySwitchRankedComparisons {
    WTF_MAKE_NONCOPYABLE(StringSwitchRecording);
public:
    StringSwitchRecording(ImageRecorder&, unsigned tableIndex);
    MacroAssembler::Jump branch(MacroAssembler&, MacroAssembler::RelationalCondition, GPRReg value, unsigned rank, intptr_t key) final;
    void recordCase(MacroAssembler::Jump leaf, unsigned rank); // The leaf jump() that executes rank, before addJump.

private:
    ImageTarget rankTarget(TargetKind, unsigned rank) const;

    ImageRecorder& m_recorder;
    unsigned m_tableIndex;
};

// The hooks of the exported headers' inline functions are declared there, since those headers cannot include this one:
// noteSupportLink in assembler/AbstractMacroAssembler.h; storePtrToVMAddress, loadPtrFromVMAddress,
// branch32WithVMAddress, branchTest8AtVMAddress and noteUnannotatedReference in jit/AssemblyHelpers.h; and
// notePointerArgument in jit/CCallHelpers.h. ImageEmission.cpp defines them. Each VM-address hook emits the recorded
// sequence of the helper with the same operation for an address of the VM's table, and otherwise makes the record
// Unrecordable(UnknownVMAddress) and emits the native one.

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
