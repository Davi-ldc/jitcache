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

#include <array>
#include <span>
#include <stdint.h>
#include <wtf/Forward.h>
#include <wtf/Variant.h>
#include <wtf/text/ASCIILiteral.h>

// The option classes of docs/JitCache/options.md (SPEC-integrator.md section 5.1). The must-match options' effective
// values go into the header; the fixed options must hold their required values, which start checks because JSC keeps
// no defaults once options freeze. Every other option is free. The functions that read options run on any thread once
// options are finalized.

namespace JSC::JITCache {

// The must-match options, by the index of the header's option records (container sub-SPEC section 3.1):
// 0 evalMode, 1 useExplicitResourceManagement, 2 useImportDefer. Each is a Bool.
inline constexpr unsigned numberOfMustMatchOptions = 3;
ASCIILiteral mustMatchOptionName(unsigned index); // index < numberOfMustMatchOptions
std::array<bool, numberOfMustMatchOptions> mustMatchOptionValues(); // the effective values, by index

// A fixed option's value, in the type OptionsList.h declares: Bool, Unsigned, Int32, Double or OptionString.
using FixedOptionValue = Variant<bool, unsigned, int32_t, double, const char*>;

// One row of options.md's fixed table.
struct FixedOptionRow {
    ASCIILiteral name;
    FixedOptionValue (*effectiveValue)(); // reads Options::name()
    FixedOptionValue requiredValue;

    // Bool, Unsigned and Int32 compare exactly and Double with ==; an OptionString required to be null holds when the
    // option is nullptr.
    bool holds() const;
};

std::span<const FixedOptionRow> fixedOptionRows(); // in options.md's order
const FixedOptionRow* checkFixedOptions(); // the first row in that order that does not hold, or null

String describeFixedOptionValue(const FixedOptionValue&); // true, 15, 0.2, null, or a string in quotes
String describeFixedOptionMismatch(const FixedOptionRow&); // "useLOLJIT: required false, effective true"

} // namespace JSC::JITCache
