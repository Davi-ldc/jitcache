# SPEC-ics: ICs and call links

Lane 4 of THREAD's Execution. This file is the lane's whole design and is binding. [SPEC-ics-history.md](SPEC-ics-history.md) records the path to each decision that is not obvious and binds nothing. Code lives in `Source/JavaScriptCore/jitcache/ics/`, namespace `JSC::JITCache::ICs`; test-only code is guarded by `ENABLE(JITCACHE_TWINS)`.

## 1. Scope

This version covers the baseline tier. When DFG and FTL arrive, the lane also owns the data-driven IC learning and call links of every tier (THREAD Execution), which is why the format has one section per tier (section 4). The lane owns:

1. One body-file section per tier, `ICsBaseline` here, holding the property-IC learning state and call-link history of the baseline CB, a call site's `this` profile included (THREAD Execution): a record per property IC and a record per call-link site of its metadata.
2. Capture of that section from a live baseline CB, with the summary's count of IC sites with cases (THREAD Capture) and the bit that keeps a body with a polymorphic site from carrying baseline counter progress, which it hands to the CB lane (THREAD Restoration; section 5.3).
3. Restoration as THREAD Caches gives it: seeding of call-link history after linking, which gives a call site its own `CallSiteData` where it holds something and restores the site's `this` profile into it, and attachment of property-IC learning state after native setup, with the section validated first when strict is on.
4. The `super_construct` store in `JIT::compileOpCall`: the native fix to the baseline `super_construct` and `super_construct_varargs` templates (THREAD Execution and Native fixes; section 7, E1).
5. The executable-allocation fault calls at the three `didFailToAllocate()` branches of `InlineCacheCompiler` (THREAD Execution; section 7, E5), and three small native additions the lane needs (section 7, E2 to E4).

Outside the lane: the IC molds, `doneLocation`s and every other byte of the image (Image lane); the LLInt property caches, `m_cachedCallee`, `m_cachedStructureID` and the scope metadata, which THREAD leaves "as native linking leaves them" (no lane seeds them); value, array and arithmetic profiles (CB and UCB lanes), except a call site's `this` profile, which lives in the site's `CallSiteData` (section 3.2) and whose flags the CB lane's summary only reads (R-CB-1); the container, directory, writer, scoring, fault plumbing and entry points (integrator).

THREAD's omissions apply: property-IC cases, handlers, stubs and the inline mirror never travel (IC cases), nor do callees, polymorphic stubs, their slot counts and `lastSeenCallee` (callee distributions). Restoration emits no IC code (THREAD Caches).

## 2. Design in brief

Capture writes one record per property IC and one per call-link site, holding raw producer facts in native index order (sections 4.2 and 4.3). One pure derivation per record kind turns a record into the consumer's state; seeding, attach and the twin check all apply it. A call site comes back as the producer's last execution left it, ready to link at its next call: with its own `CallSiteData` when it holds history, a virtual mode or a `this` profile with observations, which that data then holds, as does a tail call that ran; on the VM's executed-once data when it ran and holds neither; and as linking left it when it never ran or a reset left it with nothing learned (section 6.1; [history](SPEC-ics-history.md#a-call-site-gets-its-own-data-only-when-it-holds-something)). A recapture function gives the record a capture of the restored state yields, so a recapture of an imported body carries every fact it imported, a fold not yet replayed included (I5, section 6.1). Capture also hands the CB lane the polymorphic bit (section 5.3).

## 3. State census

Every field of the native objects this lane covers, with its fate in the consumer. "Installation" means `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` called from `CodeBlock::setupWithUnlinkedBaselineCode`; "linking" means `link_callLinkInfo` in the link pass of `CodeBlock::finishCreation`, which calls `DataOnlyCallLinkInfo::initialize` on a varargs entry and, under the fixed `useLazyLLIntCallLinkInfos` (section 10), `LazyCallLinkInfo::setTailCallNotExecuted` on an `op_tail_call` entry and `setNeverExecuted` on the other lazy entries. A value "restored" is the derivation's (section 6.1).

### 3.1 `PropertyInlineCache` and `HandlerPropertyInlineCache`

| field | group | value in the restored IC | source of the rule |
|---|---|---|---|
| `accessType` | identity | the mold's; the record repeats it so prepare can check the pairing | Image lane owns molds |
| `propertyIsInt32` | identity | the mold's | installation |
| `propertyIsString`, `propertyIsSymbol`, `prototypeIsKnownObject` | identity | false, as baseline installation leaves them; only the DFG's initializer copies them | installation |
| `canBeMegamorphic` | identity | restored | THREAD Caches |
| `preconfiguredCacheType` | shape | the mold's | installation |
| `m_cacheType` | shape | as installation leaves it: `Unset`, or `ArrayLength` when the mold's shape is `ArrayLength` ([history](SPEC-ics-history.md#the-shape-group-stays-as-installation-builds-it)) | Restoration emits no IC code |
| `m_handler` | dispatch | the bare slow-path handler installation builds | cases never travel |
| `m_slowOperation` | dispatch | restored | THREAD Caches |
| `m_inlinedHandler`, `m_inlineAccessBaseStructureID`, `byIdSelfOffset`, `m_inlineHolder` | inline mirror | empty or inert, as installation leaves them | cells |
| `countdown`, `repatchCount`, `everConsidered`, `sawNonCell`, `tookSlowPath` | learning | restored | THREAD Caches |
| `numberOfCoolDowns` | learning | restored | THREAD Caches and Restoration (IC cool-down counts) |
| `resetByGC` | learning | restored ([history](SPEC-ics-history.md#resetbygc-travels)) | THREAD "What travels and what stays local": everything else the baseline phase learned travels |
| `codeOrigin`, `callSiteIndex`, `m_identifier`, `m_globalObject`, `m_icType` | per-CB link data | installation | installation |

`RepatchingPropertyInlineCache` exists only in the FTL (`useHandlerICInFTL` is forced off); its fields belong to the FTL capture of a later version.

### 3.2 Call-link sites

The fourteen opcodes of `FOR_EACH_OPCODE_WITH_CALL_LINK_INFO` (section 4.2) hold their call link in one of two shapes (`bytecode/CallLinkInfo.h`):

- The four varargs opcodes (`OpCallVarargs`, `OpTailCallVarargs`, `OpConstructVarargs`, `OpSuperConstructVarargs`) hold a `DataOnlyCallLinkInfo` in the metadata entry, which linking initializes. These are the *inline* sites.
- The other ten, `FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO`, hold a `LazyCallLinkInfo`: one pointer, `m_data`, to a `CallSiteData`, which is a `DataOnlyCallLinkInfo` followed by an `ArrayProfile`. For `OpCall`, `OpCallIgnoreResult` and `OpTailCall` that profile is the `this` profile, which `CodeBlock::getArrayProfile` reaches through `LazyCallLinkInfo::arrayProfile()` (the file-local `arrayProfileFor` in `bytecode/CodeBlock.cpp` takes this route for a `Metadata` without an `m_arrayProfile`) and the DFG parser reads at array intrinsics such as `push` (`ByteCodeParser::getArrayMode`); for a site without data of its own the parser finds no profile and does not apply the intrinsic. The other seven never write theirs. These are the *lazy* sites. The pointer starts at one of three `CallSiteData`s the VM shares (`VM::neverExecutedCallSiteData`, `executedOnceCallSiteData` and `notExecutedTailCallSiteData`), whose `CallLinkInfo` has no owner, has `isSharedByUnlinkedCallSites` set and never changes, since `setSeen`, `clearSeen`, `reset`, `setVirtualCall` and `reconcileWeakReferencesAtGCEnd` release-assert against it. `LazyCallLinkInfo::get()` and `arrayProfile()` answer null until the site has a `CallSiteData` of its own.

A lazy site moves through these states natively, on the VM's thread:

1. Linking points it at the never-executed data, or at the not-executed tail-call data for `op_tail_call`.
2. At its first call, `LLInt::handleUnlinkedCall` moves it to the executed-once data, whose seen bit is set, and calls the callee without linking. Callers in both tiers reach that function through `operationUnlinkedCall`, which the shared data's thunk calls whenever the JIT is on, and the fixed `useJIT` keeps it on; `llint_unlinked_call` serves only runs without the JIT (`LLInt::unlinkedCall` picks between the two). `LazyCallLinkInfo::hasExecutedOnce()` then answers true.
3. At its second call, `handleUnlinkedCall` gives it its own `CallSiteData` through `LazyCallLinkInfo::ensure`, notes the `this` structure in the new profile and calls `linkFor`, which finds the seen bit set and links the site. `LazyCallLinkInfo::ensureSlow` initializes the new `CallLinkInfo` as linking initializes an inline one, sets its seen bit when the site was executed once, publishes it after a `storeStoreFence`, and reports every 32nd such allocation of the metadata table to the heap (`MetadataTable::callSiteDatasPerReport`).
4. Three paths give a site its own data before the unlinked-call path can, all through `CodeBlock::ensureCallLinkInfoAt`: a tail call before its first call, because that path finds the site through the caller's frame, which a tail call gives up (`prepareCallSiteForTailCall` in the LLInt, which calls `slow_path_ensure_call_link_info`, and `operationEnsureCallLinkInfoForTailCall` from the tail-call path of `CallLinkInfo::emitFastPathImpl`); and a direct eval whose callee is not eval, in baseline code before its virtual call (`operationEnsureCallLinkInfo` from `JIT::compileCallDirectEvalSlowCase`). That data starts unseen; a tail call's first trip through `linkFor`, right after, sets the bit.

A site keeps its own data for the metadata's life (`LazyCallLinkInfo::~LazyCallLinkInfo` frees it), and from then on its `CallLinkInfo` behaves as an inline one.

The `this` profile accumulates like any array profile. The LLInt, and baseline code that emits profiling, store the structure of a cell `this` as a pending sample (`ArrayProfile::m_lastSeenStructureID`) into whatever data the site points at, so a site on shared data writes into a profile nothing reads; `handleUnlinkedCall` stores it into the site's new data at its second call. A drain (`ArrayProfile::computeUpdatedPrediction`, from `CodeBlock::updateAllArrayProfilePredictions` or the DFG parser, without a lock) folds the sample into the observed array modes and the flags. Once the site has data of its own, each `updateAllArrayProfilePredictions` also merges the profile in both directions with its copy in the UCB (`UnlinkedArrayProfile::update`), except in a builtin: `UnlinkedCodeBlock::ensureValueAndArrayProfiles` returns early when `isBuiltinFunction()` holds, so a builtin's UCB keeps no copies, and `updateAllArrayProfilePredictions` merges nothing for it. Bun's internal modules are builtins (Bun's `InternalModuleRegistry` creates them with `createBuiltinExecutable`), so the site's own data is the only place their `this` profiles live, and this lane carries the profile there ([history](SPEC-ics-history.md#the-this-profile-travels-in-the-sites-own-data)).

| field | value in the restored site | source of the rule |
|---|---|---|
| `LazyCallLinkInfo::m_data` | as linking left it, the VM's executed-once data, or a `CallSiteData` of its own made by `CodeBlock::ensureCallLinkInfoAt`, as section 6.1 derives | THREAD Caches |
| `m_mode`, `m_hasSeenShouldRepatch` (`seenOnce()`), `m_hasSeenClosure`, `m_clearedByGC`, `m_clearedByVirtual`, `m_maxArgumentCountIncludingThisForVarargs` of an inline or own `CallLinkInfo` | restored | THREAD Caches |
| `m_callType`, `m_type`, `m_owner`, `m_codeOrigin` | linking, or `ensureSlow` with the values linking would give | linking |
| `m_callee`, `m_codeBlock`, `m_monomorphicCallDestination` | as linking or `ensureSlow` leaves them, or the polymorphic mask and the virtual-call thunk that `setVirtualCall` writes | callees never travel |
| `m_stub`, `m_lastSeenCallee`, the incoming-call list node of `CallLinkInfoBase` | empty | callees never travel |
| `m_observedArrayModes` and `m_arrayProfileFlags` of `CallSiteData::m_arrayProfile` in an own `CallSiteData` | at the three opcodes with a `this` profile, restored through `ArrayProfile::restoreAccumulatedState` (SPEC-cb.md E1); at the other seven, empty as `ensureSlow` leaves them | THREAD Caches |
| `m_lastSeenStructureID` and `m_speculationFailureStructureID` of that profile | empty, as `ensureSlow` leaves them; a pending sample never travels | THREAD Capture |
| the VM's three shared `CallSiteData`s | never written by this lane; their `CallLinkInfo` never changes, and native code writes only their profile's pending sample, which nothing reads | `isSharedByUnlinkedCallSites` |

### 3.3 The `super_construct` cache

`OpSuperConstruct::Metadata::m_cachedCallee` and `OpSuperConstructVarargs::Metadata::m_cachedCallee` hold a cell, so they start as linking leaves them (empty) in every CB, imported or not. The lane changes only how the baseline template fills them (section 7, E1).

## 4. The `ICsBaseline` section

A body file holds at most one `ICsBaseline` section, which describes the baseline CB of the capture; the integrator assigns its type id, at tier 1 (R-INT-1). A later tier's section takes the same type id with its own tier in the body's directory (SPEC-integrator.md section 6.1), so this layout needs no tier field.

### 4.1 Layout

All integers are little-endian; the targets are little-endian and `ICSection.h` asserts `std::endian::native == std::endian::little`. Records are arrays of bytes with alignment 1, and the four header words, the group words and the two `this` words of a call-link record are read and written with `memcpy`, so the lane requires no alignment of the section start.

| offset | size | content |
|---|---|---|
| 0 | 4 | `propertyICCount`: the number of property-IC records |
| 4 | 4 | `icSitesWithCases`: the summary count (section 5.3) |
| 8 | 4 | `callLinkGroupCount`: the number of call-link groups, at most 14 |
| 12 | 4 | `callLinkSiteCount`: the number of call-link records |
| 16 | 8 × groups | call-link groups: `uint32_t opcodeID`, `uint32_t siteCount` |
| then | 8 × `propertyICCount` | `PropertyICRecord`s, record i for IC i |
| then | 10 × `callLinkSiteCount` | `CallLinkRecord`s, group by group |
| then | 0 to 7 | zero bytes up to a multiple of 8 |

The size is `16 + 8·groups + 8·propertyICCount + 10·callLinkSiteCount`, rounded up to a multiple of 8, computed with `CheckedSize`. A body with 12 property ICs, five `call` sites and one `construct` site takes 192 bytes.

### 4.2 Orders

Both record arrays are dense and follow native index spaces, so restore walks them beside the CB's ICs and metadata entries with no lookup ([history](SPEC-ics-history.md#dense-records-in-native-index-spaces)).

Property-IC record i describes IC i, the IC that installation builds from mold i of `BaselineJITCode::m_unlinkedPropertyInlineCaches` and that `BaselineJITData::propertyCache(i)` returns. Mold order is a native index space THREAD lists, and the Image lane preserves it (SPEC-image.md R-ICS-1).

The call-link order is the metadata layout. The canonical opcode list is `FOR_EACH_OPCODE_WITH_CALL_LINK_INFO`: `OpCall`, `OpTailCall`, `OpCallDirectEval`, `OpConstruct`, `OpSuperConstruct`, `OpIteratorOpen`, `OpIteratorNext`, `OpAsyncIteratorOpen`, `OpAsyncIteratorNext`, `OpCallVarargs`, `OpTailCallVarargs`, `OpConstructVarargs`, `OpSuperConstructVarargs`, `OpCallIgnoreResult`. An opcode's canonical position is its index in that list. `callLinkOpcodes` in `ICSection.h` (section 4.4), generated from the macros, is the lane's one table of that order, of each opcode's site shape and of which opcodes have a `this` profile (section 3.2): capture writes a group's `opcodeID` from it, `canonicalCallLinkPosition` searches it, and capture, A4, the derivation, S2, seeding and the twin snapshot take a site's opcode, shape and `this` profile from it, so no other file of the lane spells any of them out. Groups appear only for opcodes whose CB has at least one metadata entry, in increasing canonical position; within a group, records follow metadata ID order, which is the order `MetadataTable::forEach<Op>` visits. `CodeBlock::forEachLLIntOrBaselineCallLinkInfo` visits the inline sites and the lazy sites that have their own data in the same order, and skips the other lazy sites. Record k of a group thus describes the site of that opcode whose metadata ID is k, which is how seeding finds a site's record from its instruction (section 6.3).

A CB can have no metadata table ([history](SPEC-ics-history.md#a-cb-can-have-no-metadata-table)). `UnlinkedMetadataTable::link` returns null when the table's `m_hasMetadata` is clear: generation sets the bit only in `UnlinkedMetadataTable::addEntry` and `addValueProfile`, and `UnlinkedMetadataTable::finalize` clears it when the table's offsets would overflow; the bytecode cache's decoder, which the UCB lane runs for every imported core (SPEC-ucb.codec.md), takes it from the decoding constructors and returns `UnlinkedMetadataTable::empty` for a core encoded without metadata. `CodeBlock::metadataTable()` is then null, for instance for `function id(x) { return x; }` (`enter` and `ret` carry no metadata) or `function has(o) { return "x" in o; }` (`in_by_id` carries none). Such a body can still be hot, be baseline-compiled and have property ICs, because argument profiles live in `CodeBlock::m_argumentValueProfiles` and property ICs in `BaselineJITData`, both outside the table. It has no metadata entry for any opcode, so its section has no group and no call-link record, and since `MetadataTable::forEach` reads the offset table through `this`, the lane walks metadata only through the guarded walkers of section 4.6.

`opcodeID` and `accessType` hold build-specific numeric values; the header's build ID makes them meaningful.

### 4.3 Records

`PropertyICRecord`, 8 bytes:

| byte | field | content |
|---|---|---|
| 0 | `accessType` | the IC's `JSC::AccessType` value |
| 1 | `learningBits` | bit 0 `everConsidered`, bit 1 `sawNonCell`, bit 2 `tookSlowPath`, bit 3 `resetByGC`; bits 4 to 7 zero |
| 2 | `repatchCount` | as captured |
| 3 | `numberOfCoolDowns` | as captured |
| 4 | `caseCount` | the number of entries `PropertyInlineCache::listedAccessCases` returned, at most 8 under the fixed `maxAccessVariantListSize` (sections 5.2 and 10; [history](SPEC-ics-history.md#the-ic-limit-options-are-fixed)) |
| 5 | `stateBits` | bit 0 `megamorphicCaseListed`, bit 1 `canBeMegamorphic`, bit 2 `holdsGaveUp` (section 5.2); bits 3 to 7 zero |
| 6, 7 | `reserved` | zero |

`CallLinkRecord`, 10 bytes:

| byte | field | content |
|---|---|---|
| 0 | `bits` | bits 0 and 1 mode (0 `Init`, 1 `Monomorphic`, 2 `Polymorphic`, 3 `Virtual`), bit 2 `seenOnce`, bit 3 `hasSeenClosure`, bit 4 `clearedByGC`, bit 5 `clearedByVirtual`; bits 6 and 7 zero |
| 1 | `maxArgumentCountIncludingThisForVarargs` | as captured |
| 2 to 5 | `thisArrayModes` | the `this` profile's `ArrayModes`, as captured; zero at an opcode without a `this` profile |
| 6 to 9 | `thisArrayProfileFlags` | the `this` profile's flags, `OptionSet<ArrayProfileFlag>::toRaw()`, the pruning mark included; zero at an opcode without a `this` profile |

The record holds producer facts only ([history](SPEC-ics-history.md#records-hold-producer-facts)). No field is a transformed value, so the capture record THREAD's Verification compares against is the record itself, and section 6.1 derives the consumer state from it. For an inline site and a lazy site with its own data, the bits and the maximum are those of the site's `CallLinkInfo`, and the two `this` fields hold the accumulated state of the site's own `this` profile when its opcode has one. A lazy site without its own data points at a `CallLinkInfo` and a profile that describe no site, so its record is zero except the seen bit, which is `LazyCallLinkInfo::hasExecutedOnce()`.

### 4.4 Types (`jitcache/ics/ICSection.h`)

```cpp
namespace JSC::JITCache::ICs {

enum class CallLinkModeCode : uint8_t { Init = 0, Monomorphic = 1, Polymorphic = 2, Virtual = 3 };

struct PropertyICRecord {
    uint8_t accessType;
    uint8_t learningBits;
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;
    uint8_t caseCount;
    uint8_t stateBits;
    uint8_t reserved[2];
};
static_assert(sizeof(PropertyICRecord) == 8 && alignof(PropertyICRecord) == 1);

struct CallLinkRecord {
    uint8_t bits;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    uint8_t thisArrayModes[4];          // ArrayModes, little-endian, through memcpy
    uint8_t thisArrayProfileFlags[4];   // OptionSet<ArrayProfileFlag>::toRaw(), little-endian, through memcpy
};
static_assert(sizeof(CallLinkRecord) == 10 && alignof(CallLinkRecord) == 1);

struct SectionHeader {
    uint32_t propertyICCount;
    uint32_t icSitesWithCases;
    uint32_t callLinkGroupCount;
    uint32_t callLinkSiteCount;
};

struct CallLinkGroup {
    uint32_t opcodeID;
    uint32_t siteCount;
};

namespace LearningBit {
inline constexpr uint8_t everConsidered = 1 << 0;
inline constexpr uint8_t sawNonCell = 1 << 1;
inline constexpr uint8_t tookSlowPath = 1 << 2;
inline constexpr uint8_t resetByGC = 1 << 3;
inline constexpr uint8_t mask = 0x0f;
}

namespace StateBit {
inline constexpr uint8_t megamorphicCaseListed = 1 << 0;
inline constexpr uint8_t canBeMegamorphic = 1 << 1;
inline constexpr uint8_t holdsGaveUp = 1 << 2;
inline constexpr uint8_t mask = 0x07;
}

namespace CallLinkBit {
inline constexpr uint8_t modeMask = 0x03;
inline constexpr uint8_t seenOnce = 1 << 2;
inline constexpr uint8_t hasSeenClosure = 1 << 3;
inline constexpr uint8_t clearedByGC = 1 << 4;
inline constexpr uint8_t clearedByVirtual = 1 << 5;
inline constexpr uint8_t mask = 0x3f;
}

inline constexpr size_t sectionHeaderSize = 16;
inline constexpr size_t callLinkGroupSize = 8;
inline constexpr unsigned numberOfCallLinkOpcodes = 14; // static_assert against FOR_EACH_OPCODE_WITH_CALL_LINK_INFO

// How an opcode's metadata entry holds its call link.
enum class CallLinkSiteKind : uint8_t {
    Inline,         // a DataOnlyCallLinkInfo in the entry: the four varargs opcodes
    Lazy,           // a LazyCallLinkInfo: FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO but OpTailCall
    LazyTailCall,   // OpTailCall's LazyCallLinkInfo, linked to the not-executed tail-call data
};

struct CallLinkOpcode {
    OpcodeID opcodeID;
    CallLinkSiteKind kind;
    bool hasThisProfile;    // the site's CallSiteData holds the ArrayProfile of its this value
};

// The opcode at each canonical position, generated in macro order from
// FOR_EACH_OPCODE_WITH_CALL_LINK_INFO. kind is Lazy or LazyTailCall exactly for the members of
// FOR_EACH_OPCODE_WITH_LAZY_CALL_LINK_INFO. hasThisProfile is true exactly for the members of
// FOR_EACH_OPCODE_WITH_SIMPLE_ARRAY_PROFILE whose Metadata has no m_arrayProfile, the test
// arrayProfileFor in bytecode/CodeBlock.cpp makes: OpCall, OpCallIgnoreResult and OpTailCall.
inline constexpr std::array<CallLinkOpcode, numberOfCallLinkOpcodes> callLinkOpcodes { /* generated from the macros */ };

// Canonical position of opcodeID in FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, or nullopt.
std::optional<unsigned> canonicalCallLinkPosition(OpcodeID);

// nullopt on overflow or when groupCount exceeds numberOfCallLinkOpcodes.
std::optional<size_t> sectionSize(size_t groupCount, size_t propertyICCount, size_t callLinkSiteCount);

// Metadata entries per call-link opcode, indexed by canonical position.
using CallLinkSiteCounts = std::array<uint32_t, numberOfCallLinkOpcodes>;

enum class StrictChecks : bool { No, Yes };

struct Summary {
    uint32_t icSitesWithCases { 0 };
};

enum class Check : uint8_t {
    // The span's length is not the size the header's counts give.
    SectionSize,
    // More IC sites with cases than property ICs.
    SummaryBound,
    // A group's opcode has no call link or breaks canonical order, a group is empty, or the
    // groups' site counts do not add up to the header's.
    CallLinkGroups,
    // A padding byte, a reserved byte or an unused bit is set, or a this field holds a value its
    // opcode or the array-profile bounds exclude.
    ReservedBits,
    // An access type outside the enumeration.
    EnumRange,
    // The count of IC sites with cases differs from the records.
    SummaryCount,
    // The property-IC records do not pair one to one with the prepared code's molds.
    MoldPairing,
    // A group's site count differs from the CB's metadata entries of that opcode.
    MetadataLayout,
    // A mold has canBeMegamorphic set.
    MoldMegamorphicBit,
    // The CB has run, or a call-link site is not as linking left it.
    NewbornCallLinks,
};

struct Invalid {
    Check check;
    uint32_t siteIndex; // record index when the check is per site, 0 otherwise
};

// The check's name in a fault's step, after "ics.": one lowercase token, its words joined by '-'.
ASCIILiteral description(Check);

// A parsed section, as views into the bytes it was parsed from.
struct SectionView {
    SectionHeader header;
    std::span<const uint8_t> groupBytes;            // callLinkGroupCount entries of callLinkGroupSize bytes
    std::span<const PropertyICRecord> propertyICs;  // record i for IC i
    std::span<const CallLinkRecord> callLinks;      // canonical order
    CallLinkGroup group(unsigned index) const;      // read with memcpy
};

// Slices the section by the counts its header states. With StrictChecks::Yes it first checks the
// size, the summary, the group table and every record; otherwise it trusts the counts. Reads only
// its argument and needs no VM.
std::expected<SectionView, Invalid> parseSection(std::span<const uint8_t>, StrictChecks);

// Reads the summary of a saved body from its ICsBaseline section, checking the section's size and
// the summary's bound under strict and trusting the header otherwise. Takes no lock and needs no VM.
std::expected<Summary, Invalid> readBaselineICsSummary(std::span<const uint8_t> section, StrictChecks);

// The property-IC half of the polymorphic bit: two or more cases listed, none of them megamorphic.
bool isPolymorphicPropertyIC(const PropertyICRecord&);

} // namespace JSC::JITCache::ICs
```

Each `Check` is one rule of section 4.5 or 6.2, which names its enumerator beside the rule. `description(check)` gives each one the token that `status` shows after `ics.` (R-INT-3, R-INT-4; [history](SPEC-ics-history.md#check-names)), so the step of a failed A7 reads `ics.mold-pairing`, and the twin check uses the same token for a check its recapture fails (section 11.1):

| check | rule | token |
|---|---|---|
| `SectionSize` | A1 | `section-size` |
| `SummaryBound` | A2 | `summary-bound` |
| `CallLinkGroups` | A3 | `call-link-groups` |
| `ReservedBits` | A4 | `reserved-bits` |
| `EnumRange` | A5 | `enum-range` |
| `SummaryCount` | A6 | `summary-count` |
| `MoldPairing` | A7 | `mold-pairing` |
| `MetadataLayout` | A8 | `metadata-layout` |
| `MoldMegamorphicBit` | S1 | `mold-megamorphic-bit` |
| `NewbornCallLinks` | S2 | `newborn-call-links` |

### 4.5 Well-formedness

Normal mode checks only the artifact's integrity (header, keys and checksums), which the integrator does before the lane sees a byte, and trusts the rest, the assumptions capture and prepare make included (THREAD Session). Without strict, `parseSection` slices the record arrays by the header's counts, `readBaselineICsSummary` reads the header as it stands, and prepare and capture check nothing (section 5.5), so no call into the lane can fail; debug builds still assert A1 to A6. With strict on, as in every run the test runner starts (SPEC-integrator.harness.md section 7.3), the checks validate the section's full structure: prepare (section 6.2) runs A1 to A8 and `readBaselineICsSummary` runs A1 and A2 on the header alone. Each failure is invalid material (THREAD Session) and names its check ([history](SPEC-ics-history.md#checks-run-only-under-strict-and-copy-no-native-table)).

- A1 (`SectionSize`). The span holds at least 16 bytes, `callLinkGroupCount` ≤ 14, and `sectionSize` of the header counts is defined and equals the span's length.
- A2 (`SummaryBound`). `icSitesWithCases` ≤ `propertyICCount`.
- A3 (`CallLinkGroups`). Group opcode IDs have canonical positions, the positions strictly increase, every `siteCount` is nonzero, and the `siteCount`s sum to `callLinkSiteCount` without overflow.
- A4 (`ReservedBits`). Padding bytes, `reserved`, the unused bits of `learningBits`, `stateBits` and call-link `bits` are zero. A call-link record of an opcode without a `this` profile has both `this` fields zero; elsewhere `thisArrayModes` has no bit outside `ALL_ARRAY_MODES` and `thisArrayProfileFlags` none outside the eight `ArrayProfileFlag` bits (`0xff`), the bounds SPEC-cb.md V7 sets for every array profile.
- A5 (`EnumRange`). `accessType` < `numberOfAccessTypes`.
- A6 (`SummaryCount`). `icSitesWithCases` equals the number of records with `caseCount` > 0.
- A7 (`MoldPairing`). `propertyICCount` equals the mold count of the prepared code (section 6.2), and record i's `accessType` equals mold i's.
- A8 (`MetadataLayout`). For each canonical opcode, the newborn CB's call-link site count (`callLinkSiteCounts`, section 4.6) equals the group's `siteCount`, or zero when the opcode has no group.

A7 and A8 compare the section with what another part restored (the image's molds, the UCB's metadata layout). Normal mode relies on the Image lane restoring the producer's molds (R-IMG-1) and on the UCB lane's guarantee of the producer's index spaces (THREAD Storage, R-UCB-1), and seeding and attach index the CB's ICs and sites by record position; strict verifies the pairing with one pass over the molds and one over the metadata offsets.

`parseSection` holds A1 to A6; A7, A8, S1 and S2 are functions of their own (section 6.2). A7, A8 and S1 read only plain data (the section view, the molds, the per-opcode counts), so the unit tests reach each check with crafted inputs and no VM (T7).

### 4.6 Site walkers (`jitcache/ics/ICSites.h`)

```cpp
namespace JSC::JITCache::ICs {

// Calls functor(Op::Metadata&) for each metadata entry of Op, in metadata ID order.
// Calls nothing when codeBlock.metadataTable() is null.
template<typename Op, typename Functor>
void forEachMetadataEntry(CodeBlock&, const Functor&);

// One call-link site: exactly one of the two pointers is set, as callLinkOpcodes[canonicalPosition].kind says.
struct CallLinkSite {
    unsigned canonicalPosition;
    unsigned metadataID;
    DataOnlyCallLinkInfo* inlineInfo;   // an Inline site's entry field
    LazyCallLinkInfo* lazyInfo;         // a lazy site's entry field

    const CallLinkOpcode& opcode() const { return callLinkOpcodes[canonicalPosition]; }
    // The CallLinkInfo that belongs to this site: the inline one, or a lazy site's own
    // (LazyCallLinkInfo::get()); null for a lazy site without data of its own.
    CallLinkInfo* ownCallLinkInfo() const;
};

// Calls functor(const CallLinkSite&) for every call-link site, by canonical position and, within
// a position, in metadata ID order. Calls nothing for a CB without a metadata table.
template<typename Functor>
void forEachCallLinkSite(CodeBlock&, const Functor&);

// Calls functor(const CallLinkSite&, const JSInstruction*) for every instruction of the CB
// whose opcode is in FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, in instruction order, with that
// instruction's metadata entry. Seeding walks sites this way, because a site's own CallSiteData
// needs the bytecode index that only its instruction knows.
template<typename Functor>
void forEachCallLinkInstruction(CodeBlock&, const Functor&);

// Entries per call-link opcode; all zero for a CB without a metadata table.
inline CallLinkSiteCounts callLinkSiteCounts(CodeBlock&);

}
```

`ICSites.h` is header-only, so it has no `Sources.txt` entry. `forEachMetadataEntry` reads `metadataTable()` once, returns when it is null, as `CodeBlock::forEachLLIntOrBaselineCallLinkInfo` and `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` do, and otherwise calls `MetadataTable::forEach<Op>` ([history](SPEC-ics-history.md#a-cb-can-have-no-metadata-table)). `forEachCallLinkSite` expands `FOR_EACH_OPCODE_WITH_CALL_LINK_INFO` over it, counting metadata IDs per opcode and passing the address of `metadata.m_callLinkInfo` as `inlineInfo` or `lazyInfo` by the field's type, as `CodeBlock::forEachLLIntOrBaselineCallLinkInfo` tells the shapes apart. `forEachCallLinkInstruction` walks `codeBlock.instructions()` as the link pass of `CodeBlock::finishCreation` does, and for each instruction of those opcodes passes `instruction->as<Op>().m_metadataID` and the field of `instruction->as<Op>().metadata(codeBlock)`; a CB without a metadata table has no such instruction, since every call-link opcode adds an entry. Sizing, capture, A8, S2, seeding and the twin snapshots walk call-link sites only through these functions; nothing else in the lane calls `MetadataTable::forEach` or switches over call-link opcodes. They read the instruction stream, the offset table and the entries, take no lock and allocate nothing; the layout is fixed once `UnlinkedMetadataTable::link` has run.

## 5. Capture

### 5.1 Where it runs

The integrator calls the lane at THREAD's two capture points, the end of the success path of `BaselineJITPlan::finalize` and `delta`, in the context SPEC-integrator.md sections 8.1, 8.5, 8.6 and 8.7 give them: on an eligible CB whose `BaselineJITData` is published, with no `CodeBlock::m_lock` held (L2). The lane adds no hook on the capture side.

The capture functions take `CodeBlock::m_lock` as L1 states, read, and release it before returning. They allocate no cell and no memory: the integrator sizes, charges and allocates the output (R-INT-2), and `PropertyInlineCache::listedAccessCases` returns a `Vector<AccessCase*, 16>` whose inline capacity holds the at most eight cases the fixed `maxAccessVariantListSize` allows (section 5.2; [history](SPEC-ics-history.md#the-ic-limit-options-are-fixed)). As THREAD Capture requires, they call nothing that drains a profile, materializes a property table, allocates a cell or stops for the collector; the only callees are `PropertyInlineCache::listedAccessCases`, `AccessCase::type`, `gaveUpOperationFor` (E3), the `CallLinkInfo` getters, `LazyCallLinkInfo::get`, `hasExecutedOnce` and `arrayProfile`, `ArrayProfile::observedArrayModes` and `arrayProfileFlags` (SPEC-cb.md E1), `PolymorphicCallStubRoutine::forEachDependentCell` and the walkers of section 4.6. L4 gives the reason a capture sees one consistent state ([history](SPEC-ics-history.md#a-captures-consistency-comes-from-the-pause)).

### 5.2 Classifying a property IC

For IC i of `BaselineJITData`:

1. `cases = ic.listedAccessCases(locker)`; `caseCount = cases.size()`, at most the fixed `maxAccessVariantListSize` of 8 (options.md). `InlineCacheCompiler::compileHandler` measures the list with the new case appended, a fold leaves only the megamorphic case listed, and a list that reaches the limit compiles its last case as final code, after which `tryCacheGetBy` and its siblings answer `GiveUpOnCache` (`AccessGenerationResult::shouldGiveUpNow`) and the `*GaveUp` operation caches nothing more.
2. `megamorphicCaseListed` is true when some listed case satisfies `AccessCase::isMegamorphic(case->type())` (E4). After a fold the megamorphic case is the only listed case, because `PropertyInlineCache::prependHandler` replaces the chain through `initializeWithUnitHandler` and clears the inlined handler.
3. `holdsGaveUp` is true when `ic.m_slowOperation == gaveUpOperationFor(ic.accessType)` (E3). The field's other values need no record. Installation writes the `*Optimize` operation, and outside the FTL only `repatchSlowPathCall` rewrites it, with the access's `*Optimize`, `*Megamorphic` or `*GaveUp` operation. Only the `PromoteToMegamorphic` arms of `repatchGetBy`, `repatchPutBy`, `repatchInBy` and their array forms install a `*Megamorphic` operation, after the fold, and both ways out of it empty the chain (`PropertyInlineCache::reset` back to `*Optimize` through `resetGetBy` and its siblings, a megamorphic give-up to `*GaveUp` through `repatchGetBySlowPathCall` and its siblings), so `megamorphicCaseListed` already says what a `*Megamorphic` operation would. An operation that is not the access type's `*GaveUp` restores as a reset leaves the IC.
4. `learningBits` copies `everConsidered`, `sawNonCell`, `tookSlowPath` and `resetByGC`; `repatchCount` and `numberOfCoolDowns` are copied.
5. `canBeMegamorphic` copies `ic.canBeMegamorphic`. The IC's bit has two writers, `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` and `initializeFromDFGUnlinkedPropertyInlineCache`, which copy the mold's bit, and nothing sets a baseline mold's bit (`JIT::addUnlinkedPropertyInlineCache` default-constructs each mold); `PropertyInlineCache::reset` leaves the IC's bit alone, and its only reader is `InlineCacheCompiler::tryFoldToMegamorphic`. On a baseline IC only attach sets it (section 6.4), so it means the site carries a fold an import restored.

The record keeps the case count, the megamorphic case, the `*GaveUp` operation and the fold bit as separate facts, so that section 6.1 can tell apart the four native states below and a recapture keeps a restored fold ([history](SPEC-ics-history.md#what-a-property-ic-record-keeps-about-cases-and-folds)):

- (a) A site gives up with its cases still chained. `InlineCacheCompiler::compileOneAccessCaseHandler` compiles the eighth case as `GeneratedFinalCode`, which `PropertyInlineCache::addAccessCase` prepends before `tryCacheGetBy` and its siblings answer `GiveUpOnCache`, and a `GiveUpOnCache` after earlier cases leaves them in place. Either way `repatchGetBy` and its siblings install the `*GaveUp` operation beside the cases, which keep serving their shapes, and a `PropertyInlineCache::reset` of them switches the site back to `*Optimize` (`resetGetBy` and its siblings).
- (b) `instanceof` folds into `InstanceOfMegamorphic`, and in the same slow path `tryCacheInstanceOf` returns `GiveUpOnCache` and `repatchInstanceOf` installs `operationInstanceOfGaveUp`: the site lists a megamorphic case and holds its `*GaveUp` operation.
- (c) A get, put or `in` site that folded and later gave up through `repatchGetBySlowPathCall` or its siblings has a bare chain, `m_cacheType` still `Stub` and its `*GaveUp` operation.
- (d) A fold an import restored shows only in `canBeMegamorphic` until a caching visit folds the site again; until then the IC lists no megamorphic case and holds `*Optimize`, and `delta`, which captures imported CBs, can capture it first.

### 5.3 The summary and the polymorphic bit

`icSitesWithCases` counts the property ICs whose `caseCount` is nonzero. Call links do not count: THREAD's tie-break speaks of IC sites with cases, and cases are the property ICs' `AccessCase`s ([history](SPEC-ics-history.md#the-summary-counts-property-ics-only)). A capture at the end of `BaselineJITPlan::finalize` always counts zero, because installation has just built every IC cold.

Capture and `summarizeBaselineICs` also return `hasPolymorphicSite`, which says whether the body has a site whose cases or callees an early DFG compile would see only in part. The integrator passes it to the CB lane's capture and scoring, and a body with the bit carries no baseline counter progress (THREAD Restoration; [history](SPEC-ics-history.md#the-polymorphic-bit-call-sites-included)). The bit is set when either of these holds:

- some property IC's record lists two or more cases and no megamorphic case (`isPolymorphicPropertyIC`: `caseCount >= 2 && !megamorphicCaseListed`), THREAD's "property IC whose captured record lists two or more cases, megamorphic folds aside";
- some call-link site is in mode `Polymorphic`, has `clearedByGC` clear, and its stub (`CallLinkInfo::stub`) holds two or more slots, counted with `PolymorphicCallStubRoutine::forEachDependentCell`.

THREAD sets the bit for a call site the producer left polymorphic when this SPEC shows that such a site meets the same exits, and the sites above do. A `Polymorphic` site always holds its own `CallLinkInfo`: an inline one, or a lazy site's own `CallSiteData`. The consumer restores it without its stub, seen and in `Init` (section 6.1): an inline site on its own `CallLinkInfo`, and a lazy site on data of its own when it also holds history or a `this` profile with observations or is a tail call, and on the VM's executed-once data otherwise. In each case its first call links it monomorphically to the callee that call meets, recording `lastSeenCallee`: `handleUnlinkedCall` gives an executed-once site its own data, seen, through `LazyCallLinkInfo::ensure`, and `linkFor` then calls `linkMonomorphicCall`, as it does at once for a site that already has a `CallLinkInfo` of its own. For a site without a stub, `CallLinkStatus::computeFromCallLinkInfo` answers one variant, `lastSeenCallee`, despecified to its executable when `hasSeenClosure` is set, with no slow path. `CallLinkStatus::computeFor` finds a lazy site's `CallLinkInfo` in the CB's `ICStatusMap` or, when the site got its data after the map was made, through `CodeBlock::callLinkInfoIfExistsAt`, and answers an empty status for a site that has none. A DFG compile at the counter floor acts on it: `ByteCodeParser::handleInlining` inlines the variant behind the `CheckIsConstant` that `ByteCodeParser::emitFunctionChecks` adds, and `SpeculativeJIT::compileCheckIsConstant` exits with `BadConstantValue` when another callee arrives. The producer's DFG read the stub instead, whose two or more slots give `CallLinkStatus` two or more variants or a slow path unless one callee took every call counted since the last new variant, and `handleInlining` leaves such a status to a generic call outside the FTL. The consumer's exits count toward a jettison (`CodeBlock::exitCountThresholdForReoptimization`), which records a `BadConstantValue` exit site at that bytecode (`CodeBlock::tallyFrequentExitSites`) and counts a reoptimization that doubles the CB's later thresholds. That exit site makes every later status there a closure call or a slow path (`CallLinkStatus::computeExitSiteData` and `CallLinkStatus::accountForExits`), and exit sites and reoptimization counts both travel in a later capture, the pattern of the `BadCache` exits THREAD names for property ICs.

The other call sites stay out. A lazy site without its own data never linked a callee. A stub with one slot holds closures of one executable: `linkPolymorphicCall` merges callees that share an executable into one despecified variant and sets `hasSeenClosure`, so the consumer's despecified status matches each of them and the check never exits. A site with `clearedByGC` comes back with it, and a site the producer left `Virtual` comes back virtual with `clearedByVirtual`; `computeFromCallLinkInfo` answers a slow path for both, so no compile speculates there. A `Monomorphic` site saw one callee.

At the end of `BaselineJITPlan::finalize` the property ICs are cold, but the call links hold what the LLInt linked, so that capture can already carry the bit. The bit reads the cases and stubs a capture finds, as THREAD's rule speaks of the captured record, and no restored state holds it, so the I5 round trip does not keep it: a ConsumerProducer that recaptures an imported body before a polymorphic site has cached two cases or linked two callees again captures it without the bit, with the counter progress its CB made (B6). Whether that recapture replaces a saved body that has the bit is THREAD Capture's ranking, which SPEC-cb.md section 4.3 applies to the withheld counter.

### 5.4 Call links

`callLinkSiteCounts(codeBlock)` gives the groups: each opcode with a nonzero count gets one, in canonical order. `forEachCallLinkSite(codeBlock, ...)` then writes one record per site, in the same order, through `readCallLinkRecord` (section 5.6):

- A site with a `CallLinkInfo` of its own (`CallLinkSite::ownCallLinkInfo()`) gives `mode()` mapped to its code, `seenOnce()`, `hasSeenClosure()`, `clearedByGC()`, `clearedByVirtual()` and `maxArgumentCountIncludingThisForVarargs()`. When its opcode has a `this` profile, it also gives the accumulated state of the profile of its own data, `LazyCallLinkInfo::arrayProfile()`: `observedArrayModes()` and `arrayProfileFlags().toRaw()`, the pruning mark included. Capture never reads the profile's two structure samples and calls no draining function (`computeUpdatedPrediction` or any SPEC-cb.md I2 lists), so a pending sample stays where it is and no `StructureID` is decoded (THREAD Capture).
- A lazy site without data of its own gives mode `Init`, the seen bit from `LazyCallLinkInfo::hasExecutedOnce()`, and nothing else: the `this` structures it noted went to a shared profile that nothing reads.

`readCallLinkRecord` reads the `this` profile under the `CodeBlock::m_lock` it holds for the call link. Native readers of a call link take that lock (`CallLinkStatus::computeFor`), and native readers of an array profile take none (SPEC-cb.md N3), so the lock adds nothing for the profile: a marker or compiler thread can drain it during the capture, which THREAD Capture accepts as a loss of precision. The CB lane's summary reads the flags of the same profile after this capture, in the same pause (R-CB-1).

For a `Polymorphic` site without `clearedByGC` capture also counts the stub's slots for the polymorphic bit (section 5.3), stopping at two, and records no count. `CallLinkStatus::computeFor` reads the stub under the same `CodeBlock::m_lock`, and nothing replaces or frees it during a capture (L4).

### 5.5 Strict checks

Strict checks the assumptions capture makes; each failure is a recording fault (THREAD Session) that names the check and the site index. Normal mode trusts them (section 4.5), and debug builds assert them ([history](SPEC-ics-history.md#checks-run-only-under-strict-and-copy-no-native-table)).

- The integrator's call: the CB has `BaselineJITData` (`CaptureCheck::NoBaselineJITData`), and the output span's size equals `baselineICsSectionSize` (`OutputSizeMismatch`). Writing records past either bound would corrupt memory, so these come first.
- C1 (`MoldMismatch`). IC i's `accessType` equals mold i's: record i stands for IC i, and restore pairs it with mold i (A7). (`CodeBlock::setupWithUnlinkedBaselineCode` builds IC i from mold i.)

Capture checks nothing else. The derivation of section 6.1 gives a sound state for every combination of the facts a record holds, so native invariants among them (one case beside a megamorphic case, a virtual site with `clearedByVirtual`, a zero varargs maximum outside varargs opcodes) go unchecked, and an engine change that broke one would not make a restored state unsound.

### 5.6 Interface (`jitcache/ics/ICCapture.h`)

```cpp
namespace JSC::JITCache::ICs {

// What summarizing or capturing a live CB yields: the summary and the polymorphic bit.
struct CaptureSummary {
    Summary summary;
    bool hasPolymorphicSite { false };
};

enum class CaptureCheck : uint8_t {
    NoBaselineJITData,      // the CB has no BaselineJITData
    OutputSizeMismatch,     // the output span is not baselineICsSectionSize bytes long
    MoldMismatch,           // IC i's access type differs from mold i's
};

struct CaptureError {
    CaptureCheck check;
    uint32_t siteIndex; // the IC's index for MoldMismatch, 0 otherwise
};

// The check's name in a fault's step, after "ics.": one lowercase token, its words joined by '-'.
ASCIILiteral description(CaptureCheck);

// The exact byte size of the CB's ICsBaseline section: the IC count of BaselineJITData and
// callLinkSiteCounts (zero groups without a metadata table). Reads counts only, takes no lock.
// Precondition: jitType() == BaselineJIT and baselineJITData() is non-null.
size_t baselineICsSectionSize(CodeBlock&);

// The summary and polymorphic bit the CB would capture now, for scoring candidates before
// capturing the winner. Takes CodeBlock::m_lock. Allocates no cell and no memory.
CaptureSummary summarizeBaselineICs(CodeBlock&);

// Writes the whole section into output, whose size must equal baselineICsSectionSize(codeBlock),
// and returns its summary and polymorphic bit. Takes CodeBlock::m_lock. Allocates no cell and no
// memory. Reads only.
std::expected<CaptureSummary, CaptureError> captureBaselineICs(CodeBlock&, std::span<uint8_t> output, StrictChecks);

// The one reader of live state, which capture and the twin snapshot share. Each returns the record
// capture writes for one site, under that site's CodeBlock::m_lock, which the locker witnesses.
PropertyICRecord readPropertyICRecord(const ConcurrentJSLocker&, const HandlerPropertyInlineCache&);
CallLinkRecord readCallLinkRecord(const ConcurrentJSLocker&, const CallLinkSite&);

}
```

`summarizeBaselineICs` and `captureBaselineICs` return the same summary and bit when no JS runs and no stopped-world GC phase intervenes between the two calls, which holds inside one capture point. A saved body keeps the bit as the CB lane's `counterMode` in its summary (SPEC-cb.md section 3.3), which THREAD Capture's tie-break reads.

`description(check)` gives each `CaptureCheck` its token after `ics.` (R-INT-2), as section 4.4 does for `Check`:

| check | rule | token |
|---|---|---|
| `NoBaselineJITData` | the integrator's call (section 5.5) | `no-baseline-jit-data` |
| `OutputSizeMismatch` | the integrator's call (section 5.5) | `output-size-mismatch` |
| `MoldMismatch` | C1 | `mold-mismatch` |

## 6. Restoration

The lane contributes three calls to the install function, in the order THREAD Restoration fixes; R-INT-4 gives their positions.

| call | CB state on entry | lock the lane takes |
|---|---|---|
| `prepareBaselineICs` | newborn: `finishCreation` done, `jitType()` is `None`, never run | none |
| `seedCallLinkHistory` | linked, no JIT type | none |
| `attachPropertyICState` | `BaselineJITData` published, executable not yet updated | `CodeBlock::m_lock` |

No other thread reads what the lane writes before `installCode` stores the CB in its executable (SPEC-integrator.md section 7.4), and attach still writes under the lock (L3). The seeds land in the CB that installs the import and nowhere else; the body's later CBs share the image natively with fresh IC and call-link state (THREAD Restoration), which T8 tests.

### 6.1 Derivation

Two pure functions per record kind, declared in `ICSection.h`. `restoredPropertyIC` and `restoredCallLink` turn a producer record into the consumer state: seed and attach apply them, and the twin check compares live state with them. `recapturedPropertyIC` and `recapturedCallLink` give the record that a capture of that restored state yields before any site runs: the twin check compares a capture with them (I5). The unit tests (T7) cover all four without a VM ([history](SPEC-ics-history.md#records-hold-producer-facts)).

```cpp
namespace JSC::JITCache::ICs {

struct RestoredPropertyIC {
    bool everConsidered;
    bool sawNonCell;
    bool tookSlowPath;
    bool resetByGC;
    bool foldsAtFirstCase;  // ORed into canBeMegamorphic
    bool givenUp;           // m_slowOperation becomes the access type's *GaveUp operation
    uint8_t countdown;      // always 0
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;
    friend bool operator==(const RestoredPropertyIC&, const RestoredPropertyIC&) = default;
};

// Which CallSiteData a site points at.
enum class CallSiteDataState : uint8_t {
    Inline,         // an Inline site, which has no CallSiteData
    NotExecuted,    // what linking left: the VM's never-executed data, or its not-executed tail-call data
    ExecutedOnce,   // the VM's executed-once data
    Own,            // a CallSiteData of its own
};

struct RestoredCallLink {
    CallSiteDataState data;
    // The rest describes the site's own CallLinkInfo, which exists when data is Inline or Own.
    bool isVirtual;         // seeded through CallLinkInfo::setVirtualCall
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    // The accumulated state of the site's this profile, restored into its own data when data is Own.
    uint32_t thisArrayModes;
    uint32_t thisArrayProfileFlags;
    friend bool operator==(const RestoredCallLink&, const RestoredCallLink&) = default;
};

RestoredPropertyIC restoredPropertyIC(const PropertyICRecord&);
RestoredCallLink restoredCallLink(const CallLinkOpcode&, const CallLinkRecord&);

// The record a capture of the restored state yields before any site runs.
PropertyICRecord recapturedPropertyIC(const PropertyICRecord&);
CallLinkRecord recapturedCallLink(const CallLinkOpcode&, const CallLinkRecord&);

}
```

For a property-IC record r, with `folded = r.megamorphicCaseListed || r.canBeMegamorphic`:

| field | value |
|---|---|
| `givenUp` | `r.holdsGaveUp && r.caseCount == 0` |
| `foldsAtFirstCase` | `folded && !givenUp` |
| `tookSlowPath` | `r.tookSlowPath || givenUp` |
| `everConsidered`, `sawNonCell`, `resetByGC`, `numberOfCoolDowns` | as recorded |
| `countdown` | 0, for every IC, considered or not |
| `repatchCount` | `r.repatchCount - min(r.repatchCount, r.caseCount)` |

A site is folded when it lists a megamorphic case or carries a fold an earlier import restored. It is given up when it holds the `*GaveUp` operation and lists no case: it gave up listing none, or a give-up through `repatchGetBySlowPathCall` or its siblings emptied a folded get, put or `in` site's chain (section 5.2, state (c)), a consumer's replay of a restored fold included. Natively that give-up is permanent, since the `*GaveUp` operation never caches and none of the three callers of `PropertyInlineCache::reset` can switch it back to `*Optimize` (no reference it holds can die, no watchpoint is armed, no case is added), so a given-up site restores no fold.

A site that holds `*GaveUp` beside listed cases (section 5.2, states (a) and (b)) is not given up ([history](SPEC-ics-history.md#a-site-comes-back-given-up-only-when-it-lists-no-case)). Natively its give-up lasts only as long as those cases, which do not travel, so it comes back as a reset of them leaves it, as THREAD Caches requires: on its `*Optimize` slow call, considered and empty, with the learning group the table gives. When it lists a megamorphic case it also comes back with `canBeMegamorphic`, as every site the producer folded does, which for the folded `instanceof` brings back `InstanceOfMegamorphic` beside `operationInstanceOfGaveUp` at its first caching visit, the producer's state. Native caching rebuilds the other sites' cases from the consumer's operands and gives up again where they make it. Every restored IC is thus in exactly one of three states: cold or considered, folded, or given up.

`foldsAtFirstCase` sets `canBeMegamorphic`, which `InlineCacheCompiler::tryFoldToMegamorphic` reads beside the list-size test, so the consumer's first caching visit applies the fold rules to its first case. `repatchCount` is rewound because the consumer counts one more repatch through `considerRepatchingCacheImpl` for each case it re-caches: one, the megamorphic case, at a folded site, and up to k at a site that gave up beside k cases. `countdown` is zero so the site's first consumer visit caches, and THREAD Caches' wait-counter rule applies to every IC, including one the producer never considered ([history](SPEC-ics-history.md#every-ic-at-a-zero-wait-counter)).

For a call-link record r at a site of opcode `op`, call r

- *holding* when it has what only a `CallSiteData` or `CallLinkInfo` of the site's own can hold: mode `Virtual`, `hasSeenClosure`, `clearedByGC`, `clearedByVirtual`, a nonzero varargs maximum, or, when `op.hasThisProfile`, a `this` profile with observations, a nonzero `thisArrayModes` or `thisArrayProfileFlags`;
- *empty* when it is not holding, its mode is `Init` and its seen bit is clear: the site never ran, or a reset (`CallLinkInfo::reset`, from `revertCall` when a callee's code goes away) left it as linking did. A direct eval whose callee is not `eval` is empty too although it ran: the LLInt calls such a callee without the site's call link (`commonCallDirectEval`), and baseline code gives the site data of its own and calls virtually without changing it (`JIT::compileCallDirectEvalSlowCase`). In each case the producer's next call there would not link.

| field | value |
|---|---|
| `data` | `Inline` when `op.kind` is `Inline`; otherwise `NotExecuted` when r is empty, `Own` when r is holding or `op.kind` is `LazyTailCall`, and `ExecutedOnce` otherwise |
| `isVirtual` | `mode(r) == Virtual` |
| `seenOnce` | `!empty && !isVirtual` |
| `clearedByVirtual` | `r.clearedByVirtual || isVirtual` (`setVirtualCall` sets it on a virtual site) |
| `hasSeenClosure`, `clearedByGC`, `maxArgumentCountIncludingThisForVarargs` | as recorded |
| `thisArrayModes`, `thisArrayProfileFlags` | as recorded when `op.hasThisProfile`, zero otherwise |

A site thus comes back as the producer's last execution left it, ready to link at its next call ([history](SPEC-ics-history.md#a-call-site-gets-its-own-data-only-when-it-holds-something)). A lazy site gets data of its own only when the record is holding, so no data is allocated for a site with nothing to hold; it gets the executed-once data when it ran and holds nothing, and keeps what linking left when the record is empty. A tail call that ran is the exception: no shared data holds a tail call's seen bit, since a tail call never reaches `handleUnlinkedCall`, and natively it gets its own data before its first call anyway (section 3.2), so it comes back `Own` and seen. An inline site comes back seen exactly when the record is not empty, and with the record's history.

A `this` profile with observations makes its site holding, because the profile has no other place to live in the consumer: shared data holds no site's profile, and a builtin's UCB keeps no copy for the native merge to fold in (section 3.2; [history](SPEC-ics-history.md#the-this-profile-travels-in-the-sites-own-data)). Natively only a site with data of its own has observations, since `arrayProfile()` is null without it, so a record whose `this` fields are nonzero comes from such a site, and the consumer's site gets data of its own again with the producer's modes and flags. A profile without observations makes no site holding, and a tail call that ran without any comes back with the empty profile `ensureSlow` creates.

A seen site in `Init` links monomorphically at its first consumer call: an `ExecutedOnce` site gets its own data, seen, from `handleUnlinkedCall` (section 3.2), and `linkFor` prepares the callee's CB, as it does on every trip, and with the seen bit set calls `linkMonomorphicCall`. The callee's CB is created by that call, never by the import. A `NotExecuted` site, and an inline site whose record is empty, takes the native first and second trips, as it would in the producer.

The recaptured property-IC record of r, with `d = restoredPropertyIC(r)`, keeps r's `accessType` and `numberOfCoolDowns`, takes `learningBits` from `d.everConsidered`, `d.sawNonCell`, `d.tookSlowPath` and `d.resetByGC` and `repatchCount` from `d`, has `caseCount` 0, and has `stateBits` holding `holdsGaveUp` when `d.givenUp` and `canBeMegamorphic` when `d.foldsAtFirstCase`, with `megamorphicCaseListed` clear. The recaptured call-link record of r at a site of opcode `op`, with `d = restoredCallLink(op, r)`, is zero when `d.data` is `NotExecuted`, holds only the seen bit when `d.data` is `ExecutedOnce`, and otherwise has mode `Virtual` when `d.isVirtual` and `Init` otherwise, and `d`'s seen bit, history bits, varargs maximum and `this` fields.

For every record r and opcode `op`, `restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r)` and `restoredCallLink(op, recapturedCallLink(op, r)) == restoredCallLink(op, r)`: a recaptured record lists no case, so the rewound `repatchCount` stays; it holds `*GaveUp` exactly when the site was given up, so `givenUp` and `tookSlowPath` come back; its only fold fact is `canBeMegamorphic`, set exactly when `foldsAtFirstCase`, which already excludes `givenUp`; a recaptured call link is empty exactly when r is, holding exactly when r is, and keeps `Virtual` and `clearedByVirtual` on a virtual site, the seen bit on any other site that is not empty, and the `this` fields wherever `op.hasThisProfile`, which are zero on a site that is not `Own` since observations make a site holding.

### 6.2 Prepare (`jitcache/ics/ICRestore.h`)

`prepareBaselineICs(section, preparedCode, codeBlock, strict)` returns views into the section and, with strict on, validates it first. `preparedCode` is the `BaselineJITCode` the Image lane's `PreparedImage::code()` returns before `commit`, the object `commit` returns and setup installs, so the molds that prepare checks are the ones setup turns into ICs (R-IMG-1; [history](SPEC-ics-history.md#prepare-reads-the-prepared-baselinejitcode)). Prepare writes nothing anywhere: not to the CB, the prepared code, the payload or any global (I1). The integrator keeps the payload alive and unchanged until `attachPropertyICState` returns (R-INT-4).

In normal mode prepare only parses the section (section 4.5) and computes the group starts from its group table. With `StrictChecks::Yes` it runs the check functions declared below in this order and stops at the first failure, which is invalid material (THREAD Session): `parseSection` (A1 to A6), `checkMoldPairing` with `preparedCode.m_unlinkedPropertyInlineCaches.span()` (A7), `checkMetadataLayout` with `callLinkSiteCounts(codeBlock)` (A8), `checkMolds` over the same span (S1) and `checkNewbornCodeBlock` (S2). S1 and S2 check the two assumptions restore makes about the consumer's objects:

- S1 (`MoldMegamorphicBit`). No mold of the prepared code has `canBeMegamorphic` set, so the IC's bit after attach is attach's alone, which the round trip of section 6.1 relies on. (Nothing sets `UnlinkedPropertyInlineCache::canBeMegamorphic` for a baseline mold; `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` copies it.)
- S2 (`NewbornCallLinks`). The newborn CB matches what linking leaves, which seeding writes over field by field: `jitType()` is `None`, and each call-link site `forEachCallLinkSite` visits is as linking left it. A lazy site has no data of its own (`LazyCallLinkInfo::get()` is null) and is not executed once. Every lazy site but an `op_tail_call` one is never-executed (`LazyCallLinkInfo::hasNeverExecuted`), as `setNeverExecuted` left it; an `op_tail_call` site is not, which leaves the not-executed tail-call data `setTailCallNotExecuted` gave it, the only other data linking gives. An inline site has `type()` `DataOnly`, `callType()` equal to `CallLinkInfo::callTypeFor` of its opcode, mode `Init`, the seen, closure, GC and virtual bits clear, a zero varargs maximum, no stub, no last-seen callee and `isOnList()` false. A CB without a metadata table has no site to check. A CB that ran in the LLInt fails it, so S2 also re-checks THREAD Restoration's rule that such a CB receives no imported state, which the integrator's install points guarantee.

```cpp
namespace JSC::JITCache::ICs {

// The strict checks prepare composes. Each returns the first failure it finds. The first three
// read only their arguments and need no VM; tests build molds as plain
// BaselineUnlinkedPropertyInlineCache values.
std::optional<Invalid> checkMoldPairing(const SectionView&, std::span<const BaselineUnlinkedPropertyInlineCache> molds);
std::optional<Invalid> checkMetadataLayout(const SectionView&, const CallLinkSiteCounts&);
std::optional<Invalid> checkMolds(std::span<const BaselineUnlinkedPropertyInlineCache>);
std::optional<Invalid> checkNewbornCodeBlock(CodeBlock&);

class PreparedBaselineICs {
public:
    std::span<const PropertyICRecord> propertyICs() const { return m_propertyICs; }
    std::span<const CallLinkRecord> callLinks() const { return m_callLinks; }
    // Index in callLinks() of the first record of each canonical position's group: the sum of
    // the siteCounts of the groups before it, so a position without a group has a count of zero.
    const CallLinkSiteCounts& callLinkGroupStarts() const { return m_callLinkGroupStarts; }
    Summary summary() const { return m_summary; }

private:
    friend std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t>, const BaselineJITCode&, CodeBlock&, StrictChecks);
    friend void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);     // both read m_codeBlock in their assertions
    friend void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

    std::span<const PropertyICRecord> m_propertyICs; // borrowed from the payload
    std::span<const CallLinkRecord> m_callLinks;     // borrowed from the payload, canonical order
    CallLinkSiteCounts m_callLinkGroupStarts { };    // computed from the group table
    Summary m_summary;
#if ASSERT_ENABLED
    const CodeBlock* m_codeBlock { nullptr };        // seed and attach assert they receive the prepared CB
#endif
};

// preparedCode is PreparedImage::code(), read before commit; prepare keeps no reference to it.
std::expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks);
void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);
void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

}
```

`PreparedBaselineICs` owns nothing, so a dropped preparation (a baked-fact mismatch in another lane, a failed `shouldJIT` gate) needs no cleanup, and the next newborn CB of the UCB prepares again.

### 6.3 Seeding call-link history

```
vm = codeBlock.vm()
forEachCallLinkInstruction(codeBlock, (site, instruction)):   // visits nothing when the CB has no metadata table
    k = prepared.callLinkGroupStarts()[site.canonicalPosition] + site.metadataID
    d = restoredCallLink(site.opcode(), prepared.callLinks()[k])
    switch d.data:
        NotExecuted: continue
        ExecutedOnce: site.lazyInfo->setExecutedOnce(vm); continue
        Own: info = codeBlock.ensureCallLinkInfoAt(instruction)
        Inline: info = *site.inlineInfo
    if d.isVirtual: info.setVirtualCall(vm)
    else if d.seenOnce: info.setSeen()
    if d.hasSeenClosure: info.setHasSeenClosure()
    if d.clearedByGC: info.setClearedByGC()
    if d.clearedByVirtual: info.setClearedByVirtual()
    if d.maxArgumentCountIncludingThisForVarargs: info.updateMaxArgumentCountIncludingThisForVarargs(d.maxArgumentCountIncludingThisForVarargs)
    if d.data == Own and site.opcode().hasThisProfile:
        site.lazyInfo->arrayProfile()->restoreAccumulatedState(d.thisArrayModes, OptionSet<ArrayProfileFlag>::fromRaw(d.thisArrayProfileFlags))
```

Section 4.2 makes record k the site's: within its group, records follow metadata IDs. R-UCB-1 makes the groups' site counts equal the CB's entry counts and gives each instruction the producer's metadata ID, so k stays inside the site's group; A8 verifies the counts under strict, and debug builds assert that k is below the next canonical position's start or, for the last position, below `callLinks().size()`.

Seeding walks instructions, where every other walk of the lane walks metadata, because an own `CallSiteData` needs the site's bytecode index, which only its instruction knows: `CodeBlock::ensureCallLinkInfoAt` derives the call type and the code origin from the instruction exactly as `ensureSlow`'s native callers do ([history](SPEC-ics-history.md#seeding-walks-instructions)). It reaches `LazyCallLinkInfo::ensureSlow`, which creates the data unseen, since S2 left the site never-executed or on the tail-call data, publishes it after a `storeStoreFence` and reports it to the heap in batches of `MetadataTable::callSiteDatasPerReport`. That report can request a collection (`Heap::collectIfNecessaryOrDefer`), and under the install function's `DeferGCForAWhile` (SPEC-integrator.md section 7.4) it only records deferred work. `setExecutedOnce` writes the site's pointer only, and only for a site of kind `Lazy`, whose pointer S2 left at the never-executed data, as the native method asserts.

`setVirtualCall` comes first because the `reset` it calls clears the seen bit; it allocates nothing and takes no lock, since `VM::getCTIVirtualCall` returns one of the three virtual-call thunks that `JSC_FOR_EACH_VM_DEPENDENT_EAGER_COMMON_THUNK` generates at VM construction. Every setter runs on an inline or own `CallLinkInfo`, never on a shared one: `setSeen` and `setVirtualCall` release-assert that, but `setHasSeenClosure`, `setClearedByVirtual`, `setClearedByGC` and `updateMaxArgumentCountIncludingThisForVarargs` do not check it. Each write goes through `CallLinkInfo`'s or `LazyCallLinkInfo`'s own methods, field by field as THREAD Restoration requires, leaving the CB lane's profile fields in the same metadata entry untouched. The `this` profile is written last, through `LazyCallLinkInfo::arrayProfile()`, which is non-null once `ensureCallLinkInfoAt` has given the site its own data, and `ArrayProfile::restoreAccumulatedState` (SPEC-cb.md E1, R-CB-3), which writes the modes and the flags and asserts that both structure samples are empty, as `ensureSlow` creates them (`new CallSiteData()`, whose `StructureID`s start at zero). Seeding takes no lock: native linking writes these fields without one, and nothing else reaches the newborn CB.

### 6.4 Attaching property-IC state

```
ConcurrentJSLocker locker(codeBlock.m_lock)
jitData = codeBlock.baselineJITData()
ASSERT(prepared.propertyICs().size() == the number of ICs in jitData)   // strict checked it before anything was written
for i in 0 ..< prepared.propertyICs().size():
    ic = jitData->propertyCache(i)
    r = restoredPropertyIC(prepared.propertyICs()[i])
    ic.everConsidered = r.everConsidered
    ic.sawNonCell = r.sawNonCell
    ic.tookSlowPath = r.tookSlowPath
    ic.resetByGC = r.resetByGC
    ic.canBeMegamorphic = ic.canBeMegamorphic || r.foldsAtFirstCase
    ic.countdown = 0
    ic.repatchCount = r.repatchCount
    ic.numberOfCoolDowns = r.numberOfCoolDowns
    if r.givenUp: ic.m_slowOperation = gaveUpOperationFor(ic.accessType)
```

Attach indexes the ICs by mold index, which the Image lane preserves (SPEC-image.md R-ICS-1), and A7 checks the count under strict. A restored `canBeMegamorphic` stays set for the IC's life, since `PropertyInlineCache::reset` leaves it alone, as it leaves a mold's bit: if the site's first consumer case cannot fold and is later reset, the fold rules still apply from the next first case. The shape group and the handler chain stay as installation built them ([history](SPEC-ics-history.md#the-shape-group-stays-as-installation-builds-it)), so before any visit the status classes read a restored IC as follows:

- One the producer considered reads like a native IC after a reset, considered and empty: `PropertyInlineCache::summary` answers `Simple`, and `PutByStatus::computeForPropertyInlineCache` turns `Simple` with `m_cacheType` `Unset` into `LikelyTakesSlowPath`, the generic put THREAD Caches expects for a put site whose IC had cases.
- One that gave up beside its cases is such an IC, `Simple` when its captured `tookSlowPath` is clear and `TakesSlowPath` when it is set.
- One the producer never considered answers `NoInformation`, as a cold native IC does.
- A given-up IC answers `TakesSlowPath`, with `tookSlowPath` set.

No cell is written, so no barrier is needed.

### 6.5 Postconditions

- I1. Prepare writes nothing and allocates nothing.
- I2. Seed and attach cannot fail. Attach allocates nothing; seeding allocates one `CallSiteData` per site whose derivation gives `Own`, through the native `ensureSlow`, and no cell.
- I3. After seeding, every call-link site k has the `data` state of `restoredCallLink(op, record k)`. A site whose state is `Inline` or `Own` has the mode, seen bit, the three history bits and the varargs maximum of that derivation, and callee fields as linking or `ensureSlow` left them, or the mask and virtual-call thunk `setVirtualCall` wrote. An `Own` site whose opcode has a `this` profile has the derivation's `thisArrayModes` and `thisArrayProfileFlags` in its own profile, with both structure samples empty. The CB holds exactly as many own `CallSiteData`s (`MetadataTable::numberOfOwnCallSiteDatas()`) as there are `Own` sites.
- I4. After attaching, every IC i has the learning fields of `restoredPropertyIC(record i)`, `canBeMegamorphic == mold.canBeMegamorphic || foldsAtFirstCase`, the `*GaveUp` operation exactly when `givenUp`, and every other field as installation left it.
- I5 (round trip). With no mold carrying `canBeMegamorphic` (S1), capturing the CB after attach and before any of its sites runs yields `recapturedPropertyIC(record i)` for every IC i and `recapturedCallLink(op, record k)` for every call-link site k, and each recaptured record derives the same consumer state as the record it came from (section 6.1). A ConsumerProducer whose `delta` recaptures an imported body before some of its sites run therefore commits, whenever that recapture wins THREAD's scoring, a body that restores the same state at those sites, a fold not yet replayed included (T10). The twin check verifies the capture on every import (section 11.1), and T7 checks the derivation for every record ([history](SPEC-ics-history.md#records-hold-producer-facts)).

## 7. Native edits

Each edit names its function and file. All are in this repository; the lane changes nothing in `~/bun`, because no Bun code reads or writes property-IC learning state, call links or the `super_construct` cache. The lazy call sites need no edit: capture, S2 and seeding use public native members (`LazyCallLinkInfo::get`, `arrayProfile`, `hasExecutedOnce`, `hasNeverExecuted` and `setExecutedOnce`, `CodeBlock::ensureCallLinkInfoAt`, `MetadataTable::numberOfOwnCallSiteDatas` and `ArrayProfile::observedArrayModes`). The `this` profile also needs `ArrayProfile::arrayProfileFlags` and `ArrayProfile::restoreAccumulatedState`, which the CB lane adds for every array profile (SPEC-cb.md E1) and this lane calls without editing (R-CB-3). This lane edits nothing in `bytecode/ArrayProfile.h`, where the CB lane owns class `ArrayProfile` and the UCB lane class `UnlinkedArrayProfile`.

### E1. The `super_construct` store (jit/JITCall.cpp, `JIT::compileOpCall`)

The branch for `op_super_construct` and `op_super_construct_varargs` loads `new.target` into `BaselineJITRegisters::Call::callTargetGPR` and the cached callee into `BaselineJITRegisters::Call::callLinkInfoGPR`, then stores back the cached callee, so the cache never changes. The LLInt's handlers, `op_super_construct` in llint/LowLevelInterpreter.asm and `op_super_construct_varargs` in llint/LowLevelInterpreter64.asm, store `new.target` into an empty cache and `SeenMultipleCalleeObjects` into a cache holding another cell, and the varargs form skips a `new.target` that is not a cell (`loadConstantOrVariableCell`). The branch becomes:

```cpp
} else if constexpr (Op::opcodeID == op_super_construct || Op::opcodeID == op_super_construct_varargs) {
    constexpr GPRReg newTargetGPR = BaselineJITRegisters::Call::callTargetGPR;
    constexpr GPRReg cachedCalleeGPR = BaselineJITRegisters::Call::callLinkInfoGPR;
    loadPtr(calleeFrameLowWordSlot(CallFrameSlot::thisArgument), newTargetGPR);
    JumpList done;
    if constexpr (Op::opcodeID == op_super_construct_varargs)
        done.append(branchIfNotCell(newTargetGPR));
    loadPtrFromMetadata(bytecode, Op::Metadata::offsetOfCachedCallee(), cachedCalleeGPR);
    done.append(branchPtr(Equal, newTargetGPR, cachedCalleeGPR));
    auto store = branchTestPtr(Zero, cachedCalleeGPR);
    move(TrustedImmPtr(JSCell::seenMultipleCalleeObjects()), newTargetGPR);
    store.link(this);
    storePtrToMetadata(newTargetGPR, bytecode, Op::Metadata::offsetOfCachedCallee());
    done.link(this);
}
```

Both registers are free here: the call-link load that follows overwrites `callLinkInfoGPR` (`materializePointerIntoMetadata` of the entry's `DataOnlyCallLinkInfo` for `op_super_construct_varargs`, `loadPtrFromMetadata` of the site's `CallSiteData` pointer at `LazyCallLinkInfo::offsetOfData()` for `op_super_construct`), and `CallLinkInfo::emitFastPathImpl` reloads `callTargetGPR` before using it. The store goes through `metadataTableRegister` at a layout offset and the stored constant is the literal 1, so the edit adds no reference that needs an image fixup (SPEC-image.md R-ALL-1). Like the LLInt, it issues no write barrier ([history](SPEC-ics-history.md#e1-stores-newtarget-as-the-llint-does)); `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` clears a dead cached callee in baseline CBs too, because `CodeBlock::reconcileWeakReferencesAtGCEnd` calls it whenever `JITCode::couldBeInterpreted(jitType())`, which includes `BaselineJIT`.

The fix changes native behavior for every CB while it runs baseline code, under default options and with JITCache off. A CB born in baseline (door 3, `useLLInt` off, imports) kept an empty cache and now fills it. A CB that tiered up from the LLInt kept the LLInt's last answer and now keeps updating it, so a `new.target` that first differs after tier-up moves the cache to `SeenMultipleCalleeObjects`. Either way the DFG's `ByteCodeParser` cases for both opcodes freeze a callee and emit `CheckIsConstant` only when the code has seen exactly one, as for a CB that never left the LLInt. T2 tests both paths and B5 measures them.

The template is emitted inside `JIT::compileAndLinkWithoutFinalizing`, on a JIT worker inside the plan's safepoint or on the requesting thread, holding no lock and touching no cell; the emitted store runs on the VM's thread while JS runs and, like the LLInt's, takes no lock. E1 touches only this branch, which THREAD Execution gives this lane, and the lane applies it itself (SPEC-image.sites.md section E).

### E2. `CallLinkInfo::setClearedByGC` (bytecode/CallLinkInfo.h)

A new public inline member beside `setClearedByVirtual`:

```cpp
void setClearedByGC()
{
    m_clearedByGC = true;
}
```

`m_clearedByGC` is protected and has no setter; its only writer is `CallLinkInfo::reconcileWeakReferencesAtGCEnd`. The setter's only caller is `seedCallLinkHistory`, in the context of section 6, on an inline or own `CallLinkInfo`. Like `setClearedByVirtual` beside it, the setter does not assert `!isSharedByUnlinkedCallSites()`.

### E3. The `*GaveUp` operation per access type (bytecode/Repatch.h, bytecode/Repatch.cpp)

A new declaration in Repatch.h under `ENABLE(JIT)`, defined in Repatch.cpp after the existing `appropriate*GaveUpFunction` helpers (`appropriatePutByGaveUpFunction` is file-static, the other two file-level `inline`), which it calls ([history](SPEC-ics-history.md#one-megamorphic-predicate-one-give-up-table)):

```cpp
// The *GaveUp operation a property IC of this access type holds once it gives up.
CodePtr<OperationPtrTag> NODELETE gaveUpOperationFor(AccessType);
```

The pointer is retagged to `OperationPtrTag` the way `repatchSlowPathCall` retags before storing into `m_slowOperation`, so `==` against `m_slowOperation` is exact. Capture compares with it (section 5.2) and attach installs it (section 6.4). For get, put and `in` access types it maps the access type to its `GetByKind`, `PutByKind` or `InByKind` and returns `appropriateGetByGaveUpFunction`, `appropriatePutByGaveUpFunction` or `appropriateInByGaveUpFunction` of that kind; for the others it returns the operation `repatchDeleteBy`, `repatchHasPrivateBrand`, `repatchCheckPrivateBrand`, `repatchSetPrivateBrand` and `repatchInstanceOf` install. The table is normative:

| `AccessType` | `*GaveUp` operation |
|---|---|
| `GetById` | `operationGetByIdGaveUp` |
| `GetByIdWithThis` | `operationGetByIdWithThisGaveUp` |
| `GetByIdDirect` | `operationGetByIdDirectGaveUp` |
| `GetByVal` | `operationGetByValGaveUp` |
| `GetByValWithThis` | `operationGetByValWithThisGaveUp` |
| `PutByIdStrict` | `operationPutByIdStrictGaveUp` |
| `PutByIdSloppy` | `operationPutByIdSloppyGaveUp` |
| `PutByIdDirectStrict` | `operationPutByIdDirectStrictGaveUp` |
| `PutByIdDirectSloppy` | `operationPutByIdDirectSloppyGaveUp` |
| `PutByValStrict` | `operationPutByValStrictGaveUp` |
| `PutByValSloppy` | `operationPutByValSloppyGaveUp` |
| `PutByValDirectStrict` | `operationDirectPutByValStrictGaveUp` |
| `PutByValDirectSloppy` | `operationDirectPutByValSloppyGaveUp` |
| `DefinePrivateNameByVal` | `operationPutByValDefinePrivateFieldGaveUp` |
| `DefinePrivateNameById` | `operationPutByIdDefinePrivateFieldStrictGaveUp` |
| `SetPrivateNameByVal` | `operationPutByValSetPrivateFieldGaveUp` |
| `SetPrivateNameById` | `operationPutByIdSetPrivateFieldStrictGaveUp` |
| `InById` | `operationInByIdGaveUp` |
| `InByVal` | `operationInByValGaveUp` |
| `HasPrivateName` | `operationHasPrivateNameGaveUp` |
| `HasPrivateBrand` | `operationHasPrivateBrandGaveUp` |
| `InstanceOf` | `operationInstanceOfGaveUp` |
| `DeleteByIdStrict` | `operationDeleteByIdStrictGaveUp` |
| `DeleteByIdSloppy` | `operationDeleteByIdSloppyGaveUp` |
| `DeleteByValStrict` | `operationDeleteByValStrictGaveUp` |
| `DeleteByValSloppy` | `operationDeleteByValSloppyGaveUp` |
| `GetPrivateName` | `operationGetPrivateNameGaveUp` |
| `GetPrivateNameById` | `operationGetPrivateNameByIdGaveUp` |
| `CheckPrivateBrand` | `operationCheckPrivateBrandGaveUp` |
| `SetPrivateBrand` | `operationSetPrivateBrandGaveUp` |

The function switches over every `AccessType` without a `default`, so a new access type fails to compile until the table covers it. It is pure: it reads function addresses, runs no destructor and frees nothing (hence `NODELETE`, which the helpers it calls carry too), takes no lock and may run on any thread; this lane calls it under `CodeBlock::m_lock` in capture and attach. An entry that disagreed with the engine would only cost speed: capture would record a given-up site as not given up, and the consumer would restore it as a reset leaves it, so native caching would give up again ([history](SPEC-ics-history.md#checks-run-only-under-strict-and-copy-no-native-table)).

### E4. `AccessCase::isMegamorphic` (bytecode/AccessCase.h, bytecode/AccessCase.cpp, bytecode/InlineCacheCompiler.cpp)

The file-static `bool NODELETE isMegamorphic(AccessCase::AccessType)` in InlineCacheCompiler.cpp moves, body and annotation unchanged, to a public `static bool NODELETE isMegamorphic(AccessType)` of `AccessCase`, defined in AccessCase.cpp. Its four uses in InlineCacheCompiler.cpp become `AccessCase::isMegamorphic`: in `InlineCacheCompiler::tryFoldToMegamorphic`, in the `finishCodeGeneration` lambda of `InlineCacheCompiler::compile` (spelled `JSC::isMegamorphic` there), and in the `finishPreCompiledCodeGeneration` and `finishCodeGeneration` lambdas of `InlineCacheCompiler::compileOneAccessCaseHandler`. Behavior is unchanged; capture classifies with the same predicate the fold uses, instead of a copy ([history](SPEC-ics-history.md#one-megamorphic-predicate-one-give-up-table)). The predicate is pure; its native callers run on the VM's thread under the IC's `CodeBlock::m_lock`, as before.

### E5. Executable-allocation fault calls in IC compilation (bytecode/InlineCacheCompiler.cpp)

When an IC stub or handler cannot get executable memory, THREAD Failures raises a fault before the failure's effects are written, and THREAD Execution gives this lane its call sites, the three `didFailToAllocate()` branches of `InlineCacheCompiler`. The entry point and its parameters are the integrator's (R-INT-6).

IC compilation allocates executable memory with `JITCompilationCanFail` at three sites, and each turns a failure into a give-up: `InlineCacheCompiler::compile` (repatching ICs, FTL) and `InlineCacheCompiler::compileOneAccessCaseHandler` (handler ICs, baseline and DFG) return `AccessGenerationResult::GaveUp` after `LinkBuffer::didFailToAllocate()`, and `InlineCacheCompiler::compileGetByDOMJITHandler` returns an empty code ref, which `compileOneAccessCaseHandler` turns into `GaveUp`. Every other `LinkBuffer` in the file is a must-succeed thunk, which crashes natively and keeps JSC behavior. At each of the three `didFailToAllocate()` branches, before returning, the code calls, with `JITCacheFaults.h` (R-INT-6) included by file name and `vm` the `VM&` the function holds:

```cpp
JITCache::didFailExecutableAllocation(vm, JITCache::ExecutableAllocationSite::InlineCacheHandler);
```

The call runs on the VM's thread, inside an IC slow operation that holds the caller CB's `m_lock` through a `GCSafeConcurrentJSLocker` (`tryCacheGetBy` and its siblings hold it across `PropertyInlineCache::addAccessCase`), with heap access and GC deferred. It runs before the slow path writes the `*GaveUp` operation, so the fault precedes the effect; after it the VM neither captures nor imports, so no capture can contain that give-up. Native behavior after the call is unchanged.

## 8. Interfaces and requirements

### 8.1 What the lane provides

Other parts call only these functions, declared in sections 4.4, 5.6, 6.1, 6.2, 7 and 11.1:

| function | caller | purpose |
|---|---|---|
| `baselineICsSectionSize` | integrator, capture glue | size to charge and allocate |
| `summarizeBaselineICs` | integrator, scoring | candidate's IC count and polymorphic bit |
| `captureBaselineICs` | integrator, capture glue | the section bytes, their summary and the polymorphic bit for the CB lane |
| `readBaselineICsSummary` | integrator, scoring at the key's first scoring | the saved body's IC count |
| `prepareBaselineICs` | integrator, install function | views into the payload, validated under strict |
| `description` | integrator, capture glue, scoring and install function | the token of a failed check, which `status` shows after `ics.` |
| `seedCallLinkHistory` | integrator, install function | call-link seeds |
| `attachPropertyICState` | integrator, install function | property-IC seeds |
| `restoredPropertyIC`, `restoredCallLink`, `recapturedPropertyIC`, `recapturedCallLink` | twin checks, tests | the derivation and its round trip |
| `parseSection`, `isPolymorphicPropertyIC` | integrator, bench loop | reading committed sections for B4 |
| `snapshotBaselineICs`, `checkRestoredBaselineICs` | integrator's test harness, test builds only | section 11 |
| `functionSnapshotBaselineICs` | integrator's jsc shell registration, test builds only | the shell function `jitcacheICsSnapshot` (section 11.1) |
| `gaveUpOperationFor`, `AccessCase::isMegamorphic`, `CallLinkInfo::setClearedByGC` | native code and this lane | E2 to E4 |

The check functions of section 6.2, the walkers of section 4.6 and the record readers of section 5.6 are the lane's own; outside the lane's code only its tests call them.

### 8.2 Requirements on the integrator

- R-INT-1. The type id of the ICs section, with tier 1 for `ICsBaseline`; a later tier's section takes the same type id with its own tier (SPEC-integrator.md section 6.1). The lane's bytes go into the body file unchanged and come back as a `std::span<const uint8_t>` of exactly the written length; no alignment is required.
- R-INT-2. Capture glue. For each CB the glue captures, at either capture point: call `baselineICsSectionSize`, charge that many bytes to the producer limit (THREAD Session), allocate them, call `captureBaselineICs` with the config's strict flag, and turn a `CaptureError` into a recording fault whose diagnostic names this lane and the check by its token (section 5.6). Call it before the CB lane's capture of the same CB, in the same pause, and pass `CaptureSummary::hasPolymorphicSite` to that capture (THREAD Restoration; SPEC-cb.md R-INT-5). The locks held on the call are those of L2.
- R-INT-3. Scoring. The candidate's `icSitesWithCases` and `hasPolymorphicSite` come from `summarizeBaselineICs` or from `captureBaselineICs`, called before the CB lane's `scoreLive` of the same CB in the same pause, and the bit goes to `scoreLive`, which scores the counter as SPEC-cb.md section 4.3 says (SPEC-cb.md R-INT-5). The saved body's count comes from `readBaselineICsSummary`, given the whole `ICsBaseline` section and the config's strict flag, and its `Invalid` is invalid material; the saved body's bit comes from the CB lane's summary (section 5.6).
- R-INT-4. Install glue. Call `prepareBaselineICs` after `prepareImage` succeeded and before `PreparedImage::commit`, while nothing has been written to the CB, with the section, `PreparedImage::code()` (R-IMG-1), the newborn CB and the config's strict flag. Turn an `Invalid` into invalid material whose diagnostic names this lane, the check by its token (section 4.4) and the site index, and then destroy the prepared image without `commit`, as the Image lane requires after any failed preparation, so the failure writes nothing to the CB (THREAD Restoration); support code the image's resolution generated stays, as the VM's first use of it would leave it (SPEC-image.md section 13). Keep the payload alive and unchanged until `attachPropertyICState` returns. Call `seedCallLinkHistory` after every prepare, the baked-fact comparison and the `shouldJIT` gate passed, inside the install function's `DeferGCForAWhile` and before `setupWithUnlinkedBaselineCode`, in either order with the CB lane's metadata seeds (R-CB-1); call `attachPropertyICState` right after `setupWithUnlinkedBaselineCode` returns and before the baseline counter is re-sliced. Never call either for a CB other than the prepared one, or after a failed prepare. Every baseline capture writes an `ICsBaseline` section, so a body file without one is invalid material when strict checks the directory, and normal mode trusts that it is there.
- R-INT-5. The eight option rows of options.md that name this lane, in the fixed-option table `start` checks (section 10).
- R-INT-6. The fault entry point the IC compiler calls (E5): `JSC::JITCache::didFailExecutableAllocation`, declared in `jitcache/session/JITCacheFaults.h`, whose parameters the integrator owns (THREAD Execution); this lane passes `ExecutableAllocationSite::InlineCacheHandler` beside the `VM&`. THREAD Execution requires it to be callable with `CodeBlock::m_lock` held; this lane calls it on the VM's thread with that lock held through a `GCSafeConcurrentJSLocker`, from any tier's IC compilation and in any VM (E5). It is a no-op for a VM `start` never configured, and otherwise raises the fault of THREAD Failures with the failing step `exec-alloc.ic-handler` (SPEC-integrator.md section 4.5).
- R-INT-7. Test builds (`ENABLE(JITCACHE_TWINS)`), for the JS tests of section 11.2 ([history](SPEC-ics-history.md#the-lane-owns-its-test-bindings-shapes)):
  1. Call `checkRestoredBaselineICs` right after `attachPropertyICState` returns, and report each mismatch as a twin failure.
  2. Register the lane's `JSC::JITCache::ICs::functionSnapshotBaselineICs` on the jsc shell's global object as `jitcacheICsSnapshot`, length 2, through a forwarding function of the integrator's harness, so that `jsc.cpp` includes no lane header. Section 11.1 defines its arguments and the object it returns.
  3. The shell functions `jitcacheDelta()` and `jitcacheStatus()`, in test builds only, with the behavior SPEC-integrator.harness.md section 5.2 gives them.
  4. The runner of SPEC-integrator.harness.md section 7 for `JSTests/jitcache/ics/`, in twins mode, with the oracle comparison of its section 7.6 that T6 relies on.
  5. `JSTests/jitcache/resources/jitcache.js`, shared by every lane's scripts, which reads the runner's `arguments` convention and wraps the functions of item 3 as section 11.2 describes.
- R-INT-8. The `Sources.txt` entries of section 13.
- R-INT-9. Test builds: a runner for the C++ tests, `testjitcache`, which compiles this lane's tests in `Source/JavaScriptCore/jitcache/tests/ics/`, with the framework of SPEC-integrator.harness.md section 8.1. The runner calls `JSC::initialize()` once before any test, because a test's VM needs the process-wide setup it performs; tests without a VM, T7's included, read no option and need nothing from it. A test that needs a VM receives a fresh `VM&`, created after `JSC::initialize()` with default options and without a `start` call, with its API lock held. The live tests (T11 to T15) build their own global objects and CBs (section 11.2), so the runner needs no lane-specific hook ([history](SPEC-ics-history.md#the-lane-owns-its-test-bindings-shapes)).

### 8.3 Requirements on the other lanes

- R-IMG-1 (Image lane). `const BaselineJITCode& PreparedImage::code() const` can be called before `commit`, with no effect outside the prepared objects, and returns the `BaselineJITCode` that `commit` will return and setup will install ([history](SPEC-ics-history.md#prepare-reads-the-prepared-baselinejitcode)). The returned code's `m_unlinkedPropertyInlineCaches` holds the producer's molds in mold order. SPEC-image.md sections 10.1 and 12.1 give the rest of the accessor's contract.
- R-UCB-1 (UCB lane). An imported or reused UCB has the producer's metadata layout: for each opcode with a `CallLinkInfo`, the same number of metadata entries and the same metadata IDs. THREAD Storage states this guarantee of the producer's index spaces; seeding relies on it, and A8 checks it under strict. SPEC-ucb.md section 9.1 meets it.
- R-CB-1 (CB lane). This lane owns the whole `CallSiteData` of every lazy site, its `this` profile included (THREAD Execution), and no other lane writes one. The CB lane's metadata seeds write profile fields only and never a call site's call link: no `m_callLinkInfo` of a metadata entry and no field of a `CallSiteData`. This lane in turn writes none of the fields SPEC-cb.md section 3.1 lists (its R-ICS-1). The two lanes' seeds are thus disjoint, and they may run in either order inside the seeding window. The CB lane's one access to a `CallSiteData` is a read: its summary counts the flags of each own `this` profile, read through `LazyCallLinkInfo::arrayProfile()` after this lane's capture or summary of the same CB in the same pause (R-INT-2, R-INT-3), and its state section holds no `this` profile. Every own `this` profile with a flag makes its site holding (section 6.1), so each flag of an own `this` profile that the summary counts travels in this lane's section, except one a drain adds between the two reads, which costs only precision (THREAD Capture); the flags of the UCB copy the summary ORs in travel in the UCB lane's feedback. Its baseline-counter re-slice comes after `attachPropertyICState`, as THREAD orders it, and touches no IC field. SPEC-cb.md sections 4.2 and 6.4 state the other side of this rule.
- R-CB-2 (CB lane). Its capture and `scoreLive` take `hasPolymorphicSite`, which the integrator passes from this lane (R-INT-2, R-INT-3); when it is set, they carry and score no baseline counter progress and mark the counter withheld, with `counterMode` `NotCarried` in `cb.summary` as in `cb.state`, so a saved body keeps the bit for THREAD Capture's tie-break (THREAD Restoration and Capture; section 5.6). SPEC-cb.md I16, sections 3.3, 4.3 and 4.4 and its R-INT-5 meet this.
- R-CB-3 (CB lane). Its E1 serves this lane's `this` profiles as well: `OptionSet<ArrayProfileFlag> ArrayProfile::arrayProfileFlags() const`, which capture calls under `CodeBlock::m_lock` (section 5.4), and `ArrayProfile::restoreAccumulatedState(ArrayModes, OptionSet<ArrayProfileFlag>)`, which seeding calls on the profile of data `ensureSlow` has just created for a newborn CB's site (section 6.3), with the bodies SPEC-cb.md section 9.1 gives. This lane adds no edit to `bytecode/ArrayProfile.h`, and its tasks 4 and 5 build on SPEC-cb.md task 1, which lands E1. SPEC-cb.md R-ICS-2 states the other side.

## 9. Locks, GC and threads

The lane has no lock of its own and starts no thread. Every lane function runs on the VM's thread with the API lock and heap access held.

- L1. The only lock the lane takes is `CodeBlock::m_lock`, through `ConcurrentJSLocker`, in `summarizeBaselineICs`, `captureBaselineICs` (which also reads each `this` profile under it, section 5.4), `attachPropertyICState` and `snapshotBaselineICs`; `checkRestoredBaselineICs` takes it through `snapshotBaselineICs` and then `captureBaselineICs`, never both at once. It is the innermost lock, never held for two CBs at once, and released before the function returns.
- L2. Callers hold no `CodeBlock::m_lock` (the lock is not recursive). A caller in `delta` may hold `CodeBlockSet::m_lock`, and the order `CodeBlockSet::m_lock` then `CodeBlock::m_lock` cannot invert, because native code never takes the set's lock while holding a `CodeBlock::m_lock` ([history](SPEC-ics-history.md#a-captures-consistency-comes-from-the-pause)). `CodeBlockSet::add` runs only in the two `CodeBlock` constructors, and `CodeBlockSet::remove` has no caller: dead CBs leave the set in `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks`, at the collector's End phase, without the lock. The other takers, `ConservativeRoots::add`, `VMTraps`, which jettisons under the set's lock, and `SamplingProfiler`, hold no `CodeBlock::m_lock` when they take it, and `VMInspector` only tries it.
- L3. Native readers of IC and call-link state on compiler threads take `CodeBlock::m_lock`, and attach writes under it, so no fence is needed. Seeding publishes each own `CallSiteData` through `ensureSlow`'s `storeStoreFence`, which compiler threads pair with the dependent load in `LazyCallLinkInfo::get()` and `arrayProfile()`, and writes the `this` profile after that publication; no other thread can read it then, because other threads reach the CB only through `installCode`, which stores and barriers after attach.
- L4. Apart from the `this` profiles, a capture sees one consistent state because every writer of what it reads runs on the VM's thread while JS runs or in a stopped-world GC phase; the lock does not provide it ([history](SPEC-ics-history.md#a-captures-consistency-comes-from-the-pause)). The VM-thread writers are the running code and its slow paths, mostly unlocked: `PropertyInlineCache::considerRepatchingCacheImpl` writes the learning fields before any lock is taken, the `*GaveUp` operations write `tookSlowPath` without it, and `repatchGetBy` and its siblings repoint `m_slowOperation` through `repatchSlowPathCall` after `tryCacheGetBy`'s `GCSafeConcurrentJSLocker` scope, which covers only the added case, has closed. `PropertyInlineCacheClearingWatchpoint::fireInternal` resets the IC under the lock on whichever thread fires its set, the VM's thread when a watched condition breaks while JS runs. A polymorphic call site's stub is a `GCAwareJITStubRoutine` that the VM's thread replaces or drops (`linkPolymorphicCall`, and `CallLinkInfo::reset` from `setVirtualCall`, `revertCall` and the unlinking `installCode` starts), that a collection's finalization unlinks when a slot's callee died (`CallLinkInfo::reconcileWeakReferencesAtGCEnd`), and that only a collection frees. A lazy site's `m_data` moves only on the VM's thread, in the slow paths of section 3.2, and once it holds data of the site's own it keeps it. A `this` profile is the exception: like every array profile, it is drained without a lock by concurrent markers (`CodeBlock::visitChildren`) and DFG compiler threads (`ByteCodeParser::getArrayMode`), so a capture can meet a drain midway, which equals a sample not yet drained (THREAD Capture). None of the other writers runs during a capture: JS is paused at both capture points, and GC finalization (`CodeBlock::reconcileWeakReferencesAtGCEnd`) cannot begin while the VM's thread holds heap access and reaches no stop point. Concurrent markers read the handler chain but write no learning field. The lock orders a capture only against readers on other threads: the status classes and `CallLinkStatus::computeFor` read under `CodeBlock::m_lock`.

  The lane writes no cell field and no `WriteBarrier`. It reads cells only through `listedAccessCases`, whose `AccessCase`s are ref-counted and stay alive under the lock, and through a polymorphic call site's stub, whose slots it only counts. In test builds `snapshotBaselineICs` also copies each `super_construct` cache's cell, which the shell function keeps valid by building its result under `DeferGC` (section 11.1). Neither capture nor restore can trigger a collection, which `ConcurrentJSLocker` asserts in debug builds: seeding's report of new `CallSiteData`s only records deferred work under the install function's deferral (section 6.3).
- L5. JIT worker threads and GC threads never call into the lane. The lane records nothing during compilation; everything it captures is read at a capture point.

## 10. Options

[options.md](../options.md) holds this lane's eight fixed rows, options that can change what transported state means and that Bun does not set, each with its reason, and the IC options that stay free because no state this lane carries depends on them ([history](SPEC-ics-history.md#the-ic-limit-options-are-fixed)). Beyond the reason its row gives, this lane depends on `useLazyLLIntCallLinkInfos` too: with it off, `link_callLinkInfo` would give every lazy site its own `CallSiteData` through `ensure`, so S2 would reject every newborn CB that has a lazy call site, and no producer site would be on the executed-once or never-executed data.

## 11. Verification

Everything this lane restores is history the engine cannot recompute, so under THREAD Verification its twin is the capture record passed through the derivation of section 6.1; the `super_construct` fix has a native twin, the LLInt's effect on the same cache. The lane records nothing extra in test builds, since its twins need only the section. Its check never skips itself, in either JIT mode, because it reads only the section and the fields of a CB that no other thread can reach before `installCode` (section 6; [history](SPEC-ics-history.md#ics-runs-keep-concurrent-jit-off)).

### 11.1 Twin API (`jitcache/ics/ICTwins.h`, `ENABLE(JITCACHE_TWINS)` only)

```cpp
namespace JSC::JITCache::ICs {

struct PropertyICSnapshot {
    AccessType accessType;
    unsigned bytecodeIndex;                 // codeOrigin's bytecode offset
    CacheType cacheType;
    PropertyInlineCacheSummary summary;     // PropertyInlineCache::summary under the CB lock
    bool holdsGaveUp;                       // m_slowOperation == gaveUpOperationFor(accessType)
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
    CallSiteDataState data;                 // Inline, or Own, ExecutedOnce or NotExecuted by get() and hasExecutedOnce()
    CallLinkInfo::Mode mode;                // this and the fields below as readCallLinkRecord reads them
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    uint32_t thisArrayModes;                // the own this profile's accumulated state, zero without one
    uint32_t thisArrayProfileFlags;
    bool hasStub;                           // false without a CallLinkInfo of the site's own
    bool hasLastSeenCallee;
    friend bool operator==(const CallLinkSnapshot&, const CallLinkSnapshot&) = default;
};

struct SuperConstructSnapshot {
    OpcodeID opcodeID;      // op_super_construct or op_super_construct_varargs
    unsigned metadataID;
    enum class State : uint8_t { Empty, Single, Multiple } state;
    JSCell* cachedCallee;   // the cell when Single, null otherwise; not a GC root
    friend bool operator==(const SuperConstructSnapshot&, const SuperConstructSnapshot&) = default;
};

struct BaselineICsSnapshot {
    JITType jitType;
    Vector<PropertyICSnapshot> propertyICs;         // IC order; empty unless the CB has BaselineJITData
    Vector<CallLinkSnapshot> callLinks;             // canonical order (forEachCallLinkSite)
    Vector<SuperConstructSnapshot> superConstructs; // forEachMetadataEntry over both opcodes
    friend bool operator==(const BaselineICsSnapshot&, const BaselineICsSnapshot&) = default;
};

struct TwinMismatch {
    enum class Site : uint8_t { PropertyIC, CallLink, Capture } site;
    uint32_t index;
    ASCIILiteral field;
    uint32_t expected;
    uint32_t actual;
};

BaselineICsSnapshot snapshotBaselineICs(CodeBlock&);
Vector<TwinMismatch> checkRestoredBaselineICs(const PreparedBaselineICs&, CodeBlock&);

// The jsc shell's jitcacheICsSnapshot(fn, kind), which the shell registers through a forwarding
// function.
JSC_DECLARE_HOST_FUNCTION(functionSnapshotBaselineICs);

}
```

`captureBaselineICs` and `snapshotBaselineICs` take every field a record holds from `readPropertyICRecord` and `readCallLinkRecord` (section 5.6), so a snapshot and a capture of the same state agree field for field. A snapshot's `data` is `Inline` for an inline site and, for a lazy site, `Own` when `LazyCallLinkInfo::get()` is non-null, `ExecutedOnce` when `hasExecutedOnce()` holds and `NotExecuted` otherwise. Its `this` fields are the record's: the accumulated modes and flags of an own `this` profile, never a structure sample, which only E1's assertion in `restoreAccumulatedState` checks. A CB without a metadata table snapshots with no call link and no `super_construct` cache.

`checkRestoredBaselineICs` runs right after attach, before any JS, so nothing changes the CB in between. It compares the CB's snapshot with postconditions I3 and I4, the count of own `CallSiteData`s included, then captures the CB with `StrictChecks::Yes` into a buffer of `baselineICsSectionSize` bytes, which test builds may allocate, and compares each record with `recapturedPropertyIC` or `recapturedCallLink` of the record it was restored from (I5); a `CaptureError`, or a strict `parseSection` of the captured bytes that fails, is reported as a `Capture` mismatch whose field is the check's token (sections 4.4 and 5.6).

`jitcacheICsSnapshot(fn, kind)` takes a JS function whose executable is a `FunctionExecutable` and `kind`, `"call"` (the default) or `"construct"`; anything else throws a `TypeError`. It snapshots the executable's CB of that kind, or that CB's `baselineAlternative()` when the CB is a DFG or FTL one, and returns `undefined` when the executable has no CB of that kind. Otherwise it returns `{ jitType, propertyICs, callLinks, superConstructs }`, built only from the snapshot inside the `DeferGC` scope that took it: `cachedCallee` is a raw pointer in a vector no collection scans, and its cache is weak (`CodeBlock::reconcileLLIntInlineCachesAtGCEnd` clears a dead one, and the sweep after it can free the cell), but under the deferral no collection reaches its End phase, so the cell stays valid until the result holds it ([history](SPEC-ics-history.md#the-lane-owns-its-test-bindings-shapes)). The result has these fields:

- `jitType` as `JITCode::typeName` spells it (`"None"`, `"LLInt"`, `"Baseline"`).
- Each `propertyICs` element has the fields of `PropertyICSnapshot` under the same names: `accessType` by its enumerator name (the names `JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE` lists, such as `"GetById"`); `cacheType` and `summary` by their enumerator names (`"Unset"`, `"Stub"`, `"NoInformation"`, `"TakesSlowPath"`); the rest, `holdsGaveUp` included, as numbers and booleans.
- Each `callLinks` element has `opcode` (`opcodeNames`, such as `"op_call"`), `metadataID`, `data` (`"Inline"`, `"NotExecuted"`, `"ExecutedOnce"`, `"Own"`), `mode` (`"Init"`, `"Monomorphic"`, `"Polymorphic"`, `"Virtual"`) and the other fields of `CallLinkSnapshot` under their names, `thisArrayModes` and `thisArrayProfileFlags` as numbers.
- Each `superConstructs` element has `opcode`, `metadataID`, `state` (`"Empty"`, `"Single"`, `"Multiple"`) and `cachedCallee`, the cell when `Single` and `null` otherwise.

The enumerator names come from switches without `default` in `ICTwins.cpp`, so a new enumerator breaks the build until it is named.

### 11.2 Tests

R-INT-7 supplies the shell functions and the sequence runner, R-INT-9 the C++ test runner. Every run the runner starts has strict on, and a C++ test may also call a function's normal-mode path directly, as T7 and T12 item 4 do (SPEC-integrator.harness.md section 7.3). Every test but T1 needs an `ENABLE(JITCACHE_TWINS)` build and runs in each such build the integrator makes, on x86_64 and ARM64, `debug-local` (ASan and LSan) among them; T1 runs in `debug-local` and `release-local` like any stress test.

Conventions of the JS tests in `JSTests/jitcache/ics/`:

- Each script loads `resources/ics.js` with `load("./resources/ics.js", "caller relative")`, and `ics.js` loads `JSTests/jitcache/resources/jitcache.js`, which every lane's scripts share. No file of `JSTests/jitcache/ics/` reads `arguments` or calls the integrator's shell functions of R-INT-7 itself: the shared file reads the runner's `arguments` convention (`role()`, `sequence()`, `scratchPath(name)`) and wraps those functions (`status()` for `jitcacheStatus`, `delta()` for `jitcacheDelta`), each wrapper answering `null` where its function is missing or the role is `Off`; it also holds the assertions `fail`, `check` and `checkSame`, which throw. `ics.js` binds the role, sequence, assertion and status functions as globals and is the only file that names `jitcacheICsSnapshot`. It provides `snapshot(fn, kind)`, `underTest(...bodies)`, `toBaseline(fn, kind)`, `saveJSON(name, value)` and `loadJSON(name)` over the scratch directory (through the shell's `writeFile` and `readFile`), and `delta(...bodies)`. `underTest` passes each body to `noDFG` and `noInline`. `toBaseline` calls the body with `skip`, through `new` when `kind` is `"construct"`, until its snapshot's `jitType` is `"Baseline"`, and throws after 100,000 calls. `delta` takes bodies, each a function or a `[fn, kind]` pair, throws unless each one's snapshot has `jitType` `"Baseline"` and a property IC with a nonzero `caseCount`, and then calls the shared `delta()`.
- Scripts follow SPEC-integrator.md R-ALL-4 and the runner's conventions (SPEC-integrator.harness.md sections 7.2, 7.3 and 7.6), with three clauses of this lane. `resources/ics.js` keeps no role-dependent state at its top level. Under `Off`, which T2's sequences pass as well as the oracle's runs, a script also leaves out `saveJSON` and `loadJSON`. T2's assertions read only native state (the `super_construct` cache and the JIT type), so they are native ones, which R-ALL-4 runs in every role.
- Each body under test is passed to `underTest`, and so to `noDFG` and `noInline`, before its first call, in every run. `noDFG` keeps its baseline CB its executable's `replacement()` until `delta()` and makes its capability class, a baked fact, the same in every run. `noInline` (`ScriptExecutable::setNeverInline`) keeps a DFG compile of a caller from inlining it, which `DFG::inlineFunctionForCapabilityLevel` would otherwise allow, since it reads `isInliningCandidate` and not the body's capability class; so every call runs the body's own CB, and no compiler thread reads or drains its profiles.
- Each body under test takes a leading `skip` argument and returns at once when it is true. A producing run brings it to baseline with `toBaseline` before driving its sites, so every site it drives runs in baseline code; T9 states the one exception. A Consumer creates and installs a body's CB with one `skip` call, which reaches no IC or call site, and snapshots it before any visit.
- Each body whose `delta` capture a later run checks has an anchor: a property-IC site that the producing run gives a case after `toBaseline` and that still lists it when the body is passed to `delta`, which checks it. A state without a case (cold, considered and empty, given up after a fold, non-cell, reset by GC, every call-link state) is therefore tested in a body that also holds an anchor. A body without a metadata table anchors on its own property IC, because an added `get_by_id` would give it a table. T10 runs two bodies without an anchor on purpose, each in one run, to test the scoring itself.
- A `call`, `call_ignore_result` or tail-call site whose restored state a test names is a plain call such as `f()`, whose `this` is not a cell, unless the test gives it a `this` profile on purpose: a cell `this` gives the site's own data a profile with observations once a drain runs, which brings the site back `Own` (section 6.1).
- Scripts keep alive until `delta()` every structure a site's cases name, through objects that still have it, and every callee a call site linked, except those a test kills on purpose before it snapshots, so no collection between a snapshot and `delta()` changes the state both read.
- The `Producer` run of every sequence whose `delta` captures a later run checks declares `--useConcurrentGC=false`, for the anchor argument below.

The anchor decides which capture the consumer imports ([history](SPEC-ics-history.md#which-capture-the-consumer-imports-in-the-tests)). A body's first capture, at the end of `BaselineJITPlan::finalize`, counts no IC site with cases (section 5.3), and a `delta` capture replaces it only by beating it under THREAD's scoring. `noDFG` makes the CB `CannotCompile`: `functionNoDFG` in jsc.cpp sets `ScriptExecutable::setNeverOptimize`, which `DFG::mightCompileFunctionForCall` reads, so `JIT::compileAndLinkWithoutFinalizing` clears `m_shouldEmitProfiling` and `m_canBeOptimized`. The baseline code then writes no value, array or argument profile, and `JIT::emit_op_enter` and `JIT::emit_op_loop_hint` add nothing to the baseline counter, which moves only when `op_enter` takes its slow path for a trap or for the CB's write barrier (`JIT::op_enter_handlerGenerator`), at times collections decide. Richness counts no call-link or IC learning state, so a body whose learned state lies only in call links or in ICs without a case could tie with its finalize capture, and collection timing would decide whether `delta` replaced it. An anchored body's `delta` capture counts at least one IC site with cases against none, and it is never poorer than the finalize capture of the same CB: predictions, array flags, arithmetic bits and exit sites only accumulate, and the one native way to lose a prediction bit, two drains of one profile overlapping, needs a drainer besides the collector, whose marking and finalization drains never overlap each other. The baseline plan is one: it drains on its worker thread before its safepoint, without `CodeBlock::m_lock` (`BaselineJITPlan::compileInThreadImpl`), while a concurrent marker's first visit drains outside that lock too (`CodeBlock::visitChildren`), so a marker's stale merge could land after the finalize capture read a bit the plan had just added. The producing runs' `--useConcurrentGC=false` prevents this: the collector then marks with the world stopped (`SynchronousStopTheWorldMutatorScheduler`) after suspending each compiler thread at its safepoint (`Heap::stopThePeriphery`), so no marker drain overlaps the plan's. After the plan, a body under test has no other drainer: `operationOptimize` returns before draining for `CannotCompile`, and `noInline` keeps every DFG parse away from its profiles. So the `delta` capture wins, on the IC count at the latest, and the consumer imports the state the producer snapshotted.

Live C++ tests (T11 to T15) build their objects as follows, through the shared helpers of `tests/support/TestVM.h`. A test creates a global object with `JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()))`, evaluates its source with `JSC::evaluate` and reads its functions from the global object. A newborn CB comes from `ScriptExecutable::newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope())` inside a `DeferGCForAWhile`, on a function never called (the function release-asserts that the executable has no CB of that kind); it is never installed, and the test uses it inside that scope. A baseline CB comes from one call through `JSC::call`, which installs an LLInt CB, then `JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed)`, which compiles that CB and runs door 1's finalization (setup, `installCode`, `jitSoon`); later `JSC::call`s run its baseline code. Two functions with the same body text have separate UCBs with the same metadata layout and molds, so one's `BaselineJITCode` and section pair with the other's newborn CB.

- T1. `JSTests/stress/super-construct-cached-callee-baseline-born.js`, with `//@ requireOptions("--useLLInt=false")` and the default configurations, following JSTests/README.md (iterations from `testLoopCount`, under 200 ms, failure by throwing). Derived classes made by a factory call `super()` and `super(...args)` with varying `new.target`, with `edenGC()` and `fullGC()` between waves so dead cached callees are cleared; results are checked and nothing may crash. It passes before E1 too, because the cache then stays empty; it guards the GC clearing of the cache E1 starts filling. T2 is the test that fails without E1.
- T2. `super-construct-twin.js`, two sequences: `// jitcache-runs: Off --useLLInt=false` and `// jitcache-runs: Off --useBaselineJIT=false`. Two derived classes, one calling `super()` and one `super(...args)`, have constructors that return an object before `super` when `skip` is set. Each class is constructed with `skip`, then with `new.target` A, A again and B; after each step `snapshot(Derived, "construct").superConstructs` shows `Empty`, `Single` holding A, `Single` holding A, then `Multiple`, the same in both runs. Before E1 the `--useLLInt=false` run stays `Empty`, so the test fails without the fix. A second script, `super-construct-tier-up.js`, has one sequence, `// jitcache-runs: Off` with default options, and covers a CB that tiers up from the LLInt. Each of its two derived classes, one per opcode, is constructed with `new.target` A, which the LLInt stores (`Single` holding A, `jitType` `"LLInt"`), then brought to baseline with `toBaseline(Derived, "construct")`, whose `skip` calls never reach `super` and leave the cache holding A, then constructed with B in baseline code. Its snapshot then shows `jitType` `"Baseline"` and `Multiple`. Before E1 the baseline code stores A back, so this script also fails without the fix ([history](SPEC-ics-history.md#e1-stores-newtarget-as-the-llint-does)).
- T3. `capture-states.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer drives sites into every state of section 5: a cold IC; a considered IC with no case; one and three cases; get, put and `in`, by id and by value, folded megamorphic; a getter fold (`LoadMegamorphicGetter`); a folded `instanceof`; a direct put with eight shapes (final code, `*GaveUp`); a folded get that then gave up through `repatchGetBySlowPathCall`; an immediate give-up on an uncacheable dictionary flattened before; a `get_by_id` site that lists two cases when such a dictionary reaches it, so it holds its `*GaveUp` operation beside them; a non-cell base; a case whose structure dies under `fullGC()` (`resetByGC`, back to `*Optimize`); a site past one cool-down; and a body without a metadata table, an `in` by id with three shapes. Call links, in each state of section 3.2: a `call` site never called (`NotExecuted`) and one called once (`ExecutedOnce`); a `call` site called twice as a plain `f()`, whose `this` is not a cell, so it owns its data, is `Monomorphic` and holds nothing else, which comes back `ExecutedOnce`; a `call` site (`return a.push(v)`) and a `call_ignore_result` site (the statement `a.push(v)`), each called twice with an array as `a` and followed by a `fullGC()` that folds into the site's own `this` profile the structure `handleUnlinkedCall` noted at the second call, so each holds only that profile and comes back `Own`, seen, with the producer's `thisArrayModes` and `thisArrayProfileFlags`; polymorphic; a `construct` site with two callees (`Virtual`); closures of one executable (`hasSeenClosure`); a callee collected together with its executable (`clearedByGC`), such as a function made by `new Function` whose every reference the script drops before `fullGC()` (a collected closure whose executable stays alive in its creator's CB gives `hasSeenClosure` instead, `CallLinkInfo::reconcileWeakReferencesAtGCEnd`); varargs calls of several lengths, and a varargs site never called; a tail call never run (`NotExecuted`) and one run once (`Own`, seen); and a direct eval whose callee is not eval. Each body also holds an anchor. In the producer's snapshots `holdsGaveUp` is set at exactly the sites the script drove onto their `*GaveUp` operation (the direct put, the folded `instanceof`, the folded get that gave up, the immediate give-up and the `get_by_id` beside two cases), which checks those access types' E3 entries against the engine, and `clearedByGC` is set at the collected callee's site, so the test fails if the script reached `hasSeenClosure` instead and left E2 unexercised; `thisArrayModes` is nonzero at the two `push` sites, so it also fails if no drain ran before the snapshot. The producer snapshots every body, saves the snapshots with `saveJSON` and passes every body to `delta`, running no body in between. The consumer creates each CB with a `skip` call and checks that its snapshot equals the derivation of section 6.1 applied to the saved producer snapshot; the script encodes the tables of section 6.1 independently of the C++ and reads each record field from the snapshot field of the same name. The twin check covers I3 to I5 on every import.
- T4. `first-visit.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer folds a get, a put, an `in` and an `instanceof` site, makes one site give up with no case listed (an uncacheable dictionary flattened before), makes a `get_by_id` site that reads `name`, a name `canUseMegamorphicGetById` keeps from folding, give up on its eighth case by visiting it with eight object-literal shapes in turn until its snapshot lists eight cases and `"GaveUp"`, leaves one site unreached, links a `call` site, calls a second `call` site once, leaves a third `call` site and a varargs site never called, runs a tail call once and drives a `construct` site to `Virtual`, each body holding an anchor, and passes every body to `delta`. After a `skip` call, the consumer visits each site once, the site that gave up on its eighth case seven more times with the other seven shapes, and the `call` and varargs sites the producer never called once more, snapshotting after each visit. Each folded site lists one megamorphic case after its first caching visit (`instanceof`: `InstanceOfMegamorphic`, with `holdsGaveUp`); the unreached site caches at its first visit; the site that gave up on its eighth case lists one case without `holdsGaveUp` after its first visit, and eight cases with `holdsGaveUp` after its eighth, the producer's state; the linked `call` site and the one called once, which come back `ExecutedOnce`, and the tail call, which comes back `Own` and seen, are each `Own` and `Monomorphic` after one call, and the linked site's callee, which the consumer had not called, had no CB until that call; the `call` site never called is `ExecutedOnce` after one call and `Own` and `Monomorphic` after a second, and the varargs site never called is seen and `Init` after one call and `Monomorphic` after a second, as in the `Off` run; the `construct` site stays `Virtual`; the site given up with no case never lists a case.
- T5. `status-before-visit.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer gives a `put_by_id` site cases, which anchors its body, makes a second site give up with no case listed, sends a non-cell base through a third, leaves a fourth unreached, makes a fifth, a `get_by_id` of `name`, give up on its eighth case over eight object-literal shapes (as in T4) and then visits it only with those shapes, so its `tookSlowPath` stays clear, and passes the body to `delta`. After a `skip` call and before any visit, the snapshot `summary` is `Simple` for the put site (the generic-put case of THREAD Caches), `TakesSlowPath` for the given-up site, with `tookSlowPath`, and for the non-cell site, with `sawNonCell`, `NoInformation` for the unreached site, and `Simple` for the fifth site, with `holdsGaveUp` and `tookSlowPath` clear.
- T6. The oracle comparisons of SPEC-integrator.harness.md section 7.6, over every sequence in `JSTests/jitcache/ics/`.
- T7. `Source/JavaScriptCore/jitcache/tests/ics/ICSectionTests.cpp` (no VM). Size formula and overflow; `canonicalCallLinkPosition` and each `callLinkOpcodes` entry's `kind` and `hasThisProfile` for every call-link opcode, against a table the test writes out apart from `callLinkOpcodes`, and `canonicalCallLinkPosition` for opcodes outside the list; byte layout of each record; A1 to A8 and S1, each with crafted inputs that fail exactly that check under `StrictChecks::Yes`, the molds built as plain `BaselineUnlinkedPropertyInlineCache` values and the site counts as a `CallLinkSiteCounts` array; `parseSection` under `StrictChecks::No` slicing a valid section into the same views as under `StrictChecks::Yes`; the derivation tables over every combination of `holdsGaveUp`, `megamorphicCaseListed`, `canBeMegamorphic`, `tookSlowPath` and a zero or nonzero `caseCount`, and over `repatchCount` and `caseCount` pairs including `caseCount > repatchCount`; for each of these records, `restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r)`; the call-link derivation and its round trip for each of the three kinds, with and without a `this` profile, over every value of the six call-link bits, a zero and a nonzero varargs maximum, and zero, nonzero modes alone and nonzero flags alone in the `this` fields, including the `data` state of an empty record (`NotExecuted`), of a seen record holding nothing (`ExecutedOnce` for `Lazy`, `Own` for `LazyTailCall`) and of each holding fact alone (`Own`), an unseen `Init` record whose only fact is a `this` profile among them, and the derived `this` fields, zero at an opcode without a `this` profile whatever the record holds; A4 with nonzero `this` fields at an opcode without a `this` profile, a mode bit outside `ALL_ARRAY_MODES` and a flag bit above `0xff`; `isPolymorphicPropertyIC` over case counts 0 to 3, with and without `megamorphicCaseListed`; `readBaselineICsSummary` on valid and truncated headers under `StrictChecks::Yes`.
- T8. `seeds-only-installing-cb.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The bodies live in a source string that the script runs with `runString`, which evaluates it in a new realm; the same text hits the same CodeCache entry, so the realms share the UCBs. The producer runs the string once, drives the bodies' sites, each body holding an anchor, saves their snapshots and passes every body to `delta`. The consumer runs the string twice. The first realm's CB of each body installs the import, and its snapshot before any visit equals the derivation of section 6.1 applied to the saved producer snapshot. The second realm's CB, created next, takes the parked image natively, and its snapshot before any visit shows every IC never considered, every lazy call site `NotExecuted` and every inline one in `Init` without the seen bit ([history](SPEC-ics-history.md#the-lane-owns-its-test-bindings-shapes)).
- T9. `ic-allocation-failure.js`, one `Producer` sequence per fuzz point, each with `--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=<n>`, over a range of n. The fuzzer fails whichever executable allocation comes n-th, so the script declares `jitcache-expect-fault: 0` for each of `exec-alloc.baseline-plan`, `exec-alloc.dfg-plan`, `exec-alloc.ftl-plan` and `exec-alloc.mathic-snippet`, and `jitcache-require-fault: 0 exec-alloc.ic-handler`. The script makes many IC sites cache cases that get a compiled stub rather than a shared thunk or a stateless stub, such as reads through the global proxy, so that some n in the range fails an IC allocation. Whenever `jitcacheStatus()` returns `exec-alloc.ic-handler`, a later `delta()` throws naming that fault, and the output equals the JITCache-off run's (T6). When the fuzz point is the baseline plan of the body under test, the body stays in the LLInt for the rest of the run (SPEC-integrator.md N3). The script therefore brings the body to baseline with `toBaseline`'s loop, stopped after 1,000 calls (the default threshold needs 34), and goes on without throwing when the body is still in the LLInt after them. The body's sites then run in the LLInt, and the script checks no IC state of the body. In that case the Producer requires `jitcacheStatus()` to return `exec-alloc.baseline-plan`, the fault of the integrator's plan site (SPEC-integrator.md section 9), and a later `delta()` to throw naming it ([history](SPEC-ics-history.md#t9s-body-may-stay-in-the-llint)). T9 relies on the directory's default of concurrent JIT off (SPEC-integrator.harness.md section 7.4): the fuzzer counts every thread's allocations, so a fuzz index names one allocation only when no worker compiles ([history](SPEC-ics-history.md#ics-runs-keep-concurrent-jit-off)).
- T10. `consumer-producer.js`, `// jitcache-runs: Producer --useConcurrentGC=false; ConsumerProducer; Consumer`. It tests two of the criteria a recapture can win on, richness and the IC count, so F goes without an anchor in the ConsumerProducer and G in the Producer. Body F has a `get_by_id` and an `instanceof` site that the producer folds, an `add` site the producer runs only on int32, and a witness `get_by_id` site the producer never reaches; an argument selects which of the three groups a call runs. Body G holds only `get_by_id` and `put_by_id` sites, and the producer only brings it to baseline, so whichever of its producer captures the scoring keeps lists no case and no considered IC. The producer passes F, which its folded sites anchor, to `delta`. The ConsumerProducer creates F and G with `skip` calls, runs F's `add` on doubles and F's witness once with a number as its base, without reaching the folded sites, then runs G's sites, and passes G to `delta`. F's recapture lists no case, but the `add` MathIC's repatching operation (`operationValueAddOptimize`) records the new operand types, so the recapture is richer and wins although the saved body lists two IC sites with cases. G's sites now list cases but add no profile category, because G's baseline code, compiled without DFG capability, writes no value or argument profile, so G's recapture wins on the IC count. Before any visit, the Consumer finds F's witness with `sawNonCell`, which only F's recapture carries, so that recapture is the body it imported, and F's folded sites with `canBeMegamorphic`, so the recapture passed the folds on; after one caching visit, each folded site lists a megamorphic case (for `instanceof`, `InstanceOfMegamorphic`, with `holdsGaveUp`). It finds G's visited sites considered, a state only G's recapture carries ([history](SPEC-ics-history.md#which-capture-the-consumer-imports-in-the-tests)).
- T11. `Source/JavaScriptCore/jitcache/tests/ics/ICLiveTests.cpp` (live VM). Capture is read-only and deterministic: on a baseline CB with warm property ICs and call sites, two captures with a snapshot between them produce identical bytes and leave the snapshot unchanged.
- T12. `ICLiveTests.cpp` (live VM), bodies without a metadata table and the newborn check:
  1. `function id(x) { return x; }`. Its newborn CB has a null `metadataTable()`, `callLinkSiteCounts` returns all zeros and `checkNewbornCodeBlock` passes. After `JIT::compileSync`, `baselineICsSectionSize` is 16 and the capture holds no group and no record. `prepareBaselineICs` of that section, with that CB's `BaselineJITCode` and the newborn CB of a never-called function with the same body, succeeds with strict on, and `seedCallLinkHistory` on that CB visits no site.
  2. `function has(o) { return "x" in o; }`, brought to baseline as above and then called three times through `JSC::call`, with objects of shapes A, A and B in that order. The call that installs its LLInt CB reaches no IC, since `in_by_id` has no metadata, and the IC's first visit in baseline code only spends `countdown`, which starts at 1 (`PropertyInlineCache::considerRepatchingCacheImpl`), so the second call caches A and the third B. Its CB has a null `metadataTable()`, and its capture holds one `InById` record with `caseCount` 2, `everConsidered` set and no group.
  3. `function caller(g) { return g(); }`, called once through `JSC::call`. `checkNewbornCodeBlock` fails with `NewbornCallLinks` on its CB, whose `jitType()` is no longer `None`, while the newborn CB of a never-called function with the same body passes. This item tests S2's clause on a CB that ran; item 6 tests its clauses on each site, on newborn CBs.
  4. The section of `has` with its record's `accessType` changed to `GetById`, which breaks A7 and no other check, makes `prepareBaselineICs`, given that CB's image and the newborn CB of a never-called twin of `has`, fail with `MoldPairing` under `StrictChecks::Yes` and succeed under `StrictChecks::No`, which checks nothing (section 4.5).
  5. Under `StrictChecks::Yes`, `captureBaselineICs` fails with `NoBaselineJITData` on the LLInt CB of `has` and, once that CB is in baseline, with `OutputSizeMismatch` for an output one byte shorter or longer than `baselineICsSectionSize`, and writes no byte of the output in either case.
  6. `function twoCalls(f, g) { return f() + g(); }`, `function twoTailCalls(f, g) { "use strict"; if (f) return f(); return g(); }` and `function twoSpreads(f, g, a) { return f(...a) + g(...a); }`: each untouched newborn CB passes `checkNewbornCodeBlock`. On a fresh newborn CB each time, the check fails with `NewbornCallLinks` at site index 1 after one change at the second site: for `twoCalls`, `setExecutedOnce` or `CodeBlock::ensureCallLinkInfoAt`; for `twoTailCalls`, `ensureCallLinkInfoAt`; for `twoSpreads`, whose sites are inline, setting one of the seen bit, `hasSeenClosure`, `clearedByGC`, `clearedByVirtual`, a varargs maximum, the virtual mode or another call type.
- T13. `ICLiveTests.cpp` (live VM), the twin API of section 11.1 without the runner:
  1. On the baseline CB of T11, `snapshotBaselineICs` lists one property-IC snapshot per IC and one call-link snapshot per call-link site, and each agrees, in every field the record holds, with the record `captureBaselineICs` writes at the same position.
  2. That capture, prepared with `prepareBaselineICs` under `StrictChecks::Yes` against that CB's `BaselineJITCode` and the newborn CB of a never-called function with the same body, then `seedCallLinkHistory`, `setupWithUnlinkedBaselineCode` with the same code and `attachPropertyICState` on the newborn CB, in the install function's order (section 6), leaves `checkRestoredBaselineICs` returning no mismatch. Clearing `everConsidered` afterwards on one IC whose record has it set makes the check return a non-empty list of `PropertyIC` mismatches, all at that IC's index and one of them naming `everConsidered`. The newborn CB is never run or installed, and the test uses it inside the `DeferGCForAWhile` it was created in.
  3. Seeding and attach on a richer CB: a body driven until it holds a property IC with cases, a `call` site never called, one called once, one called twice as a plain `f()`, a `call` site called twice with an array as `this`, a `construct` site gone `Virtual`, a tail call run once and a varargs site called once, then put through a full collection (`vm.heap.collectNow(Sync, CollectionScope::Full)`) that folds the `this` samples, with every callee and every structure its cases name kept alive, is captured, prepared under `StrictChecks::Yes` and applied to a newborn twin in the install function's order. Seeding thus takes every branch of section 6.3 but `setHasSeenClosure` and E2's `setClearedByGC`, which T3 reaches through the runner. Every site of the newborn CB is then in the state `restoredPropertyIC` and `restoredCallLink` give for its record, the tail call, the `Virtual` site and the site with the array `this` `Own`, the plain call site called twice and the one called once `ExecutedOnce`, the never-called site `NotExecuted`, `numberOfOwnCallSiteDatas()` is 3, `CodeBlock::getArrayProfile` at the array site's bytecode index returns a profile whose `observedArrayModes()` and `arrayProfileFlags()` equal its record's nonzero `this` fields, and `checkRestoredBaselineICs` returns no mismatch.
  4. The shell function, which the test registers on its own global object as `jitcacheICsSnapshot`, length 2: a non-function, a host function, a `kind` of `"apply"` and a `kind` of `0` each throw a `TypeError`; `has` of T12 item 2, never constructed, gives `undefined` for `"construct"`; its default kind gives the same object as `"call"`, with the fields and types of section 11.1 and one `InById` IC that is `Stub`, `Simple` and lists two cases. A derived class gives no snapshot before its first construction and none for `"call"`; after one construction in the LLInt, its `"construct"` snapshot holds one `op_super_construct` call-link site and one `super_construct` cache in state `Single` whose `cachedCallee` is the class itself.
  5. Seeding's report to the heap: a body of 33 statements `a.push(v)`, called twice through `JSC::call` with an array as `a`, so that each site gets data of its own at its second call, brought to baseline as above and put through a full collection that folds each site's sample, with the array kept alive, is captured, prepared under `StrictChecks::Yes` and applied to a newborn twin in the install function's order, inside the test's `DeferGCForAWhile`. Seeding gives all 33 sites data of their own, so its 32nd allocation reports to the heap (section 6.3), which under the deferral only records deferred work. `numberOfOwnCallSiteDatas()` is then 33 and `checkRestoredBaselineICs` returns no mismatch.
- T14. `ICLiveTests.cpp` (live VM), the polymorphic bit of section 5.3. Each body is its own function, brought to baseline as above, and after each step `summarizeBaselineICs` and `captureBaselineICs` return the same `hasPolymorphicSite`:
  1. `has` of T12 item 2: false once its record lists one case, true once a call with a second shape makes it list two.
  2. `function getX(o) { return o.x; }`, called with object literals of distinct shapes that each own `x` as a plain property until its record lists one megamorphic case: false.
  3. `function callIt(f) { return f(); }`, called with two functions of different executables until its call-link record is `Polymorphic`: true. A second function with the same text, called with two closures of one executable until its record is `Polymorphic` with `hasSeenClosure`, whose stub then holds one slot: false.
  4. `function make(C) { return new C(); }`, called with two constructors until its record is `Virtual`: false.
- T15. `ICLiveTests.cpp` (live VM), the `this` profile of a builtin, whose UCB keeps no copy (section 3.2). The test takes `Array.prototype.forEach` from one global object and calls it through `JSC::call` on a two-element array, with a callback and an array as `thisArg`, so that its `callback.@call(thisArg, ...)` site, a `call` or `call_ignore_result` site whose `this` is that array, runs twice and gets its own data. It brings the builtin's CB to baseline as above, calls it once more and runs a full collection, which folds the site's sample, with the callback and the arrays kept alive. The UCB then has `isBuiltinFunction()` set and a null `valueAndArrayProfiles()`, and the site's record in the capture has a nonzero `thisArrayModes`. The capture, prepared under `StrictChecks::Yes` with that CB's `BaselineJITCode` and the newborn CB of `Array.prototype.forEach` from a second global object, which is never called and whose UCB, the cached one or one generated again from the same source, is a builtin's with the same layout, then seeded, set up and attached in the install function's order, leaves that site `Own`, `CodeBlock::getArrayProfile` at its bytecode index returning a profile with the record's modes and flags, and `checkRestoredBaselineICs` returning no mismatch. A call of `updateAllArrayProfilePredictions()` on the newborn CB, with no sample to fold and no UCB copy to merge, leaves those modes and flags unchanged.

## 12. Bench obligations

The lane sets no number for the bench to tune. The bench measures the default, strict off (THREAD Verification), where capture and prepare run no structural check (section 4.5). No task of section 14 owns these obligations: the bench loop that THREAD Verification runs after the implementation takes them ([history](SPEC-ics-history.md#no-task-owns-the-bench-obligations)).

- B1. Capture cost: `captureBaselineICs` time per CB against its IC and call-link counts, at the end of `BaselineJITPlan::finalize` and in `delta`, reported as part of THREAD's capture pause.
- B2. Install cost: `prepareBaselineICs`, `seedCallLinkHistory` and `attachPropertyICState` per body, reported inside the import cost THREAD's installation bound measures, with the number of own `CallSiteData`s seeding allocates per body beside the number of the body's lazy sites and the number of those the consumer's run calls, and how many of those allocations a `this` profile alone made `Own`.
- B3. Section bytes per body and per artifact, for the producer-limit measurement THREAD's bench loop makes.
- B4. First-run residue: property-IC slow-path visits and call-link slow-path trips per body in the consumer's first call C1, against the producer's warmed call W (HARNESS.md, Benches), and, in bodies that carry counter progress, the number of sites the first DFG compile parses before their first consumer visit (the residue THREAD Restoration has the bench measure) and the `BadCache` and `BadConstantValue` exits that compile's code takes before its first jettison. Also the share of captured bodies whose `hasPolymorphicSite` is set, split between those where some property-IC record satisfies `isPolymorphicPropertyIC` and those where only a call site set it, since each of them gives up its counter progress (section 5.3; [history](SPEC-ics-history.md#the-polymorphic-bit-call-sites-included)).
- B5. E1's native effect: class-heavy microbenchmarks with derived constructors, with default options and with `--useLLInt=false`, before and after E1, including a derived constructor that sees one `new.target` in the LLInt and a second after it tiers up to baseline ([history](SPEC-ics-history.md#e1-stores-newtarget-as-the-llint-does)).
- B6. Recaptures that drop the bit: in ConsumerProducer runs, the bodies whose import carried no counter progress (SPEC-cb.md, `counterMode` `NotCarried`) and whose committed recapture carries it, because the recapture came before a polymorphic site cached two cases or linked two callees again (section 5.3), and the `BadCache` and `BadConstantValue` exits a later consumer's first DFG compile of those bodies takes ([history](SPEC-ics-history.md#the-polymorphic-bit-call-sites-included)).

## 13. Owned paths and the INTEGRATE manifest

Implementers of this lane write only these paths and functions.

| path | what |
|---|---|
| `Source/JavaScriptCore/jitcache/ics/ICSection.h`, `.cpp` | sections 4.1 to 4.5, every declaration of section 4.4 included (`callLinkOpcodes`, `parseSection` with A1 to A6 under strict), and the derivation and recapture functions of section 6.1 |
| `Source/JavaScriptCore/jitcache/ics/ICSites.h` | section 4.6: `CallLinkSite`, the guarded metadata walkers, the instruction walker and `callLinkSiteCounts` |
| `Source/JavaScriptCore/jitcache/ics/ICCapture.h`, `.cpp` | section 5, `CaptureSummary` and the record readers included |
| `Source/JavaScriptCore/jitcache/ics/ICRestore.h`, `.cpp` | sections 6.2 to 6.4: the check functions A7, A8, S1 and S2, `prepareBaselineICs`, seeding and attaching |
| `Source/JavaScriptCore/jitcache/ics/ICTwins.h`, `.cpp` | section 11.1: snapshot, twin check and the shell function, compiled only with `ENABLE(JITCACHE_TWINS)` |
| `Source/JavaScriptCore/jitcache/tests/ics/ICSectionTests.cpp` | T7 |
| `Source/JavaScriptCore/jitcache/tests/ics/ICLiveTests.cpp` | T11 to T15 |
| `JSTests/stress/super-construct-cached-callee-baseline-born.js` | T1 |
| `JSTests/jitcache/ics/` | T2 to T6 and T8 to T10, and `resources/ics.js` |
| jit/JITCall.cpp: the `super_construct` branch of `JIT::compileOpCall` | E1 |
| bytecode/CallLinkInfo.h: `CallLinkInfo::setClearedByGC` (new) | E2 |
| bytecode/Repatch.h and Repatch.cpp: `gaveUpOperationFor` (new) | E3 |
| bytecode/AccessCase.h and AccessCase.cpp: `AccessCase::isMegamorphic` (new) | E4 |
| bytecode/InlineCacheCompiler.cpp: removal of the file-static `isMegamorphic`; its uses in `tryFoldToMegamorphic`, `compile` and `compileOneAccessCaseHandler`; the `didFailToAllocate()` branches of `compile`, `compileOneAccessCaseHandler` and `compileGetByDOMJITHandler` | E4, E5 |

Each `.cpp` file of the lane keeps its file-local helpers in a namespace named after the file, such as `ICCaptureInternal` (SPEC-integrator.md R-ALL-8). `JSTests/jitcache/resources/jitcache.js`, which `resources/ics.js` loads, is shared by every lane and not this lane's.

Manifest entries for `docs/JitCache/specs/INTEGRATE-ics.md`:

- M1. `Source/JavaScriptCore/Sources.txt`: add `jitcache/ics/ICSection.cpp`, `jitcache/ics/ICCapture.cpp`, `jitcache/ics/ICRestore.cpp` and `jitcache/ics/ICTwins.cpp` (the last compiles to nothing without `ENABLE(JITCACHE_TWINS)`). `Source/JavaScriptCore/CMakeLists.txt`: `${JAVASCRIPTCORE_DIR}/jitcache/ics` in the private include directories, beside each other part's directory, so that the glue and the tests include the lane's headers by file name; no header is exported. `Source/JavaScriptCore/shell/CMakeLists.txt`: `jitcache/tests/ics/ICSectionTests.cpp` and `ICLiveTests.cpp` in `testjitcache_SOURCES`.
- M2. Section type ids (R-INT-1).
- M3. The eight fixed-option rows of options.md that name this lane (R-INT-5).
- M4. `JSC::JITCache::didFailExecutableAllocation(VM&, ExecutableAllocationSite)` in `jitcache/session/JITCacheFaults.h`, with the site `InlineCacheHandler`, and its behavior (R-INT-6).
- M5. Capture, scoring and install glue calls (R-INT-2 to R-INT-4).
- M6. Test-build harness (R-INT-7, R-INT-9): `checkRestoredBaselineICs` after attach; the registration of `functionSnapshotBaselineICs` as the shell's `jitcacheICsSnapshot`; the shell's `jitcacheDelta` and `jitcacheStatus`; the sequence runner with its `// jitcache-runs:` lines, `arguments` convention and Off comparison, registered for `JSTests/jitcache/ics/`; the shared `JSTests/jitcache/resources/jitcache.js`, which reads that convention and wraps those two functions (section 11.2); and the C++ test runner.

## 14. Tasks

Ordered; each fits one implementation agent. A task that needs an integrator piece builds against the requirement's signature and lands when the integrator does. A header task adds declarations and never changes the signature of anything already defined, so the tree compiles after it.

1. E2, E3 and E4: the native additions, no behavior change. Build `debug-local` and run the `JSTests/stress` tests whose names contain `megamorphic`, `instanceof` or `inline-cache`. T3 checks the E3 entries of the access types it drives onto their `*GaveUp` operation against live ICs (section 11.2).
2. E1 and T1. Independent of the other tasks. Build and run T1 and the existing `JSTests/stress` tests whose names contain `super` or `class`.
3. `ICSection` and `ICSites`: types, size, canonical positions, `parseSection` (A1 to A6 under strict), `readBaselineICsSummary`, `isPolymorphicPropertyIC`, the derivation and recapture functions (section 6.1), the walkers, and T7's format, derivation, round-trip and `isPolymorphicPropertyIC` parts.
4. `ICCapture`: section 5, after tasks 1 and 3 and SPEC-cb.md task 1, which lands `ArrayProfile::arrayProfileFlags` (R-CB-3). T12 item 1's capture checks (the null table, the zero `callLinkSiteCounts`, the section size and the empty capture), items 2 and 5, and T14.
5. `ICRestore`: sections 6.2 to 6.4, applying 6.1, after tasks 1 and 3 and SPEC-cb.md task 1, which lands `ArrayProfile::restoreAccumulatedState` (R-CB-3). T7's A7, A8 and S1 parts; T12 item 1's restore checks (`checkNewbornCodeBlock`, `prepareBaselineICs` and `seedCallLinkHistory`), and items 3, 4 and 6.
6. `ICTwins` (snapshot, twin check, shell function), T11, which reads the snapshot, T13 and T15, after tasks 2, 4 and 5 and the C++ test runner of R-INT-9 (SPEC-integrator.md task 1). The integrator's install glue calls `checkRestoredBaselineICs` in twins builds and its jsc host registers the shell function (SPEC-integrator.md tasks 8 and 12, R-INT-7), so this task needs neither and lands before both ([history](SPEC-ics-history.md#task-6-lands-before-the-integrators-glue)).
7. `resources/ics.js`, T2 to T6, T8 and T10 on the integrator's runner, after task 6 and the integrator's capture and install glue, shell functions, shared `resources/jitcache.js` and runner (R-INT-2, R-INT-4 and R-INT-7; SPEC-integrator.md tasks 8, 9, 12 and 13).
8. E5 and T9, after R-INT-6 lands (SPEC-integrator.md task 2); T9 is a runner sequence, so it also waits for the integrator's tasks 12 and 13, as task 7 does.
