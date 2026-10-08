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
