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

// Exported to Bun as <JavaScriptCore/JITCacheMaintenance.h> (SPEC-integrator.md section 3.5): it includes only WTF headers
// and JSExportMacros.h, and no member exists only under ENABLE(JITCACHE_TWINS).

#include <JavaScriptCore/JSExportMacros.h>
#include <array>
#include <optional>
#include <span>
#include <stdint.h>
#include <stdio.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/CString.h>
#include <wtf/text/WTFString.h>

namespace JSC::JITCache::Maintenance {

// The synchronous, VM-independent backend of clean and compact (SPEC-integrator.maintenance.md). It needs no VM and no
// JSC::initialize, and runs on the calling thread.

enum class Outcome : uint8_t { Done, NoArtifact, Busy, NeedsConfirmation, Declined, PlanChanged, Failed };

struct Diagnostic {
    ASCIILiteral code; // section 6
    String detail;
};

struct Eviction {
    std::array<uint8_t, 40> key;
    uint64_t bytes { 0 };
    uint64_t version { 0 }; // the envelope's commit identifier; 0 for a body whose envelope failed
    double score { 0 }; // (L + P) / B; -infinity for a body evicted as damaged or foreign
    // Plan's comparison reads every eviction field for field (section 4.4).
    friend bool operator==(const Eviction&, const Eviction&) = default;
};

struct Plan {
    std::array<uint8_t, 16> headerDigest; // the artifact's (container sub-SPEC section 3.3)
    uint64_t postCleanBytes { 0 };
    uint64_t targetBytes { 0 };
    uint64_t evictedBytes { 0 };
    Vector<Eviction> evictions; // in eviction order
    bool deletesArtifact { false };
    friend bool operator==(const Plan&, const Plan&) = default;
};

struct Report {
    Outcome outcome { Outcome::Failed };
    uint64_t temporariesRemoved { 0 };
    uint64_t bodiesEvicted { 0 };
    uint64_t bytesReclaimed { 0 };
    std::optional<Plan> plan;
    Vector<Diagnostic> diagnostics;
};

enum class Answer : uint8_t { Ask, Yes, No };
struct CompactOptions {
    Answer answer { Answer::Ask }; // for a plan that deletes the whole artifact
    std::optional<Plan> confirmedPlan; // set by the second call of section 4.4
};

JS_EXPORT_PRIVATE Report clean(const String& parentPath);
JS_EXPORT_PRIVATE Report compact(const String& parentPath, double ratio, const CompactOptions&);
// "clean <path>" or "compact <path> <ratio> [--yes | --no]" (section 5); returns the process exit code.
JS_EXPORT_PRIVATE int runCommandLine(std::span<const CString> arguments, FILE* in, FILE* out, FILE* err);

} // namespace JSC::JITCache::Maintenance
