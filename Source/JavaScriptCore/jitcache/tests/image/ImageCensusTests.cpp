#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArithProfile.h"
#include "BaselineJITCode.h"
#include "BaselineJITPlan.h"
#include "BaselineJITRegisters.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "JIT.h"
#include "JITOperations.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LLIntEntrypoint.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/image/ImageEmission.h"
#include "jitcache/image/ImageRecord.h"
#include "jitcache/image/ImageRecorder.h"
#include "jitcache/image/ImageSupport.h"
#include "jitcache/image/ImageTwinsSection.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageEmissionTestSupport.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestVM.h"
#include <optional>
#include <utility>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of the references the engine's own code embeds in an image, outside the helpers of image/ImageEmission.h.
// The shared helpers of AssemblyHelpers, CCallHelpers, ArithProfile and CallLinkInfo record theirs under a recorder
// and emit their native sequence without one, and the helper paths no baseline code takes leave the record
// unrecordable. A get_from_scope site keys the thunk the native selection links, and the tail-call operation, which no
// far call carries, is a Pointer fixup.

namespace JSC::JITCache::Tests {

namespace ImageCensusTestsInternal {

// How many near calls in code (call rel32 on x86_64, bl on ARM64) reach target, following jump islands on ARM64. code
// starts at an instruction.
static unsigned nearCallsReaching(std::span<const uint8_t> code, uintptr_t target)
{
    auto codeAddress = reinterpret_cast<uintptr_t>(code.data());
    unsigned count = 0;
#if CPU(X86_64)
    for (size_t i = 0; i + 5 <= code.size(); ++i) {
        if (code[i] != 0xe8)
            continue;
        int64_t displacement = signExtend(readLittleEndian(code.subspan(i + 1, 4)), 32);
        count += codeAddress + i + 5 + displacement == target;
    }
#elif CPU(ARM64)
    for (size_t i = 0; i + 4 <= code.size(); i += 4) {
        if ((readWord(codeAddress + i) & 0xfc000000) != 0x94000000)
            continue;
        auto branchTarget = arm64BranchTarget(codeAddress + i);
        count += branchTarget && reaches(*branchTarget, target);
    }
#else
    UNUSED_PARAM(codeAddress);
    UNUSED_PARAM(target);
#endif
    return count;
}

} // namespace ImageCensusTestsInternal

using namespace ImageCensusTestsInternal;

// Under a recorder, each shared helper emits its reference in its recorded form, records it and leaves the record
// complete. Without one, each helper that now builds its reference through a factory and an emission helper emits its
// native expression, restated below, which also checks the factory's value at that site. The inline functions keep their
// native code and call a hook only under a recorder, and restoreCalleeSavesFromEntryFrameCalleeSavesBuffer and
// copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer change only their loadPtr, which becomes
// loadPtrFromVMAddress, whose native emission imageHelpersEmitNativeWithoutRecorder checks. JITSlowPathCall::call and
// JIT::exceptionCheck(Jump) emit only inside a JIT, so the baseline compilations of the JS image tests and their twin
// checks cover them.
JITCACHE_TEST(imageSharedHelpersRecordTheirReferences, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    UnlinkedCodeBlock& unlinkedCodeBlock = *body->unlinkedCodeBlock;
    auto& binaryProfile = unlinkedCodeBlock.binaryArithProfile(0);
    auto& unaryProfile = unlinkedCodeBlock.unaryArithProfile(0);
    using TestOperation = void (*)(VM*);
    constexpr GPRReg regT0 = GPRInfo::regT0;
    constexpr GPRReg regT1 = GPRInfo::regT1;
    constexpr GPRReg regT2 = GPRInfo::regT2;
    constexpr GPRReg regT3 = GPRInfo::regT3;
    constexpr uint16_t profileMask = 0x10;

    struct ExpectedFixup {
        FixupForm form;
        ImageTarget target;
        uintptr_t value;
    };
    auto pointer = [](TargetKind kind, uint32_t a, const void* value) {
        return ExpectedFixup { FixupForm::Pointer, makeTarget(kind, a), reinterpret_cast<uintptr_t>(value) };
    };
    auto vmAddress = [&](VMAddress address, const void* value) {
        return pointer(TargetKind::VMAddress, static_cast<uint32_t>(address), value);
    };

    // Each fixup the helper recorded matches one expected reference, and every expected reference was recorded.
    auto check = [&](ASCIILiteral name, Vector<ExpectedFixup> expected, const auto& emit) {
        auto recorded = recordCode(context, vm, unlinkedCodeBlock, name, [&](CCallHelpers& jit, ImageRecorder&) {
            emit(jit);
        });
        if (!recorded)
            return;
        if (recorded->fixups.size() != expected.size()) {
            JITCACHE_FAIL(makeString(name, " recorded "_s, recorded->fixups.size(), " fixups instead of "_s, expected.size()));
            return;
        }
        Vector<bool> matched(FillWith { }, expected.size(), false);
        for (auto& fixup : recorded->fixups) {
            auto decoded = decodeFixup(recorded->linked.bytes, fixup);
            bool found = false;
            for (size_t i = 0; i < expected.size() && !found && decoded; ++i) {
                if (matched[i] || expected[i].form != fixup.form || expected[i].target != fixup.target)
                    continue;
                found = fixup.form == FixupForm::Pointer ? *decoded == expected[i].value : reaches(*decoded, expected[i].value);
                matched[i] = found;
            }
            if (!found)
                JITCACHE_FAIL(makeString(name, ": the fixup at "_s, fixup.site, " matches no reference the helper emits"_s));
        }
    };

    check("emitExceptionCheck"_s, { vmAddress(VMAddress::Exception, vm.addressOfException()) }, [&](CCallHelpers& jit) {
        jit.emitExceptionCheck(vm).link(&jit);
    });
#if ASSERT_ENABLED
    // The validation of an operation's exception register reads the VM's exception too.
    check("emitExceptionCheck(exceptionReg)"_s, { vmAddress(VMAddress::Exception, vm.addressOfException()) }, [&](CCallHelpers& jit) {
        jit.emitExceptionCheck(vm, CCallHelpers::NormalExceptionCheck, CCallHelpers::NormalJumpWidth, regT0).link(&jit);
    });
#endif
    check("emitNonNullDecodeZeroExtendedStructureID"_s, { pointer(TargetKind::StructureIDBase, 0, reinterpret_cast<const void*>(structureIDBase())) }, [&](CCallHelpers& jit) {
        jit.emitNonNullDecodeZeroExtendedStructureID(regT0, regT1);
    });
    check("branchIfTruthy"_s, { pointer(TargetKind::VMCell, static_cast<uint32_t>(VMCell::EmptyString), jsEmptyString(vm)) }, [&](CCallHelpers& jit) {
        jit.branchIfTruthy(vm, regT0, regT1, InvalidGPRReg, FPRInfo::fpRegT0, FPRInfo::fpRegT1, false, CCallHelpers::LazyBaselineGlobalObject).link(&jit);
    });
    check("getArityPadding"_s, { vmAddress(VMAddress::SoftStackLimit, vm.addressOfSoftStackLimit()) }, [&](CCallHelpers& jit) {
        CCallHelpers::JumpList stackOverflow;
        jit.getArityPadding(vm, 3, regT0, regT1, regT2, regT3, stackOverflow);
        stackOverflow.link(&jit);
    });
#if NUMBER_OF_CALLEE_SAVES_REGISTERS > 0
    check("restoreCalleeSavesFromEntryFrameCalleeSavesBuffer"_s, { vmAddress(VMAddress::TopEntryFrame, &vm.topEntryFrame) }, [&](CCallHelpers& jit) {
        jit.restoreCalleeSavesFromEntryFrameCalleeSavesBuffer(vm.topEntryFrame);
    });
    check("copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer"_s, { vmAddress(VMAddress::TopEntryFrame, &vm.topEntryFrame) }, [&](CCallHelpers& jit) {
        jit.copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer(vm.topEntryFrame);
    });
    check("copyCalleeSavesToEntryFrameCalleeSavesBuffer"_s, { vmAddress(VMAddress::TopEntryFrame, &vm.topEntryFrame) }, [&](CCallHelpers& jit) {
        jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(vm.topEntryFrame, regT0);
    });
#endif
    auto virtualCall = vm.getCTIVirtualCall(CallMode::Regular).code().untaggedPtr();
    check("emitVirtualCallWithoutMovingGlobalObject"_s, { ExpectedFixup { FixupForm::Call, makeTarget(TargetKind::VirtualCallThunk, static_cast<uint32_t>(CallMode::Regular)), reinterpret_cast<uintptr_t>(virtualCall) } }, [&](CCallHelpers& jit) {
        jit.emitVirtualCallWithoutMovingGlobalObject(vm, regT0, CallMode::Regular);
    });
    check("barrierBranch"_s, { vmAddress(VMAddress::BarrierThreshold, vm.heap.addressOfBarrierThreshold()) }, [&](CCallHelpers& jit) {
        jit.barrierBranch(vm, regT0, regT1).link(&jit);
    });
    check("jumpIfMutatorFenceNotNeeded"_s, { vmAddress(VMAddress::MutatorShouldBeFenced, vm.heap.addressOfMutatorShouldBeFenced()) }, [&](CCallHelpers& jit) {
        jit.jumpIfMutatorFenceNotNeeded(vm).link(&jit);
    });
    Vector<ExpectedFixup> topCallFrame;
#if ASSERT_ENABLED
    topCallFrame.append(vmAddress(VMAddress::TopCallFrame, &vm.topCallFrame));
#endif
    check("prepareCallOperation"_s, WTF::move(topCallFrame), [&](CCallHelpers& jit) {
        jit.prepareCallOperation(vm);
    });
    check("jumpToExceptionHandler"_s, { vmAddress(VMAddress::TargetMachinePCForThrow, &vm.targetMachinePCForThrow) }, [&](CCallHelpers& jit) {
        jit.jumpToExceptionHandler(vm);
    });
    check("ArithProfile::emitUnconditionalSet(mask)"_s, { pointer(TargetKind::UCBBinaryArithProfile, 0, binaryProfile.addressOfBits()) }, [&](CCallHelpers& jit) {
        binaryProfile.emitUnconditionalSet(jit, profileMask);
    });
    check("ArithProfile::emitUnconditionalSet(GPRReg)"_s, { pointer(TargetKind::UCBUnaryArithProfile, 0, unaryProfile.addressOfBits()) }, [&](CCallHelpers& jit) {
        unaryProfile.emitUnconditionalSet(jit, regT2);
    });
    check("CallLinkInfo::emitDataICFastPath"_s, { pointer(TargetKind::ProcessThunk, static_cast<uint32_t>(ProcessThunk::DefaultCall), LLInt::defaultCall().code().taggedPtr()) }, [&](CCallHelpers& jit) {
        CallLinkInfo::emitDataICFastPath(jit);
    });
    check("setupArguments(ImageReference)"_s, { vmAddress(VMAddress::VM, &vm) }, [&](CCallHelpers& jit) {
        jit.setupArguments<TestOperation>(ImageReference::vmAddress(vm, VMAddress::VM));
    });
    check("setupArguments(nullptr)"_s, { }, [&](CCallHelpers& jit) {
        jit.setupArguments<TestOperation>(CCallHelpers::TrustedImmPtr(nullptr));
    });

    checkNativeEmission(context, "emitExceptionCheck"_s, [&](CCallHelpers& jit) {
        jit.branchTestPtr(CCallHelpers::NonZero, CCallHelpers::AbsoluteAddress(vm.addressOfException())).link(&jit);
    }, [&](CCallHelpers& jit) {
        jit.emitExceptionCheck(vm).link(&jit);
    });
#if ASSERT_ENABLED
    checkNativeEmission(context, "emitExceptionCheck(exceptionReg)"_s, [&](CCallHelpers& jit) {
        auto ok = jit.branchPtr(CCallHelpers::Equal, CCallHelpers::AbsoluteAddress(vm.addressOfException()), regT0);
        jit.breakpoint();
        ok.link(&jit);
        jit.branchTestPtr(CCallHelpers::NonZero, regT0).link(&jit);
    }, [&](CCallHelpers& jit) {
        jit.emitExceptionCheck(vm, CCallHelpers::NormalExceptionCheck, CCallHelpers::NormalJumpWidth, regT0).link(&jit);
    });
#endif
    checkNativeEmission(context, "emitNonNullDecodeZeroExtendedStructureID"_s, [&](CCallHelpers& jit) {
        jit.or64(CCallHelpers::TrustedImm64(structureIDBase()), regT0, regT1);
    }, [&](CCallHelpers& jit) {
        jit.emitNonNullDecodeZeroExtendedStructureID(regT0, regT1);
    });
    checkNativeEmission(context, "emitVirtualCallWithoutMovingGlobalObject"_s, [&](CCallHelpers& jit) {
        jit.move(regT0, regT2);
        jit.nearCallThunk(CodeLocationLabel<JITStubRoutinePtrTag> { vm.getCTIVirtualCall(CallMode::Regular).code() });
    }, [&](CCallHelpers& jit) {
        jit.emitVirtualCallWithoutMovingGlobalObject(vm, regT0, CallMode::Regular);
    });
    checkNativeEmission(context, "ArithProfile::emitUnconditionalSet(mask)"_s, [&](CCallHelpers& jit) {
        jit.or16(CCallHelpers::TrustedImm32(profileMask), CCallHelpers::AbsoluteAddress(binaryProfile.addressOfBits()));
    }, [&](CCallHelpers& jit) {
        binaryProfile.emitUnconditionalSet(jit, profileMask);
    });
    checkNativeEmission(context, "ArithProfile::emitUnconditionalSet(GPRReg)"_s, [&](CCallHelpers& jit) {
        jit.or16(regT2, CCallHelpers::AbsoluteAddress(unaryProfile.addressOfBits()));
    }, [&](CCallHelpers& jit) {
        unaryProfile.emitUnconditionalSet(jit, regT2);
    });
#if !USE(BIGINT32)
    // AssemblyHelpers::branchIfValue for a truthiness test that skips the masquerades-as-undefined check.
    checkNativeEmission(context, "branchIfTruthy"_s, [&](CCallHelpers& jit) {
        CCallHelpers::JumpList done;
        CCallHelpers::JumpList truthy;
        auto notCell = jit.branchIfNotCell(regT0);
        auto isString = jit.branchIfString(regT0);
        auto isHeapBigInt = jit.branchIfHeapBigInt(regT0);
        truthy.append(jit.jump());
        isString.link(&jit);
        truthy.append(jit.branchPtr(CCallHelpers::NotEqual, regT0, CCallHelpers::TrustedImmPtr(jsEmptyString(vm))));
        done.append(jit.jump());
        isHeapBigInt.link(&jit);
        truthy.append(jit.branchTest32(CCallHelpers::NonZero, CCallHelpers::Address(regT0, JSBigInt::offsetOfLength())));
        done.append(jit.jump());
        notCell.link(&jit);
        auto notInt32 = jit.branchIfNotInt32(regT0);
        truthy.append(jit.branchTest32(CCallHelpers::NonZero, regT0));
        done.append(jit.jump());
        notInt32.link(&jit);
        auto notDouble = jit.branchIfNotDoubleKnownNotInt32(regT0);
        jit.unboxDouble(regT0, regT1, FPRInfo::fpRegT0);
        done.append(jit.branchDoubleZeroOrNaN(FPRInfo::fpRegT0, FPRInfo::fpRegT1));
        truthy.append(jit.jump());
        notDouble.link(&jit);
        truthy.append(jit.branch64(CCallHelpers::Equal, regT0, CCallHelpers::TrustedImm64(JSValue::encode(jsBoolean(true)))));
        done.link(&jit);
        truthy.link(&jit);
    }, [&](CCallHelpers& jit) {
        jit.branchIfTruthy(vm, regT0, regT1, InvalidGPRReg, FPRInfo::fpRegT0, FPRInfo::fpRegT1, false, CCallHelpers::LazyBaselineGlobalObject).link(&jit);
    });
#endif
    // getArityPadding for three declared parameters.
    checkNativeEmission(context, "getArityPadding"_s, [&](CCallHelpers& jit) {
        constexpr unsigned numberOfParameters = 3;
        CCallHelpers::JumpList stackOverflow;
        if (WTF::roundUpToMultipleOf(stackAlignmentRegisters(), numberOfParameters + CallFrame::headerSizeInRegisters) == numberOfParameters + CallFrame::headerSizeInRegisters)
            jit.move(CCallHelpers::TrustedImm32(numberOfParameters), regT1);
        else
            jit.move(CCallHelpers::TrustedImm32(numberOfParameters + 1), regT1);
        jit.sub32(regT1, regT0, regT1);
        jit.add32(CCallHelpers::TrustedImm32(1), regT1, regT2);
        jit.and32(CCallHelpers::TrustedImm32(~1U), regT2);
        jit.lshiftPtr(CCallHelpers::TrustedImm32(3), regT2);
        jit.subPtr(CCallHelpers::stackPointerRegister, regT2, regT3);
        stackOverflow.append(jit.branchPtr(CCallHelpers::GreaterThan, CCallHelpers::AbsoluteAddress(vm.addressOfSoftStackLimit()), regT3));
        stackOverflow.link(&jit);
    }, [&](CCallHelpers& jit) {
        CCallHelpers::JumpList stackOverflow;
        jit.getArityPadding(vm, 3, regT0, regT1, regT2, regT3, stackOverflow);
        stackOverflow.link(&jit);
    });
#if NUMBER_OF_CALLEE_SAVES_REGISTERS > 0
    // The inline copyCalleeSavesToEntryFrameCalleeSavesBuffer.
    checkNativeEmission(context, "copyCalleeSavesToEntryFrameCalleeSavesBuffer"_s, [&](CCallHelpers& jit) {
        jit.loadPtr(&vm.topEntryFrame, regT0);
        jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(regT0);
    }, [&](CCallHelpers& jit) {
        jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(vm.topEntryFrame, regT0);
    });
#endif
#if !CPU(RISCV64)
    // CallLinkInfo::emitFastPathImpl for a baseline call, which loads its CallLinkInfo from the metadata.
    checkNativeEmission(context, "CallLinkInfo::emitDataICFastPath"_s, [&](CCallHelpers& jit) {
        constexpr GPRReg calleeGPR = BaselineJITRegisters::Call::calleeGPR;
        constexpr GPRReg callLinkInfoGPR = BaselineJITRegisters::Call::callLinkInfoGPR;
        constexpr GPRReg callTargetGPR = BaselineJITRegisters::Call::callTargetGPR;
        CCallHelpers::JumpList found;
        jit.loadPtr(CCallHelpers::Address(callLinkInfoGPR, CallLinkInfo::offsetOfMonomorphicCallDestination()), callTargetGPR);
        {
            GPRReg scratchGPR = jit.scratchRegister();
            DisallowMacroScratchRegisterUsage disallowScratch(jit);
            jit.loadPtr(CCallHelpers::Address(callLinkInfoGPR, CallLinkInfo::offsetOfCallee()), scratchGPR);
            found.append(jit.branchPtr(CCallHelpers::Equal, scratchGPR, calleeGPR));
            found.append(jit.branchTestPtr(CCallHelpers::NonZero, scratchGPR, CCallHelpers::TrustedImm32(CallLinkInfo::polymorphicCalleeMask)));
        }
        jit.move(CCallHelpers::TrustedImmPtr(LLInt::defaultCall().code().taggedPtr()), callTargetGPR);
        found.link(&jit);
        jit.transferPtr(CCallHelpers::Address(callLinkInfoGPR, CallLinkInfo::offsetOfCodeBlock()), CCallHelpers::calleeFrameCodeBlockBeforeCall());
        jit.call(callTargetGPR, JSEntryPtrTag);
    }, [&](CCallHelpers& jit) {
        CallLinkInfo::emitDataICFastPath(jit);
    });
#endif
}

// The helper paths no baseline code takes, and the guard on pointer arguments, leave the record unrecordable.
JITCACHE_TEST(imageSharedHelpersMarkUnannotatedReferences, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    using TestOperation = void (*)(VM*);
    JSCell* cell = body->globalObject;

    auto check = [&](ASCIILiteral name, Unrecordable expected, const auto& emit) {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, ProducerBudget::createUnlimited());
        CCallHelpers jit;
        recorder.attachTo(jit);
        emit(jit);
        if (recorder.isRecording() || recorder.unrecordableReason() != expected)
            JITCACHE_FAIL(makeString(name, " did not leave the record unrecordable for its reason"_s));
    };

    check("setupArguments(TrustedImmPtr)"_s, Unrecordable::UnannotatedPointerArgument, [&](CCallHelpers& jit) {
        jit.setupArguments<TestOperation>(CCallHelpers::TrustedImmPtr(&vm));
    });
    check("barrierBranch(JSCell*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        jit.barrierBranch(vm, cell, GPRInfo::regT0).link(&jit);
    });
    check("barrierBranchWithoutFence(JSCell*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        jit.barrierBranchWithoutFence(cell).link(&jit);
    });
    check("branchIfTruthy(JSGlobalObject*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        jit.branchIfTruthy(vm, GPRInfo::regT0, GPRInfo::regT1, GPRInfo::regT2, FPRInfo::fpRegT0, FPRInfo::fpRegT1, true, body->globalObject).link(&jit);
    });

    // The optimizing tiers' calls, which embed their CallLinkInfo's address.
    OptimizingCallLinkInfo callLinkInfo;
    callLinkInfo.setUpCall(CallLinkInfo::Call);
    check("emitVirtualCall(CallLinkInfo*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        jit.emitVirtualCall(vm, &callLinkInfo);
    });
    check("CallLinkInfo::emitFastPath(OptimizingCallLinkInfo*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        CallLinkInfo::emitFastPath(jit, CompileTimeCallLinkInfo { &callLinkInfo });
    });
}

// A get_from_scope site of type ClosureVarWithVarInjectionChecks and one of type GlobalPropertyWithVarInjectionChecks
// reach the default branch of JIT::emit_op_get_from_scope, whose native chain links both to the thunk
// JIT::baselineThunkGenerator(GetFromScopeGlobalVar) gives. Each reader's CodeBlock is compiled twice: under a twin
// recorder, the one recorder a compilation outside production can have, whose record must hold one Call fixup in the
// site's code, with target BaselineThunk::GetFromScopeGlobalVar, reaching that thunk; and without a recorder, whose code
// must near-call that thunk once there, as the unedited engine does.
JITCACHE_TEST(imageGetFromScopeKeysTheThunkTheNativeChainLinks, Yes)
{
    VM& vm = *context.vm();
    // Each reader's scope chain passes evalScope, whose sloppy direct eval could inject a var, before the scope that holds
    // its variable: makeReaders' environment for captured, and the global object for jitcacheGlobalProperty.
    auto* globalObject = createRealm(context, vm, "globalThis.jitcacheGlobalProperty = 2; var readCaptured, readGlobalProperty; (function makeReaders() { var captured = 1; (function evalScope() { eval(''); readCaptured = function () { return captured; }; readGlobalProperty = function () { return jitcacheGlobalProperty; }; })(); })(); readCaptured(); readGlobalProperty();"_s, "the readers"_s);
    if (!globalObject)
        return;

    auto thunk = reinterpret_cast<uintptr_t>(vm.getCTIStub(JIT::baselineThunkGenerator(BaselineThunk::GetFromScopeGlobalVar)).code().untaggedPtr());
    auto thunkKey = makeTarget(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::GetFromScopeGlobalVar));

    // The twin recorder seeds the assembler. The readers have no switch, so it answers no other question from its seeds.
    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    enum class Recorder : bool {
        None,
        Twin,
    };
    auto compile = [&](CodeBlock* codeBlock, Recorder recorder) {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(codeBlock));
        JIT jit(vm, plan.get(), codeBlock);
        if (recorder == Recorder::Twin)
            jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds);
        return jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
    };

    struct Reader {
        ASCIILiteral function;
        ASCIILiteral variable;
        ResolveType type;
    };
    for (auto& reader : { Reader { "readCaptured"_s, "captured"_s, ClosureVarWithVarInjectionChecks }, Reader { "readGlobalProperty"_s, "jitcacheGlobalProperty"_s, GlobalPropertyWithVarInjectionChecks } }) {
        auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, reader.function));
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock) {
            JITCACHE_FAIL(makeString(reader.function, " has no CodeBlock"_s));
            continue;
        }

        // The site is the reader's get_from_scope of its variable, whose code runs from its instruction's start to the next
        // instruction's.
        UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
        std::optional<BytecodeIndex> site;
        std::optional<BytecodeIndex> next;
        for (const auto& instruction : unlinkedCodeBlock.instructions()) {
            if (!instruction->is<OpGetFromScope>())
                continue;
            auto bytecode = instruction->as<OpGetFromScope>();
            if (unlinkedCodeBlock.identifier(bytecode.m_var).string() != reader.variable)
                continue;
            if (bytecode.metadata(codeBlock).m_getPutInfo.resolveType() != reader.type)
                break;
            site = instruction.index();
            next = instruction.next().index();
        }
        if (!site) {
            JITCACHE_FAIL(makeString(reader.function, " has no get_from_scope of "_s, reader.variable, " of the type the test needs"_s));
            continue;
        }

        // The site's code in a compilation of the reader, as addresses.
        auto siteCode = [&](BaselineJITCode& code) -> std::optional<std::pair<uintptr_t, uintptr_t>> {
            auto start = code.m_jitCodeMap.find(*site);
            auto end = code.m_jitCodeMap.find(*next);
            if (!start || !end) {
                JITCACHE_FAIL(makeString(reader.function, "'s code map lacks the site"_s));
                return std::nullopt;
            }
            return std::pair { reinterpret_cast<uintptr_t>(start.untaggedPtr()), reinterpret_cast<uintptr_t>(end.untaggedPtr()) };
        };

        // Without a recorder: the native near call to the thunk.
        auto nativeCode = compile(codeBlock, Recorder::None);
        if (!nativeCode) {
            JITCACHE_FAIL("no executable memory for the native compilation"_s);
            return;
        }
        JITCACHE_CHECK(!nativeCode->m_jitCacheImageRecord);
        if (auto range = siteCode(*nativeCode)) {
            unsigned calls = nearCallsReaching(unsafeMakeSpan(reinterpret_cast<const uint8_t*>(range->first), range->second - range->first), thunk);
            if (calls != 1)
                JITCACHE_FAIL(makeString(reader.function, "'s native site makes "_s, calls, " near calls to the GlobalVar thunk instead of one"_s));
        }

        // Under the twin recorder: one Call fixup in the site, keyed to that thunk.
        auto twinCode = compile(codeBlock, Recorder::Twin);
        if (!twinCode) {
            JITCACHE_FAIL("no executable memory for the recorded compilation"_s);
            return;
        }
        auto* record = twinCode->m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL(makeString(reader.function, "'s record is missing or not complete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
            continue;
        }
        auto range = siteCode(*twinCode);
        if (!range)
            continue;
        auto codeStart = reinterpret_cast<uintptr_t>(twinCode->start());
        auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(twinCode->start()), record->codeSize());
        unsigned calls = 0;
        for (auto& fixup : record->fixups()) {
            // A Call's site ends its instruction, so a call inside the site's code has its site after the code's start and
            // at or before its end.
            if (fixup.form != FixupForm::Call || codeStart + fixup.site <= range->first || codeStart + fixup.site > range->second)
                continue;
            ++calls;
            auto decoded = decodeFixup(bytes, fixup);
            if (fixup.target != thunkKey || !decoded || !reaches(*decoded, thunk))
                JITCACHE_FAIL(makeString(reader.function, ": the Call fixup at "_s, fixup.site, " is not keyed to the GlobalVar thunk or does not reach it"_s));
        }
        if (calls != 1)
            JITCACHE_FAIL(makeString(reader.function, "'s recorded site holds "_s, calls, " Call fixups instead of one"_s));
    }
}

// The tail-call path of CallLinkInfo::emitFastPathImpl moves the address of operationEnsureCallLinkInfoForTailCall into a
// register and calls it, the one operation call of baseline code that no far call carries. A strict function with two
// tail calls compiles under a twin recorder, the one recorder a compilation outside production can have, to a record
// with exactly one fixup per site naming that operation's CodeSymbol, each a Pointer whose footprint holds the
// operation's address. Bun's builds load the engine at its link-time address, so no relocation comparison sees the move
// if it loses its fixup, and the corpus names a target's kind, never which operation, so this test alone would fail.
JITCACHE_TEST(imageTailCallOperationIsAPointerFixup, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = createRealm(context, vm, "function tailCallee(x) { return x + 1; } function tailCaller(x) { 'use strict'; if (x) return tailCallee(x); return tailCallee(x + 1); } tailCaller(0); tailCaller(1);"_s, "the tail caller"_s);
    if (!globalObject)
        return;
    auto* tailCaller = globalFunctionOrNull(*globalObject, "tailCaller"_s);
    CodeBlock* codeBlock = tailCaller ? tailCaller->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock) {
        JITCACHE_FAIL("tailCaller has no CodeBlock"_s);
        return;
    }
    unsigned tailCalls = 0;
    for (const auto& instruction : codeBlock->unlinkedCodeBlock()->instructions()) {
        if (instruction->is<OpTailCall>())
            ++tailCalls;
    }
    if (tailCalls != 2) {
        JITCACHE_FAIL(makeString("tailCaller holds "_s, tailCalls, " tail calls instead of two"_s));
        return;
    }
    auto symbol = CodeSymbol::of(operationEnsureCallLinkInfoForTailCall);
    if (!symbol) {
        JITCACHE_FAIL("operationEnsureCallLinkInfoForTailCall lies outside the engine's text segment"_s);
        return;
    }

    auto code = compileUnderTwinRecorder(vm, *codeBlock);
    auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
    if (!record || record->state() != RecordState::Complete) {
        JITCACHE_FAIL(makeString("tailCaller's record is missing or not complete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
        return;
    }
    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(code->start()), record->codeSize());
    ImageTarget operationTarget { .kind = TargetKind::Operation, .a = 0, .b = 0, .payload = symbol->offset };
    unsigned fixups = 0;
    for (auto& fixup : record->fixups()) {
        if (fixup.target != operationTarget)
            continue;
        ++fixups;
        auto decoded = decodeFixup(bytes, fixup);
        if (fixup.form != FixupForm::Pointer || !decoded || *decoded != reinterpret_cast<uintptr_t>(symbol->address()))
            JITCACHE_FAIL(makeString("the fixup at "_s, fixup.site, " naming the tail-call operation is no Pointer holding its address"_s));
    }
    if (fixups != 2)
        JITCACHE_FAIL(makeString("tailCaller's record holds "_s, fixups, " fixups naming the tail-call operation instead of two"_s));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
