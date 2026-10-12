# SPEC-image history

Why the decisions of [SPEC-image.md](SPEC-image.md) and [SPEC-image.sites.md](SPEC-image.sites.md) are what they are, for whoever revisits one. Each record says what first seemed right, the evidence that changed it and what the SPEC does now. Nothing here binds, and section numbers are the SPEC's current ones.

## Finding references

The first ideas were to carry a target inside the immediate types, tagging `TrustedImmPtr` and `AbsoluteAddress`, or to find references after emission: scan the linked bytes for addresses, classify immediates by value at emission, ship pre-compaction code and recompact it in the consumer, or recompile in the consumer from recorded inputs.

Tagging the immediate types reaches hundreds of `MacroAssembler` operations on two architectures, each of which would have to learn a fixed form. Scanning fails because blinding, value folding and compaction make sites unrecoverable, and a literal can equal an address. Compaction of thunk branches depends on absolute addresses, so recompacting in the consumer would move internal offsets after the side tables recorded them. Recompiling is a native compilation, which the installation bound excludes. Meanwhile the baseline image embeds references at only a few dozen places, almost all through a handful of shared helpers: exception checks, write barriers, structure decodes, slow-path calls and IC slow paths.

The SPEC annotates those sites through the helpers of section 4.3, listed by the census, and emission without a recorder stays byte for byte native (I4). Two guards (section 4.4) turn a missed thunk link or pointer argument into an unrecorded compilation. A pointer immediate missed outside the guarded paths is the remaining risk, and the twin comparison catches it wherever the producer's and the consumer's values differ (section 11.4).

## Forms and canonical footprints

THREAD asks for an image patched with the assembler's own link writers. Defining each form by the site the native writer takes means the consumer runs the code `LinkBuffer` runs, islands included, and the producer's label translation is `LinkBuffer`'s own `applyOffset`. Three forms cover every reference; a far call is a pointer because the native far call is a fixed-width pointer move followed by an indirect call. The first design checked footprints only by reading them back after patching, under strict.

The writers trust their site (N21). `ARM64Assembler::linkJump` treats a `nop` at its site as the second half of a conditional branch and rewrites the instruction before it, which for a fixup at site 0 lies before the allocation, and the x86_64 writers store their field without reading the opcode. A section that passed every other check could therefore make a writer write outside its footprint. A separate x86_64 conditional-jump form was weighed and rejected: the opcode already decides the length (`E9` at `site-5`, or `0F 80` to `0F 8F` at `site-6`, which cannot both match), so a new form would add a kind without adding information.

Capture now writes each footprint in its canonical encoding (section 8.4), and V3 checks the encoding before any writer runs. V3 first ran on every import; once THREAD made strict off by default, normal mode trusts the checksummed bytes this lane's capture wrote (I8), and V3 runs under strict.

## Fixup order

The first design kept fixups sorted by site, and V3 required sites to strictly increase. That holds on x86_64, where every site ends its footprint.

On ARM64 a `Call`'s site ends its `bl` while a `Pointer`'s or a `Jump`'s site starts its first word, so a `bl` followed at once by a recorded `movz` or `b` puts two fixups at one site. Release builds emit nothing between bytecodes, so this happens wherever a `DEFINE_SLOW_OP` op, which ends with the `bl` of `JITSlowPathCall::call`, precedes a `mov` of a string constant or a `check_traps`, as in `` var a = `${x}y`; var t = "x"; ``. V3 rejected such a body, which under strict turned cache activity off on valid work, and a sort by site alone left the pair's order to `std::sort`, so a record could list the `Pointer` first and fail its own footprint check.

Fixups now follow their footprints: by site, and at a shared site the `Call` first. V3 checks that each footprint starts at or after the end of the one before it, which holds exactly for the order a correct capture writes, and lookups name a fixup by its site and form, which together name at most one.

## Code symbols and the thunk enumeration

The consumer resolves support keys during import, and resolving a thunk key may run the thunk's generator, so a key read from a file must never name the code to run. C++ operations and slow-path functions are different: the consumer only takes their address, or hands it to `JITThunks::ctiSlowPathFunctionStub` as data. A table of operations was not available either, since the Linux build has no `JITOperationList` section.

The SPEC keys C++ functions by `CodeSymbol`, an offset from `codeSymbolAnchor` in the engine object, which the header's build ID makes stable (section 3.5), and thunk generators by the closed `BaselineThunk` enumeration that `JIT::baselineThunkGenerator` maps (section 3.4). The producer's emitters go through the same mapping.

`JIT::baselineThunkGenerator` was first defined in `JIT.cpp`. Twelve of its entries are specializations of `generateOpResolveScopeThunk` and `generateOpGetFromScopeThunk`, member templates defined only in `JITPropertyAccess.cpp`, which the unified build compiles in another bundle, and nothing instantiates them explicitly. `JIT.cpp` would link only while the native scope emitters instantiated every one, and the census replaces those calls with the mapping itself. The definition moved to `JITPropertyAccess.cpp`.

## String switch ranks

THREAD Capture first said that internal branches carry no fixup. The leaf jump of an inline `switch_string` is internal, but which case holds a given rank depends on the order of the atoms' addresses in the process, so the jump's displacement depends on the process. The SPEC gives each rank's case jump a fixup, which THREAD Capture now names as its rule's one exception.

The first version resolved the rank cases before the consumer's string tables held code addresses. Step 7 of section 10.3 now fills the tables first, and `SwitchStringRankCase` resolves, in both processes, to the location the string table holds for the rank's key (N28).

## Cached temp registers

THREAD allows a cached temp register to be reused for the same reference, which would mean tagging `CachedTempRegister` with a reference identity. Reuse windows between labels are short in baseline code, and the tag would add state to every ARM64 cache path, while invalidating after each reference costs a few instructions at sites that already pay for a three-instruction move. R4 invalidates after each reference, and bench obligation B3 measures the cost.

## When a compilation records

The first design recorded every compilation while the VM had a producing role, with per-compilation reasons for Debugger-mode bytecode, the ShadowChicken and profiler opcodes and PC-to-origin maps, and it recorded whether or not the CB's UCB had a record. It also had a `ProcessAddress` kind for `g_superSamplerCount`, and a review proposed `UnannotatedReference` for the super-sampler opcodes.

A debugger attach turns cache activity off for good, so no recording compilation meets Debugger-mode bytecode or builds PC maps (N15, N29), and their reasons went. A recording fault ends production while activity stays on, so recording follows active production. A compilation whose UCB has no record was charged for a record no capture reads; telling the two apart needs the UCB registry, whose lookup belongs on the VM thread, where both plan constructors run (`jitCompileAndSetHeuristics`, `JIT::compileSync`). The super-sampler opcodes come only from builtin-mode parsing or the fixed-off `exposePrivateIdentifiers`, and no JSC or Bun builtin calls them, so the `ProcessAddress` kind served nothing a captured body can contain. `UnannotatedReference` names paths baseline code never takes, while these opcodes do reach emission, and a status diagnostic should name them.

Now `producerContext` follows active production (R-INT-1), the plan's `jitCacheRecordsImage()` decides (R-INT-12), `NotShareable` is the one shareability reason, and `SuperSamplerOpcode` marks those compilations (section 4.1).

## ARM64 veneers

THREAD requires each external conditional branch on ARM64 to go through a local `b`. The inline form, `b.!cond` over a `b`, turns every not-taken exception check into a taken branch on the hot path. A veneer per target after the code keeps the conditional branch not taken, shares one `b` among the branches to one target and lets compaction choose the conditional branch's form from internal distances.

Deferring a jump needs storage. The first design stopped all recording on a refused charge, which left a choice between an unlinked exception check and uncharged memory. Reserving deferral storage in the first charge does not work, because the number of conditional external jumps is unknown before emission. Groups are now charged as they grow, a jump whose group's charge is refused, or that arrives once recording has stopped, is linked natively at once, and `emitVeneers` links every jump deferred before (section 4.5, I19).

## The linked size

The image's size first came from the `BaselineJITCode`'s code reference. That reference returns the handle's size, which can exceed what `LinkBuffer` wrote: on ARM64 `MetaAllocatorHandle::shrink` rounds the compacted size up to the allocator's granule, and with the libpas JIT heap the handle takes a size-class bound on both architectures (N20). The tail keeps whatever the pool held, so a capture would have carried stale bytes outside any fixup, and the twin comparison would have compared two unrelated tails.

Images and snippets are now their linked size, `LinkBuffer::size()`, for capture, copy and flush (sections 3.1 and 4.7). `commit` samples the handle's size, as `JIT::finalizeOnMainThread` samples `jitCode->size()`, so the statistic keeps one measure across native and imported code. T17 tests bodies whose allocations end past their linked size.

## Charging

The first design charged each allocation, kept a record's step-rounded charge for its life, and considered a compact record that interns its targets. Per-fixup charges sit on the emission path, and slack kept for life grows with the number of live bodies. No measurement yet asks for a compact record, and THREAD leaves producer memory to the bench.

The recorder now charges in steps of `kRecordChargeStep`, a finished compilation or regeneration gives the slack back (section 4.7, step 6, and `didLinkSnippet`), and a rebuilt record is charged its exact bytes (section 10.3, step 14). Bench obligation B2 names the interned record as the first change to measure if live records dominate.

## MathICs without inline code

The first design gave every MathIC four locations and a slow call. When `JITMathIC::generateInline` returns false the IC has neither (N17), so a body with string concatenation failed S4 at capture or V5 at import.

MathIC entries now have a state without inline code, read from the IC's null locations after the link tasks (section 6.1) rather than from a second call in the emitters, because those null locations are the native fact the import must reproduce. A partial set of locations is a programming error.

## Exported headers

The first design constructed `MathICRegeneration` by value and called `ImageEmission.h` helpers inside `JITMathIC.h`, and turned inline `AssemblyHelpers.h` functions into callers of helpers that take `ImageReference`. A review offered two ways out: keep exported headers to declarations and hooks, or export every `jitcache/` header they reach.

Bun compiles against JSC's copied headers only, and its `JavaScriptCore/JIT.h` include reaches those headers (section 14.4), so neither version would have built in Bun. `ImageEmission.h` could not have been included from `AssemblyHelpers.h` anyway: `ImageReference` derives from `CCallHelpers::ConstantMaterializer`, and `CCallHelpers.h` includes `AssemblyHelpers.h`. Exporting would have handed the lane's internals to Bun and still left that cycle. A data member that exists only in twins builds would give an exported class two layouts if the switch reached JSC's translation units and not Bun's.

Exported headers now gain only forward declarations, opaque enumerations, pointer members and hooks with JSC types (section 14.4). The hooks take the VM address itself, which `vmAddressTarget` classifies, so no `JITCache` enumeration appears in a header. `generateOutOfLine` moved to `jit/JITMathIC.cpp` with explicit instantiations (section 6.2), and twins-only additions to exported classes are member functions: `JIT::setJITCacheTwin` creates the twin recorder in `m_imageRecorder`, which every build has.

## The inline-start rewrite

The first design recorded `linkJumpToOutOfLineSnippet`'s rewrite in a recording scope of its own and dropped every fixup the rewritten bytes touched. The rewrite's assembler emits one jump and never draws a random number, so a scope buys nothing. A `CannotCompile` body keeps profile-write fixups in the inline region: `JIT::emitMathICSlow` wires the repatching operation even after a full inline snippet, and that snippet carries profile writes because `generateInline`'s `shouldEmitProfiling` defaults to true. Those fixups are live before the rewrite and dead after it, and dropping a partly covered footprint would leave producer bits outside every fixup.

The rewrite now stays native. `didRewriteInlineStart` drops only the fixups wholly inside the rewritten bytes, inserts the `SnippetEntry` fixup at the site V5 fixes, and treats a partial overlap as `InconsistentRecord` (section 6.2, edit 1).

## The MathIC allocation fault

THREAD Failures has the VM thread raise the fault for a failed allocation before the failure's effects are written. `JITMathIC::generateOutOfLine` repoints the slow call before it allocates the full snippet, unconditionally, so the repoint is no effect of the failure; the failure's own effect is the native fallback that follows. The SPEC raises the fault at each `didFailToAllocate` branch, inside the operation and before the fallback (section 6.2, edit 5), so no capture can run between them. THREAD Execution gives this lane MathIC regeneration in every tier, so the call sits at those branches whatever tier owns the MathIC and whether or not the regeneration records.

## The `negate` operations

The first idea was to change `operationArithNegateProfiled` to take the IC. Its other callers pass a profile, `add`, `sub` and `mul` repoint to operations that take the IC, and DFG and FTL use `operationArithNegateOptimize`, whose replacement takes no profile. A new `operationArithNegateProfiledNoOptimize`, sharing one static helper with `operationArithNegateProfiled`, changes only baseline code (section 6.4).

## Baked facts

The first idea was to compare the capability level exactly, through `capabilityLevel()`, and to accept a taint mismatch in the direction native sharing tolerates. The code depends on the class only through `CannotCompile` (N13), and `capabilityLevel()` memoizes into the newborn CB, while the comparison must write nothing (I13). A taint mismatch leaves the CB native, which is safe. The comparison now tests the `CannotCompile` predicate, through `computeCapabilityLevel` when the memo is unset, and compares taint exactly (section 7).

## Side tables and section headers

The first design carried the side tables' code addresses as fixups, gave each section header a tag, layout, tier and architecture, stored the code map as pairs of index and offset, and had a U check for `m_ctiDefault`. THREAD carries code addresses as offsets into their image, which keeps every fixup a byte range of code. The container's directory gives each section its type and tier, and the build ID fixes the architecture and every layout. The code map holds exactly the instruction starts (N27), so its indexes repeat what the UCB has. Nothing reads `m_ctiDefault` at run time: `CodeBlock::baselineSwitchJumpTable` has no caller, and a dense switch reaches its default by a direct jump.

The sections now hold offsets and counts-only headers (section 8.2), the code map is one offset per instruction start, paired again at install and checked by count (U5), and `m_ctiDefault` is written for every table, list tables included, with no U check.

## Validation

The first design had `prepareImage` validate its sections itself and capture check every side-table pointer in both modes. That gave one validation two owners, and THREAD leaves assumption checks to strict. A validated-view type was weighed and bought nothing, since normal mode validates nothing and every caller would take the view unvalidated anyway.

The glue now validates, under strict, before `compareBakedFacts`, which relies on U4 (section 10.3, step 1). Normal mode trusts what this lane's capture wrote (I2, I3, I8), and debug builds assert it (section 8.5).

## Check names

`ImageCheck`'s enumerators take the checks' identifiers (`V3`, `S5`), where `CBCheck` uses descriptive names, because this SPEC, its failure table and its tests name every check by identifier, and `status` then reports steps such as `image.v3` (section 8.5).

## Capture

The first design streamed capture through a 64 KiB buffer charged to the budget, with tables encoded ahead of the writes, had the glue raise a second fault when a charge was refused, and bounded island chains at four hops. Nothing capture reads changes before JS resumes, and the integrator's writer already buffers, so the buffer copied for nothing. The budget raises its own fault on a refusal. Four hops hold only for the default pool, while `jitMemoryReservationSize` sets the pool's size freely and each hop of `FixedVMPoolExecutableAllocator::islandForJumpLocation` moves more than half the jump range.

The writes now stream live code, with canonical footprints from a stack array, a refusal returns `ChargeRefused` with no second fault, and S1 bounds the chain by the pool's size over half `nearJumpRange` (section 9).

## Preparation and the rebuilt record

The first design made the constructed code reachable only through `commit`, which has effects outside the prepared objects; passed a `RecordPolicy` flag beside the budget; and had the glue raise a separate fault when the budget refused the rebuilt record. The ICs lane checks its mold pairing against the code before anything is written, so it needs the code before `commit`. The budget pointer already says whether to rebuild the record, and no capture reads a record once production has ended. The budget raises its own fault.

`PreparedImage::code()` now returns the code before `commit` (section 10.1), the record is rebuilt exactly when a budget is passed, and an import whose rebuilt record is refused stays valid without a record (section 10.3, step 14).

## The twin CB and its inputs

A twin needs a CB of the body, linked natively in the consumer, that has not run. The first design created it through `ScriptExecutable::newCodeBlockFor`; the alternatives were `newReplacementCodeBlockFor`, or a hook between native setup and `installCode` while the executable's slot is still empty. `newCodeBlockFor` `RELEASE_ASSERT`s an empty slot, which `installCode` has just filled, and the `CopyParsedBlock` constructor behind `newReplacementCodeBlockFor` shares the installed CB's metadata (N18), so writing recorded inputs into it would overwrite the live CB's. A hook before `installCode` would interleave the twin's link, compile and exceptions with the install. The twin therefore comes from its class's `create()` after `installCode` (section 11.3, step 1).

A test hook was proposed to write the twin's capability memo. The twin shares the installed CB's executable and source provider, so its level and taint equal what the baked-fact comparison already checked, and the code depends on the level only through `CannotCompile` (N13). The check verifies both and writes neither.

The first inputs were the unguarded scope kinds alone, each recorded at its emitter's read. Emission also reads the global resolve types, the LLInt modes and the enumerator bytes, and a twin linked fresh in the consumer can resolve those differently, so the check would have failed without a defect. One snapshot at compile start replaces edits at ten emitter reads (section 11.1). Writing the snapshot into the twin's metadata raised a GC hazard: `CodeBlock::reconcileLLIntInlineCachesAtGCEnd` reads the structure-or-watchpoint-set union of a scope access as a `StructureID` for most types, so the write clears it and leaves every entry in a state the GC reads safely (section 11.3, step 3).

Atom-ness cannot be written into a twin, because no constant can be made plain again, and computing the choice in `snapshotCompileInputs` would copy the templates' selection order. The templates record it where they make it, which stays exact on whatever thread the producer compiled (N25, I22). Two further designs were rejected: a twin UCB, which would resolve UCB targets to its own addresses and lose the absolute comparison of UCB targets in the decodes; and a twins-only input source at every emitter's metadata read, which needs a relaxed `RELEASE_ASSERT(!JITCode::isJIT(...))` and an edit at each read, a larger native footprint than one destructor edit. Last, a fresh consumer must replay both processes' regenerations, so a ConsumerProducer's rebuilt record takes its twin data from the imported section, and each capture computes its own producer values (section 11.1).

## When the twin check runs

The first design relied on twins runs turning `useConcurrentJIT` off, and a review then proposed a per-lane twin switch in the runner. The snapshot equals what emission read only when no JS of the VM ran during the compile. In the consumer, overwriting the UCB's arithmetic profiles races compiler threads that read them without a lock (`ByteCodeParser::makeSafe`, a baseline compile of another CB of the body), and a DFG compile that read the overwritten bits would speculate on fewer types than the run had seen. Other runs set options per run, and the option defaults to on. A check that runs unsound reports differences that are not defects, a switch a run can forget leaves it unsound, and recording every input at its read would fix only the producer side.

The check now tests both preconditions itself and reports a skip (section 11.3), and THREAD Verification's skip rule decides when a skip fails a run.

## The twin's lifetime

The first design kept every twin CB in a `Strong` list until VM destruction, so its destructor never wrote `didOptimize`, which is UCB feedback that travels. The list pinned UCBs, executables and realms and changed what dies in twins runs; the UCB lane's `drop-and-reimport.js` failed. For a CB that never received JIT code, the `didOptimize` write is the destructor's only effect on shared state (N18), so skipping that one write is enough. `CodeBlock::~CodeBlock` can run at process exit while another thread finalizes: `VM::~VM` finalizes every live cell, Bun destroys every worker's VM on exit and the main thread's under `BUN_DESTRUCT_VM_ON_EXIT`, and `process.exit()` calls `exit()`, whose exit-time destructors can run while another thread still finalizes. The registry that tells it to skip must therefore outlive exit-time destructors; `LazyNeverDestroyed` would need an explicit first caller to construct it, and any thread destroying a CB can be first. A failed twin compile also left the UCB's arithmetic profiles overwritten.

The twin now dies after its check. A process-wide registry, a `NeverDestroyed` set its first user creates and a `Lock` with no destructor, makes `~CodeBlock` skip the write, and a scope object restores the profiles on every exit (section 11.3, steps 2, 3 and 6).

## Comparing an image with its twin

The first comparison read bytes and excluded only the footprints of artifact targets. The restored image and its twin are different allocations, so every `Call` and `Jump` fixup holds a different displacement, the snippets' `ImageOffset` jumps included, and ARM64 may route through other islands; T2 could never have passed. The comparison now reads canonical bytes, compares fixup lists by logical target and decodes each side's footprints in its own context (section 11.4). Support, VM, UCB, process and structure-base targets resolve to the same value in both contexts, so the decodes compare their absolute values with no per-kind exception list.

## Relocation domains

The first relocation clause grouped targets by kind, exempted far calls and made twins builds position-independent; a later proposal compared one base per domain per pair of processes. Bun's flags build the engine position-dependent (N23), so every `Operation` target resolves alike in every process, and so does every static atom, such as the `""` constant's `StringImpl::s_emptyAtomString` (N24), which a grouping by kind would have placed with the UCB. THREAD keeps Bun's flags and skips targets inside an object loaded at its link-time address. Under those flags every process of a build has the same engine bias, so a comparison of bases would refuse every pair, and the heap has no single base: cells live in the collector's blocks, while the VM, the atoms and the UCB's profile vectors are malloc allocations.

A target's domain is now where it resolves, and the clause skips objects with a zero load bias. A reference into the engine that a record missed then goes unseen, which is harmless: the engine sits at the same address in every process of a build, so the restored image holds the right value. The twins section keeps one producer value per fixup (section 11.4).

## Bodies a process captured itself

The first clause compared relocation pairs for every import. A ConsumerProducer imports its own commits once their UCBs die or through a live attach, and a Bun worker imports its main VM's. The domains that make such an import fail (the engine image, the structure reservation and the process-wide thunks) are process-wide, so every pair was equal and those runs failed by construction. Skipping the whole check for these imports would lose comparisons that need no moved address, and keeping a ConsumerProducer from importing its own commits would make twins builds behave unlike production.

Capture now writes a 16-byte capture-process token, and the clause compares no relocation pair for a body that carries the importing process's own token, which THREAD Verification now states as its own skip; every other comparison still runs (section 11.4).

## Moving the domains

The first plan relied on address randomization to move every domain, relaunching until the bases differed. The structure reservation is aligned to its 4 GiB size, so with 28 bits of mmap randomization on x86_64 a producer and a consumer share a base about once in 256 pairs, and an arm64 kernel left at 18 bits nearly always does. Two pools at different bases can still overlap, and the two processes generate thunks in different orders, so one process's thunk can sit where the other put another. Heap coincidences are chance. ASan's allocator, which serves the twins build's heap, was measured to move between fresh processes on x86_64; ARM64 was not measured. T3 at first described only what the clause detects.

The runner now places the pool and the structure reservation apart before `JSC::initialize`, checks on each architecture that the heap moves, and repeats a run whose only reports are heap coincidences, and T3's hooks force a report in each domain (R-INT-11).

## The scope thunk key

Keying `get_from_scope`'s thunk call by the profiled resolve type seemed right. The default branch of `JIT::emit_op_get_from_scope` mixes `if` and `else if`, so `ClosureVarWithVarInjectionChecks` and `GlobalPropertyWithVarInjectionChecks` link `GetFromScopeGlobalVar`. Keying by type would change native bytes (I4) and bake a shape no fact records. The census keys the thunk the native chain links (census D4), and T20 checks it.

## Untrusted constant moves

`moveReference` seemed right for census B6, a UCB-owned cell constant moved into a register. B6's native code is `moveValue`, an untrusted `Imm64` move. On x86_64, `MacroAssembler::shouldBlind(Imm64)` screens a cell pointer as a double and draws from the assembler's random source at least once, blinding about once in 4096, while `moveReference`'s trusted move draws nothing. Without a recorder, B6 would have changed the bytes at the site whenever blinding fired and shifted every later blinding decision of the compilation, which I4 forbids. The pin comparison folds blinded moves back to their constants, so it would not have shown the change. C1 already kept its untrusted `storeValue` through `storeReferenceValue`. B6 now goes through `moveReferenceValue`, whose native sequence is `moveValue`, and section 4.3 states the rule for both.

## No task owns the bench obligations

The list first ended with task 13, "B1 to B5", which named no file. Its implementer found that the lane's figures (the recorder's peak and charges, the live records, `prepareImage`'s transient memory) live where no bench event may be recorded: on a JIT worker, inside the install total THREAD's bound counts, or inside the span the `capture` event times. Accessors that the integrator's glue reads after those spans close, with one integrator task for the reader, would answer that, and were left for the bench loop to ask for, since HARNESS treats benches as alerts and no failing case needs them. The obligations stay in section 16.2, and THREAD Execution's bench loop takes them after the implementation.

## T12 forces each refusal

T12 first asked a sweep of limits over a real body to move the refusal through the main pass, the slow paths, `emitVeneers` and a MathIC snippet. `emitVeneers` charges at most once, to grow the fixup vector when it is full as the veneers begin and the doubling crosses a step. Whether that happens depends on how many fixups the main pass and the slow paths recorded, which differs between debug and release builds and between twins and plain ones, and on x86_64 `emitVeneers` records nothing. No limit and no body refuse inside it in every build, and such a refusal leaves the same code and record as one among the far-call fixups. The sweep still runs over a real body at every step, and the unit tests force a refusal inside each phase: the main pass and the slow paths of a real body, `emitVeneers` on a recorder whose fixup vector fills as two veneers begin, and a regeneration's first snippet step. A twins-only hook that makes a recorder refuse its first charge inside `emitVeneers` would aim a real compilation there. It was not added, since it is mechanism in the landed recorder that the recorder test already covers.

## T18 makes atoms after the first install

T18 first had one consumer use other constants as property keys before its first call, so that it would hold atoms where the producer's images compare generically. The jsc shell cannot give that case. It caches each nested function as a separate update, which `encodeFunctionCodeBlock` encodes with `Encoder::NumberStrings::No`, so `CachedJSValue::decode` builds each long string constant with `decodePlainString`, a fresh `StringImpl` whatever the atom table holds. That decode runs at the function's first call, in the request whose newborn CB installs the image, and no JavaScript runs between the two, so a property key used earlier only atomizes another string with the same characters. The case therefore arises after an install: the consumer uses the function's own constant as a key, which swaps it to its atom in place, ages the CB out so the UCB's sharing slot empties, and calls again, so the live UCB attaches the body and the image installs over the atom. The producer's own late atoms give the same case at the first install, since the UCB lane interns the constants the capture marked.
