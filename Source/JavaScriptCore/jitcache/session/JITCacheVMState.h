#pragma once

#include "jitcache/base/BodyKey.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/base/VMPart.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/JITCacheFaults.h"
#include "jitcache/session/ProducerContext.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/twins/TwinReport.h"
#include <atomic>
#include <concepts>
#include <memory>
#include <optional>
#include <utility>
#include <wtf/Function.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

// The per-VM state, reached through vm.jitCacheState(). start stores the pointer once; didFinalizeHeap stores null and
// then destroys the state. Every reader runs either on the VM thread, which wrote the pointer, or on a JIT worker through
// producerContext, whose acquire load sees a fully constructed state. The types the state holds through pointers are
// only declared here, and the parts other files keep per VM are types of their own files (VMPart.h), so a lane that
// includes this header compiles against none of their headers.

namespace JSC {

class UnlinkedCodeBlock;

} // namespace JSC

namespace JSC::JITCache {

class ArtifactWriter;
class BenchReport;
class OpenedArtifact;
class ProducerLock;
class UCBRegistry; // the UCB lane's registry; its users include UCBRegistry.h

class VMState final {
    WTF_MAKE_NONCOPYABLE(VMState);
    WTF_MAKE_TZONE_ALLOCATED(VMState);
public:
    // The lanes' interface. VM thread unless noted.
    bool tracksKeys() const; // activity on (any thread)
    bool importsEnabled() const; // role Consumer or ConsumerProducer, and activity on (any thread)
    bool productionActive() const; // producing, activity on and production not ended (any thread)
    DeltaCapture deltaCapture() const; // Config::deltaCapture, which start checked (any thread)
    // Production is active with the delta capture All: delta also captures the bodies that ran without compiling, so
    // function UCB records keep their holder UFE.
    bool capturesUncompiledBodies() const;
    UCBRegistry& registry();
    // The key the UCB lane recorded for the UCB, if any. It takes only the registry's lock, a leaf.
    std::optional<BodyKey> bodyKeyOf(const UnlinkedCodeBlock&) const;
    uint64_t bodyVersion(const BodyKey&);
    BodyLookup openBody(const BodyKey&);
    SectionLookup readBodySection(const BodyKey&, SectionKind); // one section, checksummed without the body's others
    void raiseInvalidMaterial(ASCIILiteral step, String detail); // step is a full name, such as "ucb.identity"
    void raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail);
    void raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail);
#if ENABLE(JITCACHE_TWINS)
    TwinReportSink* twinReportSink(); // non-null while a twin report is open
    Ref<ProducerBudget> twinBudget(); // ProducerBudget::createUnlimited(), for twin compiles
    // Tests: while set, bodyVersion, openBody and readBodySection answer from these alone; token answers index tokens.
    void setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open);
    void clearBodyLookupForTesting();
#endif

    // The integrator's own state.
    Role role() const;
    bool producing() const; // Producer or ConsumerProducer
    bool activityOn() const;
    ProducerContext* producerContextIfActive(); // any thread
    void releaseEndedProductionMemory(); // VM thread; nothing while production is active or once it is released
    OpenedArtifact* artifact(); // null after a start fault
    BenchReport* benchReport(); // null unless Config::benchReportPath was set
    ~VMState();

    // What the glue, the hosts and status read and write. VM thread.
    StartOutcome startOutcome() const; // Started or Fault
    // The faults status reports, which delta returns too. activityFault is the recorded one, or debugger.attach when a
    // debugger turned activity off before any fault was recorded. productionFault, for a producing role only, is the
    // recorded one; while production is active and no fault is recorded, budget.limit once the budget has refused a
    // charge the VM thread has not raised yet; and once production went off with activity without a record of its own,
    // the activity fault, a start fault or a debugger's attach. firstFault is the earlier of the two, which is the
    // production fault whenever there is one.
    std::optional<FaultReport> activityFault() const;
    std::optional<FaultReport> productionFault() const;
    std::optional<FaultReport> firstFault() const;
    ProductionState productionState() const; // Ended once productionFault reports a fault
    // The integrator's progress counters, which the glue counts.
    Progress& progress();
    // What status reports as progress: a copy of the counters with the opened artifact's indexed bodies and the UCB
    // registry's imports, attaches, gate drops and misses.
    Progress statusProgress();
    ProducerBudget* producerBudget(); // producing roles, a faulted start included; null otherwise
    ArtifactWriter* writer(); // producing roles whose start did not fault; null otherwise
    // The charge of an index entry the writer adds for a key the index lacks, which the glue already made against the
    // producer budget. The end of production releases it.
    void addIndexEntryCharge(size_t bytes);
#if ASSERT_ENABLED
    unsigned& capturesInProgress(); // the finalize capture and delta assert on entry that it is zero
#endif
    // The parts other files keep per VM (VMPart.h). part returns the VM's part of that type, which it creates from the
    // arguments at the type's first use; later calls ignore them. A production part is created only while production
    // memory is held. partIfExists returns null before the first use and once the part's lifetime has ended.
    template<typename Part, typename... Arguments> Part& part(Arguments&&...);
    template<typename Part> Part* partIfExists();

private:
    // start constructs the state, and willDestroyVM destroys its VM parts, closes its twin report and ends its
    // production before the heap's last finalization.
    friend StartResult start(VM&, const Config&);
    friend void willDestroyVM(VM&);
    // The entry points of JITCacheFaults.h turn the switches off through the two members below.
    friend void didFailExecutableAllocation(VM&, ExecutableAllocationSite);
    friend void didAttachDebugger(VM&);

    // What start hands the constructor. The outcome is Started or Fault; a Fault carries the start fault, which
    // becomes the first activity fault, and neither a producer lock nor an artifact. A producing role carries its limit.
    struct StartParts {
        StartOutcome outcome { StartOutcome::Fault };
        std::optional<FaultReport> startFault;
        std::optional<size_t> producerLimitBytes;
        std::unique_ptr<ProducerLock> producerLock;
        RefPtr<OpenedArtifact> artifact;
        std::unique_ptr<BenchReport> benchReport;
#if ENABLE(JITCACHE_TWINS)
        std::unique_ptr<TwinReport> twinReport;
#endif
    };
    // Creates the registry and, for a producing role, the producer budget, the context and, over the lock and the
    // artifact, the writer, and hands the budget to the bench report.
    VMState(const Config&, StartParts&&);

    // VM thread. Turns activity off, which ends production. The report becomes the first activity fault
    // when activity was on and the first production fault when production was active; a switch already off keeps the
    // fault that turned it off, or, when a debugger turned it off, the debugger flag. A production fault is therefore
    // never recorded after the activity fault, so the earlier of the two is the production fault whenever there is one.
    // Plain stores and atomic exchanges only: nothing allocates, frees, locks or waits.
    void turnActivityOff(FaultReport&&);
    // Any thread, atomic stores only: the debugger flag, then both switches off. status reads the flag in place of a
    // recorded activity fault.
    void noteDebuggerAttached();

    // The part with the key, or null. addPart takes a key no part has.
    VMPart* findPart(const VMPartKey&);
    VMPart& addPart(const VMPartKey&, std::unique_ptr<VMPart>&&);
    // Destroys every part of the lifetime: releaseEndedProductionMemory calls it for the production parts, and
    // willDestroyVM for the others.
    void destroyParts(VMPartLifetime);

    // The role, the config and the start outcome.
    const Config m_config;
    const StartOutcome m_startOutcome;

    // The switches. Each goes from on to off only; they are turned off with release order and read with
    // acquire order, since JIT workers read production through producerContext and a debugger may attach from any thread.
    // Production starts on only for a producing role whose start did not fault.
    std::atomic<bool> m_activityOn { false };
    std::atomic<bool> m_productionActive { false };
    std::atomic<bool> m_debuggerAttached { false };

    // The first fault of each switch and the progress counters, which the VM thread writes. A start fault is the first
    // activity fault. A refusal a JIT worker's charge made and the VM thread has not raised yet has no record here, and
    // status reports it from the budget as budget.limit. The progress counters are the integrator's; status adds the UCB
    // registry's statistics.
    std::optional<FaultReport> m_activityFault;
    std::optional<FaultReport> m_productionFault;
    Progress m_progress;

    // Producing roles: the producer budget and the one context producerContext returns.
    RefPtr<ProducerBudget> m_budget;
    std::optional<ProducerContext> m_producerContext;

    std::unique_ptr<BenchReport> m_benchReport; // opened by start; flushed by the hosts' exit calls and willDestroyVM

#if ENABLE(JITCACHE_TWINS)
    std::unique_ptr<TwinReport> m_twinReport; // opened by start; willDestroyVM closes it
    const Ref<ProducerBudget> m_twinBudget { ProducerBudget::createUnlimited() };
#endif

    // The UCB lane's registry, which the constructor creates and the destructor destroys after every UCB and UFE
    // destructor has run. A pointer keeps UCBRegistry.h out of this header; its place among the members keeps the order
    // of destruction.
    const std::unique_ptr<UCBRegistry> m_registry;

    std::unique_ptr<ProducerLock> m_producerLock; // producing roles; destroying it releases the lock
    RefPtr<OpenedArtifact> m_artifact; // shared by the VMs of the process that open the artifact; null after a start fault

#if ENABLE(JITCACHE_TWINS)
    // The body-lookup override: while set, bodyVersion answers from the first, and openBody and readBodySection from the
    // second.
    Function<uint64_t(const BodyKey&)> m_bodyVersionOverride;
    Function<BodyLookup(const BodyKey&)> m_openBodyOverride;
#endif

    // Producing roles. Declared after the producer lock, the opened artifact and the budget, so the destructor destroys
    // the writer, which refers to all three, first.
    std::unique_ptr<ArtifactWriter> m_writer;

    // Production memory besides the production parts: the charge for the index entries the writer added. The end of
    // production releases it, with the production parts and the writer's staging buffer, once.
    size_t m_indexEntryChargeBytes { 0 };
    bool m_productionMemoryReleased { false };

#if ASSERT_ENABLED
    unsigned m_capturesInProgress { 0 }; // the finalize capture and delta assert that none is in progress
#endif

    // The parts, each with the key it was created under. The state has room for the three part types JITCache has
    // today, so its parts allocate no buffer; a fourth spills to the heap. Declared last, so a state destroyed with
    // parts left destroys them before every other member.
    struct PartEntry {
        const VMPartKey* key;
        std::unique_ptr<VMPart> part;
    };
    Vector<PartEntry, 3> m_parts;
};

template<typename Part, typename... Arguments>
Part& VMState::part(Arguments&&... arguments)
{
    static_assert(std::derived_from<Part, VMPart>);
    if (VMPart* existing = findPart(vmPartKey<Part>))
        return static_cast<Part&>(*existing);
    return static_cast<Part&>(addPart(vmPartKey<Part>, makeUnique<Part>(std::forward<Arguments>(arguments)...)));
}

template<typename Part>
Part* VMState::partIfExists()
{
    static_assert(std::derived_from<Part, VMPart>);
    return static_cast<Part*>(findPart(vmPartKey<Part>));
}

} // namespace JSC::JITCache
