#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "ArrayAllocationProfile.h"
#include "ArrayProfile.h"
#include "JITCacheTest.h"
#include "JSCJSValueInlines.h"
#include "LazyValueProfile.h"
#include <array>
#include <span>
#include <type_traits>
#include <wtf/CompactPointerTuple.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// U6 of SPEC-cb.md section 11.2: the native accessors E1 to E3 (section 9.1) read and write exactly the fields they
// name, and forEachOperandValueProfile leaves every bucket as it found it (I2). The profiles are built standalone,
// at the state linking leaves them in a CodeBlock's zeroed metadata (N1), so no test needs a VM.

namespace JSC::JITCache::Tests {

namespace CBAccessorTestsInternal {

// An ArrayProfile is four 32-bit fields: the two StructureID samples, the flags and the modes. The tests compare its
// whole object representation, so a write outside the named fields shows up as a changed byte.
static_assert(sizeof(ArrayModes) == sizeof(uint32_t));
static_assert(sizeof(OptionSet<ArrayProfileFlag>) == sizeof(uint32_t));
using ArrayProfileImage = std::array<uint8_t, sizeof(ArrayProfile)>;

static ArrayProfileImage imageOf(const ArrayProfile& profile)
{
    ArrayProfileImage image;
    memcpySpan(std::span { image }, asByteSpan(profile));
    return image;
}

static uint32_t loadField(const ArrayProfileImage& image, ptrdiff_t offset)
{
    uint32_t value { 0 };
    memcpySpan(asMutableByteSpan(value), std::span { image }.subspan(static_cast<size_t>(offset), sizeof(value)));
    return value;
}

static void storeField(ArrayProfileImage& image, ptrdiff_t offset, uint32_t value)
{
    memcpySpan(std::span { image }.subspan(static_cast<size_t>(offset), sizeof(value)), asByteSpan(value));
}

static bool isZero(const ArrayProfileImage& image)
{
    return image == ArrayProfileImage { };
}

// Every flag V7 admits, the pruning mark included: a CB copy carries the mark, unlike the UCB copy.
static constexpr OptionSet<ArrayProfileFlag> everyArrayProfileFlag {
    ArrayProfileFlag::MayStoreHole,
    ArrayProfileFlag::OutOfBounds,
    ArrayProfileFlag::MayBeLargeTypedArray,
    ArrayProfileFlag::MayInterceptIndexedAccesses,
    ArrayProfileFlag::UsesNonOriginalArrayStructures,
    ArrayProfileFlag::MayBeResizableOrGrowableSharedTypedArray,
    ArrayProfileFlag::DidPerformFirstRunPruning,
    ArrayProfileFlag::MayBeRegExpMatchesArray,
};
static_assert(everyArrayProfileFlag.toRaw() == 0xff);

struct ArrayProfileRestoreCase {
    ArrayModes modes;
    OptionSet<ArrayProfileFlag> flags;
};

// An ArrayAllocationProfile is one CompactPointerTuple<JSArray*, uint16_t>: the pointer half is the last array and the
// type half is the hint, (IndexingType << 8) | vectorLength, the encoding of cb.state's array 5 (section 3.2). Its whole
// object representation equals a tuple with a null pointer and the given hint exactly when the profile holds that hint
// and no last array; a change to that layout fails this comparison, which a WebKit bump then has to review.
static uint16_t hintBits(IndexingType indexingType, unsigned vectorLength)
{
    return static_cast<uint16_t>((static_cast<unsigned>(indexingType) << 8) | vectorLength);
}

static bool holdsHintWithoutLastArray(const ArrayAllocationProfile& profile, uint16_t bits)
{
    using Storage = CompactPointerTuple<JSArray*, uint16_t>;
    static_assert(sizeof(ArrayAllocationProfile) == sizeof(Storage));
    Storage expected(nullptr, bits);
    return equalSpans(asByteSpan(profile), asByteSpan(expected));
}

struct HintRestoreCase {
    IndexingType linkedIndexingType; // what linking leaves: ArrayWithUndecided, or the literal's type in F19 (N1)
    IndexingType indexingType;
    unsigned vectorLength;
};

} // namespace CBAccessorTestsInternal

using namespace CBAccessorTestsInternal;

// E1, read: arrayProfileFlags() returns the field native writers set, at the offset the JIT and the ICs write through,
// on a const profile.
JITCACHE_TEST(cbAccessorArrayProfileFlagsReadsItsField, No)
{
    ArrayProfile profile;
    JITCACHE_CHECK(isZero(imageOf(profile)));

    profile.setOutOfBounds();
    profile.setMayStoreHole();
    profile.setMayBeLargeTypedArray();
    profile.observeArrayMode(asArrayModesIgnoringTypedArrays(ArrayWithInt32));

    const ArrayProfile& constProfile = profile;
    OptionSet<ArrayProfileFlag> expected { ArrayProfileFlag::OutOfBounds, ArrayProfileFlag::MayStoreHole, ArrayProfileFlag::MayBeLargeTypedArray };
    JITCACHE_CHECK(constProfile.arrayProfileFlags() == expected);
    JITCACHE_CHECK(loadField(imageOf(profile), ArrayProfile::offsetOfArrayProfileFlags()) == constProfile.arrayProfileFlags().toRaw());
    JITCACHE_CHECK(constProfile.observedArrayModes() == asArrayModesIgnoringTypedArrays(ArrayWithInt32));
}

// E1, write: restoreAccumulatedState on a profile at its link state writes the modes and the flags, the pruning mark
// included, and leaves the two StructureID samples empty; every other byte keeps its link-state value.
JITCACHE_TEST(cbAccessorArrayProfileRestoreWritesModesAndFlags, No)
{
    const std::array cases {
        ArrayProfileRestoreCase { 0, { } },
        ArrayProfileRestoreCase {
            asArrayModesIgnoringTypedArrays(ArrayWithInt32) | asArrayModesIgnoringTypedArrays(NonArrayWithDouble) | Float64ArrayMode | CopyOnWriteArrayWithContiguousArrayMode,
            { ArrayProfileFlag::OutOfBounds, ArrayProfileFlag::DidPerformFirstRunPruning },
        },
        ArrayProfileRestoreCase { ALL_ARRAY_MODES, everyArrayProfileFlag },
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& restoreCase = cases[i];
        ArrayProfile profile;
        ArrayProfileImage before = imageOf(profile);
        JITCACHE_CHECK(isZero(before));

        profile.restoreAccumulatedState(restoreCase.modes, restoreCase.flags);

        ArrayProfileImage expected = before;
        storeField(expected, ArrayProfile::offsetOfArrayModes(), restoreCase.modes);
        storeField(expected, ArrayProfile::offsetOfArrayProfileFlags(), restoreCase.flags.toRaw());
        if (imageOf(profile) != expected)
            JITCACHE_FAIL(makeString("restoreAccumulatedState case "_s, i, " wrote outside the modes and flags, or wrote them wrong"_s));
        if (profile.observedArrayModes() != restoreCase.modes || profile.arrayProfileFlags() != restoreCase.flags)
            JITCACHE_FAIL(makeString("restoreAccumulatedState case "_s, i, " does not read back through the accessors"_s));
    }
}

// E2: restoreHint writes the hint, indexing type and vector length, and leaves the last array empty, both at the
// generic sites' link state (F16 to F18) and at a new_array_buffer site's copy-on-write one (F19).
JITCACHE_TEST(cbAccessorAllocationProfileRestoreHintWritesTheHint, No)
{
    const std::array cases {
        HintRestoreCase { ArrayWithUndecided, ArrayWithUndecided, 0 },
        HintRestoreCase { ArrayWithUndecided, ArrayWithInt32, 1 },
        HintRestoreCase { ArrayWithUndecided, ArrayWithContiguous, 7 },
        HintRestoreCase { ArrayWithUndecided, ArrayWithDouble, BASE_CONTIGUOUS_VECTOR_LEN_MAX },
        HintRestoreCase { ArrayWithUndecided, ArrayWithArrayStorage, 0 },
        HintRestoreCase { ArrayWithUndecided, ArrayWithSlowPutArrayStorage, 3 },
        HintRestoreCase { CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithInt32, 0 },
        HintRestoreCase { CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithDouble, 2 },
        HintRestoreCase { CopyOnWriteArrayWithInt32, CopyOnWriteArrayWithContiguous, BASE_CONTIGUOUS_VECTOR_LEN_MAX },
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& restoreCase = cases[i];
        ArrayAllocationProfile profile(restoreCase.linkedIndexingType);
        if (!holdsHintWithoutLastArray(profile, hintBits(restoreCase.linkedIndexingType, 0))) {
            JITCACHE_FAIL(makeString("allocation profile case "_s, i, ": the linked profile does not have the layout this test reads"_s));
            continue;
        }

        profile.restoreHint(restoreCase.indexingType, restoreCase.vectorLength);

        if (!holdsHintWithoutLastArray(profile, hintBits(restoreCase.indexingType, restoreCase.vectorLength)))
            JITCACHE_FAIL(makeString("restoreHint case "_s, i, " wrote outside the hint, or wrote it wrong"_s));
        if (profile.selectIndexingTypeConcurrently() != restoreCase.indexingType || profile.vectorLengthHintConcurrently() != restoreCase.vectorLength)
            JITCACHE_FAIL(makeString("restoreHint case "_s, i, " does not read back through the compilers' readers"_s));
    }
}

#if ENABLE(DFG_JIT)

// E3: forEachOperandValueProfile visits nothing in a holder that has no data, and otherwise each operand profile once,
// in the holder's order, as a const reference; it leaves each profile's bucket and prediction, and the standalone
// failure buckets, as they were, where any drain would have folded the pending samples into the predictions (I2).
JITCACHE_TEST(cbAccessorLazyOperandWalkerLeavesBuckets, No)
{
    CompressedLazyValueProfileHolder holder;
    unsigned visitsOfEmptyHolder = 0;
    holder.forEachOperandValueProfile([&](auto&) {
        ++visitsOfEmptyHolder;
    });
    JITCACHE_CHECK(!visitsOfEmptyHolder);

    const std::array keys {
        LazyOperandValueProfileKey(BytecodeIndex(4), Operand(virtualRegisterForArgumentIncludingThis(1))),
        LazyOperandValueProfileKey(BytecodeIndex(9, 2), Operand(virtualRegisterForLocal(3))),
        LazyOperandValueProfileKey(BytecodeIndex(17), Operand::tmp(1)),
    };
    // The first profile holds a pending int32 sample its prediction lacks, so a drain would change both.
    const std::array<SpeculatedType, 3> predictions { SpecString | SpecOther, SpecInt32Only, SpecNone };
    std::array<LazyOperandValueProfile*, 3> profiles { };
    for (size_t i = 0; i < keys.size(); ++i) {
        profiles[i] = holder.addOperandValueProfile(keys[i]);
        profiles[i]->m_prediction = predictions[i];
    }
    EncodedJSValue pendingSample = JSValue::encode(jsNumber(42));
    *profiles[0]->specFailBucket(0) = pendingSample;
    JSValue* failureBucket = holder.addSpeculationFailureValueProfile(BytecodeIndex(4));
    *failureBucket = jsNumber(7);

    Vector<const LazyOperandValueProfile*> visited;
    Vector<LazyOperandValueProfileKey> visitedKeys;
    Vector<SpeculatedType> visitedPredictions;
    holder.forEachOperandValueProfile([&](auto& profile) {
        static_assert(std::is_same_v<decltype(profile), const LazyOperandValueProfile&>);
        visited.append(&profile);
        visitedKeys.append(profile.key());
        visitedPredictions.append(profile.m_prediction);
    });

    JITCACHE_CHECK(visited.size() == keys.size());
    if (visited.size() != keys.size())
        return;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (visited[i] != profiles[i] || !(visitedKeys[i] == keys[i]) || visitedPredictions[i] != predictions[i])
            JITCACHE_FAIL(makeString("operand profile "_s, i, " was not visited in the holder's order with its key and prediction"_s));
        if (profiles[i]->m_prediction != predictions[i])
            JITCACHE_FAIL(makeString("the walk changed the prediction of operand profile "_s, i));
    }

    JITCACHE_CHECK(*profiles[0]->specFailBucket(0) == pendingSample);
    JITCACHE_CHECK(!profiles[1]->numberOfSamples());
    JITCACHE_CHECK(!profiles[2]->numberOfSamples());

    auto failureBuckets = holder.speculationFailureValueProfileBucketsMap();
    JITCACHE_CHECK(failureBuckets.size() == 1);
    JITCACHE_CHECK(failureBuckets.get(BytecodeIndex(4)) == failureBucket);
    JITCACHE_CHECK(*failureBucket == jsNumber(7));
}

#endif // ENABLE(DFG_JIT)

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
