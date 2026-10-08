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

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "FunctionExecutable.h"
#include "ImageEmission.h"
#include "ImageRecorder.h"
#include "JITCacheTest.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LLIntEntrypoint.h"
#include "LinkBuffer.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "UnlinkedCodeBlock.h"
#include <wtf/text/MakeString.h>

namespace JSC::JITCache::Tests {

namespace ImageRecordingTestsInternal {

// Every assembler a comparison emits into starts from this seed, so that a native sequence that draws (an untrusted
// immediate on x86_64) draws the same in both.
static constexpr uint32_t assemblerSeed = 0x5eed1234;

static ImageTarget makeTarget(TargetKind kind, uint32_t a = 0)
{
    return ImageTarget { .kind = kind, .a = a, .b = 0, .payload = 0 };
}

static ImageTarget commonThunkKey(CommonJITThunkID id)
{
    return makeTarget(TargetKind::CommonThunk, static_cast<uint32_t>(id));
}

static CodeLocationLabel<NoPtrTag> commonThunk(VM& vm, CommonJITThunkID id)
{
    return CodeLocationLabel<NoPtrTag>(vm.getCTIStub(id).retaggedCode<NoPtrTag>());
}

// A UCB with binary and unary arithmetic profiles, an identifier and a string constant, from a function called once so
// that its CodeBlock and UCB exist. The global object stays reachable from the caller's stack.
struct Body {
    JSGlobalObject* globalObject { nullptr };
    UnlinkedCodeBlock* unlinkedCodeBlock { nullptr };
};

static std::optional<Body> makeBody(TestContext& context, VM& vm)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("function f(a, b) { return a.x + b - -b + 'jitcache'; } f({ x: 1 }, 2);"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the test body threw"_s);
        return std::nullopt;
    }
    auto* function = jsDynamicCast<JSFunction*>(globalObject->get(globalObject, Identifier::fromString(vm, "f"_s)));
    auto* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock) {
        JITCACHE_FAIL("the test body has no CodeBlock"_s);
        return std::nullopt;
    }
    auto* unlinkedCodeBlock = codeBlock->unlinkedCodeBlock();
    if (!unlinkedCodeBlock->numberOfBinaryArithProfiles() || !unlinkedCodeBlock->numberOfUnaryArithProfiles() || !unlinkedCodeBlock->numberOfIdentifiers()) {
        JITCACHE_FAIL("the test body lacks a profile or an identifier"_s);
        return std::nullopt;
    }
    return Body { globalObject, unlinkedCodeBlock };
}

static std::optional<VirtualRegister> stringConstant(UnlinkedCodeBlock& unlinkedCodeBlock)
{
    auto& constants = unlinkedCodeBlock.constantRegisters();
    for (size_t i = 0; i < constants.size(); ++i) {
        if (constants[i].get().isString())
            return VirtualRegister(FirstConstantRegisterIndex + static_cast<int>(i));
    }
    return std::nullopt;
}

static int64_t signExtend(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

#if CPU(X86_64)
static uint64_t readLittleEndian(std::span<const uint8_t> bytes)
{
    uint64_t value = 0;
    for (size_t i = bytes.size(); i--;)
        value = (value << 8) | bytes[i];
    return value;
}
#endif

#if CPU(ARM64)
static uint32_t readWord(uintptr_t address)
{
    uint32_t word;
    memcpySpan(asMutableByteSpan(word), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(address), sizeof(word)));
    return word;
}

// The target of the branch at address on ARM64 (b, bl, b.cond, cbz, cbnz, tbz or tbnz), or nullopt for another
// instruction.
static std::optional<uintptr_t> arm64BranchTarget(uintptr_t address)
{
    uint32_t word = readWord(address);
    if ((word & 0x7c000000) == 0x14000000)
        return address + (signExtend(word & 0x3ffffff, 26) << 2);
    if ((word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000)
        return address + (signExtend((word >> 5) & 0x7ffff, 19) << 2);
    if ((word & 0x7e000000) == 0x36000000)
        return address + (signExtend((word >> 5) & 0x3fff, 14) << 2);
    return std::nullopt;
}

// The target of the conditional jump whose label, translated after compaction, is label (ARM64). Compacted to its
// direct form the jump is the branch at the label; otherwise the branch precedes the label, over a nop when the target
// is in its range, and inverted over a b to the target when it is not.
static std::optional<uintptr_t> arm64ConditionalTarget(uintptr_t label)
{
    auto isConditional = [](uint32_t word) {
        return (word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000 || (word & 0x7e000000) == 0x36000000;
    };
    constexpr uint32_t nop = 0xd503201f;
    uint32_t word = readWord(label);
    if (isConditional(word))
        return arm64BranchTarget(label);
    if (!isConditional(readWord(label - 4)))
        return std::nullopt;
    auto target = arm64BranchTarget(label - 4);
    if (word == nop)
        return target;
    if ((word & 0xfc000000) == 0x14000000 && target == label + 4)
        return arm64BranchTarget(label);
    return std::nullopt;
}
#endif

// Whether a branch to address reaches expected, following the unconditional branches of veneers and jump islands.
static bool reaches(uintptr_t address, uintptr_t expected)
{
    for (unsigned hops = 0; hops < 8; ++hops) {
        if (address == expected)
            return true;
#if CPU(ARM64)
        if (!isJITPC(reinterpret_cast<void*>(address)) || (readWord(address) & 0xfc000000) != 0x14000000)
            return false;
        address = *arm64BranchTarget(address);
#else
        return false;
#endif
    }
    return address == expected;
}

// What a fixup's footprint holds in linked code: the pointer for a Pointer, and the branch target for a Call or a
// Jump. Nullopt when the footprint does not hold its form's instruction (SPEC-image table 3.2).
static std::optional<uintptr_t> decodeFixup(std::span<const uint8_t> code, const ImageFixup& fixup)
{
    size_t site = fixup.site;
    auto codeAddress = reinterpret_cast<uintptr_t>(code.data());
#if CPU(X86_64)
    switch (fixup.form) {
    case FixupForm::Pointer:
        if (site < 10 || site > code.size() || (code[site - 10] != 0x48 && code[site - 10] != 0x49) || (code[site - 9] & 0xf8) != 0xb8)
            return std::nullopt;
        return static_cast<uintptr_t>(readLittleEndian(code.subspan(site - 8, 8)));
    case FixupForm::Call:
    case FixupForm::Jump: {
        if (site < 5 || site > code.size())
            return std::nullopt;
        bool isCall = code[site - 5] == 0xe8;
        bool isJump = code[site - 5] == 0xe9 || (site >= 6 && code[site - 6] == 0x0f && (code[site - 5] & 0xf0) == 0x80);
        if (fixup.form == FixupForm::Call ? !isCall : !isJump)
            return std::nullopt;
        int64_t displacement = signExtend(readLittleEndian(code.subspan(site - 4, 4)), 32);
        return codeAddress + site + displacement;
    }
    }
#elif CPU(ARM64)
    switch (fixup.form) {
    case FixupForm::Pointer: {
        constexpr size_t words = ARM64Assembler::NUMBER_OF_ADDRESS_ENCODING_INSTRUCTIONS;
        if (site % 4 || site + words * 4 > code.size())
            return std::nullopt;
        uint64_t value = 0;
        uint32_t first = readWord(codeAddress + site);
        for (size_t i = 0; i < words; ++i) {
            uint32_t word = readWord(codeAddress + site + i * 4);
            uint32_t opcode = i ? (0xf2800000 | (static_cast<uint32_t>(i) << 21)) : 0xd2800000;
            if ((word & 0xffe00000) != opcode || (word & 0x1f) != (first & 0x1f))
                return std::nullopt;
            value |= static_cast<uint64_t>((word >> 5) & 0xffff) << (16 * i);
        }
        return static_cast<uintptr_t>(value);
    }
    case FixupForm::Call:
        if (site < 4 || site % 4 || site > code.size() || (readWord(codeAddress + site - 4) & 0xfc000000) != 0x94000000)
            return std::nullopt;
        return arm64BranchTarget(codeAddress + site - 4);
    case FixupForm::Jump:
        if (site % 4 || site + 4 > code.size() || (readWord(codeAddress + site) & 0xfc000000) != 0x14000000)
            return std::nullopt;
        return arm64BranchTarget(codeAddress + site);
    }
#endif
    UNUSED_PARAM(codeAddress);
    return std::nullopt;
}

struct LinkedCode {
    MacroAssemblerCodeRef<JITStubRoutinePtrTag> codeRef;
    std::span<const uint8_t> bytes;
};

#if CPU(X86_64)
static std::optional<LinkedCode> linkForTest(CCallHelpers& jit)
{
    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate())
        return std::nullopt;
    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
    auto codeRef = linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s);
    return LinkedCode { WTF::move(codeRef), bytes };
}

// Whether two copies of the same code, linked at different addresses, are equal: byte for byte, except for rel32
// fields that reach the same absolute target from both copies.
static bool sameLinkedCode(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
    if (a.size() != b.size())
        return false;
    auto addressA = reinterpret_cast<uintptr_t>(a.data());
    auto addressB = reinterpret_cast<uintptr_t>(b.data());
    size_t i = 0;
    while (i < a.size()) {
        if (a[i] == b[i]) {
            ++i;
            continue;
        }
        bool matched = false;
        for (size_t start = i >= 3 ? i - 3 : 0; start <= i && !matched; ++start) {
            if (start < 1 || start + 4 > a.size())
                continue;
            bool afterBranchOpcode = a[start - 1] == 0xe8 || a[start - 1] == 0xe9 || (start >= 2 && a[start - 2] == 0x0f && (a[start - 1] & 0xf0) == 0x80);
            if (!afterBranchOpcode)
                continue;
            int64_t displacementA = signExtend(readLittleEndian(a.subspan(start, 4)), 32);
            int64_t displacementB = signExtend(readLittleEndian(b.subspan(start, 4)), 32);
            if (addressA + start + 4 + displacementA != addressB + start + 4 + displacementB)
                continue;
            matched = true;
            i = start + 4;
        }
        if (!matched)
            return false;
    }
    return true;
}
#endif

// Whether two assemblers emitted the same code: the same bytes before linking, the same jumps to link on ARM64, and on
// x86_64 the same linked code, which covers the thunk links its link tasks perform.
static bool sameEmission(CCallHelpers& a, CCallHelpers& b)
{
    auto bytesOf = [](CCallHelpers& jit) {
        auto& buffer = jit.m_assembler.buffer();
        return unsafeMakeSpan(static_cast<const uint8_t*>(buffer.data()), buffer.codeSize());
    };
    if (!equalSpans(bytesOf(a), bytesOf(b)))
        return false;
#if CPU(ARM64)
    auto& jumpsA = a.jumpsToLink();
    auto& jumpsB = b.jumpsToLink();
    if (jumpsA.size() != jumpsB.size())
        return false;
    for (size_t i = 0; i < jumpsA.size(); ++i) {
        auto& x = jumpsA[i];
        auto& y = jumpsB[i];
        if (x.from() != y.from() || x.to(&a.m_assembler) != y.to(&b.m_assembler) || x.type() != y.type() || x.condition() != y.condition()
            || x.is64Bit() != y.is64Bit() || x.bitNumber() != y.bitNumber() || x.compareRegister() != y.compareRegister()
            || x.isThunk() != y.isThunk() || x.branchType() != y.branchType())
            return false;
    }
    return true;
#else
    auto linkedA = linkForTest(a);
    auto linkedB = linkForTest(b);
    return linkedA && linkedB && sameLinkedCode(linkedA->bytes, linkedB->bytes);
#endif
}

} // namespace ImageRecordingTestsInternal

using namespace ImageRecordingTestsInternal;

// I4: without a recorder, every helper and hook emits exactly the native sequence its call sites replaced.
JITCACHE_TEST(imageHelpersEmitNativeWithoutRecorder, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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
        CCallHelpers nativeJIT;
        CCallHelpers helperJIT;
        nativeJIT.seedRandomForTwins(assemblerSeed);
        helperJIT.seedRandomForTwins(assemblerSeed);
        native(nativeJIT);
        helper(helperJIT);
        if (!sameEmission(nativeJIT, helperJIT))
            JITCACHE_FAIL(makeString("I4: "_s, name, " differs from the native sequence"_s));
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
        check("storeReferenceValue(Value)"_s, [&] (CCallHelpers& jit) {
            jit.storeValue(constantValue, Address(regT1, 8));
        }, [&] (CCallHelpers& jit) {
            storeReferenceValue(jit, ImageReference::ucbConstantCell(vm, unlinkedCodeBlock, *constant), Address(regT1, 8), StoreValueKind::Value);
        });
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
    auto body = makeBody(context, vm);
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
            JITCACHE_FAIL(makeString("I4: "_s, name, " differs from the native sequence for an unknown address"_s));
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
    auto body = makeBody(context, vm);
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
// whose target the footprint decodes to after linking (SPEC-image table 3.2, I2 and I3).
JITCACHE_TEST(imageHelpersRecordFixedFootprints, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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
    for (size_t i = 1; i < provenance->fixups.size(); ++i)
        JITCACHE_CHECK(provenance->fixups[i - 1].site < provenance->fixups[i].site);

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

    // The recorder released every byte it did not hand over (I14); the provenance's bytes are the caller's to release.
    JITCACHE_CHECK(budget->chargedBytes() == provenance->fixups.capacity() * sizeof(ImageFixup));
    budget->release(provenance->fixups.capacity() * sizeof(ImageFixup));
    JITCACHE_CHECK(!budget->chargedBytes());
}

// A thunk link that no helper made leaves the record unrecordable, and one that a helper made does not (section 4.4).
JITCACHE_TEST(imageGuardsMarkUnannotatedLinks, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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

// I19: a budget that refuses midway leaves the record Incomplete, and every conditional external jump the code emits
// is still linked to its target, through a veneer deferred before the refusal or natively after it.
JITCACHE_TEST(imageRefusedBudgetLinksEveryBranch, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto handleExceptionKey = commonThunkKey(CommonJITThunkID::HandleException);
    auto handleException = reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr());
    constexpr unsigned jumpCount = 2000;

    auto budget = ProducerBudget::create(kRecordChargeStep);
    Vector<uint32_t> sites;
    std::optional<LinkedCode> linked;
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, budget.copyRef());
        CCallHelpers jit;
        recorder.attachTo(jit);
        Vector<MacroAssembler::Jump> jumps;
        for (unsigned i = 0; i < jumpCount; ++i) {
            moveReference(jit, ImageReference::vmAddress(vm, VMAddress::VM), GPRInfo::regT2);
            auto jump = jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
            jumps.append(jump);
            linkJumpToSupport(jit, vm, jump, handleExceptionKey);
        }
        jit.ret();
        JITCACHE_CHECK(!recorder.isRecording());
        JITCACHE_CHECK(recorder.state() == RecordState::Incomplete);
        JITCACHE_CHECK(budget->hasRefused());
        recorder.emitVeneers(jit);

        LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
        if (linkBuffer.didFailToAllocate()) {
            JITCACHE_FAIL("no executable memory for the test code"_s);
            return;
        }
        for (auto& jump : jumps)
            sites.append(linkBuffer.offsetOf(jump.assemblerLabel()));
        auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
        linked.emplace(LinkedCode { linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s), bytes });
        JITCACHE_CHECK(!recorder.finishSnippet(linkBuffer));
    }
    // The recorder released everything it charged (I14).
    JITCACHE_CHECK(!budget->chargedBytes());

    auto codeAddress = reinterpret_cast<uintptr_t>(linked->bytes.data());
    auto codeEnd = codeAddress + linked->bytes.size();
    unsigned throughVeneer = 0;
    unsigned linkedNatively = 0;
    for (uint32_t site : sites) {
#if CPU(X86_64)
        // jcc rel32, which ends at the jump's label.
        ImageFixup fixup { .site = site, .form = FixupForm::Jump, .target = handleExceptionKey };
        auto target = decodeFixup(linked->bytes, fixup);
        bool linkedToThunk = target && *target == handleException;
        linkedNatively += linkedToThunk;
#elif CPU(ARM64)
        auto target = arm64ConditionalTarget(codeAddress + site);
        bool insideCode = target && *target >= codeAddress && *target < codeEnd;
        bool linkedToThunk = target && reaches(*target, handleException);
        throughVeneer += linkedToThunk && insideCode;
        linkedNatively += linkedToThunk && !insideCode;
#endif
        if (!linkedToThunk) {
            JITCACHE_FAIL(makeString("the conditional jump at "_s, site, " is not linked to its thunk"_s));
            break;
        }
    }
    UNUSED_VARIABLE(codeEnd);
    JITCACHE_CHECK(linkedNatively);
#if CPU(ARM64)
    JITCACHE_CHECK(throughVeneer);
#else
    UNUSED_VARIABLE(throughVeneer);
#endif
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
