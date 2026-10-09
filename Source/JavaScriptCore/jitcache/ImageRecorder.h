#pragma once

#if ENABLE(JIT)

#include "BakedFacts.h"
#include "BytecodeIndex.h"
#include "ImageTypes.h"
#include "MacroAssembler.h"
#include "ProducerBudget.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>

namespace JSC {

class BaselineJITCode;
class LinkBuffer;
class UnlinkedCodeBlock;
class VM;

template<PtrTag> struct CallRecord;
using FarCallRecord = CallRecord<OperationPtrTag>;

namespace JITCache {

class ImageRecord;

#if ENABLE(JITCACHE_TWINS)
struct TwinCompileInputs;
struct TwinSeeds;
enum class StrictEqualityAtomOperand : uint8_t;
#endif

// The recorder charges the producer budget in steps of this many bytes, which keeps charges off the per-fixup path.
constexpr size_t kRecordChargeStep = 4 * KB;

enum class RecordingScope : uint8_t { BaselineCompile, MathICSnippet };

// The arithmetic-profile target of profile, by the UCB vector its address lies in, or nullopt outside both vectors.
std::optional<ImageTarget> arithProfileTargetIn(UnlinkedCodeBlock&, const void* profile);

// Records every reference one baseline compilation or one MathIC snippet emits, as a fixup at a label of its assembler.
// Only the thread that runs the compilation or regeneration uses it. It reads the UCB and the VM's addresses, and never
// allocates a cell, takes a JSC lock or reads a CB.
//
// Every byte it or its baked-facts builder allocates is charged to its budget before the allocation, in steps of
// kRecordChargeStep. A refused charge makes it Incomplete and markUnrecordable makes it Unrecordable; either stops
// recording for good. Once stopped, the recording functions and the builder store nothing, the helpers of
// ImageEmission.h keep emitting their recorded forms, and a conditional external jump on ARM64 is linked natively at
// once instead of through a veneer, so every branch is linked whatever the budget answers.
class ImageRecorder final : private BakedFactsBuilder::StorageAllowance {
    WTF_MAKE_NONCOPYABLE(ImageRecorder);
    WTF_MAKE_TZONE_ALLOCATED(ImageRecorder);
public:
    ImageRecorder(RecordingScope, VM&, UnlinkedCodeBlock&, Ref<ProducerBudget>&&);
#if ENABLE(JITCACHE_TWINS)
    // A twin compile: it seeds its assembler and each BinarySwitch with the producer's seeds, and answers what emission
    // asks from the producer's compile inputs. Both must outlive the recorder.
    ImageRecorder(RecordingScope, VM&, UnlinkedCodeBlock&, Ref<ProducerBudget>&&, const TwinSeeds&, const TwinCompileInputs&);
#endif
    ~ImageRecorder(); // Releases every byte it charged and did not hand to a record.

    // Before anything is emitted. A twin recorder also seeds the assembler's random source.
    void attachTo(AbstractMacroAssemblerBase&);

    RecordingScope scope() const { return m_scope; }
    RecordState state() const { return m_state; }
    bool isRecording() const { return m_state == RecordState::Complete; }
    Unrecordable unrecordableReason() const { return m_unrecordableReason; } // None unless the state is Unrecordable.
    // BaselineCompile: whether the first step, which pays for the record object, was charged. Without it the
    // compilation attaches no record.
    bool chargedRecordObject() const { return m_chargedRecordObject; }
    VM& vm() const { return m_vm; }
    UnlinkedCodeBlock& unlinkedCodeBlock() const { return m_unlinkedCodeBlock; }

    // Emission. recordPointer, recordCall, recordJump and deferConditionalJump are called only by the helpers of
    // ImageEmission.h; the rest by those helpers or by the call sites the census names. The labels are the assembler's,
    // before compaction; finishing translates them.
    void recordPointer(AssemblerLabel site, const ImageTarget&);
    void recordCall(AssemblerLabel site, const ImageTarget&);
    void recordJump(AssemblerLabel site, const ImageTarget&);
    // Links a conditional jump to code outside its allocation. On ARM64 it goes through a veneer emitVeneers emits, or
    // is linked natively at once when the recorder has stopped or the veneer's storage cannot be charged. On x86_64 it
    // is a jcc rel32, linked natively with its fixup at its own site.
    void deferConditionalJump(MacroAssembler&, MacroAssembler::Jump, const ImageTarget&, CodeLocationLabel<NoPtrTag> nativeTarget);
    // Before the LinkBuffer: one veneer per deferred target, linked to its native location, with a Jump fixup while the
    // recorder still records. Emits nothing on x86_64.
    void emitVeneers(MacroAssembler&);
    unsigned noteMathIC(MathICKind, const void* mathIC, BytecodeIndex); // BaselineCompile only; returns the MathIC index
    ImageTarget mathICTarget(const void* mathIC) const; // the MathIC target of a noted IC; RELEASE_ASSERTs it was noted
    std::optional<ImageTarget> arithProfileTarget(const void* profile) const; // arithProfileTargetIn of the recorder's UCB
    std::optional<ImageTarget> vmAddressTarget(const void* address) const; // the VMAddress target of address, if any
    void markUnrecordable(Unrecordable);
    // BaselineCompile only. The builder asks the recorder before it stores anything, and has its growth charged first.
    BakedFactsBuilder& bakedFacts();

    bool isLinkingSupport() const { return m_linkingSupport; }
    class SupportLinkScope; // Sets isLinkingSupport() while an emission helper links to support code.

#if ENABLE(JITCACHE_TWINS)
    void didInitializeRandom(uint32_t seed); // From AbstractMacroAssemblerBase::initializeRandom: the assembler's seed.
    uint32_t binarySwitchSeed(uint32_t drawn); // Records and returns drawn; a twin returns the producer's next seed.
    void recordCompileInputs(TwinCompileInputs&&); // BaselineCompile only, at compile start.
    // BaselineCompile only, from the strict-equality templates: records chosen as the instruction's compile input and
    // returns it; a twin recorder returns the producer's input for the instruction instead.
    StrictEqualityAtomOperand strictEqualityAtomOperand(BytecodeIndex, StrictEqualityAtomOperand chosen);
    // The seed the attached assembler drew or was given, which a MathIC regeneration logs for its attach slot.
    std::optional<uint32_t> assemblerSeed() const { return m_assemblerSeed; }
#endif

    // Finishing.
    //
    // BaselineCompile only, from JIT::link once the code is linked, the link tasks have run and the BaselineJITCode is
    // built (SPEC-image.md section 4.7): records each far call with a callee as an Operation fixup at its pointer
    // placeholder, translates every label to its offset in the linked code, puts the fixups in footprint order and checks
    // them, builds the MathIC entries from each noted IC's locations, marks the record NotShareable when the code is not,
    // and returns the record JIT::link attaches to the code, holding exactly what it charged. A recorder that stopped
    // returns a record holding only its state and reason, and one whose first charge was refused returns null.
    std::unique_ptr<ImageRecord> finishBaselineCompile(LinkBuffer&, BaselineJITCode&, std::span<const FarCallRecord> farCalls);
    // MathICSnippet only, after the snippet's LinkBuffer finalized: its fixups translated, sorted and checked, the
    // allocation's start and the linked size, or nullopt once the recorder has stopped. The fixup storage stays charged
    // to this recorder until handChargeToRecord hands it to the record that keeps the provenance.
    std::optional<SnippetProvenance> finishSnippet(LinkBuffer&);
    // Once finishing has moved the recorder's storage into a record: moves bytes of the recorder's charge to that record,
    // which now holds what they paid for, and releases the rest of the charge.
    void handChargeToRecord(size_t bytes);

private:
    struct NotedMathIC {
        MathICKind kind;
        BytecodeIndex bytecodeIndex;
        const void* mathIC;
    };

    struct VeneerGroup {
        ImageTarget target;
        CodeLocationLabel<NoPtrTag> nativeTarget;
        Vector<MacroAssembler::Jump> jumps;
    };

    void record(AssemblerLabel site, FixupForm, const ImageTarget&);
    void linkJumpNatively(MacroAssembler&, MacroAssembler::Jump, CodeLocationLabel<NoPtrTag>);

    // BakedFactsBuilder::StorageAllowance: whether the builder may store, charging bytes of new storage first.
    bool allowStorage(size_t bytes) final;

    // Charges until the budget covers heldBytes. A refusal makes the recorder Incomplete.
    bool chargeFor(size_t heldBytes);
    // Grows vector to hold additional more elements, charging the new capacity first.
    template<typename T> bool reserveRecorded(Vector<T>&, size_t additional, size_t minimumCapacity);
    template<typename T> void forgetRecorded(Vector<T>&);
    void stop(RecordState);

    // Translates every recorded site, a label offset before compaction, to its offset in the linked code.
    void translateSites(LinkBuffer&);
    // Puts the fixups in footprint order (SPEC-image.md section 3.2) and checks that each footprint holds its form's
    // instruction and lies inside the linked code, at or after the end of the one before it. A violation is a programming
    // error, which makes the recorder Unrecordable(InconsistentRecord).
    bool orderAndCheckFixups(std::span<const uint8_t> linkedCode);
    // Step 1 of section 4.7: a Pointer fixup with an Operation target at the placeholder of each far call with a callee,
    // its site already an offset in the linked code.
    void recordFarCalls(LinkBuffer&, std::span<const FarCallRecord>);
    // Step 3 of section 4.7, on fixups in footprint order: one entry per noted MathIC, with the site of its slow call's
    // Operation fixup when it has inline code. Empty once the recorder stops.
    Vector<MathICRecord> buildMathICRecords(std::span<const uint8_t> linkedCode);

    RecordingScope m_scope;
    RecordState m_state { RecordState::Complete };
    Unrecordable m_unrecordableReason { Unrecordable::None };
    bool m_linkingSupport { false };
    bool m_chargedRecordObject { false };
    VM& m_vm;
    UnlinkedCodeBlock& m_unlinkedCodeBlock;
    Ref<ProducerBudget> m_budget;
    size_t m_chargedBytes { 0 };
    size_t m_heldBytes { 0 }; // What the recorder's containers and its builder hold, and a record object it will attach.

    Vector<ImageFixup> m_fixups; // Sites are pre-compaction label offsets until translateSites.
    Vector<VeneerGroup> m_veneerGroups;
    Vector<NotedMathIC> m_mathICs;
    unsigned m_mathICCount { 0 };
    mutable size_t m_mathICSearchStart { 0 };
    BakedFactsBuilder m_bakedFacts { this };

#if ENABLE(JITCACHE_TWINS)
    std::optional<uint32_t> m_assemblerSeed;
    Vector<uint32_t> m_binarySwitchSeeds;
    const TwinSeeds* m_twinSeeds { nullptr };
    const TwinCompileInputs* m_twinCompileInputs { nullptr };
    size_t m_nextTwinBinarySwitchSeed { 0 };
#endif
};

class ImageRecorder::SupportLinkScope {
    WTF_MAKE_NONCOPYABLE(SupportLinkScope);
public:
    explicit SupportLinkScope(ImageRecorder& recorder)
        : m_recorder(recorder)
        , m_wasLinkingSupport(std::exchange(recorder.m_linkingSupport, true))
    {
    }

    ~SupportLinkScope()
    {
        m_recorder.m_linkingSupport = m_wasLinkingSupport;
    }

private:
    ImageRecorder& m_recorder;
    bool m_wasLinkingSupport;
};

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
