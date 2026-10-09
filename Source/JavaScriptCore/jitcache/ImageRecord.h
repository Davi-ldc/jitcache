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

#if ENABLE(JIT)

#include "BakedFacts.h"
#include "ImageTypes.h"
#include "JSCPtrTag.h"
#include "ProducerBudget.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/CodePtr.h>
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/PtrTag.h>
#include <wtf/Ref.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

// The image record (SPEC-image.md section 5): the provenance a recording compilation leaves on its BaselineJITCode, and
// the hooks through which a MathIC regeneration keeps it describing the code, which MathICRegeneration calls (section
// 6.2). No header JSC exports includes this one; BaselineJITCode.h and JITMathIC.h only forward-declare the classes.

namespace JSC {

class CCallHelpers;
class CodeBlock;
class LinkBuffer;
class VM;

template<PtrTag> class CodeLocationLabel;
template<PtrTag> class MacroAssemblerCodeRef;

} // namespace JSC

namespace JSC::JITCache {

class ImageRecorder;

// The footprint order of SPEC-image.md section 3.2: by site, and at a shared site the Call first. Only on ARM64 can two
// fixups share a site, a Call whose bl ends there and the Pointer or Jump whose first word starts there.
inline bool precedesInFootprintOrder(const ImageFixup& a, const ImageFixup& b)
{
    if (a.site != b.site)
        return a.site < b.site;
    return a.form == FixupForm::Call && b.form != FixupForm::Call;
}

// The index of the fixup with this site and form among fixups in footprint order, where the two name at most one.
std::optional<size_t> findFixupIndex(std::span<const ImageFixup>, uint32_t site, FixupForm);

// The four code locations JIT::emitMathICSlow's link task gives a baseline MathIC, as addresses. A MathIC without inline
// code has all four null (SPEC-image.md N17); one with inline code has all four set.
struct MathICLocations {
    const void* inlineStart { nullptr };
    const void* inlineEnd { nullptr };
    const void* slowPathStart { nullptr };
    const void* slowPathCall { nullptr }; // the slow call's return point, m_slowPathCallLocation

    bool areAllSet() const { return inlineStart && inlineEnd && slowPathStart && slowPathCall; }
    bool areAllNull() const { return !inlineStart && !inlineEnd && !slowPathStart && !slowPathCall; }
};

// The locations of a MathIC a record names: mathIC is the JITAddIC, JITSubIC, JITMulIC or JITNegIC its kind says.
MathICLocations mathICLocations(MathICKind, const void* mathIC);

// What one BaselineJITCode's code and MathIC snippets embed, as fixups, and the facts its compilation baked. The recorder
// builds it on the compiling thread; from the moment JIT::link attaches it, only the thread holding the VM's API lock
// reads or writes it, and only its destructor runs elsewhere. It holds no cell, so GC never visits it.
//
// Every byte it holds is charged to its producer budget, and its charge follows those bytes: the object itself and the
// capacity of each of its containers (storageBytes). Destroying it releases exactly that charge, on any thread.
class ImageRecord {
    WTF_MAKE_NONCOPYABLE(ImageRecord);
    WTF_MAKE_TZONE_ALLOCATED(ImageRecord);
public:
    // A record whose recorder stopped before it finished, holding only its state and reason. chargedBytes is the charge it
    // takes over, which pays for the object alone.
    ImageRecord(Ref<ProducerBudget>&&, size_t chargedBytes, RecordState, Unrecordable);
    // A Complete record. imageStart and codeSize are the image's start and linked size (SPEC-image.md section 3.1); the
    // fixups are in footprint order; mathICs are in MathIC index order. chargedBytes is the charge it takes over, which must
    // equal the storageBytes of what it holds.
    ImageRecord(Ref<ProducerBudget>&&, size_t chargedBytes, const void* imageStart, uint32_t codeSize, Vector<ImageFixup>&&, Vector<MathICRecord>&&, BakedFacts&&);
    ~ImageRecord(); // Releases its charge, on any thread.

    // The bytes a record holding containers of these capacities pays for, the object included.
    static size_t storageBytes(size_t fixupCapacity, size_t mathICCapacity, size_t snippetFixupCapacity, size_t scopeFactCapacity);
    // The bytes a snippet's provenance holds, whose charge didGenerateSnippet takes over.
    static size_t storageBytes(const SnippetProvenance&);

    RecordState state() const { return m_state; }
    Unrecordable unrecordableReason() const { return m_unrecordableReason; } // None unless the state is Unrecordable.
    uint32_t codeSize() const { return m_codeSize; } // the linked size (section 3.1)
    const Vector<ImageFixup>& fixups() const LIFETIME_BOUND { return m_fixups; } // in footprint order (section 3.2)
    const Vector<MathICRecord>& mathICs() const LIFETIME_BOUND { return m_mathICs; } // in emission order; index = MathIC index
    const BakedFacts& bakedFacts() const LIFETIME_BOUND { return m_bakedFacts; }
    size_t chargedBytes() const { return m_chargedBytes; }
    // The budget the record is charged to, which a regeneration's snippet recorders charge too (section 4.8).
    Ref<ProducerBudget> budget() const { return m_budget.copyRef(); }
    // The offset of a location in the image, inside [0, codeSize]; nullopt outside it and once the record has stopped.
    std::optional<uint32_t> offsetInImage(const void*) const;

    // The regeneration hooks of SPEC-image.md section 6.2, on the VM thread inside a JIT operation. Each keeps a Complete
    // record complete, or makes it Incomplete when a charge is refused or Unrecordable when the code no longer matches
    // what the record can say; on a record in either state each does nothing.
    std::optional<unsigned> mathICIndex(const void* mathIC) const;
    // Retargets the slow call's Operation fixup to the replacement, or makes the record Unrecordable(ForeignCodeSymbol)
    // when the replacement lies outside the engine's text segment.
    void didReplaceSlowCall(unsigned mathICIndex, CodePtr<CFunctionPtrTag> replacement);
    // Stores the provenance of the snippet the MathIC's m_code now holds, replacing the earlier one, and takes over the
    // charge of storageBytes(provenance), which the snippet's recorder hands it (ImageRecorder::handChargeToRecord). The
    // replaced provenance's charge is released; on a record that is no longer Complete, the new one's is released too.
    void didGenerateSnippet(unsigned mathICIndex, SnippetProvenance&&);
    // The inline start now holds one jump to the snippet: [inlineStart, inlineStart + 4) on ARM64, one b, and
    // [inlineStart, inlineStart + 5) on x86_64, one jmp rel32. Drops every fixup whose footprint lies inside those bytes
    // and inserts the SnippetEntry Jump fixup at the site V5 fixes. A footprint the rewrite covers only in part is a
    // programming error, which makes the record Unrecordable(InconsistentRecord).
    void didRewriteInlineStart(unsigned mathICIndex);
    void markUnrecordable(Unrecordable);
    void markIncomplete();

private:
    // Unrecordable and Incomplete are final. A record that reaches either is never captured, so it frees what it holds
    // and keeps the charge of the object alone.
    void stop(RecordState, Unrecordable);
    void markInconsistent(); // ASSERTs in debug builds; Unrecordable(InconsistentRecord) in release builds.
    // Charges the bytes of new capacity before a container grows; a refusal makes the record Incomplete.
    bool chargeForGrowth(size_t bytes);
    size_t heldBytes() const;
    std::span<const uint8_t> imageCode() const;

    Ref<ProducerBudget> m_budget;
    size_t m_chargedBytes { 0 };
    RecordState m_state { RecordState::Complete };
    Unrecordable m_unrecordableReason { Unrecordable::None };
    const void* m_imageStart { nullptr }; // the image's executable memory, which lives as long as the record
    uint32_t m_codeSize { 0 };
    Vector<ImageFixup> m_fixups;
    Vector<MathICRecord> m_mathICs;
    BakedFacts m_bakedFacts;
};

// What one JITMathIC::generateOutOfLine tells the record of the code it patches (SPEC-image.md section 6.2), on the VM
// thread inside the JIT operation that regenerates, holding the API lock and no CodeBlock::m_lock.
//
// A native regeneration is active exactly when it records (section 4.1): its CodeBlock runs BaselineJIT, its VM's
// production is active, and the CodeBlock's BaselineJITCode carries a Complete record, which lists every MathIC of its
// code. Otherwise, as for every DFG and FTL MathIC and in every VM that does not record, each member does nothing and
// returns 0, except didFailToAllocate, which reports the failure from every tier and VM.
//
// Each attach gives one snippet assembler a recorder of its own, charged to the record's budget, which lives until the
// next attach or the regeneration's end. Both come after that assembler is gone, since generateOutOfLine's assemblers
// are block locals of its body and the regeneration outlives the body. A recorder that stops makes the record stop in
// the same state, so no capture reads a record that lacks a snippet the code jumps to.
class MathICRegeneration {
    WTF_MAKE_NONCOPYABLE(MathICRegeneration);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    // profileBitsAtEntry and callReplacement feed only the twins builds' regeneration log (section 11.1).
    MathICRegeneration(CodeBlock*, const void* mathIC, CodePtr<CFunctionPtrTag> callReplacement, uint16_t profileBitsAtEntry);
    ~MathICRegeneration(); // Destroys the last recorder it attached, whose assembler is already gone.

    bool isActive() const { return !!m_record; }
    unsigned mathICIndex() const { return m_mathICIndex; } // 0 when inactive
    // The offset in the image of the IC's done or slow-path-start location, the target of an ImageOffset fixup; 0 while
    // nothing records, which the helpers ignore without a recorder.
    uint32_t imageOffset(CodeLocationLabel<JSInternalPtrTag>) const;

    // Right after the snippet's assembler is created, before it emits: a MathICSnippet recorder, through
    // ImageRecorder::attachTo, while the record is still Complete.
    void attach(CCallHelpers&);
    // Before the snippet's LinkBuffer: the veneers of the conditional jumps the recorder deferred (section 4.5).
    void emitVeneers(CCallHelpers&);
    // After the snippet's FINALIZE_CODE_FOR: stores the snippet's provenance, with its linked size, in the record in place
    // of any earlier one, and gives back what the recorder charged beyond what the provenance holds (section 4.8).
    void didLinkSnippet(LinkBuffer&, const MacroAssemblerCodeRef<JITStubRoutinePtrTag>&);
    void didRewriteInlineStart(); // ImageRecord::didRewriteInlineStart, after the inline start's FINALIZE_CODE
    void didReplaceSlowCall(CodePtr<CFunctionPtrTag>); // ImageRecord::didReplaceSlowCall, after the repatch
    // At each failed snippet allocation, before the native fallback continues: the executable-allocation fault.
    void didFailToAllocate(VM&);

private:
    // Makes the record stop in the state the current recorder stopped in, if it did.
    void stopRecordLikeRecorder();
    void retireRecorder();

    ImageRecord* m_record { nullptr }; // Non-null exactly while active. Owned by the CodeBlock's BaselineJITCode.
    unsigned m_mathICIndex { 0 };
    std::unique_ptr<ImageRecorder> m_recorder;
};

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
