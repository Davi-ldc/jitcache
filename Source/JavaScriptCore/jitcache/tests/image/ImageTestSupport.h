#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "BaselineJITPlan.h"
#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "ExecutableAllocator.h"
#include "FunctionExecutable.h"
#include "ImageTwinsSection.h"
#include "ImageTypes.h"
#include "JIT.h"
#include "JITCacheTest.h"
#include "JITThunks.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ProducerBudget.h"
#include "TestVM.h"
#include "UnlinkedCodeBlock.h"
#include <optional>
#include <span>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>

// What the Image lane's tests share: the seed every compared assembler starts from, the targets and thunks a fixup
// names, a UCB to record against, the decoding of what linked code holds or branches to, and a compilation under a twin
// recorder.

namespace JSC::JITCache::Tests {

// Every assembler a comparison emits into starts from this seed, so that a native sequence that draws (an untrusted
// immediate on x86_64) draws the same in both.
inline constexpr uint32_t assemblerSeed = 0x5eed1234;

inline ImageTarget makeTarget(TargetKind kind, uint32_t a = 0)
{
    return ImageTarget { .kind = kind, .a = a, .b = 0, .payload = 0 };
}

inline ImageTarget commonThunkKey(CommonJITThunkID id)
{
    return makeTarget(TargetKind::CommonThunk, static_cast<uint32_t>(id));
}

inline CodeLocationLabel<NoPtrTag> commonThunk(VM& vm, CommonJITThunkID id)
{
    return CodeLocationLabel<NoPtrTag>(vm.getCTIStub(id).retaggedCode<NoPtrTag>());
}

// A UCB with binary and unary arithmetic profiles, an identifier and a string constant, from a function called once so
// that its CodeBlock and UCB exist, for a recorder to record against. The global object stays reachable from the caller's
// stack.
struct RecordingBody {
    JSGlobalObject* globalObject { nullptr };
    UnlinkedCodeBlock* unlinkedCodeBlock { nullptr };
};

inline std::optional<RecordingBody> makeRecordingBody(TestContext& context, VM& vm)
{
    auto* globalObject = createRealm(context, vm, "function f(a, b) { return a.x + b - -b + 'jitcache'; } f({ x: 1 }, 2);"_s, "the test body"_s);
    if (!globalObject)
        return std::nullopt;
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, "f"_s));
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
    return RecordingBody { globalObject, unlinkedCodeBlock };
}

inline int64_t signExtend(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

#if CPU(X86_64)
inline uint64_t readLittleEndian(std::span<const uint8_t> bytes)
{
    uint64_t value = 0;
    for (size_t i = bytes.size(); i--;)
        value = (value << 8) | bytes[i];
    return value;
}
#endif

#if CPU(ARM64)
inline uint32_t readWord(uintptr_t address)
{
    uint32_t word;
    memcpySpan(asMutableByteSpan(word), unsafeMakeSpan(reinterpret_cast<const uint8_t*>(address), sizeof(word)));
    return word;
}

// The target of the branch at address on ARM64 (b, bl, b.cond, cbz, cbnz, tbz or tbnz), or nullopt for another
// instruction.
inline std::optional<uintptr_t> arm64BranchTarget(uintptr_t address)
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
inline std::optional<uintptr_t> arm64ConditionalTarget(uintptr_t label)
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
inline bool reaches(uintptr_t address, uintptr_t expected)
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

// What a fixup's footprint holds in linked code: the pointer for a Pointer, and the branch target for a Call or a Jump.
// Nullopt when the footprint does not hold its form's instruction.
inline std::optional<uintptr_t> decodeFixup(std::span<const uint8_t> code, const ImageFixup& fixup)
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

// Code a test linked into executable memory, and its bytes.
struct LinkedCode {
    MacroAssemblerCodeRef<JITStubRoutinePtrTag> codeRef;
    std::span<const uint8_t> bytes;
};

// A compilation of codeBlock under a twin recorder, the one recorder a compilation outside production can have, seeded
// for the assembler alone and charging budget. Null when no executable memory was left.
inline RefPtr<BaselineJITCode> compileUnderTwinRecorder(VM& vm, CodeBlock& codeBlock, Ref<ProducerBudget>&& budget)
{
    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs compileInputs { };
    Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(&codeBlock));
    JIT jit(vm, plan.get(), &codeBlock);
    jit.setJITCacheTwin(WTF::move(budget), seeds, compileInputs);
    return jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
}

inline RefPtr<BaselineJITCode> compileUnderTwinRecorder(VM& vm, CodeBlock& codeBlock)
{
    return compileUnderTwinRecorder(vm, codeBlock, ProducerBudget::createUnlimited());
}

// The JS function the global holds, or null with no failure recorded: each caller reports a missing body in its own words.
inline JSFunction* globalFunctionOrNull(JSGlobalObject& globalObject, ASCIILiteral name)
{
    return dynamicDowncast<JSFunction>(globalValue(&globalObject, name));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
