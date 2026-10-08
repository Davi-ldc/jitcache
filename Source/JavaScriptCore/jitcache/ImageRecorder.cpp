/*
 * Copyright (C) 2026 Anthropic PBC. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ImageRecorder.h"

#if ENABLE(JIT)

#include "ArithProfile.h"
#include "ImageRecord.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "LinkBuffer.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>

#if ENABLE(JITCACHE_TWINS)
#include "ImageTwins.h"
#endif

#if ENABLE(SAMPLING_FLAGS) || ENABLE(SAMPLING_COUNTERS)
#error "JITCache does not record JIT::setSamplingFlag, clearSamplingFlag or emitCount, which embed process addresses"
#endif

// A profile's address is its bits' address (SPEC-image N26): an engine change that adds a member must fail here rather
// than shift every arithmetic-profile target.
static_assert(sizeof(JSC::BinaryArithProfile) == sizeof(JSC::BinaryArithProfileBase));
static_assert(sizeof(JSC::UnaryArithProfile) == sizeof(JSC::UnaryArithProfileBase));

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageRecorder);

namespace ImageRecorderInternal {

static ImageTarget makeTarget(TargetKind kind, uint32_t a = 0)
{
    return ImageTarget { .kind = kind, .a = a, .b = 0, .payload = 0 };
}

} // namespace ImageRecorderInternal

std::optional<ImageTarget> arithProfileTargetIn(UnlinkedCodeBlock& unlinkedCodeBlock, const void* profile)
{
    auto indexIn = []<typename Profile>(const Profile* first, size_t count, const void* profile) -> std::optional<uint32_t> {
        if (!count)
            return std::nullopt;
        auto address = reinterpret_cast<uintptr_t>(profile);
        auto begin = reinterpret_cast<uintptr_t>(first);
        if (address < begin || address - begin >= count * sizeof(Profile) || (address - begin) % sizeof(Profile))
            return std::nullopt;
        return static_cast<uint32_t>((address - begin) / sizeof(Profile));
    };
    if (size_t count = unlinkedCodeBlock.numberOfBinaryArithProfiles()) {
        if (auto index = indexIn(&unlinkedCodeBlock.binaryArithProfile(0), count, profile))
            return ImageRecorderInternal::makeTarget(TargetKind::UCBBinaryArithProfile, *index);
    }
    if (size_t count = unlinkedCodeBlock.numberOfUnaryArithProfiles()) {
        if (auto index = indexIn(&unlinkedCodeBlock.unaryArithProfile(0), count, profile))
            return ImageRecorderInternal::makeTarget(TargetKind::UCBUnaryArithProfile, *index);
    }
    return std::nullopt;
}

ImageRecorder::ImageRecorder(RecordingScope scope, VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, Ref<ProducerBudget>&& budget)
    : m_scope(scope)
    , m_vm(vm)
    , m_unlinkedCodeBlock(unlinkedCodeBlock)
    , m_budget(WTF::move(budget))
{
    // The record object a baseline compilation attaches is charged with the first step, so a recorder whose first
    // charge is refused attaches no record.
    if (m_scope == RecordingScope::BaselineCompile) {
        m_chargedRecordObject = chargeFor(sizeof(ImageRecord));
        if (m_chargedRecordObject)
            m_heldBytes = sizeof(ImageRecord);
    }
}

#if ENABLE(JITCACHE_TWINS)
ImageRecorder::ImageRecorder(RecordingScope scope, VM& vm, UnlinkedCodeBlock& unlinkedCodeBlock, Ref<ProducerBudget>&& budget, const TwinSeeds& seeds, const TwinCompileInputs& compileInputs)
    : ImageRecorder(scope, vm, unlinkedCodeBlock, WTF::move(budget))
{
    m_twinSeeds = &seeds;
    m_twinCompileInputs = &compileInputs;
}
#endif

ImageRecorder::~ImageRecorder()
{
    if (m_chargedBytes)
        m_budget->release(m_chargedBytes);
}

void ImageRecorder::attachTo(AbstractMacroAssemblerBase& masm)
{
    masm.setJITCacheRecorder(this);
#if ENABLE(JITCACHE_TWINS)
    if (m_twinSeeds) {
        masm.seedRandomForTwins(m_twinSeeds->assembler);
        m_assemblerSeed = m_twinSeeds->assembler;
    }
#endif
}

bool ImageRecorder::chargeFor(size_t heldBytes)
{
    if (heldBytes <= m_chargedBytes)
        return true;
    if (!isRecording())
        return false;
    size_t steps = roundUpToMultipleOf<kRecordChargeStep>(heldBytes - m_chargedBytes);
    if (!m_budget->tryCharge(steps)) {
        stop(RecordState::Incomplete);
        return false;
    }
    m_chargedBytes += steps;
    return true;
}

template<typename T>
bool ImageRecorder::reserveRecorded(Vector<T>& vector, size_t additional, size_t minimumCapacity)
{
    size_t needed = vector.size() + additional;
    if (needed <= vector.capacity())
        return true;
    size_t newCapacity = std::max({ needed, minimumCapacity, vector.capacity() * 2 });
    size_t addedBytes = (newCapacity - vector.capacity()) * sizeof(T);
    if (!chargeFor(m_heldBytes + addedBytes))
        return false;
    vector.reserveCapacity(newCapacity);
    ASSERT(vector.capacity() == newCapacity);
    m_heldBytes += addedBytes;
    return true;
}

template<typename T>
void ImageRecorder::forgetRecorded(Vector<T>& vector)
{
    ASSERT(m_heldBytes >= vector.capacity() * sizeof(T));
    m_heldBytes -= vector.capacity() * sizeof(T);
    vector = { };
}

void ImageRecorder::stop(RecordState state)
{
    ASSERT(state != RecordState::Complete);
    if (!isRecording())
        return;
    m_state = state;

    // Nothing reads what the recorder stored once it stops, except the deferred jumps emitVeneers still links, whose
    // storage stays. The baked-facts builder stops storing at its next request and keeps what it holds, charged, until
    // the recorder is destroyed.
    forgetRecorded(m_fixups);
    forgetRecorded(m_mathICs);
#if ENABLE(JITCACHE_TWINS)
    forgetRecorded(m_binarySwitchSeeds);
#endif
}

void ImageRecorder::markUnrecordable(Unrecordable reason)
{
    if (!isRecording())
        return;
    m_unrecordableReason = reason;
    stop(RecordState::Unrecordable);
}

void ImageRecorder::record(AssemblerLabel site, FixupForm form, const ImageTarget& target)
{
    ASSERT(site.isSet());
    if (!isRecording() || !reserveRecorded(m_fixups, 1, 16))
        return;
    m_fixups.append(ImageFixup { .site = site.offset(), .form = form, .target = target });
}

void ImageRecorder::recordPointer(AssemblerLabel site, const ImageTarget& target)
{
    record(site, FixupForm::Pointer, target);
}

void ImageRecorder::recordCall(AssemblerLabel site, const ImageTarget& target)
{
    record(site, FixupForm::Call, target);
}

void ImageRecorder::recordJump(AssemblerLabel site, const ImageTarget& target)
{
    record(site, FixupForm::Jump, target);
}

void ImageRecorder::linkJumpNatively(MacroAssembler& masm, MacroAssembler::Jump jump, CodeLocationLabel<NoPtrTag> nativeTarget)
{
    SupportLinkScope scope(*this);
    jump.linkThunk(nativeTarget, &masm);
}

void ImageRecorder::deferConditionalJump(MacroAssembler& masm, MacroAssembler::Jump jump, const ImageTarget& target, CodeLocationLabel<NoPtrTag> nativeTarget)
{
#if CPU(ARM64)
    ASSERT(jump.isConditionalForJITCache());
    if (!isRecording()) {
        linkJumpNatively(masm, jump, nativeTarget);
        return;
    }

    // A compilation has few distinct external targets, so a linear search finds a target's group.
    for (auto& group : m_veneerGroups) {
        if (group.target != target)
            continue;
        ASSERT(group.nativeTarget.dataLocation() == nativeTarget.dataLocation());
        if (!reserveRecorded(group.jumps, 1, 4)) {
            linkJumpNatively(masm, jump, nativeTarget);
            return;
        }
        group.jumps.append(jump);
        return;
    }

    if (!reserveRecorded(m_veneerGroups, 1, 4)) {
        linkJumpNatively(masm, jump, nativeTarget);
        return;
    }
    VeneerGroup group { target, nativeTarget, { } };
    if (!reserveRecorded(group.jumps, 1, 4)) {
        linkJumpNatively(masm, jump, nativeTarget);
        return;
    }
    group.jumps.append(jump);
    m_veneerGroups.append(WTF::move(group));
#else
    // A conditional external branch is a jcc rel32 here, whose own site carries its fixup.
    linkJumpNatively(masm, jump, nativeTarget);
    recordJump(jump.assemblerLabel(), target);
#endif
}

void ImageRecorder::emitVeneers(MacroAssembler& masm)
{
#if CPU(ARM64)
    // Every deferred jump gets its veneer, including those deferred before the recorder stopped, whose storage was
    // charged when they were deferred. Only the veneer's fixup depends on whether the recorder still records.
    for (auto& group : m_veneerGroups) {
        auto veneer = masm.label();
        for (auto& jump : group.jumps)
            jump.linkTo(veneer, &masm);
        auto branch = masm.jump();
        linkJumpNatively(masm, branch, group.nativeTarget);
        recordJump(branch.assemblerLabel(), group.target);
    }
    for (auto& group : m_veneerGroups)
        forgetRecorded(group.jumps);
    forgetRecorded(m_veneerGroups);
#else
    UNUSED_PARAM(masm);
    ASSERT(m_veneerGroups.isEmpty());
#endif
}

unsigned ImageRecorder::noteMathIC(MathICKind kind, const void* mathIC, BytecodeIndex bytecodeIndex)
{
    ASSERT(m_scope == RecordingScope::BaselineCompile);
    unsigned index = m_mathICCount++;
    if (isRecording() && reserveRecorded(m_mathICs, 1, 16)) {
        ASSERT(m_mathICs.size() == index);
        m_mathICs.append(NotedMathIC { kind, bytecodeIndex, mathIC });
    }
    return index;
}

ImageTarget ImageRecorder::mathICTarget(const void* mathIC) const
{
    // The slow paths reach the ICs in the order the main pass noted them, so the search resumes after the last match.
    size_t count = m_mathICs.size();
    for (size_t i = 0; i < count; ++i) {
        size_t index = (m_mathICSearchStart + i) % count;
        if (m_mathICs[index].mathIC != mathIC)
            continue;
        m_mathICSearchStart = index + 1;
        return ImageRecorderInternal::makeTarget(TargetKind::MathIC, static_cast<uint32_t>(index));
    }
    RELEASE_ASSERT_NOT_REACHED();
}

std::optional<ImageTarget> ImageRecorder::arithProfileTarget(const void* profile) const
{
    return arithProfileTargetIn(m_unlinkedCodeBlock, profile);
}

std::optional<ImageTarget> ImageRecorder::vmAddressTarget(const void* address) const
{
    auto vmAddress = vmAddressFor(m_vm, address);
    if (!vmAddress)
        return std::nullopt;
    return ImageRecorderInternal::makeTarget(TargetKind::VMAddress, static_cast<uint32_t>(*vmAddress));
}

BakedFactsBuilder& ImageRecorder::bakedFacts()
{
    ASSERT(m_scope == RecordingScope::BaselineCompile);
    return m_bakedFacts;
}

bool ImageRecorder::allowStorage(size_t bytes)
{
    if (!isRecording())
        return false;
    if (!bytes)
        return true;
    if (!chargeFor(m_heldBytes + bytes))
        return false;
    m_heldBytes += bytes;
    return true;
}

#if ENABLE(JITCACHE_TWINS)
void ImageRecorder::didInitializeRandom(uint32_t seed)
{
    if (isRecording())
        m_assemblerSeed = seed;
}

uint32_t ImageRecorder::binarySwitchSeed(uint32_t drawn)
{
    // A twin keeps answering from the producer's seeds after it stops, so that its emission stays the producer's.
    uint32_t seed = drawn;
    if (m_twinSeeds) {
        auto& recorded = m_twinSeeds->binarySwitches;
        if (m_nextTwinBinarySwitchSeed < recorded.size())
            seed = recorded[m_nextTwinBinarySwitchSeed];
        ++m_nextTwinBinarySwitchSeed;
    }
    if (isRecording() && reserveRecorded(m_binarySwitchSeeds, 1, 16))
        m_binarySwitchSeeds.append(seed);
    return seed;
}
#endif

bool ImageRecorder::translateFixups(LinkBuffer& linkBuffer)
{
    for (auto& fixup : m_fixups)
        fixup.site = linkBuffer.offsetOf(AssemblerLabel(fixup.site));
    std::sort(m_fixups.begin(), m_fixups.end(), [](const ImageFixup& a, const ImageFixup& b) {
        return a.site < b.site;
    });

    auto code = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
    size_t previousEnd = 0;
    for (auto& fixup : m_fixups) {
        // Each footprint holds its form's instruction (table 3.2), whose canonical encoding capture will write.
        auto footprint = fixupFootprint(fixup.form, fixup.site, code);
        bool consistent = footprint && footprint->begin >= previousEnd;
        if (consistent) {
            auto bytes = code.subspan(footprint->begin, footprint->size());
            consistent = isCanonicalFootprint(fixup.form, canonicalFootprint(fixup.form, bytes).span());
        }
        if (!consistent) {
            ASSERT_NOT_REACHED();
            markUnrecordable(Unrecordable::InconsistentRecord);
            return false;
        }
        previousEnd = footprint->end;
    }
    return true;
}

std::optional<SnippetProvenance> ImageRecorder::finishSnippet(LinkBuffer& linkBuffer)
{
    ASSERT(m_scope == RecordingScope::MathICSnippet);
    ASSERT(m_veneerGroups.isEmpty());
    if (!isRecording() || !translateFixups(linkBuffer))
        return std::nullopt;

    // The provenance keeps the fixups at their exact size, so that a record's charge follows its bytes. What the
    // recorder charged beyond them is released by handChargeToRecord, or when the recorder is destroyed.
    size_t capacityBytes = m_fixups.capacity() * sizeof(ImageFixup);
    m_fixups.shrinkToFit();
    ASSERT(m_heldBytes >= capacityBytes);
    m_heldBytes -= capacityBytes - m_fixups.capacity() * sizeof(ImageFixup);
    return SnippetProvenance {
        .start = linkBuffer.debugAddress(),
        .size = static_cast<uint32_t>(linkBuffer.size()),
        .fixups = std::exchange(m_fixups, { }),
    };
}

void ImageRecorder::handChargeToRecord(size_t bytes)
{
    // Finishing moved every container the recorder charged for into the record, or freed it.
    ASSERT(!m_fixups.capacity() && !m_mathICs.capacity() && !m_veneerGroups.capacity());
    RELEASE_ASSERT(bytes <= m_chargedBytes);
    if (size_t unused = m_chargedBytes - bytes)
        m_budget->release(unused);
    m_chargedBytes = 0;
    m_heldBytes = 0;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
