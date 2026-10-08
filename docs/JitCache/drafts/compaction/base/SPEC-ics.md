# SPEC-ics: ICs and call links

Lane 4 of THREAD's Execution. This file is the lane's whole design and is binding; the lane has no sub-SPECs, and [SPEC-ics-history.md](SPEC-ics-history.md) holds rationale and review records and binds nothing. Code lives in `Source/JavaScriptCore/jitcache/`, namespace `JSC::JITCache::ICs`; test-only code is guarded by `ENABLE(JITCACHE_TWINS)`.

## 1. Scope

THREAD gives this lane property-IC learning state, call-link history, the `super_construct` store in `JIT::compileOpCall` and the fault calls at the three `didFailToAllocate()` branches of `InlineCacheCompiler` (Execution), the summary's count of IC sites with cases (Capture), the bit that keeps a body with a polymorphic site from carrying baseline counter progress (Restoration) and, when DFG and FTL arrive, the data-driven IC learning and call links of every tier (Execution). This version covers the baseline tier. The lane owns:

1. One body-file section per tier, `ICsBaseline` here, holding a record per property IC of the baseline CB and a record per `DataOnlyCallLinkInfo` in its metadata.
2. Capture of that section from a live baseline CB, its summary count and the polymorphic bit it hands to the CB lane (section 5.3).
3. Restoration: seeding of call-link history after linking and attachment of property-IC learning state after native setup, with the section validated first when strict is on.
4. The native fix to the baseline `super_construct` and `super_construct_varargs` templates.
5. Three small native additions the lane needs (section 7, E2 to E4) and the executable-allocation fault calls in IC compilation (section 7, E5).

Outside the lane: the IC molds, `doneLocation`s and every other byte of the image (Image lane); the LLInt property caches, `m_cachedCallee`, `m_cachedStructureID` and the scope metadata, which THREAD leaves "as native linking leaves them" (no lane seeds them); value, array and arithmetic profiles (CB and UCB lanes); the container, directory, writer, scoring, fault plumbing and entry points (integrator).

THREAD's omissions apply: property-IC cases, handlers, stubs and the inline mirror never travel (IC cases), nor do callees, polymorphic stubs, their slot counts and `lastSeenCallee` (callee distributions). Restoration emits no IC code (THREAD Caches).

## 2. Design in brief

A property IC carries what it learned about the site (the learning group: `countdown`, `repatchCount`, `numberOfCoolDowns`, `everConsidered`, `sawNonCell`, `tookSlowPath`, `resetByGC`), the cases it holds, and how it dispatches (`m_slowOperation`, the handler chain, the inline mirror). Capture records the learning group as it is and four facts about cases and folds (section 5.2). Records hold raw producer facts; one pure function per record kind applies THREAD Caches' transformations at restore time, and the twin checks reuse it. A second gives the record that a capture of the restored state yields, and that record derives the same consumer state (I5), so a ConsumerProducer's recapture passes on every fact it imported, a fold its consumer has not yet replayed included. Capture also tells the CB lane, through the integrator, whether the body has a site whose cases or callees an early DFG compile would see only in part (section 5.3).

A call link carries its mode and scalar history (section 5.4). Restore turns every site the producer did not leave virtual into a seen-once `Init` site, which links monomorphically at its first consumer call through native `linkFor`, and a virtual site virtual through `CallLinkInfo::setVirtualCall`.

Both record arrays are dense and ordered by native index spaces (section 4.2), so restore walks them beside the CB's ICs and metadata entries with no lookup.

## 3. State census

Every field of the two native objects this lane covers, with its fate in the consumer. "Installation" means `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` (bytecode/PropertyInlineCache.cpp) called from `CodeBlock::setupWithUnlinkedBaselineCode` (bytecode/CodeBlock.cpp); "linking" means `DataOnlyCallLinkInfo::initialize` (bytecode/CallLinkInfo.cpp) called from the link pass of `CodeBlock::finishCreation`.

### 3.1 `PropertyInlineCache` and `HandlerPropertyInlineCache` (bytecode/PropertyInlineCache.h)

| field | group | value in the restored IC | source of the rule |
|---|---|---|---|
| `accessType` | identity | the mold's; the record repeats it so prepare can check the pairing | Image lane owns molds |
| `propertyIsInt32` | identity | the mold's | installation |
| `propertyIsString`, `propertyIsSymbol`, `prototypeIsKnownObject` | identity | false, as baseline installation leaves them; only the DFG's initializer copies them | installation |
| `canBeMegamorphic` | identity | the mold's value (always false for baseline molds) ORed with the derived `foldsAtFirstCase`; capture records the live value (section 5.2) | THREAD Caches |
| `preconfiguredCacheType` | shape | the mold's | installation |
| `m_cacheType` | shape | as installation leaves it: `Unset`, or `ArrayLength` when the mold's shape is `ArrayLength` | Restoration emits no IC code |
| `m_handler` | dispatch | the bare slow-path handler installation builds | cases never travel |
| `m_slowOperation` | dispatch | the `*Optimize` operation installation sets, except the `*GaveUp` operation for a given-up site, one that held its `*GaveUp` operation with no case listed (section 6.3) | THREAD Caches |
| `m_inlinedHandler`, `m_inlineAccessBaseStructureID`, `byIdSelfOffset`, `m_inlineHolder` | inline mirror | empty or inert, as installation leaves them | cells |
| `countdown` | learning | 0 | THREAD Caches (wait counter) |
| `repatchCount` | learning | captured value minus the captured case count, floored at 0 | THREAD Caches |
| `numberOfCoolDowns` | learning | captured | THREAD Caches and Restoration (IC cool-down counts) |
| `everConsidered` | learning | captured | THREAD Caches |
| `sawNonCell` | learning | captured | THREAD Caches |
| `tookSlowPath` | learning | captured, and set for a given-up site | THREAD Caches |
| `resetByGC` | learning | captured | THREAD's opening: everything else the baseline phase learned travels |
| `codeOrigin`, `callSiteIndex`, `doneLocation`, `m_identifier`, `m_globalObject`, `m_icType` | per-CB link data | installation | installation |

`RepatchingPropertyInlineCache` exists only in the FTL (`useHandlerICInFTL` is forced off); its fields belong to the FTL capture of a later version.

### 3.2 `CallLinkInfo` and `DataOnlyCallLinkInfo` (bytecode/CallLinkInfo.h)

| field | value in the restored site | source of the rule |
|---|---|---|
| `m_mode` | `Virtual` when the producer left the site `Virtual`; `Init` otherwise | THREAD Caches |
| `m_hasSeenShouldRepatch` (`seenOnce()`) | set on every site that is not virtual; cleared on a virtual site (`setVirtualCall` clears it) | THREAD Caches (seen-once) |
| `m_hasSeenClosure` | captured | THREAD Caches |
| `m_clearedByGC` | captured | THREAD Caches |
| `m_clearedByVirtual` | captured; `setVirtualCall` sets it on a virtual site | THREAD Caches |
| `m_maxArgumentCountIncludingThisForVarargs` | captured | THREAD Caches |
| `m_callType`, `m_type`, `m_owner`, `m_codeOrigin` | linking | linking |
| `m_callee`, `m_codeBlock`, `m_monomorphicCallDestination` | as linking leaves them, or the polymorphic mask and the virtual-call thunk that `setVirtualCall` writes | callees never travel |
| `m_stub`, `m_lastSeenCallee`, the incoming-call list node of `CallLinkInfoBase` | empty | callees never travel |

### 3.3 The `super_construct` cache

`OpSuperConstruct::Metadata::m_cachedCallee` and `OpSuperConstructVarargs::Metadata::m_cachedCallee` hold a cell, so they start as linking leaves them (empty) in every CB, imported or not. The lane changes only how the baseline template fills them (section 7, E1).

## 4. The `ICsBaseline` section

The integrator gives the section a type id (R-INT-1) and treats its bytes as opaque. A body file holds at most one `ICsBaseline` section, describing the baseline CB of the capture; later versions add `ICsDFG` and `ICsFTL` sections beside it, so this layout needs no tier field.

### 4.1 Layout

All integers are little-endian; the targets are little-endian and `ICSection.h` asserts `std::endian::native == std::endian::little`. Records are arrays of bytes with alignment 1, and the four header words and the group words are read and written with `memcpy`, so the lane requires no alignment of the section start.

| offset | size | content |
|---|---|---|
| 0 | 4 | `propertyICCount`: the number of property-IC records |
| 4 | 4 | `icSitesWithCases`: the summary count (section 5.3) |
| 8 | 4 | `callLinkGroupCount`: the number of call-link groups, at most 14 |
| 12 | 4 | `callLinkSiteCount`: the number of call-link records |
| 16 | 8 × groups | call-link groups: `uint32_t opcodeID`, `uint32_t siteCount` |
| then | 8 × `propertyICCount` | `PropertyICRecord`s, record i for IC i |
| then | 2 × `callLinkSiteCount` | `CallLinkRecord`s, group by group |
| then | 0 to 7 | zero bytes up to a multiple of 8 |

The size is `16 + 8·groups + 8·propertyICCount + 2·callLinkSiteCount`, rounded up to a multiple of 8, computed with `CheckedSize`. A body with 12 property ICs, five `call` sites and one `construct` site takes 144 bytes.

### 4.2 Orders

Property-IC record i describes IC i, the IC that installation builds from mold i of `BaselineJITCode::m_unlinkedPropertyInlineCaches` and that `BaselineJITData::propertyCache(i)` returns. Mold order is a native index space THREAD lists.

The call-link order is the metadata layout. The canonical opcode list is `FOR_EACH_OPCODE_WITH_CALL_LINK_INFO` (bytecode/Opcode.h): `OpCall`, `OpTailCall`, `OpCallDirectEval`, `OpConstruct`, `OpSuperConstruct`, `OpIteratorOpen`, `OpIteratorNext`, `OpAsyncIteratorOpen`, `OpAsyncIteratorNext`, `OpCallVarargs`, `OpTailCallVarargs`, `OpConstructVarargs`, `OpSuperConstructVarargs`, `OpCallIgnoreResult`. An opcode's canonical position is its index in that list. Groups appear only for opcodes whose CB has at least one metadata entry, in increasing canonical position; within a group, records follow metadata ID order, which is the order `MetadataTable::forEach<Op>` visits (bytecode/MetadataTable.h). `CodeBlock::forEachLLIntOrBaselineCallLinkInfo` (bytecode/CodeBlockInlines.h) visits the same sites in the same order.

A CB can have no metadata table. `UnlinkedMetadataTable::link` (bytecode/UnlinkedMetadataTableInlines.h) returns null when the table's `m_hasMetadata` is clear. A generated table sets it only in `UnlinkedMetadataTable::addEntry` and `addValueProfile`, so a body that added no metadata entry and no value profile links to null. A decoded table gets it from its constructors (`UnlinkedMetadataTable(bool, unsigned)`, the persistent-steps constructor and `CachedMetadataSteps::build` in runtime/CachedTypes.cpp), and for a table encoded without metadata the decoder returns the bytecode cache's empty table (`UnlinkedMetadataTable::empty`), which links to null as well. `UnlinkedMetadataTable::finalize` also clears the bit when the table's offsets would overflow. `CodeBlock::metadataTable()` is then null, for instance for `function id(x) { return x; }` (`enter` and `ret` carry no metadata) or `function has(o) { return "x" in o; }` (`in_by_id` carries none). Such a body can still be hot, be baseline-compiled and have property ICs, because argument profiles live in `CodeBlock::m_argumentValueProfiles` and property ICs in `BaselineJITData`, both outside the table. It has no metadata entry for any opcode, so its section has no group and no call-link record, and since `MetadataTable::forEach` reads the offset table through `this`, the lane walks metadata only through the guarded walkers of section 4.6.

`opcodeID` and `accessType` hold build-specific numeric values; the header's build ID makes them meaningful.

### 4.3 Records

`PropertyICRecord`, 8 bytes:

| byte | field | content |
|---|---|---|
| 0 | `accessType` | the IC's `JSC::AccessType` value |
| 1 | `learningBits` | bit 0 `everConsidered`, bit 1 `sawNonCell`, bit 2 `tookSlowPath`, bit 3 `resetByGC`; bits 4 to 7 zero |
| 2 | `repatchCount` | as captured |
| 3 | `numberOfCoolDowns` | as captured |
| 4 | `caseCount` | the number of entries `PropertyInlineCache::listedAccessCases` returned, at most 8 under the fixed `maxAccessVariantListSize` (sections 5.2 and 10.2) |
| 5 | `stateBits` | bit 0 `megamorphicCaseListed`, bit 1 `canBeMegamorphic`, bit 2 `holdsGaveUp` (section 5.2); bits 3 to 7 zero |
| 6, 7 | `reserved` | zero |

`CallLinkRecord`, 2 bytes:

| byte | field | content |
|---|---|---|
| 0 | `bits` | bits 0 and 1 mode (0 `Init`, 1 `Monomorphic`, 2 `Polymorphic`, 3 `Virtual`), bit 2 `seenOnce`, bit 3 `hasSeenClosure`, bit 4 `clearedByGC`, bit 5 `clearedByVirtual`; bits 6 and 7 zero |
| 1 | `maxArgumentCountIncludingThisForVarargs` | as captured |

The record holds producer facts only. No field is a transformed value, so the capture record THREAD's Verification compares against is the record itself, and section 6.3 derives the consumer state from it.

### 4.4 Types (`jitcache/ICSection.h`)

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
};
static_assert(sizeof(CallLinkRecord) == 2 && alignof(CallLinkRecord) == 1);

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

// Canonical position of opcodeID in FOR_EACH_OPCODE_WITH_CALL_LINK_INFO, or nullopt.
std::optional<unsigned> canonicalCallLinkPosition(OpcodeID);
bool isVarargsCallLinkOpcode(OpcodeID);

// nullopt on overflow or when groupCount exceeds numberOfCallLinkOpcodes.
std::optional<size_t> sectionSize(size_t groupCount, size_t propertyICCount, size_t callLinkSiteCount);

// Metadata entries per call-link opcode, indexed by canonical position.
using CallLinkSiteCounts = std::array<uint32_t, numberOfCallLinkOpcodes>;

enum class StrictChecks : bool { No, Yes };

struct Summary {
    uint32_t icSitesWithCases { 0 };
};

enum class Check : uint8_t {
    SectionSize,            // A1
    SummaryBound,           // A2
    CallLinkGroups,         // A3
    ReservedBits,           // A4
    EnumRange,              // A5
    SummaryCount,           // A6
    MoldPairing,            // A7
    MetadataLayout,         // A8
    MoldMegamorphicBit,     // S1
    NewbornCallLinks,       // S2
};

struct Invalid {
    Check check;
    uint32_t siteIndex; // record index when the check is per site, 0 otherwise
};

// A parsed section, as views into the bytes it was parsed from.
struct SectionView {
    SectionHeader header;
    std::span<const uint8_t> groupBytes;            // callLinkGroupCount entries of callLinkGroupSize bytes
    std::span<const PropertyICRecord> propertyICs;  // record i for IC i
    std::span<const CallLinkRecord> callLinks;      // canonical order
    CallLinkGroup group(unsigned index) const;      // read with memcpy
};

// Slices the section by the counts its header states. With StrictChecks::Yes it checks A1 to A6
// first (section 4.5); otherwise it trusts the counts. Reads only its argument and needs no VM.
Expected<SectionView, Invalid> parseSection(std::span<const uint8_t>, StrictChecks);

// Reads the summary of a saved body from its ICsBaseline section, checking A1 and A2 under
// strict and trusting the header otherwise. Takes no lock and needs no VM.
Expected<Summary, Invalid> readBaselineICsSummary(std::span<const uint8_t> section, StrictChecks);

// The property-IC half of the polymorphic bit (section 5.3): two or more cases listed, none of
// them megamorphic.
bool isPolymorphicPropertyIC(const PropertyICRecord&);

} // namespace JSC::JITCache::ICs
```

### 4.5 Well-formedness

Normal mode trusts the section, since THREAD Session has it check only the artifact's integrity (header, keys and checksums), which the integrator does before the lane sees a byte. Without strict, `parseSection` slices the record arrays by the header's counts, `readBaselineICsSummary` reads the header as it stands, and prepare checks nothing; debug builds still assert A1 to A6. With strict on, which every test runs, these checks validate the section's full structure: prepare (section 6.1) runs A1 to A8 and `readBaselineICsSummary` runs A1 and A2 on the header alone. Each failure is invalid material (THREAD Session) and names its check.

- A1. The span holds at least 16 bytes, `callLinkGroupCount` ≤ 14, and `sectionSize` of the header counts is defined and equals the span's length.
- A2. `icSitesWithCases` ≤ `propertyICCount`.
- A3. Group opcode IDs have canonical positions, the positions strictly increase, every `siteCount` is nonzero, and the `siteCount`s sum to `callLinkSiteCount` without overflow.
- A4. Padding bytes, `reserved`, the unused bits of `learningBits`, `stateBits` and call-link `bits` are zero.
- A5. `accessType` < `numberOfAccessTypes`.
- A6. `icSitesWithCases` equals the number of records with `caseCount` > 0.
- A7. `propertyICCount` equals the mold count of the prepared code (section 6.1), and record i's `accessType` equals mold i's.
- A8. For each canonical opcode, the newborn CB's call-link site count (`callLinkSiteCounts`, section 4.6) equals the group's `siteCount`, or zero when the opcode has no group.

A7 and A8 compare the section with what another part restored (the image's molds, the UCB's metadata layout). Normal mode relies on the Image lane restoring the producer's molds (R-IMG-1) and on the UCB lane's guarantee of the producer's index spaces (THREAD Storage, R-UCB-1), and seeding and attach index the CB's ICs and sites by record position; strict verifies the pairing with one pass over the molds and one over the metadata offsets.

`parseSection` holds A1 to A6; A7, A8 and the strict checks of section 6.1 are functions of their own (section 6.2). A7, A8 and S1 read only plain data (the section view, the molds, the per-opcode counts), so the unit tests reach each check with crafted inputs and no VM (T7).

### 4.6 Site walkers (`jitcache/ICSites.h`)

```cpp
namespace JSC::JITCache::ICs {

// Calls functor(Op::Metadata&) for each metadata entry of Op, in metadata ID order.
// Calls nothing when codeBlock.metadataTable() is null.
template<typename Op, typename Functor>
void forEachMetadataEntry(CodeBlock&, const Functor&);

// Calls functor(unsigned canonicalPosition, unsigned metadataID, CallLinkInfo&) for every
// call-link site, in the order of section 4.2. Calls nothing for a CB without a metadata table.
template<typename Functor>
void forEachCallLinkSite(CodeBlock&, const Functor&);

// Entries per call-link opcode; all zero for a CB without a metadata table.
inline CallLinkSiteCounts callLinkSiteCounts(CodeBlock&);

}
```

`ICSites.h` is header-only, so it has no `Sources.txt` entry. `forEachMetadataEntry` reads `metadataTable()` once, returns when it is null, as `CodeBlock::forEachLLIntOrBaselineCallLinkInfo` and `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` do, and otherwise calls `MetadataTable::forEach<Op>`. `forEachCallLinkSite` expands `FOR_EACH_OPCODE_WITH_CALL_LINK_INFO` over it, counting metadata IDs per opcode and passing `metadata.m_callLinkInfo`. Sizing, capture, A8, S2, seeding and the twin snapshots walk metadata only through these functions; nothing else in the lane calls `MetadataTable::forEach`. They read the offset table and the entries, take no lock and allocate nothing; the layout is fixed once `UnlinkedMetadataTable::link` has run.

## 5. Capture

### 5.1 Where it runs

The integrator calls the lane at THREAD's two capture points; the lane adds no hook of its own on the capture side.

| caller | thread and locks on entry | GC state | CB state |
|---|---|---|---|
| end of the success path of `BaselineJITPlan::finalize` (jit/BaselineJITPlan.cpp), after `installCode` and `jitSoon` | the VM's thread, API lock and heap access held; no JITCache lock and no `CodeBlock::m_lock` held | inside the `DeferGC` of `JITWorklist::completeAllReadyPlansForVM` or the caller's `DeferGCForAWhile` on the synchronous routes; the call must neither start nor wait for a collection nor release heap access (install.md, "Conditions around installation") | eligible: the executable's `replacement()`, `jitType()` is `BaselineJIT`, `BaselineJITData` published |
| `delta`, inside `Heap::forEachCodeBlockIgnoringJITPlans` (heap/Heap.cpp) or after it | the VM's thread, API lock and heap access held, no collector phase; possibly `CodeBlockSet::m_lock` held by the walk | the integrator's deferral; no stop point reached | eligible, imported CBs included |

The lane's capture functions take `CodeBlock::m_lock` with a `ConcurrentJSLocker` (runtime/ConcurrentJSLock.h), which carries `AssertNoGC` in debug builds, read, and release it before returning. They allocate no cell and no memory: the integrator sizes, charges and allocates the output (R-INT-2), and `PropertyInlineCache::listedAccessCases` returns a `Vector<AccessCase*, 16>` whose inline capacity holds the at most eight cases the fixed `maxAccessVariantListSize` allows (section 5.2). As THREAD Capture requires, they call nothing that drains a profile, materializes a property table, allocates a cell or stops for the collector; the only callees are `PropertyInlineCache::listedAccessCases`, `AccessCase::type`, `gaveUpOperationFor` (E3), the `CallLinkInfo` getters, `PolymorphicCallStubRoutine::forEachDependentCell` and the walkers of section 4.6.

A capture sees one consistent state because every writer of the fields it reads runs on the VM's thread or in a stopped-world GC phase; the lock does not provide it. The VM-thread writers are the running code and its slow paths, mostly unlocked: `PropertyInlineCache::considerRepatchingCacheImpl` (bytecode/PropertyInlineCache.h) writes the learning fields before any lock is taken, the `*GaveUp` operations (jit/JITOperations.cpp) write `tookSlowPath` without it, and `repatchGetBy` and its siblings repoint `m_slowOperation` through `repatchSlowPathCall` after `tryCacheGetBy`'s `GCSafeConcurrentJSLocker` scope, which covers only the added case, has closed (bytecode/Repatch.cpp). `PropertyInlineCacheClearingWatchpoint::fireInternal` resets the IC under the lock on whichever thread fires its set, the VM's thread when a watched condition breaks while JS runs. JS is paused at both capture points, so none of these writers runs during a capture. The stopped-world writer, GC finalization (`CodeBlock::reconcileWeakReferencesAtGCEnd`), cannot begin while the VM's thread holds heap access and reaches no stop point, and concurrent markers read the handler chain but write no learning field. The lock orders a capture only against readers on other threads: the status classes and `CallLinkStatus::computeFor` read under `CodeBlock::m_lock`.

### 5.2 Classifying a property IC

For IC i of `BaselineJITData`:

1. `cases = ic.listedAccessCases(locker)`; `caseCount = cases.size()`, at most the fixed `maxAccessVariantListSize` of 8 (options.md). `InlineCacheCompiler::compileHandler` measures the list with the new case appended, a fold leaves only the megamorphic case listed, and a list that reaches the limit compiles its last case as final code, after which `tryCacheGetBy` and its siblings answer `GiveUpOnCache` (`AccessGenerationResult::shouldGiveUpNow`) and the `*GaveUp` operation caches nothing more.
2. `megamorphicCaseListed` is true when some listed case satisfies `AccessCase::isMegamorphic(case->type())` (E4). After a fold the megamorphic case is the only listed case, because `PropertyInlineCache::prependHandler` replaces the chain through `initializeWithUnitHandler` and clears the inlined handler.
3. `holdsGaveUp` is true when `ic.m_slowOperation == gaveUpOperationFor(ic.accessType)` (E3). The field's other values need no record. Installation writes the `*Optimize` operation, and the only other writer outside the FTL is `repatchSlowPathCall` (bytecode/Repatch.cpp), whose callers pass the access's `*Optimize`, `*Megamorphic` or `*GaveUp` operation. A `*Megamorphic` operation sits exactly beside a listed megamorphic case: only the `PromoteToMegamorphic` arms of `repatchGetBy`, `repatchPutBy`, `repatchInBy` and their array forms install it, after the fold, and both ways out of it empty the chain: `PropertyInlineCache::reset` replaces it with `*Optimize` through `resetGetBy` and its siblings, and a megamorphic give-up with `*GaveUp` through `repatchGetBySlowPathCall` and its siblings. So `megamorphicCaseListed` already says what a `*Megamorphic` operation would, and an operation that is not the access type's `*GaveUp` restores as a reset leaves the IC.
4. `learningBits` copies `everConsidered`, `sawNonCell`, `tookSlowPath` and `resetByGC`; `repatchCount` and `numberOfCoolDowns` are copied.
5. `canBeMegamorphic` copies `ic.canBeMegamorphic`. The engine's only writers of the IC's bit are `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` and `initializeFromDFGUnlinkedPropertyInlineCache`, which copy the mold's bit; nothing writes a baseline mold's bit (`JIT::addUnlinkedPropertyInlineCache` default-constructs each mold), `PropertyInlineCache::reset` leaves the IC's bit alone, and its only reader is `InlineCacheCompiler::tryFoldToMegamorphic`. On a baseline IC only attach sets it (section 6.5), so it means the site carries a fold an import restored.

The record keeps the case count, the megamorphic case, the `*GaveUp` operation and the fold bit as separate facts, so that section 6.3 can tell apart the four native states below and a recapture keeps a restored fold:

- (a) A site gives up with its cases still chained. `InlineCacheCompiler::compileOneAccessCaseHandler` compiles the eighth case as `GeneratedFinalCode`, which `PropertyInlineCache::addAccessCase` prepends before `tryCacheGetBy` and its siblings answer `GiveUpOnCache`, and a `GiveUpOnCache` after earlier cases leaves them in place. Either way `repatchGetBy` and its siblings install the `*GaveUp` operation beside the cases, which keep serving their shapes, and a `PropertyInlineCache::reset` of them switches the site back to `*Optimize` (`resetGetBy` and its siblings in bytecode/Repatch.cpp).
- (b) `instanceof` folds into `InstanceOfMegamorphic`, and in the same slow path `tryCacheInstanceOf` returns `GiveUpOnCache` and `repatchInstanceOf` installs `operationInstanceOfGaveUp`: the site lists a megamorphic case and holds its `*GaveUp` operation.
- (c) A get, put or `in` site that folded and later gave up through `repatchGetBySlowPathCall` or its siblings has a bare chain, `m_cacheType` still `Stub` and its `*GaveUp` operation.
- (d) A fold an import restored shows only in `canBeMegamorphic` until a caching visit folds the site again; until then the IC lists no megamorphic case and holds `*Optimize`, and `delta`, which captures imported CBs, can capture it first.

### 5.3 The summary and the polymorphic bit

`icSitesWithCases` counts the property ICs whose `caseCount` is nonzero. Call links do not count: THREAD's tie-break speaks of IC sites with cases, and cases are the property ICs' `AccessCase`s. A capture at the end of `BaselineJITPlan::finalize` always counts zero, because installation has just built every IC cold.

Capture and `summarizeBaselineICs` also return `hasPolymorphicSite`, which the integrator passes to the CB lane's capture and scoring (THREAD Restoration); a body with the bit carries no baseline counter progress. The bit is set when either of these holds:

- some property IC's record lists two or more cases and no megamorphic case (`isPolymorphicPropertyIC`: `caseCount >= 2 && !megamorphicCaseListed`), THREAD's "property IC whose captured record lists two or more cases, megamorphic folds aside";
- some call-link site is in mode `Polymorphic`, has `clearedByGC` clear, and its stub (`CallLinkInfo::stub`) holds two or more slots, counted with `PolymorphicCallStubRoutine::forEachDependentCell`.

THREAD sets the bit for a call site the producer left polymorphic when this SPEC shows that such a site meets the same exits, and the sites above do. The consumer restores such a site seen once in `Init`, without its stub, so its first call links it monomorphically to the callee that call meets (`linkFor` and `linkMonomorphicCall` in bytecode/RepatchInlines.h and bytecode/Repatch.cpp), recording `lastSeenCallee`. For a site without a stub, `CallLinkStatus::computeFromCallLinkInfo` (bytecode/CallLinkStatus.cpp) answers one variant, `lastSeenCallee`, despecified to its executable when `hasSeenClosure` is set, with no slow path. A DFG compile at the counter floor acts on it: `ByteCodeParser::handleInlining` (dfg/DFGByteCodeParser.cpp) inlines the variant behind the `CheckIsConstant` that `ByteCodeParser::emitFunctionChecks` adds, and `SpeculativeJIT::compileCheckIsConstant` (dfg/DFGSpeculativeJIT.cpp) exits with `BadConstantValue` when another callee arrives. The producer's DFG read the stub instead, whose two or more slots give `CallLinkStatus` two or more variants or a slow path unless one callee took every call counted since the last new variant, and `handleInlining` leaves such a status to a generic call outside the FTL. The consumer's exits count toward a jettison (`CodeBlock::exitCountThresholdForReoptimization`), which records a `BadConstantValue` exit site at that bytecode (`CodeBlock::tallyFrequentExitSites`) and counts a reoptimization that doubles the CB's later thresholds. That exit site makes every later status there a closure call or a slow path (`CallLinkStatus::computeExitSiteData` and `CallLinkStatus::accountForExits`), and exit sites and reoptimization counts both travel in a later capture, the pattern of the `BadCache` exits THREAD names for property ICs.

The other call sites stay out. A stub with one slot holds closures of one executable: `linkPolymorphicCall` (bytecode/Repatch.cpp) merges callees that share an executable into one despecified variant and sets `hasSeenClosure`, so the consumer's despecified status matches each of them and the check never exits. A site with `clearedByGC` comes back with it, and a site the producer left `Virtual` comes back virtual with `clearedByVirtual`; `computeFromCallLinkInfo` answers a slow path for both, so no compile speculates there. A `Monomorphic` site saw one callee.

At the end of `BaselineJITPlan::finalize` the property ICs are cold, but the call links hold what the LLInt linked, so that capture can already carry the bit. The bit reads the cases and stubs a capture finds, as THREAD's rule speaks of the captured record, and it is no site's state that the I5 round trip restores: a ConsumerProducer that recaptures an imported body before a polymorphic site has cached two cases or linked two callees again captures it without the bit, with the counter progress its CB made. That recapture replaces a saved body that has the bit, such as the one it imported, only when it is richer, or is as rich and has more IC sites with cases: at a tie on both, THREAD Capture ranks the saved body's withheld counter above the recapture's, which travels (section 5.6).

### 5.4 Call links

`callLinkSiteCounts(codeBlock)` gives the groups: each opcode with a nonzero count gets one, in canonical order. `forEachCallLinkSite(codeBlock, ...)` then writes one record per site, in the same order, taking `mode()` mapped to its code, `seenOnce()`, `hasSeenClosure()`, `clearedByGC()`, `clearedByVirtual()` and `maxArgumentCountIncludingThisForVarargs()`. For a `Polymorphic` site without `clearedByGC` it also counts the stub's slots for the polymorphic bit (section 5.3), stopping at two, and records no count. `CallLinkStatus::computeFor` reads the stub under the same `CodeBlock::m_lock`, and nothing replaces or frees it during a capture (L4).

### 5.5 Strict checks

Strict checks the assumptions capture makes; each failure is a recording fault (THREAD Session) that names the check and the site index. Without strict, capture trusts them, and debug builds assert them.

- The integrator's call: the CB has `BaselineJITData`, and the output span's size equals `baselineICsSectionSize`. Writing records past either bound would corrupt memory, so these come first.
- C1. IC i's `accessType` equals mold i's: record i stands for IC i, and restore pairs it with mold i (A7). (`CodeBlock::setupWithUnlinkedBaselineCode` builds IC i from mold i.)

Capture checks nothing else. The derivation of section 6.3 gives a sound state for every combination of the facts a record holds, so native invariants among them (one case beside a megamorphic case, a virtual site with `clearedByVirtual`, a zero varargs maximum outside varargs opcodes) go unchecked, and an engine change that broke one would not make a restored state unsound.

### 5.6 Interface (`jitcache/ICCapture.h`)

```cpp
namespace JSC::JITCache::ICs {

// What summarizing or capturing a live CB yields: the summary and the polymorphic bit (section 5.3).
struct CaptureSummary {
    Summary summary;
    bool hasPolymorphicSite { false };
};

enum class CaptureCheck : uint8_t {
    NoBaselineJITData,
    OutputSizeMismatch,
    MoldMismatch,           // C1
};

struct CaptureError {
    CaptureCheck check;
    uint32_t siteIndex; // IC index for C1, 0 otherwise
};

// The exact byte size of the CB's ICsBaseline section: the IC count of BaselineJITData and
// callLinkSiteCounts (zero groups without a metadata table). Reads counts only, takes no lock.
// Precondition: jitType() == BaselineJIT and baselineJITData() is non-null.
size_t baselineICsSectionSize(CodeBlock&);

// The summary and polymorphic bit the CB would capture now, for scoring candidates before
// capturing the winner. Takes CodeBlock::m_lock. Allocates no cell (section 5.1).
CaptureSummary summarizeBaselineICs(CodeBlock&);

// Writes the whole section into output, whose size must equal baselineICsSectionSize(codeBlock),
// and returns its summary and polymorphic bit. Takes CodeBlock::m_lock. Allocates no cell
// (section 5.1). Reads only.
Expected<CaptureSummary, CaptureError> captureBaselineICs(CodeBlock&, std::span<uint8_t> output, StrictChecks);

}
```

`summarizeBaselineICs` and `captureBaselineICs` return the same summary and bit when no JS runs and no stopped-world GC phase intervenes between the two calls, which holds inside one capture point. A saved body keeps the bit as the CB lane's `counterMode` in its summary (SPEC-cb.md section 3.3), which THREAD Capture's tie-break reads.

## 6. Restoration

The lane contributes three calls to the install function, in the order THREAD Restoration fixes and at the positions R-INT-4 gives:

| call | position in the install function | thread and locks on entry | CB state |
|---|---|---|---|
| `prepareBaselineICs` | with the other lanes' prepares, after `prepareImage` and before `PreparedImage::commit` | the VM's thread, API lock and heap access; no `CodeBlock::m_lock` | newborn: `finishCreation` done, `jitType()` is `None`, never run |
| `seedCallLinkHistory` | in the CB seeding phase, before `setupWithUnlinkedBaselineCode` | same | linked, no JIT type |
| `attachPropertyICState` | right after `setupWithUnlinkedBaselineCode` returns | same; the lane takes `CodeBlock::m_lock` itself | `BaselineJITData` published, executable not yet updated |

All three run under the `DeferGCForAWhile` of `ScriptExecutable::prepareForExecutionImpl` (or of the LLInt-off route through `JIT::compileSync`) and the install function's own GC deferral. Until `installCode` stores the CB in its executable and barriers the executable, no cell a marker or a compiler thread can visit points to the CB (install.md, "Conditions around installation"), so no other thread reads what the lane writes; attach still writes under `CodeBlock::m_lock` (L3).

The seeds land in the CB that installs the import and nowhere else; the body's later CBs share the image natively with fresh IC and call-link state (THREAD Restoration).

### 6.1 Prepare

`prepareBaselineICs(section, preparedCode, codeBlock, strict)` returns views into the section and, with strict on, validates it first. `preparedCode` is the `BaselineJITCode` the Image lane's `PreparedImage::code()` returns before `commit`, the object `commit` returns and setup installs, so the molds prepare checks are the ones setup turns into ICs (R-IMG-1). Prepare writes nothing anywhere: not to the CB, the prepared code, the payload or any global (I1). The integrator keeps the payload alive and unchanged until `attachPropertyICState` returns (R-INT-4).

Without strict, prepare only parses the section, trusting it (section 4.5). With `StrictChecks::Yes` it runs the check functions of section 6.2 in this order and stops at the first failure, which is invalid material (THREAD Session): `parseSection` (A1 to A6), `checkMoldPairing` with `preparedCode.m_unlinkedPropertyInlineCaches.span()` (A7), `checkMetadataLayout` with `callLinkSiteCounts(codeBlock)` (A8), `checkMolds` over the same span (S1) and `checkNewbornCodeBlock` (S2). S1 and S2 check the two assumptions restore makes about the consumer's objects:

- S1. No mold of the prepared code has `canBeMegamorphic` set, so the IC's bit after attach is attach's alone, which the round trip of section 6.3 relies on. (Nothing sets `UnlinkedPropertyInlineCache::canBeMegamorphic` for a baseline mold; `HandlerPropertyInlineCache::initializeFromUnlinkedPropertyInlineCache` copies it.)
- S2. The newborn CB matches what linking leaves, which seeding writes over field by field: `jitType()` is `None`, and each call-link site `forEachCallLinkSite` visits has `type()` `DataOnly`, `callType()` equal to `CallLinkInfo::callTypeFor` of its opcode, mode `Init`, the seen, closure, GC and virtual bits clear, a zero varargs maximum, no stub, no last-seen callee and `isOnList()` false. A CB without a metadata table has no site to check. A CB that ran in the LLInt fails it, so S2 also re-checks THREAD Restoration's rule that such a CB receives no imported state, which the integrator's install points guarantee.

### 6.2 Checks and output of prepare (`jitcache/ICRestore.h`)

```cpp
namespace JSC::JITCache::ICs {

// The strict checks prepare composes (section 6.1). Each returns the first failure it finds.
// The first three read only their arguments and need no VM; tests build molds as plain
// BaselineUnlinkedPropertyInlineCache values.
std::optional<Invalid> checkMoldPairing(const SectionView&, std::span<const BaselineUnlinkedPropertyInlineCache> molds); // A7
std::optional<Invalid> checkMetadataLayout(const SectionView&, const CallLinkSiteCounts&);                            // A8
std::optional<Invalid> checkMolds(std::span<const BaselineUnlinkedPropertyInlineCache>);                              // S1
std::optional<Invalid> checkNewbornCodeBlock(CodeBlock&);                                                             // S2

class PreparedBaselineICs {
public:
    std::span<const PropertyICRecord> propertyICs() const { return m_propertyICs; }
    std::span<const CallLinkRecord> callLinks() const { return m_callLinks; }
    Summary summary() const { return m_summary; }

private:
    friend Expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t>, const BaselineJITCode&, CodeBlock&, StrictChecks);

    std::span<const PropertyICRecord> m_propertyICs; // borrowed from the payload
    std::span<const CallLinkRecord> m_callLinks;     // borrowed from the payload, canonical order
    Summary m_summary;
#if ASSERT_ENABLED
    const CodeBlock* m_codeBlock { nullptr };        // seed and attach assert they receive the prepared CB
#endif
};

// preparedCode is PreparedImage::code(), read before commit (R-IMG-1); prepare keeps no reference to it.
Expected<PreparedBaselineICs, Invalid> prepareBaselineICs(std::span<const uint8_t> section, const BaselineJITCode& preparedCode, CodeBlock& newbornCodeBlock, StrictChecks);
void seedCallLinkHistory(const PreparedBaselineICs&, CodeBlock&);
void attachPropertyICState(const PreparedBaselineICs&, CodeBlock&);

}
```

`PreparedBaselineICs` owns nothing, so a dropped preparation (a baked-fact mismatch in another lane, a failed `shouldJIT` gate) needs no cleanup, and the next newborn CB of the UCB prepares again.

### 6.3 Derivation

Two pure functions per record kind, declared in `ICSection.h`. `restoredPropertyIC` and `restoredCallLink` turn a producer record into the consumer state: seed and attach apply them, and the twin check compares live state with them. `recapturedPropertyIC` and `recapturedCallLink` give the record that a capture of that restored state yields before any site runs: the twin check compares a capture with them (I5). The unit tests (T7) cover all four without a VM.

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

struct RestoredCallLink {
    bool isVirtual;         // seeded through CallLinkInfo::setVirtualCall
    bool seenOnce;
    bool hasSeenClosure;
    bool clearedByGC;
    bool clearedByVirtual;
    uint8_t maxArgumentCountIncludingThisForVarargs;
    friend bool operator==(const RestoredCallLink&, const RestoredCallLink&) = default;
};

RestoredPropertyIC restoredPropertyIC(const PropertyICRecord&);
RestoredCallLink restoredCallLink(const CallLinkRecord&);

// The record a capture of the restored state yields before any site runs (I5).
PropertyICRecord recapturedPropertyIC(const PropertyICRecord&);
CallLinkRecord recapturedCallLink(const CallLinkRecord&);

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

A site that holds `*GaveUp` beside listed cases (section 5.2, states (a) and (b)) is not given up. Natively its give-up lasts only as long as those cases, which do not travel, so it comes back as a reset of them leaves it, as THREAD Caches requires: on its `*Optimize` slow call, considered and empty, with the learning group the table gives. When it lists a megamorphic case it also comes back with `canBeMegamorphic`, as every site the producer folded does, which for the folded `instanceof` brings back `InstanceOfMegamorphic` beside `operationInstanceOfGaveUp` at its first caching visit, the producer's state. Native caching rebuilds the other sites' cases from the consumer's operands and gives up again where they make it. Every restored IC is thus in exactly one of three states: cold or considered, folded, or given up.

`foldsAtFirstCase` sets `canBeMegamorphic`, which `InlineCacheCompiler::tryFoldToMegamorphic` reads beside the list-size test, so the consumer's first caching visit applies the fold rules to its first case. `repatchCount` is rewound because the consumer counts one more repatch through `considerRepatchingCacheImpl` for each case it re-caches: one, the megamorphic case, at a folded site, and up to k at a site that gave up beside k cases. `countdown` is zero so the site's first consumer visit caches, and THREAD Caches' wait-counter rule applies to every IC, including one the producer never considered.

For a call-link record r:

| field | value |
|---|---|
| `isVirtual` | `mode(r) == Virtual` |
| `seenOnce` | `!isVirtual`, for every site the producer did not leave virtual, reached or not |
| `clearedByVirtual` | `r.clearedByVirtual || isVirtual` (`setVirtualCall` sets it on a virtual site) |
| `hasSeenClosure`, `clearedByGC`, `maxArgumentCountIncludingThisForVarargs` | as recorded |

A seen-once site in `Init` links monomorphically at its first consumer call: `linkFor` (bytecode/RepatchInlines.h) prepares the callee's CB, as it does on every trip, and with the seen bit set calls `linkMonomorphicCall`. The callee's CB is created by that call, never by the import.

The recaptured property-IC record of r, with `d = restoredPropertyIC(r)`, keeps r's `accessType` and `numberOfCoolDowns`, takes `learningBits` from `d.everConsidered`, `d.sawNonCell`, `d.tookSlowPath` and `d.resetByGC` and `repatchCount` from `d`, has `caseCount` 0, and has `stateBits` holding `holdsGaveUp` when `d.givenUp` and `canBeMegamorphic` when `d.foldsAtFirstCase`, with `megamorphicCaseListed` clear. The recaptured call-link record of r, with `d = restoredCallLink(r)`, has mode `Virtual` when `d.isVirtual` and `Init` otherwise, and `d`'s seen bit, history bits and varargs maximum.

For every record r, `restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r)` and `restoredCallLink(recapturedCallLink(r)) == restoredCallLink(r)`: a recaptured record lists no case, so the rewound `repatchCount` stays; it holds `*GaveUp` exactly when the site was given up, so `givenUp` and `tookSlowPath` come back; its only fold fact is `canBeMegamorphic`, set exactly when `foldsAtFirstCase`, which already excludes `givenUp`; a virtual site keeps `Virtual` and `clearedByVirtual`, and any other site the seen bit.

### 6.4 Seeding call-link history

```
cursor = 0
forEachCallLinkSite(codeBlock, (position, metadataID, info)):   // section 4.6; no site without a metadata table
    r = restoredCallLink(prepared.callLinks()[cursor++])
    if r.isVirtual: info.setVirtualCall(codeBlock.vm())
    else if r.seenOnce: info.setSeen()
    if r.hasSeenClosure: info.setHasSeenClosure()
    if r.clearedByGC: info.setClearedByGC()        // E2
    if r.clearedByVirtual: info.setClearedByVirtual()
    if r.maxArgumentCountIncludingThisForVarargs: info.updateMaxArgumentCountIncludingThisForVarargs(r.maxArgumentCountIncludingThisForVarargs)
ASSERT(cursor == prepared.callLinks().size())
```

R-UCB-1 makes the record count equal the number of sites the walker visits, so `cursor` stays in bounds; A8 verifies it under strict and debug builds assert it. `setVirtualCall` comes first because the `reset` it calls clears the seen bit; it allocates nothing and takes no lock, since `VM::getCTIVirtualCall` returns one of the three virtual-call thunks that `JSC_FOR_EACH_VM_DEPENDENT_EAGER_COMMON_THUNK` (jit/JITThunks.h) generates at VM construction. Each write goes through `CallLinkInfo`'s own setters, field by field as THREAD Restoration requires, leaving the CB lane's profile fields in the same metadata entry untouched. Seeding takes no lock: native linking writes these fields without one, and nothing else reaches the newborn CB.

### 6.5 Attaching property-IC state

```
ConcurrentJSLocker locker(codeBlock.m_lock)
jitData = codeBlock.baselineJITData()
ASSERT(prepared.propertyICs().size() == the number of ICs in jitData)   // A7 under strict
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

A restored `canBeMegamorphic` stays set for the IC's life, since `PropertyInlineCache::reset` leaves it alone, as it leaves a mold's bit: if the site's first consumer case cannot fold and is later reset, the fold rules still apply from the next first case. The shape group and the handler chain stay as installation built them, so a restored IC the producer considered looks like a native IC after a reset, considered and empty: `PropertyInlineCache::summary` answers `Simple`, and `PutByStatus::computeForPropertyInlineCache` turns `Simple` with `m_cacheType` `Unset` into `LikelyTakesSlowPath`, the generic put THREAD Caches expects for a put site whose IC had cases. A site that gave up beside its cases is such an IC, `Simple` when its captured `tookSlowPath` is clear and `TakesSlowPath` when it is set. A restored IC the producer never considered answers `NoInformation`, as a cold native IC does, and a given-up IC `TakesSlowPath` with `tookSlowPath` set. No cell is written, so no barrier is needed.

### 6.6 Postconditions

- I1. Prepare writes nothing and allocates nothing.
- I2. Seed and attach cannot fail and allocate nothing.
- I3. After seeding, every call-link site k has the mode, seen bit, the three history bits and the varargs maximum of `restoredCallLink(record k)`; its callee fields are those linking left, or the mask and virtual-call thunk `setVirtualCall` wrote.
- I4. After attaching, every IC i has the learning fields of `restoredPropertyIC(record i)`, `canBeMegamorphic == mold.canBeMegamorphic || foldsAtFirstCase`, the `*GaveUp` operation exactly when `givenUp`, and every other field as installation left it.
- I5 (round trip). With no mold carrying `canBeMegamorphic` (S1), capturing the CB after attach and before any of its sites runs yields `recapturedPropertyIC(record i)` for every IC i and `recapturedCallLink(record k)` for every call-link site k, and each recaptured record derives the same consumer state as the record it came from (section 6.3). A ConsumerProducer whose `delta` recaptures an imported body before some of its sites run therefore commits, whenever that recapture wins THREAD's scoring, a body that restores the same state at those sites, a fold not yet replayed included (T10). The twin check verifies the capture on every import (section 11.1), and T7 checks the derivation for every record.

## 7. Native edits

Each edit names its function and file. All are in this repository; the lane changes nothing in `~/bun`, because no Bun code reads or writes property-IC learning state, call links or the `super_construct` cache.

### E1. The `super_construct` store (jit/JITCall.cpp, `JIT::compileOpCall`)

The branch for `op_super_construct` and `op_super_construct_varargs` loads `new.target` from the callee frame's this-argument slot into `BaselineJITRegisters::Call::callTargetGPR` and the cached callee into `BaselineJITRegisters::Call::callLinkInfoGPR`, then stores `callLinkInfoGPR`, the value it loaded, so the cache never changes. The LLInt's `op_super_construct` (llint/LowLevelInterpreter.asm) and `op_super_construct_varargs` (llint/LowLevelInterpreter64.asm) store `new.target` into an empty cache and `SeenMultipleCalleeObjects` into a cache holding another cell, and the varargs form skips a `new.target` that is not a cell (`loadConstantOrVariableCell(..., .done)`). The branch becomes:

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

Both registers are free here: `materializePointerIntoMetadata` overwrites `callLinkInfoGPR` next, and `CallLinkInfo::emitFastPathImpl` (bytecode/CallLinkInfo.cpp) reloads `callTargetGPR` before using it. The store goes through `metadataTableRegister` at a layout offset and the stored constant is the literal 1, so the edit adds no reference that needs an image fixup. Like the LLInt, it issues no write barrier; `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` clears a dead cached callee in baseline CBs too, because `CodeBlock::reconcileWeakReferencesAtGCEnd` calls it whenever `JITCode::couldBeInterpreted(jitType())`, which includes `BaselineJIT`.

The fix changes native behavior for every CB while it runs baseline code, under default options and with JITCache off. A CB born in baseline (door 3, `useLLInt` off, imports) kept an empty cache and now fills it. A CB that tiered up from the LLInt kept the LLInt's last answer and now keeps updating it, so a `new.target` that first differs after tier-up moves the cache to `SeenMultipleCalleeObjects`. Either way the DFG's `ByteCodeParser` cases for both opcodes freeze a callee and emit `CheckIsConstant` only when the code has seen exactly one, as for a CB that never left the LLInt. T2 tests both paths and B5 measures them.

The template is emitted inside `JIT::compileAndLinkWithoutFinalizing`, on a JIT worker inside the plan's safepoint or on the requesting thread, holding no lock and touching no cell; the emitted store runs on the VM's thread while JS runs and, like the LLInt's, takes no lock. E1 touches only this branch, which THREAD Execution gives this lane. No other part edits `JIT::compileOpCall`: the Image lane's recording reaches it only through `CallLinkInfo::emitFastPath` and leaves the `super_construct` block, with its literal stored constant, to this lane (SPEC-image.sites.md, section E), so the lane applies E1 itself.

### E2. `CallLinkInfo::setClearedByGC` (bytecode/CallLinkInfo.h)

A new public inline member beside `setClearedByVirtual`:

```cpp
void setClearedByGC()
{
    m_clearedByGC = true;
}
```

`m_clearedByGC` is protected and has no setter; its only writer is `CallLinkInfo::reconcileWeakReferencesAtGCEnd`. The setter's only caller is `seedCallLinkHistory`, in the context of section 6.

### E3. The `*GaveUp` operation per access type (bytecode/Repatch.h, bytecode/Repatch.cpp)

A new declaration in Repatch.h under `ENABLE(JIT)`, defined in Repatch.cpp after the existing `appropriate*GaveUpFunction` helpers, which it calls:

```cpp
// The *GaveUp operation a property IC of this access type holds once it gives up.
CodePtr<OperationPtrTag> gaveUpOperationFor(AccessType);
```

The pointer is retagged to `OperationPtrTag` the way `repatchSlowPathCall` retags before storing into `m_slowOperation`, so `==` against `m_slowOperation` is exact. Capture compares with it (section 5.2) and attach installs it (section 6.5). For get, put and `in` access types it maps the access type to its `GetByKind`, `PutByKind` or `InByKind` and returns `appropriateGetByGaveUpFunction`, `appropriatePutByGaveUpFunction` or `appropriateInByGaveUpFunction` of that kind; for the others it returns the operation `repatchDeleteBy`, `repatchHasPrivateBrand`, `repatchCheckPrivateBrand`, `repatchSetPrivateBrand` and `repatchInstanceOf` install. The table is normative:

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

The function switches over every `AccessType` without a `default`, so a new access type fails to compile until the table covers it. It is pure: it reads function addresses, takes no lock and may run on any thread; this lane calls it under `CodeBlock::m_lock` in capture and attach. An entry that disagreed with the engine would only cost speed: capture would record a given-up site as not given up, and the consumer would restore it as a reset leaves it, so native caching would give up again.

### E4. `AccessCase::isMegamorphic` (bytecode/AccessCase.h, bytecode/AccessCase.cpp, bytecode/InlineCacheCompiler.cpp)

The file-static `isMegamorphic(AccessCase::AccessType)` in InlineCacheCompiler.cpp moves, body unchanged, to a public `static bool isMegamorphic(AccessType)` of `AccessCase`, defined in AccessCase.cpp. Its four uses in InlineCacheCompiler.cpp become `AccessCase::isMegamorphic`: in `InlineCacheCompiler::tryFoldToMegamorphic`, in the `finishCodeGeneration` lambda of `InlineCacheCompiler::compile` (spelled `JSC::isMegamorphic` there), and in the `finishPreCompiledCodeGeneration` and `finishCodeGeneration` lambdas of `InlineCacheCompiler::compileOneAccessCaseHandler`. Behavior is unchanged; capture classifies with the same predicate the fold uses, instead of a copy. The predicate is pure; its native callers run on the VM's thread under the IC's `CodeBlock::m_lock`, as before.

### E5. Executable-allocation fault calls in IC compilation (bytecode/InlineCacheCompiler.cpp)

THREAD Failures raises the fault when an IC stub or handler cannot get executable memory, before the failure's effects are written, and THREAD Execution gives this lane the call sites, the three `didFailToAllocate()` branches of `InlineCacheCompiler`. The entry point and its parameters are the integrator's (R-INT-6).

IC compilation allocates executable memory with `JITCompilationCanFail` at three sites, and each turns a failure into a give-up: `InlineCacheCompiler::compile` (repatching ICs, FTL) and `InlineCacheCompiler::compileOneAccessCaseHandler` (handler ICs, baseline and DFG) return `AccessGenerationResult::GaveUp` after `LinkBuffer::didFailToAllocate()`, and `InlineCacheCompiler::compileGetByDOMJITHandler` returns an empty code ref, which `compileOneAccessCaseHandler` turns into `GaveUp`. Every other `LinkBuffer` in the file is a must-succeed thunk, which crashes natively and keeps JSC behavior. At each of the three `didFailToAllocate()` branches, before returning, the code calls, with `jitcache/JITCacheFaults.h` included:

```cpp
JITCache::didFailExecutableAllocation(vm, JITCache::ExecutableAllocationSite::InlineCacheHandler); // vm: the VM& the function holds
```

The call runs on the VM's thread, inside an IC slow operation that holds the caller CB's `m_lock` through a `GCSafeConcurrentJSLocker` (`tryCacheGetBy` and its siblings hold it across `PropertyInlineCache::addAccessCase`), with heap access and GC deferred. It runs before the slow path writes the `*GaveUp` operation, so the fault precedes the effect; after it the VM neither captures nor imports, so no capture can contain that give-up. Native behavior after the call is unchanged.

## 8. Interfaces and requirements

### 8.1 What the lane provides

Other parts call only these functions, declared in sections 4.4, 5.6, 6.2, 6.3, 7 and 11.1:

| function | caller | purpose |
|---|---|---|
| `baselineICsSectionSize` | integrator, capture glue | size to charge and allocate |
| `summarizeBaselineICs` | integrator, scoring | candidate's IC count and polymorphic bit |
| `captureBaselineICs` | integrator, capture glue | the section bytes, their summary and the polymorphic bit for the CB lane |
| `readBaselineICsSummary` | integrator, scoring at the key's first scoring | the saved body's IC count |
| `prepareBaselineICs` | integrator, install function | views into the payload, validated under strict |
| `seedCallLinkHistory` | integrator, install function | call-link seeds |
| `attachPropertyICState` | integrator, install function | property-IC seeds |
| `restoredPropertyIC`, `restoredCallLink`, `recapturedPropertyIC`, `recapturedCallLink` | twin checks, tests | the derivation and its round trip |
| `parseSection`, `isPolymorphicPropertyIC` | integrator, bench loop | reading committed sections for B4 |
| `snapshotBaselineICs`, `checkRestoredBaselineICs` | integrator's test harness, test builds only | section 11 |
| `functionSnapshotBaselineICs` | integrator's jsc shell registration, test builds only | the shell function `jitcacheICsSnapshot` (section 11.1) |
| `gaveUpOperationFor`, `AccessCase::isMegamorphic`, `CallLinkInfo::setClearedByGC` | native code and this lane | E2 to E4 |

The check functions of section 6.2 and the walkers of section 4.6 are the lane's own; only its tests call them directly.

### 8.2 Requirements on the integrator

- R-INT-1. A section type id for `ICsBaseline`, and later ones for `ICsDFG` and `ICsFTL`. The lane's bytes go into the body file unchanged and come back as a `std::span<const uint8_t>` of exactly the written length; no alignment is required.
- R-INT-2. Capture glue. At each capture point and for each candidate CB: call `baselineICsSectionSize`, charge that many bytes to the producer limit (THREAD Session), allocate them, call `captureBaselineICs` with the config's strict flag, and turn a `CaptureError` into a recording fault whose diagnostic names this lane and the `CaptureCheck`. Call it before the CB lane's capture of the same CB, in the same pause, and pass `CaptureSummary::hasPolymorphicSite` to that capture (THREAD Restoration; SPEC-cb.md R-INT-5); a body captured with the bit carries no baseline counter progress, so its envelope's P is zero (THREAD Maintenance). Call the lane without holding any `CodeBlock::m_lock`; `CodeBlockSet::m_lock`, held by `Heap::forEachCodeBlockIgnoringJITPlans`, may be held (section 9).
- R-INT-3. Scoring. The candidate's `icSitesWithCases` and `hasPolymorphicSite` come from `summarizeBaselineICs` or from `captureBaselineICs`, called before the CB lane's `scoreLive` of the same CB in the same pause; the bit goes to `scoreLive`, so a candidate with it scores a withheld counter with no progress, as its capture will write it (SPEC-cb.md section 4.3 and R-INT-5). The saved body's count comes from `readBaselineICsSummary`, given the whole `ICsBaseline` section and the config's strict flag, and its `Invalid` is invalid material; the saved body's bit comes from the CB lane's summary (section 5.6). THREAD Capture's tie-break order applies, which ranks a withheld counter above one that travels just before it compares progress.
- R-INT-4. Install glue. Call `prepareBaselineICs` after `prepareImage` succeeded and before `PreparedImage::commit`, while nothing has been written to the CB, with the section, `PreparedImage::code()` (R-IMG-1), the newborn CB and the config's strict flag. Turn an `Invalid` into invalid material and then destroy the prepared image without `commit`, as the Image lane requires after any failed preparation, so the failure leaves no effect outside the prepared objects. Keep the payload alive and unchanged until `attachPropertyICState` returns. Call `seedCallLinkHistory` after every prepare, the baked-fact comparison and the `shouldJIT` gate passed and before `setupWithUnlinkedBaselineCode`; call `attachPropertyICState` right after `setupWithUnlinkedBaselineCode` returns and before the baseline counter is re-sliced. Never call either for a CB other than the prepared one, or after a failed prepare. Every baseline capture writes an `ICsBaseline` section, so a body file without one is invalid material when strict checks the directory, and normal mode trusts that it is there.
- R-INT-5. The seven option rows of options.md that name this lane, in the fixed-option table `start` checks.
- R-INT-6. The fault entry point the IC compiler calls (E5): `JSC::JITCache::didFailExecutableAllocation`, declared in `jitcache/JITCacheFaults.h`, whose parameters the integrator owns (THREAD Execution); this lane passes `ExecutableAllocationSite::InlineCacheHandler` beside the `VM&`. It is called on the VM's thread with `CodeBlock::m_lock` held through a `GCSafeConcurrentJSLocker`, from any tier's IC compilation, in any VM, which THREAD Execution requires it to accept. It must not allocate a cell, stop for the collector, take any `CodeBlock::m_lock` or wait for another thread; it is a no-op for a VM `start` never configured, and otherwise raises the fault THREAD Failures describes (cache activity off, production ended) with the failing step `exec-alloc.ic-handler` (SPEC-integrator.md section 11.1).
- R-INT-7. Test builds (`ENABLE(JITCACHE_TWINS)`), for the JS tests of section 11.2:
  1. Call `checkRestoredBaselineICs` right after `attachPropertyICState` returns, and report each mismatch as a twin failure.
  2. Register the lane's `JSC::JITCache::ICs::functionSnapshotBaselineICs` on the jsc shell's global object as `jitcacheICsSnapshot`, length 2. Section 11.1 defines its arguments and the object it returns.
  3. Two more shell functions: `jitcacheDelta()`, which calls `delta` on the shell's VM and throws an `Error` naming the fault when `delta` reports one; and `jitcacheStatus()`, which returns the name of the step `status` reports as failed, or `null` when the VM recorded no fault. The shell is the host here, deciding when to call `delta` as THREAD Session lets a host do; both functions exist only in test builds, so `delta` stays an entry point of C++ only.
  4. The runner of SPEC-integrator.harness.md section 7 for `JSTests/jitcache/ics/`, in twins mode, with the oracle comparison of its section 7.6 that T6 relies on.
- R-INT-8. The `Sources.txt` entries of section 13.
- R-INT-9. Test builds: a runner for the C++ tests under `Source/JavaScriptCore/jitcache/tests/`. A test is a function registered by name, with a flag saying whether it needs a VM, and fails through a macro that records a message. The runner calls `JSC::initialize()` once before any test, because a test's VM needs the process-wide setup it performs (`Options::initialize`, `ExecutableAllocator::initialize`, the Structure address space and `LLInt::initialize`); tests without a VM, T7's included, read no option and need nothing from it. A test that needs a VM receives a fresh `VM&`, created after `JSC::initialize()` with default options and without a `start` call, with its API lock held. The live tests (T11 to T14) build their own global objects and CBs (section 11.2), so the runner needs no lane-specific hook.

### 8.3 Requirements on the other lanes

- R-IMG-1 (Image lane). Before `commit`, and without any effect outside the prepared objects, the prepared image exposes the `BaselineJITCode` setup will install: `const BaselineJITCode& PreparedImage::code() const`, valid until `commit` or the prepared image's destruction, the same object `commit` returns. Its `m_unlinkedPropertyInlineCaches` holds the producer's molds in mold order, each with the producer's `accessType` and `canBeMegamorphic`, unchanged from `prepareBaselineICs` to setup, which builds IC i from mold i natively.
- R-UCB-1 (UCB lane). An imported or reused UCB has the producer's metadata layout: for each opcode with a `CallLinkInfo`, the same number of metadata entries and the same metadata IDs. THREAD Storage states this guarantee of the producer's index spaces; seeding relies on it, and A8 checks it under strict.
- R-CB-1 (CB lane). The CB lane's metadata seeds write profile fields only and never `m_callLinkInfo` of a call opcode's metadata entry, so the two lanes' seeds are disjoint and their order inside the seeding window is free. Its baseline-counter re-slice comes after `attachPropertyICState`, as THREAD orders it, and touches no IC field.
- R-CB-2 (CB lane). Its capture and `scoreLive` take `hasPolymorphicSite`, which the integrator passes from this lane (R-INT-2, R-INT-3), and when it is set they write and score no baseline counter progress and mark the counter withheld, with `counterMode` `NotCarried` in `cb.summary` as in `cb.state`, so a saved body keeps the bit for THREAD Capture's tie-break (THREAD Restoration and Capture; section 5.6). SPEC-cb.md sections 3.3, 4.3 and 4.4 and its R-INT-5 meet this.

## 9. Locks, GC and threads

The lane has no lock of its own and starts no thread. Every lane function runs on the VM's thread with the API lock and heap access held.

- L1. The only lock the lane takes is `CodeBlock::m_lock`, through `ConcurrentJSLocker`, in `summarizeBaselineICs`, `captureBaselineICs`, `attachPropertyICState` and `snapshotBaselineICs`; `checkRestoredBaselineICs` takes it through `snapshotBaselineICs` and then `captureBaselineICs`, never both at once. It is the innermost lock, never held for two CBs at once, and released before the function returns.
- L2. Callers hold no `CodeBlock::m_lock` (the lock is not recursive). A caller in `delta` may hold `CodeBlockSet::m_lock`, and the order `CodeBlockSet::m_lock` then `CodeBlock::m_lock` cannot invert, because native code never takes the set's lock while holding a `CodeBlock::m_lock`. `CodeBlockSet::add` runs only in the two `CodeBlock` constructors (bytecode/CodeBlock.cpp), and `CodeBlockSet::remove` has no caller: dead CBs leave the set in `CodeBlockSet::clearCurrentlyExecutingAndRemoveDeadCodeBlocks`, at the collector's End phase, without the lock (heap/CodeBlockSet.cpp). The other takers, `ConservativeRoots::add`, `VMTraps` (runtime/VMTraps.cpp), which jettisons under the set's lock, and `SamplingProfiler` (runtime/SamplingProfiler.cpp), hold no `CodeBlock::m_lock` when they take it, and `VMInspector` only tries it.
- L3. Native readers of IC and call-link state on compiler threads take `CodeBlock::m_lock`, and attach writes under it, so no fence is needed. Other threads reach the CB only through `installCode`, which stores and barriers after attach.
- L4. The lane writes no cell field and no `WriteBarrier`. It reads cells only through `listedAccessCases`, whose `AccessCase`s are ref-counted and stay alive under the lock, and through a polymorphic call site's stub, whose slots it only counts. The stub is a `GCAwareJITStubRoutine` that the VM's thread replaces or drops (`linkPolymorphicCall`, and `CallLinkInfo::reset` from `setVirtualCall`, `revertCall` and the unlinking `installCode` starts), that a collection's finalization unlinks when a slot's callee died (`CallLinkInfo::reconcileWeakReferencesAtGCEnd`), and that only a collection frees; none of these runs during a capture. In test builds `snapshotBaselineICs` also copies each `super_construct` cache's cell, which the shell function keeps valid by building its result under `DeferGC` (section 11.1). Neither capture nor restore can trigger a collection, which `ConcurrentJSLocker` asserts in debug builds.
- L5. JIT worker threads and GC threads never call into the lane. The lane records nothing during compilation; everything it captures is read at a capture point.

## 10. Failures and options

### 10.1 Outcome of each fallible step

Invalid material and recording faults have the effects THREAD Failures gives them.

| step | failure | outcome |
|---|---|---|
| `captureBaselineICs` | under strict, a CB without `BaselineJITData` or an output span of the wrong size | recording fault: the integrator called the lane wrongly |
| `captureBaselineICs` | C1 under strict | recording fault; the body being written is canceled |
| charging or allocating the section (integrator) | producer limit reached | recording fault |
| `readBaselineICsSummary` | A1 or A2 under strict | invalid material |
| `prepareBaselineICs` | A1 to A8, S1 or S2 under strict | invalid material: the CB stays native, nothing was written to it, the prepared image is destroyed without `commit`, and the pending import dies with its UCB |
| `seedCallLinkHistory`, `attachPropertyICState` | none possible | not fallible |
| IC stub or handler allocation in any tier (E5) | lack of executable memory | the fault through R-INT-6, raised before the give-up is written; the native give-up then proceeds |
| `checkRestoredBaselineICs` (test builds) | a mismatch | twin failure reported by the harness; no runtime effect |

Without strict, no call into the lane can fail: capture, `readBaselineICsSummary` and prepare trust what their strict checks would verify (sections 4.5, 5.5 and 6.1).

### 10.2 Option rows

[options.md](../options.md) holds this lane's seven fixed rows, options that can change what transported state means and that Bun does not set, and the IC options that stay free because no state this lane carries depends on them.

## 11. Verification

Everything this lane restores is history the engine cannot recompute, so under THREAD Verification its twin is the capture record passed through the derivation of section 6.3; the `super_construct` fix has a native twin, the LLInt's effect on the same cache. The lane records nothing extra in test builds, since its twins need only the section. Its check never skips itself, in either JIT mode, because it reads only the section and the fields of a CB that no other thread can reach before `installCode` (section 6).

### 11.1 Twin API (`jitcache/ICTwins.h`, `ENABLE(JITCACHE_TWINS)` only)

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
};

struct SuperConstructSnapshot {
    OpcodeID opcodeID;      // op_super_construct or op_super_construct_varargs
    unsigned metadataID;
    enum class State : uint8_t { Empty, Single, Multiple } state;
    JSCell* cachedCallee;   // the cell when Single, null otherwise; not a root (section 11.1)
};

struct BaselineICsSnapshot {
    JITType jitType;
    Vector<PropertyICSnapshot> propertyICs;         // IC order; empty unless the CB has BaselineJITData
    Vector<CallLinkSnapshot> callLinks;             // canonical order (forEachCallLinkSite)
    Vector<SuperConstructSnapshot> superConstructs; // forEachMetadataEntry over both opcodes
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

// The jsc shell's jitcacheICsSnapshot(fn, kind), registered by the integrator (R-INT-7).
JSC_DECLARE_HOST_FUNCTION(functionSnapshotBaselineICs);

}
```

`captureBaselineICs` and `snapshotBaselineICs` read through one internal reader, so a snapshot and a capture of the same state agree field for field. A CB without a metadata table snapshots with no call link and no `super_construct` cache.

`checkRestoredBaselineICs` runs right after attach, before any JS, so nothing changes the CB in between. It compares the CB's snapshot with postconditions I3 and I4, then captures the CB with `StrictChecks::Yes` into a buffer of `baselineICsSectionSize` bytes, which test builds may allocate, and compares each record with `recapturedPropertyIC` or `recapturedCallLink` of the record it was restored from (I5); a `CaptureError` is reported as a `Capture` mismatch.

`jitcacheICsSnapshot(fn, kind)` takes a JS function whose executable is a `FunctionExecutable` and `kind`, `"call"` (the default) or `"construct"`; anything else throws a `TypeError`. It snapshots the executable's CB of that kind, or that CB's `baselineAlternative()` when the CB is a DFG or FTL one, and returns `undefined` when the executable has no CB of that kind. Otherwise it returns `{ jitType, propertyICs, callLinks, superConstructs }`, built only from the snapshot inside the `DeferGC` scope that took it: `cachedCallee` is a raw pointer in a vector no collection scans, and its cache is weak (`CodeBlock::reconcileLLIntInlineCachesAtGCEnd` clears a dead one, and the sweep after it can free the cell), but under the deferral no collection reaches its End phase, so the cell stays valid until the result holds it. The result has these fields:

- `jitType` as `JITCode::typeName` spells it (`"None"`, `"LLInt"`, `"Baseline"`).
- Each `propertyICs` element has the fields of `PropertyICSnapshot` under the same names: `accessType` by its enumerator name (the names `JSC_FOR_EACH_PROPERTY_INLINE_CACHE_ACCESS_TYPE` lists, such as `"GetById"`); `cacheType` and `summary` by their enumerator names (`"Unset"`, `"Stub"`, `"NoInformation"`, `"TakesSlowPath"`); the rest, `holdsGaveUp` included, as numbers and booleans.
- Each `callLinks` element has `opcode` (`opcodeNames`, such as `"op_call"`), `metadataID`, `mode` (`"Init"`, `"Monomorphic"`, `"Polymorphic"`, `"Virtual"`) and the other fields of `CallLinkSnapshot` under their names.
- Each `superConstructs` element has `opcode`, `metadataID`, `state` (`"Empty"`, `"Single"`, `"Multiple"`) and `cachedCallee`, the cell when `Single` and `null` otherwise.

The enumerator names come from switches without `default` in `ICTwins.cpp`, so a new enumerator breaks the build until it is named.

### 11.2 Tests

R-INT-7 supplies the shell functions and the sequence runner, R-INT-9 the C++ test runner. Every test but T1 needs an `ENABLE(JITCACHE_TWINS)` build and runs in each such build the integrator makes, on x86_64 and ARM64, `debug-local` (ASan and LSan) among them; T1 runs in `debug-local` and `release-local` like any stress test.

Conventions of the JS tests in `JSTests/jitcache/ics/`:

- Each script loads `resources/ics.js` with `load("./resources/ics.js", "caller relative")`. Only that file names the R-INT-7 functions and the `arguments` convention. It provides `role()`, `snapshot(fn, kind)`, `toBaseline(fn, kind)`, `saveJSON(name, value)` and `loadJSON(name)` over the scratch directory (through the shell's `writeFile` and `readFile`), `delta(...bodies)`, and assertions that throw. `toBaseline` calls the body with `skip`, through `new` when `kind` is `"construct"`, until its snapshot's `jitType` is `"Baseline"`, and throws after 100,000 calls. `delta` takes bodies, each a function or a `[fn, kind]` pair, throws unless each one's snapshot has `jitType` `"Baseline"` and a property IC with a nonzero `caseCount`, and then calls `jitcacheDelta()`.
- Each body under test is passed to `noDFG` and `noInline` before its first call, in every run. `noDFG` keeps its baseline CB its executable's `replacement()` until `delta()` and makes its capability class, a baked fact, the same in every run. `noInline` (`ScriptExecutable::setNeverInline`) keeps a DFG compile of a caller from inlining it, which `DFG::inlineFunctionForCapabilityLevel` (dfg/DFGCapabilities.h) would otherwise allow, since it reads `isInliningCandidate` and not the body's capability class; so every call runs the body's own CB, and no compiler thread reads or drains its profiles.
- Each body under test takes a leading `skip` argument and returns at once when it is true. A producing run brings it to baseline with `toBaseline` before driving its sites, so every site it drives runs in baseline code. A Consumer creates and installs a body's CB with one `skip` call, which reaches no IC or call site, and snapshots it before any visit.
- Each body whose `delta` capture a later run checks has an anchor: a property-IC site that the producing run gives a case after `toBaseline` and that still lists it when the body is passed to `delta`, which checks it. A state without a case (cold, considered and empty, given up after a fold, non-cell, reset by GC, every call-link state) is therefore tested in a body that also holds an anchor. A body without a metadata table anchors on its own property IC, because an added `get_by_id` would give it a table. T10 runs two bodies without an anchor on purpose, each in one run, to test the scoring itself.
- Scripts keep alive until `delta()` every structure a site's cases name, through objects that still have it, and every callee a call site linked, except those a test kills on purpose before it snapshots, so no collection between a snapshot and `delta()` changes the state both read.
- Scripts print only values that do not depend on the role, so the runner can compare outputs (T6).
- Each script holds its body in a function of the run's arguments, `(function main(role, scratch, artifact) { ... })(...arguments)`, and leaves nothing that depends on its role reachable when that function returns: no global binding or property holds the role, a snapshot, a loaded JSON value or a driver, and `resources/ics.js` keeps no such state at its top level (SPEC-integrator.md R-ALL-4), since the oracle compares heaps as well as output (R-INT-7, item 4). A script that cannot meet this declares `// jitcache-heap: off` with its reason (SPEC-integrator.harness.md section 7.2).
- Under the `Off` role, which the oracle's runs and T2's sequences pass, a script runs its Consumer path without `delta`, `saveJSON` or `loadJSON` and without any assertion about imported or captured state; assertions about native state, such as T2's, run in every role.
- The `Producer` run of every sequence whose `delta` captures a later run checks declares `--useConcurrentGC=false`, for the anchor argument below.

The anchor decides which capture the consumer imports. A body's first capture, at the end of `BaselineJITPlan::finalize`, counts no IC site with cases (section 5.3), and a `delta` capture replaces it only by beating it under THREAD's scoring. `noDFG` makes the CB `CannotCompile` (`functionNoDFG` in jsc.cpp sets `ScriptExecutable::setNeverOptimize`, which `DFG::mightCompileFunctionForCall` in dfg/DFGCapabilities.cpp reads), so `JIT::compileAndLinkWithoutFinalizing` (jit/JIT.cpp) clears `m_shouldEmitProfiling` and `m_canBeOptimized`: the baseline code writes no value, array or argument profile, and `JIT::emit_op_enter` and `JIT::emit_op_loop_hint` (jit/JITOpcodes.cpp) add nothing to the baseline counter, which then moves only when `op_enter` takes its slow path for a trap or for the CB's write barrier (`JIT::op_enter_handlerGenerator`), at times collections decide. Richness counts no call-link or IC learning state, so a body whose learned state lies only in call links or in ICs without a case could tie with its finalize capture, and collection timing would decide whether `delta` replaced it. An anchored body's `delta` capture counts at least one IC site with cases against none, and it is never poorer than the finalize capture of the same CB: predictions, array flags, arithmetic bits and exit sites only accumulate, and the one native way to lose a prediction bit, two drains of one profile overlapping, needs a drainer besides the collector, whose marking and finalization drains never overlap each other. The baseline plan is one: it drains on its worker thread before its safepoint, without `CodeBlock::m_lock` (`BaselineJITPlan::compileInThreadImpl`), while a concurrent marker's first visit drains outside that lock too (`CodeBlock::visitChildren`), so a marker's stale merge could land after the finalize capture read a bit the plan had just added. The producing runs' `--useConcurrentGC=false` prevents this: the collector then marks with the world stopped (`SynchronousStopTheWorldMutatorScheduler`) after suspending each compiler thread at its safepoint (`Heap::stopThePeriphery`), so no marker drain overlaps the plan's. Once the plan has finished, a body under test has no other drainer: `operationOptimize` (jit/JITOperations.cpp) returns before draining for `CannotCompile`, and `noInline` keeps every DFG parse away from its profiles. So the `delta` capture wins, on the IC count at the latest, and the consumer imports the state the producer snapshotted.

Live C++ tests (T11 to T14) build their objects as follows. A test creates a global object with `JSGlobalObject::create(vm, JSGlobalObject::createStructure(vm, jsNull()))`, evaluates its source with `JSC::evaluate` and reads its functions from the global object. A newborn CB comes from `ScriptExecutable::newCodeBlockFor(CodeSpecializationKind::CodeForCall, function, function->scope())` inside a `DeferGCForAWhile`, on a function never called (the function release-asserts that the executable has no CB of that kind); it is never installed, and the test uses it inside that scope. A baseline CB comes from one call through `JSC::call`, which installs an LLInt CB, then `JIT::compileSync(vm, codeBlock, JITCompilationMustSucceed)`, which compiles that CB and runs door 1's finalization (setup, `installCode`, `jitSoon`); later `JSC::call`s run its baseline code. Two functions with the same body text have separate UCBs with the same metadata layout and molds, so one's `BaselineJITCode` and section pair with the other's newborn CB.

- T1. `JSTests/stress/super-construct-cached-callee-baseline-born.js`, with `//@ requireOptions("--useLLInt=false")` and the default configurations, following JSTests/README.md (iterations from `testLoopCount`, under 200 ms, failure by throwing). Derived classes made by a factory call `super()` and `super(...args)` with varying `new.target`, with `edenGC()` and `fullGC()` between waves so dead cached callees are cleared; results are checked and nothing may crash. It passes before E1 too, because the cache then stays empty; it guards the GC clearing of the cache E1 starts filling. T2 is the test that fails without E1.
- T2. `super-construct-twin.js`, two sequences: `// jitcache-runs: Off --useLLInt=false` and `// jitcache-runs: Off --useBaselineJIT=false`. Two derived classes, one calling `super()` and one `super(...args)`, have constructors that return an object before `super` when `skip` is set. Each class is constructed with `skip`, then with `new.target` A, A again and B; after each step `snapshot(Derived, "construct").superConstructs` shows `Empty`, `Single` holding A, `Single` holding A, then `Multiple`, the same in both runs. Before E1 the `--useLLInt=false` run stays `Empty`, so the test fails without the fix. A second script, `super-construct-tier-up.js`, has one sequence, `// jitcache-runs: Off` with default options, and covers a CB that tiers up from the LLInt. Each of its two derived classes, one per opcode, is constructed with `new.target` A, which the LLInt stores (`Single` holding A, `jitType` `"LLInt"`), then brought to baseline with `toBaseline(Derived, "construct")`, whose `skip` calls never reach `super` and leave the cache holding A, then constructed with B in baseline code. Its snapshot then shows `jitType` `"Baseline"` and `Multiple`. Before E1 the baseline code stores A back, so this script also fails without the fix.
- T3. `capture-states.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer drives sites into every state of section 5: a cold IC; a considered IC with no case; one and three cases; get, put and `in`, by id and by value, folded megamorphic; a getter fold (`LoadMegamorphicGetter`); a folded `instanceof`; a direct put with eight shapes (final code, `*GaveUp`); a folded get that then gave up through `repatchGetBySlowPathCall`; an immediate give-up on an uncacheable dictionary flattened before; a `get_by_id` site that lists two cases when such a dictionary reaches it, so it holds its `*GaveUp` operation beside them; a non-cell base; a case whose structure dies under `fullGC()` (`resetByGC`, back to `*Optimize`); a site past one cool-down; and a body without a metadata table, an `in` by id with three shapes. Call links: never called, called once, monomorphic, polymorphic, a `construct` site with two callees (`Virtual`), closures of one executable (`hasSeenClosure`), a callee collected together with its executable (`clearedByGC`), such as a function made by `new Function` whose every reference the script drops before `fullGC()` (a collected closure whose executable stays alive in its creator's CB gives `hasSeenClosure` instead, `CallLinkInfo::reconcileWeakReferencesAtGCEnd`), varargs calls of several lengths, a tail call and a direct eval. Each body also holds an anchor. In the producer's snapshots `holdsGaveUp` is set at exactly the sites the script drove onto their `*GaveUp` operation (the direct put, the folded `instanceof`, the folded get that gave up, the immediate give-up and the `get_by_id` beside two cases), which checks those access types' E3 entries against the engine, and `clearedByGC` is set at the collected callee's site, so the test fails if the script reached `hasSeenClosure` instead and left E2 unexercised. The producer snapshots every body, saves the snapshots with `saveJSON` and passes every body to `delta`, running no body in between. The consumer creates each CB with a `skip` call and checks that its snapshot equals the derivation of section 6.3 applied to the saved producer snapshot; the script encodes the tables of section 6.3 independently of the C++ and reads each record field from the snapshot field of the same name. The twin check covers I3 to I5 on every import.
- T4. `first-visit.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer folds a get, a put, an `in` and an `instanceof` site, makes one site give up with no case listed (an uncacheable dictionary flattened before), makes a `get_by_id` site that reads `name`, a name `canUseMegamorphicGetById` (bytecode/InlineCacheCompiler.h) keeps from folding, give up on its eighth case by visiting it with eight object-literal shapes in turn until its snapshot lists eight cases and `"GaveUp"`, leaves one site unreached, links a `call` site and drives a `construct` site to `Virtual`, each body holding an anchor, and passes every body to `delta`. After a `skip` call, the consumer visits each site once, and the site that gave up on its eighth case seven more times with the other seven shapes. Each folded site lists one megamorphic case after its first caching visit (`instanceof`: `InstanceOfMegamorphic`, with `holdsGaveUp`); the unreached site caches at its first visit; the site that gave up on its eighth case lists one case without `holdsGaveUp` after its first visit, and eight cases with `holdsGaveUp` after its eighth, the producer's state; the `call` site is `Monomorphic` after one call, and its callee, which the consumer had not called, had no CB until that call; the `construct` site stays `Virtual`; the site given up with no case never lists a case.
- T5. `status-before-visit.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The producer gives a `put_by_id` site cases, which anchors its body, makes a second site give up with no case listed, sends a non-cell base through a third, leaves a fourth unreached, makes a fifth, a `get_by_id` of `name`, give up on its eighth case over eight object-literal shapes (as in T4) and then visits it only with those shapes, so its `tookSlowPath` stays clear, and passes the body to `delta`. After a `skip` call and before any visit, the snapshot `summary` is `Simple` for the put site (the generic-put case of THREAD Caches), `TakesSlowPath` for the given-up site, with `tookSlowPath`, and for the non-cell site, with `sawNonCell`, `NoInformation` for the unreached site, and `Simple` for the fifth site, with `holdsGaveUp` and `tookSlowPath` clear.
- T6. Oracle: for each sequence, the runner compares every run that configures JITCache with a JITCache-off run under the same options (R-INT-7, item 4), so every script's results, exceptions and stack traces match a JITCache-off run, and so does its reachable heap unless the script declares `jitcache-heap: off` (section 11.2).
- T7. `Source/JavaScriptCore/jitcache/tests/ICSectionTests.cpp` (no VM). Size formula and overflow; byte layout of each record; A1 to A8 and S1, each with crafted inputs that fail exactly that check under `StrictChecks::Yes`, the molds built as plain `BaselineUnlinkedPropertyInlineCache` values and the site counts as a `CallLinkSiteCounts` array; `parseSection` under `StrictChecks::No` slicing a valid section into the same views as under `StrictChecks::Yes`; the derivation tables over every combination of `holdsGaveUp`, `megamorphicCaseListed`, `canBeMegamorphic`, `tookSlowPath` and a zero or nonzero `caseCount`, and over `repatchCount` and `caseCount` pairs including `caseCount > repatchCount`; for each of these records, `restoredPropertyIC(recapturedPropertyIC(r)) == restoredPropertyIC(r)`, and the same for every call-link record; `isPolymorphicPropertyIC` over case counts 0 to 3, with and without `megamorphicCaseListed`; `readBaselineICsSummary` on valid and truncated headers under `StrictChecks::Yes`.
- T8. `seeds-only-installing-cb.js` (`// jitcache-runs: Producer --useConcurrentGC=false; Consumer`). The bodies live in a source string that the script runs with `runString`, which evaluates it in a new realm; the same text hits the same CodeCache entry, so the realms share the UCBs. The producer runs the string once, drives the bodies' sites, each body holding an anchor, saves their snapshots and passes every body to `delta`. The consumer runs the string twice. The first realm's CB of each body installs the import, and its snapshot before any visit equals the derivation of section 6.3 applied to the saved producer snapshot. The second realm's CB, created next, takes the parked image natively, and its snapshot before any visit shows every IC never considered and every call site in `Init` without the seen bit.
- T9. `ic-allocation-failure.js`, one `Producer` sequence per fuzz point, each with `--useExecutableAllocationFuzz=true --fireExecutableAllocationFuzzAt=<n>`, over a range of n. The fuzzer fails whichever executable allocation comes n-th, so the script declares `jitcache-expect-fault: 0` for each of `exec-alloc.baseline-plan`, `exec-alloc.dfg-plan`, `exec-alloc.ftl-plan` and `exec-alloc.mathic-snippet`, and `jitcache-require-fault: 0 exec-alloc.ic-handler`. The script makes many IC sites cache cases that get a compiled stub rather than a shared thunk or a stateless stub, such as reads through the global proxy, so that some n in the range fails an IC allocation. Whenever `jitcacheStatus()` returns `exec-alloc.ic-handler`, a later `delta()` throws naming that fault, and the output equals the JITCache-off run's (T6). The require directive fails the script unless some n in the range reports `exec-alloc.ic-handler`.
- T10. `consumer-producer.js`, `// jitcache-runs: Producer --useConcurrentGC=false; ConsumerProducer; Consumer`. It tests two of the criteria a recapture can win on, richness and the IC count, so F goes without an anchor in the ConsumerProducer and G in the Producer. Body F has a `get_by_id` and an `instanceof` site that the producer folds, an `add` site the producer runs only on int32, and a witness `get_by_id` site the producer never reaches; an argument selects which of the three groups a call runs. Body G holds only `get_by_id` and `put_by_id` sites, and the producer only brings it to baseline, so whichever of its producer captures the scoring keeps lists no case and no considered IC. The producer passes F, which its folded sites anchor, to `delta`. The ConsumerProducer creates F and G with `skip` calls, runs F's `add` on doubles and F's witness once with a number as its base, without reaching the folded sites, then runs G's sites, and passes G to `delta`. F's recapture lists no case, but the `add` MathIC's repatching operation (`operationValueAddOptimize`) records the new operand types, so the recapture is richer and wins although the saved body lists two IC sites with cases. G's sites now list cases but add no profile category, because G's baseline code, compiled without DFG capability, writes no value or argument profile, so G's recapture wins on the IC count. Before any visit, the Consumer finds F's witness with `sawNonCell`, which only F's recapture carries, so that recapture is the body it imported, and F's folded sites with `canBeMegamorphic`, so the recapture passed the folds on; after one caching visit, each folded site lists a megamorphic case (for `instanceof`, `InstanceOfMegamorphic`, with `holdsGaveUp`). It finds G's visited sites considered, a state only G's recapture carries.
- T11. `Source/JavaScriptCore/jitcache/tests/ICLiveTests.cpp` (live VM). Capture is read-only and deterministic: on a baseline CB with warm property ICs and call sites, two captures with a snapshot between them produce identical bytes and leave the snapshot unchanged.
- T12. `ICLiveTests.cpp` (live VM), bodies without a metadata table and the newborn check:
  1. `function id(x) { return x; }`. Its newborn CB has a null `metadataTable()`, `callLinkSiteCounts` returns all zeros and `checkNewbornCodeBlock` passes. After `JIT::compileSync`, `baselineICsSectionSize` is 16 and the capture holds no group and no record. `prepareBaselineICs` of that section, with that CB's `BaselineJITCode` and the newborn CB of a never-called function with the same body, succeeds with strict on, and `seedCallLinkHistory` on that CB visits no site.
  2. `function has(o) { return "x" in o; }`, brought to baseline as above and then called three times through `JSC::call`, with objects of shapes A, A and B in that order. The call that installs its LLInt CB reaches no IC, since `in_by_id` has no metadata, and the IC's first visit in baseline code only spends `countdown`, which starts at 1 (`PropertyInlineCache::considerRepatchingCacheImpl`), so the second call caches A and the third B. Its CB has a null `metadataTable()`, and its capture holds one `InById` record with `caseCount` 2, `everConsidered` set and no group.
  3. `function caller(g) { return g(); }`, called twice through `JSC::call`. `checkNewbornCodeBlock` on its CB fails with `NewbornCallLinks`, while the newborn CB of a never-called function with the same body passes.
  4. The section of `has` with its record's `accessType` changed to `GetById`, which breaks A7 and no other check, makes `prepareBaselineICs`, given that CB's image and the newborn CB of a never-called twin of `has`, fail with `MoldPairing` under `StrictChecks::Yes` and succeed under `StrictChecks::No`, which checks nothing (section 4.5).
- T13. `ICLiveTests.cpp` (live VM), the twin API of section 11.1 without the runner:
  1. On the baseline CB of T11, `snapshotBaselineICs` lists one property-IC snapshot per IC and one call-link snapshot per call-link site, and each agrees, in every field the record holds, with the record `captureBaselineICs` writes at the same position.
  2. That capture, prepared with `prepareBaselineICs` under `StrictChecks::Yes` against that CB's `BaselineJITCode` and the newborn CB of a never-called function with the same body, then `seedCallLinkHistory`, `setupWithUnlinkedBaselineCode` with the same code and `attachPropertyICState` on the newborn CB, in the install function's order (section 6), leaves `checkRestoredBaselineICs` returning no mismatch. Clearing `everConsidered` afterwards on one IC whose record has it set makes the check return a non-empty list of `PropertyIC` mismatches, all at that IC's index and one of them naming `everConsidered`. The newborn CB is never run or installed, and the test uses it inside the `DeferGCForAWhile` it was created in.
- T14. `ICLiveTests.cpp` (live VM), the polymorphic bit of section 5.3. Each body is its own function, brought to baseline as above, and after each step `summarizeBaselineICs` and `captureBaselineICs` return the same `hasPolymorphicSite`:
  1. `has` of T12 item 2: false once its record lists one case, true once a call with a second shape makes it list two.
  2. `function getX(o) { return o.x; }`, called with object literals of distinct shapes that each own `x` as a plain property until its record lists one megamorphic case: false.
  3. `function callIt(f) { return f(); }`, called with two functions of different executables until its call-link record is `Polymorphic`: true. A second function with the same text, called with two closures of one executable until its record is `Polymorphic` with `hasSeenClosure`, whose stub then holds one slot: false.
  4. `function make(C) { return new C(); }`, called with two constructors until its record is `Virtual`: false.

## 12. Bench obligations

The lane sets no number for the bench to tune. The bench measures the default, strict off (THREAD Verification), where capture and prepare run no structural check.

- B1. Capture cost: `captureBaselineICs` time per CB against its IC and call-link counts, at the end of `BaselineJITPlan::finalize` and in `delta`, reported as part of THREAD's capture pause.
- B2. Install cost: `prepareBaselineICs`, `seedCallLinkHistory` and `attachPropertyICState` per body, reported inside the import cost THREAD's installation bound measures.
- B3. Section bytes per body and per artifact, for the producer-limit measurement THREAD's bench loop makes.
- B4. First-run residue: property-IC slow-path visits and call-link slow-path trips per body in the consumer's first pass C1, against the producer's warmed pass W (HARNESS.md, Benches), and, in bodies that carry counter progress, the number of sites the first DFG compile parses before their first consumer visit (the residue THREAD Restoration has the bench measure) and the `BadCache` and `BadConstantValue` exits that compile's code takes before its first jettison. Also the share of captured bodies whose `hasPolymorphicSite` is set, split between those where some property-IC record satisfies `isPolymorphicPropertyIC` and those where only a call site set it, since each of them gives up its counter progress (section 5.3).
- B5. E1's native effect: class-heavy microbenchmarks with derived constructors, with default options and with `--useLLInt=false`, before and after E1, including a derived constructor that sees one `new.target` in the LLInt and a second after it tiers up to baseline.
- B6. Recaptures that drop the bit: in ConsumerProducer runs, the bodies whose import carried no counter progress (SPEC-cb.md, `counterMode` `NotCarried`) and whose committed recapture carries it, because the recapture came before a polymorphic site cached two cases or linked two callees again (section 5.3), and the `BadCache` and `BadConstantValue` exits a later consumer's first DFG compile of those bodies takes.

## 13. Owned paths and the INTEGRATE manifest

Implementers of this lane write only these paths and functions.

| path | what |
|---|---|
| `Source/JavaScriptCore/jitcache/ICSection.h`, `.cpp` | sections 4.1 to 4.5, every declaration of section 4.4 included (`parseSection` with A1 to A6 under strict), and the derivation and recapture functions of section 6.3 |
| `Source/JavaScriptCore/jitcache/ICSites.h` | section 4.6: the guarded metadata walkers and `callLinkSiteCounts` |
| `Source/JavaScriptCore/jitcache/ICCapture.h`, `.cpp` | section 5, `CaptureSummary` included |
| `Source/JavaScriptCore/jitcache/ICRestore.h`, `.cpp` | sections 6.1, 6.2, 6.4 and 6.5: the check functions A7, A8, S1 and S2, `prepareBaselineICs`, seeding and attaching |
| `Source/JavaScriptCore/jitcache/ICTwins.h`, `.cpp` | section 11.1: snapshot, twin check and the shell function, compiled only with `ENABLE(JITCACHE_TWINS)` |
| `Source/JavaScriptCore/jitcache/tests/ICSectionTests.cpp` | T7 |
| `Source/JavaScriptCore/jitcache/tests/ICLiveTests.cpp` | T11 to T14 |
| `JSTests/stress/super-construct-cached-callee-baseline-born.js` | T1 |
| `JSTests/jitcache/ics/` | T2 to T6 and T8 to T10, and `resources/ics.js` |
| jit/JITCall.cpp: the `super_construct` branch of `JIT::compileOpCall` | E1 |
| bytecode/CallLinkInfo.h: `CallLinkInfo::setClearedByGC` (new) | E2 |
| bytecode/Repatch.h and Repatch.cpp: `gaveUpOperationFor` (new) | E3 |
| bytecode/AccessCase.h and AccessCase.cpp: `AccessCase::isMegamorphic` (new) | E4 |
| bytecode/InlineCacheCompiler.cpp: removal of the file-static `isMegamorphic`; its uses in `tryFoldToMegamorphic`, `compile` and `compileOneAccessCaseHandler`; the `didFailToAllocate()` branches of `compile`, `compileOneAccessCaseHandler` and `compileGetByDOMJITHandler` | E4, E5 |

The lane's `ICSection`, `ICSites`, `ICCapture`, `ICRestore` and `ICTwins` files are bundled by unified sources, so their file-local helpers carry names unique across `jitcache/` (prefix them `ics`), and nothing in them is declared `static` at namespace scope with a generic name.

Manifest entries for `docs/JitCache/specs/INTEGRATE-ics.md`:

- M1. `Source/JavaScriptCore/Sources.txt`: add `jitcache/ICSection.cpp`, `jitcache/ICCapture.cpp`, `jitcache/ICRestore.cpp` and `jitcache/ICTwins.cpp` (the last compiles to nothing without `ENABLE(JITCACHE_TWINS)`). No header is exported, so `CMakeLists.txt` needs no entry.
- M2. Section type ids (R-INT-1).
- M3. The seven fixed-option rows of options.md that name this lane (R-INT-5).
- M4. `JSC::JITCache::didFailExecutableAllocation(VM&, ExecutableAllocationSite)` in `jitcache/JITCacheFaults.h`, with the site `InlineCacheHandler`, and its behavior (R-INT-6).
- M5. Capture, scoring and install glue calls (R-INT-2 to R-INT-4), the ICs lane's capture or summary called before the CB lane's for the same CB, its `hasPolymorphicSite` passed on.
- M6. Test-build harness (R-INT-7, R-INT-9): `checkRestoredBaselineICs` after attach; the registration of `functionSnapshotBaselineICs` as the shell's `jitcacheICsSnapshot`; the shell's `jitcacheDelta` and `jitcacheStatus`; the sequence runner with its `// jitcache-runs:` lines, `arguments` convention and Off comparison, registered for `JSTests/jitcache/ics/`; and the C++ test runner.

## 14. Tasks

Ordered; each fits one implementation agent. A task that needs an integrator piece builds against the requirement's signature and lands when the integrator does.

1. E2, E3 and E4: the native additions, no behavior change. Build `debug-local` and run the `JSTests/stress` tests whose names contain `megamorphic`, `instanceof` or `inline-cache`. T3 checks the E3 entries of the access types it drives onto their `*GaveUp` operation against live ICs (section 11.2).
2. E1 and T1. Independent of the other tasks. Build and run T1 and the existing `JSTests/stress` tests whose names contain `super` or `class`.
3. `ICSection` and `ICSites`: types, size, canonical positions, `parseSection` (A1 to A6 under strict), `readBaselineICsSummary`, `isPolymorphicPropertyIC`, the derivation and recapture functions, the walkers, and T7's format, derivation, round-trip and `isPolymorphicPropertyIC` parts.
4. `ICCapture`: section 5, after tasks 1 and 3. T12 items 1 and 2, and T14.
5. `ICRestore`: sections 6.1 to 6.5, after tasks 1 and 3. T7's A7, A8 and S1 parts, T12 items 1, 3 and 4.
6. `ICTwins` (snapshot, twin check, shell function), T11, which reads the snapshot, and T13, after tasks 2, 4 and 5 and the C++ test runner of R-INT-9 (SPEC-integrator.md task 1). The integrator's install glue calls `checkRestoredBaselineICs` in twins builds and its jsc host registers the shell function (SPEC-integrator.md tasks 8 and 12, R-INT-7), so this task needs neither and lands before both.
7. `resources/ics.js`, T2 to T6, T8 and T10 on the integrator's runner, after task 6 and the integrator's capture and install glue, shell functions and runner (R-INT-2, R-INT-4 and R-INT-7; SPEC-integrator.md tasks 8, 9, 12 and 13).
8. E5 and T9, after R-INT-6 lands (SPEC-integrator.md task 2); T9 is a runner sequence, so it also waits for the integrator's tasks 12 and 13, as task 7 does.
9. B1 to B6 inside the integrator's bench loop.
