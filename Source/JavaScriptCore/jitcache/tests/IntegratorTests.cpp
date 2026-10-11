#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "ArgList.h"
#include "ArtifactStore.h"
#include "ArtifactWriter.h"
#include "CallData.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "ConcurrentJSLock.h"
#include "Debugger.h"
#include "DeferGC.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "ICSection.h"
#include "JIT.h"
#include "JITCacheAPI.h"
#include "JITCacheCBFormat.h"
#include "JITCacheCBState.h"
#include "JITCacheCapture.h"
#include "JITCacheContainer.h"
#include "JITCacheFaults.h"
#include "JITCacheOptions.h"
#include "JITCacheParameters.h"
#include "JITCachePlatform.h"
#include "JITCacheTest.h"
#include "JITCacheVMState.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "JSLock.h"
#include "MacroAssembler.h"
#include "Options.h"
#include "ProducerBudget.h"
#include "ReleaseHeapAccessScope.h"
#include "SourceCode.h"
#include "TwinReport.h"
#include "VM.h"
#include "ValidatedBody.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <errno.h>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wtf/ASCIICType.h>
#include <wtf/FileSystem.h>
#include <wtf/JSONValues.h>
#include <wtf/Noncopyable.h>
#include <wtf/SafeStrerror.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/threads/BinarySemaphore.h>

#if OS(LINUX)
#include <elf.h>
#include <link.h>

namespace JSC::JITCache::PlatformInternal {
// Defined in JITCachePlatform.cpp, where processFacts reads each loaded object's build ID through it.
BuildID buildIDOfObject(const struct dl_phdr_info&);
} // namespace JSC::JITCache::PlatformInternal
#endif

// The integrator's C++ tests (SPEC-integrator.md section 15.1). The tests that configure a VM work on temporary
// directories and run strict, as every test that configures a VM does. A test that needs several VMs, because each fault
// and each configuration is permanent for a VM, creates them beside the test's own on its thread and destroys each under
// its API lock, which runs VM::~VM's teardown hooks; the API locks are taken and released in nested order.

namespace JSC::JITCache::Tests {

namespace IntegratorTestsInternal {

constexpr size_t maximumSize = std::numeric_limits<size_t>::max();

// A body key built from its canonical bytes (SPEC-ucb.md section 3.1), as the store's hash traits build theirs, so the
// tests need none of the UCB lane's key functions: version 1, a program body, call specialization, no mode bits, and an
// identity digest the seed varies.
static BodyKey testKey(uint8_t seed)
{
    std::array<uint8_t, BodyKey::byteSize> bytes { };
    bytes[0] = 1;
    bytes[1] = static_cast<uint8_t>(IdentityKind::Program);
    for (size_t i = 8; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(seed + i);
    return std::bit_cast<BodyKey>(bytes);
}

static Vector<uint8_t> patternBytes(size_t size, uint8_t seed)
{
    Vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(seed * 31 + i * 7);
    return bytes;
}

static bool isEightByteAligned(std::span<const uint8_t> span)
{
    return !(reinterpret_cast<uintptr_t>(span.data()) % 8);
}

static bool sameBuildID(const BuildID& a, const BuildID& b)
{
    return a.size == b.size && a.bytes == b.bytes;
}

// The bytes after an ID's size are zero, as the header's build-ID records require.
static bool hasZeroTail(const BuildID& id)
{
    return static_cast<size_t>(id.size) <= id.bytes.size() && std::ranges::all_of(std::span { id.bytes }.subspan(id.size), [](uint8_t byte) {
        return !byte;
    });
}

#if ENABLE(ASSEMBLER) && CPU(X86_64)
// Makes the two protected predicates of N11 callable, so T-CPU compares bits 7 and 8 with the predicates themselves.
struct IntegratorCPUProbe : MacroAssemblerX86_64 {
    using MacroAssemblerX86_64::supportsBMI1;
    using MacroAssemblerX86_64::supportsLZCNT;
};
#endif

#if OS(LINUX)
constexpr std::array<uint8_t, 4> gnuNoteOwner { 'G', 'N', 'U', '\0' };
constexpr std::array<uint8_t, 4> otherNoteOwner { 'G', 'N', 'X', '\0' };
constexpr uint32_t gnuPropertyNoteType = 5; // NT_GNU_PROPERTY_TYPE_0, which older <elf.h> headers lack

// One ELF note as a PT_NOTE segment holds it: the name's size, the descriptor's size and the type, then the name and
// the descriptor, each padded to the alignment from the segment's start.
static void appendNote(Vector<uint8_t>& segment, size_t alignment, uint32_t type, std::span<const uint8_t> name, std::span<const uint8_t> descriptor)
{
    auto pad = [&] {
        while (segment.size() % alignment)
            segment.append(uint8_t { 0 });
    };
    uint32_t nameSize = static_cast<uint32_t>(name.size());
    uint32_t descriptorSize = static_cast<uint32_t>(descriptor.size());
    segment.append(asByteSpan(nameSize));
    segment.append(asByteSpan(descriptorSize));
    segment.append(asByteSpan(type));
    segment.append(name);
    pad();
    segment.append(descriptor);
    pad();
}

static Vector<uint8_t> buildIDNotes(size_t alignment, std::span<const uint8_t> descriptor)
{
    Vector<uint8_t> segment;
    appendNote(segment, alignment, NT_GNU_BUILD_ID, gnuNoteOwner, descriptor);
    return segment;
}

struct CraftedSegment {
    uint32_t type;
    uint64_t alignment;
    std::span<const uint8_t> bytes;
};

// An object loaded at address 0, so each segment's virtual address is where its bytes are.
static BuildID buildIDOfCraftedObject(std::span<const CraftedSegment> segments)
{
    Vector<ElfW(Phdr)> headers;
    for (auto& segment : segments) {
        ElfW(Phdr) header { };
        header.p_type = segment.type;
        header.p_vaddr = reinterpret_cast<uintptr_t>(segment.bytes.data());
        header.p_memsz = segment.bytes.size();
        header.p_filesz = segment.bytes.size();
        header.p_align = segment.alignment;
        headers.append(header);
    }
    struct dl_phdr_info object { };
    object.dlpi_name = "crafted";
    object.dlpi_phdr = headers.span().data();
    object.dlpi_phnum = static_cast<ElfW(Half)>(headers.size());
    return PlatformInternal::buildIDOfObject(object);
}
#endif // OS(LINUX)

static String errnoText(int error)
{
    return String::fromUTF8(safeStrerror(error).data());
}

// The store's hooks and the build-ID override are process-wide, and the tests of a group share a process, so a test
// that may set one restores the defaults when it ends.
class ProcessHooksScope {
    WTF_MAKE_NONCOPYABLE(ProcessHooksScope);
public:
    ProcessHooksScope() = default;
    ~ProcessHooksScope()
    {
        StoreTesting::setFault(std::nullopt);
        StoreTesting::setRegistrySharing(true);
        StoreTesting::setInotify(true);
        StoreTesting::setFallbackListingInterval(std::nullopt);
        removeMainBuildIDForTesting(false);
    }
};

// A fresh temporary directory, removed with everything in it when the object goes.
class TemporaryDirectory {
    WTF_MAKE_NONCOPYABLE(TemporaryDirectory);
public:
    static std::unique_ptr<TemporaryDirectory> create(TestContext& context)
    {
        const char* base = getenv("TMPDIR");
        CString pattern = makeString(String::fromUTF8(base && *base ? base : "/tmp"), "/jitcache-integrator-XXXXXX"_s).utf8();
        Vector<char> path;
        path.append(pattern.spanIncludingNullTerminator());
        if (!mkdtemp(path.mutableSpan().data())) {
            JITCACHE_FAIL(makeString("mkdtemp failed: "_s, errnoText(errno)));
            return nullptr;
        }
        return std::unique_ptr<TemporaryDirectory>(new TemporaryDirectory(String::fromUTF8(path.span().data())));
    }

    ~TemporaryDirectory()
    {
        FileSystem::deleteNonEmptyDirectory(m_path);
    }

    // A path inside the directory.
    String path(StringView relative) const
    {
        return makeString(m_path, '/', relative);
    }

private:
    explicit TemporaryDirectory(String&& path)
        : m_path(WTF::move(path))
    {
    }

    const String m_path;
};

static bool pathExists(const String& path)
{
    struct stat fileStatus;
    return !::lstat(path.utf8().data(), &fileStatus);
}

static bool isDirectory(const String& path)
{
    struct stat fileStatus;
    return !::stat(path.utf8().data(), &fileStatus) && S_ISDIR(fileStatus.st_mode);
}

static bool makeDirectory(TestContext& context, const String& path)
{
    if (!::mkdir(path.utf8().data(), 0755))
        return true;
    JITCACHE_FAIL(makeString("cannot create "_s, path, ": "_s, errnoText(errno)));
    return false;
}

static bool writeFile(TestContext& context, const String& path, std::span<const uint8_t> bytes)
{
    int fd = ::open(path.utf8().data(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        JITCACHE_FAIL(makeString("cannot create "_s, path, ": "_s, errnoText(errno)));
        return false;
    }
    while (!bytes.empty()) {
        ssize_t written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            JITCACHE_FAIL(makeString("cannot write "_s, path, ": "_s, errnoText(written ? errno : EIO)));
            ::close(fd);
            return false;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    ::close(fd);
    return true;
}

// The whole file, or nothing when it cannot be read.
static std::optional<Vector<uint8_t>> readFileBytes(const String& path)
{
    int fd = ::open(path.utf8().data(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    Vector<uint8_t> bytes;
    std::array<uint8_t, 4096> buffer;
    while (true) {
        ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            ::close(fd);
            return std::nullopt;
        }
        if (!count)
            break;
        bytes.append(std::span { buffer }.first(static_cast<size_t>(count)));
    }
    ::close(fd);
    return bytes;
}

// The whole file as UTF-8 text, or a null String when it cannot be read.
static String readTextFile(const String& path)
{
    auto bytes = readFileBytes(path);
    return bytes ? String::fromUTF8(bytes->span()) : String();
}

// What JSON's grammar forbids in a line that WTF's JSON parser accepts and JSON.parse, which the runner reads twin
// reports with, rejects: a character below U+0020, which JSON allows only as whitespace between tokens, where a twin
// report writes none, and an escape JSON does not define, such as \x or \v. A null String when the line has neither.
static String jsonGrammarProblem(StringView line)
{
    for (unsigned i = 0; i < line.length(); ++i) {
        char16_t character = line[i];
        if (character < 0x20)
            return makeString("the control character "_s, static_cast<unsigned>(character), " unescaped at offset "_s, i);
        if (character != '\\')
            continue;
        unsigned escape = i++;
        if (i == line.length())
            return makeString("a backslash at offset "_s, escape, " ends the line"_s);
        switch (line[i]) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            break;
        case 'u':
            for (unsigned digit = 0; digit < 4; ++digit) {
                if (++i == line.length() || !isASCIIHexDigit(line[i]))
                    return makeString("the \\u escape at offset "_s, escape, " lacks four hexadecimal digits"_s);
            }
            break;
        default:
            return makeString("the escape at offset "_s, escape, " is none JSON defines"_s);
        }
    }
    return { };
}

// A VM beside the test's, with its API lock and heap access held, destroyed under that lock when the object goes, as
// testjitcache destroys the test's own VM, so VM::~VM runs the teardown hooks.
class ExtraVM {
    WTF_MAKE_NONCOPYABLE(ExtraVM);
public:
    ExtraVM()
        : m_vm(VM::create(HeapType::Large).leakRef())
        , m_locker(m_vm)
    {
        // The holder's reference is the last, which ~JSLockHolder drops before unlocking.
        m_vm.derefSuppressingSaferCPPChecking();
    }

    VM& vm() { return m_vm; }

private:
    VM& m_vm;
    JSLockHolder m_locker;
};

constexpr size_t testProducerLimitBytes = 64 * MB;

// Every C++ test that configures a VM sets Config::strict (SPEC-integrator.md section 15).
static Config strictConfig(const String& path, Role role)
{
    Config config;
    config.artifactPath = path;
    config.role = role;
    if (role != Role::Consumer)
        config.producerLimitBytes = testProducerLimitBytes;
    config.strict = true;
    return config;
}

static bool startsAs(TestContext& context, ASCIILiteral label, const StartResult& result, StartOutcome outcome, ASCIILiteral step = { })
{
    if (result.outcome == outcome && result.step == step)
        return true;
    JITCACHE_FAIL(makeString(label, ": expected "_s, name(outcome), step.isNull() ? ""_s : " at "_s, step, ", got "_s, toJSON(result)));
    return false;
}

static void checkFault(TestContext& context, ASCIILiteral label, const std::optional<FaultReport>& fault, FaultClass faultClass, ASCIILiteral step)
{
    if (!fault) {
        JITCACHE_FAIL(makeString(label, ": no fault, expected "_s, step));
        return;
    }
    if (fault->faultClass != faultClass || fault->stepName() != step)
        JITCACHE_FAIL(makeString(label, ": "_s, name(fault->faultClass), " at "_s, fault->stepName(), ", expected "_s, name(faultClass), " at "_s, step));
}

// An artifact a Producer created and left: its VM is destroyed, which releases the producer lock.
static bool createTestArtifact(TestContext& context, const String& path)
{
    ExtraVM producer;
    return startsAs(context, "the producer that creates the artifact"_s, JITCache::start(producer.vm(), strictConfig(path, Role::Producer)), StartOutcome::Started);
}

// A ConsumerProducer over an artifact: both switches start on.
static bool startConsumerProducer(TestContext& context, VM& vm, const String& path)
{
    if (!startsAs(context, "a ConsumerProducer"_s, JITCache::start(vm, strictConfig(path, Role::ConsumerProducer)), StartOutcome::Started))
        return false;
    VMState& state = *vm.jitCacheState();
    if (state.activityOn() && state.importsEnabled() && state.productionActive() && producerContext(vm))
        return true;
    JITCACHE_FAIL(makeString("a ConsumerProducer started with a switch off: "_s, toJSON(JITCache::status(vm))));
    return false;
}

// The state after a fault that turned activity off (section 4.2): both switches off, so nothing is tracked, imported or
// recorded, and status names the fault as the activity fault, as the end of production and as the first fault.
static void checkActivityOff(TestContext& context, VM& vm, FaultClass faultClass, ASCIILiteral step)
{
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.activityOn());
    JITCACHE_CHECK(!state.tracksKeys());
    JITCACHE_CHECK(!state.importsEnabled());
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!state.producerContextIfActive());
    JITCACHE_CHECK(!producerContext(vm));
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.state == SessionState::Started);
    JITCACHE_CHECK(!reported.activityOn);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    checkFault(context, "the activity fault"_s, reported.activityFault, faultClass, step);
    checkFault(context, "the production fault"_s, reported.productionFault, faultClass, step);
    checkFault(context, "the first fault"_s, reported.firstFault, faultClass, step);
}

// A JS function's CB after one call, which the LLInt runs, in a global object of its own.
static CodeBlock* calledCodeBlock(TestContext& context, JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource("function f() { return 1; } f();"_s, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the test source threw"_s);
        return nullptr;
    }
    auto* function = dynamicDowncast<JSFunction>(globalObject->get(globalObject, Identifier::fromString(vm, "f"_s)));
    CodeBlock* codeBlock = function && !function->isHostFunction() ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock)
        JITCACHE_FAIL("the call left no CB"_s);
    return codeBlock;
}

// The L and P commitTestBody stamps in the envelope, which a ValidatedBody the store opens returns (section 6.2).
constexpr uint32_t testBodyLLIntThreshold = 500;
constexpr uint32_t testBodyCounterProgress = 7;

// Commits a body through a producing VM's writer: every section a body of highest tier 1 holds in this build, with
// bytes drawn from the seed. The writer reads no lane format, so any bytes make a body the container accepts.
static std::optional<CommitResult> commitTestBody(TestContext& context, VMState& producer, const BodyKey& key, uint8_t seed)
{
    ArtifactWriter* writer = producer.writer();
    if (!writer) {
        JITCACHE_FAIL("the producer has no writer"_s);
        return std::nullopt;
    }
    std::array<Vector<uint8_t>, numberOfSectionKinds> bytes;
    Vector<SectionSource, numberOfSectionKinds> sources;
    for (size_t index = 0; index < numberOfSectionKinds; ++index) {
        auto kind = static_cast<SectionKind>(index);
        if (!isSectionRequired(kind, 1))
            continue;
        bytes[index] = patternBytes(16 + 8 * index, static_cast<uint8_t>(seed + index));
        sources.append(SectionSource::inMemory(kind, bytes[index].span()));
    }
    auto committed = writer->commit(CommitStamp { key, testBodyLLIntThreshold, testBodyCounterProgress, 1 }, CommitSections { sources.span() });
    if (!committed) {
        JITCACHE_FAIL(makeString("the commit failed at "_s, committed.error().check, ": "_s, committed.error().detail));
        return std::nullopt;
    }
    return *committed;
}

// The process's header with one must-match option's value flipped and its CRC recomputed, so only the comparison with
// the process's own header fails, at that option (container sub-SPEC section 3.2).
static Vector<uint8_t> headerWithFlippedOption(unsigned optionIndex)
{
    Vector<uint8_t> header;
    header.append(expectedHeader().span());
    size_t buildIDRecords = header[11];
    header[32 + 72 * buildIDRecords + 8 * optionIndex + 3] ^= 1;
    uint32_t crc = ~crc32cExtend(~0u, header.span().first(header.size() - 8));
    memcpySpan(header.mutableSpan().subspan(header.size() - 8, sizeof(crc)), asByteSpan(crc));
    return header;
}

// The commit epoch of <parent>/.cache.producer.lock, 8 bytes at offset 16 (container sub-SPEC section 2).
static std::optional<uint64_t> lockFileEpoch(const String& parent)
{
    auto bytes = readFileBytes(makeString(parent, "/.cache.producer.lock"_s));
    if (!bytes || bytes->size() < 24)
        return std::nullopt;
    uint64_t epoch = 0;
    memcpySpan(asMutableByteSpan(epoch), bytes->span().subspan(16, sizeof(epoch)));
    return epoch;
}

static bool sameScore(const CaptureScore& a, const CaptureScore& b)
{
    return a.tier == b.tier && a.richness == b.richness && a.icSitesWithCases == b.icSitesWithCases
        && a.counterWithheld == b.counterWithheld && a.counterProgress == b.counterProgress;
}

#if ENABLE(JIT)

static String scoreText(const CaptureScore& score)
{
    return makeString("{ tier "_s, static_cast<unsigned>(score.tier), ", richness "_s, score.richness, ", IC sites with cases "_s,
        score.icSitesWithCases, ", counter "_s, score.counterWithheld ? "withheld"_s : "carried"_s, ", progress "_s, score.counterProgress, " }"_s);
}

// The capture tests' bodies: two functions with one in_by_id site each and no other IC, and the objects that reach them.
// shapeA1 and shapeA2 come from one allocation site, so they share a structure, and shapeB has another. The source keeps
// every object and function in its globals, and the global object stays reachable from the test's stack, so no
// collection resets a case between two reads of the same state.
constexpr ASCIILiteral captureTestSource = "function makeA(value) { return { x: value }; }\n"
    "var shapeA1 = makeA(1);\n"
    "var shapeA2 = makeA(2);\n"
    "var shapeB = { y: 3, x: 4 };\n"
    "function firstBody(o) { return \"x\" in o; }\n"
    "function secondBody(o) { return \"x\" in o; }\n"_s;

// A global object of its own over captureTestSource, evaluated after start, so the program and its functions have keys.
static JSGlobalObject* createCaptureRealm(TestContext& context, VM& vm)
{
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    NakedPtr<Exception> exception;
    evaluate(globalObject, makeSource(captureTestSource, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
    if (exception) {
        JITCACHE_FAIL("evaluating the capture tests' source threw"_s);
        return nullptr;
    }
    return globalObject;
}

static JSValue globalValue(JSGlobalObject* globalObject, ASCIILiteral name)
{
    return globalObject->get(globalObject, Identifier::fromString(globalObject->vm(), name));
}

static JSFunction* globalFunction(TestContext& context, JSGlobalObject* globalObject, ASCIILiteral name)
{
    auto* function = dynamicDowncast<JSFunction>(globalValue(globalObject, name));
    if (!function || function->isHostFunction()) {
        JITCACHE_FAIL(makeString("the capture tests' source defines no JS function "_s, name));
        return nullptr;
    }
    return function;
}

static bool callWith(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, JSValue argument)
{
    MarkedArgumentBuffer arguments;
    arguments.append(argument);
    NakedPtr<Exception> exception;
    JSC::call(globalObject, function, getCallData(function), jsUndefined(), arguments, exception);
    if (exception) {
        JITCACHE_FAIL("calling a capture test's function threw"_s);
        return false;
    }
    return true;
}

// The function's baseline CB, brought to baseline as SPEC-ics.md T14 does: one call installs the LLInt CB, and
// JIT::compileSync, inside the deferral door 1's finalization runs under, compiles it and runs BaselineJITPlan::finalize,
// whose finalize capture commits the body in a producing VM.
static CodeBlock* bringToBaseline(TestContext& context, JSGlobalObject* globalObject, JSFunction* function, JSValue argument)
{
    VM& vm = globalObject->vm();
    if (!callWith(context, globalObject, function, argument))
        return nullptr;
    CodeBlock* codeBlock = function->jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::InterpreterThunk) {
        JITCACHE_FAIL("the first call did not install an LLInt CB"_s);
        return nullptr;
    }
    CompilationResult result;
    {
        DeferGCForAWhile deferGC(vm);
        result = JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed);
    }
    if (result != CompilationResult::CompilationSuccessful || codeBlock->jitType() != JITType::BaselineJIT || function->jsExecutable()->codeBlockForCall() != codeBlock) {
        JITCACHE_FAIL("JIT::compileSync did not install baseline code in the LLInt CB"_s);
        return nullptr;
    }
    return codeBlock;
}

// The key the parent-key registry recorded for the CB's UCB.
static std::optional<BodyKey> recordedKey(TestContext& context, VM& vm, CodeBlock& codeBlock)
{
    auto key = vm.jitCacheState()->registry().keyOf(*codeBlock.unlinkedCodeBlock());
    if (!key)
        JITCACHE_FAIL("the parent-key registry holds no key for a capture test's body"_s);
    return key;
}

#endif // ENABLE(JIT)

} // namespace IntegratorTestsInternal

using namespace IntegratorTestsInternal;

// T-BUDGET, on one thread: a charge up to the limit succeeds, one past it is refused without changing the total, the
// refusal is sticky, and the peak is the largest total.
JITCACHE_TEST(integratorBudgetLimitAndPeak, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(100);
    JITCACHE_CHECK(budget->limitBytes() == 100);
    JITCACHE_CHECK(budget->tryCharge(30));
    JITCACHE_CHECK(budget->tryCharge(50));
    JITCACHE_CHECK(budget->chargedBytes() == 80);
    budget->release(50);
    JITCACHE_CHECK(budget->chargedBytes() == 30);
    JITCACHE_CHECK(budget->peakBytes() == 80);
    JITCACHE_CHECK(budget->tryCharge(70));
    JITCACHE_CHECK(budget->chargedBytes() == 100);
    JITCACHE_CHECK(budget->peakBytes() == 100);
    JITCACHE_CHECK(!budget->hasRefused());

    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 100);

    budget->release(100);
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->peakBytes() == 100);
}

// T-BUDGET: a charge whose sum overflows is refused, even under a limit no sum can exceed.
JITCACHE_TEST(integratorBudgetRefusesOverflow, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::create(maximumSize);
    JITCACHE_CHECK(budget->tryCharge(16));
    JITCACHE_CHECK(!budget->tryCharge(maximumSize));
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(budget->chargedBytes() == 16);
    budget->release(16);
    JITCACHE_CHECK(!budget->chargedBytes());

    Ref<ProducerBudget> ended = ProducerBudget::create(maximumSize);
    ended->refuseFurtherCharges();
    JITCACHE_CHECK(ended->hasRefused());
    JITCACHE_CHECK(!ended->tryCharge(1));
}

// T-BUDGET: createUnlimited never refuses, whatever is charged and whether or not production ends.
JITCACHE_TEST(integratorBudgetUnlimited, No)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->tryCharge(maximumSize / 2));
    JITCACHE_CHECK(budget->chargedBytes() == maximumSize / 2 * 2);
    budget->release(maximumSize / 2);
    budget->release(maximumSize / 2);
    JITCACHE_CHECK(!budget->chargedBytes());
    budget->refuseFurtherCharges();
    JITCACHE_CHECK(budget->tryCharge(1));
    JITCACHE_CHECK(!budget->hasRefused());
    budget->release(1);

    Ref<ProducerBudget> saturated = ProducerBudget::createUnlimited();
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(saturated->tryCharge(maximumSize));
    JITCACHE_CHECK(!saturated->hasRefused());
}

// T-BUDGET: eight threads charge and release concurrently, each charging more than it releases, until the limit
// refuses. The total never exceeds the limit, the refusal is sticky on every thread, the peak is the largest total, and
// the balance returns to zero once every thread has released what it holds.
JITCACHE_TEST(integratorBudgetConcurrentCharges, No)
{
    constexpr size_t limit = 1 * MB;
    constexpr size_t largestCharge = 4096;
    constexpr unsigned numberOfThreads = 8;
    constexpr unsigned maximumIterations = 1000000;
    Ref<ProducerBudget> budget = ProducerBudget::create(limit);

    std::atomic<bool> exceededLimit { false };
    std::atomic<bool> refusalWasNotSticky { false };
    std::atomic<bool> neverRefused { false };
    std::atomic<size_t> largestSampledTotal { 0 };

    Vector<Ref<Thread>> threads;
    for (unsigned t = 0; t < numberOfThreads; ++t) {
        threads.append(Thread::create("JITCache budget test"_s, [&, t] {
            Vector<size_t> held;
            uint32_t random = 0x9e3779b9u * (t + 1);
            bool refused = false;
            for (unsigned iteration = 0; iteration < maximumIterations; ++iteration) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                size_t bytes = 1 + random % largestCharge;
                if (!budget->tryCharge(bytes)) {
                    refused = true;
                    if (budget->tryCharge(1)) {
                        refusalWasNotSticky = true;
                        budget->release(1);
                    }
                    break;
                }
                held.append(bytes);
                size_t total = budget->chargedBytes();
                if (total > limit)
                    exceededLimit = true;
                size_t largest = largestSampledTotal.load();
                while (largest < total && !largestSampledTotal.compare_exchange_weak(largest, total)) { }
                // One release per three charges keeps the total climbing toward the limit.
                if (iteration % 3 == 2)
                    budget->release(held.takeLast());
            }
            if (!refused)
                neverRefused = true;
            for (size_t bytes : held)
                budget->release(bytes);
        }));
    }
    for (auto& thread : threads)
        thread->waitForCompletion();

    JITCACHE_CHECK(!neverRefused.load());
    JITCACHE_CHECK(!exceededLimit.load());
    JITCACHE_CHECK(!refusalWasNotSticky.load());
    JITCACHE_CHECK(budget->hasRefused());
    JITCACHE_CHECK(!budget->tryCharge(1));
    JITCACHE_CHECK(!budget->chargedBytes());
    JITCACHE_CHECK(budget->peakBytes() <= limit);
    JITCACHE_CHECK(budget->peakBytes() >= largestSampledTotal.load());
    // The first refusal met a total within one charge of the limit, and some charge reached that total.
    JITCACHE_CHECK(budget->peakBytes() > limit - largestCharge);
}

// T-BODY: createForTesting copies each section to an 8-byte-aligned address, leaves absent kinds empty, and keeps the
// key, the version and the highest tier of the kinds it was given; a test body has no envelope, so its L and P are 0.
JITCACHE_TEST(integratorValidatedBodyForTesting, No)
{
    struct Expected {
        SectionKind kind;
        Vector<uint8_t> bytes;
    };
    std::array<Expected, 5> expected { {
        { SectionKind::UCBIdentity, patternBytes(0, 1) },
        { SectionKind::UCBCore, patternBytes(1, 2) },
        { SectionKind::ImageBaseline, patternBytes(7, 3) },
        { SectionKind::CBStateBaseline, patternBytes(8, 4) },
        { SectionKind::ICsBaseline, patternBytes(4097, 5) },
    } };
    std::array<ValidatedBody::TestSection, 5> sections;
    size_t totalSize = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        sections[i] = { expected[i].kind, expected[i].bytes.span() };
        totalSize += expected[i].bytes.size();
    }

    BodyKey key = testKey(1);
    Ref<ValidatedBody> body = ValidatedBody::createForTesting(key, 42, sections);
    JITCACHE_CHECK(body->key() == key);
    JITCACHE_CHECK(!(body->key() == testKey(2)));
    JITCACHE_CHECK(body->version() == 42);
    JITCACHE_CHECK(body->highestTier() == 1);
    JITCACHE_CHECK(!body->llintThreshold());
    JITCACHE_CHECK(!body->counterProgress());
    JITCACHE_CHECK(body->fileSize() >= totalSize);

    std::array<bool, numberOfSectionKinds> present { };
    for (auto& section : expected) {
        present[static_cast<unsigned>(section.kind)] = true;
        std::span<const uint8_t> span = body->section(section.kind);
        JITCACHE_CHECK(isEightByteAligned(span));
        JITCACHE_CHECK(span.size() == section.bytes.size());
        JITCACHE_CHECK(equalSpans(span, section.bytes.span()));
        // A copy, not the caller's bytes.
        JITCACHE_CHECK(span.empty() || span.data() != section.bytes.span().data());
    }
    for (unsigned kind = 0; kind < numberOfSectionKinds; ++kind) {
        if (!present[kind])
            JITCACHE_CHECK(body->section(static_cast<SectionKind>(kind)).empty());
    }

    // The highest tier is that of the kinds given: 0 for the UCB sections alone, and for no section at all.
    std::array<ValidatedBody::TestSection, 1> ucbOnly { { { SectionKind::UCBFeedback, sections[1].bytes } } };
    Ref<ValidatedBody> tierZero = ValidatedBody::createForTesting(testKey(3), 1, ucbOnly);
    JITCACHE_CHECK(!tierZero->highestTier());
    JITCACHE_CHECK(equalSpans(tierZero->section(SectionKind::UCBFeedback), sections[1].bytes));
    Ref<ValidatedBody> empty = ValidatedBody::createForTesting(testKey(4), 1, { });
    JITCACHE_CHECK(!empty->highestTier());
    JITCACHE_CHECK(!empty->fileSize());

    // What VMState::openBody answers with it.
    BodyLookup found = BodyLookup::found(body.copyRef());
    JITCACHE_CHECK(found.kind() == BodyLookup::Kind::Found);
    JITCACHE_CHECK(found.body() == body.ptr());
    JITCACHE_CHECK(BodyLookup::missing().kind() == BodyLookup::Kind::Missing);
    JITCACHE_CHECK(!BodyLookup::missing().body());
    JITCACHE_CHECK(BodyLookup::unusable().kind() == BodyLookup::Kind::Unusable);
    JITCACHE_CHECK(!BodyLookup::unusable().body());
}

// T-BODY: onDestroy runs exactly once, from the destructor, on the thread that drops the last reference.
JITCACHE_TEST(integratorValidatedBodyDestroyedByLastReference, No)
{
    std::atomic<unsigned> destroyCount { 0 };
    std::atomic<uint32_t> destroyingThread { 0 };
    std::atomic<uint32_t> workerThread { 0 };
    BinarySemaphore mainDroppedItsReference;

    RefPtr<ValidatedBody> body = ValidatedBody::createForTesting(testKey(5), 7, { }, [&] {
        ++destroyCount;
        destroyingThread = Thread::currentSingleton().uid();
    });
    RefPtr<ValidatedBody> handedOff = body;
    Ref<Thread> worker = Thread::create("JITCache body test"_s, [&, handedOff = WTF::move(handedOff)]() mutable {
        workerThread = Thread::currentSingleton().uid();
        mainDroppedItsReference.wait();
        handedOff = nullptr;
    });

    body = nullptr;
    JITCACHE_CHECK(!destroyCount.load());
    mainDroppedItsReference.signal();
    worker->waitForCompletion();

    JITCACHE_CHECK(destroyCount.load() == 1);
    JITCACHE_CHECK(destroyingThread.load() == workerThread.load());
    JITCACHE_CHECK(destroyingThread.load() != Thread::currentSingleton().uid());
}

// T-OPT, in the default option group: every fixed row's required value is the option's effective value, so the check
// finds no row, and the must-match reads return the effective values in the order of the header's option index.
JITCACHE_TEST(integratorFixedOptionsHoldByDefault, No)
{
    for (auto& row : fixedOptionRows()) {
        if (!row.holds())
            JITCACHE_FAIL(makeString("the default differs from options.md: "_s, describeFixedOptionMismatch(row)));
    }
    JITCACHE_CHECK(!checkFixedOptions());

    std::array<bool, numberOfMustMatchOptions> effective { { Options::evalMode(), Options::useExplicitResourceManagement(), Options::useImportDefer() } };
    JITCACHE_CHECK(mustMatchOptionValues() == effective);
    JITCACHE_CHECK(processFacts().mustMatch == effective);
    JITCACHE_CHECK(mustMatchOptionName(0) == "evalMode"_s);
    JITCACHE_CHECK(mustMatchOptionName(1) == "useExplicitResourceManagement"_s);
    JITCACHE_CHECK(mustMatchOptionName(2) == "useImportDefer"_s);
}

// The check returns the first row in options.md's order that differs, and the mismatch names both values; a Double row
// compares by value.
JITCACHE_TEST_WITH_OPTIONS(integratorFixedOptionCheckNamesFirstDifference, No, "--thresholdForJITSoon=99 --quickDFGTierUpThresholdFactor=0.3")
{
    Vector<String> differing;
    for (auto& row : fixedOptionRows()) {
        if (!row.holds())
            differing.append(describeFixedOptionMismatch(row));
    }
    JITCACHE_CHECK(differing.size() == 2);
    JITCACHE_CHECK(differing.size() == 2 && differing[0] == "thresholdForJITSoon: required 100, effective 99"_s);
    JITCACHE_CHECK(differing.size() == 2 && differing[1] == "quickDFGTierUpThresholdFactor: required 0.2, effective 0.3"_s);
    const FixedOptionRow* first = checkFixedOptions();
    JITCACHE_CHECK(first && first->name == "thresholdForJITSoon"_s);
}

// T-CPU: bit i of the CPU feature vector is the answer of predicate i of N11, and the bits after the last are zero.
JITCACHE_TEST(integratorCPUFeatureVector, No)
{
#if ENABLE(ASSEMBLER) && CPU(X86_64)
    std::array predicates {
        MacroAssemblerX86_64::supportsSSE3(),
        MacroAssemblerX86_64::supportsSupplementalSSE3(),
        MacroAssemblerX86_64::supportsSSE4_1(),
        MacroAssemblerX86_64::supportsFloatingPointRounding(),
        MacroAssemblerX86_64::supportsCountPopulation(),
        MacroAssemblerX86_64::supportsAVX(),
        MacroAssemblerX86_64::supportsAVX2(),
        IntegratorCPUProbe::supportsLZCNT(),
        IntegratorCPUProbe::supportsBMI1(),
        MacroAssemblerX86_64::supportsFloat16(),
    };
#elif ENABLE(ASSEMBLER) && CPU(ARM64)
    std::array predicates {
        MacroAssemblerARM64::supportsFloatingPointRounding(),
        MacroAssemblerARM64::supportsCountPopulation(),
        MacroAssemblerARM64::supportsFloat16(),
        MacroAssemblerARM64::supportsDotProd(),
        MacroAssemblerARM64::supportsLSE(),
        MacroAssemblerARM64::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(),
        MacroAssemblerARM64::supportsRoundFloatToIntegerFloat(),
        MacroAssemblerARM64::supportsSHA3(),
    };
#else
    std::array<bool, 0> predicates { };
#endif
    uint64_t features = processFacts().cpuFeatures;
    for (size_t bit = 0; bit < predicates.size(); ++bit) {
        bool isSet = features & (uint64_t { 1 } << bit);
        if (isSet != predicates[bit])
            JITCACHE_FAIL(makeString("CPU feature bit "_s, bit, isSet ? " is set and its predicate is false"_s : " is clear and its predicate is true"_s));
    }
    JITCACHE_CHECK(!(features >> predicates.size()));
}

// T-BUILDID: the test executable has a build ID of 1 to 64 bytes. While removeMainBuildIDForTesting is set, the facts
// report the main executable without an ID and nothing else changes.
JITCACHE_TEST(integratorBuildIDOfTestExecutable, No)
{
    ProcessFacts facts = processFacts();
    JITCACHE_CHECK(facts.mainExecutable.size >= 1 && static_cast<size_t>(facts.mainExecutable.size) <= facts.mainExecutable.bytes.size());
    JITCACHE_CHECK(hasZeroTail(facts.mainExecutable));
    JITCACHE_CHECK(!facts.engineObject || hasZeroTail(*facts.engineObject));

    removeMainBuildIDForTesting(true);
    ProcessFacts removed = processFacts();
    removeMainBuildIDForTesting(false);
    JITCACHE_CHECK(sameBuildID(removed.mainExecutable, BuildID { }));
    JITCACHE_CHECK(removed.engineObject.has_value() == facts.engineObject.has_value());
    JITCACHE_CHECK(!facts.engineObject || sameBuildID(*removed.engineObject, *facts.engineObject));
    JITCACHE_CHECK(removed.mustMatch == facts.mustMatch);
    JITCACHE_CHECK(removed.cpuFeatures == facts.cpuFeatures);
    JITCACHE_CHECK(sameBuildID(processFacts().mainExecutable, facts.mainExecutable));
}

#if OS(LINUX)
// T-BUILDID: a crafted object's ID is the descriptor of the first GNU build-ID note in its PT_NOTE segments, read past
// notes of other types and owners, in segments of either note alignment; later notes and segments do not replace it.
JITCACHE_TEST(integratorBuildIDFromCraftedNotes, No)
{
    Vector<uint8_t> loaded = patternBytes(64, 0);
    Vector<uint8_t> descriptor = patternBytes(20, 1);
    Vector<uint8_t> properties;
    appendNote(properties, 8, gnuPropertyNoteType, gnuNoteOwner, patternBytes(16, 2).span());
    Vector<uint8_t> notes;
    appendNote(notes, 4, NT_GNU_ABI_TAG, gnuNoteOwner, patternBytes(16, 3).span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, otherNoteOwner, patternBytes(20, 4).span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, gnuNoteOwner, descriptor.span());
    appendNote(notes, 4, NT_GNU_BUILD_ID, gnuNoteOwner, patternBytes(20, 5).span());
    Vector<uint8_t> laterNotes = buildIDNotes(4, patternBytes(20, 6).span());
    std::array object {
        CraftedSegment { PT_LOAD, 4096, loaded.span() },
        CraftedSegment { PT_NOTE, 8, properties.span() },
        CraftedSegment { PT_NOTE, 4, notes.span() },
        CraftedSegment { PT_NOTE, 4, laterNotes.span() },
    };
    BuildID id = buildIDOfCraftedObject(object);
    JITCACHE_CHECK(static_cast<size_t>(id.size) == descriptor.size());
    JITCACHE_CHECK(equalSpans(std::span { id.bytes }.first(id.size), descriptor.span()));
    JITCACHE_CHECK(hasZeroTail(id));

    Vector<uint8_t> eightAlignedNotes = buildIDNotes(8, descriptor.span());
    std::array eightAligned { CraftedSegment { PT_NOTE, 8, eightAlignedNotes.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(eightAligned), id));
}

// T-BUILDID: a descriptor of 1 or 64 bytes is the ID. A descriptor of 0 or 65 bytes, a note that overruns its segment,
// an object without PT_NOTE segments and one whose notes hold no GNU build ID give none.
JITCACHE_TEST(integratorBuildIDBounds, No)
{
    for (size_t size : std::array<size_t, 2> { 1, 64 }) {
        Vector<uint8_t> descriptor = patternBytes(size, 7);
        Vector<uint8_t> notes = buildIDNotes(4, descriptor.span());
        std::array object { CraftedSegment { PT_NOTE, 4, notes.span() } };
        BuildID id = buildIDOfCraftedObject(object);
        JITCACHE_CHECK(static_cast<size_t>(id.size) == size);
        JITCACHE_CHECK(equalSpans(std::span { id.bytes }.first(id.size), descriptor.span()));
        JITCACHE_CHECK(hasZeroTail(id));
    }
    for (size_t size : std::array<size_t, 2> { 0, 65 }) {
        Vector<uint8_t> notes = buildIDNotes(4, patternBytes(size, 8).span());
        std::array object { CraftedSegment { PT_NOTE, 4, notes.span() } };
        JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(object), BuildID { }));
    }

    Vector<uint8_t> truncated = buildIDNotes(4, patternBytes(20, 9).span());
    truncated.shrink(truncated.size() - 4);
    std::array overrun { CraftedSegment { PT_NOTE, 4, truncated.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(overrun), BuildID { }));

    Vector<uint8_t> loaded = patternBytes(64, 10);
    std::array withoutNotes { CraftedSegment { PT_LOAD, 4096, loaded.span() } };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(withoutNotes), BuildID { }));

    Vector<uint8_t> otherNotes;
    appendNote(otherNotes, 4, NT_GNU_ABI_TAG, gnuNoteOwner, patternBytes(16, 11).span());
    appendNote(otherNotes, 4, NT_GNU_BUILD_ID, otherNoteOwner, patternBytes(20, 12).span());
    std::array withoutBuildID {
        CraftedSegment { PT_LOAD, 4096, loaded.span() },
        CraftedSegment { PT_NOTE, 4, otherNotes.span() },
    };
    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject(withoutBuildID), BuildID { }));

    JITCACHE_CHECK(sameBuildID(buildIDOfCraftedObject({ }), BuildID { }));
}
#endif // OS(LINUX)

// T-START, for each role. A Producer creates a missing parent one level deep and is Started; a second producing VM of
// the process is Busy while the first VM's state lives and stays unconfigured; a Consumer beside the producer is Started
// and shares its opened artifact (II21); and once the producer's state is destroyed, which releases the lock, the VM that
// was busy starts as a ConsumerProducer.
JITCACHE_TEST(integratorStartOutcomesByRole, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();

    std::optional<ExtraVM> producer;
    producer.emplace();
    if (!startsAs(context, "the producer"_s, JITCache::start(producer->vm(), strictConfig(artifact, Role::Producer)), StartOutcome::Started))
        return;
    JITCACHE_CHECK(pathExists(directory->path("artifact/.cache.producer.lock"_s)));
    JITCACHE_CHECK(pathExists(directory->path("artifact/cache/header"_s)));
    JITCACHE_CHECK(isDirectory(directory->path("artifact/cache/bodies"_s)));
    Status producerStatus = JITCache::status(producer->vm());
    JITCACHE_CHECK(producerStatus.state == SessionState::Started);
    JITCACHE_CHECK(producerStatus.role == Role::Producer);
    JITCACHE_CHECK(producerStatus.strict);
    JITCACHE_CHECK(producerStatus.activityOn);
    JITCACHE_CHECK(producerStatus.production == ProductionState::Active);
    JITCACHE_CHECK(!producerStatus.activityFault && !producerStatus.productionFault && !producerStatus.firstFault);
    JITCACHE_CHECK(producerStatus.budget.limitBytes == testProducerLimitBytes);
    JITCACHE_CHECK(!producerStatus.budget.refused);
    JITCACHE_CHECK(!producerStatus.progress.indexedBodies);
    JITCACHE_CHECK(producer->vm().jitCacheState()->writer());
    JITCACHE_CHECK(!producer->vm().jitCacheState()->importsEnabled());
    JITCACHE_CHECK(producerContext(producer->vm()));

    for (Role role : { Role::Producer, Role::ConsumerProducer }) {
        startsAs(context, "a second producing VM"_s, JITCache::start(vm, strictConfig(artifact, role)), StartOutcome::Busy, "start.busy"_s);
        JITCACHE_CHECK(!vm.jitCacheState());
        JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Unconfigured);
    }

    {
        ExtraVM consumer;
        if (startsAs(context, "a consumer beside the producer"_s, JITCache::start(consumer.vm(), strictConfig(artifact, Role::Consumer)), StartOutcome::Started)) {
            VMState& state = *consumer.vm().jitCacheState();
            JITCACHE_CHECK(state.artifact() && state.artifact() == producer->vm().jitCacheState()->artifact());
            JITCACHE_CHECK(state.importsEnabled());
            JITCACHE_CHECK(!state.writer());
            JITCACHE_CHECK(!state.producerBudget());
            JITCACHE_CHECK(!producerContext(consumer.vm()));
            Status consumerStatus = JITCache::status(consumer.vm());
            JITCACHE_CHECK(consumerStatus.state == SessionState::Started);
            JITCACHE_CHECK(consumerStatus.role == Role::Consumer);
            JITCACHE_CHECK(consumerStatus.production == ProductionState::NotProducing);
            JITCACHE_CHECK(!consumerStatus.productionFault);
            JITCACHE_CHECK(!consumerStatus.budget.limitBytes);
        }
    }

    producer.reset();
    if (!startsAs(context, "the ConsumerProducer once the producer is gone"_s, JITCache::start(vm, strictConfig(artifact, Role::ConsumerProducer)), StartOutcome::Started))
        return;
    Status consumerProducer = JITCache::status(vm);
    JITCACHE_CHECK(consumerProducer.state == SessionState::Started);
    JITCACHE_CHECK(consumerProducer.role == Role::ConsumerProducer);
    JITCACHE_CHECK(consumerProducer.production == ProductionState::Active);
    JITCACHE_CHECK(vm.jitCacheState()->importsEnabled());
    JITCACHE_CHECK(vm.jitCacheState()->writer());
}

// T-START: each rejection of section 3.2 a test can reach in the default option group leaves the VM unconfigured, and a
// start on the same VM afterwards succeeds. A Config left at its defaults gives status().strict false.
JITCACHE_TEST(integratorStartRejections, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    auto checkRejected = [&](ASCIILiteral label, const Config& config, ASCIILiteral step) {
        startsAs(context, label, JITCache::start(vm, config), StartOutcome::Rejected, step);
        JITCACHE_CHECK(!vm.jitCacheState());
    };

    {
        ReleaseHeapAccessScope withoutHeapAccess(vm.heap);
        checkRejected("a call without heap access"_s, strictConfig(artifact, Role::Consumer), "start.locks"_s);
    }

    Config emptyPath = strictConfig(artifact, Role::Consumer);
    emptyPath.artifactPath = String();
    checkRejected("an empty artifactPath"_s, emptyPath, "start.config"_s);
    Config badRole = strictConfig(artifact, Role::Consumer);
    badRole.role = static_cast<Role>(7);
    checkRejected("a role out of range"_s, badRole, "start.config"_s);
    if (!defaultProducerLimitBytes) {
        Config noLimit = strictConfig(artifact, Role::Producer);
        noLimit.producerLimitBytes = std::nullopt;
        checkRejected("a producer without a limit"_s, noLimit, "start.config"_s);
    }
    Config badBenchReport = strictConfig(artifact, Role::Consumer);
    badBenchReport.benchReportPath = directory->path("missing/bench.jsonl"_s);
    checkRejected("a bench report that does not open"_s, badBenchReport, "start.config"_s);
    Config badTwinReport = strictConfig(artifact, Role::Consumer);
    badTwinReport.twinReportPath = directory->path("missing/twins.jsonl"_s);
    checkRejected("a twin report that does not open"_s, badTwinReport, "start.config"_s);

    // The build-ID step precedes every step that touches the artifact.
    removeMainBuildIDForTesting(true);
    checkRejected("a main executable without a build ID"_s, strictConfig(artifact, Role::Producer), "start.build-id"_s);
    removeMainBuildIDForTesting(false);
    JITCACHE_CHECK(!pathExists(artifact));

    // A missing parent, a parent without cache/ and a cache/ without a header, for both roles that open an artifact.
    String bare = directory->path("bare"_s);
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a missing parent"_s, strictConfig(bare, role), "start.artifact-missing"_s);
    if (!makeDirectory(context, bare))
        return;
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a parent without cache/"_s, strictConfig(bare, role), "start.artifact-missing"_s);
    if (!makeDirectory(context, directory->path("bare/cache"_s)))
        return;
    for (Role role : { Role::Consumer, Role::ConsumerProducer })
        checkRejected("a cache/ without a header"_s, strictConfig(bare, role), "start.artifact-missing"_s);

    // A debugger attached before start turned the PC-to-origin maps on for good, so that VM is rejected.
    {
        ExtraVM attached;
        JSGlobalObject* globalObject = JSGlobalObject::create(attached.vm(), JSGlobalObject::createStructure(attached.vm(), jsNull()));
        Debugger debugger(attached.vm());
        debugger.attach(globalObject);
        startsAs(context, "a VM a debugger attached to"_s, JITCache::start(attached.vm(), strictConfig(artifact, Role::Producer)), StartOutcome::Rejected, "start.pc-maps"_s);
        JITCACHE_CHECK(!attached.vm().jitCacheState());
        debugger.detach(globalObject, Debugger::TerminatingDebuggingSession);
    }
    JITCACHE_CHECK(!pathExists(artifact));

    if (!createTestArtifact(context, artifact))
        return;
    checkRejected("a producer over an artifact"_s, strictConfig(artifact, Role::Producer), "start.artifact-exists"_s);

    // A header that differs from the process's in one must-match option is incompatible and names the option. A Producer
    // is rejected at start.artifact-exists after taking the lock, which creates the lock file, and a Consumer at
    // start.incompatible. A ConsumerProducer replaces the artifact with an empty one and says why at start.replaced: it
    // removes the old artifact and the .cache.replaced an earlier replacement left, and bumps the epoch once. A Consumer
    // then opens the new artifact.
    String incompatible = directory->path("incompatible"_s);
    Vector<uint8_t> header = headerWithFlippedOption(0);
    Vector<uint8_t> bodyBytes = patternBytes(33, 4);
    String oldBodyName = String::fromUTF8(bodyFileName(testKey(10)).data());
    if (!makeDirectory(context, incompatible) || !makeDirectory(context, directory->path("incompatible/cache"_s))
        || !makeDirectory(context, directory->path("incompatible/cache/bodies"_s)) || !writeFile(context, directory->path("incompatible/cache/header"_s), header.span())
        || !writeFile(context, makeString(incompatible, "/cache/bodies/"_s, oldBodyName), bodyBytes.span()))
        return;
    checkRejected("a Producer over an incompatible artifact"_s, strictConfig(incompatible, Role::Producer), "start.artifact-exists"_s);
    StartResult rejected = JITCache::start(vm, strictConfig(incompatible, Role::Consumer));
    startsAs(context, "a Consumer of an incompatible header"_s, rejected, StartOutcome::Rejected, "start.incompatible"_s);
    JITCACHE_CHECK(rejected.detail.contains("evalMode"_s));
    JITCACHE_CHECK(!vm.jitCacheState());
    // What an earlier replacement left when it stopped before removing the old artifact.
    if (!makeDirectory(context, directory->path("incompatible/.cache.replaced"_s)) || !makeDirectory(context, directory->path("incompatible/.cache.replaced/bodies"_s))
        || !writeFile(context, directory->path("incompatible/.cache.replaced/header"_s), header.span())
        || !writeFile(context, makeString(incompatible, "/.cache.replaced/bodies/"_s, oldBodyName), bodyBytes.span()))
        return;
    std::optional<uint64_t> epochBefore = lockFileEpoch(incompatible);
    JITCACHE_CHECK(epochBefore.has_value());
    {
        ExtraVM replacing;
        StartResult replaced = JITCache::start(replacing.vm(), strictConfig(incompatible, Role::ConsumerProducer));
        if (startsAs(context, "a ConsumerProducer of an incompatible header"_s, replaced, StartOutcome::Started, "start.replaced"_s)) {
            JITCACHE_CHECK(replaced.detail.contains("evalMode"_s));
            JITCACHE_CHECK(pathExists(directory->path("incompatible/cache/header"_s)) && pathExists(directory->path("incompatible/cache/bodies"_s)));
            JITCACHE_CHECK(!pathExists(makeString(incompatible, "/cache/bodies/"_s, oldBodyName)));
            JITCACHE_CHECK(!pathExists(directory->path("incompatible/.cache.replaced"_s)));
            std::optional<uint64_t> epochAfter = lockFileEpoch(incompatible);
            JITCACHE_CHECK(epochBefore && epochAfter && *epochAfter == *epochBefore + 1);
            ExtraVM reader;
            startsAs(context, "a Consumer of the replaced artifact"_s, JITCache::start(reader.vm(), strictConfig(incompatible, Role::Consumer)), StartOutcome::Started);
        }
    }

    {
        ExtraVM defaults;
        Config config;
        config.artifactPath = artifact;
        if (startsAs(context, "a Config left at its defaults"_s, JITCache::start(defaults.vm(), config), StartOutcome::Started))
            JITCACHE_CHECK(!JITCache::status(defaults.vm()).strict);
    }

    // After every rejection, the VM still configures, and then it is configured for good.
    if (!startsAs(context, "a consumer after the rejections"_s, JITCache::start(vm, strictConfig(artifact, Role::Consumer)), StartOutcome::Started))
        return;
    JITCACHE_CHECK(JITCache::status(vm).strict);
    startsAs(context, "a second start"_s, JITCache::start(vm, strictConfig(artifact, Role::Consumer)), StartOutcome::Rejected, "start.already-configured"_s);
    JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Started);
}

// T-START: a fixed option without its required value rejects at start.fixed-option, naming the first such option of the
// table and both values. This group's process sets two fixed options off their required values.
JITCACHE_TEST_WITH_OPTIONS(integratorStartRejectsAFixedOption, Yes, "--thresholdForJITSoon=99 --quickDFGTierUpThresholdFactor=0.3")
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    StartResult result = JITCache::start(vm, strictConfig(artifact, Role::Producer));
    startsAs(context, "a process with a fixed option changed"_s, result, StartOutcome::Rejected, "start.fixed-option"_s);
    JITCACHE_CHECK(result.detail == "thresholdForJITSoon: required 100, effective 99"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(!pathExists(artifact));
}

// T-START: a failing listing is a Fault at start.io and a corrupt header a Fault at start.header. A faulted VM reports
// Faulted, with activity off and the start fault as its first fault, and rejects a second start; a faulted producing
// role released the lock, so another producing VM takes it while the faulted state lives.
JITCACHE_TEST(integratorStartFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    auto checkFaulted = [&](VM& faultedVM, ASCIILiteral step) {
        VMState& state = *faultedVM.jitCacheState();
        JITCACHE_CHECK(!state.activityOn());
        JITCACHE_CHECK(!state.productionActive());
        JITCACHE_CHECK(!state.artifact());
        JITCACHE_CHECK(!state.writer());
        JITCACHE_CHECK(!producerContext(faultedVM));
        Status reported = JITCache::status(faultedVM);
        JITCACHE_CHECK(reported.state == SessionState::Faulted);
        JITCACHE_CHECK(!reported.activityOn);
        checkFault(context, "the start fault"_s, reported.activityFault, FaultClass::StartFault, step);
        checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::StartFault, step);
        JITCACHE_CHECK(toJSON(reported).contains(makeString("\"step\":\""_s, step, '"')));
        startsAs(context, "a second start on a faulted VM"_s, JITCache::start(faultedVM, strictConfig(artifact, Role::Consumer)), StartOutcome::Rejected, "start.already-configured"_s);
    };

    {
        StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Listing, EIO, std::nullopt });
        ExtraVM faulted;
        StartResult result = JITCache::start(faulted.vm(), strictConfig(artifact, Role::ConsumerProducer));
        StoreTesting::setFault(std::nullopt);
        if (startsAs(context, "a listing that fails"_s, result, StartOutcome::Fault, "start.io"_s)) {
            checkFaulted(faulted.vm(), "start.io"_s);
            Status reported = JITCache::status(faulted.vm());
            JITCACHE_CHECK(reported.role == Role::ConsumerProducer);
            JITCACHE_CHECK(reported.production == ProductionState::Ended);
            checkFault(context, "the end of production"_s, reported.productionFault, FaultClass::StartFault, "start.io"_s);
            JITCACHE_CHECK(reported.budget.limitBytes == testProducerLimitBytes);

            ExtraVM next;
            startsAs(context, "a producing VM beside the faulted one"_s, JITCache::start(next.vm(), strictConfig(artifact, Role::ConsumerProducer)), StartOutcome::Started);
        }
    }

    String corrupt = directory->path("corrupt"_s);
    std::array<uint8_t, 64> notAHeader;
    notAHeader.fill('x');
    if (!makeDirectory(context, corrupt) || !makeDirectory(context, directory->path("corrupt/cache"_s))
        || !makeDirectory(context, directory->path("corrupt/cache/bodies"_s)) || !writeFile(context, directory->path("corrupt/cache/header"_s), notAHeader))
        return;
    VM& vm = *context.vm();
    if (startsAs(context, "a corrupt header"_s, JITCache::start(vm, strictConfig(corrupt, Role::Consumer)), StartOutcome::Fault, "start.header"_s)) {
        checkFaulted(vm, "start.header"_s);
        Status reported = JITCache::status(vm);
        JITCACHE_CHECK(reported.production == ProductionState::NotProducing);
        JITCACHE_CHECK(!reported.productionFault);
    }
}

// T-START with container test C8: a header-less cache/ holding only temporaries is reset and reused, while names that are
// not temporaries stay; one whose bodies/ holds a body name is start.not-an-artifact and keeps everything; an artifact
// with a header is start.artifact-exists.
JITCACHE_TEST(integratorStartCreatesOverARemnant, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    Vector<uint8_t> filler = patternBytes(33, 3);

    String remnant = directory->path("remnant"_s);
    TemporaryFileName bodyTemporary = temporaryFileName(TemporaryKind::Body);
    TemporaryFileName headerTemporary = temporaryFileName(TemporaryKind::Header);
    String bodyTemporaryPath = makeString(remnant, "/cache/"_s, String::fromUTF8(bodyTemporary.data()));
    String headerTemporaryPath = makeString(remnant, "/cache/"_s, String::fromUTF8(headerTemporary.data()));
    String otherPath = directory->path("remnant/cache/notes.txt"_s);
    if (!makeDirectory(context, remnant) || !makeDirectory(context, directory->path("remnant/cache"_s))
        || !writeFile(context, bodyTemporaryPath, filler.span()) || !writeFile(context, headerTemporaryPath, filler.span()) || !writeFile(context, otherPath, filler.span()))
        return;
    if (!createTestArtifact(context, remnant))
        return;
    JITCACHE_CHECK(!pathExists(bodyTemporaryPath));
    JITCACHE_CHECK(!pathExists(headerTemporaryPath));
    JITCACHE_CHECK(pathExists(otherPath));
    JITCACHE_CHECK(isDirectory(directory->path("remnant/cache/bodies"_s)));
    auto header = readFileBytes(directory->path("remnant/cache/header"_s));
    JITCACHE_CHECK(header && equalSpans(header->span(), expectedHeader().span()));
    {
        ExtraVM consumer;
        startsAs(context, "a consumer of the reused remnant"_s, JITCache::start(consumer.vm(), strictConfig(remnant, Role::Consumer)), StartOutcome::Started);
    }

    String withBody = directory->path("with-body"_s);
    String bodyPath = makeString(withBody, "/cache/bodies/"_s, String::fromUTF8(bodyFileName(testKey(9)).data()));
    String temporaryPath = makeString(withBody, "/cache/"_s, String::fromUTF8(temporaryFileName(TemporaryKind::Body).data()));
    if (!makeDirectory(context, withBody) || !makeDirectory(context, directory->path("with-body/cache"_s)) || !makeDirectory(context, directory->path("with-body/cache/bodies"_s))
        || !writeFile(context, bodyPath, filler.span()) || !writeFile(context, temporaryPath, filler.span()))
        return;
    startsAs(context, "a remnant holding a body"_s, JITCache::start(vm, strictConfig(withBody, Role::Producer)), StartOutcome::Rejected, "start.not-an-artifact"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(pathExists(bodyPath));
    JITCACHE_CHECK(pathExists(temporaryPath));
    JITCACHE_CHECK(!pathExists(directory->path("with-body/cache/header"_s)));

    startsAs(context, "a producer over an artifact"_s, JITCache::start(vm, strictConfig(remnant, Role::Producer)), StartOutcome::Rejected, "start.artifact-exists"_s);
    JITCACHE_CHECK(!vm.jitCacheState());
}

// T-FAULTS: invalid material, in either form, turns activity off, which ends production, and later faults keep the first.
JITCACHE_TEST(integratorInvalidMaterialTurnsActivityOff, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        VMState& state = *extra.vm().jitCacheState();
        state.raiseInvalidMaterial("ucb.identity"_s, "a test's detail"_s);
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "ucb.identity"_s);
        JITCACHE_CHECK(JITCache::status(extra.vm()).activityFault->detail == "a test's detail"_s);
        state.raiseRecordingFault("cb"_s, "later-check"_s, { });
        state.raiseInvalidMaterial("image"_s, "later-check"_s, { });
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "ucb.identity"_s);
    }
    {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        extra.vm().jitCacheState()->raiseInvalidMaterial("image"_s, "test-check"_s, { });
        checkActivityOff(context, extra.vm(), FaultClass::InvalidMaterial, "image.test-check"_s);
    }
}

// T-FAULTS: a recording fault ends production for good and leaves activity on, so a ConsumerProducer goes on importing,
// and its budget refuses every later charge. Invalid material afterwards turns activity off, and the recording fault
// stays the first fault.
JITCACHE_TEST(integratorRecordingFaultEndsProduction, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    if (!startConsumerProducer(context, vm, artifact))
        return;
    VMState& state = *vm.jitCacheState();

    state.raiseRecordingFault("ucb"_s, "capture-budget"_s, { });
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(state.tracksKeys());
    JITCACHE_CHECK(state.importsEnabled());
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!producerContext(vm));
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(1));
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.activityOn);
    JITCACHE_CHECK(!reported.activityFault);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    JITCACHE_CHECK(reported.budget.refused);
    checkFault(context, "the recording fault"_s, reported.productionFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);

    // The end of production frees what production held, at the next glue entry.
    state.releaseEndedProductionMemory();
    JITCACHE_CHECK(!state.keptSummaries());
    JITCACHE_CHECK(!state.producerBudget()->chargedBytes());

    state.raiseInvalidMaterial("ucb.feedback"_s, { });
    reported = JITCache::status(vm);
    JITCACHE_CHECK(!reported.activityOn);
    JITCACHE_CHECK(!state.importsEnabled());
    checkFault(context, "the later activity fault"_s, reported.activityFault, FaultClass::InvalidMaterial, "ucb.feedback"_s);
    checkFault(context, "the recording fault"_s, reported.productionFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "ucb.capture-budget"_s);
}

// Section 3.3: a budget that refused a charge the VM thread has not raised yet reports production Ended at budget.limit,
// while the switch itself stays on until a capture or delta raises it.
JITCACHE_TEST(integratorUnraisedRefusalReportsBudgetLimit, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    Config config = strictConfig(directory->path("artifact"_s), Role::Producer);
    config.producerLimitBytes = 16;
    if (!startsAs(context, "a producer with a small limit"_s, JITCache::start(vm, config), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(17));
    JITCACHE_CHECK(state.productionActive());
    Status reported = JITCache::status(vm);
    JITCACHE_CHECK(reported.activityOn);
    JITCACHE_CHECK(reported.production == ProductionState::Ended);
    JITCACHE_CHECK(reported.budget.refused);
    JITCACHE_CHECK(reported.budget.limitBytes == 16);
    JITCACHE_CHECK(!reported.activityFault);
    checkFault(context, "the unraised refusal"_s, reported.productionFault, FaultClass::RecordingFault, "budget.limit"_s);
    checkFault(context, "the first fault"_s, reported.firstFault, FaultClass::RecordingFault, "budget.limit"_s);
}

// T-FAULTS: each executable-allocation site turns activity off and names its own step, and a second report keeps the
// first (section 4.5).
JITCACHE_TEST(integratorExecutableAllocationFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    struct SiteStep {
        ExecutableAllocationSite site;
        ASCIILiteral step;
    };
    std::array sites {
        SiteStep { ExecutableAllocationSite::BaselinePlan, "exec-alloc.baseline-plan"_s },
        SiteStep { ExecutableAllocationSite::DFGPlan, "exec-alloc.dfg-plan"_s },
        SiteStep { ExecutableAllocationSite::FTLPlan, "exec-alloc.ftl-plan"_s },
        SiteStep { ExecutableAllocationSite::InlineCacheHandler, "exec-alloc.ic-handler"_s },
        SiteStep { ExecutableAllocationSite::MathICSnippet, "exec-alloc.mathic-snippet"_s },
        SiteStep { ExecutableAllocationSite::JITCacheImage, "exec-alloc.jitcache-image"_s },
    };
    for (auto& [site, step] : sites) {
        ExtraVM extra;
        if (!startConsumerProducer(context, extra.vm(), artifact))
            return;
        didFailExecutableAllocation(extra.vm(), site);
        checkActivityOff(context, extra.vm(), FaultClass::ExecutableMemory, step);
        didFailExecutableAllocation(extra.vm(), site == ExecutableAllocationSite::JITCacheImage ? ExecutableAllocationSite::BaselinePlan : ExecutableAllocationSite::JITCacheImage);
        checkActivityOff(context, extra.vm(), FaultClass::ExecutableMemory, step);
    }
}

// T-FAULTS: didFailExecutableAllocation called with a CB's m_lock held through a GCSafeConcurrentJSLocker returns, which
// it would not if it took that lock, and records its fault. The CB's function runs before start, so none of its bodies
// is JITCache's.
JITCACHE_TEST(integratorExecutableAllocationFaultUnderCodeBlockLock, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    CodeBlock* codeBlock = calledCodeBlock(context, globalObject);
    if (!codeBlock || !startConsumerProducer(context, vm, artifact))
        return;
    {
        GCSafeConcurrentJSLocker locker(codeBlock->m_lock, vm);
        didFailExecutableAllocation(vm, ExecutableAllocationSite::InlineCacheHandler);
    }
    checkActivityOff(context, vm, FaultClass::ExecutableMemory, "exec-alloc.ic-handler"_s);
}

// T-FAULTS: a JSC::Debugger attached to a global object of a configured VM turns activity off, and status names
// debugger.attach as the activity fault and as the end of production. A fault raised afterwards finds activity off and
// records nothing.
JITCACHE_TEST(integratorDebuggerAttachTurnsActivityOff, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    JSGlobalObject* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    if (!startConsumerProducer(context, vm, artifact))
        return;
    Debugger debugger(vm);
    debugger.attach(globalObject);
    checkActivityOff(context, vm, FaultClass::DebuggerAttached, "debugger.attach"_s);
    vm.jitCacheState()->raiseInvalidMaterial("ucb.identity"_s, { });
    didFailExecutableAllocation(vm, ExecutableAllocationSite::BaselinePlan);
    checkActivityOff(context, vm, FaultClass::DebuggerAttached, "debugger.attach"_s);
    debugger.detach(globalObject, Debugger::TerminatingDebuggingSession);
}

// T-FAULTS: a VM without state ignores every entry point, and start still configures it afterwards.
JITCACHE_TEST(integratorFaultEntryPointsIgnoreAnUnconfiguredVM, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;
    VM& vm = *context.vm();
    constexpr std::array sites { ExecutableAllocationSite::BaselinePlan, ExecutableAllocationSite::DFGPlan, ExecutableAllocationSite::FTLPlan,
        ExecutableAllocationSite::InlineCacheHandler, ExecutableAllocationSite::MathICSnippet, ExecutableAllocationSite::JITCacheImage };
    for (auto site : sites)
        didFailExecutableAllocation(vm, site);
    didAttachDebugger(vm);
    flushBenchReport(vm);
    JITCACHE_CHECK(!vm.jitCacheState());
    JITCACHE_CHECK(!producerContext(vm));
    JITCACHE_CHECK(JITCache::status(vm).state == SessionState::Unconfigured);
    startConsumerProducer(context, vm, artifact);
}

// T-LOOKUP, in a Consumer over an artifact holding a body the writer committed, in the order section 15.1 gives.
JITCACHE_TEST(integratorBodyLookups, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    BodyKey committedKey = testKey(21);
    BodyKey absentKey = testKey(22);
    std::optional<CommitResult> committed;
    {
        ExtraVM producer;
        if (!startsAs(context, "the producer"_s, JITCache::start(producer.vm(), strictConfig(artifact, Role::Producer)), StartOutcome::Started))
            return;
        committed = commitTestBody(context, *producer.vm().jitCacheState(), committedKey, 21);
        if (!committed)
            return;
    }

    VM& vm = *context.vm();
    if (!startsAs(context, "the consumer"_s, JITCache::start(vm, strictConfig(artifact, Role::Consumer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(JITCache::status(vm).progress.indexedBodies == 1);

    // bodyVersion reads the index alone: a hook that fails every open, and then every listing, never fires for it.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    uint64_t token = state.bodyVersion(committedKey);
    JITCACHE_CHECK(token);
    JITCACHE_CHECK(!state.bodyVersion(absentKey));
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Listing, EIO, std::nullopt });
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(!state.progress().bodyOpens);

    // openBody maps and checks the file, whose body carries the file's commit identifier, size, L and P.
    BodyLookup found = state.openBody(committedKey);
    JITCACHE_CHECK(found.kind() == BodyLookup::Kind::Found);
    if (RefPtr body = found.body()) {
        JITCACHE_CHECK(body->key() == committedKey);
        JITCACHE_CHECK(body->version() == committed->version);
        JITCACHE_CHECK(body->fileSize() == committed->fileSize);
        JITCACHE_CHECK(body->llintThreshold() == testBodyLLIntThreshold);
        JITCACHE_CHECK(body->counterProgress() == testBodyCounterProgress);
    }
    JITCACHE_CHECK(state.openBody(absentKey).kind() == BodyLookup::Kind::Missing);
    JITCACHE_CHECK(state.progress().bodyOpens == 1);

    // A transient error is a miss, counted, and the artifact is not at fault.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EMFILE, std::nullopt });
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Missing);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(state.activityOn());
    JITCACHE_CHECK(JITCache::status(vm).progress.transientOpenFailures == 1);
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);

    // The override answers alone, a test body included, whose commit identifier differs from the token it gives, and
    // moves no counter.
    constexpr uint64_t overrideToken = 7;
    constexpr uint64_t testVersion = 99;
    std::array<uint8_t, 8> feedback { 1, 2, 3, 4, 5, 6, 7, 8 };
    std::array<ValidatedBody::TestSection, 1> testSections { { { SectionKind::UCBFeedback, feedback } } };
    Ref<ValidatedBody> testBody = ValidatedBody::createForTesting(committedKey, testVersion, testSections);
    auto setOverride = [&] {
        auto token = [&](const BodyKey& key) -> uint64_t {
            return key == committedKey ? overrideToken : 0;
        };
        auto open = [&](const BodyKey& key) {
            return key == committedKey ? BodyLookup::found(testBody.copyRef()) : BodyLookup::missing();
        };
        state.setBodyLookupForTesting(WTF::move(token), WTF::move(open));
    };
    setOverride();
    JITCACHE_CHECK(state.bodyVersion(committedKey) == overrideToken);
    JITCACHE_CHECK(!state.bodyVersion(absentKey));
    BodyLookup overridden = state.openBody(committedKey);
    JITCACHE_CHECK(overridden.kind() == BodyLookup::Kind::Found && overridden.body() == testBody.ptr());
    JITCACHE_CHECK(testBody->version() != state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(absentKey).kind() == BodyLookup::Kind::Missing);
    JITCACHE_CHECK(state.progress().bodyOpens == 1);
    JITCACHE_CHECK(state.progress().transientOpenFailures == 1);

    state.clearBodyLookupForTesting();
    JITCACHE_CHECK(state.bodyVersion(committedKey) == token);
    BodyLookup again = state.openBody(committedKey);
    JITCACHE_CHECK(again.kind() == BodyLookup::Kind::Found && again.body() && again.body()->version() == committed->version);

    // Last: any other error is invalid material at container.io, which turns activity off; both calls then answer 0 and
    // Unusable, with or without the override.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::Open, EIO, std::nullopt });
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    StoreTesting::setFault(std::nullopt);
    JITCACHE_CHECK(!state.activityOn());
    checkFault(context, "the open's fault"_s, JITCache::status(vm).activityFault, FaultClass::InvalidMaterial, "container.io"_s);
    JITCACHE_CHECK(!state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    setOverride();
    JITCACHE_CHECK(!state.bodyVersion(committedKey));
    JITCACHE_CHECK(state.openBody(committedKey).kind() == BodyLookup::Kind::Unusable);
    state.clearBodyLookupForTesting();
}

// The start event of harness sub-SPEC section 9.2. A start that configures the VM keeps its report, which
// flushBenchReport writes out with the summary lines, the producer budget's limit among them; a rejected start closes the
// report again with its start event written and no summary line.
JITCACHE_TEST(integratorStartBenchEvent, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    if (!createTestArtifact(context, artifact))
        return;

    String rejectedReport = directory->path("rejected.jsonl"_s);
    {
        ExtraVM rejected;
        Config config = strictConfig(directory->path("missing"_s), Role::Consumer);
        config.benchReportPath = rejectedReport;
        startsAs(context, "a rejected start with a bench report"_s, JITCache::start(rejected.vm(), config), StartOutcome::Rejected, "start.artifact-missing"_s);
    }
    String rejectedLines = readTextFile(rejectedReport);
    JITCACHE_CHECK(rejectedLines.contains("\"event\":\"start\""_s));
    JITCACHE_CHECK(rejectedLines.contains("\"outcome\":\"rejected\",\"role\":\"consumer\",\"nanoseconds\":"_s));
    JITCACHE_CHECK(rejectedLines.contains("\"indexedBodies\":0}"_s));
    JITCACHE_CHECK(!rejectedLines.contains("\"event\":\"budget\""_s));

    VM& vm = *context.vm();
    String startedReport = directory->path("started.jsonl"_s);
    Config config = strictConfig(artifact, Role::ConsumerProducer);
    config.benchReportPath = startedReport;
    if (!startsAs(context, "a start with a bench report"_s, JITCache::start(vm, config), StartOutcome::Started))
        return;
    JITCACHE_CHECK(vm.jitCacheState()->benchReport());
    flushBenchReport(vm);
    String startedLines = readTextFile(startedReport);
    JITCACHE_CHECK(startedLines.contains("\"outcome\":\"started\",\"role\":\"consumer-producer\",\"nanoseconds\":"_s));
    JITCACHE_CHECK(startedLines.contains("\"event\":\"budget\""_s));
    JITCACHE_CHECK(startedLines.contains(makeString("\"limit\":"_s, static_cast<uint64_t>(testProducerLimitBytes))));
}

// The JSON lines of section 3.1: every field of the struct by name, enums by their names, an absent optional as null and
// a fault as {"class","step","detail"}; and the names and parseRole they share.
JITCACHE_TEST(integratorJSONLines, No)
{
    StartResult rejected { StartOutcome::Rejected, "start.config"_s, "artifactPath \"x\" is empty"_s };
    JITCACHE_CHECK(toJSON(rejected) == "{\"jitcache\":\"start\",\"outcome\":\"rejected\",\"step\":\"start.config\",\"detail\":\"artifactPath \\\"x\\\" is empty\"}"_s);
    StartResult started { StartOutcome::Started, { }, { } };
    JITCACHE_CHECK(toJSON(started) == "{\"jitcache\":\"start\",\"outcome\":\"started\",\"step\":\"\",\"detail\":\"\"}"_s);
    StartResult replaced { StartOutcome::Started, "start.replaced"_s, "its header differs in evalMode"_s };
    JITCACHE_CHECK(toJSON(replaced) == "{\"jitcache\":\"start\",\"outcome\":\"started\",\"step\":\"start.replaced\",\"detail\":\"its header differs in evalMode\"}"_s);

    DeltaResult faulted { DeltaOutcome::Faulted, { }, FaultReport { FaultClass::RecordingFault, "ucb"_s, "capture-budget"_s, "a detail"_s }, 3, 1, 4096, 2 };
    JITCACHE_CHECK(toJSON(faulted) == "{\"jitcache\":\"delta\",\"outcome\":\"faulted\",\"rejection\":\"\",\"fault\":{\"class\":\"recording-fault\",\"step\":\"ucb.capture-budget\",\"detail\":\"a detail\"},\"eligibleKeys\":3,\"committedBodies\":1,\"committedBytes\":4096,\"deferredKeys\":2}"_s);
    DeltaResult rejectedDelta { DeltaOutcome::Rejected, "delta.role"_s, std::nullopt };
    JITCACHE_CHECK(toJSON(rejectedDelta) == "{\"jitcache\":\"delta\",\"outcome\":\"rejected\",\"rejection\":\"delta.role\",\"fault\":null,\"eligibleKeys\":0,\"committedBodies\":0,\"committedBytes\":0,\"deferredKeys\":0}"_s);

    JITCACHE_CHECK(toJSON(Status { }) == "{\"jitcache\":\"status\",\"state\":\"unconfigured\",\"role\":null,\"strict\":false,\"activityOn\":false,\"activityFault\":null,\"production\":\"not-producing\",\"productionFault\":null,\"firstFault\":null,\"progress\":{\"indexedBodies\":0,\"bodyOpens\":0,\"transientOpenFailures\":0,\"imports\":0,\"seededDecodes\":0,\"attaches\":0,\"gateDrops\":0,\"misses\":0,\"installs\":0,\"bakedFactMismatches\":0,\"captureCandidates\":0,\"capturesDeferred\":0,\"capturesCommitted\":0,\"bytesCommitted\":0,\"deltaRuns\":0},\"budget\":{\"limitBytes\":0,\"chargedBytes\":0,\"peakBytes\":0,\"refused\":false}}"_s);
    Status faultedStatus;
    faultedStatus.state = SessionState::Started;
    faultedStatus.role = Role::ConsumerProducer;
    faultedStatus.activityFault = FaultReport { FaultClass::DebuggerAttached, { }, "debugger.attach"_s, { } };
    faultedStatus.production = ProductionState::Ended;
    faultedStatus.progress.installs = 5;
    faultedStatus.budget.refused = true;
    String json = toJSON(faultedStatus);
    JITCACHE_CHECK(json.contains("\"state\":\"started\""_s));
    JITCACHE_CHECK(json.contains("\"role\":\"consumer-producer\""_s));
    JITCACHE_CHECK(json.contains("\"activityFault\":{\"class\":\"debugger-attached\",\"step\":\"debugger.attach\",\"detail\":\"\"}"_s));
    JITCACHE_CHECK(json.contains("\"production\":\"ended\""_s));
    JITCACHE_CHECK(json.contains("\"installs\":5"_s));
    JITCACHE_CHECK(json.contains("\"refused\":true}}"_s));
    faultedStatus.state = SessionState::Faulted;
    JITCACHE_CHECK(toJSON(faultedStatus).contains("\"state\":\"faulted\""_s));

    JITCACHE_CHECK((FaultReport { FaultClass::InvalidMaterial, { }, "container.io"_s, { } }.stepName() == "container.io"_s));
    JITCACHE_CHECK(name(StartOutcome::Started) == "started"_s);
    JITCACHE_CHECK(name(StartOutcome::Busy) == "busy"_s);
    JITCACHE_CHECK(name(StartOutcome::Rejected) == "rejected"_s);
    JITCACHE_CHECK(name(StartOutcome::Fault) == "fault"_s);
    JITCACHE_CHECK(name(FaultClass::ExecutableMemory) == "executable-memory"_s);
    for (Role role : { Role::Consumer, Role::Producer, Role::ConsumerProducer })
        JITCACHE_CHECK(parseRole(StringView { name(role) }) == role);
    JITCACHE_CHECK(!parseRole("Consumer"_s));
    JITCACHE_CHECK(!parseRole("p-c"_s));
    JITCACHE_CHECK(name(static_cast<Role>(7)).isNull());
}

// Harness sub-SPEC H3: a group's options hold their values once JSC::initialize returns. The runner runs this test as
// testjitcache --group="--useConcurrentJIT=false --numberOfGCMarkers=1", and the runner's self-test checks that --list
// names it with exactly that string.
JITCACHE_TEST_WITH_OPTIONS(integratorHarnessReadsGroupOptions, No, "--useConcurrentJIT=false --numberOfGCMarkers=1")
{
    JITCACHE_CHECK(!Options::useConcurrentJIT());
    JITCACHE_CHECK(Options::numberOfGCMarkers() == 1);
}

// H3: testjitcache sets a group's options before notifyOptionsChanged derives the dependent ones, so a group that turns
// the JIT off also has the baseline JIT off.
JITCACHE_TEST_WITH_OPTIONS(integratorHarnessDerivesDependentOptions, No, "--useJIT=false")
{
    JITCACHE_CHECK(!Options::useJIT());
    JITCACHE_CHECK(!Options::useBaselineJIT());
}

// H3: a failing check prints its file, line and message, and the process exits with 1. The check fails only when the
// runner's self-test names this test in JITCACHE_TEST_FAIL_ON_PURPOSE, so every other run passes it.
JITCACHE_TEST(integratorHarnessReportsAFailingCheck, No)
{
    const char* purpose = getenv("JITCACHE_TEST_FAIL_ON_PURPOSE");
    bool failsOnPurpose = purpose && StringView::fromLatin1(purpose) == "integratorHarnessReportsAFailingCheck"_s;
    JITCACHE_CHECK(!failsOnPurpose);
}

// H4: every line of the twin report parses as JSON, with the fields in the order section 2 of the harness sub-SPEC gives
// them and each value as written, whatever characters a detail holds: quotes, backslashes, control characters,
// characters outside ASCII and a lone surrogate. WTF's parser also accepts raw control characters and the escapes \x
// and \v, so each line must first pass jsonGrammarProblem, as the runner's JSON.parse would require. The runner's
// self-test abort.js shows the other half of H4, that a line written right before an abort() is in the file.
JITCACHE_TEST(integratorTwinReportLinesParseAsJSON, No)
{
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String path = directory->path("twins.jsonl"_s);

    constexpr std::array<char16_t, 3> loneSurrogate { u'a', 0xD800, u'b' };
    const Vector<String> details {
        emptyString(),
        "a plain detail"_s,
        "a \"quoted\" word and a \\ backslash"_s,
        "a line break\n, a tab\t, a return\r and the controls \x01 and \x1f"_s,
        String::fromUTF8("caf\xC3\xA9 and \xF0\x9F\x98\x80"),
        String(std::span<const char16_t> { loneSurrogate }),
    };
    constexpr std::array<std::pair<TwinPart, ASCIILiteral>, 5> parts { {
        { TwinPart::UCB, "ucb"_s },
        { TwinPart::Image, "image"_s },
        { TwinPart::CB, "cb"_s },
        { TwinPart::ICs, "ics"_s },
        { TwinPart::Integrator, "integrator"_s },
    } };
    constexpr std::array<std::pair<RelocationDomain, ASCIILiteral>, 4> domains { {
        { RelocationDomain::EngineImage, "engine-image"_s },
        { RelocationDomain::ExecutablePool, "executable-pool"_s },
        { RelocationDomain::StructureReservation, "structure-reservation"_s },
        { RelocationDomain::Heap, "heap"_s },
    } };

    // Each line the report should hold, as its fields' names and values in order.
    using Fields = Vector<std::pair<String, String>>;
    Vector<Fields> expected;
    {
        auto report = TwinReport::open(path);
        if (!report) {
            JITCACHE_FAIL(makeString("cannot open a twin report at "_s, path));
            return;
        }
        for (size_t i = 0; i < details.size(); ++i) {
            auto [part, partName] = parts[i % parts.size()];
            auto [domain, domainName] = domains[i % domains.size()];
            report->difference(part, "a-difference"_s, details[i]);
            expected.append({ { "kind"_s, "difference"_s }, { "part"_s, partName }, { "check"_s, "a-difference"_s }, { "detail"_s, details[i] } });
            report->skip(part, "a-skip"_s, details[i]);
            expected.append({ { "kind"_s, "skip"_s }, { "part"_s, partName }, { "check"_s, "a-skip"_s }, { "reason"_s, details[i] } });
            report->relocationCoincidence(domain, details[i]);
            expected.append({ { "kind"_s, "coincidence"_s }, { "domain"_s, domainName }, { "detail"_s, details[i] } });
        }
        JITCACHE_CHECK(report->differences() == details.size());
        JITCACHE_CHECK(report->skips() == details.size());
        JITCACHE_CHECK(report->coincidences() == details.size());
    }

    String text = readTextFile(path);
    if (text.isNull()) {
        JITCACHE_FAIL(makeString("cannot read the twin report at "_s, path));
        return;
    }
    JITCACHE_CHECK(text.endsWith('\n'));
    Vector<String> lines = text.split('\n');
    if (lines.size() != expected.size()) {
        JITCACHE_FAIL(makeString("the report holds "_s, lines.size(), " lines, expected "_s, expected.size()));
        return;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (String problem = jsonGrammarProblem(lines[i]); !problem.isNull()) {
            JITCACHE_FAIL(makeString("line "_s, i + 1, " breaks JSON's grammar with "_s, problem, ": "_s, lines[i]));
            continue;
        }
        RefPtr<JSON::Value> value = JSON::Value::parseJSON(lines[i]);
        RefPtr<JSON::Object> object = value ? value->asObject() : nullptr;
        if (!object) {
            JITCACHE_FAIL(makeString("line "_s, i + 1, " is no JSON object: "_s, lines[i]));
            continue;
        }
        const Fields& fields = expected[i];
        Vector<String> names = fields.map([](auto& field) {
            return field.first;
        });
        if (object->keys() != names) {
            JITCACHE_FAIL(makeString("line "_s, i + 1, " does not hold the expected fields in order: "_s, lines[i]));
            continue;
        }
        for (auto& [fieldName, fieldValue] : fields) {
            if (object->getString(fieldName) != fieldValue)
                JITCACHE_FAIL(makeString("line "_s, i + 1, "'s "_s, fieldName, " is not the value written: "_s, lines[i]));
        }
    }
}

// T-SCORE: beats compares scores field by field in THREAD Capture's order, a withheld counter above one that travels, and
// a candidate wins only when it is strictly greater, so a tie keeps the saved body.
JITCACHE_TEST(integratorBeatsOrdersScores, No)
{
    const CaptureScore saved { 1, 10, 2, false, 50 };
    JITCACHE_CHECK(!beats(saved, saved));

    struct OneField {
        ASCIILiteral field;
        CaptureScore higher;
        CaptureScore lower;
    };
    std::array oneFieldChanges {
        OneField { "tier"_s, { 2, 10, 2, false, 50 }, { 0, 10, 2, false, 50 } },
        OneField { "richness"_s, { 1, 11, 2, false, 50 }, { 1, 9, 2, false, 50 } },
        OneField { "icSitesWithCases"_s, { 1, 10, 3, false, 50 }, { 1, 10, 1, false, 50 } },
        OneField { "counterWithheld"_s, { 1, 10, 2, true, 50 }, { 1, 10, 2, false, 50 } },
        OneField { "counterProgress"_s, { 1, 10, 2, false, 51 }, { 1, 10, 2, false, 49 } },
    };
    for (auto& [field, higher, lower] : oneFieldChanges) {
        // The counterWithheld row's lower score is the saved one itself, which ties.
        bool lowerTies = sameScore(lower, saved);
        if (!beats(higher, saved) || beats(saved, higher) || beats(lower, saved) || (!lowerTies && !beats(saved, lower)))
            JITCACHE_FAIL(makeString("beats misorders scores that differ only in "_s, field));
    }

    // A withheld counter beats one that travels with more progress, at equal richness and IC sites, and loses the reverse.
    const CaptureScore withheld { 1, 10, 2, true, 0 };
    const CaptureScore travels { 1, 10, 2, false, 1000 };
    JITCACHE_CHECK(beats(withheld, travels));
    JITCACHE_CHECK(!beats(travels, withheld));

    // An earlier field decides before every later one.
    JITCACHE_CHECK(beats({ 2, 0, 0, false, 0 }, { 1, 100, 100, true, 1000 }));
    JITCACHE_CHECK(beats({ 1, 11, 0, false, 0 }, { 1, 10, 100, true, 1000 }));
    JITCACHE_CHECK(beats({ 1, 10, 3, false, 0 }, { 1, 10, 2, true, 1000 }));
    JITCACHE_CHECK(!beats({ 1, 9, 100, true, 1000 }, { 1, 10, 0, false, 0 }));

    // Any baseline capture beats the empty score that stands for an absent body.
    JITCACHE_CHECK(beats({ 1, 0, 0, false, 0 }, CaptureScore { }));
    JITCACHE_CHECK(!beats(CaptureScore { }, CaptureScore { }));
}

#if ENABLE(JIT)

// T-SCORE: scoreSections over a committed body's three summary sections gives its kept score in both modes, and with
// strict on, a malformed feedback, CB summary or ICs span, the others whole, names the reader that rejected it, with the
// part and check section 8.2 raises.
JITCACHE_TEST(integratorScoreSectionsNamesTheReader, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* function = globalObject ? globalFunction(context, globalObject, "firstBody"_s) : nullptr;
    CodeBlock* codeBlock = function ? bringToBaseline(context, globalObject, function, globalValue(globalObject, "shapeA1"_s)) : nullptr;
    std::optional<BodyKey> key = codeBlock ? recordedKey(context, vm, *codeBlock) : std::nullopt;
    if (!key)
        return;

    // The finalize capture committed the body and kept the score of the bytes it built.
    std::optional<SavedScore> kept = keptScoreForTesting(vm, *key);
    BodyLookup lookup = state.openBody(*key);
    RefPtr<ValidatedBody> body = lookup.body();
    if (!kept || !body) {
        JITCACHE_FAIL(makeString("the finalize capture left no "_s, kept ? "body"_s : "kept summary"_s));
        return;
    }
    JITCACHE_CHECK(body->version() == kept->version);
    std::span<const uint8_t> feedback = body->section(SectionKind::UCBFeedback);
    std::span<const uint8_t> summary = body->section(SectionKind::CBSummaryBaseline);
    std::span<const uint8_t> ics = body->section(SectionKind::ICsBaseline);
    for (bool strict : { true, false }) {
        auto score = scoreSections(body->highestTier(), feedback, summary, ics, strict);
        if (!score || !sameScore(*score, kept->score))
            JITCACHE_FAIL(makeString("scoreSections with strict "_s, strict ? "on"_s : "off"_s, " differs from the kept score "_s, scoreText(kept->score)));
    }
    if (feedback.empty() || summary.empty() || ics.empty()) {
        JITCACHE_FAIL("a committed body lacks a summary section"_s);
        return;
    }

    auto checkRejected = [&](ASCIILiteral label, std::span<const uint8_t> feedbackSpan, std::span<const uint8_t> summarySpan, std::span<const uint8_t> icsSpan,
        ASCIILiteral part, ASCIILiteral check) {
        auto score = scoreSections(body->highestTier(), feedbackSpan, summarySpan, icsSpan, true);
        if (score) {
            JITCACHE_FAIL(makeString(label, ": scoreSections accepted it"_s));
            return;
        }
        if (score.error().part != part || score.error().check != check)
            JITCACHE_FAIL(makeString(label, ": rejected by "_s, score.error().part, " at "_s, score.error().check, ", expected "_s, part, " at "_s, check));
    };

    // Each span one byte short of its layout, the others whole.
    checkRejected("a short ucb.feedback"_s, feedback.first(feedback.size() - 1), summary, ics, "ucb"_s, "saved-summary"_s);
    auto shortSummary = summary.first(summary.size() - 1);
    auto summaryCheck = decodeSummary(shortSummary, true);
    JITCACHE_CHECK(!summaryCheck);
    if (!summaryCheck)
        checkRejected("a short cb.summary"_s, feedback, shortSummary, ics, "cb"_s, description(summaryCheck.error().check));
    checkRejected("a short ICsBaseline"_s, feedback, summary, ics.first(ics.size() - 1), "ics"_s, "SectionSize"_s);

    // An ICs header that counts more IC sites with cases than ICs breaks A2.
    Vector<uint8_t> unbounded;
    unbounded.append(ics);
    ICs::SectionHeader header;
    memcpySpan(asMutableByteSpan(header), unbounded.span().first(sizeof(header)));
    header.icSitesWithCases = header.propertyICCount + 1;
    memcpySpan(unbounded.mutableSpan().first(sizeof(header)), asByteSpan(header));
    checkRejected("an ICsBaseline with more IC sites with cases than ICs"_s, feedback, summary, unbounded.span(), "ics"_s, "SummaryBound"_s);
}

// T-DELTA: delta rejects at each step of section 3.4 a test can reach, commits a body in a Producer, and reports Faulted
// after a recording fault.
JITCACHE_TEST(integratorDeltaRejectsCommitsAndFaults, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    String artifact = directory->path("artifact"_s);
    VM& vm = *context.vm();
    auto checkRejected = [&](ASCIILiteral label, const DeltaResult& result, ASCIILiteral step) {
        if (result.outcome != DeltaOutcome::Rejected || result.rejection != step || result.fault || result.eligibleKeys || result.committedBodies)
            JITCACHE_FAIL(makeString(label, ": expected a rejection at "_s, step, ", got "_s, toJSON(result)));
    };

    checkRejected("a VM without state"_s, JITCache::delta(vm), "delta.unconfigured"_s);
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(artifact, Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    {
        ExtraVM consumer;
        if (startsAs(context, "a consumer"_s, JITCache::start(consumer.vm(), strictConfig(artifact, Role::Consumer)), StartOutcome::Started))
            checkRejected("a consumer"_s, JITCache::delta(consumer.vm()), "delta.role"_s);
    }
    {
        ReleaseHeapAccessScope withoutHeapAccess(vm.heap);
        checkRejected("a call without heap access"_s, JITCache::delta(vm), "delta.locks"_s);
    }

    // The finalize capture commits the body with no IC site with cases. In baseline code the first call spends the
    // site's countdown and the second caches a case, so delta's capture beats the saved body.
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* function = globalObject ? globalFunction(context, globalObject, "firstBody"_s) : nullptr;
    JSValue shape = globalObject ? globalValue(globalObject, "shapeA1"_s) : JSValue();
    CodeBlock* codeBlock = function ? bringToBaseline(context, globalObject, function, shape) : nullptr;
    std::optional<BodyKey> key = codeBlock ? recordedKey(context, vm, *codeBlock) : std::nullopt;
    if (!key || !callWith(context, globalObject, function, shape) || !callWith(context, globalObject, function, shape))
        return;
    std::optional<SavedScore> finalizeCapture = keptScoreForTesting(vm, *key);
    JITCACHE_CHECK(finalizeCapture && !finalizeCapture->score.icSitesWithCases);

    Progress before = state.progress();
    DeltaResult committed = JITCache::delta(vm);
    JITCACHE_CHECK(committed.outcome == DeltaOutcome::Completed);
    JITCACHE_CHECK(committed.rejection.isNull() && !committed.fault);
    JITCACHE_CHECK(committed.eligibleKeys == 1);
    JITCACHE_CHECK(committed.committedBodies == 1);
    JITCACHE_CHECK(!committed.deferredKeys);
    JITCACHE_CHECK(committed.committedBytes && committed.committedBytes == state.progress().bytesCommitted - before.bytesCommitted);
    JITCACHE_CHECK(state.progress().capturesCommitted == before.capturesCommitted + 1);
    JITCACHE_CHECK(state.progress().deltaRuns == before.deltaRuns + 1);
    std::optional<SavedScore> deltaCapture = keptScoreForTesting(vm, *key);
    JITCACHE_CHECK(deltaCapture && deltaCapture->score.icSitesWithCases == 1);
    JITCACHE_CHECK(deltaCapture && finalizeCapture && deltaCapture->version != finalizeCapture->version);

    // A recording fault ends production: delta returns Faulted with it and commits nothing.
    state.raiseRecordingFault("cb"_s, "test-check"_s, { });
    DeltaResult faulted = JITCache::delta(vm);
    JITCACHE_CHECK(faulted.outcome == DeltaOutcome::Faulted);
    JITCACHE_CHECK(!faulted.committedBodies && !faulted.committedBytes);
    checkFault(context, "delta's fault"_s, faulted.fault, FaultClass::RecordingFault, "cb.test-check"_s);
    JITCACHE_CHECK(state.progress().deltaRuns == before.deltaRuns + 1);
}

#endif // ENABLE(JIT)

// Section 4.5: a refusal made off the VM thread reaches it at the next delta, which raises budget.limit and returns
// Faulted with it.
JITCACHE_TEST(integratorDeltaRaisesAnUnraisedRefusal, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JITCACHE_CHECK(!state.producerBudget()->tryCharge(testProducerLimitBytes + 1));
    JITCACHE_CHECK(state.productionActive());
    DeltaResult result = JITCache::delta(vm);
    JITCACHE_CHECK(result.outcome == DeltaOutcome::Faulted);
    checkFault(context, "delta's fault"_s, result.fault, FaultClass::RecordingFault, "budget.limit"_s);
    JITCACHE_CHECK(!state.productionActive());
    JITCACHE_CHECK(!state.progress().deltaRuns);
}

#if ENABLE(JIT)

// T-CHARGE: once the writer's staging buffer and the kept-summary table exist, a commit of a key the index lacks charges
// exactly six of the kept-summary table's bucket sizes and six of the index's, and a later commit of the same key
// charges neither. A recording fault then releases the kept summaries', the index entries' and the staging buffer's
// charges at the next glue entry, while both keys stay in the index.
JITCACHE_TEST(integratorCommitChargesItsEntries, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* firstFunction = globalObject ? globalFunction(context, globalObject, "firstBody"_s) : nullptr;
    JSFunction* secondFunction = globalObject ? globalFunction(context, globalObject, "secondBody"_s) : nullptr;
    if (!firstFunction || !secondFunction)
        return;
    JSValue shape = globalValue(globalObject, "shapeA1"_s);
    auto chargedBytes = [&] {
        return static_cast<uint64_t>(JITCache::status(vm).budget.chargedBytes);
    };

    // The first key's finalize capture commits it, which allocates the staging buffer and creates the kept summaries.
    CodeBlock* firstCodeBlock = bringToBaseline(context, globalObject, firstFunction, shape);
    std::optional<BodyKey> firstKey = firstCodeBlock ? recordedKey(context, vm, *firstCodeBlock) : std::nullopt;
    if (!firstKey)
        return;
    JITCACHE_CHECK(state.progress().capturesCommitted == 1);
    JITCACHE_CHECK(state.keptSummaries());

    // The second key's scoring read finds the body unavailable, so its finalize capture defers and delta commits it first.
    StoreTesting::setFault(StoreTesting::Fault { StoreTesting::Call::ReadSavedSummaries, EMFILE, std::nullopt });
    CodeBlock* secondCodeBlock = bringToBaseline(context, globalObject, secondFunction, shape);
    StoreTesting::setFault(std::nullopt);
    std::optional<BodyKey> secondKey = secondCodeBlock ? recordedKey(context, vm, *secondCodeBlock) : std::nullopt;
    if (!secondKey)
        return;
    JITCACHE_CHECK(state.progress().capturesCommitted == 1);
    JITCACHE_CHECK(state.progress().capturesDeferred == 1);
    JITCACHE_CHECK(state.bodyVersion(*firstKey));
    JITCACHE_CHECK(!state.bodyVersion(*secondKey));
    JITCACHE_CHECK(!keptScoreForTesting(vm, *secondKey));

    constexpr uint64_t keptBucketBytes = sizeof(KeyValuePair<BodyKey, SavedScore>);
    constexpr uint64_t indexBucketBytes = sizeof(KeyValuePair<BodyKey, IndexEntry>);
    uint64_t beforeFirstCommit = chargedBytes();
    DeltaResult firstCommit = JITCache::delta(vm);
    JITCACHE_CHECK(firstCommit.outcome == DeltaOutcome::Completed && firstCommit.committedBodies == 1);
    std::optional<SavedScore> firstCommitScore = keptScoreForTesting(vm, *secondKey);
    JITCACHE_CHECK(firstCommitScore);
    JITCACHE_CHECK(state.bodyVersion(*secondKey));
    uint64_t afterFirstCommit = chargedBytes();
    if (afterFirstCommit != beforeFirstCommit + 6 * keptBucketBytes + 6 * indexBucketBytes)
        JITCACHE_FAIL(makeString("the second key's first commit charged "_s, static_cast<int64_t>(afterFirstCommit - beforeFirstCommit), " bytes, expected "_s, 6 * keptBucketBytes + 6 * indexBucketBytes));

    // Two calls cache a case at the second key's site, which its saved body lacks, so delta commits the key again.
    if (!callWith(context, globalObject, secondFunction, shape) || !callWith(context, globalObject, secondFunction, shape))
        return;
    DeltaResult secondCommit = JITCache::delta(vm);
    JITCACHE_CHECK(secondCommit.outcome == DeltaOutcome::Completed && secondCommit.committedBodies == 1);
    std::optional<SavedScore> secondCommitScore = keptScoreForTesting(vm, *secondKey);
    JITCACHE_CHECK(secondCommitScore && firstCommitScore && secondCommitScore->version != firstCommitScore->version);
    JITCACHE_CHECK(chargedBytes() == afterFirstCommit);

    // A fault entry point frees nothing; the next glue entry releases the kept summaries' table and two entries, the two
    // index entries' charges and the staging buffer. What stays charged is the two image records the bodies' code holds.
    state.raiseRecordingFault("cb"_s, "test-check"_s, { });
    JITCACHE_CHECK(chargedBytes() == afterFirstCommit);
    JITCACHE_CHECK(JITCache::delta(vm).outcome == DeltaOutcome::Faulted);
    uint64_t released = (8 + 2 * 6) * keptBucketBytes + 2 * 6 * indexBucketBytes + writerStagingBytes;
    if (chargedBytes() + released != afterFirstCommit)
        JITCACHE_FAIL(makeString("the end of production released "_s, static_cast<int64_t>(afterFirstCommit - chargedBytes()), " bytes, expected "_s, released));
    JITCACHE_CHECK(!state.keptSummaries());
    JITCACHE_CHECK(state.bodyVersion(*firstKey));
    JITCACHE_CHECK(state.bodyVersion(*secondKey));
}

// T-STAMP: of two bodies whose counters have made progress, the one whose in_by_id site lists two cases has a polymorphic
// site, so delta commits it with its counter withheld: P 0 in the envelope and NotCarried in cb.state and cb.summary. The
// other carries its counter, and its envelope's P is its cb.summary's progress. Each kept score equals scoreSections of
// the sections openBody returns for the body (II22, II23).
JITCACHE_TEST(integratorDeltaStampsTheCounter, Yes)
{
    ProcessHooksScope hooks;
    auto directory = TemporaryDirectory::create(context);
    if (!directory)
        return;
    VM& vm = *context.vm();
    if (!startsAs(context, "the producer"_s, JITCache::start(vm, strictConfig(directory->path("artifact"_s), Role::Producer)), StartOutcome::Started))
        return;
    VMState& state = *vm.jitCacheState();
    JSGlobalObject* globalObject = createCaptureRealm(context, vm);
    JSFunction* polymorphicFunction = globalObject ? globalFunction(context, globalObject, "firstBody"_s) : nullptr;
    JSFunction* monomorphicFunction = globalObject ? globalFunction(context, globalObject, "secondBody"_s) : nullptr;
    if (!polymorphicFunction || !monomorphicFunction)
        return;
    JSValue shapeA1 = globalValue(globalObject, "shapeA1"_s);
    JSValue shapeA2 = globalValue(globalObject, "shapeA2"_s);
    JSValue shapeB = globalValue(globalObject, "shapeB"_s);
    CodeBlock* polymorphic = bringToBaseline(context, globalObject, polymorphicFunction, shapeA1);
    CodeBlock* monomorphic = bringToBaseline(context, globalObject, monomorphicFunction, shapeA1);
    if (!polymorphic || !monomorphic)
        return;

    // In baseline code each site's first visit spends its countdown and each later one with a new shape caches it: shapes
    // A, A and B leave two cases, and A three times leaves one. Each call adds an entry's points to the baseline counter.
    for (JSValue shape : { shapeA1, shapeA2, shapeB }) {
        if (!callWith(context, globalObject, polymorphicFunction, shape))
            return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!callWith(context, globalObject, monomorphicFunction, shapeA1))
            return;
    }
    DeltaResult result = JITCache::delta(vm);
    JITCACHE_CHECK(result.outcome == DeltaOutcome::Completed);
    JITCACHE_CHECK(result.committedBodies == 2);

    struct Expected {
        ASCIILiteral label;
        CodeBlock* codeBlock;
        bool counterWithheld;
    };
    for (auto& [label, codeBlock, counterWithheld] : std::array { Expected { "the polymorphic body"_s, polymorphic, true }, Expected { "the monomorphic body"_s, monomorphic, false } }) {
        std::optional<BodyKey> key = recordedKey(context, vm, *codeBlock);
        if (!key)
            return;
        std::optional<SavedScore> kept = keptScoreForTesting(vm, *key);
        BodyLookup lookup = state.openBody(*key);
        RefPtr<ValidatedBody> body = lookup.body();
        if (!kept || !body) {
            JITCACHE_FAIL(makeString(label, " has no "_s, kept ? "body"_s : "kept summary"_s));
            continue;
        }
        JITCACHE_CHECK(body->version() == kept->version);
        JITCACHE_CHECK(kept->score.counterWithheld == counterWithheld);

        std::span<const uint8_t> stateSection = body->section(SectionKind::CBStateBaseline);
        std::span<const uint8_t> summarySection = body->section(SectionKind::CBSummaryBaseline);
        if (stateSection.size() < sizeof(CBFormat::StateHeader) || summarySection.size() < sizeof(CBFormat::SummaryHeader)) {
            JITCACHE_FAIL(makeString(label, ": cb.state or cb.summary is shorter than its header"_s));
            continue;
        }
        CBFormat::StateHeader stateHeader;
        memcpySpan(asMutableByteSpan(stateHeader), stateSection.first(sizeof(stateHeader)));
        CBFormat::SummaryHeader summaryHeader;
        memcpySpan(asMutableByteSpan(summaryHeader), summarySection.first(sizeof(summaryHeader)));
        auto expectedMode = static_cast<uint8_t>(counterWithheld ? CBFormat::CounterMode::NotCarried : CBFormat::CounterMode::Carried);
        if (stateHeader.counterMode != expectedMode || summaryHeader.counterMode != expectedMode) {
            JITCACHE_FAIL(makeString(label, ": the counter modes are "_s, static_cast<unsigned>(stateHeader.counterMode), " and "_s,
                static_cast<unsigned>(summaryHeader.counterMode), ", expected "_s, static_cast<unsigned>(expectedMode)));
        }
        if (counterWithheld) {
            JITCACHE_CHECK(!body->counterProgress());
            JITCACHE_CHECK(!summaryHeader.counterProgress);
        } else {
            JITCACHE_CHECK(summaryHeader.counterProgress);
            JITCACHE_CHECK(body->counterProgress() == summaryHeader.counterProgress);
        }
        JITCACHE_CHECK(body->counterProgress() == kept->score.counterProgress);

        auto scored = scoreSections(body->highestTier(), body->section(SectionKind::UCBFeedback), summarySection, body->section(SectionKind::ICsBaseline), true);
        if (!scored || !sameScore(*scored, kept->score))
            JITCACHE_FAIL(makeString(label, ": the kept score "_s, scoreText(kept->score), " differs from scoreSections of its sections"_s));
    }
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
