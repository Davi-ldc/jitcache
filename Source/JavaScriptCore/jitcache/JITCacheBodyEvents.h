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

namespace JSC {

class UnlinkedCodeBlock;
class VM;

namespace JITCache {

struct BodyEventCounts; // bytecode/UnlinkedCodeBlock.h

} // namespace JITCache

} // namespace JSC

namespace JSC::JITCache {

// The VMs' retired body-event totals (SPEC-integrator.harness.md section 10.3). A UCB counts what this process did with
// its body (section 10.1), and a dying UCB adds its counts to its VM's totals, so the body-event dump's total keeps the
// counts of UCBs that died before it. A UCB dies on whichever thread sweeps its VM's heap, so the totals live in a
// process-wide map from VM to counts under a leaf lock.
void retireBodyEventCounts(const UnlinkedCodeBlock&); // the thread sweeping the UCB's VM: adds its counts to the VM's totals
BodyEventCounts retiredBodyEvents(VM&); // VM thread: the totals so far
void forgetRetiredBodyEvents(VM&); // didFinalizeHeap

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
