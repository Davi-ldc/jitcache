#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "CCallHelpers.h"
#include "LinkBuffer.h"
#include "UnlinkedCodeBlock.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/image/ImageRecorder.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <optional>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// What the tests of emission share: whether two assemblers emitted the same code, which shows that a helper without a
// recorder emits exactly its native sequence, and the code a sequence emits under a fresh recorder, linked, with the
// fixups the recorder recorded.

namespace JSC::JITCache::Tests {

#if CPU(X86_64)
inline std::optional<LinkedCode> linkForTest(CCallHelpers& jit)
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
inline bool sameLinkedCode(std::span<const uint8_t> a, std::span<const uint8_t> b)
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
inline bool sameEmission(CCallHelpers& a, CCallHelpers& b)
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

// helper, without a recorder, emits exactly what native does, both starting from seed.
template<typename Native, typename Helper>
void checkNativeEmission(TestContext& context, ASCIILiteral name, const Native& native, const Helper& helper, uint32_t seed = assemblerSeed)
{
    CCallHelpers nativeJIT;
    CCallHelpers helperJIT;
    nativeJIT.seedRandomForTwins(seed);
    helperJIT.seedRandomForTwins(seed);
    native(nativeJIT);
    helper(helperJIT);
    if (!sameEmission(nativeJIT, helperJIT))
        JITCACHE_FAIL(makeString(name, " differs from the native sequence"_s));
}

struct RecordedCode {
    LinkedCode linked;
    Vector<ImageFixup> fixups; // In footprint order, sites relative to the code.
};

// The code emit produces under a fresh recorder, followed by a ret, linked, with the fixups the recorder recorded. Reports
// and returns nullopt when the recorder stopped recording or no executable memory was left.
template<typename Emit>
std::optional<RecordedCode> recordCode(TestContext& context, VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, ASCIILiteral name, const Emit& emit)
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

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
