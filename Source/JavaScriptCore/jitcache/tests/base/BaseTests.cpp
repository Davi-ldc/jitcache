#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "CodeSpecializationKind.h"
#include "ParserModes.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/base/JITCacheSHA256.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <span>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// The tests of base/: SHA-256 on every path the CPU offers, through runSHA256SelfTest, which JITCacheSHA256.cpp defines
// beside the paths it keeps private, the 40 canonical bytes of a BodyKey and their hex form, and ProducerBudget's
// charges, on one thread and on eight.

namespace JSC::JITCache::Tests {

namespace BaseTestsInternal {

constexpr size_t maximumSize = std::numeric_limits<size_t>::max();

} // namespace BaseTestsInternal

using namespace BaseTestsInternal;

// The FIPS 180-4 examples on every path and through the SHA256 class, whole and in updates that split them across blocks,
// and random messages of up to four blocks compared with the portable path.
JITCACHE_TEST(baseSHA256AgreesOnEveryPath, No)
{
    String failure;
    if (!runSHA256SelfTest(failure))
        JITCACHE_FAIL(failure);
}

// BodyKey::make lays out the 40 canonical bytes, fromBytes reads them back, and fromBytes rejects each field out of range.
JITCACHE_TEST(baseBodyKeyBytes, No)
{
    Digest256 identityDigest;
    identityDigest.fill(0x42);
    BodyKey key = BodyKey::make(IdentityKind::Child, CodeSpecializationKind::CodeForConstruct, CodeGenerationMode::Debugger, identityDigest);
    std::array<uint8_t, BodyKey::byteSize> bytes;
    memcpySpan(std::span { bytes }, key.bytes());
    bool laidOut = bytes[0] == 1 && bytes[1] == static_cast<uint8_t>(IdentityKind::Child) && bytes[2] == 1 && bytes[3] == OptionSet<CodeGenerationMode> { CodeGenerationMode::Debugger }.toRaw()
        && !bytes[4] && !bytes[5] && !bytes[6] && !bytes[7] && key.identityDigest() == identityDigest;
    if (!laidOut)
        JITCACHE_FAIL("BodyKey::make does not lay out the 40 canonical bytes"_s);
    auto parse = [](const std::array<uint8_t, BodyKey::byteSize>& candidate) {
        return BodyKey::fromBytes(std::span<const uint8_t, BodyKey::byteSize> { candidate });
    };
    if (parse(bytes) != key)
        JITCACHE_FAIL("BodyKey::fromBytes does not read back a key's bytes"_s);
    struct Mutation {
        size_t offset;
        uint8_t value;
        ASCIILiteral what;
    };
    static constexpr std::array<Mutation, 11> mutations { {
        { 0, 0, "version 0"_s }, { 0, 2, "version 2"_s }, { 1, 0, "identity kind 0"_s }, { 1, 8, "identity kind 8"_s }, { 2, 2, "specialization 2"_s },
        { 3, 0x08, "a mode bit outside CodeGenerationMode"_s }, { 3, 0x80, "the top mode bit"_s },
        { 4, 1, "reserved byte 4"_s }, { 5, 1, "reserved byte 5"_s }, { 6, 1, "reserved byte 6"_s }, { 7, 1, "reserved byte 7"_s },
    } };
    for (const Mutation& mutation : mutations) {
        auto crafted = bytes;
        crafted[mutation.offset] = mutation.value;
        if (parse(crafted))
            JITCACHE_FAIL(makeString("BodyKey::fromBytes accepts "_s, mutation.what));
    }
}

// bodyKeyFromHex reads back the digits bodyKeyHexDigits writes, and rejects a character that is no hex digit.
JITCACHE_TEST(baseBodyKeyHex, No)
{
    Digest256 identityDigest;
    for (size_t index = 0; index < identityDigest.size(); ++index)
        identityDigest[index] = static_cast<uint8_t>(29 * index + 7);
    BodyKey key = BodyKey::make(IdentityKind::Program, CodeSpecializationKind::CodeForCall, { }, identityDigest);
    auto digits = bodyKeyHexDigits(key);
    JITCACHE_CHECK(bodyKeyFromHex(digits) == key);
    digits[0] = 'G';
    JITCACHE_CHECK(!bodyKeyFromHex(digits));
}

// On one thread: a charge up to the limit succeeds, one past it is refused without changing the total, the refusal is
// sticky, and the peak is the largest total.
JITCACHE_TEST(integratorBudgetLimitAndPeak, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(100);
    JITCACHE_CHECK(budget->limitBytes() == 100);
    JITCACHE_CHECK(budget->tryCharge(30));
    JITCACHE_CHECK(budget->tryCharge(50));
    JITCACHE_CHECK(budget->chargedBytes() == 80);
    budget->release(50);
    JITCACHE_CHECK(budget->chargedBytes() == 30);
    JITCACHE_CHECK(budget->peakBytes() == 80);
    JITCACHE_CHECK(budget->tryCharge(70));
    JITCACHE_CHECK(budget->chargedBytes() == 100);
    JITCACHE_CHECK(budget->peakBytes() == 100);
    JITCACHE_CHECK(!budget->hasRefused());

    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 100);

    budget->release(100);
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->peakBytes() == 100);
}

// A charge whose sum overflows is refused, even under a limit no sum can exceed.
JITCACHE_TEST(integratorBudgetRefusesOverflow, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(maximumSize);
    JITCACHE_CHECK(budget->tryCharge(16));
    JITCACHE_CHECK(!budget->tryCharge(maximumSize));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 16);
    budget->release(16);
    JITCACHE_CHECK(!budget->chargedBytes());

    Ref<ProducerBudget> ended = ProducerBudget::create(maximumSize);
    ended->refuseFurtherCharges();
    JITCACHE_CHECK(ended->hasRefused());
    JITCACHE_CHECK(!ended->tryCharge(1));
}

// createUnlimited never refuses, whatever is charged and whether or not production ends.
JITCACHE_TEST(integratorBudgetUnlimited, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->chargedBytes() == maximumSize / 2 * 2);
    budget->release(maximumSize / 2);
    budget->release(maximumSize / 2);
    JITCACHE_CHECK(!budget->chargedBytes());
    budget->refuseFurtherCharges();
    JITCACHE_CHECK(budget->tryCharge(1));
    JITCACHE_CHECK(!budget->hasRefused());
    budget->release(1);

    Ref<ProducerBudget> saturated = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(!saturated->hasRefused());
}

// Eight threads charge and release concurrently, each charging more than it releases, until the limit refuses. The total
// never exceeds the limit, the refusal is sticky on every thread, the peak is the largest total, and the balance returns
// to zero once every thread has released what it holds.
JITCACHE_TEST(integratorBudgetConcurrentCharges, No)
{
    constexpr size_t limit = 1 * MB;
    constexpr size_t largestCharge = 4096;
    constexpr unsigned numberOfThreads = 8;
    constexpr unsigned maximumIterations = 1000000;
    Ref<ProducerBudget> budget = ProducerBudget::create(limit);

    std::atomic<bool> exceededLimit { false };
    std::atomic<bool> refusalWasNotSticky { false };
    std::atomic<bool> neverRefused { false };
    std::atomic<size_t> largestSampledTotal { 0 };

    Vector<Ref<Thread>> threads;
    for (unsigned t = 0; t < numberOfThreads; ++t) {
        threads.append(Thread::create("JITCache budget test"_s, [&, t] {
            Vector<size_t> held;
            uint32_t random = 0x9e3779b9u * (t + 1);
            bool refused = false;
            for (unsigned iteration = 0; iteration < maximumIterations; ++iteration) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                size_t bytes = 1 + random % largestCharge;
                if (!budget->tryCharge(bytes)) {
                    refused = true;
                    if (budget->tryCharge(1)) {
                        refusalWasNotSticky = true;
                        budget->release(1);
                    }
                    break;
                }
                held.append(bytes);
                size_t total = budget->chargedBytes();
                if (total > limit)
                    exceededLimit = true;
                size_t largest = largestSampledTotal.load();
                while (largest < total && !largestSampledTotal.compare_exchange_weak(largest, total)) { }
                // One release per three charges keeps the total climbing toward the limit.
                if (iteration % 3 == 2)
                    budget->release(held.takeLast());
            }
            if (!refused)
                neverRefused = true;
            for (size_t bytes : held)
                budget->release(bytes);
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();

    JITCACHE_CHECK(!neverRefused.load());
    JITCACHE_CHECK(!exceededLimit.load());
    JITCACHE_CHECK(!refusalWasNotSticky.load());
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(budget->peakBytes() <= limit);
    JITCACHE_CHECK(budget->peakBytes() >= largestSampledTotal.load());
    // The first refusal met a total within one charge of the limit, and some charge reached that total.
    JITCACHE_CHECK(budget->peakBytes() > limit - largestCharge);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
