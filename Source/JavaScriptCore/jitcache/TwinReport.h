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

#include <memory>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache {

// The twin report every part's twin checks write to (harness sub-SPEC section 2). Each call appends one JSON line and
// writes it out at once, so a later crash keeps it:
//   {"kind":"difference","part":"image","check":"…","detail":"…"}
//   {"kind":"skip","part":"image","check":"…","reason":"…"}
//   {"kind":"coincidence","domain":"engine-image","detail":"…"}
// with the parts spelled ucb, image, cb, ics and integrator, and the domains engine-image, executable-pool,
// structure-reservation and heap. Details name the body by its key in hex and the CB by CodeBlock::dump. The report is
// used on the VM thread only, and a part reports through it only while VMState::twinReportSink() is non-null
// (SPEC-integrator.md R-ALL-2).

enum class TwinPart : uint8_t { UCB, Image, CB, ICs, Integrator };
enum class RelocationDomain : uint8_t { EngineImage, ExecutablePool, StructureReservation, Heap };

class TwinReport {
    WTF_MAKE_NONCOPYABLE(TwinReport);
    WTF_MAKE_TZONE_ALLOCATED(TwinReport);
public:
    static std::unique_ptr<TwinReport> open(const String& path); // appends; null when the file cannot be opened
    void difference(TwinPart, ASCIILiteral check, String detail);
    void skip(TwinPart, ASCIILiteral check, String reason);
    void relocationCoincidence(RelocationDomain, String detail);
    unsigned differences() const;
    unsigned skips() const;
    unsigned coincidences() const;
    ~TwinReport(); // closes the file

private:
    explicit TwinReport(int fd);

    const int m_fd; // opened for appending; each line goes out in one write, with no buffer of its own
    unsigned m_differences { 0 };
    unsigned m_skips { 0 };
    unsigned m_coincidences { 0 };
};

using TwinReportSink = TwinReport; // the name SPEC-ucb.md uses

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
