#pragma once

#include "CachedTypes.h"
#include "ParserModes.h"
#include "jitcache/ucb/UCBKeys.h"
#include <optional>
#include <span>

namespace JSC {

class JSString;
class UnlinkedCodeBlock;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The ucb.identity section and the rebuild of the butterflies generation built from atom strings.
// UnlinkedCodeBlockCoreKind is the codec's (CachedTypes.h).

struct FunctionParseFields {
    CodeFeatures features;
    LexicallyScopedFeatures lexicallyScopedFeatures;
    bool hasCapturedVariables;
};

struct IdentitySection { // a parsed ucb.identity section; the spans borrow the section's bytes
    UnlinkedCodeBlockCoreKind coreKind;
    BodyKey key;
    Digest256 contextDigest;
    std::optional<Digest256> holderDigest; // core kind Function only
    std::optional<FunctionParseFields> functionParseFields; // core kind Function only
    uint32_t constantCount;
    std::span<const uint8_t> butterflyMap;
    // The child holder digests, 32 bytes each; empty when the body lists none. A trailing partial digest is dropped.
    std::span<const uint8_t> childHolderDigests;

    size_t childHolderDigestCount() const { return childHolderDigests.size() / sizeof(Digest256); }
};

// Reads each field where the layout puts it and trusts it.
IdentitySection parseIdentitySection(std::span<const uint8_t>);
UnlinkedCodeBlockCoreKind coreKindFor(IdentityKind);
// Makes a resolved JSString's value its atom and sets its isDefinitelyAtom bit, as JSString::toAtomString does:
// existingAtomOrNull() when the value is already an atom (it calls markAsAtom), otherwise AtomStringImpl::add and then
// JSString::swapToAtomString unconditionally, which ends in markAsAtom even when the add made the StringImpl itself the
// atom. Value profiling reads that bit (speculationFromCell gives SpecStringIdent only for a marked cell). VM thread;
// allocates no cell. runtime/JSString.h declares it a friend, for the private swapToAtomString.
void atomizeStringConstant(VM&, const JSString&);
// On an unpublished imported UCB, which is trusted to fit its body; the caller holds a DeferGC. For each constant the
// butterfly map marks, as ArrayNode::emitBytecode builds one: JSCellButterfly::tryCreate with
// vm.cellButterflyOnlyAtomStringsStructure and the decoded butterfly's length; for each element, atomizeStringConstant,
// then setIndex with vm.atomStringToJSStringMap.ensureValue(atom, [&] { return element; }), the VM's canonical JSString
// for that atom; then the constant register takes the new butterfly through UnlinkedCodeBlock::constantRegister, with
// a write barrier on the UCB. VM thread; allocates one cell per marked constant, and a failed tryCreate crashes, as it
// does in generation.
void rebuildAtomStringButterflies(VM&, UnlinkedCodeBlock&, const IdentitySection&);
size_t identitySectionSize(uint32_t constantCount, size_t childHolderDigestCount = 0);
// Fills exactly identitySectionSize(constant count of the UCB, childHolderDigests.size()) bytes: the butterfly map from
// the UCB's constants, then the child holder digests.
void writeIdentitySection(std::span<uint8_t> out, VM&, const BodyKey&, const Digest256& contextDigest, const std::optional<Digest256>& holderDigest,
    UnlinkedCodeBlockCoreKind, const std::optional<FunctionParseFields>&, UnlinkedCodeBlock&, std::span<const Digest256> childHolderDigests = { });

} // namespace JSC::JITCache
