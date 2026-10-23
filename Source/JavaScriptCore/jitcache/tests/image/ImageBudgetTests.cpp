#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BaselineJITCode.h"
#include "CCallHelpers.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "FunctionExecutable.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "LinkBuffer.h"
#include "SourceCode.h"
#include "jitcache/base/ProducerBudget.h"
#include "jitcache/image/ImageCapture.h"
#include "jitcache/image/ImageEmission.h"
#include "jitcache/image/ImageRecord.h"
#include "jitcache/image/ImageRecorder.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/session/JITCacheAPI.h"
#include "jitcache/session/ProducerContext.h"
#include "jitcache/tests/image/ImageTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include "jitcache/tests/support/TestVM.h"
#include <array>
#include <optional>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

// Tests of how image recording meets the producer budget (image/ImageRecorder.h and .cpp, with the finalize capture and
// delta that raise a refusal): a refused charge leaves the record Incomplete, which no capture reads, while every jump the
// code emits stays linked, whether the refusal falls in the main pass, the slow paths, emitVeneers or a MathIC snippet,
// and the records give back every byte they charged.

namespace JSC::JITCache::Tests {

namespace ImageBudgetTestsInternal {

// A program whose function jitcacheBig needs many of kRecordChargeStep's steps of record: 160 property loads, each feeding
// a mul and an add MathIC, with their slow calls, profile writes and exception checks. The program's own loops stay in the
// LLInt, so jitcacheBig is the first body that compiles.
static String bigBodySource()
{
    StringBuilder source;
    source.append("function jitcacheBig(o) {\n    var sum = 0;\n"_s);
    for (unsigned index = 0; index < 160; ++index)
        source.append("    sum = sum + o.p"_s, index, " * "_s, index + 1, ";\n"_s);
    source.append("    return sum;\n}\n"_s);
    source.append("var jitcacheBigObject = {};\nfor (var k = 0; k < 160; ++k)\n    jitcacheBigObject['p' + k] = k;\n"_s);
    source.append("for (var i = 0; i < 60; ++i)\n    jitcacheBig(jitcacheBigObject);\n"_s);
    return source.toString();
}

// The body of the budget sweep. Each of its 160 statements loads a property, multiplies and adds it, whose MathICs' slow
// calls end with exception checks in the slow paths, and concatenates a constant string, a site with no inline code, so
// its operation call and exception check sit in the main pass. jitcacheSweep runs once, in the LLInt, which fills the
// profiles those shapes follow. It holds no switch and no strict equality, so a twin recorder answers nothing from its
// seeds and inputs but the assembler's seed.
static String sweepBodySource()
{
    constexpr unsigned statements = 160;
    StringBuilder source;
    source.append("function jitcacheSweep(o, s) {\n    var sum = 0;\n    var text = \"\";\n"_s);
    for (unsigned index = 0; index < statements; ++index)
        source.append("    sum = sum + o.p"_s, index, " * "_s, index + 1, ";\n    text = \"k\" + s;\n"_s);
    source.append("    return sum + text.length;\n}\n"_s);
    source.append("var jitcacheSweepObject = {};\nfor (var k = 0; k < "_s, statements, "; ++k)\n    jitcacheSweepObject['p' + k] = k;\njitcacheSweep(jitcacheSweepObject, 's');\n"_s);
    return source.toString();
}

#if CPU(ARM64)
// An exception check's jump to HandleException. Deferred while the recorder records, it reaches the thunk through a
// veneer, the b emitVeneers placed at the image's tail; deferred after the recorder stopped, it is linked natively, by
// its own branch or, when compaction kept the inverted form, by the b right after it.
struct ExceptionCheckJump {
    uint32_t offset;
    bool throughVeneer;
};

// The conditional branches that reach handleException in [start, arityEntry), the image's prologue, main pass and slow
// paths, which the passes emitted, and so deferred, in code order. allocationEnd bounds the image's allocation. Nullopt
// when a conditional branch there targets itself, the form the assembler emits for one that nothing linked.
static std::optional<Vector<ExceptionCheckJump>> exceptionCheckJumps(uintptr_t start, uintptr_t arityEntry, uintptr_t allocationEnd, uintptr_t handleException)
{
    auto isConditional = [](uint32_t word) {
        return (word & 0xff000010) == 0x54000000 || (word & 0x7e000000) == 0x34000000 || (word & 0x7e000000) == 0x36000000;
    };
    Vector<ExceptionCheckJump> jumps;
    for (uintptr_t address = start; address < arityEntry; address += 4) {
        if (!isConditional(readWord(address)))
            continue;
        uintptr_t target = *arm64BranchTarget(address);
        if (target == address)
            return std::nullopt;
        // The inverted form branches over the b that takes the jump.
        if (target == address + 8 && (readWord(address + 4) & 0xfc000000) == 0x14000000)
            target = *arm64BranchTarget(address + 4);
        if (!reaches(target, handleException))
            continue;
        bool throughVeneer = target >= arityEntry && target < allocationEnd;
        jumps.append(ExceptionCheckJump { static_cast<uint32_t>(address - start), throughVeneer });
    }
    return jumps;
}
#endif

} // namespace ImageBudgetTestsInternal

using namespace ImageBudgetTestsInternal;

// A budget that refuses midway leaves the record Incomplete, and every conditional external jump the code emits is still
// linked to its target, through a veneer deferred before the refusal or natively after it.
JITCACHE_TEST(imageRefusedBudgetLinksEveryBranch, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    auto handleExceptionKey = commonThunkKey(CommonJITThunkID::HandleException);
    auto handleException = reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr());
    constexpr unsigned jumpCount = 2000;

    auto budget = ProducerBudget::create(kRecordChargeStep);
    Vector<uint32_t> sites;
    std::optional<LinkedCode> linked;
    {
        ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, budget.copyRef());
        CCallHelpers jit;
        recorder.attachTo(jit);
        Vector<MacroAssembler::Jump> jumps;
        for (unsigned i = 0; i < jumpCount; ++i) {
            moveReference(jit, ImageReference::vmAddress(vm, VMAddress::VM), GPRInfo::regT2);
            auto jump = jit.branch32(CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
            jumps.append(jump);
            linkJumpToSupport(jit, vm, jump, handleExceptionKey);
        }
        jit.ret();
        JITCACHE_CHECK(!recorder.isRecording());
        JITCACHE_CHECK(recorder.state() == RecordState::Incomplete);
        JITCACHE_CHECK(budget->hasRefused());
        recorder.emitVeneers(jit);

        LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
        if (linkBuffer.didFailToAllocate()) {
            JITCACHE_FAIL("no executable memory for the test code"_s);
            return;
        }
        for (auto& jump : jumps)
            sites.append(linkBuffer.offsetOf(jump.assemblerLabel()));
        auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
        linked.emplace(LinkedCode { linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s), bytes });
        JITCACHE_CHECK(!recorder.finishSnippet(linkBuffer));
    }
    // The recorder released everything it charged.
    JITCACHE_CHECK(!budget->chargedBytes());

    auto codeAddress = reinterpret_cast<uintptr_t>(linked->bytes.data());
    auto codeEnd = codeAddress + linked->bytes.size();
    unsigned throughVeneer = 0;
    unsigned linkedNatively = 0;
    for (uint32_t site : sites) {
#if CPU(X86_64)
        // jcc rel32, which ends at the jump's label.
        ImageFixup fixup { .site = site, .form = FixupForm::Jump, .target = handleExceptionKey };
        auto target = decodeFixup(linked->bytes, fixup);
        bool linkedToThunk = target && *target == handleException;
        linkedNatively += linkedToThunk;
#elif CPU(ARM64)
        auto target = arm64ConditionalTarget(codeAddress + site);
        bool insideCode = target && *target >= codeAddress && *target < codeEnd;
        bool linkedToThunk = target && reaches(*target, handleException);
        throughVeneer += linkedToThunk && insideCode;
        linkedNatively += linkedToThunk && !insideCode;
#endif
        if (!linkedToThunk) {
            JITCACHE_FAIL(makeString("the conditional jump at "_s, site, " is not linked to its thunk"_s));
            break;
        }
    }
    UNUSED_VARIABLE(codeEnd);
    JITCACHE_CHECK(linkedNatively);
#if CPU(ARM64)
    JITCACHE_CHECK(throughVeneer);
#else
    UNUSED_VARIABLE(throughVeneer);
#endif
}

// The part of a refused budget a script cannot see. In a Producer whose limit admits two of kRecordChargeStep's steps,
// jitcacheBig records until a later step is refused: its code links and runs, its record is Incomplete, which
// isImageCapturable refuses, the finalize capture raises the recording fault at budget.limit and commits nothing, and
// once the VM is destroyed, with its records and its production memory, the budget holds no charge. LeakSanitizer
// checks the rest when testjitcache exits; image/budget.js runs the limits as a producer would.
JITCACHE_TEST_WITH_OPTIONS(imageTinyBudgetLeavesRecordsIncompleteAndReleasesEverything, No, "--useConcurrentJIT=false")
{
    auto directory = TemporaryDirectory::create(context, "jitcache-image"_s);
    if (!directory)
        return;
    RefPtr<ProducerBudget> budget;
    {
        ExtraVM producer;
        VM& vm = producer.vm();
        JITCache::Config config;
        config.artifactPath = directory->path("artifact"_s);
        config.role = Role::Producer;
        config.producerLimitBytes = 2 * kRecordChargeStep;
        StartResult started = JITCache::start(vm, config);
        if (started.outcome != StartOutcome::Started) {
            JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
            return;
        }
        ProducerContext* production = JITCache::producerContext(vm);
        if (!production) {
            JITCACHE_FAIL("the started producer has no producer context"_s);
            return;
        }
        budget = production->budget();

        auto* globalObject = createRealm(context, vm, bigBodySource(), "the big body"_s);
        if (!globalObject)
            return;
        auto* function = globalFunctionOrNull(*globalObject, "jitcacheBig"_s);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
            JITCACHE_FAIL("jitcacheBig did not compile to baseline code"_s);
            return;
        }
        RefPtr<JSC::JITCode> jitCode = codeBlock->jitCode();
        auto& code = static_cast<BaselineJITCode&>(*jitCode);
        auto* record = code.m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Incomplete)
            JITCACHE_FAIL(makeString("jitcacheBig's record is missing or not Incomplete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ')'));
        JITCACHE_CHECK(!isImageCapturable(code));
        JITCACHE_CHECK(budget->hasRefused());

        Status reported = JITCache::status(vm);
        JITCACHE_CHECK(reported.production == ProductionState::Ended);
        JITCACHE_CHECK(reported.productionFault && reported.productionFault->stepName() == "budget.limit"_s);
        JITCACHE_CHECK(!reported.progress.capturesCommitted);
        JITCACHE_CHECK(!reported.progress.bytesCommitted);
    }
    // The VM is gone: every record and all production memory gave back what they charged.
    JITCACHE_CHECK(budget && !budget->chargedBytes());
}

// The budget sweep over the main pass and the slow paths. jitcacheSweep compiles under a twin recorder whose budget
// admits one more kRecordChargeStep each time, until its record is complete. Every compilation whose budget refused a
// charge leaves an Incomplete record, which isImageCapturable refuses, and once its code dies the budget holds nothing.
// On ARM64 the exception checks show where the refusal fell: those deferred before it reach HandleException through
// veneers and those deferred after it natively, so in code order no veneered one follows a native one, no conditional
// branch is left unlinked, and each image keeps as many as the complete compilation. The sweep must refuse inside the
// main pass at least once, between a veneered and a native jump before the last instruction, and inside the slow paths
// at least once, between a veneered and a native jump after it. x86_64 links every conditional external jump natively
// as it records it, so there only the records and the balances are checked. A refusal inside emitVeneers is
// imageRefusalInsideEmitVeneersLinksEveryVeneer's, and one inside a MathIC snippet imageRefusedRegenerationLinksItsSnippet's.
JITCACHE_TEST(imageBudgetSweepLinksEveryExceptionCheck, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = createRealm(context, vm, sweepBodySource(), "the sweep's body"_s);
    if (!globalObject)
        return;
    auto* function = globalFunctionOrNull(*globalObject, "jitcacheSweep"_s);
    CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || JITCode::isJIT(codeBlock->jitType())) {
        JITCACHE_FAIL("jitcacheSweep has no CodeBlock in the LLInt"_s);
        return;
    }

#if CPU(ARM64)
    auto handleException = reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr());
    // An image's exception checks, and the offset of its last instruction, the end of the main pass.
    struct Scan {
        Vector<ExceptionCheckJump> jumps;
        uint32_t lastInstruction { 0 };
    };
    auto scan = [&](BaselineJITCode& code) -> std::optional<Scan> {
        auto start = reinterpret_cast<uintptr_t>(code.start());
        auto arityEntry = reinterpret_cast<uintptr_t>(code.addressForCall(ArityCheckMode::MustCheckArity).untaggedPtr());
        auto jumps = exceptionCheckJumps(start, arityEntry, start + code.size(), handleException);
        if (!jumps) {
            JITCACHE_FAIL("a conditional branch of jitcacheSweep's image targets itself"_s);
            return std::nullopt;
        }
        Scan result { WTF::move(*jumps), 0 };
        code.m_jitCodeMap.forEach([&](BytecodeIndex, CodeLocationLabel<JSEntryPtrTag> location) {
            result.lastInstruction = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(location.untaggedPtr()) - start);
        });
        return result;
    };

    // The complete compilation defers every exception check while it records.
    size_t exceptionChecks = 0;
    {
        auto code = compileUnderTwinRecorder(vm, *codeBlock);
        auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("the unlimited compilation of jitcacheSweep left no complete record"_s);
            return;
        }
        auto complete = scan(*code);
        if (!complete)
            return;
        for (auto& jump : complete->jumps) {
            if (!jump.throughVeneer) {
                JITCACHE_FAIL(makeString("the complete compilation links the exception check at "_s, jump.offset, " natively"_s));
                return;
            }
        }
        exceptionChecks = complete->jumps.size();
        if (!exceptionChecks) {
            JITCACHE_FAIL("jitcacheSweep's image holds no exception check"_s);
            return;
        }
    }
    unsigned mainPassRefusals = 0;
    unsigned slowPathRefusals = 0;
#endif

    bool completed = false;
    constexpr unsigned maximumSteps = 512;
    for (unsigned steps = 1; steps <= maximumSteps && !completed; ++steps) {
        auto budget = ProducerBudget::create(steps * kRecordChargeStep);
        {
            auto code = compileUnderTwinRecorder(vm, *codeBlock, budget.copyRef());
            if (!code) {
                JITCACHE_FAIL("no executable memory for the sweep's compilation"_s);
                return;
            }
            auto* record = code->m_jitCacheImageRecord.get();
            if (!record || (record->state() != RecordState::Complete && record->state() != RecordState::Incomplete)) {
                JITCACHE_FAIL(makeString("at "_s, steps, " steps, the record is missing or neither complete nor incomplete"_s));
                return;
            }
            completed = record->state() == RecordState::Complete;
            if (!completed)
                JITCACHE_CHECK(!isImageCapturable(*code));
#if CPU(ARM64)
            auto result = scan(*code);
            if (!result)
                return;
            auto& jumps = result->jumps;
            if (jumps.size() != exceptionChecks) {
                JITCACHE_FAIL(makeString("at "_s, steps, " steps, the image holds "_s, jumps.size(), " exception checks instead of "_s, exceptionChecks));
                return;
            }
            size_t firstNative = jumps.size();
            for (size_t index = 0; index < jumps.size(); ++index) {
                if (!jumps[index].throughVeneer) {
                    firstNative = index;
                    break;
                }
            }
            for (size_t index = firstNative; index < jumps.size(); ++index) {
                if (jumps[index].throughVeneer) {
                    JITCACHE_FAIL(makeString("at "_s, steps, " steps, the exception check at "_s, jumps[index].offset, " reaches a veneer after a native one"_s));
                    return;
                }
            }
            if (completed && firstNative < jumps.size())
                JITCACHE_FAIL(makeString("the complete compilation at "_s, steps, " steps links an exception check natively"_s));
            // A refusal between two exception checks of one pass: a veneered one, then a native one.
            if (firstNative && firstNative < jumps.size()) {
                if (jumps[firstNative].offset < result->lastInstruction)
                    ++mainPassRefusals;
                else if (jumps[firstNative - 1].offset > result->lastInstruction)
                    ++slowPathRefusals;
            }
#endif
        }
        // The code is gone, and its record with it.
        JITCACHE_CHECK(!budget->chargedBytes());
    }
    JITCACHE_CHECK(completed);
#if CPU(ARM64)
    if (!mainPassRefusals || !slowPathRefusals)
        JITCACHE_FAIL(makeString("the sweep refused "_s, mainPassRefusals, " times inside the main pass and "_s, slowPathRefusals, " times inside the slow paths"_s));
#endif
}

#if CPU(ARM64)
// The refusal inside emitVeneers. emitVeneers charges only to record a veneer's fixup, and only when the fixup vector is
// full, so a real compilation refuses there only by chance. Here each try's recorder, under a budget of one
// kRecordChargeStep, defers a conditional jump to each of two thunks, records a number of Pointer fixups and emits the
// veneers. emitVeneers emits and links each veneer before recordJump charges for its fixup. So at the first count whose
// refusal falls inside emitVeneers, the first veneer's fixup takes the vector's last free entry, and the refused charge is
// the last veneer's, once both veneers exist. At the next count the vector is full as emitVeneers begins: the first
// veneer's charge is refused, the record never holds a veneer fixup, and the second veneer is emitted and linked by a
// recorder that has stopped. One count more is refused before the veneers, which shows that the vector was full there.
// At both counts inside, each jump must reach its thunk through a veneer of its own.
JITCACHE_TEST(imageRefusalInsideEmitVeneersLinksEveryVeneer, Yes)
{
    VM& vm = *context.vm();
    auto body = makeRecordingBody(context, vm);
    if (!body)
        return;
    std::array keys { commonThunkKey(CommonJITThunkID::HandleException), commonThunkKey(CommonJITThunkID::ThrowStackOverflowAtPrologue) };
    std::array thunks {
        reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::HandleException).untaggedPtr()),
        reinterpret_cast<uintptr_t>(commonThunk(vm, CommonJITThunkID::ThrowStackOverflowAtPrologue).untaggedPtr()),
    };

    enum class Refusal : uint8_t {
        None,
        BeforeVeneers,
        InsideVeneers,
    };
    struct Attempt {
        Refusal refusal { Refusal::None };
        std::array<uint32_t, 2> sites { }; // The two jumps' labels in the linked code, when refused inside the veneers.
        std::optional<LinkedCode> linked;
    };
    // One try with pointers Pointer fixups recorded between the deferrals and the veneers. Nullopt when no executable
    // memory was left for the code.
    auto attempt = [&](unsigned pointers) -> std::optional<Attempt> {
        auto budget = ProducerBudget::create(kRecordChargeStep);
        Attempt result;
        {
            ImageRecorder recorder(RecordingScope::MathICSnippet, vm, *body->unlinkedCodeBlock, budget.copyRef());
            CCallHelpers jit;
            recorder.attachTo(jit);
            std::array<MacroAssembler::Jump, 2> jumps;
            for (size_t index = 0; index < jumps.size(); ++index) {
                jumps[index] = jit.branch32(index ? CCallHelpers::NotEqual : CCallHelpers::Equal, GPRInfo::regT0, GPRInfo::regT1);
                linkJumpToSupport(jit, vm, jumps[index], keys[index]);
            }
            for (unsigned index = 0; index < pointers; ++index)
                moveReference(jit, ImageReference::vmAddress(vm, VMAddress::VM), GPRInfo::regT2);
            jit.ret();
            if (!recorder.isRecording())
                result.refusal = Refusal::BeforeVeneers;
            else {
                recorder.emitVeneers(jit);
                if (!recorder.isRecording()) {
                    result.refusal = Refusal::InsideVeneers;
                    JITCACHE_CHECK(recorder.state() == RecordState::Incomplete);
                    LinkBuffer linkBuffer(jit, nullptr, LinkBuffer::Profile::Uncategorized, JITCompilationCanFail);
                    if (linkBuffer.didFailToAllocate()) {
                        JITCACHE_FAIL("no executable memory for the test code"_s);
                        return std::nullopt;
                    }
                    for (size_t index = 0; index < jumps.size(); ++index)
                        result.sites[index] = linkBuffer.offsetOf(jumps[index].assemblerLabel());
                    auto bytes = unsafeMakeSpan(static_cast<const uint8_t*>(linkBuffer.debugAddress()), linkBuffer.size());
                    result.linked.emplace(LinkedCode { linkBuffer.finalizeCodeWithoutDisassembly<JITStubRoutinePtrTag>("JITCache image recording test"_s), bytes });
                    JITCACHE_CHECK(!recorder.finishSnippet(linkBuffer));
                }
            }
        }
        // The recorder released everything it charged.
        JITCACHE_CHECK(!budget->chargedBytes());
        return result;
    };
    // Each jump of a try refused inside the veneers reaches its thunk through a veneer of its own.
    auto reachesThroughOwnVeneers = [&](const Attempt& refused, unsigned pointers) {
        auto codeAddress = reinterpret_cast<uintptr_t>(refused.linked->bytes.data());
        auto codeEnd = codeAddress + refused.linked->bytes.size();
        std::array<uintptr_t, 2> veneers { };
        for (size_t index = 0; index < refused.sites.size(); ++index) {
            auto veneer = arm64ConditionalTarget(codeAddress + refused.sites[index]);
            bool throughVeneer = veneer && *veneer >= codeAddress && *veneer < codeEnd && (readWord(*veneer) & 0xfc000000) == 0x14000000;
            if (!throughVeneer || !reaches(*veneer, thunks[index])) {
                JITCACHE_FAIL(makeString("with "_s, pointers, " pointers the conditional jump at "_s, refused.sites[index], " does not reach its thunk through a veneer"_s));
                return false;
            }
            veneers[index] = *veneer;
        }
        if (veneers[0] == veneers[1]) {
            JITCACHE_FAIL(makeString("with "_s, pointers, " pointers both jumps reach their thunks through one veneer"_s));
            return false;
        }
        return true;
    };

    for (unsigned pointers = 0; pointers < 1024; ++pointers) {
        // The first count refused at all.
        auto lastVeneerRefused = attempt(pointers);
        if (!lastVeneerRefused)
            return;
        if (lastVeneerRefused->refusal == Refusal::None)
            continue;
        if (lastVeneerRefused->refusal == Refusal::BeforeVeneers) {
            JITCACHE_FAIL(makeString("with "_s, pointers, " pointers the budget refused before the veneers, and no smaller count refused inside them"_s));
            return;
        }
        if (!reachesThroughOwnVeneers(*lastVeneerRefused, pointers))
            return;

        auto firstVeneerRefused = attempt(pointers + 1);
        if (!firstVeneerRefused)
            return;
        if (firstVeneerRefused->refusal != Refusal::InsideVeneers) {
            JITCACHE_FAIL(makeString("with "_s, pointers + 1, " pointers the budget did not refuse inside the veneers"_s));
            return;
        }
        if (!reachesThroughOwnVeneers(*firstVeneerRefused, pointers + 1))
            return;

        // One more Pointer fixup is refused, so the vector was full as emitVeneers began the try before, and the first
        // veneer's fixup was the charge it refused.
        auto pastFull = attempt(pointers + 2);
        if (!pastFull)
            return;
        if (pastFull->refusal != Refusal::BeforeVeneers)
            JITCACHE_FAIL(makeString("with "_s, pointers + 2, " pointers the budget did not refuse before the veneers, so with "_s, pointers + 1, " the first veneer's charge was not the refused one"_s));
        return;
    }
    JITCACHE_FAIL("no count of recorded pointers made the budget refuse inside emitVeneers"_s);
}
#endif

// The refusal inside a MathIC snippet. In a Producer, jitcacheRegenerated compiles from int32 profiles with a Complete
// record, which its finalize capture commits. The test then charges the budget until less than one kRecordChargeStep is
// left: the next regeneration's log entry still fits, its snippet recorder's first step does not. The first double
// regenerates the add's full snippet under a recorder that stops before it records anything, so the snippet links every
// jump back into the image natively, and the record becomes Incomplete. Each later call takes a path of that snippet,
// back to the image's done location or, for a string and for an operand whose valueOf throws, through its conditional
// jumps to the image's slow path, and must return or throw what the engine computes. delta then raises the refusal at
// budget.limit, and once the VM is destroyed the budget holds nothing.
JITCACHE_TEST_WITH_OPTIONS(imageRefusedRegenerationLinksItsSnippet, No, "--useConcurrentJIT=false")
{
    auto directory = TemporaryDirectory::create(context, "jitcache-image"_s);
    if (!directory)
        return;
    RefPtr<ProducerBudget> budget;
    {
        ExtraVM producer;
        VM& vm = producer.vm();
        JITCache::Config config;
        config.artifactPath = directory->path("artifact"_s);
        config.role = Role::Producer;
        config.producerLimitBytes = 64 * MB;
        StartResult started = JITCache::start(vm, config);
        if (started.outcome != StartOutcome::Started) {
            JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
            return;
        }
        ProducerContext* production = JITCache::producerContext(vm);
        if (!production) {
            JITCACHE_FAIL("the started producer has no producer context"_s);
            return;
        }
        budget = production->budget();

        auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
        auto run = [&](ASCIILiteral source) {
            NakedPtr<Exception> exception;
            evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
            if (exception)
                JITCACHE_FAIL(makeString("evaluating \""_s, source, "\" threw"_s));
            return !exception;
        };
        if (!run("function jitcacheRegenerated(a, b) { return a + b; }\nfor (var i = 0; i < 40; ++i)\n    jitcacheRegenerated(i, 1);\n"_s))
            return;
        auto* function = globalFunctionOrNull(*globalObject, "jitcacheRegenerated"_s);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
            JITCACHE_FAIL("jitcacheRegenerated did not compile to baseline code"_s);
            return;
        }
        RefPtr<JSC::JITCode> jitCode = codeBlock->jitCode();
        auto& code = static_cast<BaselineJITCode&>(*jitCode);
        auto* record = code.m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("jitcacheRegenerated's record is missing or not complete"_s);
            return;
        }
        JITCACHE_CHECK(JITCache::status(vm).progress.capturesCommitted);

        size_t exhausting = budget->limitBytes() - budget->chargedBytes() - (kRecordChargeStep - 1);
        if (!budget->tryCharge(exhausting)) {
            JITCACHE_FAIL("the test could not charge the budget"_s);
            return;
        }
        bool regenerated = run("if (jitcacheRegenerated(1.5, 2) !== 3.5) throw new Error('1.5 + 2');"_s);
        JITCACHE_CHECK(regenerated);
        JITCACHE_CHECK(budget->hasRefused());
        JITCACHE_CHECK(record->state() == RecordState::Incomplete);
        bool hasSnippet = false;
        code.forEachMathIC([&](auto& mathIC) {
            hasSnippet |= !!mathIC.m_code;
        });
        JITCACHE_CHECK(hasSnippet);

        bool tookEveryPath = run(
            "if (jitcacheRegenerated(2.5, 0.25) !== 2.75) throw new Error('2.5 + 0.25');\n"
            "if (jitcacheRegenerated(1, 2.5) !== 3.5) throw new Error('1 + 2.5');\n"
            "if (jitcacheRegenerated(2147483647, 1) !== 2147483648) throw new Error('2147483647 + 1');\n"
            "if (jitcacheRegenerated('a', 1) !== 'a1') throw new Error('a + 1');\n"
            "var jitcacheThrew = false;\n"
            "try { jitcacheRegenerated({ valueOf() { throw new Error('valueOf throws'); } }, 1); } catch (error) { jitcacheThrew = error.message === 'valueOf throws'; }\n"
            "if (!jitcacheThrew) throw new Error('the exception valueOf threw did not reach the caller');\n"_s);
        JITCACHE_CHECK(tookEveryPath);
        JITCACHE_CHECK(function->jsExecutable()->codeBlockForCall() == codeBlock);
        JITCACHE_CHECK(codeBlock->jitType() == JITType::BaselineJIT);

        DeltaResult result = JITCache::delta(vm);
        JITCACHE_CHECK(result.outcome == DeltaOutcome::Faulted);
        JITCACHE_CHECK(result.fault && result.fault->stepName() == "budget.limit"_s);
        budget->release(exhausting);
    }
    // The VM is gone, with every record and all production memory.
    JITCACHE_CHECK(budget && !budget->chargedBytes());
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
