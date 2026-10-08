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

#include "config.h"
#include "ImageEmission.h"

#if ENABLE(JIT)

#include "ImageRecorder.h"
#include "ImageSupport.h"
#include "JSCJSValueInlines.h"
#include "LLIntEntrypoint.h"
#include "UnlinkedCodeBlock.h"
#include "VM.h"

namespace JSC::JITCache {

namespace ImageEmissionInternal {

static ImageTarget makeTarget(TargetKind kind, uint32_t a = 0)
{
    return ImageTarget { .kind = kind, .a = a, .b = 0, .payload = 0 };
}

// R3: the absolute-address forms materialize the address into the scratch register r11 on x86_64 and into the memory
// temp x17 on ARM64, whose cached value they invalidate (R4). On ARM64 the operations on Address(x17) may also use the
// data temp x16, which the value-holding forms use as their scratch register (R5).
#if CPU(ARM64)
static constexpr GPRReg referenceTemp = MacroAssembler::memoryTempRegister;
static constexpr GPRReg valueTemp = MacroAssembler::dataTempRegister;
#else
static constexpr GPRReg referenceTemp = MacroAssembler::s_scratchRegister;
static constexpr GPRReg valueTemp = MacroAssembler::s_scratchRegister;
#endif

// A helper's register operands differ from the temps its recorded sequence uses.
[[maybe_unused]] static bool isTemp(GPRReg reg)
{
    return reg == referenceTemp || reg == valueTemp;
}

static GPRReg takeReferenceTemp(MacroAssembler& masm)
{
#if CPU(ARM64)
    return masm.memoryTempRegisterForReference();
#else
    return masm.scratchRegister();
#endif
}

// A reference the recorder of its assembler records, with the target it records.
struct Recording {
    ImageRecorder& recorder;
    ImageTarget target;

    // R2: a Pointer is a moveWithPatch of the value, recorded at its label, never folded or blinded.
    void emitPointer(MacroAssembler& masm, const void* value, GPRReg gpr) const
    {
        auto label = masm.moveWithPatch(MacroAssembler::TrustedImmPtr(value), gpr);
        recorder.recordPointer(label.label(), target);
    }

    // R3: the address as a Pointer into the reference temp, and the Address the recorded form then operates on.
    MacroAssembler::Address emitAddress(MacroAssembler& masm, const void* address) const
    {
        GPRReg temp = takeReferenceTemp(masm);
        emitPointer(masm, address, temp);
        return MacroAssembler::Address(temp);
    }
};

// How masm records reference, or nullopt when masm emits the native sequence: without a recorder, or for a reference
// whose target emission cannot name, which leaves the record unrecordable (census A14).
static std::optional<Recording> recordingOf(MacroAssembler& masm, const ImageReference& reference)
{
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]]
        return std::nullopt;
    auto target = reference.recordedTarget(*recorder);
    if (!target) {
        recorder->markUnrecordable(Unrecordable::InconsistentRecord);
        return std::nullopt;
    }
    return Recording { *recorder, *target };
}

static CodeLocationLabel<NoPtrTag> supportLocation(VM& vm, const ImageTarget& supportKey)
{
    return CodeLocationLabel<NoPtrTag>(resolveSupport(vm, supportKey));
}

// R6: a jump to code outside its allocation. On ARM64 a conditional one goes through a veneer; any other carries its
// Jump fixup at its own site.
static void linkExternalJump(MacroAssembler& masm, ImageRecorder& recorder, MacroAssembler::Jump jump, const ImageTarget& target, CodeLocationLabel<NoPtrTag> location)
{
    if (jump.isConditionalForJITCache()) {
        recorder.deferConditionalJump(masm, jump, target, location);
        return;
    }
    {
        ImageRecorder::SupportLinkScope scope(recorder);
        jump.linkThunk(location, &masm);
    }
    recorder.recordJump(jump.assemblerLabel(), target);
}

static std::optional<ImageReference> vmAddressReference(MacroAssembler& masm, const void* address)
{
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder)
        return std::nullopt;
    auto target = recorder->vmAddressTarget(address);
    if (!target) {
        recorder->markUnrecordable(Unrecordable::UnknownVMAddress);
        return std::nullopt;
    }
    return ImageReference(*target, address);
}

} // namespace ImageEmissionInternal

ImageReference::ImageReference(const ImageTarget& target, const void* value)
    : ImageReference(target, value, TargetState::Known)
{
}

ImageReference::ImageReference(const ImageTarget& target, const void* value, TargetState targetState)
    : m_target(target)
    , m_value(value)
    , m_targetState(targetState)
{
}

ImageReference ImageReference::vmAddress(VM& vm, VMAddress address)
{
    auto target = ImageEmissionInternal::makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(address));
    return ImageReference(target, resolveTarget(ResolutionContext { .vm = vm }, target));
}

ImageReference ImageReference::vmCell(VM& vm, VMCell cell)
{
    auto target = ImageEmissionInternal::makeTarget(TargetKind::VMCell, static_cast<uint32_t>(cell));
    return ImageReference(target, resolveTarget(ResolutionContext { .vm = vm }, target));
}

ImageReference ImageReference::processThunk(ProcessThunk thunk)
{
    // The process-wide entry resolveTarget gives in every VM.
    RELEASE_ASSERT(thunk == ProcessThunk::DefaultCall);
    return ImageReference(ImageEmissionInternal::makeTarget(TargetKind::ProcessThunk, static_cast<uint32_t>(thunk)), LLInt::defaultCall().code().taggedPtr());
}

ImageReference ImageReference::ucbConstantCell(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, VirtualRegister constant)
{
    ASSERT(constant.isConstant());
    auto target = ImageEmissionInternal::makeTarget(TargetKind::UCBConstantCell, static_cast<uint32_t>(constant.toConstantIndex()));
    return ImageReference(target, resolveTarget(ResolutionContext { .vm = vm, .ucb = &unlinkedCodeBlock }, target));
}

ImageReference ImageReference::ucbConstantAtom(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, VirtualRegister constant)
{
    ASSERT(constant.isConstant());
    auto target = ImageEmissionInternal::makeTarget(TargetKind::UCBConstantAtom, static_cast<uint32_t>(constant.toConstantIndex()));
    return ImageReference(target, resolveTarget(ResolutionContext { .vm = vm, .ucb = &unlinkedCodeBlock }, target));
}

ImageReference ImageReference::ucbIdentifier(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, unsigned identifierIndex)
{
    auto target = ImageEmissionInternal::makeTarget(TargetKind::UCBIdentifier, identifierIndex);
    return ImageReference(target, resolveTarget(ResolutionContext { .vm = vm, .ucb = &unlinkedCodeBlock }, target));
}

ImageReference ImageReference::arithProfileIn(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, const void* profile, TargetKind kind)
{
    auto target = arithProfileTargetIn(unlinkedCodeBlock, profile);
    if (!target || target->kind != kind) {
        // Baseline emitters pass profiles of their own UCB, so this is a programming error; a recorder that meets the
        // reference makes its record unrecordable.
        ASSERT_NOT_REACHED();
        return ImageReference(ImageEmissionInternal::makeTarget(kind), profile, TargetState::Unknown);
    }
    return ImageReference(*target, resolveTarget(ResolutionContext { .vm = vm, .ucb = &unlinkedCodeBlock }, *target));
}

ImageReference ImageReference::arithProfile(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, const BinaryArithProfile& profile)
{
    return arithProfileIn(vm, unlinkedCodeBlock, &profile, TargetKind::UCBBinaryArithProfile);
}

ImageReference ImageReference::arithProfile(VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, const UnaryArithProfile& profile)
{
    return arithProfileIn(vm, unlinkedCodeBlock, &profile, TargetKind::UCBUnaryArithProfile);
}

ImageReference ImageReference::mathIC(const void* mathIC)
{
    return ImageReference(ImageEmissionInternal::makeTarget(TargetKind::MathIC), mathIC, TargetState::MathICIndexFromRecorder);
}

ImageReference ImageReference::switchTableBase(unsigned tableIndex, const void* base)
{
    return ImageReference(ImageEmissionInternal::makeTarget(TargetKind::SwitchTableBase, tableIndex), base);
}

std::optional<ImageTarget> ImageReference::recordedTarget(const ImageRecorder& recorder) const
{
    switch (m_targetState) {
    case TargetState::Known:
        return m_target;
    case TargetState::MathICIndexFromRecorder:
        // A recorder that stopped records nothing and has dropped the ICs it noted.
        if (!recorder.isRecording())
            return m_target;
        return recorder.mathICTarget(m_value);
    case TargetState::Unknown:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ImageReference::materialize(CCallHelpers& jit, GPRReg gpr) const
{
    moveReference(jit, *this, gpr);
}

void ImageReference::store(CCallHelpers& jit, CCallHelpers::Address address) const
{
    auto recording = ImageEmissionInternal::recordingOf(jit, *this);
    if (!recording) {
        jit.storePtr(CCallHelpers::TrustedImmPtr(m_value), address);
        return;
    }
    ASSERT(!ImageEmissionInternal::isTemp(address.base));
    GPRReg temp = ImageEmissionInternal::takeReferenceTemp(jit);
    recording->emitPointer(jit, m_value, temp);
    jit.storePtr(temp, address);
}

void moveReference(MacroAssembler& masm, const ImageReference& reference, GPRReg gpr)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, reference);
    if (!recording) {
        masm.move(MacroAssembler::TrustedImmPtr(reference.value()), gpr);
        return;
    }
    recording->emitPointer(masm, reference.value(), gpr);
}

MacroAssembler::Jump branchPtrWithReference(MacroAssembler& masm, MacroAssembler::RelationalCondition condition, GPRReg left, const ImageReference& right)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, right);
    if (!recording)
        return masm.branchPtr(condition, left, MacroAssembler::TrustedImmPtr(right.value()));
    ASSERT(!ImageEmissionInternal::isTemp(left));
    GPRReg scratch = masm.scratchRegister();
    recording->emitPointer(masm, right.value(), scratch);
    return masm.branchPtr(condition, left, scratch);
}

void storeReferenceValue(AssemblyHelpers& jit, const ImageReference& cell, MacroAssembler::Address address, StoreValueKind kind)
{
    auto recording = ImageEmissionInternal::recordingOf(jit, cell);
    if (!recording) {
        JSValue value(static_cast<JSCell*>(const_cast<void*>(cell.value())));
        switch (kind) {
        case StoreValueKind::Value:
            jit.storeValue(value, address);
            return;
        case StoreValueKind::Trusted:
            jit.storeTrustedValue(value, address);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    // A cell's JSValue encoding is its pointer, so the Pointer is the value.
    ASSERT(!ImageEmissionInternal::isTemp(address.base));
    GPRReg scratch = jit.scratchRegister();
    recording->emitPointer(jit, cell.value(), scratch);
    jit.store64(scratch, address);
}

MacroAssembler::Jump branchPtrAtReference(MacroAssembler& masm, MacroAssembler::RelationalCondition condition, const ImageReference& address, GPRReg right)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording)
        return masm.branchPtr(condition, MacroAssembler::AbsoluteAddress(address.value()), right);
    ASSERT(!ImageEmissionInternal::isTemp(right));
    return masm.branchPtr(condition, recording->emitAddress(masm, address.value()), right);
}

MacroAssembler::Jump branchTestPtrAtReference(MacroAssembler& masm, MacroAssembler::ResultCondition condition, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording)
        return masm.branchTestPtr(condition, MacroAssembler::AbsoluteAddress(address.value()));
    return masm.branchTestPtr(condition, recording->emitAddress(masm, address.value()));
}

MacroAssembler::Jump branchTest32AtReference(MacroAssembler& masm, MacroAssembler::ResultCondition condition, const ImageReference& address, MacroAssembler::TrustedImm32 mask)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording)
        return masm.branchTest32(condition, MacroAssembler::AbsoluteAddress(address.value()), mask);
    return masm.branchTest32(condition, recording->emitAddress(masm, address.value()), mask);
}

MacroAssembler::Jump branchTest8AtReference(MacroAssembler& masm, MacroAssembler::ResultCondition condition, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording)
        return masm.branchTest8(condition, MacroAssembler::AbsoluteAddress(address.value()));
    return masm.branchTest8(condition, recording->emitAddress(masm, address.value()));
}

MacroAssembler::Jump branch32WithReferenceAt(MacroAssembler& masm, MacroAssembler::RelationalCondition condition, GPRReg left, const ImageReference& rightAddress)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, rightAddress);
    if (!recording)
        return masm.branch32(condition, left, MacroAssembler::AbsoluteAddress(rightAddress.value()));
    ASSERT(!ImageEmissionInternal::isTemp(left));
    return masm.branch32(condition, left, recording->emitAddress(masm, rightAddress.value()));
}

void store8AtReference(MacroAssembler& masm, MacroAssembler::TrustedImm32 imm, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording) {
        masm.store8(imm, const_cast<void*>(address.value()));
        return;
    }
    masm.store8(imm, recording->emitAddress(masm, address.value()));
}

void storePtrAtReference(MacroAssembler& masm, GPRReg gpr, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording) {
        masm.storePtr(gpr, const_cast<void*>(address.value()));
        return;
    }
    ASSERT(!ImageEmissionInternal::isTemp(gpr));
    masm.storePtr(gpr, recording->emitAddress(masm, address.value()));
}

void loadPtrAtReference(MacroAssembler& masm, const ImageReference& address, GPRReg gpr)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording) {
        masm.loadPtr(address.value(), gpr);
        return;
    }
    ASSERT(!ImageEmissionInternal::isTemp(gpr));
    masm.loadPtr(recording->emitAddress(masm, address.value()), gpr);
}

void or16AtReference(MacroAssembler& masm, MacroAssembler::TrustedImm32 mask, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording) {
        masm.or16(mask, MacroAssembler::AbsoluteAddress(address.value()));
        return;
    }
#if CPU(ARM64)
    // The or must need no temp, so that the address in x17 and the halfword in x16 are the only registers it touches.
    RELEASE_ASSERT(LogicalImmediate::create32(mask.m_value).isValid());
    auto location = recording->emitAddress(masm, address.value());
    GPRReg halfword = masm.scratchRegister();
    masm.load16(location, halfword);
    masm.or32(mask, halfword);
    masm.store16(halfword, location);
#else
    masm.or16(mask, recording->emitAddress(masm, address.value()));
#endif
}

void or16AtReference(MacroAssembler& masm, GPRReg mask, const ImageReference& address)
{
    auto recording = ImageEmissionInternal::recordingOf(masm, address);
    if (!recording) {
        masm.or16(mask, MacroAssembler::AbsoluteAddress(address.value()));
        return;
    }
    ASSERT(!ImageEmissionInternal::isTemp(mask));
    auto location = recording->emitAddress(masm, address.value());
#if CPU(ARM64)
    GPRReg halfword = masm.scratchRegister();
    masm.load16(location, halfword);
    masm.or32(mask, halfword);
    masm.store16(halfword, location);
#else
    masm.or16(mask, location);
#endif
}

void orStructureIDBase(MacroAssembler& masm, GPRReg source, GPRReg dest)
{
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        masm.or64(MacroAssembler::TrustedImm64(structureIDBase()), source, dest);
        return;
    }
    // R5: one form on each architecture, whatever the base's value.
    ASSERT(!ImageEmissionInternal::isTemp(source));
    ASSERT(!ImageEmissionInternal::isTemp(dest));
    GPRReg scratch = masm.scratchRegister();
    ImageEmissionInternal::Recording recording { *recorder, ImageEmissionInternal::makeTarget(TargetKind::StructureIDBase) };
    recording.emitPointer(masm, reinterpret_cast<const void*>(structureIDBase()), scratch);
    masm.or64(scratch, source, dest);
}

void nearCallSupport(MacroAssembler& masm, VM& vm, const ImageTarget& supportKey)
{
    auto location = ImageEmissionInternal::supportLocation(vm, supportKey);
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        masm.nearCallThunk(location);
        return;
    }
    auto call = masm.nearCall();
    {
        ImageRecorder::SupportLinkScope scope(*recorder);
        call.linkThunk(location, &masm);
    }
    recorder->recordCall(call.m_label, supportKey);
}

void nearTailCallSupport(MacroAssembler& masm, VM& vm, const ImageTarget& supportKey)
{
    auto location = ImageEmissionInternal::supportLocation(vm, supportKey);
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        masm.nearTailCallThunk(location);
        return;
    }
    auto call = masm.nearTailCall();
    {
        ImageRecorder::SupportLinkScope scope(*recorder);
        call.linkThunk(location, &masm);
    }
    recorder->recordJump(call.m_label, supportKey);
}

void jumpSupport(MacroAssembler& masm, VM& vm, const ImageTarget& supportKey)
{
    auto location = ImageEmissionInternal::supportLocation(vm, supportKey);
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        masm.jumpThunk(location);
        return;
    }
    ImageEmissionInternal::linkExternalJump(masm, *recorder, masm.jump(), supportKey, location);
}

void linkJumpToSupport(MacroAssembler& masm, VM& vm, MacroAssembler::Jump jump, const ImageTarget& supportKey)
{
    auto location = ImageEmissionInternal::supportLocation(vm, supportKey);
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        jump.linkThunk(location, &masm);
        return;
    }
    ImageEmissionInternal::linkExternalJump(masm, *recorder, jump, supportKey, location);
}

void jumpToImage(MacroAssembler& masm, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag> location)
{
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        masm.jumpThunk(location);
        return;
    }
    auto target = ImageEmissionInternal::makeTarget(TargetKind::ImageOffset, imageOffset);
    ImageEmissionInternal::linkExternalJump(masm, *recorder, masm.jump(), target, location.retagged<NoPtrTag>());
}

void linkJumpsToImage(MacroAssembler& masm, const MacroAssembler::JumpList& jumps, uint32_t imageOffset, CodeLocationLabel<JSInternalPtrTag> location)
{
    auto* recorder = masm.jitCacheRecorder();
    if (!recorder) [[likely]] {
        jumps.linkThunk(location, &masm);
        return;
    }
    auto target = ImageEmissionInternal::makeTarget(TargetKind::ImageOffset, imageOffset);
    auto nativeLocation = location.retagged<NoPtrTag>();
    for (auto& jump : jumps.jumps())
        ImageEmissionInternal::linkExternalJump(masm, *recorder, jump, target, nativeLocation);
}

void storePtrToVMAddress(MacroAssembler& masm, GPRReg gpr, const void* address)
{
    if (auto reference = ImageEmissionInternal::vmAddressReference(masm, address)) {
        storePtrAtReference(masm, gpr, *reference);
        return;
    }
    masm.storePtr(gpr, const_cast<void*>(address));
}

void loadPtrFromVMAddress(MacroAssembler& masm, const void* address, GPRReg gpr)
{
    if (auto reference = ImageEmissionInternal::vmAddressReference(masm, address)) {
        loadPtrAtReference(masm, *reference, gpr);
        return;
    }
    masm.loadPtr(address, gpr);
}

MacroAssembler::Jump branch32WithVMAddress(MacroAssembler& masm, MacroAssembler::RelationalCondition condition, GPRReg left, const void* address)
{
    if (auto reference = ImageEmissionInternal::vmAddressReference(masm, address))
        return branch32WithReferenceAt(masm, condition, left, *reference);
    return masm.branch32(condition, left, MacroAssembler::AbsoluteAddress(address));
}

MacroAssembler::Jump branchTest8AtVMAddress(MacroAssembler& masm, MacroAssembler::ResultCondition condition, const void* address)
{
    if (auto reference = ImageEmissionInternal::vmAddressReference(masm, address))
        return branchTest8AtReference(masm, condition, *reference);
    return masm.branchTest8(condition, MacroAssembler::AbsoluteAddress(address));
}

void noteSupportLink(ImageRecorder& recorder)
{
    if (!recorder.isLinkingSupport())
        recorder.markUnrecordable(Unrecordable::UnannotatedSupportLink);
}

void noteUnannotatedReference(ImageRecorder& recorder)
{
    recorder.markUnrecordable(Unrecordable::UnannotatedReference);
}

void notePointerArgument(ImageRecorder& recorder)
{
    recorder.markUnrecordable(Unrecordable::UnannotatedPointerArgument);
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
