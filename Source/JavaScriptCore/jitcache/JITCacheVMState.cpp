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
#include "JITCacheVMState.h"

#include "ArtifactStore.h"
#include "JITCacheBench.h"
#include "JITCacheContainer.h"
#include <wtf/SafeStrerror.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

// The VMState members that neither create nor destroy a state nor free production memory (SPEC-integrator.md section
// 17): the switches, the fault records and the members that raise faults, the producer context, the body lookups with
// their test override, and what the glue, the hosts and status read and write on the state's parts. JITCacheAPI.cpp
// defines the constructor, the destructor and releaseEndedProductionMemory beside start and the teardown hooks
// (sections 4.1, 4.2 and 4.6), since destroying a state or its production memory runs the destructors of every part.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMState);

namespace JITCacheVMStateInternal {

// The detail of invalid material an open raises: the errno's text for container.io, and the body's key either way.
static String invalidBodyDetail(const BodyKey& key, const StoreFailure& failure)
{
    if (failure.error)
        return makeString(String::fromUTF8(safeStrerror(failure.error).data()), "; body "_s, bodyKeyHex(key));
    return makeString("body "_s, bodyKeyHex(key));
}

} // namespace JITCacheVMStateInternal

bool VMState::tracksKeys() const
{
    return activityOn();
}

bool VMState::importsEnabled() const
{
    return (role() == Role::Consumer || role() == Role::ConsumerProducer) && activityOn();
}

bool VMState::productionActive() const
{
    // Production is off whenever activity is (II2). Both switches are read, so a reader on another thread that sees a
    // debugger's attach on one of them sees production ended.
    return producing() && m_productionActive.load(std::memory_order_acquire) && activityOn();
}

bool VMState::strict() const
{
    return m_config.strict;
}

UCBRegistry& VMState::registry()
{
    return m_registry;
}

uint64_t VMState::bodyVersion(const BodyKey& key)
{
    if (!activityOn())
        return 0;
#if ENABLE(JITCACHE_TWINS)
    if (m_bodyVersionOverride)
        return m_bodyVersionOverride(key);
#endif
    // Activity is on only after a start that did not fault, which leaves the opened artifact.
    OpenedArtifact* artifact = m_artifact.get();
    ASSERT(artifact);

    // The index's token, without a system call unless the epoch moved (container sub-SPEC section 7.1).
    BenchReport* report = m_benchReport.get();
    uint64_t startNanoseconds = report ? benchThreadCPUNanoseconds() : 0;
    uint64_t token = artifact->token(key);
    if (report)
        report->countLookup(token ? BenchReport::Lookup::BodyVersionPresent : BenchReport::Lookup::BodyVersionAbsent, benchThreadCPUNanoseconds() - startNanoseconds);
    return token;
}

BodyLookup VMState::openBody(const BodyKey& key)
{
    if (!activityOn())
        return BodyLookup::unusable();
#if ENABLE(JITCACHE_TWINS)
    if (m_openBodyOverride)
        return m_openBodyOverride(key);
#endif
    OpenedArtifact* artifact = m_artifact.get();
    ASSERT(artifact);

    BenchReport* report = m_benchReport.get();
    uint64_t startNanoseconds = report ? benchThreadCPUNanoseconds() : 0;
    // Full validation exactly when strict is on; both modes run the integrity checks (section 4.3).
    BodyOpen opened = artifact->open(key, strict() ? ValidationMode::Full : ValidationMode::Integrity);
    if (opened.mappedBytes)
        ++m_progress.bodyOpens;

    BenchReport::Lookup tally = BenchReport::Lookup::OpenBodyMissing;
    BodyLookup lookup = [&] {
        switch (opened.outcome) {
        case StoreOutcome::Found:
            tally = BenchReport::Lookup::OpenBodyFound;
            return BodyLookup::found(opened.body.releaseNonNull());
        case StoreOutcome::Absent:
            // A file that vanished included; the store took the key out of its index unless its token changed meanwhile.
            return BodyLookup::missing();
        case StoreOutcome::Unavailable:
            // EMFILE, ENFILE or ENOMEM: the artifact is not at fault, and the body is as good as none for now.
            ++m_progress.transientOpenFailures;
            return BodyLookup::missing();
        case StoreOutcome::Invalid:
            // The UCB lane reads Unusable as "the integrator raised the fault" (SPEC-ucb.md section 7.3.1).
            raiseInvalidMaterial(opened.failure.check, JITCacheVMStateInternal::invalidBodyDetail(key, opened.failure));
            tally = BenchReport::Lookup::OpenBodyUnusable;
            return BodyLookup::unusable();
        }
        RELEASE_ASSERT_NOT_REACHED();
        return BodyLookup::unusable();
    }();

    if (report) {
        uint64_t nanoseconds = benchThreadCPUNanoseconds() - startNanoseconds;
        report->countLookup(tally, nanoseconds);
        // Harness sub-SPEC section 9.2: each openBody that maps a file.
        if (opened.mappedBytes)
            report->record("open"_s, { { "key"_s, bodyKeyHex(key) }, { "bytes"_s, opened.mappedBytes }, { "nanoseconds"_s, nanoseconds } });
    }
    return lookup;
}

void VMState::raiseInvalidMaterial(ASCIILiteral step, String detail)
{
    turnActivityOff(FaultReport { FaultClass::InvalidMaterial, { }, step, WTF::move(detail) });
}

void VMState::raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail)
{
    turnActivityOff(FaultReport { FaultClass::InvalidMaterial, part, check, WTF::move(detail) });
}

void VMState::raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail)
{
    // A recording fault ends production for good and leaves activity on, so a ConsumerProducer goes on importing. Its
    // budget refuses every later charge, so recorders still running on JIT workers fail their next charge and leave
    // their records incomplete (section 4.5).
    if (m_productionActive.exchange(false, std::memory_order_acq_rel) && !m_productionFault)
        m_productionFault = FaultReport { FaultClass::RecordingFault, part, check, WTF::move(detail) };
    if (m_budget)
        m_budget->refuseFurtherCharges();
}

#if ENABLE(JITCACHE_TWINS)
TwinReportSink* VMState::twinReportSink()
{
    return m_twinReport.get();
}

Ref<ProducerBudget> VMState::twinBudget()
{
    return m_twinBudget.copyRef();
}

void VMState::setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open)
{
    // The override answers alone, without the store, the index or a progress counter, and changes neither the role nor
    // strictness; the activity test still runs first (section 6.2).
    ASSERT(token && open);
    m_bodyVersionOverride = WTF::move(token);
    m_openBodyOverride = WTF::move(open);
}

void VMState::clearBodyLookupForTesting()
{
    m_bodyVersionOverride = nullptr;
    m_openBodyOverride = nullptr;
}
#endif

Role VMState::role() const
{
    return m_config.role;
}

bool VMState::producing() const
{
    return role() == Role::Producer || role() == Role::ConsumerProducer;
}

bool VMState::activityOn() const
{
    return m_activityOn.load(std::memory_order_acquire);
}

ProducerContext* VMState::producerContextIfActive()
{
    // The context is built before the state is published and lives until the state is destroyed, so a JIT worker that
    // obtained it may keep using it after production ends (section 4.4).
    if (!m_producerContext || !productionActive())
        return nullptr;
    return &m_producerContext.value();
}

OpenedArtifact* VMState::artifact()
{
    return m_artifact.get();
}

BenchReport* VMState::benchReport()
{
    return m_benchReport.get();
}

StartOutcome VMState::startOutcome() const
{
    return m_startOutcome;
}

std::optional<FaultReport> VMState::activityFault() const
{
    if (m_activityFault)
        return m_activityFault;
    // A debugger's attach records no report, since it may run on any thread. A fault recorded after it found activity
    // already off and recorded nothing, so the attach came first.
    if (m_debuggerAttached.load(std::memory_order_acquire))
        return FaultReport { FaultClass::DebuggerAttached, { }, "debugger.attach"_s, { } };
    return std::nullopt;
}

std::optional<FaultReport> VMState::productionFault() const
{
    if (!producing())
        return std::nullopt;
    if (m_productionFault)
        return m_productionFault;
    if (productionActive()) {
        // A charge a JIT worker made was refused, which the VM thread raises at its next capture or delta (section 4.5).
        // A recording fault refuses further charges too, but it records itself first.
        if (m_budget && m_budget->hasRefused())
            return FaultReport { FaultClass::RecordingFault, { }, "budget.limit"_s, { } };
        return std::nullopt;
    }
    // Production is off whenever activity is (II2), and the two ways activity goes off without recording a production
    // fault are a start fault, when production never began, and a debugger's attach.
    return activityFault();
}

std::optional<FaultReport> VMState::firstFault() const
{
    // A production fault is never recorded after the activity fault (turnActivityOff), and the two productionFault
    // reports without a record come either while activity is still on or as the activity fault itself.
    if (auto fault = productionFault())
        return fault;
    return activityFault();
}

ProductionState VMState::productionState() const
{
    if (!producing())
        return ProductionState::NotProducing;
    // A refusal the VM thread has not raised yet already ends production as status reports it (section 3.3).
    if (productionActive() && !(m_budget && m_budget->hasRefused()))
        return ProductionState::Active;
    return ProductionState::Ended;
}

Progress& VMState::progress()
{
    return m_progress;
}

ProducerBudget* VMState::producerBudget()
{
    return m_budget.get();
}

ArtifactWriter* VMState::writer()
{
    return m_writer.get();
}

KeptSummaries* VMState::keptSummaries()
{
    return m_keptSummaries.get();
}

void VMState::setKeptSummaries(KeptSummariesHolder&& summaries)
{
    // The capture glue creates them only while production is active, before releaseEndedProductionMemory can free them.
    ASSERT(!m_keptSummaries);
    ASSERT(!m_productionMemoryReleased);
    m_keptSummaries = WTF::move(summaries);
}

void VMState::addIndexEntryCharge(size_t bytes)
{
    ASSERT(m_budget);
    ASSERT(!m_productionMemoryReleased);
    m_indexEntryChargeBytes += bytes;
}

#if ASSERT_ENABLED
unsigned& VMState::capturesInProgress()
{
    return m_capturesInProgress;
}
#endif

#if ENABLE(JITCACHE_TWINS)
ImageTwinCheckState* VMState::imageTwinCheckState()
{
    return m_imageTwinCheckState.get();
}

void VMState::setImageTwinCheckState(ImageTwinCheckStateHolder&& twinCheckState)
{
    ASSERT(!m_imageTwinCheckState);
    m_imageTwinCheckState = WTF::move(twinCheckState);
}
#endif

void VMState::turnActivityOff(FaultReport&& report)
{
    bool activityWasOn = m_activityOn.exchange(false, std::memory_order_acq_rel);
    bool productionWasActive = m_productionActive.exchange(false, std::memory_order_acq_rel);
    if (productionWasActive && !m_productionFault)
        m_productionFault = report;
    if (activityWasOn && !m_activityFault)
        m_activityFault = WTF::move(report);
}

void VMState::noteDebuggerAttached()
{
    // The flag goes first, so a thread whose acquire load sees activity off also sees why.
    m_debuggerAttached.store(true, std::memory_order_release);
    m_activityOn.store(false, std::memory_order_release);
    m_productionActive.store(false, std::memory_order_release);
}

} // namespace JSC::JITCache
