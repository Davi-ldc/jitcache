/*
 * Copyright (C) 2008-2022 Apple Inc. All rights reserved.
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
#include "BytecodeOperandsForCheckpoint.h"
#include "CommonSlowPathsInlines.h"
#include "ImageEmission.h"
#include "ImageTypes.h"
#include "JIT.h"
#include "JSCInlines.h"
#include <optional>
#include <type_traits>
#include <utility>

namespace JSC {

// JITCache: the support keys the baseline emitters link through the helpers of ImageEmission.h (SPEC-image.md sections
// 3.3 and 3.4), with every field their kind does not use zero.
namespace JITCacheSupportKey {

constexpr JITCache::ImageTarget baselineThunk(JITCache::BaselineThunk thunk)
{
    return JITCache::ImageTarget { .kind = JITCache::TargetKind::BaselineThunk, .a = static_cast<uint32_t>(thunk), .b = 0, .payload = 0 };
}

constexpr JITCache::ImageTarget commonThunk(CommonJITThunkID thunk)
{
    return JITCache::ImageTarget { .kind = JITCache::TargetKind::CommonThunk, .a = static_cast<uint32_t>(thunk), .b = 0, .payload = 0 };
}

constexpr JITCache::ImageTarget inlineCacheSlowPathThunk(AccessType accessType)
{
    return JITCache::ImageTarget { .kind = JITCache::TargetKind::InlineCacheSlowPathThunk, .a = static_cast<uint32_t>(accessType), .b = 0, .payload = 0 };
}

constexpr JITCache::ImageTarget processThunk(JITCache::ProcessThunk thunk)
{
    return JITCache::ImageTarget { .kind = JITCache::TargetKind::ProcessThunk, .a = static_cast<uint32_t>(thunk), .b = 0, .payload = 0 };
}

} // namespace JITCacheSupportKey

ALWAYS_INLINE bool JIT::isOperandConstantDouble(VirtualRegister src)
{
    if (!src.isConstant())
        return false;
    if (m_unlinkedCodeBlock->constantSourceCodeRepresentation(src) == SourceCodeRepresentation::LinkTimeConstant)
        return false;
    return getConstantOperand(src).isDouble();
}

ALWAYS_INLINE bool JIT::isOperandConstantInt(VirtualRegister src)
{
    if (!src.isConstant())
        return false;
    if (m_unlinkedCodeBlock->constantSourceCodeRepresentation(src) == SourceCodeRepresentation::LinkTimeConstant)
        return false;
    return getConstantOperand(src).isInt32();
}

ALWAYS_INLINE bool JIT::isKnownCell(VirtualRegister src)
{
    if (!src.isConstant())
        return false;
    if (m_unlinkedCodeBlock->constantSourceCodeRepresentation(src) == SourceCodeRepresentation::LinkTimeConstant) {
        // All link time constants are cells.
        return true;
    }
    return getConstantOperand(src).isCell();
}

ALWAYS_INLINE JSValue JIT::getConstantOperand(VirtualRegister src)
{
    ASSERT(src.isConstant());
    RELEASE_ASSERT(m_unlinkedCodeBlock->constantSourceCodeRepresentation(src) != SourceCodeRepresentation::LinkTimeConstant);
    return m_unlinkedCodeBlock->getConstant(src);
}

ALWAYS_INLINE void JIT::emitLoadCharacterString(RegisterID src, RegisterID dst, JumpList& failures)
{
    failures.append(branchIfNotString(src));
    loadPtr(MacroAssembler::Address(src, JSString::offsetOfValue()), dst);
    failures.append(branchIfRopeStringImpl(dst));
    failures.append(branch32(NotEqual, MacroAssembler::Address(dst, StringImpl::lengthMemoryOffset()), TrustedImm32(1)));
    loadPtr(MacroAssembler::Address(dst, StringImpl::dataOffset()), regT1);

    auto is16Bit = branchTest32(Zero, Address(dst, StringImpl::flagsOffset()), TrustedImm32(StringImpl::flagIs8Bit()));
    load8(MacroAssembler::Address(regT1, 0), dst);
    auto done = jump();
    is16Bit.link(this);
    load16(MacroAssembler::Address(regT1, 0), dst);
    done.link(this);
}

ALWAYS_INLINE void JIT::updateTopCallFrame()
{
    uint32_t locationBits = CallSiteIndex(m_bytecodeIndex.offset()).bits();
    store32(TrustedImm32(locationBits), highWordFor(CallFrameSlot::argumentCountIncludingThis));
    prepareCallOperation(*m_vm);
}

template<typename OperationType>
ALWAYS_INLINE MacroAssembler::Call JIT::appendCallWithExceptionCheck(const CodePtr<CFunctionPtrTag> function)
{
    updateTopCallFrame();
    MacroAssembler::Call call = appendCall(function);
    using ResultType = typename FunctionTraits<OperationType>::ResultType;
    if constexpr (isExceptionOperationResult<ResultType>) {
#if ASSERT_ENABLED
        Jump ok = JITCache::branchPtrAtReference(*this, Equal, JITCache::ImageReference::vmAddress(vm(), JITCache::VMAddress::Exception), operationExceptionRegister<ResultType>());
        breakpoint();
        ok.link(this);
#endif
        exceptionCheck(branchTestPtr(NonZero, operationExceptionRegister<ResultType>()));
    } else
        exceptionCheck();
    return call;
}

template<typename OperationType>
ALWAYS_INLINE void JIT::appendCallWithExceptionCheck(Address function)
{
    updateTopCallFrame();
    appendCall(function);
    using ResultType = typename FunctionTraits<OperationType>::ResultType;
    if constexpr (isExceptionOperationResult<ResultType>) {
#if ASSERT_ENABLED
        Jump ok = JITCache::branchPtrAtReference(*this, Equal, JITCache::ImageReference::vmAddress(vm(), JITCache::VMAddress::Exception), operationExceptionRegister<ResultType>());
        breakpoint();
        ok.link(this);
#endif
        exceptionCheck(branchTestPtr(NonZero, operationExceptionRegister<ResultType>()));
    } else
        exceptionCheck();
}

template<typename OperationType>
ALWAYS_INLINE MacroAssembler::Call JIT::appendCallSetJSValueResult(const CodePtr<CFunctionPtrTag> function, VirtualRegister dst)
{
    updateTopCallFrame();
    MacroAssembler::Call call = appendCall(function);
    emitPutVirtualRegister(dst, returnValueGPR);
    return call;
}

template<typename OperationType>
ALWAYS_INLINE void JIT::appendCallSetJSValueResult(Address function, VirtualRegister dst)
{
    updateTopCallFrame();
    appendCall(function);
    emitPutVirtualRegister(dst, returnValueGPR);
}

template<typename OperationType>
ALWAYS_INLINE MacroAssembler::Call JIT::appendCallWithExceptionCheckSetJSValueResult(const CodePtr<CFunctionPtrTag> function, VirtualRegister dst)
{
    MacroAssembler::Call call = appendCallWithExceptionCheck<OperationType>(function);
    emitPutVirtualRegister(dst, returnValueGPR);
    return call;
}

template<typename OperationType>
ALWAYS_INLINE void JIT::appendCallWithExceptionCheckSetJSValueResult(Address function, VirtualRegister dst)
{
    appendCallWithExceptionCheck<OperationType>(function);
    emitPutVirtualRegister(dst, returnValueGPR);
}

template<typename OperationType, typename Bytecode>
ALWAYS_INLINE MacroAssembler::Call JIT::appendCallWithExceptionCheckSetJSValueResultWithProfile(const Bytecode& bytecode, const CodePtr<CFunctionPtrTag> function, VirtualRegister dst)
{
    MacroAssembler::Call call = appendCallWithExceptionCheck<OperationType>(function);
    emitValueProfilingSite(bytecode, returnValueGPR);
    emitPutVirtualRegister(dst, returnValueGPR);
    return call;
}

template<typename OperationType, typename Bytecode>
ALWAYS_INLINE void JIT::appendCallWithExceptionCheckSetJSValueResultWithProfile(const Bytecode& bytecode, Address function, VirtualRegister dst)
{
    appendCallWithExceptionCheck<OperationType>(function);
    emitValueProfilingSite(bytecode, returnValueGPR);
    emitPutVirtualRegister(dst, returnValueGPR);
}

ALWAYS_INLINE void JIT::linkAllSlowCasesUpToBytecodeIndex(Vector<SlowCaseEntry>& slowCases, Vector<SlowCaseEntry>::iterator& iter, BytecodeIndex bytecodeIndex)
{
    while (iter != slowCases.end() && iter->to <= bytecodeIndex)
        linkSlowCase(iter);
}

ALWAYS_INLINE bool JIT::hasAnySlowCases(Vector<SlowCaseEntry>& slowCases, Vector<SlowCaseEntry>::iterator& iter, BytecodeIndex bytecodeIndex)
{
    if (iter != slowCases.end() && iter->to == bytecodeIndex)
        return true;
    return false;
}

inline void JIT::advanceToNextCheckpoint()
{
    ASSERT_WITH_MESSAGE(m_bytecodeIndex, "This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set");
    ASSERT(m_unlinkedCodeBlock->instructionAt(m_bytecodeIndex)->hasCheckpoints());
    m_bytecodeIndex = BytecodeIndex(m_bytecodeIndex.offset(), m_bytecodeIndex.checkpoint() + 1);

    auto result = m_checkpointLabels.add(m_bytecodeIndex, label());
    ASSERT_UNUSED(result, result.isNewEntry);
}

inline void JIT::emitJumpSlowToHotForCheckpoint(Jump jump)
{
    ASSERT_WITH_MESSAGE(m_bytecodeIndex, "This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set");
    ASSERT(m_unlinkedCodeBlock->instructionAt(m_bytecodeIndex)->hasCheckpoints());
    m_bytecodeIndex = BytecodeIndex(m_bytecodeIndex.offset(), m_bytecodeIndex.checkpoint() + 1);

    auto iter = m_checkpointLabels.find(m_bytecodeIndex);
    ASSERT(iter != m_checkpointLabels.end());
    if (jump.isSet())
        jump.linkTo(iter->value, this);
}

inline void JIT::setFastPathResumePoint()
{
    ASSERT_WITH_MESSAGE(m_bytecodeIndex, "This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set");
    auto result = m_fastPathResumeLabels.add(m_bytecodeIndex, label());
    ASSERT_UNUSED(result, result.isNewEntry);
}

inline MacroAssembler::Label JIT::fastPathResumePoint() const
{
    ASSERT_WITH_MESSAGE(m_bytecodeIndex, "This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set");
    // Location set by setFastPathResumePoint
    auto iter = m_fastPathResumeLabels.find(m_bytecodeIndex);
    if (iter != m_fastPathResumeLabels.end())
        return iter->value;
    // Next instruction in sequence
    const auto* currentInstruction = m_unlinkedCodeBlock->instructions().at(m_bytecodeIndex).ptr();
    return m_labels[m_bytecodeIndex.offset() + currentInstruction->size()];
}


ALWAYS_INLINE void JIT::addSlowCase(Jump jump)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.

    m_slowCases.append(SlowCaseEntry(jump, m_bytecodeIndex));
}

ALWAYS_INLINE void JIT::addSlowCase(const JumpList& jumpList)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.

    for (const Jump& jump : jumpList.jumps())
        m_slowCases.append(SlowCaseEntry(jump, m_bytecodeIndex));
}

ALWAYS_INLINE void JIT::addSlowCase()
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.
    
    Jump emptyJump; // Doing it this way to make Windows happy.
    m_slowCases.append(SlowCaseEntry(emptyJump, m_bytecodeIndex));
}

ALWAYS_INLINE void JIT::addJump(Jump jump, int relativeOffset)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.

    m_jmpTable.append(JumpTable(jump, m_bytecodeIndex.offset() + relativeOffset));
}

ALWAYS_INLINE void JIT::addJump(const JumpList& jumpList, int relativeOffset)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.

    for (auto& jump : jumpList.jumps())
        addJump(jump, relativeOffset);
}

ALWAYS_INLINE void JIT::emitJumpSlowToHot(Jump jump, int relativeOffset)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.

    jump.linkTo(m_labels[m_bytecodeIndex.offset() + relativeOffset], this);
}

#if ENABLE(SAMPLING_FLAGS)
ALWAYS_INLINE void JIT::setSamplingFlag(int32_t flag)
{
    ASSERT(flag >= 1);
    ASSERT(flag <= 32);
    or32(TrustedImm32(1u << (flag - 1)), AbsoluteAddress(SamplingFlags::addressOfFlags()));
}

ALWAYS_INLINE void JIT::clearSamplingFlag(int32_t flag)
{
    ASSERT(flag >= 1);
    ASSERT(flag <= 32);
    and32(TrustedImm32(~(1u << (flag - 1))), AbsoluteAddress(SamplingFlags::addressOfFlags()));
}
#endif

#if ENABLE(SAMPLING_COUNTERS)
ALWAYS_INLINE void JIT::emitCount(AbstractSamplingCounter& counter, int32_t count)
{
    add64(TrustedImm32(count), AbsoluteAddress(counter.addressOfCounter()));
}
#endif

ALWAYS_INLINE bool JIT::isOperandConstantChar(VirtualRegister src)
{
    if (!src.isConstant())
        return false;
    if (m_unlinkedCodeBlock->constantSourceCodeRepresentation(src) == SourceCodeRepresentation::LinkTimeConstant)
        return false;
    return getConstantOperand(src).isString() && asString(getConstantOperand(src).asCell())->length() == 1;
}

template<typename Bytecode>
inline void JIT::emitValueProfilingSite(const Bytecode& bytecode, BytecodeIndex bytecodeIndex, GPRReg value)
{
    if (!shouldEmitProfiling())
        return;

    ptrdiff_t offset = -static_cast<ptrdiff_t>(valueProfileOffsetFor<Bytecode>(bytecode, bytecodeIndex.checkpoint())) * sizeof(ValueProfile) + ValueProfile::offsetOfFirstBucket() - sizeof(UnlinkedMetadataTable::LinkingData);
    storeValue(value, Address(GPRInfo::metadataTableRegister, offset));
}

template<typename Bytecode>
inline void JIT::emitValueProfilingSite(const Bytecode& bytecode, GPRReg value)
{
    emitValueProfilingSite(bytecode, m_bytecodeIndex, value);
}

template <typename Bytecode>
inline void JIT::emitArrayProfilingSiteWithCell(const Bytecode& bytecode, ptrdiff_t offsetOfArrayProfile, RegisterID cellGPR, RegisterID scratchGPR)
{
    if (shouldEmitProfiling()) {
        load32(Address(cellGPR, JSCell::structureIDOffset()), scratchGPR);
        store32ToMetadata(scratchGPR, bytecode, offsetOfArrayProfile);
    }
}

template <typename Bytecode>
inline void JIT::emitArrayProfilingSiteWithCell(const Bytecode& bytecode, RegisterID cellGPR, RegisterID scratchGPR)
{
    emitArrayProfilingSiteWithCell(bytecode, Bytecode::Metadata::offsetOfArrayProfile() + ArrayProfile::offsetOfLastSeenStructureID(), cellGPR, scratchGPR);
}

inline void JIT::emitArrayProfilingSiteWithCellAndProfile(RegisterID cellGPR, RegisterID profileGPR, RegisterID scratchGPR)
{
    if (shouldEmitProfiling()) {
        load32(Address(cellGPR, JSCell::structureIDOffset()), scratchGPR);
        store32(scratchGPR, Address(profileGPR, ArrayProfile::offsetOfLastSeenStructureID()));
    }
}

ALWAYS_INLINE int32_t JIT::getOperandConstantInt(VirtualRegister src)
{
    return getConstantOperand(src).asInt32();
}

ALWAYS_INLINE double JIT::getOperandConstantDouble(VirtualRegister src)
{
    return getConstantOperand(src).asDouble();
}

ALWAYS_INLINE void JIT::emitGetVirtualRegister(VirtualRegister src, GPRReg dst)
{
    ASSERT(m_bytecodeIndex); // This method should only be called during hot/cold path generation, so that m_bytecodeIndex is set.
    if (src.isConstant()) {
        if (m_profiledCodeBlock->isConstantOwnedByUnlinkedCodeBlock(src)) {
            // JITCache: a cell the UCB owns is a reference; any other constant is a literal (census B6).
            JSValue value = m_unlinkedCodeBlock->getConstant(src);
            if (value && value.isCell())
                JITCache::moveReferenceValue(*this, JITCache::ImageReference::ucbConstantCell(vm(), *m_unlinkedCodeBlock, src), dst);
            else
                moveValue(value, dst);
        } else
            loadCodeBlockConstant(src, dst);
        return;
    }
    loadValue(addressFor(src), dst);
}

ALWAYS_INLINE void JIT::emitPutVirtualRegister(VirtualRegister dst, GPRReg from)
{
    storeValue(from, addressFor(dst));
}

ALWAYS_INLINE JIT::Jump JIT::emitJumpIfNotInt(GPRReg reg1, GPRReg reg2, GPRReg scratch)
{
    and64(reg1, reg2, scratch);
    return branchIfNotInt32(scratch);
}

ALWAYS_INLINE void JIT::emitJumpSlowCaseIfNotInt(GPRReg reg1, GPRReg reg2, GPRReg scratch)
{
    addSlowCase(emitJumpIfNotInt(reg1, reg2, scratch));
}

ALWAYS_INLINE void JIT::emitJumpSlowCaseIfNotInt(GPRReg gpr)
{
    addSlowCase(branchIfNotInt32(gpr));
}

ALWAYS_INLINE void JIT::emitJumpSlowCaseIfNotJSCell(GPRReg reg)
{
    addSlowCase(branchIfNotCell(reg));
}

ALWAYS_INLINE void JIT::emitJumpSlowCaseIfNotJSCell(GPRReg gpr, VirtualRegister vReg)
{
    if (!isKnownCell(vReg))
        emitJumpSlowCaseIfNotJSCell(gpr);
}

ALWAYS_INLINE int JIT::jumpTarget(const JSInstruction* instruction, int target)
{
    if (target)
        return target;
    return m_unlinkedCodeBlock->outOfLineJumpOffset(instruction);
}

template<typename Op>
ALWAYS_INLINE ECMAMode JIT::ecmaMode(Op op)
{
    return op.m_ecmaMode;
}

template<>
ALWAYS_INLINE ECMAMode JIT::ecmaMode<OpPutById>(OpPutById op)
{
    return op.m_flags.ecmaMode();
}

template<>
ALWAYS_INLINE ECMAMode JIT::ecmaMode<OpPutPrivateName>(OpPutPrivateName)
{
    return ECMAMode::strict();
}

template<size_t minAlign, typename Bytecode>
ALWAYS_INLINE MacroAssembler::Address JIT::computeBaseAddressForMetadata(const Bytecode& bytecode, GPRReg metadataGPR)
{
    // This function attempts to decide what the best base address is when
    // loading fields from a bytecode instruction's metadata. If offsets are
    // small enough, we want to emit a single load directly offset from the
    // metadata table register. But if they're too big, we want to materialize
    // the metadata offset of the current instruction into a register only
    // once, so repeated loads don't need to redo that arithmetic.

    // Note: minAlign is very important to get right, but hard to check for
    // automatically. It should be the minimum alignment or access size across
    // all the operations we use this address for. This is critical on ARM64
    // (where this function is most useful) because we have a substantially
    // larger addressible range when we can assume the access is aligned to
    // the access size. One important note is that we assume that any accesses
    // larger than the specified minAlign are also aligned - only then is it
    // the case that an offset being encodable at a smaller access size also
    // implies that it is encodable at a larger access size.

    using Metadata = typename Bytecode::Metadata;
    static_assert(WTF::roundUpToMultipleOf<minAlign>(alignof(Metadata)) == alignof(Metadata));
    uint32_t metadataOffset = m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode);

#if CPU(X86) || CPU(X86_64)
    // On x86 and x86_64, we can directly encode up to a 32-bit displacement.
    // We use 1 << 30 to be extra conservative that adding a further offset
    // won't cause a signed overflow.
    bool shouldEncodeMetadataOffsetInline = metadataOffset < (1u << 30);
#elif CPU(ARM64)
    // On arm64, we can only encode a 9-bit signed unaligned offset, or a
    // 12-bit unsigned aligned offset. We use the same checks used elsewhere
    // in the macro assembler to see if our offset fits one of those.
    auto canEncodeOffset = [](int32_t offset) -> bool {
        return ARM64Assembler::canEncodePImmOffset<minAlign * 8>(offset) || ARM64Assembler::canEncodeSImmOffset(offset);
    };
    bool shouldEncodeMetadataOffsetInline = canEncodeOffset(metadataOffset + sizeof(Metadata));
#else
    bool shouldEncodeMetadataOffsetInline = false;
#endif

    if (shouldEncodeMetadataOffsetInline)
        return Address(GPRInfo::metadataTableRegister, metadataOffset);
    addPtr(TrustedImm32(metadataOffset), GPRInfo::metadataTableRegister, metadataGPR);
    return Address(metadataGPR);
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::loadPtrFromMetadata(const Bytecode& bytecode, size_t offset, GPRReg result)
{
    loadPtr(Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), result);
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::loadPairPtrFromMetadata(const Bytecode& bytecode, size_t offset, GPRReg result1, GPRReg result2)
{
    loadPairPtr(Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), result1, result2);
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::load32FromMetadata(const Bytecode& bytecode, size_t offset, GPRReg result)
{
    load32(Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), result);
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::load8FromMetadata(const Bytecode& bytecode, size_t offset, GPRReg result)
{
    load8(Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), result);
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::load16FromMetadata(const Bytecode& bytecode, size_t offset, GPRReg result)
{
    load16(Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), result);
}

template <typename ValueType, typename Bytecode>
ALWAYS_INLINE void JIT::store8ToMetadata(ValueType value, const Bytecode& bytecode, size_t offset)
{
    store8(value, Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset));
}

template <typename ValueType, typename Bytecode>
ALWAYS_INLINE void JIT::store16ToMetadata(ValueType value, const Bytecode& bytecode, size_t offset)
{
    store16(value, Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset));
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::store32ToMetadata(GPRReg value, const Bytecode& bytecode, size_t offset)
{
    store32(value, Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset));
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::storePtrToMetadata(GPRReg value, const Bytecode& bytecode, size_t offset)
{
    storePtr(value, Address(GPRInfo::metadataTableRegister, m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset));
}

template <typename Bytecode>
ALWAYS_INLINE void JIT::materializePointerIntoMetadata(const Bytecode& bytecode, size_t offset, GPRReg result)
{
    addPtr(TrustedImm32(m_profiledCodeBlock->metadataTable()->offsetInMetadataTable(bytecode) + offset), GPRInfo::metadataTableRegister, result);
}

ALWAYS_INLINE void JIT::loadConstant(CCallHelpers& jit, JITConstantPool::Constant constantIndex, GPRReg result)
{
    jit.loadPtr(Address(GPRInfo::jitDataRegister, BaselineJITData::offsetOfTrailingData() + static_cast<uintptr_t>(constantIndex) * sizeof(void*)), result);
}

ALWAYS_INLINE void JIT::loadGlobalObject(CCallHelpers& jit, GPRReg result)
{
    jit.loadPtr(Address(GPRInfo::jitDataRegister, BaselineJITData::offsetOfGlobalObject()), result);
}

ALWAYS_INLINE void JIT::loadConstant(JITConstantPool::Constant constantIndex, GPRReg result)
{
    loadConstant(*this, constantIndex, result);
}

ALWAYS_INLINE void JIT::loadGlobalObject(GPRReg result)
{
    loadGlobalObject(*this, result);
}

ALWAYS_INLINE void JIT::loadPropertyInlineCache(CCallHelpers& jit, PropertyInlineCacheIndex index, GPRReg result)
{
    jit.subPtr(GPRInfo::jitDataRegister, TrustedImm32(static_cast<uintptr_t>(index.m_index + 1) * sizeof(HandlerPropertyInlineCache)), result);
}

ALWAYS_INLINE void JIT::loadPropertyInlineCache(PropertyInlineCacheIndex index, GPRReg result)
{
    loadPropertyInlineCache(*this, index, result);
}

ALWAYS_INLINE static void loadAddrOfCodeBlockConstantBuffer(JIT &jit, GPRReg dst)
{
    jit.loadPtr(jit.addressFor(CallFrameSlot::codeBlock), dst);
    jit.loadPtr(JIT::Address(dst, CodeBlock::offsetOfConstantsVectorBuffer()), dst);
}

ALWAYS_INLINE void JIT::loadCodeBlockConstant(VirtualRegister constant, GPRReg dst)
{
    RELEASE_ASSERT(constant.isConstant());
    loadAddrOfCodeBlockConstantBuffer(*this, dst);
    loadValue(Address(dst, constant.toConstantIndex() * sizeof(Register)), dst);
}

// The MathIC opcodes, each with the generator its IC holds and the profiled operation its emitter passes, whose argument
// registers the generator works in.
template<typename Op> struct JITMathICTraits;

template<> struct JITMathICTraits<OpAdd> {
    using Generator = JITAddGenerator;
    using ProfiledOperation = decltype(&operationValueAddProfiled);
};

template<> struct JITMathICTraits<OpSub> {
    using Generator = JITSubGenerator;
    using ProfiledOperation = decltype(&operationValueSubProfiled);
};

template<> struct JITMathICTraits<OpMul> {
    using Generator = JITMulGenerator;
    using ProfiledOperation = decltype(&operationValueMulProfiled);
};

template<> struct JITMathICTraits<OpNegate> {
    using Generator = JITNegGenerator;
    using ProfiledOperation = decltype(&operationArithNegateProfiled);
};

// The operands of a binary MathIC: their types from the bytecode, and the value of the first operand that is an int32
// constant the UCB owns, as JIT::isOperandConstantInt decides, read from the UCB alone. The fast path, the slow path and
// the generator take them from here, so they agree on which operand stays a constant.
template<typename Op>
inline std::pair<SnippetOperand, SnippetOperand> binaryMathICOperandsFor(const UnlinkedCodeBlock& unlinkedCodeBlock, const Op& bytecode)
{
    auto constantInt32 = [&](VirtualRegister operand) -> std::optional<int32_t> {
        if (!operand.isConstant() || unlinkedCodeBlock.constantSourceCodeRepresentation(operand) == SourceCodeRepresentation::LinkTimeConstant)
            return std::nullopt;
        JSValue value = unlinkedCodeBlock.getConstant(operand);
        if (!value.isInt32())
            return std::nullopt;
        return value.asInt32();
    };

    SnippetOperand leftOperand(bytecode.m_operandTypes.first());
    SnippetOperand rightOperand(bytecode.m_operandTypes.second());
    if (auto constant = constantInt32(bytecode.m_lhs))
        leftOperand.setConstInt32(*constant);
    else if (auto constant = constantInt32(bytecode.m_rhs))
        rightOperand.setConstInt32(*constant);
    return { leftOperand, rightOperand };
}

template<typename Op>
auto JIT::mathICGeneratorFor(const UnlinkedCodeBlock& unlinkedCodeBlock, const JSInstruction* instruction)
{
    using Generator = typename JITMathICTraits<Op>::Generator;
    using ProfiledOperation = typename JITMathICTraits<Op>::ProfiledOperation;

    if constexpr (std::is_same_v<Op, OpNegate>) {
        UNUSED_PARAM(unlinkedCodeBlock);
        UNUSED_PARAM(instruction);
        // The result shares the source's register, and the global object's register is the scratch.
        constexpr GPRReg srcGPR = preferredArgumentGPR<ProfiledOperation, 1>();
        constexpr GPRReg scratchGPR = preferredArgumentGPR<ProfiledOperation, 0>();
        static_assert(noOverlap(srcGPR, scratchGPR));
        return Generator(srcGPR, srcGPR, scratchGPR);
    } else {
        constexpr GPRReg leftGPR = preferredArgumentGPR<ProfiledOperation, 1>();
        constexpr GPRReg rightGPR = preferredArgumentGPR<ProfiledOperation, 2>();
        constexpr GPRReg resultGPR = returnValueGPR;
        constexpr GPRReg scratchGPR = regT5;
        static_assert(noOverlap(leftGPR, rightGPR, scratchGPR));
        static_assert(noOverlap(resultGPR, scratchGPR));
        auto [leftOperand, rightOperand] = binaryMathICOperandsFor(unlinkedCodeBlock, instruction->as<Op>());
        return Generator(leftOperand, rightOperand, resultGPR, leftGPR, rightGPR, fpRegT0, fpRegT1, scratchGPR);
    }
}

} // namespace JSC

#endif // ENABLE(JIT)
