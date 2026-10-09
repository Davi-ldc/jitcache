#pragma once

#if ENABLE(JIT)

#include "ImageTypes.h"
#include "JumpTable.h"
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/CodePtr.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

// How a target becomes an address (SPEC-image.md sections 3.4 to 3.8). The producer's helpers compute every value they
// emit through resolveTarget and resolveSupport, and the consumer resolves every fixup through the same two functions, so
// the emitted value and the consumer's resolution cannot diverge.

namespace JSC {

class JSCell;
class UnlinkedCodeBlock;
class VM;

namespace JITCache {

// A C++ function (an operation or a slow-path function) as its offset from codeSymbolAnchor (SPEC-image.md section 3.5).
// The header's build ID covers the object holding the anchor (R-INT-8), so equal builds give equal offsets. The text
// segment is the executable PT_LOAD segment of that object, found once per process and cached.
struct CodeSymbol {
    int64_t offset { 0 };

    static std::optional<CodeSymbol> of(const void* function); // nullopt outside the engine's text segment
    template<typename Function> requires std::is_function_v<Function>
    static std::optional<CodeSymbol> of(Function* function) { return of(std::bit_cast<const void*>(function)); }

    const void* address() const; // RELEASE_ASSERTs isValid
    static bool isValid(int64_t offset); // inside the text segment, instruction-aligned (4 bytes on ARM64)

    friend bool operator==(const CodeSymbol&, const CodeSymbol&) = default;
};

// The function CodeSymbol offsets are measured from. It does nothing and is never called; the integrator also finds the
// engine object by its address (SPEC-integrator.md section 5.2).
void codeSymbolAnchor();

// The tables of SPEC-image.md section 3.6, both ways. vmAddress and vmCell take a valid entry.
const void* vmAddress(VM&, VMAddress);
std::optional<VMAddress> vmAddressFor(VM&, const void* address); // nullopt outside table 3.6
JSCell* vmCell(VM&, VMCell);

// The ranges of the target fields that name an enumeration (table 3.3), which strict checks (V3) before any resolution.
bool isValidCommonThunk(uint32_t);
bool isValidBaselineThunk(uint32_t); // ConsistencyCheck only in ASSERT_ENABLED builds, the only ones with its generator
bool isValidAccessType(uint32_t);
bool isValidCallMode(uint32_t);
bool isValidProcessThunk(uint32_t);
bool isValidVMAddress(uint32_t);
bool isValidVMCell(uint32_t);

// Whether JIT::emit_op_switch_string gives a string table with this many keys its inline tree of atom comparisons:
// 1 to maximumInlineStringSwitchCaseCount keys, an option options.md fixes.
bool hasInlineStringSwitch(size_t keyCount);

// The ranks of the inline string switches (SPEC-image.md section 3.7).
struct StringSwitchRank {
    UniquedStringImpl* key { nullptr }; // a key of the UCB's unlinked string table, an atom
    unsigned indexInTable { 0 }; // its OffsetLocation::m_indexInTable
};
struct StringSwitchRanks {
    // Indexed by string table. For a table hasInlineStringSwitch accepts, every key sorted by signed address, the order
    // BinarySwitch sorts its IntPtr cases in, so element r is rank r; empty for any other table.
    Vector<Vector<StringSwitchRank>> tables;
};
StringSwitchRanks rankStringSwitches(const UnlinkedCodeBlock&);

// What resolveTarget reads besides the VM. Each span is indexed as its target's a: mathICs and snippetStarts by MathIC
// index (a snippet start is null for a MathIC without one), switchTableBases by simple switch table, stringSwitchTables
// by string switch table.
struct ResolutionContext {
    VM& vm;
    // UCB targets; null when resolving support or VM data only. Not const: the arithmetic profile accessors the UCB
    // targets read have no const overload.
    UnlinkedCodeBlock* ucb { nullptr };
    // Artifact targets; null or empty when resolving support or data only.
    const void* imageStart { nullptr };
    std::span<void* const> mathICs;
    std::span<const void* const> switchTableBases;
    std::span<const StringJumpTable> stringSwitchTables; // with their code locations filled (section 3.7)
    std::span<const void* const> snippetStarts;
    const StringSwitchRanks* ranks { nullptr };
};

// The value a valid target resolves to: what the image holds at the fixup, an address or a branch target. The producer's
// recording makes every target valid and strict checks it before anything resolves, so normal mode resolves without
// checking; debug builds ASSERT the conditions of table 3.3. Resolving a support target may generate the support code
// through the native lookup, as the VM's first use of it would: on the VM thread, with must-succeed allocation.
const void* resolveTarget(const ResolutionContext&, const ImageTarget&);

// A support target (Operation, CommonThunk, BaselineThunk, SlowPathThunk, InlineCacheSlowPathThunk, VirtualCallThunk or
// ProcessThunk) through the native lookup its key names.
CodePtr<NoPtrTag> resolveSupport(VM&, const ImageTarget&);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
