#include "config.h"
#include "jitcache/glue/InstallTwins.h"

#include "BaselineJITCode.h"
#include "CodeBlock.h"
#include "JSCInlines.h"
#include "VM.h"
#include "jitcache/base/BodyKey.h"
#include "jitcache/base/VMPart.h"
#include "jitcache/glue/JITCacheGlue.h"
#include "jitcache/ics/ICTwins.h"
#include "jitcache/image/ImageSection.h"
#include "jitcache/image/ImageTwins.h"
#include "jitcache/session/JITCacheVMState.h"
#include "jitcache/store/ValidatedBody.h"
#include "jitcache/twins/TwinReport.h"
#include <optional>
#include <utility>
#include <wtf/StringPrintStream.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

#if ENABLE(JITCACHE_TWINS)

namespace JSC::JITCache {

#if ENABLE(JIT)

// The image twin check an install leaves for the end of prepareForExecutionImpl, where the scope the twin needs exists.
// The body's reference keeps the bytes the view reads alive until the check returns.
struct PendingImageTwinCheck {
    CodeBlock* codeBlock;
    Ref<BaselineJITCode> code;
    Ref<ValidatedBody> body;
    ImageSectionsView view;
};

// The VM's image twin-check state, a part of the VM's state that willDestroyVM destroys (VMPart.h). The install creates
// it at the VM's first stash, so the Twins object exists no later than the VM's first checkImage and dies during VM
// destruction.
struct ImageTwinCheckState final : VMPart {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(ImageTwinCheckState);

    static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilVMDestruction;

    Twins twins;
    std::optional<PendingImageTwinCheck> pending; // at most one
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(ImageTwinCheckState);

namespace InstallTwinsInternal {

// A twin report's detail names the body by its key in hex and the CB by CodeBlock::dump.
static String twinCheckSubject(const BodyKey& key, CodeBlock& codeBlock)
{
    return makeString("body "_s, bodyKeyHex(key), ", "_s, WTF::toString(codeBlock));
}

static ASCIILiteral icsTwinSiteName(ICs::TwinMismatch::Site site)
{
    switch (site) {
    case ICs::TwinMismatch::Site::PropertyIC:
        return "property IC"_s;
    case ICs::TwinMismatch::Site::CallLink:
        return "call link"_s;
    case ICs::TwinMismatch::Site::Capture:
        return "capture"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

// The image twin check of a CB that JIT::compileSync installed outside prepareForExecutionImpl, which has no scope to give
// the twin. The CB is named by the body alone, since nothing kept it alive since its install.
static void reportImageTwinCheckWithoutScope(TwinReport& report, const PendingImageTwinCheck& pending)
{
    report.skip(TwinPart::Image, "no-scope"_s, makeString("body "_s, bodyKeyHex(pending.body->key()), ": installed by JIT::compileSync outside ScriptExecutable::prepareForExecutionImpl, so the twin has no scope"_s));
}

} // namespace InstallTwinsInternal

void reportICsTwinMismatches(TwinReport& report, const BodyKey& key, CodeBlock& codeBlock, const Vector<ICs::TwinMismatch>& mismatches)
{
    using namespace InstallTwinsInternal;
    if (mismatches.isEmpty())
        return;
    String subject = twinCheckSubject(key, codeBlock);
    for (const ICs::TwinMismatch& mismatch : mismatches)
        report.difference(TwinPart::ICs, mismatch.field, makeString(subject, ": "_s, icsTwinSiteName(mismatch.site), ' ', mismatch.index, ", expected "_s, mismatch.expected, ", actual "_s, mismatch.actual));
}

void stashImageTwinCheck(VMState& state, CodeBlock& codeBlock, Ref<BaselineJITCode>&& code, Ref<ValidatedBody>&& body, const ImageSectionsView& view)
{
    ImageTwinCheckState& twinCheckState = state.part<ImageTwinCheckState>();
    if (twinCheckState.pending) {
        if (TwinReport* report = state.twinReportSink())
            InstallTwinsInternal::reportImageTwinCheckWithoutScope(*report, *twinCheckState.pending);
    }
    twinCheckState.pending.emplace(PendingImageTwinCheck { &codeBlock, WTF::move(code), WTF::move(body), view });
}

void didFinishPrepareForExecution(VM& vm, CodeBlock& codeBlock, JSScope* scope)
{
    VMState* state = vm.jitCacheState();
    if (!state)
        return;
    ImageTwinCheckState* twinCheckState = state->partIfExists<ImageTwinCheckState>();
    if (!twinCheckState || !twinCheckState->pending)
        return;

    // The check is consumed here whatever follows. The local keeps the body, and with it the bytes the view reads, until
    // the check returns, and a prepareForExecutionImpl the check itself reaches finds nothing pending.
    std::optional<PendingImageTwinCheck> pending = std::exchange(twinCheckState->pending, std::nullopt);
    TwinReport* report = state->twinReportSink();
    if (!report)
        return;

    // The stash holds this CB when the install ran for it, at either install point: no JS ran since, so it still runs the
    // code that install committed. Otherwise JIT::compileSync installed another CB outside prepareForExecutionImpl.
    if (pending->codeBlock != &codeBlock || codeBlock.jitCode().get() != pending->code.ptr()) {
        InstallTwinsInternal::reportImageTwinCheckWithoutScope(*report, *pending);
        return;
    }
    twinCheckState->twins.checkImage(vm, codeBlock, scope, pending->code.get(), pending->view, *report);
}

#else // !ENABLE(JIT)

void didFinishPrepareForExecution(VM&, CodeBlock&, JSScope*)
{
}

#endif // ENABLE(JIT)

} // namespace JSC::JITCache

#endif // ENABLE(JITCACHE_TWINS)
