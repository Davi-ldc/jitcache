#pragma once

#include "CachedTypes.h"
#include "ParserModes.h"
#include "UCBKeys.h"
#include <optional>
#include <span>

namespace JSC {

class JSString;
class UnlinkedCodeBlock;
class UnlinkedFunctionExecutable;
class VM;

} // namespace JSC

namespace JSC::JITCache {

// The ucb.identity section, the constant-map checks C8 and C10 and the atomization of section 7.3.3 (SPEC-ucb.md section 4).
// UnlinkedCodeBlockCoreKind is the codec's (SPEC-ucb.codec.md, section 2).

struct FunctionParseFields {
    CodeFeatures features;
    LexicallyScopedFeatures lexicallyScopedFeatures;
    bool hasCapturedVariables;
};

struct IdentitySection { // a parsed ucb.identity section; the two maps borrow the section's bytes
    UnlinkedCodeBlockCoreKind coreKind;
    CoreProvenance provenance;
    BodyKey key;
    Digest256 contextDigest;
    Digest256 coreDigest;
    std::optional<Digest256> holderDigest; // core kind Function only
    std::optional<FunctionParseFields> functionParseFields; // core kind Function only
    uint32_t constantCount;
    std::span<const uint8_t> atomMap;
    std::span<const uint8_t> butterflyMap;
};

// With strict, empty when the section breaks a rule of section 4.2. Without, never empty: it reads each field where the
// layout puts it and trusts it (THREAD Session).
std::optional<IdentitySection> parseIdentitySection(std::span<const uint8_t>, bool strict);
UnlinkedCodeBlockCoreKind coreKindFor(IdentityKind);
bool constantMapsFit(UnlinkedCodeBlock&, const IdentitySection&); // C8; allocates nothing
bool markedConstantsAreAtoms(UnlinkedCodeBlock&, const IdentitySection&); // C10; allocates nothing
// SHA-256 of the bytes encodeUnlinkedCodeBlockCore would return for the UCB and holder, fed page by page through
// forEachUnlinkedCodeBlockCoreChunk (SPEC-ucb.codec.md, E13), so no payload is assembled. `holder` is the UFE whose slot
// holds a function UCB, and null for any other (codec E14). Matching (section 7.3.2). VM thread; allocates no cell.
Digest256 coreDigestOf(VM&, const UnlinkedCodeBlock&, const UnlinkedFunctionExecutable* holder);
// Section 7.3.3 step 5, on an unpublished UCB matched to the body, which passed C8 with strict on and is trusted to fit it
// otherwise: atomizeStringConstant on each constant the atom map marks.
void atomizeMarkedConstants(VM&, UnlinkedCodeBlock&, const IdentitySection&);
// Makes a resolved JSString's value its atom and sets its isDefinitelyAtom bit, as JSString::toAtomString does:
// existingAtomOrNull() when the value is already an atom (it calls markAsAtom), otherwise AtomStringImpl::add and then
// JSString::swapToAtomString unconditionally, which ends in markAsAtom even when the add made the StringImpl itself the
// atom. Value profiling reads that bit (speculationFromCell gives SpecStringIdent only for a marked cell). VM thread;
// allocates no cell (F19). runtime/JSString.h declares it a friend, for the private swapToAtomString.
void atomizeStringConstant(VM&, const JSString&);
// Section 7.3.1 step 8, on an unpublished imported UCB, which passed C8 with strict on and is trusted to fit its body
// otherwise; the caller holds a DeferGC. For each constant the butterfly map marks, as ArrayNode::emitBytecode builds one
// (F23): JSCellButterfly::tryCreate with vm.cellButterflyOnlyAtomStringsStructure and the decoded butterfly's length; for
// each element, atomizeStringConstant, then setIndex with vm.atomStringToJSStringMap.ensureValue(atom, [&] { return
// element; }), the VM's canonical JSString for that atom; then the constant register takes the new butterfly through
// UnlinkedCodeBlock::constantRegister, with a write barrier on the UCB. VM thread; allocates one cell per marked constant,
// and a failed tryCreate crashes, as it does in generation.
void rebuildAtomStringButterflies(VM&, UnlinkedCodeBlock&, const IdentitySection&);
size_t identitySectionSize(uint32_t constantCount);
// Fills exactly identitySectionSize(constant count of the UCB) bytes: the atom map from the UCB's constants, and the
// butterfly map from them too unless keptButterflyMap, a record's kept map of ceil(N / 8) bytes, replaces it (section 8.2).
void writeIdentitySection(std::span<uint8_t> out, VM&, const BodyKey&, const Digest256& contextDigest, CoreProvenance, const Digest256& coreDigest,
    const std::optional<Digest256>& holderDigest, UnlinkedCodeBlockCoreKind, const std::optional<FunctionParseFields>&, UnlinkedCodeBlock&,
    std::optional<std::span<const uint8_t>> keptButterflyMap = std::nullopt);

} // namespace JSC::JITCache
