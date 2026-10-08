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

#include <wtf/TZoneMallocInlines.h>

// The VMState members that neither create nor destroy a state nor read the store (SPEC-integrator.md section 17, task 2):
// the switches, the fault records and the members that raise faults, and the producer context. The body lookups and their
// test override live beside the store's reads (task 5), releaseEndedProductionMemory beside the memory it releases
// (task 9), and the constructor and destructor beside start (task 7).

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMState);

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
