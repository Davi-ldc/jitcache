#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "UCBKeys.h"

namespace JSC {

class SourceCode;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// Twin comparison (SPEC-ucb.md section 13.2). The engine calls these itself, after the step that succeeded, whenever
// VMState::twinReportSink() is non-null (R-INT-10). Each reports every difference to the sink and returns.

class TwinReport;
class ValidatedBody;
struct RequestState;
using TwinReportSink = TwinReport; // as TwinReport.h names it (SPEC-integrator.harness.md section 2)

// T1 to T6, against a twin generated in the consumer from the same request with the helpers of UCBTwinGeneration.h.
void verifyImport(const RequestState&, UnlinkedCodeBlock& imported, const ValidatedBody&, TwinReportSink&);
// A seeded decoded UCB, or an attach; holder as for coreDigestOf (section 4.5).
void verifyMatched(UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder, bool seeded, const ValidatedBody&, TwinReportSink&);
// T7: a root digest taken from builtin metadata or a provider equals the digest of its text.
void verifySuppliedDigest(const SourceCode& rootSource, const RootSourceDigest&, TwinReportSink&);
unsigned verifyRegistry(VM&, TwinReportSink*); // the number of violations; each is also reported when the sink is non-null

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
