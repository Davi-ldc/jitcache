#pragma once

#if ENABLE(JIT)

#include "ExecutableMemoryHandle.h"
#include "jitcache/image/BakedFacts.h"
#include "jitcache/image/ImageSection.h"
#include <optional>
#include <span>
#include <stdint.h>
#include <wtf/Noncopyable.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

// The preparation of an imported image at install: the image and its MathIC snippets copied once into executable memory,
// every fixup patched with its form's link writer, and the BaselineJITCode built around them, all private to a
// PreparedImage until commit. The install glue calls parseImageSections, compareBakedFacts (BakedFacts.h), prepareImage
// and, once every lane's preparation succeeded, PreparedImage::commit, so that a failure anywhere leaves the CB native.

namespace JSC {

class BaselineJITCode;
class CodeBlock;
class UnlinkedCodeBlock;
class VM;

namespace JITCache {

class ProducerBudget;
class SupportTargetTable;

// The wall time of four stages of prepareImage, read from CLOCK_MONOTONIC for the install event when its caller asks:
// the executable allocations, the copies of the linked bytes, the resolution and patching of every fixup, and the
// instruction-cache flush.
struct PrepareTimings {
    uint64_t allocateNanoseconds { 0 };
    uint64_t copyNanoseconds { 0 };
    uint64_t patchNanoseconds { 0 };
    uint64_t flushNanoseconds { 0 };
};

// A finished BaselineJITCode that nothing outside it references yet. Destroying it frees the code, its snippets, its
// MathICs and its rebuilt record, and leaves the VM's statistics untouched; thunks the resolution generated stay, as the
// VM's first use of them would leave them.
class PreparedImage {
    WTF_MAKE_NONCOPYABLE(PreparedImage);
public:
    PreparedImage(PreparedImage&&);
    ~PreparedImage();

    // Infallible, and the only step with effects outside the prepared objects: the code-size sample
    // JIT::finalizeOnMainThread takes, the cross-modifying fence, and the reports to PerfLog and GdbJIT under useJITDump
    // and useGdbJITInfo, as native finalization does. Consumes the object and returns the code setup will install, with
    // the producer's molds in mold order.
    Ref<BaselineJITCode> commit(VM&, CodeBlock& installing) &&;

private:
    friend std::optional<PreparedImage> prepareImage(VM&, UnlinkedCodeBlock&, const ImageSectionsView&, SupportTargetTable&, ProducerBudget*, PrepareTimings*);
    PreparedImage(Ref<BaselineJITCode>&&, Ref<ExecutableMemoryHandle>&& imageMemory);

    RefPtr<BaselineJITCode> m_code;
    RefPtr<ExecutableMemoryHandle> m_imageMemory; // the code's executable memory, for commit's reports
};

// Prepares the image on the VM thread, with the API lock and heap access, inside the install function's GC deferral and
// with no JSC lock held. It allocates no cell and writes nothing to the CB, the UCB or the VM's statistics, so a failure
// leaves no trace. The UCB is the import's, with the producer's index spaces; it is not const because the MathICs and
// the UCB targets take its arithmetic profiles through accessors with no const overload. The table is the VM's: it
// holds the support targets earlier preparations resolved and keeps the ones this one resolves. A non-null budget,
// given exactly in a ConsumerProducer VM whose production is active, rebuilds the image's record, in twins builds with
// the twin data of the twins section; a refused charge leaves the image without one, and the budget raises the
// recording fault. The borrowed payload need live only until the call returns: the prepared image keeps copies of
// everything it reads. A non-null timings receives the stages' times; null costs nothing. Returns nullopt when the
// image or a snippet got no executable memory.
std::optional<PreparedImage> prepareImage(VM&, UnlinkedCodeBlock&, const ImageSectionsView&, SupportTargetTable&, ProducerBudget*, PrepareTimings* = nullptr);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
