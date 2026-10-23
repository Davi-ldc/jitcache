#pragma once

#include <stdint.h>
#include <wtf/Noncopyable.h>

// State that one part of JITCache keeps for each configured VM, in a type its own files define. VMState::part creates a
// part at its type's first use, and the state destroys it when its lifetime ends. A part type derives from VMPart and
// declares its lifetime:
//
//     static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilProductionEnds;

namespace JSC::JITCache {

enum class VMPartLifetime : uint8_t {
    UntilVMDestruction, // until willDestroyVM, which runs before the heap's last finalization
    // Production memory: until releaseEndedProductionMemory, which the first glue entry after production ends calls,
    // and willDestroyVM too.
    UntilProductionEnds,
};

struct VMPartKey {
    VMPartLifetime lifetime;
};

// The key the state finds a type's part by: one variable for each part type, whose address names the type and whose
// value gives the part's lifetime. A type that inherits its lifetime from another part type still has a key of its own.
template<typename Part>
inline constexpr VMPartKey vmPartKey { Part::vmPartLifetime };

class VMPart {
    WTF_MAKE_NONCOPYABLE(VMPart);
public:
    virtual ~VMPart() = default;

protected:
    VMPart() = default;
};

} // namespace JSC::JITCache
