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

#include "JITCacheAPI.h"
#include "ProducerBudget.h"
#include "TwinReport.h"
#include "ValidatedBody.h"
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

// The per-VM state (SPEC-integrator.md section 4.1), reached through vm.jitCacheState(). start stores the pointer once;
// didFinalizeHeap stores null and then destroys the state (section 4.6). Every reader runs either on the VM thread,
// which wrote the pointer, or on a JIT worker through producerContext (section 4.4), whose acquire load sees a fully
// constructed state. The integrator types the state holds through pointers are only declared here, so a lane that
// includes this header compiles against none of their headers.

class ArtifactWriter;
class BenchReport;
class OpenedArtifact;
class ProducerLock;
class UCBRegistry;
struct ImageTwinCheckState; // twins builds; defined in JITCacheInstall.cpp (harness sub-SPEC section 3)

class VMState final {
    WTF_MAKE_NONCOPYABLE(VMState);
    WTF_MAKE_TZONE_ALLOCATED(VMState);
public:
    // The lanes' interface (UCB R-INT-1 to R-INT-3 and R-INT-10). VM thread unless noted.
    bool tracksKeys() const; // activity on (any thread)
    bool importsEnabled() const; // role Consumer or ConsumerProducer, and activity on (any thread)
    bool productionActive() const; // producing, activity on and production not ended (any thread)
    bool strict() const; // Config::strict (any thread)
    UCBRegistry& registry();
    uint64_t bodyVersion(const BodyKey&);
    BodyLookup openBody(const BodyKey&);
    void raiseInvalidMaterial(ASCIILiteral step, String detail); // step is a full name, such as "ucb.identity"
    void raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail);
    void raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail);
#if ENABLE(JITCACHE_TWINS)
    TwinReportSink* twinReportSink(); // non-null while a twin report is open
    Ref<ProducerBudget> twinBudget(); // ProducerBudget::createUnlimited(), for twin compiles
    // Tests (section 6.2): while set, bodyVersion and openBody answer from these alone; token answers index tokens.
    void setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open);
    void clearBodyLookupForTesting();
#endif

    // The integrator's own state.
    Role role() const;
    bool producing() const; // Producer or ConsumerProducer
    bool activityOn() const;
    ProducerContext* producerContextIfActive(); // any thread
    void releaseEndedProductionMemory(); // VM thread; nothing while production is active or once it is released (section 4.2)
    OpenedArtifact* artifact(); // null after a start fault
    BenchReport* benchReport(); // null unless Config::benchReportPath was set
    ~VMState(); // defined in JITCacheAPI.cpp
    // Task 7 (section 17) completes the private part.
};

} // namespace JSC::JITCache
