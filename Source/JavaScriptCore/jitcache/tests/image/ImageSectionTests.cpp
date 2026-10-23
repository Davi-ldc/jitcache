#include "config.h"

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "jitcache/image/BakedFacts.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageTypes.h"
#include "jitcache/tests/image/ImageSectionTestSupport.h"
#include "jitcache/tests/support/JITCacheTest.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

// Tests of image.baseline and baked-facts.baseline (image/ImageSection.h, image/BakedFacts.h and their .cpp). Valid
// sections, built for the build's architecture from a live UCB, parse to views that read back the model they were
// encoded from, and a section truncated at any region or entry boundary cannot be located and fails its section's
// check. Every section is parsed from an allocation of exactly its size, so ASan reports any read past its end.

namespace JSC::JITCache::Tests {

using namespace ImageSectionTestSupport;

namespace ImageSectionTestsInternal {

static uint64_t sectionSize(const TestSection& section)
{
    ImageSectionSize size(headerOf(section));
    for (auto& table : section.simpleTables)
        size.addSimpleSwitchTable(table.offsets.size());
    for (auto& table : section.stringTables)
        size.addStringSwitchTable(table.size());
    for (auto& mathIC : section.mathICs) {
        if (mathIC.entry.hasSnippet() && mathIC.snippet)
            size.addSnippet(mathIC.snippet->code.size(), mathIC.snippet->fixups.size());
    }
    return size.bytes();
}

// Everything the view reads back equals the model it was encoded from.
static void checkViewMatches(TestContext& context, const ImageSectionsView& view, const TestSection& section)
{
    JITCACHE_CHECK(view.header() == headerOf(section));
    JITCACHE_CHECK(equalSpans(view.code(), section.code.span()));
    auto fixups = view.fixups();
    JITCACHE_CHECK(fixups.size() == section.fixups.size());
    for (size_t index = 0; index < std::min(fixups.size(), section.fixups.size()); ++index) {
        JITCACHE_CHECK(fixups[index] == section.fixups[index]);
        JITCACHE_CHECK(fixups.find(section.fixups[index].site, section.fixups[index].form) == section.fixups[index]);
    }
    for (unsigned index = 0; index < section.calls.size(); ++index)
        JITCACHE_CHECK(view.call(index) == section.calls[index]);
    for (unsigned index = 0; index < section.molds.size(); ++index)
        JITCACHE_CHECK(view.mold(index) == section.molds[index]);
    unsigned simpleTables = 0;
    view.forEachSimpleSwitchTable([&](unsigned index, const ImageSimpleSwitchTable& table) {
        ++simpleTables;
        auto& expected = section.simpleTables[index];
        JITCACHE_CHECK(table.defaultOffset == expected.defaultOffset && table.offsets.size() == expected.offsets.size());
        for (size_t entry = 0; entry < std::min(table.offsets.size(), expected.offsets.size()); ++entry)
            JITCACHE_CHECK(table.offsets[entry] == expected.offsets[entry]);
    });
    JITCACHE_CHECK(simpleTables == section.simpleTables.size());
    unsigned stringTables = 0;
    view.forEachStringSwitchTable([&](unsigned index, const ImageOffsetArray& offsets) {
        ++stringTables;
        auto& expected = section.stringTables[index];
        JITCACHE_CHECK(offsets.size() == expected.size());
        for (size_t entry = 0; entry < std::min(offsets.size(), expected.size()); ++entry)
            JITCACHE_CHECK(offsets[entry] == expected[entry]);
    });
    JITCACHE_CHECK(stringTables == section.stringTables.size());
    auto codeMap = view.codeMap();
    JITCACHE_CHECK(codeMap.size() == section.codeMap.size());
    for (size_t index = 0; index < std::min(codeMap.size(), section.codeMap.size()); ++index)
        JITCACHE_CHECK(codeMap[index] == section.codeMap[index]);
    for (unsigned index = 0; index < section.constantPool.size(); ++index)
        JITCACHE_CHECK(view.constantPoolEntry(index) == section.constantPool[index]);
    for (unsigned index = 0; index < section.mathICs.size(); ++index)
        JITCACHE_CHECK(view.mathIC(index) == section.mathICs[index].entry);
    unsigned snippets = 0;
    view.forEachSnippet([&](unsigned index, const ImageSnippet& snippet) {
        ++snippets;
        auto& expected = section.mathICs[index].snippet;
        JITCACHE_CHECK(expected && equalSpans(snippet.code, expected->code.span()) && snippet.fixups.size() == expected->fixups.size());
        for (size_t entry = 0; expected && entry < std::min(snippet.fixups.size(), expected->fixups.size()); ++entry)
            JITCACHE_CHECK(snippet.fixups[entry] == expected->fixups[entry]);
    });
    JITCACHE_CHECK(snippets == 1);
    auto& bakedFacts = view.bakedFacts();
    JITCACHE_CHECK(bakedFacts.capabilityLevel() == section.bakedFacts.capabilityLevel);
    JITCACHE_CHECK(bakedFacts.couldBeTainted() == section.bakedFacts.couldBeTainted);
    JITCACHE_CHECK(bakedFacts.scopeFactCount() == section.bakedFacts.scopeFacts.size());
    for (unsigned index = 0; index < std::min<size_t>(bakedFacts.scopeFactCount(), section.bakedFacts.scopeFacts.size()); ++index)
        JITCACHE_CHECK(bakedFacts.scopeFact(index) == section.bakedFacts.scopeFacts[index]);
}

} // namespace ImageSectionTestsInternal

// image.baseline and baked-facts.baseline: the writers' sizes, the view reading back the model, and truncation at every
// region and entry boundary, which leaves a section that cannot be located.
JITCACHE_TEST(imageSectionParsing, Yes)
{
    using namespace ImageSectionTestsInternal;
    VM& vm = *context.vm();
    auto body = makeBody(context, vm);
    if (!body)
        return;
    auto valid = makeValidSection(*body);

    // The valid section: the writers' sizes and the view reading back the model.
    Vector<size_t> imageBoundaries;
    auto image = encodeImage(valid, &imageBoundaries);
    auto bakedFacts = encodeBakedFacts(valid.bakedFacts);
    auto twins = encodeTwins(valid);
    JITCACHE_CHECK(image.size() == sectionSize(valid));
    JITCACHE_CHECK(bakedFacts.size() == bakedFactsSectionSize(valid.bakedFacts));
    {
        ExactSections sections(image.span(), bakedFacts.span(), twins.span());
        auto view = parseImageSections(sections.spans());
        if (view)
            checkViewMatches(context, *view, valid);
        else
            JITCACHE_FAIL(makeString("the valid section failed "_s, description(view.error())));
    }

    // Truncation at every region and entry boundary: a section that cannot be located fails Header or BakedFacts.
    imageBoundaries.append(0);
    for (size_t boundary : imageBoundaries) {
        if (boundary >= image.size())
            continue;
        ExactSections sections(image.span().first(boundary), bakedFacts.span(), twins.span());
        auto view = parseImageSections(sections.spans());
        if (view)
            JITCACHE_FAIL(makeString("an image section truncated at "_s, boundary, " parsed"_s));
        else if (view.error() != ImageCheck::Header)
            JITCACHE_FAIL(makeString("an image section truncated at "_s, boundary, " failed "_s, description(view.error())));
    }
    for (size_t boundary = 0; boundary < bakedFacts.size(); boundary += (boundary < bakedFactsHeaderSize ? 4 : bakedFactsEntrySize)) {
        ExactSections sections(image.span(), bakedFacts.span().first(boundary), twins.span());
        auto view = parseImageSections(sections.spans());
        if (view)
            JITCACHE_FAIL(makeString("a baked-facts section truncated at "_s, boundary, " parsed"_s));
        else if (view.error() != ImageCheck::BakedFacts)
            JITCACHE_FAIL(makeString("a baked-facts section truncated at "_s, boundary, " failed "_s, description(view.error())));
    }
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
