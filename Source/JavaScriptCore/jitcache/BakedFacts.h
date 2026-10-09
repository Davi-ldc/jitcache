#pragma once

#if ENABLE(JIT)

#include "DFGCommon.h"
#include "GetPutInfo.h"
#include "ImageTypes.h"
#include <expected>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

// The facts a baseline compilation read from its CB and turned into unguarded code: the capability class, the taint,
// and the kind and depth of every unguarded scope access (SPEC-image.md section 7), with the codec of the
// baked-facts.baseline section (section 8.3) and the comparison an install runs on a newborn CB.

namespace JSC {
class CodeBlock;
}

namespace JSC::JITCache {

class ImageSectionsView;

// The opcode byte of a scope fact.
enum class ScopeOpcode : uint8_t {
    ResolveScope = 1,
    GetFromScope = 2,
    PutToScope = 3,
};

struct ScopeFact {
    uint32_t bytecodeOffset { 0 };
    ScopeOpcode opcode { };
    ResolveType resolveType { GlobalProperty };
    uint32_t localScopeDepth { 0 }; // resolve_scope with ClosureVar only, else 0

    friend bool operator==(const ScopeFact&, const ScopeFact&) = default;
};

// The resolve types each opcode's emitter bakes without a guard, the only ones recording writes (section 7):
// resolve_scope ModuleVar, ClosureVar and ClosureVarWithVarInjectionChecks; get_from_scope ClosureVar; put_to_scope
// ClosureVar, ResolvedClosureVar and ClosureVarWithVarInjectionChecks from the main pass and ModuleVar from the slow pass.
bool isBakedScopeShape(ScopeOpcode, ResolveType);

struct BakedFacts {
    DFG::CapabilityLevel capabilityLevel { DFG::CapabilityLevelNotSet }; // as read by the compilation
    bool couldBeTainted { false };
    Vector<ScopeFact> scopeFacts; // sorted by bytecodeOffset, one per instruction at most

    friend bool operator==(const BakedFacts&, const BakedFacts&) = default;
};

// The recorder's builder, filled on the compiling thread through ImageRecorder::bakedFacts().
class BakedFactsBuilder {
    WTF_MAKE_NONCOPYABLE(BakedFactsBuilder);
public:
    // Asked before the builder stores anything: with bytes 0 for the code-block facts, which live in the builder, and
    // with the bytes of new capacity before the fact vector grows, which the allowance charges first. Once it refuses,
    // the builder stores nothing more and keeps what it holds.
    class StorageAllowance {
    public:
        virtual bool allowStorage(size_t bytes) = 0;

    protected:
        ~StorageAllowance() = default;
    };

    explicit BakedFactsBuilder(StorageAllowance* = nullptr); // null: every request is allowed

    void setCodeBlockFacts(DFG::CapabilityLevel, bool couldBeTainted);
    void addScopeFact(ScopeFact); // in any order: the slow pass adds put_to_scope's ModuleVar fact after the main pass
    BakedFacts finish(); // sorts the facts by bytecode offset and holds them at their exact size; step 5 of section 4.7

private:
    StorageAllowance* m_allowance;
    BakedFacts m_facts;
};

// The baked-facts.baseline section (SPEC-image.md section 8.3): an 8-byte header (u8 capability level, u8 taint, u16
// reserved, u32 scopeFactCount), then scopeFactCount entries of 12 bytes (u32 bytecode offset, u8 opcode, u8 resolve
// type, u16 reserved, u32 local scope depth), sorted by bytecode offset. Little-endian, read with memcpy-based loads.
inline constexpr size_t bakedFactsHeaderSize = 8;
inline constexpr size_t bakedFactsEntrySize = 12;

size_t bakedFactsSectionSize(const BakedFacts&);
// Streams the section through the sink with no buffer of its own; false once the sink refuses.
[[nodiscard]] bool writeBakedFactsSection(const BakedFacts&, const ImageSectionSink&);

// A located baked-facts section: a span into the borrowed payload, read on access.
class BakedFactsView {
public:
    DFG::CapabilityLevel capabilityLevel() const { return m_capabilityLevel; }
    bool couldBeTainted() const { return m_couldBeTainted; }
    uint32_t scopeFactCount() const { return m_scopeFactCount; }
    ScopeFact scopeFact(unsigned index) const;

    std::span<const uint8_t> bytes() const { return m_bytes; } // the whole section

    friend bool operator==(const BakedFactsView&, const BakedFactsView&);

private:
    friend std::expected<BakedFactsView, ImageCheck> parseBakedFactsSection(std::span<const uint8_t>, bool strict);
    friend class ImageSectionsView; // which holds one, located after its image section
    BakedFactsView() = default;

    std::span<const uint8_t> m_bytes;
    DFG::CapabilityLevel m_capabilityLevel { DFG::CapabilityLevelNotSet };
    bool m_couldBeTainted { false };
    uint32_t m_scopeFactCount { 0 };
};

// Locates the section. A section shorter than its header or its counted entries cannot be located and fails V7 in either
// mode. Under strict it runs V7 (SPEC-image.md section 8.5); with strict off, debug builds ASSERT what V7 checks.
std::expected<BakedFactsView, ImageCheck> parseBakedFactsSection(std::span<const uint8_t>, bool strict);

enum class BakedFactsResult : uint8_t {
    Match,
    Mismatch,
};

// The comparison of SPEC-image.md section 7, on the VM thread, on a linked CB that has not run. It writes nothing to
// the CB: its capability class comes from capabilityLevelState() when set and otherwise from computeCapabilityLevel(),
// which does not memoize. The class matches when both sides agree on DFG::CannotCompile, the one test the code depends
// on (N13); the taint matches when couldBeTainted() equals the recorded value; a scope fact matches when the CB's
// metadata for its instruction holds the same resolve type, and for a resolve_scope of type ClosureVar the same depth.
// Every fact names an instruction of its own opcode, which recording guarantees and U4 checks under strict. The install
// glue only branches on the result, so nothing reports which fact differed.
BakedFactsResult compareBakedFacts(const ImageSectionsView&, CodeBlock& newborn);

} // namespace JSC::JITCache

#endif // ENABLE(JIT)
