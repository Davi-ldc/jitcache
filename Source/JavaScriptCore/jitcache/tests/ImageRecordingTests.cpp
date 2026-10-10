#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArithProfile.h"
#include "BakedFacts.h"
#include "BaselineJITPlan.h"
#include "BaselineJITRegisters.h"
#include "BinarySwitch.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "ImageCapture.h"
#include "ImageEmission.h"
#include "ImagePrepare.h"
#include "ImageRecord.h"
#include "ImageRecorder.h"
#include "ImageSection.h"
#include "ImageTwins.h"
#include "JIT.h"
#include "JITCacheTest.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LLIntEntrypoint.h"
#include "LinkBuffer.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "TwinReport.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <array>
#include <wtf/text/MakeString.h>

namespace JSC::JITCache::Tests {

namespace ImageRecordingTestsInternal {

// Every assembler a comparison emits into starts from this seed, so that a native sequence that draws (an untrusted
// immediate on x86_64) draws the same in both.
static constexpr uint32_t assemblerSeed = 0x5eed1234;

// A seed whose first two draws are 0 modulo MacroAssembler::BlindingModulus (64). On x86_64, MacroAssembler::shouldBlind
// blinds an untrusted 64-bit cell pointer exactly when two draws are: one while it screens the pointer as a double, and
// one after. So the first untrusted cell immediate an assembler seeded with this emits is blinded.
static constexpr uint32_t blindingSeed = 0x5eefa81a;

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
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "f"_s)));
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

// I4: helper, without a recorder, emits exactly what native does, both starting from seed.
template<typename Native, typename Helper>
static void checkNativeEmission(TestContext& context, ASCIILiteral name, const Native& native, const Helper& helper, uint32_t seed = assemblerSeed)
{
    CCallHelpers nativeJIT;
    CCallHelpers helperJIT;
    nativeJIT.seedRandomForTwins(seed);
    helperJIT.seedRandomForTwins(seed);
    native(nativeJIT);
    helper(helperJIT);
    if (!sameEmission(nativeJIT, helperJIT))
        JITCACHE_FAIL(makeString("I4: "_s, name, " differs from the native sequence"_s));
}

// Whether native, started from seed, blinds: on x86_64, the one architecture that blinds, it must emit differently from
// trusted, the same sequence with every immediate trusted, which never blinds and draws nothing. An I4 check from that
// seed then fails a helper that emits the trusted form where native's immediate is untrusted. Elsewhere nothing blinds,
// so there is nothing to assert.
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
        JITCACHE_FAIL(makeString(name, ": the native sequence does not blind under the seed its I4 check starts from"_s));
#else
    UNUSED_PARAM(context);
    UNUSED_PARAM(name);
    UNUSED_PARAM(seed);
    UNUSED_PARAM(native);
    UNUSED_PARAM(trusted);
#endif
}

struct RecordedCode {
    LinkedCode linked;
    Vector<ImageFixup> fixups; // In footprint order, sites relative to the code.
};

// The code emit produces under a fresh recorder, followed by a ret, linked, with the fixups the recorder recorded. Reports
// and returns nullopt when the recorder stopped recording or no executable memory was left.
template<typename Emit>
static std::optional<RecordedCode> recordCode(TestContext& context, VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, ASCIILiteral name, const Emit& emit)
{
    auto budget = ProducerBudget::createUnlimited();
    ImageRecorder recorder(RecordingScope::MathICSnippet, vm, unlinkedCodeBlock, budget.copyRef());
    CCallHelpers jit;
    recorder.attachTo(jit);
    emit(jit, recorder);
    jit.ret();
    recorder.emitVeneers(jit);
    if (!recorder.isRecording()) {
        JITCACHE_FAIL(makeString(name, " left the record unrecordable"_s));
        return std::nullopt;
    }
    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
    if (linkBuffer.didFailToAllocate()) {
        JITCACHE_FAIL("no executable memory for the test code"_s);
        return std::nullopt;
    }
    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
    auto codeRef = linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s);
    auto provenance = recorder.finishSnippet(linkBuffer);
    if (!provenance) {
        JITCACHE_FAIL(makeString(name, " produced no provenance"_s));
        return std::nullopt;
    }
    // The fixups leave with the test, which releases their charge here; the budget only counts it.
    size_t fixupBytes = provenance->fixups.capacity() * sizeof(ImageFixup);
    recorder.handChargeToRecord(fixupBytes);
    budget->release(fixupBytes);
    return RecordedCode { LinkedCode { WTF::move(codeRef), bytes }, WTF::move(provenance->fixups) };
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

// Census part A. Under a recorder, each shared helper emits its reference in its recorded form, records it and leaves
// the record complete. Without one, each helper that now builds its reference through a factory and an emission helper
// emits its census native expression, restated below (I4), which also checks the factory's value at that site. The
// inline functions keep their native code and call a hook only under a recorder, and
// restoreCalleeSavesFromEntryFrameCalleeSavesBuffer and
// copyLLIntBaselineCalleeSavesFromFrameOrRegisterToEntryFrameCalleeSavesBuffer change only their loadPtr, which becomes
// loadPtrFromVMAddress, whose I4 imageHelpersEmitNativeWithoutRecorder checks. JITSlowPathCall::call and
// JIT::exceptionCheck(Jump) emit only inside a JIT, so the baseline compilations of the JS image tests and their twin
// checks cover them.
JITCACHE_TEST(imageSharedHelpersRecordTheirReferences, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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
    // AssemblyHelpers::branchIfValue for a truthiness test that skips the masquerades-as-undefined check (A7).
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
    // A8, three declared parameters.
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
    // The inline A10.
    checkNativeEmission(context, "copyCalleeSavesToEntryFrameCalleeSavesBuffer"_s, [&](CCallHelpers& jit) {
        jit.loadPtr(&vm.topEntryFrame, regT0);
        jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(regT0);
    }, [&](CCallHelpers& jit) {
        jit.copyCalleeSavesToEntryFrameCalleeSavesBuffer(vm.topEntryFrame, regT0);
    });
#endif
#if !CPU(RISCV64)
    // CallLinkInfo::emitFastPathImpl for a baseline call, which loads its CallLinkInfo from the metadata (A15).
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

// The census paths no baseline code takes, and the guard on pointer arguments, leave the record unrecordable.
JITCACHE_TEST(imageSharedHelpersMarkUnannotatedReferences, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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

    // The optimizing tiers' calls, which embed their CallLinkInfo's address (A11a, A15a).
    OptimizingCallLinkInfo callLinkInfo;
    callLinkInfo.setUpCall(CallLinkInfo::Call);
    check("emitVirtualCall(CallLinkInfo*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        jit.emitVirtualCall(vm, &callLinkInfo);
    });
    check("CallLinkInfo::emitFastPath(OptimizingCallLinkInfo*)"_s, Unrecordable::UnannotatedReference, [&](CCallHelpers& jit) {
        CallLinkInfo::emitFastPath(jit, CompileTimeCallLinkInfo { &callLinkInfo });
    });
}

// An inline switch_string's tree records its ranks (SPEC-image section 3.7, I15): every comparison against the key of
// rank r is a Pointer fixup with target SwitchStringRankAtom(table, r) holding that key, every rank has one, and the leaf
// jump that executes rank r is that rank's only SwitchStringRankCase fixup.
JITCACHE_TEST(imageStringSwitchRecordsRanks, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
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

// T10 (SPEC-image section 6.4, I16). With the LLInt off, negate's CodeBlock is born in baseline before its site runs, so
// the site's MathIC starts from an empty profile: the first int32 regenerates it with an int32 fast path, and the first
// double repoints its slow call and gives it the full snippet. Each later call reaches that slow call with a type no
// earlier call showed the profile. The type reaches the UCB profile only if the operation observed it through the IC's
// arithProfile(); an operation that took the IC's address as its profile would OR it into that pointer instead. So the
// UCB profile gaining each type shows both that it keeps learning and that no slow call wrote into the IC.
JITCACHE_TEST_WITH_OPTIONS(imageNegateProfilesThroughICAfterRegeneration, Yes, "--useLLInt=false")
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    auto run = [&](ASCIILiteral source) {
        NakedPtr<Exception> exception;
        evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception)
            JITCACHE_FAIL(makeString("evaluating \""_s, source, "\" threw"_s));
        return !exception;
    };

    if (!run("function negate(x) { return -x; } negate(1); negate(1.5);"_s))
        return;
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "negate"_s)));
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("negate has no baseline CodeBlock"_s);
        return;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *codeBlock->unlinkedCodeBlock();
    if (unlinkedCodeBlock.numberOfUnaryArithProfiles() != 1) {
        JITCACHE_FAIL("negate's body does not have exactly one unary arithmetic profile"_s);
        return;
    }
    UnaryArithProfile& profile = unlinkedCodeBlock.unaryArithProfile(0);

    // The repatching operation observed each operand before regenerating, and the double's result after.
    JITCACHE_CHECK(profile.argObservedType() == ObservedType().withInt32().withNumber());
    JITCACHE_CHECK(profile.didObserveDouble());
    JITCACHE_CHECK(!profile.didObserveBigInt());

    // After the regeneration: a non-number operand, then a BigInt result.
    if (!run("negate('x');"_s))
        return;
    JITCACHE_CHECK(profile.argObservedType().sawNonNumber());
    if (!run("negate(5n);"_s))
        return;
    JITCACHE_CHECK(profile.didObserveBigInt());

    // Every call ran this baseline code, so each observation came from its MathIC's slow call.
    JITCACHE_CHECK(function->jsExecutable()->codeBlockForCall() == codeBlock);
    JITCACHE_CHECK(codeBlock->jitType() == JITType::BaselineJIT);
}

// T20 (census D4). A get_from_scope site of type ClosureVarWithVarInjectionChecks and one of type
// GlobalPropertyWithVarInjectionChecks reach the default branch of JIT::emit_op_get_from_scope, whose native chain links
// both to the thunk JIT::baselineThunkGenerator(GetFromScopeGlobalVar) gives. Each reader's CodeBlock is compiled twice:
// under a twin recorder, the one recorder a compilation outside production can have (SPEC-image section 11.3, step 4),
// whose record must hold one Call fixup in the site's code, with target BaselineThunk::GetFromScopeGlobalVar, reaching
// that thunk; and without a recorder, whose code must near-call that thunk once there, as the unedited engine does (I4).
JITCACHE_TEST(imageGetFromScopeKeysTheThunkTheNativeChainLinks, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    // Each reader's scope chain passes evalScope, whose sloppy direct eval could inject a var, before the scope that holds
    // its variable: makeReaders' environment for captured, and the global object for jitcacheGlobalProperty.
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("globalThis.jitcacheGlobalProperty = 2; var readCaptured, readGlobalProperty; (function makeReaders() { var captured = 1; (function evalScope() { eval(''); readCaptured = function () { return captured; }; readGlobalProperty = function () { return jitcacheGlobalProperty; }; })(); })(); readCaptured(); readGlobalProperty();"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the readers threw"_s);
        return;
    }

    auto thunk = reinterpret_cast<uintptr_t>(vm.getCTIStub(JIT::baselineThunkGenerator(BaselineThunk::GetFromScopeGlobalVar)).code().untaggedPtr());
    auto thunkKey = makeTarget(TargetKind::BaselineThunk, static_cast<uint32_t>(BaselineThunk::GetFromScopeGlobalVar));

    // The twin recorder seeds the assembler. The readers have no strict equality and no switch, so it answers no other
    // question from its seeds and inputs.
    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs compileInputs { };
    enum class Recorder : bool {
        None,
        Twin,
    };
    auto compile = [&](CodeBlock* codeBlock, Recorder recorder) {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(codeBlock));
        JIT jit(vm, plan.get(), codeBlock);
        if (recorder == Recorder::Twin)
            jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, compileInputs);
        return jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
    };

    struct Reader {
        ASCIILiteral function;
        ASCIILiteral variable;
        ResolveType type;
    };
    for (auto& reader : { Reader { "readCaptured"_s, "captured"_s, ClosureVarWithVarInjectionChecks }, Reader { "readGlobalProperty"_s, "jitcacheGlobalProperty"_s, GlobalPropertyWithVarInjectionChecks } }) {
        auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, reader.function)));
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

// T9 (SPEC-image section 7, I13). counter and freshCounter have the same body text in one scope, so their UCBs share
// bytecode offsets and resolve every scope access alike, as an import's UCB has the producer's index spaces (R-UCB-1).
// A twin recorder compiles counter's CodeBlock, and the baked facts its record holds become the section. Each newborn
// CodeBlock of freshCounter, linked by newCodeBlockFor and never installed or run, must match that section unchanged and
// mismatch it once the test changes one fact: the executable's never-optimize flag set before linking, its source
// provider's taint set before linking, or one scope fact's metadata written after linking. No comparison may memoize
// the newborn's capability level.
JITCACHE_TEST(imageBakedFactsCompareWithNewbornCodeBlocks, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("var counter, freshCounter; (function makeCounters() { var count = 0; counter = function () { count = count + 1; return count; }; freshCounter = function () { count = count + 1; return count; }; })(); counter();"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the counters threw"_s);
        return;
    }
    auto* counter = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "counter"_s)));
    auto* freshCounter = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "freshCounter"_s)));
    CodeBlock* codeBlock = counter ? counter->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || !freshCounter || freshCounter->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("counter has no CodeBlock, or freshCounter is missing or was called"_s);
        return;
    }

    // The twin recorder is the one recorder a compilation outside production can have (T20). The counters have no switch
    // and no strict equality, so it answers nothing from its seeds and inputs but the assembler's seed, and both outlive it.
    RefPtr<BaselineJITCode> code;
    {
        TwinSeeds seeds { };
        seeds.assembler = assemblerSeed;
        TwinCompileInputs compileInputs { };
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(codeBlock));
        JIT jit(vm, plan.get(), codeBlock);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, compileInputs);
        code = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
    }
    auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
    if (!record || record->state() != RecordState::Complete) {
        JITCACHE_FAIL(makeString("counter's record is missing or not complete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
        return;
    }

    // The baked-facts section the record gives, and beside it the smallest image.baseline section that passes V1 to V6:
    // four zero bytes of code and no fixup, call, mold, table, code-map entry or MathIC.
    Vector<uint8_t> bakedFactsBytes;
    auto append = [&](std::span<const uint8_t> bytes) {
        bakedFactsBytes.append(bytes);
        return true;
    };
    ImageSectionSink sink = append;
    if (!writeBakedFactsSection(record->bakedFacts(), sink)) {
        JITCACHE_FAIL("writing the baked facts failed"_s);
        return;
    }
    ImageSectionHeader header;
    header.codeSize = 4;
    Vector<uint8_t> imageBytes(FillWith { }, static_cast<size_t>(ImageSectionSize(header).bytes()), 0);
    auto encodedHeader = encodeImageSectionHeader(header);
    memcpySpan(imageBytes.mutableSpan(), std::span<const uint8_t> { encodedHeader });

    // A twins build parses the twins section beside them (section 11.2): the record's twin data, with no producer value,
    // since the image holds no fixup.
    Vector<uint8_t> twinsBytes;
    auto appendTwins = [&](std::span<const uint8_t> bytes) {
        twinsBytes.append(bytes);
        return true;
    };
    ImageSectionSink twinsSink = appendTwins;
    if (!writeTwinsSection(record->twinData(), { }, captureProcessToken(), twinsSink)) {
        JITCACHE_FAIL("writing the twins section failed"_s);
        return;
    }

    auto facts = parseBakedFactsSection(bakedFactsBytes.span(), true);
    auto view = parseImageSections(ImageSectionSpans { .image = imageBytes.span(), .bakedFacts = bakedFactsBytes.span(), .twins = twinsBytes.span() }, true);
    if (!facts || !view) {
        JITCACHE_FAIL(makeString("the sections do not parse: "_s, description(!facts ? facts.error() : view.error())));
        return;
    }
    if (facts->capabilityLevel() == DFG::CannotCompile || facts->couldBeTainted()) {
        JITCACHE_FAIL("counter compiled as CannotCompile or tainted, so neither flag can make a newborn differ"_s);
        return;
    }
    auto hasFact = [&](ScopeOpcode opcode) {
        for (unsigned index = 0; index < facts->scopeFactCount(); ++index) {
            if (facts->scopeFact(index).opcode == opcode)
                return true;
        }
        return false;
    };
    JITCACHE_CHECK(hasFact(ScopeOpcode::ResolveScope));
    JITCACHE_CHECK(hasFact(ScopeOpcode::GetFromScope));
    JITCACHE_CHECK(hasFact(ScopeOpcode::PutToScope));

    // Runs change, then the comparison, on a newborn CB of freshCounter, used only inside the deferral scope that created
    // it. None is ever installed, so each is the executable's first CB for calls.
    FunctionExecutable* freshExecutable = freshCounter->jsExecutable();
    auto compareNewborn = [&](ASCIILiteral name, BakedFactsResult expected, const auto& change) {
        DeferGCForAWhile deferGC(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        CodeBlock* newborn = freshExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, freshCounter, freshCounter->scope());
        if (scope.exception()) {
            scope.clearException();
            JITCACHE_FAIL(makeString(name, ": newCodeBlockFor threw"_s));
            return;
        }
        if (!newborn || newborn->jitType() != JITType::None) {
            JITCACHE_FAIL(makeString(name, ": newCodeBlockFor gave no newborn CB"_s));
            return;
        }
        if (newborn->capabilityLevelState() != DFG::CapabilityLevelNotSet) {
            JITCACHE_FAIL(makeString(name, ": linking memoized the capability level"_s));
            return;
        }
        change(*newborn);
        if (compareBakedFacts(*view, *newborn) != expected)
            JITCACHE_FAIL(makeString(name, ": compareBakedFacts gives the wrong result"_s));
        if (newborn->capabilityLevelState() != DFG::CapabilityLevelNotSet)
            JITCACHE_FAIL(makeString(name, ": the comparison memoized the capability level"_s));
    };
    auto unchanged = [](CodeBlock&) { };

    compareNewborn("an unchanged newborn"_s, BakedFactsResult::Match, unchanged);

    // computeCapabilityLevel reads the flag, which makes every CB of the executable CannotCompile.
    freshExecutable->setNeverOptimize(true);
    compareNewborn("a never-optimize executable"_s, BakedFactsResult::Mismatch, unchanged);
    freshExecutable->setNeverOptimize(false);

    // The CodeBlock constructor takes couldBeTainted from the provider.
    SourceProvider* provider = freshExecutable->source().provider();
    SourceTaintedOrigin originalTaint = provider->sourceTaintedOrigin();
    provider->setSourceTaintedOrigin(SourceTaintedOrigin::KnownTainted);
    compareNewborn("a tainted provider"_s, BakedFactsResult::Mismatch, unchanged);
    provider->setSourceTaintedOrigin(originalTaint);

    // Each scope fact's resolve type becomes GlobalVar, which no baked fact has. The collector reads a resolve_scope
    // entry's cell union whatever its type, and skips the structure-or-watchpoint union of a GlobalVar get_from_scope or
    // put_to_scope entry (CodeBlock::reconcileLLIntInlineCachesAtGCEnd), so the newborn stays safe to collect.
    for (unsigned index = 0; index < facts->scopeFactCount(); ++index) {
        ScopeFact fact = facts->scopeFact(index);
        compareNewborn("a changed scope type"_s, BakedFactsResult::Mismatch, [&](CodeBlock& newborn) {
            auto instruction = newborn.instructions().at(BytecodeIndex(fact.bytecodeOffset));
            switch (fact.opcode) {
            case ScopeOpcode::ResolveScope:
                instruction->as<OpResolveScope>().metadata(&newborn).m_resolveType = GlobalVar;
                return;
            case ScopeOpcode::GetFromScope: {
                auto& metadata = instruction->as<OpGetFromScope>().metadata(&newborn);
                metadata.m_getPutInfo = GetPutInfo(metadata.m_getPutInfo.resolveMode(), GlobalVar, metadata.m_getPutInfo.initializationMode(), metadata.m_getPutInfo.ecmaMode());
                return;
            }
            case ScopeOpcode::PutToScope: {
                auto& metadata = instruction->as<OpPutToScope>().metadata(&newborn);
                metadata.m_getPutInfo = GetPutInfo(metadata.m_getPutInfo.resolveMode(), GlobalVar, metadata.m_getPutInfo.initializationMode(), metadata.m_getPutInfo.ecmaMode());
                return;
            }
            }
        });
        // A resolve_scope of type ClosureVar also bakes its depth.
        if (fact.opcode == ScopeOpcode::ResolveScope && fact.resolveType == ClosureVar) {
            compareNewborn("a changed scope depth"_s, BakedFactsResult::Mismatch, [&](CodeBlock& newborn) {
                newborn.instructions().at(BytecodeIndex(fact.bytecodeOffset))->as<OpResolveScope>().metadata(&newborn).m_localScopeDepth = fact.localScopeDepth + 1;
            });
        }
    }
}

namespace ImageRecordingTestsInternal {

// One section of a capture, in a buffer of its own, which starts 8-byte aligned as the container hands sections over.
template<typename Write>
static bool collectSection(Vector<uint8_t>& bytes, const Write& write)
{
    auto append = [&](std::span<const uint8_t> data) {
        bytes.append(data);
        return true;
    };
    ImageSectionSink sink = append;
    return write(sink);
}

// T21's import and check, in a frame of their own so that the test's frame never holds the twin. producerBody's
// CodeBlock compiles under a twin recorder, the one recorder a compilation outside production can have (T20), is
// installed and is captured with strict on. importBody, the same text in the same scope, so that its UCB has the
// producer's index spaces (R-UCB-1) and its scope accesses resolve alike, gets a newborn CodeBlock, which takes the
// prepared image as the install glue gives it, through native setup and installCode (SPEC-integrator.md section 7.2),
// before Twins::checkImage checks it.
static NEVER_INLINE bool imageImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& producerFunction, JSFunction& importFunction)
{
    CodeBlock* producer = producerFunction.jsExecutable()->codeBlockForCall();
    if (!producer || JITCode::isJIT(producer->jitType())) {
        JITCACHE_FAIL("producerBody has no CodeBlock in the LLInt"_s);
        return false;
    }

    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs noInputs { };
    RefPtr<BaselineJITCode> producerCode;
    {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(producer));
        JIT jit(vm, plan.get(), producer);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, noInputs);
        producerCode = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
        if (!producerCode || JIT::finalizeOnMainThread(producer, plan.get(), producerCode) != CompilationResult::CompilationSuccessful) {
            JITCACHE_FAIL("the producer's compilation failed"_s);
            return false;
        }
    }
    producer->ownerExecutable()->installCode(producer);

    Vector<uint8_t> imageBytes;
    Vector<uint8_t> bakedFactsBytes;
    Vector<uint8_t> twinsBytes;
    {
        Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
        auto capture = captureImage(vm, *producer, *producerCode, budget.get(), true);
        if (!capture) {
            JITCACHE_FAIL(makeString("the capture failed with outcome "_s, static_cast<unsigned>(capture.error().outcome), " at "_s, description(capture.error().check)));
            return false;
        }
        bool written = collectSection(imageBytes, [&](const ImageSectionSink& sink) { return capture->writeImageSection(sink); })
            && collectSection(bakedFactsBytes, [&](const ImageSectionSink& sink) { return capture->writeBakedFactsSection(sink); })
            && collectSection(twinsBytes, [&](const ImageSectionSink& sink) { return capture->writeTwinsSection(sink); });
        if (!written) {
            JITCACHE_FAIL("writing the captured sections failed"_s);
            return false;
        }
        JITCACHE_CHECK(imageBytes.size() == capture->imageSectionSize());
        JITCACHE_CHECK(bakedFactsBytes.size() == capture->bakedFactsSectionSize());
        JITCACHE_CHECK(twinsBytes.size() == capture->twinsSectionSize());
    }

    // The install function's deferral keeps the newborn alive until installCode publishes it.
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionExecutable* importExecutable = importFunction.jsExecutable();
    CodeBlock* newborn = importExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, &importFunction, importFunction.scope());
    if (scope.exception() || !newborn) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor gave importBody no newborn CodeBlock"_s);
        return false;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *newborn->unlinkedCodeBlock();

    auto view = parseImageSections(ImageSectionSpans { .image = imageBytes.span(), .bakedFacts = bakedFactsBytes.span(), .twins = twinsBytes.span() }, true);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(view.error())));
        return false;
    }
    if (auto valid = validateImageSectionsAgainst(*view, unlinkedCodeBlock); !valid) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(valid.error()), " against importBody's UCB"_s));
        return false;
    }
    if (compareBakedFacts(*view, *newborn) != BakedFactsResult::Match) {
        JITCACHE_FAIL("the newborn's baked facts differ from the capture's"_s);
        return false;
    }
    auto prepared = prepareImage(vm, unlinkedCodeBlock, *view, nullptr, true);
    if (!prepared) {
        JITCACHE_FAIL(makeString("preparing the image failed at "_s, description(prepared.error().check)));
        return false;
    }
    Ref<BaselineJITCode> restored = WTF::move(*prepared).commit(vm, *newborn);
    newborn->setupWithUnlinkedBaselineCode(restored.copyRef());
    importExecutable->installCode(newborn);

    auto report = TwinReport::open("/dev/null"_s);
    if (!report) {
        JITCACHE_FAIL("the twin report cannot be opened"_s);
        return false;
    }
    Twins twins;
    twins.checkImage(vm, *newborn, importFunction.scope(), restored.get(), *view, *report);
    // The image is its own twin's: both preconditions hold, so nothing is skipped, and nothing differs.
    if (report->differences() || report->skips())
        JITCACHE_FAIL(makeString("the twin check reported "_s, report->differences(), " differences and "_s, report->skips(), " skips"_s));
    // The check returned, dropping its hold, so nothing reaches the twin, but nothing has swept it yet.
    JITCACHE_CHECK(imageTwinCountForTesting() == 1);
    return true;
}

// A CodeBlock of importBody's UCB linked natively, never installed and left unreachable as the twin is.
static NEVER_INLINE bool imageLinkUnreachableCodeBlock(TestContext& context, VM& vm, JSFunction& importFunction)
{
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionExecutable* executable = importFunction.jsExecutable();
    auto* unlinkedCodeBlock = uncheckedDowncast<UnlinkedFunctionCodeBlock>(executable->codeBlockForCall()->unlinkedCodeBlock());
    auto* codeBlock = FunctionCodeBlock::create(vm, executable, unlinkedCodeBlock, importFunction.scope());
    if (scope.exception() || !codeBlock) {
        scope.clearException();
        JITCACHE_FAIL("linking a native CodeBlock of importBody failed"_s);
        return false;
    }
    return true;
}

// Conservative scanning reads the frames a collection runs in, which reuse the stack the frames above left behind;
// zeroing it first keeps a stale pointer there from holding a CodeBlock the test expects to die.
static NEVER_INLINE void imageClearDeadStack()
{
    std::array<volatile uint8_t, 128 * KB> bytes;
    for (auto& byte : bytes)
        byte = 0;
}

} // namespace ImageRecordingTestsInternal

// T21 (SPEC-image.md section 11.3, step 2; N18). The image check's twin CodeBlock is registered as long as it lives,
// dies in the first full collection after the check, since nothing reaches it, and writes no didOptimize as it dies,
// while a CodeBlock of the same body linked natively and left unreachable the same way writes False. A dying CodeBlock
// writes didOptimize only through its metadata table, and UnlinkedMetadataTable::link() gives none to a body with no
// metadata entry or value profile, such as `return a + b`. The bodies read a captured variable, whose resolve_scope and
// get_from_scope give them a table, so that a write by the twin would show.
JITCACHE_TEST_WITH_OPTIONS(imageTwinDiesWithoutWritingDidOptimize, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("var producerBody, importBody; (function makeBodies() { var captured = 3; producerBody = function (a, b) { return a + b + captured; }; importBody = function (a, b) { return a + b + captured; }; })(); producerBody(1, 2);"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the bodies threw"_s);
        return;
    }
    auto* producerFunction = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "producerBody"_s)));
    auto* importFunction = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "importBody"_s)));
    if (!producerFunction || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("a body is missing, or importBody was called"_s);
        return;
    }
    JITCACHE_CHECK(!imageTwinCountForTesting());

    if (!imageImportAndCheckTwin(context, vm, *producerFunction, *importFunction))
        return;
    UnlinkedCodeBlock* unlinkedCodeBlock = importFunction->jsExecutable()->codeBlockForCall()->unlinkedCodeBlock();
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::Indeterminate);

    // The collection sweeps synchronously, so the twin dies in it and leaves the registry without writing didOptimize.
    imageClearDeadStack();
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    JITCACHE_CHECK(!imageTwinCountForTesting());
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::Indeterminate);

    // The native CodeBlock's death writes False, so the check above would have seen a write by the twin.
    if (!imageLinkUnreachableCodeBlock(context, vm, *importFunction))
        return;
    imageClearDeadStack();
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::False);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
