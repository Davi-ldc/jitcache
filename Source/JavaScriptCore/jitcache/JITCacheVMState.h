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
#include "JITCacheFaults.h"
#include "ProducerBudget.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "ValidatedBody.h"
#include <atomic>
#include <memory>
#include <optional>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
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
struct ImageTwinCheckState; // twins builds; defined in JITCacheInstall.cpp (harness sub-SPEC section 3)
struct KeptSummaries; // the capture glue's kept summaries (section 8.3); defined in JITCacheCapture.cpp

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
    // Task 7 (section 17) adds the constructor and the accessors later tasks use.

private:
    // The entry points of JITCacheFaults.h turn the switches off through the two members below (section 4.5).
    friend void didFailExecutableAllocation(VM&, ExecutableAllocationSite);
    friend void didAttachDebugger(VM&);

    // VM thread. Turns activity off, which ends production (section 4.2). The report becomes the first activity fault
    // when activity was on and the first production fault when production was active; a switch already off keeps the
    // fault that turned it off, or, when a debugger turned it off, the debugger flag. A production fault is therefore
    // never recorded after the activity fault, so the earlier of the two is the production fault whenever there is one.
    // Plain stores and atomic exchanges only: nothing allocates, frees, locks or waits.
    void turnActivityOff(FaultReport&&);
    // Any thread, atomic stores only: the debugger flag, then both switches off. status reads the flag in place of a
    // recorded activity fault.
    void noteDebuggerAttached();

    // The role, the config and the start outcome.
    const Config m_config;
    const StartOutcome m_startOutcome;

    // The switches (section 4.2). Each goes from on to off only; they are turned off with release order and read with
    // acquire order, since JIT workers read production through producerContext and a debugger may attach from any thread.
    // Production starts on only for a producing role whose start did not fault.
    std::atomic<bool> m_activityOn { false };
    std::atomic<bool> m_productionActive { false };
    std::atomic<bool> m_debuggerAttached { false };

    // The first fault of each switch and the progress counters, which the VM thread writes. A start fault is the first
    // activity fault. A refusal a JIT worker's charge made and the VM thread has not raised yet has no record here, and
    // status reports it from the budget as budget.limit (section 3.3). The progress counters are the integrator's;
    // status adds the UCB registry's statistics.
    std::optional<FaultReport> m_activityFault;
    std::optional<FaultReport> m_productionFault;
    Progress m_progress;

    // Producing roles: the producer budget and the one context producerContext returns (section 4.4).
    RefPtr<ProducerBudget> m_budget;
    std::optional<ProducerContext> m_producerContext;

    std::unique_ptr<BenchReport> m_benchReport; // start's step 3; flushed by the hosts' exit calls and willDestroyVM

#if ENABLE(JITCACHE_TWINS)
    std::unique_ptr<TwinReport> m_twinReport; // start's step 3; willDestroyVM closes it
    const Ref<ProducerBudget> m_twinBudget { ProducerBudget::createUnlimited() };
#endif

    // The UCB lane's registry, which the destructor destroys after every UCB and UFE destructor has run (section 4.6).
    UCBRegistry m_registry;

    std::unique_ptr<ProducerLock> m_producerLock; // producing roles; destroying it releases the lock
    RefPtr<OpenedArtifact> m_artifact; // shared by the VMs of the process that open the artifact; null after a start fault

#if ENABLE(JITCACHE_TWINS)
    // The body-lookup override (section 6.2): while set, bodyVersion answers from the first and openBody from the second.
    Function<uint64_t(const BodyKey&)> m_bodyVersionOverride;
    Function<BodyLookup(const BodyKey&)> m_openBodyOverride;
#endif

    // Producing roles. Declared after the producer lock, the opened artifact and the budget, so the destructor destroys
    // the writer, which refers to all three, first.
    std::unique_ptr<ArtifactWriter> m_writer;

    // Production memory (sections 4.2, 4.4 and 8.3): the kept summaries, which the capture glue creates with a deleter
    // of its own, so neither this header nor the destructor's file names the capture glue's types, and the charge for
    // the index entries the writer added. The end of production releases both.
    std::unique_ptr<KeptSummaries, void (*)(KeptSummaries*)> m_keptSummaries { nullptr, nullptr };
    size_t m_indexEntryChargeBytes { 0 };

#if ASSERT_ENABLED
    unsigned m_capturesInProgress { 0 }; // the finalize capture and delta assert that none is in progress (section 8.7)
#endif

#if ENABLE(JITCACHE_TWINS)
    // Harness sub-SPEC section 3: created with JITCacheInstall.cpp's deleter at the VM's first stash, destroyed by
    // willDestroyVM; an empty holder calls nothing.
    std::unique_ptr<ImageTwinCheckState, void (*)(ImageTwinCheckState*)> m_imageTwinCheckState { nullptr, nullptr };
#endif
};

} // namespace JSC::JITCache
