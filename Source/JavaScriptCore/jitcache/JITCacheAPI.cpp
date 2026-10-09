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

#include "config.h"
#include "JITCacheAPI.h"

#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "JITCacheBench.h"
#include "JITCacheContainer.h"
#include "JITCacheGlue.h"
#include "JITCacheOptions.h"
#include "JITCacheParameters.h"
#include "JITCachePlatform.h"
#include "JITCacheVMState.h"
#include "ProducerBudget.h"
#include "TwinReport.h"
#include "UCBRegistry.h"
#include "VM.h"
#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

#if OS(LINUX)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// The public interface of THREAD Session apart from delta, which JITCacheCapture.cpp defines (SPEC-integrator.md section
// 3), and the per-VM state's lifecycle (sections 4.1, 4.2 and 4.6): start creates the one VMState a configured VM has,
// releaseEndedProductionMemory frees what production held, and the teardown hooks VM::~VM calls end and destroy the
// state. The state is created and destroyed here because destroying it runs the destructors of every part's types.

namespace JSC::JITCache {

namespace JITCacheAPIInternal {

static ASCIILiteral sessionStateName(SessionState state)
{
    switch (state) {
    case SessionState::Unconfigured:
        return "unconfigured"_s;
    case SessionState::Created:
        return "created"_s;
    case SessionState::Opened:
        return "opened"_s;
    case SessionState::Faulted:
        return "faulted"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral productionStateName(ProductionState state)
{
    switch (state) {
    case ProductionState::NotProducing:
        return "not-producing"_s;
    case ProductionState::Active:
        return "active"_s;
    case ProductionState::Ended:
        return "ended"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

static ASCIILiteral deltaOutcomeName(DeltaOutcome outcome)
{
    switch (outcome) {
    case DeltaOutcome::Completed:
        return "completed"_s;
    case DeltaOutcome::Rejected:
        return "rejected"_s;
    case DeltaOutcome::Faulted:
        return "faulted"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// The JSON lines of section 3.1: every field of the struct by name, enums by their names, an absent optional as null
// and a fault as {"class","step","detail"}.

static void appendKey(StringBuilder& json, ASCIILiteral key)
{
    json.append(",\""_s, key, "\":"_s);
}

static void appendString(StringBuilder& json, ASCIILiteral key, const String& value)
{
    appendKey(json, key);
    json.appendQuotedJSONString(value);
}

static void appendBoolean(StringBuilder& json, ASCIILiteral key, bool value)
{
    appendKey(json, key);
    json.append(value ? "true"_s : "false"_s);
}

static void appendNumber(StringBuilder& json, ASCIILiteral key, uint64_t value)
{
    appendKey(json, key);
    json.append(value);
}

static void appendFault(StringBuilder& json, ASCIILiteral key, const std::optional<FaultReport>& fault)
{
    appendKey(json, key);
    if (!fault) {
        json.append("null"_s);
        return;
    }
    json.append("{\"class\":"_s);
    json.appendQuotedJSONString(String { name(fault->faultClass) });
    json.append(",\"step\":"_s);
    json.appendQuotedJSONString(fault->stepName());
    json.append(",\"detail\":"_s);
    json.appendQuotedJSONString(fault->detail);
    json.append('}');
}

static void appendProgress(StringBuilder& json, const Progress& progress)
{
    // The first field opens the object, so appendNumber's leading comma comes after the brace.
    json.append(",\"progress\":{\"indexedBodies\":"_s, progress.indexedBodies);
    appendNumber(json, "bodyOpens"_s, progress.bodyOpens);
    appendNumber(json, "transientOpenFailures"_s, progress.transientOpenFailures);
    appendNumber(json, "imports"_s, progress.imports);
    appendNumber(json, "seededDecodes"_s, progress.seededDecodes);
    appendNumber(json, "attaches"_s, progress.attaches);
    appendNumber(json, "gateDrops"_s, progress.gateDrops);
    appendNumber(json, "misses"_s, progress.misses);
    appendNumber(json, "installs"_s, progress.installs);
    appendNumber(json, "bakedFactMismatches"_s, progress.bakedFactMismatches);
    appendNumber(json, "captureCandidates"_s, progress.captureCandidates);
    appendNumber(json, "capturesDeferred"_s, progress.capturesDeferred);
    appendNumber(json, "capturesCommitted"_s, progress.capturesCommitted);
    appendNumber(json, "bytesCommitted"_s, progress.bytesCommitted);
    appendNumber(json, "deltaRuns"_s, progress.deltaRuns);
    json.append('}');
}

static void appendBudget(StringBuilder& json, const BudgetSnapshot& budget)
{
    json.append(",\"budget\":{\"limitBytes\":"_s, static_cast<uint64_t>(budget.limitBytes));
    appendNumber(json, "chargedBytes"_s, budget.chargedBytes);
    appendNumber(json, "peakBytes"_s, budget.peakBytes);
    appendBoolean(json, "refused"_s, budget.refused);
    json.append('}');
}

#if OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))

// Steps 3 to 7 of section 3.2 on the platforms JITCache serves. Every descriptor carries O_CLOEXEC, directories are
// created with mode 0755 and files with 0644 (container sub-SPEC section 1.1). The opened artifact pins descriptors of
// its own, so the ones here close when start returns.

static bool isRole(Role role)
{
    return role == Role::Consumer || role == Role::Producer || role == Role::ConsumerProducer;
}

// A descriptor closed when it goes out of scope.
class ScopedDescriptor {
    WTF_MAKE_NONCOPYABLE(ScopedDescriptor);
public:
    ScopedDescriptor() = default;
    explicit ScopedDescriptor(int fd)
        : m_fd(fd)
    {
    }
    ScopedDescriptor(ScopedDescriptor&& other)
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }
    ScopedDescriptor& operator=(ScopedDescriptor&& other)
    {
        if (this != &other) {
            closeIfOpen();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }
    ~ScopedDescriptor() { closeIfOpen(); }

    explicit operator bool() const { return m_fd >= 0; }
    int get() const { return m_fd; }

    // Closes the descriptor and returns 0, or the errno of a close that failed.
    int close()
    {
        if (m_fd < 0)
            return 0;
        return ::close(std::exchange(m_fd, -1)) ? errno : 0;
    }

private:
    void closeIfOpen()
    {
        if (m_fd >= 0)
            ::close(std::exchange(m_fd, -1));
    }

    int m_fd { -1 };
};

static int openAt(int directoryFd, const char* name, int flags, mode_t mode = 0)
{
    int fd;
    do {
        fd = ::openat(directoryFd, name, flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

static int openDirectoryAt(int directoryFd, const char* name)
{
    return openAt(directoryFd, name, O_RDONLY | O_DIRECTORY);
}

static String errorText(int error)
{
    return String::fromUTF8(safeStrerror(error).data());
}

static StartResult rejection(ASCIILiteral step, String&& detail)
{
    return { StartOutcome::Rejected, step, WTF::move(detail) };
}

static StartResult ioFault(const String& path, ASCIILiteral what, int error)
{
    return { StartOutcome::Fault, "start.io"_s, makeString(what, " in "_s, path, ": "_s, errorText(error)) };
}

// A header-less cache/ is reused only when bodies/ holds no body name: its temporaries are removed, and the caller
// creates bodies/ when it is missing (container sub-SPEC section 1.3). Returns the result that stops start, or nothing.
static std::optional<StartResult> resetRemnant(int cacheFd, const String& path)
{
    ScopedDescriptor bodies { openDirectoryAt(cacheFd, ArtifactNames::bodiesDirectory.characters()) };
    if (bodies) {
        bool holdsBody = false;
        int error = listDirectory(bodies.get(), [&](std::span<const char> name, uint64_t) {
            if (bodyKeyFromFileName(name))
                holdsBody = true;
        });
        if (error)
            return ioFault(path, "cannot list cache/bodies/"_s, error);
        if (holdsBody)
            return rejection("start.not-an-artifact"_s, makeString(path, "/cache holds committed bodies but no header"_s));
    } else if (int error = errno; error != ENOENT)
        return ioFault(path, "cannot open cache/bodies/"_s, error);

    // Collected first and removed after the listing, so no entry goes while the directory is being read.
    Vector<TemporaryFileName> temporaries;
    int error = listDirectory(cacheFd, [&](std::span<const char> name, uint64_t) {
        if (!temporaryKindOfFileName(name) || name.size() > maximumTemporaryFileNameLength)
            return;
        TemporaryFileName temporary;
        memcpySpan(std::span { temporary.characters }, name);
        temporaries.append(temporary);
    });
    if (error)
        return ioFault(path, "cannot list cache/"_s, error);
    for (auto& temporary : temporaries) {
        if (::unlinkat(cacheFd, temporary.data(), 0) && errno != ENOENT)
            return ioFault(path, "cannot remove a temporary from cache/"_s, errno);
    }
    return std::nullopt;
}

// The process's header bytes go to a header temporary, which a rename publishes as cache/header.
static std::optional<StartResult> writeHeader(int cacheFd, const String& path)
{
    TemporaryFileName temporary = temporaryFileName(TemporaryKind::Header);
    ScopedDescriptor file { openAt(cacheFd, temporary.data(), O_WRONLY | O_CREAT | O_EXCL, 0644) };
    if (!file)
        return ioFault(path, "cannot create a header temporary in cache/"_s, errno);
    std::span<const uint8_t> remaining = expectedHeader().span();
    while (!remaining.empty()) {
        ssize_t written = ::write(file.get(), remaining.data(), remaining.size());
        if (written <= 0) {
            int error = written ? errno : EIO;
            if (error == EINTR)
                continue;
            return ioFault(path, "cannot write the header temporary in cache/"_s, error);
        }
        remaining = remaining.subspan(static_cast<size_t>(written));
    }
    if (int error = file.close())
        return ioFault(path, "cannot close the header temporary in cache/"_s, error);
    if (::renameat(cacheFd, temporary.data(), cacheFd, ArtifactNames::header.characters()))
        return ioFault(path, "cannot publish cache/header"_s, errno);
    return std::nullopt;
}

// A Producer's creation (container sub-SPEC section 1.3), under the producer lock: cache/ with bodies/, or a remnant
// reset, then the header. A failure leaves whatever the step created, which the next Producer or clean finishes or
// removes. Returns cache/'s descriptor.
static std::expected<ScopedDescriptor, StartResult> createArtifact(int parentFd, const String& path)
{
    ScopedDescriptor cache { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()) };
    if (!cache) {
        int error = errno;
        if (error != ENOENT)
            return std::unexpected(ioFault(path, "cannot open cache/"_s, error));
        if (::mkdirat(parentFd, ArtifactNames::cacheDirectory.characters(), 0755))
            return std::unexpected(ioFault(path, "cannot create cache/"_s, errno));
        cache = ScopedDescriptor { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()) };
        if (!cache)
            return std::unexpected(ioFault(path, "cannot open cache/"_s, errno));
    } else {
        // An artifact exists exactly when cache/header does (container sub-SPEC section 1.2).
        struct stat headerStatus;
        if (!::fstatat(cache.get(), ArtifactNames::header.characters(), &headerStatus, AT_SYMLINK_NOFOLLOW))
            return std::unexpected(rejection("start.artifact-exists"_s, makeString(path, " already holds an artifact"_s)));
        if (int error = errno; error != ENOENT)
            return std::unexpected(ioFault(path, "cannot look for cache/header"_s, error));
        if (auto stopped = resetRemnant(cache.get(), path))
            return std::unexpected(WTF::move(*stopped));
    }
    if (::mkdirat(cache.get(), ArtifactNames::bodiesDirectory.characters(), 0755) && errno != EEXIST)
        return std::unexpected(ioFault(path, "cannot create cache/bodies/"_s, errno));
    if (auto stopped = writeHeader(cache.get(), path))
        return std::unexpected(WTF::move(*stopped));
    return cache;
}

// A Consumer's or ConsumerProducer's artifact: cache/ and a header compatible with the process's (container sub-SPEC
// section 3.2), read whole, which is at most 4 KiB; a reader reads one byte more to tell a larger file. Returns cache/'s
// descriptor.
static std::expected<ScopedDescriptor, StartResult> openCompatibleArtifact(int parentFd, const String& path)
{
    ScopedDescriptor cache { openDirectoryAt(parentFd, ArtifactNames::cacheDirectory.characters()) };
    if (!cache) {
        int error = errno;
        if (error == ENOENT)
            return std::unexpected(rejection("start.artifact-missing"_s, makeString(path, " holds no cache/ directory"_s)));
        return std::unexpected(ioFault(path, "cannot open cache/"_s, error));
    }
    ScopedDescriptor header { openAt(cache.get(), ArtifactNames::header.characters(), O_RDONLY) };
    if (!header) {
        int error = errno;
        if (error == ENOENT)
            return std::unexpected(rejection("start.artifact-missing"_s, makeString(path, "/cache holds no header"_s)));
        return std::unexpected(ioFault(path, "cannot open cache/header"_s, error));
    }

    std::array<uint8_t, maximumArtifactHeaderFileBytes + 1> buffer { };
    size_t size = 0;
    while (size < buffer.size()) {
        auto destination = std::span { buffer }.subspan(size);
        ssize_t count = ::read(header.get(), destination.data(), destination.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(ioFault(path, "cannot read cache/header"_s, errno));
        }
        if (!count)
            break;
        size += static_cast<size_t>(count);
    }

    HeaderCheck check = checkArtifactHeader(std::span { buffer }.first(size), expectedHeader().span());
    switch (check.verdict) {
    case HeaderVerdict::Compatible:
        return cache;
    case HeaderVerdict::Incompatible:
        return std::unexpected(rejection("start.incompatible"_s, WTF::move(check.detail)));
    case HeaderVerdict::Corrupt:
        return std::unexpected(StartResult { StartOutcome::Fault, "start.header"_s, WTF::move(check.detail) });
    }
    RELEASE_ASSERT_NOT_REACHED();
    return std::unexpected(StartResult { StartOutcome::Fault, "start.header"_s, { } });
}

struct ArtifactAccess {
    std::unique_ptr<ProducerLock> producerLock; // producing roles
    RefPtr<OpenedArtifact> artifact;
};

// Step 7 of section 3.2, in its order. A failure returns the result that ends start; the producer lock is a local
// until success, so a rejection, busy or a fault taken after it was acquired releases it (steps 7 and 9).
static std::expected<ArtifactAccess, StartResult> openArtifact(const Config& config)
{
    const String& path = config.artifactPath;
    CString pathBytes = path.utf8();

    ScopedDescriptor parent { openDirectoryAt(AT_FDCWD, pathBytes.data()) };
    if (!parent) {
        int error = errno;
        if (error != ENOENT)
            return std::unexpected(ioFault(path, "cannot open the artifact's directory"_s, error));
        if (config.role != Role::Producer)
            return std::unexpected(rejection("start.artifact-missing"_s, makeString(path, " does not exist"_s)));
        // One level only: a missing grandparent is an I/O error.
        if (::mkdir(pathBytes.data(), 0755) && errno != EEXIST)
            return std::unexpected(ioFault(path, "cannot create the artifact's directory"_s, errno));
        parent = ScopedDescriptor { openDirectoryAt(AT_FDCWD, pathBytes.data()) };
        if (!parent)
            return std::unexpected(ioFault(path, "cannot open the artifact's directory"_s, errno));
    }

    std::unique_ptr<ProducerLock> producerLock;
    if (config.role != Role::Consumer) {
        auto acquired = ProducerLock::tryAcquire(parent.get());
        if (!acquired) {
            if (acquired.error().busy)
                return std::unexpected(StartResult { StartOutcome::Busy, "start.busy"_s, makeString("another producer or maintenance holds the producer lock of "_s, path) });
            return std::unexpected(ioFault(path, "cannot take the producer lock"_s, acquired.error().error));
        }
        producerLock = WTF::move(*acquired);
    }

    auto cache = config.role == Role::Producer ? createArtifact(parent.get(), path) : openCompatibleArtifact(parent.get(), path);
    if (!cache)
        return std::unexpected(WTF::move(cache.error()));

    // The VMs of the process that open this directory share one object and its index (II21). Every input of the header
    // is process-wide, so a compatible header's bytes are the process's own.
    auto taken = ArtifactRegistry::take(parent.get(), cache->get(), expectedHeader().span());
    if (!taken)
        return std::unexpected(ioFault(path, "cannot open and list cache/bodies/"_s, taken.error()));
    return ArtifactAccess { WTF::move(producerLock), WTF::move(*taken) };
}

#endif // OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))

} // namespace JITCacheAPIInternal

String FaultReport::stepName() const
{
    if (part.isEmpty())
        return check;
    return makeString(part, '.', check);
}

ASCIILiteral name(StartOutcome outcome)
{
    switch (outcome) {
    case StartOutcome::Created:
        return "created"_s;
    case StartOutcome::Opened:
        return "opened"_s;
    case StartOutcome::Busy:
        return "busy"_s;
    case StartOutcome::Rejected:
        return "rejected"_s;
    case StartOutcome::Fault:
        return "fault"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

ASCIILiteral name(Role role)
{
    switch (role) {
    case Role::Consumer:
        return "consumer"_s;
    case Role::Producer:
        return "producer"_s;
    case Role::ConsumerProducer:
        return "consumer-producer"_s;
    }
    // A host can hand start any value; start rejects one outside the enumeration at start.config, and it has no name.
    return { };
}

ASCIILiteral name(FaultClass faultClass)
{
    switch (faultClass) {
    case FaultClass::StartFault:
        return "start-fault"_s;
    case FaultClass::InvalidMaterial:
        return "invalid-material"_s;
    case FaultClass::ExecutableMemory:
        return "executable-memory"_s;
    case FaultClass::RecordingFault:
        return "recording-fault"_s;
    case FaultClass::DebuggerAttached:
        return "debugger-attached"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

std::optional<Role> parseRole(StringView text)
{
    for (Role role : { Role::Consumer, Role::Producer, Role::ConsumerProducer }) {
        if (text == name(role))
            return role;
    }
    return std::nullopt;
}

StartResult start(VM& vm, const Config& config)
{
#if !(OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64)))
    // Step 0. THREAD's targets are Linux x86_64 and ARM64; VM::~VM tears a state down only with the JIT enabled.
    UNUSED_PARAM(vm);
    UNUSED_PARAM(config);
    return { StartOutcome::Rejected, "start.platform"_s, "JITCache runs only on Linux, on x86_64 and ARM64, with the JIT enabled"_s };
#else
    using namespace JITCacheAPIInternal;

    // Step 1.
    if (!vm.currentThreadIsHoldingAPILock() || !vm.heap.hasHeapAccess())
        return { StartOutcome::Rejected, "start.locks"_s, "the calling thread does not hold the VM's API lock and heap access"_s };

    // Step 2: created, opened and fault configure the VM for good.
    if (vm.jitCacheState())
        return { StartOutcome::Rejected, "start.already-configured"_s, "start already configured this VM"_s };

    // The start event of harness sub-SPEC section 9.2 measures the whole call, so a call that names a bench report
    // reads the clock here. Every outcome from step 3 on records it while a report is open.
    uint64_t startNanoseconds = config.benchReportPath.isEmpty() ? 0 : benchThreadCPUNanoseconds();
    auto recordStartEvent = [&](BenchReport* report, const StartResult& result, uint64_t indexedBodies) {
        if (!report)
            return;
        using BenchValue = decltype(BenchField::value);
        BenchValue role = isRole(config.role) ? BenchValue { String { name(config.role) } } : BenchValue { nullptr };
        report->record("start"_s, {
            { "outcome"_s, String { name(result.outcome) } },
            { "role"_s, WTF::move(role) },
            { "nanoseconds"_s, benchThreadCPUNanoseconds() - startNanoseconds },
            { "indexedBodies"_s, indexedBodies },
        });
    };

    // Step 3. The detail names the field.
    if (config.artifactPath.isEmpty())
        return { StartOutcome::Rejected, "start.config"_s, "artifactPath is empty"_s };
    if (!isRole(config.role))
        return { StartOutcome::Rejected, "start.config"_s, makeString("role "_s, static_cast<unsigned>(config.role), " is none of Consumer, Producer and ConsumerProducer"_s) };
    bool producing = config.role != Role::Consumer;
    std::optional<size_t> producerLimitBytes;
    if (producing) {
        producerLimitBytes = config.producerLimitBytes ? config.producerLimitBytes : defaultProducerLimitBytes;
        if (!producerLimitBytes)
            return { StartOutcome::Rejected, "start.config"_s, "producerLimitBytes is required for a producing role until defaultProducerLimitBytes is set"_s };
    }
    std::unique_ptr<BenchReport> benchReport;
    if (!config.benchReportPath.isEmpty()) {
        benchReport = BenchReport::open(config.benchReportPath);
        if (!benchReport) {
            int error = errno;
            return { StartOutcome::Rejected, "start.config"_s, makeString("benchReportPath "_s, config.benchReportPath, " does not open: "_s, errorText(error)) };
        }
    }

    // From here on, an outcome that does not configure the VM records the start event and closes the reports again,
    // which are locals until step 10 hands them to the state.
    auto rejectWithoutState = [&](StartResult&& result) -> StartResult {
        recordStartEvent(benchReport.get(), result, 0);
        return WTF::move(result);
    };

#if ENABLE(JITCACHE_TWINS)
    std::unique_ptr<TwinReport> twinReport;
    if (!config.twinReportPath.isEmpty()) {
        twinReport = TwinReport::open(config.twinReportPath);
        if (!twinReport) {
            int error = errno;
            return rejectWithoutState({ StartOutcome::Rejected, "start.config"_s, makeString("twinReportPath "_s, config.twinReportPath, " does not open: "_s, errorText(error)) });
        }
    }
#endif

    // Step 4.
    if (const FixedOptionRow* row = checkFixedOptions())
        return rejectWithoutState({ StartOutcome::Rejected, "start.fixed-option"_s, describeFixedOptionMismatch(*row) });

    // Step 5: a debugger attached before start turned the maps on (N10), and images carry none.
    if (vm.shouldBuilderPCToCodeOriginMapping())
        return rejectWithoutState({ StartOutcome::Rejected, "start.pc-maps"_s, "the VM builds PC-to-origin maps, which an attached debugger or an option turned on"_s });

    // Step 6: binaries without a build ID cannot use the cache (THREAD Storage).
    ProcessFacts facts = processFacts();
    if (!facts.mainExecutable.size)
        return rejectWithoutState({ StartOutcome::Rejected, "start.build-id"_s, "the main executable has no GNU build ID"_s });
    if (facts.engineObject && !facts.engineObject->size)
        return rejectWithoutState({ StartOutcome::Rejected, "start.build-id"_s, "the engine object has no GNU build ID"_s });

    // Steps 7 to 9: Created for a Producer and Opened for the other roles, or the step's Fault, after which the lock the
    // step took is released, since a faulted VM never produces.
    auto access = openArtifact(config);
    if (!access && access.error().outcome != StartOutcome::Fault)
        return rejectWithoutState(WTF::move(access.error()));
    StartResult result = access
        ? StartResult { config.role == Role::Producer ? StartOutcome::Created : StartOutcome::Opened, { }, { } }
        : WTF::move(access.error());

    // Step 10.
    VMState::StartParts parts;
    parts.outcome = result.outcome;
    parts.producerLimitBytes = producerLimitBytes;
    if (access) {
        parts.producerLock = WTF::move(access->producerLock);
        parts.artifact = WTF::move(access->artifact);
    } else
        parts.startFault = FaultReport { FaultClass::StartFault, { }, result.step, result.detail };
    parts.benchReport = WTF::move(benchReport);
#if ENABLE(JITCACHE_TWINS)
    parts.twinReport = WTF::move(twinReport);
#endif
    std::unique_ptr<VMState> state { new VMState(config, WTF::move(parts)) };
    recordStartEvent(state->benchReport(), result, state->artifact() ? state->artifact()->indexedBodies() : 0);
    vm.setJITCacheState(state.release());
    return result;
#endif // OS(LINUX) && ENABLE(JIT) && (CPU(X86_64) || CPU(ARM64))
}

Status status(VM& vm)
{
    Status result;
    VMState* state = vm.jitCacheState();
    if (!state)
        return result;
    ASSERT(vm.currentThreadIsHoldingAPILock());

    // Reads only: no file opens, no fault is raised and no state changes (section 3.3).
    switch (state->startOutcome()) {
    case StartOutcome::Created:
        result.state = SessionState::Created;
        break;
    case StartOutcome::Opened:
        result.state = SessionState::Opened;
        break;
    case StartOutcome::Fault:
        result.state = SessionState::Faulted;
        break;
    case StartOutcome::Busy:
    case StartOutcome::Rejected:
        // Neither outcome creates a state (II1).
        RELEASE_ASSERT_NOT_REACHED();
        break;
    }
    result.role = state->role();
    result.strict = state->strict();
    result.activityOn = state->activityOn();
    result.activityFault = state->activityFault();
    result.production = state->productionState();
    result.productionFault = state->productionFault();
    result.firstFault = state->firstFault();

    result.progress = state->progress();
    if (OpenedArtifact* artifact = state->artifact())
        result.progress.indexedBodies = artifact->indexedBodies();
    const UCBStatistics& statistics = state->registry().statistics();
    result.progress.imports = statistics.imports;
    result.progress.seededDecodes = statistics.seededDecodes;
    result.progress.attaches = statistics.attaches;
    result.progress.gateDrops = statistics.gateDrops;
    result.progress.misses = 0;
    for (uint64_t misses : statistics.misses)
        result.progress.misses += misses;

    if (ProducerBudget* budget = state->producerBudget())
        result.budget = BudgetSnapshot { budget->limitBytes(), budget->chargedBytes(), budget->peakBytes(), budget->hasRefused() };
    return result;
}

void flushBenchReport(VM& vm)
{
    if (BenchReport* report = benchReport(vm))
        report->flush();
}

String toJSON(const StartResult& result)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"start\""_s);
    appendString(json, "outcome"_s, name(result.outcome));
    appendString(json, "step"_s, result.step);
    appendString(json, "detail"_s, result.detail);
    json.append('}');
    return json.toString();
}

String toJSON(const DeltaResult& result)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"delta\""_s);
    appendString(json, "outcome"_s, deltaOutcomeName(result.outcome));
    appendString(json, "rejection"_s, result.rejection);
    appendFault(json, "fault"_s, result.fault);
    appendNumber(json, "eligibleKeys"_s, result.eligibleKeys);
    appendNumber(json, "committedBodies"_s, result.committedBodies);
    appendNumber(json, "committedBytes"_s, result.committedBytes);
    appendNumber(json, "deferredKeys"_s, result.deferredKeys);
    json.append('}');
    return json.toString();
}

String toJSON(const Status& status)
{
    using namespace JITCacheAPIInternal;
    StringBuilder json;
    json.append("{\"jitcache\":\"status\""_s);
    appendString(json, "state"_s, sessionStateName(status.state));
    appendKey(json, "role"_s);
    if (status.role)
        json.appendQuotedJSONString(String { name(*status.role) });
    else
        json.append("null"_s);
    appendBoolean(json, "strict"_s, status.strict);
    appendBoolean(json, "activityOn"_s, status.activityOn);
    appendFault(json, "activityFault"_s, status.activityFault);
    appendString(json, "production"_s, productionStateName(status.production));
    appendFault(json, "productionFault"_s, status.productionFault);
    appendFault(json, "firstFault"_s, status.firstFault);
    appendProgress(json, status.progress);
    appendBudget(json, status.budget);
    json.append('}');
    return json.toString();
}

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
    , m_producerLock(WTF::move(parts.producerLock))
    , m_artifact(WTF::move(parts.artifact))
{
    // A start fault is the first activity fault, and production, off whenever activity is (II2), never begins.
    ASSERT(m_startOutcome == StartOutcome::Created || m_startOutcome == StartOutcome::Opened || m_startOutcome == StartOutcome::Fault);
    ASSERT((m_startOutcome == StartOutcome::Fault) == !!m_activityFault);
    ASSERT(!!parts.producerLimitBytes == producing());
    ASSERT((m_startOutcome != StartOutcome::Fault) == !!m_artifact);
    if (!parts.producerLimitBytes)
        return;

    // The producing roles' budget and its one context (section 4.4), whose limit and charges each flush's budget line
    // writes (harness sub-SPEC section 9.2). The writer refers to the budget, the producer lock and the artifact, all
    // declared before it, so the destructor destroys it first.
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
    // of their declaration: the writer before the producer lock, which releases the lock, the opened artifact and the
    // budget it refers to, and the registry after every UCB and UFE destructor has run (section 4.1).
    m_productionActive.store(false, std::memory_order_release);
    releaseEndedProductionMemory();
}

void VMState::releaseEndedProductionMemory()
{
    // A flag test while production is active and once the memory is released (section 4.2).
    if (m_productionMemoryReleased || productionActive())
        return;
    m_productionMemoryReleased = true;
    // The deleter the capture glue supplied releases the charges of the entries and the buckets (section 8.3).
    m_keptSummaries.reset();
    if (m_writer)
        m_writer->releaseStagingBuffer();
    // The entries themselves stay in the index the process's VMs share, for the VMs that import; only their charge
    // ends with production (section 4.4).
    if (m_indexEntryChargeBytes)
        m_budget->release(std::exchange(m_indexEntryChargeBytes, 0));
}

void willDestroyVM(VM& vm)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // GC is deferred for good and no compilation of the VM runs (N5, N6).
#if ENABLE(JITCACHE_TWINS)
    state->m_imageTwinCheckState.reset();
    state->m_twinReport = nullptr;
#endif
    if (state->m_benchReport)
        state->m_benchReport->flush();
    // Neither VM destruction nor process exit performs an implicit delta (THREAD Failures): production ends without a
    // fault, and the memory it held goes now.
    state->m_productionActive.store(false, std::memory_order_release);
    state->releaseEndedProductionMemory();
}

void didFinalizeHeap(VM& vm)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    // Every UCB and UFE destructor has run, so the registry outlived every cell of the VM (SPEC-ucb.md section 6.3).
    // Destroying the state releases the producer lock and drops the opened artifact, which goes with the last VM of the
    // process that holds it.
    vm.setJITCacheState(nullptr);
    delete state;
}

} // namespace JSC::JITCache
