#include "config.h"
#include "ImageRecorder.h"

#if ENABLE(JIT)

#include "ArithProfile.h"
#include "BaselineJITCode.h"
#include "ImageRecord.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "JIT.h"
#include "LinkBuffer.h"
#include "UnlinkedCodeBlock.h"
#include <algorithm>
#include <limits>
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

static std::span<const uint8_t> linkedBytesOf(LinkBuffer& linkBuffer)
{
    // The bytes the LinkBuffer wrote, its linked size; the allocation can extend past them (SPEC-image.md N20).
    return unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
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
    forgetRecorded(m_compileInputs.inputs);
    forgetRecorded(m_compileInputs.binaryArithBits);
    forgetRecorded(m_compileInputs.unaryArithBits);
    forgetRecorded(m_strictEqualityInputs);
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

void ImageRecorder::recordCompileInputs(TwinCompileInputs&& inputs)
{
    ASSERT(m_scope == RecordingScope::BaselineCompile);
    ASSERT(!m_compileInputs.inputs.capacity() && !m_compileInputs.binaryArithBits.capacity() && !m_compileInputs.unaryArithBits.capacity());
    if (!isRecording())
        return;
    // snapshotCompileInputs allocated the snapshot at its exact size; from here on the recorder holds those bytes, so
    // their charge comes now, and a refusal drops them with the recorder's storage (section 4.8).
    size_t bytes = inputs.inputs.capacity() * sizeof(CompileInput)
        + inputs.binaryArithBits.capacity() * sizeof(uint16_t)
        + inputs.unaryArithBits.capacity() * sizeof(uint16_t);
    if (!chargeFor(m_heldBytes + bytes))
        return;
    m_heldBytes += bytes;
    m_compileInputs = WTF::move(inputs);
}

StrictEqualityAtomOperand ImageRecorder::strictEqualityAtomOperand(BytecodeIndex bytecodeIndex, StrictEqualityAtomOperand chosen)
{
    ASSERT(m_scope == RecordingScope::BaselineCompile);
    // A twin answers from the producer's input at the instruction, after it stops too, so that it compares inline where
    // the producer did; W4 gave every instruction that asks one such input, naming an atom of this UCB.
    StrictEqualityAtomOperand answer = chosen;
    uint32_t bytecodeOffset = bytecodeIndex.offset();
    if (m_twinCompileInputs) {
        auto& inputs = m_twinCompileInputs->inputs;
        auto input = std::ranges::lower_bound(inputs, bytecodeOffset, { }, &CompileInput::bytecodeOffset);
        if (input != inputs.end() && input->bytecodeOffset == bytecodeOffset && input->kind == CompileInputKind::StrictEqualityAtomOperand)
            answer = static_cast<StrictEqualityAtomOperand>(input->value);
        else
            ASSERT_NOT_REACHED();
    }
    if (isRecording() && reserveRecorded(m_strictEqualityInputs, 1, 16)) {
        m_strictEqualityInputs.append(CompileInput {
            .bytecodeOffset = bytecodeOffset,
            .kind = CompileInputKind::StrictEqualityAtomOperand,
            .value = static_cast<uint8_t>(answer),
            .localScopeDepth = 0,
        });
    }
    return answer;
}

std::optional<TwinData> ImageRecorder::finishTwinData()
{
    ASSERT(isRecording());
    // Both lists are in bytecode order and name different opcodes, so a merge keeps the inputs sorted with one per
    // instruction. The merged storage is charged before it is allocated, and the two lists are freed after.
    auto& snapshot = m_compileInputs.inputs;
    size_t count = snapshot.size() + m_strictEqualityInputs.size();
    Vector<CompileInput> merged;
    if (!reserveRecorded(merged, count, count))
        return std::nullopt;
    size_t snapshotIndex = 0;
    size_t strictEqualityIndex = 0;
    while (snapshotIndex < snapshot.size() || strictEqualityIndex < m_strictEqualityInputs.size()) {
        bool takesSnapshot = strictEqualityIndex == m_strictEqualityInputs.size()
            || (snapshotIndex < snapshot.size() && snapshot[snapshotIndex].bytecodeOffset < m_strictEqualityInputs[strictEqualityIndex].bytecodeOffset);
        merged.append(takesSnapshot ? snapshot[snapshotIndex++] : m_strictEqualityInputs[strictEqualityIndex++]);
    }
    forgetRecorded(snapshot);
    forgetRecorded(m_strictEqualityInputs);
    m_binarySwitchSeeds.shrinkToFit();

    TwinData data;
    // Every baseline compilation draws for its entry nop, so the assembler has reported its seed.
    ASSERT(m_assemblerSeed);
    data.seeds.assembler = m_assemblerSeed.value_or(0);
    data.seeds.binarySwitches = std::exchange(m_binarySwitchSeeds, { });
    data.compileInputs.inputs = WTF::move(merged);
    data.compileInputs.binaryArithBits = std::exchange(m_compileInputs.binaryArithBits, { });
    data.compileInputs.unaryArithBits = std::exchange(m_compileInputs.unaryArithBits, { });
    data.compileInputs.compiledHoldingAPILock = m_compileInputs.compiledHoldingAPILock;
    return data;
}
#endif

void ImageRecorder::translateSites(LinkBuffer& linkBuffer)
{
    for (auto& fixup : m_fixups)
        fixup.site = linkBuffer.offsetOf(AssemblerLabel(fixup.site));
}

bool ImageRecorder::orderAndCheckFixups(std::span<const uint8_t> linkedCode)
{
    // Distinct fixups never compare equal in a valid record: two at one site and of one form, or a Pointer and a Jump at
    // one site, overlap, which the check below reports.
    std::sort(m_fixups.begin(), m_fixups.end(), precedesInFootprintOrder);

    size_t previousEnd = 0;
    for (auto& fixup : m_fixups) {
        // Each footprint holds its form's instruction (table 3.2), whose canonical encoding capture will write.
        auto footprint = fixupFootprint(fixup.form, fixup.site, linkedCode);
        bool consistent = footprint && footprint->begin >= previousEnd;
        if (consistent) {
            auto bytes = linkedCode.subspan(footprint->begin, footprint->size());
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

void ImageRecorder::recordFarCalls(LinkBuffer& linkBuffer, std::span<const FarCallRecord> farCalls)
{
    ASSERT(isRecording());
    auto count = static_cast<size_t>(std::ranges::count_if(farCalls, [](const FarCallRecord& farCall) {
        return !!farCall.callee;
    }));
    if (!reserveRecorded(m_fixups, count, 0))
        return;

    for (auto& farCall : farCalls) {
        if (!farCall.callee)
            continue;
        auto symbol = CodeSymbol::of(farCall.callee.untaggedPtr());
        if (!symbol) {
            markUnrecordable(Unrecordable::ForeignCodeSymbol);
            return;
        }
        // JIT::link fills the placeholder through LinkBuffer::link, whose writer takes this site (N2).
        auto site = farCallPointerSite(linkBuffer.offsetOf(farCall.from.m_label));
        if (!site) {
            ASSERT_NOT_REACHED();
            markUnrecordable(Unrecordable::InconsistentRecord);
            return;
        }
        m_fixups.append(ImageFixup {
            .site = *site,
            .form = FixupForm::Pointer,
            .target = ImageTarget { .kind = TargetKind::Operation, .a = 0, .b = 0, .payload = symbol->offset },
        });
    }
}

Vector<MathICRecord> ImageRecorder::buildMathICRecords(std::span<const uint8_t> linkedCode)
{
    ASSERT(isRecording());
    ASSERT(m_mathICs.size() == m_mathICCount);
    size_t recordBytes = m_mathICs.size() * sizeof(MathICRecord);
    if (!chargeFor(m_heldBytes + recordBytes))
        return { };
    m_heldBytes += recordBytes;

    auto codeStart = reinterpret_cast<uintptr_t>(linkedCode.data());
    auto offsetInCode = [&](const void* location) -> std::optional<uint32_t> {
        auto address = reinterpret_cast<uintptr_t>(location);
        if (address < codeStart || address - codeStart > linkedCode.size())
            return std::nullopt;
        return static_cast<uint32_t>(address - codeStart);
    };

    Vector<MathICRecord> records;
    records.reserveInitialCapacity(m_mathICs.size());
    bool consistent = true;
    for (auto& noted : m_mathICs) {
        MathICRecord record {
            .kind = noted.kind,
            .bytecodeIndex = noted.bytecodeIndex,
            .mathIC = const_cast<void*>(noted.mathIC),
            .slowCallPointerSite = std::nullopt,
            .snippet = std::nullopt,
        };
        // The link tasks of JIT::emitMathICSlow have set all four locations of an IC with inline code; an IC without
        // inline code never reaches emitMathICSlow and keeps all four null (section 6.1, N17).
        auto locations = mathICLocations(noted.kind, noted.mathIC);
        if (locations.areAllSet()) {
            auto slowCall = offsetInCode(locations.slowPathCall);
            auto site = slowCall ? farCallPointerSite(*slowCall) : std::nullopt;
            auto fixup = site ? findFixupIndex(m_fixups.span(), *site, FixupForm::Pointer) : std::nullopt;
            if (!fixup || m_fixups[*fixup].target.kind != TargetKind::Operation) {
                consistent = false;
                break;
            }
            record.slowCallPointerSite = *site;
        } else if (!locations.areAllNull()) {
            consistent = false;
            break;
        }
        records.append(WTF::move(record));
    }

    if (!consistent) {
        records = { };
        m_heldBytes -= recordBytes;
        ASSERT_NOT_REACHED();
        markUnrecordable(Unrecordable::InconsistentRecord);
        return { };
    }
    forgetRecorded(m_mathICs);
    return records;
}

std::unique_ptr<ImageRecord> ImageRecorder::finishBaselineCompile(LinkBuffer& linkBuffer, BaselineJITCode& jitCode, std::span<const FarCallRecord> farCalls)
{
    using namespace ImageRecorderInternal;
    ASSERT(m_scope == RecordingScope::BaselineCompile);
    // emitVeneers ran before the LinkBuffer was built.
    ASSERT(m_veneerGroups.isEmpty());
    // The record object is charged with the recorder's first step; a recorder whose first charge was refused attaches no
    // record, which capture and regeneration treat as an image without one.
    if (!m_chargedRecordObject)
        return nullptr;

    auto code = linkedBytesOf(linkBuffer);
    ASSERT(code.size() <= std::numeric_limits<uint32_t>::max());

    // Steps 1 and 2: the far calls' placeholders join the recorded fixups, every site an offset in the linked code, all
    // in footprint order and inside [0, codeSize).
    if (isRecording()) {
        translateSites(linkBuffer);
        recordFarCalls(linkBuffer, farCalls);
    }
    if (isRecording())
        orderAndCheckFixups(code);

    // Step 3: the MathIC entries, read after the link tasks FINALIZE_BASELINE_CODE ran.
    Vector<MathICRecord> mathICs;
    if (isRecording())
        mathICs = buildMathICRecords(code);

    // Step 4: only the profiler opcodes clear m_isShareable (N29).
    if (!jitCode.m_isShareable)
        markUnrecordable(Unrecordable::NotShareable);

#if ENABLE(JITCACHE_TWINS)
    // The twin data the record keeps beside the provenance (section 11.1), with the strict-equality inputs merged into the
    // snapshot's.
    std::optional<TwinData> twinData;
    if (isRecording())
        twinData = finishTwinData();
#endif

    // Steps 5 and 6. The builder's storage goes into the record or is freed here, so that the record's charge follows
    // its bytes and the recorder keeps none.
    BakedFacts bakedFacts = m_bakedFacts.finish();
    if (!isRecording()) {
        mathICs = { };
        bakedFacts = { };
#if ENABLE(JITCACHE_TWINS)
        twinData = std::nullopt;
#endif
        auto record = makeUnique<ImageRecord>(m_budget.copyRef(), sizeof(ImageRecord), m_state, m_unrecordableReason);
        handChargeToRecord(sizeof(ImageRecord));
        return record;
    }

    m_fixups.shrinkToFit();
    ASSERT(mathICs.size() == mathICs.capacity());
    size_t recordBytes = ImageRecord::storageBytes(m_fixups.capacity(), mathICs.capacity(), 0, bakedFacts.scopeFacts.capacity());
    auto record = makeUnique<ImageRecord>(m_budget.copyRef(), recordBytes, linkBuffer.debugAddress(), static_cast<uint32_t>(code.size()), std::exchange(m_fixups, { }), WTF::move(mathICs), WTF::move(bakedFacts));
#if ENABLE(JITCACHE_TWINS)
    ASSERT(twinData);
    size_t twinBytes = twinDataStorageBytes(*twinData);
    record->adoptTwinData(WTF::move(*twinData), twinBytes);
    recordBytes += twinBytes;
#endif
    handChargeToRecord(recordBytes);
    return record;
}

std::optional<SnippetProvenance> ImageRecorder::finishSnippet(LinkBuffer& linkBuffer)
{
    using namespace ImageRecorderInternal;
    ASSERT(m_scope == RecordingScope::MathICSnippet);
    ASSERT(m_veneerGroups.isEmpty());
    if (!isRecording())
        return std::nullopt;
    translateSites(linkBuffer);
    if (!orderAndCheckFixups(linkedBytesOf(linkBuffer)))
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
#if ENABLE(JITCACHE_TWINS)
    ASSERT(!m_binarySwitchSeeds.capacity() && !m_strictEqualityInputs.capacity());
    ASSERT(!m_compileInputs.inputs.capacity() && !m_compileInputs.binaryArithBits.capacity() && !m_compileInputs.unaryArithBits.capacity());
#endif
    RELEASE_ASSERT(bytes <= m_chargedBytes);
    if (size_t unused = m_chargedBytes - bytes)
        m_budget->release(unused);
    m_chargedBytes = 0;
    m_heldBytes = 0;
}

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
