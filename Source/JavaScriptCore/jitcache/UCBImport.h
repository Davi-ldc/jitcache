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

#include "CodeSpecializationKind.h"
#include "ParserModes.h"
#include "UCBKeys.h"
#include "UCBRequests.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC {

class SourceCode;
class UnlinkedCodeBlock;
class UnlinkedFunctionCodeBlock;
class UnlinkedFunctionExecutable;
class UnlinkedGlobalCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The engine's entry points, which the request objects call (SPEC-ucb.md section 7.7). All run on the VM thread holding the
// API lock and heap access. A request's key mode is fixed by the UCB it acts on (section 3.7), so requestKey is asked for
// one mode per request; recordDecodedSlot computes the other slot's key itself.

// The request's key with the given mode and the snapshot's bits, computed once per request and cached in the state with the
// source digest it rests on and the provider of a supplied digest (section 3.7); empty when the request has no key.
std::optional<BodyKey> requestKey(RequestState&, OptionSet<CodeGenerationMode> keyMode);
// The request's context digest and a UFE body's holder digest, each computed at most once, by the first step that reads it
// (section 3.4). requestContext requires a key; a root body's context reads requestHolderDigest.
const Digest256& requestContext(RequestState&);
const Digest256& requestHolderDigest(RequestState&);
// What a record made by the request keeps of its context (section 3.4): the inputs of section 3.7's context column, or for
// a direct eval the digest requestContext computed, which it computes now if it has not and production is active (R-INT-1).
RecordedContext recordedContext(RequestState&);
// The key of a program or module the native path decoded: the request's key with the decoded UCB's own mode, or empty when
// the decoded UCB's own with-scope bit differs from the snapshot's (THREAD Identity, section 7.3.3).
std::optional<BodyKey> decodedRootKey(RequestState&, const UnlinkedGlobalCodeBlock&);

UnlinkedCodeBlock* importBody(RequestState&); // section 7.3.1
void seedDecoded(RequestState&, UnlinkedCodeBlock&); // section 7.3.3, or a record when it does not seed
void attachLive(RequestState&, UnlinkedCodeBlock&); // section 7.3.4
void recordGenerated(RequestState&, UnlinkedCodeBlock&); // section 7.3.5
void recordDecodedSlot(RequestState&, UnlinkedFunctionCodeBlock&, CodeSpecializationKind); // section 7.3.5; never settles
void recordRoot(VM&, UnlinkedFunctionExecutable&, IdentityKind, const SourceCode& rootSource, LexicallyScopedFeatures,
    std::optional<int32_t> parameterEnd, const Digest256* builtinMetadataDigest = nullptr); // section 7.3.6

} // namespace JSC::JITCache
