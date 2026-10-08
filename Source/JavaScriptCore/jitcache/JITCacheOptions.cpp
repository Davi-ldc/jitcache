/*
 * Copyright (C) 2026 The JITCache Authors. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "JITCacheOptions.h"

#include "Options.h"
#include <type_traits>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringCommon.h>
#include <wtf/text/WTFString.h>

// The tables of docs/JitCache/options.md for this pin. Each entry reads Options::name(), so a renamed or removed option
// fails the build, and a static assertion fails it when the option's type in OptionsList.h is not the one given here.

// The must-match table, in the order of the header's option index (container sub-SPEC section 3.1).
#define JITCACHE_FOR_EACH_MUST_MATCH_OPTION(v) \
    v(evalMode) \
    v(useExplicitResourceManagement) \
    v(useImportDefer)

// The fixed table, in its order, which start walks: v(name, Type, requiredValue).
#define JITCACHE_FOR_EACH_FIXED_OPTION(v) \
    v(useJIT, Bool, true) \
    v(useBaselineJIT, Bool, true) \
    v(useDFGJIT, Bool, true) \
    v(useBaselineJITCodeSharing, Bool, true) \
    v(useLOLJIT, Bool, false) \
    v(forceUnlinkedDFG, Bool, false) \
    v(forceDebuggerBytecodeGeneration, Bool, false) \
    v(debuggerTriggersBreakpointException, Bool, false) \
    v(alwaysUseShadowChicken, Bool, false) \
    v(useSamplingProfiler, Bool, false) \
    v(alwaysGeneratePCToCodeOriginMap, Bool, false) \
    v(switchJumpTableAmountThreshold, Unsigned, 15) \
    v(useTailCalls, Bool, true) \
    v(optimizeRecursiveTailCalls, Bool, true) \
    v(exposePrivateIdentifiers, Bool, false) \
    v(functionOverrides, OptionString, nullptr) \
    v(returnEarlyFromInfiniteLoopsForFuzzing, Bool, false) \
    v(thresholdForJITAfterWarmUp, Int32, 500) \
    v(thresholdForJITSoon, Int32, 100) \
    v(useExceptionFuzz, Bool, false) \
    v(traceBaselineJITExecution, Bool, false) \
    v(useProfiler, Bool, false) \
    v(useTypeProfiler, Bool, false) \
    v(useControlFlowProfiler, Bool, false) \
    v(forceGCSlowPaths, Bool, false) \
    v(useJITAsserts, Bool, ASSERT_ENABLED) \
    v(useJITDebugAssertions, Bool, ASSERT_ENABLED) \
    v(eagerlyUpdateTopCallFrame, Bool, false) \
    v(maximumInlineStringSwitchCaseCount, Unsigned, 64) \
    v(maximumOptimizationCandidateBytecodeCost, Unsigned, 100000) \
    v(executionCounterIncrementForEntry, Int32, 15) \
    v(executionCounterIncrementForLoop, Int32, 1) \
    v(thresholdForOptimizeAfterWarmUp, Int32, 1000) \
    v(thresholdForOptimizeAfterLongWarmUp, Int32, 1000) \
    v(quickDFGTierUpThresholdFactor, Double, 0.2) \
    v(evalThresholdMultiplier, Int32, 10) \
    v(reoptimizationRetryCounterMax, Unsigned, 21) \
    v(minimumOptimizationDelay, Unsigned, 1) \
    v(maximumOptimizationDelay, Unsigned, 5) \
    v(useArrayAllocationProfiling, Bool, true) \
    v(jitPolicyScale, Double, 1.0) \
    v(forceEagerCompilation, Bool, false) \
    v(repatchCountForCoolDown, Unsigned, 8) \
    v(initialCoolDownCount, Unsigned, 20) \
    v(useLLIntICs, Bool, true) \
    v(forceICFailure, Bool, false) \
    v(maxAccessVariantListSize, Unsigned, 8) \
    v(thresholdForUndesiredMegamorphicAccessVariantListSize, Double, 0.5) \
    v(maxPolymorphicCallVariantListSize, Unsigned, 8) \
    v(prototypeHitCountForLLIntCaching, Unsigned, 2)

namespace JSC::JITCache {

namespace OptionsInternal {

#define JITCACHE_ASSERT_MUST_MATCH_OPTION_TYPE(name_) \
    static_assert(std::is_same_v<decltype(Options::name_()), OptionsStorage::Bool&>, "the header records " #name_ " as a Bool");
JITCACHE_FOR_EACH_MUST_MATCH_OPTION(JITCACHE_ASSERT_MUST_MATCH_OPTION_TYPE)
#undef JITCACHE_ASSERT_MUST_MATCH_OPTION_TYPE

#define JITCACHE_ASSERT_FIXED_OPTION_TYPE(name_, type_, requiredValue_) \
    static_assert(std::is_same_v<decltype(Options::name_()), OptionsStorage::type_&>, "options.md gives " #name_ " the type " #type_);
JITCACHE_FOR_EACH_FIXED_OPTION(JITCACHE_ASSERT_FIXED_OPTION_TYPE)
#undef JITCACHE_ASSERT_FIXED_OPTION_TYPE

#define JITCACHE_MUST_MATCH_OPTION_NAME(name_) #name_ ""_s,
static constexpr std::array mustMatchOptionNames { JITCACHE_FOR_EACH_MUST_MATCH_OPTION(JITCACHE_MUST_MATCH_OPTION_NAME) };
#undef JITCACHE_MUST_MATCH_OPTION_NAME
static_assert(mustMatchOptionNames.size() == numberOfMustMatchOptions);

#define JITCACHE_FIXED_OPTION_ROW(name_, type_, requiredValue_) \
    FixedOptionRow { #name_ ""_s, \
        [] { return FixedOptionValue { WTF::InPlaceType<OptionsStorage::type_>, Options::name_() }; }, \
        FixedOptionValue { WTF::InPlaceType<OptionsStorage::type_>, static_cast<OptionsStorage::type_>(requiredValue_) } },
static constexpr FixedOptionRow fixedOptionTable[] = { JITCACHE_FOR_EACH_FIXED_OPTION(JITCACHE_FIXED_OPTION_ROW) };
#undef JITCACHE_FIXED_OPTION_ROW

} // namespace OptionsInternal

ASCIILiteral mustMatchOptionName(unsigned index)
{
    RELEASE_ASSERT(index < numberOfMustMatchOptions);
    return OptionsInternal::mustMatchOptionNames[index];
}

std::array<bool, numberOfMustMatchOptions> mustMatchOptionValues()
{
#define JITCACHE_MUST_MATCH_OPTION_VALUE(name_) Options::name_(),
    return { { JITCACHE_FOR_EACH_MUST_MATCH_OPTION(JITCACHE_MUST_MATCH_OPTION_VALUE) } };
#undef JITCACHE_MUST_MATCH_OPTION_VALUE
}

bool FixedOptionRow::holds() const
{
    FixedOptionValue effective = effectiveValue();
    auto* requiredString = std::get_if<const char*>(&requiredValue);
    auto* effectiveString = std::get_if<const char*>(&effective);
    if (requiredString && effectiveString) {
        if (!*requiredString || !*effectiveString)
            return *requiredString == *effectiveString;
        return equalSpans(unsafeSpan(*requiredString), unsafeSpan(*effectiveString));
    }
    // Both sides of a row have the option's type, and variant equality compares their values with ==.
    return effective == requiredValue;
}

std::span<const FixedOptionRow> fixedOptionRows()
{
    return std::span { OptionsInternal::fixedOptionTable };
}

const FixedOptionRow* checkFixedOptions()
{
    for (auto& row : fixedOptionRows()) {
        if (!row.holds())
            return &row;
    }
    return nullptr;
}

String describeFixedOptionValue(const FixedOptionValue& value)
{
    return WTF::switchOn(value,
        [](bool boolean) -> String {
            return boolean ? "true"_s : "false"_s;
        },
        [](unsigned number) -> String {
            return String::number(number);
        },
        [](int32_t number) -> String {
            return String::number(number);
        },
        [](double number) -> String {
            return String::number(number);
        },
        [](const char* characters) -> String {
            if (!characters)
                return "null"_s;
            return makeString('"', String::fromLatin1(characters), '"');
        });
}

String describeFixedOptionMismatch(const FixedOptionRow& row)
{
    return makeString(row.name, ": required "_s, describeFixedOptionValue(row.requiredValue), ", effective "_s, describeFixedOptionValue(row.effectiveValue()));
}

} // namespace JSC::JITCache

#undef JITCACHE_FOR_EACH_FIXED_OPTION
#undef JITCACHE_FOR_EACH_MUST_MATCH_OPTION
