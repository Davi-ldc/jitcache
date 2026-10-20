#pragma once

#include "JITCacheBun.h"
#include <expected>
#include <span>
#include <stdint.h>
#include <wtf/Platform.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSModuleRecord;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// A module's analysis: the requested modules, the import and export entries, the star exports and the import.meta and
// top-level-await flags of the record the loader links, carried as the ucb.analysis section in Bun's self-contained
// module-info form, which Bun reads back into a record as it does for an ESM bytecode build. JITCacheBun.h declares
// importModuleAnalysis, Bun's entry, which UCBAnalysis.cpp defines.

class ProducerBudget;

enum class AnalysisEncodeFailure : uint8_t { Unrepresentable, BudgetRefused };

// A module's analysis in Bun's self-contained module-info form, from a record that JSC's analysis parse built. The bytes
// stay charged to the budget; a failure leaves nothing charged.
std::expected<Vector<uint8_t>, AnalysisEncodeFailure> encodeModuleAnalysis(VM&, const JSModuleRecord&, ProducerBudget&);

// Whether the bytes are one whole analysis section that the analysis parse can produce; when they are not, failure says
// in words the first property they lack.
bool validateModuleAnalysis(std::span<const uint8_t>, String& failure);

#if ENABLE(JITCACHE_TWINS)
// The first field in which a record built from saved bytes differs from the one the analysis parse builds, comparing
// the requested modules, the import entries, the export entries, the star exports and then the flags, each container
// in its order; null when none does. Feature bits other than import.meta are not compared, since nothing reads them.
String firstModuleAnalysisDifference(const JSModuleRecord& fromArtifact, const JSModuleRecord& fromParse);
#endif

} // namespace JSC::JITCache
