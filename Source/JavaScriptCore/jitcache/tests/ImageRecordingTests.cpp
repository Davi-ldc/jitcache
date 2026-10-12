#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArithProfile.h"
#include "BakedFacts.h"
#include "BaselineJITPlan.h"
#include "BaselineJITRegisters.h"
#include "BinarySwitch.h"
#include "BuiltinExecutableCreator.h"
#include "BytecodeStructs.h"
#include "CCallHelpers.h"
#include "CallLinkInfo.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "DisallowMacroScratchRegisterUsage.h"
#include "ExecutableAllocator.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "ImageCapture.h"
#include "ImageEmission.h"
#include "ImagePrepare.h"
#include "ImageRecord.h"
#include "ImageRecorder.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "ImageTwins.h"
#include "JIT.h"
#include "JITCacheAPI.h"
#include "JITCacheTest.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "LLIntEntrypoint.h"
#include "LinkBuffer.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "TopExceptionScope.h"
#include "TwinReport.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include <algorithm>
#include <array>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/DataLog.h>
#include <wtf/FileSystem.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

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
#if CPU(ARM64)
        // ARM64's AssemblerBuffer declares no data(). releaseAssemblerData() returns an rvalue reference to the
        // buffer's storage, and a const reference bound to it moves nothing, so the buffer keeps its bytes.
        const AssemblerData& storage = buffer.releaseAssemblerData();
        return unsafeMakeSpan(reinterpret_cast<const uint8_t*>(storage.buffer()), buffer.codeSize());
#else
        return unsafeMakeSpan(static_cast<const uint8_t*>(buffer.data()), buffer.codeSize());
#endif
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

// A capture's three sections, each in a buffer of its own, beside the code it captured.
struct CapturedImage {
    RefPtr<BaselineJITCode> code;
    Vector<uint8_t> image;
    Vector<uint8_t> bakedFacts;
    Vector<uint8_t> twins;
};

// Captures captured.code, the baseline code codeBlock runs, with strict on, which runs S1 to S4, into captured's three
// sections.
static bool imageCaptureInto(TestContext& context, VM& vm, CodeBlock& codeBlock, CapturedImage& captured)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto capture = captureImage(vm, codeBlock, *captured.code, budget.get(), true);
    if (!capture) {
        JITCACHE_FAIL(makeString("the capture failed with outcome "_s, static_cast<unsigned>(capture.error().outcome), " at "_s, description(capture.error().check)));
        return false;
    }
    bool written = collectSection(captured.image, [&](const ImageSectionSink& sink) { return capture->writeImageSection(sink); })
        && collectSection(captured.bakedFacts, [&](const ImageSectionSink& sink) { return capture->writeBakedFactsSection(sink); })
        && collectSection(captured.twins, [&](const ImageSectionSink& sink) { return capture->writeTwinsSection(sink); });
    if (!written) {
        JITCACHE_FAIL("writing the captured sections failed"_s);
        return false;
    }
    JITCACHE_CHECK(captured.image.size() == capture->imageSectionSize());
    JITCACHE_CHECK(captured.bakedFacts.size() == capture->bakedFactsSectionSize());
    JITCACHE_CHECK(captured.twins.size() == capture->twinsSectionSize());
    return true;
}

// producerFunction's CodeBlock, still in the LLInt, compiles under a twin recorder, the one recorder a compilation outside
// production can have (T20), is installed and is captured with strict on, which runs S1 to S4.
static NEVER_INLINE std::optional<CapturedImage> imageCompileAndCapture(TestContext& context, VM& vm, JSFunction& producerFunction)
{
    CodeBlock* producer = producerFunction.jsExecutable()->codeBlockForCall();
    if (!producer || JITCode::isJIT(producer->jitType())) {
        JITCACHE_FAIL("the producing function has no CodeBlock in the LLInt"_s);
        return std::nullopt;
    }

    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs noInputs { };
    CapturedImage captured;
    {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(producer));
        JIT jit(vm, plan.get(), producer);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, noInputs);
        captured.code = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
        if (!captured.code || JIT::finalizeOnMainThread(producer, plan.get(), captured.code) != CompilationResult::CompilationSuccessful) {
            JITCACHE_FAIL("the producer's compilation failed"_s);
            return std::nullopt;
        }
    }
    producer->ownerExecutable()->installCode(producer);

    if (!imageCaptureInto(context, vm, *producer, captured))
        return std::nullopt;
    return captured;
}

// importFunction, the producer's text in the producer's scope, so that its UCB has the producer's index spaces (R-UCB-1)
// and its scope accesses resolve alike, gets a newborn CodeBlock. The newborn takes the image prepareImage restores from
// the captured sections with strict on, which runs S5, as the install glue gives it, through native setup and installCode
// (SPEC-integrator.md section 7.2), and Twins::checkImage then checks it. inspect sees the restored code and the parsed
// sections before setup, and aroundCheck receives a function that runs the check, which it calls once. Returns whether
// the import installed.
template<typename Inspect, typename AroundCheck>
static NEVER_INLINE bool imageImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& importFunction, const CapturedImage& captured, const Inspect& inspect, const AroundCheck& aroundCheck)
{
    // The install function's deferral keeps the newborn alive until installCode publishes it.
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionExecutable* importExecutable = importFunction.jsExecutable();
    CodeBlock* newborn = importExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, &importFunction, importFunction.scope());
    if (scope.exception() || !newborn) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor gave the importing function no newborn CodeBlock"_s);
        return false;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *newborn->unlinkedCodeBlock();

    auto view = parseImageSections(ImageSectionSpans { .image = captured.image.span(), .bakedFacts = captured.bakedFacts.span(), .twins = captured.twins.span() }, true);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(view.error())));
        return false;
    }
    if (auto valid = validateImageSectionsAgainst(*view, unlinkedCodeBlock); !valid) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(valid.error()), " against the importing function's UCB"_s));
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
    inspect(restored.get(), *view);
    newborn->setupWithUnlinkedBaselineCode(restored.copyRef());
    importExecutable->installCode(newborn);

    auto report = TwinReport::open("/dev/null"_s);
    if (!report) {
        JITCACHE_FAIL("the twin report cannot be opened"_s);
        return false;
    }
    size_t twinsBefore = imageTwinCountForTesting();
    Twins twins;
    aroundCheck([&] {
        twins.checkImage(vm, *newborn, importFunction.scope(), restored.get(), *view, *report);
    });
    // The image is its own twin's: both preconditions hold, so nothing is skipped, and nothing differs.
    if (report->differences() || report->skips())
        JITCACHE_FAIL(makeString("the twin check reported "_s, report->differences(), " differences and "_s, report->skips(), " skips"_s));
    // The check returned, dropping its hold, so nothing reaches the twin, but nothing has swept it yet.
    JITCACHE_CHECK(imageTwinCountForTesting() == twinsBefore + 1);
    return true;
}

template<typename Inspect>
static bool imageImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& importFunction, const CapturedImage& captured, const Inspect& inspect)
{
    return imageImportAndCheckTwin(context, vm, importFunction, captured, inspect, [](const auto& check) {
        check();
    });
}

// T21's capture, import and check, in a frame of their own so that the test's frame never holds the twin.
static NEVER_INLINE bool imageCaptureImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& producerFunction, JSFunction& importFunction)
{
    auto captured = imageCompileAndCapture(context, vm, producerFunction);
    return captured && imageImportAndCheckTwin(context, vm, importFunction, *captured, [](BaselineJITCode&, const ImageSectionsView&) { });
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

    if (!imageCaptureImportAndCheckTwin(context, vm, *producerFunction, *importFunction))
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

namespace ImageRecordingTestsInternal {

// A compilation of codeBlock under a twin recorder, the one recorder a compilation outside production can have (T20),
// seeded for the assembler alone and charging budget. Null when no executable memory was left.
static RefPtr<BaselineJITCode> compileUnderTwinRecorder(VM& vm, CodeBlock& codeBlock, Ref<ProducerBudget>&& budget)
{
    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs compileInputs { };
    Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(&codeBlock));
    JIT jit(vm, plan.get(), &codeBlock);
    jit.setJITCacheTwin(WTF::move(budget), seeds, compileInputs);
    return jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
}

static RefPtr<BaselineJITCode> compileUnderTwinRecorder(VM& vm, CodeBlock& codeBlock)
{
    return compileUnderTwinRecorder(vm, codeBlock, ProducerBudget::createUnlimited());
}

static JSFunction* globalFunction(JSGlobalObject& globalObject, ASCIILiteral name)
{
    VM& vm = globalObject.vm();
    return dynamicDowncast<JSFunction>(globalObject.get(&globalObject, Identifier::fromString(vm, name)));
}

// A VM of the test's own, with its API lock held, destroyed under that lock when the object goes, as testjitcache destroys
// a test's VM, so that VM::~VM runs JITCache's teardown hooks while the test still runs.
class OwnedVM {
    WTF_MAKE_NONCOPYABLE(OwnedVM);
public:
    OwnedVM()
        : m_vm(VM::create(HeapType::Large).leakRef())
        , m_locker(m_vm)
    {
        // The holder's reference is the last, which ~JSLockHolder drops before unlocking.
        m_vm.derefSuppressingSaferCPPChecking();
    }

    VM& vm() { return m_vm; }

private:
    VM& m_vm;
    JSLockHolder m_locker;
};

// A fresh directory for an artifact, removed with everything in it when the object goes. Its path is empty when mkdtemp
// failed, and error() says why.
class TemporaryArtifactDirectory {
    WTF_MAKE_NONCOPYABLE(TemporaryArtifactDirectory);
public:
    TemporaryArtifactDirectory()
    {
        const char* base = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(base && *base ? base : "/tmp"), "/jitcache-image-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (mkdtemp(path.mutableSpan().data()))
            m_path = String::fromUTF8(path.span().data());
        else
            m_error = errno;
    }

    ~TemporaryArtifactDirectory()
    {
        if (!m_path.isEmpty())
            FileSystem::deleteNonEmptyDirectory(m_path);
    }

    const String& path() const { return m_path; }
    int error() const { return m_error; }

private:
    String m_path;
    int m_error { 0 };
};

// A program whose function jitcacheBig needs many of kRecordChargeStep's steps of record: 160 property loads, each feeding
// a mul and an add MathIC, with their slow calls, profile writes and exception checks. The program's own loops stay in the
// LLInt, so jitcacheBig is the first body that compiles.
static String bigBodySource()
{
    StringBuilder source;
    source.append("function jitcacheBig(o) {\n    var sum = 0;\n"_s);
    for (unsigned index = 0; index < 160; ++index)
        source.append("    sum = sum + o.p"_s, index, " * "_s, index + 1, ";\n"_s);
    source.append("    return sum;\n}\n"_s);
    source.append("var jitcacheBigObject = {};\nfor (var k = 0; k < 160; ++k)\n    jitcacheBigObject['p' + k] = k;\n"_s);
    source.append("for (var i = 0; i < 60; ++i)\n    jitcacheBig(jitcacheBigObject);\n"_s);
    return source.toString();
}

} // namespace ImageRecordingTestsInternal

// T11 (SPEC-image.md section 4.1). A body whose emission meets a reason of section 4.1 compiles under a recorder into code
// whose record is Unrecordable for that reason, which isImageCapturable refuses, so no capture ever reads it. A body
// generated after VM::enableControlFlowProfiler, as Bun's coverage turns the profiler on, holds op_profile_control_flow,
// whose emitter clears m_isShareable (N29, census C24). A builtin-mode function made with createBuiltinExecutable, as
// $vm.createBuiltin makes one, is where @superSamplerBegin and @superSamplerEnd lex (census C20). The two guards of section
// 4.4, triggered through their hooks, are imageGuardsMarkUnannotatedLinks's and imageSharedHelpersMarkUnannotatedReferences's.
JITCACHE_TEST(imageUnrecordableBodiesAreNeverCaptured, Yes)
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

    // The builtin's body is generated at its first call, before the profiler changes the code-generation mode of the bodies
    // generated after it (JSGlobalObject::defaultCodeGenerationMode), so the super-sampler opcodes are the one reason its
    // emission meets.
    SourceCode builtinSource = makeSource("(function () { @superSamplerBegin(); var sampled = 1; @superSamplerEnd(); return sampled; })"_s, SourceOrigin(), SourceTaintedOrigin::Untainted);
    UnlinkedFunctionExecutable* builtin = createBuiltinExecutable(vm, builtinSource, Identifier::fromString(vm, "jitcacheSampled"_s), ImplementationVisibility::Public, ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None);
    globalObject->putDirect(vm, Identifier::fromString(vm, "jitcacheSampled"_s), JSFunction::create(vm, globalObject, builtin->link(vm, nullptr, builtinSource), globalObject));
    if (!run("jitcacheSampled();"_s))
        return;
    vm.enableControlFlowProfiler();
    if (!run("function jitcacheCovered(a) { if (a) return 1; return 2; } jitcacheCovered(1);"_s))
        return;

    struct Case {
        ASCIILiteral function;
        Unrecordable reason;
    };
    for (auto& unrecordable : { Case { "jitcacheSampled"_s, Unrecordable::SuperSamplerOpcode }, Case { "jitcacheCovered"_s, Unrecordable::NotShareable } }) {
        auto* function = globalFunction(*globalObject, unrecordable.function);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock) {
            JITCACHE_FAIL(makeString(unrecordable.function, " has no CodeBlock"_s));
            continue;
        }
        auto code = compileUnderTwinRecorder(vm, *codeBlock);
        if (!code) {
            JITCACHE_FAIL("no executable memory for the recorded compilation"_s);
            return;
        }
        auto* record = code->m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Unrecordable || record->unrecordableReason() != unrecordable.reason)
            JITCACHE_FAIL(makeString(unrecordable.function, "'s record is missing or not Unrecordable for its reason (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
        JITCACHE_CHECK(!isImageCapturable(*code));
        if (unrecordable.reason == Unrecordable::NotShareable)
            JITCACHE_CHECK(!code->m_isShareable);
    }
}

// T12 (SPEC-image.md sections 4.8 and 13, I14), the part a script cannot see. In a Producer whose limit admits two of
// kRecordChargeStep's steps, jitcacheBig records until a later step is refused: its code links and runs, its record is
// Incomplete, which isImageCapturable refuses, the finalize capture raises the recording fault at budget.limit and
// commits nothing, and once the VM is destroyed, with its records and its production memory, the budget holds no charge.
// LeakSanitizer checks the rest when testjitcache exits; image/budget.js runs the limits as a producer would.
JITCACHE_TEST_WITH_OPTIONS(imageTinyBudgetLeavesRecordsIncompleteAndReleasesEverything, No, "--useConcurrentJIT=false")
{
    TemporaryArtifactDirectory directory;
    if (directory.path().isEmpty()) {
        JITCACHE_FAIL(makeString("mkdtemp failed: "_s, String::fromUTF8(safeStrerror(directory.error()).data())));
        return;
    }
    RefPtr<ProducerBudget> budget;
    {
        OwnedVM producer;
        VM& vm = producer.vm();
        JITCache::Config config;
        config.artifactPath = makeString(directory.path(), "/artifact"_s);
        config.role = Role::Producer;
        config.producerLimitBytes = 2 * kRecordChargeStep;
        config.strict = true;
        StartResult started = JITCache::start(vm, config);
        if (started.outcome != StartOutcome::Started) {
            JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
            return;
        }
        ProducerContext* production = JITCache::producerContext(vm);
        if (!production) {
            JITCACHE_FAIL("the started producer has no producer context"_s);
            return;
        }
        budget = production->budget();

        auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
        NakedPtr<Exception> exception;
        evaluate(globalObject, makeSource(bigBodySource(), SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception) {
            JITCACHE_FAIL("evaluating the big body threw"_s);
            return;
        }
        auto* function = globalFunction(*globalObject, "jitcacheBig"_s);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
            JITCACHE_FAIL("jitcacheBig did not compile to baseline code"_s);
            return;
        }
        RefPtr<JSC::JITCode> jitCode = codeBlock->jitCode();
        auto& code = static_cast<BaselineJITCode&>(*jitCode);
        auto* record = code.m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Incomplete)
            JITCACHE_FAIL(makeString("jitcacheBig's record is missing or not Incomplete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ')'));
        JITCACHE_CHECK(!isImageCapturable(code));
        JITCACHE_CHECK(budget->hasRefused());

        Status reported = JITCache::status(vm);
        JITCACHE_CHECK(reported.production == ProductionState::Ended);
        JITCACHE_CHECK(reported.productionFault && reported.productionFault->stepName() == "budget.limit"_s);
        JITCACHE_CHECK(!reported.progress.capturesCommitted);
        JITCACHE_CHECK(!reported.progress.bytesCommitted);
    }
    // The VM is gone: every record and all production memory gave back what they charged (I14).
    JITCACHE_CHECK(budget && !budget->chargedBytes());
}

namespace ImageRecordingTestsInternal {

// The body of T12's sweep. Each of its 160 statements loads a property, multiplies and adds it, whose MathICs' slow calls
// end with exception checks in the slow paths, and concatenates a constant string, a site with no inline code (N17), so
// its operation call and exception check sit in the main pass. jitcacheSweep runs once, in the LLInt, which fills the
// profiles those shapes follow. It holds no switch and no strict equality, so a twin recorder answers nothing from its
// seeds and inputs but the assembler's seed.
static String sweepBodySource()
{
    constexpr unsigned statements = 160;
    StringBuilder source;
    source.append("function jitcacheSweep(o, s) {\n    var sum = 0;\n    var text = \"\";\n"_s);
    for (unsigned index = 0; index < statements; ++index)
        source.append("    sum = sum + o.p"_s, index, " * "_s, index + 1, ";\n    text = \"k\" + s;\n"_s);
    source.append("    return sum + text.length;\n}\n"_s);
    source.append("var jitcacheSweepObject = {};\nfor (var k = 0; k < "_s, statements, "; ++k)\n    jitcacheSweepObject['p' + k] = k;\njitcacheSweep(jitcacheSweepObject, 's');\n"_s);
    return source.toString();
}

#if CPU(ARM64)
// An exception check's jump to HandleException. Deferred while the recorder records, it reaches the thunk through a
// veneer, the b emitVeneers placed at the image's tail; deferred after the recorder stopped, it is linked natively, by
// its own branch or, when compaction kept the inverted form, by the b right after it.
struct ExceptionCheckJump {
    uint32_t offset;
    bool throughVeneer;
};

// The conditional branches that reach handleException in [start, arityEntry), the image's prologue, main pass and slow
// paths, which the passes emitted, and so deferred, in code order. allocationEnd bounds the image's allocation. Nullopt
// when a conditional branch there targets itself, the form the assembler emits for one that nothing linked.
static std::optional<Vector<ExceptionCheckJump>> exceptionCheckJumps(uintptr_t start, uintptr_t arityEntry, uintptr_t allocationEnd, uintptr_t handleException)
{
    auto isConditional = [](uint32_t word) {
        return (word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000 || (word & 0x7e000000) == 0x36000000;
    };
    Vector<ExceptionCheckJump> jumps;
    for (uintptr_t address = start; address < arityEntry; address += 4) {
        if (!isConditional(readWord(address)))
            continue;
        uintptr_t target = *arm64BranchTarget(address);
        if (target == address)
            return std::nullopt;
        // The inverted form branches over the b that takes the jump.
        if (target == address + 8 && (readWord(address + 4) & 0xfc000000) == 0x14000000)
            target = *arm64BranchTarget(address + 4);
        if (!reaches(target, handleException))
            continue;
        bool throughVeneer = target >= arityEntry && target < allocationEnd;
        jumps.append(ExceptionCheckJump { static_cast<uint32_t>(address - start), throughVeneer });
    }
    return jumps;
}
#endif

} // namespace ImageRecordingTestsInternal

// T12 (SPEC-image.md sections 4.5 and 4.8, I14, I19), the main pass and the slow paths. jitcacheSweep compiles under a
// twin recorder whose budget admits one more kRecordChargeStep each time, until its record is complete. Every compilation
// whose budget refused a charge leaves an Incomplete record, which isImageCapturable refuses, and once its code dies the
// budget holds nothing (I14). On ARM64 the exception checks show where the refusal fell: those deferred before it reach
// HandleException through veneers and those deferred after it natively, so in code order no veneered one follows a native
// one, no conditional branch is left unlinked, and each image keeps as many as the complete compilation. The sweep must
// refuse inside the main pass at least once, between a veneered and a native jump before the last instruction, and
// inside the slow paths at least once, between a veneered and a native jump after it. x86_64 links every
// conditional external jump natively as it records it, so there only the records and the balances are checked. A refusal
// inside emitVeneers is imageRefusalInsideEmitVeneersLinksEveryVeneer's, and one inside a MathIC snippet
// imageRefusedRegenerationLinksItsSnippet's.
JITCACHE_TEST(imageBudgetSweepLinksEveryExceptionCheck, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(sweepBodySource(), SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the sweep's body threw"_s);
        return;
    }
    auto* function = globalFunction(*globalObject, "jitcacheSweep"_s);
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || JITCode::isJIT(codeBlock->jitType())) {
        JITCACHE_FAIL("jitcacheSweep has no CodeBlock in the LLInt"_s);
        return;
    }

#if CPU(ARM64)
    auto handleException = reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr());
    // An image's exception checks, and the offset of its last instruction, the end of the main pass.
    struct Scan {
        Vector<ExceptionCheckJump> jumps;
        uint32_t lastInstruction { 0 };
    };
    auto scan = [&](BaselineJITCode& code) -> std::optional<Scan> {
        auto start = reinterpret_cast<uintptr_t>(code.start());
        auto arityEntry = reinterpret_cast<uintptr_t>(code.addressForCall(ArityCheckMode::MustCheckArity).untaggedPtr());
        auto jumps = exceptionCheckJumps(start, arityEntry, start + code.size(), handleException);
        if (!jumps) {
            JITCACHE_FAIL("a conditional branch of jitcacheSweep's image targets itself"_s);
            return std::nullopt;
        }
        Scan result { WTF::move(*jumps), 0 };
        code.m_jitCodeMap.forEach([&](BytecodeIndex, CodeLocationLabel<JSEntryPtrTag> location) {
            result.lastInstruction = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(location.untaggedPtr()) - start);
        });
        return result;
    };

    // The complete compilation defers every exception check while it records.
    size_t exceptionChecks = 0;
    {
        auto code = compileUnderTwinRecorder(vm, *codeBlock);
        auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("the unlimited compilation of jitcacheSweep left no complete record"_s);
            return;
        }
        auto complete = scan(*code);
        if (!complete)
            return;
        for (auto& jump : complete->jumps) {
            if (!jump.throughVeneer) {
                JITCACHE_FAIL(makeString("the complete compilation links the exception check at "_s, jump.offset, " natively"_s));
                return;
            }
        }
        exceptionChecks = complete->jumps.size();
        if (!exceptionChecks) {
            JITCACHE_FAIL("jitcacheSweep's image holds no exception check"_s);
            return;
        }
    }
    unsigned mainPassRefusals = 0;
    unsigned slowPathRefusals = 0;
#endif

    bool completed = false;
    constexpr unsigned maximumSteps = 512;
    for (unsigned steps = 1; steps <= maximumSteps && !completed; ++steps) {
        auto budget = ProducerBudget::create(steps * kRecordChargeStep);
        {
            auto code = compileUnderTwinRecorder(vm, *codeBlock, budget.copyRef());
            if (!code) {
                JITCACHE_FAIL("no executable memory for the sweep's compilation"_s);
                return;
            }
            auto* record = code->m_jitCacheImageRecord.get();
            if (!record || (record->state() != RecordState::Complete && record->state() != RecordState::Incomplete)) {
                JITCACHE_FAIL(makeString("at "_s, steps, " steps, the record is missing or neither complete nor incomplete"_s));
                return;
            }
            completed = record->state() == RecordState::Complete;
            if (!completed)
                JITCACHE_CHECK(!isImageCapturable(*code));
#if CPU(ARM64)
            auto result = scan(*code);
            if (!result)
                return;
            auto& jumps = result->jumps;
            if (jumps.size() != exceptionChecks) {
                JITCACHE_FAIL(makeString("at "_s, steps, " steps, the image holds "_s, jumps.size(), " exception checks instead of "_s, exceptionChecks));
                return;
            }
            size_t firstNative = jumps.size();
            for (size_t index = 0; index < jumps.size(); ++index) {
                if (!jumps[index].throughVeneer) {
                    firstNative = index;
                    break;
                }
            }
            for (size_t index = firstNative; index < jumps.size(); ++index) {
                if (jumps[index].throughVeneer) {
                    JITCACHE_FAIL(makeString("at "_s, steps, " steps, the exception check at "_s, jumps[index].offset, " reaches a veneer after a native one"_s));
                    return;
                }
            }
            if (completed && firstNative < jumps.size())
                JITCACHE_FAIL(makeString("the complete compilation at "_s, steps, " steps links an exception check natively"_s));
            // A refusal between two exception checks of one pass: a veneered one, then a native one.
            if (firstNative && firstNative < jumps.size()) {
                if (jumps[firstNative].offset < result->lastInstruction)
                    ++mainPassRefusals;
                else if (jumps[firstNative - 1].offset > result->lastInstruction)
                    ++slowPathRefusals;
            }
#endif
        }
        // The code is gone, and its record with it.
        JITCACHE_CHECK(!budget->chargedBytes());
    }
    JITCACHE_CHECK(completed);
#if CPU(ARM64)
    if (!mainPassRefusals || !slowPathRefusals)
        JITCACHE_FAIL(makeString("the sweep refused "_s, mainPassRefusals, " times inside the main pass and "_s, slowPathRefusals, " times inside the slow paths"_s));
#endif
}

#if CPU(ARM64)
// T12, the refusal inside emitVeneers (SPEC-image.md sections 4.5 and 4.8, I19). emitVeneers charges only to record a
// veneer's fixup, and only when the fixup vector is full, so a real compilation refuses there only by chance. Here each
// try's recorder, under a budget of one kRecordChargeStep, defers a conditional jump to each of two thunks, records a
// number of Pointer fixups and emits the veneers. emitVeneers emits and links each veneer before recordJump charges for
// its fixup. So at the first count whose refusal falls inside emitVeneers, the first veneer's fixup takes the vector's
// last free entry, and the refused charge is the last veneer's, once both veneers exist. At the next count the vector is
// full as emitVeneers begins: the first veneer's charge is refused, the record never holds a veneer fixup, and the second
// veneer is emitted and linked by a recorder that has stopped. One count more is refused before the veneers, which shows
// that the vector was full there. At both counts inside, each jump must reach its thunk through a veneer of its own.
JITCACHE_TEST(imageRefusalInsideEmitVeneersLinksEveryVeneer, Yes)
{
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    std::array keys { commonThunkKey(CommonJITThunkID::HandleException), commonThunkKey(CommonJITThunkID::ThrowStackOverflowAtPrologue) };
    std::array thunks {
        reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr()),
        reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::ThrowStackOverflowAtPrologue).untaggedPtr()),
    };

    enum class Refusal : uint8_t {
        None,
        BeforeVeneers,
        InsideVeneers,
    };
    struct Attempt {
        Refusal refusal { Refusal::None };
        std::array<uint32_t, 2> sites { }; // The two jumps' labels in the linked code, when refused inside the veneers.
        std::optional<LinkedCode> linked;
    };
    // One try with pointers Pointer fixups recorded between the deferrals and the veneers. Nullopt when no executable
    // memory was left for the code.
    auto attempt = [&](unsigned pointers) -> std::optional<Attempt> {
        auto budget = ProducerBudget::create(kRecordChargeStep);
        Attempt result;
        {
            ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, budget.copyRef());
            CCallHelpers jit;
            recorder.attachTo(jit);
            std::array<MacroAssembler::Jump, 2> jumps;
            for (size_t index = 0; index < jumps.size(); ++index) {
                jumps[index] = jit.branch32(index ? CCallHelpers::NotEqual : CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
                linkJumpToSupport(jit, vm, jumps[index], keys[index]);
            }
            for (unsigned index = 0; index < pointers; ++index)
                moveReference(jit, ImageReference::vmAddress(vm, VMAddress::VM), GPRInfo::regT2);
            jit.ret();
            if (!recorder.isRecording())
                result.refusal = Refusal::BeforeVeneers;
            else {
                recorder.emitVeneers(jit);
                if (!recorder.isRecording()) {
                    result.refusal = Refusal::InsideVeneers;
                    JITCACHE_CHECK(recorder.state() == RecordState::Incomplete);
                    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
                    if (linkBuffer.didFailToAllocate()) {
                        JITCACHE_FAIL("no executable memory for the test code"_s);
                        return std::nullopt;
                    }
                    for (size_t index = 0; index < jumps.size(); ++index)
                        result.sites[index] = linkBuffer.offsetOf(jumps[index].assemblerLabel());
                    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
                    result.linked.emplace(LinkedCode { linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s), bytes });
                    JITCACHE_CHECK(!recorder.finishSnippet(linkBuffer));
                }
            }
        }
        // The recorder released everything it charged (I14).
        JITCACHE_CHECK(!budget->chargedBytes());
        return result;
    };
    // Each jump of a try refused inside the veneers reaches its thunk through a veneer of its own.
    auto reachesThroughOwnVeneers = [&](const Attempt& refused, unsigned pointers) {
        auto codeAddress = reinterpret_cast<uintptr_t>(refused.linked->bytes.data());
        auto codeEnd = codeAddress + refused.linked->bytes.size();
        std::array<uintptr_t, 2> veneers { };
        for (size_t index = 0; index < refused.sites.size(); ++index) {
            auto veneer = arm64ConditionalTarget(codeAddress + refused.sites[index]);
            bool throughVeneer = veneer && *veneer >= codeAddress && *veneer < codeEnd && (readWord(*veneer) & 0xfc000000) == 0x14000000;
            if (!throughVeneer || !reaches(*veneer, thunks[index])) {
                JITCACHE_FAIL(makeString("with "_s, pointers, " pointers the conditional jump at "_s, refused.sites[index], " does not reach its thunk through a veneer"_s));
                return false;
            }
            veneers[index] = *veneer;
        }
        if (veneers[0] == veneers[1]) {
            JITCACHE_FAIL(makeString("with "_s, pointers, " pointers both jumps reach their thunks through one veneer"_s));
            return false;
        }
        return true;
    };

    for (unsigned pointers = 0; pointers < 1024; ++pointers) {
        // The first count refused at all.
        auto lastVeneerRefused = attempt(pointers);
        if (!lastVeneerRefused)
            return;
        if (lastVeneerRefused->refusal == Refusal::None)
            continue;
        if (lastVeneerRefused->refusal == Refusal::BeforeVeneers) {
            JITCACHE_FAIL(makeString("with "_s, pointers, " pointers the budget refused before the veneers, and no smaller count refused inside them"_s));
            return;
        }
        if (!reachesThroughOwnVeneers(*lastVeneerRefused, pointers))
            return;

        auto firstVeneerRefused = attempt(pointers + 1);
        if (!firstVeneerRefused)
            return;
        if (firstVeneerRefused->refusal != Refusal::InsideVeneers) {
            JITCACHE_FAIL(makeString("with "_s, pointers + 1, " pointers the budget did not refuse inside the veneers"_s));
            return;
        }
        if (!reachesThroughOwnVeneers(*firstVeneerRefused, pointers + 1))
            return;

        // One more Pointer fixup is refused, so the vector was full as emitVeneers began the try before, and the first
        // veneer's fixup was the charge it refused.
        auto pastFull = attempt(pointers + 2);
        if (!pastFull)
            return;
        if (pastFull->refusal != Refusal::BeforeVeneers)
            JITCACHE_FAIL(makeString("with "_s, pointers + 2, " pointers the budget did not refuse before the veneers, so with "_s, pointers + 1, " the first veneer's charge was not the refused one"_s));
        return;
    }
    JITCACHE_FAIL("no count of recorded pointers made the budget refuse inside emitVeneers"_s);
}
#endif

// T12, the refusal inside a MathIC snippet (SPEC-image.md sections 4.8 and 6.2, I19). In a Producer, jitcacheRegenerated
// compiles from int32 profiles with a Complete record, which its finalize capture commits. The test then charges the
// budget until less than one kRecordChargeStep is left: the next regeneration's log entry still fits, its snippet
// recorder's first step does not. The first double regenerates the add's full snippet under a recorder that stops before
// it records anything, so the snippet links every jump back into the image natively, and the record becomes Incomplete.
// Each later call takes a path of that snippet, back to the image's done location or, for a string and for an operand
// whose valueOf throws, through its conditional jumps to the image's slow path, and must return or throw what the engine
// computes. delta then raises the refusal at budget.limit, and once the VM is destroyed the budget holds nothing.
JITCACHE_TEST_WITH_OPTIONS(imageRefusedRegenerationLinksItsSnippet, No, "--useConcurrentJIT=false")
{
    TemporaryArtifactDirectory directory;
    if (directory.path().isEmpty()) {
        JITCACHE_FAIL(makeString("mkdtemp failed: "_s, String::fromUTF8(safeStrerror(directory.error()).data())));
        return;
    }
    RefPtr<ProducerBudget> budget;
    {
        OwnedVM producer;
        VM& vm = producer.vm();
        JITCache::Config config;
        config.artifactPath = makeString(directory.path(), "/artifact"_s);
        config.role = Role::Producer;
        config.producerLimitBytes = 64 * MB;
        config.strict = true;
        StartResult started = JITCache::start(vm, config);
        if (started.outcome != StartOutcome::Started) {
            JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
            return;
        }
        ProducerContext* production = JITCache::producerContext(vm);
        if (!production) {
            JITCACHE_FAIL("the started producer has no producer context"_s);
            return;
        }
        budget = production->budget();

        auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
        auto run = [&](ASCIILiteral source) {
            NakedPtr<Exception> exception;
            evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
            if (exception)
                JITCACHE_FAIL(makeString("evaluating \""_s, source, "\" threw"_s));
            return !exception;
        };
        if (!run("function jitcacheRegenerated(a, b) { return a + b; }\nfor (var i = 0; i < 40; ++i)\n    jitcacheRegenerated(i, 1);\n"_s))
            return;
        auto* function = globalFunction(*globalObject, "jitcacheRegenerated"_s);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
            JITCACHE_FAIL("jitcacheRegenerated did not compile to baseline code"_s);
            return;
        }
        RefPtr<JSC::JITCode> jitCode = codeBlock->jitCode();
        auto& code = static_cast<BaselineJITCode&>(*jitCode);
        auto* record = code.m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("jitcacheRegenerated's record is missing or not complete"_s);
            return;
        }
        JITCACHE_CHECK(JITCache::status(vm).progress.capturesCommitted);

        size_t exhausting = budget->limitBytes() - budget->chargedBytes() - (kRecordChargeStep - 1);
        if (!budget->tryCharge(exhausting)) {
            JITCACHE_FAIL("the test could not charge the budget"_s);
            return;
        }
        bool regenerated = run("if (jitcacheRegenerated(1.5, 2) !== 3.5) throw new Error('1.5 + 2');"_s);
        JITCACHE_CHECK(regenerated);
        JITCACHE_CHECK(budget->hasRefused());
        JITCACHE_CHECK(record->state() == RecordState::Incomplete);
        bool hasSnippet = false;
        code.forEachMathIC([&](auto& mathIC) {
            hasSnippet |= !!mathIC.m_code;
        });
        JITCACHE_CHECK(hasSnippet);

        bool tookEveryPath = run(
            "if (jitcacheRegenerated(2.5, 0.25) !== 2.75) throw new Error('2.5 + 0.25');\n"
            "if (jitcacheRegenerated(1, 2.5) !== 3.5) throw new Error('1 + 2.5');\n"
            "if (jitcacheRegenerated(2147483647, 1) !== 2147483648) throw new Error('2147483647 + 1');\n"
            "if (jitcacheRegenerated('a', 1) !== 'a1') throw new Error('a + 1');\n"
            "var jitcacheThrew = false;\n"
            "try { jitcacheRegenerated({ valueOf() { throw new Error('valueOf throws'); } }, 1); } catch (error) { jitcacheThrew = error.message === 'valueOf throws'; }\n"
            "if (!jitcacheThrew) throw new Error('the exception valueOf threw did not reach the caller');\n"_s);
        JITCACHE_CHECK(tookEveryPath);
        JITCACHE_CHECK(function->jsExecutable()->codeBlockForCall() == codeBlock);
        JITCACHE_CHECK(codeBlock->jitType() == JITType::BaselineJIT);

        DeltaResult result = JITCache::delta(vm);
        JITCACHE_CHECK(result.outcome == DeltaOutcome::Faulted);
        JITCACHE_CHECK(result.fault && result.fault->stepName() == "budget.limit"_s);
        budget->release(exhausting);
    }
    // The VM is gone, with every record and all production memory.
    JITCACHE_CHECK(budget && !budget->chargedBytes());
}

namespace ImageRecordingTestsInternal {

// The distance T13 puts between an image and its support code: the reach of an ARM64 bl or b, beyond which patching
// places jump islands. x86_64's rel32 reaches the whole pool, so the same distance needs none there.
static constexpr size_t islandDistance = 128 * MB;
#if CPU(ARM64)
static_assert(islandDistance == MacroAssembler::nearJumpRange);
#endif

// The thunk a Call or Jump fixup branches to, through the native lookup its support key names; nullopt for a Pointer and
// for a branch whose target lies in the image or its snippets.
static std::optional<uintptr_t> supportBranchTarget(VM& vm, const ImageFixup& fixup)
{
    if (fixup.form == FixupForm::Pointer)
        return std::nullopt;
    switch (fixup.target.kind) {
    case TargetKind::CommonThunk:
    case TargetKind::BaselineThunk:
    case TargetKind::SlowPathThunk:
    case TargetKind::InlineCacheSlowPathThunk:
    case TargetKind::VirtualCallThunk:
    case TargetKind::ProcessThunk:
        return reinterpret_cast<uintptr_t>(resolveSupport(vm, fixup.target).untaggedPtr());
    default:
        return std::nullopt;
    }
}

// Every support branch of an image reaches its thunk the way an image that far from its thunks must: through a jump
// island on ARM64 and directly on x86_64. Returns how many it checked.
template<typename Fixups>
static unsigned checkFarSupportBranches(TestContext& context, VM& vm, ASCIILiteral image, std::span<const uint8_t> code, const Fixups& fixups)
{
    unsigned checked = 0;
    for (size_t index = 0; index < fixups.size(); ++index) {
        ImageFixup fixup = fixups[index];
        auto support = supportBranchTarget(vm, fixup);
        if (!support)
            continue;
        ++checked;
        auto direct = decodeFixup(code, fixup);
#if CPU(ARM64)
        bool reachesFarThunk = direct && *direct != *support && reaches(*direct, *support);
#else
        bool reachesFarThunk = direct && *direct == *support;
#endif
        if (!reachesFarThunk) {
            JITCACHE_FAIL(makeString(image, ": the support branch at "_s, fixup.site, " does not reach its thunk the way a far image must"_s));
            break;
        }
    }
    return checked;
}

// Two functions with one text in one scope, as T21's bodies, each 1000 statements of property loads and arithmetic with
// their slow calls and exception checks. Their image is larger than the pages the JIT heap shares among its small and
// medium allocations, so every allocation of its size comes from the heap's large-object free lists, which give each the
// lowest free range that holds it. jitcacheFarProducer runs once, in the LLInt.
static String farBodiesSource()
{
    constexpr unsigned statements = 1000;
    StringBuilder text;
    text.append("function (o) {\n    var sum = 0;\n"_s);
    for (unsigned index = 0; index < statements; ++index)
        text.append("    sum = sum + o.p"_s, index % 64, " * "_s, index + 1, ";\n"_s);
    text.append("    return sum;\n}"_s);
    String body = text.toString();
    return makeString("var jitcacheFarProducer, jitcacheFarImport;\n(function makeFarBodies() {\n    jitcacheFarProducer = "_s, body,
        ";\n    jitcacheFarImport = "_s, body, ";\n})();\nvar jitcacheFarObject = {};\nfor (var k = 0; k < 64; ++k)\n    jitcacheFarObject['p' + k] = k;\njitcacheFarProducer(jitcacheFarObject);\n"_s);
}

} // namespace ImageRecordingTestsInternal

// T13 (SPEC-image.md sections 3.2, 9 and 10.3). In the ASan builds that run this test the executable pool is the libpas
// JIT heap, where useRandomizingExecutableIslandAllocation only allocates and frees a few blocks before each allocation,
// so no option puts an image out of a bl's reach of its thunks, and the test makes the distance itself. A first
// compilation of jitcacheFarProducer generates every thunk its image calls or jumps to. Blocks of the image's size then
// fill the pool until one starts more than islandDistance past the farthest of those thunks, and stay allocated until the
// test ends, so every image allocated after them lies out of reach too: the producer's, captured with strict on, whose
// S1 follows its islands; the one prepareImage restores for jitcacheFarImport with strict on, which the writers patch
// through islands and whose S5 follows them; and the twin that Twins::checkImage compiles and compares with it, through
// the islands of both. Every support branch of the producer's and the restored image must reach its thunk through an
// island on ARM64, and directly on x86_64, whose rel32 reaches the whole pool.
JITCACHE_TEST_WITH_OPTIONS(imageImportPatchesThroughIslandsToFarSupport, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(farBodiesSource(), SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the far bodies threw"_s);
        return;
    }
    auto* producerFunction = globalFunction(*globalObject, "jitcacheFarProducer"_s);
    auto* importFunction = globalFunction(*globalObject, "jitcacheFarImport"_s);
    CodeBlock* producer = producerFunction ? producerFunction->jsExecutable()->codeBlockForCall() : nullptr;
    if (!producer || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("a far body is missing, or jitcacheFarImport was called"_s);
        return;
    }

    // The thunks every compilation of the body branches to, and the size of its image.
    uintptr_t farthestSupport = 0;
    size_t codeSize = 0;
    {
        auto code = compileUnderTwinRecorder(vm, *producer);
        auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("the first compilation of jitcacheFarProducer left no complete record"_s);
            return;
        }
        codeSize = record->codeSize();
        for (auto& fixup : record->fixups()) {
            if (auto support = supportBranchTarget(vm, fixup))
                farthestSupport = std::max(farthestSupport, *support);
        }
    }
    if (!farthestSupport) {
        JITCACHE_FAIL("jitcacheFarProducer's image branches to no thunk"_s);
        return;
    }
    uintptr_t reach = farthestSupport + islandDistance;

    Vector<RefPtr<ExecutableMemoryHandle>> fill;
    while (true) {
        auto block = ExecutableAllocator::singleton().allocate(codeSize, JITCompilationCanFail);
        if (!block) {
            JITCACHE_FAIL(makeString("the pool held "_s, fill.size(), " blocks of "_s, codeSize, " bytes, none out of the thunks' reach"_s));
            return;
        }
        auto start = reinterpret_cast<uintptr_t>(block->start().untaggedPtr());
        fill.append(WTF::move(block));
        if (start > reach)
            break;
    }
    auto isFar = [&](const void* start) {
        return reinterpret_cast<uintptr_t>(start) > reach;
    };

    auto captured = imageCompileAndCapture(context, vm, *producerFunction);
    if (!captured)
        return;
    auto* producerRecord = captured->code->m_jitCacheImageRecord.get();
    if (!producerRecord || producerRecord->state() != RecordState::Complete || !isFar(captured->code->start())) {
        JITCACHE_FAIL("the producer's image is not complete, or lies within reach of its thunks"_s);
        return;
    }
    auto producerCode = unsafeMakeSpan(static_cast<const uint8_t*>(captured->code->start()), producerRecord->codeSize());
    JITCACHE_CHECK(checkFarSupportBranches(context, vm, "the producer's image"_s, producerCode, producerRecord->fixups()));

    bool imported = imageImportAndCheckTwin(context, vm, *importFunction, *captured, [&](BaselineJITCode& restored, const ImageSectionsView& view) {
        if (!isFar(restored.start())) {
            JITCACHE_FAIL("the restored image lies within reach of its thunks"_s);
            return;
        }
        auto restoredCode = unsafeMakeSpan(static_cast<const uint8_t*>(restored.start()), view.header().codeSize);
        JITCACHE_CHECK(checkFarSupportBranches(context, vm, "the restored image"_s, restoredCode, view.fixups()));
    });
    JITCACHE_CHECK(imported);
}

namespace ImageRecordingTestsInternal {

// T17's fills, one per side, for the bytes each allocation holds past its linked size. None is zero, so the check of
// performJITMemcpy for runs of zeros never objects.
static constexpr uint8_t producerSlackFill = 0xa5;
static constexpr uint8_t restoredSlackFill = 0x5a;
static constexpr uint8_t twinSlackFill = 0xc3;

// The bytes an allocation of handleSize bytes at start holds past its linkedSize, after writing fill over them when one is
// given. Empty when the handle ends at the linked size.
static Vector<uint8_t> imageSlackBytes(const void* start, size_t linkedSize, size_t handleSize, std::optional<uint8_t> fill)
{
    if (!start || handleSize <= linkedSize)
        return { };
    auto slack = unsafeMakeSpan(static_cast<const uint8_t*>(start), handleSize).subspan(linkedSize);
    if (fill) {
        Vector<uint8_t> filled(FillWith { }, slack.size(), *fill);
        performJITMemcpy<jitMemcpyRepatch>(const_cast<uint8_t*>(slack.data()), filled.span().data(), filled.size());
    }
    Vector<uint8_t> bytes;
    bytes.append(slack);
    return bytes;
}

// Whether section is the canonical form (SPEC-image.md section 8.4) of exactly linkedCode under its fixups.
static bool imageSectionHoldsLinkedBytes(std::span<const uint8_t> section, std::span<const uint8_t> linkedCode, std::span<const ImageFixup> fixups)
{
    Vector<uint8_t> canonical;
    auto append = [&](std::span<const uint8_t> bytes) {
        canonical.append(bytes);
        return true;
    };
    ImageSectionSink sink = append;
    ImageSectionWriter writer(sink);
    return writer.writeCanonicalCode(linkedCode, fixups) && equalSpans(canonical.span(), section);
}

// An allocation a LinkBuffer announced: its name and its handle, [start, start + handleSize).
struct ImageAnnouncedAllocation {
    String name;
    uintptr_t start { 0 };
    size_t handleSize { 0 };
};

// The allocation in one line LinkBuffer::finalizeCodeWithDisassemblyImpl prints while logJIT is on, "Generated JIT code
// for <name>: [<start>, <end>) <size> bytes", where [start, end) is the allocation's whole handle; nullopt for any other
// line.
static std::optional<ImageAnnouncedAllocation> imageParseAnnouncedAllocation(StringView line)
{
    constexpr auto prefix = "Generated JIT code for "_s;
    if (!line.startsWith(prefix))
        return std::nullopt;
    size_t bracket = line.reverseFind(": ["_s);
    if (bracket == notFound || bracket < prefix.length())
        return std::nullopt;
    StringView range = line.substring(static_cast<unsigned>(bracket + 3));
    size_t comma = range.find(", "_s);
    size_t close = range.find(')');
    if (comma == notFound || close == notFound || close < comma + 2)
        return std::nullopt;
    auto address = [](StringView text) -> std::optional<uint64_t> {
        if (!text.startsWith("0x"_s))
            return std::nullopt;
        return parseInteger<uint64_t>(text.substring(2), 16);
    };
    auto start = address(range.left(static_cast<unsigned>(comma)));
    auto end = address(range.substring(static_cast<unsigned>(comma + 2), static_cast<unsigned>(close - comma - 2)));
    if (!start || !end || *end < *start)
        return std::nullopt;
    StringView name = line.substring(static_cast<unsigned>(prefix.length()), static_cast<unsigned>(bracket - prefix.length()));
    return ImageAnnouncedAllocation { name.toString(), static_cast<uintptr_t>(*start), static_cast<size_t>(*end - *start) };
}

// The allocations LinkBuffer announces through dataLog while logJIT is on (imageParseAnnouncedAllocation), each on the
// thread that linked it, right after linking and before anything else reads the code. Installed as the process's data
// file, it lets T17 reach the twin image and twin snippets Twins::checkImage links and frees before it returns. While
// armed, it fills each one's bytes past the linked size it was told with twinSlackFill and keeps what they then hold:
// the first baseline image is the twin's, and each snippet after it the replay of the next regeneration in log order.
class ImageTwinSlackLog final : public PrintStream {
    WTF_MAKE_NONCOPYABLE(ImageTwinSlackLog);
public:
    ImageTwinSlackLog() = default;

    void arm(uint32_t imageLinkedSize, Vector<uint32_t>&& snippetLinkedSizes)
    {
        m_armed = true;
        m_imageLinkedSize = imageLinkedSize;
        m_snippetLinkedSizes = WTF::move(snippetLinkedSizes);
        m_twinImageSlack = std::nullopt;
        m_twinSnippetSlacks.clear();
    }

    void disarm() { m_armed = false; }

    // Nullopt when the check announced no baseline image.
    const std::optional<Vector<uint8_t>>& twinImageSlack() const { return m_twinImageSlack; }
    const Vector<Vector<uint8_t>>& twinSnippetSlacks() const { return m_twinSnippetSlacks; }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    void vprintf(const char* format, va_list argumentList) final WTF_ATTRIBUTE_PRINTF(2, 0)
    {
        // Complete lines are acted on once the lock is released, since filling calls into the engine.
        Vector<String> lines;
        {
            Locker locker { m_lock };
            StringPrintStream formatted;
            formatted.vprintf(format, argumentList);
            m_partialLine.append(formatted.toString());
            String text = m_partialLine.toString();
            m_partialLine.clear();
            unsigned lineStart = 0;
            for (size_t newline = text.find('\n'); newline != notFound; newline = text.find('\n', lineStart)) {
                lines.append(text.substring(lineStart, static_cast<unsigned>(newline) - lineStart));
                lineStart = static_cast<unsigned>(newline) + 1;
            }
            m_partialLine.append(StringView(text).substring(lineStart));
        }
        for (auto& line : lines)
            announce(line);
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

private:
    void announce(StringView line)
    {
        if (!m_armed)
            return;
        auto allocation = imageParseAnnouncedAllocation(line);
        if (!allocation)
            return;
        auto* start = reinterpret_cast<const void*>(allocation->start);
        if (allocation->name.startsWith("Baseline JIT code for "_s)) {
            if (!m_twinImageSlack)
                m_twinImageSlack = imageSlackBytes(start, m_imageLinkedSize, allocation->handleSize, twinSlackFill);
            return;
        }
        if (allocation->name.startsWith("JITMathIC: generating out of line"_s) && allocation->handleSize && m_twinSnippetSlacks.size() < m_snippetLinkedSizes.size())
            m_twinSnippetSlacks.append(imageSlackBytes(start, m_snippetLinkedSizes[m_twinSnippetSlacks.size()], allocation->handleSize, twinSlackFill));
    }

    Lock m_lock;
    StringBuilder m_partialLine;
    bool m_armed { false };
    uint32_t m_imageLinkedSize { 0 };
    Vector<uint32_t> m_snippetLinkedSizes;
    std::optional<Vector<uint8_t>> m_twinImageSlack;
    Vector<Vector<uint8_t>> m_twinSnippetSlacks;
};

// Makes stream the process's data file while the scope lives, then gives standard error back. WTF::setDataFile keeps the
// stream it is handed without ever deleting it, so its owner keeps owning it.
class ImageDataFileScope {
    WTF_MAKE_NONCOPYABLE(ImageDataFileScope);
public:
    explicit ImageDataFileScope(PrintStream& stream)
    {
        WTF::setDataFile(std::unique_ptr<PrintStream>(&stream));
    }

    ~ImageDataFileScope()
    {
        WTF::setDataFile(static_cast<const char*>(nullptr));
    }
};

static constexpr unsigned slackBodyCount = 24;

// T17's bodies: pairs of one text in one scope, as T21's, the first of each pair the producer's and the second never
// called, so its first CodeBlock is the import's newborn. Body k stores 2k + 1 properties, so the images' linked sizes
// spread over many offsets from the allocator's size classes, and returns one arithmetic operation, whose MathIC is the
// body's only one and whose kind and operands cycle with k, so the snippets' linked sizes vary too. The program calls
// each producer 40 times with int32s, which compiles it from int32 profiles, and once more with a double, which
// regenerates its MathIC's full snippet.
static String slackBodiesSource()
{
    static constexpr std::array operations { "x + o.p0"_s, "o.p0 - x"_s, "x * o.p0"_s, "-x"_s, "x + 7"_s, "9 - x"_s, "x * 3"_s, "o.p1 + x"_s };
    StringBuilder makers;
    for (unsigned index = 0; index < slackBodyCount; ++index) {
        StringBuilder body;
        body.append("function (o, x) {\n"_s);
        for (unsigned statement = 0; statement <= 2 * index; ++statement)
            body.append("        o.s"_s, statement, " = o.p"_s, statement % 8, ";\n"_s);
        body.append("        return "_s, operations[index % operations.size()], ";\n    }"_s);
        String text = body.toString();
        makers.append("    jitcacheSlackProducers.push("_s, text, ");\n    jitcacheSlackImports.push("_s, text, ");\n"_s);
    }
    return makeString("var jitcacheSlackProducers = [], jitcacheSlackImports = [];\n(function makeSlackBodies() {\n"_s, makers.toString(), "})();\n"
        "var jitcacheSlackObject = { p0: 1, p1: 2, p2: 3, p3: 4, p4: 5, p5: 6, p6: 7, p7: 8 };\n"
        "for (var i = 0; i < 40; ++i) {\n    for (var k = 0; k < jitcacheSlackProducers.length; ++k)\n        jitcacheSlackProducers[k](jitcacheSlackObject, i);\n}\n"
        "for (var k = 0; k < jitcacheSlackProducers.length; ++k)\n    jitcacheSlackProducers[k](jitcacheSlackObject, 0.5);\n"_s);
}

// What each side holds past the linked size of one body's image and of its snippet: the producer's, the restored image's
// and the twin's.
struct ImageSlackOfBody {
    Vector<uint8_t> producerImage;
    Vector<uint8_t> restoredImage;
    Vector<uint8_t> twinImage;
    Vector<uint8_t> producerSnippet;
    Vector<uint8_t> restoredSnippet;
    Vector<uint8_t> twinSnippet;
};

// T17 for one body. producerFunction's code, whose record must be complete with its one MathIC regenerated, gets the
// producer's fill past its linked sizes and is captured with strict on, and the sections must hold exactly the linked
// bytes. importFunction's newborn CodeBlock then imports them: the restored image and snippet get the restored fill before
// setup, and the twin's get the twin fill as the check links them. Returns what each side holds past its linked sizes
// once the check reported nothing.
static NEVER_INLINE std::optional<ImageSlackOfBody> imageCheckSlackOfBody(TestContext& context, VM& vm, JSFunction& producerFunction, JSFunction& importFunction, ImageTwinSlackLog& log)
{
    CodeBlock* codeBlock = producerFunction.jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("a slack body did not compile to baseline code"_s);
        return std::nullopt;
    }
    CapturedImage captured;
    captured.code = static_cast<BaselineJITCode*>(codeBlock->jitCode().get());
    auto* record = captured.code->m_jitCacheImageRecord.get();
    if (!record || record->state() != RecordState::Complete || record->mathICs().size() != 1 || !record->mathICs()[0].snippet) {
        JITCACHE_FAIL("a slack body's record is missing, not complete, or not one regenerated MathIC"_s);
        return std::nullopt;
    }
    const MathICRecord& mathIC = record->mathICs()[0];
    uint32_t codeSize = record->codeSize();
    uint32_t snippetSize = mathIC.snippet->size;
    MathICCodeState producerSnippet = mathICCodeState(mathIC.kind, mathIC.mathIC);
    if (producerSnippet.snippetStart != mathIC.snippet->start) {
        JITCACHE_FAIL("a slack body's MathIC holds another snippet than its record describes"_s);
        return std::nullopt;
    }

    // A capture that read past a linked size would carry the producer's fill.
    imageSlackBytes(captured.code->start(), codeSize, captured.code->size(), producerSlackFill);
    imageSlackBytes(producerSnippet.snippetStart, snippetSize, producerSnippet.snippetHandleSize, producerSlackFill);
    if (!imageCaptureInto(context, vm, *codeBlock, captured))
        return std::nullopt;

    auto view = parseImageSections(ImageSectionSpans { .image = captured.image.span(), .bakedFacts = captured.bakedFacts.span(), .twins = captured.twins.span() }, true);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(view.error())));
        return std::nullopt;
    }
    auto linkedImage = unsafeMakeSpan(static_cast<const uint8_t*>(captured.code->start()), codeSize);
    if (view->header().codeSize != codeSize || !imageSectionHoldsLinkedBytes(view->code(), linkedImage, record->fixups().span()))
        JITCACHE_FAIL("the image section's code is not exactly the image's linked bytes"_s);
    unsigned snippets = 0;
    view->forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        ++snippets;
        auto linkedSnippet = unsafeMakeSpan(static_cast<const uint8_t*>(mathIC.snippet->start), snippetSize);
        if (snippet.code.size() != snippetSize || !imageSectionHoldsLinkedBytes(snippet.code, linkedSnippet, mathIC.snippet->fixups.span()))
            JITCACHE_FAIL("the image section's snippet is not exactly the snippet's linked bytes"_s);
    });
    JITCACHE_CHECK(snippets == 1);

    RefPtr<BaselineJITCode> restored;
    MathICCodeState restoredSnippet;
    bool imported = imageImportAndCheckTwin(context, vm, importFunction, captured, [&](BaselineJITCode& code, const ImageSectionsView&) {
        restored = &code;
        code.forEachMathIC([&](auto& restoredMathIC) {
            if (restoredMathIC.m_code) {
                restoredSnippet.snippetStart = restoredMathIC.m_code.code().untaggedPtr();
                restoredSnippet.snippetHandleSize = restoredMathIC.m_code.size();
            }
        });
        imageSlackBytes(code.start(), codeSize, code.size(), restoredSlackFill);
        imageSlackBytes(restoredSnippet.snippetStart, snippetSize, restoredSnippet.snippetHandleSize, restoredSlackFill);
    }, [&](const auto& check) {
        log.arm(codeSize, Vector<uint32_t> { snippetSize });
        check();
        log.disarm();
    });
    if (!imported || !restored || !restoredSnippet.snippetStart) {
        if (imported)
            JITCACHE_FAIL("the restored image has no snippet"_s);
        return std::nullopt;
    }
    if (!log.twinImageSlack() || log.twinSnippetSlacks().size() != 1) {
        JITCACHE_FAIL(makeString("the twin check announced "_s, log.twinImageSlack() ? 1u : 0u, " images and "_s, log.twinSnippetSlacks().size(), " snippets, not one of each"_s));
        return std::nullopt;
    }

    ImageSlackOfBody slack;
    slack.producerImage = imageSlackBytes(captured.code->start(), codeSize, captured.code->size(), std::nullopt);
    slack.restoredImage = imageSlackBytes(restored->start(), codeSize, restored->size(), std::nullopt);
    slack.twinImage = *log.twinImageSlack();
    slack.producerSnippet = imageSlackBytes(producerSnippet.snippetStart, snippetSize, producerSnippet.snippetHandleSize, std::nullopt);
    slack.restoredSnippet = imageSlackBytes(restoredSnippet.snippetStart, snippetSize, restoredSnippet.snippetHandleSize, std::nullopt);
    slack.twinSnippet = log.twinSnippetSlacks()[0];
    return slack;
}

} // namespace ImageRecordingTestsInternal

// T17 (SPEC-image.md sections 3.1, 8.4 and 11.4, N20). An allocation's handle (JITCodeWithCodeRef::size, a MathIC's
// m_code.size()) can end past what its LinkBuffer wrote, the linked size the record keeps, and those bytes hold whatever
// the pool held. In a Producer, every body of slackBodiesSource compiles with a complete record and regenerates its one
// MathIC. For each, the producer's image and snippet are filled past their linked sizes and captured with strict on, and
// the sections must hold exactly the linked bytes; the import then fills the restored image and snippet past theirs, and
// the twin's are filled as Twins::checkImage links them, so wherever two sides have slack it differs while the check
// compares, and the check must report nothing. Afterwards each side's slack must still hold its own fill, at least one
// body's image must have slack on all three sides, and on ARM64 at least one body's snippet must too.
JITCACHE_TEST_WITH_OPTIONS(imageSlackStaysOutsideCaptureAndTwinCheck, No, "--useConcurrentJIT=false --logJIT=true")
{
    ImageTwinSlackLog log;
    ImageDataFileScope dataFile(log);
    TemporaryArtifactDirectory directory;
    if (directory.path().isEmpty()) {
        JITCACHE_FAIL(makeString("mkdtemp failed: "_s, String::fromUTF8(safeStrerror(directory.error()).data())));
        return;
    }
    OwnedVM producer;
    VM& vm = producer.vm();
    JITCache::Config config;
    config.artifactPath = makeString(directory.path(), "/artifact"_s);
    config.role = Role::Producer;
    config.producerLimitBytes = 64 * MB;
    config.strict = true;
    StartResult started = JITCache::start(vm, config);
    if (started.outcome != StartOutcome::Started) {
        JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
        return;
    }

    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(slackBodiesSource(), SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the slack bodies threw"_s);
        return;
    }
    auto* producers = globalObject->get(globalObject, Identifier::fromString(vm, "jitcacheSlackProducers"_s)).getObject();
    auto* imports = globalObject->get(globalObject, Identifier::fromString(vm, "jitcacheSlackImports"_s)).getObject();
    if (!producers || !imports) {
        JITCACHE_FAIL("the slack bodies' arrays are missing"_s);
        return;
    }

    auto holdsOwnFill = [&](unsigned index, ASCIILiteral side, const Vector<uint8_t>& slack, uint8_t fill) {
        if (!slack.isEmpty() && !std::ranges::all_of(slack.span(), [&](uint8_t byte) { return byte == fill; }))
            JITCACHE_FAIL(makeString("body "_s, index, ": "_s, side, " no longer holds its fill past its linked size"_s));
    };
    unsigned imageCandidates = 0;
    unsigned snippetCandidates = 0;
    StringBuilder slackSizes;
    for (unsigned index = 0; index < slackBodyCount; ++index) {
        auto* producerFunction = dynamicDowncast<JSFunction>(producers->get(globalObject, index));
        auto* importFunction = dynamicDowncast<JSFunction>(imports->get(globalObject, index));
        if (!producerFunction || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
            JITCACHE_FAIL(makeString("slack body "_s, index, " is missing, or its import was called"_s));
            return;
        }
        // The earlier bodies' twins die and are swept here, so no allocation of this body's check sweeps one and changes
        // the twin count the check compares.
        imageClearDeadStack();
        vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
        auto slack = imageCheckSlackOfBody(context, vm, *producerFunction, *importFunction, log);
        if (!slack)
            return;
        holdsOwnFill(index, "the producer's image"_s, slack->producerImage, producerSlackFill);
        holdsOwnFill(index, "the restored image"_s, slack->restoredImage, restoredSlackFill);
        holdsOwnFill(index, "the twin's image"_s, slack->twinImage, twinSlackFill);
        holdsOwnFill(index, "the producer's snippet"_s, slack->producerSnippet, producerSlackFill);
        holdsOwnFill(index, "the restored snippet"_s, slack->restoredSnippet, restoredSlackFill);
        holdsOwnFill(index, "the twin's snippet"_s, slack->twinSnippet, twinSlackFill);
        imageCandidates += !slack->producerImage.isEmpty() && !slack->restoredImage.isEmpty() && !slack->twinImage.isEmpty();
        snippetCandidates += !slack->producerSnippet.isEmpty() && !slack->restoredSnippet.isEmpty() && !slack->twinSnippet.isEmpty();
        slackSizes.append(index ? "; "_s : ""_s, "image "_s, slack->producerImage.size(), '/', slack->restoredImage.size(), '/', slack->twinImage.size(),
            ", snippet "_s, slack->producerSnippet.size(), '/', slack->restoredSnippet.size(), '/', slack->twinSnippet.size());
    }
    // On x86_64 a handle ends past its linked size, which LinkBuffer pads to jitAllocationGranule, only when the JIT
    // heap serves the request from a larger size class (N20). For an allocation as small as a snippet the heap usually
    // has a class of exactly its size, and whether it uses a larger one depends on the sizes the process allocated
    // first. On ARM64, compaction shortens a snippet after its LinkBuffer allocated it and the JIT heap does not shrink
    // an allocation that small, so the producer's and the twin's snippets keep slack; the restored snippet, allocated
    // at the linked size, has slack when that size is off the heap's 16-byte alignment, which the varied operations
    // make likely. So the check requires a snippet with slack only on ARM64.
    if (!imageCandidates || (isARM64() && !snippetCandidates))
        JITCACHE_FAIL(makeString("no body has slack on all three sides for its image ("_s, imageCandidates, " do) or its snippet ("_s, snippetCandidates, " do); slack bytes, producer/restored/twin: "_s, slackSizes.toString()));
}

namespace ImageRecordingTestsInternal {

// The exported headers the Image lane edits (SPEC-image.md section 14.4), by the names their copies carry.
static constexpr std::array imageEditedExportedHeaders {
    "AbstractMacroAssembler.h"_s,
    "LinkBuffer.h"_s,
    "MacroAssemblerARM64.h"_s,
    "AssemblyHelpers.h"_s,
    "BaselineJITCode.h"_s,
    "CCallHelpers.h"_s,
    "JIT.h"_s,
    "JITCodeMap.h"_s,
    "JITMathIC.h"_s,
    "JITOperations.h"_s,
};

static std::optional<String> imageReadText(const String& path)
{
    auto bytes = FileSystem::readEntireFile(path);
    if (!bytes)
        return std::nullopt;
    return String(bytes->span());
}

// The file each #include directive of a header names, without its directories: "X.h" for "X.h", "jitcache/X.h" and
// <JavaScriptCore/X.h>. Every directive counts, whatever preprocessor condition it sits under, since another build's
// switches can turn it on.
static Vector<String> imageIncludedFileNames(StringView text)
{
    Vector<String> names;
    for (StringView line : text.split('\n')) {
        unsigned index = 0;
        auto skipSpaces = [&] {
            while (index < line.length() && isASCIIWhitespace(line[index]))
                ++index;
        };
        skipSpaces();
        if (index >= line.length() || line[index] != '#')
            continue;
        ++index;
        skipSpaces();
        constexpr auto include = "include"_s;
        if (!line.substring(index).startsWith(include))
            continue;
        index += static_cast<unsigned>(include.length());
        skipSpaces();
        if (index >= line.length() || (line[index] != '"' && line[index] != '<'))
            continue;
        char16_t close = line[index] == '"' ? '"' : '>';
        unsigned start = index + 1;
        size_t end = line.find(close, start);
        if (end == notFound)
            continue;
        StringView path = line.substring(start, static_cast<unsigned>(end) - start);
        size_t slash = path.reverseFind('/');
        names.append((slash == notFound ? path : path.substring(static_cast<unsigned>(slash) + 1)).toString());
    }
    return names;
}

// The source root CMake configured the build from, which Bun's recipe sets to this repository.
static String imageCMakeHomeDirectory(const String& buildDirectory)
{
    auto cache = imageReadText(FileSystem::pathByAppendingComponent(buildDirectory, "CMakeCache.txt"_s));
    if (!cache)
        return { };
    constexpr auto prefix = "CMAKE_HOME_DIRECTORY:INTERNAL="_s;
    for (StringView line : StringView(*cache).split('\n')) {
        if (line.startsWith(prefix))
            return line.substring(static_cast<unsigned>(prefix.length())).toString();
    }
    return { };
}

} // namespace ImageRecordingTestsInternal

// T19 (SPEC-image.md section 14.4, I21; SPEC-integrator.md II18), the check over the copied headers; the other half, Bun's
// build against those copies, is a build. Bun compiles against the copies JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS puts in
// <build>/JavaScriptCore/PrivateHeaders/JavaScriptCore and against no source directory, so no header the Image lane edits
// may include a jitcache/ header, and every jitcache/ header any copy includes must be a copy itself. testjitcache runs
// from <build>/bin, and the build's CMake cache names the source root whose jitcache/ directory lists the module's
// headers.
JITCACHE_TEST(imageExportedHeadersIncludeOnlyExportedJITCacheHeaders, No)
{
    String executable = FileSystem::realPath("/proc/self/exe"_s);
    String buildDirectory = FileSystem::parentPath(FileSystem::parentPath(executable));
    String sourceRoot = imageCMakeHomeDirectory(buildDirectory);
    if (sourceRoot.isEmpty()) {
        JITCACHE_FAIL(makeString("no CMAKE_HOME_DIRECTORY in the CMake cache of "_s, buildDirectory, ", the build testjitcache ("_s, executable, ") runs from"_s));
        return;
    }
    std::array<StringView, 3> jitcacheComponents { "Source"_s, "JavaScriptCore"_s, "jitcache"_s };
    String jitcacheDirectory = FileSystem::pathByAppendingComponents(sourceRoot, jitcacheComponents);
    std::array<StringView, 3> copiesComponents { "JavaScriptCore"_s, "PrivateHeaders"_s, "JavaScriptCore"_s };
    String copiesDirectory = FileSystem::pathByAppendingComponents(buildDirectory, copiesComponents);

    auto headersIn = [](const String& directory) {
        HashSet<String> headers;
        for (auto& name : FileSystem::listDirectory(directory)) {
            if (name.endsWith(".h"_s))
                headers.add(name);
        }
        return headers;
    };
    HashSet<String> jitcacheHeaders = headersIn(jitcacheDirectory);
    HashSet<String> copies = headersIn(copiesDirectory);
    if (!jitcacheHeaders.contains("ImageRecorder.h"_s) || !jitcacheHeaders.contains("JITCacheAPI.h"_s)) {
        JITCACHE_FAIL(makeString(jitcacheDirectory, " does not list the module's headers"_s));
        return;
    }
    if (!copies.contains("JITCacheAPI.h"_s)) {
        JITCACHE_FAIL(makeString(copiesDirectory, " holds no copy of the exported JITCacheAPI.h"_s));
        return;
    }
    for (auto edited : imageEditedExportedHeaders) {
        if (!copies.contains(edited))
            JITCACHE_FAIL(makeString("the exported header "_s, edited, " has no copy in "_s, copiesDirectory));
    }

    for (auto& copy : copies) {
        auto text = imageReadText(FileSystem::pathByAppendingComponent(copiesDirectory, copy));
        if (!text) {
            JITCACHE_FAIL(makeString("the copy "_s, copy, " cannot be read"_s));
            continue;
        }
        bool editedByImageLane = std::ranges::any_of(imageEditedExportedHeaders, [&](ASCIILiteral edited) { return copy == edited; });
        for (auto& included : imageIncludedFileNames(*text)) {
            if (!jitcacheHeaders.contains(included))
                continue;
            // I21: the headers this lane edits reach recording only through the hooks they declare themselves.
            if (editedByImageLane)
                JITCACHE_FAIL(makeString("I21: the exported header "_s, copy, " includes the jitcache/ header "_s, included));
            // II18: a jitcache/ header an exported header includes must resolve among the copies Bun sees.
            if (!copies.contains(included))
                JITCACHE_FAIL(makeString("II18: the exported header "_s, copy, " includes "_s, included, ", a jitcache/ header that is not exported"_s));
        }
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
