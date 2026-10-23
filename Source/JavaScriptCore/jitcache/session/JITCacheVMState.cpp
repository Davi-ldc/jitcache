#include "config.h"
#include "jitcache/session/JITCacheVMState.h"

#include "jitcache/base/BenchClock.h"
#include "jitcache/session/JITCacheBench.h"
#include "jitcache/store/ArtifactStore.h"
#include "jitcache/store/ArtifactWriter.h"
#include "jitcache/store/JITCacheFiles.h"
#include "jitcache/store/JITCacheParameters.h"
#include "jitcache/ucb/UCBRegistry.h"
#include <utility>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

// Every VMState member: the constructor, the destructor and the release of production memory; the switches, the fault
// records and the members that raise faults; the producer context; the body lookups with their test override; what the
// glue, the hosts and status read and write; and the per-VM parts. Destroying a state runs the destructors of the types
// it holds through pointers, so this file includes their headers; a part's destructor is virtual, so it includes no
// part's header. start and the teardown hooks, which create and destroy the state, are in JITCacheAPI.cpp.

namespace JSC::JITCache {

WTF_MAKE_TZONE_ALLOCATED_IMPL(VMState);

namespace JITCacheVMStateInternal {

// The detail of invalid material an open raises: the errno's text for container.io, and the body's key either way.
static String invalidBodyDetail(const BodyKey& key, const StoreFailure& failure)
{
    if (failure.error)
        return makeString(errorText(failure.error), "; body "_s, bodyKeyHex(key));
    return makeString("body "_s, bodyKeyHex(key));
}

} // namespace JITCacheVMStateInternal

VMState::VMState(const Config& config, StartParts&& parts)
    : m_config(config)
    , m_startOutcome(parts.outcome)
    , m_activityOn(parts.outcome != StartOutcome::Fault)
    , m_productionActive(parts.outcome != StartOutcome::Fault && config.role != Role::Consumer)
    , m_activityFault(WTF::move(parts.startFault))
    , m_benchReport(WTF::move(parts.benchReport))
#if ENABLE(JITCACHE_TWINS)
    , m_twinReport(WTF::move(parts.twinReport))
#endif
    , m_registry(makeUnique<UCBRegistry>())
    , m_producerLock(WTF::move(parts.producerLock))
    , m_artifact(WTF::move(parts.artifact))
{
    // A start fault is the first activity fault, and production, off whenever activity is, never begins.
    ASSERT(m_startOutcome == StartOutcome::Started || m_startOutcome == StartOutcome::Fault);
    ASSERT((m_startOutcome == StartOutcome::Fault) == !!m_activityFault);
    ASSERT(!!parts.producerLimitBytes == producing());
    ASSERT((m_startOutcome != StartOutcome::Fault) == !!m_artifact);
    // Every role's report reads its own artifact's index statistics at each flush.
    if (m_benchReport && m_artifact)
        m_benchReport->setArtifact(m_artifact.copyRef());
    if (!parts.producerLimitBytes)
        return;

    // The producing roles' budget and its one context, whose limit and charges each flush's budget line writes. The writer
    // refers to the budget, the producer lock and the artifact, all declared before it, so the destructor destroys it
    // first.
    Ref<ProducerBudget> budget = ProducerBudget::create(*parts.producerLimitBytes);
    m_budget = budget.copyRef();
    m_producerContext.emplace(budget.copyRef());
    if (m_benchReport)
        m_benchReport->setBudget(budget.copyRef());
    if (m_producerLock && m_artifact)
        m_writer = makeUnique<ArtifactWriter>(*m_artifact, *m_producerLock, budget.get(), writerStagingBytes);
}

VMState::~VMState()
{
    // didFinalizeHeap destroys the state once willDestroyVM has ended production and released its memory. A state
    // destroyed without that still releases each production charge once, and then its members go in the reverse order
    // of their declaration: the parts left first, the writer before the producer lock, which releases the lock, the
    // opened artifact and the budget it refers to, and the registry after every UCB and UFE destructor has run.
    m_productionActive.store(false, std::memory_order_release);
    releaseEndedProductionMemory();
}

void VMState::releaseEndedProductionMemory()
{
    // A flag test while production is active and once the memory is released.
    if (m_productionMemoryReleased || productionActive())
        return;
    m_productionMemoryReleased = true;
    // The production parts first: the kept summaries release the charges of their entries and buckets.
    destroyParts(VMPartLifetime::UntilProductionEnds);
    if (m_writer)
        m_writer->releaseStagingBuffer();
    // The entries themselves stay in the index the process's VMs share, for the VMs that import; only their charge
    // ends with production.
    if (m_indexEntryChargeBytes)
        m_budget->release(std::exchange(m_indexEntryChargeBytes, 0));
}

bool VMState::tracksKeys() const
{
    return activityOn();
}

bool VMState::importsEnabled() const
{
    return (role() == Role::Consumer || role() == Role::ConsumerProducer) && activityOn();
}

bool VMState::productionActive() const
{
    // Production is off whenever activity is. Both switches are read, so a reader on another thread that sees a
    // debugger's attach on one of them sees production ended.
    return producing() && m_productionActive.load(std::memory_order_acquire) && activityOn();
}

DeltaCapture VMState::deltaCapture() const
{
    return m_config.deltaCapture;
}

bool VMState::capturesUncompiledBodies() const
{
    return deltaCapture() == DeltaCapture::All && productionActive();
}

UCBRegistry& VMState::registry()
{
    return *m_registry;
}

std::optional<BodyKey> VMState::bodyKeyOf(const UnlinkedCodeBlock& codeBlock) const
{
    return m_registry->keyOf(codeBlock);
}

uint64_t VMState::bodyVersion(const BodyKey& key)
{
    if (!activityOn())
        return 0;
#if ENABLE(JITCACHE_TWINS)
    if (m_bodyVersionOverride)
        return m_bodyVersionOverride(key);
#endif
    // Activity is on only after a start that did not fault, which leaves the opened artifact.
    OpenedArtifact* artifact = m_artifact.get();
    ASSERT(artifact);

    // The index's token, which needs no system call unless the pack changed since the last read.
    BenchReport* report = m_benchReport.get();
    uint64_t startNanoseconds = report ? benchThreadCPUNanoseconds() : 0;
    uint64_t token = artifact->token(key);
    if (report)
        report->countLookup(token ? BenchReport::Lookup::BodyVersionPresent : BenchReport::Lookup::BodyVersionAbsent, benchThreadCPUNanoseconds() - startNanoseconds);
    return token;
}

BodyLookup VMState::openBody(const BodyKey& key)
{
    if (!activityOn())
        return BodyLookup::unusable();
#if ENABLE(JITCACHE_TWINS)
    if (m_openBodyOverride)
        return m_openBodyOverride(key);
#endif
    OpenedArtifact* artifact = m_artifact.get();
    ASSERT(artifact);

    BenchReport* report = m_benchReport.get();
    uint64_t startNanoseconds = report ? benchThreadCPUNanoseconds() : 0;
    BodyOpen opened = artifact->open(key);
    if (opened.bodyBytes)
        ++m_progress.bodyOpens;

    BenchReport::Lookup tally = BenchReport::Lookup::OpenBodyMissing;
    BodyLookup lookup = [&] {
        switch (opened.outcome) {
        case StoreOutcome::Found:
            tally = BenchReport::Lookup::OpenBodyFound;
            return BodyLookup::found(opened.body.releaseNonNull());
        case StoreOutcome::Absent:
            // A file that vanished included; the store took the key out of its index unless its token changed meanwhile.
            return BodyLookup::missing();
        case StoreOutcome::Unavailable:
            // EMFILE, ENFILE or ENOMEM: the artifact is not at fault, and the body is as good as none for now.
            ++m_progress.transientOpenFailures;
            return BodyLookup::missing();
        case StoreOutcome::Invalid:
            // The UCB lane reads Unusable as "the integrator raised the fault".
            raiseInvalidMaterial(opened.failure.check, JITCacheVMStateInternal::invalidBodyDetail(key, opened.failure));
            tally = BenchReport::Lookup::OpenBodyUnusable;
            return BodyLookup::unusable();
        }
        RELEASE_ASSERT_NOT_REACHED();
        return BodyLookup::unusable();
    }();

    if (report) {
        uint64_t nanoseconds = benchThreadCPUNanoseconds() - startNanoseconds;
        report->countLookup(tally, nanoseconds);
        // The bench report's open event: each openBody that reached a body.
        if (opened.bodyBytes)
            report->record("open"_s, { { "key"_s, bodyKeyHex(key) }, { "bytes"_s, opened.bodyBytes }, { "nanoseconds"_s, nanoseconds } });
    }
    return lookup;
}

SectionLookup VMState::readBodySection(const BodyKey& key, SectionKind kind)
{
    using Kind = SectionLookup::Kind;
    if (!activityOn())
        return SectionLookup { Kind::Unusable, nullptr, nullptr, { } };
#if ENABLE(JITCACHE_TWINS)
    // The override's body, whose section no checksum covers, as the override's open gives it.
    if (m_openBodyOverride) {
        BodyLookup lookup = m_openBodyOverride(key);
        switch (lookup.kind()) {
        case BodyLookup::Kind::Missing:
            return SectionLookup { Kind::Absent, nullptr, nullptr, { } };
        case BodyLookup::Kind::Unusable:
            return SectionLookup { Kind::Unusable, nullptr, nullptr, { } };
        case BodyLookup::Kind::Found: {
            RefPtr<ValidatedBody> body = lookup.body();
            std::span<const uint8_t> bytes = body->section(kind);
            return SectionLookup { Kind::Found, nullptr, WTF::move(body), bytes };
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
#endif
    OpenedArtifact* artifact = m_artifact.get();
    ASSERT(artifact);

    // One section, checksummed alone; the module's request opens the whole body later. It counts neither as a body open
    // nor as a lookup: the UCB lane's module-analysis event times it.
    SectionRead read = artifact->readSection(key, kind);
    switch (read.outcome) {
    case StoreOutcome::Found:
        return SectionLookup { Kind::Found, WTF::move(read.pack), nullptr, read.section };
    case StoreOutcome::Absent:
        return SectionLookup { Kind::Absent, nullptr, nullptr, { } };
    case StoreOutcome::Unavailable:
        // No fault hook reaches the read; a body it could not read is as good as none.
        ASSERT_NOT_REACHED();
        return SectionLookup { Kind::Absent, nullptr, nullptr, { } };
    case StoreOutcome::Invalid:
        // The UCB lane reads Unusable as "the integrator raised the fault", as for openBody.
        raiseInvalidMaterial(read.failure.check, JITCacheVMStateInternal::invalidBodyDetail(key, read.failure));
        return SectionLookup { Kind::Unusable, nullptr, nullptr, { } };
    }
    RELEASE_ASSERT_NOT_REACHED();
    return SectionLookup { Kind::Unusable, nullptr, nullptr, { } };
}

void VMState::raiseInvalidMaterial(ASCIILiteral step, String detail)
{
    turnActivityOff(FaultReport { FaultClass::InvalidMaterial, { }, step, WTF::move(detail) });
}

void VMState::raiseInvalidMaterial(ASCIILiteral part, ASCIILiteral check, String detail)
{
    turnActivityOff(FaultReport { FaultClass::InvalidMaterial, part, check, WTF::move(detail) });
}

void VMState::raiseRecordingFault(ASCIILiteral part, ASCIILiteral check, String detail)
{
    // A recording fault ends production for good and leaves activity on, so a ConsumerProducer goes on importing. Its
    // budget refuses every later charge, so recorders still running on JIT workers fail their next charge and leave
    // their records incomplete.
    if (m_productionActive.exchange(false, std::memory_order_acq_rel) && !m_productionFault)
        m_productionFault = FaultReport { FaultClass::RecordingFault, part, check, WTF::move(detail) };
    if (m_budget)
        m_budget->refuseFurtherCharges();
}

#if ENABLE(JITCACHE_TWINS)
TwinReportSink* VMState::twinReportSink()
{
    return m_twinReport.get();
}

Ref<ProducerBudget> VMState::twinBudget()
{
    return m_twinBudget.copyRef();
}

void VMState::setBodyLookupForTesting(Function<uint64_t(const BodyKey&)>&& token, Function<BodyLookup(const BodyKey&)>&& open)
{
    // The override answers alone, without the store, the index or a progress counter, and leaves the role as it is;
    // the activity test still runs first.
    ASSERT(token && open);
    m_bodyVersionOverride = WTF::move(token);
    m_openBodyOverride = WTF::move(open);
}

void VMState::clearBodyLookupForTesting()
{
    m_bodyVersionOverride = nullptr;
    m_openBodyOverride = nullptr;
}
#endif

Role VMState::role() const
{
    return m_config.role;
}

bool VMState::producing() const
{
    return role() == Role::Producer || role() == Role::ConsumerProducer;
}

bool VMState::activityOn() const
{
    return m_activityOn.load(std::memory_order_acquire);
}

ProducerContext* VMState::producerContextIfActive()
{
    // The context is built before the state is published and lives until the state is destroyed, so a JIT worker that
    // obtained it may keep using it after production ends.
    if (!m_producerContext || !productionActive())
        return nullptr;
    return &m_producerContext.value();
}

OpenedArtifact* VMState::artifact()
{
    return m_artifact.get();
}

BenchReport* VMState::benchReport()
{
    return m_benchReport.get();
}

StartOutcome VMState::startOutcome() const
{
    return m_startOutcome;
}

std::optional<FaultReport> VMState::activityFault() const
{
    if (m_activityFault)
        return m_activityFault;
    // A debugger's attach records no report, since it may run on any thread. A fault recorded after it found activity
    // already off and recorded nothing, so the attach came first.
    if (m_debuggerAttached.load(std::memory_order_acquire))
        return FaultReport { FaultClass::DebuggerAttached, { }, "debugger.attach"_s, { } };
    return std::nullopt;
}

std::optional<FaultReport> VMState::productionFault() const
{
    if (!producing())
        return std::nullopt;
    if (m_productionFault)
        return m_productionFault;
    if (productionActive()) {
        // A charge a JIT worker made was refused, which the VM thread raises at its next capture or delta.
        // A recording fault refuses further charges too, but it records itself first.
        if (m_budget && m_budget->hasRefused())
            return FaultReport { FaultClass::RecordingFault, { }, "budget.limit"_s, { } };
        return std::nullopt;
    }
    // Production is off whenever activity is, and the two ways activity goes off without recording a production
    // fault are a start fault, when production never began, and a debugger's attach.
    return activityFault();
}

std::optional<FaultReport> VMState::firstFault() const
{
    // A production fault is never recorded after the activity fault (turnActivityOff), and the two productionFault
    // reports without a record come either while activity is still on or as the activity fault itself.
    if (auto fault = productionFault())
        return fault;
    return activityFault();
}

ProductionState VMState::productionState() const
{
    if (!producing())
        return ProductionState::NotProducing;
    // A refusal the VM thread has not raised yet already ends production as status reports it.
    if (productionActive() && !(m_budget && m_budget->hasRefused()))
        return ProductionState::Active;
    return ProductionState::Ended;
}

Progress& VMState::progress()
{
    return m_progress;
}

Progress VMState::statusProgress()
{
    Progress result = m_progress;
    if (OpenedArtifact* artifact = m_artifact.get())
        result.indexedBodies = artifact->indexedBodies();
    const UCBStatistics& statistics = m_registry->statistics();
    result.imports = statistics.imports;
    result.attaches = statistics.attaches;
    result.gateDrops = statistics.gateDrops;
    result.misses = 0;
    for (uint64_t misses : statistics.misses)
        result.misses += misses;
    return result;
}

ProducerBudget* VMState::producerBudget()
{
    return m_budget.get();
}

ArtifactWriter* VMState::writer()
{
    return m_writer.get();
}

void VMState::addIndexEntryCharge(size_t bytes)
{
    ASSERT(m_budget);
    ASSERT(!m_productionMemoryReleased);
    m_indexEntryChargeBytes += bytes;
}

#if ASSERT_ENABLED
unsigned& VMState::capturesInProgress()
{
    return m_capturesInProgress;
}
#endif

VMPart* VMState::findPart(const VMPartKey& key)
{
    for (auto& entry : m_parts) {
        if (entry.key == &key)
            return entry.part.get();
    }
    return nullptr;
}

VMPart& VMState::addPart(const VMPartKey& key, std::unique_ptr<VMPart>&& newPart)
{
    // A production part is created only while production is active, before releaseEndedProductionMemory can destroy it.
    ASSERT(!findPart(key));
    ASSERT(key.lifetime != VMPartLifetime::UntilProductionEnds || !m_productionMemoryReleased);
    VMPart& added = *newPart;
    m_parts.append(PartEntry { &key, WTF::move(newPart) });
    return added;
}

void VMState::destroyParts(VMPartLifetime lifetime)
{
    // Each part is destroyed in place while the list closes up, so no part's destructor may reach the state's parts.
    m_parts.removeAllMatching([&](const PartEntry& entry) {
        return entry.key->lifetime == lifetime;
    });
}

void VMState::turnActivityOff(FaultReport&& report)
{
    bool activityWasOn = m_activityOn.exchange(false, std::memory_order_acq_rel);
    bool productionWasActive = m_productionActive.exchange(false, std::memory_order_acq_rel);
    if (productionWasActive && !m_productionFault)
        m_productionFault = report;
    if (activityWasOn && !m_activityFault)
        m_activityFault = WTF::move(report);
}

void VMState::noteDebuggerAttached()
{
    // The flag goes first, so a thread whose acquire load sees activity off also sees why.
    m_debuggerAttached.store(true, std::memory_order_release);
    m_activityOn.store(false, std::memory_order_release);
    m_productionActive.store(false, std::memory_order_release);
}

} // namespace JSC::JITCache
