#pragma once

#include <wtf/Platform.h>
#include <wtf/Ref.h>
#include <wtf/Vector.h>

// The install's twin checks in twins builds: the ICs lane's mismatches, reported as they are found, and the image twin
// check, which waits for the end of prepareForExecutionImpl, where the scope the twin needs exists. InstallTwins.cpp
// defines them, with the hook that runs the image check (JITCacheGlue.h).

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

namespace JSC {

class BaselineJITCode;
class CodeBlock;

} // namespace JSC

namespace JSC::JITCache {

class BodyKey;
class ImageSectionsView;
class TwinReport;
class ValidatedBody;
class VMState;

namespace ICs {

struct TwinMismatch;

} // namespace ICs

// The ICs lane's twin check, after it attaches its state: each mismatch it finds becomes a difference named by its
// field, with the site, the index, the expected and the actual value in the detail.
void reportICsTwinMismatches(TwinReport&, const BodyKey&, CodeBlock&, const Vector<ICs::TwinMismatch>&);

// Stashes the image twin check of the CB an install just committed for the end of prepareForExecutionImpl, creating the
// VM's image twin-check state at its first stash. A check still pending belongs to a CB that JIT::compileSync installed
// outside prepareForExecutionImpl, whose scope no later call can give, so it is reported as that skip before it is
// replaced. The body's reference keeps the bytes the view reads alive until the check returns.
void stashImageTwinCheck(VMState&, CodeBlock&, Ref<BaselineJITCode>&&, Ref<ValidatedBody>&&, const ImageSectionsView&);

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
