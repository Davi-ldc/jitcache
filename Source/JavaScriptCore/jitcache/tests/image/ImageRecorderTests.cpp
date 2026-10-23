#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArithProfile.h"
#include "BinarySwitch.h"
#include "CCallHelpers.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "LLIntEntrypoint.h"
#include "LinkBuffer.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/image/ImageEmission.h"
#include "jitcache/image/ImageRecorder.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageEmissionTestSupport.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of ImageRecorder and ImageEmission (image/ImageRecorder.h, image/ImageEmission.h and their .cpp). Without a
// recorder, every emission helper and hook emits exactly the native sequence its call sites replaced. With one, each
// records its reference as one fixup in its form's fixed footprint, the recorder classifies the targets it is handed, the
// guards mark a link or a reference no helper annotated, and an inline string switch records the ranks of its keys.

namespace JSC::JITCache::Tests {

namespace ImageRecorderTestsInternal {

// A seed whose first two draws are 0 modulo MacroAssembler::BlindingModulus (64). On x86_64, MacroAssembler::shouldBlind
// blinds an untrusted 64-bit cell pointer exactly when two draws are: one while it screens the pointer as a double, and
// one after. So the first untrusted cell immediate an assembler seeded with this emits is blinded.
static constexpr uint32_t blindingSeed = 0x5eefa81a;

static std::optional<VirtualRegister> stringConstant(UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& constants = unlinkedCodeBlock.constantRegisters();
    for (size_t i = 0; i < constants.size(); ++i) {
        if (constants[i].get().isString())
            return VirtualRegister(FirstConstantRegisterIndex + static_cast<int>(i));
    }
    return std::nullopt;
}

// Whether native, started from seed, blinds: on x86_64, the one architecture that blinds, it must emit differently from
// trusted, the same sequence with every immediate trusted, which never blinds and draws nothing. A native-emission check
// from that seed then fails a helper that emits the trusted form where native's immediate is untrusted. Elsewhere nothing
// blinds, so there is nothing to assert.
template<typename Native, typename Trusted>
static void checkNativeSequenceBlinds(TestContext& context, ASCIILiteral name, uint32_t seed, const Native& native, const Trusted& trusted)
{
#if CPU(X86_64)
    CCallHelpers nativeJIT;
    CCallHelpers trustedJIT;
    nativeJIT.seedRandomForTwins(seed);
    trustedJIT.seedRandomForTwins(seed);
    native(nativeJIT);
    trusted(trustedJIT);
    if (sameEmission(nativeJIT, trustedJIT))
        JITCACHE_FAIL(makeString(name, ": the native sequence does not blind under the seed its native-emission check starts from"_s));
#else
    UNUSED_PARAM(context);
    UNUSED_PARAM(name);
    UNUSED_PARAM(seed);
    UNUSED_PARAM(native);
    UNUSED_PARAM(trusted);
#endif
}

} // namespace ImageRecorderTestsInternal

using namespace ImageRecorderTestsInternal;

// Without a recorder, every helper and hook emits exactly the native sequence its call sites replaced.
JITCACHE_TEST(imageHelpersEmitNativeWithoutRecorder, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    UnlinkedCodeBlock& unlinkedCodeBlock = *body->unlinkedCodeBlock;
    auto constant = stringConstant(unlinkedCodeBlock);
    JITCACHE_CHECK(constant);

    using Address = CCallHelpers::Address;
    using AbsoluteAddress = CCallHelpers::AbsoluteAddress;
    using TrustedImm32 = CCallHelpers::TrustedImm32;
    using TrustedImmPtr = CCallHelpers::TrustedImmPtr;
    constexpr GPRReg regT0 = GPRInfo::regT0;
    constexpr GPRReg regT1 = GPRInfo::regT1;
    constexpr GPRReg regT2 = GPRInfo::regT2;

    auto check = [&](ASCIILiteral name, const auto& native, const auto& helper) {
        checkNativeEmission(context, name, native, helper);
    };

    auto handleException = commonThunk(vm, CommonJITThunkID::HandleException);
    auto handleExceptionKey = commonThunkKey(CommonJITThunkID::HandleException);
    auto stackOverflowKey = commonThunkKey(CommonJITThunkID::ThrowStackOverflowAtPrologue);
    auto stackOverflow = commonThunk(vm, CommonJITThunkID::ThrowStackOverflowAtPrologue);
    CodeLocationLabel<JSInternalPtrTag> imageLocation(vm.getCTIStub(CommonJITThunkID::ThrowStackOverflowAtPrologue).retaggedCode<JSInternalPtrTag>());
    auto& binaryProfile = unlinkedCodeBlock.binaryArithProfile(0);
    auto& unaryProfile = unlinkedCodeBlock.unaryArithProfile(0);
    int switchTable[4] { };

    check("moveReference"_s, [&] (CCallHelpers& jit) {
        jit.move(TrustedImmPtr(&vm), regT0);
    }, [&] (CCallHelpers& jit) {
        moveReference(jit, ImageReference::vmAddress(vm, VMAddress::VM), regT0);
    });
    check("ImageReference::materialize"_s, [&] (CCallHelpers& jit) {
        jit.move(TrustedImmPtr(unlinkedCodeBlock.identifier(0).impl()), regT1);
    }, [&] (CCallHelpers& jit) {
        ImageReference::ucbIdentifier(vm, unlinkedCodeBlock, 0).materialize(jit, regT1);
    });
    check("ImageReference::store"_s, [&] (CCallHelpers& jit) {
        jit.storePtr(TrustedImmPtr(&binaryProfile), Address(regT2, 16));
    }, [&] (CCallHelpers& jit) {
        ImageReference::arithProfile(vm, unlinkedCodeBlock, binaryProfile).store(jit, Address(regT2, 16));
    });
    check("branchPtrWithReference"_s, [&] (CCallHelpers& jit) {
        jit.branchPtr(CCallHelpers::NotEqual, regT0, TrustedImmPtr(jsEmptyString(vm))).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchPtrWithReference(jit, CCallHelpers::NotEqual, regT0, ImageReference::vmCell(vm, VMCell::EmptyString)).link(&jit);
    });
    if (constant) {
        JSValue constantValue = unlinkedCodeBlock.getConstant(*constant);
        // The two helpers whose native sequence is an untrusted Imm64, which on x86_64 draws from the random source and
        // may be blinded. Each check starts from the seed under which the native sequence blinds, so that a helper that
        // emitted the trusted form, which draws nothing, would fail it. The second move shows the draws stay in step
        // after a blinding.
        auto nativeStore = [&] (CCallHelpers& jit) {
            jit.storeValue(constantValue, Address(regT1, 8));
        };
        checkNativeSequenceBlinds(context, "storeReferenceValue(Value)"_s, blindingSeed, nativeStore, [&] (CCallHelpers& jit) {
            jit.storeTrustedValue(constantValue, Address(regT1, 8));
        });
        checkNativeEmission(context, "storeReferenceValue(Value)"_s, nativeStore, [&] (CCallHelpers& jit) {
            storeReferenceValue(jit, ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant), Address(regT1, 8), StoreValueKind::Value);
        }, blindingSeed);
        auto nativeMoves = [&] (CCallHelpers& jit) {
            jit.moveValue(constantValue, regT0);
            jit.moveValue(constantValue, regT2);
        };
        checkNativeSequenceBlinds(context, "moveReferenceValue"_s, blindingSeed, nativeMoves, [&] (CCallHelpers& jit) {
            jit.moveTrustedValue(constantValue, regT0);
            jit.moveTrustedValue(constantValue, regT2);
        });
        checkNativeEmission(context, "moveReferenceValue"_s, nativeMoves, [&] (CCallHelpers& jit) {
            moveReferenceValue(jit, ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant), regT0);
            moveReferenceValue(jit, ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant), regT2);
        }, blindingSeed);
        check("branchPtrWithReference(atom)"_s, [&] (CCallHelpers& jit) {
            jit.branchPtr(CCallHelpers::Equal, regT2, TrustedImmPtr(asString(constantValue)->tryGetValueImpl())).link(&jit);
        }, [&] (CCallHelpers& jit) {
            branchPtrWithReference(jit, CCallHelpers::Equal, regT2, ImageReference::ucbConstantAtom(vm, unlinkedCodeBlock, *constant)).link(&jit);
        });
    }
    check("storeReferenceValue(Trusted)"_s, [&] (CCallHelpers& jit) {
        jit.storeTrustedValue(vm.smallStrings.sentinelString(), Address(regT1, 24));
    }, [&] (CCallHelpers& jit) {
        storeReferenceValue(jit, ImageReference::vmCell(vm, VMCell::SmallStringsSentinel), Address(regT1, 24), StoreValueKind::Trusted);
    });
    check("branchPtrAtReference"_s, [&] (CCallHelpers& jit) {
        jit.branchPtr(CCallHelpers::GreaterThan, AbsoluteAddress(vm.addressOfSoftStackLimit()), regT1).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchPtrAtReference(jit, CCallHelpers::GreaterThan, ImageReference::vmAddress(vm, VMAddress::SoftStackLimit), regT1).link(&jit);
    });
    check("branchTestPtrAtReference"_s, [&] (CCallHelpers& jit) {
        jit.branchTestPtr(CCallHelpers::NonZero, AbsoluteAddress(vm.addressOfException())).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchTestPtrAtReference(jit, CCallHelpers::NonZero, ImageReference::vmAddress(vm, VMAddress::Exception)).link(&jit);
    });
    check("branchTest32AtReference"_s, [&] (CCallHelpers& jit) {
        jit.branchTest32(CCallHelpers::NonZero, AbsoluteAddress(vm.traps().trapBitsAddress()), TrustedImm32(0x6)).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchTest32AtReference(jit, CCallHelpers::NonZero, ImageReference::vmAddress(vm, VMAddress::TrapBits), TrustedImm32(0x6)).link(&jit);
    });
    check("branchTest8AtReference"_s, [&] (CCallHelpers& jit) {
        jit.branchTest8(CCallHelpers::Zero, AbsoluteAddress(vm.heap.addressOfMutatorShouldBeFenced())).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchTest8AtReference(jit, CCallHelpers::Zero, ImageReference::vmAddress(vm, VMAddress::MutatorShouldBeFenced)).link(&jit);
    });
    check("branch32WithReferenceAt"_s, [&] (CCallHelpers& jit) {
        jit.branch32(CCallHelpers::Above, regT0, AbsoluteAddress(vm.heap.addressOfBarrierThreshold())).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branch32WithReferenceAt(jit, CCallHelpers::Above, regT0, ImageReference::vmAddress(vm, VMAddress::BarrierThreshold)).link(&jit);
    });
    check("store8AtReference"_s, [&] (CCallHelpers& jit) {
        jit.store8(TrustedImm32(1), vm.addressOfMightBeExecutingTaintedCode());
    }, [&] (CCallHelpers& jit) {
        store8AtReference(jit, TrustedImm32(1), ImageReference::vmAddress(vm, VMAddress::MightBeExecutingTaintedCode));
    });
    check("storePtrAtReference"_s, [&] (CCallHelpers& jit) {
        jit.storePtr(regT0, &vm.topCallFrame);
    }, [&] (CCallHelpers& jit) {
        storePtrAtReference(jit, regT0, ImageReference::vmAddress(vm, VMAddress::TopCallFrame));
    });
    check("loadPtrAtReference"_s, [&] (CCallHelpers& jit) {
        jit.loadPtr(&vm.topEntryFrame, regT2);
    }, [&] (CCallHelpers& jit) {
        loadPtrAtReference(jit, ImageReference::vmAddress(vm, VMAddress::TopEntryFrame), regT2);
    });
    check("or16AtReference(TrustedImm32)"_s, [&] (CCallHelpers& jit) {
        jit.or16(TrustedImm32(0x10), AbsoluteAddress(binaryProfile.addressOfBits()));
    }, [&] (CCallHelpers& jit) {
        or16AtReference(jit, TrustedImm32(0x10), ImageReference::arithProfile(vm, unlinkedCodeBlock, binaryProfile));
    });
    check("or16AtReference(GPRReg)"_s, [&] (CCallHelpers& jit) {
        jit.or16(regT2, AbsoluteAddress(unaryProfile.addressOfBits()));
    }, [&] (CCallHelpers& jit) {
        or16AtReference(jit, regT2, ImageReference::arithProfile(vm, unlinkedCodeBlock, unaryProfile));
    });
    check("orStructureIDBase"_s, [&] (CCallHelpers& jit) {
        jit.or64(CCallHelpers::TrustedImm64(structureIDBase()), regT0, regT1);
    }, [&] (CCallHelpers& jit) {
        orStructureIDBase(jit, regT0, regT1);
    });
    check("ImageReference::switchTableBase"_s, [&] (CCallHelpers& jit) {
        jit.move(TrustedImmPtr(switchTable), regT2);
    }, [&] (CCallHelpers& jit) {
        moveReference(jit, ImageReference::switchTableBase(0, switchTable), regT2);
    });
    check("ImageReference::processThunk"_s, [&] (CCallHelpers& jit) {
        jit.move(TrustedImmPtr(LLInt::defaultCall().code().taggedPtr()), regT0);
    }, [&] (CCallHelpers& jit) {
        moveReference(jit, ImageReference::processThunk(ProcessThunk::DefaultCall), regT0);
    });
    check("ImageReference::mathIC"_s, [&] (CCallHelpers& jit) {
        jit.move(TrustedImmPtr(switchTable + 1), regT1);
    }, [&] (CCallHelpers& jit) {
        moveReference(jit, ImageReference::mathIC(switchTable + 1), regT1);
    });
    check("nearCallSupport"_s, [&] (CCallHelpers& jit) {
        jit.nearCallThunk(handleException);
    }, [&] (CCallHelpers& jit) {
        nearCallSupport(jit, vm, handleExceptionKey);
    });
    check("nearTailCallSupport"_s, [&] (CCallHelpers& jit) {
        jit.nearTailCallThunk(stackOverflow);
    }, [&] (CCallHelpers& jit) {
        nearTailCallSupport(jit, vm, stackOverflowKey);
    });
    check("jumpSupport"_s, [&] (CCallHelpers& jit) {
        jit.jumpThunk(stackOverflow);
    }, [&] (CCallHelpers& jit) {
        jumpSupport(jit, vm, stackOverflowKey);
    });
    check("linkJumpToSupport(conditional)"_s, [&] (CCallHelpers& jit) {
        jit.branch32(CCallHelpers::Equal, regT0, regT1).linkThunk(handleException, &jit);
    }, [&] (CCallHelpers& jit) {
        linkJumpToSupport(jit, vm, jit.branch32(CCallHelpers::Equal, regT0, regT1), handleExceptionKey);
    });
    check("linkJumpToSupport(unconditional)"_s, [&] (CCallHelpers& jit) {
        jit.jump().linkThunk(handleException, &jit);
    }, [&] (CCallHelpers& jit) {
        linkJumpToSupport(jit, vm, jit.jump(), handleExceptionKey);
    });
    check("jumpToImage"_s, [&] (CCallHelpers& jit) {
        jit.jumpThunk(imageLocation);
    }, [&] (CCallHelpers& jit) {
        jumpToImage(jit, 64, imageLocation);
    });
    check("linkJumpsToImage"_s, [&] (CCallHelpers& jit) {
        CCallHelpers::JumpList jumps;
        jumps.append(jit.branchTest32(CCallHelpers::Zero, regT0));
        jumps.append(jit.jump());
        jumps.linkThunk(imageLocation, &jit);
    }, [&] (CCallHelpers& jit) {
        CCallHelpers::JumpList jumps;
        jumps.append(jit.branchTest32(CCallHelpers::Zero, regT0));
        jumps.append(jit.jump());
        linkJumpsToImage(jit, jumps, 128, imageLocation);
    });

    // The hooks emit natively without a recorder, as the inline functions that call them only with one rely on.
    check("storePtrToVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.storePtr(GPRInfo::callFrameRegister, &vm.topCallFrame);
    }, [&] (CCallHelpers& jit) {
        storePtrToVMAddress(jit, GPRInfo::callFrameRegister, &vm.topCallFrame);
    });
    check("loadPtrFromVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.loadPtr(&vm.targetMachinePCForThrow, regT1);
    }, [&] (CCallHelpers& jit) {
        loadPtrFromVMAddress(jit, &vm.targetMachinePCForThrow, regT1);
    });
    check("branch32WithVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.branch32(CCallHelpers::Above, regT2, AbsoluteAddress(vm.heap.addressOfBarrierThreshold())).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branch32WithVMAddress(jit, CCallHelpers::Above, regT2, vm.heap.addressOfBarrierThreshold()).link(&jit);
    });
    check("branchTest8AtVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.branchTest8(CCallHelpers::Zero, AbsoluteAddress(vm.heap.addressOfMutatorShouldBeFenced())).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchTest8AtVMAddress(jit, CCallHelpers::Zero, vm.heap.addressOfMutatorShouldBeFenced()).link(&jit);
    });
}

// A VM-address hook given an address outside the VM's table emits the native sequence under a recorder too, and makes
// the record Unrecordable(UnknownVMAddress).
JITCACHE_TEST(imageHooksEmitNativeForUnknownAddresses, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    static void* unknownAddress;
    constexpr GPRReg regT0 = GPRInfo::regT0;

    auto check = [&](ASCIILiteral name, const auto& native, const auto& hook) {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, ProducerBudget::createUnlimited());
        CCallHelpers nativeJIT;
        CCallHelpers hookJIT;
        nativeJIT.seedRandomForTwins(assemblerSeed);
        hookJIT.seedRandomForTwins(assemblerSeed);
        recorder.attachTo(hookJIT);
        native(nativeJIT);
        hook(hookJIT);
        if (!sameEmission(nativeJIT, hookJIT))
            JITCACHE_FAIL(makeString(name, " differs from the native sequence for an unknown address"_s));
        if (recorder.isRecording() || recorder.unrecordableReason() != Unrecordable::UnknownVMAddress)
            JITCACHE_FAIL(makeString(name, " left the record recordable for an unknown address"_s));
    };

    check("storePtrToVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.storePtr(regT0, &unknownAddress);
    }, [&] (CCallHelpers& jit) {
        storePtrToVMAddress(jit, regT0, &unknownAddress);
    });
    check("loadPtrFromVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.loadPtr(&unknownAddress, regT0);
    }, [&] (CCallHelpers& jit) {
        loadPtrFromVMAddress(jit, &unknownAddress, regT0);
    });
    check("branch32WithVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.branch32(CCallHelpers::Above, regT0, CCallHelpers::AbsoluteAddress(&unknownAddress)).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branch32WithVMAddress(jit, CCallHelpers::Above, regT0, &unknownAddress).link(&jit);
    });
    check("branchTest8AtVMAddress"_s, [&] (CCallHelpers& jit) {
        jit.branchTest8(CCallHelpers::Zero, CCallHelpers::AbsoluteAddress(&unknownAddress)).link(&jit);
    }, [&] (CCallHelpers& jit) {
        branchTest8AtVMAddress(jit, CCallHelpers::Zero, &unknownAddress).link(&jit);
    });
}

// The recorder classifies the addresses the hooks and the arithmetic-profile writes hand it, and names a noted MathIC
// by its index.
JITCACHE_TEST(imageRecorderClassifiesTargets, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    UnlinkedCodeBlock& unlinkedCodeBlock = *body->unlinkedCodeBlock;
    ImageRecorder recorder(RecordingScope::BaselineCompile, vm, unlinkedCodeBlock, ProducerBudget::createUnlimited());
    JITCACHE_CHECK(recorder.chargedRecordObject());

    for (uint32_t a = 1; a <= static_cast<uint32_t>(VMAddress::SyncResumeCallCache); ++a) {
        auto reference = ImageReference::vmAddress(vm, static_cast<VMAddress>(a));
        auto target = recorder.vmAddressTarget(reference.value());
        JITCACHE_CHECK(target == makeTarget(TargetKind::VMAddress, a));
    }
    static int notAVMAddress;
    JITCACHE_CHECK(!recorder.vmAddressTarget(&notAVMAddress));

    auto binary = recorder.arithProfileTarget(&unlinkedCodeBlock.binaryArithProfile(0));
    JITCACHE_CHECK(binary == makeTarget(TargetKind::UCBBinaryArithProfile, 0));
    auto unary = recorder.arithProfileTarget(&unlinkedCodeBlock.unaryArithProfile(0));
    JITCACHE_CHECK(unary == makeTarget(TargetKind::UCBUnaryArithProfile, 0));
    JITCACHE_CHECK(!recorder.arithProfileTarget(&notAVMAddress));

    int mathICs[2] { };
    JITCACHE_CHECK(!recorder.noteMathIC(MathICKind::Add, &mathICs[0], BytecodeIndex(0)));
    JITCACHE_CHECK(recorder.noteMathIC(MathICKind::Negate, &mathICs[1], BytecodeIndex(4)) == 1);
    auto second = ImageReference::mathIC(&mathICs[1]).recordedTarget(recorder);
    JITCACHE_CHECK(second == makeTarget(TargetKind::MathIC, 1));
    auto first = ImageReference::mathIC(&mathICs[0]).recordedTarget(recorder);
    JITCACHE_CHECK(first == makeTarget(TargetKind::MathIC, 0));
    JITCACHE_CHECK(recorder.isRecording());
}

// With a recorder, every helper emits its reference in its form's fixed footprint and records exactly one fixup there,
// whose target the footprint decodes to after linking.
JITCACHE_TEST(imageHelpersRecordFixedFootprints, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    UnlinkedCodeBlock& unlinkedCodeBlock = *body->unlinkedCodeBlock;
    auto constant = stringConstant(unlinkedCodeBlock);
    if (!constant) {
        JITCACHE_FAIL("the test body has no string constant"_s);
        return;
    }

    using Address = CCallHelpers::Address;
    using TrustedImm32 = CCallHelpers::TrustedImm32;
    constexpr GPRReg regT0 = GPRInfo::regT0;
    constexpr GPRReg regT1 = GPRInfo::regT1;
    constexpr GPRReg regT2 = GPRInfo::regT2;

    struct Expectation {
        FixupForm form;
        ImageTarget target;
        uintptr_t value;
        bool matched { false };
    };
    Vector<Expectation> expectations;
    auto expectPointerAt = [&](const ImageTarget& target, const void* value) {
        expectations.append(Expectation { FixupForm::Pointer, target, reinterpret_cast<uintptr_t>(value) });
    };
    auto expectPointer = [&](const ImageReference& reference) {
        expectPointerAt(reference.target(), reference.value());
        return reference;
    };
    auto expectBranch = [&](FixupForm form, const ImageTarget& target, const void* location) {
        expectations.append(Expectation { form, target, reinterpret_cast<uintptr_t>(location) });
    };

    auto handleExceptionKey = commonThunkKey(CommonJITThunkID::HandleException);
    auto handleException = commonThunk(vm, CommonJITThunkID::HandleException);
    auto stackOverflowKey = commonThunkKey(CommonJITThunkID::ThrowStackOverflowAtPrologue);
    auto stackOverflow = commonThunk(vm, CommonJITThunkID::ThrowStackOverflowAtPrologue);
    CodeLocationLabel<JSInternalPtrTag> imageLocation(vm.getCTIStub(CommonJITThunkID::ThrowStackOverflowAtPrologue).retaggedCode<JSInternalPtrTag>());
    int switchTable[4] { };

    auto budget = ProducerBudget::createUnlimited();
    std::optional<SnippetProvenance> provenance;
    size_t linkedSize = 0;
    std::optional<LinkedCode> linked;
    Vector<MacroAssembler::Jump> conditionals;
    Vector<uint32_t> conditionalSites;
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, unlinkedCodeBlock, budget.copyRef());
        CCallHelpers jit;
        recorder.attachTo(jit);

        moveReference(jit, expectPointer(ImageReference::vmAddress(vm, VMAddress::VM)), regT0);
        expectPointer(ImageReference::ucbIdentifier(vm, unlinkedCodeBlock, 0)).materialize(jit, regT1);
        expectPointer(ImageReference::arithProfile(vm, unlinkedCodeBlock, unlinkedCodeBlock.binaryArithProfile(0))).store(jit, Address(regT2, 16));
        branchPtrWithReference(jit, CCallHelpers::NotEqual, regT0, expectPointer(ImageReference::vmCell(vm, VMCell::EmptyString))).link(&jit);
        branchPtrWithReference(jit, CCallHelpers::Equal, regT2, expectPointer(ImageReference::ucbConstantAtom(vm, unlinkedCodeBlock, *constant))).link(&jit);
        storeReferenceValue(jit, expectPointer(ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant)), Address(regT1, 8), StoreValueKind::Value);
        moveReferenceValue(jit, expectPointer(ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant)), regT0);
        storeReferenceValue(jit, expectPointer(ImageReference::vmCell(vm, VMCell::SmallStringsSentinel)), Address(regT1, 24), StoreValueKind::Trusted);
        branchPtrAtReference(jit, CCallHelpers::GreaterThan, expectPointer(ImageReference::vmAddress(vm, VMAddress::SoftStackLimit)), regT1).link(&jit);
        branch32WithReferenceAt(jit, CCallHelpers::Above, regT0, expectPointer(ImageReference::vmAddress(vm, VMAddress::BarrierThreshold))).link(&jit);
        branchTest32AtReference(jit, CCallHelpers::NonZero, expectPointer(ImageReference::vmAddress(vm, VMAddress::TrapBits)), TrustedImm32(0x6)).link(&jit);
        branchTest8AtReference(jit, CCallHelpers::Zero, expectPointer(ImageReference::vmAddress(vm, VMAddress::MutatorShouldBeFenced))).link(&jit);
        store8AtReference(jit, TrustedImm32(1), expectPointer(ImageReference::vmAddress(vm, VMAddress::MightBeExecutingTaintedCode)));
        storePtrAtReference(jit, regT0, expectPointer(ImageReference::vmAddress(vm, VMAddress::TopCallFrame)));
        loadPtrAtReference(jit, expectPointer(ImageReference::vmAddress(vm, VMAddress::TopEntryFrame)), regT2);
        or16AtReference(jit, TrustedImm32(0x10), expectPointer(ImageReference::arithProfile(vm, unlinkedCodeBlock, unlinkedCodeBlock.binaryArithProfile(0))));
        or16AtReference(jit, regT2, expectPointer(ImageReference::arithProfile(vm, unlinkedCodeBlock, unlinkedCodeBlock.unaryArithProfile(0))));
        moveReference(jit, expectPointer(ImageReference::switchTableBase(3, switchTable)), regT1);
        moveReference(jit, expectPointer(ImageReference::processThunk(ProcessThunk::DefaultCall)), regT0);
        expectPointerAt(makeTarget(TargetKind::StructureIDBase), reinterpret_cast<const void*>(structureIDBase()));
        orStructureIDBase(jit, regT0, regT1);

        // The hooks emit their helper's recorded sequence for an address of the VM's table.
        expectPointerAt(makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::TopCallFrame)), &vm.topCallFrame);
        storePtrToVMAddress(jit, GPRInfo::callFrameRegister, &vm.topCallFrame);
        expectPointerAt(makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::TargetMachinePCForThrow)), &vm.targetMachinePCForThrow);
        loadPtrFromVMAddress(jit, &vm.targetMachinePCForThrow, regT1);
        expectPointerAt(makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::BarrierThreshold)), vm.heap.addressOfBarrierThreshold());
        branch32WithVMAddress(jit, CCallHelpers::Above, regT2, vm.heap.addressOfBarrierThreshold()).link(&jit);
        expectPointerAt(makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(VMAddress::MutatorShouldBeFenced)), vm.heap.addressOfMutatorShouldBeFenced());
        branchTest8AtVMAddress(jit, CCallHelpers::Zero, vm.heap.addressOfMutatorShouldBeFenced()).link(&jit);

        expectBranch(FixupForm::Call, handleExceptionKey, handleException.untaggedPtr());
        nearCallSupport(jit, vm, handleExceptionKey);
        expectBranch(FixupForm::Jump, stackOverflowKey, stackOverflow.untaggedPtr());
        nearTailCallSupport(jit, vm, stackOverflowKey);
        expectBranch(FixupForm::Jump, stackOverflowKey, stackOverflow.untaggedPtr());
        jumpSupport(jit, vm, stackOverflowKey);
        expectBranch(FixupForm::Jump, handleExceptionKey, handleException.untaggedPtr());
        linkJumpToSupport(jit, vm, jit.jump(), handleExceptionKey);
        expectBranch(FixupForm::Jump, makeTarget(TargetKind::ImageOffset, 64), imageLocation.untaggedPtr());
        jumpToImage(jit, 64, imageLocation);

        // Conditional external jumps: on ARM64 one veneer per target carries the fixup, on x86_64 each jcc does.
        auto exceptionCheck = branchTestPtrAtReference(jit, CCallHelpers::NonZero, expectPointer(ImageReference::vmAddress(vm, VMAddress::Exception)));
        conditionals.append(exceptionCheck);
        linkJumpToSupport(jit, vm, exceptionCheck, handleExceptionKey);
        auto secondCheck = jit.branch32(CCallHelpers::Equal, regT0, regT1);
        conditionals.append(secondCheck);
        linkJumpToSupport(jit, vm, secondCheck, handleExceptionKey);
        CCallHelpers::JumpList toImage;
        auto toImageConditional = jit.branchTest32(CCallHelpers::Zero, regT2);
        conditionals.append(toImageConditional);
        toImage.append(toImageConditional);
        toImage.append(jit.jump());
        linkJumpsToImage(jit, toImage, 128, imageLocation);
        expectBranch(FixupForm::Jump, makeTarget(TargetKind::ImageOffset, 128), imageLocation.untaggedPtr());
#if CPU(ARM64)
        expectBranch(FixupForm::Jump, handleExceptionKey, handleException.untaggedPtr());
        expectBranch(FixupForm::Jump, makeTarget(TargetKind::ImageOffset, 128), imageLocation.untaggedPtr());
#else
        expectBranch(FixupForm::Jump, handleExceptionKey, handleException.untaggedPtr());
        expectBranch(FixupForm::Jump, handleExceptionKey, handleException.untaggedPtr());
        expectBranch(FixupForm::Jump, makeTarget(TargetKind::ImageOffset, 128), imageLocation.untaggedPtr());
#endif
        jit.ret();
        recorder.emitVeneers(jit);
        JITCACHE_CHECK(recorder.isRecording());

        LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
        if (linkBuffer.didFailToAllocate()) {
            JITCACHE_FAIL("no executable memory for the test code"_s);
            return;
        }
        for (auto& jump : conditionals)
            conditionalSites.append(linkBuffer.offsetOf(jump.assemblerLabel()));
        linkedSize = linkBuffer.size();
        auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
        linked.emplace(LinkedCode { linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s), bytes });
        provenance = recorder.finishSnippet(linkBuffer);
        if (provenance)
            recorder.handChargeToRecord(provenance->fixups.capacity() * sizeof(ImageFixup));
    }

    if (!provenance) {
        JITCACHE_FAIL("the recorder produced no provenance"_s);
        return;
    }
    JITCACHE_CHECK(provenance->start == linked->bytes.data());
    JITCACHE_CHECK(provenance->size == linkedSize);
    JITCACHE_CHECK(provenance->fixups.size() == expectations.size());
    // Footprint order: by site, and at a shared site the Call first. Only ARM64 shares a site, since a Call's site ends
    // its footprint while a Pointer's or a Jump's starts it; the bl of nearCallSupport followed at once by the b of
    // nearTailCallSupport above is such a pair.
    bool sawSharedSite = false;
    for (size_t i = 1; i < provenance->fixups.size(); ++i) {
        auto& previous = provenance->fixups[i - 1];
        auto& fixup = provenance->fixups[i];
        bool sharedSite = previous.site == fixup.site && previous.form == FixupForm::Call && fixup.form != FixupForm::Call;
        sawSharedSite |= sharedSite;
#if CPU(ARM64)
        JITCACHE_CHECK(previous.site < fixup.site || sharedSite);
#else
        JITCACHE_CHECK(previous.site < fixup.site);
#endif
    }
#if CPU(ARM64)
    JITCACHE_CHECK(sawSharedSite);
#else
    JITCACHE_CHECK(!sawSharedSite);
#endif

    for (auto& fixup : provenance->fixups) {
        auto decoded = decodeFixup(linked->bytes, fixup);
        if (!decoded) {
            JITCACHE_FAIL(makeString("the fixup at "_s, fixup.site, " does not hold its form's instruction"_s));
            continue;
        }
        bool found = false;
        for (auto& expectation : expectations) {
            if (expectation.matched || expectation.form != fixup.form || expectation.target != fixup.target)
                continue;
            bool valueMatches = fixup.form == FixupForm::Pointer ? *decoded == expectation.value : reaches(*decoded, expectation.value);
            if (!valueMatches)
                continue;
            expectation.matched = true;
            found = true;
            break;
        }
        if (!found)
            JITCACHE_FAIL(makeString("the fixup at "_s, fixup.site, " matches no reference the test emitted"_s));
    }

#if CPU(ARM64)
    // Every conditional external jump reaches a veneer, a b inside the code that carries a Jump fixup.
    auto codeAddress = reinterpret_cast<uintptr_t>(linked->bytes.data());
    for (uint32_t site : conditionalSites) {
        auto target = arm64ConditionalTarget(codeAddress + site);
        bool reachesVeneer = false;
        if (target && *target >= codeAddress && *target < codeAddress + linkedSize) {
            uint32_t veneerSite = static_cast<uint32_t>(*target - codeAddress);
            for (auto& fixup : provenance->fixups)
                reachesVeneer |= fixup.form == FixupForm::Jump && fixup.site == veneerSite;
        }
        if (!reachesVeneer)
            JITCACHE_FAIL(makeString("the conditional jump at "_s, site, " does not reach a veneer"_s));
    }
#else
    UNUSED_VARIABLE(conditionalSites);
#endif

    // The recorder released every byte it did not hand over; the provenance's bytes are the caller's to release.
    JITCACHE_CHECK(budget->chargedBytes() == provenance->fixups.capacity() * sizeof(ImageFixup));
    budget->release(provenance->fixups.capacity() * sizeof(ImageFixup));
    JITCACHE_CHECK(!budget->chargedBytes());
}

// A thunk link that no helper made leaves the record unrecordable, and one that a helper made does not.
JITCACHE_TEST(imageGuardsMarkUnannotatedLinks, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, ProducerBudget::createUnlimited());
        CCallHelpers jit;
        recorder.attachTo(jit);
        jumpSupport(jit, vm, commonThunkKey(CommonJITThunkID::HandleException));
        nearCallSupport(jit, vm, commonThunkKey(CommonJITThunkID::HandleException));
        linkJumpToSupport(jit, vm, jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1), commonThunkKey(CommonJITThunkID::HandleException));
        recorder.emitVeneers(jit);
        JITCACHE_CHECK(recorder.isRecording());
        jit.jumpThunk(commonThunk(vm, CommonJITThunkID::HandleException));
        JITCACHE_CHECK(!recorder.isRecording());
        JITCACHE_CHECK(recorder.unrecordableReason() == Unrecordable::UnannotatedSupportLink);
    }
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, ProducerBudget::createUnlimited());
        notePointerArgument(recorder);
        JITCACHE_CHECK(recorder.unrecordableReason() == Unrecordable::UnannotatedPointerArgument);
    }
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, ProducerBudget::createUnlimited());
        noteUnannotatedReference(recorder);
        JITCACHE_CHECK(recorder.unrecordableReason() == Unrecordable::UnannotatedReference);
        // The first reason is final.
        notePointerArgument(recorder);
        JITCACHE_CHECK(recorder.unrecordableReason() == Unrecordable::UnannotatedReference);
    }
}

// An inline switch_string's tree records its ranks: every comparison against the key of rank r is a Pointer fixup with
// target SwitchStringRankAtom(table, r) holding that key, every rank has one, and the leaf jump that executes rank r is
// that rank's only SwitchStringRankCase fixup.
JITCACHE_TEST(imageStringSwitchRecordsRanks, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    constexpr unsigned tableIndex = 2;
    // Distinct ints lie at least four bytes apart, so no two keys are consecutive values, as with distinct atoms, and the
    // tree compares every key. The keys go in against their address order, so that a case's index and rank differ.
    static int keyStorage[9];
    Vector<int64_t> keys;
    for (size_t i = std::size(keyStorage); i--;)
        keys.append(static_cast<int64_t>(reinterpret_cast<intptr_t>(&keyStorage[i])));
    Vector<int64_t> sortedKeys = keys;
    std::sort(sortedKeys.begin(), sortedKeys.end());
    unsigned keyCount = keys.size();

    Vector<unsigned> executions(FillWith { }, keyCount, 0);
    auto recorded = recordCode(context, vm, *body->unlinkedCodeBlock, "the string switch"_s, [&](CCallHelpers& jit, ImageRecorder& recorder) {
        StringSwitchRecording recording(recorder, tableIndex);
        BinarySwitch binarySwitch(GPRInfo::regT0, keys.span(), BinarySwitch::IntPtr);
        binarySwitch.setRankedComparisons(&recording);
        CCallHelpers::JumpList leaves;
        while (binarySwitch.advance(jit)) {
            unsigned rank = binarySwitch.caseRank();
            if (rank >= keyCount || sortedKeys[rank] != keys[binarySwitch.caseIndex()]) {
                JITCACHE_FAIL(makeString("case "_s, binarySwitch.caseIndex(), " executes with rank "_s, rank));
                return;
            }
            ++executions[rank];
            auto leaf = jit.jump();
            recording.recordCase(leaf, rank);
            leaves.append(leaf);
        }
        binarySwitch.fallThrough().link(&jit);
        leaves.link(&jit);
    });
    if (!recorded)
        return;
    for (unsigned rank = 0; rank < keyCount; ++rank)
        JITCACHE_CHECK(executions[rank] == 1);

    Vector<unsigned> atomFixups(FillWith { }, keyCount, 0);
    Vector<unsigned> caseFixups(FillWith { }, keyCount, 0);
    auto codeStart = reinterpret_cast<uintptr_t>(recorded->linked.bytes.data());
    auto codeEnd = codeStart + recorded->linked.bytes.size();
    for (auto& fixup : recorded->fixups) {
        auto& target = fixup.target;
        auto decoded = decodeFixup(recorded->linked.bytes, fixup);
        bool isRankFixup = (target.kind == TargetKind::SwitchStringRankAtom || target.kind == TargetKind::SwitchStringRankCase)
            && target.a == tableIndex && target.b < keyCount && !target.payload;
        if (!isRankFixup || !decoded) {
            JITCACHE_FAIL(makeString("the fixup at "_s, fixup.site, " is not a rank fixup of the table"_s));
            continue;
        }
        if (target.kind == TargetKind::SwitchStringRankAtom) {
            JITCACHE_CHECK(fixup.form == FixupForm::Pointer);
            JITCACHE_CHECK(*decoded == static_cast<uintptr_t>(sortedKeys[target.b]));
            ++atomFixups[target.b];
            continue;
        }
        // The case jump is an internal branch, which stays inside the code.
        JITCACHE_CHECK(fixup.form == FixupForm::Jump);
        JITCACHE_CHECK(*decoded >= codeStart && *decoded < codeEnd);
        ++caseFixups[target.b];
    }
    for (unsigned rank = 0; rank < keyCount; ++rank) {
        JITCACHE_CHECK(atomFixups[rank] >= 1);
        JITCACHE_CHECK(caseFixups[rank] == 1);
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
