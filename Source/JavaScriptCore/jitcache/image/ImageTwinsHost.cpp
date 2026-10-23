#include "config.h"
#include "jitcache/image/ImageTwinsHost.h"

#if ENABLE(JITCACHE_TWINS)

#include "jitcache/image/ImageTwins.h"
#include <array>
#include <utility>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringView.h>

namespace JSC::JITCache {

bool setImageTestHookNamed(const char* name)
{
    StringView hookName = StringView::fromLatin1(name);
#if ENABLE(JIT)
    static constexpr std::array<std::pair<ASCIILiteral, ImageTestHook>, 2> hooks { {
        { "relocation-pairs"_s, ImageTestHook::RelocationPairs },
        { "operation-pair"_s, ImageTestHook::OperationPair },
    } };
    for (auto& [hookFlagName, hook] : hooks) {
        if (hookName == hookFlagName) {
            setImageTestHook(hook);
            return true;
        }
    }
#else
    UNUSED_PARAM(hookName);
#endif
    return false;
}

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
