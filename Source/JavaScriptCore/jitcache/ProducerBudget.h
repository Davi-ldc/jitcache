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

#include <atomic>
#include <stddef.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

// The producer working-memory limit (THREAD Session; SPEC-integrator.md section 4.4). tryCharge returns false at once
// once the budget has refused; otherwise it adds the bytes to the charged total with a compare-and-swap loop, and a sum
// that overflows or exceeds the limit sets the sticky refused flag and returns false without changing the total; a
// success raises the peak. release subtracts and asserts that the total stays at or above zero. Every member uses
// atomics, so a charge, a release and a read may run on any thread, and a record destroyed after its VM's session
// releases into a budget it still references.
class ProducerBudget final : public ThreadSafeRefCounted<ProducerBudget> {
    WTF_MAKE_TZONE_ALLOCATED(ProducerBudget);
public:
    static Ref<ProducerBudget> create(size_t limitBytes);
    static Ref<ProducerBudget> createUnlimited(); // twin compiles; never refuses
    [[nodiscard]] bool tryCharge(size_t bytes); // any thread
    void release(size_t bytes); // any thread
    bool hasRefused() const; // any thread
    void refuseFurtherCharges(); // VM thread, when production ends
    size_t limitBytes() const;
    size_t chargedBytes() const;
    size_t peakBytes() const;

private:
    ProducerBudget(size_t limitBytes, bool isUnlimited);

    const size_t m_limitBytes; // SIZE_MAX for an unlimited budget
    const bool m_isUnlimited; // createUnlimited: no charge is refused, an overflowing one included
    std::atomic<size_t> m_chargedBytes { 0 };
    std::atomic<size_t> m_peakBytes { 0 };
    std::atomic<bool> m_hasRefused { false };
};

// What a recording compilation charges against: the state's one context, which lives until the state is destroyed.
class ProducerContext {
public:
    explicit ProducerContext(Ref<ProducerBudget>&&);
    Ref<ProducerBudget> budget() const;

private:
    const Ref<ProducerBudget> m_budget;
};

// Any thread, no lock. Loads vm.jitCacheState() with acquire order and returns its producerContextIfActive(): non-null
// exactly while the role produces, cache activity is on and production has not ended, the same context each time, and
// null for good from then on. The object lives until the state is destroyed, after cancelAllPlansForVM has stopped every
// compilation of the VM, so a JIT worker that obtained it may use it for its whole compilation.
ProducerContext* producerContext(VM&);

} // namespace JSC::JITCache
