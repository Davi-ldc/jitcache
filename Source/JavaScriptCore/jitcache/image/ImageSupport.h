#pragma once

#if ENABLE(JIT)

#include "JumpTable.h"
#include "jitcache/base/EngineObject.h"
#include "jitcache/base/VMPart.h"
#include "jitcache/image/ImageTypes.h"
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <type_traits>
#include <wtf/CodePtr.h>
#include <wtf/HashMap.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/UniquedStringImpl.h>

// How a target becomes an address. The producer's helpers compute every value they emit through resolveTarget and
// resolveSupport, and the consumer resolves every fixup through the same two functions, so the emitted value and the
// consumer's resolution cannot diverge. The Image lane's emitters, capture, preparation and twin check include this
// header, and so does the install glue, which keeps each VM's SupportTargetTable.

namespace JSC {

class JSCell;
class UnlinkedCodeBlock;
class VM;

namespace JITCache {

// A C++ function (an operation or a slow-path function) as its offset from codeSymbolAnchor. The artifact header's build
// ID covers the object holding the anchor, so equal builds give equal offsets. The text segment is that object's
// executable PT_LOAD segment that holds the anchor (EngineObject.h).
struct CodeSymbol {
    int64_t offset { 0 };

    static std::optional<CodeSymbol> of(const void* function); // nullopt outside the engine's text segment
    template<typename Function> requires std::is_function_v<Function>
    static std::optional<CodeSymbol> of(Function* function) { return of(std::bit_cast<const void*>(function)); }

    const void* address() const; // RELEASE_ASSERTs isValid
    static bool isValid(int64_t offset); // inside the text segment, instruction-aligned (4 bytes on ARM64)

    friend bool operator==(const CodeSymbol&, const CodeSymbol&) = default;
};

// The VM data and cells an image may name, both ways. vmAddress and vmCell take a valid entry.
const void* vmAddress(VM&, VMAddress);
std::optional<VMAddress> vmAddressFor(VM&, const void* address); // nullopt for an address no VMAddress names
JSCell* vmCell(VM&, VMCell);

// The ranges of the target fields that name an enumeration, which resolution asserts before it reads the field.
bool isValidCommonThunk(uint32_t);
bool isValidBaselineThunk(uint32_t); // ConsistencyCheck only in ASSERT_ENABLED builds, the only ones with its generator
bool isValidAccessType(uint32_t);
bool isValidCallMode(uint32_t);
bool isValidVMAddress(uint32_t);
bool isValidVMCell(uint32_t);

// Whether JIT::emit_op_switch_string gives a string table with this many keys its inline tree of atom comparisons:
// 1 to maximumInlineStringSwitchCaseCount keys, an option JITCache's start requires at its default.
bool hasInlineStringSwitch(size_t keyCount);

// The ranks of the inline string switches: the consumer writes its own atom at each recorded rank of the producer's
// tree.
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

// The support targets a VM's preparations resolved: the baseline thunks, the property ICs' slow-path thunks by access
// type and the slow-path function thunks, whose native lookups each take JITThunks' lock, a hash lookup and a
// reference. JITThunks keeps every thunk it hands out for the VM's life, so a kept address stays valid. The other kinds
// resolve without a lock: a process address, an array element or a lazily created entry. The install glue keeps one
// per VM, as a part of the VM's state (VMPart.h), and passes it to prepareImage. VM thread.
class SupportTargetTable final : public VMPart {
    WTF_MAKE_NONCOPYABLE(SupportTargetTable);
    WTF_MAKE_TZONE_ALLOCATED(SupportTargetTable);
public:
    static constexpr VMPartLifetime vmPartLifetime = VMPartLifetime::UntilVMDestruction;

    SupportTargetTable() = default;

    // As resolveSupport; the first resolution of a kept kind fills its slot.
    CodePtr<NoPtrTag> resolve(VM&, const ImageTarget&);

private:
    // One slot per access type. The engine counts them in PropertyInlineCache.h, too heavy a header for the engine
    // files that include this one, so the count is written here and resolve asserts that it equals the engine's.
    static constexpr size_t numberOfAccessTypeSlots = 30;

    std::array<CodePtr<NoPtrTag>, static_cast<size_t>(BaselineThunk::ConsistencyCheck) + 1> m_baselineThunks { };
    std::array<CodePtr<NoPtrTag>, numberOfAccessTypeSlots> m_inlineCacheSlowPathThunks { };
    HashMap<uintptr_t, CodePtr<NoPtrTag>> m_slowPathThunks; // by the slow-path function's address
};

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
    std::span<const StringJumpTable> stringSwitchTables; // with their code locations filled
    std::span<const void* const> snippetStarts;
    const StringSwitchRanks* ranks { nullptr };
    // The VM's table of resolved support targets, which saves the native lookup after a target's first resolution; null
    // resolves each through resolveSupport. Only prepareImage, on the VM thread, passes one.
    SupportTargetTable* supportTargets { nullptr };
};

// The value a valid target resolves to: what the image holds at the fixup, an address or a branch target. The producer's
// recording makes every target valid, so resolution does not check it; debug builds ASSERT that each field the kind uses
// is in range. Resolving a support target may generate the support code through the native lookup, as the VM's first
// use of it would: on the VM thread, with must-succeed allocation.
const void* resolveTarget(const ResolutionContext&, const ImageTarget&);

// A support target (Operation, CommonThunk, BaselineThunk, SlowPathThunk, InlineCacheSlowPathThunk, VirtualCallThunk or
// ProcessThunk) through the native lookup its key names.
CodePtr<NoPtrTag> resolveSupport(VM&, const ImageTarget&);

} // namespace JITCache
} // namespace JSC

#endif // ENABLE(JIT)
