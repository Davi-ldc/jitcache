#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "BakedFacts.h"
#include "BaselineJITCode.h"
#include "BaselineJITPlan.h"
#include "BuiltinExecutableCreator.h"
#include "BytecodeStructs.h"
#include "CodeBlock.h"
#include "Completion.h"
#include "DeferGC.h"
#include "ExecutableAllocator.h"
#include "FunctionCodeBlock.h"
#include "FunctionExecutable.h"
#include "ImageCapture.h"
#include "ImagePrepare.h"
#include "ImageRecord.h"
#include "ImageSection.h"
#include "ImageSupport.h"
#include "ImageTestSupport.h"
#include "ImageTwins.h"
#include "ImageTwinsSection.h"
#include "ImageTypes.h"
#include "JIT.h"
#include "JITCacheAPI.h"
#include "JITCacheTest.h"
#include "JSCInlines.h"
#include "JSFunction.h"
#include "JSGlobalObject.h"
#include "ProducerBudget.h"
#include "SourceCode.h"
#include "SourceProvider.h"
#include "TestFiles.h"
#include "TestVM.h"
#include "TopExceptionScope.h"
#include "TwinReport.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionExecutable.h"
#include <algorithm>
#include <array>
#include <optional>
#include <stdarg.h>
#include <wtf/DataLog.h>
#include <wtf/Lock.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/StringView.h>

// Tests of capturing a baseline image and installing it in a newborn CodeBlock, with the twin check that follows
// (image/ImageCapture, ImagePrepare, BakedFacts, ImageSection, ImageSupport, ImageTwins and ImageTwinsSection, .h and
// .cpp): the baked facts compared with newborn CodeBlocks, the twin's life and death, bodies that are never captured,
// patching through jump islands, and the slack past an allocation's linked size, which neither the capture nor the
// twin check reads.

namespace JSC::JITCache::Tests {

namespace ImageInstallTestsInternal {

// One section of a capture, in a buffer of its own, which starts 8-byte aligned as the container hands sections over.
template<typename Write>
static bool collectSection(Vector<uint8_t>& bytes, const Write& write)
{
    auto append = [&](std::span<const uint8_t> data) {
        bytes.append(data);
        return true;
    };
    ImageSectionSink sink = append;
    return write(sink);
}

// A capture's three sections, each in a buffer of its own, beside the code it captured.
struct CapturedImage {
    RefPtr<BaselineJITCode> code;
    Vector<uint8_t> image;
    Vector<uint8_t> bakedFacts;
    Vector<uint8_t> twins;
};

// Captures captured.code, the baseline code codeBlock runs, with strict on, so every check of the capture runs, into
// captured's three sections.
static bool imageCaptureInto(TestContext& context, VM& vm, CodeBlock& codeBlock, CapturedImage& captured)
{
    Ref<ProducerBudget> budget = ProducerBudget::createUnlimited();
    auto capture = captureImage(vm, codeBlock, *captured.code, budget.get(), true);
    if (!capture) {
        JITCACHE_FAIL(makeString("the capture failed with outcome "_s, static_cast<unsigned>(capture.error().outcome), " at "_s, description(capture.error().check)));
        return false;
    }
    bool written = collectSection(captured.image, [&](const ImageSectionSink& sink) { return capture->writeImageSection(sink); })
        && collectSection(captured.bakedFacts, [&](const ImageSectionSink& sink) { return capture->writeBakedFactsSection(sink); })
        && collectSection(captured.twins, [&](const ImageSectionSink& sink) { return capture->writeTwinsSection(sink); });
    if (!written) {
        JITCACHE_FAIL("writing the captured sections failed"_s);
        return false;
    }
    JITCACHE_CHECK(captured.image.size() == capture->imageSectionSize());
    JITCACHE_CHECK(captured.bakedFacts.size() == capture->bakedFactsSectionSize());
    JITCACHE_CHECK(captured.twins.size() == capture->twinsSectionSize());
    return true;
}

// producerFunction's CodeBlock, still in the LLInt, compiles under a twin recorder, the one recorder a compilation outside
// production can have, is installed and is captured with strict on.
static NEVER_INLINE std::optional<CapturedImage> imageCompileAndCapture(TestContext& context, VM& vm, JSFunction& producerFunction)
{
    CodeBlock* producer = producerFunction.jsExecutable()->codeBlockForCall();
    if (!producer || JITCode::isJIT(producer->jitType())) {
        JITCACHE_FAIL("the producing function has no CodeBlock in the LLInt"_s);
        return std::nullopt;
    }

    TwinSeeds seeds { };
    seeds.assembler = assemblerSeed;
    TwinCompileInputs noInputs { };
    CapturedImage captured;
    {
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(producer));
        JIT jit(vm, plan.get(), producer);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, noInputs);
        captured.code = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
        if (!captured.code || JIT::finalizeOnMainThread(producer, plan.get(), captured.code) != CompilationResult::CompilationSuccessful) {
            JITCACHE_FAIL("the producer's compilation failed"_s);
            return std::nullopt;
        }
    }
    producer->ownerExecutable()->installCode(producer);

    if (!imageCaptureInto(context, vm, *producer, captured))
        return std::nullopt;
    return captured;
}

// importFunction, the producer's text in the producer's scope, so that its UCB has the producer's index spaces and its
// scope accesses resolve alike, gets a newborn CodeBlock. The newborn takes the image prepareImage restores from the
// captured sections with strict on, as the install glue gives it, through native setup and installCode, and
// Twins::checkImage then checks it. inspect sees the restored code and the parsed sections before setup, and aroundCheck
// receives a function that runs the check, which it calls once. Returns whether the import installed.
template<typename Inspect, typename AroundCheck>
static NEVER_INLINE bool imageImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& importFunction, const CapturedImage& captured, const Inspect& inspect, const AroundCheck& aroundCheck)
{
    // The install function's deferral keeps the newborn alive until installCode publishes it.
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionExecutable* importExecutable = importFunction.jsExecutable();
    CodeBlock* newborn = importExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, &importFunction, importFunction.scope());
    if (scope.exception() || !newborn) {
        scope.clearException();
        JITCACHE_FAIL("newCodeBlockFor gave the importing function no newborn CodeBlock"_s);
        return false;
    }
    UnlinkedCodeBlock& unlinkedCodeBlock = *newborn->unlinkedCodeBlock();

    auto view = parseImageSections(ImageSectionSpans { .image = captured.image.span(), .bakedFacts = captured.bakedFacts.span(), .twins = captured.twins.span() }, true);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(view.error())));
        return false;
    }
    if (auto valid = validateImageSectionsAgainst(*view, unlinkedCodeBlock); !valid) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(valid.error()), " against the importing function's UCB"_s));
        return false;
    }
    if (compareBakedFacts(*view, *newborn) != BakedFactsResult::Match) {
        JITCACHE_FAIL("the newborn's baked facts differ from the capture's"_s);
        return false;
    }
    auto prepared = prepareImage(vm, unlinkedCodeBlock, *view, nullptr, true);
    if (!prepared) {
        JITCACHE_FAIL(makeString("preparing the image failed at "_s, description(prepared.error().check)));
        return false;
    }
    Ref<BaselineJITCode> restored = WTF::move(*prepared).commit(vm, *newborn);
    inspect(restored.get(), *view);
    newborn->setupWithUnlinkedBaselineCode(restored.copyRef());
    importExecutable->installCode(newborn);

    auto report = TwinReport::open("/dev/null"_s);
    if (!report) {
        JITCACHE_FAIL("the twin report cannot be opened"_s);
        return false;
    }
    size_t twinsBefore = imageTwinCountForTesting();
    Twins twins;
    aroundCheck([&] {
        twins.checkImage(vm, *newborn, importFunction.scope(), restored.get(), *view, *report);
    });
    // The image is its own twin's: both preconditions hold, so nothing is skipped, and nothing differs.
    if (report->differences() || report->skips())
        JITCACHE_FAIL(makeString("the twin check reported "_s, report->differences(), " differences and "_s, report->skips(), " skips"_s));
    // The check returned, dropping its hold, so nothing reaches the twin, but nothing has swept it yet.
    JITCACHE_CHECK(imageTwinCountForTesting() == twinsBefore + 1);
    return true;
}

template<typename Inspect>
static bool imageImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& importFunction, const CapturedImage& captured, const Inspect& inspect)
{
    return imageImportAndCheckTwin(context, vm, importFunction, captured, inspect, [](const auto& check) {
        check();
    });
}

// The twin test's capture, import and check, in a frame of their own so that the test's frame never holds the twin.
static NEVER_INLINE bool imageCaptureImportAndCheckTwin(TestContext& context, VM& vm, JSFunction& producerFunction, JSFunction& importFunction)
{
    auto captured = imageCompileAndCapture(context, vm, producerFunction);
    return captured && imageImportAndCheckTwin(context, vm, importFunction, *captured, [](BaselineJITCode&, const ImageSectionsView&) { });
}

// A CodeBlock of importBody's UCB linked natively, never installed and left unreachable as the twin is.
static NEVER_INLINE bool imageLinkUnreachableCodeBlock(TestContext& context, VM& vm, JSFunction& importFunction)
{
    DeferGCForAWhile deferGC(vm);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    FunctionExecutable* executable = importFunction.jsExecutable();
    auto* unlinkedCodeBlock = uncheckedDowncast<UnlinkedFunctionCodeBlock>(executable->codeBlockForCall()->unlinkedCodeBlock());
    auto* codeBlock = FunctionCodeBlock::create(vm, executable, unlinkedCodeBlock, importFunction.scope());
    if (scope.exception() || !codeBlock) {
        scope.clearException();
        JITCACHE_FAIL("linking a native CodeBlock of importBody failed"_s);
        return false;
    }
    return true;
}

// Conservative scanning reads the frames a collection runs in, which reuse the stack the frames above left behind;
// zeroing it first keeps a stale pointer there from holding a CodeBlock the test expects to die.
static NEVER_INLINE void imageClearDeadStack()
{
    std::array<volatile uint8_t, 128 * KB> bytes;
    for (auto& byte : bytes)
        byte = 0;
}

// The distance the island test puts between an image and its support code: the reach of an ARM64 bl or b, beyond which
// patching places jump islands. x86_64's rel32 reaches the whole pool, so the same distance needs none there.
static constexpr size_t islandDistance = 128 * MB;
#if CPU(ARM64)
static_assert(islandDistance == MacroAssembler::nearJumpRange);
#endif

// The thunk a Call or Jump fixup branches to, through the native lookup its support key names; nullopt for a Pointer and
// for a branch whose target lies in the image or its snippets.
static std::optional<uintptr_t> supportBranchTarget(VM& vm, const ImageFixup& fixup)
{
    if (fixup.form == FixupForm::Pointer)
        return std::nullopt;
    switch (fixup.target.kind) {
    case TargetKind::CommonThunk:
    case TargetKind::BaselineThunk:
    case TargetKind::SlowPathThunk:
    case TargetKind::InlineCacheSlowPathThunk:
    case TargetKind::VirtualCallThunk:
    case TargetKind::ProcessThunk:
        return reinterpret_cast<uintptr_t>(resolveSupport(vm, fixup.target).untaggedPtr());
    default:
        return std::nullopt;
    }
}

// Every support branch of an image reaches its thunk the way an image that far from its thunks must: through a jump
// island on ARM64 and directly on x86_64. Returns how many it checked.
template<typename Fixups>
static unsigned checkFarSupportBranches(TestContext& context, VM& vm, ASCIILiteral image, std::span<const uint8_t> code, const Fixups& fixups)
{
    unsigned checked = 0;
    for (size_t index = 0; index < fixups.size(); ++index) {
        ImageFixup fixup = fixups[index];
        auto support = supportBranchTarget(vm, fixup);
        if (!support)
            continue;
        ++checked;
        auto direct = decodeFixup(code, fixup);
#if CPU(ARM64)
        bool reachesFarThunk = direct && *direct != *support && reaches(*direct, *support);
#else
        bool reachesFarThunk = direct && *direct == *support;
#endif
        if (!reachesFarThunk) {
            JITCACHE_FAIL(makeString(image, ": the support branch at "_s, fixup.site, " does not reach its thunk the way a far image must"_s));
            break;
        }
    }
    return checked;
}

// Two functions with one text in one scope, as the twin test's bodies, each 1000 statements of property loads and
// arithmetic with their slow calls and exception checks. Their image is larger than the pages the JIT heap shares among
// its small and medium allocations, so every allocation of its size comes from the heap's large-object free lists, which
// give each the lowest free range that holds it. jitcacheFarProducer runs once, in the LLInt.
static String farBodiesSource()
{
    constexpr unsigned statements = 1000;
    StringBuilder text;
    text.append("function (o) {\n    var sum = 0;\n"_s);
    for (unsigned index = 0; index < statements; ++index)
        text.append("    sum = sum + o.p"_s, index % 64, " * "_s, index + 1, ";\n"_s);
    text.append("    return sum;\n}"_s);
    String body = text.toString();
    return makeString("var jitcacheFarProducer, jitcacheFarImport;\n(function makeFarBodies() {\n    jitcacheFarProducer = "_s, body,
        ";\n    jitcacheFarImport = "_s, body, ";\n})();\nvar jitcacheFarObject = {};\nfor (var k = 0; k < 64; ++k)\n    jitcacheFarObject['p' + k] = k;\njitcacheFarProducer(jitcacheFarObject);\n"_s);
}

// The slack test's fills, one per side, for the bytes each allocation holds past its linked size. None is zero, so the
// check of performJITMemcpy for runs of zeros never objects.
static constexpr uint8_t producerSlackFill = 0xa5;
static constexpr uint8_t restoredSlackFill = 0x5a;
static constexpr uint8_t twinSlackFill = 0xc3;

// The bytes an allocation of handleSize bytes at start holds past its linkedSize, after writing fill over them when one is
// given. Empty when the handle ends at the linked size.
static Vector<uint8_t> imageSlackBytes(const void* start, size_t linkedSize, size_t handleSize, std::optional<uint8_t> fill)
{
    if (!start || handleSize <= linkedSize)
        return { };
    auto slack = unsafeMakeSpan(static_cast<const uint8_t*>(start), handleSize).subspan(linkedSize);
    if (fill) {
        Vector<uint8_t> filled(FillWith { }, slack.size(), *fill);
        performJITMemcpy<jitMemcpyRepatch>(const_cast<uint8_t*>(slack.data()), filled.span().data(), filled.size());
    }
    Vector<uint8_t> bytes;
    bytes.append(slack);
    return bytes;
}

// Whether section is the canonical form of exactly linkedCode under its fixups.
static bool imageSectionHoldsLinkedBytes(std::span<const uint8_t> section, std::span<const uint8_t> linkedCode, std::span<const ImageFixup> fixups)
{
    Vector<uint8_t> canonical;
    auto append = [&](std::span<const uint8_t> bytes) {
        canonical.append(bytes);
        return true;
    };
    ImageSectionSink sink = append;
    ImageSectionWriter writer(sink);
    return writer.writeCanonicalCode(linkedCode, fixups) && equalSpans(canonical.span(), section);
}

// An allocation a LinkBuffer announced: its name and its handle, [start, start + handleSize).
struct ImageAnnouncedAllocation {
    String name;
    uintptr_t start { 0 };
    size_t handleSize { 0 };
};

// The allocation in one line LinkBuffer::finalizeCodeWithDisassemblyImpl prints while logJIT is on, "Generated JIT code
// for <name>: [<start>, <end>) <size> bytes", where [start, end) is the allocation's whole handle; nullopt for any other
// line.
static std::optional<ImageAnnouncedAllocation> imageParseAnnouncedAllocation(StringView line)
{
    constexpr auto prefix = "Generated JIT code for "_s;
    if (!line.startsWith(prefix))
        return std::nullopt;
    size_t bracket = line.reverseFind(": ["_s);
    if (bracket == notFound || bracket < prefix.length())
        return std::nullopt;
    StringView range = line.substring(static_cast<unsigned>(bracket + 3));
    size_t comma = range.find(", "_s);
    size_t close = range.find(')');
    if (comma == notFound || close == notFound || close < comma + 2)
        return std::nullopt;
    auto address = [](StringView text) -> std::optional<uint64_t> {
        if (!text.startsWith("0x"_s))
            return std::nullopt;
        return parseInteger<uint64_t>(text.substring(2), 16);
    };
    auto start = address(range.left(static_cast<unsigned>(comma)));
    auto end = address(range.substring(static_cast<unsigned>(comma + 2), static_cast<unsigned>(close - comma - 2)));
    if (!start || !end || *end < *start)
        return std::nullopt;
    StringView name = line.substring(static_cast<unsigned>(prefix.length()), static_cast<unsigned>(bracket - prefix.length()));
    return ImageAnnouncedAllocation { name.toString(), static_cast<uintptr_t>(*start), static_cast<size_t>(*end - *start) };
}

// The allocations LinkBuffer announces through dataLog while logJIT is on (imageParseAnnouncedAllocation), each on the
// thread that linked it, right after linking and before anything else reads the code. Installed as the process's data
// file, it lets the slack test reach the twin image and twin snippets Twins::checkImage links and frees before it
// returns. While armed, it fills each one's bytes past the linked size it was told with twinSlackFill and keeps what they
// then hold: the first baseline image is the twin's, and each snippet after it the replay of the next regeneration in log
// order.
class ImageTwinSlackLog final : public PrintStream {
    WTF_MAKE_NONCOPYABLE(ImageTwinSlackLog);
public:
    ImageTwinSlackLog() = default;

    void arm(uint32_t imageLinkedSize, Vector<uint32_t>&& snippetLinkedSizes)
    {
        m_armed = true;
        m_imageLinkedSize = imageLinkedSize;
        m_snippetLinkedSizes = WTF::move(snippetLinkedSizes);
        m_twinImageSlack = std::nullopt;
        m_twinSnippetSlacks.clear();
    }

    void disarm() { m_armed = false; }

    // Nullopt when the check announced no baseline image.
    const std::optional<Vector<uint8_t>>& twinImageSlack() const { return m_twinImageSlack; }
    const Vector<Vector<uint8_t>>& twinSnippetSlacks() const { return m_twinSnippetSlacks; }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    void vprintf(const char* format, va_list argumentList) final WTF_ATTRIBUTE_PRINTF(2, 0)
    {
        // Complete lines are acted on once the lock is released, since filling calls into the engine.
        Vector<String> lines;
        {
            Locker locker { m_lock };
            StringPrintStream formatted;
            formatted.vprintf(format, argumentList);
            m_partialLine.append(formatted.toString());
            String text = m_partialLine.toString();
            m_partialLine.clear();
            unsigned lineStart = 0;
            for (size_t newline = text.find('\n'); newline != notFound; newline = text.find('\n', lineStart)) {
                lines.append(text.substring(lineStart, static_cast<unsigned>(newline) - lineStart));
                lineStart = static_cast<unsigned>(newline) + 1;
            }
            m_partialLine.append(StringView(text).substring(lineStart));
        }
        for (auto& line : lines)
            announce(line);
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

private:
    void announce(StringView line)
    {
        if (!m_armed)
            return;
        auto allocation = imageParseAnnouncedAllocation(line);
        if (!allocation)
            return;
        auto* start = reinterpret_cast<const void*>(allocation->start);
        if (allocation->name.startsWith("Baseline JIT code for "_s)) {
            if (!m_twinImageSlack)
                m_twinImageSlack = imageSlackBytes(start, m_imageLinkedSize, allocation->handleSize, twinSlackFill);
            return;
        }
        if (allocation->name.startsWith("JITMathIC: generating out of line"_s) && allocation->handleSize && m_twinSnippetSlacks.size() < m_snippetLinkedSizes.size())
            m_twinSnippetSlacks.append(imageSlackBytes(start, m_snippetLinkedSizes[m_twinSnippetSlacks.size()], allocation->handleSize, twinSlackFill));
    }

    Lock m_lock;
    StringBuilder m_partialLine;
    bool m_armed { false };
    uint32_t m_imageLinkedSize { 0 };
    Vector<uint32_t> m_snippetLinkedSizes;
    std::optional<Vector<uint8_t>> m_twinImageSlack;
    Vector<Vector<uint8_t>> m_twinSnippetSlacks;
};

// Makes stream the process's data file while the scope lives, then gives standard error back. WTF::setDataFile keeps the
// stream it is handed without ever deleting it, so its owner keeps owning it.
class ImageDataFileScope {
    WTF_MAKE_NONCOPYABLE(ImageDataFileScope);
public:
    explicit ImageDataFileScope(PrintStream& stream)
    {
        WTF::setDataFile(std::unique_ptr<PrintStream>(&stream));
    }

    ~ImageDataFileScope()
    {
        WTF::setDataFile(static_cast<const char*>(nullptr));
    }
};

static constexpr unsigned slackBodyCount = 24;

// The slack test's bodies: pairs of one text in one scope, as the twin test's, the first of each pair the producer's and
// the second never called, so its first CodeBlock is the import's newborn. Body k stores 2k + 1 properties, so the
// images' linked sizes spread over many offsets from the allocator's size classes, and returns one arithmetic operation,
// whose MathIC is the body's only one and whose kind and operands cycle with k, so the snippets' linked sizes vary too.
// The program calls each producer 40 times with int32s, which compiles it from int32 profiles, and once more with a
// double, which regenerates its MathIC's full snippet.
static String slackBodiesSource()
{
    static constexpr std::array operations { "x + o.p0"_s, "o.p0 - x"_s, "x * o.p0"_s, "-x"_s, "x + 7"_s, "9 - x"_s, "x * 3"_s, "o.p1 + x"_s };
    StringBuilder makers;
    for (unsigned index = 0; index < slackBodyCount; ++index) {
        StringBuilder body;
        body.append("function (o, x) {\n"_s);
        for (unsigned statement = 0; statement <= 2 * index; ++statement)
            body.append("        o.s"_s, statement, " = o.p"_s, statement % 8, ";\n"_s);
        body.append("        return "_s, operations[index % operations.size()], ";\n    }"_s);
        String text = body.toString();
        makers.append("    jitcacheSlackProducers.push("_s, text, ");\n    jitcacheSlackImports.push("_s, text, ");\n"_s);
    }
    return makeString("var jitcacheSlackProducers = [], jitcacheSlackImports = [];\n(function makeSlackBodies() {\n"_s, makers.toString(), "})();\n"
        "var jitcacheSlackObject = { p0: 1, p1: 2, p2: 3, p3: 4, p4: 5, p5: 6, p6: 7, p7: 8 };\n"
        "for (var i = 0; i < 40; ++i) {\n    for (var k = 0; k < jitcacheSlackProducers.length; ++k)\n        jitcacheSlackProducers[k](jitcacheSlackObject, i);\n}\n"
        "for (var k = 0; k < jitcacheSlackProducers.length; ++k)\n    jitcacheSlackProducers[k](jitcacheSlackObject, 0.5);\n"_s);
}

// What each side holds past the linked size of one body's image and of its snippet: the producer's, the restored image's
// and the twin's.
struct ImageSlackOfBody {
    Vector<uint8_t> producerImage;
    Vector<uint8_t> restoredImage;
    Vector<uint8_t> twinImage;
    Vector<uint8_t> producerSnippet;
    Vector<uint8_t> restoredSnippet;
    Vector<uint8_t> twinSnippet;
};

// The slack test for one body. producerFunction's code, whose record must be complete with its one MathIC regenerated,
// gets the producer's fill past its linked sizes and is captured with strict on, and the sections must hold exactly the
// linked bytes. importFunction's newborn CodeBlock then imports them: the restored image and snippet get the restored fill
// before setup, and the twin's get the twin fill as the check links them. Returns what each side holds past its linked
// sizes once the check reported nothing.
static NEVER_INLINE std::optional<ImageSlackOfBody> imageCheckSlackOfBody(TestContext& context, VM& vm, JSFunction& producerFunction, JSFunction& importFunction, ImageTwinSlackLog& log)
{
    CodeBlock* codeBlock = producerFunction.jsExecutable()->codeBlockForCall();
    if (!codeBlock || codeBlock->jitType() != JITType::BaselineJIT) {
        JITCACHE_FAIL("a slack body did not compile to baseline code"_s);
        return std::nullopt;
    }
    CapturedImage captured;
    captured.code = static_cast<BaselineJITCode*>(codeBlock->jitCode().get());
    auto* record = captured.code->m_jitCacheImageRecord.get();
    if (!record || record->state() != RecordState::Complete || record->mathICs().size() != 1 || !record->mathICs()[0].snippet) {
        JITCACHE_FAIL("a slack body's record is missing, not complete, or not one regenerated MathIC"_s);
        return std::nullopt;
    }
    const MathICRecord& mathIC = record->mathICs()[0];
    uint32_t codeSize = record->codeSize();
    uint32_t snippetSize = mathIC.snippet->size;
    MathICCodeState producerSnippet = mathICCodeState(mathIC.kind, mathIC.mathIC);
    if (producerSnippet.snippetStart != mathIC.snippet->start) {
        JITCACHE_FAIL("a slack body's MathIC holds another snippet than its record describes"_s);
        return std::nullopt;
    }

    // A capture that read past a linked size would carry the producer's fill.
    imageSlackBytes(captured.code->start(), codeSize, captured.code->size(), producerSlackFill);
    imageSlackBytes(producerSnippet.snippetStart, snippetSize, producerSnippet.snippetHandleSize, producerSlackFill);
    if (!imageCaptureInto(context, vm, *codeBlock, captured))
        return std::nullopt;

    auto view = parseImageSections(ImageSectionSpans { .image = captured.image.span(), .bakedFacts = captured.bakedFacts.span(), .twins = captured.twins.span() }, true);
    if (!view) {
        JITCACHE_FAIL(makeString("the captured sections fail "_s, description(view.error())));
        return std::nullopt;
    }
    auto linkedImage = unsafeMakeSpan(static_cast<const uint8_t*>(captured.code->start()), codeSize);
    if (view->header().codeSize != codeSize || !imageSectionHoldsLinkedBytes(view->code(), linkedImage, record->fixups().span()))
        JITCACHE_FAIL("the image section's code is not exactly the image's linked bytes"_s);
    unsigned snippets = 0;
    view->forEachSnippet([&](unsigned, const ImageSnippet& snippet) {
        ++snippets;
        auto linkedSnippet = unsafeMakeSpan(static_cast<const uint8_t*>(mathIC.snippet->start), snippetSize);
        if (snippet.code.size() != snippetSize || !imageSectionHoldsLinkedBytes(snippet.code, linkedSnippet, mathIC.snippet->fixups.span()))
            JITCACHE_FAIL("the image section's snippet is not exactly the snippet's linked bytes"_s);
    });
    JITCACHE_CHECK(snippets == 1);

    RefPtr<BaselineJITCode> restored;
    MathICCodeState restoredSnippet;
    bool imported = imageImportAndCheckTwin(context, vm, importFunction, captured, [&](BaselineJITCode& code, const ImageSectionsView&) {
        restored = &code;
        code.forEachMathIC([&](auto& restoredMathIC) {
            if (restoredMathIC.m_code) {
                restoredSnippet.snippetStart = restoredMathIC.m_code.code().untaggedPtr();
                restoredSnippet.snippetHandleSize = restoredMathIC.m_code.size();
            }
        });
        imageSlackBytes(code.start(), codeSize, code.size(), restoredSlackFill);
        imageSlackBytes(restoredSnippet.snippetStart, snippetSize, restoredSnippet.snippetHandleSize, restoredSlackFill);
    }, [&](const auto& check) {
        log.arm(codeSize, Vector<uint32_t> { snippetSize });
        check();
        log.disarm();
    });
    if (!imported || !restored || !restoredSnippet.snippetStart) {
        if (imported)
            JITCACHE_FAIL("the restored image has no snippet"_s);
        return std::nullopt;
    }
    if (!log.twinImageSlack() || log.twinSnippetSlacks().size() != 1) {
        JITCACHE_FAIL(makeString("the twin check announced "_s, log.twinImageSlack() ? 1u : 0u, " images and "_s, log.twinSnippetSlacks().size(), " snippets, not one of each"_s));
        return std::nullopt;
    }

    ImageSlackOfBody slack;
    slack.producerImage = imageSlackBytes(captured.code->start(), codeSize, captured.code->size(), std::nullopt);
    slack.restoredImage = imageSlackBytes(restored->start(), codeSize, restored->size(), std::nullopt);
    slack.twinImage = *log.twinImageSlack();
    slack.producerSnippet = imageSlackBytes(producerSnippet.snippetStart, snippetSize, producerSnippet.snippetHandleSize, std::nullopt);
    slack.restoredSnippet = imageSlackBytes(restoredSnippet.snippetStart, snippetSize, restoredSnippet.snippetHandleSize, std::nullopt);
    slack.twinSnippet = log.twinSnippetSlacks()[0];
    return slack;
}

} // namespace ImageInstallTestsInternal

using namespace ImageInstallTestsInternal;

// counter and freshCounter have the same body text in one scope, so their UCBs share bytecode offsets and resolve every
// scope access alike, as an import's UCB has the producer's index spaces. A twin recorder compiles counter's CodeBlock,
// and the baked facts its record holds become the section. Each newborn CodeBlock of freshCounter, linked by
// newCodeBlockFor and never installed or run, must match that section unchanged and mismatch it once the test changes one
// fact: the executable's never-optimize flag set before linking, its source provider's taint set before linking, or one
// scope fact's metadata written after linking. No comparison may memoize the newborn's capability level.
JITCACHE_TEST(imageBakedFactsCompareWithNewbornCodeBlocks, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = createRealm(context, vm, "var counter, freshCounter; (function makeCounters() { var count = 0; counter = function () { count = count + 1; return count; }; freshCounter = function () { count = count + 1; return count; }; })(); counter();"_s, "the counters"_s);
    if (!globalObject)
        return;
    auto* counter = dynamicDowncast<JSFunction>(globalValue(globalObject, "counter"_s));
    auto* freshCounter = dynamicDowncast<JSFunction>(globalValue(globalObject, "freshCounter"_s));
    CodeBlock* codeBlock = counter ? counter->jsExecutable()->codeBlockForCall() : nullptr;
    if (!codeBlock || !freshCounter || freshCounter->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("counter has no CodeBlock, or freshCounter is missing or was called"_s);
        return;
    }

    // The twin recorder is the one recorder a compilation outside production can have. The counters have no switch and no
    // strict equality, so it answers nothing from its seeds and inputs but the assembler's seed, and both outlive it.
    RefPtr<BaselineJITCode> code;
    {
        TwinSeeds seeds { };
        seeds.assembler = assemblerSeed;
        TwinCompileInputs compileInputs { };
        Ref<BaselineJITPlan> plan = adoptRef(*new BaselineJITPlan(codeBlock));
        JIT jit(vm, plan.get(), codeBlock);
        jit.setJITCacheTwin(ProducerBudget::createUnlimited(), seeds, compileInputs);
        code = jit.compileAndLinkWithoutFinalizing(JITCompilationCanFail);
    }
    auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
    if (!record || record->state() != RecordState::Complete) {
        JITCACHE_FAIL(makeString("counter's record is missing or not complete (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
        return;
    }

    // The baked-facts section the record gives, and beside it the smallest image.baseline section that passes the image
    // section's checks: four zero bytes of code and no fixup, call, mold, table, code-map entry or MathIC.
    Vector<uint8_t> bakedFactsBytes;
    auto append = [&](std::span<const uint8_t> bytes) {
        bakedFactsBytes.append(bytes);
        return true;
    };
    ImageSectionSink sink = append;
    if (!writeBakedFactsSection(record->bakedFacts(), sink)) {
        JITCACHE_FAIL("writing the baked facts failed"_s);
        return;
    }
    ImageSectionHeader header;
    header.codeSize = 4;
    Vector<uint8_t> imageBytes(FillWith { }, static_cast<size_t>(ImageSectionSize(header).bytes()), 0);
    auto encodedHeader = encodeImageSectionHeader(header);
    memcpySpan(imageBytes.mutableSpan(), std::span<const uint8_t> { encodedHeader });

    // A twins build parses the twins section beside them: the record's twin data, with no producer value, since the
    // image holds no fixup.
    Vector<uint8_t> twinsBytes;
    auto appendTwins = [&](std::span<const uint8_t> bytes) {
        twinsBytes.append(bytes);
        return true;
    };
    ImageSectionSink twinsSink = appendTwins;
    if (!writeTwinsSection(record->twinData(), { }, captureProcessToken(), twinsSink)) {
        JITCACHE_FAIL("writing the twins section failed"_s);
        return;
    }

    auto facts = parseBakedFactsSection(bakedFactsBytes.span(), true);
    auto view = parseImageSections(ImageSectionSpans { .image = imageBytes.span(), .bakedFacts = bakedFactsBytes.span(), .twins = twinsBytes.span() }, true);
    if (!facts || !view) {
        JITCACHE_FAIL(makeString("the sections do not parse: "_s, description(!facts ? facts.error() : view.error())));
        return;
    }
    if (facts->capabilityLevel() == DFG::CannotCompile || facts->couldBeTainted()) {
        JITCACHE_FAIL("counter compiled as CannotCompile or tainted, so neither flag can make a newborn differ"_s);
        return;
    }
    auto hasFact = [&](ScopeOpcode opcode) {
        for (unsigned index = 0; index < facts->scopeFactCount(); ++index) {
            if (facts->scopeFact(index).opcode == opcode)
                return true;
        }
        return false;
    };
    JITCACHE_CHECK(hasFact(ScopeOpcode::ResolveScope));
    JITCACHE_CHECK(hasFact(ScopeOpcode::GetFromScope));
    JITCACHE_CHECK(hasFact(ScopeOpcode::PutToScope));

    // Runs change, then the comparison, on a newborn CB of freshCounter, used only inside the deferral scope that created
    // it. None is ever installed, so each is the executable's first CB for calls.
    FunctionExecutable* freshExecutable = freshCounter->jsExecutable();
    auto compareNewborn = [&](ASCIILiteral name, BakedFactsResult expected, const auto& change) {
        DeferGCForAWhile deferGC(vm);
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        CodeBlock* newborn = freshExecutable->newCodeBlockFor(CodeSpecializationKind::CodeForCall, freshCounter, freshCounter->scope());
        if (scope.exception()) {
            scope.clearException();
            JITCACHE_FAIL(makeString(name, ": newCodeBlockFor threw"_s));
            return;
        }
        if (!newborn || newborn->jitType() != JITType::None) {
            JITCACHE_FAIL(makeString(name, ": newCodeBlockFor gave no newborn CB"_s));
            return;
        }
        if (newborn->capabilityLevelState() != DFG::CapabilityLevelNotSet) {
            JITCACHE_FAIL(makeString(name, ": linking memoized the capability level"_s));
            return;
        }
        change(*newborn);
        if (compareBakedFacts(*view, *newborn) != expected)
            JITCACHE_FAIL(makeString(name, ": compareBakedFacts gives the wrong result"_s));
        if (newborn->capabilityLevelState() != DFG::CapabilityLevelNotSet)
            JITCACHE_FAIL(makeString(name, ": the comparison memoized the capability level"_s));
    };
    auto unchanged = [](CodeBlock&) { };

    compareNewborn("an unchanged newborn"_s, BakedFactsResult::Match, unchanged);

    // computeCapabilityLevel reads the flag, which makes every CB of the executable CannotCompile.
    freshExecutable->setNeverOptimize(true);
    compareNewborn("a never-optimize executable"_s, BakedFactsResult::Mismatch, unchanged);
    freshExecutable->setNeverOptimize(false);

    // The CodeBlock constructor takes couldBeTainted from the provider.
    SourceProvider* provider = freshExecutable->source().provider();
    SourceTaintedOrigin originalTaint = provider->sourceTaintedOrigin();
    provider->setSourceTaintedOrigin(SourceTaintedOrigin::KnownTainted);
    compareNewborn("a tainted provider"_s, BakedFactsResult::Mismatch, unchanged);
    provider->setSourceTaintedOrigin(originalTaint);

    // Each scope fact's resolve type becomes GlobalVar, which no baked fact has. The collector reads a resolve_scope
    // entry's cell union whatever its type, and skips the structure-or-watchpoint union of a GlobalVar get_from_scope or
    // put_to_scope entry (CodeBlock::reconcileLLIntInlineCachesAtGCEnd), so the newborn stays safe to collect.
    for (unsigned index = 0; index < facts->scopeFactCount(); ++index) {
        ScopeFact fact = facts->scopeFact(index);
        compareNewborn("a changed scope type"_s, BakedFactsResult::Mismatch, [&](CodeBlock& newborn) {
            auto instruction = newborn.instructions().at(BytecodeIndex(fact.bytecodeOffset));
            switch (fact.opcode) {
            case ScopeOpcode::ResolveScope:
                instruction->as<OpResolveScope>().metadata(&newborn).m_resolveType = GlobalVar;
                return;
            case ScopeOpcode::GetFromScope: {
                auto& metadata = instruction->as<OpGetFromScope>().metadata(&newborn);
                metadata.m_getPutInfo = GetPutInfo(metadata.m_getPutInfo.resolveMode(), GlobalVar, metadata.m_getPutInfo.initializationMode(), metadata.m_getPutInfo.ecmaMode());
                return;
            }
            case ScopeOpcode::PutToScope: {
                auto& metadata = instruction->as<OpPutToScope>().metadata(&newborn);
                metadata.m_getPutInfo = GetPutInfo(metadata.m_getPutInfo.resolveMode(), GlobalVar, metadata.m_getPutInfo.initializationMode(), metadata.m_getPutInfo.ecmaMode());
                return;
            }
            }
        });
        // A resolve_scope of type ClosureVar also bakes its depth.
        if (fact.opcode == ScopeOpcode::ResolveScope && fact.resolveType == ClosureVar) {
            compareNewborn("a changed scope depth"_s, BakedFactsResult::Mismatch, [&](CodeBlock& newborn) {
                newborn.instructions().at(BytecodeIndex(fact.bytecodeOffset))->as<OpResolveScope>().metadata(&newborn).m_localScopeDepth = fact.localScopeDepth + 1;
            });
        }
    }
}

// The image check's twin CodeBlock is registered as long as it lives, dies in the first full collection after the check,
// since nothing reaches it, and writes no didOptimize as it dies, while a CodeBlock of the same body linked natively and
// left unreachable the same way writes False. A dying CodeBlock writes didOptimize only through its metadata table, and
// UnlinkedMetadataTable::link() gives none to a body with no metadata entry or value profile, such as `return a + b`. The
// bodies read a captured variable, whose resolve_scope and get_from_scope give them a table, so that a write by the twin
// would show.
JITCACHE_TEST_WITH_OPTIONS(imageTwinDiesWithoutWritingDidOptimize, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto* globalObject = createRealm(context, vm, "var producerBody, importBody; (function makeBodies() { var captured = 3; producerBody = function (a, b) { return a + b + captured; }; importBody = function (a, b) { return a + b + captured; }; })(); producerBody(1, 2);"_s, "the bodies"_s);
    if (!globalObject)
        return;
    auto* producerFunction = dynamicDowncast<JSFunction>(globalValue(globalObject, "producerBody"_s));
    auto* importFunction = dynamicDowncast<JSFunction>(globalValue(globalObject, "importBody"_s));
    if (!producerFunction || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("a body is missing, or importBody was called"_s);
        return;
    }
    JITCACHE_CHECK(!imageTwinCountForTesting());

    if (!imageCaptureImportAndCheckTwin(context, vm, *producerFunction, *importFunction))
        return;
    UnlinkedCodeBlock* unlinkedCodeBlock = importFunction->jsExecutable()->codeBlockForCall()->unlinkedCodeBlock();
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::Indeterminate);

    // The collection sweeps synchronously, so the twin dies in it and leaves the registry without writing didOptimize.
    imageClearDeadStack();
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    JITCACHE_CHECK(!imageTwinCountForTesting());
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::Indeterminate);

    // The native CodeBlock's death writes False, so the check above would have seen a write by the twin.
    if (!imageLinkUnreachableCodeBlock(context, vm, *importFunction))
        return;
    imageClearDeadStack();
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    JITCACHE_CHECK(unlinkedCodeBlock->didOptimize() == TriState::False);
}

// A body whose emission meets a reason a record cannot be kept for compiles under a recorder into code whose record is
// Unrecordable for that reason, which isImageCapturable refuses, so no capture ever reads it. A body generated after
// VM::enableControlFlowProfiler, as Bun's coverage turns the profiler on, holds op_profile_control_flow, whose emitter
// clears m_isShareable. A builtin-mode function made with createBuiltinExecutable, as $vm.createBuiltin makes one, is
// where @superSamplerBegin and @superSamplerEnd lex. The two guards, triggered through their hooks, are
// imageGuardsMarkUnannotatedLinks's and imageSharedHelpersMarkUnannotatedReferences's.
JITCACHE_TEST(imageUnrecordableBodiesAreNeverCaptured, Yes)
{
    VM& vm = *context.vm();
    auto* globalObject = JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()));
    auto run = [&](ASCIILiteral source) {
        NakedPtr<Exception> exception;
        evaluate(globalObject, makeSource(source, SourceOrigin(), SourceTaintedOrigin::Untainted), JSValue(), exception);
        if (exception)
            JITCACHE_FAIL(makeString("evaluating \""_s, source, "\" threw"_s));
        return !exception;
    };

    // The builtin's body is generated at its first call, before the profiler changes the code-generation mode of the bodies
    // generated after it (JSGlobalObject::defaultCodeGenerationMode), so the super-sampler opcodes are the one reason its
    // emission meets.
    SourceCode builtinSource = makeSource("(function () { @superSamplerBegin(); var sampled = 1; @superSamplerEnd(); return sampled; })"_s, SourceOrigin(), SourceTaintedOrigin::Untainted);
    UnlinkedFunctionExecutable* builtin = createBuiltinExecutable(vm, builtinSource, Identifier::fromString(vm, "jitcacheSampled"_s), ImplementationVisibility::Public, ConstructorKind::None, ConstructAbility::CannotConstruct, InlineAttribute::None);
    globalObject->putDirect(vm, Identifier::fromString(vm, "jitcacheSampled"_s), JSFunction::create(vm, globalObject, builtin->link(vm, nullptr, builtinSource), globalObject));
    if (!run("jitcacheSampled();"_s))
        return;
    vm.enableControlFlowProfiler();
    if (!run("function jitcacheCovered(a) { if (a) return 1; return 2; } jitcacheCovered(1);"_s))
        return;

    struct Case {
        ASCIILiteral function;
        Unrecordable reason;
    };
    for (auto& unrecordable : { Case { "jitcacheSampled"_s, Unrecordable::SuperSamplerOpcode }, Case { "jitcacheCovered"_s, Unrecordable::NotShareable } }) {
        auto* function = globalFunctionOrNull(*globalObject, unrecordable.function);
        CodeBlock* codeBlock = function ? function->jsExecutable()->codeBlockForCall() : nullptr;
        if (!codeBlock) {
            JITCACHE_FAIL(makeString(unrecordable.function, " has no CodeBlock"_s));
            continue;
        }
        auto code = compileUnderTwinRecorder(vm, *codeBlock);
        if (!code) {
            JITCACHE_FAIL("no executable memory for the recorded compilation"_s);
            return;
        }
        auto* record = code->m_jitCacheImageRecord.get();
        if (!record || record->state() != RecordState::Unrecordable || record->unrecordableReason() != unrecordable.reason)
            JITCACHE_FAIL(makeString(unrecordable.function, "'s record is missing or not Unrecordable for its reason (state "_s, record ? static_cast<unsigned>(record->state()) : 0u, ", reason "_s, record ? static_cast<unsigned>(record->unrecordableReason()) : 0u, ')'));
        JITCACHE_CHECK(!isImageCapturable(*code));
        if (unrecordable.reason == Unrecordable::NotShareable)
            JITCACHE_CHECK(!code->m_isShareable);
    }
}

// In the ASan builds that run this test the executable pool is the libpas JIT heap, where
// useRandomizingExecutableIslandAllocation only allocates and frees a few blocks before each allocation, so no option
// puts an image out of a bl's reach of its thunks, and the test makes the distance itself. A first compilation of
// jitcacheFarProducer generates every thunk its image calls or jumps to. Blocks of the image's size then fill the pool
// until one starts more than islandDistance past the farthest of those thunks, and stay allocated until the test ends, so
// every image allocated after them lies out of reach too: the producer's, captured with strict on, whose checks follow
// its islands; the one prepareImage restores for jitcacheFarImport with strict on, which the writers patch through
// islands and whose check follows them; and the twin that Twins::checkImage compiles and compares with it, through the
// islands of both. Every support branch of the producer's and the restored image must reach its thunk through an island
// on ARM64, and directly on x86_64, whose rel32 reaches the whole pool.
JITCACHE_TEST_WITH_OPTIONS(imageImportPatchesThroughIslandsToFarSupport, Yes, "--useConcurrentJIT=false")
{
    VM& vm = *context.vm();
    auto* globalObject = createRealm(context, vm, farBodiesSource(), "the far bodies"_s);
    if (!globalObject)
        return;
    auto* producerFunction = globalFunctionOrNull(*globalObject, "jitcacheFarProducer"_s);
    auto* importFunction = globalFunctionOrNull(*globalObject, "jitcacheFarImport"_s);
    CodeBlock* producer = producerFunction ? producerFunction->jsExecutable()->codeBlockForCall() : nullptr;
    if (!producer || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
        JITCACHE_FAIL("a far body is missing, or jitcacheFarImport was called"_s);
        return;
    }

    // The thunks every compilation of the body branches to, and the size of its image.
    uintptr_t farthestSupport = 0;
    size_t codeSize = 0;
    {
        auto code = compileUnderTwinRecorder(vm, *producer);
        auto* record = code ? code->m_jitCacheImageRecord.get() : nullptr;
        if (!record || record->state() != RecordState::Complete) {
            JITCACHE_FAIL("the first compilation of jitcacheFarProducer left no complete record"_s);
            return;
        }
        codeSize = record->codeSize();
        for (auto& fixup : record->fixups()) {
            if (auto support = supportBranchTarget(vm, fixup))
                farthestSupport = std::max(farthestSupport, *support);
        }
    }
    if (!farthestSupport) {
        JITCACHE_FAIL("jitcacheFarProducer's image branches to no thunk"_s);
        return;
    }
    uintptr_t reach = farthestSupport + islandDistance;

    Vector<RefPtr<ExecutableMemoryHandle>> fill;
    while (true) {
        auto block = ExecutableAllocator::singleton().allocate(codeSize, JITCompilationCanFail);
        if (!block) {
            JITCACHE_FAIL(makeString("the pool held "_s, fill.size(), " blocks of "_s, codeSize, " bytes, none out of the thunks' reach"_s));
            return;
        }
        auto start = reinterpret_cast<uintptr_t>(block->start().untaggedPtr());
        fill.append(WTF::move(block));
        if (start > reach)
            break;
    }
    auto isFar = [&](const void* start) {
        return reinterpret_cast<uintptr_t>(start) > reach;
    };

    auto captured = imageCompileAndCapture(context, vm, *producerFunction);
    if (!captured)
        return;
    auto* producerRecord = captured->code->m_jitCacheImageRecord.get();
    if (!producerRecord || producerRecord->state() != RecordState::Complete || !isFar(captured->code->start())) {
        JITCACHE_FAIL("the producer's image is not complete, or lies within reach of its thunks"_s);
        return;
    }
    auto producerCode = unsafeMakeSpan(static_cast<const uint8_t*>(captured->code->start()), producerRecord->codeSize());
    JITCACHE_CHECK(checkFarSupportBranches(context, vm, "the producer's image"_s, producerCode, producerRecord->fixups()));

    bool imported = imageImportAndCheckTwin(context, vm, *importFunction, *captured, [&](BaselineJITCode& restored, const ImageSectionsView& view) {
        if (!isFar(restored.start())) {
            JITCACHE_FAIL("the restored image lies within reach of its thunks"_s);
            return;
        }
        auto restoredCode = unsafeMakeSpan(static_cast<const uint8_t*>(restored.start()), view.header().codeSize);
        JITCACHE_CHECK(checkFarSupportBranches(context, vm, "the restored image"_s, restoredCode, view.fixups()));
    });
    JITCACHE_CHECK(imported);
}

// An allocation's handle (JITCodeWithCodeRef::size, a MathIC's m_code.size()) can end past what its LinkBuffer wrote, the
// linked size the record keeps, and those bytes hold whatever the pool held. In a Producer, every body of
// slackBodiesSource compiles with a complete record and regenerates its one MathIC. For each, the producer's image and
// snippet are filled past their linked sizes and captured with strict on, and the sections must hold exactly the linked
// bytes; the import then fills the restored image and snippet past theirs, and the twin's are filled as
// Twins::checkImage links them, so wherever two sides have slack it differs while the check compares, and the check must
// report nothing. Afterwards each side's slack must still hold its own fill, at least one body's image must have slack on
// all three sides, and on ARM64 at least one body's snippet must too.
JITCACHE_TEST_WITH_OPTIONS(imageSlackStaysOutsideCaptureAndTwinCheck, No, "--useConcurrentJIT=false --logJIT=true")
{
    ImageTwinSlackLog log;
    ImageDataFileScope dataFile(log);
    auto directory = TemporaryDirectory::create(context, "jitcache-image"_s);
    if (!directory)
        return;
    ExtraVM producer;
    VM& vm = producer.vm();
    JITCache::Config config;
    config.artifactPath = directory->path("artifact"_s);
    config.role = Role::Producer;
    config.producerLimitBytes = 64 * MB;
    config.strict = true;
    StartResult started = JITCache::start(vm, config);
    if (started.outcome != StartOutcome::Started) {
        JITCACHE_FAIL(makeString("the producer did not start: "_s, toJSON(started)));
        return;
    }

    auto* globalObject = createRealm(context, vm, slackBodiesSource(), "the slack bodies"_s);
    if (!globalObject)
        return;
    auto* producers = globalValue(globalObject, "jitcacheSlackProducers"_s).getObject();
    auto* imports = globalValue(globalObject, "jitcacheSlackImports"_s).getObject();
    if (!producers || !imports) {
        JITCACHE_FAIL("the slack bodies' arrays are missing"_s);
        return;
    }

    auto holdsOwnFill = [&](unsigned index, ASCIILiteral side, const Vector<uint8_t>& slack, uint8_t fill) {
        if (!slack.isEmpty() && !std::ranges::all_of(slack.span(), [&](uint8_t byte) { return byte == fill; }))
            JITCACHE_FAIL(makeString("body "_s, index, ": "_s, side, " no longer holds its fill past its linked size"_s));
    };
    unsigned imageCandidates = 0;
    unsigned snippetCandidates = 0;
    StringBuilder slackSizes;
    for (unsigned index = 0; index < slackBodyCount; ++index) {
        auto* producerFunction = dynamicDowncast<JSFunction>(producers->get(globalObject, index));
        auto* importFunction = dynamicDowncast<JSFunction>(imports->get(globalObject, index));
        if (!producerFunction || !importFunction || importFunction->jsExecutable()->codeBlockForCall()) {
            JITCACHE_FAIL(makeString("slack body "_s, index, " is missing, or its import was called"_s));
            return;
        }
        // The earlier bodies' twins die and are swept here, so no allocation of this body's check sweeps one and changes
        // the twin count the check compares.
        imageClearDeadStack();
        vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
        auto slack = imageCheckSlackOfBody(context, vm, *producerFunction, *importFunction, log);
        if (!slack)
            return;
        holdsOwnFill(index, "the producer's image"_s, slack->producerImage, producerSlackFill);
        holdsOwnFill(index, "the restored image"_s, slack->restoredImage, restoredSlackFill);
        holdsOwnFill(index, "the twin's image"_s, slack->twinImage, twinSlackFill);
        holdsOwnFill(index, "the producer's snippet"_s, slack->producerSnippet, producerSlackFill);
        holdsOwnFill(index, "the restored snippet"_s, slack->restoredSnippet, restoredSlackFill);
        holdsOwnFill(index, "the twin's snippet"_s, slack->twinSnippet, twinSlackFill);
        imageCandidates += !slack->producerImage.isEmpty() && !slack->restoredImage.isEmpty() && !slack->twinImage.isEmpty();
        snippetCandidates += !slack->producerSnippet.isEmpty() && !slack->restoredSnippet.isEmpty() && !slack->twinSnippet.isEmpty();
        slackSizes.append(index ? "; "_s : ""_s, "image "_s, slack->producerImage.size(), '/', slack->restoredImage.size(), '/', slack->twinImage.size(),
            ", snippet "_s, slack->producerSnippet.size(), '/', slack->restoredSnippet.size(), '/', slack->twinSnippet.size());
    }
    // On x86_64 a handle ends past its linked size, which LinkBuffer pads to jitAllocationGranule, only when the JIT
    // heap serves the request from a larger size class. For an allocation as small as a snippet the heap usually has a
    // class of exactly its size, and whether it uses a larger one depends on the sizes the process allocated first. On
    // ARM64, compaction shortens a snippet after its LinkBuffer allocated it and the JIT heap does not shrink an
    // allocation that small, so the producer's and the twin's snippets keep slack; the restored snippet, allocated at
    // the linked size, has slack when that size is off the heap's 16-byte alignment, which the varied operations make
    // likely. So the check requires a snippet with slack only on ARM64.
    if (!imageCandidates || (isARM64() && !snippetCandidates))
        JITCACHE_FAIL(makeString("no body has slack on all three sides for its image ("_s, imageCandidates, " do) or its snippet ("_s, snippetCandidates, " do); slack bytes, producer/restored/twin: "_s, slackSizes.toString()));
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
