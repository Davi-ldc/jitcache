#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "GetPutInfo.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageTwinsSection.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageSectionTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <span>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of image-twins.baseline (image/ImageTwinsSection.h, .cpp), which a twins build always reads beside
// image.baseline and baked-facts.baseline. A valid section, built for the build's architecture from a live UCB, parses
// to a view that reads back the model it was encoded from, and a section truncated at any boundary cannot be located
// and fails its check. Every section is parsed from an allocation of exactly its size, so ASan reports any read past
// its end.

namespace JSC::JITCache::Tests {

using namespace ImageSectionTestSupport;

namespace ImageTwinsSectionTestsInternal {

// Region offsets of the twins section, computed apart from the lane's own arithmetic, for truncating its bytes.
struct TwinsLayout {
    Vector<size_t> regenerations; // each regeneration entry's start
    Vector<size_t> boundaries; // every region's start and the section's end
};

static TwinsLayout twinsLayoutOf(const TestSection& section)
{
    auto& data = section.twins.data;
    TwinsLayout layout;
    size_t offset = imageTwinsHeaderSize;
    auto region = [&](size_t bytes) {
        layout.boundaries.append(offset);
        offset += alignImageSectionOffset(bytes);
    };
    region(4 * data.seeds.binarySwitches.size());
    region(16 * data.compileInputs.inputs.size());
    region(2 * data.compileInputs.binaryArithBits.size());
    region(2 * data.compileInputs.unaryArithBits.size());
    layout.boundaries.append(offset);
    for (auto& regeneration : data.regenerations) {
        layout.regenerations.append(offset);
        offset += alignImageSectionOffset(16 + 4 * regeneration.assemblerSeeds.size());
    }
    region(8 * producerValuesOf(section).size());
    layout.boundaries.append(offset);
    return layout;
}

// The twins view reads back the model it was encoded from and decodes to the twin data the model holds.
static void checkTwinsMatch(TestContext& context, const ImageTwinsView& twins, const TestSection& section)
{
    auto& data = section.twins.data;
    auto producerValues = producerValuesOf(section);
    auto& header = twins.header();
    JITCACHE_CHECK(twins.compiledHoldingAPILock() == data.compileInputs.compiledHoldingAPILock);
    JITCACHE_CHECK(header.assemblerSeed == data.seeds.assembler);
    JITCACHE_CHECK(header.producerValueCount == producerValues.size());
    JITCACHE_CHECK(equalSpans(twins.captureProcessToken(), std::span<const uint8_t> { section.twins.token }));
    for (unsigned index = 0; index < std::min<size_t>(header.producerValueCount, producerValues.size()); ++index)
        JITCACHE_CHECK(twins.producerValue(index) == producerValues[index]);

    auto decoded = twins.twinData();
    JITCACHE_CHECK(decoded.seeds.assembler == data.seeds.assembler);
    JITCACHE_CHECK(decoded.seeds.binarySwitches == data.seeds.binarySwitches);
    JITCACHE_CHECK(decoded.compileInputs.inputs == data.compileInputs.inputs);
    JITCACHE_CHECK(decoded.compileInputs.binaryArithBits == data.compileInputs.binaryArithBits);
    JITCACHE_CHECK(decoded.compileInputs.unaryArithBits == data.compileInputs.unaryArithBits);
    JITCACHE_CHECK(decoded.compileInputs.compiledHoldingAPILock == data.compileInputs.compiledHoldingAPILock);
    JITCACHE_CHECK(decoded.regenerations.size() == data.regenerations.size());
    for (size_t index = 0; index < std::min(decoded.regenerations.size(), data.regenerations.size()); ++index) {
        auto& read = decoded.regenerations[index];
        auto& written = data.regenerations[index];
        JITCACHE_CHECK(read.mathICIndex == written.mathICIndex && read.profileBitsAtEntry == written.profileBitsAtEntry);
        JITCACHE_CHECK(read.replacement == written.replacement && read.assemblerSeeds == written.assemblerSeeds);
    }
    // A rebuilt record charges exactly what the decoded data holds.
    JITCACHE_CHECK(twinDataStorageBytes(decoded) == twins.twinDataStorageBytes());
}

static size_t inputIndexOfKind(const Vector<CompileInput>& inputs, CompileInputKind kind)
{
    for (size_t index = 0; index < inputs.size(); ++index) {
        if (inputs[index].kind == kind)
            return index;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} // namespace ImageTwinsSectionTestsInternal

// image-twins.baseline: the compile inputs the snapshot took from the body, the writer's size, the view reading back the
// model and its twin data, and truncation, which leaves a section that cannot be located.
JITCACHE_TEST(imageTwinsSectionParsing, Yes)
{
    using namespace ImageTwinsSectionTestsInternal;
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto valid = makeValidSection(*body);
    auto layout = twinsLayoutOf(valid);
    auto& validInputs = valid.twins.data.compileInputs.inputs;

    // The snapshot the valid section carries: the test's thread held the API lock, and the closure variable's
    // resolve_scope has its input, a ClosureVar with its depth.
    JITCACHE_CHECK(valid.twins.data.compileInputs.compiledHoldingAPILock);
    auto& resolveScopeInputOfBody = validInputs[inputIndexOfKind(validInputs, CompileInputKind::ResolveScopeType)];
    JITCACHE_CHECK(resolveScopeInputOfBody.bytecodeOffset == body->resolveScopeOffset);
    JITCACHE_CHECK(resolveScopeInputOfBody.value == static_cast<uint8_t>(ClosureVar) && resolveScopeInputOfBody.localScopeDepth && !resolveScopeInputOfBody.moduleImportSlot);

    // The valid section: the writer's size and the view reading back the model.
    auto image = encodeImage(valid);
    auto bakedFacts = encodeBakedFacts(valid.bakedFacts);
    auto twins = encodeTwins(valid);
    JITCACHE_CHECK(twins.size() == twinsSectionSize(valid.twins.data, producerValuesOf(valid).size()));
    JITCACHE_CHECK(twins.size() == layout.boundaries.last());
    {
        ExactSections sections(image.span(), bakedFacts.span(), twins.span());
        auto view = parseImageSections(sections.spans());
        if (view)
            checkTwinsMatch(context, view->twins(), valid);
        else
            JITCACHE_FAIL(makeString("the valid section failed "_s, description(view.error())));
    }

    // A body without the section, and truncation at every region boundary and inside the header and each regeneration's
    // fixed fields: a section that cannot be located fails TwinsHeader.
    Vector<size_t> boundaries = layout.boundaries;
    boundaries.append(0);
    boundaries.append(imageTwinsHeaderSize - 1);
    for (size_t offset : layout.regenerations)
        boundaries.append(offset + imageTwinsRegenerationFixedSize - 1);
    for (size_t boundary : boundaries) {
        if (boundary >= twins.size())
            continue;
        ExactSections sections(image.span(), bakedFacts.span(), twins.span().first(boundary));
        auto view = parseImageSections(sections.spans());
        if (view)
            JITCACHE_FAIL(makeString("a twins section truncated at "_s, boundary, " parsed"_s));
        else if (view.error() != ImageCheck::TwinsHeader)
            JITCACHE_FAIL(makeString("a twins section truncated at "_s, boundary, " failed "_s, description(view.error())));
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
