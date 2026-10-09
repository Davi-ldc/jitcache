#pragma once

#include <wtf/Platform.h>

#if ENABLE(JITCACHE_TWINS) && ENABLE(JIT)

#include "CallLinkInfo.h"
#include "ICRestore.h"
#include "InlineCacheHandler.h"
#include "JITCode.h"
#include "JSCJSValue.h"
#include "Opcode.h"
#include "PropertyInlineCache.h"
#include "PropertyInlineCacheSummary.h"
#include <cstdint>
#include <wtf/Vector.h>
#include <wtf/text/ASCIILiteral.h>

// The ICs lane's twin API (SPEC-ics.md section 11.1), built only in test builds. Everything this lane restores is
// history the engine cannot recompute, so its twin is the capture record passed through the derivation of section 6.1;
// the super_construct cache, whose baseline store E1 fixes, has a native twin in the LLInt's effect on the same cache.
//
// A snapshot reads a CB's IC and call-link state through the reader capture uses (readPropertyICRecord and
// readCallLinkRecord in ICCapture.h), under the CB's m_lock, so a snapshot and a capture of the same state agree field
// for field. checkRestoredBaselineICs runs right after attachPropertyICState, before any JS: it compares the snapshot
// with postconditions I3 and I4, then captures the CB with strict on and compares each record with the recapture of the
// record it was restored from (I5). It reads only the section and a CB no other thread reaches before installCode, so it
// never skips itself, in either JIT mode.

namespace JSC {
class CallFrame;
class CodeBlock;
class JSCell;
class JSGlobalObject;
} // namespace JSC

namespace JSC::JITCache::ICs {

struct PropertyICSnapshot {
    AccessType accessType;
    unsigned bytecodeIndex; // codeOrigin's bytecode offset
    CacheType cacheType;
    PropertyInlineCacheSummary summary; // PropertyInlineCache::summary under the CB lock
    bool holdsGaveUp; // m_slowOperation == gaveUpOperationFor(accessType)
    uint8_t caseCount;
    bool megamorphicCaseListed;
    bool everConsidered;
    bool sawNonCell;
    bool tookSlowPath;
    bool resetByGC;
    bool canBeMegamorphic;
    uint8_t countdown;
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;
    friend bool operator==(const PropertyICSnapshot&, const PropertyICSnapshot&) = default;
};

struct CallLinkSnapshot {
    OpcodeID opcodeID;
    unsigned metadataID;
    CallLinkInfo::Mode mode;
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    bool hasStub;
    bool hasLastSeenCallee;
    friend bool operator==(const CallLinkSnapshot&, const CallLinkSnapshot&) = default;
};

struct SuperConstructSnapshot {
    OpcodeID opcodeID; // op_super_construct or op_super_construct_varargs
    unsigned metadataID;
    enum class State : uint8_t { Empty, Single, Multiple } state;
    // The cell when Single, null otherwise. Not a root: the cache is weak, so a holder keeps the pointer valid only by
    // keeping every collection from its End phase, as functionSnapshotBaselineICs does with DeferGC.
    JSCell* cachedCallee;
    friend bool operator==(const SuperConstructSnapshot&, const SuperConstructSnapshot&) = default;
};

struct BaselineICsSnapshot {
    JITType jitType;
    Vector<PropertyICSnapshot> propertyICs; // IC order; empty unless the CB has BaselineJITData
    Vector<CallLinkSnapshot> callLinks; // canonical order (forEachCallLinkSite)
    Vector<SuperConstructSnapshot> superConstructs; // forEachMetadataEntry over both opcodes
    friend bool operator==(const BaselineICsSnapshot&, const BaselineICsSnapshot&) = default;
};

// One difference between the restored state and its twin. index is the IC index for PropertyIC, the call-link site
// index in canonical order for CallLink, and the site the failed check names, or 0, for Capture. field names what
// differs; expected and actual are its values, with booleans as 0 and 1 and enumerations by their numeric value.
struct TwinMismatch {
    enum class Site : uint8_t { PropertyIC, CallLink, Capture } site;
    uint32_t index;
    ASCIILiteral field;
    uint32_t expected;
    uint32_t actual;
};

// Takes CodeBlock::m_lock, which the caller must not hold, and releases it before returning. Allocates no cell.
BaselineICsSnapshot snapshotBaselineICs(CodeBlock&);

// Right after attachPropertyICState returns, on the CB prepareBaselineICs prepared, before any JS. Takes CodeBlock::m_lock
// through snapshotBaselineICs and then captureBaselineICs, never both at once. Allocates no cell.
Vector<TwinMismatch> checkRestoredBaselineICs(const PreparedBaselineICs&, CodeBlock&);

// The jsc shell's jitcacheICsSnapshot(fn, kind), registered by the integrator (R-INT-7).
JSC_DECLARE_HOST_FUNCTION(functionSnapshotBaselineICs);

} // namespace JSC::JITCache::ICs

#endif // ENABLE(JITCACHE_TWINS) && ENABLE(JIT)
