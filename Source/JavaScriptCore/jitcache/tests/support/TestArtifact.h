#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/glue/JITCacheCapture.h"
#include "jitcache/tests/support/TestPack.h"
#include "jitcache/tests/support/TestStart.h"
#include <wtf/text/MakeString.h>
#include <wtf/text/WTFString.h>

// Both halves of an artifact test's support, the pack fixtures of TestPack.h and the start helpers of TestStart.h, with
// the comparison and text of capture scores, which only the capture glue's tests use.

namespace JSC::JITCache::Tests {

inline bool sameScore(const CaptureScore& a, const CaptureScore& b)
{
    return a.tier == b.tier && a.richness == b.richness && a.icSitesWithCases == b.icSitesWithCases
        && a.counterWithheld == b.counterWithheld && a.counterProgress == b.counterProgress;
}

// The score as a failure message prints it.
inline String scoreText(const CaptureScore& score)
{
    return makeString("{ tier "_s, static_cast<unsigned>(score.tier), ", richness "_s, score.richness, ", IC sites with cases "_s,
        score.icSitesWithCases, ", counter "_s, score.counterWithheld ? "withheld"_s : "carried"_s, ", progress "_s, score.counterProgress, " }"_s);
}

} // namespace JSC::JITCache::Tests

#endif // ENABLE(JITCACHE_TWINS)
