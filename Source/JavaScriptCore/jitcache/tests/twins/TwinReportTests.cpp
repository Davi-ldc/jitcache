#include "config.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/tests/support/JITCacheTest.h"
#include "jitcache/tests/support/TestFiles.h"
#include "jitcache/twins/TwinReport.h"
#include <array>
#include <utility>
#include <wtf/ASCIICType.h>
#include <wtf/JSONValues.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

// Tests of TwinReport (twins/TwinReport.h and .cpp), whose lines the runner parses as JSON.

namespace JSC::JITCache::Tests {

namespace TwinReportTestsInternal {

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

} // namespace TwinReportTestsInternal

using namespace TwinReportTestsInternal;

// Every line of the twin report parses as JSON, with the fields in the order TwinReport writes them and each value as
// written, whatever characters a detail holds: quotes, backslashes, control characters, characters outside ASCII and a
// lone surrogate. WTF's parser also accepts raw control characters and the escapes \x and \v, so each line must first
// pass jsonGrammarProblem, as the runner's JSON.parse would require. The runner's self-test abort.js shows the other
// half, that a line written right before an abort() is in the file.
JITCACHE_TEST(integratorTwinReportLinesParseAsJSON, No)
{
    auto directory = TemporaryDirectory::create(context, "jitcache-integrator"_s);
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

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
