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

#include "ProducerBudget.h"
#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <stdint.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Variant.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

} // namespace JSC

namespace JSC::JITCache {

class BodyKey;

// The bench report (harness sub-SPEC section 9). When Config::benchReportPath is set, the VMState keeps one, which
// appends JSON lines to that path, buffered and flushed when the buffer passes 1 MiB, after each delta, at each host's
// exit (flushBenchReport) and in willDestroyVM. Every line holds event, pid and the VM's ordinal in the process, then the
// event's fields. Times are nanoseconds of CLOCK_THREAD_CPUTIME_ID on the thread that did the work, except a counted
// span's step breakdown, which reads CLOCK_MONOTONIC; sizes are bytes, and keys are lowercase hex. The report is off by
// default and is not charged to the producer budget. Any part may call record for the measurements its own bench
// obligations name, at the cost of one load and one test when the report is off.

struct BenchField {
    ASCIILiteral name; // a nested field joins its parts with '.', as "restore.carried"
    Variant<std::nullptr_t, bool, int64_t, uint64_t, double, String> value; // WTF's Variant, which WTF::switchOn visits; a key as a lowercase hex String
};

class BenchReport final {
    WTF_MAKE_NONCOPYABLE(BenchReport);
    WTF_MAKE_TZONE_ALLOCATED(BenchReport);
public:
    // start's step 3 (SPEC-integrator.md section 3.2): appends to path; null when the file cannot be opened. The report
    // takes the VM's ordinal in the process, which every line carries, from a process-wide counter.
    static std::unique_ptr<BenchReport> open(const String& path);
    ~BenchReport(); // closes the file; its owner flushes it first (section 9.1)

    void record(ASCIILiteral event, std::initializer_list<BenchField>); // VM thread; flushes past 1 MiB
    void flush(); // VM thread: the buffered lines, then the lookup and budget lines
    void addRelinkNanoseconds(uint64_t); // VM thread: the relink accumulator of section 9.3
    uint64_t takeRelinkNanoseconds(); // VM thread: returns the accumulator and resets it

    // VM thread. The budget whose limit, charged and peak bytes each flush's budget line writes (section 9.2); the state
    // sets its producer budget once it exists, and a report without one writes zeros.
    void setBudget(RefPtr<ProducerBudget>&&);

    // VM thread. The lookup line each flush writes (section 9.2): the count and CPU time of bodyVersion for absent and
    // for present keys, and of openBody by result, which VMState::bodyVersion and VMState::openBody add to
    // (SPEC-integrator.md section 6.2).
    enum class Lookup : uint8_t { BodyVersionAbsent, BodyVersionPresent, OpenBodyFound, OpenBodyMissing, OpenBodyUnusable };
    static constexpr unsigned numberOfLookups = 5;
    void countLookup(Lookup, uint64_t nanoseconds);

private:
    BenchReport(int fd, unsigned vmOrdinal);

    struct LookupTally {
        uint64_t count { 0 };
        uint64_t nanoseconds { 0 };
    };

    const int m_fd; // opened for appending
    const int m_pid;
    const unsigned m_vmOrdinal;
    StringBuilder m_buffer; // the lines not yet written
    uint64_t m_relinkNanoseconds { 0 };
    RefPtr<ProducerBudget> m_budget;
    std::array<LookupTally, numberOfLookups> m_lookups { };
};

// Null when no report is open.
BenchReport* benchReport(VM&);

// Any thread. The clock every event's times read (section 9.1): CLOCK_THREAD_CPUTIME_ID, in nanoseconds. The vDSO does not
// serve it on Linux, so each read is a system call inside the span it measures; a counted span reads it only where it
// begins and ends.
uint64_t benchThreadCPUNanoseconds();

// A key as every event writes it: the lowercase hex of its 40 canonical bytes, which is also its body file's name
// without ".bin" (container sub-SPEC section 1.1).
String bodyKeyHex(const BodyKey&);

// A scope around ScriptExecutable::installCode's relink of incoming calls (section 9.3). It reads the thread CPU clock
// when benchReport(vm) is non-null and !vm.heap.currentThreadIsDoingGCWork(), and otherwise measures nothing, so it
// touches the report only on the VM thread outside GC work.
class RelinkTimer {
    WTF_MAKE_NONCOPYABLE(RelinkTimer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    explicit RelinkTimer(VM&);
    ~RelinkTimer(); // when it measured, adds the elapsed time to that report's relink accumulator

private:
    BenchReport* m_report { nullptr }; // null when it measures nothing
    uint64_t m_startNanoseconds { 0 };
};

} // namespace JSC::JITCache
